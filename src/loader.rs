#![no_std]
#![no_main]
extern crate alloc;
mod entry;
mod loader_screen;
mod paths;
mod quiesce;

use alloc::{format, string::String};
use brainux_probe::bundle::{
    Bundle, CAPACITY, DTB_DEST, FRAMEBUFFER, FRAMEBUFFER_RESERVE, IMAGE_DEST,
};
use core::{arch::global_asm, ptr};
use reflex::{Action, Application, Event, fs, peg};

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
    sd_ready: bool,
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

fn sd_ready() -> bool {
    // Probe the firmware mount, not the app path (the loader may be in internal storage).
    unsafe { fs::Directory::open(cr"SD0:\") }.is_some()
}

fn main() -> Loader {
    let sd_ready = sd_ready();
    match load() {
        Ok(loaded) => Loader {
            loaded,
            sd_ready,
            status: String::from("Boot image verified"),
        },
        Err(status) => Loader {
            loaded: 0,
            status,
            sd_ready,
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
        let direct = code == peg::key::ENTER;
        if direct {
            self.sd_ready = sd_ready();
            if !self.sd_ready {
                self.status = String::from("Insert a readable SD card, then press Enter again.");
                return Action::Redraw;
            }
        }
        if direct || code == b'D' as u16 || code == b'd' as u16 {
            unsafe {
                quiesce::progress("Boot: launch requested");
                if let Err(error) = quiesce::check() {
                    self.status = String::from(error);
                    return Action::Redraw;
                }
                if let Err(error) = save_boot_snapshot() {
                    self.status = error;
                    return Action::Redraw;
                }
                quiesce::progress("Boot: verifying payload");
                let bytes =
                    core::slice::from_raw_parts(ptr::addr_of!(PAYLOAD).cast::<u8>(), self.loaded);
                let bundle = Bundle::parse(bytes).expect("Payload changed after verification");
                let tree = fdt::Fdt::new(bundle.dtb).expect("Verified DTB");
                let selector = tree
                    .find_node("/chosen")
                    .and_then(|node| node.property("brainux,boot-root"))
                    .filter(|p| p.value.len() == 4)
                    .map(|p| p.value.as_ptr() as usize - bundle.dtb.as_ptr() as usize);
                if direct && selector.is_none() {
                    self.status = String::from("Update BOOT.BIN for SD boot. D: diagnostics");
                    return Action::Redraw;
                }
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
                if let Some(offset) = selector {
                    let payload_offset =
                        bundle.dtb.as_ptr() as usize - ptr::addr_of!(PAYLOAD) as usize + offset;
                    ptr::copy_nonoverlapping(
                        u32::from(direct).to_be_bytes().as_ptr(),
                        ptr::addr_of_mut!(PAYLOAD).cast::<u8>().add(payload_offset),
                        4,
                    );
                }
                quiesce::stop();
                handoff(image, image_len, dtb, dtb_len);
            }
        }
        Action::None
    }
    fn draw(&self, pixels: &mut [u16]) {
        loader_screen::draw(pixels, self.loaded, self.sd_ready, &self.status);
    }
}
const _: () = {
    assert!(IMAGE_DEST == 0x6100_8000);
    assert!(DTB_DEST == 0x6700_0000);
};
