
set pagination off
set confirm off
set remotetimeout 60
target extended-remote localhost:3333
monitor reset halt
file d:/project/mcu/oop/joc-base/build_stage2/stm32f407_minimal.elf
break app_slot_load_app
commands
  silent
  printf "*** app_slot_load_app CALLED ***\n"
  printf "    hdr->magic=%p abi=%d entry=%p\n", hdr->magic, hdr->abi_version, hdr->entry
  continue
end
break *0x08060080
commands
  silent
  printf "*** APP ENTRY REACHED *** sp=0x%08X\n", $sp
  continue
end
break *0x08001953
commands
  silent
  printf "*** APP CALL RETURNED -> app_main_task resumed at console_run (App ran OK) ***\n"
  continue
end
break rtos_fault_handler
commands
  silent
  printf "*** FAULT *** cfsr=0x%08X fault_pc=0x%08X mmfar=0x%08X control=0x%08X\n", *(unsigned*)0xE000ED28, ((unsigned*)frame)[6], *(unsigned*)0xE000ED34, g_fault_control
  printf "  frame r0=0x%08X r1=0x%08X r2=0x%08X r3=0x%08X\n", ((unsigned*)frame)[0], ((unsigned*)frame)[1], ((unsigned*)frame)[2], ((unsigned*)frame)[3]
  printf "  frame r12=0x%08X lr=0x%08X pc=0x%08X xpsr=0x%08X\n", ((unsigned*)frame)[4], ((unsigned*)frame)[5], ((unsigned*)frame)[6], ((unsigned*)frame)[7]
  printf "  msp=0x%08X psp=0x%08X\n", $msp, $psp
  continue
end
continue
