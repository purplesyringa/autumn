ENTRY(_start)
OUTPUT_FORMAT(binary)

SECTIONS {
    . = 0x4000000;
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
