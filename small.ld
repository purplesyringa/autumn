ENTRY(_start)
OUTPUT_FORMAT(elf64-x86-64)

SECTIONS {
    . = 0x400000 + SIZEOF_HEADERS;
    .text : {
        *(.text .text.* .data .data.* .rodata .rodata.*)
    }
    .bss : {
        *(.bss .bss.*) *(COMMON)
    }
    /DISCARD/ : {
        *(.note.*)
    }
}
