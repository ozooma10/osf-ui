#!/usr/bin/env bash
# Host-side native unit tests: compiles the REAL runtime sources under test
# against stubs/pch.h (std umbrella + REX log stub) and runs them on the
# desktop toolchain — no Windows, SFSE, or game required. The native mirror of
# devtools/harness. Requires a C++23 compiler; fetches the pinned
# nlohmann/json single header on first run.
set -euo pipefail
cd "$(dirname "$0")"

DEPS=.deps
BUILD=.build
NLOHMANN_VERSION=v3.11.3

mkdir -p "$DEPS/nlohmann" "$BUILD"
if [[ ! -f "$DEPS/nlohmann/json.hpp" ]]; then
    echo "fetching nlohmann/json $NLOHMANN_VERSION ..."
    curl -fsSL --max-time 120 \
        -o "$DEPS/nlohmann/json.hpp" \
        "https://raw.githubusercontent.com/nlohmann/json/$NLOHMANN_VERSION/single_include/nlohmann/json.hpp"
fi

CXX="${CXX:-clang++}"
"$CXX" -std=c++2b -Wall -Wextra -g \
    -I ../../src -I "$DEPS" -I stubs \
    -include stubs/pch.h \
    settings_store_tests.cpp \
    ../../src/runtime/SettingsStore.cpp \
    ../../src/runtime/Json.cpp \
    -o "$BUILD/settings_store_tests"

"$BUILD/settings_store_tests"
