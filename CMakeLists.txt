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
find_package(Python3 REQUIRED COMPONENTS Interpreter)
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
# The FIX 4.4 dictionary the session checks against (note 006, D1, D2): the FIX
# Trading Community's orchestrations (Apache-2.0) at a pinned commit, fetched
# into build/_deps, checked by hash, and turned into a header at build time.
set(FIXGW_ORCHESTRA_COMMIT cd24169a2abd8daba7c360987c7a46ca11873a12)
set(FIXGW_ORCHESTRA_DIR ${CMAKE_BINARY_DIR}/_deps/fix_orchestrations)
set(FIXGW_ORCHESTRA_FILES OrchestraFIX44.xml FIX44Session.xml)
set(FIXGW_ORCHESTRA_SHA256 a36262895e90bbcad2948e0c98072173a571a253ba63107a89617441c67a9f86
                           90232af493951c0d88447725712cb620056374e87cd019f4e26b11c51889e475)
foreach(name hash IN ZIP_LISTS FIXGW_ORCHESTRA_FILES FIXGW_ORCHESTRA_SHA256)
  if(NOT EXISTS ${FIXGW_ORCHESTRA_DIR}/${name})
    file(DOWNLOAD "https://raw.githubusercontent.com/FIXTradingCommunity/orchestrations/${FIXGW_ORCHESTRA_COMMIT}/FIX%20Standard/${name}"
         ${FIXGW_ORCHESTRA_DIR}/${name} EXPECTED_HASH SHA256=${hash} TLS_VERIFY ON)
  endif()
endforeach()
add_custom_command(
  OUTPUT ${CMAKE_BINARY_DIR}/generated/fixgw/fix44_dictionary.hpp
  COMMAND Python3::Interpreter -I ${CMAKE_SOURCE_DIR}/tools/gen_dictionary.py
          ${FIXGW_ORCHESTRA_DIR}/OrchestraFIX44.xml ${FIXGW_ORCHESTRA_DIR}/FIX44Session.xml
          ${FIXGW_ORCHESTRA_COMMIT} ${CMAKE_BINARY_DIR}/generated/fixgw/fix44_dictionary.hpp
  DEPENDS ${CMAKE_SOURCE_DIR}/tools/gen_dictionary.py ${FIXGW_ORCHESTRA_DIR}/OrchestraFIX44.xml
          ${FIXGW_ORCHESTRA_DIR}/FIX44Session.xml
  COMMENT "Generating the FIX 4.4 dictionary")
add_custom_target(fix44_dictionary DEPENDS ${CMAKE_BINARY_DIR}/generated/fixgw/fix44_dictionary.hpp)
target_include_directories(fixgw INTERFACE ${CMAKE_BINARY_DIR}/generated)
add_dependencies(fixgw fix44_dictionary)

add_library(lfipc INTERFACE)
target_include_directories(lfipc SYSTEM INTERFACE ${lockfree_ipc_SOURCE_DIR}/include ${CMAKE_BINARY_DIR}/generated)
target_compile_definitions(lfipc INTERFACE FIXGW_LFIPC_COMMIT="${FIXGW_LFIPC_COMMIT}")
add_dependencies(lfipc build_info)

foreach(t conformance_test orders_test matrices_test replay_test indoubt_test framing_test decode_test encode_test representation_a_test simd_equivalence_test session_test journal_test recovery_test durability_test crash_test transport_test acceptor_gate_test engine_test pipeline_test engine_crash_test engine_alloc_test)
  add_executable(${t} tests/${t}.cpp)
  target_link_libraries(${t} PRIVATE fixgw lfipc Threads::Threads)  # the engine uses lockfree-ipc's rings
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

# Interoperability with QuickFIX (note 006, D6): v1.16.0 fetched and built in
# build/ (never committed), with its example programs. Off by default: the
# build takes minutes. Its source is not read; it is run as a black box, and
# interop/qf_initiator.cpp is written to its documented API.
option(FIXGW_INTEROP "Build QuickFIX and the interoperability tests" OFF)
if(FIXGW_INTEROP)
  include(ExternalProject)
  set(FIXGW_QUICKFIX_COMMIT 2ce8a60)  # tag v1.16.0, licence read in note 001
  set(QF_PREFIX ${CMAKE_BINARY_DIR}/_deps/quickfix)
  ExternalProject_Add(quickfix
    GIT_REPOSITORY https://github.com/quickfix/quickfix.git
    GIT_TAG v1.16.0
    GIT_SHALLOW TRUE
    PREFIX ${QF_PREFIX}
    CMAKE_ARGS -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=${QF_PREFIX}/install -DQUICKFIX_TESTS=OFF
               -DQUICKFIX_EXAMPLES=ON -DQUICKFIX_SHARED_LIBS=OFF -DHAVE_SSL=OFF -DCMAKE_CXX_FLAGS=-w
    # The header copies first: no target depends on them, so a clean parallel
    # build otherwise compiles the examples before the headers they include.
    BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --target QUICKFIX_HEADERS_COPY QUICKFIX_ALLOCATOR_HEADERS_COPY
          COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR>
    BUILD_BYPRODUCTS ${QF_PREFIX}/install/lib/libquickfix.a
    UPDATE_COMMAND "")
  ExternalProject_Get_Property(quickfix SOURCE_DIR BINARY_DIR)
  set(QF_SOURCE_DIR ${SOURCE_DIR})
  set(QF_BINARY_DIR ${BINARY_DIR})
  add_library(quickfix_lib STATIC IMPORTED)
  set_target_properties(quickfix_lib PROPERTIES IMPORTED_LOCATION ${QF_PREFIX}/install/lib/libquickfix.a)
  file(MAKE_DIRECTORY ${QF_PREFIX}/install/include)
  target_include_directories(quickfix_lib SYSTEM INTERFACE ${QF_PREFIX}/install/include)
  add_dependencies(quickfix_lib quickfix)
  add_executable(qf_initiator interop/qf_initiator.cpp)
  target_link_libraries(qf_initiator PRIVATE quickfix_lib Threads::Threads)
  target_compile_options(qf_initiator PRIVATE -Wall -Wextra -Wpedantic -Werror)
  add_executable(interop_test interop/interop_test.cpp)
  target_link_libraries(interop_test PRIVATE fixgw lfipc Threads::Threads)
  add_test(NAME interop COMMAND interop_test ${QF_SOURCE_DIR}/lib/executor $<TARGET_FILE:qf_initiator>
                                ${QF_PREFIX}/install/share/quickfix/FIX44.xml)
  set_tests_properties(interop PROPERTIES LABELS interop)
endif()

add_executable(net_peer bench/net_peer.cpp)
target_link_libraries(net_peer PRIVATE fixgw lfipc Threads::Threads)
add_executable(net_bench bench/net_bench.cpp)
target_link_libraries(net_bench PRIVATE fixgw lfipc Threads::Threads)
target_compile_definitions(net_bench PRIVATE LFIPC_RESULTS_DIR="${CMAKE_SOURCE_DIR}/results")

add_executable(store_bench bench/store_bench.cpp)
target_link_libraries(store_bench PRIVATE fixgw lfipc Threads::Threads)
target_compile_definitions(store_bench PRIVATE LFIPC_RESULTS_DIR="${CMAKE_SOURCE_DIR}/results")

add_executable(codec_bench bench/codec_bench.cpp)
target_link_libraries(codec_bench PRIVATE fixgw lfipc Threads::Threads)
target_compile_definitions(codec_bench PRIVATE LFIPC_RESULTS_DIR="${CMAKE_SOURCE_DIR}/results")

# Conformance shown case by case (note 006): a stale CONFORMANCE.md, or a case
# the test does not show, fails the suite.
add_test(NAME conformance_check COMMAND Python3::Interpreter -I ${CMAKE_SOURCE_DIR}/run.py conformance --check
                                       --test $<TARGET_FILE:conformance_test> --matrices $<TARGET_FILE:matrices_test>)

# One number everywhere: a stale RESULTS.md or README figure fails the suite.
if(Python3_FOUND)
  add_test(NAME report_check COMMAND Python3::Interpreter -I ${CMAKE_SOURCE_DIR}/run.py report --check
                                    --lockfree-ipc ${lockfree_ipc_SOURCE_DIR})
endif()
