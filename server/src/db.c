// SQLite를 감싸는 헬퍼 계층: 스레드 안전 보장을 위해 모든 쿼리를 직렬화한다.
#include "db.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "utils.h"

// 모든 DB 접근 전에 뮤텍스를 잡아 SQLite 연결을 직렬화한다.
static void db_lock(db_ctx_t *db) {
    pthread_mutex_lock(&db->mutex);
}

static void db_unlock(db_ctx_t *db) {
    pthread_mutex_unlock(&db->mutex);
}

// 최근 SQLite 에러 메시지를 안전하게 가져온다.
const char *db_errmsg(db_ctx_t *db) {
    if (!db || !db->conn) {
        return "db not initialized";
    }
    return sqlite3_errmsg(db->conn);
}

// DB 파일을 열고 외래키/타임아웃 등의 기본 설정을 적용한다.
int db_init(db_ctx_t *db, const char *path) {
    if (!db || !path) {
        return -1;
    }
    memset(db, 0, sizeof(*db));
    if (pthread_mutex_init(&db->mutex, NULL) != 0) {
        return -1;
    }

    int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX;
    if (sqlite3_open_v2(path, &db->conn, flags, NULL) != SQLITE_OK) {
        pthread_mutex_destroy(&db->mutex);
        return -1;
    }
    sqlite3_busy_timeout(db->conn, 5000);
    if (sqlite3_exec(db->conn, "PRAGMA foreign_keys = ON", NULL, NULL, NULL) != SQLITE_OK) {
        sqlite3_close(db->conn);
        db->conn = NULL;
        pthread_mutex_destroy(&db->mutex);
        return -1;
    }
    return 0;
}

// 안전한 종료: 핸들을 닫고 뮤텍스를 파괴한다.
void db_close(db_ctx_t *db) {
    if (!db) return;
    if (db->conn) {
        sqlite3_close(db->conn);
        db->conn = NULL;
    }
    pthread_mutex_destroy(&db->mutex);
}

// schema.sql을 통째로 실행해서 테이블을 준비한다.
int db_run_schema(db_ctx_t *db, const char *schema_path) {
    if (!db || !schema_path) {
        return -1;
    }
    size_t len = 0;
    char *sql = read_file(schema_path, &len);
    if (!sql) {
        return -1;
    }
    char *errmsg = NULL;
    db_lock(db);
    int rc = sqlite3_exec(db->conn, sql, NULL, NULL, &errmsg);
    db_unlock(db);
    free(sql);
    if (rc != SQLITE_OK) {
        if (errmsg) {
            log_error("Schema error: %s", errmsg);
            sqlite3_free(errmsg);
        }
        return -1;
    }
    return 0;
}

// sqlite3_column_blob의 내용을 고정 길이 버퍼로 복사한다.
static int copy_blob(sqlite3_stmt *stmt, int col, unsigned char *dest, size_t dest_len) {
    const void *blob = sqlite3_column_blob(stmt, col);
    int bytes = sqlite3_column_bytes(stmt, col);
    if (!blob || bytes <= 0 || (size_t)bytes > dest_len) {
        return -1;
    }
    memcpy(dest, blob, (size_t)bytes);
    return bytes;
}

// username으로 사용자 ID/해시/솔트를 읽어 온다.
int db_get_user_credentials(db_ctx_t *db, const char *username, int *user_id,
                            unsigned char *hash_out, size_t hash_len,
                            unsigned char *salt_out, size_t salt_len) {
    if (!db || !username || !user_id || !hash_out || !salt_out) {
        return -1;
    }
    const char *sql = "SELECT id, password_hash, salt FROM users WHERE username = ?";
    db_lock(db);
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db->conn, sql, -1, &stmt, NULL) != SQLITE_OK) {
        db_unlock(db);
        return -1;
    }
    sqlite3_bind_text(stmt, 1, username, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    int result = -1;
    if (rc == SQLITE_ROW) {
        *user_id = sqlite3_column_int(stmt, 0);
        if (copy_blob(stmt, 1, hash_out, hash_len) > 0 &&
            copy_blob(stmt, 2, salt_out, salt_len) > 0) {
            result = 0;
        }
    } else {
        result = -1;
    }
    sqlite3_finalize(stmt);
    db_unlock(db);
    return result;
}

// username이 이미 있으면 업데이트하고 없으면 삽입한다.
int db_upsert_user(db_ctx_t *db, const char *username,
                   const unsigned char *hash, size_t hash_len,
                   const unsigned char *salt, size_t salt_len) {
    if (!db || !username || !hash || !salt) {
        return -1;
    }
    const char *sql =
        "INSERT INTO users(username, password_hash, salt) VALUES(?, ?, ?) \n"
        "ON CONFLICT(username) DO UPDATE SET password_hash=excluded.password_hash, salt=excluded.salt";
    db_lock(db);
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db->conn, sql, -1, &stmt, NULL) != SQLITE_OK) {
        db_unlock(db);
        return -1;
    }
    sqlite3_bind_text(stmt, 1, username, -1, SQLITE_TRANSIENT);
    sqlite3_bind_blob(stmt, 2, hash, (int)hash_len, SQLITE_TRANSIENT);
    sqlite3_bind_blob(stmt, 3, salt, (int)salt_len, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    db_unlock(db);
    return rc == SQLITE_DONE ? 0 : -1;
}

// 신규 가입 시 users 테이블에 추가하고 rowid를 돌려준다.
int db_create_user(db_ctx_t *db, const char *username,
                   const unsigned char *hash, size_t hash_len,
                   const unsigned char *salt, size_t salt_len,
                   int *user_id_out) {
    if (!db || !username || !hash || !salt) {
        return -1;
    }
    const char *sql = "INSERT INTO users(username, password_hash, salt) VALUES(?, ?, ?)";
    db_lock(db);
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db->conn, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        db_unlock(db);
        return -1;
    }
    sqlite3_bind_text(stmt, 1, username, -1, SQLITE_TRANSIENT);
    sqlite3_bind_blob(stmt, 2, hash, (int)hash_len, SQLITE_TRANSIENT);
    sqlite3_bind_blob(stmt, 3, salt, (int)salt_len, SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt);
    if (rc == SQLITE_DONE && user_id_out) {
        *user_id_out = (int)sqlite3_last_insert_rowid(db->conn);
    }
    sqlite3_finalize(stmt);
    db_unlock(db);
    if (rc == SQLITE_DONE) {
        return 0;
    }
    if (rc == SQLITE_CONSTRAINT) {
        return SQLITE_CONSTRAINT;
    }
    return -1;
}

// 세션 토큰을 upsert 해서 중복 로그인도 안전하게 갱신한다.
int db_create_session(db_ctx_t *db, const char *token, int user_id, time_t expires_at) {
    if (!db || !token) return -1;
    const char *sql =
        "INSERT INTO sessions(token, user_id, expires_at) VALUES(?, ?, ?) \n"
        "ON CONFLICT(token) DO UPDATE SET user_id=excluded.user_id, expires_at=excluded.expires_at";
    db_lock(db);
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db->conn, sql, -1, &stmt, NULL) != SQLITE_OK) {
        db_unlock(db);
        return -1;
    }
    sqlite3_bind_text(stmt, 1, token, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, user_id);
    sqlite3_bind_int64(stmt, 3, (sqlite3_int64)expires_at);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    db_unlock(db);
    return rc == SQLITE_DONE ? 0 : -1;
}

// 토큰으로 세션을 찾고 만료 시각을 돌려준다.
int db_get_session(db_ctx_t *db, const char *token, int *user_id, time_t *expires_at) {
    if (!db || !token || !user_id || !expires_at) {
        return -1;
    }
    const char *sql = "SELECT user_id, expires_at FROM sessions WHERE token = ?";
    db_lock(db);
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db->conn, sql, -1, &stmt, NULL) != SQLITE_OK) {
        db_unlock(db);
        return -1;
    }
    sqlite3_bind_text(stmt, 1, token, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    int result = -1;
    if (rc == SQLITE_ROW) {
        *user_id = sqlite3_column_int(stmt, 0);
        *expires_at = (time_t)sqlite3_column_int64(stmt, 1);
        result = 0;
    }
    sqlite3_finalize(stmt);
    db_unlock(db);
    return result;
}

// 단일 세션을 삭제한다.
int db_delete_session(db_ctx_t *db, const char *token) {
    if (!db || !token) return -1;
    const char *sql = "DELETE FROM sessions WHERE token = ?";
    db_lock(db);
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db->conn, sql, -1, &stmt, NULL) != SQLITE_OK) {
        db_unlock(db);
        return -1;
    }
    sqlite3_bind_text(stmt, 1, token, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    db_unlock(db);
    return rc == SQLITE_DONE ? 0 : -1;
}

// 만료 시각이 기준보다 과거인 세션을 일괄 삭제한다.
int db_purge_expired_sessions(db_ctx_t *db, time_t now) {
    if (!db) return -1;
    const char *sql = "DELETE FROM sessions WHERE expires_at < ?";
    db_lock(db);
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db->conn, sql, -1, &stmt, NULL) != SQLITE_OK) {
        db_unlock(db);
        return -1;
    }
    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)now);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    db_unlock(db);
    return 0;
}

// 모든 비디오 메타데이터를 순회하며 콜백에 전달한다.
int db_list_videos(db_ctx_t *db,
                   int (*callback)(void *userdata, int id, const char *title,
                                   const char *filename, const char *description,
                                   int duration_seconds),
                   void *userdata) {
    if (!db || !callback) {
        return -1;
    }
    const char *sql =
        "SELECT id, title, filename, IFNULL(description, ''), duration_seconds FROM videos ORDER BY id";
    db_lock(db);
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db->conn, sql, -1, &stmt, NULL) != SQLITE_OK) {
        db_unlock(db);
        return -1;
    }
    int rc;
    int result = 0;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        int id = sqlite3_column_int(stmt, 0);
        const char *title = (const char *)sqlite3_column_text(stmt, 1);
        const char *filename = (const char *)sqlite3_column_text(stmt, 2);
        const char *description = (const char *)sqlite3_column_text(stmt, 3);
        int duration = sqlite3_column_int(stmt, 4);
        if (callback(userdata, id, title ? title : "", filename ? filename : "",
                     description ? description : "", duration) != 0) {
            result = 1;
            break;
        }
    }
    sqlite3_finalize(stmt);
    db_unlock(db);
    return (rc == SQLITE_DONE || rc == SQLITE_ROW) ? result : -1;
}

// 페이지네이션과 검색어 필터를 지원하는 비디오 조회 함수
int db_query_videos(db_ctx_t *db, const char *search_term, int limit, int offset,
                    int (*callback)(void *userdata, int id, const char *title,
                                    const char *filename, const char *description,
                                    int duration_seconds),
                    void *userdata, int *has_more_out) {
    if (!db || !callback || limit <= 0 || offset < 0) {
        return -1;
    }
    const char *base_sql =
        "SELECT id, title, filename, IFNULL(description, ''), duration_seconds FROM videos ORDER BY id LIMIT ? OFFSET ?";
    const char *search_sql =
        "SELECT id, title, filename, IFNULL(description, ''), duration_seconds FROM videos "
        "WHERE title LIKE ? OR filename LIKE ? OR IFNULL(description, '') LIKE ? "
        "ORDER BY id LIMIT ? OFFSET ?";
    const char *sql = base_sql;
    char pattern[256];
    const char *like_term = NULL;
    if (search_term && *search_term) {
        // 부분 일치를 위해 %검색어% 패턴을 만든다.
        size_t len = strnlen(search_term, sizeof(pattern) - 3);
        pattern[0] = '%';
        memcpy(pattern + 1, search_term, len);
        pattern[len + 1] = '%';
        pattern[len + 2] = '\0';
        like_term = pattern;
        sql = search_sql;
    }
    // 다음 페이지 존재 여부를 판단하기 위해 1건 더 가져온다.
    int limit_with_extra = limit + 1;
    db_lock(db);
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db->conn, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        db_unlock(db);
        return -1;
    }
    int param = 1;
    if (like_term) {
        sqlite3_bind_text(stmt, param++, like_term, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, param++, like_term, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, param++, like_term, -1, SQLITE_TRANSIENT);
    }
    sqlite3_bind_int(stmt, param++, limit_with_extra);
    sqlite3_bind_int(stmt, param++, offset);
    int emitted = 0;
    int total_rows = 0;
    int callback_error = 0;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        if (total_rows < limit) {
            int id = sqlite3_column_int(stmt, 0);
            const char *title = (const char *)sqlite3_column_text(stmt, 1);
            const char *filename = (const char *)sqlite3_column_text(stmt, 2);
            const char *description = (const char *)sqlite3_column_text(stmt, 3);
            int duration = sqlite3_column_int(stmt, 4);
            if (callback(userdata, id, title ? title : "", filename ? filename : "",
                         description ? description : "", duration) != 0) {
                callback_error = 1;
                break;
            }
            emitted++;
        }
        total_rows++;
    }
    if (has_more_out) {
        *has_more_out = total_rows > limit ? 1 : 0;
    }
    sqlite3_finalize(stmt);
    db_unlock(db);
    if (callback_error) {
        return -1;
    }
    if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
        return -1;
    }
    (void)emitted;
    return 0;
}

// 단일 ID의 비디오 레코드를 꺼내 필요한 필드를 채운다.
int db_get_video_by_id(db_ctx_t *db, int video_id,
                       char *title_out, size_t title_len,
                       char *filename_out, size_t filename_len,
                       char *description_out, size_t desc_len,
                       int *duration_seconds_out) {
    if (!db || !filename_out) {
        return -1;
    }
    const char *sql =
        "SELECT title, filename, IFNULL(description,''), duration_seconds FROM videos WHERE id = ?";
    db_lock(db);
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db->conn, sql, -1, &stmt, NULL) != SQLITE_OK) {
        db_unlock(db);
        return -1;
    }
    sqlite3_bind_int(stmt, 1, video_id);
    int rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        db_unlock(db);
        return -1;
    }
    const char *title = (const char *)sqlite3_column_text(stmt, 0);
    const char *filename = (const char *)sqlite3_column_text(stmt, 1);
    const char *description = (const char *)sqlite3_column_text(stmt, 2);
    if (title_out && title_len > 0 && title) {
        snprintf(title_out, title_len, "%s", title);
    }
    if (filename_out && filename_len > 0 && filename) {
        snprintf(filename_out, filename_len, "%s", filename);
    }
    if (description_out && desc_len > 0 && description) {
        snprintf(description_out, desc_len, "%s", description);
    }
    if (duration_seconds_out) {
        *duration_seconds_out = sqlite3_column_int(stmt, 3);
    }
    sqlite3_finalize(stmt);
    db_unlock(db);
    return 0;
}

// 실제 파일명을 기준으로 videos 테이블을 동기화한다.
int db_upsert_video(db_ctx_t *db, const char *title, const char *filename,
                    const char *description, int duration_seconds, int *video_id_out) {
    if (!db || !filename) {
        return -1;
    }
    const char *sql =
        "INSERT INTO videos(title, filename, description, duration_seconds) VALUES(?, ?, ?, ?) \n"
        "ON CONFLICT(filename) DO UPDATE SET title=excluded.title, description=excluded.description, duration_seconds=excluded.duration_seconds RETURNING id";
    db_lock(db);
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db->conn, sql, -1, &stmt, NULL) != SQLITE_OK) {
        db_unlock(db);
        return -1;
    }
    sqlite3_bind_text(stmt, 1, title ? title : filename, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, filename, -1, SQLITE_TRANSIENT);
    if (description) {
        sqlite3_bind_text(stmt, 3, description, -1, SQLITE_TRANSIENT);
    } else {
        sqlite3_bind_null(stmt, 3);
    }
    sqlite3_bind_int(stmt, 4, duration_seconds);
    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW && video_id_out) {
        *video_id_out = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    db_unlock(db);
    return (rc == SQLITE_ROW || rc == SQLITE_DONE) ? 0 : -1;
}

// 파일명이 제거된 비디오 레코드를 삭제한다.
int db_delete_video_by_filename(db_ctx_t *db, const char *filename) {
    if (!db || !filename) {
        return -1;
    }
    const char *sql = "DELETE FROM videos WHERE filename = ?";
    db_lock(db);
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db->conn, sql, -1, &stmt, NULL) != SQLITE_OK) {
        db_unlock(db);
        return -1;
    }
    sqlite3_bind_text(stmt, 1, filename, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    db_unlock(db);
    return rc == SQLITE_DONE ? 0 : -1;
}

// 실제 디렉터리에 없는 항목을 videos 테이블에서 삭제한다.
int db_prune_missing_videos(db_ctx_t *db, const char *const *filenames, size_t count) {
    if (!db) return -1;
    db_lock(db);
    int rc = -1;
    char *errmsg = NULL;
    sqlite3_stmt *insert_stmt = NULL;
    const char *create_sql = "CREATE TEMP TABLE IF NOT EXISTS temp_existing(filename TEXT PRIMARY KEY)";
    if (sqlite3_exec(db->conn, create_sql, NULL, NULL, &errmsg) != SQLITE_OK) {
        goto done;
    }
    if (errmsg) {
        sqlite3_free(errmsg);
        errmsg = NULL;
    }
    if (sqlite3_exec(db->conn, "DELETE FROM temp_existing", NULL, NULL, &errmsg) != SQLITE_OK) {
        goto done;
    }
    if (errmsg) {
        sqlite3_free(errmsg);
        errmsg = NULL;
    }
    if (sqlite3_prepare_v2(db->conn,
                           "INSERT OR IGNORE INTO temp_existing(filename) VALUES(?)",
                           -1, &insert_stmt, NULL) != SQLITE_OK) {
        goto done;
    }
    for (size_t i = 0; i < count; ++i) {
        const char *name = filenames ? filenames[i] : NULL;
        if (!name || !*name) continue;
        sqlite3_bind_text(insert_stmt, 1, name, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(insert_stmt) != SQLITE_DONE) {
            goto done;
        }
        sqlite3_reset(insert_stmt);
        sqlite3_clear_bindings(insert_stmt);
    }
    sqlite3_finalize(insert_stmt);
    insert_stmt = NULL;
    const char *delete_sql =
        "DELETE FROM videos WHERE filename NOT IN (SELECT filename FROM temp_existing)";
    if (sqlite3_exec(db->conn, delete_sql, NULL, NULL, &errmsg) != SQLITE_OK) {
        goto done;
    }
    rc = 0;
done:
    if (insert_stmt) {
        sqlite3_finalize(insert_stmt);
    }
    sqlite3_exec(db->conn, "DELETE FROM temp_existing", NULL, NULL, NULL);
    if (errmsg) {
        sqlite3_free(errmsg);
    }
    db_unlock(db);
    return rc;
}

// 시청 위치를 upsert하여 마지막 재생 위치를 기록한다.
int db_update_watch_history(db_ctx_t *db, int user_id, int video_id, double position_seconds) {
    if (!db) return -1;
    const char *sql =
        "INSERT INTO watch_history(user_id, video_id, position_seconds, updated_at) VALUES(?, ?, ?, CURRENT_TIMESTAMP) \n"
        "ON CONFLICT(user_id, video_id) DO UPDATE SET position_seconds=excluded.position_seconds, updated_at=CURRENT_TIMESTAMP";
    db_lock(db);
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db->conn, sql, -1, &stmt, NULL) != SQLITE_OK) {
        db_unlock(db);
        return -1;
    }
    sqlite3_bind_int(stmt, 1, user_id);
    sqlite3_bind_int(stmt, 2, video_id);
    sqlite3_bind_double(stmt, 3, position_seconds);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    db_unlock(db);
    return rc == SQLITE_DONE ? 0 : -1;
}

// 특정 사용자/비디오에 대한 마지막 위치를 조회한다.
int db_get_watch_history(db_ctx_t *db, int user_id, int video_id, double *position_seconds_out) {
    if (!db || !position_seconds_out) {
        return -1;
    }
    const char *sql = "SELECT position_seconds FROM watch_history WHERE user_id = ? AND video_id = ?";
    db_lock(db);
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db->conn, sql, -1, &stmt, NULL) != SQLITE_OK) {
        db_unlock(db);
        return -1;
    }
    sqlite3_bind_int(stmt, 1, user_id);
    sqlite3_bind_int(stmt, 2, video_id);
    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        *position_seconds_out = sqlite3_column_double(stmt, 0);
        sqlite3_finalize(stmt);
        db_unlock(db);
        return 0;
    }
    sqlite3_finalize(stmt);
    db_unlock(db);
    return -1;
}

// 사용자의 watch_history 목록을 최신순으로 스트리밍한다.
int db_list_watch_history(db_ctx_t *db, int user_id,
                          int (*callback)(void *userdata, int video_id, double position_seconds,
                                          const char *updated_at),
                          void *userdata) {
    if (!db || !callback) {
        return -1;
    }
    const char *sql =
        "SELECT video_id, position_seconds, updated_at FROM watch_history WHERE user_id = ? ORDER BY updated_at DESC";
    db_lock(db);
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db->conn, sql, -1, &stmt, NULL) != SQLITE_OK) {
        db_unlock(db);
        return -1;
    }
    sqlite3_bind_int(stmt, 1, user_id);
    int rc;
    int result = 0;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        int video_id = sqlite3_column_int(stmt, 0);
        double position = sqlite3_column_double(stmt, 1);
        const char *updated_at = (const char *)sqlite3_column_text(stmt, 2);
        if (callback(userdata, video_id, position, updated_at ? updated_at : "") != 0) {
            result = 1;
            break;
        }
    }
    sqlite3_finalize(stmt);
    db_unlock(db);
    return (rc == SQLITE_DONE || rc == SQLITE_ROW) ? result : -1;
}

// user_id를 표시용 사용자명으로 변환한다.
int db_get_username_by_id(db_ctx_t *db, int user_id, char *username_out, size_t len) {
    if (!db || !username_out || len == 0) {
        return -1;
    }
    const char *sql = "SELECT username FROM users WHERE id = ?";
    db_lock(db);
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db->conn, sql, -1, &stmt, NULL) != SQLITE_OK) {
        db_unlock(db);
        return -1;
    }
    sqlite3_bind_int(stmt, 1, user_id);
    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        const char *username = (const char *)sqlite3_column_text(stmt, 0);
        if (username) {
            snprintf(username_out, len, "%s", username);
            sqlite3_finalize(stmt);
            db_unlock(db);
            return 0;
        }
    }
    sqlite3_finalize(stmt);
    db_unlock(db);
    return -1;
}
