cmake_minimum_required(VERSION 3.25)
project(osfui_cef_host LANGUAGES C CXX)
if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "The CEF host requires Windows x64")
endif()
set(CMAKE_CONFIGURATION_TYPES Release)
set(USE_SANDBOX ON CACHE BOOL "Use the bundled sandbox bootstrap" FORCE)
set(USE_ATL OFF CACHE BOOL "ATL is not needed" FORCE)
set(CEF_RUNTIME_LIBRARY_FLAG /MT CACHE STRING "Static runtime" FORCE)
set(api_version 15000)
list(APPEND CMAKE_MODULE_PATH "${CEF_ROOT}/cmake")
find_package(CEF REQUIRED)
add_subdirectory("${CEF_LIBCEF_DLL_WRAPPER_PATH}" libcef_dll_wrapper)
set(REPO "${CMAKE_CURRENT_SOURCE_DIR}/../..")
if(NOT EXISTS "${JSON_INCLUDE}/nlohmann/json.hpp")
  message(FATAL_ERROR "Set JSON_INCLUDE to the nlohmann_json include directory")
endif()

# CEF 138+ supplies the sandbox in bootstrap.exe. Our entry point lives in a DLL.
add_library(osfui_cef_host SHARED Host.cpp Graphics.cpp SpatialAudio.cpp
  "${REPO}/tools/browser_shared/BrowserPipe.cpp")
SET_LIBRARY_TARGET_PROPERTIES(osfui_cef_host)
target_compile_features(osfui_cef_host PRIVATE cxx_std_23)
target_compile_options(osfui_cef_host PRIVATE /EHsc /utf-8)
target_compile_definitions(osfui_cef_host PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_include_directories(osfui_cef_host PRIVATE "${CEF_ROOT}" "${JSON_INCLUDE}"
  "${REPO}/src" "${REPO}/tools/browser_shared" "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(osfui_cef_host PRIVATE libcef_dll_wrapper "${CEF_ROOT}/Release/libcef.lib"
  ${CEF_STANDARD_LIBS} d3d11 dxgi d3dcompiler user32 shell32 ole32 oleaut32 uuid advapi32 taskschd comsuppw)
target_link_libraries(osfui_cef_host PRIVATE xaudio2)


# Embed the scripts so the helper and bridge are always one matched payload.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/bridge.js" BRIDGE_SCRIPT)
file(READ "${REPO}/tools/webview2_host/scripts/network-guard.js" NETWORK_SCRIPT)
file(READ "${REPO}/src/Compat/V1/web-transport.js" LEGACY_SCRIPT)
file(READ "${REPO}/tools/browser_shared/performance-overlay.js" PERFORMANCE_SCRIPT)
configure_file(Scripts.h.in Scripts.h @ONLY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  bridge.js "${REPO}/tools/webview2_host/scripts/network-guard.js"
  "${REPO}/src/Compat/V1/web-transport.js" "${REPO}/tools/browser_shared/performance-overlay.js")

set(RUNTIME_DIR "${CMAKE_BINARY_DIR}/runtime")
set_target_properties(osfui_cef_host PROPERTIES RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/host")
add_custom_target(stage_cef_runtime ALL
  COMMAND ${CMAKE_COMMAND} -E remove_directory "${RUNTIME_DIR}"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${RUNTIME_DIR}"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:osfui_cef_host>" "${RUNTIME_DIR}/osfui_cef_host.dll"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CEF_ROOT}/Release/bootstrap.exe" "${RUNTIME_DIR}/osfui_cef_host.exe"
  COMMAND ${CMAKE_COMMAND} -E copy_directory "${CEF_ROOT}/Resources" "${RUNTIME_DIR}"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CEF_ROOT}/LICENSE.txt" "${RUNTIME_DIR}/CEF-LICENSE.txt"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CEF_ROOT}/CREDITS.html" "${RUNTIME_DIR}/CREDITS.html"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/cef.lock.json" "${RUNTIME_DIR}/cef.lock.json"
  DEPENDS osfui_cef_host)
foreach(FILE IN LISTS CEF_BINARY_FILES)
  add_custom_command(TARGET stage_cef_runtime POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CEF_ROOT}/Release/${FILE}" "${RUNTIME_DIR}/${FILE}")
endforeach()
