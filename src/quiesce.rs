use core::{
    arch::asm,
    ptr::{read_volatile, write_volatile},
};

unsafe fn read(address: usize) -> u32 {
    unsafe { read_volatile(address as *const u32) }
}
unsafe fn write(address: usize, value: u32) {
    unsafe { write_volatile(address as *mut u32, value) }
}
unsafe fn clocked(slot: usize) -> bool {
    unsafe { read(0x403f_0000 + slot * 4) & (1 << 30) != 0 }
}
unsafe fn wait(address: usize, mask: u32, expected: u32) {
    let started = unsafe { reflex::runtime::micros() };
    while unsafe { read(address) } & mask != expected {
        assert!(
            unsafe { reflex::runtime::micros() }.wrapping_sub(started) < 100_000,
            "Peripheral stop timeout at {address:08x}"
        );
    }
}

unsafe fn progress(message: &str) {
    const { assert!(reflex::display::HEIGHT >= 450) };
    unsafe {
        // Match Reflex: RUN need not clear while the firmware display is active.
        for y in 410..450 {
            for x in 0..reflex::display::WIDTH {
                reflex::display::write_pixel(x, y, 0xffff);
            }
        }
        reflex::text::draw(
            |x, y, color| reflex::display::write_pixel(x, y, color),
            12,
            420,
            message,
            0,
            0xffff,
            1,
        );
        reflex::display::present();
    }
}

/// Refuse unsupported bus masters before changing any hardware.
pub unsafe fn check() -> Result<(), &'static str> {
    unsafe {
        if clocked(36) {
            return Err("CAAM clock active; cannot boot yet");
        }
        if clocked(52) {
            return Err("Second USB controller active; cannot boot yet");
        }
        for offset in [0xa0, 0x140, 0x144, 0x148] {
            if read(0x40b3_0000 + offset) & (1 << 30) != 0 {
                return Err("VIU/GPU clock active; cannot boot yet");
            }
        }
    }
    Ok(())
}

/// No firmware calls or return to PEG are allowed after this point.
/// Storage files must already be closed, payloads validated, and audio stopped.
pub unsafe fn stop() {
    unsafe {
        asm!("cpsid if", options(nostack, preserves_flags));
        check().expect("Bus master changed before handoff");
        progress("Boot 1/6: storage");
        // Finish card transactions before resetting either controller.
        for (slot, base) in [(55, 0x4037_0000), (56, 0x4038_0000)] {
            if clocked(slot) {
                wait(base + 0x24, 0x7, 0);
                write(base + 0x38, 0);
                write(base + 0x2c, read(base + 0x2c) | (1 << 24));
                wait(base + 0x2c, 1 << 24, 0);
            }
        }
        progress("Boot 2/6: USB");
        if clocked(51) {
            write(0x4033_0148, 0);
            write(0x4033_0140, read(0x4033_0140) & !1);
            write(0x4033_0140, read(0x4033_0140) | 2);
            wait(0x4033_0140, 2, 0);
        }
        progress("Boot 3/6: DMA");
        if clocked(8) {
            write(0x4008_000c, 0);
            write(0x4008_0000, read(0x4008_0000) | (1 << 5));
            for channel in 0..32 {
                let csr = 0x4008_1000 + channel * 32 + 0x1c;
                let started = reflex::runtime::micros();
                while read_volatile(csr as *const u16) & (1 << 6) != 0 {
                    assert!(
                        reflex::runtime::micros().wrapping_sub(started) < 100_000,
                        "eDMA busy"
                    );
                }
            }
        }
        progress("Boot 4/6: watchdogs");
        for (slot, base) in [(61, 0x403d_0000), (67, 0x4043_0000)] {
            let gate = 0x403f_0000 + slot * 4;
            write(gate, read(gate) | (1 << 30));
            let cs = read(base);
            if cs & (1 << 7) != 0 {
                assert!(cs & (1 << 5) != 0, "Watchdog cannot be disabled");
                if cs & (1 << 13) != 0 {
                    write(base + 4, 0xd928_c520);
                } else {
                    write(base + 4, 0xc520);
                    write(base + 4, 0xd928);
                }
                wait(base, 1 << 11, 1 << 11);
                write(base, cs & !((1 << 7) | (1 << 14)));
                wait(base, 1 << 10, 1 << 10);
                assert!(read(base) & (1 << 7) == 0, "Watchdog still enabled");
            }
        }
        progress("Boot 5/6: MU");
        if clocked(34) {
            write(0x4022_0064, read(0x4022_0064) & 0xffff);
        }
        // LCD keeps reading reserved firmware RAM, outside the Linux memory range.
        progress("Boot 6/6: MMU -> Linux");
        asm!(
            "mov r0, #0",
            "mcr p15, 0, r0, c14, c2, 1",
            "mcr p15, 0, r0, c14, c3, 1",
            "dsb sy",
            out("r0") _, options(nostack, preserves_flags),
        );
    }
}
