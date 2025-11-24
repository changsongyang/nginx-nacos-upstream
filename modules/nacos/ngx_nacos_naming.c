//
// Created by pc on 6/27/25.
//

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_nacos_grpc.h>
#include <ngx_nacos_data.h>
#include <yaij/api/yajl_gen.h>
#include <yaij/api/yajl_parse.h>
#include <yaij/api/yajl_tree.h>

struct {
    ngx_nacos_main_conf_t *nmcf;
    ngx_nacos_grpc_conn_t *conn;
    ngx_event_t reconnect_timer;
    ngx_uint_t reconnect_time;
    ngx_event_t subscribe_timer;
    ngx_event_t health_timer;
    ngx_flag_t support_ability_negotiation;
#ifdef NGX_HAVE_NACOS_DYNAMIC_KEY
    ngx_rbtree_t dynamic_keys;
    ngx_rbtree_node_t dynamic_keys_sentinel;
#endif
} ngx_nacos_naming;

#ifdef NGX_HAVE_NACOS_DYNAMIC_KEY

typedef struct {
    ngx_str_node_t node;
    ngx_event_t event;
    ngx_flag_t naming;
    ngx_nacos_dynamic_key_t key;
} ngx_nacos_dynamic_naming_key_t;

#define nacos_naming_dynamic_key(key)                                       \
    (ngx_nacos_dynamic_naming_key_t *) ((u_char *) (key) -                  \
                                        offsetof(                           \
                                            ngx_nacos_dynamic_naming_key_t, \
                                            key))

static ngx_int_t ngx_nacos_naming_subscribe_all_dynamic_keys(
    ngx_nacos_grpc_conn_t *gc);
static ngx_int_t ngx_nacos_naming_subscribe_dynamic_key(
    ngx_nacos_grpc_conn_t *gc, ngx_nacos_dynamic_naming_key_t *dnk,
    int subscribe);

static void ngx_nacos_naming_notify_dynamic_key(u_char *full_data_id,
                                                size_t len,
                                                ngx_nacos_key_t *nk);

static void ngx_nacos_dynamic_naming_post_handler(ngx_event_t *ev);
#endif

static ngx_int_t ngx_nacos_grpc_naming_conn_handler(
    ngx_nacos_grpc_conn_t *conn, enum ngx_nacos_grpc_event_state state);

static ngx_int_t ngx_nacos_grpc_naming_bi_handler(ngx_nacos_grpc_stream_t *st,
                                                  ngx_nacos_payload_t *p);

static ngx_int_t ngx_nacos_server_check_handler(ngx_nacos_grpc_stream_t *st,
                                                ngx_nacos_payload_t *p);

static void ngx_nacos_naming_reconnect_timer_handler(ngx_event_t *ev);

static void ngx_nacos_naming_subscribe_timer_handler(ngx_event_t *ev);

static ngx_int_t ngx_nacos_subscribe_service_handler(
    ngx_nacos_grpc_stream_t *st, ngx_nacos_payload_t *p);

static ngx_int_t ngx_nacos_naming_register_handler(ngx_nacos_grpc_stream_t *st,
                                                   ngx_nacos_payload_t *p);

static ngx_int_t ngx_nacos_grpc_notify_address_shm(ngx_nacos_grpc_conn_t *gc,
                                                   yajl_val json);

static void ngx_nacos_naming_health_handler(ngx_event_t *ev);

ngx_int_t ngx_nacos_naming_init(ngx_nacos_main_conf_t *nmcf) {
    ngx_nacos_naming.nmcf = nmcf;
    ngx_nacos_naming.reconnect_timer.log = nmcf->error_log;
    ngx_nacos_naming.reconnect_timer.handler =
        ngx_nacos_naming_reconnect_timer_handler;
    ngx_nacos_naming.subscribe_timer.log = nmcf->error_log;
    ngx_nacos_naming.subscribe_timer.handler =
        ngx_nacos_naming_subscribe_timer_handler;
    ngx_nacos_naming.health_timer.log = nmcf->error_log;
    ngx_nacos_naming.health_timer.handler = ngx_nacos_naming_health_handler;

#ifdef NGX_HAVE_NACOS_DYNAMIC_KEY
    ngx_rbtree_init(&ngx_nacos_naming.dynamic_keys,
                    &ngx_nacos_naming.dynamic_keys_sentinel,
                    ngx_str_rbtree_insert_value);
#endif

    ngx_nacos_naming_reconnect_timer_handler(&ngx_nacos_naming.reconnect_timer);
    return NGX_OK;
}

static void ngx_nacos_naming_reconnect_timer_handler(ngx_event_t *ev) {
    ngx_nacos_grpc_conn_t *conn;
    if (ngx_nacos_naming.reconnect_time == 0) {
        ngx_nacos_naming.reconnect_time = 3000;
    } else {
        ngx_nacos_naming.reconnect_time = ngx_nacos_naming.reconnect_time * 2;
    }

    if (ngx_nacos_naming.health_timer.timer_set) {
        ngx_del_timer(&ngx_nacos_naming.health_timer);
    }

    if (ngx_nacos_naming.subscribe_timer.timer_set) {
        ngx_del_timer(&ngx_nacos_naming.subscribe_timer);
    }

    conn = ngx_nacos_naming.conn;
    if (conn != NULL) {
        ngx_nacos_naming.conn = NULL;
        ngx_nacos_grpc_close_connection(conn, NGX_NC_TIRED);
    }

    conn = ngx_nacos_open_grpc_conn(ngx_nacos_naming.nmcf,
                                    ngx_nacos_grpc_naming_conn_handler);
    if (conn == NULL) {
        ngx_add_timer(&ngx_nacos_naming.reconnect_timer,
                      ngx_nacos_naming.reconnect_time);
        return;
    }
    ngx_nacos_naming.conn = conn;
}

static void ngx_nacos_naming_health_handler(ngx_event_t *ev) {
    ngx_nacos_grpc_stream_t *st;
    ngx_nacos_payload_t payload = {
        .type = HealthCheckRequest,
        .end = 1,
    };

    if (ngx_nacos_naming.conn == NULL) {
        return;
    }

    ngx_str_set(&payload.json_str, NACOS_INTERNAL_REQUEST);
    st = ngx_nacos_grpc_request(ngx_nacos_naming.conn, NULL);
    if (st == NULL) {
        ngx_add_timer(&ngx_nacos_naming.reconnect_timer,
                      ngx_nacos_naming.reconnect_time);
        return;
    }
    if (ngx_nacos_grpc_send(st, &payload) != NGX_OK) {
        ngx_nacos_grpc_close_stream(st, 1);
        ngx_add_timer(&ngx_nacos_naming.reconnect_timer,
                      ngx_nacos_naming.reconnect_time);
    } else {
        ngx_add_timer(&ngx_nacos_naming.health_timer, 10000);
    }
}

static void ngx_nacos_naming_subscribe_timer_handler(ngx_event_t *ev) {
    ngx_nacos_grpc_stream_t *st;
    ngx_nacos_key_t **key;
    ngx_uint_t idx, len, mi, ml;
    yajl_gen gen;
    ngx_nacos_register_t **nrp;
    ngx_str_t *m;
    ngx_int_t rc;

    ngx_nacos_payload_t payload = {
        .type = SubscribeServiceRequest,
        .end = 1,
    };

    gen = NULL;
    rc = NGX_ERROR;
    key = ngx_nacos_naming.nmcf->keys.elts;

    if (ngx_nacos_naming.conn == NULL) {
        return;
    }

    for (idx = 0, len = ngx_nacos_naming.nmcf->keys.nelts; idx < len; idx++) {
        gen = yajl_gen_alloc(NULL);
        if (gen == NULL) {
            goto free;
        }

        if (yajl_gen_map_open(gen) != yajl_gen_status_ok) {
            goto free;
        }

        //  headers: {}
        if (yajl_gen_string(gen, (u_char *) "headers", sizeof("headers") - 1) !=
                yajl_gen_status_ok ||
            yajl_gen_map_open(gen) != yajl_gen_status_ok ||
            yajl_gen_map_close(gen) != yajl_gen_status_ok) {
            goto free;
        }

        if (yajl_gen_string(gen, (u_char *) "namespace",
                            sizeof("namespace") - 1) != yajl_gen_status_ok ||
            yajl_gen_string(gen, ngx_nacos_naming.nmcf->service_namespace.data,
                            ngx_nacos_naming.nmcf->service_namespace.len) !=
                yajl_gen_status_ok) {
            goto free;
        }

        if (yajl_gen_string(gen, (u_char *) "serviceName",
                            sizeof("serviceName") - 1) != yajl_gen_status_ok ||
            yajl_gen_string(gen, key[idx]->data_id.data,
                            key[idx]->data_id.len) != yajl_gen_status_ok) {
            goto free;
        }

        if (yajl_gen_string(gen, (u_char *) "groupName",
                            sizeof("groupName") - 1) != yajl_gen_status_ok ||
            yajl_gen_string(gen, key[idx]->group.data, key[idx]->group.len) !=
                yajl_gen_status_ok) {
            goto free;
        }

        if (yajl_gen_string(gen, (u_char *) "subscribe",
                            sizeof("subscribe") - 1) != yajl_gen_status_ok ||
            yajl_gen_bool(gen, 1) != yajl_gen_status_ok) {
            goto free;
        }

        if (yajl_gen_string(gen, (u_char *) "clusters",
                            sizeof("clusters") - 1) != yajl_gen_status_ok ||
            yajl_gen_string(gen, (u_char *) "", 0) != yajl_gen_status_ok) {
            goto free;
        }

        if (yajl_gen_map_close(gen) != yajl_gen_status_ok) {
            goto free;
        }
        if (yajl_gen_get_buf(gen, (const u_char **) &payload.json_str.data,
                             &payload.json_str.len) != yajl_gen_status_ok) {
            goto free;
        }

        st = ngx_nacos_grpc_request(ngx_nacos_naming.conn,
                                    ngx_nacos_subscribe_service_handler);
        if (st == NULL) {
            goto free;
        }
        st->handler_ctx = key[idx];
        if (ngx_nacos_grpc_send(st, &payload) != NGX_OK) {
            ngx_nacos_grpc_close_stream(st, 1);
            goto free;
        }
        yajl_gen_free(gen);
        gen = NULL;
    }

#ifdef NGX_HAVE_NACOS_DYNAMIC_KEY
    (void) ngx_nacos_naming_subscribe_all_dynamic_keys(ngx_nacos_naming.conn);
#endif

    // register service
    len = ngx_nacos_naming.nmcf->register_services.nelts;
    if (len > 0) {
        nrp = ngx_nacos_naming.nmcf->register_services.elts;
        payload.type = InstanceRequest;

        for (idx = 0; idx < len; idx++) {
            gen = yajl_gen_alloc(NULL);
            if (gen == NULL) {
                goto free;
            }
            if (yajl_gen_map_open(gen) != yajl_gen_status_ok ||
                yajl_gen_string(gen, (const unsigned char *) "module",
                                sizeof("module") - 1) != yajl_gen_status_ok ||
                yajl_gen_string(gen, (const unsigned char *) "naming",
                                sizeof("naming") - 1) != yajl_gen_status_ok ||
                yajl_gen_string(gen, (const unsigned char *) "type", 4) !=
                    yajl_gen_status_ok ||
                yajl_gen_string(gen, (const unsigned char *) "registerInstance",
                                sizeof("registerInstance") - 1) !=
                    yajl_gen_status_ok ||
                yajl_gen_string(gen, (const unsigned char *) "serviceName",
                                11) != yajl_gen_status_ok ||
                yajl_gen_string(gen, nrp[idx]->data_id.data,
                                nrp[idx]->data_id.len) != yajl_gen_status_ok ||
                yajl_gen_string(gen, (const unsigned char *) "groupName", 9) !=
                    yajl_gen_status_ok ||
                yajl_gen_string(gen, nrp[idx]->group.data,
                                nrp[idx]->group.len) != yajl_gen_status_ok ||
                yajl_gen_string(gen, (const unsigned char *) "namespace", 9) !=
                    yajl_gen_status_ok ||
                yajl_gen_string(gen,
                                ngx_nacos_naming.nmcf->service_namespace.data,
                                ngx_nacos_naming.nmcf->service_namespace.len) !=
                    yajl_gen_status_ok) {
                goto free;
            }
            if (yajl_gen_string(gen, (const unsigned char *) "instance",
                                sizeof("instance") - 1) != yajl_gen_status_ok ||
                yajl_gen_map_open(gen) != yajl_gen_status_ok) {
                goto free;
            }

            if (yajl_gen_string(gen, (const unsigned char *) "ip", 2) !=
                    yajl_gen_status_ok ||
                yajl_gen_string(gen, nrp[idx]->ip.data, nrp[idx]->ip.len) !=
                    yajl_gen_status_ok ||
                yajl_gen_string(gen, (const unsigned char *) "port", 4) !=
                    yajl_gen_status_ok ||
                yajl_gen_integer(gen, (long long int) nrp[idx]->port) !=
                    yajl_gen_status_ok ||
                yajl_gen_string(gen, (const unsigned char *) "weight", 6) !=
                    yajl_gen_status_ok ||
                yajl_gen_double(gen, (double) nrp[idx]->weight / 100.0) !=
                    yajl_gen_status_ok ||
                yajl_gen_string(gen, (const unsigned char *) "healthy", 7) !=
                    yajl_gen_status_ok ||
                yajl_gen_bool(gen, nrp[idx]->healthy != 0) !=
                    yajl_gen_status_ok ||
                yajl_gen_string(gen, (const unsigned char *) "ephemeral", 9) !=
                    yajl_gen_status_ok ||
                yajl_gen_bool(gen, 1) != yajl_gen_status_ok ||
                yajl_gen_string(gen, (const unsigned char *) "clusterName",
                                11) != yajl_gen_status_ok ||
                yajl_gen_string(gen, nrp[idx]->cluster.data,
                                nrp[idx]->cluster.len) != yajl_gen_status_ok ||
                yajl_gen_string(gen, (const unsigned char *) "metadata", 8) !=
                    yajl_gen_status_ok ||
                yajl_gen_map_open(gen) != yajl_gen_status_ok) {
                goto free;
            }

            m = nrp[idx]->metadata.elts;
            for (ml = nrp[idx]->metadata.nelts, mi = 0; mi < ml; mi += 2) {
                if (yajl_gen_string(gen, m[mi].data, m[mi].len) !=
                        yajl_gen_status_ok ||
                    yajl_gen_string(gen, m[mi + 1].data, m[mi + 1].len) !=
                        yajl_gen_status_ok) {
                    goto free;
                }
            }
            if (yajl_gen_map_close(gen) !=
                yajl_gen_status_ok) {  // close metadata
                goto free;
            }
            if (yajl_gen_map_close(gen) !=
                yajl_gen_status_ok) {  // close instance
                goto free;
            }
            if (yajl_gen_map_close(gen) != yajl_gen_status_ok) {  // close root
                goto free;
            }

            if (yajl_gen_get_buf(
                    gen, (const unsigned char **) &payload.json_str.data,
                    &payload.json_str.len) != yajl_gen_status_ok) {
                goto free;
            }

            st = ngx_nacos_grpc_request(ngx_nacos_naming.conn,
                                        ngx_nacos_naming_register_handler);
            if (st == NULL) {
                goto free;
            }
            st->handler_ctx = nrp[idx];
            if (ngx_nacos_grpc_send(st, &payload)) {
                ngx_nacos_grpc_close_stream(st, 1);
                goto free;
            }
            yajl_gen_free(gen);
            gen = NULL;
        }
    }

    ngx_add_timer(&ngx_nacos_naming.health_timer, 10000);
    rc = NGX_OK;
free:
    if (gen != NULL) {
        yajl_gen_free(gen);
    }

    if (rc != NGX_OK) {
        ngx_add_timer(&ngx_nacos_naming.reconnect_timer,
                      ngx_nacos_naming.reconnect_time);
    }
}

static ngx_int_t ngx_nacos_subscribe_service_handler(
    ngx_nacos_grpc_stream_t *st, ngx_nacos_payload_t *p) {
    ngx_nacos_key_t *key;
    const char *act;

    key = st->handler_ctx;
    act = key->ctx ? "subscribe" : "unsubscribe";

    if (p->msg_state != pl_success || p->type != SubscribeServiceResponse) {
        ngx_log_error(NGX_LOG_ERR, st->conn->conn->log, 0,
                      "nacos grpc %s naming %V@@%V error", act, &key->data_id,
                      &key->group);
        return NGX_ERROR;
    }
    ngx_log_error(NGX_LOG_INFO, st->conn->conn->log, 0,
                  "nacos grpc %s naming %V@@%V successfully", act,
                  &key->data_id, &key->group);
    return NGX_DONE;
}

static ngx_int_t ngx_nacos_naming_register_handler(ngx_nacos_grpc_stream_t *st,
                                                   ngx_nacos_payload_t *p) {
    ngx_nacos_register_t *nr;
    nr = st->handler_ctx;
    if (p->msg_state != pl_success || p->type != InstanceResponse) {
        ngx_log_error(NGX_LOG_ERR, st->conn->conn->log, 0,
                      "nacos grpc register service %V@@%V error", &nr->data_id,
                      &nr->group);
        return NGX_ERROR;
    }

    ngx_log_error(NGX_LOG_INFO, st->conn->conn->log, 0,
                  "nacos grpc register service %V@@%V successfully",
                  &nr->data_id, &nr->group);
    return NGX_DONE;
}

static ngx_int_t ngx_nacos_grpc_naming_conn_handler(
    ngx_nacos_grpc_conn_t *conn, enum ngx_nacos_grpc_event_state state) {
    ngx_nacos_grpc_stream_t *st;
    ngx_nacos_payload_t payload = {
        .type = ServerCheckRequest,
        .end = 1,
    };

    if (conn != ngx_nacos_naming.conn) {
        return NGX_ERROR;
    }

    if (state != nc_connected) {
        ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0,
                      "nacos grpc naming conn error");
        ngx_add_timer(&ngx_nacos_naming.reconnect_timer,
                      ngx_nacos_naming.reconnect_time);
        ngx_nacos_naming.conn = NULL;

        if (ngx_nacos_naming.health_timer.timer_set) {
            ngx_del_timer(&ngx_nacos_naming.health_timer);
        }

        if (ngx_nacos_naming.subscribe_timer.timer_set) {
            ngx_del_timer(&ngx_nacos_naming.subscribe_timer);
        }

        return NGX_ERROR;
    }
    ngx_nacos_naming.reconnect_time = 0;

    st = ngx_nacos_grpc_request(conn, ngx_nacos_server_check_handler);
    if (st == NULL) {
        return NGX_ERROR;
    }

    ngx_str_set(&payload.json_str, NACOS_INTERNAL_REQUEST);
    return ngx_nacos_grpc_send(st, &payload);
}

static ngx_int_t ngx_nacos_grpc_naming_bi_handler(ngx_nacos_grpc_stream_t *st,
                                                  ngx_nacos_payload_t *p) {
    ngx_nacos_payload_t payload = {
        .end = 0,
    };
    u_char buf[256];
    yajl_val val, requestId;
    val = NULL;

    if (p->msg_state != pl_success) {
        return NGX_ERROR;
    }

    if (p->type == SetupAckRequest) {
        if (ngx_nacos_naming.subscribe_timer.timer_set) {
            ngx_del_timer(&ngx_nacos_naming.subscribe_timer);
        }

        if (ngx_nacos_naming.support_ability_negotiation) {
            ngx_nacos_naming_subscribe_timer_handler(
                &ngx_nacos_naming.subscribe_timer);
        }

        return NGX_OK;
    }

    if (p->type == NotifySubscriberRequest) {
        val = yajl_tree_parse_with_len((const char *) p->json_str.data,
                                       p->json_str.len);
        if (val == NULL) {
            goto err;
        }

        if (ngx_nacos_grpc_notify_address_shm(st->conn, val) == NGX_ERROR) {
            ngx_log_error(NGX_LOG_WARN, st->conn->conn->log, 0,
                          "nacos grpc notify address error");
        }

        requestId = yajl_tree_get_field(val, "requestId", yajl_t_string);
        if (requestId == NULL) {
            ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0,
                          "NotifySubscriberRequest has no request id");
            goto err;
        }

        payload.type = NotifySubscriberResponse;
        payload.json_str.len =
            ngx_snprintf(buf, sizeof(buf),
                         "{\"resultCode\":200,\"requestId\":\"%s\"}",
                         YAJL_GET_STRING(requestId)) -
            buf;
        payload.json_str.data = buf;
        if (ngx_nacos_grpc_send(st, &payload) != NGX_OK) {
            goto err;
        }
    } else if (p->type == ClientDetectionRequest) {
        val = yajl_tree_parse_with_len((const char *) p->json_str.data,
                                       p->json_str.len);
        requestId = yajl_tree_get_field(val, "requestId", yajl_t_string);
        if (requestId == NULL) {
            ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0,
                          "NotifySubscriberRequest has no request id");
            goto err;
        }
        payload.type = ClientDetectionResponse;
        payload.json_str.len =
            ngx_snprintf(buf, sizeof(buf),
                         "{\"resultCode\":200,\"requestId\":\"%s\"}",
                         YAJL_GET_STRING(requestId)) -
            buf;
        payload.json_str.data = buf;
        if (ngx_nacos_grpc_send(st, &payload) != NGX_OK) {
            goto err;
        }
    }

    if (val != NULL) {
        yajl_tree_free(val);
    }

    return NGX_OK;

err:
    if (val != NULL) {
        yajl_tree_free(val);
    }

    return NGX_ERROR;
}

static ngx_int_t ngx_nacos_server_check_handler(ngx_nacos_grpc_stream_t *st,
                                                ngx_nacos_payload_t *p) {
    yajl_val val, san;
    ngx_nacos_grpc_stream_t *bi_st;
    ngx_nacos_payload_t payload = {
        .type = ConnectionSetupRequest,
        .end = 0,
    };

    if (p->msg_state != pl_success || p->type != ServerCheckResponse) {
        return NGX_ERROR;
    }

    bi_st = NULL;
    val = yajl_tree_parse_with_len((const char *) p->json_str.data,
                                   p->json_str.len);
    if (ngx_nacos_check_response(st->conn->conn->log, val) != NGX_OK) {
        goto err;
    }

    san = yajl_tree_get_field(val, "supportAbilityNegotiation", yajl_t_true);
    ngx_nacos_naming.support_ability_negotiation = YAJL_IS_TRUE(san) ? 1 : 0;

    bi_st =
        ngx_nacos_grpc_bi_request(st->conn, ngx_nacos_grpc_naming_bi_handler);
    if (bi_st == NULL) {
        goto err;
    }

    ngx_str_set(&payload.json_str,
                "{\"tenant\":\"\",\"clientVersion\":\"nginx-nacos-module:v2.10."
                "0\",\"abilityTable\":{},"
                "\"labels\":{\"source\":\"sdk\",\"module\":\"naming\"}}");

    if (ngx_nacos_grpc_send(bi_st, &payload) != NGX_OK) {
        goto err;
    }
    if (!ngx_nacos_naming.support_ability_negotiation) {
        ngx_add_timer(&ngx_nacos_naming.subscribe_timer, 1000);
    }
    if (val != NULL) {
        yajl_tree_free(val);
    }
    return NGX_DONE;
err:
    if (val != NULL) {
        yajl_tree_free(val);
    }
    if (bi_st != NULL) {
        ngx_nacos_grpc_close_stream(bi_st, 1);
    }
    return NGX_ERROR;
}

static ngx_int_t ngx_nacos_grpc_notify_address_shm(ngx_nacos_grpc_conn_t *gc,
                                                   yajl_val json) {
    yajl_val s_name, g_name;
    ngx_nacos_resp_json_parser_t parser;
    ngx_nacos_key_t *key;
    ngx_pool_t *pool;
    ngx_int_t rc;
    char *adr;
    u_char *tmp;
    ngx_nacos_data_t cache;
    size_t len;

    pool = NULL;
    rc = NGX_ERROR;

    if (json == NULL) {
        goto err;
    }

    pool = ngx_create_pool(512, gc->conn->log);
    if (pool == NULL) {
        goto err;
    }

    ngx_memzero(&parser, sizeof(parser));
    parser.json = yajl_tree_get_field(json, "serviceInfo", yajl_t_object);
    if (parser.json == NULL) {
        goto err;
    }
    s_name = yajl_tree_get_field(parser.json, "name", yajl_t_string);
    if (s_name == NULL) {
        goto err;
    }
    g_name = yajl_tree_get_field(parser.json, "groupName", yajl_t_string);
    if (g_name == NULL) {
        goto err;
    }

    len = ngx_strlen(g_name->u.string) + ngx_strlen(s_name->u.string) + 2;
    tmp = ngx_palloc(pool, len + 1);
    if (tmp == NULL) {
        goto err;
    }
    ngx_sprintf(tmp, "%s@@%s", g_name->u.string, s_name->u.string);
    tmp[len] = 0;

    parser.pool = pool;
    parser.log = pool->log;

    key = ngx_nacos_hash_find_key(ngx_nacos_naming.nmcf->key_hash, tmp);
    if (key != NULL) {
        parser.prev_version = ngx_nacos_shmem_version(key);
        adr = ngx_nacos_parse_addrs_from_json(&parser);
        if (adr == NULL) {
            goto err;
        }
        rc = ngx_nacos_update_shm(ngx_nacos_naming.nmcf, key, adr, pool->log);
        if (rc == NGX_OK) {
            ngx_log_error(NGX_LOG_INFO, pool->log, 0,
                          "nacos service %V@@%V is updated!!!", &key->group,
                          &key->data_id);
        }
        cache.pool = pool;
        cache.data_id = key->data_id;
        cache.group = key->group;
        cache.version = parser.current_version;
        cache.adr = adr;
        rc = ngx_nacos_write_disk_data(ngx_nacos_naming.nmcf, &cache);
    } else {
        key = ngx_pcalloc(pool, sizeof(ngx_nacos_key_t));
        if (key == NULL) {
            goto err;
        }
        key->data_id.len = ngx_strlen(s_name->u.string);
        key->data_id.data = tmp;
        key->group.len = ngx_strlen(g_name->u.string);
        key->group.data = tmp + key->data_id.len + 2;
        key->ctx = ngx_palloc(pool, sizeof(ngx_nacos_key_ctx_t));
        if (key->ctx == NULL) {
            goto err;
        }
        adr = ngx_nacos_parse_addrs_from_json(&parser);
        if (adr == NULL) {
            goto err;
        }
        key->ctx->data = adr;
        key->ctx->version = parser.current_version;
        rc = NGX_OK;
    }

#ifdef NGX_HAVE_NACOS_DYNAMIC_KEY
    ngx_nacos_naming_notify_dynamic_key(tmp, len, key);
#endif

err:
    if (pool != NULL) {
        ngx_destroy_pool(pool);
    }

    return rc;
}

#ifdef NGX_HAVE_NACOS_DYNAMIC_KEY

ngx_nacos_dynamic_key_t *ngx_nacos_dynamic_naming_key_add(ngx_str_t *data_id,
                                                          ngx_str_t *group) {
    ngx_pool_t *pool;
    ngx_nacos_dynamic_naming_key_t *k;
    ngx_uint_t key_len;
    u_char *data;

    if (group == NULL || group->len == 0) {
        group = &ngx_nacos_naming.nmcf->default_group;
    }

    key_len = data_id->len + group->len + 3;
    if (key_len > 512) {
        return NULL;
    }

    pool = ngx_create_pool(512, ngx_nacos_naming.nmcf->error_log);
    if (pool == NULL) {
        return NULL;
    }
    k = ngx_pcalloc(pool, sizeof(ngx_nacos_dynamic_naming_key_t));
    if (k == NULL) {
        ngx_destroy_pool(pool);
        return NULL;
    }
    k->key.pool = pool;
    k->naming = 1;
    k->event.data = k;
    k->event.handler = ngx_nacos_dynamic_naming_post_handler;

    data = ngx_palloc(pool, key_len);
    if (data == NULL) {
        ngx_destroy_pool(pool);
        return NULL;
    }

    k->node.str.data = data;
    k->node.str.len = key_len - 1;

    ngx_memcpy(data, group->data, group->len);
    k->key.group.len = group->len;
    k->key.group.data = data;
    data += group->len;
    *data++ = '@';
    *data++ = '@';
    ngx_memcpy(data, data_id->data, data_id->len);
    k->key.data_id.len = data_id->len;
    k->key.data_id.data = data;
    data += data_id->len;
    *data = 0;

    k->node.node.key = ngx_crc32_long(k->node.str.data, k->node.str.len);
    ngx_rbtree_insert(&ngx_nacos_naming.dynamic_keys, &k->node.node);

    k->event.active = 1;
    ngx_post_event(&k->event, &ngx_posted_events);

    return &k->key;
}

void ngx_nacos_dynamic_naming_key_del(ngx_nacos_dynamic_key_t *key) {
    ngx_nacos_grpc_conn_t *gc;
    ngx_int_t rc;
    ngx_nacos_dynamic_naming_key_t *dnk;

    dnk = nacos_naming_dynamic_key(key);

    if (!dnk->naming || dnk->event.closed) {
        return;
    }
    dnk->event.closed = 1;

    if (dnk->event.active) {
        dnk->event.active = 0;
        ngx_delete_posted_event(&dnk->event);
    }

    gc = ngx_nacos_naming.conn;
    if (gc != NULL && gc->stat == working) {
        rc = ngx_nacos_naming_subscribe_dynamic_key(gc, dnk, 0);
        if (rc == NGX_ERROR) {
            ngx_log_error(NGX_LOG_WARN, key->pool->log, 0,
                          "nacos unsubscribe naming dynamic key %V failed",
                          &dnk->node.str);
        } else {
            ngx_log_error(
                NGX_LOG_INFO, key->pool->log, 0,
                "nacos unsubscribe naming dynamic key %V successful!!!",
                &dnk->node.str);
        }
    }
    ngx_post_event(&dnk->event, &ngx_posted_events);
}

static ngx_int_t ngx_nacos_naming_subscribe_all_dynamic_keys(
    ngx_nacos_grpc_conn_t *gc) {
    ngx_rbtree_node_t *node, *sentinel;
    ngx_nacos_dynamic_naming_key_t *key, *prev;
    ngx_nacos_key_t *nk;
    ngx_hash_t *key_hash;
    ngx_int_t rc;

    prev = NULL;
    sentinel = &ngx_nacos_naming.dynamic_keys_sentinel;

    if (ngx_nacos_naming.dynamic_keys.root == sentinel) {
        return NGX_OK;
    }

    key_hash = ngx_nacos_naming.nmcf->key_hash;

    for (node = ngx_rbtree_min(ngx_nacos_naming.dynamic_keys.root, sentinel);
         node; node = ngx_rbtree_next(&ngx_nacos_naming.dynamic_keys, node)) {
        key = (ngx_nacos_dynamic_naming_key_t *) node;
        if (prev != NULL && prev->node.node.key == key->node.node.key &&
            prev->node.str.len == key->node.str.len &&
            ngx_strncmp(prev->node.str.data, key->node.str.data,
                        key->node.str.len) == 0) {
            continue;
        }

        if (key->event.closed) {
            continue;
        }

        nk = ngx_nacos_hash_find_key(key_hash, key->node.str.data);
        if (nk != NULL) {
            continue;
        }

        rc = ngx_nacos_naming_subscribe_dynamic_key(gc, key, 1);
        if (rc != NGX_OK) {
            return rc;
        }
        prev = key;
    }

    return NGX_OK;
}

static ngx_int_t ngx_nacos_naming_subscribe_dynamic_key(
    ngx_nacos_grpc_conn_t *gc, ngx_nacos_dynamic_naming_key_t *dnk,
    int subscribe) {
    yajl_gen gen;
    ngx_nacos_grpc_stream_t *st;
    ngx_int_t rc;
    ngx_nacos_key_t *nk;
    u_char *data;
    ngx_nacos_dynamic_key_t *key;

    ngx_nacos_payload_t payload = {
        .type = SubscribeServiceRequest,
        .end = 1,
    };

    key = &dnk->key;

    gen = NULL;
    rc = NGX_ERROR;
    st = NULL;

    gen = yajl_gen_alloc(NULL);
    if (gen == NULL) {
        goto free;
    }

    if (yajl_gen_map_open(gen) != yajl_gen_status_ok) {
        goto free;
    }

    //  headers: {}
    if (yajl_gen_string(gen, (u_char *) "headers", sizeof("headers") - 1) !=
            yajl_gen_status_ok ||
        yajl_gen_map_open(gen) != yajl_gen_status_ok ||
        yajl_gen_map_close(gen) != yajl_gen_status_ok) {
        goto free;
    }

    if (yajl_gen_string(gen, (u_char *) "namespace", sizeof("namespace") - 1) !=
            yajl_gen_status_ok ||
        yajl_gen_string(gen, ngx_nacos_naming.nmcf->service_namespace.data,
                        ngx_nacos_naming.nmcf->service_namespace.len) !=
            yajl_gen_status_ok) {
        goto free;
    }

    if (yajl_gen_string(gen, (u_char *) "serviceName",
                        sizeof("serviceName") - 1) != yajl_gen_status_ok ||
        yajl_gen_string(gen, key->data_id.data, key->data_id.len) !=
            yajl_gen_status_ok) {
        goto free;
    }

    if (yajl_gen_string(gen, (u_char *) "groupName", sizeof("groupName") - 1) !=
            yajl_gen_status_ok ||
        yajl_gen_string(gen, key->group.data, key->group.len) !=
            yajl_gen_status_ok) {
        goto free;
    }

    if (yajl_gen_string(gen, (u_char *) "subscribe", sizeof("subscribe") - 1) !=
            yajl_gen_status_ok ||
        yajl_gen_bool(gen, subscribe) != yajl_gen_status_ok) {
        goto free;
    }

    if (yajl_gen_string(gen, (u_char *) "clusters", sizeof("clusters") - 1) !=
            yajl_gen_status_ok ||
        yajl_gen_string(gen, (u_char *) "", 0) != yajl_gen_status_ok) {
        goto free;
    }

    if (yajl_gen_map_close(gen) != yajl_gen_status_ok) {
        goto free;
    }
    if (yajl_gen_get_buf(gen, (const u_char **) &payload.json_str.data,
                         &payload.json_str.len) != yajl_gen_status_ok) {
        goto free;
    }

    st = ngx_nacos_grpc_request(gc, ngx_nacos_subscribe_service_handler);
    if (st == NULL) {
        goto free;
    }

    nk = ngx_pcalloc(st->pool, sizeof(ngx_nacos_key_t));
    if (nk == NULL) {
        goto free;
    }
    // dy_key 的生命时期和 st 不一样
    data = ngx_palloc(st->pool, key->data_id.len + key->group.len + 3);
    ngx_memcpy(data, dnk->node.str.data, key->data_id.len + key->group.len + 3);

    nk->group.len = key->group.len;
    nk->group.data = data;

    nk->data_id.len = key->data_id.len;
    nk->data_id.data = data + key->group.len + 2;

    if (subscribe) {
        nk->ctx = (ngx_nacos_key_ctx_t *) st;  // HACK mark subscribe
    }

    st->handler_ctx = nk;

    if (ngx_nacos_grpc_send(st, &payload) != NGX_OK) {
        goto free;
    }
    st = NULL;
    yajl_gen_free(gen);
    gen = NULL;
    rc = NGX_OK;

free:
    if (gen != NULL) {
        yajl_gen_free(gen);
    }

    if (st != NULL) {
        ngx_nacos_grpc_close_stream(st, 1);
    }

    return rc;
}

static void ngx_nacos_naming_visit_dynamic_key(
    ngx_nacos_dynamic_naming_key_t *prev, ngx_rbtree_node_t *node,
    ngx_rbtree_node_t *sentinel, ngx_nacos_key_t *nk) {
    ngx_nacos_dynamic_naming_key_t *dnk;

    dnk = (ngx_nacos_dynamic_naming_key_t *) node;
    if (prev->node.node.key != dnk->node.node.key ||
        prev->node.str.len != dnk->node.str.len ||
        ngx_strncmp(prev->node.str.data, dnk->node.str.data,
                    dnk->node.str.len) != 0) {
        return;
    }

    if (!dnk->event.closed && dnk->key.handler(&dnk->key, nk) != NGX_OK) {
        ngx_nacos_dynamic_naming_key_del(&dnk->key);
    }

    if (node->left != sentinel) {
        ngx_nacos_naming_visit_dynamic_key(dnk, node->left, sentinel, nk);
    }
    if (node->right != sentinel) {
        ngx_nacos_naming_visit_dynamic_key(dnk, node->right, sentinel, nk);
    }
}

static void ngx_nacos_naming_notify_dynamic_key(u_char *full_data_id,
                                                size_t len,
                                                ngx_nacos_key_t *nk) {
    ngx_uint_t h;
    ngx_str_t name;
    ngx_str_node_t *node;

    name.data = full_data_id;
    name.len = len;
    h = ngx_crc32_long(full_data_id, len);
    node = ngx_str_rbtree_lookup(&ngx_nacos_naming.dynamic_keys, &name, h);
    if (node == NULL) {
        return;
    }

    ngx_nacos_naming_visit_dynamic_key(
        (ngx_nacos_dynamic_naming_key_t *) node, &node->node,
        &ngx_nacos_naming.dynamic_keys_sentinel, nk);
}

static void ngx_nacos_dynamic_naming_post_handler(ngx_event_t *ev) {
    ngx_nacos_grpc_conn_t *gc;
    ngx_int_t rc;
    ngx_pool_t *pool;
    ngx_nacos_key_t *nk;
    ngx_hash_t *key_hash;

    ngx_nacos_dynamic_naming_key_t *dnk;
    ngx_nacos_dynamic_key_t *key;

    dnk = ev->data;
    key = &dnk->key;
    pool = key->pool;

    if (ev->closed) {
        ngx_rbtree_delete(&ngx_nacos_naming.dynamic_keys, &dnk->node.node);
        ngx_destroy_pool(pool);
    } else {
        gc = ngx_nacos_naming.conn;

        key_hash = ngx_nacos_naming.nmcf->key_hash;
        nk = ngx_nacos_hash_find_key(key_hash, dnk->node.str.data);
        if (nk != NULL && key->handler(key, nk) != NGX_OK) {
            ngx_rbtree_delete(&ngx_nacos_naming.dynamic_keys, &dnk->node.node);
            ngx_destroy_pool(pool);
            return;
        }

        if (gc != NULL && gc->stat == working) {
            rc = ngx_nacos_naming_subscribe_dynamic_key(gc, dnk, 1);
            if (rc == NGX_ERROR) {
                ngx_log_error(NGX_LOG_WARN, pool->log, 0,
                              "nacos subscribe naming dynamic key %V failed",
                              &dnk->node.str);
            } else {
                ngx_log_error(
                    NGX_LOG_INFO, pool->log, 0,
                    "nacos subscribe naming dynamic key %V successfully",
                    &dnk->node.str);
            }
        }
    }
}

#endif
