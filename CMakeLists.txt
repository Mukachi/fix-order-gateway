cmake_minimum_required(VERSION 3.28)
project(fix-order-gateway LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_library(fixgw INTERFACE)
target_include_directories(fixgw INTERFACE include)
target_compile_options(fixgw INTERFACE -Wall -Wextra -Wpedantic -Werror)
# x86-64-v3 (AVX2, BMI1/2, LZCNT, MOVBE, FMA): the codec's SIMD paths (note 009).
# A named level, recorded in every run's flags; other targets use the scalar paths.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
  set(FIXGW_ARCH_FLAGS -march=x86-64-v3)
  target_compile_options(fixgw INTERFACE ${FIXGW_ARCH_FLAGS})
endif()

find_package(Threads REQUIRED)
find_package(Python3 COMPONENTS Interpreter)
enable_testing()

# lockfree-ipc: its queues and measurement harness (note 001), fetched at a
# pinned commit into build/_deps. Headers only: SOURCE_SUBDIR names no
# directory, so its own tests, benchmarks and baselines are not built here.
include(FetchContent)
set(FIXGW_LFIPC_COMMIT 1a42550fae76b10ce41094017c8184914e3a9f04)
FetchContent_Declare(lockfree_ipc
  GIT_REPOSITORY https://github.com/Mukachi/lockfree-ipc.git
  GIT_TAG ${FIXGW_LFIPC_COMMIT}
  GIT_SHALLOW FALSE
  SOURCE_SUBDIR none)
FetchContent_MakeAvailable(lockfree_ipc)

# Build provenance for this repository, written by lockfree-ipc's script into
# lfipc/build_info.hpp, where its environment capture reads it (lockfree-ipc
# note 004). The harness commit is recorded beside it.
string(TOUPPER "${CMAKE_BUILD_TYPE}" build_type_upper)
add_custom_target(build_info ALL
  COMMAND ${CMAKE_COMMAND} -DSRC=${CMAKE_SOURCE_DIR} -DOUT=${CMAKE_BINARY_DIR}/generated/lfipc/build_info.hpp
          -DBUILD_TYPE=${CMAKE_BUILD_TYPE}
          "-DFLAGS=${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_${build_type_upper}} -Wall -Wextra -Wpedantic -Werror ${FIXGW_ARCH_FLAGS}"
          -P ${lockfree_ipc_SOURCE_DIR}/cmake/build_info.cmake
  BYPRODUCTS ${CMAKE_BINARY_DIR}/generated/lfipc/build_info.hpp
  COMMENT "Recording build provenance")
add_library(lfipc INTERFACE)
target_include_directories(lfipc SYSTEM INTERFACE ${lockfree_ipc_SOURCE_DIR}/include ${CMAKE_BINARY_DIR}/generated)
target_compile_definitions(lfipc INTERFACE FIXGW_LFIPC_COMMIT="${FIXGW_LFIPC_COMMIT}")
add_dependencies(lfipc build_info)

foreach(t framing_test decode_test encode_test representation_a_test simd_equivalence_test session_test)
  add_executable(${t} tests/${t}.cpp)
  target_link_libraries(${t} PRIVATE fixgw)
  add_test(NAME ${t} COMMAND ${t})
endforeach()

add_executable(hot_path_subject tests/hot_path_subject.cpp)
target_link_libraries(hot_path_subject PRIVATE fixgw)
if(Python3_FOUND AND CMAKE_OBJDUMP AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64"
   AND NOT CMAKE_CXX_FLAGS MATCHES "-fsanitize")
  add_test(NAME check_hot_path
           COMMAND Python3::Interpreter -I ${CMAKE_SOURCE_DIR}/tests/check_hot_path.py
                   ${CMAKE_OBJDUMP} $<TARGET_FILE:hot_path_subject>)
endif()

# Fuzzing (note 002, S7): clang only, a separate build directory, e.g.
#   CXX=clang++ cmake -S . -B build-fuzz -DFIXGW_FUZZ=ON && cmake --build build-fuzz --target fuzz_codec
option(FIXGW_FUZZ "Build the libFuzzer targets (clang)" OFF)
if(FIXGW_FUZZ)
  if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    message(FATAL_ERROR "FIXGW_FUZZ needs clang (libFuzzer)")
  endif()
  add_executable(fuzz_codec fuzz/fuzz_codec.cpp)
  target_link_libraries(fuzz_codec PRIVATE fixgw)
  target_compile_options(fuzz_codec PRIVATE -g -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=all)
  target_link_options(fuzz_codec PRIVATE -fsanitize=fuzzer,address,undefined)
  add_executable(fuzz_diff fuzz/fuzz_diff.cpp)
  target_link_libraries(fuzz_diff PRIVATE fixgw)
  target_compile_options(fuzz_diff PRIVATE -g -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=all)
  target_link_options(fuzz_diff PRIVATE -fsanitize=fuzzer,address,undefined)
  add_executable(fuzz_session fuzz/fuzz_session.cpp)
  target_link_libraries(fuzz_session PRIVATE fixgw)
  target_compile_options(fuzz_session PRIVATE -g -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=all)
  target_link_options(fuzz_session PRIVATE -fsanitize=fuzzer,address,undefined)
endif()

add_executable(codec_bench bench/codec_bench.cpp)
target_link_libraries(codec_bench PRIVATE fixgw lfipc Threads::Threads)
target_compile_definitions(codec_bench PRIVATE LFIPC_RESULTS_DIR="${CMAKE_SOURCE_DIR}/results")

# One number everywhere: a stale RESULTS.md or README figure fails the suite.
if(Python3_FOUND)
  add_test(NAME report_check COMMAND Python3::Interpreter -I ${CMAKE_SOURCE_DIR}/run.py report --check)
endif()
