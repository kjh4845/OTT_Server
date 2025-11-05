#include "ffmpeg.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "utils.h"

int ffmpeg_initialize(server_ctx_t *server) {
    if (!server) return -1;
    if (ensure_directory(server->thumb_dir) != 0) {
        log_error("Failed to ensure thumbnail directory %s: %s", server->thumb_dir, strerror(errno));
        return -1;
    }
    return 0;
}

int ffmpeg_ensure_thumbnail(server_ctx_t *server, int video_id,
                            const char *video_path, char *thumb_path,
                            size_t thumb_path_len) {
    if (!server || !video_path || !thumb_path) {
        return -1;
    }
    if (snprintf(thumb_path, thumb_path_len, "%s/%d.jpg", server->thumb_dir, video_id) >= (int)thumb_path_len) {
        return -1;
    }
    struct stat video_stat;
    if (stat(video_path, &video_stat) != 0) {
        return -1;
    }
    struct stat thumb_stat;
    if (stat(thumb_path, &thumb_stat) == 0) {
        if (thumb_stat.st_mtime >= video_stat.st_mtime) {
            return 0; // cached thumbnail is fresh
        }
    }

    pid_t pid = fork();
    if (pid == 0) {
        execlp("ffmpeg", "ffmpeg", "-y", "-loglevel", "error", "-ss", "5",
               "-i", video_path, "-vframes", "1", "-vf", "scale=320:-1", thumb_path, (char *)NULL);
        _exit(1);
    } else if (pid < 0) {
        log_error("fork() failed for ffmpeg: %s", strerror(errno));
        return -1;
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        log_error("waitpid failed for ffmpeg: %s", strerror(errno));
        return -1;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        log_error("ffmpeg failed to generate thumbnail for %s", video_path);
        unlink(thumb_path);
        return -1;
    }
    return 0;
}
