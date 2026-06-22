# ENA Idiom Map — Phase 0 protocol surface + OpenBSD glue surface

Load-bearing reference for the OpenBSD `ena(4)` port. Tasks 4–8 transcribe register
offsets, struct layouts, and admin opcodes **from the citations in this file**, and
mirror the OpenBSD idiom patterns recorded here. A wrong citation here becomes a
plausible-but-wrong DMA bug later — every transcription target below has a verified
`file:line`.

## 0. Source-of-truth paths (read this before anything else)

The Phase 0 plan/brief says ena-com lives at `kernel/linux/ena/`. **That is wrong.**
The real locations:

| Source | ena-com (protocol) path | glue path |
| --- | --- | --- |
| amzn-drivers (canonical) | `reference/amzn-drivers/kernel/linux/common/ena_com/` | `kernel/linux/ena/` is the Linux *netdev glue*, **not** ena-com |
| FreeBSD (vendored) | `reference/freebsd/sys/contrib/ena-com/` (+ `ena_defs/` subdir for the `*_defs.h`) | `reference/freebsd/sys/dev/ena/` |
| NetBSD (closest to OpenBSD) | bundled inside `reference/netbsd/sys/dev/pci/` | `reference/netbsd/sys/dev/pci/if_ena.c`, `if_enavar.h` |

Notes that bite later:
- In **amzn-drivers**, all `*_defs.h` are flat in `common/ena_com/`
  (`ena_regs_defs.h`, `ena_admin_defs.h`, `ena_common_defs.h`).
- In **FreeBSD's** tree the same `*_defs.h` live one level down in
  `sys/contrib/ena-com/ena_defs/`. The `ena_com.c`/`ena_com.h`/`ena_plat.h` are at
  the top of `ena-com/`. Line numbers in this doc for `ena_com.c` and the `*_defs.h`
  are taken from the **FreeBSD** tree (per the brief), which is byte-for-byte the
  vendored ena-com. Register offsets are cited from **amzn-drivers**
  `ena_regs_defs.h` (the offsets are identical across trees).
- `ena_plat.h` is the **platform shim** — it is the file we replace wholesale with
  OpenBSD idiom. It is the Rosetta stone showing exactly which `bus_space`/`bus_dma`
  call each ena-com macro expands to (FreeBSD spelling). See §3.

---

## 1. ENA device-protocol surface (Phase 0 subset)

### 1a. BAR0 register offsets — `ena_regs_defs.h`

File: `reference/amzn-drivers/kernel/linux/common/ena_com/ena_regs_defs.h`
(identical offsets in `reference/freebsd/sys/contrib/ena-com/ena_defs/ena_regs_defs.h`).

| Concept | `ENA_REGS_*` symbol | offset | file:line |
| --- | --- | --- | --- |
| Version | `ENA_REGS_VERSION_OFF` | 0x0 | ena_regs_defs.h:37 |
| Controller version | `ENA_REGS_CONTROLLER_VERSION_OFF` | 0x4 | ena_regs_defs.h:38 |
| Caps (reset timeout, DMA addr width, admin cmd TO) | `ENA_REGS_CAPS_OFF` | 0x8 | ena_regs_defs.h:39 |
| Caps ext | `ENA_REGS_CAPS_EXT_OFF` | 0xc | ena_regs_defs.h:40 |
| Admin SQ base low | `ENA_REGS_AQ_BASE_LO_OFF` | 0x10 | ena_regs_defs.h:41 |
| Admin SQ base high | `ENA_REGS_AQ_BASE_HI_OFF` | 0x14 | ena_regs_defs.h:42 |
| Admin SQ caps (depth, entry size) | `ENA_REGS_AQ_CAPS_OFF` | 0x18 | ena_regs_defs.h:43 |
| Admin CQ base low | `ENA_REGS_ACQ_BASE_LO_OFF` | 0x20 | ena_regs_defs.h:44 |
| Admin CQ base high | `ENA_REGS_ACQ_BASE_HI_OFF` | 0x24 | ena_regs_defs.h:45 |
| Admin CQ caps | `ENA_REGS_ACQ_CAPS_OFF` | 0x28 | ena_regs_defs.h:46 |
| Admin SQ doorbell | `ENA_REGS_AQ_DB_OFF` | 0x2c | ena_regs_defs.h:47 |
| Admin CQ tail | `ENA_REGS_ACQ_TAIL_OFF` | 0x30 | ena_regs_defs.h:48 |
| AENQ caps | `ENA_REGS_AENQ_CAPS_OFF` | 0x34 | ena_regs_defs.h:49 |
| AENQ base low | `ENA_REGS_AENQ_BASE_LO_OFF` | 0x38 | ena_regs_defs.h:50 |
| AENQ base high | `ENA_REGS_AENQ_BASE_HI_OFF` | 0x3c | ena_regs_defs.h:51 |
| AENQ head doorbell | `ENA_REGS_AENQ_HEAD_DB_OFF` | 0x40 | ena_regs_defs.h:52 |
| AENQ tail | `ENA_REGS_AENQ_TAIL_OFF` | 0x44 | ena_regs_defs.h:53 |
| Interrupt mask | `ENA_REGS_INTR_MASK_OFF` | 0x4c | ena_regs_defs.h:54 |
| Device control (reset trigger) | `ENA_REGS_DEV_CTL_OFF` | 0x54 | ena_regs_defs.h:55 |
| Device status (READY etc.) | `ENA_REGS_DEV_STS_OFF` | 0x58 | ena_regs_defs.h:56 |
| MMIO readless request | `ENA_REGS_MMIO_REG_READ_OFF` | 0x5c | ena_regs_defs.h:57 |
| MMIO readless resp low | `ENA_REGS_MMIO_RESP_LO_OFF` | 0x60 | ena_regs_defs.h:58 |
| MMIO readless resp high | `ENA_REGS_MMIO_RESP_HI_OFF` | 0x64 | ena_regs_defs.h:59 |

Field masks/shifts needed in Phase 0 (same file):
- `ENA_REGS_DEV_STS_READY_MASK` 0x1 (ena_regs_defs.h:119) — gate before admin init/reset.
- `ENA_REGS_DEV_STS_RESET_IN_PROGRESS_MASK` 0x8 (ena_regs_defs.h:125) — reset handshake poll bit.
- `ENA_REGS_DEV_CTL_DEV_RESET_MASK` 0x1 (ena_regs_defs.h:106) — write to start reset.
- `ENA_REGS_DEV_CTL_RESET_REASON_SHIFT` 28 / `..._MASK` 0xf0000000 (ena_regs_defs.h:115–116).
- `ENA_REGS_CAPS_RESET_TIMEOUT_SHIFT` 1 / `..._MASK` 0x3e (ena_regs_defs.h:83–84) — reset timeout, units 100ms.
- `ENA_REGS_CAPS_DMA_ADDR_WIDTH_SHIFT` 8 / `..._MASK` 0xff00 (ena_regs_defs.h:85–86) — DMA addr width for `bus_dmamap` boundary.
- `ENA_REGS_CAPS_ADMIN_CMD_TO_SHIFT` 16 / `..._MASK` 0xf0000 (ena_regs_defs.h:87–88) — admin completion timeout.
- `ENA_REGS_AQ_CAPS_AQ_DEPTH_MASK` 0xffff / `..._ENTRY_SIZE_SHIFT` 16 (ena_regs_defs.h:91–93) — written when programming AQ caps.
- `ENA_REGS_ACQ_CAPS_*` (ena_regs_defs.h:96–98) — same for ACQ.
- `ENA_REGS_MMIO_REG_READ_REQ_ID_MASK` 0xffff / `..._REG_OFF_SHIFT` 16 (ena_regs_defs.h:136–138) — readless request encoding.
- `enum ena_regs_reset_reason_types` (ena_regs_defs.h:9) — `ENA_REGS_RESET_NORMAL` = 0 is the Phase 0 reset reason.

### 1b. Admin queue init / submit / poll / reset — `ena_com.c`

File: `reference/freebsd/sys/contrib/ena-com/ena_com.c`.

| Concept | ena-com symbol | file:line | Phase 0 role |
| --- | --- | --- | --- |
| Readless MMIO read (the only way to read BAR regs once enabled) | `ena_com_reg_bar_read32` (static) | ena_com.c:886 | reads via DMA resp buffer; falls back to raw `ENA_REG_READ32` if readless disabled (line 902) |
| Allocate readless resp DMA buffer | `ena_com_mmio_reg_read_request_init` | ena_com.c:2050 | called FIRST in bring-up; allocates the coherent resp buffer |
| (Re)write resp buffer addr into BAR | `ena_com_mmio_reg_read_request_write_dev_addr` | ena_com.c:2100 | must be re-issued after each reset (called at ena_com.c:2621) |
| Select readless vs raw read | `ena_com_set_mmio_read_mode` | ena_com.c:2076 | driven by PCI revision bit |
| Device reset handshake | `ena_com_dev_reset` | ena_com.c:2566 | writes DEV_CTL reset bit, polls DEV_STS reset-in-progress on/off, recomputes admin timeout |
| Admin queue init (program AQ/ACQ/AENQ base+caps into BAR) | `ena_com_admin_init` | ena_com.c:2112 | gates on `DEV_STS_READY`, sets depth `ENA_ADMIN_QUEUE_DEPTH`, writes AQ/ACQ base lo/hi + caps regs |
| Build & enqueue an admin cmd | `__ena_com_submit_admin_cmd` (static) | ena_com.c:255 | low-level; sets command_id+phase, copies into SQ entry |
| Submit wrapper | `ena_com_submit_admin_cmd` (static) | ena_com.c:337 | acquires comp_ctx |
| Wait for completion (dispatch) | `ena_com_wait_and_process_admin_cq` (static) | ena_com.c:952 | chooses polling vs interrupt path |
| ... polling variant | `ena_com_wait_and_process_admin_cq_polling` (static) | ena_com.c:598 | **Phase 0 uses polling** |
| ... interrupt variant | `ena_com_wait_and_process_admin_cq_interrupts` (static) | ena_com.c:821 | post-Phase-0 |
| Submit + wait (the one-call API) | `ena_com_execute_admin_command` | ena_com.c:1424 | every GET_FEATURE goes through this |
| Get device attributes/features | `ena_com_get_dev_attr_feat` | ena_com.c:2334 | issues GET_FEATURE for DEVICE_ATTRIBUTES, MAX_QUEUES(_EXT), AENQ_CONFIG, STATELESS_OFFLOAD, HW_HINTS, LLQ |
| Single GET_FEATURE issue | `ena_com_get_feature` (static, called from get_dev_attr_feat) | ena_com.c:~2300 (calls execute at 2300) | wraps execute_admin_command with a get_feat_cmd |

**Canonical bring-up ordering** (from NetBSD `ena_device_init`,
`reference/netbsd/sys/dev/pci/if_ena.c:3234`) — this is the exact sequence Task 4+
must reproduce in OpenBSD idiom:

1. `ena_com_mmio_reg_read_request_init` (if_ena.c:3244) — alloc readless resp DMA buf.
2. `ena_com_set_mmio_read_mode` (if_ena.c:3256) — from PCI revision bit.
3. `ena_com_dev_reset(ENA_REGS_RESET_NORMAL)` (if_ena.c:3258).
4. `ena_com_validate_version` (if_ena.c:3264).
5. `ena_com_get_dma_width` (if_ena.c:3270) — feeds `bus_dma` addr-width constraint.
6. `ena_com_admin_init` (if_ena.c:3279) — programs AQ/ACQ/AENQ into BAR.
7. `ena_com_set_admin_polling_mode(true)` (if_ena.c:3291) — **Phase 0 stays here**.
8. `ena_com_get_dev_attr_feat` (if_ena.c:3296).
9. (later) `ena_com_set_admin_polling_mode(false)` + `ena_com_admin_aenq_enable`
   (if_ena.c:3343–3345) — only after MSI-X is up; **out of Phase 0 scope**.

### 1c. Admin descriptor + completion structs + opcodes — `ena_admin_defs.h`

File: `reference/freebsd/sys/contrib/ena-com/ena_defs/ena_admin_defs.h`
(identical in amzn-drivers `common/ena_com/ena_admin_defs.h`).

| Concept | symbol | file:line | notes |
| --- | --- | --- | --- |
| Admin opcodes enum | `enum ena_admin_aq_opcode` | ena_admin_defs.h:57 | `ENA_ADMIN_GET_FEATURE` = 8 (line 62), `ENA_ADMIN_SET_FEATURE` = 9 (line 63) |
| Completion status enum | `enum ena_admin_aq_completion_status` | ena_admin_defs.h:67 | `ENA_ADMIN_SUCCESS` = 0 (line 68) |
| Feature-id enum | `enum ena_admin_aq_feature_id` | ena_admin_defs.h:80 | `ENA_ADMIN_DEVICE_ATTRIBUTES` = 1 (line 81); MAX_QUEUES_NUM=2, MAX_QUEUES_EXT=7, AENQ_CONFIG=26, LLQ=4 |
| AQ common descriptor (command_id/opcode/flags+phase) | `struct ena_admin_aq_common_desc` | ena_admin_defs.h:196 | 4 bytes: u16 command_id, u8 opcode, u8 flags(bit0=phase) |
| AQ entry (the 64-byte SQ slot) | `struct ena_admin_aq_entry` | ena_admin_defs.h:236 | common_desc + union(inline_data_w1[3] / ctrl_buff) + inline_data_w4[12] |
| ACQ common descriptor | `struct ena_admin_acq_common_desc` | ena_admin_defs.h:248 | u16 command, u8 status, u8 flags(bit0=phase), u16 ext_status, u16 sq_head_indx |
| ACQ entry (the CQ slot) | `struct ena_admin_acq_entry` | ena_admin_defs.h:270 | common_desc + response_specific_data[14] |
| ctrl buffer info (control-data DMA ptr) | `struct ena_admin_ctrl_buff_info` | ena_admin_defs.h:219 | u32 length + `ena_common_mem_addr` |
| 48-bit DMA address pair | `struct ena_common_mem_addr` | ena_common_defs.h:41 | u32 mem_addr_low, u16 mem_addr_high, u16 reserved |
| get/set feature common desc | `struct ena_admin_get_set_feature_common_desc` | ena_admin_defs.h:566 | u8 flags(select), u8 feature_id, u8 feature_version |
| GET_FEATURE command | `struct ena_admin_get_feat_cmd` | ena_admin_defs.h:1077 | common_desc + ctrl_buff + feat_common + raw[11] |
| GET_FEATURE response | `struct ena_admin_get_feat_resp` | ena_admin_defs.h:1137 | acq_common_desc + union of feature descs |
| Device attributes desc (in resp union) | `struct ena_admin_device_attr_feature_desc` | ena_admin_defs.h:585 | impl_id, device_version, supported_features bitmap, capabilities, MAC, MTU |

Phase 0 reads only the GET_FEATURE/DEVICE_ATTRIBUTES path. CREATE_SQ/CREATE_CQ
opcodes (ena_admin_defs.h:58–61) and their structs are out of scope until IO queues.

---

## 2. OpenBSD glue surface — how OpenBSD spells each ena-com primitive

ena-com's platform layer is `ena_plat.h`
(`reference/freebsd/sys/contrib/ena-com/ena_plat.h`). We **replace it** with an
OpenBSD shim. Below, each ena-com macro → the FreeBSD bus_* it expands to → the
OpenBSD idiom + a verified citation into `if_mcx.c` / `if_vio.c` / `virtio_pci.c`
(fetched from openbsd/src master; copies at `/tmp/if_mcx.c`, `/tmp/if_vio.c`,
`/tmp/virtio_pci.c`).

### 2a. BAR0 register mapping

| ena-com | OpenBSD spelling | citation |
| --- | --- | --- |
| (glue maps reg bar into `ena_bus.reg_bar_t/h`) | `pci_mapreg_map(pa, BAR, memtype, flags, &iot, &ioh, ...)` | `if_mcx.c:2757` (mcx_attach, maps `MCX_HCA_BAR`); `virtio_pci.c:489` / `:563` |

ENA BAR0 is the register BAR; there is also a `mem_bar` (LLQ) we ignore in Phase 0.

### 2b. MMIO register read / write

| ena-com macro (`ena_plat.h`) | FreeBSD expansion | OpenBSD idiom | citation |
| --- | --- | --- | --- |
| `ENA_REG_WRITE32(bus,val,off)` (ena_plat.h:403) | `wmb(); bus_space_write_4(reg_bar_t, reg_bar_h, off, val)` (ena_plat.h:409) | `bus_space_write_4(sc->sc_memt, sc->sc_memh, r, v)` | `if_mcx.c:8264` (mcx_wr); virtio cfg writes `virtio_pci.c:946–962` |
| `ENA_REG_READ32(bus,off)` (ena_plat.h:415) | `bus_space_read_4(...); rmb()` (ena_plat.h:329–335) | `bus_space_read_4(sc->sc_memt, sc->sc_memh, r)` | `if_mcx.c:8254` (mcx_rd); virtio cfg reads `virtio_pci.c:911–925` |

mcx uses the `_raw_4`/byte-swap variants because its registers are big-endian; ENA
registers are little-endian, so use plain `bus_space_{read,write}_4`. The
`wmb()`/`rmb()` barriers in ena-com map to OpenBSD `membar_*` or rely on
`bus_space_barrier` — keep the ordering (barrier before doorbell write).

### 2c. DMA coherent allocation (admin SQ/CQ, AENQ, readless resp buffer)

ena-com `ENA_MEM_ALLOC_COHERENT*` (ena_plat.h:364–390) → FreeBSD `ena_dma_alloc`
(bus_dma_tag_create + bus_dmamem_alloc + bus_dmamap_load). The handle type is
`ena_mem_handle_t { paddr, vaddr, tag, map, seg, nseg }` (ena_plat.h:300).

OpenBSD idiom is the 4-call helper used by every modern driver:

| step | OpenBSD call | if_mcx citation | if_vio citation |
| --- | --- | --- | --- |
| 1. create map | `bus_dmamap_create(sc->sc_dmat, size, 1, size, 0, BUS_DMA_*, &map)` | `if_mcx.c:8301` | `if_vio.c:405` |
| 2. alloc phys | `bus_dmamem_alloc(sc->sc_dmat, size, align, 0, &seg, 1, &nseg, BUS_DMA_*)` | `if_mcx.c:8306` | `if_vio.c:410` |
| 3. map to kva | `bus_dmamem_map(sc->sc_dmat, &seg, nseg, size, &kva, BUS_DMA_*)` | `if_mcx.c:8310` | `if_vio.c:414` |
| 4. load (get DMA addr) | `bus_dmamap_load(sc->sc_dmat, map, kva, size, NULL, BUS_DMA_*)` | `if_mcx.c:8313` | `if_vio.c:417` |

Wrap this as a single `ena_dmamem`-style struct (mirror `mcx_dmamem` at
`if_mcx.c:8296`). The bus DMA address comes from `map->dm_segs[0].ds_addr` and is
split into low/high for the `ENA_REGS_*_BASE_LO/HI` writes and for
`ena_common_mem_addr`. NetBSD's equivalent allocator: `if_ena.c:276`
(`bus_dmamem_alloc`) / `if_ena.c:293` (`bus_dmamap_load`).

### 2d. DMA sync placement (the doorbell/completion ordering)

ena-com hides this in `ENA_DB_SYNC_*` (ena_plat.h:418–422) and inside its submit/poll
helpers. OpenBSD does it explicitly:

| when | OpenBSD | if_mcx citation | if_vio citation |
| --- | --- | --- | --- |
| before ringing SQ doorbell (descriptor written) | `bus_dmamap_sync(..., PREWRITE)` then write doorbell | `if_mcx.c:3286` → doorbell write `:3289` | `if_vio.c:1281` |
| before device fills a buffer | `bus_dmamap_sync(..., PREREAD)` | — | `if_vio.c:1505` |
| after reading a completion | `bus_dmamap_sync(..., POSTREAD)` then read phase/status | `if_mcx.c:3148` → status read `:3151` | `if_vio.c:1589` |

Rule for the admin CQ poll loop: `POSTREAD`-sync **before** each phase-bit check, then
re-check. Mirror mcx's `mcx_cmdq_poll` (`if_mcx.c:3142`) ownership-bit loop —
structurally identical to ena-com's `ena_com_wait_and_process_admin_cq_polling`
(ena_com.c:598).

### 2e. Interrupt establish

| ena-com / glue need | OpenBSD idiom | citation |
| --- | --- | --- |
| map admin (mgmt) MSI-X vector | `pci_intr_map_msix(pa, vec, &ih)` | `if_mcx.c:2895` (vector 0 = admin); `virtio_pci.c:990` |
| establish handler | `pci_intr_establish(sc_pc, ih, IPL_NET|IPL_MPSAFE, handler, arg, name)` | `if_mcx.c:2900`; INTx fallback `virtio_pci.c:730` |
| per-queue MSI-X (post-Phase-0) | `pci_intr_map_msix(pa, vec, &ih)` + `pci_intr_establish_cpu(...)` | `if_mcx.c:3014`/`:3021`; `virtio_pci.c:1000` |
| INTx fallback path | `pci_intr_map_msi()` else `pci_intr_map()` | `virtio_pci.c:716` |

NetBSD ena glue equivalent: `pci_intr_alloc` (`if_ena.c:1937`), count via
`pci_msix_count` (`if_ena.c:3099`), establish via `pci_intr_establish_xname`
(`if_ena.c:2012`, `:2060`).

**Phase 0 needs NO interrupt at all** — admin is polled
(`ena_com_set_admin_polling_mode(true)`). The MSI-X establish path is Phase-1+, but
is mapped here because the arm64 caveat (§4) determines whether it will ever work.

### 2f. Softc / attach skeleton

| concept | OpenBSD model | citation |
| --- | --- | --- |
| softc holding `pci_chipset_tag_t`, `bus_dma_tag_t`, `bus_space_tag_t/handle` | `struct mcx_softc` (sc_pc:2450, sc_dmat:2455, sc_memt:2456, sc_memh:2457) | `if_mcx.c:2443` |
| match by PCI id | `pci_matchbyid(aux, table, nitems)` | `if_mcx.c:2733` (mcx_match) |
| attach: pull `pa->pa_pc`, `pa->pa_dmat`, then map BAR | `mcx_attach` | `if_mcx.c:2739` (sc_pc=`:2751`, sc_dmat=`:2753`, map BAR=`:2757`) |
| NetBSD ena attach (full ena-com driving) | `ena_attach` | `if_ena.c:3678`; bring-up `ena_device_init` `if_ena.c:3234` |

ENA's vendor/device ids come from `ena_pci_id_tbl.h` (Linux glue) — transcribe in a
later task; not a Phase 0 protocol target.

---

## 3. Decision notes — re-implement vs. transcribe-as-constants

We do **not** port ena-com verbatim. We transcribe the *protocol* (offsets, struct
layouts, opcodes — they are the device ABI and must match bit-for-bit) and
**re-implement** the *mechanism* (DMA, MMIO, sync, poll) in OpenBSD idiom.

### Transcribe as constants / layouts (copy exactly, do NOT paraphrase)

| ena-com source | what | OpenBSD home |
| --- | --- | --- |
| `ena_regs_defs.h` (all `ENA_REGS_*_OFF` + masks/shifts) | BAR0 register map | `#define`s in `if_ena.c` / `ena_hw.h` |
| `ena_admin_defs.h` structs (aq_entry, acq_entry, common_desc, get_feat_cmd/resp, device_attr_feature_desc) | admin ABI structs | `struct` defs in an `ena_admin.h` — keep field order/sizes exact, use `__packed`, `uint{8,16,32}_t` |
| `ena_admin_defs.h` enums (aq_opcode, feature_id, completion_status, reset_reason) | opcode/id constants | enums/`#define`s |
| `ena_common_defs.h` `ena_common_mem_addr` | 48-bit addr pair | `struct`, `__packed` |

The bitfield-accessor inlines in `ena_admin_defs.h` (e.g.
`set_ena_admin_aq_common_desc_phase`, line 1448) are FreeBSD-style; re-implement as
small OpenBSD static inlines or open-coded mask/shift — they are not ABI, just
helpers over the ABI byte `flags`.

### Re-implement in OpenBSD idiom (logic ported, calls swapped)

| ena-com function | OpenBSD re-implementation | exact OpenBSD types/calls |
| --- | --- | --- |
| `ENA_MEM_ALLOC_COHERENT` / `ena_dma_alloc` | `ena_dmamem_alloc()` helper | `bus_dma_tag_t`, `bus_dmamap_t`, `bus_dma_segment_t`; the 4-call sequence (§2c) |
| `ENA_REG_WRITE32` / `ENA_REG_READ32` | `ena_reg_write`/`ena_reg_read` inlines | `bus_space_tag_t`/`bus_space_handle_t` + `bus_space_{write,read}_4`, barrier before doorbell |
| `ena_com_reg_bar_read32` (readless via DMA resp) | port the seq-num/timeout loop as-is | OpenBSD `delay()`/`bus_dmamap_sync(POSTREAD)`; resp buf from `ena_dmamem_alloc` |
| `ena_com_dev_reset` | port logic verbatim (it's just reg writes + polls) | uses our reg read/write + `delay()` |
| `ena_com_admin_init` | port logic | uses `ena_dmamem_alloc` for SQ/CQ/AENQ + reg writes |
| `ena_com_execute_admin_command` + poll variant | port logic | poll the ACQ phase bit with `bus_dmamap_sync(POSTREAD)`; mirror `mcx_cmdq_poll` (`if_mcx.c:3142`) |
| `ena_com_get_dev_attr_feat` / `ena_com_get_feature` | port logic | build `ena_admin_get_feat_cmd`, call our execute |
| interrupt establish (Phase 1+) | OpenBSD-native, NOT ported | `pci_intr_map_msix` + `pci_intr_establish` (§2e) |

Rationale: ena-com's spinlocks, atomics, `ENA_MIGHT_SLEEP`, and comp_ctx machinery
assume the Linux/FreeBSD plat shim. For the **polled, single-threaded Phase 0
bring-up**, that machinery collapses to: alloc a couple of DMA buffers, write base
addrs into BAR, push one descriptor, spin on a phase bit. Porting the *logic* of the
four reset/init/execute/get_feat functions while swapping the plat calls is far less
bug-prone than dragging in ena-com's locking layer.

---

## 4. arm64 MSI-X / GIC-ITS caveat — impact on the establish path

From `~/projects/aws-arm-bsd/QEMU-OPENBSD.md`: on OpenBSD/arm64 under QEMU `-machine
virt` on a1/Graviton1 KVM, virtio-pci's default **MSI-X via the GIC ITS is not
delivered** — the guest hangs right after device probe. The build harness forces
`vectors=0` (legacy INTx) to boot.

What this means for ena(4):

1. **Phase 0 sidesteps the problem entirely** — admin queue is **polled**
   (`ena_com_set_admin_polling_mode(true)`, NetBSD `if_ena.c:3291`). No
   `pci_intr_establish` is on the Phase 0 critical path. Device attributes can be
   read with zero interrupts. **Do not let an interrupt-setup failure block the
   Phase 0 milestone.**

2. **The hardware ENA has NO INTx fallback.** Unlike virtio (which can run on a pin),
   real ENA on Graviton requires working MSI-X — it has no legacy interrupt line.
   So the `vectors=0` escape hatch that makes virtio boot does **not** exist for ENA.
   On real EC2 Graviton metal the platform is `acpipci(4)` + `agintc(4)` with
   `agintcmsi` providing the GIC-ITS MSI-X frame; that path must actually deliver for
   IO queues to ever work. This is the central Phase-1 risk and is the whole reason
   the project exists.

3. **Implication for the establish path (Phase 1+):** mirror the mcx multi-vector
   MSI-X idiom (`pci_intr_map_msix` at `if_mcx.c:2895`/`:3014`,
   `pci_intr_establish[_cpu]` at `:2900`/`:3021`), NOT the virtio INTx fallback.
   Vector 0 = admin/AENQ, vectors 1..N = IO queues, matching NetBSD's layout
   (`pci_msix_count` `if_ena.c:3099`, `pci_intr_establish_xname` `if_ena.c:2012`).
   Whether GIC-ITS actually delivers these on our target is an open hardware question
   to validate empirically once IO queues exist — it is the make-or-break test, and
   the QEMU/a1 environment (which fails MSI-X) is **not** a faithful proxy for it.

---

## Quick index of transcription targets for Tasks 4–8

- Registers: `reference/amzn-drivers/kernel/linux/common/ena_com/ena_regs_defs.h`
  (offsets at lines 37–60; masks 68–146).
- Admin structs/enums: `reference/freebsd/sys/contrib/ena-com/ena_defs/ena_admin_defs.h`
  (opcodes:57, feature_id:80, aq_common_desc:196, aq_entry:236, acq_common_desc:248,
  acq_entry:270, get_feat_cmd:1077, get_feat_resp:1137, device_attr:585) +
  `ena_common_defs.h:41`.
- Bring-up logic to port: `reference/freebsd/sys/contrib/ena-com/ena_com.c`
  (reg_bar_read32:886, mmio_read_request_init:2050, dev_reset:2566, admin_init:2112,
  execute_admin_command:1424, wait_polling:598, get_dev_attr_feat:2334).
- Plat shim being replaced (shows the bus_* mapping):
  `reference/freebsd/sys/contrib/ena-com/ena_plat.h` (DMA:300/364, reg rw:403/415).
- OpenBSD idiom models: `/tmp/if_mcx.c` (dmamem_alloc:8296, rd/wr:8254/8264,
  cmdq poll:3142, msix:2895, softc:2443, attach:2739) and `/tmp/if_vio.c` +
  `/tmp/virtio_pci.c` (dmamem:400, mapreg:489, msix:990, INTx fallback:716/730).
- NetBSD glue ordering: `reference/netbsd/sys/dev/pci/if_ena.c`
  (ena_device_init:3234, ena_attach:3678).
