#ifndef DB_H
#define DB_H

// SQLite 기반 영속 계층 인터페이스

#include <pthread.h>
#include <sqlite3.h>
#include <time.h>
#include <stddef.h>

typedef struct {
    sqlite3 *conn;        // SQLite 핸들
    pthread_mutex_t mutex; // 단일 연결을 여러 스레드에서 보호하기 위한 뮤텍스
} db_ctx_t;

int db_init(db_ctx_t *db, const char *path);
void db_close(db_ctx_t *db);
int db_run_schema(db_ctx_t *db, const char *schema_path);
const char *db_errmsg(db_ctx_t *db);

int db_get_user_credentials(db_ctx_t *db, const char *username, int *user_id,
                            unsigned char *hash_out, size_t hash_len,
                            unsigned char *salt_out, size_t salt_len);
int db_upsert_user(db_ctx_t *db, const char *username,
                   const unsigned char *hash, size_t hash_len,
                   const unsigned char *salt, size_t salt_len);
int db_create_user(db_ctx_t *db, const char *username,
                   const unsigned char *hash, size_t hash_len,
                   const unsigned char *salt, size_t salt_len,
                   int *user_id_out);

int db_create_session(db_ctx_t *db, const char *token, int user_id, time_t expires_at);
int db_get_session(db_ctx_t *db, const char *token, int *user_id, time_t *expires_at);
int db_delete_session(db_ctx_t *db, const char *token);
int db_purge_expired_sessions(db_ctx_t *db, time_t now);

int db_list_videos(db_ctx_t *db,
                   int (*callback)(void *userdata, int id, const char *title,
                                   const char *filename, const char *description,
                                   int duration_seconds),
                   void *userdata);
int db_query_videos(db_ctx_t *db, const char *search_term, int limit, int offset,
                    int (*callback)(void *userdata, int id, const char *title,
                                    const char *filename, const char *description,
                                    int duration_seconds),
                    void *userdata, int *has_more_out);
int db_get_video_by_id(db_ctx_t *db, int video_id,
                       char *title_out, size_t title_len,
                       char *filename_out, size_t filename_len,
                       char *description_out, size_t desc_len,
                       int *duration_seconds_out);
int db_upsert_video(db_ctx_t *db, const char *title, const char *filename,
                    const char *description, int duration_seconds, int *video_id_out);
int db_delete_video_by_filename(db_ctx_t *db, const char *filename);
int db_prune_missing_videos(db_ctx_t *db, const char *const *filenames, size_t count);

int db_update_watch_history(db_ctx_t *db, int user_id, int video_id, double position_seconds);
int db_get_watch_history(db_ctx_t *db, int user_id, int video_id, double *position_seconds_out);
int db_list_watch_history(db_ctx_t *db, int user_id,
                          int (*callback)(void *userdata, int video_id, double position_seconds,
                                          const char *updated_at),
                          void *userdata);
int db_get_username_by_id(db_ctx_t *db, int user_id, char *username_out, size_t len);

#endif
