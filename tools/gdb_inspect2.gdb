set pagination off
set print pretty off
file build/stm32f407_minimal.elf
target remote localhost:3333
monitor halt
echo === VTOR ===\n
x/1xw 0xE000ED08
echo === WWDG vector slot (VTOR+0x34) ===\n
set $vtor = *(unsigned*)0xE000ED08
x/1xw $vtor + 0x34
echo === WWDG CR/CFR/SR (0x40002C00) ===\n
x/3xw 0x40002C00
echo === NVIC ISER0 (0xE000E100) ===\n
x/1xw 0xE000E100
echo === NVIC ISPR0 (0xE000E200) ===\n
x/1xw 0xE000E200
echo === RCC APB1ENR (0x40023840) ===\n
x/1xw 0x40023840
