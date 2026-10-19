# afterglow's world, with WAMR embedded: built for Android's C library by system/world/build.sh
cmake_minimum_required(VERSION 3.14)
project(world C)

set(WAMR_BUILD_PLATFORM "android")
set(WAMR_BUILD_TARGET "AARCH64")
set(WAMR_BUILD_INTERP 1)
set(WAMR_BUILD_FAST_INTERP 1)
set(WAMR_BUILD_AOT 1)
set(WAMR_BUILD_JIT 0)
set(WAMR_BUILD_FAST_JIT 0)
set(WAMR_BUILD_LIBC_BUILTIN 0)
set(WAMR_BUILD_LIBC_WASI 1)
set(WAMR_BUILD_INSTRUCTION_METERING 1)
set(WAMR_BUILD_REF_TYPES 1)   # clang 19+ emits reference types by default
include(${WAMR_ROOT_DIR}/build-scripts/runtime_lib.cmake)
add_library(vmlib STATIC ${WAMR_RUNTIME_LIB_SOURCE})

add_executable(world world.c ext.c gles.c paint.c sign.c head.c lens.c memory.c)
target_include_directories(world PRIVATE ${STB_DIR})
target_compile_options(world PRIVATE -Wall -Wextra)
target_link_libraries(world vmlib dl m log)
