# Reference Source Provenance

| Source | URL | Pinned commit | License | Use |
|--------|-----|---------------|---------|-----|
| amzn-drivers (ena-com + spec) | github.com/amzn/amzn-drivers | eba0c722005466fdd2c68398daf0ac01093a92bc | BSD-2 | protocol source of truth |
| FreeBSD (sys/dev/ena, sys/contrib/ena-com) | github.com/freebsd/freebsd-src | 92bdcb08113c05ec18ad420a4d84dd858e7f5e64 | BSD-2 | glue + HAL reference |
| NetBSD (sys/dev/pci/if_ena*) | github.com/NetBSD/src | feb359c4071a943fd06a5d69697a36c0c76b77b6 | BSD-2 | closest glue to OpenBSD |
| Linux (drivers/.../amazon/ena) | github.com/torvalds/linux | 83f1454877cc292b88baf13c829c16ce6937d120 | GPL | READ-ONLY, do not copy |

## Environment variables for fetch-sources.sh

```sh
export AMZN_REF=eba0c722005466fdd2c68398daf0ac01093a92bc
export FREEBSD_REF=92bdcb08113c05ec18ad420a4d84dd858e7f5e64
export NETBSD_REF=feb359c4071a943fd06a5d69697a36c0c76b77b6
export LINUX_REF=83f1454877cc292b88baf13c829c16ce6937d120
```

## Path correction

The brief assumed ena-com in amzn-drivers lives at `kernel/linux/ena/ena_com.c`.
The actual path is `kernel/linux/common/ena_com/ena_com.c` (and siblings).
The fetch script and verification gate use the corrected path.

## ena-com freshness comparison

Both sources use `ENA_COMMON_SPEC_VERSION_MAJOR=2, MINOR=0`.

| Metric | amzn-drivers | FreeBSD sys/contrib/ena-com |
|--------|-------------|----------------------------|
| Last commit to ena_com.c | 2026-04-30 | 2024-10-15 |
| Commit message | linux/ena: Fix max_entries_in_tx_burst calculation | ena: Upgrade ena-com to freebsd v2.8.0 |
| ena_com.c line count | 3820 | 3552 |

**Winner: amzn-drivers** — last touched April 2026 (18 months newer than FreeBSD's
October 2024 vendor import), and carries 268 more lines of ena_com.c code.
All downstream tasks should treat `amzn-drivers/kernel/linux/common/ena_com/` as
the protocol source of truth.
