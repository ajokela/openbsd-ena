# OpenBSD/arm64 EC2 Serial Console — Definitive Diagnosis (2026-06-19)

## The device (confirmed via Amazon Linux probe on identical hardware)
EC2 Graviton serial console = **16550A, memory-mapped at phys 0x80008000**,
exposed as **PCI device 1d0f:8250** (BAR0, 4K, IRQ 14), 115200 8n1.
Linux: `console=ttyS0,115200n8`, `uart:16550A mmio:0x80008000`.
ACPI **SPCR** table (OEM "AMAZON") designates it (base 0x80008000, SystemMemory).

## Why OpenBSD/arm64 has no console tty on EC2 (init: can't open /dev/console)
1. It's a PCI UART -> `com at acpi` (which DOES honor SPCR for console marking)
   never sees it; that path only handles ACPI-namespace UARTs (PNP0501 etc.).
2. `com at puc` is the right path but `pucdata.c` has NO 1d0f:8250 entry and no
   generic 16550-class fallback -> nothing attaches.
3. No global SPCR->comconsaddr hook on arm64, so even once `com` attaches it does
   NOT auto-become /dev/console. (`set tty com0` muted the kernel: the bootloader's
   default com0 != 0x80008000, and the kernel lost the console after ACPI init.)

## The fix (written, ready to apply — NOT yet validated on hardware)
A. pucdata.c: add an entry
     { {0x1d0f,0x8250,0,0}, {0xffff,0xffff,0,0}, { {PUC_PORT_COM,0x10,0x0000} } }
B. com_puc.c: in com_puc_attach, detect the SPCR-designated console via the global
   `acpi_softc->sc_tables` (mirror com_acpi_is_console using acpireg.h SPCR_SIG /
   GAS_SYSTEM_MEMORY / struct acpi_spcr); if base.address == sc_iobase, call
   `comcnattach(sc_iot, sc_iobase, B115200, sc_frequency, CS8...)` before
   com_attach_subr so cn_tab -> com and /dev/console works.
   Patch generator: docs/phase1-results/com_puc_console_patch.py
C. RAMDISK.ENA config: add `puc* at pci?` + `com* at puc?` (in ena_ramdisk_build.py).

## Status when paused
Patches written, NOT yet built/booted (build host i-0506 was terminated mid-work —
collateral from sharing the Name=qemu-openbsd-builder tag with the user's parallel
qemu_openbsd_build.py, whose cleanup terminates by that name). Next session: launch a
build host with a DISTINCT Name, apply A+B+C, build RAMDISK.ENA, bake, boot, verify
`com0 at puc` + `(console)` + no /dev/console loop, then the ena autorun ping test.

## This is upstream-worthy
Adding OpenBSD/arm64 EC2 serial-console support (pucdata + com_puc SPCR console) is a
real contribution independent of ena(4).
