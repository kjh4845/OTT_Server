// 비디오 라이브러리 동기화, 스트리밍, 썸네일 API를 담당하는 모듈
#include "video.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "db.h"
#include "ffmpeg.h"
#include "http.h"
#include "utils.h"

// 공통 보안 헤더에 추가 헤더를 덧붙여 응답용 문자열을 만든다.
static void build_header(char *dest, size_t len, server_ctx_t *server, const char *extra) {
    if (!dest || len == 0) return;
    if (extra && *extra) {
        snprintf(dest, len, "%s%s", server->security_headers, extra);
    } else {
        snprintf(dest, len, "%s", server->security_headers);
    }
}

#define VIDEO_DEFAULT_LIMIT 12
#define VIDEO_MAX_LIMIT 50

// URL 인코딩 해석을 위한 헥사 문자 → 값 변환
static int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// 쿼리 파라미터에 있는 %XX 표현을 실제 문자로 되돌린다.
static int url_decode_component(const char *src, size_t len, char *dst, size_t dst_len) {
    if (!src || !dst || dst_len == 0) {
        return -1;
    }
    size_t di = 0;
    for (size_t si = 0; si < len; ++si) {
        char c = src[si];
        if (c == '+') {
            c = ' ';
        } else if (c == '%' && si + 2 < len) {
            int hi = hex_value(src[si + 1]);
            int lo = hex_value(src[si + 2]);
            if (hi >= 0 && lo >= 0) {
                c = (char)((hi << 4) | lo);
                si += 2;
            }
        }
        if (di + 1 >= dst_len) {
            return -1;
        }
        dst[di++] = c;
    }
    dst[di] = '\0';
    return 0;
}

// name=value 쌍에서 원하는 파라미터를 추출한다.
static int query_get_param(const char *query, const char *name, char *out, size_t out_len) {
    if (!query || !*query || !name || !out || out_len == 0) {
        return -1;
    }
    size_t name_len = strlen(name);
    const char *cursor = query;
    while (*cursor) {
        const char *amp = strchr(cursor, '&');
        size_t pair_len = amp ? (size_t)(amp - cursor) : strlen(cursor);
        const char *eq = memchr(cursor, '=', pair_len);
        size_t key_len = eq ? (size_t)(eq - cursor) : pair_len;
        if (key_len == name_len && strncmp(cursor, name, name_len) == 0) {
            const char *value_start = eq ? eq + 1 : cursor + key_len;
            size_t value_len = eq ? pair_len - key_len - 1 : 0;
            if (value_len == 0) {
                out[0] = '\0';
                return 0;
            }
            return url_decode_component(value_start, value_len, out, out_len);
        }
        if (!amp) {
            break;
        }
        cursor = amp + 1;
    }
    return -1;
}

// 정수 쿼리 파라미터를 파싱한다.
static int query_get_int(const char *query, const char *name, int *value_out) {
    char buf[32];
    if (query_get_param(query, name, buf, sizeof(buf)) != 0) {
        return -1;
    }
    char *end = NULL;
    long v = strtol(buf, &end, 10);
    if (!end || *end != '\0') {
        return -1;
    }
    *value_out = (int)v;
    return 0;
}

// 검색어 앞뒤 공백 제거
static void trim_spaces(char *s) {
    if (!s) return;
    size_t len = strlen(s);
    size_t start = 0;
    while (start < len && isspace((unsigned char)s[start])) {
        start++;
    }
    size_t end = len;
    while (end > start && isspace((unsigned char)s[end - 1])) {
        end--;
    }
    if (start > 0) {
        memmove(s, s + start, end - start);
    }
    s[end - start] = '\0';
}

// 환경 변수에서 정수를 읽되 파싱 오류 시 기본값을 돌려준다.
static int getenv_int(const char *name, int fallback) {
    const char *value = getenv(name);
    if (!value || !*value) {
        return fallback;
    }
    char *end = NULL;
    long v = strtol(value, &end, 10);
    if (!end || *end != '\0') {
        return fallback;
    }
    if (v < 0 || v > INT_MAX) {
        return fallback;
    }
    return (int)v;
}

// media 디렉터리 변경을 감지하기 위한 상태 구조체
typedef struct {
    pthread_t thread;
    int running;
    int stop;
    time_t last_mtime;
    int interval_sec;
    server_ctx_t *server;
} media_watch_state_t;

static media_watch_state_t g_media_watch = {0};

// 디렉터리의 mtime을 가져온다. 실패 시 0을 반환한다.
static time_t dir_mtime(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) {
        return 0;
    }
    return st.st_mtime;
}

// usleep 대신 짧게 여러 번 슬립해서 stop 플래그를 빠르게 반영한다.
static void sleep_with_stop(media_watch_state_t *state) {
    if (!state || state->interval_sec <= 0) return;
    int slices = state->interval_sec * 10; // 100ms x interval_sec
    for (int i = 0; i < slices && !state->stop; ++i) {
        usleep(100000);
    }
}

// media 디렉터리 변경 시 자동으로 DB와 동기화하는 백그라운드 루프
static void *media_watch_loop(void *arg) {
    media_watch_state_t *state = (media_watch_state_t *)arg;
    while (!state->stop) {
        time_t current = dir_mtime(state->server->media_dir);
        if (current > 0 && current != state->last_mtime) {
            if (sync_media_directory(state->server) == 0) {
                state->last_mtime = dir_mtime(state->server->media_dir);
                log_info("Media hot-reload: detected change, library synchronized");
            } else {
                log_warn("Media hot-reload: sync failed (will retry)");
            }
        }
        sleep_with_stop(state);
    }
    return NULL;
}

// 핫리로드 워처를 시작한다. 실패해도 서버는 계속 동작한다.
static int start_media_watcher(server_ctx_t *server) {
    if (!server) return -1;
    if (g_media_watch.running) return 0;
    g_media_watch.server = server;
    g_media_watch.interval_sec = getenv_int("MEDIA_WATCH_INTERVAL_SEC", 2);
    if (g_media_watch.interval_sec <= 0) {
        g_media_watch.interval_sec = 2;
    }
    g_media_watch.last_mtime = dir_mtime(server->media_dir);
    g_media_watch.stop = 0;
    if (pthread_create(&g_media_watch.thread, NULL, media_watch_loop, &g_media_watch) != 0) {
        log_warn("Media hot-reload watcher disabled (thread creation failed)");
        memset(&g_media_watch, 0, sizeof(g_media_watch));
        return -1;
    }
    g_media_watch.running = 1;
    return 0;
}

// 사용자별 재생 위치를 보관하는 맵 구조체
typedef struct {
    int video_id;
    double position;
} resume_entry_t;

typedef struct {
    resume_entry_t *items;
    size_t count;
    size_t capacity;
} resume_map_t;

// 디렉터리 스캔 시 파일명을 임시로 저장하는 동적 배열
typedef struct {
    char **items;
    size_t count;
    size_t capacity;
} filename_list_t;

static void resume_map_free(resume_map_t *map) {
    if (!map) return;
    free(map->items);
    map->items = NULL;
    map->count = 0;
    map->capacity = 0;
}

static void filename_list_free(filename_list_t *list) {
    if (!list) return;
    for (size_t i = 0; i < list->count; ++i) {
        free(list->items[i]);
    }
    free(list->items);
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

// realloc을 이용해 파일명 배열 크기를 자동 확장한다.
static int filename_list_add(filename_list_t *list, const char *name) {
    if (!list || !name) return -1;
    if (list->count == list->capacity) {
        size_t new_cap = list->capacity == 0 ? 8 : list->capacity * 2;
        char **new_items = realloc(list->items, new_cap * sizeof(char *));
        if (!new_items) {
            return -1;
        }
        list->items = new_items;
        list->capacity = new_cap;
    }
    size_t len = strlen(name);
    char *copy = malloc(len + 1);
    if (!copy) {
        return -1;
    }
    memcpy(copy, name, len + 1);
    list->items[list->count++] = copy;
    return 0;
}

// 비디오 ID별 마지막 재생 지점을 저장한다.
static int resume_map_add(resume_map_t *map, int video_id, double position) {
    if (!map) return -1;
    for (size_t i = 0; i < map->count; ++i) {
        if (map->items[i].video_id == video_id) {
            map->items[i].position = position;
            return 0;
        }
    }
    if (map->count == map->capacity) {
        size_t new_cap = map->capacity == 0 ? 8 : map->capacity * 2;
        resume_entry_t *new_items = realloc(map->items, new_cap * sizeof(resume_entry_t));
        if (!new_items) {
            return -1;
        }
        map->items = new_items;
        map->capacity = new_cap;
    }
    map->items[map->count].video_id = video_id;
    map->items[map->count].position = position;
    map->count++;
    return 0;
}

static const resume_entry_t *resume_map_find(const resume_map_t *map, int video_id) {
    if (!map) return NULL;
    for (size_t i = 0; i < map->count; ++i) {
        if (map->items[i].video_id == video_id) {
            return &map->items[i];
        }
    }
    return NULL;
}

// db_list_watch_history 콜백에서 resume_map을 채운다.
static int collect_resume(void *userdata, int video_id, double position_seconds, const char *updated_at) {
    (void)updated_at;
    resume_map_t *map = userdata;
    return resume_map_add(map, video_id, position_seconds);
}

static int has_mp4_extension(const char *name) {
    const char *ext = strrchr(name, '.');
    if (!ext) return 0;
    return strcasecmp(ext, ".mp4") == 0;
}

// 파일명에서 확장자와 언더스코어를 제거해 사람이 읽을 제목을 만든다.
static void make_title(const char *filename, char *title, size_t len) {
    if (!filename || !title || len == 0) return;
    const char *base = filename;
    const char *slash = strrchr(filename, '/');
    if (slash) base = slash + 1;
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s", base);
    char *dot = strrchr(tmp, '.');
    if (dot) *dot = '\0';
    for (char *p = tmp; *p; ++p) {
        if (*p == '_' || *p == '-') {
            *p = ' ';
        }
    }
    if (tmp[0] == '\0') {
        snprintf(title, len, "%s", filename);
    } else {
        snprintf(title, len, "%s", tmp);
    }
}

// media 디렉터리를 스캔하여 DB와 동기화한다.
static int sync_media_directory(server_ctx_t *server) {
    DIR *dir = opendir(server->media_dir);
    if (!dir) {
        log_warn("Failed to open media directory %s: %s", server->media_dir, strerror(errno));
        return -1;
    }
    filename_list_t files = {0};
    int result = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        if (!has_mp4_extension(ent->d_name)) continue;
        // 비디오 파일명에서 자동으로 제목을 만들어 DB에 upsert 한다.
        char title[256];
        make_title(ent->d_name, title, sizeof(title));
        if (db_upsert_video(&server->db, title, ent->d_name, NULL, 0, NULL) != 0) {
            log_warn("Failed to upsert video %s", ent->d_name);
            result = -1;
            break;
        }
        if (filename_list_add(&files, ent->d_name) != 0) {
            log_warn("Failed to track media filename %s", ent->d_name);
            result = -1;
            break;
        }
    }
    closedir(dir);
    if (result != 0) {
        log_error("Media synchronization aborted; see warnings above for details");
        filename_list_free(&files);
        return -1;
    }
    if (db_prune_missing_videos(&server->db, (const char *const *)files.items, files.count) != 0) {
        log_warn("Failed to prune missing videos; stale entries may remain");
    }
    filename_list_free(&files);
    return 0;
}

// 서버 시작 시 미디어 디렉터리를 한 번 동기화한다.
int video_initialize(server_ctx_t *server) {
    if (!server) return -1;
    int rc = sync_media_directory(server);
    if (rc != 0) {
        return rc;
    }
    if (start_media_watcher(server) != 0) {
        log_warn("Media hot-reload watcher is not running; new files will appear on next API call");
    }
    return 0;
}

typedef struct {
    request_ctx_t *ctx;
    string_builder_t *sb;
    int first;
    const resume_map_t *history;
    size_t emitted;
} list_ctx_t;

// 비디오 리스트 JSON 배열에 한 항목을 추가한다.
static int append_video_row(void *userdata, int id, const char *title,
                            const char *filename, const char *description,
                            int duration_seconds) {
    list_ctx_t *data = userdata;
    request_ctx_t *ctx = data->ctx;
    double resume = 0;
    int has_resume = 0;
    // history map에서 이 비디오에 대한 마지막 재생 지점을 찾는다.
    const resume_entry_t *entry = resume_map_find(data->history, id);
    if (entry) {
        resume = entry->position;
        has_resume = 1;
    }
    const char *prefix = data->first ? "" : ",";
    if (sb_append(data->sb, "%s{\"id\":%d,\"title\":", prefix, id) != 0) return -1;
    if (sb_append_json_string(data->sb, title) != 0) return -1;
    if (sb_append(data->sb, ",\"filename\":") != 0) return -1;
    if (sb_append_json_string(data->sb, filename) != 0) return -1;
    if (sb_append(data->sb, ",\"description\":") != 0) return -1;
    if (sb_append_json_string(data->sb, description) != 0) return -1;
    if (sb_append(data->sb, ",\"duration\":%d", duration_seconds) != 0) return -1;
    char thumb_url[128];
    snprintf(thumb_url, sizeof(thumb_url), "/api/videos/%d/thumbnail", id);
    if (sb_append(data->sb, ",\"thumbnailUrl\":") != 0) return -1;
    if (sb_append_json_string(data->sb, thumb_url) != 0) return -1;
    char stream_url[128];
    snprintf(stream_url, sizeof(stream_url), "/api/videos/%d/stream", id);
    if (sb_append(data->sb, ",\"streamUrl\":") != 0) return -1;
    if (sb_append_json_string(data->sb, stream_url) != 0) return -1;
    if (sb_append(data->sb, ",\"resumeSeconds\":%.3f", has_resume ? resume : 0.0) != 0) return -1;
    if (sb_append(data->sb, "}") != 0) return -1;
    data->first = 0;
    data->emitted++;
    return 0;
}

// /api/videos: 페이지네이션, 검색, 재생 위치를 포함한 목록을 반환한다.
void video_handle_list(request_ctx_t *ctx) {
    if (!ctx->authenticated) {
        router_send_json_error(ctx, 401, "Unauthorized");
        return;
    }
    sync_media_directory(ctx->server);
    int limit = VIDEO_DEFAULT_LIMIT;
    int cursor = 0;
    if (query_get_int(ctx->request->query, "limit", &limit) == 0) {
        if (limit < 1) limit = VIDEO_DEFAULT_LIMIT;
        if (limit > VIDEO_MAX_LIMIT) limit = VIDEO_MAX_LIMIT;
    } else {
        limit = VIDEO_DEFAULT_LIMIT;
    }
    if (query_get_int(ctx->request->query, "cursor", &cursor) == 0) {
        if (cursor < 0) cursor = 0;
    } else {
        cursor = 0;
    }
    char search_term[128] = {0};
    int has_query = 0;
    if (query_get_param(ctx->request->query, "q", search_term, sizeof(search_term)) == 0) {
        trim_spaces(search_term);
        if (search_term[0] != '\0') {
            has_query = 1;
        }
    }
    // 사용자별 시청 기록을 읽어와 resume_map에 채워 넣는다.
    resume_map_t map = {0};
    if (db_list_watch_history(&ctx->server->db, ctx->user_id, collect_resume, &map) != 0) {
        resume_map_free(&map);
        router_send_json_error(ctx, 500, "Failed to load history");
        return;
    }
    string_builder_t sb;
    if (sb_init(&sb, 512) != 0) {
        resume_map_free(&map);
        router_send_json_error(ctx, 500, "Allocation failed");
        return;
    }
    if (sb_append(&sb, "{\"videos\":[") != 0) {
        sb_free(&sb);
        router_send_json_error(ctx, 500, "Allocation failed");
        return;
    }
    list_ctx_t data = {.ctx = ctx, .sb = &sb, .first = 1, .history = &map, .emitted = 0};
    int has_more = 0;
    if (db_query_videos(&ctx->server->db, has_query ? search_term : NULL, limit, cursor,
                        append_video_row, &data, &has_more) != 0) {
        sb_free(&sb);
        resume_map_free(&map);
        router_send_json_error(ctx, 500, "Failed to query videos");
        return;
    }
    // 다음 페이지 커서 및 쿼리 문자열을 응답 본문에 포함시킨다.
    int next_cursor = cursor + (int)data.emitted;
    if (sb_append(&sb, "],\"cursor\":%d,\"limit\":%d,\"nextCursor\":%d,\"hasMore\":%s,\"query\":",
                  cursor, limit, next_cursor, has_more ? "true" : "false") != 0) {
        sb_free(&sb);
        resume_map_free(&map);
        router_send_json_error(ctx, 500, "Allocation failed");
        return;
    }
    if (has_query) {
        if (sb_append_json_string(&sb, search_term) != 0) {
            sb_free(&sb);
            resume_map_free(&map);
            router_send_json_error(ctx, 500, "Allocation failed");
            return;
        }
    } else {
        if (sb_append(&sb, "null") != 0) {
            sb_free(&sb);
            resume_map_free(&map);
            router_send_json_error(ctx, 500, "Allocation failed");
            return;
        }
    }
    if (sb_append(&sb, "}") != 0) {
        sb_free(&sb);
        resume_map_free(&map);
        router_send_json_error(ctx, 500, "Allocation failed");
        return;
    }
    router_send_json(ctx, 200, sb.data, NULL);
    sb_free(&sb);
    resume_map_free(&map);
}

static int parse_int(const char *s) {
    if (!s) return -1;
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (!end || *end != '\0') {
        return -1;
    }
    if (v < 0 || v > INT_MAX) {
        return -1;
    }
    return (int)v;
}

// HTTP Range 헤더를 파싱해 시작/끝 바이트를 계산한다.
static int parse_range_header(const char *header, off_t file_size, off_t *start_out, off_t *end_out) {
    if (!header || !start_out || !end_out) return -1;
    if (strncmp(header, "bytes=", 6) != 0) {
        return -1;
    }
    const char *range = header + 6;
    const char *dash = strchr(range, '-');
    if (!dash) {
        return -1;
    }
    off_t start = 0;
    off_t end = file_size - 1;
    if (dash == range) {
        // suffix range
        off_t suffix = strtoll(dash + 1, NULL, 10);
        if (suffix <= 0) return -1;
        if (suffix > file_size) suffix = file_size;
        start = file_size - suffix;
    } else {
        start = strtoll(range, NULL, 10);
        if (start < 0 || start >= file_size) return -1;
        if (*(dash + 1)) {
            end = strtoll(dash + 1, NULL, 10);
            if (end < start) return -1;
            if (end >= file_size) end = file_size - 1;
        }
    }
    *start_out = start;
    *end_out = end;
    return 0;
}

// /api/videos/:id/stream: MP4 파일을 Range 스트리밍한다.
void video_handle_stream(request_ctx_t *ctx) {
    if (!ctx->authenticated) {
        router_send_json_error(ctx, 401, "Unauthorized");
        return;
    }
    const char *id_str = router_get_param(ctx, "id");
    int video_id = parse_int(id_str);
    if (video_id <= 0) {
        router_send_json_error(ctx, 400, "Invalid video id");
        return;
    }
    char filename[256];
    if (db_get_video_by_id(&ctx->server->db, video_id, NULL, 0, filename, sizeof(filename), NULL, 0, NULL) != 0) {
        router_send_json_error(ctx, 404, "Video not found");
        return;
    }
    char path[PATH_MAX];
    if (snprintf(path, sizeof(path), "%s/%s", ctx->server->media_dir, filename) >= (int)sizeof(path)) {
        router_send_json_error(ctx, 500, "Path too long");
        return;
    }
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
        router_send_json_error(ctx, 404, "Video not found");
        return;
    }
    off_t file_size = st.st_size;
    const char *range = http_get_header(ctx->request, "Range");
    char headers[512];
    if (range) {
        off_t start = 0, end = 0;
        if (parse_range_header(range, file_size, &start, &end) != 0) {
            router_send_json_error(ctx, 416, "Invalid range");
            return;
        }
        size_t length = (size_t)(end - start + 1);
        char extra[256];
        snprintf(extra, sizeof(extra),
                 "Accept-Ranges: bytes\r\nContent-Range: bytes %lld-%lld/%lld\r\n",
                 (long long)start, (long long)end, (long long)file_size);
        build_header(headers, sizeof(headers), ctx->server, extra);
        if (http_send_file_response(ctx->client_fd, 206, http_status_text(206), "video/mp4",
                                    path, start, length, 1, headers) != 0) {
            log_warn("Failed to stream range for video %d", video_id);
        }
    } else {
        build_header(headers, sizeof(headers), ctx->server, "Accept-Ranges: bytes\r\n");
        if (http_send_file_response(ctx->client_fd, 200, http_status_text(200), "video/mp4",
                                    path, 0, 0, 1, headers) != 0) {
            log_warn("Failed to stream video %d", video_id);
        }
    }
}

// /api/videos/:id/thumbnail: 캐시된 썸네일 이미지를 내려준다.
void video_handle_thumbnail(request_ctx_t *ctx) {
    if (!ctx->authenticated) {
        router_send_json_error(ctx, 401, "Unauthorized");
        return;
    }
    const char *id_str = router_get_param(ctx, "id");
    int video_id = parse_int(id_str);
    if (video_id <= 0) {
        router_send_json_error(ctx, 400, "Invalid video id");
        return;
    }
    char filename[256];
    if (db_get_video_by_id(&ctx->server->db, video_id, NULL, 0, filename, sizeof(filename), NULL, 0, NULL) != 0) {
        router_send_json_error(ctx, 404, "Video not found");
        return;
    }
    char video_path[PATH_MAX];
    if (snprintf(video_path, sizeof(video_path), "%s/%s", ctx->server->media_dir, filename) >= (int)sizeof(video_path)) {
        router_send_json_error(ctx, 500, "Path too long");
        return;
    }
    char thumb_path[PATH_MAX];
    if (ffmpeg_ensure_thumbnail(ctx->server, video_id, video_path, thumb_path, sizeof(thumb_path)) != 0) {
        router_send_json_error(ctx, 500, "Thumbnail error");
        return;
    }
    char headers[256];
    build_header(headers, sizeof(headers), ctx->server, NULL);
    if (http_send_file_response(ctx->client_fd, 200, http_status_text(200), "image/jpeg",
                                thumb_path, 0, 0, 0, headers) != 0) {
        log_warn("Failed to send thumbnail for video %d", video_id);
    }
}

// 서버 종료 시 백그라운드 워처 스레드를 정리한다.
void video_shutdown(void) {
    if (!g_media_watch.running) {
        return;
    }
    g_media_watch.stop = 1;
    pthread_join(g_media_watch.thread, NULL);
    memset(&g_media_watch, 0, sizeof(g_media_watch));
}
