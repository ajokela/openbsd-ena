# `ena(4)` — an OpenBSD driver for the AWS Elastic Network Adapter

A from-scratch OpenBSD/arm64 device driver for **AWS's Elastic Network Adapter
(ENA)** — the Nitro NIC that every modern EC2 instance, Graviton included,
presents to its guest. OpenBSD's tree has no `ena` driver, which is the single
reason OpenBSD/arm64 cannot run as a native EC2 guest: the instance boots, comes
up, and finds no network interface it understands. This is an effort to fix that.

> **Status: experimental, single-developer, not upstreamed.** The hard part —
> the device bring-up and data path — **works on real hardware**: on a live EC2
> Graviton2 (`t4g`) instance the driver attaches, creates its IO queues, brings
> the link up, and moves packets in both directions (a DHCP `DISCOVER`/`OFFER`
> round-trip completes). It is **not** production-ready, not reviewed by OpenBSD,
> and not yet a thing you would deploy. See [What "working" means](#what-working-means)
> for the exact line between the two.

The full story of building and debugging this is written up in two posts:
- [The OS That Couldn't See the Network](https://tinycomputers.io/posts/the-os-that-couldnt-see-the-network-native-openbsd-arm64-on-aws-graviton/) — why OpenBSD can't run natively on Graviton, and the QEMU-on-metal workaround that came first.
- [The Doorbell That Killed the Device](https://tinycomputers.io/posts/the-doorbell-that-killed-the-device-an-ena-driver-for-openbsd-on-graviton/) — writing this driver, and the bring-up bug that took five wrong theories to find.

---

## Why this exists

Modern EC2 instances expose their network through the **Elastic Network
Adapter**, a custom Amazon device. Linux has a driver for it, FreeBSD has one,
NetBSD's community AMIs have one. OpenBSD does not — not out of oversight, but
because OpenBSD is a small, fiercely curated tree and effectively nobody has run
it as a first-class EC2 guest, so nobody wrote the driver for Amazon's
proprietary NIC.

The consequence is sharp: an OpenBSD/arm64 instance on EC2 boots fine and then
finds no usable network interface. No SSH, no console you can drive, no way to
hand it work — a billable void. The previous project (first post above) worked
around this by *never letting OpenBSD touch the ENA at all*: run OpenBSD as a
KVM guest inside QEMU on a bare-metal Graviton host, hand it virtio devices it
already understands, and let the Linux host own the real Amazon adapter. That is
a good build server. It is not OpenBSD running natively on the cloud.

This driver is the other half of the itch: teach OpenBSD to speak ENA directly.

## What the driver does

ENA is a message-passing device that shares DMA rings with the host. The driver
implements the full Phase-1 bring-up and data path:

- **Admin queue** — a polled submission/completion ring pair for configuring the
  device (reset, read attributes, set features, create IO queues).
- **Host attributes** — registers the 4 KB host-info page the device expects
  (`SET_FEATURE(HOST_ATTR_CONFIG)`), identifying a real host driver.
- **AENQ** (Asynchronous Event Notification Queue) — the device's back-channel
  for link-state changes and ~1 Hz keep-alive heartbeats, drained from the
  management MSI-X interrupt.
- **IO queues** — a single RX queue and a single TX queue (`CREATE_CQ` /
  `CREATE_SQ`), with `bus_dma(9)`-mapped descriptor rings and mbuf buffers.
- **TX** — both the LLQ "push-mode" path (header written directly into a device
  BAR window, negotiated when the device advertises it) and the host-memory
  fallback used on VFs that don't offer LLQ.
- **RX** — completion-driven receive into 2 KB clusters, fed up the `ifnet`/`ifq`
  stack.
- **Link state** — propagated to the `ifnet` layer from AENQ link-change events.

It attaches as `ena*` on the PCI bus (`pci(4)`), matching Amazon's vendor ID
`0x1d0f`.

### The bug that defined the project

For a long while the device would attach cleanly, run for a few microseconds,
and then set its `FATAL_ERROR` status bit on its own — after which it silently
dropped the first `CREATE_CQ`, so no IO queues could be created and there was no
network. The root cause turned out to be ordering: the AENQ ring was being
registered (its base/caps written) *late*, in a separate step long after the
admin-queue handshake, whereas `ena-com` registers it *inside* admin init,
before the device goes "running." Registering it late left the device's AENQ
subsystem half-initialized; the head-doorbell write that activates the ring then
tripped `FATAL_ERROR`. Moving registration into admin init (the
`ena_aenq_register()` helper, called from `ena_admin_init()`) fixed it — and, as a bonus, made the device start
reporting the AENQ event groups it had previously claimed not to support, which
confirmed the two symptoms shared one cause. The long version is in the second
blog post.

## Repository layout

```
sys/dev/pci/
  if_ena.c              the driver
  if_enavar.h           softc, DMA helpers, queue geometry
  if_enareg.h           device register offsets + admin/IO ABI structs
  ena.files.fragment    the config(8) "files" line to splice into files.pci
docs/
  ena-idiom-map.md      per-symbol citation map: which ena-com/FreeBSD line each
                        register, struct, and constant was transcribed from
  ena-absence-check.md  confirmation that OpenBSD has no existing ena driver
  PHASE1-NOTES.md       working notes for the data-path bring-up
  phase0-results/       console logs from the attach milestone (real EC2)
  phase1-results/       console logs from the data-path milestone (real EC2),
                        plus EC2-CONSOLE-FINDINGS.md (the arm64 serial-console
                        story) and a com(4) SPCR-console patch used to get a
                        console at all on headless Graviton
reference/
  PROVENANCE.md         where the BSD reference sources come from + their licenses
  fetch-sources.sh      fetches the (gitignored) ena-com / FreeBSD reference trees
LICENSE                 BSD 3-Clause (+ ABI provenance note)
```

The `reference/` source trees (Amazon `ena-com`, FreeBSD `sys/dev/ena`) are **not
checked in** — they are third-party, large, and in the case of the Linux driver,
GPL. `reference/fetch-sources.sh` pulls them locally for development; they are
`.gitignore`d.

## How it's built and tested

OpenBSD can't (yet) run on Graviton, which makes testing an ENA driver for
Graviton circular. Two harnesses break the loop. The orchestration tooling lives
in a separate repository; what follows is the shape of it.

1. **Build host.** An `a1.metal` (bare-metal Graviton) instance runs Ubuntu with
   KVM. OpenBSD/arm64 runs as a QEMU/KVM guest on it (virtio disk + NIC), which
   is where the kernel is built. This is the build server from the first blog
   post.

2. **AMI bake → real instance.** The built kernel is assembled into a `bsd.rd`
   ramdisk, written into a bootable arm64 root volume, snapshotted, and
   registered as an AMI (`--ena-support`, UEFI boot mode). A `t4g.nano`
   (Graviton2) instance is launched from it, the serial console is captured, and
   the instance is terminated. This is the **real-hardware** test: a genuine EC2
   Graviton2 VF. The `docs/*-results/` logs come from these boots. End-to-end it
   takes ~15-20 minutes.

3. **VFIO fast loop.** For iteration, a *second* ENI is attached to the
   `a1.metal` host and passed through to the OpenBSD guest via VFIO/`vfio-pci`
   (the host's ARM SMMU does the DMA translation). The guest's `ena(4)` then
   binds a real ENA device with no AMI bake — ~2 minutes per iteration instead of
   ~18. The passed-through device on metal is a full-featured ENI rather than a
   VF, so final validation still uses path (2) on a real `t4g` VF.

### Getting a console at all

A side-quest worth flagging: a headless OpenBSD/arm64 EC2 instance has no
console until something attaches a tty to the SPCR-designated UART, which on
Graviton has no `_HID` namespace device for the existing ACPI drivers to bind.
`docs/phase1-results/` documents the SPCR-only console attach used here; it's an
independent fix that any native-OpenBSD-on-EC2 effort needs.

### Integrating the driver into an OpenBSD source tree

The driver is a standard `pci(4)` attachment. To build it into a kernel:

- drop `if_ena.c`, `if_enavar.h`, `if_enareg.h` into `sys/dev/pci/`
- add the line from `ena.files.fragment` to `sys/dev/pci/files.pci`
- add `ena* at pci?` to your kernel config (e.g. `GENERIC`)

It compiles cleanly under OpenBSD 7.9/arm64 with `-Werror`.

## What "working" means

Being precise, because "a working ENA driver" can mean several things and only
the first is true today.

**What works**, verified on a real EC2 Graviton2 instance:
- attach, reset, admin queue, host attributes, device attributes, AENQ, and IO
  queue creation all complete; `DEV_STS` stays healthy through bring-up
- the link comes up and AENQ keep-alive events flow
- **TX and RX work** — packets the device really puts on / takes off the wire; a
  DHCP `DISCOVER`/`OFFER` round-trip completes
- compiles under `-Werror`; the data path is the normal `ifnet`/`ifq` path

**What does not work yet / is unverified:**
- **No full native install.** Testing boots the `bsd.rd` install ramdisk, not a
  disk-installed OpenBSD that comes up multiuser with `ena0` as its primary NIC
  and lets you SSH in over it. That milestone — the one that would retire the
  QEMU-shim build server — is not done.
- **Robustness is thin.** One DHCP round-trip is not throughput, multi-queue,
  checksum offload, MTU changes, link flaps, or days of uptime.
- **No device reset / recovery.** There is no keep-alive watchdog driving a
  device reset if the link or device wedges (FreeBSD/Linux have this; it's a
  Phase-2 concern).
- **Not reviewed, not upstreamed.** No OpenBSD developer has looked at it; it has
  not been submitted to `tech@` and would need cleanup and review first. The
  per-file license headers carry an empty `$OpenBSD$` tag as a placeholder, not a
  claim of inclusion.

In short: the genuinely hard, genuinely doubted thing — does a from-scratch ENA
implementation function on real hardware — is answered **yes**. Turning that into
something you'd run is future work.

## Provenance and licensing

The driver code is licensed **BSD 3-Clause** (see `LICENSE`).

It was written by porting from **BSD-licensed sources only**: Amazon's `ena-com`
hardware-abstraction layer (BSD-2-Clause / Linux-OpenIB dual-licensed) and
FreeBSD's `sys/dev/ena` driver (BSD-2-Clause). The Linux `ena` driver is
GPL-2.0 and was treated as **read-only reference** — consulted for intent, never
copied. The device register offsets and on-the-wire descriptor layouts are the
ENA hardware ABI (facts, not expression); `docs/ena-idiom-map.md` maps each one
to the exact `ena-com`/FreeBSD line it was transcribed from, and
`reference/PROVENANCE.md` records where those trees come from.

## Roadmap

- **Phase 0 — attach** ✓ — PCI attach, admin queue, read device attributes (real EC2)
- **Phase 1 — data path** ✓ — IO queues, link, TX/RX, DHCP round-trip (real EC2 Graviton2)
- **Phase 2 — make it real** — userland-driven `dhclient`/`ping` from a full
  install; keep-alive watchdog + device reset/recovery; multi-queue; offloads;
  MTU/jumbo; stress/soak; cleanup and a submission to OpenBSD `tech@`

## Credits

- Amazon's [`amzn-drivers`](https://github.com/amzn/amzn-drivers) `ena-com` and
  the FreeBSD `ena(4)` driver — the BSD-licensed references this was ported from.
- The OpenBSD project, whose `if_mcx.c` and other PCI/`bus_dma` drivers were the
  idiom this follows.

This was built by a single developer with heavy AI assistance (Claude); the
blog posts are candid about where that helped and where it confidently went
wrong.

## Disclaimer

Experimental software for a network device on someone else's hardware. It can
fault the device, panic the kernel, or behave in ways not yet tested. Run it on
disposable instances you own. No warranty; see `LICENSE`.
