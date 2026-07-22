#!/usr/bin/env bash
# Host-side native unit tests: compiles the REAL runtime sources under test
# against stubs/pch.h (std umbrella + REX log stub) and runs them on the
# desktop toolchain — no Windows, SFSE, or game required. The native mirror of
# devtools/harness. Requires a C++23 compiler; fetches the pinned
# nlohmann/json single header on first run.
#
# The shared runtime/api sources are listed by many suites (Json.cpp — which
# drags in the ~25k-line nlohmann/json header — appears in ten of them), so the
# build compiles each DISTINCT translation unit exactly once to an object and
# only re-links per suite, and it fans those compiles across every core. That
# turns ~51 serial TU compiles into ~26 parallel ones.
set -euo pipefail
cd "$(dirname "$0")"

DEPS=.deps
BUILD=.build
NLOHMANN_VERSION=v3.11.3

mkdir -p "$DEPS/nlohmann" "$BUILD/obj"
if [[ ! -f "$DEPS/nlohmann/json.hpp" ]]; then
    echo "fetching nlohmann/json $NLOHMANN_VERSION ..."
    curl -fsSL --max-time 120 \
        -o "$DEPS/nlohmann/json.hpp" \
        "https://raw.githubusercontent.com/nlohmann/json/$NLOHMANN_VERSION/single_include/nlohmann/json.hpp"
fi

CXX="${CXX:-clang++}"
# One flag set for every object compile and every link.
BASEFLAGS="-std=c++2b -Wall -Wextra -g -I ../../src -I ../../sdk -I $DEPS -I stubs"

# Each suite is "<name> <translation units...>". Keep the source lists in sync
# with what each suite actually exercises; duplicates across suites are free
# (compiled once, see UNIQUE below).
SUITES=(
"settings_store_tests settings_store_tests.cpp ../../src/runtime/SettingsStore.cpp ../../src/runtime/Json.cpp"
"settings_module_tests settings_module_tests.cpp ../../src/runtime/SettingsModule.cpp ../../src/runtime/MessageBridge.cpp ../../src/runtime/SettingsStore.cpp ../../src/runtime/Json.cpp"
"settings_mirror_tests settings_mirror_tests.cpp ../../src/api/SettingsMirror.cpp ../../src/runtime/SettingsStore.cpp ../../src/runtime/Json.cpp"
"settings_subscriptions_tests settings_subscriptions_tests.cpp ../../src/api/SettingsSubscriptions.cpp ../../src/api/SettingsMirror.cpp ../../src/runtime/SettingsStore.cpp ../../src/runtime/Json.cpp"
"hotkey_service_tests hotkey_service_tests.cpp ../../src/runtime/HotkeyService.cpp ../../src/runtime/SettingsStore.cpp ../../src/runtime/Json.cpp ../../src/input/InputRouter.cpp"
"hotkey_subscriptions_tests hotkey_subscriptions_tests.cpp ../../src/api/HotkeySubscriptions.cpp"
"bridge_api_tests bridge_api_tests.cpp ../../src/api/BridgeApi.cpp ../../src/api/SettingsMirror.cpp ../../src/api/SettingsSubscriptions.cpp ../../src/api/HotkeySubscriptions.cpp ../../src/runtime/MessageBridge.cpp ../../src/runtime/SettingsStore.cpp ../../src/runtime/Json.cpp"
"papyrus_action_tests papyrus_action_tests.cpp ../../src/api/PapyrusApi.cpp ../../src/api/BridgeApi.cpp ../../src/api/SettingsMirror.cpp ../../src/api/SettingsSubscriptions.cpp ../../src/api/HotkeySubscriptions.cpp ../../src/runtime/MessageBridge.cpp ../../src/runtime/SettingsStore.cpp ../../src/runtime/Json.cpp"
"vanilla_keys_tests vanilla_keys_tests.cpp ../../src/runtime/VanillaKeys.cpp ../../src/runtime/Json.cpp"
"localization_service_tests localization_service_tests.cpp ../../src/runtime/LocalizationService.cpp ../../src/runtime/Json.cpp"
"view_manifest_tests view_manifest_tests.cpp ../../src/runtime/ViewManifest.cpp ../../src/runtime/Json.cpp"
"cursor_shape_tests cursor_shape_tests.cpp"
)

# Deterministic object path for a source (flatten dir separators into the name).
objname() { printf '%s/obj/%s.o' "$BUILD" "$(printf '%s' "$1" | tr './ ' '___')"; }

# --- Collect the distinct translation units across all suites.
declare -A seen=()
UNIQUE=()
for suite in "${SUITES[@]}"; do
    read -r _name srcs <<< "$suite"
    for s in $srcs; do
        [[ -n "${seen[$s]:-}" ]] && continue
        seen[$s]=1
        UNIQUE+=("$s")
    done
done

# --- Compile each distinct unit once, in parallel. xargs exits nonzero (so the
# --- pipeline fails under `set -o pipefail`) if any compile fails.
compile_one() {
    local src=$1 obj
    obj="$BUILD/obj/$(printf '%s' "$src" | tr './ ' '___').o"
    echo "  cc $src"
    $CXX $BASEFLAGS -include stubs/pch.h -c "$src" -o "$obj"
}
export -f compile_one
export CXX BASEFLAGS BUILD

echo "== compiling ${#UNIQUE[@]} translation units (-P $(nproc)) =="
printf '%s\n' "${UNIQUE[@]}" | xargs -P "$(nproc 2>/dev/null || echo 4)" -n1 -I{} bash -c 'compile_one "$1"' _ {}

# --- Link each suite from its (already built) objects.
echo "== linking ${#SUITES[@]} suites =="
for suite in "${SUITES[@]}"; do
    read -r name srcs <<< "$suite"
    objs=()
    for s in $srcs; do objs+=("$(objname "$s")"); done
    $CXX $BASEFLAGS "${objs[@]}" -o "$BUILD/$name"
done

# --- Run. exit code = number of failing checks.
failures=0
for suite in "${SUITES[@]}"; do
    read -r name _ <<< "$suite"
    echo "== $name =="
    "$BUILD/$name" || failures=$((failures + $?))
done
exit "$failures"
