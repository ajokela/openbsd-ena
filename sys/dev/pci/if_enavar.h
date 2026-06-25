/*	$OpenBSD$	*/

/*-
 * Softc and register-accessor definitions for the Amazon Elastic Network
 * Adapter (ENA) driver.
 *
 * Phase 0 skeleton: PCI attach/detach + BAR0 register window only. The
 * admin queue, DMA helpers, and MSI-X state are added by later tasks; the
 * softc field set and accessor macros here are the stable interface those
 * tasks consume (see docs/ena-idiom-map.md).
 *
 * softc layout mirrors struct mcx_softc (if_mcx.c:2443): pci_chipset_tag_t,
 * pcitag_t, the bus_space tag/handle/size triple, and bus_dma_tag_t.
 * Task 5 adds struct arpcom sc_ac (ethernet softc wrapper) and struct
 * ifmedia sc_media, mirroring mcx_softc (if_mcx.c:2445-2446).
 */

#ifndef _DEV_PCI_IF_ENAVAR_H_
#define _DEV_PCI_IF_ENAVAR_H_

#include <sys/timeout.h>		/* struct timeout (RX refill) */
#include <sys/task.h>			/* struct task (reset/recovery task) */
#include <sys/mutex.h>			/* struct mutex (AENQ drain serialization) */
#include <net/if.h>			/* struct if_rxring */

/*
 * Single-segment, physically contiguous DMA buffer. Mirrors struct
 * mcx_dmamem (if_mcx.c:2251) and its MCX_DMA_* accessors. Allocated with
 * ena_dmamem_alloc(); ENA_DMA_DVA() yields the device (bus) address to
 * program into the ENA_REGS_*_BASE_LO/HI registers and ena_common_mem_addr
 * fields, ENA_DMA_KVA() the CPU mapping. edm_map drives bus_dmamap_sync().
 */
struct ena_dma {
	bus_dmamap_t		 edm_map;
	bus_dma_segment_t	 edm_seg;
	caddr_t			 edm_kva;
	bus_size_t		 edm_size;
};
#define ENA_DMA_MAP(_d)	((_d)->edm_map)
#define ENA_DMA_DVA(_d)	((_d)->edm_map->dm_segs[0].ds_addr)
#define ENA_DMA_KVA(_d)	((void *)(_d)->edm_kva)
#define ENA_DMA_LEN(_d)	((_d)->edm_size)

/*
 * Admin queue geometry, transcribed from ena-com:
 *   ENA_ADMIN_QUEUE_DEPTH		32 entries
 *	(reference/amzn-drivers/.../ena_com/ena_com.c:15)
 *   sizeof(struct ena_admin_aq_entry)	64 bytes  (SQ slot)
 *	(reference/amzn-drivers/.../ena_com/ena_admin_defs.h:236:
 *	 4-byte common_desc + 12-byte union + 48-byte inline_data_w4[12])
 *   sizeof(struct ena_admin_acq_entry)	64 bytes  (CQ slot)
 *	(reference/amzn-drivers/.../ena_com/ena_admin_defs.h:270:
 *	 8-byte common_desc + 56-byte response_specific_data[14])
 *
 * The actual ena_admin_* structs are transcribed in a later task; the byte
 * counts below are pinned here so the ring allocations in Task 5 are sized
 * exactly. ENA_ADMIN_SQ_SIZE() / _CQ_SIZE() must equal those struct sizes
 * once they land (cross-checked then).
 */
#define ENA_ADMIN_QUEUE_DEPTH		32
/*
 * Default admin-command completion timeout, used when the CAPS register's
 * ADMIN_CMD_TO field reads back zero. ena-com ADMIN_CMD_TIMEOUT_US, 3s
 * (reference/freebsd/sys/contrib/ena-com/ena_com.c:40).
 */
#define ENA_ADMIN_CMD_TIMEOUT_US	3000000
#define ENA_ADMIN_AQ_ENTRY_SIZE		64	/* ena_admin_aq_entry  */
#define ENA_ADMIN_ACQ_ENTRY_SIZE	64	/* ena_admin_acq_entry */
#define ENA_ADMIN_SQ_SIZE(depth)	((depth) * ENA_ADMIN_AQ_ENTRY_SIZE)
#define ENA_ADMIN_CQ_SIZE(depth)	((depth) * ENA_ADMIN_ACQ_ENTRY_SIZE)

/*
 * AENQ geometry, transcribed from ena-com:
 *   ENA_ASYNC_QUEUE_DEPTH		16 entries
 *	(reference/amzn-drivers/.../ena_com/ena_com.c:14)
 *   sizeof(struct ena_admin_aenq_entry)	64 bytes (16-byte common_desc +
 *	48-byte inline_data_w4[12])
 *	(reference/amzn-drivers/.../ena_com/ena_admin_defs.h:1370)
 *   ADMIN_AENQ_SIZE(depth) == depth * sizeof(ena_admin_aenq_entry)
 *	(reference/amzn-drivers/.../ena_com/ena_com.h:45)
 * Depth is a power of two so the masked-head ring index in ena_aenq_intr can
 * use (depth - 1) as a wrap mask.
 */
#define ENA_AENQ_DEPTH			16
#define ENA_AENQ_ENTRY_SIZE		64	/* ena_admin_aenq_entry */
#define ENA_AENQ_SIZE(depth)		((depth) * ENA_AENQ_ENTRY_SIZE)

/*
 * INTR_MASK register values (ena_com.c:35, 1758-1763). Writing
 * ENA_REGS_ADMIN_INTR_MASK (bit 0) masks the management interrupt; writing 0
 * unmasks it. Phase 1 unmasks so the AENQ event interrupt can fire while the
 * admin completion queue itself stays polled.
 */
#define ENA_REGS_ADMIN_INTR_MASK	1

/*
 * IO RX queue geometry (Task 6). Phase 1 brings up a single RX queue.
 *
 *   ENA_RX_QUEUE_DEPTH		number of RX descriptors / completion slots.
 *	A power of two so the masked-head ring index uses (depth - 1) as a
 *	wrap mask and the phase bit flips exactly on wrap. ena-com sizes IO
 *	queues from the device's max queue feature; 1024 is the common default
 *	(reference/freebsd/sys/dev/ena/ena.h ENA_DEFAULT_RING_SIZE) and well
 *	within every ENA device's advertised maximum.
 *
 *   sizeof(struct ena_eth_io_rx_desc)		16 bytes (RX SQ slot)
 *	(reference/amzn-drivers/.../ena_com/ena_eth_io_defs.h:172)
 *   sizeof(struct ena_eth_io_rx_cdesc_base)	16 bytes (RX CQ slot)
 *	(reference/amzn-drivers/.../ena_com/ena_eth_io_defs.h:206)
 *	The RX CQ uses the 4-word base cdesc (non-extended); ena_com selects
 *	this size in ena_com_io_cq_create (ena_com.c:406-414) when
 *	use_extended_cdesc is false, which is the Phase 1 case.
 *
 * The RX data buffer size is the cluster the device DMAs each frame into. We
 * use a 2KB cluster (MCLBYTES); the device's length field caps the frame to
 * the buffer length we program per descriptor, and Phase 1 keeps the MTU at
 * the 1500-byte default so a single 2KB buffer holds any frame (single-desc
 * RX, first+last set on one cdesc).
 */
#define ENA_RX_QUEUE_DEPTH		1024
#define ENA_RX_DESC_SIZE		16	/* ena_eth_io_rx_desc */
#define ENA_RX_CDESC_SIZE		16	/* ena_eth_io_rx_cdesc_base */
#define ENA_RX_SQ_SIZE(depth)		((depth) * ENA_RX_DESC_SIZE)
#define ENA_RX_CQ_SIZE(depth)		((depth) * ENA_RX_CDESC_SIZE)
#define ENA_RX_BUF_SIZE			MCLBYTES /* per-descriptor DMA buffer */

/*
 * IO TX queue geometry (Task 7). Phase 1 brings up a single TX queue.
 *
 *   ENA_TX_QUEUE_DEPTH		number of TX submission descriptors / completion
 *	slots. Power of two so the masked index uses (depth - 1) as a wrap mask
 *	and the phase bit flips exactly on wrap. Matches ENA_RX_QUEUE_DEPTH and
 *	the common ENA_DEFAULT_RING_SIZE (reference/freebsd/sys/dev/ena/ena.h).
 *
 *   sizeof(struct ena_eth_io_tx_desc)	16 bytes (TX submission descriptor)
 *	(reference/amzn-drivers/.../ena_com/ena_eth_io_defs.h:24)
 *   sizeof(struct ena_eth_io_tx_cdesc)	8 bytes (TX completion descriptor)
 *	(reference/amzn-drivers/.../ena_com/ena_eth_io_defs.h:146)
 *	ena_com selects the 8-byte (non-extended) TX cdesc in ena_com_create_io_cq
 *	(ena_com.c:406-414) when use_extended_cdesc is false (the Phase 1 case);
 *	the CQ entry size in words programmed into CREATE_CQ is 8/4 == 2.
 *
 * LLQ entry geometry. We negotiate the vendor-default placement:
 *   header location	INLINE_HEADER (header lives in the descriptor list entry)
 *   stride control	MULTIPLE_DESCS_PER_ENTRY
 *   descs before hdr	2  (the first entry holds up to 2 descriptors, then the
 *			    packet header, then the rest of the entry)
 *   entry size		128 bytes
 * giving tx_max_header_size = 128 - (2 * 16) = 96 bytes of inline header room
 * and descs_per_entry = 128 / 16 = 8. ena_com derives these in
 * ena_com_config_llq_info (ena_com.c:644-772) / ena_com_config_dev_mode
 * (ena_com.c:3488-3515). We pin the values here and verify the device supports
 * each chosen bit before committing (see ena_tx_llq_negotiate).
 *
 * Phase-1 single-entry restriction: with descs_num_before_header == 2 the first
 * LLQ entry holds up to 2 data descriptors plus the inline header. We m_defrag
 * any packet down to at most ENA_TX_MAX_SEGS (2) data segments so every packet
 * occupies EXACTLY ONE 128-byte LLQ entry. This keeps the LLQ window write a
 * single contiguous bus_space_write_region_4 per packet and sidesteps the
 * multi-entry bounce-buffer spanning logic in ena_com (ena_eth_com.c:234-259),
 * which is the highest-risk part of the push path. Documented trade-off: a
 * packet that cannot be coalesced to <= 2 segments after m_defrag is dropped.
 */
#define ENA_TX_QUEUE_DEPTH		1024
#define ENA_TX_DESC_SIZE		16	/* ena_eth_io_tx_desc */
#define ENA_TX_CDESC_SIZE		8	/* ena_eth_io_tx_cdesc */
#define ENA_TX_CQ_SIZE(depth)		((depth) * ENA_TX_CDESC_SIZE)

#define ENA_LLQ_ENTRY_SIZE		128	/* bytes per LLQ list entry */
#define ENA_LLQ_DESCS_BEFORE_HEADER	2	/* descs preceding inline header */
#define ENA_LLQ_HEADER_OFFSET		(ENA_LLQ_DESCS_BEFORE_HEADER * \
					    ENA_TX_DESC_SIZE)		/* 32 */
#define ENA_TX_MAX_HEADER_SIZE		(ENA_LLQ_ENTRY_SIZE - \
					    ENA_LLQ_HEADER_OFFSET)	/* 96 */
#define ENA_TX_MAX_SEGS			2	/* data segments per packet */

/*
 * Host-memory (non-LLQ) TX SQ ring size, used only on the documented fallback
 * branch where the device does not support LLQ. ena_com strides this ring by
 * sizeof(ena_eth_io_tx_desc).
 */
#define ENA_TX_SQ_SIZE(depth)		((depth) * ENA_TX_DESC_SIZE)

/*
 * One RX slot: the mbuf cluster handed to the device and its DMA map. req_id
 * (the index into rxq_slots) is echoed back in each RX completion's req_id, so
 * the completion handler recovers the slot directly.
 */
struct ena_rx_slot {
	bus_dmamap_t		 rxs_map;	/* per-buffer DMA map */
	struct mbuf		*rxs_m;		/* cluster mbuf, NULL if free */
};

/*
 * IO RX queue (Task 6). The RX SQ (rxq_sq_dma) is a host->device ring of
 * ena_eth_io_rx_desc that point the device at free buffers; the RX CQ
 * (rxq_cq_dma) is a device->host ring of ena_eth_io_rx_cdesc_base completions.
 * Mirrors struct mcx_rx (if_mcx.c:2300) in OpenBSD idiom.
 *
 *   rxq_sq_idx / rxq_cq_idx	device-returned queue indices (CREATE_SQ/CQ).
 *   rxq_sq_db			RX SQ doorbell BAR offset (CREATE_SQ resp).
 *   rxq_cq_db			RX CQ head doorbell BAR offset (CREATE_CQ resp).
 *   rxq_unmask_off		RX CQ interrupt-unmask BAR offset (CREATE_CQ resp);
 *				written to re-arm the IO MSI-X after it fires.
 *   rxq_sq_tail		monotonic SQ producer index; the value written
 *				to the SQ doorbell (ena-com io_sq->tail).
 *   rxq_sq_phase		producer phase bit stamped into each rx_desc.
 *   rxq_cq_head		monotonic CQ consumer index (masked to ring).
 *   rxq_cq_phase		consumer phase compared against each cdesc.
 *   rxq_rxr			if_rxr ring accounting (mbuf cluster budget).
 *   rxq_refill			timeout that retries fill when clusters are
 *				temporarily unavailable.
 */
struct ena_softc;
struct intrmap;
struct ifqueue;
struct ifiqueue;

struct ena_rxq {
	struct ena_softc	*rxq_sc;	/* backptr (refill callout / ISR) */
	unsigned int		 rxq_id;	/* queue index [0, sc_nqueues) */
	struct ifiqueue		*rxq_ifiq;	/* stack RX input queue (delivery) */
	int			 rxq_created;	/* CREATE_CQ/SQ done (teardown gate) */
	struct ena_dma		 rxq_sq_dma;	/* RX SQ ring (host->device) */
	struct ena_dma		 rxq_cq_dma;	/* RX CQ ring (device->host) */
	struct ena_rx_slot	*rxq_slots;	/* depth ena_rx_slot entries */
	uint16_t		 rxq_depth;
	uint16_t		 rxq_sq_idx;	/* device SQ index */
	uint16_t		 rxq_cq_idx;	/* device CQ index */
	bus_size_t		 rxq_sq_db;	/* SQ doorbell BAR offset */
	bus_size_t		 rxq_cq_db;	/* CQ head doorbell BAR offset */
	bus_size_t		 rxq_unmask_off; /* CQ intr unmask BAR offset */
	uint16_t		 rxq_sq_tail;	/* SQ producer index (doorbell) */
	uint8_t			 rxq_sq_phase;	/* SQ producer phase bit */
	uint16_t		 rxq_cq_head;	/* CQ consumer index */
	uint8_t			 rxq_cq_phase;	/* CQ consumer phase bit */
	struct if_rxring	 rxq_rxr;	/* RX ring accounting */
	struct timeout		 rxq_refill;	/* deferred refill */
};

/*
 * One TX slot: the mbuf in flight and its DMA map. The slot index is the
 * req_id stamped into the tx_desc; the device echoes it back in the TX
 * completion's req_id so ena_txeof recovers the slot and frees the mbuf.
 */
struct ena_tx_slot {
	bus_dmamap_t		 txs_map;	/* per-packet DMA map */
	struct mbuf		*txs_m;		/* packet mbuf, NULL if free */
	uint16_t		 txs_ndesc;	/* SQ descriptors this packet used
					 * (host placement; for txq_sq_avail) */
};

/*
 * IO TX queue (Task 7).
 *
 * In LLQ (push) mode there is NO host-side TX SQ ring: the host writes each
 * 128-byte LLQ entry (descriptors + inline header) directly into the device
 * LLQ memory window (txq_llq_t/txq_llq_h, mapped from BAR2 at the device-
 * reported llq_descriptors_offset). The TX CQ (txq_cq_dma) is a device->host
 * ring of ena_eth_io_tx_cdesc completions in host memory.
 *
 * In the documented host-memory fallback (device does not support LLQ) the SQ
 * descriptors live in a host DMA ring (txq_sq_dma) instead; txq_llq == 0.
 *
 *   txq_sq_idx / txq_cq_idx	device-returned queue indices (CREATE_SQ/CQ).
 *   txq_sq_db			TX SQ doorbell BAR offset (CREATE_SQ resp).
 *   txq_unmask_off		TX CQ interrupt-unmask BAR offset (CREATE_CQ resp).
 *   txq_llq			1 if LLQ (push) placement, 0 if host-memory.
 *   txq_llq_off		llq_descriptors_offset within BAR2 (CREATE_SQ resp).
 *   txq_llq_t / txq_llq_h	bus_space tag/handle for the BAR2 LLQ window.
 *   txq_sq_tail		monotonic SQ producer index; written to the SQ
 *				doorbell and used (masked) as the LLQ entry index.
 *   txq_sq_phase		producer phase bit stamped into each tx_desc.
 *   txq_cq_head		monotonic CQ consumer index (masked to ring).
 *   txq_cq_phase		consumer phase compared against each cdesc.
 *   txq_prod / txq_cons	host slot ring producer/consumer (free-slot count).
 */
struct ena_txq {
	struct ena_softc	*txq_sc;	/* backptr */
	unsigned int		 txq_id;	/* queue index [0, sc_nqueues) */
	struct ifqueue		*txq_ifq;	/* stack TX output queue */
	int			 txq_created;	/* CREATE_CQ/SQ done (teardown gate) */
	struct ena_dma		 txq_sq_dma;	/* host-mem TX SQ ring (fallback) */
	struct ena_dma		 txq_cq_dma;	/* TX CQ ring (device->host) */
	struct ena_tx_slot	*txq_slots;	/* depth ena_tx_slot entries */
	uint16_t		 txq_depth;
	uint16_t		 txq_sq_idx;	/* device SQ index */
	uint16_t		 txq_cq_idx;	/* device CQ index */
	bus_size_t		 txq_sq_db;	/* SQ doorbell BAR offset */
	bus_size_t		 txq_unmask_off; /* CQ intr unmask BAR offset */
	int			 txq_llq;	/* LLQ placement in use */
	bus_size_t		 txq_llq_off;	/* LLQ desc offset in BAR2 */
	bus_space_tag_t		 txq_llq_t;	/* BAR2 LLQ window tag */
	bus_space_handle_t	 txq_llq_h;	/* BAR2 LLQ window handle */
	uint16_t		 txq_sq_tail;	/* SQ producer index (doorbell) */
	uint8_t			 txq_sq_phase;	/* SQ producer phase bit */
	uint16_t		 txq_cq_head;	/* CQ consumer index */
	uint8_t			 txq_cq_phase;	/* CQ consumer phase bit */
	uint16_t		 txq_prod;	/* host slot producer */
	uint16_t		 txq_cons;	/* host slot consumer */
	uint16_t		 txq_sq_avail;	/* free SQ descriptor slots
					 * (host placement; LLQ unused) */
};

/*
 * Per-IO-vector interrupt container (one per IO queue pair). Mirrors struct
 * igc_queue: the ISR arg carries the softc + queue index so it services
 * exactly its RX+TX pair; tag is the pci_intr_establish_cpu handle.
 */
struct ena_queue {
	struct ena_softc	*sc;
	unsigned int		 idx;		/* queue index (msix_vector - 1) */
	void			*tag;		/* pci_intr handle */
	char			 name[24];	/* "enaN:M" interrupt string */
};

/* Hard cap on IO queue pairs (clamped by device max, ncpu, msix table size). */
#define ENA_MAX_IO_QUEUES	8

struct ena_softc {
	struct device		 sc_dev;

	/*
	 * Ethernet interface and media state (Task 5). sc_ac.ac_if is the
	 * ifnet; sc_ac.ac_enaddr holds the MAC address copied from the
	 * GET_FEATURE(DEVICE_ATTRIBUTES) response in ena_get_dev_attr.
	 * sc_media is the ifmedia instance; it is init'd and torn down in
	 * ena_attach / ena_detach. Mirrors mcx_softc (if_mcx.c:2445-2446).
	 */
	struct arpcom		 sc_ac;		/* ethernet softc wrapper */
	struct ifmedia		 sc_media;	/* ifmedia for link state */

	/* PCI plumbing (from struct pci_attach_args at attach) */
	pci_chipset_tag_t	 sc_pc;
	pcitag_t		 sc_tag;

	/* BAR0 register window */
	bus_space_tag_t		 sc_memt;
	bus_space_handle_t	 sc_memh;
	bus_size_t		 sc_mems;

	/*
	 * BAR2 LLQ memory window (Task 7). Mapped in ena_attach only when the
	 * device supports LLQ (sc_tx_llq set by GET_FEATURE(LLQ)); the TX push
	 * path writes LLQ entries into this window at the per-SQ offset the
	 * CREATE_SQ response reports (llq_descriptors_offset).
	 */
	bus_space_tag_t		 sc_llq_t;
	bus_space_handle_t	 sc_llq_h;
	bus_size_t		 sc_llq_s;	/* 0 if BAR2 not mapped */

	/*
	 * Negotiated TX placement (Task 7), filled by ena_tx_llq_negotiate from
	 * GET_FEATURE(ENA_ADMIN_LLQ). sc_tx_llq == 1 selects device (LLQ/push)
	 * placement; 0 selects host-memory placement (documented fallback).
	 * The geometry fields are the values echoed back via SET_FEATURE(LLQ)
	 * and used to size/lay out each LLQ entry.
	 */
	int			 sc_tx_llq;		/* LLQ placement chosen */
	uint16_t		 sc_llq_entry_size;	/* bytes per LLQ entry */
	uint16_t		 sc_llq_descs_before_hdr; /* descs before header */
	uint16_t		 sc_tx_max_header;	/* max inline header bytes */

	/*
	 * Cached stateless-offload capabilities, filled by ena_get_dev_attr
	 * from GET_FEATURE(STATELESS_OFFLOAD_CONFIG). All implicitly 0 (no
	 * offload) when the softc is zeroed or the device lacks the feature.
	 */
	uint8_t			 sc_tx_csum_l3;	/* dev supports TX IPv4 header csum */
	uint8_t			 sc_tx_csum_l4;	/* dev supports TX L4 IPv4 csum (PART) */
	uint8_t			 sc_rx_csum_l3;	/* dev supports RX IPv4 header csum */
	uint8_t			 sc_rx_csum_l4;	/* dev supports RX L4 IPv4 csum */
	uint32_t		 sc_max_mtu;	/* device max L2-payload MTU (GET dev_attr) */
	uint32_t		 sc_rx_buf_size; /* per-desc RX cluster size, derived from if_mtu */
	uint32_t		 sc_supported_features; /* GET dev_attr feature bitmap (gates RSS SETs) */

	/* DMA tag for admin/IO queue allocations (Tasks 5-8) */
	bus_dma_tag_t		 sc_dmat;

	/*
	 * Admin queue DMA rings (Task 5). sc_aq_dma holds the submission
	 * queue (host writes descriptors, device reads them); sc_acq_dma
	 * holds the completion queue (device writes completions, host reads
	 * them). Both are ENA_ADMIN_QUEUE_DEPTH entries deep. sc_admin_depth
	 * is the active depth; sc_aq_phase / sc_acq_phase track the
	 * producer/consumer phase (toggle) bits used by the polled admin
	 * path in Tasks 7-8.
	 */
	struct ena_dma		 sc_aq_dma;	/* admin SQ ring */
	struct ena_dma		 sc_acq_dma;	/* admin CQ ring */
	struct ena_dma		 sc_mmio_resp_dma; /* readless MMIO resp region */
	struct ena_dma		 sc_host_attr_dma; /* 4KB host-info page (HOST_ATTR) */
	struct ena_dma		 sc_rss_key_dma;     /* 48B RSS Toeplitz key (ctrl buf) */
	struct ena_dma		 sc_rss_hashctrl_dma; /* 256B RSS per-proto fields (ctrl buf) */
	struct ena_dma		 sc_rss_indtbl_dma;  /* 512B RSS indirection table (ctrl buf) */
	int			 sc_rss_active;	/* RSS configured (multi-queue RX spread) */
	uint16_t		 sc_admin_depth;
	uint8_t			 sc_aq_phase;	/* SQ producer phase bit */
	uint8_t			 sc_acq_phase;	/* CQ consumer phase bit */
	uint16_t		 sc_aq_tail;	/* SQ producer index (host) */
	uint16_t		 sc_acq_head;	/* CQ consumer index (host) */
	uint16_t		 sc_admin_cmd_id; /* next command_id to issue */
	uint32_t		 sc_admin_cmd_to_us; /* admin completion TO */

	/*
	 * MSI-X interrupt handles (Task 3+).
	 *   sc_nvec:    number of vectors established (2 for Phase 1+).
	 *   sc_mgmt_ih: vector 0 — management (admin poll + AENQ, Task 4).
	 *   sc_io_ih:   vector 1 — IO (RX + TX completion, Tasks 6/7).
	 *
	 * ENA convention (ena_netdev.h:100-102):
	 *   ENA_MGMNT_IRQ_IDX    = 0  (admin/AENQ)
	 *   ENA_IO_IRQ_FIRST_IDX = 1  (first IO queue)
	 * The MSI-X vector index is passed to the device per-CQ via the
	 * CREATE_CQ admin command's msix_vector field (ena_com.c:1470); that
	 * wiring is Tasks 6/7.  No separate SET_FEATURE(MSIX) call exists.
	 */
	int			 sc_nvec;	/* vectors established (1 + sc_nqueues) */
	void			*sc_mgmt_ih;	/* management vector handle (vector 0) */
	struct intrmap		*sc_intrmap;	/* IO vector -> CPU distribution */
	unsigned int		 sc_nqueues;	/* active IO queue pairs (<= alloc) */
	unsigned int		 sc_nqueues_alloc; /* allocated/established count (free/disestablish by this) */
	struct ena_queue	*sc_queues;	/* sc_nqueues_alloc IO interrupt containers */
	unsigned int		 sc_max_io_queues; /* device max IO queue pairs (GET MAX_QUEUES_EXT) */
	unsigned int		 sc_max_rx_depth; /* device max RX SQ/CQ depth */
	unsigned int		 sc_max_tx_depth; /* device max TX SQ/CQ depth */

	/*
	 * Asynchronous Event Notification Queue (Task 4). sc_aenq_dma holds the
	 * device->host AENQ ring (the device DMA-writes events, the host reads
	 * them in ena_aenq_intr). sc_aenq_depth is the active depth;
	 * sc_aenq_head is the host consumer index (monotonic, masked to the ring
	 * by depth-1) which is also written to the AENQ head doorbell;
	 * sc_aenq_phase is the consumer phase (toggle) bit the host compares
	 * against each entry's PHASE flag. sc_link_up is the last link state the
	 * AENQ LINK_CHANGE handler observed, consumed by ifmedia in Task 5.
	 */
	struct ena_dma		 sc_aenq_dma;	/* AENQ ring (device->host) */
	uint16_t		 sc_aenq_depth;
	uint16_t		 sc_aenq_head;	/* consumer index + head doorbell */
	uint8_t			 sc_aenq_phase;	/* consumer phase bit */
	int			 sc_link_up;	/* last AENQ link state */

	/*
	 * Keep-alive watchdog (Increment 1: detection only). The softc is
	 * zeroed at attach, so 0-init of these is safe. sc_wd_tick is a 1 Hz
	 * softclock callout armed in ena_init and cancelled in ena_stop;
	 * ena_tick reads DEV_STS and the keep-alive timestamp, sets
	 * ENA_FLAG_TRIGGER_RESET on a fault, and re-arms. sc_keepalive_last is
	 * written in the mgmt-MSI-X ISR (single aligned volatile store, no lock)
	 * and read in the tick. sc_keepalive_timeo gates the keep-alive check
	 * (0 = disabled, set only when KEEP_ALIVE was actually subscribed).
	 * sc_aenq_groups is the AENQ group mask the device actually negotiated.
	 */
	struct timeout		 sc_wd_tick;	/* 1 Hz watchdog callout */
	struct mutex		 sc_aenq_mtx;	/* serialize AENQ drain: mgmt ISR vs tick */
	struct task		 sc_reset_task;	/* process-ctx device reset/recovery */
	volatile uint32_t	 sc_keepalive_last; /* getuptime() of last KEEP_ALIVE */
	uint32_t		 sc_keepalive_timeo; /* keep-alive staleness threshold secs; 0 disables */
	volatile unsigned int	 sc_flags;	/* ENA_FLAG_* (TRIGGER_RESET etc.) */
	int			 sc_wd_active;	/* watchdog tick master-enable */
	unsigned int		 sc_wd_fatal_count; /* consecutive bad DEV_STS reads (debounce) */
	uint32_t		 sc_wd_cooldown;   /* getuptime() until which the watchdog is suppressed (post-reset) */
	uint32_t		 sc_aenq_groups; /* AENQ groups negotiated (subscribed) */

	/*
	 * IO RX queue (Task 6). Phase 1 runs a single RX queue. Created in
	 * ena_init (CREATE_CQ then CREATE_SQ), torn down in ena_stop
	 * (DESTROY_SQ then DESTROY_CQ). sc_rx_created gates the teardown so a
	 * stop on a never-started interface is a no-op.
	 */
	struct ena_rxq		*sc_rxq;	/* sc_nqueues RX queues (mallocarray) */

	/*
	 * IO TX queue (Task 7). Phase 1 runs a single TX queue. Created in
	 * ena_init (CREATE_CQ then CREATE_SQ), torn down in ena_stop
	 * (DESTROY_SQ then DESTROY_CQ). sc_tx_created gates the teardown so a
	 * stop on a never-started interface is a no-op.
	 */
	struct ena_txq		*sc_txq;	/* sc_nqueues TX queues (mallocarray) */
};

int	ena_dmamem_alloc(struct ena_softc *, struct ena_dma *, bus_size_t,
	    bus_size_t);
void	ena_dmamem_free(struct ena_softc *, struct ena_dma *);

/*
 * BAR0 register accessors. ENA registers are little-endian, so plain
 * bus_space_{read,write}_4 is correct (no byte swap, unlike mcx's _raw_4
 * variants). Mirrors the ena-com ENA_REG_READ32/ENA_REG_WRITE32 plat macros
 * (ena_plat.h:403/415) re-expressed in OpenBSD idiom.
 */
#define ENA_REG_RD32(sc, o)						\
	bus_space_read_4((sc)->sc_memt, (sc)->sc_memh, (o))
#define ENA_REG_WR32(sc, o, v)						\
	bus_space_write_4((sc)->sc_memt, (sc)->sc_memh, (o), (v))

/*
 * Doorbell write: the descriptor DMA must be visible to the device before
 * the doorbell store reaches the BAR. The caller does the bus_dmamap_sync()
 * PREWRITE; this adds the post-store BARRIER_WRITE that ena-com's
 * ENA_REG_WRITE32 carries as a wmb() (ena_plat.h:403-409). Mirrors mcx's
 * doorbell write + mcx_bar(BUS_SPACE_BARRIER_WRITE) (if_mcx.c:3289-3290).
 */
#define ENA_REG_WR32_DB(sc, o, v)					\
	do {								\
		bus_space_write_4((sc)->sc_memt, (sc)->sc_memh, (o), (v)); \
		bus_space_barrier((sc)->sc_memt, (sc)->sc_memh, (o),	\
		    sizeof(uint32_t), BUS_SPACE_BARRIER_WRITE);		\
	} while (0)

/*
 * Pre-store barrier write: the barrier is issued BEFORE the store, mirroring
 * ena-com's ENA_REG_WRITE32 (ena_plat.h:403-407 = wmb() then write). Unlike
 * ENA_REG_WR32_DB (post-store), this orders all prior accesses -- in particular
 * the cacheable CQ-descriptor consume and its bus_dmamap_sync(POSTREAD) -- ahead
 * of this device-register store. Use it for a register write whose correctness
 * depends on preceding (cacheable) work being globally visible first and that
 * has no trailing device read to anchor ordering: the IO-CQ interrupt unmask
 * re-arm. NB: on arm64 bus_space_barrier(BARRIER_WRITE) is a dmb -- it ORDERS
 * accesses, it does not itself flush a posted MMIO write (only a read-back
 * does); ordering the unmask after the consume is exactly what ena-com's wmb()
 * provides here.
 */
#define ENA_REG_WR32_PREDB(sc, o, v)					\
	do {								\
		bus_space_barrier((sc)->sc_memt, (sc)->sc_memh, (o),	\
		    sizeof(uint32_t), BUS_SPACE_BARRIER_WRITE);		\
		bus_space_write_4((sc)->sc_memt, (sc)->sc_memh, (o), (v)); \
	} while (0)

int	ena_reset(struct ena_softc *);
int	ena_admin_init(struct ena_softc *);
int	ena_admin_poll(struct ena_softc *, struct ena_admin_aq_entry *,
	    struct ena_admin_acq_entry *);
int	ena_get_dev_attr(struct ena_softc *);
int	ena_set_mtu(struct ena_softc *, uint32_t);
int	ena_rss_config(struct ena_softc *);
int	ena_aenq_init(struct ena_softc *);
void	ena_aenq_intr(struct ena_softc *);

int	ena_intr_mgmt(void *);
int	ena_intr_io_queue(void *);

/* ifnet/ifmedia routines (Task 5) */
int	ena_ioctl(struct ifnet *, u_long, caddr_t);
void	ena_start(struct ifqueue *);
int	ena_media_change(struct ifnet *);
void	ena_media_status(struct ifnet *, struct ifmediareq *);
int	ena_init(struct ena_softc *);
void	ena_stop(struct ena_softc *);

/* Keep-alive watchdog (Increment 1: detection only) */
void	ena_tick(void *);
void	ena_link_state(struct ena_softc *);

/* Device reset / recovery (Increment 2) */
void	ena_reset_task(void *);
void	ena_destroy_device(struct ena_softc *, int *);
int	ena_restore_device(struct ena_softc *, int);

/* IO RX path (Task 6) */
void	ena_cq_unmask(struct ena_softc *, bus_size_t);
void	ena_rx_fill(struct ena_rxq *);
int	ena_rxeof(struct ena_rxq *);
int	ena_rx_intr(struct ena_rxq *);

/* IO TX path (Task 7) */
int	ena_tx_llq_map(struct ena_softc *, struct pci_attach_args *);
int	ena_tx_llq_setup(struct ena_softc *);
int	ena_tx_llq_negotiate(struct ena_softc *, struct pci_attach_args *);
int	ena_tx_create(struct ena_softc *, unsigned int);
void	ena_tx_destroy(struct ena_softc *, unsigned int);
int	ena_encap(struct ena_txq *, struct mbuf *);
int	ena_txeof(struct ena_txq *);

#endif /* _DEV_PCI_IF_ENAVAR_H_ */
