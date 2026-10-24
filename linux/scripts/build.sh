#!/bin/sh
# Builds tigomonitord into build/linux and runs the shared-header host tests.
# Run from the repo root, inside the dev container (see linux/Dockerfile.dev).
set -eu
cmake -S linux -B build/linux -DCMAKE_BUILD_TYPE=RelWithDebInfo -DTIGOD_VERSION="${TIGOD_VERSION:-dev}"
cmake --build build/linux -j"$(nproc)"
for t in protocol_test node_identity_test; do
  g++ -std=c++17 -Wall -Wextra -I components/tigo_monitor "tests/host/$t.cpp" -o "build/linux/$t"
  "build/linux/$t" | tail -1
done
