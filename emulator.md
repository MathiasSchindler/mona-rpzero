Wie bereits erwähnt, nutzt der Zero 2 W denselben Prozessor wie der Pi 3B, weshalb raspi3b die stabilste Wahl für die Emulation ist.

## Kernel-first Workflow (empfohlen)

Das Repo ist auf den **bare-metal** Workflow ausgelegt: eigener Kernel + kleines Userland, direkt in QEMU geladen (kein SD-Image / kein Linux).

Voraussetzungen (Debian/Ubuntu grob):

- `qemu-system-aarch64`
- AArch64 Cross-Toolchain (z.B. `aarch64-linux-gnu-gcc`)

Hinweis: QEMU verlangt für `-M raspi3b` aktuell **1 GiB RAM** (Pi 3B-Profil). Das entspricht nicht dem echten Zero 2 W, ist aber für die Emulation der praktikabelste Weg.

## Bare-metal Quickstart

Build:

```bash
make
```

Run:

```bash
make run
```

Clean:

```bash
make clean
```

Exception-Dump testen (absichtlicher Fault):

```bash
make run TEST_FAULT=1
```

### Make-Variablen (Overrides)

- `DTB=...` Pfad zur DTB-Datei
- `MEM=1024` RAM in MiB für QEMU (Default: 1024)

Beispiel:

```bash
make DTB=out/bcm2710-rpi-zero-2-w.dtb
make run MEM=1024
```