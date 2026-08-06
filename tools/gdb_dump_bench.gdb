set pagination off
target remote localhost:3333
monitor reset halt
printf "g_bench_result addr = %p\n", &g_bench_result
x/44xw &g_bench_result
quit
