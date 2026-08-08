set pagination off
target extended-remote localhost:3333
monitor reset halt
# jump straight to App entry from reset context
set $pc = 0x08060080
set $sp = 0x20017000
set $cpsr = 0x01000000
stepi
printf "pc1=0x%08X\n", $pc
stepi
printf "pc2=0x%08X\n", $pc
stepi
printf "pc3=0x%08X\n", $pc
stepi
printf "pc4=0x%08X\n", $pc
printf "cfsr=0x%08X\n", *(unsigned*)0xE000ED28
detach
