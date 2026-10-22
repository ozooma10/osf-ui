#!/usr/bin/env bash
# Native desktop unit tests: compiles the real runtime sources against stubs/pch.h and runs them
# without SFSE or the game. Needs C++23; fetches pinned nlohmann/json headers on first run.
# Each distinct translation unit compiles once, in parallel; suites only re-link.
set -euo pipefail
cd "$(dirname "$0")"

DEPS=.deps
BUILD=.build
NLOHMANN_VERSION=v3.11.3

# Rebuilt from scratch every run; this also evicts binaries of removed suites.
rm -rf "$BUILD"
mkdir -p "$DEPS/nlohmann" "$BUILD/obj"
for header in json.hpp json_fwd.hpp; do
    if [[ ! -s "$DEPS/nlohmann/$header" ]]; then
        echo "fetching nlohmann/$header $NLOHMANN_VERSION ..."
        curl -fsSL --max-time 120 \
            -o "$DEPS/nlohmann/$header.tmp" \
            "https://raw.githubusercontent.com/nlohmann/json/$NLOHMANN_VERSION/single_include/nlohmann/$header"
        mv -f "$DEPS/nlohmann/$header.tmp" "$DEPS/nlohmann/$header"
    fi
done

# Toolchain: CXX (default clang++); falls back to MSVC cl via vcvars64 when no clang/g++ is on PATH.
MSVC=0
if [[ -z "${CXX:-}" ]] && ! command -v clang++ >/dev/null 2>&1 && ! command -v g++ >/dev/null 2>&1; then
    VSWHERE="/c/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe"
    if [[ -x "$VSWHERE" ]]; then
        VSROOT="$("$VSWHERE" -latest -property installationPath | tr -d '\r')"
        [[ -n "$VSROOT" ]] && MSVC=1
    fi
fi
CXX="${CXX:-clang++}"
BASEFLAGS="-std=c++2b -Wall -Wextra -g -I stubs -I ../../src -I ../../sdk -I ../../tools/browser_shared -I $DEPS"

# Each suite is "<name> <translation units...>"; units shared across suites compile once.
SUITES=(
"json_tests json_tests.cpp ../../src/Core/Json.cpp"
"settings_client_tests settings_client_tests.cpp ../../src/Dependency/OSFSettingsClient.cpp ../../src/API/BridgeApi.cpp ../../src/Views/ViewRequestQueue.cpp ../../src/Bridge/MessageBridge.cpp ../../src/Core/Json.cpp"
"settings_view_example_tests settings_view_example_tests.cpp ../../examples/settings-view/src/SettingsViewConsumer.cpp ../../src/API/BridgeApi.cpp ../../src/Views/ViewRequestQueue.cpp ../../src/Bridge/MessageBridge.cpp ../../src/Core/Json.cpp"
"bridge_api_tests bridge_api_tests.cpp ../../src/API/BridgeApi.cpp ../../src/Views/ViewRequestQueue.cpp ../../src/Bridge/MessageBridge.cpp ../../src/Core/Json.cpp"
"api_version_tests api_version_tests.cpp ../../src/API/Exports.cpp ../../src/API/BridgeApi.cpp ../../src/Views/ViewRequestQueue.cpp ../../src/Bridge/MessageBridge.cpp ../../src/Core/Json.cpp"
"papyrus_form_tests papyrus_form_tests.cpp ../../src/Compat/V1/PapyrusAdapter.cpp ../../src/API/PapyrusApi.cpp ../../src/API/BridgeApi.cpp ../../src/Views/ViewRequestQueue.cpp ../../src/Bridge/MessageBridge.cpp ../../src/Bridge/RetainedStateStore.cpp ../../src/Core/Json.cpp"
"view_manifest_tests view_manifest_tests.cpp ../../src/Views/ViewManifest.cpp ../../src/Views/ViewManager.cpp ../../src/Core/Json.cpp"
"dev_view_files_tests dev_view_files_tests.cpp ../../src/Views/Dev/DevViewFiles.cpp ../../src/Views/ViewCache.cpp"
"view_cache_tests view_cache_tests.cpp ../../src/Views/ViewCache.cpp"
"absolute_mouse_mapping_tests absolute_mouse_mapping_tests.cpp"
"adaptive_view_geometry_tests adaptive_view_geometry_tests.cpp"
"gamepad_navigation_tests gamepad_navigation_tests.cpp"
"gamepad_session_tests gamepad_session_tests.cpp ../../src/Input/GamepadSession.cpp ../../src/Input/ViewInputGrants.cpp"
"browser_host_recovery_tests browser_host_recovery_tests.cpp"
"browser_host_retirement_tests browser_host_retirement_tests.cpp"
"browser_backend_tests browser_backend_tests.cpp"
"shared_frame_state_tests shared_frame_state_tests.cpp"
"runtime_lifecycle_tests runtime_lifecycle_tests.cpp runtime_doubles.cpp surface_frames_double.cpp ../../src/Surfaces/SurfaceService.cpp ../../src/Views/ViewLoadTracker.cpp ../../src/Views/ViewRecoveryTracker.cpp ../../src/Runtime/Runtime.cpp ../../src/Runtime/RuntimeFrame.cpp ../../src/Views/RuntimeViews.cpp ../../src/Bridge/RuntimeBridge.cpp ../../src/Input/RuntimeInput.cpp ../../src/Input/InputCaptureController.cpp ../../src/Input/PointerInputState.cpp ../../src/Input/RelativePointerSession.cpp ../../src/Input/GamepadSession.cpp ../../src/Input/ViewInputGrants.cpp ../../src/Views/ViewManifest.cpp ../../src/Views/ViewManager.cpp ../../src/Views/ViewPresentationController.cpp ../../src/Views/ViewOpenCoordinator.cpp ../../src/Views/ViewRevealGate.cpp ../../src/Views/ViewRequestQueue.cpp ../../src/API/BridgeApi.cpp ../../src/Bridge/MessageBridge.cpp ../../src/Bridge/RetainedStateStore.cpp ../../src/Dependency/OSFSettingsClient.cpp ../../src/Core/Json.cpp"
"view_reveal_gate_tests view_reveal_gate_tests.cpp ../../src/Views/ViewRevealGate.cpp"
"wv2_bounded_queue_tests wv2_bounded_queue_tests.cpp"
"wv2_messages_tests wv2_messages_tests.cpp ../../src/Core/Json.cpp"
"wv2_cdp_input_tests wv2_cdp_input_tests.cpp ../../src/Core/Json.cpp"
"wv2_mouse_buttons_tests wv2_mouse_buttons_tests.cpp"
"wv2_capture_performance_tests wv2_capture_performance_tests.cpp"
"wv2_performance_gate_tests wv2_performance_gate_tests.cpp"
"local_view_uri_tests local_view_uri_tests.cpp"
"remote_uri_tests remote_uri_tests.cpp"
"browser_surface_tests browser_surface_tests.cpp"
"browser_audio_tests browser_audio_tests.cpp"
"wv2_media_navigation_tests wv2_media_navigation_tests.cpp ../../src/Core/Json.cpp"
"cef_navigation_tests cef_navigation_tests.cpp"
"cef_assets_tests cef_assets_tests.cpp"
"cef_frame_pacer_tests cef_frame_pacer_tests.cpp"
"cef_composition_tests cef_composition_tests.cpp"
"cef_paint_damage_tests cef_paint_damage_tests.cpp"
"view_presentation_controller_tests view_presentation_controller_tests.cpp ../../src/Views/ViewPresentationController.cpp"
"view_open_coordinator_tests view_open_coordinator_tests.cpp ../../src/Views/ViewOpenCoordinator.cpp ../../src/Views/ViewPresentationController.cpp"
"view_request_queue_tests view_request_queue_tests.cpp ../../src/Views/ViewRequestQueue.cpp"
"wndproc_chain_tests wndproc_chain_tests.cpp"
"papyrus_call_tests papyrus_call_tests.cpp"
)

objname() { printf '%s/obj/%s.o' "$BUILD" "$(printf '%s' "$1" | tr './ ' '___')"; }

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
    # MSVC: one cmd script runs vcvars64 once, compiles every TU with /MP, then links each suite.
    # /Fo<dir> flattens objects to basenames, so reject basename collisions.
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
        FLAGS="/nologo /std:c++latest /EHsc /I \"$HERE_W\\stubs\" /I \"$HERE_W\\..\\..\\src\" /I \"$HERE_W\\..\\..\\sdk\" /I \"$HERE_W\\..\\..\\tools\\browser_shared\" /I \"$HERE_W\\$DEPS\" /FI \"$HERE_W\\stubs\\pch.h\""
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
    # Compile each distinct unit once, in parallel; xargs fails the pipeline if any compile fails.
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

    echo "== linking ${#SUITES[@]} suites =="
    for suite in "${SUITES[@]}"; do
        read -r name srcs <<< "$suite"
        objs=()
        for s in $srcs; do objs+=("$(objname "$s")"); done
        $CXX $BASEFLAGS "${objs[@]}" -o "$BUILD/$name"
    done
fi

# Run. Exit code = number of failing checks.
failures=0
for suite in "${SUITES[@]}"; do
    read -r name _ <<< "$suite"
    echo "== $name =="
    bin="$BUILD/$name"
    [[ -f "$bin" ]] || bin="$bin.exe"
    if [[ "$name" == runtime_lifecycle_tests ]]; then
        for scenario in normal developer-mode stale-selection missing-settings renderer-init-failure input-hook-failure focus-menu-failure menu-events-failure suppression-failure recovery game-device-failure surfaces surface-input surface-fullscreen browser-surface browser-disabled browser-consent-on browser-consent-off browser-consent-failed surface-reload; do
            "$bin" "$scenario" || failures=$((failures + $?))
        done
    else
        "$bin" || failures=$((failures + $?))
    fi
done
if (( failures > 0 )); then
    echo "native checks failed: $failures" >&2
    exit 1
fi
exit 0
