ENTRY(_start)

PHDRS
{
  text PT_LOAD FLAGS(5); /* R + X */
  data PT_LOAD FLAGS(6); /* R + W */
}

SECTIONS
{
  /* Provided by ld -defsym=__user_base=... */
  . = __user_base;

  .text : ALIGN(16) {
    *(.text.startup)
    *(.text .text.*)
  } :text

  .rodata : ALIGN(16) {
    *(.rodata .rodata.*)
  } :text

  .data : ALIGN(16) {
    *(.data .data.*)
  } :data

  .bss : ALIGN(16) {
    *(.bss .bss.* COMMON)
  } :data

  /DISCARD/ : {
    *(.comment .note .note.*)
  }
}
