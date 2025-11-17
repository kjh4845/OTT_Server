# syntax=docker/dockerfile:1

FROM ubuntu:22.04 AS build
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y \
    build-essential pkg-config \
    libsqlite3-dev libssl-dev \
    ffmpeg \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /app/server
COPY ./server /app/server
# Host builds can leave behind macOS binaries; clean before rebuilding for Linux.
RUN make clean && make

FROM ubuntu:22.04 AS run
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y \
    libsqlite3-0 libssl3 ffmpeg \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /app
COPY --from=build /app/server/ott_server /app/ott_server
COPY ./web /app/web
COPY ./server/schema.sql /app/server/schema.sql
RUN mkdir -p /app/media /app/web/thumbnails /app/data
ENV PORT=3000 \
    MEDIA_DIR=/app/media \
    THUMB_DIR=/app/web/thumbnails \
    SESSION_TTL_HOURS=24
EXPOSE 3000
HEALTHCHECK --interval=30s --timeout=3s --start-period=10s \
  CMD [ -f "/app/web/public/index.html" ] || exit 1
CMD ["/app/ott_server"]
