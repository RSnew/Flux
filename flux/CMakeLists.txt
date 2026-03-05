cmake_minimum_required(VERSION 3.16)
project(flux CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 编译选项
add_compile_options(-Wall -Wextra -O2)

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
)

# 链接 pthread（FileWatcher 使用线程）
find_package(Threads REQUIRED)
# 链接 OpenSSL（HTTPS 支持）
find_package(OpenSSL REQUIRED)
target_link_libraries(flux Threads::Threads OpenSSL::SSL OpenSSL::Crypto)

# 安装
install(TARGETS flux DESTINATION bin)
