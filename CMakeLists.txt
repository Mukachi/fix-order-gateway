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

find_package(Threads REQUIRED)
find_package(Python3 COMPONENTS Interpreter)
enable_testing()

# lockfree-ipc: its queues and measurement harness (note 001), fetched at a
# pinned commit into build/_deps. Headers only: SOURCE_SUBDIR names no
# directory, so its own tests, benchmarks and baselines are not built here.
include(FetchContent)
set(FIXGW_LFIPC_COMMIT efbccdc1ba7226cd7444b9a225bdf9d08ce2591d)
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
          "-DFLAGS=${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_${build_type_upper}} -Wall -Wextra -Wpedantic -Werror"
          -P ${lockfree_ipc_SOURCE_DIR}/cmake/build_info.cmake
  BYPRODUCTS ${CMAKE_BINARY_DIR}/generated/lfipc/build_info.hpp
  COMMENT "Recording build provenance")
add_library(lfipc INTERFACE)
target_include_directories(lfipc SYSTEM INTERFACE ${lockfree_ipc_SOURCE_DIR}/include ${CMAKE_BINARY_DIR}/generated)
target_compile_definitions(lfipc INTERFACE FIXGW_LFIPC_COMMIT="${FIXGW_LFIPC_COMMIT}")
add_dependencies(lfipc build_info)

foreach(t framing_test decode_test)
  add_executable(${t} tests/${t}.cpp)
  target_link_libraries(${t} PRIVATE fixgw)
  add_test(NAME ${t} COMMAND ${t})
endforeach()
