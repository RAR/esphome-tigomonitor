# tigomonitord as a container. Build from the repo root:
#
#   docker build -t tigomonitord -f linux/Dockerfile .
#
# then see linux/docker-compose.yml, or the Raspberry Pi guide's "Run it in
# Docker" section, for running it.
FROM debian:trixie-slim AS build
RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential cmake pkg-config \
      libmosquitto-dev nlohmann-json3-dev zlib1g-dev libsqlite3-dev libsystemd-dev libssl-dev \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY linux linux
COPY components/tigo_monitor components/tigo_monitor
COPY components/tigo_server/web components/tigo_server/web
COPY site/public/favicon.svg site/public/favicon.svg
ARG VERSION=dev
RUN cmake -S linux -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr -DTIGOD_VERSION="$VERSION" \
    && cmake --build build -j"$(nproc)" \
    && DESTDIR=/out cmake --install build --strip

FROM debian:trixie-slim
RUN apt-get update && apt-get install -y --no-install-recommends \
      libmosquitto1 libsqlite3-0 libsystemd0 libssl3t64 zlib1g ca-certificates \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --system --uid 999 --user-group --groups dialout --home-dir /var/lib/tigomonitor \
         --no-create-home --shell /usr/sbin/nologin tigomonitor \
    && install -d -o tigomonitor -g tigomonitor -m 700 /var/lib/tigomonitor
COPY --from=build /out/usr /usr
COPY LICENSE /usr/share/doc/tigomonitord/copyright
COPY linux/third_party/cpp-httplib/LICENSE /usr/share/doc/tigomonitord/copyright.cpp-httplib
USER tigomonitor
VOLUME /var/lib/tigomonitor
# A fixed port inside the container; publish it as whatever you like. The
# web UI's port setting has no effect here.
EXPOSE 8080
ENTRYPOINT ["/usr/bin/tigomonitord", "--port", "8080"]
