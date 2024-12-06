//
// Created by dear on 2024/12/6.
//

#ifndef NGINX_NACOS_HTTP_V2_H
#define NGINX_NACOS_HTTP_V2_H

#include <ngx_config.h>
#include <ngx_core.h>


#define HTTP_V2_STATE_BUFFER_SIZE    16

#define HTTP_V2_DEFAULT_FRAME_SIZE   (1 << 14)
#define HTTP_V2_MAX_FRAME_SIZE       ((1 << 24) - 1)

#define HTTP_V2_INT_OCTETS           4
#define HTTP_V2_MAX_FIELD                                                 \
    (127 + (1 << (HTTP_V2_INT_OCTETS - 1) * 7) - 1)

#define HTTP_V2_STREAM_ID_SIZE       4

#define HTTP_V2_FRAME_HEADER_SIZE    9

/* frame types */
#define HTTP_V2_DATA_FRAME           0x0
#define HTTP_V2_HEADERS_FRAME        0x1
#define HTTP_V2_PRIORITY_FRAME       0x2
#define HTTP_V2_RST_STREAM_FRAME     0x3
#define HTTP_V2_SETTINGS_FRAME       0x4
#define HTTP_V2_PUSH_PROMISE_FRAME   0x5
#define HTTP_V2_PING_FRAME           0x6
#define HTTP_V2_GOAWAY_FRAME         0x7
#define HTTP_V2_WINDOW_UPDATE_FRAME  0x8
#define HTTP_V2_CONTINUATION_FRAME   0x9

/* frame flags */
#define HTTP_V2_NO_FLAG              0x00
#define HTTP_V2_ACK_FLAG             0x01
#define HTTP_V2_END_STREAM_FLAG      0x01
#define HTTP_V2_END_HEADERS_FLAG     0x04
#define HTTP_V2_PADDED_FLAG          0x08
#define HTTP_V2_PRIORITY_FLAG        0x20

#define HTTP_V2_MAX_WINDOW           ((1U << 31) - 1)
#define HTTP_V2_DEFAULT_WINDOW       65535

#define HTTP_V2_DEFAULT_WEIGHT       16


typedef struct {
    ngx_str_t name;
    ngx_str_t value;
} ngx_nacos_http_v2_filed_t;

ngx_str_t *ngx_nacos_http_v2_get_static_name(ngx_uint_t index);
ngx_str_t *ngx_nacos_http_v2_get_static_value(ngx_uint_t index);

#define ngx_nacos_http_v2_indexed(i)      (128 + (i))
#define ngx_nacos_http_v2_inc_indexed(i)  (64 + (i))


#define HTTP_V2_AUTHORITY_INDEX       1

#define HTTP_V2_METHOD_INDEX          2
#define HTTP_V2_METHOD_GET_INDEX      2
#define HTTP_V2_METHOD_POST_INDEX     3

#define HTTP_V2_PATH_INDEX            4
#define HTTP_V2_PATH_ROOT_INDEX       4

#define HTTP_V2_SCHEME_HTTP_INDEX     6
#define HTTP_V2_SCHEME_HTTPS_INDEX    7

#define HTTP_V2_STATUS_INDEX          8
#define HTTP_V2_STATUS_200_INDEX      8
#define HTTP_V2_STATUS_204_INDEX      9
#define HTTP_V2_STATUS_206_INDEX      10
#define HTTP_V2_STATUS_304_INDEX      11
#define HTTP_V2_STATUS_400_INDEX      12
#define HTTP_V2_STATUS_404_INDEX      13
#define HTTP_V2_STATUS_500_INDEX      14

#define HTTP_V2_ACCEPT_ENCODING_INDEX 16
#define HTTP_V2_ACCEPT_LANGUAGE_INDEX 17
#define HTTP_V2_CONTENT_LENGTH_INDEX  28
#define HTTP_V2_CONTENT_TYPE_INDEX    31
#define HTTP_V2_DATE_INDEX            33
#define HTTP_V2_LAST_MODIFIED_INDEX   44
#define HTTP_V2_LOCATION_INDEX        46
#define HTTP_V2_SERVER_INDEX          54
#define HTTP_V2_USER_AGENT_INDEX      58
#define HTTP_V2_VARY_INDEX            59
#define HTTP_V2_ENCODE_RAW            0
#define HTTP_V2_ENCODE_HUFF           0x80


#define ngx_nacos_http_v2_write_name(dst, src, len, tmp)                            \
    ngx_nacos_http_v2_string_encode(dst, src, len, tmp, 1)
#define ngx_nacos_http_v2_write_value(dst, src, len, tmp)                           \
    ngx_nacos_http_v2_string_encode(dst, src, len, tmp, 0)

u_char *ngx_nacos_http_v2_string_encode(u_char *dst, u_char *src, size_t len,
                                  u_char *tmp, ngx_uint_t lower);

ngx_int_t ngx_nacos_http_v2_huff_decode(u_char *state, u_char *src, size_t len,
                                  u_char **dst, ngx_uint_t last, ngx_log_t *log);
size_t ngx_nacos_http_v2_huff_encode(u_char *src, size_t len, u_char *dst,
                               ngx_uint_t lower);
#endif  // NGINX_NACOS_HTTP_V2_H
