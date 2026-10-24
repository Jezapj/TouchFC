set(TOUCHFC_FFMPEG_ROOT "${CMAKE_SOURCE_DIR}/third_party/ffmpeg" CACHE PATH "Prebuilt FFmpeg root (include/, lib/, bin/)")
set(TOUCHFC_HAS_FFMPEG OFF)
if(EXISTS "${TOUCHFC_FFMPEG_ROOT}/include/libavcodec/avcodec.h")
  set(TOUCHFC_HAS_FFMPEG ON)
endif()

# Opt-in: drop the ONNX Runtime Windows x64 package into third_party/onnxruntime.
# https://github.com/microsoft/onnxruntime/releases (onnxruntime-win-x64-*.zip).
set(TOUCHFC_ORT_ROOT "${CMAKE_SOURCE_DIR}/third_party/onnxruntime" CACHE PATH "Prebuilt ONNX Runtime root")
set(TOUCHFC_HAS_ORT OFF)
if(EXISTS "${TOUCHFC_ORT_ROOT}/include/onnxruntime_cxx_api.h")
  set(TOUCHFC_HAS_ORT ON)
endif()

set(TOUCHFC_SDK_VER "10.0.26100.0")
if(CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION)
  set(TOUCHFC_SDK_VER "${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}")
endif()
set(TOUCHFC_CPPWINRT "C:/Program Files (x86)/Windows Kits/10/Include/${TOUCHFC_SDK_VER}/cppwinrt")
if(NOT EXISTS "${TOUCHFC_CPPWINRT}/winrt/base.h")
  message(FATAL_ERROR "C++/WinRT headers not found at ${TOUCHFC_CPPWINRT}")
endif()

include(FetchContent)
FetchContent_Declare(
  imgui
  GIT_REPOSITORY https://github.com/ocornut/imgui.git
  GIT_TAG v1.91.8
  GIT_SHALLOW TRUE)
FetchContent_GetProperties(imgui)
if(NOT imgui_POPULATED)
  FetchContent_Populate(imgui)
endif()

add_library(touchfc_imgui STATIC
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp)
target_include_directories(touchfc_imgui PUBLIC
  ${imgui_SOURCE_DIR}
  ${imgui_SOURCE_DIR}/backends)
target_compile_definitions(touchfc_imgui PUBLIC IMGUI_IMPL_WIN32_DISABLE_GAMEPAD)
if(MSVC)
  target_compile_options(touchfc_imgui PRIVATE /W3 /EHsc)
endif()

add_executable(touchfc
  src/main.cpp
  src/pipeline.cpp
  src/present.cpp
  src/d3d_util.cpp
  src/threads_win.cpp
  src/selftest_source.cpp
  src/replay_source.cpp
  src/wgc_source.cpp
  src/stub_detector.cpp
  src/ort_detector.cpp)
target_include_directories(touchfc PRIVATE
  src
  ${TOUCHFC_CPPWINRT})
target_link_libraries(touchfc PRIVATE
  touchfc_hdr
  touchfc_imgui
  d3d11
  dxgi
  d3dcompiler
  dwmapi
  dcomp
  windowsapp
  avrt
  ole32
  uuid)
target_compile_definitions(touchfc PRIVATE
  WIN32_LEAN_AND_MEAN
  NOMINMAX
  UNICODE
  _UNICODE
  _CRT_SECURE_NO_WARNINGS)
if(TOUCHFC_HAS_FFMPEG)
  target_compile_definitions(touchfc PRIVATE TOUCHFC_HAS_FFMPEG=1)
  target_include_directories(touchfc PRIVATE "${TOUCHFC_FFMPEG_ROOT}/include")
  target_link_directories(touchfc PRIVATE "${TOUCHFC_FFMPEG_ROOT}/lib")
  target_link_libraries(touchfc PRIVATE avcodec avformat avutil swscale swresample)
  file(GLOB TOUCHFC_FFMPEG_DLLS "${TOUCHFC_FFMPEG_ROOT}/bin/*.dll")
  foreach(dll ${TOUCHFC_FFMPEG_DLLS})
    add_custom_command(TARGET touchfc POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different "${dll}" "$<TARGET_FILE_DIR:touchfc>")
  endforeach()
else()
  message(STATUS "FFmpeg headers not found; replay mode will report an error. Run scripts/fetch_ffmpeg.ps1.")
endif()
if(TOUCHFC_HAS_ORT)
  target_compile_definitions(touchfc PRIVATE TOUCHFC_HAS_ORT=1)
  target_include_directories(touchfc PRIVATE "${TOUCHFC_ORT_ROOT}/include")
  target_link_directories(touchfc PRIVATE "${TOUCHFC_ORT_ROOT}/lib")
  target_link_libraries(touchfc PRIVATE onnxruntime)
  file(GLOB TOUCHFC_ORT_DLLS "${TOUCHFC_ORT_ROOT}/lib/*.dll")
  foreach(dll ${TOUCHFC_ORT_DLLS})
    add_custom_command(TARGET touchfc POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different "${dll}" "$<TARGET_FILE_DIR:touchfc>")
  endforeach()
else()
  message(STATUS "ONNX Runtime not found; stub detector only. Drop onnxruntime-win-x64-*.zip into third_party/onnxruntime/.")
endif()
# MSVC enables AVX2 for preprocess code paths at /arch:AVX2. The runtime still
# falls back to scalar when the dispatcher is called with a non-mul-8 width.
if(MSVC)
  target_compile_options(touchfc PRIVATE /arch:AVX2)
endif()
touchfc_warnings(touchfc)
set_target_properties(touchfc PROPERTIES CXX_STANDARD 20)
if(MSVC)
  target_compile_options(touchfc PRIVATE /bigobj)
endif()
