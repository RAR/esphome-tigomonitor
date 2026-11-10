#!/bin/sh
# Builds tigomonitord into build/linux and runs the shared-header host tests.
# Run from the repo root, inside the dev container (see linux/Dockerfile.dev).
set -eu
cmake -S linux -B build/linux -DCMAKE_BUILD_TYPE=RelWithDebInfo -DTIGOD_VERSION="${TIGOD_VERSION:-dev}"
cmake --build build/linux -j"$(nproc)"
for t in barcode_bindings_test protocol_test node_identity_test cca_ble_protocol_test master_protocol_test layout_test bus_watch_test link_stats_test discovery_policy_test mesh_history_test table_edit_test; do
  g++ -std=c++17 -Wall -Wextra -I components/tigo_monitor "tests/host/$t.cpp" -o "build/linux/$t"
  # To a log, not piped through tail: a pipeline's status is tail's, so a
  # failing test would leave this script, and CI, green.
  if ! "build/linux/$t" >"build/linux/$t.log" 2>&1; then
    cat "build/linux/$t.log"
    echo "$t failed" >&2
    exit 1
  fi
  tail -1 "build/linux/$t.log"
done
# The bus master's core, against a fake port.
g++ -std=c++17 -O2 -Wall -Wextra -I components/tigo_monitor tests/host/master_core_test.cpp \
  components/tigo_master/master_core.cpp -lpthread -o build/linux/master_core_test
if ! build/linux/master_core_test >build/linux/master_core_test.log 2>&1; then
  cat build/linux/master_core_test.log
  echo "master_core_test failed" >&2
  exit 1
fi
tail -1 build/linux/master_core_test.log
