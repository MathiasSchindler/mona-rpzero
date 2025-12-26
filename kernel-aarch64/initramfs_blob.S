.section .rodata.initramfs, "a"

.global initramfs_start
.global initramfs_end

/* Embedded initramfs (CPIO newc), built by userland/Makefile. */
initramfs_start:
    .incbin "../userland/build/initramfs.cpio"
initramfs_end:
