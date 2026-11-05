CARGO ?= cargo
OBJCOPY ?= llvm-objcopy-20
TARGET := armv7a-none-eabi
APP := target/APP/BrainuxProbe
LINKER := $(abspath ../brain_5th_hack/crates/reflex-cli/link.x)

.PHONY: all check fmt
all:
	$(CARGO) rustc --locked --release --target $(TARGET) -- -C target-cpu=cortex-a7 -C relocation-model=static -C link-arg=-T$(LINKER) -C link-arg=--no-undefined
	mkdir -p $(APP)
	$(OBJCOPY) -O binary target/$(TARGET)/release/brainux-probe $(APP)/AppMain.bin
	touch $(APP)/index.din
	cp ../brain_5th_hack/crates/reflex/assets/FONT-LICENSE.txt $(APP)/FONT-LICENSE.txt

check:
	$(CARGO) fmt --check
	$(CARGO) clippy --locked --target $(TARGET) -- -D warnings

fmt:
	$(CARGO) fmt

