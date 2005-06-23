
/*
 * Copyright (C) Igor Sysoev
 */


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_event.h>


static ngx_int_t ngx_cmp_sockaddr(struct sockaddr *sa1, struct sockaddr *sa2);
static void ngx_clean_old_cycles(ngx_event_t *ev);


volatile ngx_cycle_t  *ngx_cycle;
ngx_array_t            ngx_old_cycles;

static ngx_pool_t     *ngx_temp_pool;
static ngx_event_t     ngx_cleaner_event;

ngx_uint_t             ngx_test_config;

#if (NGX_THREADS)
ngx_tls_key_t          ngx_core_tls_key;
#endif


/* STUB NAME */
static ngx_connection_t  dumb;
/* STUB */

#ifdef NGX_ERROR_LOG_PATH
static ngx_str_t  error_log = ngx_string(NGX_ERROR_LOG_PATH);
#else
static ngx_str_t  error_log = ngx_null_string;
#endif


ngx_cycle_t *ngx_init_cycle(ngx_cycle_t *old_cycle)
{
    void                      *rv;
    ngx_uint_t                 i, n, failed;
    ngx_log_t                 *log;
    ngx_conf_t                 conf;
    ngx_pool_t                *pool;
    ngx_cycle_t               *cycle, **old;
    ngx_socket_t               fd;
    ngx_list_part_t           *part;
    ngx_open_file_t           *file;
    ngx_listening_t           *ls, *nls;
    ngx_core_conf_t           *ccf;
    ngx_core_module_t         *module;
#if (NGX_HAVE_DEFERRED_ACCEPT && defined SO_ACCEPTFILTER)
    struct accept_filter_arg   af;
#endif
#if (NGX_HAVE_DEFERRED_ACCEPT && defined TCP_DEFER_ACCEPT)
    int                        timeout;
#endif

    log = old_cycle->log;

    pool = ngx_create_pool(NGX_CYCLE_POOL_SIZE, log);
    if (pool == NULL) {
        return NULL;
    }
    pool->log = log;

    cycle = ngx_pcalloc(pool, sizeof(ngx_cycle_t));
    if (cycle == NULL) {
        ngx_destroy_pool(pool);
        return NULL;
    }

    cycle->pool = pool;
    cycle->log = log;
    cycle->old_cycle = old_cycle;
    cycle->conf_file = old_cycle->conf_file;
    cycle->root.len = sizeof(NGX_PREFIX) - 1;
    cycle->root.data = (u_char *) NGX_PREFIX;


    n = old_cycle->pathes.nelts ? old_cycle->pathes.nelts : 10;

    cycle->pathes.elts = ngx_pcalloc(pool, n * sizeof(ngx_path_t *));
    if (cycle->pathes.elts == NULL) {
        ngx_destroy_pool(pool);
        return NULL;
    }

    cycle->pathes.nelts = 0;
    cycle->pathes.size = sizeof(ngx_path_t *);
    cycle->pathes.nalloc = n;
    cycle->pathes.pool = pool;


    if (old_cycle->open_files.part.nelts) {
        n = old_cycle->open_files.part.nelts;
        for (part = old_cycle->open_files.part.next; part; part = part->next) {
            n += part->nelts;
        }

    } else {
        n = 20;
    }

    if (ngx_list_init(&cycle->open_files, pool, n, sizeof(ngx_open_file_t))
                                                                  == NGX_ERROR)
    {
        ngx_destroy_pool(pool);
        return NULL;
    }


    cycle->new_log = ngx_log_create_errlog(cycle, NULL);
    if (cycle->new_log == NULL) {
        ngx_destroy_pool(pool);
        return NULL;
    }

    cycle->new_log->file->name = error_log;


    n = old_cycle->listening.nelts ? old_cycle->listening.nelts : 10;

    cycle->listening.elts = ngx_pcalloc(pool, n * sizeof(ngx_listening_t));
    if (cycle->listening.elts == NULL) {
        ngx_destroy_pool(pool);
        return NULL;
    }

    cycle->listening.nelts = 0;
    cycle->listening.size = sizeof(ngx_listening_t);
    cycle->listening.nalloc = n;
    cycle->listening.pool = pool;


    cycle->conf_ctx = ngx_pcalloc(pool, ngx_max_module * sizeof(void *));
    if (cycle->conf_ctx == NULL) {
        ngx_destroy_pool(pool);
        return NULL;
    }


    for (i = 0; ngx_modules[i]; i++) {
        if (ngx_modules[i]->type != NGX_CORE_MODULE) {
            continue;
        }

        module = ngx_modules[i]->ctx;

        if (module->create_conf) {
            rv = module->create_conf(cycle);
            if (rv == NGX_CONF_ERROR) {
                ngx_destroy_pool(pool);
                return NULL;
            }
            cycle->conf_ctx[ngx_modules[i]->index] = rv;
        }
    }


    ngx_memzero(&conf, sizeof(ngx_conf_t));
    /* STUB: init array ? */
    conf.args = ngx_array_create(pool, 10, sizeof(ngx_str_t));
    if (conf.args == NULL) {
        ngx_destroy_pool(pool);
        return NULL;
    }

    conf.ctx = cycle->conf_ctx;
    conf.cycle = cycle;
    conf.pool = pool;
    conf.log = log;
    conf.module_type = NGX_CORE_MODULE;
    conf.cmd_type = NGX_MAIN_CONF;

#if 0
    log->log_level = NGX_LOG_DEBUG_ALL;
#endif

    if (ngx_conf_parse(&conf, &cycle->conf_file) != NGX_CONF_OK) {
        ngx_destroy_pool(pool);
        return NULL;
    }

    if (ngx_test_config) {
        ngx_log_error(NGX_LOG_INFO, log, 0,
                      "the configuration file %s syntax is ok",
                      cycle->conf_file.data);
    }


    for (i = 0; ngx_modules[i]; i++) {
        if (ngx_modules[i]->type != NGX_CORE_MODULE) {
            continue;
        }

        module = ngx_modules[i]->ctx;

        if (module->init_conf) {
            if (module->init_conf(cycle, cycle->conf_ctx[ngx_modules[i]->index])
                                                              == NGX_CONF_ERROR)
            {
                ngx_destroy_pool(pool);
                return NULL;
            }
        }
    }


    failed = 0;


#if !(NGX_WIN32)
    if (ngx_create_pidfile(cycle, old_cycle) == NGX_ERROR) {
        failed = 1;
    }
#endif


    if (!failed) {
         ccf = (ngx_core_conf_t *) ngx_get_conf(cycle->conf_ctx,
                                                ngx_core_module);

        if (ngx_create_pathes(cycle, ccf->user) == NGX_ERROR) {
            failed = 1;
        }
    }


    if (!failed) {

        /* open the new files */

        part = &cycle->open_files.part;
        file = part->elts;

        for (i = 0; /* void */ ; i++) {

            if (i >= part->nelts) {
                if (part->next == NULL) {
                    break;
                }
                part = part->next;
                file = part->elts;
                i = 0;
            }

            if (file[i].name.data == NULL) {
                continue;
            }

            file[i].fd = ngx_open_file(file[i].name.data,
                                       NGX_FILE_RDWR,
                                       NGX_FILE_CREATE_OR_OPEN|NGX_FILE_APPEND);

            ngx_log_debug3(NGX_LOG_DEBUG_CORE, log, 0,
                           "log: %p %d \"%s\"",
                           &file[i], file[i].fd, file[i].name.data);

            if (file[i].fd == NGX_INVALID_FILE) {
                ngx_log_error(NGX_LOG_EMERG, log, ngx_errno,
                              ngx_open_file_n " \"%s\" failed",
                              file[i].name.data);
                failed = 1;
                break;
            }

#if (NGX_WIN32)
            if (ngx_file_append_mode(file[i].fd) == NGX_ERROR) {
                ngx_log_error(NGX_LOG_EMERG, log, ngx_errno,
                              ngx_file_append_mode_n " \"%s\" failed",
                              file[i].name.data);
                failed = 1;
                break;
            }
#else
            if (fcntl(file[i].fd, F_SETFD, FD_CLOEXEC) == -1) {
                ngx_log_error(NGX_LOG_EMERG, log, ngx_errno,
                              "fcntl(FD_CLOEXEC) \"%s\" failed",
                              file[i].name.data);
                failed = 1;
                break;
            }
#endif
        }
    }

    cycle->log = cycle->new_log;
    pool->log = cycle->new_log;

    if (cycle->log->log_level == 0) {
        cycle->log->log_level = NGX_LOG_ERR;
    }

    if (!failed) {

        /* handle the listening sockets */

        if (old_cycle->listening.nelts) {
            ls = old_cycle->listening.elts;
            for (i = 0; i < old_cycle->listening.nelts; i++) {
                ls[i].remain = 0;
            }

            nls = cycle->listening.elts;
            for (n = 0; n < cycle->listening.nelts; n++) {

                for (i = 0; i < old_cycle->listening.nelts; i++) {
                    if (ls[i].ignore) {
                        continue;
                    }

                    if (ngx_cmp_sockaddr(nls[n].sockaddr, ls[i].sockaddr)
                        == NGX_OK)
                    {
                        fd = ls[i].fd;
#if (NGX_WIN32)
                        /*
                         * Winsock assignes a socket number divisible by 4 so
                         * to find a connection we divide a socket number by 4.
                         */

                        fd /= 4;
#endif
                        if (fd >= (ngx_socket_t) cycle->connection_n) {
                            ngx_log_error(NGX_LOG_EMERG, log, 0,
                                        "%d connections is not enough to hold "
                                        "an open listening socket on %V, "
                                        "required at least %d connections",
                                        cycle->connection_n,
                                        &ls[i].addr_text, fd);
                            failed = 1;
                            break;
                        }

                        nls[n].fd = ls[i].fd;
                        nls[n].remain = 1;
                        ls[i].remain = 1;

#if (NGX_HAVE_DEFERRED_ACCEPT && defined SO_ACCEPTFILTER)

                        /*
                         * FreeBSD, except the most recent versions,
                         * can not remove accept filter
                         */
                        nls[n].deferred_accept = ls[i].deferred_accept;

                        if (ls[i].accept_filter && nls[n].accept_filter) {
                            if (ngx_strcmp(ls[i].accept_filter,
                                           nls[n].accept_filter) != 0)
                            {
                                nls[n].delete_deferred = 1;
                                nls[n].add_deferred = 1;
                            }

                        } else if (ls[i].accept_filter) {
                            nls[n].delete_deferred = 1;

                        } else if (nls[n].accept_filter) {
                            nls[n].add_deferred = 1;
                        }
#endif

#if (NGX_HAVE_DEFERRED_ACCEPT && defined TCP_DEFER_ACCEPT)

                        if (ls[n].deferred_accept && !nls[n].deferred_accept) {
                            nls[n].delete_deferred = 1;

                        } else if (ls[i].deferred_accept
                                   != nls[n].deferred_accept)
                        {
                            nls[n].add_deferred = 1;
                        }
#endif
                        break;
                    }
                }

                if (nls[n].fd == -1) {
                    nls[n].open = 1;
                }
            }

        } else {
            ls = cycle->listening.elts;
            for (i = 0; i < cycle->listening.nelts; i++) {
                ls[i].open = 1;
#if (NGX_HAVE_DEFERRED_ACCEPT && defined SO_ACCEPTFILTER)
                if (ls[i].accept_filter) {
                    ls[i].add_deferred = 1;
                }
#endif
#if (NGX_HAVE_DEFERRED_ACCEPT && defined TCP_DEFER_ACCEPT)
                if (ls[i].deferred_accept) {
                    ls[i].add_deferred = 1;
                }
#endif
            }
        }

        if (!ngx_test_config && !failed) {
            if (ngx_open_listening_sockets(cycle) == NGX_ERROR) {
                failed = 1;
            }

#if (NGX_HAVE_DEFERRED_ACCEPT)

            if (!failed) {
                ls = cycle->listening.elts;
                for (i = 0; i < cycle->listening.nelts; i++) {

#ifdef SO_ACCEPTFILTER
                    if (ls[i].delete_deferred) {
                        if (setsockopt(ls[i].fd, SOL_SOCKET, SO_ACCEPTFILTER,
                                       NULL, 0) == -1)
                        {
                            ngx_log_error(NGX_LOG_ALERT, log, ngx_errno,
                                        "setsockopt(SO_ACCEPTFILTER, NULL) "
                                        "for %V failed, ignored",
                                        &ls[i].addr_text);

                            if (ls[i].accept_filter) {
                                ngx_log_error(NGX_LOG_ALERT, log, 0,
                                        "could not change the accept filter "
                                        "to \"%s\" for %V, ignored",
                                        ls[i].accept_filter, &ls[i].addr_text);
                            }

                            continue;
                        }

                        ls[i].deferred_accept = 0;
                    }

                    if (ls[i].add_deferred) {
                        ngx_memzero(&af, sizeof(struct accept_filter_arg));
                        (void) ngx_cpystrn((u_char *) af.af_name,
                                           (u_char *) ls[i].accept_filter, 16);

                        if (setsockopt(ls[i].fd, SOL_SOCKET, SO_ACCEPTFILTER,
                                 &af, sizeof(struct accept_filter_arg)) == -1)
                        {
                            ngx_log_error(NGX_LOG_ALERT, log, ngx_errno,
                                        "setsockopt(SO_ACCEPTFILTER, \"%s\") "
                                        "for %V failed, ignored",
                                        ls[i].accept_filter, &ls[i].addr_text);
                            continue;
                        }

                        ls[i].deferred_accept = 1;
                    }
#endif

#ifdef TCP_DEFER_ACCEPT
                    if (ls[i].add_deferred || ls[i].delete_deferred) {
                        timeout = 0;

                        if (ls[i].add_deferred) {
                            timeout = (int) (ls[i].post_accept_timeout / 1000);
                        }

                        if (setsockopt(ls[i].fd, IPPROTO_TCP, TCP_DEFER_ACCEPT,
                                       &timeout, sizeof(int)) == -1)
                        {
                            ngx_log_error(NGX_LOG_ALERT, log, ngx_errno,
                                        "setsockopt(TCP_DEFER_ACCEPT, %d) "
                                        "for %V failed, ignored",
                                        timeout, &ls[i].addr_text);
                            continue;
                        }
                    }

                    if (ls[i].add_deferred) {
                        ls[i].deferred_accept = 1;
                    }
#endif
                }
            }
#endif
        }
    }

    if (failed) {

        /* rollback the new cycle configuration */

        part = &cycle->open_files.part;
        file = part->elts;

        for (i = 0; /* void */ ; i++) {

            if (i >= part->nelts) {
                if (part->next == NULL) {
                    break;
                }
                part = part->next;
                file = part->elts;
                i = 0;
            }

            if (file[i].fd == NGX_INVALID_FILE
                || file[i].fd == ngx_stderr_fileno)
            {
                continue;
            }

            if (ngx_close_file(file[i].fd) == NGX_FILE_ERROR) {
                ngx_log_error(NGX_LOG_EMERG, log, ngx_errno,
                              ngx_close_file_n " \"%s\" failed",
                              file[i].name.data);
            }
        }

        if (ngx_test_config) {
            ngx_destroy_pool(pool);
            return NULL;
        }

        ls = cycle->listening.elts;
        for (i = 0; i < cycle->listening.nelts; i++) {
            if (ls[i].fd == -1 || !ls[i].open) {
                continue;
            }

            if (ngx_close_socket(ls[i].fd) == -1) {
                ngx_log_error(NGX_LOG_EMERG, log, ngx_socket_errno,
                              ngx_close_socket_n " %V failed",
                              &ls[i].addr_text);
            }
        }

        ngx_destroy_pool(pool);
        return NULL;
    }


    /* commit the new cycle configuration */

#if !(NGX_WIN32)

    if (!ngx_test_config && cycle->log->file->fd != STDERR_FILENO) {

        ngx_log_debug3(NGX_LOG_DEBUG_CORE, log, 0,
                       "dup2: %p %d \"%s\"",
                       cycle->log->file,
                       cycle->log->file->fd, cycle->log->file->name.data);

        if (dup2(cycle->log->file->fd, STDERR_FILENO) == NGX_ERROR) {
            ngx_log_error(NGX_LOG_EMERG, log, ngx_errno,
                          "dup2(STDERR) failed");
            /* fatal */
            exit(1);
        }
    }

#endif

    pool->log = cycle->log;

    for (i = 0; ngx_modules[i]; i++) {
        if (ngx_modules[i]->init_module) {
            if (ngx_modules[i]->init_module(cycle) == NGX_ERROR) {
                /* fatal */
                exit(1);
            }
        }
    }


    /* close and delete stuff that lefts from an old cycle */

    /* close the unneeded listening sockets */

    ls = old_cycle->listening.elts;
    for (i = 0; i < old_cycle->listening.nelts; i++) {
        if (ls[i].remain) {
            continue;
        }

        if (ngx_close_socket(ls[i].fd) == -1) {
            ngx_log_error(NGX_LOG_EMERG, log, ngx_socket_errno,
                          ngx_close_socket_n " %V failed",
                          &ls[i].addr_text);
        }
    }


    /* close the unneeded open files */

    part = &old_cycle->open_files.part;
    file = part->elts;

    for (i = 0; /* void */ ; i++) {

        if (i >= part->nelts) {
            if (part->next == NULL) {
                break;
            }
            part = part->next;
            file = part->elts;
            i = 0;
        }

        if (file[i].fd == NGX_INVALID_FILE || file[i].fd == ngx_stderr_fileno) {
            continue;
        }

        if (ngx_close_file(file[i].fd) == NGX_FILE_ERROR) {
            ngx_log_error(NGX_LOG_EMERG, log, ngx_errno,
                          ngx_close_file_n " \"%s\" failed",
                          file[i].name.data);
        }
    }

    if (old_cycle->connections == NULL) {
        /* an old cycle is an init cycle */
        ngx_destroy_pool(old_cycle->pool);
        return cycle;
    }

    if (ngx_process == NGX_PROCESS_MASTER) {
        ngx_destroy_pool(old_cycle->pool);
        return cycle;
    }

    if (ngx_temp_pool == NULL) {
        ngx_temp_pool = ngx_create_pool(128, cycle->log);
        if (ngx_temp_pool == NULL) {
            ngx_log_error(NGX_LOG_EMERG, cycle->log, 0,
                          "can not create ngx_temp_pool");
            exit(1);
        }

        n = 10;
        ngx_old_cycles.elts = ngx_pcalloc(ngx_temp_pool,
                                          n * sizeof(ngx_cycle_t *));
        if (ngx_old_cycles.elts == NULL) {
            exit(1);
        }
        ngx_old_cycles.nelts = 0;
        ngx_old_cycles.size = sizeof(ngx_cycle_t *);
        ngx_old_cycles.nalloc = n;
        ngx_old_cycles.pool = ngx_temp_pool;

        ngx_cleaner_event.handler = ngx_clean_old_cycles;
        ngx_cleaner_event.log = cycle->log;
        ngx_cleaner_event.data = &dumb;
        dumb.fd = (ngx_socket_t) -1;
    }

    ngx_temp_pool->log = cycle->log;

    old = ngx_array_push(&ngx_old_cycles);
    if (old == NULL) {
        exit(1);
    }
    *old = old_cycle;

    if (!ngx_cleaner_event.timer_set) {
        ngx_add_timer(&ngx_cleaner_event, 30000);
        ngx_cleaner_event.timer_set = 1;
    }

    return cycle;
}


static ngx_int_t ngx_cmp_sockaddr(struct sockaddr *sa1, struct sockaddr *sa2)
{
    struct sockaddr_in  *sin1, *sin2;

    /* AF_INET only */

    if (sa1->sa_family != AF_INET || sa2->sa_family != AF_INET) {
        return NGX_DECLINED;
    }

    sin1 = (struct sockaddr_in *) sa1;
    sin2 = (struct sockaddr_in *) sa2;

    if (sin1->sin_addr.s_addr != sin2->sin_addr.s_addr) {
        return NGX_DECLINED;
    }

    if (sin1->sin_port != sin2->sin_port) {
        return NGX_DECLINED;
    }

    return NGX_OK;
}


#if !(NGX_WIN32)

ngx_int_t ngx_create_pidfile(ngx_cycle_t *cycle, ngx_cycle_t *old_cycle)
{
    ngx_uint_t        trunc;
    size_t            len;
    u_char            pid[NGX_INT64_LEN];
    ngx_file_t        file;
    ngx_core_conf_t  *ccf, *old_ccf;

    if (!ngx_test_config && old_cycle && old_cycle->conf_ctx == NULL) {

        /*
         * do not create the pid file in the first ngx_init_cycle() call
         * because we need to write the demonized process pid 
         */

        return NGX_OK;
    }

    ccf = (ngx_core_conf_t *) ngx_get_conf(cycle->conf_ctx, ngx_core_module);

    if (!ngx_test_config && old_cycle) {
        old_ccf = (ngx_core_conf_t *) ngx_get_conf(old_cycle->conf_ctx,
                                                   ngx_core_module);

        if (ccf->pid.len == old_ccf->pid.len
            && ngx_strcmp(ccf->pid.data, old_ccf->pid.data) == 0)
        {

            /* pid file name is the same */

            return NGX_OK;
        }
    }

    ngx_memzero(&file, sizeof(ngx_file_t));
    file.name = (ngx_inherited && getppid() > 1) ? ccf->newpid : ccf->pid;
    file.log = cycle->log;

    trunc = ngx_test_config ? 0: NGX_FILE_TRUNCATE;

    file.fd = ngx_open_file(file.name.data, NGX_FILE_RDWR,
                            NGX_FILE_CREATE_OR_OPEN|trunc);

    if (file.fd == NGX_INVALID_FILE) {
        ngx_log_error(NGX_LOG_EMERG, cycle->log, ngx_errno,
                      ngx_open_file_n " \"%s\" failed", file.name.data);
        return NGX_ERROR;
    }

    if (!ngx_test_config) {
        len = ngx_sprintf(pid, "%P%N", ngx_pid) - pid;

        if (ngx_write_file(&file, pid, len, 0) == NGX_ERROR) {
            return NGX_ERROR;
        }
    }

    if (ngx_close_file(file.fd) == NGX_FILE_ERROR) {
        ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,
                      ngx_close_file_n " \"%s\" failed", file.name.data);
    }

    ngx_delete_pidfile(old_cycle);

    return NGX_OK;
}


void ngx_delete_pidfile(ngx_cycle_t *cycle)
{   
    u_char           *name;
    ngx_core_conf_t  *ccf;

    if (cycle == NULL || cycle->conf_ctx == NULL) {
        return;
    }

    ccf = (ngx_core_conf_t *) ngx_get_conf(cycle->conf_ctx, ngx_core_module);

    if (ngx_inherited && getppid() > 1) {
        name = ccf->newpid.data;

    } else { 
        name = ccf->pid.data;
    }

    if (ngx_delete_file(name) == NGX_FILE_ERROR) {
        ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,
                      ngx_delete_file_n " \"%s\" failed", name);
    }
}

#endif


void ngx_reopen_files(ngx_cycle_t *cycle, ngx_uid_t user)
{
    ngx_fd_t          fd;
    ngx_uint_t        i;
    ngx_list_part_t  *part;
    ngx_open_file_t  *file;
#if !(NGX_WIN32)
    ngx_file_info_t   fi;
#endif

    part = &cycle->open_files.part;
    file = part->elts;

    for (i = 0; /* void */ ; i++) {

        if (i >= part->nelts) {
            if (part->next == NULL) {
                break;
            }
            part = part->next;
            file = part->elts;
            i = 0;
        }

        if (file[i].name.data == NULL) {
            continue;
        }

        fd = ngx_open_file(file[i].name.data, NGX_FILE_RDWR,
                           NGX_FILE_CREATE_OR_OPEN|NGX_FILE_APPEND);

        ngx_log_debug3(NGX_LOG_DEBUG_EVENT, cycle->log, 0,
                       "reopen file \"%s\", old:%d new:%d",
                       file[i].name.data, file[i].fd, fd);

        if (fd == NGX_INVALID_FILE) {
            ngx_log_error(NGX_LOG_EMERG, cycle->log, ngx_errno,
                          ngx_open_file_n " \"%s\" failed", file[i].name.data);
            continue;
        }

#if (NGX_WIN32)
        if (ngx_file_append_mode(fd) == NGX_ERROR) {
            ngx_log_error(NGX_LOG_EMERG, cycle->log, ngx_errno,
                          ngx_file_append_mode_n " \"%s\" failed",
                          file[i].name.data);

            if (ngx_close_file(fd) == NGX_FILE_ERROR) {
                ngx_log_error(NGX_LOG_EMERG, cycle->log, ngx_errno,
                              ngx_close_file_n " \"%s\" failed",
                              file[i].name.data);
            }

            continue;
        }
#else
        if (user != (ngx_uid_t) NGX_CONF_UNSET_UINT) {

            if (ngx_file_info((const char *) file[i].name.data, &fi) == -1) {
                ngx_log_error(NGX_LOG_EMERG, cycle->log, ngx_errno,
                              ngx_file_info_n " \"%s\" failed",
                              file[i].name.data);

                if (ngx_close_file(fd) == NGX_FILE_ERROR) {
                    ngx_log_error(NGX_LOG_EMERG, cycle->log, ngx_errno,
                                  ngx_close_file_n " \"%s\" failed",
                                  file[i].name.data);
                }
            }

            if (fi.st_uid != user) {
                if (chown((const char *) file[i].name.data, user, -1) == -1) {
                    ngx_log_error(NGX_LOG_EMERG, cycle->log, ngx_errno,
                                  "chown(\"%s\", %d) failed",
                                  file[i].name.data, user);

                    if (ngx_close_file(fd) == NGX_FILE_ERROR) {
                        ngx_log_error(NGX_LOG_EMERG, cycle->log, ngx_errno,
                                      ngx_close_file_n " \"%s\" failed",
                                      file[i].name.data);
                    }
                }
            }

            if ((fi.st_mode & (S_IRUSR|S_IWUSR)) != (S_IRUSR|S_IWUSR)) {

                fi.st_mode |= (S_IRUSR|S_IWUSR);

                if (chmod((const char *) file[i].name.data, fi.st_mode) == -1) {
                    ngx_log_error(NGX_LOG_EMERG, cycle->log, ngx_errno,
                                  "chmod() \"%s\" failed", file[i].name.data);

                    if (ngx_close_file(fd) == NGX_FILE_ERROR) {
                        ngx_log_error(NGX_LOG_EMERG, cycle->log, ngx_errno,
                                      ngx_close_file_n " \"%s\" failed",
                                      file[i].name.data);
                    }
                }
            }
        }

        if (fcntl(fd, F_SETFD, FD_CLOEXEC) == -1) {
            ngx_log_error(NGX_LOG_EMERG, cycle->log, ngx_errno,
                          "fcntl(FD_CLOEXEC) \"%s\" failed",
                          file[i].name.data);

            if (ngx_close_file(fd) == NGX_FILE_ERROR) {
                ngx_log_error(NGX_LOG_EMERG, cycle->log, ngx_errno,
                              ngx_close_file_n " \"%s\" failed",
                              file[i].name.data);
            }

            continue;
        }
#endif

        if (ngx_close_file(file[i].fd) == NGX_FILE_ERROR) {
            ngx_log_error(NGX_LOG_EMERG, cycle->log, ngx_errno,
                          ngx_close_file_n " \"%s\" failed",
                          file[i].name.data);
        }

        file[i].fd = fd;
    }

#if !(NGX_WIN32)

    if (cycle->log->file->fd != STDERR_FILENO) {
        if (dup2(cycle->log->file->fd, STDERR_FILENO) == -1) {
            ngx_log_error(NGX_LOG_EMERG, cycle->log, ngx_errno,
                          "dup2(STDERR) failed");
        }
    }

#endif
}


static void ngx_clean_old_cycles(ngx_event_t *ev)
{
    ngx_uint_t     i, n, found, live;
    ngx_log_t     *log;
    ngx_cycle_t  **cycle;

    log = ngx_cycle->log;
    ngx_temp_pool->log = log;

    ngx_log_debug0(NGX_LOG_DEBUG_CORE, log, 0, "clean old cycles");

    live = 0;

    cycle = ngx_old_cycles.elts;
    for (i = 0; i < ngx_old_cycles.nelts; i++) {

        if (cycle[i] == NULL) {
            continue;
        }

        found = 0;

        for (n = 0; n < cycle[i]->connection_n; n++) {
            if (cycle[i]->connections[n].fd != (ngx_socket_t) -1) {
                found = 1;

                ngx_log_debug1(NGX_LOG_DEBUG_CORE, log, 0, "live fd:%d", n);

                break;
            }
        }

        if (found) {
            live = 1;
            continue;
        }

        ngx_log_debug1(NGX_LOG_DEBUG_CORE, log, 0, "clean old cycle: %d", i);

        ngx_destroy_pool(cycle[i]->pool);
        cycle[i] = NULL;
    }

    ngx_log_debug1(NGX_LOG_DEBUG_CORE, log, 0, "old cycles status: %d", live);

    if (live) {
        ngx_add_timer(ev, 30000);

    } else {
        ngx_destroy_pool(ngx_temp_pool);
        ngx_temp_pool = NULL;
        ngx_old_cycles.nelts = 0;
    }
}
