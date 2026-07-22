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
        -I ../../src -I ../../sdk -I "$DEPS" -I stubs \
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

compile settings_mirror_tests \
    settings_mirror_tests.cpp \
    ../../src/api/SettingsMirror.cpp \
    ../../src/runtime/SettingsStore.cpp \
    ../../src/runtime/Json.cpp

compile settings_subscriptions_tests \
    settings_subscriptions_tests.cpp \
    ../../src/api/SettingsSubscriptions.cpp \
    ../../src/api/SettingsMirror.cpp \
    ../../src/runtime/SettingsStore.cpp \
    ../../src/runtime/Json.cpp

compile hotkey_service_tests \
    hotkey_service_tests.cpp \
    ../../src/runtime/HotkeyService.cpp \
    ../../src/runtime/SettingsStore.cpp \
    ../../src/runtime/Json.cpp \
    ../../src/input/InputRouter.cpp

compile hotkey_subscriptions_tests \
    hotkey_subscriptions_tests.cpp \
    ../../src/api/HotkeySubscriptions.cpp

compile bridge_api_tests \
    bridge_api_tests.cpp \
    ../../src/api/BridgeApi.cpp \
    ../../src/api/SettingsMirror.cpp \
    ../../src/api/SettingsSubscriptions.cpp \
    ../../src/api/HotkeySubscriptions.cpp \
    ../../src/runtime/MessageBridge.cpp \
    ../../src/runtime/SettingsStore.cpp \
    ../../src/runtime/Json.cpp

compile papyrus_action_tests \
    papyrus_action_tests.cpp \
    ../../src/api/PapyrusApi.cpp \
    ../../src/api/BridgeApi.cpp \
    ../../src/api/SettingsMirror.cpp \
    ../../src/api/SettingsSubscriptions.cpp \
    ../../src/api/HotkeySubscriptions.cpp \
    ../../src/runtime/MessageBridge.cpp \
    ../../src/runtime/SettingsStore.cpp \
    ../../src/runtime/Json.cpp

compile vanilla_keys_tests \
    vanilla_keys_tests.cpp \
    ../../src/runtime/VanillaKeys.cpp \
    ../../src/runtime/Json.cpp

compile localization_service_tests \
	localization_service_tests.cpp \
	../../src/runtime/LocalizationService.cpp \
	../../src/runtime/Json.cpp

compile view_manifest_tests \
	view_manifest_tests.cpp \
	../../src/runtime/ViewManifest.cpp \
	../../src/runtime/Json.cpp

compile cursor_shape_tests \
	cursor_shape_tests.cpp

failures=0
for t in settings_store_tests settings_module_tests settings_mirror_tests settings_subscriptions_tests hotkey_service_tests hotkey_subscriptions_tests bridge_api_tests papyrus_action_tests vanilla_keys_tests localization_service_tests view_manifest_tests cursor_shape_tests; do
    echo "== $t =="
    "$BUILD/$t" || failures=$((failures + $?))
done
exit "$failures"
