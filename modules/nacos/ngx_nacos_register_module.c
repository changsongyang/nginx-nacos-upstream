//
// Created by pc on 9/28/25.
//
#include <ngx_event.h>
#include <ngx_nacos.h>
#include <ngx_nacos_data.h>

static char *ngx_nacos_register(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);

static char *ngx_nacos_metadata(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);

static char *ngx_nacos_init_register_conf(ngx_conf_t *cf, void *conf);

static ngx_command_t cmds[] = {
    {ngx_string("register"),
     NGX_NACOS_MAIN_CONF | NGX_CONF_BLOCK | NGX_CONF_TAKE1, ngx_nacos_register,
     0, 0, NULL},
    {ngx_string("port"), NGX_NACOS_REGISTER_CONF | NGX_CONF_TAKE1,
     ngx_conf_set_num_slot, 0, offsetof(ngx_nacos_register_t, port), NULL},
    {ngx_string("group"), NGX_NACOS_REGISTER_CONF | NGX_CONF_TAKE1,
     ngx_conf_set_str_slot, 0, offsetof(ngx_nacos_register_t, group), NULL},
    {ngx_string("ip"), NGX_NACOS_REGISTER_CONF | NGX_CONF_TAKE1,
     ngx_conf_set_str_slot, 0, offsetof(ngx_nacos_register_t, ip), NULL},
    {ngx_string("cluster"), NGX_NACOS_REGISTER_CONF | NGX_CONF_TAKE1,
     ngx_conf_set_str_slot, 0, offsetof(ngx_nacos_register_t, cluster), NULL},
    {ngx_string("weight"), NGX_NACOS_REGISTER_CONF | NGX_CONF_TAKE1,
     ngx_conf_set_num_slot, 0, offsetof(ngx_nacos_register_t, weight), NULL},
    {ngx_string("healthy"), NGX_NACOS_REGISTER_CONF | NGX_CONF_FLAG,
     ngx_conf_set_flag_slot, 0, offsetof(ngx_nacos_register_t, healthy), NULL},
    {ngx_string("metadata"), NGX_NACOS_REGISTER_CONF | NGX_CONF_TAKE2,
     ngx_nacos_metadata, 0, offsetof(ngx_nacos_register_t, metadata), NULL},
    ngx_null_command};

static ngx_nacos_module_t register_module_ctx = {NULL,
                                                 ngx_nacos_init_register_conf};

ngx_module_t ngx_nacos_register_module = {NGX_MODULE_V1,
                                          &register_module_ctx,
                                          cmds,
                                          NGX_NACOS_MODULE,
                                          NULL, /* init master */
                                          NULL, /* init module */
                                          NULL, /* init process */
                                          NULL, /* init thread */
                                          NULL, /* exit thread */
                                          NULL, /* exit process */
                                          NULL, /* exit master */
                                          NGX_MODULE_V1_PADDING};

static char *ngx_nacos_register(ngx_conf_t *cf, ngx_command_t *cmd,
                                void *conf) {
    ngx_nacos_main_conf_t *nmcf;
    ngx_nacos_register_t *nr, **nrp;
    ngx_str_t *value;
    ngx_conf_t pcf;
    void ***ctx;
    char *rv;

    value = cf->args->elts;
    nmcf = conf;

    if (nmcf == NULL) {
        return "require nacos block";
    }
    if (nmcf->register_services.size == 0) {
        if (ngx_array_init(&nmcf->register_services, cf->pool, 4,
                           sizeof(ngx_nacos_register_t *)) != NGX_OK) {
            return NGX_CONF_ERROR;
        }
    }
    nrp = ngx_array_push(&nmcf->register_services);
    if (nrp == NULL) {
        return NGX_CONF_ERROR;
    }

    nr = ngx_pcalloc(cf->pool, sizeof(ngx_nacos_register_t));
    if (nr == NULL) {
        return NGX_CONF_ERROR;
    }
    if (ngx_array_init(&nr->metadata, cf->pool, 4, sizeof(ngx_str_t)) !=
        NGX_OK) {
        return NGX_CONF_ERROR;
    }

    nr->healthy = NGX_CONF_UNSET_SIZE;
    nr->port = NGX_CONF_UNSET_SIZE;
    nr->weight = NGX_CONF_UNSET_SIZE;

    nr->data_id = value[1];

    pcf = *cf;

    cf->cmd_type = NGX_NACOS_REGISTER_CONF;
    ctx = cf->ctx;
    (*ctx)[ngx_nacos_register_module.ctx_index] = nr;
    rv = ngx_conf_parse(cf, NULL);
    (*ctx)[ngx_nacos_register_module.ctx_index] = conf;
    *cf = pcf;

    *nrp = nr;

    return rv;
}

static char *ngx_nacos_metadata(ngx_conf_t *cf, ngx_command_t *cmd,
                                void *conf) {
    ngx_nacos_register_t *nr;
    ngx_str_t *kv, *value, key, val;
    ngx_uint_t i, len;
    nr = conf;
    value = cf->args->elts;
    key = value[1];
    val = value[2];

    if (key.len ==0 || val.len == 0) {
        return "invalid metadata. both key and value are required";
    }

    value = nr->metadata.elts;
    for (i = 0, len = nr->metadata.nelts; i < len; i += 2) {
        if (key.len == value[i].len &&
            ngx_strncmp(key.data, value[i].data, key.len) == 0) {
            return "duplicate metadata key";
        }
    }

    kv = ngx_array_push_n(&nr->metadata, 2);
    if (kv == NULL) {
        return NGX_CONF_ERROR;
    }
    kv[0] = key;
    kv[1] = val;
    return NGX_CONF_OK;
}

static char *ngx_nacos_init_register_conf(ngx_conf_t *cf, void *conf) {
    ngx_nacos_main_conf_t *nmcf;
    ngx_nacos_register_t **nrp;
    ngx_uint_t i, n;

    nmcf = conf;
    nrp = nmcf->register_services.elts;
    for (i = 0, n = nmcf->register_services.nelts; i < n; i++) {
        ngx_conf_init_value(nrp[i]->healthy, 1);
        ngx_conf_init_uint_value(nrp[i]->weight, 100);

        if (nrp[i]->port >= 65535) {
            return "invalid register port";
        }
        if (nrp[i]->weight > 10000) {
            return "invalid register weight";
        }
        if (nrp[i]->data_id.len == 0) {
            return "invalid register service_name";
        }
        if (nrp[i]->group.len == 0) {
            nrp[i]->group = nmcf->default_group;
        }
        if (nrp[i]->cluster.len == 0) {
            ngx_str_set(&nrp[i]->cluster, "DEFAULT");
        }
        if (nrp[i]->ip.len == 0) {
            nrp[i]->ip = nmcf->local_ip;
        }
    }
    return NGX_CONF_OK;
}