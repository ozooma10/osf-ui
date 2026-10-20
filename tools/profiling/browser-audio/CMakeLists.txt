cmake_minimum_required(VERSION 3.25)
project(osfui_browser_audio_probe LANGUAGES CXX)
add_executable(osfui_browser_audio_probe main.cpp)
target_compile_features(osfui_browser_audio_probe PRIVATE cxx_std_20)
target_compile_definitions(osfui_browser_audio_probe PRIVATE
    WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00 NTDDI_VERSION=0x0A00000A)
target_compile_options(osfui_browser_audio_probe PRIVATE /W4 /EHsc /utf-8)
target_include_directories(osfui_browser_audio_probe PRIVATE "${JSON_INCLUDE}")
target_link_libraries(osfui_browser_audio_probe PRIVATE ole32 mmdevapi)
