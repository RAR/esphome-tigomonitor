#!/bin/sh
# Builds tigomonitord into build/linux and runs the shared-header host tests.
# Run from the repo root, inside the dev container (see linux/Dockerfile.dev).
set -eu
cmake -S linux -B build/linux -DCMAKE_BUILD_TYPE=RelWithDebInfo -DTIGOD_VERSION="${TIGOD_VERSION:-dev}"
cmake --build build/linux -j"$(nproc)"
for t in barcode_bindings_test protocol_test node_identity_test cca_ble_protocol_test master_protocol_test layout_test layout_alloc_test bus_watch_test bus_book_test bus_alloc_test array_grid_test link_stats_test discovery_policy_test mesh_history_test table_edit_test tap_config_test; do
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
# The ESP32's gzipped web page, token spliced in (components/tigo_server).
python3 components/tigo_server/web_assets_gen.py --out build/linux/web_assets.h
g++ -std=c++17 -Wall -Wextra -I build/linux -I components/tigo_server \
  -DAPP_HTML_PATH='"components/tigo_server/web/app.html"' tests/host/gzip_splice_test.cpp -lz \
  -o build/linux/gzip_splice_test
if ! build/linux/gzip_splice_test >build/linux/gzip_splice_test.log 2>&1; then
  cat build/linux/gzip_splice_test.log
  echo "gzip_splice_test failed" >&2
  exit 1
fi
tail -1 build/linux/gzip_splice_test.log
# The web UI's sign-in sessions.
g++ -std=c++17 -Wall -Wextra -I linux/src tests/host/web_session_test.cpp -o build/linux/web_session_test
if ! build/linux/web_session_test >build/linux/web_session_test.log 2>&1; then
  cat build/linux/web_session_test.log
  echo "web_session_test failed" >&2
  exit 1
fi
tail -1 build/linux/web_session_test.log
# The bus master's core, against a fake port.
g++ -std=c++17 -O2 -Wall -Wextra -I components/tigo_monitor tests/host/master_core_test.cpp \
  components/tigo_master/master_core.cpp -lpthread -o build/linux/master_core_test
if ! build/linux/master_core_test >build/linux/master_core_test.log 2>&1; then
  cat build/linux/master_core_test.log
  echo "master_core_test failed" >&2
  exit 1
fi
tail -1 build/linux/master_core_test.log
# The array layout (Topology > Array).
g++ -std=c++17 -Wall -Wextra -I linux/src -I components/tigo_monitor tests/host/array_layout_test.cpp -o build/linux/array_layout_test
if ! build/linux/array_layout_test >build/linux/array_layout_test.log 2>&1; then
  cat build/linux/array_layout_test.log
  echo "array_layout_test failed" >&2
  exit 1
fi
tail -1 build/linux/array_layout_test.log
# The PGW pool rule (pgw.h), on top of the array layout.
g++ -std=c++17 -Wall -Wextra -I linux/src -I components/tigo_monitor tests/host/pgw_test.cpp -o build/linux/pgw_test
if ! build/linux/pgw_test >build/linux/pgw_test.log 2>&1; then
  cat build/linux/pgw_test.log
  echo "pgw_test failed" >&2
  exit 1
fi
tail -1 build/linux/pgw_test.log
# The TAPs heard run by another master (heard_taps.h), for a takeover
# with no saved TAP list.
g++ -std=c++17 -Wall -Wextra -I linux/src tests/host/heard_taps_test.cpp -o build/linux/heard_taps_test
if ! build/linux/heard_taps_test >build/linux/heard_taps_test.log 2>&1; then
  cat build/linux/heard_taps_test.log
  echo "heard_taps_test failed" >&2
  exit 1
fi
tail -1 build/linux/heard_taps_test.log
