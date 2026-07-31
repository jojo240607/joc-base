target remote localhost:3333
echo === g_app_ctx (0x20000000) ===\n
x/8xw 0x20000000
echo === c->uart device obj (0x20015600) ===\n
x/32xw 0x20015600
echo === vtable it points to ===\n
x/8xw *0x20015600
echo === msp/psp context frame at sp ===\n
x/16xw $sp
echo === DONE ===\n
