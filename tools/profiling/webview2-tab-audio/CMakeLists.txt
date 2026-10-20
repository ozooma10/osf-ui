cmake_minimum_required(VERSION 3.25)
project(osfui_webview2_tab_audio_probe LANGUAGES CXX)
set(HOST_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../webview2_host")
set(AUDIO_HEADERS "${CMAKE_CURRENT_BINARY_DIR}/production-audio-assets")
file(MAKE_DIRECTORY "${AUDIO_HEADERS}")
foreach(asset manifest.json collector.html collector.js)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${HOST_ROOT}/audio-extension/${asset}")
    file(READ "${HOST_ROOT}/audio-extension/${asset}" asset_hex HEX)
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," asset_bytes "${asset_hex}")
    file(WRITE "${AUDIO_HEADERS}/${asset}.h" "${asset_bytes}0x00\n")
endforeach()
add_executable(osfui_webview2_tab_audio_probe main.cpp AudioMeter.cpp
    "${HOST_ROOT}/SpatialAudio.cpp" "${HOST_ROOT}/EmbeddedAudioExtension.cpp")
target_compile_features(osfui_webview2_tab_audio_probe PRIVATE cxx_std_20)
target_compile_definitions(osfui_webview2_tab_audio_probe PRIVATE
    WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
target_compile_options(osfui_webview2_tab_audio_probe PRIVATE /W4 /EHsc /utf-8)
target_include_directories(osfui_webview2_tab_audio_probe PRIVATE
    "${JSON_INCLUDE}" "${WEBVIEW2_SDK}/build/native/include"
    "${HOST_ROOT}" "${CMAKE_CURRENT_SOURCE_DIR}/../../browser_shared" "${AUDIO_HEADERS}")
target_link_libraries(osfui_webview2_tab_audio_probe PRIVATE
    "${WEBVIEW2_SDK}/build/native/x64/WebView2LoaderStatic.lib"
    ole32 oleaut32 user32 shlwapi version mmdevapi)
