//
// Created by dear on 22-5-22.
//

#include <ngx_event.h>
#include <ngx_nacos.h>
#include <ngx_nacos_aux.h>
#include <ngx_nacos_data.h>

static char *ngx_nacos_init_conf(ngx_cycle_t *cycle, void *conf);

static char *ngx_nacos_conf_block(ngx_conf_t *cf, ngx_command_t *cmd,
                                  void *conf);

static ngx_core_module_t nacos_module = {
    ngx_string("nacos"),
    NULL,                // 解析配置文件之前执行
    ngx_nacos_init_conf  // 解析配置文件之后执行
};

static ngx_command_t cmds[] = {
    {ngx_string("nacos"), NGX_MAIN_CONF | NGX_CONF_BLOCK | NGX_CONF_NOARGS,
     ngx_nacos_conf_block, 0, 0, NULL},
    // {ngx_string("dynamic_service_key"),
    //  NGX_NACOS_MAIN_CONF | NGX_DIRECT_CONF | NGX_CONF_TAKE12,
    //  ngx_nacos_conf_dynamic_service_key, 0, 0, NULL},
    ngx_null_command};

ngx_module_t ngx_nacos_module = {NGX_MODULE_V1,
                                 &nacos_module,
                                 cmds,
                                 NGX_CORE_MODULE,
                                 NULL, /* init master */
                                 NULL, /* init module */
                                 NULL, /* init process */
                                 NULL, /* init thread */
                                 NULL, /* exit thread */
                                 NULL, /* exit process */
                                 NULL, /* exit master */
                                 NGX_MODULE_V1_PADDING};

static char *ngx_nacos_init_conf(ngx_cycle_t *cycle, void *conf) {
    ngx_nacos_main_conf_t *ncf = conf;
    ngx_hash_init_t ha;
    ngx_hash_key_t *hkeys;
    ngx_nacos_key_t **k_ptr;
    ngx_pool_t *temp;
    ngx_uint_t i, n;
    u_char *buf;
    size_t buf_len;
    if (ncf == NULL) {  // no nacos config
        return NGX_CONF_OK;
    }
    if (ncf->keys.nelts == 0 && ncf->config_keys.nelts == 0) {
        return "remove nacos block if not need";
    }

    temp = ngx_create_pool(4096, cycle->log);
    if (temp == NULL) {
        return NGX_CONF_ERROR;
    }

    // nacos_keys_hash
    ha.pool = cycle->pool;
    ha.temp_pool = temp;
    {
        ha.bucket_size = ncf->keys_bucket_size;
        ha.max_size = ncf->keys_hash_max_size;
        ha.key = ngx_hash_key;
        ha.name = "nacos_keys_hash";
        ha.hash = NULL;

        n = ncf->keys.nelts;
        k_ptr = ncf->keys.elts;

        hkeys = ngx_palloc(ha.temp_pool, n * sizeof(*hkeys));
        if (hkeys == NULL) {
            ngx_destroy_pool(temp);
            return NGX_CONF_ERROR;
        }

        for (i = 0; i < n; ++i) {
            buf_len = k_ptr[i]->group.len + k_ptr[i]->data_id.len + 5;
            buf = ngx_palloc(ha.temp_pool, buf_len);
            if (buf == NULL) {
                ngx_destroy_pool(temp);
                return NGX_CONF_ERROR;
            }

            hkeys[i].key.len =
                ngx_snprintf(buf, buf_len - 1, "%V@@%V", &k_ptr[i]->group,
                             &k_ptr[i]->data_id) -
                buf;
            hkeys[i].key.data = buf;
            hkeys[i].value = k_ptr[i];
            hkeys[i].key_hash = ha.key(buf, hkeys[i].key.len);
        }

        if (ngx_hash_init(&ha, hkeys, n) != NGX_OK) {
            ngx_destroy_pool(temp);
            return NGX_CONF_ERROR;
        }
        ncf->key_hash = ha.hash;
    }

    {
        ha.bucket_size = ncf->config_keys_bucket_size;
        ha.max_size = ncf->config_keys_hash_max_size;
        ha.key = ngx_hash_key;
        ha.name = "nacos_config_keys_hash";
        ha.hash = NULL;

        n = ncf->config_keys.nelts;
        k_ptr = ncf->config_keys.elts;

        hkeys = ngx_palloc(ha.temp_pool, n * sizeof(*hkeys));
        if (hkeys == NULL) {
            ngx_destroy_pool(temp);
            return NGX_CONF_ERROR;
        }

        for (i = 0; i < n; ++i) {
            buf_len = k_ptr[i]->group.len + k_ptr[i]->data_id.len + 5;
            buf = ngx_palloc(ha.temp_pool, buf_len);
            if (buf == NULL) {
                ngx_destroy_pool(temp);
                return NGX_CONF_ERROR;
            }
            hkeys[i].key.len =
                ngx_snprintf(buf, buf_len - 1, "%V@@%V", &k_ptr[i]->group,
                             &k_ptr[i]->data_id) -
                buf;
            hkeys[i].key.data = buf;
            hkeys[i].value = k_ptr[i];
            hkeys[i].key_hash = ha.key(buf, hkeys[i].key.len);
        }

        if (ngx_hash_init(&ha, hkeys, n) != NGX_OK) {
            ngx_destroy_pool(temp);
            return NGX_CONF_ERROR;
        }
        ncf->config_key_hash = ha.hash;
    }

    ngx_destroy_pool(temp);
    return NGX_CONF_OK;
}

static char *ngx_nacos_conf_block(ngx_conf_t *cf, ngx_command_t *cmd,
                                  void *conf) {
    ngx_conf_t pcf;
    char *rv;
    ngx_int_t i, nacos_module_count;
    void **conf_ctx;
    ngx_nacos_module_t *ctx;
    ngx_nacos_main_conf_t *ncf, **mncf = conf;

    rv = NGX_CONF_ERROR;

    if (*mncf) {
        return "is duplicate";
    }
    ncf = *mncf = ngx_pcalloc(cf->pool, sizeof(*ncf));
    if (ncf == NULL) {
        return NGX_CONF_ERROR;
    }

    pcf = *cf;
    cf->module_type = NGX_NACOS_MODULE;
    cf->cmd_type = NGX_NACOS_MAIN_CONF;

    nacos_module_count = ngx_count_modules(cf->cycle, NGX_NACOS_MODULE);
    conf_ctx = ngx_pcalloc(cf->pool, sizeof(void *) * nacos_module_count);
    if (conf_ctx == NULL) {
        goto end;
    }

    for (i = 0; cf->cycle->modules[i]; i++) {
        if (cf->cycle->modules[i]->type != NGX_NACOS_MODULE) {
            continue;
        }
        ctx = cf->cycle->modules[i]->ctx;
        if (ctx && ctx->create_conf) {
            conf_ctx[cf->cycle->modules[i]->ctx_index] = ctx->create_conf(cf);

            if (conf_ctx[cf->cycle->modules[i]->ctx_index] == NULL) {
                return "nacos_create_conf failed";
            }
        } else {
            conf_ctx[cf->cycle->modules[i]->ctx_index] = ncf;
        }
    }
    cf->ctx = (void *) &conf_ctx;

    rv = ngx_conf_parse(cf, NULL);

    if (rv != NGX_CONF_OK) {
        goto end;
    }

    for (i = 0; cf->cycle->modules[i]; i++) {
        if (cf->cycle->modules[i]->type != NGX_NACOS_MODULE) {
            continue;
        }
        ctx = cf->cycle->modules[i]->ctx;
        if (ctx && ctx->init_conf) {
            rv = ctx->init_conf(cf, conf_ctx[cf->cycle->modules[i]->ctx_index]);
            if (rv != NGX_CONF_OK) {
                goto end;
            }
        }
    }
    rv = NGX_CONF_OK;
    *cf = pcf;

    if (ngx_nacos_aux_init(cf) != NGX_OK) {
        rv = NGX_CONF_ERROR;
    }

end:
    return rv;
}

ngx_nacos_main_conf_t *ngx_nacos_get_main_conf(ngx_conf_t *cf) {
    return (ngx_nacos_main_conf_t *)
        cf->cycle->conf_ctx[ngx_nacos_module.index];
}

char *ngx_nacos_add_runner(ngx_conf_t *cf, ngx_nacos_runner_handler_t handler,
                           void *data) {
    ngx_nacos_runner_t *r;
    ngx_nacos_main_conf_t *nmcf = ngx_nacos_get_main_conf(cf);
    if (nmcf == NULL) {
        return "nacos block is required";
    }

    if (nmcf->runners.size == 0 &&
        ngx_array_init(&nmcf->runners, cf->pool, 4,
                       sizeof(ngx_nacos_runner_t)) != NGX_OK) {
        return NGX_CONF_ERROR;
    }

    r = ngx_array_push(&nmcf->runners);
    if (r == NULL) {
        return NGX_CONF_ERROR;
    }
    r->handler = handler;
    r->data = data;
    return NGX_CONF_OK;
}