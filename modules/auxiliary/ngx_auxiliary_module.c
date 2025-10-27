//
// Created by dear on 22-5-28.
//
#include "ngx_auxiliary_module.h"
#include <ngx_channel.h>

static void ngx_aux_process_cycle(ngx_cycle_t *cycle, void *data);

static void ngx_pass_open_channel(ngx_cycle_t *cycle);

static ngx_int_t ngx_aux_init_master(ngx_cycle_t *cycle);

static void *ngx_aux_create_conf(ngx_cycle_t *cycle);

static ngx_core_module_t auxiliary_module = {
    ngx_string("auxiliary"),
    ngx_aux_create_conf,  // 解析配置文件之前执行
    NULL                  // 解析配置文件之后执行
};

ngx_module_t ngx_auxiliary_module = {NGX_MODULE_V1,
                                     &auxiliary_module,
                                     NULL,
                                     NGX_CORE_MODULE,
                                     NULL,                /* init master */
                                     NULL,                /* init module */
                                     ngx_aux_init_master, /* init process */
                                     NULL,                /* init thread */
                                     NULL,                /* exit thread */
                                     NULL,                /* exit process */
                                     NULL,                /* exit master */
                                     NGX_MODULE_V1_PADDING};

#define ngx_aux_get_main_conf_ptr(cf)   \
    (ngx_aux_proc_main_conf_t **) &(cf) \
        ->cycle->conf_ctx[ngx_auxiliary_module.index]

static void *ngx_aux_create_conf(ngx_cycle_t *cycle) {
    ngx_aux_proc_main_conf_t *mcf;
    mcf = ngx_pcalloc(cycle->pool, sizeof(ngx_aux_proc_main_conf_t));
    if (mcf == NULL) {
        return NULL;
    }
    if (ngx_array_init(&mcf->process, cycle->pool, 4,
                       sizeof(ngx_aux_proc_t *)) != NGX_OK) {
        return NULL;
    }
    return mcf;
}

ngx_int_t ngx_aux_add_proc(ngx_conf_t *cf, ngx_aux_proc_t *proc) {
    ngx_aux_proc_main_conf_t **ptr, *mcf;
    ngx_aux_proc_t **n_proc;
    ngx_uint_t i;

    ptr = ngx_aux_get_main_conf_ptr(cf);

    mcf = *ptr;
    if (mcf == NULL) {
        return NGX_ERROR;
    }

    n_proc = mcf->process.elts;
    for (i = 0; i < mcf->process.nelts; i++) {
        if (n_proc[i]->name.len == proc->name.len &&
            ngx_strncmp(n_proc[i]->name.data, proc->name.data,
                        proc->name.len) == 0) {
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "process \"%V\" exists",
                               &proc->name);
            return NGX_ERROR;
        }
    }

    n_proc = ngx_array_push(&mcf->process);
    if (n_proc == NULL) {
        return NGX_ERROR;
    }
    *n_proc = proc;
    return NGX_OK;
}

void ngx_aux_start_auxiliary_processes(ngx_cycle_t *cycle, ngx_uint_t respawn) {
    ngx_aux_proc_main_conf_t *mcf;
    ngx_aux_proc_t **proc;
    ngx_uint_t i, n;
    size_t len;
    char buf[256], *name;

    mcf = (ngx_aux_proc_main_conf_t *) ngx_get_conf(cycle->conf_ctx,
                                                    ngx_auxiliary_module);
    if (mcf == NULL) {
        return;
    }

    n = mcf->process.nelts;
    proc = mcf->process.elts;

    for (i = 0; i < n; ++i) {
        len = ngx_sprintf((u_char *) buf, "auxiliary: %V", &proc[i]->name) -
              (u_char *) buf;
        buf[len] = 0;
        ngx_log_error(NGX_LOG_NOTICE, cycle->log, 0,
                      "start auxiliary processes %s", buf);
        name = ngx_palloc(cycle->pool, len + 1);
        if (name == NULL) {
            name = (char *) proc[i]->name.data;
        } else {
            ngx_memcpy(name, buf, len);
            name[len] = 0;
        }

        ngx_spawn_process(
            cycle, ngx_aux_process_cycle, proc[i], name,
            respawn ? NGX_PROCESS_JUST_RESPAWN : NGX_PROCESS_RESPAWN);
        ngx_pass_open_channel(cycle);
    }
}

static void ngx_pass_open_channel(ngx_cycle_t *cycle) {
    ngx_int_t i;
    ngx_channel_t ch;

    ch.command = NGX_CMD_OPEN_CHANNEL;
    ch.pid = ngx_processes[ngx_process_slot].pid;
    ch.slot = ngx_process_slot;
    ch.fd = ngx_processes[ngx_process_slot].channel[0];

    for (i = 0; i < ngx_last_process; i++) {
        if (i == ngx_process_slot || ngx_processes[i].pid == -1 ||
            ngx_processes[i].channel[0] == -1) {
            continue;
        }

        ngx_log_debug6(NGX_LOG_DEBUG_CORE, cycle->log, 0,
                       "pass channel s:%i pid:%P fd:%d to s:%i pid:%P fd:%d",
                       ch.slot, ch.pid, ch.fd, i, ngx_processes[i].pid,
                       ngx_processes[i].channel[0]);

        ngx_write_channel(ngx_processes[i].channel[0], &ch,
                          sizeof(ngx_channel_t), cycle->log);
    }
}

static void ngx_aux_process_cycle(ngx_cycle_t *cycle, void *data) {
    char buf[256];
    ngx_int_t ret;
    size_t len;
    ngx_aux_proc_t *proc = data;
    ngx_process = NGX_PROCESS_HELPER;
#if defined(HAVE_PRIVILEGED_PROCESS_PATCH) && !NGX_WIN32
    ngx_is_privileged_agent = 1;
#endif

    ngx_use_accept_mutex = 0;

    ngx_close_listening_sockets(cycle);

    cycle->connection_n = 512;

#ifdef NGX_AUX_PATCHED  // supress compile error
    ngx_worker_aux_process_init(cycle);
#endif
    len = ngx_sprintf((u_char *) buf, "auxiliary %V", &proc->name) -
          (u_char *) buf;
    buf[len] = 0;
    ngx_setproctitle(buf);

    ret = proc->process(cycle, proc);
    if (ret != NGX_OK) {
        ngx_log_error(NGX_LOG_NOTICE, cycle->log, ret,
                      "exiting auxiliary process: %s", buf);
        exit(1);
    }

    for (;;) {
        if (ngx_terminate || ngx_quit) {
            ngx_log_error(NGX_LOG_NOTICE, cycle->log, 0, "exiting");
#ifdef NGX_AUX_PATCHED  // supress compile error
            ngx_worker_aux_process_exit(cycle);
#endif
        }

        if (ngx_reopen) {
            ngx_reopen = 0;
            ngx_log_error(NGX_LOG_NOTICE, cycle->log, 0, "reopening logs");
            ngx_reopen_files(cycle, -1);
        }

        ngx_process_events_and_timers(cycle);
    }
}

static ngx_int_t ngx_aux_init_master(ngx_cycle_t *cycle) {
    ngx_aux_proc_main_conf_t *mcf;
    ngx_aux_proc_t **proc;
    ngx_uint_t i, n;

    if (ngx_process != NGX_PROCESS_SINGLE
#ifndef NGX_AUX_PATCHED  // run as aux process not need run in worker 0
        && ngx_worker != 0
#endif
    ) {
        return NGX_OK;
    }

    mcf = (ngx_aux_proc_main_conf_t *) ngx_get_conf(cycle->conf_ctx,
                                                    ngx_auxiliary_module);
    if (mcf == NULL) {
        return NGX_ERROR;
    }
    proc = mcf->process.elts;
    n = mcf->process.nelts;

    for (i = 0; i < n; ++i) {
        if (proc[i]->process(cycle, proc[i]) != NGX_OK) {
            return NGX_ERROR;
        }
    }

    ngx_log_error(NGX_LOG_INFO, cycle->log, 0, "auxiliary run in single mod");
    return NGX_OK;
}
