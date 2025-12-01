#ifndef SERVER_H
#define SERVER_H

// 서버 전체에서 공유하는 전역 컨텍스트 구조체 정의

#include <limits.h>

#include "db.h"
#include "threadpool.h"

typedef struct server_ctx {
    int listen_fd;          // accept()용 리스닝 소켓
    int epoll_fd;           // 이벤트 루프에서 사용하는 epoll/poll 핸들러
    thread_pool_t pool;     // 워커 스레드 풀
    db_ctx_t db;            // SQLite 연결 래퍼
    char media_dir[PATH_MAX];
    char thumb_dir[PATH_MAX];
    char static_dir[PATH_MAX];
    char db_path[PATH_MAX];
    char data_dir[PATH_MAX];
    char security_headers[512]; // 모든 응답에 삽입할 보안 헤더
    int port;
    int session_ttl_hours;  // 세션 만료 시간(시간 단위)
} server_ctx_t;

#endif
