#!/bin/sh
# Builds the arm64 .deb here, for a Pi, without waiting on CI: Debian trixie
# arm64 under QEMU (docker's binfmt), into build/arm64, which is kept, so
# after the first build only what changed recompiles. The version is CI's
# (git describe), with +local when the tree has uncommitted changes.
#
#   linux/scripts/local-deb.sh            → build/deb/tigomonitord_<ver>_arm64.deb
#
# Run from the repo root on the host (not inside the dev container). Builds
# the arm64 dev image first if it's missing or older than Dockerfile.dev, and
# registers QEMU's arm64 emulation if a reboot dropped it.
# Tests are separate: linux/scripts/build.sh and the e2e scripts, natively.
set -eu
img=tigod-dev:arm64
# Docker's arm64 emulation is gone after a reboot: register it again.
if [ ! -e /proc/sys/fs/binfmt_misc/qemu-aarch64 ]; then
  docker run --privileged --rm tonistiigi/binfmt:qemu-v9.2.2 --install arm64 >/dev/null
fi
built="$(docker image inspect "$img" --format '{{.Created}}' 2>/dev/null || true)"
changed="$(git log -1 --format=%cI -- linux/Dockerfile.dev)"
if [ -z "$built" ] || [ "$(date -d "$built" +%s)" -lt "$(date -d "$changed" +%s)" ]; then
  docker build --platform linux/arm64 -t "$img" -f linux/Dockerfile.dev linux
fi
version="$(git describe --tags | sed -E 's/^v//; s/-([0-9]+)-g/+\1.g/')"
git diff --quiet HEAD -- components linux || version="$version+local"
docker run --rm --platform linux/arm64 -v "$PWD":/src -w /src -e TIGOD_BUILD_DIR=build/arm64 \
  "$img" linux/packaging/build-deb.sh "$version" | tail -1
