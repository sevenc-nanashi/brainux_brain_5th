# Third-party sources

- Reflex: `../brain_5th_hack`, MIT, copyright 2026 Nanashi. The app bundles its font license;
  source is used as a path dependency and is not copied into this repository.
- Linux: brain-hackers/linux-brain `5e99ce4dc22d77bb029133d7aa6483486c684f20`, GPL-2.0.
  Local changes are in `kernel/pwh1.patch`, `kernel/pwh1-refresh.c`, and `kernel/pwh1-brainux.dts`.
  The cache-maintenance sequence in `src/handoff.S` follows the ARMv7 set/way algorithm also
  implemented in Linux's `arch/arm/mm/cache-v7.S`.
- BusyBox: 1.36.1, `1a64f6a20aaf6ea4dbba68bbfa8cc1ab7e5c57c4`, GPL-2.0-only.
  Configuration changes and initramfs assembly are recorded in `scripts/build-linux.rb`.
- fdt 0.1.5: MPL-2.0, https://crates.io/crates/fdt/0.1.5 . Used unmodified.
- sha2 0.10.9 and its RustCrypto dependencies: MIT OR Apache-2.0.
  Exact dependency versions and source checksums are in Cargo.lock.

`make fetch` obtains the Linux and BusyBox source used by `make image`.
When redistributing the generated kernel/BusyBox binaries, provide their corresponding
source, the local board files/patch, build scripts and configuration under their licenses.
