# cmake/ram_summary.cmake — POST_BUILD 主 RAM 占用统计日志
#
# 用法（由 CMakeLists 的 add_custom_command POST_BUILD 调用）：
#   cmake -P ram_summary.cmake
#     -DEXE=<elf 路径>
#     -DSIZE_TOOL=<size 工具路径，如 arm-none-eabi-size>
#     -DREGION_NAME=<RAM|DRAM|SRAM|DTCM>
#     -DREGION_ORIGIN=<主 RAM 区域起点，十六进制，如 0x20000000>
#     -DREGION_LENGTH=<主 RAM 区域长度，十进制字节，如 131072>
#
# 原理：用 `size -A`（sysv 格式，size/addr 均为十进制）列出各段，统计落在
# [REGION_ORIGIN, REGION_ORIGIN+REGION_LENGTH) 窗口内的段，取“区域起点 → 最高段
# 末尾”的跨度作为已用量 —— 与链接器 --print-memory-usage 口径一致（已实测对照：
# F407 soak RAM=101736B、ESP32C3 DRAM=130016B 均与 ld 输出逐字节吻合）。
#
# 排除 .app_* 分区段（.app_slot/.app_data/.app_bss 位于独立 APP_RAM/APP_SLOT_RAM
# 区域，地址数值上与主 RAM 窗口重叠，不计入主 RAM 占用）。

if(NOT DEFINED EXE OR NOT DEFINED SIZE_TOOL OR NOT DEFINED REGION_NAME OR NOT DEFINED REGION_ORIGIN OR NOT DEFINED REGION_LENGTH)
  message(FATAL_ERROR "ram_summary.cmake: 缺少 EXE/SIZE_TOOL/REGION_NAME/REGION_ORIGIN/REGION_LENGTH")
endif()

execute_process(COMMAND "${SIZE_TOOL}" -A "${EXE}"
  OUTPUT_VARIABLE _out ERROR_VARIABLE _err RESULT_VARIABLE _rc)
if(NOT _rc EQUAL 0)
  message(WARNING "ram_summary: size 运行失败(${_rc}) ${_err}")
  return()
endif()

math(EXPR _org "${REGION_ORIGIN}")
math(EXPR _len "${REGION_LENGTH}")
math(EXPR _top "${_org} + ${_len}")
set(_hi "${_org}")
string(REPLACE "\n" ";" _lines "${_out}")
foreach(_l IN LISTS _lines)
  # size -A 行格式：<section>  <size(dec)>  <addr(dec)>
  if(_l MATCHES "^[ \t]*(\\.?[A-Za-z_][A-Za-z0-9_.]*)[ \t]+([0-9]+)[ \t]+([0-9]+)")
    # 先保存捕获组，后面的 string(REGEX MATCH) 会覆盖 CMAKE_MATCH_*
    set(_sname "${CMAKE_MATCH_1}")
    set(_ssize "${CMAKE_MATCH_2}")
    set(_saddr "${CMAKE_MATCH_3}")
    string(REGEX MATCH "^\\.app_" _is_app "${_sname}")
    if(NOT _is_app)
      math(EXPR _addr "${_saddr}")
      math(EXPR _end  "${_addr} + ${_ssize}")
      if(_addr GREATER_EQUAL ${_org} AND _end LESS_EQUAL ${_top})
        if(_end GREATER ${_hi})
          set(_hi "${_end}")
        endif()
      endif()
    endif()
  endif()
endforeach()

math(EXPR _used "${_hi} - ${_org}")
set(_pct "0.0")
set(_permil 0)
if(NOT ${_len} EQUAL 0)
  math(EXPR _permil "${_used} * 1000 / ${_len}")   # 千分比（整数）
  math(EXPR _pi "${_permil} / 10")
  math(EXPR _pf "${_permil} % 10")
  set(_pct "${_pi}.${_pf}")
endif()

set(_status "OK")
if(${_used} GREATER ${_len})
  math(EXPR _over "${_used} - ${_len}")
  set(_status "OVERFLOW +${_over} B")
elseif(${_permil} GREATER 900)
  set(_status "WARN >90%")
endif()

message(STATUS "${REGION_NAME} usage: ${_used} B / ${_len} B (${_pct}%) [${_status}]")
