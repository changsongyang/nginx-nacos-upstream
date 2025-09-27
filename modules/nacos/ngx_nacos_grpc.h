#ifndef NGINX_NACOS_NGX_NACOS_GRPC_H
#define NGINX_NACOS_NGX_NACOS_GRPC_H

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_event_connect.h>
#include <ngx_nacos.h>
#include <ngx_nacos_payload.h>

typedef struct ngx_nacos_grpc_stream_s ngx_nacos_grpc_stream_t;
typedef struct ngx_nacos_grpc_conn_s ngx_nacos_grpc_conn_t;
typedef struct ngx_nacos_grpc_buf_s ngx_nacos_grpc_buf_t;
typedef struct ngx_nacos_grpc_ctx_s ngx_nacos_grpc_ctx_t;

typedef ngx_int_t (*grpc_buf_callback)(ngx_nacos_grpc_stream_t *s,
                                       ngx_int_t state);

struct ngx_nacos_grpc_buf_s {
    ngx_nacos_grpc_stream_t *stream;
    ngx_nacos_grpc_buf_t *next;
    u_char *b;
    size_t cap;
    size_t len;
    size_t consume_win;
    unsigned sent_header : 1;
    unsigned sent_request : 1;
};
typedef struct {
    enum ngx_nacos_payload_type type;
    ngx_str_t json_str;
    ngx_flag_t end;
    enum { pl_success = 1, pl_fail, pl_error } msg_state;
} ngx_nacos_payload_t;

typedef ngx_int_t (*stream_handler)(ngx_nacos_grpc_stream_t *st,
                                    ngx_nacos_payload_t *p);
enum ngx_nacos_grpc_event_state { nc_connected, nc_error };

typedef ngx_int_t (*nc_grpc_event_handler)(
    ngx_nacos_grpc_conn_t *conn, enum ngx_nacos_grpc_event_state state);

typedef struct {
    ngx_nacos_grpc_buf_t *head;
    ngx_nacos_grpc_buf_t *tail;
} ngx_nacos_grpc_bufs_t;

struct ngx_nacos_grpc_conn_s {
    ngx_nacos_main_conf_t *nmcf;
    nc_grpc_event_handler handler;
    ngx_connection_t *conn;
    ngx_pool_t *pool;
    uint32_t next_stream_id;
    enum { init, connecting, prepare_grpc, working } stat;
    ngx_peer_connection_t peer;
    ngx_nacos_grpc_stream_t *m_stream;
    ngx_rbtree_t st_tree;
    ngx_rbtree_node_t st_sentinel;
    ngx_queue_t all_streams;
    ngx_nacos_grpc_bufs_t io_sending_bufs;
    struct {
        ngx_uint_t header_table_size;
        ngx_uint_t max_conn_streams;
        ngx_uint_t init_window_size;
        ngx_uint_t max_frame_size;
        ngx_uint_t max_header_list_size;
    } settings;
    ngx_buf_t *read_buf;
    ngx_uint_t heartbeat;
    enum { parse_frame_header = 0, parse_frame_payload } parse_stat;
    size_t frame_size;
    ngx_uint_t frame_stream_id;
    u_char frame_type;
    u_char frame_flags;
    unsigned frame_start : 1;
    unsigned frame_end : 1;
};

struct ngx_nacos_grpc_stream_s {
    ngx_rbtree_node_t node;
    ngx_queue_t queue;
    ngx_pool_t *pool;
    ngx_nacos_grpc_conn_t *conn;
    size_t send_win;
    size_t recv_win;
    ngx_uint_t stream_id;
    stream_handler stream_handler;
    void *handler_ctx;
    ngx_nacos_grpc_bufs_t sending_bufs;
    ngx_buf_t *tmp_buf;
    ngx_uint_t resp_status;
    ngx_uint_t grpc_status;
    size_t proto_len;
    u_char padding;
    u_char parsing_state;
    size_t buf_size;
    unsigned header_sent : 1;
    unsigned req_sent : 1;
    unsigned end_stream : 1;
    unsigned end_header : 1;
    unsigned resp_grpc_encode : 1;
    unsigned store_proto_size_buf : 1;
};

ngx_nacos_grpc_conn_t *ngx_nacos_open_grpc_conn(ngx_nacos_main_conf_t *conf,
                                                nc_grpc_event_handler handler);

ngx_nacos_grpc_stream_t *ngx_nacos_grpc_request(ngx_nacos_grpc_conn_t *conn,
                                                stream_handler handler);

ngx_nacos_grpc_stream_t *ngx_nacos_grpc_bi_request(ngx_nacos_grpc_conn_t *conn,
                                                   stream_handler handler);
ngx_int_t ngx_nacos_grpc_send(ngx_nacos_grpc_stream_t *st,
                              ngx_nacos_payload_t *p);

void ngx_nacos_grpc_close_stream(ngx_nacos_grpc_stream_t *st, ngx_flag_t force);

void ngx_nacos_grpc_close_connection(ngx_nacos_grpc_conn_t *gc,
                                     ngx_int_t status);

#endif
