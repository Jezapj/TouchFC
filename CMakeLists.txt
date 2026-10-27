cmake_minimum_required(VERSION 3.24)

project(touchfc LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(TOUCHFC_BUILD_TESTS "Build core tests" ON)
option(TOUCHFC_BUILD_BENCH "Build core benchmarks" ON)
option(TOUCHFC_BUILD_APP "Build the Windows app" ON)
option(TOUCHFC_TSAN "Build tests with ThreadSanitizer (clang/gcc)" OFF)
option(TOUCHFC_USE_VCPKG "Find Catch2, benchmark and hdr via vcpkg" OFF)

if(TOUCHFC_TSAN AND NOT WIN32)
  add_compile_options(-fsanitize=thread -g -O1 -fno-omit-frame-pointer)
  add_link_options(-fsanitize=thread)
endif()

function(touchfc_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive- /Zc:__cplusplus /EHsc /utf-8 /wd4324)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
  endif()
endfunction()

add_library(touchfc_headers INTERFACE)
target_include_directories(touchfc_headers INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/core/include)

if(TOUCHFC_USE_VCPKG)
  find_package(Catch2 CONFIG REQUIRED)
  find_package(benchmark CONFIG REQUIRED)
  find_package(hdr_histogram CONFIG REQUIRED)
  find_package(nlohmann_json CONFIG REQUIRED)
else()
  include(FetchContent)
  set(CATCH_INSTALL_DOCS OFF CACHE BOOL "" FORCE)
  set(CATCH_INSTALL_EXTRAS OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG v3.7.1
    GIT_SHALLOW TRUE)
  set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(
    benchmark
    GIT_REPOSITORY https://github.com/google/benchmark.git
    GIT_TAG v1.9.1
    GIT_SHALLOW TRUE)
  set(HDR_HISTOGRAM_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
  set(HDR_HISTOGRAM_BUILD_SHARED OFF CACHE BOOL "" FORCE)
  set(HDR_HISTOGRAM_INSTALL_SHARED OFF CACHE BOOL "" FORCE)
  set(HDR_HISTOGRAM_INSTALL_STATIC OFF CACHE BOOL "" FORCE)
  set(HDR_LOG_REQUIRED "DISABLED" CACHE STRING "" FORCE)
  FetchContent_Declare(
    hdr_histogram_proj
    GIT_REPOSITORY https://github.com/HdrHistogram/HdrHistogram_c.git
    GIT_TAG 0.11.8
    GIT_SHALLOW TRUE)
  if(TOUCHFC_BUILD_TESTS)
    FetchContent_MakeAvailable(Catch2)
  endif()
  if(TOUCHFC_BUILD_BENCH)
    FetchContent_MakeAvailable(benchmark)
  endif()
  FetchContent_MakeAvailable(hdr_histogram_proj)
  # Profiles (M10). The release tarball is the single-include header tree only.
  FetchContent_Declare(
    nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
    URL_HASH SHA256=d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d)
  FetchContent_MakeAvailable(nlohmann_json)
endif()
target_link_libraries(touchfc_headers INTERFACE nlohmann_json::nlohmann_json)

if(TARGET hdr_histogram_static)
  set(TOUCHFC_HDR_TARGET hdr_histogram_static)
elseif(TARGET hdr_histogram::hdr_histogram_static)
  set(TOUCHFC_HDR_TARGET hdr_histogram::hdr_histogram_static)
elseif(TARGET hdr_histogram::hdr_histogram)
  set(TOUCHFC_HDR_TARGET hdr_histogram::hdr_histogram)
elseif(TARGET hdr_histogram)
  set(TOUCHFC_HDR_TARGET hdr_histogram)
else()
  message(FATAL_ERROR "HdrHistogram target not found")
endif()

add_library(touchfc_hdr STATIC core/src/hdr_sink.cpp)
target_link_libraries(touchfc_hdr PUBLIC touchfc_headers ${TOUCHFC_HDR_TARGET})
target_compile_definitions(touchfc_hdr PRIVATE _CRT_SECURE_NO_WARNINGS)
touchfc_warnings(touchfc_hdr)

if(TOUCHFC_BUILD_TESTS)
  add_executable(touchfc_tests core/tests/test_core.cpp)
  target_link_libraries(touchfc_tests PRIVATE touchfc_hdr Catch2::Catch2WithMain)
  touchfc_warnings(touchfc_tests)
  if(NOT TOUCHFC_USE_VCPKG)
    # Catch2WithMain is available after FetchContent.
  endif()
  enable_testing()
  add_test(NAME touchfc_tests COMMAND $<TARGET_FILE:touchfc_tests>)
endif()

if(TOUCHFC_BUILD_BENCH)
  add_executable(touchfc_bench core/bench/bench_main.cpp)
  target_link_libraries(touchfc_bench PRIVATE touchfc_headers benchmark::benchmark)
  touchfc_warnings(touchfc_bench)
  # Preprocess fast path needs AVX2 to be enabled by the compiler. The
  # dispatcher still falls back to scalar at runtime for non-mul-8 widths.
  if(MSVC)
    target_compile_options(touchfc_bench PRIVATE /arch:AVX2)
  else()
    target_compile_options(touchfc_bench PRIVATE -mavx2)
  endif()
endif()

if(TOUCHFC_BUILD_TESTS)
  if(MSVC)
    target_compile_options(touchfc_tests PRIVATE /arch:AVX2)
  else()
    target_compile_options(touchfc_tests PRIVATE -mavx2)
  endif()
endif()

if(TOUCHFC_BUILD_APP AND WIN32)
  add_subdirectory(app)
endif()
