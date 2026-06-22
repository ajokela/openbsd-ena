# Phase 1 Carry-Forward Notes

Phase 0 is complete and hardware-validated (see `phase0-results/`). The final
whole-branch review found **no Critical issues**; the validated attach path is
correct. The items below are robustness / upstream-quality hardening to address
as Phase 1 (IO queues) is built. None require rework of the Phase 0 path before
building on it.

## Important (settle early or fix before tech@ submission)

1. **Endianness convention — settle BEFORE writing Phase 1 RX/TX descriptors.**
   ENA descriptor fields are little-endian; arm64/amd64 are LE so native reads
   are correct in practice, but multi-byte descriptor reads should be wrapped in
   `letoh16`/`letoh32` (and `htole*` on writes) to be portable and pass tech@.
   Settling this now means the much larger Phase 1 descriptor code is written
   endian-correct from the start rather than retrofitted. Affected Phase 0 sites
   (low priority to backfill): `if_ena.c` admin completion + `dev_attr` reads.

2. **Add closing `bus_dmamap_sync(POSTREAD|POSTWRITE)` before ring unload in
   `ena_detach`** (mirror `if_mcx.c` teardown call sites), so the rings are
   released from device ownership symmetrically. `ena_reset` already quiesces the
   device first, so this is hardening, not a live bug.

3. **Guard the readless/MMIO all-ones sentinel in `ena_reset`.** A device
   returning `0xFFFFFFFF` for `DEV_STS`/`CAPS` (link-down PCIe / surprise-removal)
   would pass the `READY_MASK` check and derive a bogus timeout. Add an all-ones
   guard before trusting status/caps. (ena-com's readless path does this.)

4. **Make `ena_detach` robust to a partially-failed attach** — e.g.
   `if (sc->sc_mems == 0) return (0);` at the top, and/or only free rings whose
   `edm_map` is non-NULL. Safe today (detach only follows successful attach) but
   cheap insurance.

## Minor (polish)

5. `if_enareg.h`: rename `reserved7[2]` in `device_attr_feature_desc` to the real
   `uint16_t flow_steering_max_entries;` (same layout; accurate ABI naming).
6. `ena_admin_poll`: validate the completion `command_id` against the issued id
   (ena-com does via its comp_ctx table). Unneeded for single-outstanding Phase 0;
   useful once admin commands pipeline.
7. Comment that the `sc_admin_cmd_id & qmask` wrap is intentionally the ring depth,
   not the 12-bit `COMMAND_ID` field width (matches ena-com).
8. For tech@ submission, consider the plain `/*` ISC header block (OpenBSD idiom)
   vs. the `/*-` FreeBSD form currently used.

## Separate, non-ena issue to triage for Phase 1

- **`nvme0: unable to create io q`** on the EBS root volume (both Graviton2 and
  Graviton3). This is an `nvme(4)`/Nitro IO-queue problem, unrelated to `ena`, but
  it stops the boot from reaching multiuser/login — so Phase 1 (which wants DHCP +
  ping on a running system, not just `dmesg`) needs this resolved to get a usable
  shell. Investigate OpenBSD `nvme(4)` MSI-X / IO-queue creation under Nitro.

## Phase 1 scope (the "it works" phase, planned separately)

RX/TX descriptor rings, mbuf DMA, per-queue MSI-X interrupts (real interrupt-driven
path, not polled), `ifnet`/`ifmedia` integration (splice the `ena.files.fragment`
`ifnet`/`ether`/`ifmedia` attributes then), link state, DHCP + first ping.
