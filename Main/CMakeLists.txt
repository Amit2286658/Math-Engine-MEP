cmake_minimum_required(VERSION 3.16)
project(cmath_engine VERSION 1.0.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# Strict warning parity between CMake and Makefile builds
add_compile_options(
    -Wall
    -Wextra
    -Wconversion
    -Wcast-qual
    -Wshadow
    -Wformat=2
    -Werror
)

# Optional shared library build (e.g. for Android NDK / JNI `libmathengine.so`)
option(MATH_ENGINE_BUILD_SHARED "Build mathengine as a shared library (.so/.dylib)" OFF)
option(MATH_ENGINE_SEPARATE_STDLIB "Omit embedded stdlib_precompiled.h; load precompiled/stdlib.mathc separately" OFF)

# Android NDK configuration support
if(ANDROID)
    message(STATUS "Configuring cmath_engine for Android NDK (ABI=${ANDROID_ABI}, API=${ANDROID_PLATFORM})")
    add_compile_options(-fPIC -fvisibility=hidden)
else()
    # Hardening flags for host builds
    add_compile_options(-U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=2)
    add_compile_options(-fstack-protector-strong)
    add_compile_options(-fvisibility=hidden)
endif()

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

# Debug: debug symbols, light optimization (needed for fortify to take effect),
# sanitizers if the toolchain supports them.
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_compile_options(-g -Og)

    if(NOT ANDROID)
        include(CheckCSourceRuns)

        set(CMAKE_REQUIRED_FLAGS "-fsanitize=address")
        check_c_source_runs("int main(void) { return 0; }" ASAN_SUPPORTED)
        unset(CMAKE_REQUIRED_FLAGS)

        set(CMAKE_REQUIRED_FLAGS "-fsanitize=undefined")
        check_c_source_runs("int main(void) { return 0; }" UBSAN_SUPPORTED)
        unset(CMAKE_REQUIRED_FLAGS)

        set(ENABLED_SANITIZERS "")
        if(ASAN_SUPPORTED)
            list(APPEND ENABLED_SANITIZERS "address")
        else()
            message(WARNING "ASan not available on this toolchain -- skipping")
        endif()
        if(UBSAN_SUPPORTED)
            list(APPEND ENABLED_SANITIZERS "undefined")
        else()
            message(WARNING "UBSan not available on this toolchain -- skipping")
        endif()

        if(ENABLED_SANITIZERS)
            list(JOIN ENABLED_SANITIZERS "," SANITIZER_FLAG_LIST)
            message(STATUS "Enabling sanitizers: ${SANITIZER_FLAG_LIST}")
            add_compile_options(-fsanitize=${SANITIZER_FLAG_LIST})
            add_link_options(-fsanitize=${SANITIZER_FLAG_LIST})
        else()
            message(WARNING "No sanitizers available on this toolchain -- building plain debug")
        endif()
    endif()
endif()

# Release: optimized, no sanitizers
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    add_compile_options(-O2)
endif()

# Gather engine implementation sources separated by folder/responsibility:
#   - Core/Code/*.c          : Core engine pipeline (arena, sym_table, lexer, parser, evaluator, math_engine)
#   - Core/compile/*.c       : Extension compiler (`compiler.c`), `<[dep]` linker (`linker.c`),
#                              `.mathc` binary serializer (`mathc_bin.c`) & user loader (`user_loader.c`)
#   - Core/session/*.c       : Parallel in-memory sessions (`session.c`) & order-tagged snapshots (`snapshot.c`)
#   - Core/lib/delegate/*.c  : All `@` intrinsic C delegates (@add, @sin, @vnew, @mzero, @calc_*, etc.)
#   - Core/lib/hardcode/*.c  : All hardcoded grammar & ergonomic rules (reserved kw, implicit *, UFCS, etc.)
file(GLOB_RECURSE CORE_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/Core/Code/*.c"
    "${CMAKE_SOURCE_DIR}/Core/compile/*.c"
    "${CMAKE_SOURCE_DIR}/Core/session/*.c"
    "${CMAKE_SOURCE_DIR}/Core/jni/*.c"
    "${CMAKE_SOURCE_DIR}/Core/lib/delegate/*.c"
    "${CMAKE_SOURCE_DIR}/Core/lib/hardcode/*.c"
)
file(GLOB_RECURSE TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/Test/*.c")

# Reusable library target (STATIC by default; SHARED when MATH_ENGINE_BUILD_SHARED=ON for JNI/NDK)
if(MATH_ENGINE_BUILD_SHARED)
    add_library(mathengine SHARED ${CORE_SOURCES})
    target_compile_definitions(mathengine PRIVATE MATH_ENGINE_SHARED=1)
else()
    add_library(mathengine STATIC ${CORE_SOURCES})
endif()

if(MATH_ENGINE_SEPARATE_STDLIB)
    target_compile_definitions(mathengine PRIVATE MATH_ENGINE_SEPARATE_STDLIB=1)
endif()

set_target_properties(mathengine PROPERTIES
    C_VISIBILITY_PRESET hidden
    POSITION_INDEPENDENT_CODE ON
)

target_include_directories(mathengine PUBLIC
    "${CMAKE_SOURCE_DIR}/Core/Headers"
    "${CMAKE_SOURCE_DIR}/Core/compile"
    "${CMAKE_SOURCE_DIR}/Core/session"
    "${CMAKE_SOURCE_DIR}/Core/jni"
    "${CMAKE_SOURCE_DIR}/Core/lib/delegate"
    "${CMAKE_SOURCE_DIR}/Core/lib/hardcode"
)
target_link_libraries(mathengine PUBLIC m)

# Test & demo executable (`myapp` / `cmath_tests`) linked against `mathengine`
if(NOT ANDROID)
    find_package(Threads REQUIRED)
    add_executable(myapp ${TEST_SOURCES})
    target_link_libraries(myapp PRIVATE mathengine Threads::Threads)

    include(CTest)
    enable_testing()
    add_test(
        NAME cmath_suite
        COMMAND myapp
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    )
endif()

# Optional standalone AOT extension compiler & linker (`make stdlib`)
if(NOT ANDROID AND EXISTS "${CMAKE_SOURCE_DIR}/Library/mathc.c")
    add_executable(mathc EXCLUDE_FROM_ALL
        "${CMAKE_SOURCE_DIR}/Library/mathc.c"
    )
    target_link_libraries(mathc PRIVATE mathengine)

    add_custom_target(stdlib
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_SOURCE_DIR}/precompiled"
        COMMAND mathc
            "${CMAKE_SOURCE_DIR}/precompiled/stdlib.mathc"
            "${CMAKE_SOURCE_DIR}/Core/Headers/stdlib_precompiled.h"
            k_stdlib_mathc
            "${CMAKE_SOURCE_DIR}/Library/algebra.math"
            "${CMAKE_SOURCE_DIR}/Library/arith.math"
            "${CMAKE_SOURCE_DIR}/Library/biology.math"
            "${CMAKE_SOURCE_DIR}/Library/bootstrap.math"
            "${CMAKE_SOURCE_DIR}/Library/calculus.math"
            "${CMAKE_SOURCE_DIR}/Library/chemistry.math"
            "${CMAKE_SOURCE_DIR}/Library/complex.math"
            "${CMAKE_SOURCE_DIR}/Library/conversions.math"
            "${CMAKE_SOURCE_DIR}/Library/geom.math"
            "${CMAKE_SOURCE_DIR}/Library/mat.math"
            "${CMAKE_SOURCE_DIR}/Library/physics.math"
            "${CMAKE_SOURCE_DIR}/Library/string.math"
            "${CMAKE_SOURCE_DIR}/Library/trig.math"
            "${CMAKE_SOURCE_DIR}/Library/units.math"
            "${CMAKE_SOURCE_DIR}/Library/vec.math"
        DEPENDS mathc
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        COMMENT "Compiling & linking Library/*.math -> precompiled/stdlib.mathc & Core/Headers/stdlib_precompiled.h"
    )
endif()
