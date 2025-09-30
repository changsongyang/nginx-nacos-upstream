//
// Created by eleme on 2023/2/17.
//

#include <ngx_config.h>
#include <ngx_core.h>
#include <nacos_grpc_service.pb.h>
#include <ngx_nacos_http_v2.h>
#include <ngx_nacos_data.h>
#include <ngx_nacos_grpc.h>
#include <pb/pb_decode.h>
#include <pb/pb_encode.h>
#include <yaij/api/yajl_gen.h>
#include <yaij/api/yajl_tree.h>
#include <assert.h>

static struct {
    ngx_str_t type_name;
    enum ngx_nacos_payload_type payload_type;
} payload_type_mapping[] = {
#define PC(name) {ngx_string(#name), name},
    PAYLOAD_TYPE_LIST
#undef PC
};

#define NGX_NACOS_GRPC_DEFAULT_GRPC_STATUS 10000
#define NGX_NACOS_GRPC_DEFAULT_PING_INTERVAL 90000
#define NGX_NACOS_GRPC_CONFIG_BATCH_SIZE 100

static ngx_nacos_grpc_stream_t *ngx_nacos_grpc_create_stream(
    ngx_nacos_grpc_conn_t *gc);

static ngx_nacos_grpc_buf_t *ngx_nacos_grpc_alloc_buf(
    ngx_nacos_grpc_stream_t *st, size_t cap);

ngx_inline void ngx_nacos_grpc_free_buf(ngx_nacos_grpc_stream_t *st,
                                        ngx_nacos_grpc_buf_t *buf) {
    assert(st == buf->stream);
    ngx_free(buf);
    --st->buf_size;
}

static ngx_int_t ngx_nacos_grpc_send_buf(ngx_nacos_grpc_buf_t *buf,
                                         ngx_flag_t can_block);

static ngx_int_t ngx_nacos_grpc_do_send(ngx_nacos_grpc_conn_t *gc);

// read buf. 128K. max frame
#define READ_BUF_CAP (1 << 17)
#define MAX_FRAME_SIZE (READ_BUF_CAP - 9)

static u_char ngx_nacos_grpc_connection_start[] =
    "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n" /* connection preface */

    "\x00\x00\x18\x04\x00\x00\x00\x00\x00" /* settings frame */
    "\x00\x01\x00\x00\x00\x00"             /* header table size */
    "\x00\x02\x00\x00\x00\x00"             /* disable push */
    "\x00\x04\x7f\xff\xff\xff"             /* initial window */
    "\x00\x05\x00\x01\xff\xf7"             /* max frame size 128K - 9 */
    "\x00\x00\x04\x08\x00\x00\x00\x00\x00" /* window update frame */
    "\x7f\xff\x00\x00";

static void ngx_nacos_grpc_event_handler(ngx_event_t *ev);

static ngx_int_t ngx_nacos_grpc_write_handler(ngx_nacos_grpc_conn_t *gc,
                                              ngx_event_t *ev);

static ngx_int_t ngx_nacos_grpc_read_handler(ngx_nacos_grpc_conn_t *gc,
                                             ngx_event_t *ev);

static ngx_int_t ngx_nacos_grpc_send_conn_start(ngx_nacos_grpc_stream_t *st);

static ngx_int_t ngx_nacos_grpc_parse_frame(ngx_nacos_grpc_conn_t *gc);

typedef ngx_int_t (*ngx_nacos_grpc_frame_handler)(ngx_nacos_grpc_conn_t *gc);

static ngx_int_t ngx_nacos_grpc_parse_unknown_frame(ngx_nacos_grpc_conn_t *gc);

static ngx_int_t ngx_nacos_grpc_parse_data_frame(ngx_nacos_grpc_conn_t *gc);

static ngx_int_t ngx_nacos_grpc_parse_header_frame(ngx_nacos_grpc_conn_t *gc);

static ngx_int_t ngx_nacos_grpc_parse_rst_frame(ngx_nacos_grpc_conn_t *gc);

static ngx_int_t ngx_nacos_grpc_parse_settings_frame(ngx_nacos_grpc_conn_t *gc);

static ngx_int_t ngx_nacos_grpc_parse_ping_frame(ngx_nacos_grpc_conn_t *gc);

static ngx_int_t ngx_nacos_grpc_parse_goaway_frame(ngx_nacos_grpc_conn_t *gc);

static ngx_int_t ngx_nacos_grpc_parse_window_update_frame(
    ngx_nacos_grpc_conn_t *gc);

static ngx_nacos_grpc_stream_t *ngx_nacos_grpc_find_stream(
    ngx_nacos_grpc_conn_t *gc, ngx_uint_t st_id);

static ngx_int_t ngx_nacos_grpc_update_send_window(ngx_nacos_grpc_conn_t *gc,
                                                   ngx_uint_t st_id,
                                                   ngx_uint_t win_update);

static void ngx_nacos_grpc_consume_sending_buf(
    ngx_nacos_grpc_stream_t *st, ngx_nacos_grpc_bufs_t *target_bufs);

static ngx_int_t ngx_nacos_grpc_parse_proto_msg(ngx_nacos_grpc_stream_t *st,
                                                ngx_str_t *proto_msg);

static ngx_nacos_grpc_buf_t *ngx_nacos_grpc_encode_request(
    ngx_nacos_grpc_stream_t *st, ngx_str_t *mtd);

static ngx_int_t ngx_nacos_grpc_decode_ule128(u_char **pp, const u_char *last,
                                              size_t *result);

static bool ngx_nacos_grpc_pb_decode_str(pb_istream_t *stream,
                                         const pb_field_t *field, void **arg);
static bool ngx_nacos_grpc_pb_encode_str(pb_ostream_t *stream,
                                         const pb_field_t *field,
                                         void *const *arg);
static bool ngx_nacos_grpc_encode_user_pass(pb_ostream_t *stream,
                                            const pb_field_t *field,
                                            void *const *arg);
static ngx_inline void ngx_nacos_grpc_encode_frame_header(
    ngx_nacos_grpc_stream_t *st, u_char *b, u_char type, u_char flags,
    size_t len) {
    b[0] = (len >> 16) & 0xFF;
    b[1] = (len >> 8) & 0xFF;
    b[2] = len & 0xFF;
    b[3] = type;
    b[4] = flags;
    b[5] = (st->stream_id >> 24) & 0x7F;
    b[6] = (st->stream_id >> 16) & 0xFF;
    b[7] = (st->stream_id >> 8) & 0xFF;
    b[8] = st->stream_id & 0xFF;
}

typedef struct {
    ngx_str_t type;
    ngx_str_t json;
    Payload payload;
    size_t encoded_len;
    ngx_nacos_grpc_buf_t *buf;
} ngx_nacos_grpc_payload_encode_t;

static ngx_int_t ngx_nacos_grpc_encode_payload_init(
    ngx_nacos_grpc_payload_encode_t *en, ngx_nacos_grpc_stream_t *st);

static ngx_int_t ngx_nacos_grpc_encode_payload(
    const ngx_nacos_grpc_payload_encode_t *en, ngx_nacos_grpc_stream_t *st);

static ngx_nacos_grpc_buf_t *ngx_nacos_grpc_encode_data_msg(
    ngx_nacos_grpc_stream_t *st, ngx_nacos_grpc_payload_encode_t *en,
    ngx_flag_t end_stream);

typedef struct {
    ngx_str_t input;
    Payload result;
    ngx_str_t type;
    ngx_str_t out_json;
    ngx_nacos_payload_t payload;
} ngx_nacos_grpc_payload_decode_t;

static ngx_int_t ngx_nacos_grpc_decode_payload(
    ngx_nacos_grpc_stream_t *st, ngx_nacos_grpc_payload_decode_t *de);

static ngx_int_t ngx_nacos_grpc_realloc_tmp_buf(ngx_nacos_grpc_stream_t *st,
                                                size_t asize);

static ngx_int_t ngx_nacos_grpc_send_win_update_frame(
    ngx_nacos_grpc_stream_t *st, size_t win_update);

static ngx_int_t ngx_nacos_send_ping_frame(ngx_nacos_grpc_conn_t *gc);

static bool ngx_nacos_grpc_pb_write_buf(pb_ostream_t *stream,
                                        const pb_byte_t *buf, size_t count);

static ngx_str_t http2_err[] = {
    ngx_string("NO_ERROR(0L)"),
    ngx_string("PROTOCOL_ERROR(1L)"),
    ngx_string("INTERNAL_ERROR(2L)"),
    ngx_string("FLOW_CONTROL_ERROR(3L)"),
    ngx_string("SETTINGS_TIMEOUT(4L)"),
    ngx_string("STREAM_CLOSED(5L)"),
    ngx_string("FRAME_SIZE_ERROR(6L)"),
    ngx_string("REFUSED_STREAM(7L)"),
    ngx_string("CANCEL(8L)"),
    ngx_string("COMPRESSION_ERROR(9L)"),
    ngx_string("CONNECT_ERROR(10L)"),
    ngx_string("ENHANCE_YOUR_CALM(11L)"),
    ngx_string("INADEQUATE_SECURITY(12L)"),
    ngx_string("HTTP_1_1_REQUIRED(13L)"),
    ngx_string("UNKNOWN_ERROR(MAX)"),
};

static const ngx_nacos_grpc_frame_handler frame_handlers[] = {
    ngx_nacos_grpc_parse_data_frame,
    ngx_nacos_grpc_parse_header_frame,
    ngx_nacos_grpc_parse_unknown_frame,
    ngx_nacos_grpc_parse_rst_frame,
    ngx_nacos_grpc_parse_settings_frame,
    ngx_nacos_grpc_parse_unknown_frame,
    ngx_nacos_grpc_parse_ping_frame,
    ngx_nacos_grpc_parse_goaway_frame,
    ngx_nacos_grpc_parse_window_update_frame,
    ngx_nacos_grpc_parse_header_frame};

ngx_nacos_grpc_conn_t *ngx_nacos_open_grpc_conn(ngx_nacos_main_conf_t *conf,
                                                nc_grpc_event_handler handler) {
    ngx_connection_t *c;
    ngx_nacos_grpc_conn_t *gc;
    ngx_pool_t *pool;
    ngx_uint_t try;
    ngx_int_t rc;

    pool = ngx_create_pool(conf->udp_pool_size, conf->error_log);
    if (pool == NULL) {
        return NULL;
    }

    gc = ngx_pcalloc(pool, sizeof(*gc));
    if (gc == NULL) {
        ngx_destroy_pool(pool);
        return NULL;
    }
    gc->handler = handler;
    gc->nmcf = conf;
    gc->read_buf = ngx_create_temp_buf(pool, READ_BUF_CAP);
    if (gc->read_buf == NULL) {
        ngx_destroy_pool(pool);
        return NULL;
    }

    ngx_queue_init(&gc->all_streams);
    ngx_rbtree_init(&gc->st_tree, &gc->st_sentinel, ngx_rbtree_insert_value);

    gc->peer.start_time = ngx_current_msec;
    gc->peer.log_error = NGX_ERROR_INFO;
    gc->peer.log = conf->error_log;
    gc->peer.get = ngx_nacos_aux_get_addr;
    gc->peer.free = ngx_nacos_aux_free_addr;
    gc->peer.data = &conf->grpc_server_list;

    try = 0;

connect:
    rc = ngx_event_connect_peer(&gc->peer);
    try++;
    if (rc == NGX_ERROR) {
        if (gc->peer.name) {
            ngx_log_error(NGX_LOG_WARN, gc->peer.log, 0,
                          "http connection connect to %V error", gc->peer.name);
        }
        if (gc->peer.sockaddr) {
            gc->peer.free(&gc->peer, gc->peer.data, NGX_ERROR);
        }
        if (try < conf->server_list.nelts) {
            goto connect;
        }
        goto connect_failed;
    }

    c = gc->peer.connection;
    c->data = gc;
    c->pool = pool;
    c->write->handler = ngx_nacos_grpc_event_handler;
    c->read->handler = ngx_nacos_grpc_event_handler;
    c->requests = 0;
    gc->conn = c;
    gc->pool = pool;
    gc->settings.init_window_size = HTTP_V2_DEFAULT_WINDOW;
    gc->settings.max_frame_size = HTTP_V2_DEFAULT_FRAME_SIZE;

    if (rc == NGX_AGAIN) {
        // connecting
        gc->stat = connecting;
        c->log->action = "nacos http connection connecting";
        ngx_add_timer(c->write, 3000);  // set connect time out
        return gc;
    }

    // rc == NGX_OK
    rc = ngx_nacos_grpc_write_handler(gc, c->write);
    if (rc == NGX_OK || rc == NGX_AGAIN) {
        return gc;
    }

connect_failed:
    ngx_log_error(NGX_LOG_WARN, gc->peer.log, 0,
                  "create http connection error after try %d", try);
    ngx_destroy_pool(pool);
    return NULL;
}

static ngx_nacos_grpc_stream_t *ngx_nacos_grpc_request_internal(
    ngx_nacos_grpc_conn_t *conn, ngx_str_t *req, stream_handler handler) {
    ngx_nacos_grpc_buf_t *hbuf = NULL;
    ngx_nacos_grpc_stream_t *st;

    st = ngx_nacos_grpc_create_stream(conn);
    if (st == NULL) {
        goto err;
    }

    st->stream_handler = handler;
    hbuf = ngx_nacos_grpc_encode_request(st, req);
    if (hbuf == NULL) {
        goto err;
    }
    hbuf->sent_header = 1;
    if (ngx_nacos_grpc_send_buf(hbuf, 1) != NGX_OK) {
        goto err;
    }
    return st;
err:
    if (hbuf != NULL) {
        ngx_nacos_grpc_free_buf(st, hbuf);
    }
    if (st != NULL) {
        ngx_nacos_grpc_close_stream(st, 1);
    }
    return NULL;
}

ngx_nacos_grpc_stream_t *ngx_nacos_grpc_request(ngx_nacos_grpc_conn_t *conn,
                                                stream_handler handler) {
    ngx_str_t req;
    ngx_str_set(&req, "/Request/request");
    return ngx_nacos_grpc_request_internal(conn, &req, handler);
}

ngx_nacos_grpc_stream_t *ngx_nacos_grpc_bi_request(ngx_nacos_grpc_conn_t *conn,
                                                   stream_handler handler) {
    ngx_str_t req;
    ngx_str_set(&req, "/BiRequestStream/requestBiStream");
    return ngx_nacos_grpc_request_internal(conn, &req, handler);
}

ngx_int_t ngx_nacos_grpc_send(ngx_nacos_grpc_stream_t *st,
                              ngx_nacos_payload_t *p) {
    ngx_nacos_grpc_buf_t *bbuf;

    ngx_nacos_grpc_payload_encode_t en;

    en.type = payload_type_mapping[p->type].type_name;
    en.json = p->json_str;
    bbuf = ngx_nacos_grpc_encode_data_msg(st, &en, p->end);
    if (bbuf == NULL) {
        return NGX_ERROR;
    }

    return ngx_nacos_grpc_send_buf(bbuf, 1);
}

static void ngx_nacos_grpc_event_handler(ngx_event_t *ev) {
    ngx_connection_t *c;
    ngx_nacos_grpc_conn_t *gc;
    ngx_int_t rc;

    c = ev->data;
    gc = c->data;
    if (ev == c->read) {
        rc = ngx_nacos_grpc_read_handler(gc, ev);
        if (rc == NGX_AGAIN && ngx_handle_read_event(ev, 0) != NGX_OK) {
            rc = NGX_ERROR;
        }
    } else {
        rc = ngx_nacos_grpc_write_handler(gc, ev);
        if (rc == NGX_AGAIN && ngx_handle_write_event(ev, 0) != NGX_OK) {
            rc = NGX_ERROR;
        }
    }

    if (rc == NGX_ERROR || rc == NGX_DONE) {
        ngx_nacos_grpc_close_connection(
            gc, rc == NGX_ERROR ? NGX_NC_ERROR : NGX_NC_TIRED);
    }
}

static ngx_int_t ngx_nacos_grpc_write_handler(ngx_nacos_grpc_conn_t *gc,
                                              ngx_event_t *ev) {
    int err;
    socklen_t len;
    ngx_connection_t *c = gc->peer.connection;

    if (ev->timedout) {
        ev->timedout = 0;
        if (gc->stat == connecting) {
            return NGX_ERROR;
        }
        if (gc->stat == working) {
            // send ping.
            if (ngx_nacos_send_ping_frame(gc) == NGX_ERROR) {
                return NGX_ERROR;
            }
        } else {
            return NGX_ERROR;
        }
    }

    if (gc->stat == connecting) {
        if (ev->timer_set) {
            ngx_del_timer(ev);
        }

        err = 0;
        len = sizeof(err);
        if (getsockopt(c->fd, SOL_SOCKET, SO_ERROR, (void *) &err, &len) ==
            -1) {
            err = ngx_socket_errno;
        }

        if (err) {
            c->log->action = "connecting to upstream";
            (void) ngx_connection_error(c, err, "connect() failed");
            return NGX_ERROR;
        }

        ngx_log_debug0(NGX_LOG_DEBUG_CORE, gc->conn->log, 0,
                       "http connection connect successfully");
        gc->stat = prepare_grpc;
    }

    if (gc->stat == prepare_grpc && gc->m_stream == NULL) {
        gc->m_stream = ngx_nacos_grpc_create_stream(gc);
        if (gc->m_stream == NULL) {
            return NGX_ERROR;
        }
        return ngx_nacos_grpc_send_conn_start(gc->m_stream);
    }

    if (ev->ready) {
        return ngx_nacos_grpc_do_send(gc);
    }

    return NGX_OK;
}

static ngx_int_t ngx_nacos_grpc_read_handler(ngx_nacos_grpc_conn_t *gc,
                                             ngx_event_t *ev) {
    ssize_t rc;

    if (gc->stat < prepare_grpc) {
        // write handler 处理
        return NGX_OK;
    }

    for (;;) {
        rc = gc->conn->recv(gc->conn, gc->read_buf->last,
                            (gc->read_buf->end - gc->read_buf->last));
        if (rc > 0) {
            gc->read_buf->last += rc;
            rc = ngx_nacos_grpc_parse_frame(gc);
            if (rc == NGX_DONE || rc == NGX_ERROR) {
                return rc;
            }
            continue;
        } else if (rc == 0) {
            return NGX_DONE;
        } else if (rc == NGX_AGAIN) {
            return NGX_AGAIN;
        }
    }
}

static ngx_int_t ngx_nacos_grpc_parse_frame(ngx_nacos_grpc_conn_t *gc) {
    ngx_buf_t *b;
    size_t len;
    ngx_int_t rc;
    u_char *pp, *lp;

    b = gc->read_buf;

    for (;;) {
        len = b->last - b->pos;

        if (gc->parse_stat == parse_frame_header) {
            if (len < 9) {
                break;
            }
            gc->frame_size =
                (((size_t) b->pos[0]) << 16) | (b->pos[1] << 8) | (b->pos[2]);
            gc->frame_type = b->pos[3];
            gc->frame_flags = b->pos[4];
            gc->frame_stream_id = (((ngx_uint_t) (b->pos[5] & 0x7f)) << 24) |
                                  (b->pos[6] << 16) | (b->pos[7] << 8) |
                                  b->pos[8];
            gc->parse_stat = parse_frame_payload;
            b->pos += 9;
            gc->frame_start = 1;
            len -= 9;

            if (gc->frame_type >
                sizeof(frame_handlers) / sizeof(ngx_nacos_grpc_frame_handler)) {
                ngx_log_error(NGX_LOG_ERR, gc->conn->log, 0,
                              "nacos http2 protocol error. error frame type");
                return NGX_ERROR;
            }

            if (gc->frame_size > MAX_FRAME_SIZE) {
                ngx_log_error(
                    NGX_LOG_ERR, gc->conn->log, 0,
                    "nacos http2 protocol error. exceed max frame size");
                return NGX_ERROR;
            }
        }

        if (gc->parse_stat == parse_frame_payload) {
            gc->frame_end = len >= gc->frame_size ? 1 : 0;
            if (!gc->frame_end) {
                break;
            }

            lp = b->last;
            if ((size_t) (lp - b->pos) > gc->frame_size) {
                b->last = b->pos + gc->frame_size;
            }
            pp = b->last;
            rc = frame_handlers[gc->frame_type](gc);
            b->pos = pp;
            b->last = lp;

            if (rc != NGX_OK) {
                return rc;
            }
            gc->parse_stat = parse_frame_header;
        }
    }

    len = b->last - b->pos;
    if (len == 0) {
        b->pos = b->last = b->start;
    } else if (len > 0 && len * 4 < (size_t) (b->end - b->start) &&
               (size_t) (b->end - b->pos) * 2 < (size_t) (b->end - b->start)) {
        ngx_memcpy(b->start, b->pos, len);
        b->pos = b->start;
        b->last = b->pos + len;
    }
    return NGX_AGAIN;
}

static ngx_nacos_grpc_stream_t *ngx_nacos_grpc_create_stream(
    ngx_nacos_grpc_conn_t *gc) {
    ngx_pool_t *pool;
    ngx_nacos_grpc_stream_t *st;

    if (gc->next_stream_id == 0) {
        pool = gc->pool;
    } else {
        pool = ngx_create_pool(gc->nmcf->udp_pool_size, gc->nmcf->error_log);
        if (pool == NULL) {
            return NULL;
        }
    }

    st = ngx_pcalloc(pool, sizeof(*st));
    if (st == NULL) {
        goto err;
    }

    st->pool = pool;
    st->conn = gc;
    st->send_win = gc->settings.init_window_size;
    st->recv_win = 0x7fffffff;  // 发送了 init_window 和 window_update
    st->grpc_status = NGX_NACOS_GRPC_DEFAULT_GRPC_STATUS;
    st->stream_id = st->node.key = gc->next_stream_id;
    if (gc->next_stream_id == 0) {
        gc->next_stream_id = 1;
    } else {
        gc->next_stream_id += 2;
    }

    ngx_queue_insert_tail(&gc->all_streams, &st->queue);
    ngx_rbtree_insert(&gc->st_tree, &st->node);
    return st;
err:
    if (gc->next_stream_id != 0) {
        ngx_destroy_pool(pool);
    }
    return NULL;
}

void ngx_nacos_grpc_close_stream(ngx_nacos_grpc_stream_t *st,
                                 ngx_flag_t force) {
    ngx_nacos_grpc_conn_t *gc;
    ngx_nacos_payload_t payload;
    ngx_nacos_grpc_buf_t *h, *n;

    gc = st->conn;
    if (st == gc->m_stream) {
        return;
    }

    if (st->stream_handler) {
        ngx_memzero(&payload, sizeof(payload));
        payload.end = 1;
        payload.type = NONE__END;
        payload.msg_state = pl_error;
        st->stream_handler(st, &payload);
        st->stream_handler = NULL;
    }

    if (!force && st->header_sent && !st->end_stream) {
        // TODO send reset frame
    }

    if (force) {
        h = st->sending_bufs.head;
        while (h != NULL) {
            st->buf_size--;
            n = h->next;
            ngx_free(h);
            h = n;
        }
    }

    if (st->buf_size != 0) {
        return;
    }

    ngx_queue_remove(&st->queue);
    ngx_rbtree_delete(&gc->st_tree, &st->node);
    ngx_destroy_pool(st->pool);
}

static ngx_int_t ngx_nacos_grpc_send_conn_start(ngx_nacos_grpc_stream_t *st) {
    ngx_nacos_grpc_buf_t *buf;

    buf = ngx_nacos_grpc_alloc_buf(st,
                                   sizeof(ngx_nacos_grpc_connection_start) - 1);
    if (buf == NULL) {
        return NGX_ERROR;
    }

    buf->stream = st;
    buf->len = sizeof(ngx_nacos_grpc_connection_start) - 1;
    ngx_memcpy(buf->b, ngx_nacos_grpc_connection_start, buf->len);

    return ngx_nacos_grpc_send_buf(buf, 0);
}

static ngx_nacos_grpc_buf_t *ngx_nacos_grpc_alloc_buf(
    ngx_nacos_grpc_stream_t *st, size_t cap) {
    ngx_nacos_grpc_buf_t *buf;
    u_char *b;

    buf = ngx_alloc(sizeof(ngx_nacos_grpc_buf_t) + cap, st->pool->log);
    if (buf == NULL) {
        return NULL;
    }

    st->buf_size++;

    b = ((u_char *) buf) + sizeof(ngx_nacos_grpc_buf_t);
    buf->b = b;
    buf->stream = st;
    buf->next = NULL;
    buf->cap = cap;
    buf->len = 0;
    buf->consume_win = 0;
    buf->sent_header = buf->sent_request = 0;
    return buf;
}

static ngx_int_t ngx_nacos_grpc_send_buf(ngx_nacos_grpc_buf_t *buf,
                                         ngx_flag_t can_block) {
    ngx_nacos_grpc_stream_t *st, *m_st;
    ngx_nacos_grpc_conn_t *gc;
    ngx_nacos_grpc_bufs_t *bufs;

    st = buf->stream;
    gc = st->conn;
    m_st = gc->m_stream;

    bufs = can_block && st != m_st ? &st->sending_bufs : &gc->io_sending_bufs;
    if (bufs->tail) {
        bufs->tail->next = buf;
    } else {
        bufs->head = buf;
    }
    while (buf->next) {
        buf = buf->next;
    }
    bufs->tail = buf;

    if (can_block && st != m_st) {
        ngx_nacos_grpc_consume_sending_buf(st, &m_st->sending_bufs);
        ngx_nacos_grpc_consume_sending_buf(m_st, &gc->io_sending_bufs);
    }

    if (ngx_nacos_grpc_do_send(gc) == NGX_ERROR) {
        return NGX_ERROR;
    }
    return NGX_OK;
}

// return NGX_OK/NGX_AGAIN/NGX_ERROR
static ngx_int_t ngx_nacos_grpc_do_send(ngx_nacos_grpc_conn_t *gc) {
    ngx_nacos_grpc_buf_t *h, *t, *n;
    ssize_t r;
    ngx_connection_t *conn;
    ngx_nacos_grpc_stream_t *st;

    h = gc->io_sending_bufs.head;
    conn = gc->conn;
    r = NGX_OK;

    if (h == NULL) {
        return r;
    }

    do {
        r = conn->send(gc->conn, h->b, h->len);
        if (r == (ssize_t) h->len) {
            h = h->next;
            r = NGX_OK;
        } else if (r > 0) {
            h->len -= r;
            h->b += r;
            r = NGX_AGAIN;
            break;
        } else {
            break;
        }
    } while (h != NULL);

    t = h;
    h = gc->io_sending_bufs.head;
    gc->io_sending_bufs.head = t;
    if (t == NULL) {
        gc->io_sending_bufs.tail = NULL;
    }

    while (h != t) {
        n = h->next;
        st = h->stream;
        if (h->sent_header) {
            st->header_sent = 1;
        }
        if (h->sent_request) {
            st->req_sent = 1;
        }
        ngx_nacos_grpc_free_buf(st, h);
        if (st->buf_size == 0 && st->req_sent && st->end_stream) {
            ngx_nacos_grpc_close_stream(h->stream, 0);
        }
        h = n;
    }

    return r;
}

void ngx_nacos_grpc_close_connection(ngx_nacos_grpc_conn_t *gc,
                                     ngx_int_t status) {
    ngx_queue_t *stq;
    ngx_nacos_grpc_stream_t *st;
    ngx_nacos_grpc_buf_t *h, *n;

    if (gc->handler) {
        gc->handler(gc, nc_error);
        gc->handler = NULL;
    }

    if (gc->m_stream) {
        st = gc->m_stream;
        ngx_queue_remove(&st->queue);
        ngx_rbtree_delete(&gc->st_tree, &st->node);

        h = st->sending_bufs.head;
        st->sending_bufs.head = NULL;
        st->sending_bufs.tail = NULL;
        while (h != NULL) {
            n = h->next;
            st = h->stream;
            st->buf_size--;
            ngx_free(h);
            h = n;
        }
    }

    h = gc->io_sending_bufs.head;
    while (h != NULL) {
        n = h->next;
        st = h->stream;
        st->buf_size--;
        ngx_free(h);
        h = n;
    }

    h = gc->io_sending_bufs.head = NULL;
    h = gc->io_sending_bufs.tail = NULL;

    while (!(ngx_queue_empty(&gc->all_streams))) {
        stq = ngx_queue_last(&gc->all_streams);
        st = ngx_queue_data(stq, ngx_nacos_grpc_stream_t, queue);
        ngx_nacos_grpc_close_stream(st, 1);
    }

    ngx_close_connection(gc->conn);
    gc->peer.free(&gc->peer, gc->peer.data, status);
    ngx_destroy_pool(gc->pool);
}

static ngx_int_t ngx_nacos_grpc_parse_unknown_frame(ngx_nacos_grpc_conn_t *gc) {
    ngx_log_debug2(NGX_LOG_DEBUG_CORE, gc->conn->log, 0,
                   "unknown frame type:%d, flags:%d", gc->frame_type,
                   gc->frame_flags);
    return NGX_ERROR;
}

static ngx_int_t ngx_nacos_grpc_parse_data_frame(ngx_nacos_grpc_conn_t *gc) {
    ngx_nacos_grpc_stream_t *st;
    ngx_buf_t *buf, *tb;
    u_char *p, *t;
    ngx_int_t rc;
    size_t len, msg_size;
    ngx_str_t proto_msg;
    enum state { parsing_prefix, parsing_msg };

    st = ngx_nacos_grpc_find_stream(gc, gc->frame_stream_id);
    if (gc->frame_stream_id == 0 || st == NULL) {
        ngx_log_error(NGX_LOG_ERR, gc->conn->log, 0,
                      "nacos server sent data frame "
                      "zero stream id: %uz",
                      gc->frame_stream_id);
        return NGX_ERROR;
    }

    if (!st->end_header) {
        ngx_log_error(NGX_LOG_ERR, gc->conn->log, 0,
                      "nacos server sent data frame "
                      "header not complete: %uz",
                      gc->frame_stream_id);
        return NGX_ERROR;
    }

    rc = NGX_OK;
    tb = st->tmp_buf;
    buf = gc->read_buf;

    for (;;) {
        p = buf->pos;
        len = buf->last - p;

        if (len == 0) {
            rc = NGX_OK;
            break;
        }

        if (gc->frame_start) {
            if (gc->frame_flags & HTTP_V2_PADDED_FLAG) {
                if (len < 1) {
                    return NGX_AGAIN;
                }
                st->padding = p[0];
                ++p;
                buf->pos = p;
                st->recv_win--;
            } else {
                st->padding = 0;
            }
        }

        if (st->parsing_state == parsing_prefix) {
            if (st->store_proto_size_buf) {
                tb = st->tmp_buf;
                if (len > 5 - (size_t) (tb->last - tb->pos)) {
                    msg_size = 5 - (tb->last - tb->pos);
                } else {
                    msg_size = len;
                }
                memcpy(tb->last, p, msg_size);
                tb->last += msg_size;
                p += msg_size;
                buf->pos = p;
                if (tb->last - tb->pos < 5) {
                    rc = NGX_OK;
                    break;
                }
            } else if (len < 5) {
                if (ngx_nacos_grpc_realloc_tmp_buf(st, 256) != NGX_OK) {
                    rc = NGX_ERROR;
                    break;
                }

                tb = st->tmp_buf;
                memcpy(tb->last, p, len);
                tb->last += len;
                p += len;
                buf->pos = p;
                rc = NGX_OK;
                st->store_proto_size_buf = 1;
                break;
            }

            if (st->store_proto_size_buf) {
                t = tb->pos;
                tb->pos = tb->last = tb->start;
            } else {
                t = p;
                p += 5;
                len -= 5;
                buf->pos = p;
            }

            if (t[0] != 0) {
                ngx_log_error(NGX_LOG_ERR, gc->conn->log, 0,
                              "nacos server sent data frame "
                              "send compressed msg: %uz",
                              gc->frame_stream_id);
                rc = NGX_ERROR;
                break;
            }
            msg_size = (t[1] << 24) | (t[2] << 16) | (t[3] << 8) | t[4];

            if (ngx_nacos_grpc_realloc_tmp_buf(st, msg_size) != NGX_OK) {
                rc = NGX_ERROR;
                break;
            }
            tb = st->tmp_buf;
            st->parsing_state = parsing_msg;
            st->proto_len = msg_size;
            st->recv_win -= 5;
        }

        if (st->parsing_state == parsing_msg) {
            if (len > st->proto_len - (tb->last - tb->pos)) {
                len = st->proto_len - (tb->last - tb->pos);
            }
            ngx_memcpy(tb->last, p, len);
            tb->last += len;
            p += len;
            buf->pos = p;
            st->recv_win -= len;
            if (gc->frame_end && st->padding) {
                tb->last -= st->padding;
                st->padding = 0;
            }
            len = tb->last - tb->pos;
            if (st->proto_len > 300000) {
                st->padding = 0;
            }
            if (len == st->proto_len) {
                proto_msg.len = len;
                proto_msg.data = tb->pos;
                rc = ngx_nacos_grpc_parse_proto_msg(st, &proto_msg);
                tb->pos = tb->last = tb->start;
                st->parsing_state = parsing_prefix;
                if (rc != NGX_OK) {
                    break;
                }
            }
        }
    }

    if (rc == NGX_ERROR) {
        return NGX_ERROR;
    }

    if (gc->frame_end) {
        st->end_stream = gc->frame_flags & HTTP_V2_END_STREAM_FLAG ? 1 : 0;
    }

    if (rc == NGX_OK && gc->frame_end && !st->end_stream &&
        st->recv_win < 1024 * 1024) {
        if (ngx_nacos_grpc_send_win_update_frame(st, 512 * 1024 * 1024) ==
            NGX_ERROR) {
            return NGX_ERROR;
        }
    }

    if (gc->frame_end && gc->m_stream->recv_win < 5 * 1024 * 1024) {
        if (ngx_nacos_grpc_send_win_update_frame(
                gc->m_stream, 500 * 1024 * 1024) == NGX_ERROR) {
            return NGX_ERROR;
        }
    }
    if (st->end_stream) {
        ngx_nacos_grpc_close_stream(st, 0);
    }
    return rc;
}

static ngx_int_t ngx_nacos_grpc_parse_proto_msg(ngx_nacos_grpc_stream_t *st,
                                                ngx_str_t *proto_msg) {
    ngx_nacos_grpc_payload_decode_t de;
    ngx_int_t rc;

    if (st->resp_status == 200) {
        rc = NGX_OK;
        if (st->resp_grpc_encode && st->stream_handler) {
            de.input = *proto_msg;

            rc = ngx_nacos_grpc_decode_payload(st, &de);

            if (rc == NGX_OK) {
                rc = st->stream_handler(st, &de.payload);
                if (rc != NGX_OK && rc != NGX_DONE) {
                    ngx_log_error(
                        NGX_LOG_WARN, st->conn->conn->log, 0,
                        "receive nacos proto msg [%V]:[%d] when receiving: %V",
                        &de.type, rc, &de.out_json);
                }
            } else {
                ngx_memzero(&de.payload, sizeof(de.payload));
                de.payload.type = NONE__END;
                de.payload.msg_state = pl_error;
                rc = st->stream_handler(st, &de.payload);
                ngx_log_error(NGX_LOG_WARN, st->conn->conn->log, 0,
                              "decode proto msg error:[%d]", rc);
            }

            if (rc != NGX_OK) {
                st->stream_handler = NULL;
            }

            if (rc != NGX_ERROR) {
                rc = NGX_OK;
            }
        }
        return rc;
    }
    return NGX_ERROR;
}

static ngx_int_t ngx_nacos_grpc_parse_header_frame(ngx_nacos_grpc_conn_t *gc) {
    ngx_nacos_grpc_stream_t *st;
    u_char flags;
    ngx_buf_t *b, *tb;
    size_t min, len, field_len;
    ngx_uint_t dep;
    ngx_flag_t huffmanEncoded;
    u_char ch, index, *p, *last, *tp, tmp[512];
    ngx_str_t key, value;
    ngx_int_t rc;

    enum {
        s_start = 0,
        s_indexed_header_name,
        s_literal_header_name_length_prefix,
        s_literal_header_name_length,
        s_literal_header_name,
        s_literal_header_value_length_prefix,
        s_literal_header_value_length,
        s_literal_header_value,
    } state;

    enum parsing_stat { p_start = 0, p_receiving, p_end_header };

    st = ngx_nacos_grpc_find_stream(gc, gc->frame_stream_id);
    if (st == NULL) {
        ngx_log_error(NGX_LOG_WARN, gc->conn->log, 0,
                      "nacos server sent header frame "
                      "unknown stream id: %uz",
                      gc->frame_stream_id);
        return NGX_OK;
    }

    b = gc->read_buf;
    len = b->last - b->pos;
    p = b->pos;

    flags = gc->frame_flags;

    if (gc->frame_type == HTTP_V2_HEADERS_FRAME) {
        if (st->parsing_state == s_start) {
            st->resp_grpc_encode = 0;
            st->grpc_status = NGX_NACOS_GRPC_DEFAULT_GRPC_STATUS;
            // 解析 padding length 和 PRIORITY
            min = (flags & HTTP_V2_PADDED_FLAG ? 1 : 0) +
                  (flags & HTTP_V2_PRIORITY_FLAG ? 5 : 0);
            if (gc->frame_size < min) {
                ngx_log_error(NGX_LOG_ERR, gc->conn->log, 0,
                              "nacos server sent headers frame "
                              "with invalid length: %uz",
                              gc->frame_size);
                return NGX_ERROR;
            }
            if (len < min) {
                return NGX_AGAIN;
            }

            if (flags & HTTP_V2_PADDED_FLAG) {
                st->padding = *p++;
            }
            if (gc->frame_size < st->padding + min) {
                ngx_log_error(NGX_LOG_ERR, gc->conn->log, 0,
                              "nacos server sent headers frame "
                              "with invalid length: %uz",
                              gc->frame_size);
                return NGX_ERROR;
            }

            if (flags & HTTP_V2_PRIORITY_FLAG) {
                dep = ((p[0] & 0x7F) << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
                if (st->stream_id == dep) {
                    ngx_log_error(
                        NGX_LOG_ERR, gc->conn->log, 0,
                        "nacos server sent headers frame "
                        "with dependency stream id of the same as self: %uz",
                        gc->frame_size);
                    return NGX_ERROR;
                }
                p += 5;
            }
            b->pos = p;
            len = b->last - p;
            gc->frame_size -= min;
            gc->frame_flags &= ~(HTTP_V2_PADDED_FLAG | HTTP_V2_PRIORITY_FLAG);
            st->parsing_state = p_receiving;
        }
    }

    if (st->parsing_state == p_receiving) {
        if (ngx_nacos_grpc_realloc_tmp_buf(st, len) != NGX_OK) {
            return NGX_ERROR;
        }
        tb = st->tmp_buf;
        ngx_memcpy(tb->last, p, len);
        tb->last += len;
        b->pos += len;

        if (gc->frame_type == HTTP_V2_HEADERS_FRAME) {
            if (st->padding && gc->frame_end) {
                // remove padding
                tb->last -= st->padding;
                st->padding = 0;
            }
        }

        if (gc->frame_end &&
            (gc->frame_flags & HTTP_V2_END_HEADERS_FLAG) != 0) {
            st->end_header = 1;
            st->parsing_state = p_end_header;
            goto parse_header;
        }
        return NGX_OK;
    }

    ngx_log_error(NGX_LOG_ERR, gc->conn->log, 0,
                  "nacos server sent headers frame "
                  "with unknown header status");
    return NGX_ERROR;

// parsing header real
parse_header:
    p = tb->pos;
    last = tb->last;
    state = 0;
    huffmanEncoded = 0;
    field_len = 0;
    index = 0;
    ngx_str_null(&key);
    ngx_str_null(&value);
    for (; p < last;) {
        switch (state) {
            case s_start:
                ch = *p++;
                if ((ch & 0x80) == 0x80) {
                    index = ch & ~0x80;
                    if (index == 0 || index > 61) {
                        ngx_log_error(NGX_LOG_ERR, gc->conn->log, 0,
                                      "nacos server sent invalid http2 "
                                      "table index: %ui",
                                      index);
                        return NGX_ERROR;
                    }
                    key = *ngx_nacos_http_v2_get_static_name(index);
                    value = *ngx_nacos_http_v2_get_static_value(index);
                    goto parse;
                } else if ((ch & 0xc0) == 0x40) {
                    index = ch & ~0xc0;
                    if (index > 61) {
                        ngx_log_error(NGX_LOG_ERR, gc->conn->log, 0,
                                      "nacos server sent invalid http2 "
                                      "table index: %ui",
                                      index);
                        return NGX_ERROR;
                    }
                    if (index == 0) {
                        state = s_literal_header_name_length_prefix;
                    } else if (index == 0x3F) {
                        state = s_indexed_header_name;
                    } else {
                        key = *ngx_nacos_http_v2_get_static_name(index);
                        state = s_literal_header_value_length_prefix;
                    }
                } else if ((ch & 0xe0) == 0x20) {
                    index = ch & 0x1F;
                    if (index > 0) {
                        ngx_log_error(NGX_LOG_ERR, gc->conn->log, 0,
                                      "nacos server sent invalid http2 "
                                      "dynamic table size update: %ui",
                                      index);
                        return NGX_ERROR;
                    }
                } else {
                    index = ch & 0x0F;
                    if (index == 0) {
                        state = s_literal_header_name_length_prefix;
                    } else if (index == 0x0F) {
                        state = s_indexed_header_name;
                    } else {
                        key = *ngx_nacos_http_v2_get_static_name(index);
                        state = s_literal_header_value_length_prefix;
                    }
                }
                break;
            case s_indexed_header_name:
                field_len = index;
                if (ngx_nacos_grpc_decode_ule128(&p, last, &field_len) !=
                    NGX_OK) {
                    return NGX_ERROR;
                }
                if (field_len == 0 || field_len > 61) {
                    ngx_log_error(NGX_LOG_ERR, gc->conn->log, 0,
                                  "nacos server sent invalid http2 "
                                  "table index: %ui",
                                  index);
                    return NGX_ERROR;
                }
                index = field_len & 0xFF;
                key = *ngx_nacos_http_v2_get_static_name(index);
                state = s_literal_header_value_length_prefix;
                break;
            case s_literal_header_name_length_prefix:
                ch = *p++;
                huffmanEncoded = (ch & 0x80) == 0x80;
                index = ch & 0x7F;
                if (index == 0x7f) {
                    state = s_literal_header_name_length;
                } else {
                    field_len = index;
                    state = s_literal_header_name;
                }
                break;
            case s_literal_header_name_length:
                field_len = index;
                if (ngx_nacos_grpc_decode_ule128(&p, last, &field_len) !=
                    NGX_OK) {
                    return NGX_ERROR;
                }
                state = s_literal_header_name;
                break;
            case s_literal_header_name:
                if (field_len > (size_t) (last - p)) {
                    ngx_log_error(NGX_LOG_ERR, gc->conn->log, 0,
                                  "nacos server sent invalid http2 "
                                  "header name length: %ui",
                                  field_len);
                    return NGX_ERROR;
                }
                if (huffmanEncoded) {
                    ch = 0;
                    tp = tmp;
                    if (ngx_nacos_http_v2_huff_decode(&ch, p, field_len, &tp, 1,
                                                      gc->conn->log) !=
                        NGX_OK) {
                        ngx_log_error(
                            NGX_LOG_ERR, gc->conn->log, 0,
                            "nacos server sent invalid encoded header");
                        return NGX_ERROR;
                    }
                    key.data = tmp;
                    key.len = tp - key.data;
                } else {
                    key.data = p;
                    key.len = field_len;
                }
                p += field_len;
                state = s_literal_header_value_length_prefix;
                break;
            case s_literal_header_value_length_prefix:
                ch = *p++;
                huffmanEncoded = (ch & 0x80) == 0x80;
                index = ch & 0x7F;
                if (index == 0x7f) {
                    state = s_literal_header_value_length;
                } else if (index == 0) {
                    ngx_str_set(&value, "");
                    goto parse;
                } else {
                    field_len = index;
                    state = s_literal_header_value;
                }
                break;
            case s_literal_header_value_length:
                field_len = index;
                if (ngx_nacos_grpc_decode_ule128(&p, last, &field_len) !=
                    NGX_OK) {
                    return NGX_ERROR;
                }
                state = s_literal_header_value;
                break;
            case s_literal_header_value:
                if (field_len > (size_t) (last - p)) {
                    ngx_log_error(NGX_LOG_ERR, gc->conn->log, 0,
                                  "nacos server sent invalid http2 "
                                  "header value length: %ui",
                                  field_len);
                    return NGX_ERROR;
                }
                if (huffmanEncoded) {
                    ch = 0;
                    tp = tmp;
                    if (ngx_nacos_http_v2_huff_decode(&ch, p, field_len, &tp, 1,
                                                      gc->conn->log) !=
                        NGX_OK) {
                        ngx_log_error(
                            NGX_LOG_ERR, gc->conn->log, 0,
                            "nacos server sent invalid encoded header");
                        return NGX_ERROR;
                    }
                    value.data = tmp;
                    value.len = tp - value.data;
                } else {
                    value.data = p;
                    value.len = field_len;
                }
                p += field_len;
                goto parse;
        }
        continue;
    parse:
        if (key.len == sizeof(":status") - 1 &&
            ngx_strncmp(key.data, ":status", key.len) == 0) {
            if (value.len != 3) {
                ngx_log_error(NGX_LOG_ERR, gc->conn->log, 0,
                              "nacos server sent invalid :status \"%V\"",
                              &value);
                return NGX_ERROR;
            }
            st->resp_status = ngx_atoi(value.data, 3);
        } else if (key.len == sizeof("content-type") - 1 &&
                   ngx_strncmp(key.data, "content-type", key.len) == 0) {
            if (value.len == sizeof("application/grpc") - 1 &&
                ngx_strncmp(value.data, "application/grpc", value.len) == 0) {
                st->resp_grpc_encode = 1;
            } else {
                st->resp_grpc_encode = 0;
            }
        } else if (key.len == sizeof("grpc-status") - 1 &&
                   ngx_strncmp(key.data, "grpc-status", key.len) == 0) {
            if (value.len > 2 || value.len == 0) {
                ngx_log_error(NGX_LOG_ERR, gc->conn->log, 0,
                              "nacos server sent invalid grpc-status \"%V\"",
                              &value);
                return NGX_ERROR;
            }
            st->grpc_status = ngx_atoi(value.data, value.len);
        }
        state = s_start;
    }

    tb->pos = tb->last = tb->start;
    st->parsing_state = 0;
    rc = NGX_OK;
    st->end_stream = flags & HTTP_V2_END_STREAM_FLAG ? 1 : 0;
    if (st->end_stream && st->req_sent) {
        ngx_nacos_grpc_close_stream(st, 0);
    }

    return rc;
}

static ngx_int_t ngx_nacos_grpc_parse_rst_frame(ngx_nacos_grpc_conn_t *gc) {
    ngx_nacos_grpc_stream_t *st;
    ngx_int_t rc;
    u_char *p;
    ngx_uint_t reason;

    rc = NGX_OK;
    st = ngx_nacos_grpc_find_stream(gc, gc->frame_stream_id);
    if (st == NULL) {
        ngx_log_error(NGX_LOG_ERR, gc->conn->log, 0,
                      "nacos server sent rst frame "
                      "unknown stream id: %uz",
                      gc->frame_stream_id);
        goto err;
    }

    if (gc->frame_size != 4) {
        ngx_log_error(NGX_LOG_ERR, gc->conn->log, 0,
                      "nacos server sent rst frame "
                      "invalid length: %uz",
                      gc->frame_size);
        return NGX_ERROR;
    }

    p = gc->read_buf->pos;
    reason = (((size_t) p[0]) << 24) | ((size_t) p[1] << 16) |
             ((size_t) p[2] << 8) | ((size_t) p[3]);
    if (reason < sizeof(http2_err) / sizeof(http2_err[0])) {
        ngx_log_error(NGX_LOG_ERR, gc->conn->log, 0,
                      "nacos server sent rst frame on %ud "
                      "reason: %V",
                      gc->frame_stream_id, &http2_err[reason]);
    }

    st->end_stream = 1;
    rc = NGX_OK;
err:
    ngx_nacos_grpc_close_stream(st, 1);
    return rc;
}

static ngx_int_t ngx_nacos_grpc_parse_settings_frame(
    ngx_nacos_grpc_conn_t *gc) {
    ngx_uint_t key, value, window_update;
    ngx_buf_t *b;
    size_t len;
    ngx_nacos_grpc_buf_t *buf;
    ngx_queue_t *node;
    ngx_nacos_grpc_stream_t *st;

    if (gc->frame_stream_id) {
        ngx_log_error(NGX_LOG_ERR, gc->conn->log, 0,
                      "nacos server sent settings frame "
                      "with non-zero stream id: %ui",
                      gc->frame_stream_id);
        return NGX_ERROR;
    }
    if (gc->frame_flags & HTTP_V2_ACK_FLAG) {
        if (gc->frame_size != 0) {
            ngx_log_error(NGX_LOG_ERR, gc->conn->log, 0,
                          "nacos server sent settings frame "
                          "with ack flag and non-zero length: %uz",
                          gc->frame_size);
            return NGX_ERROR;
        }
        if (gc->stat == prepare_grpc) {
            gc->stat = working;
            if (gc->handler(gc, nc_connected) != NGX_OK) {
                return NGX_ERROR;
            }

            ngx_add_timer(gc->conn->write, NGX_NACOS_GRPC_DEFAULT_PING_INTERVAL);
        }
        return NGX_OK;
    }

    if (gc->frame_size % 6 != 0) {
        ngx_log_error(NGX_LOG_ERR, gc->conn->log, 0,
                      "nacos server sent settings frame "
                      "with invalid length: %uz",
                      gc->frame_size);
        return NGX_ERROR;
    }
    b = gc->read_buf;
    len = 0;

    while (b->last - b->pos >= 6 && len < gc->frame_size) {
        key = (b->pos[0] << 8) | b->pos[1];
        value = ((ngx_uint_t) b->pos[2] << 24) |
                ((ngx_uint_t) b->pos[3] << 16) | ((ngx_uint_t) b->pos[4] << 8) |
                b->pos[5];
        b->pos += 6;
        len += 6;
        switch (key) {
            case 1:
                gc->settings.header_table_size = value;
                break;
            case 3:
                gc->settings.max_conn_streams = value;
                break;
            case 4:
                if (value > HTTP_V2_MAX_WINDOW) {
                    ngx_log_error(NGX_LOG_ERR, gc->conn->log, 0,
                                  "nacos server sent settings frame "
                                  "with too large initial window size: %ui",
                                  value);
                    return NGX_ERROR;
                }
                window_update = value - gc->settings.init_window_size;
                gc->settings.init_window_size = value;
                for (node = ngx_queue_head(&gc->all_streams);
                     node != ngx_queue_sentinel(&gc->all_streams);
                     node = ngx_queue_next(node)) {
                    st = ngx_queue_data(node, ngx_nacos_grpc_stream_t, queue);
                    if (st == gc->m_stream) {
                        continue;
                    }
                    if (ngx_nacos_grpc_update_send_window(
                            gc, st->stream_id, window_update) == NGX_ERROR) {
                        return NGX_ERROR;
                    }
                }

                break;
            case 5:
                gc->settings.max_frame_size = value;
                break;
            case 6:
                gc->settings.max_header_list_size = value;
                break;
            default:
                ngx_log_error(NGX_LOG_ERR, gc->conn->log, 0,
                              "nacos server sent settings frame "
                              "with invalid key: %uz value: %uz",
                              key, value);
                return NGX_ERROR;
        }
    }
    if (len < gc->frame_size) {
        gc->frame_size -= len;
        return NGX_AGAIN;
    }
    buf = ngx_nacos_grpc_alloc_buf(gc->m_stream, 9);
    if (buf == NULL) {
        return NGX_ERROR;
    }
    ngx_memzero(buf->b, 9);
    buf->b[3] = HTTP_V2_SETTINGS_FRAME;
    buf->b[4] = HTTP_V2_ACK_FLAG;
    buf->len = 9;
    return ngx_nacos_grpc_send_buf(buf, 0);
}

static ngx_int_t ngx_nacos_grpc_parse_ping_frame(ngx_nacos_grpc_conn_t *gc) {
    u_char *p;
    uint64_t data;
    ngx_nacos_grpc_buf_t *buf;

    if (gc->frame_size != 8) {
        ngx_log_error(NGX_LOG_ERR, gc->conn->log, 0,
                      "nacos server sent ping frame "
                      "with invalid length: %uz",
                      gc->frame_size);
        return NGX_ERROR;
    }

    if (!gc->frame_end) {
        return NGX_AGAIN;
    }
    p = gc->read_buf->pos;
    gc->read_buf->pos = p + 8;
    data = ((uint64_t) p[0] << 56) | ((uint64_t) p[1] << 48) |
           ((uint64_t) p[2] << 40) | ((uint64_t) p[3] << 32) |
           ((uint64_t) p[4] << 24) | ((uint64_t) p[5] << 16) |
           ((uint64_t) p[6] << 8) | (uint64_t) p[7];
    if (gc->frame_flags & HTTP_V2_ACK_FLAG) {
        if (data != gc->heartbeat) {
            ngx_log_error(NGX_LOG_ERR, gc->conn->log, 0,
                          "nacos server sent ping frame "
                          "with invalid ack: %uz",
                          data);
            return NGX_ERROR;
        }
        ngx_add_timer(gc->conn->write, NGX_NACOS_GRPC_DEFAULT_PING_INTERVAL);
        return NGX_OK;
    }

    buf = ngx_nacos_grpc_alloc_buf(gc->m_stream, 9 + 8);
    if (buf == NULL) {
        return NGX_ERROR;
    }
    ngx_nacos_grpc_encode_frame_header(gc->m_stream, buf->b, HTTP_V2_PING_FRAME,
                                       HTTP_V2_ACK_FLAG, 8);
    buf->len = 9 + 8;
    p = buf->b + 9;
    p[0] = (data >> 56) & 0xFF;
    p[1] = (data >> 48) & 0xFF;
    p[2] = (data >> 40) & 0xFF;
    p[3] = (data >> 32) & 0xFF;
    p[4] = (data >> 24) & 0xFF;
    p[5] = (data >> 16) & 0xFF;
    p[6] = (data >> 8) & 0xFF;
    p[7] = data & 0xFF;
    return ngx_nacos_grpc_send_buf(buf, 0);
}

static ngx_int_t ngx_nacos_grpc_parse_goaway_frame(ngx_nacos_grpc_conn_t *gc) {
    u_char *p;
    ngx_uint_t last_st_id, err_code;
    ngx_str_t err_msg, *err_name;
    if (!gc->frame_end) {
        return NGX_AGAIN;
    }

    p = gc->read_buf->pos;

    last_st_id = ((p[0] & 0x7F) << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
    p += 4;
    err_code = (p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
    p += 4;
    err_msg.data = p;
    err_msg.len = gc->frame_size - 8;
    gc->read_buf->pos = gc->read_buf->last;

    if (err_code < sizeof(http2_err) / sizeof(ngx_str_t)) {
        err_name = &http2_err[err_code];
    } else {
        err_name = &http2_err[sizeof(http2_err) / sizeof(ngx_str_t) - 1];
    }

    ngx_log_error(NGX_LOG_ERR, gc->conn->log, 0,
                  "nacos server sent goaway frame:\n"
                  "\tlast_stream_id:%ul\n"
                  "\terr_name:%V\n\terr_msg:%V",
                  last_st_id, err_name, &err_msg);
    return NGX_DONE;
}

static ngx_int_t ngx_nacos_grpc_parse_window_update_frame(
    ngx_nacos_grpc_conn_t *gc) {
    ngx_buf_t *b;
    ngx_uint_t win_update;
    if (gc->frame_size != 4) {
        ngx_log_error(NGX_LOG_ERR, gc->conn->log, 0,
                      "nacos server sent window update frame "
                      "with invalid length: %uz",
                      gc->frame_size);
        return NGX_ERROR;
    }

    b = gc->read_buf;
    if (b->last - b->pos < 4) {
        return NGX_AGAIN;
    }
    win_update =
        (b->pos[0] & 0x7f) << 24 | b->pos[1] << 16 | b->pos[2] << 8 | b->pos[3];
    b->pos += 4;

    return ngx_nacos_grpc_update_send_window(gc, gc->frame_stream_id,
                                             win_update);
}

static ngx_nacos_grpc_stream_t *ngx_nacos_grpc_find_stream(
    ngx_nacos_grpc_conn_t *gc, ngx_uint_t st_id) {
    ngx_rbtree_node_t *node, *sentinel;

    node = gc->st_tree.root;
    sentinel = gc->st_tree.sentinel;
    while (node != sentinel) {
        if (st_id != node->key) {
            node = st_id < node->key ? node->left : node->right;
            continue;
        }
        return (ngx_nacos_grpc_stream_t *) ((char *) node -
                                            offsetof(ngx_nacos_grpc_stream_t,
                                                     node));
    }
    return NULL;
}

static ngx_int_t ngx_nacos_grpc_update_send_window(ngx_nacos_grpc_conn_t *gc,
                                                   ngx_uint_t st_id,
                                                   ngx_uint_t win_update) {
    ngx_nacos_grpc_stream_t *st;
    ngx_int_t rc;

    rc = NGX_OK;
    st = ngx_nacos_grpc_find_stream(gc, st_id);
    if (st == NULL) {
        if (st_id >= gc->next_stream_id) {
            ngx_log_error(NGX_LOG_WARN, gc->conn->log, 0,
                          "nacos server unknown frame id");
        }
        return NGX_OK;
    }

    if (win_update > (size_t) HTTP_V2_MAX_WINDOW - st->send_win) {
        ngx_log_error(NGX_LOG_ERR, gc->conn->log, 0,
                      "nacos server sent too large window update");
        return NGX_ERROR;
    }

    st->send_win += win_update;

    if (st->sending_bufs.head != NULL) {
        if (st != gc->m_stream) {
            ngx_nacos_grpc_consume_sending_buf(st, &gc->m_stream->sending_bufs);
            st = gc->m_stream;
        }
        ngx_nacos_grpc_consume_sending_buf(st, &gc->io_sending_bufs);
        rc = ngx_nacos_grpc_do_send(gc);
    }

    if (rc == NGX_AGAIN) {
        rc = NGX_OK;
    }
    return rc;
}

static void ngx_nacos_grpc_consume_sending_buf(
    ngx_nacos_grpc_stream_t *st, ngx_nacos_grpc_bufs_t *target_bufs) {
    ngx_nacos_grpc_buf_t *h, *t;
    ngx_flag_t stream_block;

    h = st->sending_bufs.head;
    if (h != NULL) {
        t = NULL;
        do {
            stream_block = st->send_win < h->consume_win;
            if (stream_block) {
                break;
            }
            st->send_win -= h->consume_win;
            t = h;
            h = h->next;
        } while (h != NULL);

        if (h != st->sending_bufs.head) {
            if (target_bufs->tail) {
                target_bufs->tail->next = st->sending_bufs.head;
            } else {
                target_bufs->head = st->sending_bufs.head;
            }
            target_bufs->tail = t;

            t->next = NULL;
            st->sending_bufs.head = h;
            if (h == NULL) {
                st->sending_bufs.tail = NULL;
            }
        }
    }
}

static ngx_nacos_grpc_buf_t *ngx_nacos_grpc_encode_request(
    ngx_nacos_grpc_stream_t *st, ngx_str_t *mtd) {
    ngx_nacos_grpc_buf_t *buf;
    size_t len;
    u_char *b;
    u_char tmp[128];

    buf = ngx_nacos_grpc_alloc_buf(st, 1024);
    if (buf == NULL) {
        return NULL;
    }

    b = buf->b + 9;
    // :method: POST
    *b++ = ngx_nacos_http_v2_indexed(HTTP_V2_METHOD_POST_INDEX);
    // :schema: http
    *b++ = ngx_nacos_http_v2_indexed(HTTP_V2_SCHEME_HTTP_INDEX);
    // path:
    *b++ = ngx_nacos_http_v2_inc_indexed(HTTP_V2_PATH_INDEX);
    b = ngx_nacos_http_v2_write_value(b, mtd->data, mtd->len, tmp);
    // AUTHORITY
    *b++ = ngx_nacos_http_v2_inc_indexed(HTTP_V2_AUTHORITY_INDEX);
    b = ngx_nacos_http_v2_write_value(b, (u_char *) "nacos-server",
                                      sizeof("nacos-server") - 1, tmp);
    // user-agent
    *b++ = ngx_nacos_http_v2_inc_indexed(HTTP_V2_USER_AGENT_INDEX);
    b = ngx_nacos_http_v2_write_value(b, (u_char *) "nginx-nacos-grpc-client",
                                      sizeof("nginx-nacos-grpc-client") - 1,
                                      tmp);
    // content-type
    *b++ = ngx_nacos_http_v2_inc_indexed(HTTP_V2_CONTENT_TYPE_INDEX);
    b = ngx_nacos_http_v2_write_value(b, (u_char *) "application/grpc",
                                      sizeof("application/grpc") - 1, tmp);
    // te: trailers
    *b++ = 0;
    b = ngx_nacos_http_v2_write_name(b, (u_char *) "te", sizeof("te") - 1, tmp);
    b = ngx_nacos_http_v2_write_value(b, (u_char *) "trailers",
                                      sizeof("trailers") - 1, tmp);
    // grpc-accept-encoding: identity
    *b++ = 0;
    b = ngx_nacos_http_v2_write_name(b, (u_char *) "grpc-accept-encoding",
                                     sizeof("grpc-accept-encoding") - 1, tmp);
    b = ngx_nacos_http_v2_write_value(b, (u_char *) "identity",
                                      sizeof("identity") - 1, tmp);
    buf->len = len = b - buf->b;
    ngx_nacos_grpc_encode_frame_header(st, buf->b, HTTP_V2_HEADERS_FRAME,
                                       HTTP_V2_END_HEADERS_FLAG, len - 9);
    return buf;
}

static ngx_int_t ngx_nacos_grpc_decode_ule128(u_char **pp, const u_char *last,
                                              size_t *result) {
    ngx_flag_t resultStartedAtZero;
    size_t shift;
    u_char b, *p = *pp;
    size_t r = *result;
    for (resultStartedAtZero = r == 0, shift = 0; p < last; ++p, shift += 7) {
        b = *p;
        if (shift == 56 &&
            ((b & 0x80) != 0 || (b == 0x7F && !resultStartedAtZero))) {
            return NGX_ERROR;
        }
        if ((b & 0x80) == 0) {
            r += ((b & 0x7FL) << shift);
            if (r > 0x7FFFFFFF) {
                return NGX_ERROR;
            }
            *pp = p + 1;
            *result = r;
            return NGX_OK;
        }
        r += (b & 0x7FL) << shift;
    }
    return NGX_ERROR;
}

static bool ngx_nacos_grpc_pb_encode_str(pb_ostream_t *stream,
                                         const pb_field_t *field,
                                         void *const *arg) {
    ngx_str_t *s = *arg;
    if (!pb_encode_tag_for_field(stream, field)) {
        return false;
    }

    if (s == NULL || s->len == 0) {
        return pb_encode_string(stream, NULL, 0);
    }
    return pb_encode_string(stream, s->data, s->len);
}

static bool ngx_nacos_grpc_encode_user_pass(pb_ostream_t *stream,
                                            const pb_field_t *field,
                                            void *const *arg) {
    ngx_str_t key;
    ngx_str_t value;
    ngx_nacos_grpc_conn_t *conn;
    ngx_nacos_main_conf_t *mcf;
    Metadata_HeadersEntry entry;

    conn = *arg;
    mcf = conn->nmcf;
    entry.key.arg = &key;
    entry.key.funcs.encode = ngx_nacos_grpc_pb_encode_str;
    entry.value.arg = &value;
    entry.value.funcs.encode = ngx_nacos_grpc_pb_encode_str;

    ngx_str_set(&key, "clientIp");
    value = mcf->local_ip;
    if (!pb_encode_tag_for_field(stream, field)) {
        return false;
    }
    if (!pb_encode_submessage(stream, Metadata_HeadersEntry_fields, &entry)) {
        return false;
    }

    ngx_str_set(&key, "clientVersion");
    ngx_str_set(&value, "nacos-nginx-plugin-2.10.0");
    if (!pb_encode_tag_for_field(stream, field)) {
        return false;
    }
    if (!pb_encode_submessage(stream, Metadata_HeadersEntry_fields, &entry)) {
        return false;
    }

    if (mcf->access_token.len) {
        ngx_str_set(&key, "accessToken");
        value = mcf->access_token;
        if (!pb_encode_tag_for_field(stream, field)) {
            return false;
        }
        if (!pb_encode_submessage(stream, Metadata_HeadersEntry_fields,
                                  &entry)) {
            return false;
        }
    }

    ngx_str_set(&key, "namespace");
    value = mcf->service_namespace;
    if (!pb_encode_tag_for_field(stream, field)) {
        return false;
    }
    if (!pb_encode_submessage(stream, Metadata_HeadersEntry_fields, &entry)) {
        return false;
    }
    return true;
}

static ngx_int_t ngx_nacos_grpc_encode_payload_init(
    ngx_nacos_grpc_payload_encode_t *en, ngx_nacos_grpc_stream_t *st) {
    ngx_memzero(&en->payload, sizeof(en->payload));
    en->payload.body.value.arg = &en->json;
    en->payload.body.value.funcs.encode = ngx_nacos_grpc_pb_encode_str;
    en->payload.metadata.type.arg = &en->type;
    en->payload.metadata.type.funcs.encode = ngx_nacos_grpc_pb_encode_str;
    en->payload.has_metadata = 1;
    en->payload.has_body = 1;

    en->payload.metadata.headers.arg = st->conn;
    en->payload.metadata.headers.funcs.encode = ngx_nacos_grpc_encode_user_pass;

    en->payload.metadata.clientIp.arg = &st->conn->nmcf->local_ip;
    en->payload.metadata.clientIp.funcs.encode = ngx_nacos_grpc_pb_encode_str;

    return pb_get_encoded_size(&en->encoded_len, Payload_fields, &en->payload)
               ? NGX_OK
               : NGX_ERROR;
}

static ngx_int_t ngx_nacos_grpc_encode_payload(
    const ngx_nacos_grpc_payload_encode_t *en, ngx_nacos_grpc_stream_t *st) {
    pb_ostream_t buffer;

    if (en->buf != NULL) {
        buffer = pb_ostream_from_buffer(NULL, en->encoded_len);
        buffer.callback = ngx_nacos_grpc_pb_write_buf;
        buffer.state = en->buf;
        if (pb_encode(&buffer, Payload_fields, &en->payload) &&
            buffer.bytes_written == en->encoded_len) {
            return NGX_OK;
        }

        if (buffer.errmsg != NULL) {
            ngx_log_error(NGX_LOG_ERR, st->conn->conn->log, 0,
                          "[nacos] encode payload failed: %s", buffer.errmsg);
        }
    }
    return NGX_ERROR;
}

static bool ngx_nacos_grpc_pb_write_buf(pb_ostream_t *stream,
                                        const pb_byte_t *buf, size_t count) {
    ngx_nacos_grpc_buf_t *cur;
    size_t cnt, s;

    cur = (ngx_nacos_grpc_buf_t *) stream->state;
    while (count > 0) {
        cnt = cur->cap - cur->len;
        if (cnt == 0) {
            cur = cur->next;
            continue;
        }
        s = ngx_min(cnt, count);
        ngx_memcpy(cur->b + cur->len, buf, s);
        cur->len += s;
        buf += s;
        count -= s;
    }
    stream->state = cur;
    return true;
}

static ngx_nacos_grpc_buf_t *ngx_nacos_grpc_encode_data_msg(
    ngx_nacos_grpc_stream_t *st, ngx_nacos_grpc_payload_encode_t *en,
    ngx_flag_t end_stream) {
    size_t b_len, c_len, data_len;
    u_char *b;
    ngx_nacos_grpc_buf_t *head_buf, *last_buf, *buf;
    ngx_uint_t max_frame_size;

    head_buf = NULL;
    last_buf = NULL;
    buf = NULL;
    if (ngx_nacos_grpc_encode_payload_init(en, st) != NGX_OK) {
        ngx_log_error(NGX_LOG_ERR, st->conn->conn->log, 0,
                      "protobuf payload init error");
        goto err;
    }
    data_len = en->encoded_len;
    b_len = data_len + 5;

    max_frame_size = st->conn->settings.max_frame_size;

    while (b_len > 0) {
        c_len = ngx_min(b_len, max_frame_size);
        buf = ngx_nacos_grpc_alloc_buf(st, c_len + 9);
        if (buf == NULL) {
            goto err;
        }

        b = buf->b;
        b_len -= c_len;
        ngx_nacos_grpc_encode_frame_header(
            st, b, HTTP_V2_DATA_FRAME,
            end_stream && b_len == 0 ? HTTP_V2_END_STREAM_FLAG : 0, c_len);
        buf->consume_win = c_len;
        buf->len = 9;
        if (head_buf == NULL) {
            head_buf = buf;
            b = head_buf->b;
            // first data frame
            b[9] = 0;
            b[10] = (data_len >> 24) & 0xFF;
            b[11] = (data_len >> 16) & 0xFF;
            b[12] = (data_len >> 8) & 0xFF;
            b[13] = data_len & 0xFF;
            buf->len += 5;
        } else {
            last_buf->next = buf;
        }
        last_buf = buf;
    }

    if (end_stream) {
        last_buf->sent_request = 1;
    }

    en->buf = head_buf;
    if (ngx_nacos_grpc_encode_payload(en, st) != NGX_OK) {
        ngx_log_error(NGX_LOG_ERR, st->conn->conn->log, 0,
                      "protobuf encode not match length");
        goto err;
    }
    return buf;

err:
    if (head_buf != NULL) {
        buf = head_buf;
        do {
            last_buf = buf->next;
            ngx_nacos_grpc_free_buf(st, buf);
            buf = last_buf;
        } while (buf);
    }
    return NULL;
}

static ngx_int_t ngx_nacos_grpc_realloc_tmp_buf(ngx_nacos_grpc_stream_t *st,
                                                size_t asize) {
    size_t s, last_len;
    ngx_buf_t *buf;

    buf = st->tmp_buf;

    if (buf && asize <= (size_t) (buf->end - buf->last)) {
        return NGX_OK;
    }

    last_len = buf ? (buf->last - buf->pos) : 0;
    asize += last_len;
    s = 512;
    while (s < asize) {
        s <<= 1;
    }
    // s >= asize > last_len;

    st->tmp_buf = ngx_create_temp_buf(st->pool, s);
    if (st->tmp_buf == NULL) {
        return NGX_ERROR;
    }

    if (last_len) {
        ngx_memcpy(st->tmp_buf->last, buf->pos, last_len);
        st->tmp_buf->last += last_len;
    }
    return NGX_OK;
}

static ngx_int_t ngx_nacos_grpc_send_win_update_frame(
    ngx_nacos_grpc_stream_t *st, size_t win_update) {
    ngx_nacos_grpc_buf_t *buf;
    u_char *p;
    ngx_int_t rc;

    buf = ngx_nacos_grpc_alloc_buf(st, 9 + 4);
    if (buf == NULL) {
        return NGX_ERROR;
    }
    ngx_nacos_grpc_encode_frame_header(st, buf->b, HTTP_V2_WINDOW_UPDATE_FRAME,
                                       0, 4);
    buf->len = 9 + 4;
    p = buf->b + 9;
    p[0] = (win_update >> 24) & 0x7F;
    p[1] = (win_update >> 16) & 0xFF;
    p[2] = (win_update >> 8) & 0xFF;
    p[3] = win_update & 0xFF;
    rc = ngx_nacos_grpc_send_buf(buf, 0);
    if (rc == NGX_OK) {
        st->recv_win += win_update;
    }
    return rc;
}

static ngx_int_t ngx_nacos_send_ping_frame(ngx_nacos_grpc_conn_t *gc) {
    ngx_nacos_grpc_buf_t *buf;
    u_char *p;
    ngx_uint_t data;
    buf = ngx_nacos_grpc_alloc_buf(gc->m_stream, 9 + 8);
    if (buf == NULL) {
        return NGX_ERROR;
    }
    ngx_nacos_grpc_encode_frame_header(gc->m_stream, buf->b, HTTP_V2_PING_FRAME,
                                       0, 8);
    p = buf->b + 9;
    data = ++gc->heartbeat;
    p[0] = (data >> 56) & 0xFF;
    p[1] = (data >> 48) & 0xFF;
    p[2] = (data >> 40) & 0xFF;
    p[3] = (data >> 32) & 0xFF;
    p[4] = (data >> 24) & 0xFF;
    p[5] = (data >> 16) & 0xFF;
    p[6] = (data >> 8) & 0xFF;
    p[7] = data & 0xFF;
    buf->len = 9 + 8;
    return ngx_nacos_grpc_send_buf(buf, 0);
}

static bool ngx_nacos_grpc_pb_decode_str(pb_istream_t *stream,
                                         const pb_field_t *field, void **arg) {
    ngx_str_t *t = *arg;
    t->len = stream->bytes_left;
    t->data = stream->state;
    stream->state = t->data + t->len;
    stream->bytes_left -= t->len;
    return true;
}

static ngx_int_t ngx_nacos_grpc_decode_payload(
    ngx_nacos_grpc_stream_t *st, ngx_nacos_grpc_payload_decode_t *de) {
    size_t i, len;
    pb_istream_t buffer;

    ngx_memzero(&de->result, sizeof(de->result));
    ngx_memzero(&de->payload, sizeof(de->payload));
    de->result.body.value.funcs.decode = ngx_nacos_grpc_pb_decode_str;
    de->result.body.value.arg = &de->out_json;
    de->result.metadata.type.funcs.decode = ngx_nacos_grpc_pb_decode_str;
    de->result.metadata.type.arg = &de->type;

    de->payload.msg_state = pl_error;

    buffer = pb_istream_from_buffer(de->input.data, de->input.len);
    if (!pb_decode(&buffer, Payload_fields, &de->result)) {
        ngx_log_error_core(NGX_ERROR_ERR, st->conn->conn->log, 0,
                           "decode protobuf error:%s", buffer.errmsg);
        de->payload.msg_state = pl_fail;
        return NGX_ERROR;
    }

    for (i = 0,
        len = sizeof(payload_type_mapping) / sizeof(payload_type_mapping[0]);
         i < len; ++i) {
        if (de->type.len == payload_type_mapping[i].type_name.len &&
            ngx_strncmp(de->type.data, payload_type_mapping[i].type_name.data,
                        payload_type_mapping[i].type_name.len) == 0) {
            de->payload.type = payload_type_mapping[i].payload_type;
            de->payload.msg_state = pl_success;
            break;
        }
    }

    de->payload.json_str = de->out_json;

    return NGX_OK;
}

