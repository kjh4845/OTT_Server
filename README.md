# OTT_Server
This is OTT_Server project with C

High-performance OTT-style media server implemented in C11 using POSIX sockets, epoll, and a pthread-based worker pool. The server streams MP4 files with HTTP Range support, manages authentication and viewing history, and generates cached thumbnails with FFmpeg. A lightweight HTML/CSS/JS front-end is included, together with Docker tooling for reproducible builds.

## Features

- **Custom HTTP stack** – epoll-based event loop with worker pool, manual HTTP parsing, static file serving, and Range (206) streaming via `sendfile` when available.
- **Authentication & sessions** – salted PBKDF2-HMAC-SHA256 password hashing with OpenSSL, cookie-based sessions stored in SQLite with configurable TTL.
- **Media catalogue** – automatic discovery of `.mp4` files, metadata stored in SQLite, and JSON APIs for listing and streaming.
- **Watch history** – progress tracking with resumable playback, persisted per-user in SQLite.
- **FFmpeg thumbnails** – on-demand capture of poster frames cached to disk.
- **Front-end** – simple UI for login, library browsing, and playback with resume prompts; video cards are fully clickable for faster navigation.
- **Container support** – multi-stage Dockerfile and `docker-compose.yml` for turnkey deployment.

## Project layout

```
ott-c/
├─ server/              # C source, headers, Makefile, schema, Dockerfile
├─ web/                 # Static front-end assets (HTML, CSS, JS, thumbnails)
├─ media/               # Place MP4 files here (mounted into the container)
├─ data/                # SQLite database storage (mounted volume)
├─ docker-compose.yml   # Multi-service orchestration
└─ README.md
```

## Configuration

The server reads environment variables (defaults shown):

| Variable | Description | Default |
| --- | --- | --- |
| `PORT` | HTTP listen port | `3000` |
| `MEDIA_DIR` | Directory containing MP4 assets | `./media` (or `/app/media` in Docker) |
| `THUMB_DIR` | Thumbnail cache directory | `./web/thumbnails` |
| `SESSION_TTL_HOURS` | Session lifetime in hours | `24` |
| `DB_PATH` | Optional override for SQLite file | `data/app.db` |

The SQLite schema is defined in `server/schema.sql`. On first launch the server seeds default accounts for smoke testing:

- `test / test1234`
- `demo / demo1234`
- `guest / guestpass`
- `sample / sample1234`

## Building & running locally

### Prerequisites

- GCC or Clang with C11 support
- POSIX environment (Linux recommended; macOS uses `poll()` fallback)
- Development libraries: `libsqlite3`, `libssl` (OpenSSL), `ffmpeg`

### Build

```bash
cd server
make
```

### Run

```bash
./ott_server
```

By default the server scans `../media` for MP4 files, serves the front-end from `../web/public`, and stores data in `../data/app.db`. Visit [http://localhost:3000](http://localhost:3000) to access the UI.

## Docker workflow

```bash
# Build the image
docker compose build

# Launch the stack in the background
docker compose up -d

# Tail logs
docker compose logs -f
```

Volumes mount the host `media/`, `web/thumbnails/`, and `data/` directories into the container, ensuring media, cached thumbnails, and the SQLite database persist across restarts.

## API overview

| Method | Endpoint | Description |
| --- | --- | --- |
| `POST` | `/api/auth/login` | Authenticate user (sets HttpOnly session cookie) |
| `POST` | `/api/auth/logout` | Destroy current session |
| `GET` | `/api/auth/me` | Return authenticated user info |
| `GET` | `/api/videos` | List available videos, thumbnails, and resume markers |
| `GET` | `/api/videos/:id/thumbnail` | Fetch/generate JPEG thumbnail |
| `GET` | `/api/videos/:id/stream` | Stream MP4 content with Range support |
| `GET` | `/api/history` | Retrieve watch history for current user |
| `POST` | `/api/history/:id` | Update progress (seconds) for a video |

All API responses are JSON; errors return payloads of the form `{"error":"message"}`. Streaming endpoints return binary data with appropriate headers.

## Development notes

- Sockets are served via an epoll-driven acceptor on Linux; macOS builds transparently fall back to `poll()`.
- HTTP connections are closed after every response (no keep-alive) for simplicity.
- Range requests validate bounds and reply with `206 Partial Content`, `Accept-Ranges`, and `Content-Range` headers.
- FFmpeg is invoked as `ffmpeg -ss 5 -vframes 1 -vf scale=320:-1` and thumbnails are cached under `THUMB_DIR` with automatic regeneration when the source file updates.
- Watch-history updates normalise positions near the end of a title back to zero to mark completion.

## Maintenance

- Rebuild the binary: `cd server && make clean && make`.
- Reset cached thumbnails: `rm -rf web/thumbnails/*`.
- Inspect the SQLite database: `sqlite3 data/app.db`.
- To add new users manually, reuse the PBKDF2 routine (`auth_hash_password`) to generate salt/hash pairs before inserting into the `users` table.

## 2025-11-07 Updates

- **Media sync cleanup** – `sync_media_directory` now collects the actual filenames under `media/` and calls the new `db_prune_missing_videos` helper so any deleted MP4s are dropped from the `videos` table automatically, preventing stale entries without manual SQL.
- **Database helper** – `db_prune_missing_videos` (in `server/src/db.c`) loads the live filename set into a temporary table and deletes rows not in that set, keeping watch-history FKs intact while avoiding recursive DB locks.
- **Tooling tweaks** – debug symbols are enabled in `server/Makefile` for easier gdb/valgrind work inside Docker
- **Verification** – rebuild via `docker compose build` and sanity-check with `docker compose run --rm --entrypoint sh ott -c "/app/ott_server"`; stop any ad-hoc containers afterwards (e.g., `docker stop $(docker ps -q --filter name=ott-c-ott-run)`).
