target remote localhost:3333
set height 0
monitor halt
info registers pc sp
x/1wx 0xE000ED28
x/1wx 0xE000ED2C
x/1wx 0xE000ED38
bt 6
