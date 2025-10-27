//
// Created by pc on 10/21/25.
//
#include <ngx_event.h>
#include <ngx_http.h>
#include <ngx_nacos.h>
#include <lauxlib.h>
#include <ngx_http_lua_api.h>
#include <ngx_nacos_data.h>

static char *ngx_http_init_nacos_by_lua_file(ngx_conf_t *cf, ngx_command_t *cmd,
                                             void *conf);

static void *ngx_http_nacos_lua_create_main_conf(ngx_conf_t *cf);

static ngx_int_t ngx_http_nacos_lua_post_config(ngx_conf_t *cf);

static char *ngx_http_nacos_lua_init(ngx_conf_t *cf, void *conf);

static void ngx_http_nacos_lua_runner(void *data);

static int ngx_http_nacos_lua_create_module(lua_State *L);

static int ngx_http_nacos_lua_subscribe_service(lua_State *L);

static int ngx_http_nacos_lua_listen_config(lua_State *L);

static int ngx_http_nacos_lua_log(lua_State *L);

static int ngx_http_nacos_lua_handler_unsubscribe(lua_State *L);

static int ngx_http_nacos_lua_handler_gc(lua_State *L);

static ngx_int_t ngx_http_nacos_lua_handler_clean(lua_State *L);

static int ngx_http_nacos_lua_handler_index(lua_State *L);

static int ngx_http_nacos_lua_handler_newindex(lua_State *L);

static int ngx_http_nacos_lua_handler_tostring(lua_State *L);

static ngx_int_t ngx_http_nacos_dynamic_naming_handler(
    ngx_nacos_dynamic_key_t *key, ngx_nacos_key_t *nk);

static ngx_int_t ngx_http_nacos_dynamic_config_handler(
    ngx_nacos_dynamic_key_t *key, ngx_nacos_key_t *nk);

static int ngx_http_nacos_lua_subscribe(lua_State *L, int naming);

static const luaL_Reg handler_meta[] = {
    {"__call", ngx_http_nacos_lua_handler_unsubscribe},
    {"__gc", ngx_http_nacos_lua_handler_gc},
    {"__index", ngx_http_nacos_lua_handler_index},
    {"__newindex", ngx_http_nacos_lua_handler_newindex},
    {"__tostring", ngx_http_nacos_lua_handler_tostring},
    {NULL, NULL}};

typedef struct {
    ngx_nacos_dynamic_key_t *dk;
    ngx_flag_t naming;
    lua_State *lua;
    int callback_ref;
    ngx_uint_t notified;
} ngx_http_nacos_lua_ctx_t;

typedef struct ngx_http_nacos_lua_main_conf_s ngx_http_nacos_lua_main_conf_t;

typedef struct {
    ngx_str_t name;
    ngx_http_nacos_lua_main_conf_t *nlmf;
} ngx_http_nacos_lua_co_t;

struct ngx_http_nacos_lua_main_conf_s {
    lua_State *lua;
    ngx_nacos_main_conf_t *nmcf;
    ngx_array_t co_list;
};

static ngx_command_t cmds[] = {
    {ngx_string("init_nacos_by_lua_file"), NGX_HTTP_MAIN_CONF | NGX_CONF_TAKE1,
     ngx_http_init_nacos_by_lua_file, 0, 0, NULL},

    ngx_null_command};

#define NACOS_LUA_CONF_U "__nacos_lua_main_conf"
#define NACOS_LUA_HANDLER "nacos.Handler"

#define nacos_lua_handler_ptr(L) \
    ((ngx_http_nacos_lua_ctx_t **) luaL_checkudata((L), 1, NACOS_LUA_HANDLER))
#define nacos_lua_handler_ctx(L) (*nacos_lua_handler_ptr(L))

static ngx_http_module_t http_nacos_lua_module_ctx = {
    NULL,                           /* preconfiguration */
    ngx_http_nacos_lua_post_config, /* postconfiguration */

    ngx_http_nacos_lua_create_main_conf, /* create main configuration */
    ngx_http_nacos_lua_init,             /* init main configuration */

    NULL, /* create server configuration */
    NULL, /* merge server configuration */

    NULL, /* create location configuration */
    NULL  /* merge location configuration */
};

static ngx_flag_t nacos_process;

ngx_module_t ngx_http_nacos_lua_module = {NGX_MODULE_V1,
                                          &http_nacos_lua_module_ctx,
                                          cmds,
                                          NGX_HTTP_MODULE,
                                          NULL, /* init master */
                                          NULL, /* init module */
                                          NULL, /* init process */
                                          NULL, /* init thread */
                                          NULL, /* exit thread */
                                          NULL, /* exit process */
                                          NULL, /* exit master */
                                          NGX_MODULE_V1_PADDING};

static void *ngx_http_nacos_lua_create_main_conf(ngx_conf_t *cf) {
    return ngx_pcalloc(cf->pool, sizeof(ngx_http_nacos_lua_main_conf_t));
}

static ngx_inline ngx_http_nacos_lua_main_conf_t *
ngx_http_nacos_lua_get_main_conf(lua_State *L) {
    ngx_http_nacos_lua_main_conf_t *nlmf;

    lua_getglobal(L, NACOS_LUA_CONF_U);
    nlmf = lua_touserdata(L, -1);
    lua_pop(L, 1);

    return nlmf;
}

static char *ngx_http_init_nacos_by_lua_file(ngx_conf_t *cf, ngx_command_t *cmd,
                                             void *conf) {
    ngx_str_t *value;
    ngx_http_nacos_lua_co_t *co, **cop;
    ngx_http_nacos_lua_main_conf_t *nlmf = conf;

    if (!nlmf->nmcf) {
        nlmf->nmcf = ngx_nacos_get_main_conf(cf);
        if (nlmf->nmcf == NULL) {
            return "no nacos module";
        }
        if (ngx_array_init(&nlmf->co_list, cf->pool, 4,
                           sizeof(ngx_http_nacos_lua_co_t *)) != NGX_OK) {
            return NGX_CONF_ERROR;
        }
    }

    value = cf->args->elts;

    cop = ngx_array_push(&nlmf->co_list);
    if (cop == NULL) {
        return NGX_CONF_ERROR;
    }

    co = ngx_pcalloc(cf->pool, sizeof(ngx_http_nacos_lua_co_t));
    if (co == NULL) {
        return NGX_CONF_ERROR;
    }
    *cop = co;

    co->name = value[1];
    co->nlmf = nlmf;

    if (ngx_get_full_name(cf->pool, (ngx_str_t *) &ngx_cycle->prefix,
                          &co->name) != NGX_OK) {
        return NGX_CONF_ERROR;
    }

    return ngx_nacos_add_runner(cf, ngx_http_nacos_lua_runner, co);
}

static char *ngx_http_nacos_lua_init(ngx_conf_t *cf, void *conf) {
    if (ngx_http_lua_add_package_preload(
            cf, "nacos", ngx_http_nacos_lua_create_module) != NGX_OK) {
        return NGX_CONF_ERROR;
    }

    return NGX_CONF_OK;
}

static int ngx_http_nacos_lua_create_module(lua_State *L) {
    if (!nacos_process) {
        return luaL_error(L, "nacos process only");
    }

    luaL_newmetatable(L, NACOS_LUA_HANDLER);
    luaL_register(L, NULL, handler_meta);

    lua_createtable(L, 0, 3);

    lua_pushcfunction(L, ngx_http_nacos_lua_subscribe_service);
    lua_setfield(L, -2, "subscribe_service");

    lua_pushcfunction(L, ngx_http_nacos_lua_listen_config);
    lua_setfield(L, -2, "listen_config");

    lua_pushcfunction(L, ngx_http_nacos_lua_log);
    lua_setfield(L, -2, "log");

    return 1;
}

static int ngx_http_nacos_lua_subscribe_service(lua_State *L) {
    return ngx_http_nacos_lua_subscribe(L, 1);
}

static int ngx_http_nacos_lua_listen_config(lua_State *L) {
    return ngx_http_nacos_lua_subscribe(L, 0);
}

static int ngx_http_nacos_lua_subscribe(lua_State *L, int naming) {
    int nargs;
    const char *data_id, *group;
    ngx_str_t sd, sg;
    ngx_nacos_dynamic_key_t *dk;
    ngx_http_nacos_lua_ctx_t *ctx, **ctx_ptr;

    nargs = lua_gettop(L);

    if (nargs != 3) {
        return luaL_error(L, "invalid arguments");
    }
    data_id = luaL_checklstring(L, 1, NULL);
    group = luaL_checklstring(L, 2, NULL);

    if (!lua_isfunction(L, 3)) {
        return luaL_error(L, "arg3 require listen function");
    }

    sd.data = (u_char *) data_id;
    sg.data = (u_char *) group;
    sd.len = ngx_strlen(data_id);
    sg.len = ngx_strlen(group);

    if (naming) {
        dk = ngx_nacos_dynamic_naming_key_add(&sd, &sg);
    } else {
        dk = ngx_nacos_dynamic_config_key_add(&sd, &sg);
    }
    if (dk == NULL) {
        return luaL_error(L, "add dynamic key failed");
    }

    ctx = ngx_pcalloc(dk->pool, sizeof(ngx_http_nacos_lua_ctx_t));
    if (ctx == NULL) {
        ngx_nacos_dynamic_config_key_del(dk);
        return luaL_error(L, "alloc memory failed");
    }

    ctx->lua = L;
    ctx->naming = naming;
    dk->handler_ctx = ctx;
    dk->handler = ngx_http_nacos_dynamic_config_handler;
    if (naming) {
        dk->handler = ngx_http_nacos_dynamic_naming_handler;
    }

    ctx->dk = dk;
    lua_pushvalue(L, 3);
    ctx->callback_ref = luaL_ref(L, LUA_REGISTRYINDEX);

    ctx_ptr = lua_newuserdata(L, sizeof(void *));
    *ctx_ptr = ctx;

    luaL_getmetatable(L, NACOS_LUA_HANDLER);
    lua_setmetatable(L, -2);

    return 1;
}

#define NGX_DOUBLE_LEN 25

/* Get the maximum possible length, not the actual length */
static ngx_inline size_t ngx_http_lua_get_num_len(lua_State *L, int idx) {
    double num;

    num = (double) lua_tonumber(L, idx);
    if (num == (double) (int32_t) num) {
        return NGX_INT32_LEN;
    }

    return NGX_DOUBLE_LEN;
}

static ngx_inline u_char *ngx_http_lua_write_num(lua_State *L, int idx,
                                                 u_char *dst) {
    double num;
    int n;

    num = (double) lua_tonumber(L, idx);
    /*
     * luajit format number with only 14 significant digits.
     * To be consistent with lujit, don't use (double) (long) below
     * or integer greater than 99,999,999,999,999 will different from luajit.
     */
    if (num == (double) (int32_t) num) {
        dst = ngx_snprintf(dst, NGX_INT64_LEN, "%D", (int32_t) num);

    } else {
        /*
         * The maximum number of significant digits is 14 in lua.
         * Please refer to lj_strfmt.c for more details.
         */
        n = snprintf((char *) dst, NGX_DOUBLE_LEN, "%.14g", num);
        if (n < 0) {
            ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, ngx_errno,
                          "snprintf(\"%f\") failed");

        } else {
            dst += n;
        }
    }

    return dst;
}

static int ngx_http_nacos_lua_log(lua_State *L) {
    ngx_log_t *log;
    const char *msg;
    ngx_uint_t level;
    u_char *buf;
    u_char *p, *q;
    ngx_str_t name;
    int nargs, i;
    size_t size, len;
    size_t src_len = 0;
    int type;
    lua_Debug ar;
    ngx_http_nacos_lua_main_conf_t *nlmf;

    nlmf = ngx_http_nacos_lua_get_main_conf(L);
    if (nlmf == NULL) {
        log = ngx_cycle->log;
    } else {
        log = nlmf->nmcf->error_log;
    }

    level = luaL_checkint(L, 1);
    if (level > NGX_LOG_DEBUG) {
        msg = lua_pushfstring(L, "bad log level: %d", level);
        return luaL_argerror(L, 1, msg);
    }

    /* remove log-level param from stack */
    lua_remove(L, 1);

    if (level > log->log_level) {
        return 0;
    }

    /* add debug info */

    lua_getstack(L, 1, &ar);
    lua_getinfo(L, "Snl", &ar);

    /* get the basename of the Lua source file path, stored in q */
    name.data = (u_char *) ar.short_src;
    if (name.data == NULL) {
        name.len = 0;

    } else {
        p = name.data;
        while (*p != '\0') {
            if (*p == '/' || *p == '\\') {
                name.data = p + 1;
            }

            p++;
        }

        name.len = p - name.data;
    }

    nargs = lua_gettop(L);

    size = name.len + NGX_INT_T_LEN + sizeof(":: ") - 1;

    if (*ar.namewhat != '\0' && *ar.what == 'L') {
        src_len = ngx_strlen(ar.name);
        size += src_len + sizeof("(): ") - 1;
    }

    for (i = 1; i <= nargs; i++) {
        type = lua_type(L, i);
        switch (type) {
            case LUA_TNUMBER:
                size += ngx_http_lua_get_num_len(L, i);
                break;

            case LUA_TSTRING:
                lua_tolstring(L, i, &len);
                size += len;
                break;

            case LUA_TNIL:
                size += sizeof("nil") - 1;
                break;

            case LUA_TBOOLEAN:
                if (lua_toboolean(L, i)) {
                    size += sizeof("true") - 1;

                } else {
                    size += sizeof("false") - 1;
                }

                break;

            case LUA_TTABLE:
                if (!luaL_callmeta(L, i, "__tostring")) {
                    return luaL_argerror(L, i,
                                         "expected table to have "
                                         "__tostring metamethod");
                }

                lua_tolstring(L, -1, &len);
                size += len;
                break;

            case LUA_TLIGHTUSERDATA:
                if (lua_touserdata(L, i) == NULL) {
                    size += sizeof("null") - 1;
                    break;
                }

                continue;

            default:
                msg = lua_pushfstring(L,
                                      "string, number, boolean, or nil "
                                      "expected, got %s",
                                      lua_typename(L, type));
                return luaL_argerror(L, i, msg);
        }
    }

    buf = lua_newuserdata(L, size);

    p = ngx_copy(buf, name.data, name.len);

    *p++ = ':';

    p = ngx_snprintf(p, NGX_INT_T_LEN, "%d",
                     ar.currentline > 0 ? ar.currentline : ar.linedefined);

    *p++ = ':';
    *p++ = ' ';

    if (*ar.namewhat != '\0' && *ar.what == 'L') {
        p = ngx_copy(p, ar.name, src_len);
        *p++ = '(';
        *p++ = ')';
        *p++ = ':';
        *p++ = ' ';
    }

    for (i = 1; i <= nargs; i++) {
        type = lua_type(L, i);
        switch (type) {
            case LUA_TNUMBER:
                p = ngx_http_lua_write_num(L, i, p);
                break;

            case LUA_TSTRING:
                q = (u_char *) lua_tolstring(L, i, &len);
                p = ngx_copy(p, q, len);
                break;

            case LUA_TNIL:
                *p++ = 'n';
                *p++ = 'i';
                *p++ = 'l';
                break;

            case LUA_TBOOLEAN:
                if (lua_toboolean(L, i)) {
                    *p++ = 't';
                    *p++ = 'r';
                    *p++ = 'u';
                    *p++ = 'e';

                } else {
                    *p++ = 'f';
                    *p++ = 'a';
                    *p++ = 'l';
                    *p++ = 's';
                    *p++ = 'e';
                }

                break;

            case LUA_TTABLE:
                luaL_callmeta(L, i, "__tostring");
                q = (u_char *) lua_tolstring(L, -1, &len);
                p = ngx_copy(p, q, len);
                break;

            case LUA_TLIGHTUSERDATA:
                *p++ = 'n';
                *p++ = 'u';
                *p++ = 'l';
                *p++ = 'l';

                break;

            default:
                return luaL_error(L, "impossible to reach here");
        }
    }

    if (p - buf > (off_t) size) {
        return luaL_error(L, "buffer error: %d > %d", (int) (p - buf),
                          (int) size);
    }

    ngx_log_error(level, log, 0, "%s%*s", "[nacos_lua]", (size_t) (p - buf),
                  buf);

    return 0;
}

static int ngx_http_nacos_lua_handler_unsubscribe(lua_State *L) {
    if (ngx_http_nacos_lua_handler_clean(L) == NGX_OK) {
        return 0;
    }
    return luaL_error(L, "[bug]no ctx found, after gc??");
}

static int ngx_http_nacos_lua_handler_gc(lua_State *L) {
    (void) ngx_http_nacos_lua_handler_clean(L);
    return 0;
}

static int ngx_http_nacos_lua_handler_index(lua_State *L) {
    ngx_http_nacos_lua_ctx_t *ctx;
    const char *key;

    key = luaL_checkstring(L, 2);
    ctx = nacos_lua_handler_ctx(L);

    if (ctx == NULL) {
        return luaL_error(L, "cannot get %s after unsubscribe", key);
    }

    if (ctx->naming) {
        if (strcmp(key, "service_name") == 0) {
            lua_pushlstring(L, (char *) ctx->dk->data_id.data,
                            ctx->dk->data_id.len);
            return 1;
        }
    } else {
        if (strcmp(key, "data_id") == 0) {
            lua_pushlstring(L, (char *) ctx->dk->data_id.data,
                            ctx->dk->data_id.len);
            return 1;
        }
    }

    if (strcmp(key, "group") == 0) {
        lua_pushlstring(L, (char *) ctx->dk->group.data, ctx->dk->group.len);
        return 1;
    }
    if (strcmp(key, "notified") == 0) {
        lua_pushboolean(L, (int) ctx->notified);
        return 1;
    }

    lua_getmetatable(L, 1);
    lua_pushvalue(L, 2);
    lua_rawget(L, -2);

    return 1;
}

static int ngx_http_nacos_lua_handler_newindex(lua_State *L) {
    return luaL_error(L, "cannot index handler");
}

static int ngx_http_nacos_lua_handler_tostring(lua_State *L) {
    ngx_http_nacos_lua_ctx_t *ctx;
    static u_char temp[1024];
    size_t len;

    ctx = nacos_lua_handler_ctx(L);
    len = ngx_snprintf(temp, sizeof(temp),
                       "nacos_config_handler[data_id=%V,group=%V,notified=%s]",
                       &ctx->dk->data_id, &ctx->dk->group,
                       ctx->notified ? "true" : "false") -
          temp;

    lua_pushlstring(L, (char *) temp, len);
    return 1;
}

static ngx_int_t ngx_http_nacos_dynamic_naming_handler(
    ngx_nacos_dynamic_key_t *key, ngx_nacos_key_t *nk) {
    ngx_http_nacos_lua_ctx_t *ctx;
    ngx_http_nacos_lua_main_conf_t *nlmf;
    ngx_uint_t version;
    ngx_nacos_service_addrs_t out_addrs;
    ngx_pool_t *pool;
    lua_State *L;
    int rv;
    ngx_uint_t i;
    ngx_nacos_service_addr_t *addr;

    ctx = key->handler_ctx;
    ctx->notified++;
    L = ctx->lua;
    nlmf = ngx_http_nacos_lua_get_main_conf(L);

    version = 0;
    out_addrs.version = 0;
    pool = ngx_create_pool(512, nlmf->nmcf->error_log);
    if (pool == NULL) {
        goto err;
    }

    if (ngx_array_init(&out_addrs.addrs, pool, 16,
                       sizeof(ngx_nacos_service_addr_t)) != NGX_OK) {
        ngx_log_error(NGX_LOG_ERR, nlmf->nmcf->error_log, 0,
                      "failed to init out_addrs in lua : %V&&%V", &nk->data_id,
                      &nk->group);
        goto err;
    }

    if (nax_nacos_get_addrs(nk, &version, &out_addrs) != NGX_OK) {
        ngx_log_error(NGX_LOG_ERR, nlmf->nmcf->error_log, 0,
                      "failed to get config in lua : %V&&%V", &nk->data_id,
                      &nk->group);
        goto err;
    }

    lua_rawgeti(L, LUA_REGISTRYINDEX, ctx->callback_ref);
    lua_createtable(L, (int) out_addrs.addrs.nelts, 2);
    lua_pushlstring(L, (char *) key->data_id.data, key->data_id.len);
    lua_setfield(L, -2, "service_name");
    lua_pushlstring(L, (char *) key->group.data, key->group.len);
    lua_setfield(L, -2, "group");

    addr = out_addrs.addrs.elts;
    for (i = 0; i < out_addrs.addrs.nelts; i++) {
        lua_createtable(L, 0, 4);
        lua_pushlstring(L, (char *) addr[i].host.data, addr[i].host.len);
        lua_setfield(L, -2, "ip");
        lua_pushinteger(L, addr[i].port);
        lua_setfield(L, -2, "port");
        lua_pushinteger(L, addr[i].weight);
        lua_setfield(L, -2, "weight");
        lua_pushlstring(L, (char *) addr[i].cluster.data, addr[i].cluster.len);
        lua_setfield(L, -2, "cluster");
        lua_rawseti(L, -2, (int) i + 1);
    }

    rv = lua_pcall(L, 1, 0, 0);
    if (rv) {
        ngx_log_error(
            NGX_LOG_ERR, nlmf->nmcf->error_log, 0,
            "failed to run lua function in handler naming(%V@@%V): %s",
            &key->group, &key->data_id, lua_tostring(L, -1));
    }
err:
    if (pool) {
        ngx_destroy_pool(pool);
    }
    return NGX_OK;
}

static ngx_int_t ngx_http_nacos_dynamic_config_handler(
    ngx_nacos_dynamic_key_t *key, ngx_nacos_key_t *nk) {
    ngx_http_nacos_lua_ctx_t *ctx;
    ngx_http_nacos_lua_main_conf_t *nlmf;
    ngx_nacos_config_fetcher_t fetcher;
    lua_State *L;
    int rv;

    ctx = key->handler_ctx;
    ctx->notified++;
    L = ctx->lua;
    nlmf = ngx_http_nacos_lua_get_main_conf(L);

    ngx_memzero(&fetcher, sizeof(fetcher));
    fetcher.pool = ngx_create_pool(512, nlmf->nmcf->error_log);
    if (fetcher.pool == NULL) {
        return NGX_OK;
    }

    if (nax_nacos_get_config(nk, &fetcher) != NGX_OK) {
        ngx_log_error(NGX_LOG_ERR, nlmf->nmcf->error_log, 0,
                      "failed to get config in lua : %V&&%V", &nk->data_id,
                      &nk->group);
        goto err;
    }

    lua_rawgeti(L, LUA_REGISTRYINDEX, ctx->callback_ref);
    lua_createtable(L, 0, 4);
    lua_pushlstring(L, (char *) fetcher.md5.data, fetcher.md5.len);
    lua_setfield(L, -2, "md5");
    lua_pushlstring(L, (char *) fetcher.config.data, fetcher.config.len);
    lua_setfield(L, -2, "data");
    lua_pushlstring(L, (char *) key->data_id.data, key->data_id.len);
    lua_setfield(L, -2, "data_id");
    lua_pushlstring(L, (char *) key->group.data, key->group.len);
    lua_setfield(L, -2, "group");

    rv = lua_pcall(L, 1, 0, 0);
    if (rv) {
        ngx_log_error(
            NGX_LOG_ERR, nlmf->nmcf->error_log, 0,
            "failed to run lua function in handler config(%V@@%V): %s",
            &nk->group, &nk->data_id, lua_tostring(L, -1));
    }
err:
    if (fetcher.pool) {
        ngx_destroy_pool(fetcher.pool);
    }
    return NGX_OK;
}

static void ngx_http_nacos_lua_runner(void *data) {
    int s;
    lua_State *L;
    ngx_http_nacos_lua_co_t *co;
    ngx_http_nacos_lua_main_conf_t *nlmf;

    nacos_process = 1;
    co = data;
    nlmf = co->nlmf;

    L = nlmf->lua;

    s = luaL_loadfile(L, (char *) co->name.data);
    if (s == 0) {
        s = lua_pcall(L, 0, 0, 0);
        if (s) {
            ngx_log_error(NGX_LOG_EMERG, nlmf->nmcf->error_log, 0,
                          "failed to run lua file (%i): %s", s,
                          lua_tostring(L, -1));
            lua_pop(L, 1);
        }
    } else {
        ngx_log_error(NGX_LOG_EMERG, nlmf->nmcf->error_log, 0,
                      "failed to load lua file (%i): %s", s,
                      lua_tostring(L, -1));
    }
}

static ngx_int_t ngx_http_nacos_lua_post_config(ngx_conf_t *cf) {
    ngx_http_nacos_lua_main_conf_t *nlmf;

    nlmf = ngx_http_conf_get_module_main_conf(cf, ngx_http_nacos_lua_module);
    nlmf->lua = ngx_http_lua_get_global_state(cf);
    if (nlmf->lua == NULL) {
        if (ngx_process == NGX_PROCESS_SIGNALLER || ngx_test_config) {
            return NGX_OK;
        }
        ngx_conf_log_error(
            NGX_LOG_EMERG, cf, 0,
            "no lua state. ngx_http_lua_module is required before"
            " ngx_http_nacos_lua_module");
        return NGX_ERROR;
    }
    lua_pushlightuserdata(nlmf->lua, nlmf);
    lua_setglobal(nlmf->lua, NACOS_LUA_CONF_U);
    return NGX_OK;
}

static ngx_int_t ngx_http_nacos_lua_handler_clean(lua_State *L) {
    ngx_http_nacos_lua_ctx_t **ctx_ptr, *ctx;
    ctx_ptr = nacos_lua_handler_ptr(L);
    ctx = *ctx_ptr;

    if (!ctx) {
        return NGX_DONE;
    }

    luaL_unref(L, LUA_REGISTRYINDEX, ctx->callback_ref);

    if (ctx->naming) {
        ngx_nacos_dynamic_naming_key_del(ctx->dk);
    } else {
        ngx_nacos_dynamic_config_key_del(ctx->dk);
    }

    *ctx_ptr = NULL;
    return NGX_OK;
}