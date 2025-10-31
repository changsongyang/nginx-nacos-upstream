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
#ifdef NGX_HAVE_NACOS_DYNAMIC_KEY
    ngx_rbtree_t dynamic_keys;
    ngx_rbtree_node_t dynamic_keys_sentinel;
#endif
} nacos_config;

#ifdef NGX_HAVE_NACOS_DYNAMIC_KEY

typedef struct {
    ngx_str_node_t node;
    ngx_event_t event;
    ngx_str_t md5;
    ngx_nacos_dynamic_key_t key;
} ngx_nacos_dynamic_config_key_t;

#define nacos_config_dynamic_key(key)                                       \
    (ngx_nacos_dynamic_config_key_t *) ((u_char *) (key) -                  \
                                        offsetof(                           \
                                            ngx_nacos_dynamic_config_key_t, \
                                            key))
static ngx_int_t ngx_nacos_config_subscribe_all_dynamic_keys(
    ngx_nacos_grpc_conn_t *gc);

static ngx_int_t ngx_nacos_config_subscribe_dynamic_key(
    ngx_nacos_grpc_conn_t *gc, ngx_nacos_dynamic_config_key_t *dck,
    int subscribe);

static void ngx_nacos_config_notify_dynamic_key(u_char *full_data_id,
                                                size_t len,
                                                ngx_nacos_key_t *nk);

static void ngx_nacos_dynamic_config_post_handler(ngx_event_t *ev);
#endif

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

typedef ngx_int_t (*ngx_nacos_config_md5_getter_t)(ngx_nacos_key_t *key,
                                                   ngx_str_t *tmp);
static ngx_int_t ngx_nacos_grpc_subscribe_configs(
    ngx_nacos_grpc_conn_t *gc, ngx_nacos_key_t **key, size_t n, int subscribe,
    ngx_nacos_config_md5_getter_t get_md5);

static ngx_int_t ngx_nacos_grpc_config_change_notified(
    ngx_nacos_grpc_conn_t *gc, yajl_val root);

static ngx_int_t ngx_nacos_grpc_send_config_query_request(
    ngx_nacos_grpc_conn_t *gc, ngx_str_t *group, ngx_str_t *data_id,
    ngx_nacos_key_t *key);

#define NGX_NACOS_MD5_LEN 128

ngx_int_t ngx_nacos_config_init(ngx_nacos_main_conf_t *nmcf) {
    nacos_config.nmcf = nmcf;

#ifdef NGX_HAVE_NACOS_DYNAMIC_KEY
    ngx_rbtree_init(&nacos_config.dynamic_keys,
                    &nacos_config.dynamic_keys_sentinel,
                    ngx_str_rbtree_insert_value);
#endif

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

    ngx_str_set(&payload.json_str,
                "{\"tenant\":\"\",\"clientVersion\":\"nginx-nacos-module:v2.10."
                "0\",\"abilityTable\":{},"
                "\"labels\":{\"source\":\"sdk\",\"module\":\"config\"}}");

    if (ngx_nacos_grpc_send(bi_st, &payload) != NGX_OK) {
        goto err;
    }
    if (!nacos_config.support_ability_negotiation) {
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

        if (ngx_nacos_grpc_config_change_notified(st->conn, val) != NGX_OK) {
            ngx_log_error(NGX_LOG_WARN, nacos_config.nmcf->error_log, 0,
                          "ConfigChangeNotifyRequest has not processed");
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
    ngx_nacos_main_conf_t *ncf;

    if (nacos_config.conn == NULL) {
        return;
    }

    ncf = nacos_config.nmcf;
    if (ngx_nacos_grpc_subscribe_configs(
            nacos_config.conn, ncf->config_keys.elts, ncf->config_keys.nelts, 1,
            ngx_nacos_get_config_md5) != NGX_OK
#ifdef NGX_HAVE_NACOS_DYNAMIC_KEY
        ||
        ngx_nacos_config_subscribe_all_dynamic_keys(nacos_config.conn) != NGX_OK
#endif
    ) {
        goto err;
    }

    ngx_add_timer(&nacos_config.subscribe_timer, 180000);
    return;
err:
    ngx_add_timer(&nacos_config.reconnect_timer, nacos_config.reconnect_time);
}

static ngx_int_t ngx_nacos_grpc_subscribe_configs(
    ngx_nacos_grpc_conn_t *gc, ngx_nacos_key_t **key, size_t n, int subscribe,
    ngx_nacos_config_md5_getter_t get_md5) {
    size_t idx;
    ngx_nacos_grpc_stream_t *st;
    yajl_gen gen;
    ngx_int_t rc;
    ngx_str_t tmp;
    ngx_nacos_main_conf_t *ncf;
    u_char tmp_buf[NGX_NACOS_MD5_LEN];

    ngx_nacos_payload_t payload = {
        .type = ConfigBatchListenRequest,
        .end = 1,
    };

    if (n == 0) {
        return NGX_OK;
    }

    ncf = gc->nmcf;
    st = NULL;
    gen = NULL;
    rc = NGX_ERROR;

    gen = yajl_gen_alloc(NULL);
    if (gen == NULL) {
        goto err;
    }

    // {"listen":true, "configListenContexts": [
    if (yajl_gen_map_open(gen) != yajl_gen_status_ok ||  // {
        yajl_gen_string(gen, (u_char *) "listen", sizeof("listen") - 1) !=
            yajl_gen_status_ok ||                               // "listen"
        yajl_gen_bool(gen, subscribe) != yajl_gen_status_ok ||  // "true

        yajl_gen_string(gen, (u_char *) "configListenContexts",
                        sizeof("configListenContexts") - 1) !=
            yajl_gen_status_ok ||
        yajl_gen_array_open(gen) !=
            yajl_gen_status_ok  // "configListenContexts":[
    ) {
        goto err;
    }

    for (idx = 0; idx < n; ++idx) {
        tmp.data = tmp_buf;
        tmp.len = sizeof(tmp_buf);
        if (get_md5(key[idx], &tmp) != NGX_OK) {
            goto err;
        }
        if (tmp.len == 0) {
            ngx_str_set(&tmp, "-");
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

    st = ngx_nacos_grpc_request(gc, ngx_nacos_config_subscribe_handler);

    if (st == NULL) {
        goto err;
    }

    rc = ngx_nacos_grpc_send(st, &payload);
    if (rc != NGX_OK) {
        ngx_nacos_grpc_close_stream(st, 1);
        goto err;
    }

err:
    if (gen != NULL) {
        yajl_gen_free(gen);
    }
    return rc;
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
        if (rc != NGX_OK) {
            ngx_log_error(NGX_LOG_WARN, st->conn->conn->log, 0,
                          "nacos ConfigChangeBatchListenResponse "
                          "send_config_query failed [%d]");
            break;
        }
    }
end:
    return rc;
}

static ngx_int_t ngx_nacos_grpc_config_change_notified(
    ngx_nacos_grpc_conn_t *gc, yajl_val root) {
    yajl_val s_name, g_name, t_name;
    ngx_nacos_key_t *key;
    ngx_str_t group, data_id;
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
    group.len = strlen(g_name->u.string);
    group.data = tmp;
    data_id.len = len - group.len - 2;
    data_id.data = tmp + group.len + 2;
    rc = ngx_nacos_grpc_send_config_query_request(gc, &group, &data_id, key);
end:
    return rc;
}

#define CONFIG_QUERY_JSON \
    "{\"dataId\":\"%V\"," \
    "\"group\":\"%V\",\"tenant\":\"%V\"}"

static ngx_int_t ngx_nacos_grpc_send_config_query_request(
    ngx_nacos_grpc_conn_t *gc, ngx_str_t *group, ngx_str_t *data_id,
    ngx_nacos_key_t *key) {
    ngx_nacos_main_conf_t *ncf;
    ngx_nacos_grpc_stream_t *st;
    size_t b_len;
    static u_char tmp[1024];
    ngx_nacos_payload_t payload = {
        .type = ConfigQueryRequest,
        .end = 1,
    };

    ncf = nacos_config.nmcf;
    st = ngx_nacos_grpc_request(gc, ngx_nacos_config_query_handler);
    if (st == NULL) {
        goto err;
    }
    if (key == NULL) {
        key = ngx_pcalloc(st->pool, sizeof(ngx_nacos_key_t));
        if (key == NULL) {
            goto err;
        }
        key->data_id = *data_id;
        key->group = *group;
        key->data_id.data = ngx_pstrdup(st->pool, data_id);
        if (key->data_id.data == NULL) {
            goto err;
        }
        key->group.data = ngx_pstrdup(st->pool, group);
        if (key->group.data == NULL) {
            goto err;
        }
    }

    st->handler_ctx = key;

    b_len = ngx_snprintf(tmp, sizeof(tmp), CONFIG_QUERY_JSON, data_id, group,
                         &ncf->config_tenant) -
            (u_char *) tmp;

    payload.json_str.len = b_len;
    payload.json_str.data = tmp;

    if (ngx_nacos_grpc_send(st, &payload) != NGX_OK) {
        goto err;
    }
    return NGX_OK;

err:
    if (st != NULL) {
        ngx_nacos_grpc_close_stream(st, 0);
    }
    return NGX_ERROR;
}

/**
*     String content;
    String md5;
    long lastModified;
 */
#define EMPTY_RESPONSE "{\"content\":\"\",\"md5\":\"\",\"lastModified\":1}"

static ngx_int_t ngx_nacos_config_query_handler(ngx_nacos_grpc_stream_t *st,
                                                ngx_nacos_payload_t *p) {
    yajl_val val;
    ngx_int_t rv;
    ngx_nacos_key_t *key;
    yajl_val code;

    key = st->handler_ctx;
    rv = NGX_ERROR;
    val = NULL;

    if (p->msg_state != pl_success || p->type != ConfigQueryResponse) {
        goto err;
    }

    val = yajl_tree_parse_with_len((const char *) p->json_str.data,
                                   p->json_str.len);
    if (ngx_nacos_check_response(st->conn->conn->log, val) != NGX_OK) {
        // may be config not exists
        if (YAJL_IS_OBJECT(val) &&
            (code = yajl_tree_get_field(val, "errorCode", yajl_t_number)) !=
                NULL &&
            YAJL_GET_INTEGER(code) == 300) {
            yajl_tree_free(val);
            val = yajl_tree_parse(EMPTY_RESPONSE, NULL, 0);
            if (val != NULL) {
                goto next;
            }
        }
        goto err;
    }

next:
    if (ngx_nacos_grpc_notify_config_shm(st, val) == NGX_ERROR) {
        ngx_log_error(NGX_LOG_WARN, st->conn->conn->log, 0,
                      "nacos config %V@@%V is updated failed!!!", &key->group,
                      &key->data_id);
    }

    rv = NGX_DONE;

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
    u_char *full_data_id;
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
    parser.prev_version = key->ctx ? ngx_nacos_shmem_version(key) : 0;
    adr = ngx_nacos_parse_config_from_json(&parser);
    if (adr == NULL) {
        ngx_log_error(NGX_LOG_WARN, parser.pool->log, 0,
                      "nacos config %V@@%V is updated failed!!!", &key->group,
                      &key->data_id);
        goto end;
    }

    if (key->ctx) {
        rc =
            ngx_nacos_update_shm(nacos_config.nmcf, key, adr, parser.pool->log);
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
    } else {
        key->ctx = ngx_pcalloc(st->pool, sizeof(ngx_nacos_key_ctx_t));
        if (key->ctx == NULL) {
            goto end;
        }
        key->ctx->version = parser.current_version;
        key->ctx->data = adr;
    }

    full_data_id = ngx_palloc(st->pool, key->data_id.len + key->group.len + 3);
    if (full_data_id == NULL) {
        goto end;
    }
    ngx_sprintf(full_data_id, "%V@@%V", &key->group, &key->data_id);
    full_data_id[key->data_id.len + key->group.len + 2] = 0;

    // for dynamic config
#ifdef NGX_HAVE_NACOS_DYNAMIC_KEY
    ngx_nacos_config_notify_dynamic_key(
        full_data_id, key->data_id.len + key->group.len + 2, key);
#endif
    rc = NGX_OK;
end:

    return rc;
}

#ifdef NGX_HAVE_NACOS_DYNAMIC_KEY

ngx_nacos_dynamic_key_t *ngx_nacos_dynamic_config_key_add(ngx_str_t *data_id,
                                                          ngx_str_t *group) {
    ngx_pool_t *pool;
    ngx_nacos_dynamic_config_key_t *k;
    ngx_uint_t key_len;
    u_char *data;

    if (group == NULL || group->len == 0) {
        group = &nacos_config.nmcf->default_group;
    }

    key_len = data_id->len + group->len + 3;
    if (key_len > 512) {
        return NULL;
    }

    pool = ngx_create_pool(512, nacos_config.nmcf->error_log);
    if (pool == NULL) {
        return NULL;
    }
    k = ngx_pcalloc(pool, sizeof(ngx_nacos_dynamic_config_key_t));
    if (k == NULL) {
        ngx_destroy_pool(pool);
        return NULL;
    }
    k->key.pool = pool;
    k->event.data = k;
    k->event.handler = ngx_nacos_dynamic_config_post_handler;

    data = ngx_palloc(pool, key_len);
    if (data == NULL) {
        ngx_destroy_pool(pool);
        return NULL;
    }

    k->md5.len = 0;
    k->md5.data = ngx_palloc(pool, NGX_NACOS_MD5_LEN);
    if (k->md5.data == NULL) {
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
    ngx_rbtree_insert(&nacos_config.dynamic_keys, &k->node.node);

    k->event.active = 1;
    ngx_post_event(&k->event, &ngx_posted_events);

    return &k->key;
}

void ngx_nacos_dynamic_config_key_del(ngx_nacos_dynamic_key_t *key) {
    ngx_nacos_grpc_conn_t *gc;
    ngx_int_t rc;
    ngx_nacos_dynamic_config_key_t *dck;
    dck = nacos_config_dynamic_key(key);

    if (dck->event.closed) {
        return;
    }
    dck->event.closed = 1;

    if (dck->event.active) {
        dck->event.active = 0;
        ngx_delete_posted_event(&dck->event);
    }

    gc = nacos_config.conn;
    if (gc != NULL && gc->stat == working) {
        rc = ngx_nacos_config_subscribe_dynamic_key(gc, dck, 0);
        if (rc == NGX_ERROR) {
            ngx_log_error(NGX_LOG_WARN, key->pool->log, 0,
                          "nacos unsubscribe config dynamic key %V failed",
                          &dck->node.str);
        } else {
            ngx_log_error(
                NGX_LOG_INFO, key->pool->log, 0,
                "nacos unsubscribe config dynamic key %V successful!!!",
                &dck->node.str);
        }
    }
    ngx_post_event(&dck->event, &ngx_posted_events);
}

static ngx_int_t ngx_nacos_config_get_dy_md5(ngx_nacos_key_t *key,
                                             ngx_str_t *tmp) {
    ngx_nacos_dynamic_config_key_t *dck;
    dck = (ngx_nacos_dynamic_config_key_t *) key->ctx;
    *tmp = dck->md5;
    return NGX_OK;
}

static ngx_int_t ngx_nacos_config_subscribe_all_dynamic_keys(
    ngx_nacos_grpc_conn_t *gc) {
    ngx_pool_t *pool;

    ngx_rbtree_node_t *node, *sentinel;
    ngx_nacos_dynamic_config_key_t *dck, *prev;
    ngx_nacos_key_t *nk, **nkp;
    ngx_hash_t *key_hash;
    ngx_array_t keys;
    ngx_int_t rc;

    rc = NGX_ERROR;
    pool = ngx_create_pool(512, nacos_config.nmcf->error_log);
    if (pool == NULL) {
        goto err;
    }

    if (ngx_array_init(&keys, pool, 16, sizeof(ngx_nacos_key_t *)) != NGX_OK) {
        goto err;
    }

    prev = NULL;
    sentinel = &nacos_config.dynamic_keys_sentinel;

    if (nacos_config.dynamic_keys.root == sentinel) {
        return NGX_OK;
    }

    key_hash = nacos_config.nmcf->config_key_hash;

    for (node = ngx_rbtree_min(nacos_config.dynamic_keys.root, sentinel); node;
         node = ngx_rbtree_next(&nacos_config.dynamic_keys, node)) {
        dck = (ngx_nacos_dynamic_config_key_t *) node;
        if (prev != NULL && prev->node.node.key == dck->node.node.key &&
            prev->node.str.len == dck->node.str.len &&
            ngx_strncmp(prev->node.str.data, dck->node.str.data,
                        dck->node.str.len) == 0) {
            continue;
        }

        if (dck->event.closed) {
            continue;
        }

        nk = ngx_nacos_hash_find_key(key_hash, dck->node.str.data);
        if (nk != NULL) {
            continue;
        }

        nk = ngx_pcalloc(pool, sizeof(ngx_nacos_key_t));
        if (nk == NULL) {
            goto err;
        }
        nk->group = dck->key.group;
        nk->data_id = dck->key.data_id;
        nk->ctx = (ngx_nacos_key_ctx_t *) dck;

        nkp = ngx_array_push(&keys);
        if (nkp == NULL) {
            goto err;
        }
        *nkp = nk;
        prev = dck;
    }

    rc = ngx_nacos_grpc_subscribe_configs(gc, keys.elts, keys.nelts, 1,
                                          ngx_nacos_config_get_dy_md5);
err:
    if (pool != NULL) {
        ngx_destroy_pool(pool);
    }
    return rc;
}

static ngx_int_t ngx_nacos_config_subscribe_dynamic_key(
    ngx_nacos_grpc_conn_t *gc, ngx_nacos_dynamic_config_key_t *dck,
    int subscribe) {
    ngx_nacos_key_t nk, *nkp;

    ngx_memzero(&nk, sizeof(ngx_nacos_key_t));
    nkp = &nk;
    nk.group = dck->key.group;
    nk.data_id = dck->key.data_id;
    nk.ctx = (ngx_nacos_key_ctx_t *) dck;
    return ngx_nacos_grpc_subscribe_configs(gc, &nkp, 1, subscribe,
                                            ngx_nacos_config_get_dy_md5);
}

static void ngx_nacos_config_visit_dynamic_key(
    ngx_nacos_dynamic_config_key_t *prev, ngx_rbtree_node_t *node,
    ngx_rbtree_node_t *sentinel, ngx_nacos_key_t *nk) {
    ngx_nacos_dynamic_config_key_t *dck;

    dck = (ngx_nacos_dynamic_config_key_t *) node;
    if (prev->node.node.key != dck->node.node.key ||
        prev->node.str.len != dck->node.str.len ||
        ngx_strncmp(prev->node.str.data, dck->node.str.data,
                    dck->node.str.len) != 0) {
        return;
    }

    dck->md5.len = NGX_NACOS_MD5_LEN;
    if (ngx_nacos_get_config_md5(nk, &dck->md5)!=NGX_OK) {
        ngx_log_error(NGX_LOG_ERR, nacos_config.nmcf->error_log, 0,
                      "nacos get dynamic config md5 failed");
        return;
    }

    if (!dck->event.closed && dck->key.handler(&dck->key, nk) != NGX_OK) {
        ngx_nacos_dynamic_config_key_del(&dck->key);
    }

    if (node->left != sentinel) {
        ngx_nacos_config_visit_dynamic_key(dck, node->left, sentinel, nk);
    }
    if (node->right != sentinel) {
        ngx_nacos_config_visit_dynamic_key(dck, node->right, sentinel, nk);
    }
}

static void ngx_nacos_config_notify_dynamic_key(u_char *full_data_id,
                                                size_t len,
                                                ngx_nacos_key_t *nk) {
    ngx_uint_t h;
    ngx_str_t name;
    ngx_str_node_t *node;

    name.data = full_data_id;
    name.len = len;
    h = ngx_crc32_long(full_data_id, len);
    node = ngx_str_rbtree_lookup(&nacos_config.dynamic_keys, &name, h);
    if (node == NULL) {
        return;
    }

    ngx_nacos_config_visit_dynamic_key((ngx_nacos_dynamic_config_key_t *) node,
                                       &node->node,
                                       &nacos_config.dynamic_keys_sentinel, nk);
}

static void ngx_nacos_dynamic_config_post_handler(ngx_event_t *ev) {
    ngx_nacos_grpc_conn_t *gc;
    ngx_int_t rc;
    ngx_pool_t *pool;
    ngx_nacos_key_t *nk;
    ngx_hash_t *key_hash;

    ngx_nacos_dynamic_config_key_t *dck;
    ngx_nacos_dynamic_key_t *key;

    dck = ev->data;
    key = &dck->key;
    pool = key->pool;

    if (ev->closed) {
        ngx_rbtree_delete(&nacos_config.dynamic_keys, &dck->node.node);
        ngx_destroy_pool(pool);
    } else {
        ev->active = 0;
        gc = nacos_config.conn;

        key_hash = nacos_config.nmcf->config_key_hash;
        nk = ngx_nacos_hash_find_key(key_hash, dck->node.str.data);
        if (nk != NULL && key->handler(key, nk) != NGX_OK) {
            ngx_rbtree_delete(&nacos_config.dynamic_keys, &dck->node.node);
            ngx_destroy_pool(pool);
            return;
        }

        if (gc != NULL && gc->stat == working) {
            rc = ngx_nacos_config_subscribe_dynamic_key(gc, dck, 1);
            if (rc == NGX_ERROR) {
                ngx_log_error(NGX_LOG_WARN, pool->log, 0,
                              "nacos subscribe config dynamic key %V failed",
                              &dck->node.str);
            } else {
                ngx_log_error(NGX_LOG_INFO, pool->log, 0,
                              "nacos subscribe config dynamic key %V success",
                              &dck->node.str);
            }
        }
    }
}

#endif
