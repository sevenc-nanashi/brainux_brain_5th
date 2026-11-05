#![no_std]
#![no_main]
extern crate alloc;
mod entry;
mod paths;
mod quiesce;

use alloc::{format, string::String};
use brainux_probe::bundle::{
    Bundle, CAPACITY, DTB_DEST, FRAMEBUFFER, FRAMEBUFFER_RESERVE, IMAGE_DEST,
};
use core::{arch::global_asm, ptr};
use reflex::{Action, Application, Event, display, fs, peg, text};

global_asm!(include_str!("handoff.S"), options(raw));
unsafe extern "C" {
    fn handoff(image: u32, image_len: u32, dtb: u32, dtb_len: u32) -> !;
}
#[unsafe(link_section = ".payload")]
static mut PAYLOAD: [u8; CAPACITY] = [0; CAPACITY];
#[repr(C, align(16384))]
struct PageTable([u32; 4096]);
#[unsafe(link_section = ".page_table")]
#[used]
static mut PAGE_TABLE: PageTable = PageTable([0; 4096]);

struct Loader {
    status: String,
    loaded: usize,
    armed: bool,
}

unsafe fn verify_mapping() -> Result<(), &'static str> {
    let cpu = unsafe { brainux_probe::cpu::Cpu::read() };
    if cpu.midr & 0xff0f_fff0 != 0x410f_c070 || !cpu.supports_translation_probe() {
        return Err("Unsupported CPU/MMU");
    }
    if !matches!(cpu.cpsr & 31, 0x1f | 0x13) {
        return Err("Unsupported CPU mode");
    }
    for offset in (0..0x00f0_0000).step_by(4096) {
        if unsafe { cpu.translate(0x9000_0000 + offset) } != Ok(0x6710_0000 + offset)
            || unsafe { cpu.translate(0x6710_0000 + offset) } != Ok(0x6710_0000 + offset)
        {
            return Err("App/identity mapping mismatch");
        }
    }
    for offset in (0..FRAMEBUFFER_RESERVE).step_by(4096) {
        if unsafe { cpu.translate(FRAMEBUFFER + offset) } != Ok(FRAMEBUFFER + offset) {
            return Err("Reserved framebuffer mapping mismatch");
        }
    }
    for address in [
        0x4008_0000,
        0x4022_0000,
        0x4033_0000,
        0x4037_0000,
        0x4038_0000,
        0x403d_0000,
        0x403f_0000,
        0x4043_0000,
        0x40aa_0000,
        0x40b3_0000,
        0x410a_3c00,
    ] {
        if unsafe { cpu.translate(address) } != Ok(address) {
            return Err("MMIO mapping mismatch");
        }
    }
    Ok(())
}

fn load() -> Result<usize, String> {
    unsafe { verify_mapping() }.map_err(String::from)?;
    unsafe { quiesce::check() }.map_err(String::from)?;
    let path = unsafe { paths::beside_app(b"BOOT.BIN") }?;
    let size = unsafe { fs::metadata(&path) }
        .map_err(|e| format!("BOOT.BIN stat: {e}"))?
        .size as usize;
    if !(64..=CAPACITY).contains(&size) {
        return Err(String::from("BOOT.BIN exceeds staging RAM"));
    }
    let mut file =
        unsafe { fs::File::open(&path) }.ok_or_else(|| String::from("Cannot open BOOT.BIN"))?;
    let buffer =
        unsafe { core::slice::from_raw_parts_mut(ptr::addr_of_mut!(PAYLOAD).cast::<u8>(), size) };
    let mut offset = 0;
    while offset < size {
        let length = file
            .read(&mut buffer[offset..])
            .map_err(|e| format!("BOOT.BIN read: {e}"))?;
        if length == 0 {
            return Err(String::from("BOOT.BIN truncated"));
        }
        offset += length;
    }
    file.close().map_err(|e| format!("BOOT.BIN close: {e}"))?;
    Bundle::parse(buffer).map_err(String::from)?;
    Ok(size)
}

fn main() -> Loader {
    match load() {
        Ok(loaded) => Loader {
            loaded,
            armed: false,
            status: String::from("BOOT.BIN verified. Enter: prepare boot"),
        },
        Err(status) => Loader {
            loaded: 0,
            armed: false,
            status,
        },
    }
}

fn save_boot_snapshot() -> Result<(), String> {
    use core::fmt::Write;
    let mut report = String::from("Brainux boot display/watchdog snapshot v2\n");
    // Read configuration registers before disabling firmware services.
    for address in [
        0x40aa_0000usize,
        0x40aa_0010,
        0x40aa_0020,
        0x40aa_0030,
        0x40aa_0040,
        0x40aa_0050,
        0x40aa_0210,
        0x40aa_0220,
        0x40aa_0230,
        0x40b3_00a8,
        0x40b3_00a4,
    ] {
        let value = unsafe { ptr::read_volatile(address as *const u32) };
        writeln!(report, "{address:08x}={value:08x}").unwrap();
    }
    for (gate, base) in [
        (0x403f_00f4usize, 0x403d_0000usize),
        (0x403f_010c, 0x4043_0000),
    ] {
        let pcc = unsafe { ptr::read_volatile(gate as *const u32) };
        writeln!(report, "{gate:08x}={pcc:08x}").unwrap();
        // Do not enable a stopped clock merely to collect diagnostics.
        if pcc & (1 << 30) != 0 {
            for address in [base, base + 8, base + 12] {
                let value = unsafe { ptr::read_volatile(address as *const u32) };
                writeln!(report, "{address:08x}={value:08x}").unwrap();
            }
        }
    }
    let path = unsafe { paths::beside_app(b"BUXBOOT.TXT") }?;
    let mut file = unsafe { fs::File::create(&path) }
        .ok_or_else(|| String::from("Cannot create BUXBOOT.TXT"))?;
    file.write(report.as_bytes())
        .map_err(|e| format!("Boot log write: {e}"))?;
    file.close().map_err(|e| format!("Boot log close: {e}"))
}

impl Application for Loader {
    fn event(&mut self, event: Event) -> Action {
        let Event::Key {
            code, down: true, ..
        } = event
        else {
            return Action::None;
        };
        if code == peg::key::ESCAPE {
            return Action::Exit;
        }
        if self.loaded == 0 {
            return Action::None;
        }
        if code == peg::key::ENTER && !self.armed {
            self.armed = true;
            self.status = String::from("B: boot Linux (reset to return)   Esc: cancel");
            return Action::Redraw;
        }
        if self.armed && (code == b'B' as u16 || code == b'b' as u16) {
            unsafe {
                quiesce::progress("Boot: B received (display 2)");
                if let Err(error) = quiesce::check() {
                    self.status = String::from(error);
                    self.armed = false;
                    return Action::Redraw;
                }
                if let Err(error) = save_boot_snapshot() {
                    self.status = error;
                    self.armed = false;
                    return Action::Redraw;
                }
                quiesce::progress("Boot: verifying payload");
                let bytes =
                    core::slice::from_raw_parts(ptr::addr_of!(PAYLOAD).cast::<u8>(), self.loaded);
                let bundle = Bundle::parse(bytes).expect("Payload changed after verification");
                let physical = |slice: &[u8]| slice.as_ptr() as u32 - 0x9000_0000 + 0x6710_0000;
                let image = physical(bundle.image);
                let dtb = physical(bundle.dtb);
                let image_len = bundle.image.len() as u32;
                let dtb_len = bundle.dtb.len() as u32;
                let table = ptr::addr_of_mut!(PAGE_TABLE).cast::<u32>();
                for section in 0..4096 {
                    table.add(section).write_volatile(0);
                }
                for section in 0x600..0x680 {
                    table
                        .add(section)
                        .write_volatile((section as u32) << 20 | 0x1c02);
                }
                for section in 0..15 {
                    table
                        .add(0x900 + section)
                        .write_volatile(((0x671 + section as u32) << 20) | 0x1c02);
                }
                quiesce::stop();
                handoff(image, image_len, dtb, dtb_len);
            }
        }
        Action::None
    }
    fn draw(&self, pixels: &mut [u16]) {
        pixels.fill(0xffff);
        for (y, line) in [
            (30, "Brainux / PW-H1 Linux loader (display 2)"),
            (90, self.status.as_str()),
            (160, "Image -> 61008000   DTB -> 67000000"),
            (210, "USB serial console: 115200 / ttyGS0"),
            (290, "Experimental: V690_H1 only"),
            (350, "Esc: exit"),
        ] {
            text::draw(
                |x, y, c| pixels[y * display::WIDTH + x] = c,
                12,
                y,
                line,
                0,
                0xffff,
                1,
            );
        }
    }
}
const _: () = {
    assert!(IMAGE_DEST == 0x6100_8000);
    assert!(DTB_DEST == 0x6700_0000);
};
