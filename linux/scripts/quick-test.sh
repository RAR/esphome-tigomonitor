#!/bin/sh
# Before a local install: the build and the host suites, then only the e2e
# scripts named, in the dev container. CI still runs every e2e on a PR.
#
#   linux/scripts/quick-test.sh bus-log master     → build.sh, bus-log-e2e.sh, master-e2e.sh
#
# Run from the repo root on the host. Each script's output goes to
# build/quick-<name>.log; a failure prints that log's FAIL lines.
set -eu
mkdir -p build
docker run --rm -v "$PWD":/src -w /src tigod-dev sh -c 'linux/scripts/build.sh > build/quick-build.log 2>&1' \
  || { tail -20 build/quick-build.log; echo "build failed" >&2; exit 1; }
echo "build and host suites: ok"
fail=0
for n in "$@"; do
  s="linux/scripts/${n%-e2e}-e2e.sh"
  [ -f "$s" ] || { echo "no such e2e: $s" >&2; exit 2; }
  if docker run --rm -v "$PWD":/src -w /src tigod-dev "$s" > "build/quick-$n.log" 2>&1; then
    echo "$n: ok"
  else
    grep FAIL "build/quick-$n.log" || tail -10 "build/quick-$n.log"
    echo "$n: FAILED" >&2
    fail=1
  fi
done
exit $fail
