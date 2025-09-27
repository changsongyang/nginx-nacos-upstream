//
// Created by pc on 9/23/25.
//

#include <ngx_nacos_payload.h>

ngx_int_t ngx_nacos_check_response(ngx_log_t *log, yajl_val val) {
    yajl_val code;
    if (val == NULL || !YAJL_IS_OBJECT(val)) {
        ngx_log_error(NGX_LOG_ERR, log, 0,
                      "[NACOS] response is not a json object");
        return NGX_ERROR;
    }
    code = yajl_tree_get_field(val, "resultCode", yajl_t_number);
    if (code == NULL || !YAJL_IS_INTEGER(code)) {
        ngx_log_error(NGX_LOG_ERR, log, 0, "[NACOS] code is not number");
        return NGX_ERROR;
    }
    if (YAJL_GET_INTEGER(code) != 200) {
        ngx_log_error(NGX_LOG_ERR, log, 0, "[NACOS] code is not 200");
        return NGX_ERROR;
    }
    return NGX_OK;
}
