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

typedef struct {
    ngx_nacos_main_conf_t *nmcf;
    ngx_nacos_grpc_conn_t *conn;
    ngx_event_t reconnect_timer;
    ngx_uint_t reconnect_time;
    ngx_event_t subscribe_timer;
    ngx_event_t health_timer;
    ngx_flag_t support_ability_negotiation;
} ngx_nacos_naming_t;

ngx_nacos_naming_t ngx_nacos_naming;

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
    ngx_nacos_naming.reconnect_timer.handler =
        ngx_nacos_naming_reconnect_timer_handler;
    ngx_nacos_naming.subscribe_timer.handler =
        ngx_nacos_naming_subscribe_timer_handler;
    ngx_nacos_naming.health_timer.handler = ngx_nacos_naming_health_handler;
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
    key = ngx_nacos_naming.nmcf->keys.elts;
    static u_char tmp[512];
    size_t b_len;
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

    if (ngx_nacos_naming.conn == NULL) {
        return;
    }

    for (idx = 0, len = ngx_nacos_naming.nmcf->keys.nelts; idx < len; idx++) {
        st = ngx_nacos_grpc_request(ngx_nacos_naming.conn,
                                    ngx_nacos_subscribe_service_handler);
        if (st == NULL) {
            goto free;
        }
        st->handler_ctx = key[idx];
        b_len = ngx_snprintf(tmp, sizeof(tmp),
                             "{\"headers\":{},"
                             "\"namespace\":\"%V\","
                             "\"serviceName\":\"%V\","
                             "\"groupName\":\"%V\","
                             "\"subscribe\":true,"
                             "\"clusters\":\"\"}",
                             &ngx_nacos_naming.nmcf->service_namespace,
                             &key[idx]->data_id, &key[idx]->group) -
                (u_char *) tmp;
        payload.json_str.data = tmp;
        payload.json_str.len = b_len;
        if (ngx_nacos_grpc_send(st, &payload) != NGX_OK) {
            ngx_nacos_grpc_close_stream(st, 1);
            goto free;
        }
    }

    // register service
    len = ngx_nacos_naming.nmcf->register_services.nelts;
    if (len > 0) {
        nrp = ngx_nacos_naming.nmcf->register_services.elts;
        payload.type = InstanceRequest;

        for (idx = 0; idx < len; idx++) {
            gen = yajl_gen_alloc(NULL);
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
    ngx_nacos_key_t *key = st->handler_ctx;
    if (p->msg_state != pl_success || p->type != SubscribeServiceResponse) {
        ngx_log_error(NGX_LOG_ERR, st->conn->conn->log, 0,
                      "nacos grpc subscribe naming %V@@%V error", &key->data_id,
                      &key->group);
        return NGX_ERROR;
    }
    ngx_log_error(NGX_LOG_INFO, st->conn->conn->log, 0,
                  "nacos grpc subscribe naming %V@@%V successfully",
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

        payload.type = SetupAckResponse;
        ngx_str_set(&payload.json_str, "{\"resultCode\":200}");
        if (ngx_nacos_grpc_send(st, &payload) != NGX_OK) {
            goto err;
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

        if (ngx_nacos_grpc_notify_address_shm(st->conn, val) != NGX_OK) {
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
    ngx_nacos_payload_t payload = {
        .type = ConnectionSetupRequest,
        .end = 0,
    };

    ngx_str_set(
        &payload.json_str,
        "{\"tenant\":\"\",\"clientVersion\":\"nginx-nacos-module:v2.10.0\",\"abilityTable\":{},"
        "\"labels\":{\"source\":\"sdk\",\"module\":\"naming\"}}");

    if (ngx_nacos_grpc_send(bi_st, &payload) != NGX_OK) {
        goto err;
    }
    if (!YAJL_IS_TRUE(san)) {
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
    u_char tmp[256];
    ngx_nacos_data_t cache;
    size_t len;

    pool = NULL;
    rc = NGX_ERROR;

    if (json == NULL) {
        goto end;
    }
    ngx_memzero(&parser, sizeof(parser));
    parser.json = yajl_tree_get_field(json, "serviceInfo", yajl_t_object);
    if (parser.json == NULL) {
        goto end;
    }
    s_name = yajl_tree_get_field(parser.json, "name", yajl_t_string);
    if (s_name == NULL) {
        goto end;
    }
    g_name = yajl_tree_get_field(parser.json, "groupName", yajl_t_string);
    if (g_name == NULL) {
        goto end;
    }

    len = ngx_snprintf(tmp, sizeof(tmp) - 1, "%s@@%s", YAJL_GET_STRING(g_name),
                       YAJL_GET_STRING(s_name)) -
          tmp;
    tmp[len] = 0;

    key = ngx_nacos_hash_find_key(ngx_nacos_naming.nmcf->key_hash, tmp);
    if (key == NULL) {
        ngx_log_error(NGX_LOG_WARN, gc->conn->log, 0,
                      "nacos server sent address with unknown server:%s", tmp);
        goto end;
    }

    pool = ngx_create_pool(512, gc->conn->log);
    if (pool == NULL) {
        goto end;
    }

    parser.pool = pool;
    parser.log = pool->log;
    parser.prev_version = ngx_nacos_shmem_version(key);
    adr = ngx_nacos_parse_addrs_from_json(&parser);
    if (adr == NULL) {
        goto end;
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

end:
    if (pool != NULL) {
        ngx_destroy_pool(pool);
    }

    return rc;
}
