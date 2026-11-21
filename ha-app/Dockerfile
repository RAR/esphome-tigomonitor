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
# until a bus is chosen and whenever the bus goes quiet in the daytime. The
# Supervisor waits for healthy before it counts the app as started, and
# restarts an app it sees as unhealthy. Liveness is covered without it: the
# daemon exits if its main loop sticks for a minute, and the app's
# watchdog restarts it if the web port stops answering.
HEALTHCHECK NONE
COPY run.sh /run.sh
RUN chmod 755 /run.sh
ENTRYPOINT ["/run.sh"]
