# The TigoMonitor app: Home Assistant builds this on top of the released
# tigomonitord image (build.yaml), adding only the start script.
ARG BUILD_FROM
FROM ${BUILD_FROM}

# Home Assistant keeps an app's state in /data, which belongs to root.
USER root
# curl and jq: the start script asks the Supervisor for the MQTT broker.
RUN apt-get update \
 && apt-get install -y --no-install-recommends curl jq ca-certificates \
 && rm -rf /var/lib/apt/lists/*
# Not the image's healthcheck: it reports /api/health, which is unhealthy
# until a bus is chosen and whenever the bus goes quiet, and the Supervisor
# restarts an unhealthy app when its watchdog is on. Not HEALTHCHECK NONE
# either: that still leaves a healthcheck in the image ({"Test": ["NONE"]}),
# and the Supervisor counts an app with one as started only when Docker
# reports it healthy or unhealthy, which NONE never does, so the app sat at
# "starting" for good. This asks only whether the web server answers: any
# HTTP reply (a 401 too) is alive. HTTPS, if someone turns it on, after.
HEALTHCHECK --interval=30s --timeout=5s --start-period=60s --start-interval=2s --retries=3 \
  CMD curl -s -o /dev/null http://127.0.0.1:8080/ || curl -sk -o /dev/null https://127.0.0.1:8080/ || exit 1
COPY run.sh /run.sh
RUN chmod 755 /run.sh
ENTRYPOINT ["/run.sh"]
