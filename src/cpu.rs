use core::arch::asm;

pub struct Cpu {
    pub cpsr: u32,
    pub midr: u32,
    pub sctlr: u32,
    pub ttbcr: u32,
    pub ttbr0: u32,
    pub ttbr1: u32,
    pub dacr: u32,
    pub ctr: u32,
    pub clidr: u32,
    pub vbar: u32,
    pub cntfrq: u32,
}

impl Cpu {
    /// Requires privileged ARMv7-A execution on PW-H1.
    pub unsafe fn read() -> Self {
        macro_rules! mrc {
            ($instruction:literal) => {{
                let value: u32;
                unsafe { asm!($instruction, out(reg) value, options(nostack, preserves_flags)) };
                value
            }};
        }
        Self {
            cpsr: mrc!("mrs {}, cpsr"),
            midr: mrc!("mrc p15, 0, {}, c0, c0, 0"),
            sctlr: mrc!("mrc p15, 0, {}, c1, c0, 0"),
            ttbcr: mrc!("mrc p15, 0, {}, c2, c0, 2"),
            ttbr0: mrc!("mrc p15, 0, {}, c2, c0, 0"),
            ttbr1: mrc!("mrc p15, 0, {}, c2, c0, 1"),
            dacr: mrc!("mrc p15, 0, {}, c3, c0, 0"),
            ctr: mrc!("mrc p15, 0, {}, c0, c0, 1"),
            clidr: mrc!("mrc p15, 1, {}, c0, c0, 1"),
            vbar: mrc!("mrc p15, 0, {}, c12, c0, 0"),
            cntfrq: mrc!("mrc p15, 0, {}, c14, c0, 0"),
        }
    }

    pub fn supports_translation_probe(&self) -> bool {
        self.sctlr & 1 != 0 && self.ttbcr & (1 << 31) == 0
    }

    /// ATS1CPR tests privileged read access without reading the target address.
    /// Requires enabled MMU with short descriptors; preserves PAR and IRQ/FIQ state.
    pub unsafe fn translate(&self, address: u32) -> Result<u32, u32> {
        assert!(self.supports_translation_probe());
        let par: u32;
        unsafe {
            asm!(
                "mrs {saved}, cpsr",
                "cpsid if",
                "mrc p15, 0, {previous}, c7, c4, 0",
                "mcr p15, 0, {address}, c7, c8, 0",
                "isb",
                "mrc p15, 0, {par}, c7, c4, 0",
                "mcr p15, 0, {previous}, c7, c4, 0",
                "isb",
                "msr cpsr_c, {saved}",
                address = in(reg) address,
                saved = out(reg) _,
                previous = out(reg) _,
                par = out(reg) par,
                options(nostack, preserves_flags),
            );
        }
        decode_par(address, par)
    }
}

const fn decode_par(address: u32, par: u32) -> Result<u32, u32> {
    if par & 1 != 0 {
        Err(par)
    } else {
        // PAR.SS identifies a 16 MiB supersection, whose low 24 bits come from VA.
        let mask = if par & 2 != 0 {
            0xff00_0000
        } else {
            0xffff_f000
        };
        Ok((par & mask) | (address & !mask))
    }
}

const _: () = {
    assert!(matches!(
        decode_par(0x9000_0008, 0x6710_0000),
        Ok(0x6710_0008)
    ));
    assert!(matches!(
        decode_par(0x9050_0123, 0x6700_0002),
        Ok(0x6750_0123)
    ));
    assert!(matches!(
        decode_par(0x9000_0000, 0x0000_000b),
        Err(0x0000_000b)
    ));
};
