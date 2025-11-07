#include "video.h"

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "db.h"
#include "ffmpeg.h"
#include "http.h"
#include "utils.h"

static void build_header(char *dest, size_t len, server_ctx_t *server, const char *extra) {
    if (!dest || len == 0) return;
    if (extra && *extra) {
        snprintf(dest, len, "%s%s", server->security_headers, extra);
    } else {
        snprintf(dest, len, "%s", server->security_headers);
    }
}

typedef struct {
    int video_id;
    double position;
} resume_entry_t;

typedef struct {
    resume_entry_t *items;
    size_t count;
    size_t capacity;
} resume_map_t;

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

int video_initialize(server_ctx_t *server) {
    if (!server) return -1;
    return sync_media_directory(server);
}

typedef struct {
    request_ctx_t *ctx;
    string_builder_t *sb;
    int first;
    const resume_map_t *history;
} list_ctx_t;

static int append_video_row(void *userdata, int id, const char *title,
                            const char *filename, const char *description,
                            int duration_seconds) {
    list_ctx_t *data = userdata;
    request_ctx_t *ctx = data->ctx;
    double resume = 0;
    int has_resume = 0;
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
    return 0;
}

void video_handle_list(request_ctx_t *ctx) {
    if (!ctx->authenticated) {
        router_send_json_error(ctx, 401, "Unauthorized");
        return;
    }
    sync_media_directory(ctx->server);
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
    list_ctx_t data = {.ctx = ctx, .sb = &sb, .first = 1, .history = &map};
    if (db_list_videos(&ctx->server->db, append_video_row, &data) != 0) {
        sb_free(&sb);
        resume_map_free(&map);
        router_send_json_error(ctx, 500, "Failed to query videos");
        return;
    }
    if (sb_append(&sb, "]}") != 0) {
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
