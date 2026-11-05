OUTPUT_FORMAT("elf32-littlearm")
OUTPUT_ARCH(arm)
ENTRY(_start)
MEMORY { APP (rwx) : ORIGIN = 0x90000000, LENGTH = 15M }
SECTIONS {
    .text : { KEEP(*(.text.entry)) *(.text .text.*) *(.rodata .rodata.*) . = ALIGN(16); } > APP
    .page_table 0x90500000 (NOLOAD) : { KEEP(*(.page_table)) } > APP
    .data 0x90504000 : AT(LOADADDR(.text) + SIZEOF(.text)) {
        __data_start = .; *(.data .data.*) . = ALIGN(16); __data_end = .;
    } > APP
    __data_load = LOADADDR(.data);
    .bss (NOLOAD) : {
        __bss_start = .; *(.bss .bss.*) *(COMMON) . = ALIGN(16); __bss_end = .;
    } > APP
    .payload 0x90600000 (NOLOAD) : { KEEP(*(.payload)) } > APP
    /DISCARD/ : { *(.ARM.exidx*) *(.ARM.extab*) *(.eh_frame*) *(.comment) }
}
ASSERT(_start == 0x90000000, "AppMain entry moved")
ASSERT(LOADADDR(.data) + SIZEOF(.data) <= 0x90500000, "Code overlaps data")
ASSERT(__bss_end <= 0x90600000, "Data overlaps payload")
ASSERT(SIZEOF(.payload) == 0x900000, "Payload size changed")
ASSERT(SIZEOF(.page_table) == 0x4000, "Page table size changed")
