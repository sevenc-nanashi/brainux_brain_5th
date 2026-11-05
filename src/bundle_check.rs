use brainux_probe::bundle::Bundle;
fn main() {
    let path = std::env::args()
        .nth(1)
        .expect("usage: bundle-check BOOT.BIN");
    let bytes = std::fs::read(path).expect("read BOOT.BIN");
    let bundle = Bundle::parse(&bytes).expect("validate BOOT.BIN");
    println!(
        "Image: {} bytes, DTB: {} bytes; SHA-256 and board layout verified",
        bundle.image.len(),
        bundle.dtb.len()
    );
}
