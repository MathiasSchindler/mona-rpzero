ENTRY(_start)

SECTIONS
{
  /* Raspberry Pi firmware (and many QEMU Pi-ish setups) load kernel8.img at 0x80000. */
  . = 0x0000000000080000;
  __kernel_start = .;

  .text : ALIGN(16) {
    KEEP(*(.text.boot))
    *(.text .text.*)
  }

  .rodata : ALIGN(16) {
    *(.rodata .rodata.*)
  }

  .data : ALIGN(16) {
    *(.data .data.*)
  }

  .bss : ALIGN(16) {
    __bss_start = .;
    *(.bss .bss.* COMMON)
    __bss_end = .;
  }

  __kernel_end = .;

  /DISCARD/ : {
    *(.comment .note .note.*)
  }
}
