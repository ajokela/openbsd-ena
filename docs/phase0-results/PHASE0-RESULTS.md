# OpenBSD `ena(4)` Phase 0 — Results

> **Date:** 2026-06-18/19. **Outcome: SUCCESS.** A freshly-written, OpenBSD-idiom
> `ena(4)` driver attaches to a real ENA device on real AWS Graviton instances,
> brings up its admin queue, and reads device attributes — over the serial console,
> exactly as the brief envisioned.

## What was proven

The hand-written driver (`sys/dev/pci/if_ena.c` + `if_enavar.h` + `if_enareg.h`):

1. **Compiles clean** under the OpenBSD kernel's `-Werror -Wall` against the real
   OpenBSD 7.9/arm64 source tree — zero warnings/errors, no code changes needed
   after review. `if_ena.o` (51 KB) links into a 19.5 MB `bsd`.
2. **Attaches on real hardware** (Graviton2 `t4g.nano`): PCI attach, BAR0 map,
   MSI-X establishment, device reset, admin-queue bring-up, and GET_FEATURE all work.
3. **Reads real device attributes** — MAC and MTU come back correct (not zeros),
   validating the Task 8 Critical fix (DEVICE_ATTRIBUTES returns inline in the ACQ,
   not via an indirect control buffer).

## Graviton2 (`t4g.nano`) serial console — the milestone lines

```
OpenBSD 7.9 (GENERIC.ENA) #1: Thu Jun 18 20:27:23 EDT 2026
smbios0: Amazon EC2 t4g.nano
agintc0 at mainbus0 ... agintcmsi0 at agintc0
acpipci0 at acpi0 PCI0
pci0 at acpipci0
ena0 at pci0 dev 5 function 0 "Amazon.com Elastic Network Adapter (VF)" rev 0x00: msix
ena0: ENA ver 0.10, ctrl ver 0, MTU max 9216, address 12:ad:bd:73:5d:45
ena0: attached
```

Full capture: `g2-t4g-nano-console.txt`.

Phase 0 sub-goals, all confirmed on silicon:
- PCI attach + BAR map — `ena0 at pci0 dev 5`
- **MSI-X on arm64** — `: msix` (the GIC-ITS plumbing that was the scariest unknown works)
- Device reset + admin queue — read `ENA ver 0.10`
- GET_FEATURE inline path — real MAC `12:ad:bd:73:5d:45`, `MTU max 9216`
- Clean attach — `ena0: attached`

## Graviton3 (`c7g.medium`) — the gating criterion — PASSED

Graviton3 (ARM Neoverse V1) carries the newest ENA, where stale HALs silently fail
to link. Using current `ena-com`, our driver attaches cleanly:

```
smbios0: Amazon EC2 c7g.medium
cpu0 at mainbus0 mpidr 0: ARM Neoverse V1 r1p1
ena0 at pci0 dev 5 function 0 "Amazon.com Elastic Network Adapter (VF)" rev 0x00: msix
ena0: ENA ver 0.10, ctrl ver 0, MTU max 9216, address 12:6b:79:e4:ac:bd
ena0: attached
```

Full capture: `g3-c7g-medium-console.txt`. **The spec's gating done-criterion is met.**

## Known unrelated issue

`nvme0: unable to create io q` on the EBS root volume — a separate Nitro/`nvme(4)`
problem that prevents the boot from reaching multiuser/login. It is independent of
`ena(4)`; the Phase 0 milestone is the `ena` attach over serial, which is met. Worth
investigating before any "boots to a usable system" goal (Phase 1+), but out of
scope for the Phase 0 spike.

## Go/no-go for Phase 1 (IO queues)

**GO.** The plumbing layer came up clean — PCI/BAR/`bus_dma`/MSI-X/admin-queue/reset
all work on real hardware with idiom-correct code. Per the brief, "if this comes up
clean, the rest is a bounded grind." The next phase (RX/TX descriptor rings, mbuf DMA,
per-queue interrupts, link + DHCP + first ping) is bounded engineering, planned
separately. The `nvme` root issue should be triaged so future phases can reach a
logged-in system rather than driving everything from `dmesg`.

## Reproduction

Build host + AMI bake + boot, via `~/projects/aws-arm-bsd` (branch `ena-phase0`):
`tooling/ena_host.py up` (persistent guest) → `tooling/ena_loop.py build_kernel` →
install built `bsd` as `/bsd` in the guest → `qemu-img convert` qcow2 to an EBS volume
→ snapshot → `register-image --architecture arm64 --boot-mode uefi --ena-support` →
launch a Graviton instance → `get-console-output --latest` for `dmesg`.
AMI built this round: `ami-02ca818571e880a6c`.
