file build/stm32f407_minimal.elf
target remote localhost:3333
monitor reset run
break console_run
continue
break rtos_fault_handler
continue
echo === FAULT HANDLER HIT (first) ===\n
info registers r0 r1 pc
set $frame = $r0
set $lr = $r1
set $fo = ($lr & 0x10) ? 0 : 0x12
set $pcslot = $frame + $fo + 6
set $uart = *(unsigned int*)0x20000000
printf "frame=0x%08x lr=0x%08x fo=%u pcslot=0x%08x uart=0x%08x\n", $frame, $lr, $fo, $pcslot, $uart
echo === bytes at pcslot (the write target) ===\n
x/8xw $pcslot
echo === bytes at uart object (first 16) ===\n
x/16xw $uart
echo === END ===\n
