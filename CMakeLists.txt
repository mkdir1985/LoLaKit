cmake_minimum_required(VERSION 3.25)

project(LoLaKit VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
option(LOLAKIT_BUILD_BENCHMARKS "Build benchmark executables" ON)

add_library(lolakit_core INTERFACE)
add_library(lolakit::core ALIAS lolakit_core)

target_include_directories(
  lolakit_core
  INTERFACE
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_compile_features(lolakit_core INTERFACE cxx_std_17)

if(MSVC)
  target_compile_options(
    lolakit_core
    INTERFACE
      /permissive-
      /Zc:__cplusplus
      /Zc:preprocessor
      /W4
      /wd4324
  )
else()
  target_compile_options(
    lolakit_core
    INTERFACE
      -Wall
      -Wextra
      -Wno-padded
      -Wno-c99-extensions
  )
endif()

enable_testing()

add_executable(lolakit_core_smoke tests/compile_core_smoke.cpp)
target_link_libraries(lolakit_core_smoke PRIVATE lolakit::core)

add_executable(lolakit_core_unit_tests tests/core_unit_tests.cpp)
target_link_libraries(lolakit_core_unit_tests PRIVATE lolakit::core)

if(MSVC)
  target_compile_options(lolakit_core_smoke PRIVATE /EHsc)
  target_compile_options(lolakit_core_unit_tests PRIVATE /EHsc)
endif()

if(NOT WIN32)
  find_package(Threads REQUIRED)
  target_link_libraries(lolakit_core_smoke PRIVATE Threads::Threads)
  target_link_libraries(lolakit_core_unit_tests PRIVATE Threads::Threads)
endif()

add_test(NAME lolakit_core_smoke COMMAND lolakit_core_smoke)
add_test(NAME lolakit_core_unit_tests COMMAND lolakit_core_unit_tests)

if(LOLAKIT_BUILD_BENCHMARKS)
  add_executable(lolakit_core_benchmark benchmarks/core_benchmark.cpp)
  target_link_libraries(lolakit_core_benchmark PRIVATE lolakit::core)

  if(MSVC)
    target_compile_options(lolakit_core_benchmark PRIVATE /EHsc)
  endif()

  if(NOT WIN32)
    find_package(Threads REQUIRED)
    target_link_libraries(lolakit_core_benchmark PRIVATE Threads::Threads)
  endif()

  if(WIN32)
    add_executable(lolakit_io_benchmark benchmarks/io_benchmark.cpp)
    target_link_libraries(lolakit_io_benchmark PRIVATE lolakit::core)

    if(MSVC)
      target_compile_options(lolakit_io_benchmark PRIVATE /EHsc)
    endif()
  endif()
endif()

