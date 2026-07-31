target remote localhost:3333
monitor reset halt
break *0x08000718
continue
echo === HIT Default_Handler ===\n
info registers
echo === SP region ===\n
x/64xw $sp
echo === 0x10001c00 ===\n
x/64xw 0x10001c00
echo === 0x10001e00 ===\n
x/64xw 0x10001e00
echo === DONE ===\n
