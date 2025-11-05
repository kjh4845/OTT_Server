#ifndef SERVER_H
#define SERVER_H

#include <limits.h>

#include "db.h"
#include "threadpool.h"

typedef struct server_ctx {
    int listen_fd;
    int epoll_fd;
    thread_pool_t pool;
    db_ctx_t db;
    char media_dir[PATH_MAX];
    char thumb_dir[PATH_MAX];
    char static_dir[PATH_MAX];
    char db_path[PATH_MAX];
    char data_dir[PATH_MAX];
    char security_headers[512];
    int port;
    int session_ttl_hours;
} server_ctx_t;

#endif
