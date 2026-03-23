cmake_minimum_required(VERSION 3.16)
project(flux C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 编译选项
add_compile_options(-Wall -Wextra -O2)

# ── LLVM JIT support (optional) ────────────────────────────────────
set(FLUX_HAS_LLVM_FLAG OFF)
set(HOMEBREW_LLVM_PREFIX "")

# Check for Homebrew LLVM first (macOS), then system LLVM
if(APPLE)
    execute_process(
        COMMAND brew --prefix llvm
        OUTPUT_VARIABLE HOMEBREW_LLVM_PREFIX
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE BREW_RESULT
    )
    if(BREW_RESULT EQUAL 0 AND EXISTS "${HOMEBREW_LLVM_PREFIX}/lib/cmake/llvm")
        set(LLVM_DIR "${HOMEBREW_LLVM_PREFIX}/lib/cmake/llvm")
        message(STATUS "Found Homebrew LLVM at: ${HOMEBREW_LLVM_PREFIX}")
    endif()
endif()

find_package(LLVM CONFIG QUIET)
if(LLVM_FOUND)
    message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION} at: ${LLVM_DIR}")

    llvm_map_components_to_libnames(LLVM_LIBS
        core support irreader orcjit native
        passes instcombine scalaropts transformutils
        analysis target mc
    )

    set(FLUX_HAS_LLVM_FLAG ON)
    message(STATUS "  LLVM JIT: ENABLED")
else()
    message(STATUS "LLVM not found — LLVM JIT disabled (build continues without it)")
endif()

# 源文件
add_executable(flux
    src/main.cpp
    src/lexer.cpp
    src/parser.cpp
    src/typechecker.cpp
    src/interpreter.cpp
    src/stdlib.cpp
    src/compiler.cpp
    src/vm.cpp
    src/watcher.cpp
    src/pkgmgr.cpp
    src/llvm_jit.cpp
)

# 链接 pthread（FileWatcher 使用线程）
find_package(Threads REQUIRED)
# 链接 OpenSSL（HTTPS 支持）
find_package(OpenSSL REQUIRED)
target_link_libraries(flux Threads::Threads OpenSSL::SSL OpenSSL::Crypto)

# LLVM: header is PIMPL, so only llvm_jit.cpp needs LLVM includes
if(FLUX_HAS_LLVM_FLAG)
    # FLUX_HAS_LLVM on the whole target (just a preprocessor flag, header uses it for PIMPL)
    target_compile_definitions(flux PRIVATE FLUX_HAS_LLVM)

    # Get LLVM-specific compile flags (include paths + definitions)
    # Apply ONLY to llvm_jit.cpp to avoid header search path pollution
    set(LLVM_CXX_EXTRA "")
    if(NOT "${HOMEBREW_LLVM_PREFIX}" STREQUAL "")
        execute_process(
            COMMAND ${HOMEBREW_LLVM_PREFIX}/bin/llvm-config --cxxflags
            OUTPUT_VARIABLE LLVM_CXX_RAW
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
        )
    else()
        execute_process(
            COMMAND llvm-config --cxxflags
            OUTPUT_VARIABLE LLVM_CXX_RAW
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
        )
    endif()
    # Extract -I and -D flags only
    string(REGEX MATCHALL "-I[^ ]+" LLVM_I_FLAGS "${LLVM_CXX_RAW}")
    string(REGEX MATCHALL "-D[^ ]+" LLVM_D_FLAGS "${LLVM_CXX_RAW}")
    list(APPEND LLVM_CXX_EXTRA ${LLVM_I_FLAGS} ${LLVM_D_FLAGS})
    string(REPLACE ";" " " LLVM_CXX_EXTRA_STR "${LLVM_CXX_EXTRA}")

    set_source_files_properties(src/llvm_jit.cpp PROPERTIES
        COMPILE_FLAGS "${LLVM_CXX_EXTRA_STR} -Wno-unused-parameter -Wno-comment"
    )

    target_link_libraries(flux ${LLVM_LIBS})
endif()

# 安装
install(TARGETS flux DESTINATION bin)
