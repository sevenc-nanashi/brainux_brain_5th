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

const FRAMEBUFFER: usize = 0x60a0_0008;
const PEG_FRAMEBUFFER: usize = 0x60ac_a810;
const FRAME_BYTES: usize = reflex::display::WIDTH * reflex::display::HEIGHT * 2;

unsafe fn displayed_buffer() -> usize {
    let address = unsafe { read(0x40aa_0040) } as usize;
    assert!(
        matches!(address, FRAMEBUFFER | PEG_FRAMEBUFFER),
        "Unknown LCD buffer: {address:08x}"
    );
    address
}

unsafe fn present_buffer(address: usize) {
    unsafe {
        let ctr: u32;
        asm!("mrc p15, 0, {ctr}, c0, c0, 1", ctr = out(reg) ctr, options(nostack, preserves_flags));
        let line_size = 4usize << ((ctr >> 16) & 15);
        for line in ((address & !(line_size - 1))..address + FRAME_BYTES).step_by(line_size) {
            asm!("mcr p15, 0, {line}, c7, c10, 1", line = in(reg) line, options(nostack, preserves_flags));
        }
        asm!("dsb sy", options(nostack, preserves_flags));
    }
}

unsafe fn select_linux_buffer() {
    unsafe {
        // Match mxsfb's continuous-scanout setup before the uncached Image copy.
        write(0x40aa_0014, 1 << 24);
        write(0x40aa_0028, 7 << 21);
        write(0x40aa_0024, 4 << 21);
        // Linux owns only the primary plane, not the firmware alpha-surface DMA.
        write(0x40aa_0218, 1);
        asm!("dsb sy", options(nostack, preserves_flags));
        assert!(
            read(0x40aa_0010) & (1 << 24) != 0,
            "LCD recovery not enabled"
        );
        assert!(
            read(0x40aa_0020) & (7 << 21) == 4 << 21,
            "LCD DMA requests not configured"
        );
        assert!(read(0x40aa_0210) & 1 == 0, "LCD overlay still enabled");
        let source = displayed_buffer() as *const u16;
        for pixel in 0..FRAME_BYTES / 2 {
            write_volatile(
                (FRAMEBUFFER as *mut u16).add(pixel),
                read_volatile(source.add(pixel)),
            );
        }
        present_buffer(FRAMEBUFFER);
        // DOTCLK scanout latches NEXT_BUF at a frame boundary; do not stop RUN.
        write(0x40aa_0050, FRAMEBUFFER as u32);
        asm!("dsb sy", options(nostack, preserves_flags));
        wait(0x40aa_0040, u32::MAX, FRAMEBUFFER as u32);
    }
}

const _: () = {
    assert!(FRAME_BYTES == 0xc8280);
    assert!(FRAMEBUFFER + FRAME_BYTES <= PEG_FRAMEBUFFER);
    assert!(PEG_FRAMEBUFFER + FRAME_BYTES < 0x6100_0000);
};

pub unsafe fn progress(message: &str) {
    const { assert!(reflex::display::HEIGHT >= 120) };
    unsafe {
        let buffer = displayed_buffer() as *mut u16;
        let pixel = |x, y, color| {
            write_volatile(buffer.add(reflex::display::framebuffer_index(x, y)), color);
        };
        for y in 80..120 {
            for x in 0..reflex::display::WIDTH {
                pixel(x, y, 0xffff);
            }
        }
        reflex::text::draw(pixel, 12, 90, message, 0, 0xffff, 1);
        present_buffer(buffer as usize);
        // Let scanout show the marker before stopping the next peripheral.
        let started = reflex::runtime::micros();
        while reflex::runtime::micros().wrapping_sub(started) < 250_000 {
            core::hint::spin_loop();
        }
    }
}

/// Refuse unsupported bus masters before changing any hardware.
pub unsafe fn check() -> Result<(), &'static str> {
    unsafe {
        let ctrl = read(0x40aa_0000);
        if ctrl & ((1 << 19) | (1 << 17) | (3 << 8) | (1 << 5) | 1)
            != (1 << 19) | (1 << 17) | (1 << 5) | 1
            || read(0x40aa_0030) != 0x0356_01e0
        {
            return Err("Unsupported LCD scanout mode");
        }
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
        progress("Boot: selecting Linux framebuffer");
        select_linux_buffer();
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
            asm!("dsb sy", options(nostack, preserves_flags));
            let cs = read(base);
            if cs & (1 << 7) != 0 {
                let timeout = read(base + 8);
                let window = read(base + 12);
                // UPDATE=0 also describes the initial, directly configurable state.
                if cs & (1 << 5) != 0 {
                    asm!("dmb sy", options(nostack, preserves_flags));
                    if cs & (1 << 13) != 0 {
                        write(base + 4, 0xd928_c520);
                    } else {
                        write(base + 4, 0xc520);
                        write(base + 4, 0xd928);
                    }
                    asm!("dmb sy", options(nostack, preserves_flags));
                    wait(base, 1 << 11, 1 << 11);
                }
                write(base + 12, window);
                write(base + 8, timeout);
                write(base, (cs & !((1 << 7) | (1 << 14))) | (1 << 5));
                asm!("dsb sy", options(nostack, preserves_flags));
                wait(base, 1 << 10, 1 << 10);
                let after = read(base);
                assert!(
                    after & (1 << 7) == 0,
                    "Watchdog {base:08x}: CS {cs:08x} -> {after:08x}, PCC {:08x}",
                    read(gate)
                );
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
