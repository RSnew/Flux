/* linker.ld — FluxOS kernel linker script */

ENTRY(_start)

SECTIONS
{
    /* Load kernel at 1MB — standard address above real-mode memory */
    . = 1M;

    /* Multiboot header must come first */
    .multiboot ALIGN(4) : {
        *(.multiboot)
    }

    /* Code section */
    .text ALIGN(4K) : {
        *(.text)
    }

    /* Read-only data */
    .rodata ALIGN(4K) : {
        *(.rodata)
        *(.rodata.*)
    }

    /* Initialized data */
    .data ALIGN(4K) : {
        *(.data)
    }

    /* Uninitialized data (zero-filled) */
    .bss ALIGN(4K) : {
        *(COMMON)
        *(.bss)
    }

    /* Discard unwanted sections */
    /DISCARD/ : {
        *(.comment)
        *(.note.*)
    }
}
