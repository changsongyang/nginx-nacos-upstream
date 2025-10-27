//
// Created by dear on 22-6-1.
//

#include <ngx_auxiliary_module.h>
#include <ngx_event_connect.h>
#include <ngx_nacos.h>
#include <ngx_nacos_aux.h>
#include <ngx_nacos_grpc.h>
#include <ngx_nacos_udp.h>
#include <ngx_nacos_http_parse.h>
#include <ngx_nacos_data.h>

static ngx_int_t ngx_aux_nacos_proc_handler(ngx_cycle_t *cycle,
                                            ngx_aux_proc_t *p);

static void ngx_nacos_aux_refresh_token_handler(ngx_event_t *ev);

static void ngx_nacos_aux_login_http_write_handler(ngx_event_t *ev);

static void ngx_nacos_aux_login_http_read_handler(ngx_event_t *ev);

static u_char access_token_buf[2048];

static struct {
    ngx_nacos_main_conf_t *ncf;
    ngx_nacos_udp_conn_t *uc;
    ngx_nacos_grpc_ctx_t *gc;
    ngx_uint_t err_times;
    ngx_uint_t token_expire_time;
    ngx_str_t login_body;
    ngx_event_t token_refresh_timer;
    ngx_str_t login_header;
} aux_ctx;

typedef struct {
    ngx_peer_connection_t peer;
    enum { sl_connecting, sl_sending, sl_receiving } state;
    ngx_connection_t *conn;
    ngx_chain_t *send_chain;
    ngx_nacos_http_parse_t parser;
} ngx_nacos_aux_login_conn_t;

static void ngx_nacos_aux_close_login_http_connection(
    ngx_nacos_aux_login_conn_t *lc);

static ngx_aux_proc_t aux_proc = {ngx_string("nacos"), NULL,
                                  ngx_aux_nacos_proc_handler};

#define LOGIN_REQ                                         \
    "POST /nacos/v1/auth/users/login HTTP/1.0\r\n"        \
    "Host: %V\r\n"                                        \
    "User-Agent: nacos-nginx-plugin-v2.10.0\r\n"          \
    "Content-Type: application/x-www-form-urlencoded\r\n" \
    "Content-Length: %uz\r\n"                             \
    "Connection: close\r\n"                               \
    "\r\n"

ngx_int_t ngx_nacos_aux_init(ngx_conf_t *cf) {
    ngx_nacos_main_conf_t *ncf;
    ngx_nacos_login_info_t li;
    size_t header_len, body_len;
    u_char *buf;
    ngx_int_t rv;

    ncf = ngx_nacos_get_main_conf(cf);

    if (ncf->username.len > 0 && ncf->password.len > 0) {
        body_len =
            ncf->username.len +
            2 * ngx_escape_uri(NULL, ncf->username.data, ncf->username.len,
                               NGX_ESCAPE_URI_COMPONENT) +
            ncf->password.len +
            2 * ngx_escape_uri(NULL, ncf->password.data, ncf->password.len,
                               NGX_ESCAPE_URI_COMPONENT) +
            sizeof("username=") + sizeof("&password=");
        header_len = sizeof(LOGIN_REQ) + ncf->server_host.len + 10;

        aux_ctx.login_header.data = ngx_palloc(cf->pool, header_len + body_len);
        if (aux_ctx.login_header.data == NULL) {
            rv = NGX_ERROR;
            goto end;
        }

        buf = aux_ctx.login_header.data;
        buf = ngx_sprintf(buf, LOGIN_REQ, &ncf->server_host, body_len - 2);
        aux_ctx.login_header.len = buf - aux_ctx.login_header.data;
        aux_ctx.login_body.data = buf;

        ngx_memcpy(buf, "username=", sizeof("username=") - 1);
        buf += sizeof("username=") - 1;
        buf = (u_char *) ngx_escape_uri(buf, ncf->username.data,
                                        ncf->username.len,
                                        NGX_ESCAPE_URI_COMPONENT);
        ngx_memcpy(buf, "&password=", sizeof("&password=") - 1);
        buf += sizeof("&password=") - 1;
        buf = (u_char *) ngx_escape_uri(buf, ncf->password.data,
                                        ncf->password.len,
                                        NGX_ESCAPE_URI_COMPONENT);
        *buf = '\0';
        aux_ctx.login_body.len = buf - aux_ctx.login_body.data;

        ngx_memzero(&li, sizeof(li));
        li.login_body = aux_ctx.login_body;
        li.token.data = access_token_buf;
        li.token.len = 0;
        li.token_buf_size = sizeof(access_token_buf);
        li.username = ncf->username;
        li.password = ncf->password;

        if (ngx_nacos_fetch_access_token(ncf, &li) != NGX_OK) {
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                               "nacos fetch access_token error");
            rv = NGX_ERROR;
            goto end;
        }
        ncf->access_token = li.token;
        aux_ctx.token_expire_time = li.token_expire_time;
    }

    aux_ctx.ncf = ncf;
    aux_proc.data = &aux_ctx;
    rv = ngx_aux_add_proc(cf, &aux_proc);
end:
    return rv;
}

static ngx_int_t ngx_aux_nacos_proc_handler(ngx_cycle_t *cycle,
                                            ngx_aux_proc_t *p) {
    ngx_nacos_udp_conn_t *uc;
    ngx_int_t rc;
    ngx_nacos_runner_t *runner;
    ngx_uint_t i, len;
    ngx_nacos_main_conf_t *ncf = aux_ctx.ncf;

#ifndef NGX_AUX_PATCHED
    if (ngx_process != NGX_PROCESS_SINGLE) {
        ngx_log_error(NGX_LOG_WARN, ncf->error_log, 0,
                      "nacos process running is worker %d", ngx_worker);
    }
#endif
    aux_ctx.token_refresh_timer.handler = ngx_nacos_aux_refresh_token_handler;
    if (ncf->access_token.len) {
        ngx_add_timer(&aux_ctx.token_refresh_timer, aux_ctx.token_expire_time);
    }

    if (ncf->udp_port.len && ncf->udp_port.data) {
        uc = ngx_nacos_open_udp(ncf);
        if (uc == NULL) {
            return NGX_ERROR;
        }

        aux_ctx.uc = uc;
    } else {
        rc = ngx_nacos_naming_init(ncf);
        if (rc != NGX_OK) {
            return NGX_ERROR;
        }
        rc = ngx_nacos_config_init(ncf);
        if (rc != NGX_OK) {
            return NGX_ERROR;
        }
    }

    runner = ncf->runners.elts;
    len = ncf->runners.nelts;
    for (i = 0; i < len; i++) {
        runner[i].handler(runner[i].data);
    }

    return NGX_OK;
}

static void ngx_nacos_aux_login_http_write_handler(ngx_event_t *ev) {
    ngx_connection_t *c;
    ngx_nacos_aux_login_conn_t *lc;
    int err;
    socklen_t len;
    ngx_pool_t *pool;
    ngx_buf_t *buf;
    ngx_chain_t *cl, *chain, *ln;

    c = ev->data;
    lc = c->data;
    pool = c->pool;

    if (lc->state >= sl_receiving) {
        // write
        return;
    }

    if (lc->state == sl_connecting) {
        if (ev->timedout) {
            ngx_log_error(NGX_LOG_WARN, c->log, 0,
                          "nacos login http connection connect tcp timed out");
            goto err;
        }

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
            c->log->action = "connecting to nacos http login connection";
            (void) ngx_connection_error(c, err, "connect() failed");
            goto err;
        }

        ngx_log_debug0(NGX_LOG_DEBUG_CORE, c->log, 0,
                       "http connection connect successfully");
        lc->state = sl_sending;
    }

    if (lc->state == sl_sending) {
        if (lc->send_chain == NULL) {
            cl = ngx_alloc_chain_link(pool);
            if (cl == NULL) {
                goto err;
            }
            buf = ngx_calloc_buf(pool);
            if (buf == NULL) {
                goto err;
            }

            buf->pos = buf->start = aux_ctx.login_header.data;
            buf->last = buf->end = buf->pos + aux_ctx.login_header.len;
            buf->temporary = 1;
            buf->memory = 1;
            cl->buf = buf;
            lc->send_chain = cl;

            cl = ngx_alloc_chain_link(pool);
            if (cl == NULL) {
                goto err;
            }
            buf = ngx_calloc_buf(pool);
            if (buf == NULL) {
                goto err;
            }
            buf->pos = buf->start = aux_ctx.login_body.data;
            buf->last = buf->end = buf->pos + aux_ctx.login_body.len;
            buf->temporary = 1;
            buf->last_buf = 1;
            cl->buf = buf;
            lc->send_chain->next = cl;
            cl->next = NULL;
        }

        chain = c->send_chain(c, lc->send_chain, 0);
        if (chain == NGX_CHAIN_ERROR) {
            goto err;
        }

        for (cl = lc->send_chain; cl && cl != chain; /* void */) {
            ln = cl;
            cl = cl->next;
            ngx_free_chain(pool, ln);
        }
        lc->send_chain = chain;
        if (chain == NULL) {
            lc->state = sl_receiving;
            ngx_add_timer(c->read, 5000);
        }
    }

    return;

err:
    c->error = 1;
    ngx_nacos_aux_close_login_http_connection(lc);
    ngx_add_timer(&aux_ctx.token_refresh_timer, 5000);
}

static void ngx_nacos_aux_login_http_read_handler(ngx_event_t *ev) {
    ssize_t rd;
    ngx_int_t rc;
    yajl_val json;
    ngx_connection_t *c;
    ngx_nacos_aux_login_conn_t *lc;
    ngx_pool_t *pool;
    ngx_nacos_login_info_t li;
    ngx_uint_t recon_time;

    c = ev->data;
    lc = c->data;
    pool = c->pool;
    json = NULL;
    recon_time = 5000;

    if (lc->state != sl_receiving) {
        return;
    }

    if (ev->timedout) {
        ngx_log_error(NGX_LOG_WARN, c->log, 0,
                      "nacos login http connection read tcp timed out");
        goto err;
    }

    if (ev->timer_set) {
        ngx_del_timer(ev);
    }
    c->log->action = "nacos parsing login http response";
    if (lc->parser.buf == NULL) {
        lc->parser.buf = ngx_palloc(pool, 64 * 1024);
        if (lc->parser.buf == NULL) {
            goto err;
        }
    }

    for (;;) {
        c->log->action = "nacos reading response";
        rd = c->recv(c, lc->parser.buf + lc->parser.limit,
                     64 * 1024 - lc->parser.limit);
        if (rd >= 0) {
            lc->parser.limit += rd;
            lc->parser.conn_eof = ev->eof;
            c->log->action = "nacos parsing response";
            rc = ngx_nacos_http_parse(&lc->parser);
            if (rc == NGX_ERROR) {
                ngx_log_debug0(NGX_LOG_DEBUG_CORE, c->log, 0,
                               "http connection protocol error");
                goto err;
            }

            if (rc == NGX_OK) {
                goto request_complete;
            }

            if (ev->eof) {
                // close premature
                ngx_log_debug0(NGX_LOG_DEBUG_CORE, c->log, 0,
                               "http connection close abruptly");
                goto err;
            }
        } else if (rd == NGX_AGAIN) {
            return;
        } else {
            ngx_log_debug0(NGX_LOG_DEBUG_CORE, c->log, 0,
                           "http connection encounter socket error");
            // socket error
            goto err;
        }
    }

request_complete:
    ngx_log_debug1(NGX_LOG_DEBUG_CORE, c->log, 0,
                   "http connection read response successfully,status=%d",
                   nc->parser.status);
    c->requests++;

    if (lc->parser.json_parser) {
        json = yajl_tree_finish_get(lc->parser.json_parser);
        if (json == NULL) {
            goto err;
        }
        ngx_log_error(NGX_LOG_INFO, c->log, 0,
                      "http connection received json response");

        li.token_buf_size = sizeof(access_token_buf);
        li.token = aux_ctx.ncf->access_token;
        if (ngx_nacos_get_access_token_from_login_json(json, &li) != NGX_OK) {
            ngx_log_error(NGX_LOG_ERR, c->log, 0,
                          "nacos login http response json error");
            goto err;
        }
        aux_ctx.token_expire_time = li.token_expire_time;
        aux_ctx.ncf->access_token = li.token;
        recon_time = li.token_expire_time;
        ngx_log_error(NGX_LOG_INFO, c->log, 0,
                      "nacos login http response get token: %V", &li.token);
    }

err:
    if (lc->parser.json_parser) {
        yajl_tree_free_parser(lc->parser.json_parser);
        lc->parser.json_parser = NULL;
    }
    ngx_nacos_aux_close_login_http_connection(lc);
    ngx_add_timer(&aux_ctx.token_refresh_timer, recon_time);
}

static void ngx_nacos_aux_close_login_http_connection(
    ngx_nacos_aux_login_conn_t *lc) {
    ngx_pool_t *pool;
    ngx_uint_t err;
    pool = lc->conn->pool;
    err = lc->conn->error;
    ngx_close_connection(lc->conn);
    lc->peer.free(&lc->peer, lc->peer.data, err ? NGX_NC_ERROR : NGX_NC_TIRED);
    ngx_destroy_pool(pool);
}

static void ngx_nacos_aux_refresh_token_handler(ngx_event_t *ev) {
    ngx_pool_t *pool;
    ngx_connection_t *c;
    ngx_nacos_aux_login_conn_t *lc;
    ngx_int_t rc;
    ngx_uint_t try;
    ngx_peer_connection_t *peer;

    try = 0;

    pool = ngx_create_pool(aux_ctx.ncf->udp_pool_size, aux_ctx.ncf->error_log);
    if (pool == NULL) {
        return;
    }
    lc = ngx_pcalloc(pool, sizeof(*lc));
    peer = &lc->peer;

    peer->start_time = ngx_current_msec;
    peer->log_error = NGX_ERROR_INFO;
    peer->log = aux_ctx.ncf->error_log;
    peer->get = ngx_nacos_aux_get_addr;
    peer->free = ngx_nacos_aux_free_addr;
    peer->data = &aux_ctx.ncf->server_list;

connect:
    rc = ngx_event_connect_peer(peer);
    try++;
    if (rc == NGX_ERROR) {
        if (peer->name) {
            ngx_log_error(NGX_LOG_WARN, peer->log, 0,
                          "http connection connect to %V error", peer->name);
        }
        if (peer->sockaddr) {
            peer->free(peer, peer->data, NGX_ERROR);
        }
        if (try < aux_ctx.ncf->server_list.nelts) {
            goto connect;
        }
        goto connect_failed;
    }

    c = peer->connection;
    c->data = lc;
    c->pool = pool;
    c->write->handler = ngx_nacos_aux_login_http_write_handler;
    c->read->handler = ngx_nacos_aux_login_http_read_handler;
    c->requests = 0;

    lc->conn = c;

    if (rc == NGX_AGAIN) {  // connecting
        lc->state = sl_connecting;
        c->log->action = "nacos login http connection connecting";
        ngx_add_timer(c->write, 3000);  // set connect time out
        return;
    }

    lc->state = sl_sending;
    // rc == NGX_OK
    ngx_nacos_aux_login_http_write_handler(c->write);
    if (rc == NGX_OK) {
        return;
    }

connect_failed:
    ngx_log_error(NGX_LOG_WARN, peer->log, 0,
                  "create http connection error after try %d", try);
    ngx_destroy_pool(pool);
    ngx_add_timer(&aux_ctx.token_refresh_timer, 5000);
}

void ngx_nacos_aux_free_addr(ngx_peer_connection_t *pc, void *data,
                             ngx_uint_t state) {
    ngx_uint_t i, n;
    ngx_array_t *adr_list;

    adr_list = data;
    i = aux_ctx.ncf->cur_srv_index;
    n = (ngx_uint_t) adr_list->nelts;

    if (state == NGX_NC_TIRED) {
        aux_ctx.err_times = 0;
        ngx_log_error(NGX_LOG_INFO, pc->log, 0,
                      "closing connection from nacos:%V", pc->name);
        return;
    }
    aux_ctx.err_times++;
    aux_ctx.ncf->cur_srv_index = ++i % n;
    ngx_log_error(NGX_LOG_ERR, aux_ctx.ncf->error_log, 0,
                  "use next addr because of error");
}

ngx_int_t ngx_nacos_aux_get_addr(ngx_peer_connection_t *pc, void *data) {
    ngx_addr_t *t;
    ngx_array_t *adr_list;
    ngx_uint_t i, n;

    adr_list = data;
    t = adr_list->elts;
    i = aux_ctx.ncf->cur_srv_index;
    n = adr_list->nelts;
    if (i >= n) {
        i %= n;
    }

    pc->name = &t[i].name;
    pc->sockaddr = t[i].sockaddr;
    pc->socklen = t[i].socklen;
    ngx_log_error_core(NGX_LOG_INFO, pc->log, 0,
                       "connecting to nacos server: %V", pc->name);
    return NGX_OK;
}