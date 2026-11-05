#![no_std]
#![no_main]
extern crate alloc;

mod cpu;

use alloc::{ffi::CString, format, string::String};
use core::fmt::Write;
use cpu::Cpu;
use reflex::{Action, Application, Event, display, fs, peg, text};

core::arch::global_asm!(
    include_str!("../../brain_5th_hack/crates/reflex-macros/src/startup.S"),
    options(raw)
);
#[global_allocator]
static ALLOCATOR: reflex::allocator::SystemAllocator = reflex::allocator::SystemAllocator::new();
static mut CONTEXT: *const reflex::app::AppContext = core::ptr::null();

#[unsafe(no_mangle)]
unsafe extern "C" fn __reflex_main(context: *const reflex::app::AppContext) {
    unsafe {
        CONTEXT = context;
        reflex::lifecycle::start(context, main, &ALLOCATOR);
    }
}

#[panic_handler]
fn panic(info: &core::panic::PanicInfo<'_>) -> ! {
    reflex::lifecycle::panic(info)
}

unsafe fn report_path() -> Result<CString, String> {
    let exec = unsafe { core::ffi::CStr::from_ptr((*CONTEXT).exec_path.cast()) };
    let metadata =
        unsafe { fs::metadata(exec) }.map_err(|code| format!("exec_path stat failed: {code}"))?;
    let mut path = exec.to_bytes().to_vec();
    if metadata.mode & 0xf000 != 0x4000 {
        let separator = path
            .iter()
            .rposition(|b| *b == b'\\' || *b == b'/')
            .ok_or_else(|| String::from("exec_path has no directory"))?;
        path.truncate(separator + 1);
    }
    if !matches!(path.last(), Some(b'\\' | b'/')) {
        path.push(b'\\');
    }
    path.extend_from_slice(b"BUXPROBE.TXT");
    Ok(CString::new(path).expect("exec_path contains no interior NUL"))
}

struct Probe {
    path: Result<CString, String>,
    report: String,
    status: String,
    page: usize,
}

fn main() -> Probe {
    let path = unsafe { report_path() };
    let report = unsafe { capture() };
    let status = match save(&path, &report) {
        Ok(()) => String::from("Saved: BUXPROBE.TXT next to app"),
        Err(error) => error,
    };
    Probe {
        path,
        report,
        status,
        page: 0,
    }
}

fn save(path: &Result<CString, String>, report: &str) -> Result<(), String> {
    let path = path.as_ref().map_err(Clone::clone)?;
    let mut file = unsafe { fs::File::create(path) }
        .ok_or_else(|| format!("Open failed: {}", path.to_string_lossy()))?;
    file.write(report.as_bytes())
        .map_err(|code| format!("Write failed: {code}"))?;
    file.flush()
        .map_err(|code| format!("Flush failed: {code}"))?;
    file.close().map_err(|code| format!("Close failed: {code}"))
}

unsafe fn capture() -> String {
    let cpu = unsafe { Cpu::read() };
    let mut report =
        String::from("Brainux PW-H1 boot probe v1\nTarget: V690_H1 (not auto-detected)\n");
    for (name, value) in [
        ("CPSR", cpu.cpsr),
        ("MIDR", cpu.midr),
        ("SCTLR", cpu.sctlr),
        ("TTBCR", cpu.ttbcr),
        ("TTBR0", cpu.ttbr0),
        ("TTBR1", cpu.ttbr1),
        ("DACR", cpu.dacr),
        ("CTR", cpu.ctr),
        ("CLIDR", cpu.clidr),
        ("VBAR", cpu.vbar),
        ("CNTFRQ", cpu.cntfrq),
    ] {
        writeln!(report, "{name}={value:08x}").unwrap();
    }
    if !cpu.supports_translation_probe() {
        report.push_str("Mapping probe requires MMU on, TTBCR.EAE=0.\n");
        return report;
    }
    report.push_str("\nPrivileged read translations (VA -> PA):\n");
    let stack_marker = 0u32;
    for (name, address) in [
        ("entry", 0x9000_0000),
        ("data", 0x9050_0000),
        ("app-end", 0x90ef_ffff),
        ("app-alias", 0x9100_0000),
        ("stack", core::ptr::addr_of!(stack_marker) as u32),
        ("report-heap", report.as_ptr() as u32),
        ("dram-alias", 0x3000_0000),
        ("dram", 0x6000_0000),
        ("framebuffer", 0x60a0_0008),
        ("app-physical", 0x6710_0000),
        ("gic", 0x4002_1000),
        ("edma", 0x4008_0000),
        ("mu", 0x4022_0000),
        ("usb", 0x4033_0000),
        ("usdhc0", 0x4037_0000),
        ("usdhc1", 0x4038_0000),
        ("wdog1", 0x403d_0000),
        ("wdog2", 0x4043_0000),
        ("lcdif", 0x40aa_0000),
        ("tstmr", 0x410a_3c00),
    ] {
        match unsafe { cpu.translate(address) } {
            Ok(physical) => writeln!(report, "{name}: {address:08x} -> {physical:08x}"),
            Err(par) => writeln!(report, "{name}: {address:08x} FAULT PAR={par:08x}"),
        }
        .unwrap();
    }
    report.push_str("\nStatic map checks (every 4 KiB page):\n");
    for (virtual_start, physical_start, length) in [
        (0x3000_0000u32, 0x6000_0000u32, 0x0800_0000u32),
        (0x60f0_0000, 0x60f0_0000, 0x0710_0000),
        (0x9000_0000, 0x6710_0000, 0x00f0_0000),
    ] {
        let mut mismatch = None;
        for offset in (0..length).step_by(4096) {
            let actual = unsafe { cpu.translate(virtual_start + offset) };
            if actual != Ok(physical_start + offset) {
                mismatch = Some((offset, actual));
                break;
            }
        }
        match mismatch {
            None => writeln!(
                report,
                "{virtual_start:08x} +{length:08x} -> {physical_start:08x} OK"
            ),
            Some((offset, actual)) => writeln!(
                report,
                "{virtual_start:08x} +{offset:08x} mismatch: {actual:x?}"
            ),
        }
        .unwrap();
    }
    report.push_str("\nNo MMU/cache shutdown or Linux jump performed.\n");
    report
}

impl Application for Probe {
    fn event(&mut self, event: Event) -> Action {
        let Event::Key {
            code, down: true, ..
        } = event
        else {
            return Action::None;
        };
        match code {
            peg::key::ESCAPE => return Action::Exit,
            peg::key::RIGHT | peg::key::PAGE_DOWN => {
                if (self.page + 1) * 12 < self.report.lines().count() {
                    self.page += 1;
                }
            }
            peg::key::LEFT | peg::key::PAGE_UP => self.page = self.page.saturating_sub(1),
            peg::key::ENTER => {
                self.status = match save(&self.path, &self.report) {
                    Ok(()) => String::from("Saved: BUXPROBE.TXT next to app"),
                    Err(error) => error,
                };
            }
            _ => return Action::None,
        }
        Action::Redraw
    }

    fn draw(&self, pixels: &mut [u16]) {
        pixels.fill(0xffff);
        let mut draw = |x, y, value: &str| {
            text::draw(
                |px, py, color| pixels[py * display::WIDTH + px] = color,
                x,
                y,
                value,
                0,
                0xffff,
                1,
            )
        };
        draw(12, 12, "Brainux / PW-H1 boot probe");
        draw(12, 44, &self.status);
        for (line, value) in self
            .report
            .lines()
            .skip(self.page * 12)
            .take(12)
            .enumerate()
        {
            draw(12, 86 + line * 26, value);
        }
        draw(
            12,
            424,
            &format!(
                "Page {} / {}",
                self.page + 1,
                self.report.lines().count().div_ceil(12)
            ),
        );
        draw(12, 450, "Left/Right: page  Enter: save  Esc: exit");
    }
}
