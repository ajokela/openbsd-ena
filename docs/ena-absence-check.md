# ENA Driver Absence Check — OpenBSD -current

**Date:** 2026-06-18
**Purpose:** Confirm there is no existing `ena(4)` driver in OpenBSD -current before
beginning new driver development, and record what Amazon/ENA-related PCI IDs already
exist upstream.

---

## Step 1: Mirror Reachability

```
$ git ls-remote https://github.com/openbsd/src.git | head -1
e607e174aea6c03a27fa493a7e743e3cfd78d85c	HEAD
```

Mirror confirmed reachable. HEAD commit: `e607e174aea6c03a27fa493a7e743e3cfd78d85c`.

---

## Step 2: Check `files.pci` for any `ena` driver

```
$ curl -s 'https://raw.githubusercontent.com/openbsd/src/master/sys/dev/pci/files.pci' \
    | grep -i -n 'ena' || echo "NO ENA MATCH"
NO ENA MATCH
```

**Result:** No `ena` entry exists in `sys/dev/pci/files.pci`. The driver name `ena` is
free to use.

---

## Step 3: Check `pcidevs` for Amazon (vendor 0x1d0f) entries

### Search by vendor ID

```
$ curl -s 'https://raw.githubusercontent.com/openbsd/src/master/sys/dev/pci/pcidevs' \
    | grep -n '1d0f\|1D0F' || echo "NO 0x1d0f VENDOR"
NO 0x1d0f VENDOR
```

**Result:** Amazon's PCI vendor ID `0x1d0f` is not yet present in OpenBSD's `pcidevs`.

### Search by name "amazon"

```
$ curl -s 'https://raw.githubusercontent.com/openbsd/src/master/sys/dev/pci/pcidevs' \
    | grep -i 'amazon\|0x1d0f' || echo "NO AMAZON IDS"
product VIATECH VT86C926	0x0926	VT86C926 Amazon
```

**Result:** The only match is a VIA Technologies product (`VT86C926`) whose description
contains the word "Amazon" — this is unrelated to Amazon Web Services or the ENA NIC.
There are no Amazon (0x1d0f) vendor or product entries in `pcidevs`.

---

## Step 4: Broad `ena`-in-path check (supplemental)

A search of all top-level tree paths in the openbsd/src master tree containing "ena"
returned only GNU compiler/toolchain files (OpenACC, rename utilities, basename tests)
— nothing in `sys/dev` or any network driver subtree.

---

## Conclusions

| Check | Result |
|---|---|
| `ena` in `sys/dev/pci/files.pci` | **ABSENT** |
| Amazon vendor `0x1d0f` in `pcidevs` | **ABSENT** |
| Any `ena` network driver path in tree | **ABSENT** |

**The name `ena` is available.** No collision with existing upstream work.

### PCI IDs to add in a future task

When the time comes, the following entries will need to be added to `pcidevs`:

```
vendor AMAZON		0x1d0f	Amazon
product AMAZON ENA	0xec20	Elastic Network Adapter
product AMAZON ENA_LLQ	0xec21	Elastic Network Adapter (LLQ)
```

ENA device IDs sourced from the Linux `ena` driver (`drivers/net/ethernet/amazon/ena/`).
Verify against the latest kernel before adding.
