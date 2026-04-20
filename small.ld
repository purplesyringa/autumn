ENTRY(_start)
OUTPUT_FORMAT(binary)

SECTIONS {
    . = 0x400000 + SIZEOF_HEADERS;
    .text : {
        *(.text.start)
        *(.text .text.*)
        *(.data .data.* .rodata .rodata.*)
    }
    .bss : {
        *(.bss .bss.*) *(COMMON)
    }
    /DISCARD/ : {
        *(.note.*)
    }
}
