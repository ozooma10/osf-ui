#!/usr/bin/env bash
# Native desktop unit tests: compiles the REAL runtime sources under test
# against stubs/pch.h (std umbrella + REX log stub) and runs them on the
# desktop toolchain — no Windows, SFSE, or game required. The native mirror of
# devtools/harness. Requires a C++23 compiler; fetches the locked
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

# The build dir is disposable and is rebuilt from scratch every run (there is
# no cross-run incrementality to preserve). Recreating it also evicts binaries
# of suites that no longer exist, which would otherwise remain runnable.
rm -rf "$BUILD"
mkdir -p "$DEPS/nlohmann" "$BUILD/obj"
if [[ ! -f "$DEPS/nlohmann/json.hpp" ]]; then
    echo "fetching nlohmann/json $NLOHMANN_VERSION ..."
    curl -fsSL --max-time 120 \
        -o "$DEPS/nlohmann/json.hpp" \
        "https://raw.githubusercontent.com/nlohmann/json/$NLOHMANN_VERSION/single_include/nlohmann/json.hpp"
fi

# Toolchain: CI and Unix developers use a GNU-driver compiler (CXX, default
# clang++). On a Windows box with no clang/g++ on PATH, fall back to MSVC cl
# through vcvars64 — the "MSVC dev shell" path AGENTS.md describes — with the
# same suite list, force-included stubs, and exit-code protocol.
MSVC=0
if [[ -z "${CXX:-}" ]] && ! command -v clang++ >/dev/null 2>&1 && ! command -v g++ >/dev/null 2>&1; then
    VSWHERE="/c/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe"
    if [[ -x "$VSWHERE" ]]; then
        VSROOT="$("$VSWHERE" -latest -property installationPath | tr -d '\r')"
        [[ -n "$VSROOT" ]] && MSVC=1
    fi
fi
CXX="${CXX:-clang++}"
BASEFLAGS="-std=c++2b -Wall -Wextra -g -I ../../src -I ../../sdk -I ../../tools/webview2_shared -I $DEPS -I stubs"

# Each suite is "<name> <translation units...>". Keep the source lists in sync
# with what each suite actually exercises; duplicates across suites are free
# (compiled once, see UNIQUE below).
SUITES=(
"json_tests json_tests.cpp ../../src/Core/Json.cpp"
"settings_store_tests settings_store_tests.cpp ../../src/Settings/SettingsStore.cpp ../../src/Core/Json.cpp"
"settings_module_tests settings_module_tests.cpp ../../src/Settings/SettingsModule.cpp ../../src/Bridge/MessageBridge.cpp ../../src/Settings/SettingsStore.cpp ../../src/Core/Json.cpp"
"settings_mirror_tests settings_mirror_tests.cpp ../../src/API/SettingsMirror.cpp ../../src/Settings/SettingsStore.cpp ../../src/Core/Json.cpp"
"settings_subscriptions_tests settings_subscriptions_tests.cpp ../../src/API/SettingsSubscriptions.cpp ../../src/API/SettingsMirror.cpp ../../src/Settings/SettingsStore.cpp ../../src/Core/Json.cpp"
"health_registry_tests health_registry_tests.cpp ../../src/Diagnostics/HealthRegistry.cpp ../../src/Bridge/MessageBridge.cpp ../../src/Core/Json.cpp"
"runtime_health_tests runtime_health_tests.cpp ../../src/Diagnostics/HealthReconciler.cpp ../../src/Diagnostics/HealthRegistry.cpp ../../src/Bridge/MessageBridge.cpp ../../src/Core/Json.cpp"
"hotkey_service_tests hotkey_service_tests.cpp ../../src/Bindings/HotkeyService.cpp ../../src/Settings/SettingsStore.cpp ../../src/Core/Json.cpp ../../src/Input/KeyNames.cpp"
"hotkey_subscriptions_tests hotkey_subscriptions_tests.cpp ../../src/API/HotkeySubscriptions.cpp"
"scan_code_tests scan_code_tests.cpp ../../src/Input/KeyNames.cpp"
"key_label_tests key_label_tests.cpp ../../src/Input/KeyLabels.cpp ../../src/Input/KeyNames.cpp"
"bridge_api_tests bridge_api_tests.cpp ../../src/API/BridgeApi.cpp ../../src/API/SettingsMirror.cpp ../../src/API/SettingsSubscriptions.cpp ../../src/API/HotkeySubscriptions.cpp ../../src/Bridge/MessageBridge.cpp ../../src/Settings/SettingsStore.cpp ../../src/Core/Json.cpp"
"v1_native_bridge_tests v1_native_bridge_tests.cpp ../../src/Compat/V1/NativeBridge.cpp ../../src/API/BridgeApi.cpp ../../src/API/SettingsMirror.cpp ../../src/API/SettingsSubscriptions.cpp ../../src/API/HotkeySubscriptions.cpp ../../src/Bridge/MessageBridge.cpp ../../src/Settings/SettingsStore.cpp ../../src/Core/Json.cpp"
"papyrus_action_tests papyrus_action_tests.cpp ../../src/API/PapyrusApi.cpp ../../src/Compat/V1/Papyrus.cpp ../../src/API/BridgeApi.cpp ../../src/API/SettingsMirror.cpp ../../src/API/SettingsSubscriptions.cpp ../../src/API/HotkeySubscriptions.cpp ../../src/Bridge/MessageBridge.cpp ../../src/Settings/SettingsStore.cpp ../../src/Bridge/RetainedStateStore.cpp ../../src/Core/Json.cpp"
"papyrus_form_tests papyrus_form_tests.cpp ../../src/API/PapyrusApi.cpp ../../src/Compat/V1/Papyrus.cpp ../../src/API/BridgeApi.cpp ../../src/API/SettingsMirror.cpp ../../src/API/SettingsSubscriptions.cpp ../../src/API/HotkeySubscriptions.cpp ../../src/Bridge/MessageBridge.cpp ../../src/Settings/SettingsStore.cpp ../../src/Core/Json.cpp"
"control_map_policy_tests control_map_policy_tests.cpp ../../src/Bindings/ControlMapPolicy.cpp"
"localization_service_tests localization_service_tests.cpp ../../src/Localization/LocalizationService.cpp ../../src/Core/Json.cpp"
"view_manifest_tests view_manifest_tests.cpp ../../src/Views/ViewManifest.cpp ../../src/Core/Json.cpp"
"dev_view_files_tests dev_view_files_tests.cpp ../../src/Views/Dev/DevViewFiles.cpp"
"cursor_shape_tests cursor_shape_tests.cpp"
"gamepad_navigation_tests gamepad_navigation_tests.cpp"
"gamepad_session_tests gamepad_session_tests.cpp ../../src/Input/GamepadSession.cpp"
"browser_host_recovery_tests browser_host_recovery_tests.cpp"
"deferred_main_thread_work_tests deferred_main_thread_work_tests.cpp"
"output_size_observation_tests output_size_observation_tests.cpp"
"runtime_lifecycle_contract_tests runtime_lifecycle_contract_tests.cpp"
"view_policy_store_tests view_policy_store_tests.cpp ../../src/Views/ViewPolicyStore.cpp ../../src/Core/Json.cpp"
"view_reveal_gate_tests view_reveal_gate_tests.cpp ../../src/Views/ViewRevealGate.cpp"
"wv2_bounded_queue_tests wv2_bounded_queue_tests.cpp"
"wv2_messages_tests wv2_messages_tests.cpp ../../src/Core/Json.cpp"
"local_view_uri_tests local_view_uri_tests.cpp"
"v1_navigation_tests v1_navigation_tests.cpp"
"view_presentation_controller_tests view_presentation_controller_tests.cpp ../../src/Views/ViewPresentationController.cpp"
"wndproc_chain_tests wndproc_chain_tests.cpp"
"papyrus_call_tests papyrus_call_tests.cpp"
"ui_pass_policy_tests ui_pass_policy_tests.cpp"
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

if [[ "$MSVC" == 1 ]]; then
    # --- MSVC path: one generated cmd script runs vcvars64 once, compiles every
    # distinct TU in one /MP invocation, then links each suite. cl's /Fo<dir>
    # flattens objects to basenames, so refuse a basename collision up front.
    declare -A bybase=()
    for s in "${UNIQUE[@]}"; do
        b="$(basename "$s")"
        if [[ -n "${bybase[$b]:-}" ]]; then
            echo "MSVC mode: duplicate TU basename: $s vs ${bybase[$b]}" >&2
            exit 1
        fi
        bybase[$b]="$s"
    done
    HERE_W="$(cygpath -w "$(pwd)")"
    winsrc() { printf '%s\\%s' "$HERE_W" "${1//\//\\}"; }
    CMDFILE="$BUILD/msvc_build.cmd"
    {
        echo '@echo off'
        echo "call \"$(cygpath -w "$VSROOT")\\VC\\Auxiliary\\Build\\vcvars64.bat\" >nul || exit /b 1"
        echo "cd /d \"$HERE_W\\$BUILD\" || exit /b 1"
        FLAGS="/nologo /std:c++latest /EHsc /I \"$HERE_W\\..\\..\\src\" /I \"$HERE_W\\..\\..\\sdk\" /I \"$HERE_W\\..\\..\\tools\\webview2_shared\" /I \"$HERE_W\\$DEPS\" /I \"$HERE_W\\stubs\" /FI \"$HERE_W\\stubs\\pch.h\""
        printf 'cl %s /MP /c' "$FLAGS"
        for s in "${UNIQUE[@]}"; do printf ' "%s"' "$(winsrc "$s")"; done
        printf ' /Foobj\\ || exit /b 1\n'
        for suite in "${SUITES[@]}"; do
            read -r name srcs <<< "$suite"
            printf 'cl /nologo'
            for s in $srcs; do printf ' "obj\\%s.obj"' "$(basename "$s" .cpp)"; done
            printf ' /Fe:%s.exe || exit /b 1\n' "$name"
        done
    } > "$CMDFILE"
    echo "== MSVC (cl via vcvars64): compiling ${#UNIQUE[@]} translation units, linking ${#SUITES[@]} suites =="
    cmd //c "$(cygpath -w "$CMDFILE")"
else
    # --- Compile each distinct unit once, in parallel. xargs exits nonzero (so
    # --- the pipeline fails under `set -o pipefail`) if any compile fails.
    compile_one() {
        local src=$1 obj
        obj="$BUILD/obj/$(printf '%s' "$src" | tr './ ' '___').o"
        echo "  cc $src"
        $CXX $BASEFLAGS -include stubs/pch.h -c "$src" -o "$obj"
    }
    export -f compile_one
    export CXX BASEFLAGS BUILD

    JOBS="$(getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"
    echo "== compiling ${#UNIQUE[@]} translation units (-P $JOBS) =="
    printf '%s\n' "${UNIQUE[@]}" | xargs -P "$JOBS" -n1 -I{} bash -c 'compile_one "$1"' _ {}

    # --- Link each suite from its (already built) objects.
    echo "== linking ${#SUITES[@]} suites =="
    for suite in "${SUITES[@]}"; do
        read -r name srcs <<< "$suite"
        objs=()
        for s in $srcs; do objs+=("$(objname "$s")"); done
        $CXX $BASEFLAGS "${objs[@]}" -o "$BUILD/$name"
    done
fi

# --- Run. exit code = number of failing checks.
failures=0
for suite in "${SUITES[@]}"; do
    read -r name _ <<< "$suite"
    echo "== $name =="
    bin="$BUILD/$name"
    [[ -f "$bin" ]] || bin="$bin.exe"
    "$bin" || failures=$((failures + $?))
done
exit "$failures"
