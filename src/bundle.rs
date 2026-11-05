use sha2::{Digest, Sha256};

pub const CAPACITY: usize = 9 * 1024 * 1024;
pub const MAX_IMAGE: usize = CAPACITY - 64 - 65536;
pub const IMAGE_DEST: u32 = 0x6100_8000;
pub const DTB_DEST: u32 = 0x6700_0000;
pub const FRAMEBUFFER: u32 = 0x66e0_0000;
pub const FRAMEBUFFER_RESERVE: u32 = 0x0010_0000;
pub const FRAME_BYTES: u32 = 480 * 854 * 2;

pub struct Bundle<'a> {
    pub image: &'a [u8],
    pub dtb: &'a [u8],
}

impl<'a> Bundle<'a> {
    pub fn parse(bytes: &'a [u8]) -> Result<Self, &'static str> {
        if bytes.len() < 64 || bytes.len() > CAPACITY || &bytes[..8] != b"BUXBOOT1" {
            return Err("Invalid BOOT.BIN header");
        }
        let image_len = u32::from_le_bytes(bytes[8..12].try_into().unwrap()) as usize;
        let dtb_len = u32::from_le_bytes(bytes[12..16].try_into().unwrap()) as usize;
        if !(4096..=MAX_IMAGE).contains(&image_len) || !(40..=65536).contains(&dtb_len) {
            return Err("Image/DTB size out of bounds");
        }
        let dtb_offset = 64 + image_len.next_multiple_of(8);
        if dtb_offset + dtb_len != bytes.len() || bytes[48..64] != [0; 16] {
            return Err("BOOT.BIN length/version mismatch");
        }
        if Sha256::digest(&bytes[64..])[..] != bytes[16..48] {
            return Err("BOOT.BIN checksum mismatch");
        }
        let bundle = Self {
            image: &bytes[64..64 + image_len],
            dtb: &bytes[dtb_offset..],
        };
        let tree = fdt::Fdt::new(bundle.dtb).map_err(|_| "Invalid DTB")?;
        if tree.total_size() != dtb_len {
            return Err("DTB length mismatch");
        }
        let root = tree.find_node("/").ok_or("DTB root missing")?;
        let compatible = root.compatible().ok_or("DTB compatible missing")?;
        if !compatible.all().any(|name| name == "sharp,pw-h1-brainux") {
            return Err("DTB is not the Brainux PW-H1 profile");
        }
        let memory = tree
            .find_node("/memory@61000000")
            .ok_or("DTB memory missing")?;
        if memory
            .property("reg")
            .ok_or("DTB memory reg missing")?
            .value
            != [0x61, 0, 0, 0, 7, 0, 0, 0]
        {
            return Err("DTB RAM layout mismatch");
        }
        if root.property("#address-cells").map(|p| p.value) != Some(&[0, 0, 0, 1])
            || root.property("#size-cells").map(|p| p.value) != Some(&[0, 0, 0, 1])
        {
            return Err("DTB cell sizes mismatch");
        }
        let reserved = tree
            .find_node("/reserved-memory")
            .ok_or("DTB framebuffer reservation missing")?;
        if reserved.property("#address-cells").map(|p| p.value) != Some(&[0, 0, 0, 1])
            || reserved.property("#size-cells").map(|p| p.value) != Some(&[0, 0, 0, 1])
            || reserved.property("ranges").map(|p| p.value) != Some(&[])
        {
            return Err("DTB reservation addressing mismatch");
        }
        let reservation = tree
            .find_node("/reserved-memory/framebuffer@66e00000")
            .ok_or("DTB framebuffer reservation missing")?;
        let framebuffer = tree
            .find_node("/chosen/framebuffer@66e00000")
            .ok_or("DTB framebuffer address mismatch")?;
        if reservation.property("reg").map(|p| p.value) != Some(&[0x66, 0xe0, 0, 0, 0, 0x10, 0, 0])
            || reservation.property("no-map").map(|p| p.value) != Some(&[])
            || reservation.property("reusable").is_some()
            || framebuffer.property("reg").map(|p| p.value)
                != Some(&[0x66, 0xe0, 0, 0, 0, 0x0c, 0x82, 0x80])
        {
            return Err("DTB framebuffer reservation/layout mismatch");
        }
        Ok(bundle)
    }
}

const _: () = {
    assert!(IMAGE_DEST + (MAX_IMAGE as u32) < DTB_DEST);
    assert!(DTB_DEST + 65536 < 0x6710_0000);
    assert!(0x6770_0000u32 + CAPACITY as u32 == 0x6800_0000);
    assert!(IMAGE_DEST + (MAX_IMAGE as u32) < FRAMEBUFFER);
    assert!(FRAME_BYTES < FRAMEBUFFER_RESERVE);
    assert!(FRAMEBUFFER + FRAMEBUFFER_RESERVE <= DTB_DEST);
    assert!(FRAMEBUFFER == 0x66e0_0000); // handoff.S and DTB profile
    assert!(FRAME_BYTES == 0xc8280);
    assert!(FRAMEBUFFER_RESERVE == 0x0010_0000);
};
