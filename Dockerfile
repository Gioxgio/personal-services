# syntax=docker/dockerfile:1
# Single Dockerfile for the merged repo; one target stage per service:
#   docker build --target download-service   -t download-service .
#   docker build --target notification-service -t notification-service .
#   docker build --target stream-sniper      -t stream-sniper .

FROM debian:bookworm-slim AS download-builder
RUN apt-get update && apt-get install -y --no-install-recommends build-essential && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY download-service /src
RUN make -j"$(nproc)"

FROM debian:bookworm-slim AS download-service
RUN apt-get update && apt-get install -y --no-install-recommends ca-certificates curl ffmpeg && rm -rf /var/lib/apt/lists/* \
 && curl -fsSL -o /usr/local/bin/yt-dlp https://github.com/yt-dlp/yt-dlp/releases/latest/download/yt-dlp && chmod a+rx /usr/local/bin/yt-dlp \
 && mkdir -p /var/lib/download-service && chown -R download:download /var/lib/download-service \
 && useradd --system --home-dir /nonexistent --shell /usr/sbin/nologin download || true
WORKDIR /var/lib/download-service
COPY --from=download-builder /src/build/download-service /usr/local/bin/download-service
ENV PORT=8080 DOWNLOAD_STORAGE_PATH=/var/lib/download-service YTDLP_PATH=/usr/local/bin/yt-dlp CALLBACK_URL=http://notification-service:8080/callback/download
EXPOSE 8080
USER download
STOPSIGNAL SIGTERM
ENTRYPOINT ["download-service"]

FROM debian:bookworm-slim AS notification-builder
RUN apt-get update && apt-get install -y --no-install-recommends build-essential libcurl4-openssl-dev && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY notification-service /src
RUN make -j"$(nproc)"

FROM debian:bookworm-slim AS notification-service
RUN apt-get update && apt-get install -y --no-install-recommends libcurl4 ca-certificates && rm -rf /var/lib/apt/lists/* \
 && useradd --system --home-dir /nonexistent --shell /usr/sbin/nologin notification
WORKDIR /src
COPY --from=notification-builder /src/build/notification-service /usr/local/bin/notification-service
ENV PORT=8080
EXPOSE 8080
USER notification
STOPSIGNAL SIGTERM
ENTRYPOINT ["notification-service"]

FROM alpine:3.24 AS stream-sniper-builder
RUN apk add --no-cache build-base pkgconf curl-dev
WORKDIR /src
COPY stream-sniper /src
RUN make && make install PREFIX=/usr/local

FROM alpine:3.24 AS stream-sniper
RUN apk add --no-cache libcurl ffmpeg ca-certificates python3 curl \
 && curl -fsSL -o /usr/local/bin/yt-dlp https://github.com/yt-dlp/yt-dlp/releases/latest/download/yt-dlp && chmod 0755 /usr/local/bin/yt-dlp
COPY --from=stream-sniper-builder /usr/local/bin/stream-sniper /usr/local/bin/stream-sniper
COPY --from=stream-sniper-builder /usr/local/libexec/stream-sniper/ /usr/local/libexec/stream-sniper/
ENV SS_WRAPPER_DIR=/usr/local/libexec/stream-sniper SS_NOTIFY_URL=http://notification-service:8080/notifications
WORKDIR /data
COPY stream-sniper/channels.txt.example /data/channels.txt
ENTRYPOINT ["stream-sniper"]
CMD ["/data/channels.txt", "-o", "/data/recordings"]
