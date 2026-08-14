# Generate intg_test.rs - must be run standalone
import sys

content = r"""//! Integration test: exercise Rust-to-RTOS ABI paths via g_app_slot function table.
//!
//! Test A: Device open/close loop - iterate known devices, open->write->close
//! Test B: msleep precision - verify via tick_count delta
//! Test C: Concurrent uart0 write - 2 tasks share the log ring

use core::ffi::c_void;

use crate::abi::*;
use crate::device::Device;
use crate::info;
use crate::rtos_sync::{msleep, spawn_rt, tick_count, RT_NONE};
use crate::warn;

#[link_section = ".rust_bss"]
static mut INTG_STACK: [u8; 4096] = [0u8; 4096];

/* ========================================================================
 * Test A: Device open/close loop
 * ======================================================================== */

const DEV_NAMES: &[&[u8]] = &[
    b"uart0\0",
    b"usb0\0",
    b"pwm0\0",
    b"i2c0\0",
    b"spi0\0",
    b"uart1\0",
];

const TEST_PAYLOAD: &[u8] = b"INTG\x0d\x0a";

extern "C" fn intg_entry_a(_arg: *mut c_void) {
    info!(tag: "intg", "A: device open/close begin");
    let mut pass = 0u32;
    let mut fail = 0u32;

    for name in DEV_NAMES {
        let dev = Device::open(name);
        match dev {
            Some(d) => {
                let wr = d.write(TEST_PAYLOAD);
                if wr > 0 {
                    pass += 1;
                } else {
                    warn!(tag: "intg", "A: open OK but write={}",
                          core::str::from_utf8(name).unwrap_or("?"), wr);
                    pass += 1;
                }
            }
            None => {
                warn!(tag: "intg", "A: {} open FAIL",
                      core::str::from_utf8(name).unwrap_or("?"));
                fail += 1;
            }
        }
    }
    info!(tag: "intg", "A: done pass={} fail={} total={}", pass, fail, DEV_NAMES.len());
}

/* ========================================================================
 * Test B: msleep precision
 * ======================================================================== */

extern "C" fn intg_entry_b(_arg: *mut c_void) {
    info!(tag: "intg", "B: msleep precision begin");
    let test_ms: &[u32] = &[1, 5, 10, 20, 50, 100];

    for n in test_ms {
        let t0 = tick_count();
        msleep(*n);
        let dt = tick_count().wrapping_sub(t0);
        let err = if dt >= *n { dt - *n } else { *n - dt };
        if err > 2 {
            warn!(tag: "intg", "B: msleep({}) dt={} err={} (expect err<=2)", n, dt, err);
        }
    }
    info!(tag: "intg", "B: done");
}

/* ========================================================================
 * Test C: Concurrent uart0 writes
 * ======================================================================== */

#[link_section = ".rust_bss"]
static mut INTG_C_STACK1: [u8; 1024] = [0u8; 1024];
#[link_section = ".rust_bss"]
static mut INTG_C_STACK2: [u8; 1024] = [0u8; 1024];

extern "C" fn intg_entry_c1(_arg: *mut c_void) {
    for i in 0..20 {
        info!(tag: "intg_c1", "ping {}", i);
        msleep(10);
    }
    info!(tag: "intg_c1", "done");
}

extern "C" fn intg_entry_c2(_arg: *mut c_void) {
    for i in 0..20 {
        info!(tag: "intg_c2", "pong {}", i);
        msleep(10);
    }
    info!(tag: "intg_c2", "done");
}

extern "C" fn intg_entry_c(_arg: *mut c_void) {
    info!(tag: "intg", "C: concurrent uart0 write begin");

    unsafe {
        spawn_rt(
            b"intg_c1\0",
            intg_entry_c1,
            20,
            INTG_C_STACK1.as_mut_ptr(),
            INTG_C_STACK1.len(),
            1,
            RT_NONE,
            0,
            0,
        );
        spawn_rt(
            b"intg_c2\0",
            intg_entry_c2,
            21,
            INTG_C_STACK2.as_mut_ptr(),
            INTG_C_STACK2.len(),
            1,
            RT_NONE,
            0,
            0,
        );
    }

    msleep(500);
    info!(tag: "intg", "C: done");
}

/* ========================================================================
 * Entry
 * ======================================================================== */

pub fn run_intg_test() {
    crate::log::spawn_log_task();
    msleep(5);

    info!(tag: "intg", "=== integration test start ===");

    unsafe {
        spawn_rt(b"intg_a\0", intg_entry_a, 20,
                 INTG_STACK.as_mut_ptr(), INTG_STACK.len(), 1, RT_NONE, 0, 0);
    }
    msleep(50);

    unsafe {
        spawn_rt(b"intg_b\0", intg_entry_b, 20,
                 INTG_STACK.as_mut_ptr(), INTG_STACK.len(), 1, RT_NONE, 0, 0);
    }
    msleep(50);

    unsafe {
        spawn_rt(b"intg_c\0", intg_entry_c, 20,
                 INTG_STACK.as_mut_ptr(), INTG_STACK.len(), 1, RT_NONE, 0, 0);
    }

    msleep(1500);
    info!(tag: "intg", "=== integration test end ===");
}
"""

with open('D:/projects/mcu/os/joc-app-rust/src/intg_test.rs', 'w', encoding='utf-8') as f:
    f.write(content)

# Verify
with open('D:/projects/mcu/os/joc-app-rust/src/intg_test.rs', 'rb') as f:
    data = f.read()
data.decode('utf-8')  # raises if not valid
print('intg_test.rs: OK, {} bytes, valid UTF-8'.format(len(data)))