#!/usr/bin/env bash
# §6.6 覆盖率：用 QEMU 跑固件并经由 TCP 串口收集 gcov（实验性）。
#
# 前提：
#   - 用 -DCOVERAGE=ON 构建（cmake -S . -B build_cov -G Ninja -DCOVERAGE=ON && cmake --build build_cov）
#   - 本机有能模拟 STM32F4 的 QEMU（mainstream QEMU 对 F4 外设支持有限，复杂 BIST
#     可能在未实现的外设上 fault；若如此，优先用真实硬件 COM 口跑 coverage_collect.py）。
#
# 用法：
#   ./tools/qemu_cov.sh            # 起 QEMU（-serial tcp::1234），另开终端跑：
#   python tools/coverage_collect.py --tcp 127.0.0.1:1234 --build build_cov
#
# 说明：固件在控制台收到 RTOSCOV 后把 .gcda 以二进制帧经该 TCP 串口导出，
# collector 收帧落盘并跑 gcov。可先在 QEMU 里敲 RTOSALL 跑一遍自测再发 RTOSCOV。
set -e

QEMU="${QEMU:-qemu-system-arm}"
KERNEL="${KERNEL:-build_cov/stm32f407_minimal.elf}"
SERIAL="${SERIAL:-tcp::1234,server,nowait}"

# 以下 machine/CPU 仅作示例；按你本机 QEMU 实际支持的 F4 机型调整。
exec "$QEMU" \
  -M netduinoplus2 \
  -cpu cortex-m4 \
  -kernel "$KERNEL" \
  -nographic \
  -serial "$SERIAL" \
  "$@"
