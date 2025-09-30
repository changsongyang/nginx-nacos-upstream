//
// Created by pc on 9/23/25.
//
#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_nacos_grpc.h>
#include <ngx_nacos_data.h>
#include <yaij/api/yajl_gen.h>
#include <yaij/api/yajl_tree.h>

struct {
    ngx_nacos_main_conf_t *nmcf;
    ngx_nacos_grpc_conn_t *conn;
    ngx_event_t reconnect_timer;
    ngx_uint_t reconnect_time;
    ngx_event_t subscribe_timer;
    ngx_event_t health_timer;
    ngx_flag_t support_ability_negotiation;
} nacos_config;

static void ngx_nacos_config_reconnect_timer_handler(ngx_event_t *ev);

static void ngx_nacos_config_health_handler(ngx_event_t *ev);

static void ngx_nacos_config_subscribe_timer_handler(ngx_event_t *ev);

static ngx_int_t ngx_nacos_grpc_config_conn_handler(
    ngx_nacos_grpc_conn_t *conn, enum ngx_nacos_grpc_event_state state);

static ngx_int_t ngx_nacos_config_server_check_handler(
    ngx_nacos_grpc_stream_t *st, ngx_nacos_payload_t *p);

static ngx_int_t ngx_nacos_grpc_config_bi_handler(ngx_nacos_grpc_stream_t *st,
                                                  ngx_nacos_payload_t *p);

static ngx_int_t ngx_nacos_config_subscribe_handler(ngx_nacos_grpc_stream_t *st,
                                                    ngx_nacos_payload_t *p);

static ngx_int_t ngx_nacos_config_query_handler(ngx_nacos_grpc_stream_t *st,
                                                ngx_nacos_payload_t *p);

static ngx_int_t ngx_nacos_grpc_config_change_deal(ngx_nacos_grpc_stream_t *st,
                                                   yajl_val root);

static ngx_int_t ngx_nacos_grpc_notify_config_shm(ngx_nacos_grpc_stream_t *st,
                                                  yajl_val json);

static ngx_int_t ngx_nacos_grpc_config_change_notified(
    ngx_nacos_grpc_conn_t *gc, yajl_val root);

static ngx_int_t ngx_nacos_grpc_send_config_query_request(
    ngx_nacos_grpc_conn_t *gc, ngx_nacos_key_t *key);

ngx_int_t ngx_nacos_config_init(ngx_nacos_main_conf_t *nmcf) {
    nacos_config.nmcf = nmcf;
    nacos_config.reconnect_timer.handler =
        ngx_nacos_config_reconnect_timer_handler;
    nacos_config.health_timer.handler = ngx_nacos_config_health_handler;
    nacos_config.subscribe_timer.handler =
        ngx_nacos_config_subscribe_timer_handler;
    ngx_nacos_config_reconnect_timer_handler(&nacos_config.reconnect_timer);

    return NGX_OK;
}

static void ngx_nacos_config_reconnect_timer_handler(ngx_event_t *ev) {
    ngx_nacos_grpc_conn_t *conn;
    if (nacos_config.reconnect_time == 0) {
        nacos_config.reconnect_time = 3000;
    } else {
        nacos_config.reconnect_time = nacos_config.reconnect_time * 2;
    }

    if (nacos_config.health_timer.timer_set) {
        ngx_del_timer(&nacos_config.health_timer);
    }

    if (nacos_config.subscribe_timer.timer_set) {
        ngx_del_timer(&nacos_config.subscribe_timer);
    }

    conn = nacos_config.conn;
    if (conn != NULL) {
        nacos_config.conn = NULL;
        ngx_nacos_grpc_close_connection(conn, NGX_NC_TIRED);
    }

    conn = ngx_nacos_open_grpc_conn(nacos_config.nmcf,
                                    ngx_nacos_grpc_config_conn_handler);
    if (conn == NULL) {
        ngx_add_timer(&nacos_config.reconnect_timer,
                      nacos_config.reconnect_time);
        return;
    }
    nacos_config.conn = conn;
}

static void ngx_nacos_config_health_handler(ngx_event_t *ev) {
    ngx_nacos_grpc_stream_t *st;
    ngx_nacos_payload_t payload = {
        .type = HealthCheckRequest,
        .end = 1,
    };

    if (nacos_config.conn == NULL) {
        return;
    }

    ngx_str_set(&payload.json_str, NACOS_INTERNAL_REQUEST);
    st = ngx_nacos_grpc_request(nacos_config.conn, NULL);
    if (st == NULL) {
        ngx_add_timer(&nacos_config.reconnect_timer,
                      nacos_config.reconnect_time);
        return;
    }
    if (ngx_nacos_grpc_send(st, &payload) != NGX_OK) {
        ngx_nacos_grpc_close_stream(st, 1);
        ngx_add_timer(&nacos_config.reconnect_timer,
                      nacos_config.reconnect_time);
    } else {
        ngx_add_timer(&nacos_config.health_timer, 10000);
    }
}

static ngx_int_t ngx_nacos_grpc_config_conn_handler(
    ngx_nacos_grpc_conn_t *conn, enum ngx_nacos_grpc_event_state state) {
    ngx_nacos_grpc_stream_t *st;
    ngx_nacos_payload_t payload = {
        .type = ServerCheckRequest,
        .end = 1,
    };

    if (conn != nacos_config.conn) {
        return NGX_ERROR;
    }

    if (state != nc_connected) {
        ngx_log_error(NGX_LOG_ERR, nacos_config.nmcf->error_log, 0,
                      "nacos grpc config connection error");
        ngx_add_timer(&nacos_config.reconnect_timer,
                      nacos_config.reconnect_time);
        nacos_config.conn = NULL;
        if (nacos_config.health_timer.timer_set) {
            ngx_del_timer(&nacos_config.health_timer);
        }

        if (nacos_config.subscribe_timer.timer_set) {
            ngx_del_timer(&nacos_config.subscribe_timer);
        }

        return NGX_ERROR;
    }
    nacos_config.reconnect_time = 0;

    st = ngx_nacos_grpc_request(conn, ngx_nacos_config_server_check_handler);
    if (st == NULL) {
        return NGX_ERROR;
    }

    ngx_str_set(&payload.json_str, NACOS_INTERNAL_REQUEST);
    return ngx_nacos_grpc_send(st, &payload);
}

static ngx_int_t ngx_nacos_config_server_check_handler(
    ngx_nacos_grpc_stream_t *st, ngx_nacos_payload_t *p) {
    yajl_val val, san;
    ngx_nacos_grpc_stream_t *bi_st;

    if (p->msg_state != pl_success || p->type != ServerCheckResponse) {
        return NGX_ERROR;
    }

    if (!nacos_config.health_timer.timer_set) {
        ngx_add_timer(&nacos_config.health_timer, 10000);
    }

    bi_st = NULL;
    val = yajl_tree_parse_with_len((const char *) p->json_str.data,
                                   p->json_str.len);
    if (ngx_nacos_check_response(st->conn->conn->log, val) != NGX_OK) {
        goto err;
    }

    san = yajl_tree_get_field(val, "supportAbilityNegotiation", yajl_t_true);
    nacos_config.support_ability_negotiation = YAJL_IS_TRUE(san) ? 1 : 0;

    bi_st =
        ngx_nacos_grpc_bi_request(st->conn, ngx_nacos_grpc_config_bi_handler);
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
        "\"labels\":{\"source\":\"sdk\",\"module\":\"config\"}}");

    if (ngx_nacos_grpc_send(bi_st, &payload) != NGX_OK) {
        goto err;
    }
    if (!YAJL_IS_TRUE(san)) {
        ngx_add_timer(&nacos_config.subscribe_timer, 1000);
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

static ngx_int_t ngx_nacos_grpc_config_bi_handler(ngx_nacos_grpc_stream_t *st,
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
        if (nacos_config.subscribe_timer.timer_set) {
            ngx_del_timer(&nacos_config.subscribe_timer);
        }

        payload.type = SetupAckResponse;
        ngx_str_set(&payload.json_str, "{\"resultCode\":200}");
        if (ngx_nacos_grpc_send(st, &payload) != NGX_OK) {
            goto err;
        }

        if (nacos_config.support_ability_negotiation) {
            ngx_nacos_config_subscribe_timer_handler(
                &nacos_config.subscribe_timer);
        }
        return NGX_OK;
    }

    if (p->type == ClientDetectionRequest) {
        val = yajl_tree_parse_with_len((const char *) p->json_str.data,
                                       p->json_str.len);
        requestId = yajl_tree_get_field(val, "requestId", yajl_t_string);
        if (requestId == NULL) {
            ngx_log_error(NGX_LOG_ERR, nacos_config.nmcf->error_log, 0,
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
    } else if (p->type == ConfigChangeNotifyRequest) {
        val = yajl_tree_parse_with_len((const char *) p->json_str.data,
                                       p->json_str.len);

        if (ngx_nacos_grpc_config_change_notified(st->conn, val) != NGX_OK) {
            ngx_log_error(NGX_LOG_WARN, nacos_config.nmcf->error_log, 0,
                          "ConfigChangeNotifyRequest has not processed");
        }

        requestId = yajl_tree_get_field(val, "requestId", yajl_t_string);
        if (requestId == NULL) {
            ngx_log_error(NGX_LOG_ERR, nacos_config.nmcf->error_log, 0,
                          "ConfigChangeNotifyRequest has no request id");
            goto err;
        }

        payload.type = ConfigChangeNotifyResponse;
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

static void ngx_nacos_config_subscribe_timer_handler(ngx_event_t *ev) {
    ngx_str_t tmp;
    ngx_nacos_main_conf_t *ncf;
    ngx_uint_t idx, len;
    ngx_nacos_key_t **key;
    ngx_nacos_grpc_stream_t *st;
    yajl_gen gen;
    u_char tmp_buf[128];
    // ngx_uint_t i, l;
    // yajl_gen_status gen_status;
    ngx_nacos_payload_t payload = {
        .type = ConfigBatchListenRequest,
        .end = 1,
    };

    if (nacos_config.conn == NULL) {
        return;
    }

    ncf = nacos_config.nmcf;

    key = ncf->config_keys.elts;
    len = ncf->config_keys.nelts;

    st = NULL;
    gen = NULL;

    gen = yajl_gen_alloc(NULL);
    if (gen == NULL) {
        goto err;
    }

    // {"listen":true, "configListenContexts": [
    if (yajl_gen_map_open(gen) != yajl_gen_status_ok ||  // {
        yajl_gen_string(gen, (u_char *) "listen", sizeof("listen") - 1) !=
            yajl_gen_status_ok ||                       // "listen"
        yajl_gen_bool(gen, 1) != yajl_gen_status_ok ||  // "true

        yajl_gen_string(gen, (u_char *) "configListenContexts",
                        sizeof("configListenContexts") - 1) !=
            yajl_gen_status_ok ||
        yajl_gen_array_open(gen) !=
            yajl_gen_status_ok  // "configListenContexts":[
    ) {
        goto err;
    }

    for (idx = 0; idx < len; ++idx) {
        tmp.data = tmp_buf;
        tmp.len = sizeof(tmp_buf);
        if (ngx_nacos_get_config_md5(key[idx], &tmp) != NGX_OK) {
            goto err;
        }

        if (yajl_gen_map_open(gen) != yajl_gen_status_ok ||  // {
            yajl_gen_string(gen, (u_char *) "group", sizeof("group") - 1) !=
                yajl_gen_status_ok ||  // "group": group
            yajl_gen_string(gen, key[idx]->group.data, key[idx]->group.len) !=
                yajl_gen_status_ok ||

            yajl_gen_string(gen, (u_char *) "dataId", sizeof("dataId") - 1) !=
                yajl_gen_status_ok ||  // "dataId": dataId
            yajl_gen_string(gen, key[idx]->data_id.data,
                            key[idx]->data_id.len) != yajl_gen_status_ok ||

            yajl_gen_string(gen, (u_char *) "md5", sizeof("md5") - 1) !=
                yajl_gen_status_ok ||  // "md5": group
            yajl_gen_string(gen, tmp.data, tmp.len) != yajl_gen_status_ok ||

            yajl_gen_string(gen, (u_char *) "tenant", sizeof("tenant") - 1) !=
                yajl_gen_status_ok ||  // "tenant": tenant
            yajl_gen_string(gen, ncf->config_tenant.data,
                            ncf->config_tenant.len) != yajl_gen_status_ok ||

            yajl_gen_map_close(gen) != yajl_gen_status_ok) {
            goto err;
        }
    }

    // ], }
    if (yajl_gen_array_close(gen) != yajl_gen_status_ok ||
        yajl_gen_map_close(gen) != yajl_gen_status_ok) {
        goto err;
    }

    if (yajl_gen_get_buf(gen, (const unsigned char **) &payload.json_str.data,
                         &payload.json_str.len) != yajl_gen_status_ok) {
        goto err;
    }

    st = ngx_nacos_grpc_request(nacos_config.conn,
                                ngx_nacos_config_subscribe_handler);

    if (st == NULL) {
        goto err;
    }

    if (ngx_nacos_grpc_send(st, &payload) != NGX_OK) {
        goto err;
    }

    yajl_gen_free(gen);

    ngx_add_timer(&nacos_config.subscribe_timer, 180000);

    return;

err:
    if (st != NULL) {
        ngx_nacos_grpc_close_stream(st, 1);
    }
    if (gen != NULL) {
        yajl_gen_free(gen);
    }
    ngx_add_timer(&nacos_config.reconnect_timer, nacos_config.reconnect_time);
}

static ngx_int_t ngx_nacos_config_subscribe_handler(ngx_nacos_grpc_stream_t *st,
                                                    ngx_nacos_payload_t *p) {
    yajl_val val;
    ngx_int_t rv;

    val = NULL;
    rv = NGX_ERROR;
    if (p->msg_state != pl_success ||
        p->type != ConfigChangeBatchListenResponse) {
        goto err;
    }

    val = yajl_tree_parse_with_len((const char *) p->json_str.data,
                                   p->json_str.len);
    if (ngx_nacos_check_response(st->conn->conn->log, val) != NGX_OK) {
        goto err;
    }

    (void) ngx_nacos_grpc_config_change_deal(st, val);
    rv = NGX_DONE;

err:
    if (val != NULL) {
        yajl_tree_free(val);
    }

    return rv;
}

static ngx_int_t ngx_nacos_grpc_config_change_deal(ngx_nacos_grpc_stream_t *st,
                                                   yajl_val root) {
    yajl_val *it, *et, changedConfigs;
    ngx_int_t rc;

    rc = NGX_ERROR;

    if (root == NULL) {
        goto end;
    }

    changedConfigs = yajl_tree_get_field(root, "changedConfigs", yajl_t_array);
    if (changedConfigs == NULL) {
        goto end;
    }
    rc = NGX_OK;
    it = YAJL_GET_ARRAY(changedConfigs)->values;
    et = it + YAJL_GET_ARRAY(changedConfigs)->len;
    for (; it < et; ++it) {
        if (!YAJL_IS_OBJECT(*it)) {
            ngx_log_error(NGX_LOG_WARN, st->conn->conn->log, 0,
                          "nacos ConfigChangeBatchListenResponse "
                          "changedConfigs is not object");
            continue;
        }
        rc = ngx_nacos_grpc_config_change_notified(st->conn, *it);
        if (rc == NGX_ERROR) {
            ngx_log_error(NGX_LOG_WARN, st->conn->conn->log, 0,
                          "nacos ConfigChangeBatchListenResponse "
                          "send_config_query failed");
            break;
        }
        ngx_log_error(NGX_LOG_INFO, st->conn->conn->log, 0,
                      "nacos ConfigChangeBatchListenResponse "
                      "send_config_query [%d]",
                      rc);
    }
end:
    return rc;
}

static ngx_int_t ngx_nacos_grpc_config_change_notified(
    ngx_nacos_grpc_conn_t *gc, yajl_val root) {
    yajl_val s_name, g_name, t_name;
    ngx_nacos_key_t *key;
    ngx_int_t rc;
    u_char tmp[512];
    size_t len;
    char *name;

    rc = NGX_ERROR;

    if (root == NULL) {
        goto end;
    }
    s_name = yajl_tree_get_field(root, "dataId", yajl_t_string);
    if (s_name == NULL) {
        goto end;
    }
    g_name = yajl_tree_get_field(root, "group", yajl_t_string);
    if (g_name == NULL) {
        goto end;
    }
    t_name = yajl_tree_get_field(root, "tenant", yajl_t_string);
    name = YAJL_GET_STRING(t_name);
    if (name == NULL) {
        name = "";
    }
    if (nacos_config.nmcf->config_tenant.len > 0 &&
        (ngx_strlen(name) != nacos_config.nmcf->config_tenant.len ||
         ngx_strcmp(nacos_config.nmcf->config_tenant.data, name) != 0)) {
        ngx_log_error(NGX_LOG_WARN, gc->conn->log, 0,
                      "nacos server sent config change with unknown tenant:%s",
                      YAJL_GET_STRING(t_name));
        goto end;
    }

    len = ngx_snprintf(tmp, sizeof(tmp) - 1, "%s@@%s", YAJL_GET_STRING(g_name),
                       YAJL_GET_STRING(s_name)) -
          tmp;
    tmp[len] = 0;

    key = ngx_nacos_hash_find_key(nacos_config.nmcf->config_key_hash, tmp);
    if (key == NULL) {
        ngx_log_error(
            NGX_LOG_WARN, gc->conn->log, 0,
            "nacos server sent config change with unknown data group:%s", tmp);
        goto end;
    }

    rc = ngx_nacos_grpc_send_config_query_request(gc, key);
end:

    return rc;
}

#define CONFIG_QUERY_JSON \
    "{\"dataId\":\"%V\"," \
    "\"group\":\"%V\",\"tenant\":\"%V\"}"

static ngx_int_t ngx_nacos_grpc_send_config_query_request(
    ngx_nacos_grpc_conn_t *gc, ngx_nacos_key_t *key) {
    ngx_nacos_main_conf_t *ncf;
    ngx_nacos_grpc_stream_t *st;
    size_t b_len;
    static u_char tmp[1024];
    ngx_nacos_payload_t payload = {
        .type = ConfigQueryRequest,
        .end = 1,
    };

    st = NULL;

    ncf = nacos_config.nmcf;

    st = ngx_nacos_grpc_request(gc, ngx_nacos_config_query_handler);
    if (st == NULL) {
        goto err;
    }
    st->handler_ctx = key;

    b_len = ngx_snprintf(tmp, sizeof(tmp), CONFIG_QUERY_JSON, &key->data_id,
                         &key->group, &ncf->config_tenant) -
            (u_char *) tmp;

    payload.json_str.len = b_len;
    payload.json_str.data = tmp;

    if (ngx_nacos_grpc_send(st, &payload) != NGX_OK) {
        goto err;
    }
    return NGX_OK;

err:
    if (st != NULL) {
        ngx_nacos_grpc_close_stream(st, 1);
    }
    return NGX_ERROR;
}
static ngx_int_t ngx_nacos_config_query_handler(ngx_nacos_grpc_stream_t *st,
                                                ngx_nacos_payload_t *p) {
    yajl_val val;
    ngx_int_t rv;

    rv = NGX_ERROR;
    val = NULL;

    if (p->msg_state != pl_success || p->type != ConfigQueryResponse) {
        goto err;
    }

    val = yajl_tree_parse_with_len((const char *) p->json_str.data,
                                   p->json_str.len);
    if (ngx_nacos_check_response(st->conn->conn->log, val) != NGX_OK) {
        goto err;
    }

    if (ngx_nacos_grpc_notify_config_shm(st, val) == NGX_ERROR) {
        goto err;
    }

    rv = NGX_OK;

err:
    if (val != NULL) {
        yajl_tree_free(val);
    }
    return rv;
}

static ngx_int_t ngx_nacos_grpc_notify_config_shm(ngx_nacos_grpc_stream_t *st,
                                                  yajl_val json) {
    ngx_nacos_key_t *key;
    ngx_nacos_resp_json_parser_t parser;
    ngx_int_t rc;
    char *adr;
    ngx_nacos_data_t cache;

    key = st->handler_ctx;
    rc = NGX_ERROR;

    ngx_memzero(&parser, sizeof(parser));
    if (json == NULL) {
        goto end;
    }

    parser.json = json;
    parser.pool = st->pool;
    parser.log = st->pool->log;
    parser.prev_version = ngx_nacos_shmem_version(key);
    adr = ngx_nacos_parse_config_from_json(&parser);
    if (adr == NULL) {
        ngx_log_error(NGX_LOG_WARN, parser.pool->log, 0,
                      "nacos config %V@@%V is updated failed!!!", &key->group,
                      &key->data_id);
        goto end;
    }
    rc = ngx_nacos_update_shm(nacos_config.nmcf, key, adr, parser.pool->log);
    if (rc == NGX_OK) {
        ngx_log_error(NGX_LOG_INFO, parser.pool->log, 0,
                      "nacos config %V@@%V is updated!!!", &key->group,
                      &key->data_id);
    }
    cache.pool = st->pool;
    cache.data_id = key->data_id;
    cache.group = key->group;
    cache.version = parser.current_version;
    cache.adr = adr;
    rc = ngx_nacos_write_disk_data(nacos_config.nmcf, &cache);

end:

    return rc;
}
