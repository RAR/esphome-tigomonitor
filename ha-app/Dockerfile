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
COPY run.sh /run.sh
RUN chmod 755 /run.sh
ENTRYPOINT ["/run.sh"]
