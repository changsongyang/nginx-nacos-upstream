//
// Created by dear on 22-5-20.
//

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>
#include <ngx_nacos.h>
#include <ngx_nacos_data.h>

typedef struct {
    ngx_str_t name;
    ngx_array_t *cluster_lengths;
    ngx_array_t *cluster_values;
} ngx_http_nacos_cluster_t;

typedef struct {
    ngx_http_upstream_srv_conf_t *uscf;
    ngx_http_upstream_init_pt original_init_upstream;
    ngx_str_t data_id;
    ngx_str_t group;
    ngx_uint_t weight;
    ngx_uint_t max_fails;
    time_t fail_timeout;
    ngx_http_nacos_cluster_t *clusters;  // ngx_http_nacos_cluster_t
    ngx_uint_t cluster_size;
} ngx_http_nacos_srv_conf_t;

typedef struct {
    ngx_pool_t *pool;
    ngx_uint_t ref;
    ngx_nacos_key_t *key;
    ngx_uint_t version;
    ngx_nacos_service_addrs_t addrs;
    ngx_flag_t use_cluster;
    ngx_http_upstream_srv_conf_t *origin;
    ngx_http_upstream_srv_conf_t *us;  // no cluster
    ngx_array_t *clustered_us;         // ngx_http_upstream_srv_conf_t
} ngx_http_nacos_peers_t;

typedef struct {
    ngx_str_t cluster;
    // TODO other log
} ngx_http_nacos_ctx_t;

typedef struct {
    ngx_http_nacos_peers_t *peers;
    // origin
    void *original_data;
    ngx_event_get_peer_pt original_get_peer;
    ngx_event_free_peer_pt original_free_peer;
#if (NGX_HTTP_SSL)
    ngx_event_set_peer_session_pt original_set_session;
    ngx_event_save_peer_session_pt original_save_session;
#endif
} ngx_http_nacos_rrp_t;

static void *ngx_http_nacos_create_srv_conf(ngx_conf_t *cf);

static char *ngx_http_conf_use_nacos_address(ngx_conf_t *cf, ngx_command_t *cmd,
                                             void *conf);

static char *ngx_http_conf_nacos_use_cluster(ngx_conf_t *cf, ngx_command_t *cmd,
                                             void *conf);

static ngx_int_t ngx_http_nacos_init_upstream(ngx_conf_t *cf,
                                              ngx_http_upstream_srv_conf_t *us);

static ngx_int_t ngx_http_nacos_init_peers(ngx_http_request_t *r,
                                           ngx_http_upstream_srv_conf_t *us);

static ngx_http_upstream_srv_conf_t *ngx_http_nacos_select_upstream(
    ngx_http_nacos_peers_t *peers, ngx_str_t *cluster);

static ngx_http_nacos_peers_t *ngx_http_get_nacos_peers(
    ngx_http_request_t *r, ngx_http_upstream_srv_conf_t *us);

static ngx_int_t ngx_http_nacos_get_peer(ngx_peer_connection_t *pc, void *data);

static void ngx_http_nacos_free_peer(ngx_peer_connection_t *pc, void *data,
                                     ngx_uint_t state);

static ngx_int_t ngx_http_nacos_add_variables(ngx_conf_t *cf);

static ngx_int_t ngx_http_nacos_real_cluster_variable(
    ngx_http_request_t *r, ngx_http_variable_value_t *v, uintptr_t data);

#if (NGX_HTTP_SSL)

static ngx_int_t ngx_http_nacos_peer_session(ngx_peer_connection_t *pc,
                                             void *data);

static void ngx_http_nacos_save_peer_session(ngx_peer_connection_t *pc,
                                             void *data);

#endif

static ngx_http_module_t ngx_http_nacos_module_ctx = {
    ngx_http_nacos_add_variables, /* preconfiguration */
    NULL,                         /* postconfiguration */

    NULL, /* create main configuration */
    NULL, /* init main configuration */

    ngx_http_nacos_create_srv_conf, /* create server configuration */
    NULL,                           /* merge server configuration */

    NULL, /* create location configuration */
    NULL  /* merge location configuration */
};

static ngx_command_t cmds[] = {
    {ngx_string("nacos_subscribe_service"), NGX_HTTP_UPS_CONF | NGX_CONF_1MORE,
     ngx_http_conf_use_nacos_address, NGX_HTTP_SRV_CONF_OFFSET, 0, NULL},
    {ngx_string("nacos_use_cluster"), NGX_HTTP_UPS_CONF | NGX_CONF_1MORE,
     ngx_http_conf_nacos_use_cluster, NGX_HTTP_SRV_CONF_OFFSET, 0, NULL},
    ngx_null_command};

static ngx_http_variable_t ngx_http_nacos_vars[] = {
    {ngx_string("nacos_real_cluster"), NULL,
     ngx_http_nacos_real_cluster_variable, 0, NGX_HTTP_VAR_NOCACHEABLE, 0},
    ngx_http_null_variable};

ngx_module_t ngx_http_nacos_upstream_module = {NGX_MODULE_V1,
                                               &ngx_http_nacos_module_ctx,
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

static void *ngx_http_nacos_create_srv_conf(ngx_conf_t *cf) {
    return ngx_pcalloc(cf->pool, sizeof(ngx_http_nacos_srv_conf_t));
}
static ngx_int_t ngx_http_nacos_add_server(ngx_http_nacos_peers_t *peers,
                                           ngx_http_nacos_srv_conf_t *nscf,
                                           ngx_pool_t *temp_pool);

static char *ngx_http_conf_use_nacos_address(ngx_conf_t *cf, ngx_command_t *cmd,
                                             void *conf) {
    ngx_http_nacos_srv_conf_t *nlcf = conf;
    ngx_uint_t i;
    ngx_uint_t n = cf->args->nelts;
    ngx_str_t *value = cf->args->elts, s;
    ngx_nacos_sub_t tmp;
    ngx_nacos_main_conf_t *mf;
    ngx_int_t weight, max_fails;
    time_t fail_timeout;

    if (nlcf->uscf) {
        return "is duplicate";
    }

    weight = 1;
    max_fails = 1;
    fail_timeout = 10;

    ngx_memzero(&tmp, sizeof(tmp));

    for (i = 1; i < n; ++i) {
        if (value[i].len > 8 &&
            ngx_strncmp(value[i].data, "service_name=", 13) == 0) {
            tmp.data_id.data = value[i].data + 13;
            tmp.data_id.len = value[i].len - 13;
            continue;
        }
        if (value[i].len > 6 && ngx_strncmp(value[i].data, "group=", 6) == 0) {
            tmp.group.data = value[i].data + 6;
            tmp.group.len = value[i].len - 6;
            continue;
        }
        if (value[i].len > 7 && ngx_strncmp(value[i].data, "weight=", 7) == 0) {
            weight = ngx_atoi(value[i].data + 7, value[i].len - 7);
            if (weight <= 0) {
                ngx_conf_log_error(
                    NGX_LOG_EMERG, cf, 0,
                    "weight= must be number: invalid parameter \"%V\"",
                    &value[i]);
                return NGX_CONF_ERROR;
            }
            continue;
        }

        if (value[i].len > 10 &&
            ngx_strncmp(value[i].data, "max_fails=", 10) == 0) {
            max_fails = ngx_atoi(value[i].data + 10, value[i].len - 10);
            if (max_fails < 0) {
                ngx_conf_log_error(
                    NGX_LOG_EMERG, cf, 0,
                    "max_fails= must be number: invalid parameter \"%V\"",
                    &value[i]);
                return NGX_CONF_ERROR;
            }
            continue;
        }

        if (value[i].len > 10 &&
            ngx_strncmp(value[i].data, "max_fails=", 10) == 0) {
            max_fails = ngx_atoi(value[i].data + 10, value[i].len - 10);
            if (max_fails < 0) {
                ngx_conf_log_error(
                    NGX_LOG_EMERG, cf, 0,
                    "max_fails= must be number: invalid parameter \"%V\"",
                    &value[i]);
                return NGX_CONF_ERROR;
            }
            continue;
        }

        if (value[i].len > 13 &&
            ngx_strncmp(value[i].data, "fail_timeout=", 13) == 0) {
            s.len = value[i].len - 13;
            s.data = &value[i].data[13];

            fail_timeout = ngx_parse_time(&s, 1);

            if (fail_timeout == (time_t) NGX_ERROR) {
                ngx_conf_log_error(
                    NGX_LOG_EMERG, cf, 0,
                    "fail_timeout= must be time: invalid parameter \"%V\"",
                    &value[i]);
                return NGX_CONF_ERROR;
            }

            continue;
        }

        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "invalid parameter \"%V\"",
                           &value[i]);
        return NGX_CONF_ERROR;
    }

    if (!tmp.data_id.len) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "require data_id");
        return NGX_CONF_ERROR;
    }

    nlcf->data_id = tmp.data_id;
    nlcf->group = tmp.group;
    nlcf->weight = weight;
    nlcf->max_fails = max_fails;
    nlcf->fail_timeout = fail_timeout;

    nlcf->uscf =
        ngx_http_conf_get_module_srv_conf(cf, ngx_http_upstream_module);
    nlcf->original_init_upstream = nlcf->uscf->peer.init_upstream;
    if (!nlcf->original_init_upstream) {
        nlcf->original_init_upstream = ngx_http_upstream_init_round_robin;
    }
    nlcf->uscf->peer.init_upstream = ngx_http_nacos_init_upstream;

    mf = ngx_nacos_get_main_conf(cf);
    if (mf == NULL) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "nacos block is required before");
        return NGX_CONF_ERROR;
    }

    return NGX_CONF_OK;
}

static char *ngx_http_conf_nacos_use_cluster(ngx_conf_t *cf, ngx_command_t *cmd,
                                             void *conf) {
    ngx_uint_t i, n;
    ngx_http_script_compile_t sc;
    ngx_str_t *value;
    ngx_http_nacos_cluster_t *cluster;
    ngx_http_nacos_srv_conf_t *nscf = conf;

    if (nscf->clusters != NULL) {
        return "is duplicate";
    }

    n = cf->args->nelts;
    cluster = ngx_pcalloc(cf->pool, sizeof(ngx_http_nacos_cluster_t) * (n - 1));
    if (cluster == NULL) {
        return NGX_CONF_ERROR;
    }

    nscf->clusters = cluster;
    nscf->cluster_size = n - 1;

    value = cf->args->elts;
    for (i = 1; i < n; i++) {
        if (value[i].len == 0) {
            return "invalid cluster";
        }
        cluster->name = value[i];

        ngx_memzero(&sc, sizeof(ngx_http_script_compile_t));
        sc.variables = ngx_http_script_variables_count(&cluster->name);

        if (sc.variables) {
            sc.cf = cf;
            sc.source = &cluster->name;
            sc.lengths = &cluster->cluster_lengths;
            sc.values = &cluster->cluster_values;
            sc.complete_lengths = 1;
            sc.complete_values = 1;

            if (ngx_http_script_compile(&sc) != NGX_OK) {
                return NGX_CONF_ERROR;
            }
        }
        cluster++;
    }

    return NGX_CONF_OK;
}

static u_char *ngx_http_nacos_log_handler(ngx_log_t *log, u_char *buf, size_t len) {
    ngx_http_nacos_peers_t *peers;
    u_char *p = buf;
    if (log->action) {
        p = ngx_snprintf(buf, len, " while %s", log->action);
        len -= p - buf;
    }

    peers = log->data;
    p = ngx_snprintf(p, len, ": %V:%V", &peers->key->group,
                     &peers->key->data_id);
    return p;
}

static ngx_http_nacos_peers_t *ngx_http_nacos_create_peers(
    ngx_log_t *log, ngx_flag_t clustered,
    ngx_http_upstream_srv_conf_t *origin) {
    ngx_pool_t *pool;
    ngx_http_nacos_peers_t *peers;
    ngx_log_t *new_log;

    pool = ngx_create_pool(2048, log);
    if (pool == NULL) {
        return NULL;
    }

    new_log = ngx_palloc(pool, sizeof(ngx_log_t));
    if (new_log == NULL) {
        ngx_destroy_pool(pool);
        return NULL;
    }
    *new_log = *log;
    pool->log = new_log;

    peers = ngx_pcalloc(pool, sizeof(ngx_http_nacos_peers_t));
    if (peers == NULL) {
        ngx_destroy_pool(pool);
        return NULL;
    }

    new_log->data = peers;
    new_log->handler = ngx_http_nacos_log_handler;
    new_log->action = "nacos update addrs";

    peers->origin = origin;
    peers->use_cluster = clustered;
    peers->pool = pool;
    peers->ref = 1;
    if (ngx_array_init(&peers->addrs.addrs, peers->pool, 16,
                       sizeof(ngx_nacos_service_addr_t)) != NGX_OK) {
        ngx_destroy_pool(pool);
        return NULL;
    }
    return peers;
}

static ngx_int_t ngx_http_nacos_add_server(ngx_http_nacos_peers_t *peers,
                                           ngx_http_nacos_srv_conf_t *nscf,
                                           ngx_pool_t *temp_pool) {
    ngx_http_upstream_server_t *server;
    ngx_http_upstream_srv_conf_t *us;
    ngx_nacos_service_addr_t *adr;
    ngx_uint_t i, n;
    ngx_url_t u;
    ngx_conf_t cf;

    n = peers->addrs.addrs.nelts;
    adr = peers->addrs.addrs.elts;
    for (i = 0; i < n; ++i) {
        ngx_memzero(&u, sizeof(u));
        u.url = adr[i].host;
        u.default_port = adr[i].port;
        if (ngx_parse_url(peers->pool, &u) != NGX_OK) {
            continue;
        }

        us = ngx_http_nacos_select_upstream(peers, &adr[i].cluster);
        if (us == NULL) {
            return NGX_ERROR;
        }

        server = ngx_array_push(us->servers);
        if (server == NULL) {
            return NGX_ERROR;
        }
        ngx_memzero(server, sizeof(*server));
        server->addrs = u.addrs;
        server->naddrs = u.naddrs;
        server->name = u.url;
        server->weight =
            (ngx_uint_t) (adr[i].weight / 0.01 * (double) nscf->weight);
        server->max_fails = nscf->max_fails;
        server->fail_timeout = nscf->fail_timeout;
    }

    if (peers->addrs.addrs.nelts > 0) {
        if (peers->us) {
            memset(&cf, 0, sizeof(cf));
            cf.pool = peers->pool;
            cf.temp_pool = temp_pool;
            cf.log = temp_pool->log;
            if (nscf->original_init_upstream(&cf, peers->us) != NGX_OK) {
                return NGX_ERROR;
            }
        } else {
            us = peers->clustered_us->elts;
            n = peers->clustered_us->nelts;
            for (i = 0; i < n; ++i) {
                memset(&cf, 0, sizeof(cf));
                cf.pool = peers->pool;
                cf.temp_pool = temp_pool;
                cf.log = temp_pool->log;
                if (nscf->original_init_upstream(&cf, us + i) != NGX_OK) {
                    return NGX_ERROR;
                }
            }
        }
    }
    return NGX_OK;
}

static ngx_int_t ngx_http_nacos_init_upstream(
    ngx_conf_t *cf, ngx_http_upstream_srv_conf_t *us) {
    ngx_nacos_sub_t sub;
    ngx_pool_t *pool;
    ngx_http_nacos_peers_t *peers;
    ngx_http_nacos_srv_conf_t *ncf;

    ncf = ngx_http_conf_upstream_srv_conf(us, ngx_http_nacos_upstream_module);

    peers = ngx_http_nacos_create_peers(cf->log, ncf->cluster_size > 0, us);
    if (peers == NULL) {
        return NGX_ERROR;
    }

    pool = peers->pool;

    sub.key_ptr = &peers->key;
    sub.data_id = ncf->data_id;
    sub.group = ncf->group;
    if (ngx_nacos_subscribe_naming(cf, &sub) != NGX_OK) {
        ngx_destroy_pool(pool);
        return NGX_ERROR;
    }

    if (!ngx_nacos_shmem_change(peers->key, peers->version)) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "nacos no addrs????");
        ngx_destroy_pool(pool);
        return NGX_ERROR;
    }

    if (nax_nacos_get_addrs(peers->key, &peers->version, &peers->addrs) !=
        NGX_OK) {
        ngx_destroy_pool(pool);
        return NGX_ERROR;
    }

    if (ngx_http_nacos_add_server(peers, ncf, cf->temp_pool) != NGX_OK) {
        ngx_destroy_pool(pool);
        return NGX_ERROR;
    }

    us->peer.init = ngx_http_nacos_init_peers;
    us->peer.data = peers;
    return NGX_OK;
}

static ngx_http_upstream_srv_conf_t *ngx_http_nacos_select_upstream(
    ngx_http_nacos_peers_t *peers, ngx_str_t *cluster) {
    ngx_http_upstream_srv_conf_t *us;
    ngx_uint_t i, n;

    if (!peers->use_cluster) {
        us = peers->us;
        if (us == NULL) {
            us = ngx_palloc(peers->pool, sizeof(*us));
            if (us == NULL) {
                return NULL;
            }
            *us = *peers->origin;
            us->servers = ngx_array_create(peers->pool, 16,
                                           sizeof(ngx_http_upstream_server_t));
            if (us->servers == NULL) {
                return NULL;
            }
            peers->us = us;
            ngx_str_set(&us->host, "nacos-no-cluster");
        }
    } else {
        if (peers->clustered_us == NULL) {
            peers->clustered_us = ngx_array_create(peers->pool, 4, sizeof(*us));
            if (peers->clustered_us == NULL) {
                return NULL;
            }
        }
        us = peers->clustered_us->elts;
        n = peers->clustered_us->nelts;
        if (n > 0) {
            for (i = 0; i < n; ++i) {
                if (us[i].host.len == cluster->len &&
                    ngx_strncmp(us[i].host.data, cluster->data, cluster->len) ==
                        0) {
                    return us + i;
                }
            }
        }

        us = ngx_array_push(peers->clustered_us);
        if (us == NULL) {
            return NULL;
        }
        *us = *peers->origin;

        us->servers = ngx_array_create(peers->pool, 16,
                                       sizeof(ngx_http_upstream_server_t));
        if (us->servers == NULL) {
            return NULL;
        }

        us->host.len = cluster->len;
        us->host.data = ngx_palloc(peers->pool, cluster->len + 1);
        if (us->host.data == NULL) {
            return NULL;
        }
        ngx_memcpy(us->host.data, cluster->data, cluster->len);
        us->host.data[cluster->len] = 0;
    }

    return us;
}

static ngx_int_t ngx_http_nacos_init_peers(ngx_http_request_t *r,
                                           ngx_http_upstream_srv_conf_t *us) {
    ngx_http_nacos_peers_t *peers;
    ngx_http_nacos_rrp_t *rrp;
    ngx_http_upstream_srv_conf_t *selected_us, *uc;
    ngx_str_t cluster;
    ngx_http_nacos_srv_conf_t *nusf;
    ngx_int_t rc;
    ngx_uint_t i, n;
    ngx_http_nacos_cluster_t *ct;
    ngx_uint_t ci, cn;
    ngx_http_nacos_ctx_t *ctx;

    ctx = ngx_http_get_module_ctx(r, ngx_http_nacos_upstream_module);
    if (ctx == NULL) {
        ctx = ngx_pcalloc(r->pool, sizeof(*ctx));
        if (ctx == NULL) {
            return NGX_ERROR;
        }
        ngx_http_set_ctx(r, ctx, ngx_http_nacos_upstream_module);
    }

    rrp = r->upstream->peer.data;
    if (rrp == NULL) {
        rrp = ngx_palloc(r->pool, sizeof(*rrp));
        if (rrp == NULL) {
            return NGX_ERROR;
        }
    } else {
        r->upstream->peer.data = NULL;
    }

    peers = ngx_http_get_nacos_peers(r, us);
    if (peers == NULL || peers->addrs.addrs.nelts == 0) {
        return NGX_ERROR;
    }

    nusf = ngx_http_conf_upstream_srv_conf(us, ngx_http_nacos_upstream_module);

    if (!peers->use_cluster) {
        selected_us = peers->us;
    } else {
        selected_us = NULL;
        ct = nusf->clusters;
        for (ci = 0, cn = nusf->cluster_size; ci < cn; ++ci) {
            ngx_memzero(&cluster, sizeof(cluster));
            if (ct->cluster_lengths == NULL) {
                cluster = ct->name;
            } else {
                if (ngx_http_script_run(r, &cluster, ct->cluster_lengths->elts,
                                        0, ct->cluster_values->elts) == NULL) {
                    return NGX_ERROR;
                }
            }
            if (peers->clustered_us == NULL ||
                peers->clustered_us->nelts == 0) {
                return NGX_ERROR;
            }
            n = peers->clustered_us->nelts;
            uc = peers->clustered_us->elts;
            for (i = 0; i < n; ++i) {
                if (cluster.len == uc[i].host.len &&
                    ngx_strncmp(cluster.data, uc[i].host.data, cluster.len) ==
                        0) {
                    selected_us = uc + i;
                    ctx->cluster = cluster;
                    goto found;
                }
            }

            ct++;
        }
    }
found:
    if (selected_us == NULL) {
        return NGX_ERROR;
    }

    rc = selected_us->peer.init(r, selected_us);
    if (rc != NGX_OK) {
        return rc;
    }

    rrp->peers = peers;
    rrp->original_data = r->upstream->peer.data;
    rrp->original_get_peer = r->upstream->peer.get;
    rrp->original_free_peer = r->upstream->peer.free;

    r->upstream->peer.data = rrp;
    r->upstream->peer.get = ngx_http_nacos_get_peer;
    r->upstream->peer.free = ngx_http_nacos_free_peer;
#if (NGX_HTTP_SSL)
    rrp->original_set_session = r->upstream->peer.set_session;
    rrp->original_save_session = r->upstream->peer.save_session;
    r->upstream->peer.set_session = ngx_http_nacos_peer_session;
    r->upstream->peer.save_session = ngx_http_nacos_save_peer_session;
#endif

    return NGX_OK;
}

static ngx_http_nacos_peers_t *ngx_http_get_nacos_peers(
    ngx_http_request_t *r, ngx_http_upstream_srv_conf_t *us) {
    ngx_http_nacos_peers_t *peers, *new_peers;
    ngx_http_nacos_srv_conf_t *nusf;
    ngx_int_t rc;

    peers = us->peer.data;

    if (!ngx_nacos_shmem_change(peers->key, peers->version)) {
        return peers;
    }

    new_peers =
        ngx_http_nacos_create_peers(r->pool->log, peers->use_cluster, us);
    if (new_peers == NULL) {
        return NULL;
    }
    new_peers->key = peers->key;
    new_peers->version = peers->version;
    rc = nax_nacos_get_addrs(new_peers->key, &new_peers->version,
                             &new_peers->addrs);
    if (rc != NGX_OK) {
        ngx_destroy_pool(new_peers->pool);
        return NULL;
    }

    nusf = ngx_http_conf_upstream_srv_conf(us, ngx_http_nacos_upstream_module);
    if (ngx_http_nacos_add_server(new_peers, nusf, r->pool) != NGX_OK) {
        ngx_destroy_pool(new_peers->pool);
        return NULL;
    }

    us->peer.data = new_peers;
    if (--peers->ref == 0) {
        ngx_destroy_pool(peers->pool);
    }
    return new_peers;
}

static ngx_int_t ngx_http_nacos_get_peer(ngx_peer_connection_t *pc,
                                         void *data) {
    ngx_http_nacos_rrp_t *rrp;
    ngx_int_t rc;

    rrp = data;
    if ((rc = rrp->original_get_peer(pc, rrp->original_data)) != NGX_OK) {
        return rc;
    }

    rrp->peers->ref++;
    return NGX_OK;
}

static void ngx_http_nacos_free_peer(ngx_peer_connection_t *pc, void *data,
                                     ngx_uint_t state) {
    ngx_http_nacos_rrp_t *rrp;
    ngx_http_nacos_peers_t *peers;

    rrp = data;
    peers = rrp->peers;
    rrp->original_free_peer(pc, rrp->original_data, state);
    if (--peers->ref == 0) {
        rrp->peers = NULL;
        ngx_destroy_pool(peers->pool);
    }
}

#if (NGX_HTTP_SSL)

static ngx_int_t ngx_http_nacos_peer_session(ngx_peer_connection_t *pc,
                                             void *data) {
    ngx_http_nacos_rrp_t *rrp = data;
    return rrp->original_set_session(pc, rrp->original_data);
}

static void ngx_http_nacos_save_peer_session(ngx_peer_connection_t *pc,
                                             void *data) {
    ngx_http_nacos_rrp_t *rrp = data;
    rrp->original_save_session(pc, rrp->original_data);
}

#endif

static ngx_int_t ngx_http_nacos_add_variables(ngx_conf_t *cf) {
    ngx_http_variable_t *var, *v;

    for (v = ngx_http_nacos_vars; v->name.len; v++) {
        var = ngx_http_add_variable(cf, &v->name, v->flags);
        if (var == NULL) {
            return NGX_ERROR;
        }

        var->get_handler = v->get_handler;
        var->data = v->data;
    }

    return NGX_OK;
}

static ngx_int_t ngx_http_nacos_real_cluster_variable(
    ngx_http_request_t *r, ngx_http_variable_value_t *v, uintptr_t data) {
    ngx_http_nacos_ctx_t *ctx;

    v->valid = 1;
    v->no_cacheable = 0;
    v->not_found = 0;

    ctx = ngx_http_get_module_ctx(r, ngx_http_nacos_upstream_module);
    if (ctx == NULL || ctx->cluster.len == 0) {
        v->not_found = 1;
        return NGX_OK;
    }

    v->len = ctx->cluster.len;
    v->data = ctx->cluster.data;
    return NGX_OK;
}
