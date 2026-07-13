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
compile() { # <output> <sources...>
    local out=$1
    shift
    "$CXX" -std=c++2b -Wall -Wextra -g \
        -I ../../src -I "$DEPS" -I stubs \
        -include stubs/pch.h \
        "$@" -o "$BUILD/$out"
}

compile settings_store_tests \
    settings_store_tests.cpp \
    ../../src/runtime/SettingsStore.cpp \
    ../../src/runtime/Json.cpp

compile settings_module_tests \
    settings_module_tests.cpp \
    ../../src/runtime/SettingsModule.cpp \
    ../../src/runtime/MessageBridge.cpp \
    ../../src/runtime/SettingsStore.cpp \
    ../../src/runtime/Json.cpp

failures=0
for t in settings_store_tests settings_module_tests; do
    echo "== $t =="
    "$BUILD/$t" || failures=$((failures + $?))
done
exit "$failures"
