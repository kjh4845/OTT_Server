#include "history.h"

#include <limits.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "db.h"
#include "utils.h"

void history_handle_get(request_ctx_t *ctx) {
    if (!ctx->authenticated) {
        router_send_json_error(ctx, 401, "Unauthorized");
        return;
    }
    string_builder_t sb;
    if (sb_init(&sb, 512) != 0) {
        router_send_json_error(ctx, 500, "Allocation failed");
        return;
    }
    if (sb_append(&sb, "{\"history\":[") != 0) {
        sb_free(&sb);
        router_send_json_error(ctx, 500, "Allocation failed");
        return;
    }
    db_ctx_t *db = &ctx->server->db;
    const char *sql =
        "SELECT w.video_id, w.position_seconds, IFNULL(w.updated_at,''), IFNULL(v.title,'') "
        "FROM watch_history w JOIN videos v ON v.id = w.video_id "
        "WHERE w.user_id = ? ORDER BY w.updated_at DESC";
    pthread_mutex_lock(&db->mutex);
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db->conn, sql, -1, &stmt, NULL) != SQLITE_OK) {
        pthread_mutex_unlock(&db->mutex);
        sb_free(&sb);
        router_send_json_error(ctx, 500, "Failed to prepare history query");
        return;
    }
    sqlite3_bind_int(stmt, 1, ctx->user_id);
    int first = 1;
    int rc;
    int error = 0;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        int video_id = sqlite3_column_int(stmt, 0);
        double position = sqlite3_column_double(stmt, 1);
        const char *updated_at = (const char *)sqlite3_column_text(stmt, 2);
        const char *title = (const char *)sqlite3_column_text(stmt, 3);
        const char *prefix = first ? "" : ",";
        if (sb_append(&sb, "%s{\"videoId\":%d,\"position\":%.3f,\"updatedAt\":", prefix, video_id, position) != 0) {
            error = 1;
            break;
        }
        if (sb_append_json_string(&sb, updated_at ? updated_at : "") != 0) {
            error = 1;
            break;
        }
        if (sb_append(&sb, ",\"title\":") != 0) {
            error = 1;
            break;
        }
        if (sb_append_json_string(&sb, title ? title : "") != 0) {
            error = 1;
            break;
        }
        char thumb_url[128];
        snprintf(thumb_url, sizeof(thumb_url), "/api/videos/%d/thumbnail", video_id);
        if (sb_append(&sb, ",\"thumbnailUrl\":") != 0) {
            error = 1;
            break;
        }
        if (sb_append_json_string(&sb, thumb_url) != 0) {
            error = 1;
            break;
        }
        char stream_url[128];
        snprintf(stream_url, sizeof(stream_url), "/api/videos/%d/stream", video_id);
        if (sb_append(&sb, ",\"streamUrl\":") != 0) {
            error = 1;
            break;
        }
        if (sb_append_json_string(&sb, stream_url) != 0) {
            error = 1;
            break;
        }
        if (sb_append(&sb, "}") != 0) {
            error = 1;
            break;
        }
        first = 0;
    }
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&db->mutex);
    if (error || (rc != SQLITE_DONE && rc != SQLITE_ROW)) {
        sb_free(&sb);
        router_send_json_error(ctx, 500, "Failed to read history");
        return;
    }
    if (sb_append(&sb, "]}") != 0) {
        sb_free(&sb);
        router_send_json_error(ctx, 500, "Allocation failed");
        return;
    }
    router_send_json(ctx, 200, sb.data, NULL);
    sb_free(&sb);
}

static int parse_int(const char *s) {
    if (!s) return -1;
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (!end || *end != '\0' || v < 0 || v > INT_MAX) return -1;
    return (int)v;
}

void history_handle_update(request_ctx_t *ctx) {
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
    (void)filename;
    if (!ctx->request->body || ctx->request->body_length == 0) {
        router_send_json_error(ctx, 400, "Missing payload");
        return;
    }
    double position = 0.0;
    if (json_get_double(ctx->request->body, "position", &position) != 0 || position < 0) {
        router_send_json_error(ctx, 400, "Invalid position");
        return;
    }
    if (db_update_watch_history(&ctx->server->db, ctx->user_id, video_id, position) != 0) {
        router_send_json_error(ctx, 500, "Failed to update history");
        return;
    }
    router_send_json(ctx, 200, "{\"status\":\"ok\"}", NULL);
}
