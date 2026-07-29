#!/usr/bin/env bash
# ===========================================================================
# PC 主机单元测试运行器（docs/ostest.md §8）：用本机 gcc 编译并运行 os_test/common
# 下的全部单测（atomic / lock / barrier / name_table / pool），汇总 PASS/FAIL。
# 与 run_tests.bat 等价的 *nix 版本。
# 用法：  cd os_test && ./run_tests.sh
# ===========================================================================
set -u
CC="${CC:-gcc}"
CFLAGS="-O2 -Wall -Wextra -std=gnu11 -I../src"
LIBS="../src/common/pool.c"
FAILS=0
PASSED=0

echo "=== jOS host unit-tests ==="
for T in atomic_test lock_test barrier_test name_table_test pool_test; do
    echo "--- building $T ---"
    if ! "$CC" $CFLAGS "common/$T.c" $LIBS -o "$T"; then
        echo "  $T: BUILD FAIL"
        FAILS=$((FAILS+1))
        continue
    fi
    if ./"$T" >/dev/null; then
        echo "  $T: PASS"
        PASSED=$((PASSED+1))
    else
        echo "  $T: FAIL"
        FAILS=$((FAILS+1))
    fi
done

echo "=== summary: passed=$PASSED failed=$FAILS ==="
[ "$FAILS" -eq 0 ]
