file build/stm32f407_minimal.elf
target remote localhost:3333
monitor reset run
break console_run
continue
echo === REACHED console_run; reading c->uart ===\n
set $ua = *(unsigned int*)0x20000000
printf "uart_addr=0x%08x\n", $ua
watch *(unsigned int*)$ua
echo === watching object; continue then python sends RTOSROBUST ===\n
continue
echo === WP HIT ===\n
info registers pc lr sp
bt
printf "uart_addr_now=0x%08x\n", *(unsigned int*)0x20000000
x/32xw $ua
echo === END ===\n
