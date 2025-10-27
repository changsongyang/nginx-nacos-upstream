//
// Created by pc on 9/28/25.
//

#include <ngx_event.h>
#include <ngx_nacos.h>
#include <ngx_nacos_data.h>

static char *ngx_nacos_conf_server_list(ngx_conf_t *cf, ngx_command_t *cmd,
                                        void *conf);

static char *ngx_nacos_conf_error_log(ngx_conf_t *cf, ngx_command_t *cmd,
                                      void *conf);

static ngx_int_t ngx_nacos_init_key_zone(ngx_shm_zone_t *zone, void *data);

static ngx_int_t ngx_nacos_subscribe(ngx_conf_t *cf, ngx_nacos_sub_t *sub,
                                     int naming);

// static char *ngx_nacos_conf_dynamic_service_key(ngx_conf_t *cf,
//                                                 ngx_command_t *cmd, void
//                                                 *conf);

static void *ngx_nacos_create_core_conf(ngx_conf_t *cf);

static char *ngx_nacos_inif_core_conf(ngx_conf_t *cf, void *conf);

static ngx_nacos_module_t nacos_module_ctx = {
    ngx_nacos_create_core_conf,
    ngx_nacos_inif_core_conf,
};

static ngx_command_t cmds[] = {
    {ngx_string("server_list"), NGX_NACOS_MAIN_CONF | NGX_CONF_1MORE,
     ngx_nacos_conf_server_list, 0,
     offsetof(ngx_nacos_main_conf_t, server_list), NULL},
    {ngx_string("grpc_server_list"), NGX_NACOS_MAIN_CONF | NGX_CONF_1MORE,
     ngx_nacos_conf_server_list, 0,
     offsetof(ngx_nacos_main_conf_t, grpc_server_list), NULL},
    {ngx_string("udp_port"), NGX_NACOS_MAIN_CONF | NGX_CONF_TAKE1,
     ngx_conf_set_str_slot, 0, offsetof(ngx_nacos_main_conf_t, udp_port), NULL},
    {ngx_string("udp_ip"), NGX_NACOS_MAIN_CONF | NGX_CONF_TAKE1,
     ngx_conf_set_str_slot, 0, offsetof(ngx_nacos_main_conf_t, udp_ip), NULL},
    {ngx_string("udp_bind"), NGX_NACOS_MAIN_CONF | NGX_CONF_TAKE1,
     ngx_conf_set_str_slot, 0, offsetof(ngx_nacos_main_conf_t, udp_bind), NULL},
    {ngx_string("username"), NGX_NACOS_MAIN_CONF | NGX_CONF_TAKE1,
     ngx_conf_set_str_slot, 0, offsetof(ngx_nacos_main_conf_t, username), NULL},
    {ngx_string("password"), NGX_NACOS_MAIN_CONF | NGX_CONF_TAKE1,
     ngx_conf_set_str_slot, 0, offsetof(ngx_nacos_main_conf_t, password), NULL},
    {ngx_string("default_group"), NGX_NACOS_MAIN_CONF | NGX_CONF_TAKE1,
     ngx_conf_set_str_slot, 0, offsetof(ngx_nacos_main_conf_t, default_group),
     NULL},
    {ngx_string("local_ip"), NGX_NACOS_MAIN_CONF | NGX_CONF_TAKE1,
     ngx_conf_set_str_slot, 0, offsetof(ngx_nacos_main_conf_t, local_ip), NULL},
    {ngx_string("config_tenant"), NGX_NACOS_MAIN_CONF | NGX_CONF_TAKE1,
     ngx_conf_set_str_slot, 0, offsetof(ngx_nacos_main_conf_t, config_tenant),
     NULL},
    {ngx_string("service_namespace"), NGX_NACOS_MAIN_CONF | NGX_CONF_TAKE1,
     ngx_conf_set_str_slot, 0,
     offsetof(ngx_nacos_main_conf_t, service_namespace), NULL},
    {ngx_string("server_host"), NGX_NACOS_MAIN_CONF | NGX_CONF_TAKE1,
     ngx_conf_set_str_slot, 0, offsetof(ngx_nacos_main_conf_t, server_host),
     NULL},
    {ngx_string("key_zone_size"), NGX_NACOS_MAIN_CONF | NGX_CONF_TAKE1,
     ngx_conf_set_size_slot, 0, offsetof(ngx_nacos_main_conf_t, key_zone_size),
     NULL},
    {ngx_string("keys_hash_max_size"), NGX_NACOS_MAIN_CONF | NGX_CONF_TAKE1,
     ngx_conf_set_num_slot, 0,
     offsetof(ngx_nacos_main_conf_t, keys_hash_max_size), NULL},
    {ngx_string("keys_bucket_size"), NGX_NACOS_MAIN_CONF | NGX_CONF_TAKE1,
     ngx_conf_set_num_slot, 0,
     offsetof(ngx_nacos_main_conf_t, keys_bucket_size), NULL},
    {ngx_string("config_keys_hash_max_size"),
     NGX_NACOS_MAIN_CONF | NGX_CONF_TAKE1, ngx_conf_set_num_slot, 0,
     offsetof(ngx_nacos_main_conf_t, config_keys_hash_max_size), NULL},
    {ngx_string("config_keys_bucket_size"),
     NGX_NACOS_MAIN_CONF | NGX_CONF_TAKE1, ngx_conf_set_num_slot, 0,
     offsetof(ngx_nacos_main_conf_t, config_keys_bucket_size), NULL},
    {ngx_string("udp_pool_size"), NGX_NACOS_MAIN_CONF | NGX_CONF_TAKE1,
     ngx_conf_set_size_slot, 0, offsetof(ngx_nacos_main_conf_t, udp_pool_size),
     NULL},
    {ngx_string("error_log"), NGX_NACOS_MAIN_CONF | NGX_CONF_1MORE,
     ngx_nacos_conf_error_log, 0, 0, NULL},
    {ngx_string("cache_dir"), NGX_NACOS_MAIN_CONF | NGX_CONF_1MORE,
     ngx_conf_set_str_slot, 0, offsetof(ngx_nacos_main_conf_t, cache_dir),
     NULL},
    // {ngx_string("dynamic_service_key"),
    //  NGX_NACOS_MAIN_CONF |  NGX_CONF_TAKE12,
    //  ngx_nacos_conf_dynamic_service_key, 0, 0, NULL},
    ngx_null_command};

ngx_module_t ngx_nacos_core_module = {NGX_MODULE_V1,
                                      &nacos_module_ctx,
                                      cmds,
                                      NGX_NACOS_MODULE,
                                      NULL, /* init master */
                                      NULL, /* init module */
                                      NULL, /* init process */
                                      NULL, /* init thread */
                                      NULL, /* exit thread */
                                      NULL, /* exit process */
                                      NULL, /* exit master */
                                      NGX_MODULE_V1_PADDING};

static void *ngx_nacos_create_core_conf(ngx_conf_t *cf) {
    ngx_nacos_main_conf_t *ncf = ngx_nacos_get_main_conf(cf);

    if (ngx_array_init(&ncf->server_list, cf->pool, 4, sizeof(ngx_addr_t)) !=
        NGX_OK) {
        return NGX_CONF_ERROR;
    }
    if (ngx_array_init(&ncf->grpc_server_list, cf->pool, 4,
                       sizeof(ngx_addr_t)) != NGX_OK) {
        return NGX_CONF_ERROR;
    }

    ncf->keys_bucket_size = NGX_CONF_UNSET_UINT;
    ncf->keys_hash_max_size = NGX_CONF_UNSET_UINT;
    ncf->config_keys_bucket_size = NGX_CONF_UNSET_UINT;
    ncf->config_keys_hash_max_size = NGX_CONF_UNSET_UINT;
    ncf->key_zone_size = NGX_CONF_UNSET_SIZE;
    ncf->udp_pool_size = NGX_CONF_UNSET_SIZE;

    return ncf;
}

static char *ngx_nacos_inif_core_conf(ngx_conf_t *cf, void *conf) {
    ngx_nacos_main_conf_t *ncf;
    ngx_int_t i;
    ngx_url_t u;
    ncf = conf;

    if (!ncf->server_list.nelts) {
        return "nacos server_list is empty";
    }

    if (ncf->udp_port.len && ncf->udp_port.data) {
        if ((i = ngx_atoi(ncf->udp_port.data, ncf->udp_port.len)) ==
            NGX_ERROR) {
            return "nacos udp_port not number";
        }
        if (i <= 0 || i > 65535) {
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                               "nacos udp_port=\"%V\" is invalid", i);
            return NGX_CONF_ERROR;
        }
        if (!ncf->udp_ip.len || !ncf->udp_ip.data) {
            return "nacos udp_ip is not config";
        }
        if (!ncf->udp_bind.data) {
            ncf->udp_bind.len = ncf->udp_ip.len + ncf->udp_port.len + 1;
            ncf->udp_bind.data = ngx_palloc(cf->pool, ncf->udp_bind.len);
            if (ncf->udp_bind.data == NULL) {
                return NGX_CONF_ERROR;
            }
            memcpy(ncf->udp_bind.data, ncf->udp_ip.data, ncf->udp_ip.len);
            ncf->udp_bind.data[ncf->udp_ip.len] = ':';
            memcpy(ncf->udp_bind.data + ncf->udp_ip.len + 1, ncf->udp_port.data,
                   ncf->udp_port.len);
        }
        ngx_memzero(&u, sizeof(ngx_url_t));
        u.url = ncf->udp_bind;
        u.listen = 1;
        if (ngx_parse_url(cf->pool, &u) != NGX_OK) {
            if (u.err) {
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                                   "%s in upstream \"%V\"", u.err, &u.url);
            }

            return NGX_CONF_ERROR;
        }

        ncf->udp_addr = u.addrs[0];
    } else if (ncf->grpc_server_list.nelts == 0) {
        return "nacos grpc_server_list is not config";
    }

    if (!ncf->cache_dir.len) {
        ngx_str_set(&ncf->cache_dir, "nacos_cache");
    }
    if (ngx_conf_full_name(cf->cycle, &ncf->cache_dir, 0) != NGX_OK) {
        return NGX_CONF_ERROR;
    }

    if (!ncf->default_group.data) {
        ngx_str_set(&ncf->default_group, "DEFAULT_GROUP");
    }

    if (!ncf->server_host.data) {
        ngx_str_set(&ncf->server_host, "nacos");
    }

    if (!ncf->service_namespace.data) {
        ngx_str_set(&ncf->service_namespace, "public");
    }

    if (!ncf->error_log) {
        ncf->error_log = &cf->cycle->new_log;
    }
    if (ncf->local_ip.len == 0 &&
        ngx_nacos_fetch_local_ip(ncf, cf->pool) != NGX_OK) {
        return "ngx_nacos_fetch_local_ip failed";
    }

    ngx_conf_init_size_value(ncf->key_zone_size, 4 << 20);  // 4M
    ngx_conf_init_size_value(ncf->udp_pool_size, 8192);
    ngx_conf_init_uint_value(ncf->keys_hash_max_size, 128);
    ngx_conf_init_uint_value(ncf->keys_bucket_size, 128);
    ngx_conf_init_uint_value(ncf->config_keys_hash_max_size, 128);
    ngx_conf_init_uint_value(ncf->config_keys_bucket_size, 128);
    ncf->keys_bucket_size =
        ngx_align(ncf->keys_bucket_size, ngx_cacheline_size);
    ncf->config_keys_bucket_size =
        ngx_align(ncf->config_keys_bucket_size, ngx_cacheline_size);

    return NGX_CONF_OK;
}

static char *ngx_nacos_conf_server_list(ngx_conf_t *cf, ngx_command_t *cmd,
                                        void *conf) {
    ngx_array_t *list_arr;
    ngx_uint_t i, j, n;
    ngx_str_t *value;
    ngx_url_t u;
    ngx_addr_t *adr;

    list_arr = (ngx_array_t *) (((char *) conf) + cmd->offset);
    value = cf->args->elts;
    n = cf->args->nelts;

    for (i = 1; i < n; ++i) {
        memset(&u, 0, sizeof(u));
        u.url = value[i];
        u.default_port = 8848;
        if (ngx_parse_url(cf->pool, &u) != NGX_OK) {
            if (u.err) {
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                                   "%s in nacos server_list \"%V\"", u.err,
                                   &u.url);
            }
            return NGX_CONF_ERROR;
        }

        for (j = 0; j < u.naddrs; ++j) {
            adr = ngx_array_push(list_arr);
            if (adr == NULL) {
                return NGX_CONF_ERROR;
            }
            *adr = u.addrs[j];
        }
    }

    return NGX_CONF_OK;
}

static char *ngx_nacos_conf_error_log(ngx_conf_t *cf, ngx_command_t *cmd,
                                      void *conf) {
    ngx_nacos_main_conf_t *mcf = conf;
    return ngx_log_set_log(cf, &mcf->error_log);
}

/** TODO dynamic  subscribe/unsubscribe service or config
static char *ngx_nacos_conf_dynamic_service_key(ngx_conf_t *cf,
                                                ngx_command_t *cmd,
                                                void *conf) {
    ngx_nacos_main_conf_t *mcf = conf;
    ngx_nacos_sub_t sub;
    ngx_str_t *value;
    ngx_uint_t i, n;
    ngx_int_t rc;

    if (mcf->dy_svc_config_key != NULL) {
        return "\"dynamic_service_key\" duplicated";
    }

    ngx_memzero(&sub, sizeof(sub));

    value = cf->args->elts;
    for (i = 1, n = cf->args->nelts; i < n; i++) {
        if (value[i].len > 8 &&
            ngx_strncmp(value[i].data, "data_id=", 8) == 0) {
            sub.data_id.data = value[i].data + 8;
            sub.data_id.len = value[i].len - 8;
            continue;
        }
        if (value[i].len > 6 && ngx_strncmp(value[i].data, "group=", 6) == 0) {
            sub.group.data = value[i].data + 6;
            sub.group.len = value[i].len - 6;
            continue;
        }

        return "\"dynamic_service_key\" is invalid, unknown parameter";
    }
    if (sub.group.len == 0) {
        sub.group = mcf->default_group;
    }
    if (sub.data_id.len == 0 || sub.group.len == 0) {
        return "\"dynamic_service_key\" is invalid, both \"data_id\" and "
               "\"group\" are required";
    }
    sub.key_ptr = &mcf->dy_svc_config_key;
    rc = ngx_nacos_subscribe_config(cf, &sub);
    if (rc != NGX_OK) {
        return "\"dynamic_service_key\" failed";
    }
    return NGX_CONF_OK;
} */

ngx_int_t ngx_nacos_subscribe_naming(ngx_conf_t *cf, ngx_nacos_sub_t *sub) {
    return ngx_nacos_subscribe(cf, sub, 1);
}

ngx_int_t ngx_nacos_subscribe_config(ngx_conf_t *cf, ngx_nacos_sub_t *sub) {
    return ngx_nacos_subscribe(cf, sub, 0);
}

static ngx_int_t ngx_nacos_subscribe(ngx_conf_t *cf, ngx_nacos_sub_t *sub,
                                     int naming) {
    ngx_nacos_main_conf_t *mcf;
    ngx_uint_t i, n;
    ngx_nacos_key_t **kptr, *k;
    ngx_nacos_data_t tmp;
    ngx_str_t zone_name;
    ngx_int_t rc;
    ngx_array_t *all_keys;

    mcf = ngx_nacos_get_main_conf(cf);
    all_keys = naming ? &mcf->keys : &mcf->config_keys;
    if (!naming && mcf->udp_port.len) {
        ngx_conf_log_error(NGX_LOG_WARN, cf, 0,
                           "nacos config is not supported by udp");
        return NGX_ERROR;
    }

    tmp.data_id = sub->data_id;
    tmp.group = sub->group;
    if (!tmp.group.len) {
        tmp.group = mcf->default_group;
    }
    if (tmp.data_id.len + tmp.group.len + 2 >= 512) {
        ngx_conf_log_error(NGX_LOG_WARN, cf, 0,
                           "nacos data_id and group is too long");
        return NGX_ERROR;
    }

    if (mcf->zone == NULL) {
        ngx_str_set(&zone_name, "nacos_zone");
        mcf->zone = ngx_shared_memory_add(cf, &zone_name, mcf->key_zone_size,
                                          &ngx_nacos_core_module);
        if (mcf->zone == NULL) {
            return NGX_ERROR;
        }
        mcf->zone->noreuse = 1;
        mcf->zone->data = mcf;
        mcf->zone->init = ngx_nacos_init_key_zone;
        mcf->cur_srv_index = rand() % mcf->server_list.nelts;
    }

    if (all_keys->size == 0) {
        if (ngx_array_init(all_keys, cf->pool, 16, sizeof(ngx_nacos_key_t *)) !=
            NGX_OK) {
            return NGX_ERROR;
        }
    } else {
        n = all_keys->nelts;
        kptr = all_keys->elts;
        for (i = 0; i < n; ++i) {
            if (nacos_key_eq(kptr[i], &tmp)) {
                *sub->key_ptr = kptr[i];
                return NGX_OK;
            }
        }
    }

    tmp.pool = cf->temp_pool;
    rc = ngx_nacos_fetch_disk_data(mcf, &tmp);
    if (rc == NGX_ERROR) {
        return NGX_ERROR;
    }
    if (rc == NGX_DECLINED) {
        if (naming) {
            if (ngx_nacos_fetch_addrs_net_data(mcf, &tmp) != NGX_OK) {
                return NGX_ERROR;
            }
        } else {
            if (ngx_nacos_fetch_config_net_data(mcf, &tmp) != NGX_OK) {
                return NGX_ERROR;
            }
        }
    }

    kptr = ngx_array_push(all_keys);
    if (kptr == NULL) {
        return NGX_ERROR;
    }
    k = ngx_palloc(cf->pool, sizeof(*k));
    if (k == NULL) {
        return NGX_ERROR;
    }

    *kptr = k;
    k->data_id = tmp.data_id;
    k->group = tmp.group;
    k->ctx = ngx_palloc(cf->temp_pool, sizeof(ngx_nacos_key_ctx_t));
    if (k->ctx == NULL) {
        return NGX_ERROR;
    }
    k->ctx->wrlock = 0;
    k->ctx->version = tmp.version;
    k->ctx->data = tmp.adr;
    k->use_shared = 0;
    *sub->key_ptr = k;
    return NGX_OK;
}

static ngx_int_t ngx_nacos_init_key_zone(ngx_shm_zone_t *zone, void *data) {
    ngx_nacos_main_conf_t *mcf;
    ngx_nacos_key_t **key;
    ngx_nacos_key_ctx_t *ctx;
    ngx_uint_t i, n;
    size_t len;
    char *c;

    mcf = zone->data;
    mcf->sh = (ngx_slab_pool_t *) zone->shm.addr;
    key = mcf->keys.elts;
    n = mcf->keys.nelts;

    for (i = 0; i < n; ++i) {
        ctx = key[i]->ctx;
        c = ctx->data;
        key[i]->use_shared = 1;
        key[i]->ctx = ngx_slab_alloc_locked(mcf->sh, sizeof(*ctx));
        if (key[i]->ctx == NULL) {
            return NGX_ERROR;
        }
        if (c != NULL) {
            len = *(size_t *) c;
            key[i]->ctx->data = ngx_slab_alloc_locked(mcf->sh, len);
            if (key[i]->ctx->data == NULL) {
                return NGX_ERROR;
            }
            key[i]->ctx->wrlock = ctx->wrlock;
            key[i]->ctx->version = ctx->version;
            memcpy(key[i]->ctx->data, c, len);
        } else {
            key[i]->ctx->data = NULL;
            key[i]->ctx->wrlock = ctx->wrlock;
            key[i]->ctx->version = ctx->version;
        }
    }

    key = mcf->config_keys.elts;
    n = mcf->config_keys.nelts;
    for (i = 0; i < n; ++i) {
        ctx = key[i]->ctx;
        c = ctx->data;
        key[i]->use_shared = 1;
        key[i]->ctx = ngx_slab_alloc_locked(mcf->sh, sizeof(*ctx));
        if (key[i]->ctx == NULL) {
            return NGX_ERROR;
        }
        if (c != NULL) {
            len = *(size_t *) c;
            key[i]->ctx->data = ngx_slab_alloc_locked(mcf->sh, len);
            if (key[i]->ctx->data == NULL) {
                return NGX_ERROR;
            }
            key[i]->ctx->wrlock = ctx->wrlock;
            key[i]->ctx->version = ctx->version;
            memcpy(key[i]->ctx->data, c, len);
        } else {
            key[i]->ctx->data = NULL;
            key[i]->ctx->wrlock = ctx->wrlock;
            key[i]->ctx->version = ctx->version;
        }
    }

    return NGX_OK;
}
