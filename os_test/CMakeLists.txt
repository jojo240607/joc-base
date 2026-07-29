# ===========================================================================
# Host (PC) 单元测试工程（独立 project，不继承根目录 arm 工具链）。
# 编译并运行 os_test/common 下的固件无关单测：atomic / lock / barrier /
# name_table / pool。对应 docs/ostest.md §8 的“本地 PC 上单元测试”。
#
# 用法：
#   cmake -S os_test -B build_host
#   cmake --build build_host
#   cd build_host && ctest --output-on-failure
# ===========================================================================
cmake_minimum_required(VERSION 3.20)
project(ostest_host C)

set(OSTEST_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)
# 头文件包含：common/* 以 "common/xxx.h" 形式被引用（含路径指向仓库 src/）
include_directories(${OSTEST_ROOT}/src)   # 使 "common/xxx.h" 形式被解析

# 各测试源 + 需要的库实现
set(HOST_TESTS
    atomic_test
    lock_test
    barrier_test
    name_table_test
    pool_test)

foreach(t ${HOST_TESTS})
    add_executable(${t} common/${t}.c)
    # pool_test 需要链接 OOC 内存池实现
    if(t STREQUAL "pool_test")
        target_sources(${t} PRIVATE ${OSTEST_ROOT}/src/common/pool.c)
    endif()
    target_compile_options(${t} PRIVATE -O2 -Wall -Wextra -std=gnu11)
    add_test(NAME ${t} COMMAND ${t})
endforeach()
