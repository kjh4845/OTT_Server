// 메인 이벤트 루프 및 서버 초기화를 담당하는 엔트리 포인트 파일
#include "auth.h"
#include "ffmpeg.h"
#include "history.h"
#include "http.h"
#include "router.h"
#include "server.h"
#include "utils.h"
#include "video.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#if defined(__linux__)
#include <sys/epoll.h>
#define USE_EPOLL 1
#else
#include <poll.h>
#define USE_EPOLL 0
#endif
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

// 에지 트리거 I/O 대기 시 한 번에 수용할 최대 이벤트 수
#define MAX_EVENTS 128

static volatile sig_atomic_t g_running = 1;

// SIGINT/SIGTERM을 받으면 메인 루프가 종료되도록 플래그만 갱신한다.
static void handle_signal(int signum) {
    (void)signum;
    g_running = 0;
}

// 논블로킹 소켓으로 전환한다.
static int make_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) {
        return -1;
    }
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        return -1;
    }
    return 0;
}

// 블로킹 소켓으로 되돌린다. 워커 스레드에서 요청을 처리할 때 사용한다.
static int make_blocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) {
        return -1;
    }
    if (fcntl(fd, F_SETFL, flags & ~O_NONBLOCK) == -1) {
        return -1;
    }
    return 0;
}

// TCP 서버 소켓을 만들고 논블로킹 상태로 리스닝 준비까지 마친다.
static int create_listen_socket(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    if (listen(fd, 128) < 0) {
        close(fd);
        return -1;
    }
    if (make_nonblocking(fd) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

// 기본 디렉터리 + 상대 경로를 조합한 결과를 out 버퍼에 채운다.
static int join_path(const char *base, const char *path, char *out, size_t len) {
    if (snprintf(out, len, "%s/%s", base, path) >= (int)len) {
        return -1;
    }
    return 0;
}

// 정적 파일 확장자에 맞춰 심플한 MIME 타입을 선택한다.
static const char *mime_type_for_path(const char *path) {
    const char *ext = strrchr(path, '.');
    if (!ext) return "application/octet-stream";
    ext++;
    if (strcasecmp(ext, "html") == 0) return "text/html; charset=utf-8";
    if (strcasecmp(ext, "css") == 0) return "text/css; charset=utf-8";
    if (strcasecmp(ext, "js") == 0) return "application/javascript";
    if (strcasecmp(ext, "json") == 0) return "application/json";
    if (strcasecmp(ext, "png") == 0) return "image/png";
    if (strcasecmp(ext, "jpg") == 0 || strcasecmp(ext, "jpeg") == 0) return "image/jpeg";
    if (strcasecmp(ext, "svg") == 0) return "image/svg+xml";
    if (strcasecmp(ext, "ico") == 0) return "image/x-icon";
    if (strcasecmp(ext, "mp4") == 0) return "video/mp4";
    return "application/octet-stream";
}

// 경로 순회 공격을 방지하기 위해 ".." 토큰을 거부한다.
static int is_safe_path(const char *path) {
    if (strstr(path, "..")) {
        return 0;
    }
    return 1;
}

// API 응답 패턴을 재사용하기 위한 JSON 에러 헬퍼
static int send_json_error(server_ctx_t *server, int fd, int status, const char *message) {
    char body[512];
    int len = snprintf(body, sizeof(body), "{\"error\":\"%s\"}", message ? message : "error");
    if (len < 0 || (size_t)len >= sizeof(body)) {
        return -1;
    }
    return http_send_response(fd, status, http_status_text(status), "application/json",
                               body, (size_t)len, server->security_headers);
}

// 웹 클라이언트 정적 자산(css/js/html 등)을 찾아 내려준다.
static int serve_static_file(server_ctx_t *server, request_ctx_t *ctx) {
    const char *path = ctx->request->path;
    if (!path || !*path) {
        return send_json_error(server, ctx->client_fd, 404, "Not Found");
    }
    if (!is_safe_path(path)) {
        return send_json_error(server, ctx->client_fd, 403, "Forbidden");
    }
    char relative[512];
    if (strcmp(path, "/") == 0) {
        strcpy(relative, "index.html");
    } else if (path[0] == '/') {
        snprintf(relative, sizeof(relative), "%s", path + 1);
    } else {
        snprintf(relative, sizeof(relative), "%s", path);
    }

    char full_path[PATH_MAX];
    if (join_path(server->static_dir, relative, full_path, sizeof(full_path)) != 0) {
        return send_json_error(server, ctx->client_fd, 500, "Path too long");
    }
    struct stat st;
    if (stat(full_path, &st) != 0 || S_ISDIR(st.st_mode)) {
        return send_json_error(server, ctx->client_fd, 404, "Not Found");
    }
    const char *mime = mime_type_for_path(relative);
    return http_send_file_response(ctx->client_fd, 200, "OK", mime, full_path, 0, 0, 1,
                                   server->security_headers);
}

typedef struct client_task {
    server_ctx_t *server;
    int fd;
} client_task_t;

// 워커 스레드에서 실행되며 HTTP 요청 파싱 → 라우팅 → 응답까지 담당한다.
static void handle_client(void *arg) {
    client_task_t *task = (client_task_t *)arg;
    server_ctx_t *server = task->server;
    int fd = task->fd;
    free(task);

    make_blocking(fd);

    http_request_t req;
    if (http_parse_request(fd, &req, NULL, 0) != 0) {
        close(fd);
        return;
    }

    request_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.server = server;
    ctx.client_fd = fd;
    ctx.request = &req;

    auth_authenticate_request(&ctx);

    if (strncmp(req.path, "/api/", 5) == 0) {
        router_handle(&ctx);
    } else {
        int rc = serve_static_file(server, &ctx);
        (void)rc;
    }

    http_free_request(&req);
    close(fd);
}

// 환경 변수 또는 후보 경로 목록에서 우선순위대로 경로를 선택한다.
static void choose_path(const char *env_name, const char *candidates[], size_t count,
                        char *out, size_t len, int ensure_dir) {
    const char *value = getenv(env_name);
    if (value && *value) {
        snprintf(out, len, "%s", value);
        if (ensure_dir) {
            ensure_directory(out);
        }
        return;
    }
    for (size_t i = 0; i < count; ++i) {
        if (!candidates[i]) continue;
        if (ensure_dir) {
            if (ensure_directory(candidates[i]) == 0) {
                snprintf(out, len, "%s", candidates[i]);
                return;
            }
        } else {
            struct stat st;
            if (stat(candidates[i], &st) == 0) {
                snprintf(out, len, "%s", candidates[i]);
                return;
            }
        }
    }
    if (count > 0 && candidates[0]) {
        snprintf(out, len, "%s", candidates[0]);
        if (ensure_dir) {
            ensure_directory(out);
        }
    } else {
        out[0] = '\0';
    }
}

int main(void) {
    // 신호 처리기 등록: Ctrl+C(SIGINT) 등으로 안전하게 종료한다.
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    signal(SIGPIPE, SIG_IGN);

    server_ctx_t server;
    memset(&server, 0, sizeof(server));

    // 모든 응답에 공통으로 추가할 보안 헤더 세트
    const char *security =
        "X-Content-Type-Options: nosniff\r\n"
        "X-Frame-Options: DENY\r\n"
        "Content-Security-Policy: default-src 'self'; img-src 'self' data:; media-src 'self'; style-src 'self' 'unsafe-inline'; script-src 'self';\r\n";
    snprintf(server.security_headers, sizeof(server.security_headers), "%s", security);

    const char *static_candidates[] = {"./web/public", "../web/public", NULL};
    choose_path("STATIC_DIR", static_candidates, ARRAY_SIZE(static_candidates),
                server.static_dir, sizeof(server.static_dir), 0);
    struct stat static_stat;
    if (server.static_dir[0] == '\0' ||
        stat(server.static_dir, &static_stat) != 0 || !S_ISDIR(static_stat.st_mode)) {
        log_error("Static directory not found: %s", server.static_dir[0] ? server.static_dir : "(unset)");
        return 1;
    }
    log_info("Static directory: %s", server.static_dir);

    const char *media_candidates[] = {"./media", "../media", NULL};
    choose_path("MEDIA_DIR", media_candidates, ARRAY_SIZE(media_candidates),
                server.media_dir, sizeof(server.media_dir), 1);
    if (server.media_dir[0] == '\0') {
        log_error("Failed to determine media directory");
        return 1;
    }
    log_info("Media directory: %s", server.media_dir);

    const char *thumb_candidates[] = {"./web/thumbnails", "../web/thumbnails", NULL};
    choose_path("THUMB_DIR", thumb_candidates, ARRAY_SIZE(thumb_candidates),
                server.thumb_dir, sizeof(server.thumb_dir), 1);
    if (server.thumb_dir[0] == '\0') {
        log_error("Failed to determine thumbnail directory");
        return 1;
    }
    log_info("Thumbnail directory: %s", server.thumb_dir);

    const char *data_candidates[] = {"./data", "../data", NULL};
    choose_path("DATA_DIR", data_candidates, ARRAY_SIZE(data_candidates),
                server.data_dir, sizeof(server.data_dir), 1);
    if (server.data_dir[0] == '\0') {
        log_error("Failed to determine data directory");
        return 1;
    }
    log_info("Data directory: %s", server.data_dir);

    const char *db_path_env = getenv("DB_PATH");
    if (db_path_env && *db_path_env) {
        snprintf(server.db_path, sizeof(server.db_path), "%s", db_path_env);
    } else {
        if (join_path(server.data_dir, "app.db", server.db_path, sizeof(server.db_path)) != 0) {
            log_error("Database path too long");
            return 1;
        }
    }
    log_info("Database path: %s", server.db_path);

    const char *port_env = getenv("PORT");
    server.port = port_env ? atoi(port_env) : 3000;
    if (server.port <= 0) server.port = 3000;

    const char *ttl_env = getenv("SESSION_TTL_HOURS");
    server.session_ttl_hours = ttl_env ? atoi(ttl_env) : 24;
    if (server.session_ttl_hours <= 0) server.session_ttl_hours = 24;

    if (db_init(&server.db, server.db_path) != 0) {
        log_error("Failed to open database: %s", db_errmsg(&server.db));
        return 1;
    }
    char schema_path[PATH_MAX];
    const char *schema_candidates[] = {"./schema.sql", "../schema.sql", "./server/schema.sql", NULL};
    choose_path("SCHEMA_PATH", schema_candidates, ARRAY_SIZE(schema_candidates),
                schema_path, sizeof(schema_path), 0);
    struct stat schema_stat;
    if (schema_path[0] == '\0' ||
        stat(schema_path, &schema_stat) != 0 || !S_ISREG(schema_stat.st_mode)) {
        log_error("Schema file not found: %s", schema_path[0] ? schema_path : "(unset)");
        return 1;
    }
    if (db_run_schema(&server.db, schema_path) != 0) {
        log_error("Failed to run schema: %s", db_errmsg(&server.db));
        db_close(&server.db);
        return 1;
    }

    if (auth_initialize(&server) != 0) {
        log_error("Failed to initialize auth");
        db_close(&server.db);
        return 1;
    }
    if (video_initialize(&server) != 0) {
        log_error("Failed to initialize video module");
        db_close(&server.db);
        return 1;
    }
    if (ffmpeg_initialize(&server) != 0) {
        log_error("Failed to initialize ffmpeg module");
        db_close(&server.db);
        return 1;
    }

    size_t worker_count = (size_t)sysconf(_SC_NPROCESSORS_ONLN);
    if (worker_count == 0) worker_count = 4;
    worker_count *= 2;
    if (thread_pool_init(&server.pool, worker_count) != 0) {
        log_error("Failed to init thread pool");
        db_close(&server.db);
        return 1;
    }

    // 라우터가 참조할 HTTP 엔드포인트 테이블 정의
    route_entry_t routes[] = {
        {HTTP_POST, "/api/auth/login", auth_handle_login},
        {HTTP_POST, "/api/auth/register", auth_handle_register},
        {HTTP_POST, "/api/auth/logout", auth_handle_logout},
        {HTTP_GET, "/api/auth/me", auth_handle_me},
        {HTTP_GET, "/api/videos", video_handle_list},
        {HTTP_GET, "/api/videos/:id/stream", video_handle_stream},
        {HTTP_GET, "/api/videos/:id/thumbnail", video_handle_thumbnail},
        {HTTP_GET, "/api/history", history_handle_get},
        {HTTP_POST, "/api/history/:id", history_handle_update},
    };
    router_set_routes(routes, ARRAY_SIZE(routes));

    int listen_fd = create_listen_socket(server.port);
    if (listen_fd < 0) {
        log_error("Failed to create listen socket on port %d", server.port);
        thread_pool_destroy(&server.pool);
        db_close(&server.db);
        return 1;
    }
    server.listen_fd = listen_fd;

    server.epoll_fd = -1; // non-Linux 환경에서는 poll()을 사용할 수 있으므로 기본값은 -1

#if USE_EPOLL
    // Linux 환경에서는 epoll로 대량 접속을 효율적으로 감지한다.
    int epoll_fd = epoll_create1(0);
    if (epoll_fd < 0) {
        log_error("Failed to create epoll instance");
        close(listen_fd);
        thread_pool_destroy(&server.pool);
        db_close(&server.db);
        return 1;
    }
    server.epoll_fd = epoll_fd;

    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = listen_fd;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listen_fd, &ev) < 0) {
        log_error("epoll_ctl ADD listen fd failed");
        close(epoll_fd);
        close(listen_fd);
        thread_pool_destroy(&server.pool);
        db_close(&server.db);
        return 1;
    }

    log_info("Server listening on port %d", server.port);

    struct epoll_event events[MAX_EVENTS];
    while (g_running) {
        // epoll_wait으로 새 연결/데이터 도착을 기다린다.
        int n = epoll_wait(epoll_fd, events, MAX_EVENTS, 1000);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            log_error("epoll_wait error: %s", strerror(errno));
            break;
        }
        for (int i = 0; i < n; ++i) {
            if (events[i].data.fd == listen_fd) {
                for (;;) {
                    struct sockaddr_in client_addr;
                    socklen_t client_len = sizeof(client_addr);
                    int client_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &client_len);
                    if (client_fd < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            break;
                        }
                        log_warn("accept failed: %s", strerror(errno));
                        break;
                    }
                    if (make_nonblocking(client_fd) != 0) {
                        close(client_fd);
                        continue;
                    }
                    struct epoll_event client_ev;
                    client_ev.events = EPOLLIN;
                    client_ev.data.fd = client_fd;
                    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &client_ev) < 0) {
                        log_warn("epoll_ctl ADD client failed: %s", strerror(errno));
                        close(client_fd);
                    }
                }
            } else {
                int client_fd = events[i].data.fd;
                epoll_ctl(epoll_fd, EPOLL_CTL_DEL, client_fd, NULL);
                client_task_t *task = malloc(sizeof(client_task_t));
                if (!task) {
                    close(client_fd);
                    continue;
                }
                task->server = &server;
                task->fd = client_fd;
                thread_pool_submit(&server.pool, handle_client, task);
            }
        }
    }

    if (server.epoll_fd >= 0) {
        close(epoll_fd);
        server.epoll_fd = -1;
    }
#else
    // epoll을 쓸 수 없는 플랫폼(BSD, macOS 등)은 poll() 기반 루프를 사용한다.
    log_info("Server listening on port %d", server.port);
    struct pollfd fds[MAX_EVENTS];
    nfds_t fd_count = 1;
    memset(fds, 0, sizeof(fds));
    fds[0].fd = listen_fd;
    fds[0].events = POLLIN;

    while (g_running) {
        int n = poll(fds, fd_count, 1000);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            log_error("poll error: %s", strerror(errno));
            break;
        }
        if (fds[0].revents & POLLIN) {
            for (;;) {
                struct sockaddr_in client_addr;
                socklen_t client_len = sizeof(client_addr);
                int client_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &client_len);
                if (client_fd < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        break;
                    }
                    log_warn("accept failed: %s", strerror(errno));
                    break;
                }
                if (make_nonblocking(client_fd) != 0) {
                    close(client_fd);
                    continue;
                }
                if (fd_count >= MAX_EVENTS) {
                    log_warn("Too many open connections, dropping client");
                    close(client_fd);
                    continue;
                }
                fds[fd_count].fd = client_fd;
                fds[fd_count].events = POLLIN;
                fds[fd_count].revents = 0;
                fd_count++;
            }
        }
        for (nfds_t i = 1; i < fd_count;) {
            if (fds[i].revents & POLLIN) {
                int client_fd = fds[i].fd;
                fds[i] = fds[fd_count - 1];
                fd_count--;
                client_task_t *task = malloc(sizeof(client_task_t));
                if (!task) {
                    close(client_fd);
                    continue;
                }
                task->server = &server;
                task->fd = client_fd;
                thread_pool_submit(&server.pool, handle_client, task);
                continue;
            }
            if (fds[i].revents & (POLLERR | POLLHUP | POLLNVAL)) {
                close(fds[i].fd);
                fds[i] = fds[fd_count - 1];
                fd_count--;
                continue;
            }
            fds[i].revents = 0;
            i++;
        }
    }
    for (nfds_t i = 1; i < fd_count; ++i) {
        close(fds[i].fd);
    }
#endif

    log_info("Shutting down...");
    close(listen_fd);
    video_shutdown();
    thread_pool_destroy(&server.pool);
    db_close(&server.db);
    return 0;
}
