/*	$OpenBSD$	*/

/*-
 * Register definitions for the Amazon Elastic Network Adapter (ENA).
 *
 * BAR0 register offsets, field masks, and shifts are transcribed verbatim
 * from the Amazon ena-com protocol headers (BSD-2-Clause / Linux-OpenIB
 * dual-licensed). Source of truth:
 *   reference/amzn-drivers/kernel/linux/common/ena_com/ena_regs_defs.h
 * (identical offsets in FreeBSD's vendored
 *   sys/contrib/ena-com/ena_defs/ena_regs_defs.h).
 *
 * Do NOT edit offsets/masks by hand: they are the device ABI and must match
 * the hardware bit-for-bit. See docs/ena-idiom-map.md for the citation map.
 */

#ifndef _DEV_PCI_IF_ENAREG_H_
#define _DEV_PCI_IF_ENAREG_H_

/*
 * BAR layout. ENA exposes the register block in BAR0; BAR2 is the optional
 * LLQ "mem bar" which Phase 0 ignores.
 *	reference/freebsd/sys/dev/ena/ena.h (ENA_REG_BAR == 0, ENA_MEM_BAR == 2)
 */
#define ENA_PCI_BAR0		0x10	/* PCI_MAPREG_START: BAR0 reg block */
#define ENA_PCI_MEM_BAR		0x18	/* BAR2: LLQ mem bar (mapped in Task 7) */

/*
 * BAR0 register offsets.
 *	ena_regs_defs.h:37-59 (offsets identical across all ena-com trees)
 */
#define ENA_REGS_VERSION_OFF			0x0	/* ena_regs_defs.h:37 */
#define ENA_REGS_CONTROLLER_VERSION_OFF		0x4	/* ena_regs_defs.h:38 */
#define ENA_REGS_CAPS_OFF			0x8	/* ena_regs_defs.h:39 */
#define ENA_REGS_CAPS_EXT_OFF			0xc	/* ena_regs_defs.h:40 */
#define ENA_REGS_AQ_BASE_LO_OFF			0x10	/* ena_regs_defs.h:41 */
#define ENA_REGS_AQ_BASE_HI_OFF			0x14	/* ena_regs_defs.h:42 */
#define ENA_REGS_AQ_CAPS_OFF			0x18	/* ena_regs_defs.h:43 */
#define ENA_REGS_ACQ_BASE_LO_OFF		0x20	/* ena_regs_defs.h:44 */
#define ENA_REGS_ACQ_BASE_HI_OFF		0x24	/* ena_regs_defs.h:45 */
#define ENA_REGS_ACQ_CAPS_OFF			0x28	/* ena_regs_defs.h:46 */
#define ENA_REGS_AQ_DB_OFF			0x2c	/* ena_regs_defs.h:47 */
#define ENA_REGS_ACQ_TAIL_OFF			0x30	/* ena_regs_defs.h:48 */
#define ENA_REGS_AENQ_CAPS_OFF			0x34	/* ena_regs_defs.h:49 */
#define ENA_REGS_AENQ_BASE_LO_OFF		0x38	/* ena_regs_defs.h:50 */
#define ENA_REGS_AENQ_BASE_HI_OFF		0x3c	/* ena_regs_defs.h:51 */
#define ENA_REGS_AENQ_HEAD_DB_OFF		0x40	/* ena_regs_defs.h:52 */
#define ENA_REGS_AENQ_TAIL_OFF			0x44	/* ena_regs_defs.h:53 */
#define ENA_REGS_INTR_MASK_OFF			0x4c	/* ena_regs_defs.h:54 */
#define ENA_REGS_DEV_CTL_OFF			0x54	/* ena_regs_defs.h:55 */
#define ENA_REGS_DEV_STS_OFF			0x58	/* ena_regs_defs.h:56 */
#define ENA_REGS_MMIO_REG_READ_OFF		0x5c	/* ena_regs_defs.h:57 */
#define ENA_REGS_MMIO_RESP_LO_OFF		0x60	/* ena_regs_defs.h:58 */
#define ENA_REGS_MMIO_RESP_HI_OFF		0x64	/* ena_regs_defs.h:59 */

/*
 * Field masks/shifts consumed by the Phase 0 bring-up path (Tasks 5-8).
 *	ena_regs_defs.h:83-138
 */

/* CAPS register (ena_regs_defs.h:83-88) */
#define ENA_REGS_CAPS_RESET_TIMEOUT_SHIFT	1
#define ENA_REGS_CAPS_RESET_TIMEOUT_MASK	0x3e
#define ENA_REGS_CAPS_DMA_ADDR_WIDTH_SHIFT	8
#define ENA_REGS_CAPS_DMA_ADDR_WIDTH_MASK	0xff00
#define ENA_REGS_CAPS_ADMIN_CMD_TO_SHIFT	16
#define ENA_REGS_CAPS_ADMIN_CMD_TO_MASK		0xf0000

/* AQ_CAPS register (ena_regs_defs.h:91-93) */
#define ENA_REGS_AQ_CAPS_AQ_DEPTH_MASK		0xffff
#define ENA_REGS_AQ_CAPS_AQ_ENTRY_SIZE_SHIFT	16
#define ENA_REGS_AQ_CAPS_AQ_ENTRY_SIZE_MASK	0xffff0000

/* ACQ_CAPS register (ena_regs_defs.h:96-98) */
#define ENA_REGS_ACQ_CAPS_ACQ_DEPTH_MASK	0xffff
#define ENA_REGS_ACQ_CAPS_ACQ_ENTRY_SIZE_SHIFT	16
#define ENA_REGS_ACQ_CAPS_ACQ_ENTRY_SIZE_MASK	0xffff0000

/* AENQ_CAPS register (ena_regs_defs.h:101-103) */
#define ENA_REGS_AENQ_CAPS_AENQ_DEPTH_MASK	0xffff
#define ENA_REGS_AENQ_CAPS_AENQ_ENTRY_SIZE_SHIFT 16
#define ENA_REGS_AENQ_CAPS_AENQ_ENTRY_SIZE_MASK	0xffff0000

/* DEV_CTL register (ena_regs_defs.h:106, 115-116) */
#define ENA_REGS_DEV_CTL_DEV_RESET_MASK		0x1
#define ENA_REGS_DEV_CTL_RESET_REASON_SHIFT	28
#define ENA_REGS_DEV_CTL_RESET_REASON_MASK	0xf0000000

/* DEV_STS register (ena_regs_defs.h:119, 125) */
#define ENA_REGS_DEV_STS_READY_MASK		0x1
#define ENA_REGS_DEV_STS_RESET_IN_PROGRESS_MASK	0x8
#define ENA_REGS_DEV_STS_FATAL_ERROR_MASK	0x20

/* MMIO readless request register (ena_regs_defs.h:136-138) */
#define ENA_REGS_MMIO_REG_READ_REQ_ID_MASK	0xffff
#define ENA_REGS_MMIO_REG_READ_REG_OFF_SHIFT	16
#define ENA_REGS_MMIO_REG_READ_REG_OFF_MASK	0xffff0000

/*
 * All-ones MMIO sentinel: any 32-bit BAR read returning 0xffffffff indicates
 * the device is absent or surprise-removed (PCIe config space / MMIO reads
 * return all-ones on link-down or device removal). Used as an early check in
 * ena_reset before trusting DEV_STS / CAPS values.
 */
#define ENA_MMIO_INVAL				0xffffffffU

/*
 * Reset reasons (ena_regs_defs.h:9). Phase 0 only uses NORMAL.
 */
#define ENA_REGS_RESET_NORMAL			0

/*
 * Admin queue command/completion ABI structs.
 *
 * These are the device ABI: the byte layout of the SQ/CQ ring slots the
 * device DMAs. Transcribed verbatim (field order, types, sizes) from
 *   reference/freebsd/sys/contrib/ena-com/ena_defs/ena_admin_defs.h
 *   reference/freebsd/sys/contrib/ena-com/ena_defs/ena_common_defs.h
 * (identical in amzn-drivers common/ena_com/). __packed pins the layout so
 * no compiler padding diverges from hardware; the CTASSERTs in if_ena.c
 * verify the slot sizes against ENA_ADMIN_{AQ,ACQ}_ENTRY_SIZE.
 *
 * Phase 0 only submits/polls generic entries (GET_FEATURE lands in Task 8),
 * so we transcribe the common descriptors and the two 64-byte ring-slot
 * entries; the get/set-feature command bodies are added in Task 8.
 */

/* ena_common_defs.h:41 — 48-bit DMA address pair (low 32 + high 16). */
struct ena_common_mem_addr {
	uint32_t	mem_addr_low;
	uint16_t	mem_addr_high;
	uint16_t	reserved16;	/* MBZ */
} __packed;

/* ena_admin_defs.h:196 — AQ (submission) common descriptor. */
struct ena_admin_aq_common_desc {
	uint16_t	command_id;	/* 11:0 command_id, 15:12 reserved */
	uint8_t		opcode;		/* enum ena_admin_aq_opcode */
	uint8_t		flags;		/* bit0 phase, bit1 ctrl_data, ... */
} __packed;

/* ena_admin_defs.h:219 — control buffer descriptor (length + DMA addr). */
struct ena_admin_ctrl_buff_info {
	uint32_t			length;
	struct ena_common_mem_addr	address;
} __packed;

/* ena_admin_defs.h:236 — 64-byte SQ ring slot. */
struct ena_admin_aq_entry {
	struct ena_admin_aq_common_desc	aq_common_descriptor;
	union {
		uint32_t			inline_data_w1[3];
		struct ena_admin_ctrl_buff_info	control_buffer;
	} u;
	uint32_t			inline_data_w4[12];
} __packed;

/* ena_admin_defs.h:248 — ACQ (completion) common descriptor. */
struct ena_admin_acq_common_desc {
	uint16_t	command;	/* 11:0 command_id, 15:12 reserved */
	uint8_t		status;		/* enum ena_admin_aq_completion_status */
	uint8_t		flags;		/* bit0 phase, 7:1 reserved */
	uint16_t	extended_status;
	uint16_t	sq_head_indx;	/* AQ entry consumed by device */
} __packed;

/* ena_admin_defs.h:270 — 64-byte CQ ring slot. */
struct ena_admin_acq_entry {
	struct ena_admin_acq_common_desc	acq_common_descriptor;
	uint32_t				response_specific_data[14];
} __packed;

/*
 * aq/acq common-descriptor flags + command_id field encodings.
 *	ena_admin_defs.h:1321-1334
 */
#define ENA_ADMIN_AQ_COMMON_DESC_COMMAND_ID_MASK	0x0fff	/* GENMASK(11,0) */
#define ENA_ADMIN_AQ_COMMON_DESC_PHASE_MASK		0x1	/* BIT(0) */
#define ENA_ADMIN_ACQ_COMMON_DESC_COMMAND_ID_MASK	0x0fff	/* GENMASK(11,0) */
#define ENA_ADMIN_ACQ_COMMON_DESC_PHASE_MASK		0x1	/* BIT(0) */

/*
 * Admin opcodes / completion status (ena_admin_defs.h:57-68). Phase 0 reads
 * only GET_FEATURE; SUCCESS == 0 is the completion the poll returns.
 */
#define ENA_ADMIN_GET_FEATURE				8
#define ENA_ADMIN_SET_FEATURE				9
#define ENA_ADMIN_SUCCESS				0

/*
 * Feature ids for the get/set-feature admin commands (ena_admin_defs.h:80).
 * Phase 0 only issues DEVICE_ATTRIBUTES.
 */
#define ENA_ADMIN_DEVICE_ATTRIBUTES			1	/* ena_admin_defs.h:81 */

/*
 * aq_common_desc.flags control-data bits (ena_admin_defs.h:1323-1326). When a
 * GET_FEATURE response is delivered through an indirect control buffer (rather
 * than inline in the ACQ entry), CTRL_DATA_INDIRECT marks that the
 * control_buffer.address/length point at the response DMA buffer.
 */
#define ENA_ADMIN_AQ_COMMON_DESC_CTRL_DATA_MASK		0x2	/* BIT(1) */
#define ENA_ADMIN_AQ_COMMON_DESC_CTRL_DATA_INDIRECT_MASK 0x4	/* BIT(2) */

/*
 * VERSION register field masks/shifts (ena_regs_defs.h:68-70). Plain MMIO
 * read of ENA_REGS_VERSION_OFF; major in bits 15:8, minor in bits 7:0.
 */
#define ENA_REGS_VERSION_MINOR_VERSION_MASK		0xff
#define ENA_REGS_VERSION_MAJOR_VERSION_SHIFT		8
#define ENA_REGS_VERSION_MAJOR_VERSION_MASK		0xff00

/*
 * CONTROLLER_VERSION register field masks/shifts (ena_regs_defs.h:73-79).
 * Plain MMIO read of ENA_REGS_CONTROLLER_VERSION_OFF.
 */
#define ENA_REGS_CONTROLLER_VERSION_SUBMINOR_VERSION_MASK	0xff
#define ENA_REGS_CONTROLLER_VERSION_MINOR_VERSION_SHIFT		8
#define ENA_REGS_CONTROLLER_VERSION_MINOR_VERSION_MASK		0xff00
#define ENA_REGS_CONTROLLER_VERSION_MAJOR_VERSION_SHIFT		16
#define ENA_REGS_CONTROLLER_VERSION_MAJOR_VERSION_MASK		0xff0000
#define ENA_REGS_CONTROLLER_VERSION_IMPL_ID_SHIFT		24
#define ENA_REGS_CONTROLLER_VERSION_IMPL_ID_MASK		0xff000000

/*
 * GET/SET_FEATURE admin command/response ABI (ena_admin_defs.h). Transcribed
 * verbatim (field order, types, sizes) from
 *   reference/freebsd/sys/contrib/ena-com/ena_defs/ena_admin_defs.h
 * (identical in amzn-drivers common/ena_com/). __packed pins the layout to the
 * device ABI. Phase 0 only issues GET_FEATURE(DEVICE_ATTRIBUTES); the response
 * union member we read is dev_attr.
 */

/* ena_admin_defs.h:566 — get/set-feature common descriptor. */
struct ena_admin_get_set_feature_common_desc {
	uint8_t		flags;		/* 1:0 select (0x1 current / 0x3 default) */
	uint8_t		feature_id;	/* enum ena_admin_aq_feature_id */
	uint8_t		feature_version; /* zero based */
	uint8_t		reserved8;
} __packed;

/* ena_admin_defs.h:585 — DEVICE_ATTRIBUTES response payload. */
struct ena_admin_device_attr_feature_desc {
	uint32_t	impl_id;
	uint32_t	device_version;
	uint32_t	supported_features;	/* bitmap of ena_admin_aq_feature_id */
	uint32_t	capabilities;		/* bitmap of ena_admin_aq_caps_id */
	uint32_t	phys_addr_width;
	uint32_t	virt_addr_width;
	uint8_t		mac_addr[6];		/* unicast MAC, network byte order */
	uint16_t	flow_steering_max_entries; /* 0 if not supported */
	uint32_t	max_mtu;
} __packed;

/* ena_admin_defs.h:944 — STATELESS_OFFLOAD_CONFIG response payload. */
struct ena_admin_feature_offload_desc {
	uint32_t	tx;		/* TX offload support bitmap */
	uint32_t	rx_supported;	/* RX offload support bitmap */
	uint32_t	rx_enabled;	/* RX offload currently-enabled bitmap */
} __packed;

/* ena_admin_defs.h:1077 — GET_FEATURE command (64-byte SQ payload). */
struct ena_admin_get_feat_cmd {
	struct ena_admin_aq_common_desc			aq_common_descriptor;
	struct ena_admin_ctrl_buff_info			control_buffer;
	struct ena_admin_get_set_feature_common_desc	feat_common;
	uint32_t					raw[11];
} __packed;

/* ena_admin_defs.h:1137 — GET_FEATURE response (64-byte CQ payload). */
struct ena_admin_get_feat_resp {
	struct ena_admin_acq_common_desc	acq_common_desc;
	union {
		uint32_t				raw[14];
		struct ena_admin_device_attr_feature_desc dev_attr;
		struct ena_admin_feature_offload_desc	offload;
	} u;
} __packed;

/*
 * SET_FEATURE feature ids consumed by Task 4 (AENQ bring-up).
 *	ena_admin_defs.h:69 — ENA_ADMIN_AENQ_CONFIG = 26.
 */
#define ENA_ADMIN_AENQ_CONFIG				26

/*
 * MTU feature (ena_admin_defs.h:91 — ENA_ADMIN_MTU = 14). SET_FEATURE(MTU)
 * programs the device's maximum L2-payload frame size. The value EXCLUDES the
 * Ethernet header: it is the plain 1500/9000 if_mtu, not mtu+header.
 */
#define ENA_ADMIN_MTU					14

/*
 * RSS feature ids (ena_admin_defs.h:80-92). All three SET_FEATURE(RSS_*)
 * commands carry their bulk payload (hash key / per-proto field table /
 * indirection table) in the admin control buffer (flags |=
 * ENA_ADMIN_AQ_COMMON_DESC_CTRL_DATA_INDIRECT_MASK, control_buffer.address/
 * length pointing at a DMA region); only the small command body is inline.
 */
#define ENA_ADMIN_RSS_HASH_FUNCTION			10
#define ENA_ADMIN_RSS_INDIRECTION_TABLE_CONFIG		12
#define ENA_ADMIN_RSS_HASH_INPUT			18

#define ENA_ADMIN_TOEPLITZ				1	/* hash function */
#define ENA_ADMIN_RSS_KEY_PARTS				10	/* 40-byte key */
#define ENA_RX_RSS_TABLE_LOG_SIZE			7
#define ENA_RX_RSS_TABLE_SIZE	(1U << ENA_RX_RSS_TABLE_LOG_SIZE)	/* 128 */

/* ena_admin_flow_hash_proto indices into the per-proto field table. */
#define ENA_ADMIN_RSS_TCP4				0
#define ENA_ADMIN_RSS_UDP4				1
#define ENA_ADMIN_RSS_TCP6				2
#define ENA_ADMIN_RSS_UDP6				3
#define ENA_ADMIN_RSS_IP4				4
#define ENA_ADMIN_RSS_IP6				5
#define ENA_ADMIN_RSS_IP4_FRAG				6
#define ENA_ADMIN_RSS_NOT_IP				7
#define ENA_ADMIN_RSS_PROTO_NUM				16

/* ena_admin_flow_hash_fields: which header fields feed the hash per proto. */
#define ENA_ADMIN_RSS_L2_DA				0x01
#define ENA_ADMIN_RSS_L2_SA				0x02
#define ENA_ADMIN_RSS_L3_DA				0x04
#define ENA_ADMIN_RSS_L3_SA				0x08
#define ENA_ADMIN_RSS_L4_DP				0x10
#define ENA_ADMIN_RSS_L4_SP				0x20
/* input-sort bits for SET_FEATURE(RSS_HASH_INPUT) enabled_input_sort. */
#define ENA_ADMIN_RSS_L3_SORT				0x02	/* BIT(1) */
#define ENA_ADMIN_RSS_L4_SORT				0x04	/* BIT(2) */

/*
 * Stateless offload capability feature (ena_admin_defs.h:64). GET_FEATURE on
 * this id returns ena_admin_feature_offload_desc; the driver caches which RX/TX
 * checksum offloads the device supports.
 */
#define ENA_ADMIN_STATELESS_OFFLOAD_CONFIG		11

/*
 * feature_offload_desc support bit masks (ena_admin_defs.h:1475-1492). The TX
 * masks apply to the .tx word; the RX masks apply to the .rx_supported word.
 */
#define ENA_ADMIN_FEATURE_OFFLOAD_DESC_TX_L3_CSUM_IPV4_MASK	0x1	/* BIT(0) */
#define ENA_ADMIN_FEATURE_OFFLOAD_DESC_TX_L4_IPV4_CSUM_PART_MASK	0x2	/* BIT(1) */
#define ENA_ADMIN_FEATURE_OFFLOAD_DESC_TX_L4_IPV4_CSUM_FULL_MASK	0x4	/* BIT(2) */
#define ENA_ADMIN_FEATURE_OFFLOAD_DESC_RX_L3_CSUM_IPV4_MASK	0x1	/* BIT(0) */
#define ENA_ADMIN_FEATURE_OFFLOAD_DESC_RX_L4_IPV4_CSUM_MASK	0x2	/* BIT(1) */

/*
 * Host attributes (ena_admin_defs.h:71). The driver registers a 4KB host-info
 * page via SET_FEATURE(HOST_ATTR_CONFIG) right after admin init; the device
 * uses it to identify a real host driver and faults if it is never provided.
 */
#define ENA_ADMIN_HOST_ATTR_CONFIG			28
#define ENA_ADMIN_OS_LINUX				1	/* ena_com.h:30 */
/* ENA spec version the driver implements: (major << 8) | minor = 2.0. */
#define ENA_ADMIN_HOST_INFO_SPEC_VERSION		((2 << 8) | 0)
/* ENA driver version 2.0.0, encoded as major | minor << 8 | subminor << 16. */
#define ENA_ADMIN_HOST_INFO_DRIVER_VERSION		0x00000002

/* ena_admin_defs.h:1043 — 4KB host-info page (only the head is meaningful). */
struct ena_admin_host_info {
	uint32_t	os_type;		/* ENA_ADMIN_OS_* */
	uint8_t		os_dist_str[128];
	uint32_t	os_dist;
	uint8_t		kernel_ver_str[32];
	uint32_t	kernel_ver;
	uint32_t	driver_version;
	uint32_t	supported_network_features[2];
	uint16_t	ena_spec_version;
	uint16_t	bdf;
	uint16_t	num_cpus;
	uint16_t	reserved;
	uint32_t	driver_supported_features;
} __packed;

/* ena_admin_defs.h:1281 — SET_FEATURE(HOST_ATTR_CONFIG) body. */
struct ena_admin_set_feature_host_attr_desc {
	struct ena_common_mem_addr	os_info_ba;	/* 4KB host_info phys addr */
	struct ena_common_mem_addr	debug_ba;	/* unused (MBZ) */
	uint32_t			debug_area_size;/* 0 */
} __packed;

/*
 * ena_admin_defs.h:789 — SET_FEATURE(MTU) body. The mtu field is the L2
 * payload MTU (Ethernet header excluded), so it maps directly to if_mtu.
 */
struct ena_admin_set_feature_mtu_desc {
	uint32_t	mtu;		/* L2 payload MTU (no Ethernet header) */
} __packed;

/*
 * AENQ_CONFIG feature payload (ena_admin_defs.h:936). Two 32-bit group
 * bitmasks: supported_groups read back via GET_FEATURE, enabled_groups
 * written via SET_FEATURE to subscribe a subset.
 */
struct ena_admin_feature_aenq_desc {
	uint32_t	supported_groups;	/* groups the device can report */
	uint32_t	enabled_groups;		/* groups to actually report */
} __packed;

/*
 * RSS command bodies + control-buffer payloads (ena_admin_defs.h:876-1050).
 * The *_desc/_function/_input/_table structs are SET_FEATURE command bodies
 * (inline in the u union); the larger *_control structs and the entry array
 * are written into the admin control-buffer DMA region. Multibyte fields LE.
 */
struct ena_admin_feature_rss_flow_hash_function {	/* body, 12B */
	uint32_t	supported_func;
	uint32_t	selected_func;	/* 1 << ENA_ADMIN_TOEPLITZ */
	uint32_t	init_val;	/* 0: keep device default seed */
} __packed;

struct ena_admin_feature_rss_flow_hash_control {	/* ctrl buf, 48B */
	uint32_t	key_parts;	/* = ENA_ADMIN_RSS_KEY_PARTS (10) */
	uint32_t	reserved;
	uint32_t	key[ENA_ADMIN_RSS_KEY_PARTS];	/* 40-byte hash key */
} __packed;

struct ena_admin_feature_rss_flow_hash_input {		/* body, 4B */
	uint16_t	supported_input_sort;
	uint16_t	enabled_input_sort;	/* L3_SORT | L4_SORT */
} __packed;

struct ena_admin_proto_input {				/* 4B */
	uint16_t	fields;		/* ENA_ADMIN_RSS_L3_SA | ... */
	uint16_t	reserved2;
} __packed;

struct ena_admin_feature_rss_hash_control {		/* ctrl buf, 256B */
	struct ena_admin_proto_input	supported_fields[ENA_ADMIN_RSS_PROTO_NUM];
	struct ena_admin_proto_input	selected_fields[ENA_ADMIN_RSS_PROTO_NUM];
	struct ena_admin_proto_input	reserved2[ENA_ADMIN_RSS_PROTO_NUM];
	struct ena_admin_proto_input	reserved3[ENA_ADMIN_RSS_PROTO_NUM];
} __packed;

struct ena_admin_rss_ind_table_entry {			/* ctrl buf entry, 4B */
	uint16_t	cq_idx;		/* device-returned RX CQ index */
	uint16_t	reserved;
} __packed;

struct ena_admin_feature_rss_ind_table {		/* body, 16B */
	uint16_t	min_size;
	uint16_t	max_size;
	uint16_t	size;		/* log2 table size (ENA_RX_RSS_TABLE_LOG_SIZE) */
	uint8_t		flags;
	uint8_t		reserved;
	uint32_t	inline_index;	/* 0xFFFFFFFF = whole table in ctrl buf */
	struct ena_admin_rss_ind_table_entry	inline_entry;
} __packed;

/* ena_admin_defs.h:1281 — SET_FEATURE command (64-byte SQ payload). */
struct ena_admin_set_feat_cmd {
	struct ena_admin_aq_common_desc			aq_common_descriptor;
	struct ena_admin_ctrl_buff_info			control_buffer;
	struct ena_admin_get_set_feature_common_desc	feat_common;
	union {
		uint32_t				raw[11];
		struct ena_admin_feature_aenq_desc	aenq;
		struct ena_admin_set_feature_host_attr_desc host_attr;
		struct ena_admin_set_feature_mtu_desc	mtu;
		struct ena_admin_feature_rss_flow_hash_function	flow_hash_func;
		struct ena_admin_feature_rss_flow_hash_input	flow_hash_input;
		struct ena_admin_feature_rss_ind_table	ind_table;
	} u;
} __packed;

/*
 * Asynchronous Event Notification Queue (AENQ) ABI. The device DMA-writes
 * these entries into the host AENQ ring to report link-state changes,
 * keep-alive, fatal errors, and warnings. Transcribed verbatim (field order,
 * types, sizes) from
 *   reference/amzn-drivers/kernel/linux/common/ena_com/ena_admin_defs.h
 * with __packed pinning the layout to the device ABI.
 */

/* ena_admin_defs.h:1336 — AENQ common descriptor (16 bytes). */
struct ena_admin_aenq_common_desc {
	uint16_t	group;		/* enum ena_admin_aenq_group */
	uint16_t	syndrome;
	uint8_t		flags;		/* bit0 phase, 7:1 reserved (MBZ) */
	uint8_t		reserved1[3];
	uint32_t	timestamp_low;
	uint32_t	timestamp_high;
} __packed;

/*
 * ena_admin_defs.h:1354 — AENQ groups. Phase 1 subscribes LINK_CHANGE +
 * KEEP_ALIVE; FATAL_ERROR / WARNING are dispatched (logged) but not acted on.
 */
#define ENA_ADMIN_LINK_CHANGE		0	/* ena_admin_defs.h:1355 */
#define ENA_ADMIN_FATAL_ERROR		1	/* ena_admin_defs.h:1356 */
#define ENA_ADMIN_WARNING		2	/* ena_admin_defs.h:1357 */
#define ENA_ADMIN_KEEP_ALIVE		4	/* ena_admin_defs.h:1359 */

/*
 * Keep-alive watchdog (Increment 1: detection only). The device emits a
 * KEEP_ALIVE AENQ event roughly once per second; if none arrives for
 * ENA_KEEPALIVE_TIMEOUT_S the device is presumed dead. The watchdog also
 * samples DEV_STS for FATAL_ERROR / loss of READY. Values mirror FreeBSD
 * ena.h: DEFAULT_KEEP_ALIVE_TO (ena.h, 6s) and the 1 Hz wd callout
 * (ena_timer_service / DEFAULT_DEVICE_RESET_TO context).
 */
#define ENA_WATCHDOG_HZ			1	/* watchdog tick period, seconds */
#define ENA_KEEPALIVE_TIMEOUT_S		6	/* dead if no KEEP_ALIVE for this long (device beats ~1Hz) */
#define ENA_WD_FATAL_THRESH		3	/* consecutive bad DEV_STS reads before reset (debounce) */
#define ENA_WD_COOLDOWN_S		60	/* XXX iter1 DIAG: long cooldown to isolate a single reset */
#define ENA_FLAG_TRIGGER_RESET		0x01	/* reset requested (used fully in increment 2) */
#define ENA_FLAG_RESETTING		0x02	/* reset task mid-rebuild; guards re-entry */
#define ENA_FLAG_DYING			0x04	/* detach in progress; suppress reset */
/*
 * DEV_STS FATAL_ERROR (0x20) and READY (0x1) masks are defined above as
 * ENA_REGS_DEV_STS_FATAL_ERROR_MASK / ENA_REGS_DEV_STS_READY_MASK; the
 * watchdog reuses those.
 */

/* ena_admin_defs.h:1370 — 64-byte AENQ ring slot. */
struct ena_admin_aenq_entry {
	struct ena_admin_aenq_common_desc	aenq_common_desc;
	uint32_t				inline_data_w4[12];
} __packed;

/* ena_admin_defs.h:1377 — LINK_CHANGE event descriptor (overlays entry). */
struct ena_admin_aenq_link_change_desc {
	struct ena_admin_aenq_common_desc	aenq_common_desc;
	uint32_t				flags;	/* bit0 link_status */
} __packed;

/* aenq_common_desc flags (ena_admin_defs.h:1546). */
#define ENA_ADMIN_AENQ_COMMON_DESC_PHASE_MASK		0x1	/* BIT(0) */

/* aenq_link_change_desc flags (ena_admin_defs.h:1549). */
#define ENA_ADMIN_AENQ_LINK_CHANGE_DESC_LINK_STATUS_MASK 0x1	/* BIT(0) */

/*
 * IO queue create/destroy admin command/response ABI (Task 6). Transcribed
 * verbatim (field order, types, sizes) from
 *   reference/amzn-drivers/kernel/linux/common/ena_com/ena_admin_defs.h
 * __packed pins the layout to the device ABI; the CTASSERTs in if_ena.c verify
 * the CREATE_CQ/CREATE_SQ commands still fit a 64-byte SQ slot and their
 * responses a 64-byte CQ slot.
 *
 * Phase 1 creates exactly one RX CQ + RX SQ (host placement, physically
 * contiguous, interrupt mode), so only the fields ena_com programs are used.
 */

/* ena_admin_defs.h:32-35 — IO queue opcodes. */
#define ENA_ADMIN_CREATE_SQ		1	/* ena_admin_defs.h:32 */
#define ENA_ADMIN_DESTROY_SQ		2	/* ena_admin_defs.h:33 */
#define ENA_ADMIN_CREATE_CQ		3	/* ena_admin_defs.h:34 */
#define ENA_ADMIN_DESTROY_CQ		4	/* ena_admin_defs.h:35 */

/* ena_admin_defs.h:96-103 — SQ/CQ descriptor placement policy. */
#define ENA_ADMIN_PLACEMENT_POLICY_HOST	1	/* descriptors in host memory */
#define ENA_ADMIN_PLACEMENT_POLICY_DEV	3	/* descriptors in device (LLQ) */

/* ena_admin_defs.h:118-131 — SQ completion policy. */
#define ENA_ADMIN_COMPLETION_POLICY_DESC 0	/* cqe per sq descriptor */

/* ena_admin_defs.h:330-333 — SQ direction. */
#define ENA_ADMIN_SQ_DIRECTION_TX	1	/* ena_admin_defs.h:331 */
#define ENA_ADMIN_SQ_DIRECTION_RX	2	/* ena_admin_defs.h:332 */

/* ena_admin_defs.h:221 — SQ identity descriptor (used by DESTROY_SQ). */
struct ena_admin_sq {
	uint16_t	sq_idx;
	uint8_t		sq_identity;	/* 7:5 sq_direction */
	uint8_t		reserved1;
} __packed;

/* ena_admin_defs.h:272 — CREATE_SQ command (64-byte SQ payload). */
struct ena_admin_aq_create_sq_cmd {
	struct ena_admin_aq_common_desc	aq_common_descriptor;
	uint8_t				sq_identity;	/* 7:5 sq_direction */
	uint8_t				reserved8_w1;
	uint8_t				sq_caps_2;	/* 3:0 placement,
							   6:4 completion */
	uint8_t				sq_caps_3;	/* 0 is_phys_contig */
	uint16_t			cq_idx;		/* associated CQ */
	uint16_t			sq_depth;	/* entries */
	struct ena_common_mem_addr	sq_ba;		/* SQ ring base addr */
	struct ena_common_mem_addr	sq_head_writeback;
	uint32_t			reserved0_w7;
	uint32_t			reserved0_w8;
} __packed;

/* ena_admin_defs.h:335 — CREATE_SQ response (64-byte CQ payload). */
struct ena_admin_acq_create_sq_resp_desc {
	struct ena_admin_acq_common_desc	acq_common_desc;
	uint16_t				sq_idx;
	uint16_t				reserved;
	uint32_t				sq_doorbell_offset; /* BAR off */
	uint32_t				llq_descriptors_offset;
	uint32_t				llq_headers_offset;
} __packed;

/* ena_admin_defs.h:356 — DESTROY_SQ command (64-byte SQ payload). */
struct ena_admin_aq_destroy_sq_cmd {
	struct ena_admin_aq_common_desc	aq_common_descriptor;
	struct ena_admin_sq		sq;
} __packed;

/* ena_admin_defs.h:366 — CREATE_CQ command (64-byte SQ payload). */
struct ena_admin_aq_create_cq_cmd {
	struct ena_admin_aq_common_desc	aq_common_descriptor;
	uint8_t				cq_caps_1;	/* 5 interrupt_mode */
	uint8_t				cq_caps_2;	/* 4:0 entry_size_words */
	uint16_t			cq_depth;	/* entries, power of 2 */
	uint32_t			msix_vector;	/* MSI-X vector for CQ */
	struct ena_common_mem_addr	cq_ba;		/* CQ ring base addr */
} __packed;

/* ena_admin_defs.h:394 — CREATE_CQ response (64-byte CQ payload). */
struct ena_admin_acq_create_cq_resp_desc {
	struct ena_admin_acq_common_desc	acq_common_desc;
	uint16_t				cq_idx;
	uint16_t				cq_actual_depth;
	uint32_t				numa_node_register_offset;
	uint32_t				cq_head_db_register_offset;
	uint32_t				cq_interrupt_unmask_register_offset;
} __packed;

/* ena_admin_defs.h:409 — DESTROY_CQ command (64-byte SQ payload). */
struct ena_admin_aq_destroy_cq_cmd {
	struct ena_admin_aq_common_desc	aq_common_descriptor;
	uint16_t			cq_idx;
	uint16_t			reserved1;
} __packed;

/*
 * CREATE_SQ/CREATE_CQ field masks/shifts (ena_admin_defs.h:1446-1464).
 */
/* ena_admin_defs.h:1446-1447 — sq_identity in ena_admin_sq. */
#define ENA_ADMIN_SQ_SQ_DIRECTION_SHIFT			5
#define ENA_ADMIN_SQ_SQ_DIRECTION_MASK			0xe0	/* GENMASK(7,5) */
/* ena_admin_defs.h:1453-1459 — aq_create_sq_cmd. */
#define ENA_ADMIN_AQ_CREATE_SQ_CMD_SQ_DIRECTION_SHIFT	5
#define ENA_ADMIN_AQ_CREATE_SQ_CMD_SQ_DIRECTION_MASK	0xe0	/* GENMASK(7,5) */
#define ENA_ADMIN_AQ_CREATE_SQ_CMD_PLACEMENT_POLICY_MASK 0x0f	/* GENMASK(3,0) */
#define ENA_ADMIN_AQ_CREATE_SQ_CMD_COMPLETION_POLICY_SHIFT 4
#define ENA_ADMIN_AQ_CREATE_SQ_CMD_COMPLETION_POLICY_MASK 0x70	/* GENMASK(6,4) */
#define ENA_ADMIN_AQ_CREATE_SQ_CMD_IS_PHYSICALLY_CONTIGUOUS_MASK 0x1 /* BIT(0) */
/* ena_admin_defs.h:1462-1464 — aq_create_cq_cmd. */
#define ENA_ADMIN_AQ_CREATE_CQ_CMD_INTERRUPT_MODE_ENABLED_SHIFT 5
#define ENA_ADMIN_AQ_CREATE_CQ_CMD_INTERRUPT_MODE_ENABLED_MASK 0x20 /* BIT(5) */
#define ENA_ADMIN_AQ_CREATE_CQ_CMD_CQ_ENTRY_SIZE_WORDS_MASK 0x1f /* GENMASK(4,0) */

/*
 * IO RX descriptor ABI (Task 6). Transcribed verbatim (field order, types,
 * sizes) from
 *   reference/amzn-drivers/kernel/linux/common/ena_com/ena_eth_io_defs.h
 * __packed pins the layout to the device ABI. The host writes ena_eth_io_rx_desc
 * into the RX SQ; the device writes ena_eth_io_rx_cdesc_base into the RX CQ.
 */

/* ena_eth_io_defs.h:172 — RX submission descriptor (16 bytes, host->device). */
struct ena_eth_io_rx_desc {
	uint16_t	length;		/* buffer length, 0 means 64KB */
	uint8_t		reserved2;	/* MBZ */
	uint8_t		ctrl;		/* 0 phase, 2 first, 3 last, 4 comp_req */
	uint16_t	req_id;
	uint16_t	reserved6;	/* MBZ */
	uint32_t	buff_addr_lo;
	uint16_t	buff_addr_hi;
	uint16_t	reserved16_w3;	/* MBZ */
} __packed;

/* ena_eth_io_defs.h:206 — RX completion descriptor, 4-word base (device->host). */
struct ena_eth_io_rx_cdesc_base {
	uint32_t	status;		/* 24 phase, 26 first, 27 last, 30 buffer */
	uint16_t	length;
	uint16_t	req_id;
	uint32_t	hash;
	uint16_t	sub_qid;
	uint8_t		offset;
	uint8_t		reserved;
} __packed;

/* ena_eth_io_defs.h:355-361 — rx_desc ctrl bits (host->device). */
#define ENA_ETH_IO_RX_DESC_PHASE_MASK		0x1	/* BIT(0) */
#define ENA_ETH_IO_RX_DESC_FIRST_SHIFT		2
#define ENA_ETH_IO_RX_DESC_FIRST_MASK		0x4	/* BIT(2) */
#define ENA_ETH_IO_RX_DESC_LAST_SHIFT		3
#define ENA_ETH_IO_RX_DESC_LAST_MASK		0x8	/* BIT(3) */
#define ENA_ETH_IO_RX_DESC_COMP_REQ_SHIFT	4
#define ENA_ETH_IO_RX_DESC_COMP_REQ_MASK	0x10	/* BIT(4) */

/* ena_eth_io_defs.h:381-389 — rx_cdesc_base status bits (device->host). */
#define ENA_ETH_IO_RX_CDESC_BASE_PHASE_SHIFT	24
#define ENA_ETH_IO_RX_CDESC_BASE_PHASE_MASK	0x01000000U /* BIT(24) */
#define ENA_ETH_IO_RX_CDESC_BASE_FIRST_SHIFT	26
#define ENA_ETH_IO_RX_CDESC_BASE_FIRST_MASK	0x04000000U /* BIT(26) */
#define ENA_ETH_IO_RX_CDESC_BASE_LAST_SHIFT	27
#define ENA_ETH_IO_RX_CDESC_BASE_LAST_MASK	0x08000000U /* BIT(27) */
#define ENA_ETH_IO_RX_CDESC_BASE_BUFFER_SHIFT	30
#define ENA_ETH_IO_RX_CDESC_BASE_BUFFER_MASK	0x40000000U /* BIT(30) */

/*
 * rx_cdesc_base status csum/proto fields (ena_eth_io_defs.h:364-378). Used for
 * RX checksum-offload decode in ena_rxeof. L3/L4 proto index identify the parsed
 * packet type; the *_CSUM_ERR bits flag a bad checksum; CSUM_CHECKED marks that
 * the device actually validated the L4 checksum; IPV4_FRAG marks a fragment
 * (L4 csum not meaningful).
 */
#define ENA_ETH_IO_RX_CDESC_BASE_L3_PROTO_IDX_MASK	0x0000001fU
#define ENA_ETH_IO_RX_CDESC_BASE_L4_PROTO_IDX_SHIFT	8
#define ENA_ETH_IO_RX_CDESC_BASE_L4_PROTO_IDX_MASK	0x00001f00U
#define ENA_ETH_IO_RX_CDESC_BASE_L3_CSUM_ERR_MASK	0x00002000U /* BIT(13) */
#define ENA_ETH_IO_RX_CDESC_BASE_L4_CSUM_ERR_MASK	0x00004000U /* BIT(14) */
#define ENA_ETH_IO_RX_CDESC_BASE_IPV4_FRAG_MASK		0x00008000U /* BIT(15) */
#define ENA_ETH_IO_RX_CDESC_BASE_L4_CSUM_CHECKED_MASK	0x00010000U /* BIT(16) */

/* L3/L4 proto index enum values (ena_eth_io_defs.h:11,19,20). */
#define ENA_ETH_IO_L3_PROTO_IPV4	8
#define ENA_ETH_IO_L4_PROTO_TCP		12
#define ENA_ETH_IO_L4_PROTO_UDP		13

/*
 * IO completion-queue interrupt register (intr_reg). The device masks an IO
 * CQ's MSI-X interrupt after it fires; the host re-arms it by writing this
 * register (with INTR_UNMASK set) to the per-CQ unmask BAR offset returned in
 * the CREATE_CQ response (cq_interrupt_unmask_register_offset). The RX/TX delay
 * fields select interrupt moderation; Phase 1 uses zero delays (no moderation).
 * Mirrors ena_com_update_intr_reg (ena_com.h:1351) / ena_com_unmask_intr
 * (ena_eth_com.h:91, writes intr_control to io_cq->unmask_reg).
 *	ena_eth_io_defs.h:392-399 — intr_reg field masks/shifts.
 */
#define ENA_ETH_IO_INTR_REG_RX_INTR_DELAY_MASK	0x00007fffU /* GENMASK(14,0) */
#define ENA_ETH_IO_INTR_REG_TX_INTR_DELAY_SHIFT	15
#define ENA_ETH_IO_INTR_REG_TX_INTR_DELAY_MASK	0x3fff8000U /* GENMASK(29,15) */
#define ENA_ETH_IO_INTR_REG_INTR_UNMASK_SHIFT	30
#define ENA_ETH_IO_INTR_REG_INTR_UNMASK_MASK	0x40000000U /* BIT(30) */

/*
 * LLQ (Low Latency Queue) feature negotiation ABI (Task 7). Transcribed
 * verbatim (field order, types, sizes) from
 *   reference/amzn-drivers/kernel/linux/common/ena_com/ena_admin_defs.h
 * __packed pins the layout to the device ABI. The device reports its LLQ
 * capabilities via GET_FEATURE(ENA_ADMIN_LLQ); the driver echoes its chosen
 * placement back via SET_FEATURE(ENA_ADMIN_LLQ).
 */

/* ena_admin_defs.h:58 — feature id for GET/SET_FEATURE(LLQ). */
#define ENA_ADMIN_LLQ					4

/* ena_admin_defs.h:81-83 — LLQ feature version (GET uses VERSION_1). */
#define ENA_ADMIN_LLQ_FEATURE_VERSION_1			1

/* ena_admin_defs.h:610-615 — header location control (bitfield). */
#define ENA_ADMIN_LLQ_INLINE_HEADER			1	/* hdr in desc list */
#define ENA_ADMIN_LLQ_HEADER_RING			2	/* hdr in sep. ring */

/* ena_admin_defs.h:617-621 — LLQ ring entry size (bitfield). */
#define ENA_ADMIN_LLQ_LIST_ENTRY_SIZE_128B		1
#define ENA_ADMIN_LLQ_LIST_ENTRY_SIZE_192B		2
#define ENA_ADMIN_LLQ_LIST_ENTRY_SIZE_256B		4

/* ena_admin_defs.h:623-629 — number of descriptors before the header. */
#define ENA_ADMIN_LLQ_NUM_DESCS_BEFORE_HEADER_1		1
#define ENA_ADMIN_LLQ_NUM_DESCS_BEFORE_HEADER_2		2

/* ena_admin_defs.h:637-640 — descriptor stride control (bitfield). */
#define ENA_ADMIN_LLQ_SINGLE_DESC_PER_ENTRY		1
#define ENA_ADMIN_LLQ_MULTIPLE_DESCS_PER_ENTRY		2

/* ena_admin_defs.h:642-645 — accel mode feature flags. */
#define ENA_ADMIN_DISABLE_META_CACHING			0	/* bit index */
#define ENA_ADMIN_LIMIT_TX_BURST			1	/* bit index */

/* ena_admin_defs.h:647-670 — accel mode get/set union. */
struct ena_admin_accel_mode_get {
	uint16_t	supported_flags;
	uint16_t	max_tx_burst_size;
} __packed;

struct ena_admin_accel_mode_set {
	uint16_t	enabled_flags;
	uint16_t	reserved;
} __packed;

struct ena_admin_accel_mode_req {
	union {
		uint32_t			raw[2];
		struct ena_admin_accel_mode_get	get;
		struct ena_admin_accel_mode_set	set;
	} u;
} __packed;

/* ena_admin_defs.h:672-731 — LLQ feature descriptor (GET/SET payload). */
struct ena_admin_feature_llq_desc {
	uint32_t	max_llq_num;
	uint32_t	max_llq_depth;
	uint16_t	header_location_ctrl_supported;
	uint16_t	header_location_ctrl_enabled;
	uint16_t	entry_size_ctrl_supported;
	uint16_t	entry_size_ctrl_enabled;
	uint16_t	desc_num_before_header_supported;
	uint16_t	desc_num_before_header_enabled;
	uint16_t	descriptors_stride_ctrl_supported;
	uint16_t	descriptors_stride_ctrl_enabled;
	uint8_t		feature_version;
	uint8_t		entry_size_recommended;
	uint16_t	max_wide_llq_depth;
	struct ena_admin_accel_mode_req	accel_mode;
} __packed;

/*
 * GET_FEATURE(LLQ) response: the device writes the llq descriptor INLINE into
 * the ACQ completion entry, overlaying ena_admin_get_feat_resp.u (raw[14]).
 * The llq desc must fit that 56-byte payload.
 */
struct ena_admin_get_feat_llq_resp {
	struct ena_admin_acq_common_desc	acq_common_desc;
	union {
		uint32_t				raw[14];
		struct ena_admin_feature_llq_desc	llq;
	} u;
} __packed;

/*
 * SET_FEATURE(LLQ) command: the chosen placement is written in the llq union
 * member of the SET_FEATURE command body (overlays raw[11]).
 */
struct ena_admin_set_feat_llq_cmd {
	struct ena_admin_aq_common_desc			aq_common_descriptor;
	struct ena_admin_ctrl_buff_info			control_buffer;
	struct ena_admin_get_set_feature_common_desc	feat_common;
	union {
		uint32_t				raw[11];
		struct ena_admin_feature_llq_desc	llq;
	} u;
} __packed;

/*
 * IO TX descriptor ABI (Task 7). Transcribed verbatim (field order, types,
 * sizes) from
 *   reference/amzn-drivers/kernel/linux/common/ena_com/ena_eth_io_defs.h
 * __packed pins the layout to the device ABI. In LLQ (push) mode the host
 * writes ena_eth_io_tx_desc bytes (plus the packet header) directly into the
 * device LLQ memory window; the device writes ena_eth_io_tx_cdesc into the TX
 * CQ (host memory) as completions.
 */

/* ena_eth_io_defs.h:24 — TX submission descriptor (16 bytes). */
struct ena_eth_io_tx_desc {
	uint32_t	len_ctrl;		/* length + first/last/phase/... */
	uint32_t	meta_ctrl;		/* l3/l4 proto, csum, req_id_lo */
	uint32_t	buff_addr_lo;
	uint32_t	buff_addr_hi_hdr_sz;	/* addr_hi[15:0] + header_len[31:24] */
} __packed;

/* ena_eth_io_defs.h:100 — TX metadata descriptor (16 bytes, overlays tx_desc). */
struct ena_eth_io_tx_meta_desc {
	uint32_t	len_ctrl;
	uint32_t	word1;
	uint32_t	word2;
	uint32_t	reserved;
} __packed;

/* ena_eth_io_defs.h:146 — TX completion descriptor (8 bytes, device->host). */
struct ena_eth_io_tx_cdesc {
	uint16_t	req_id;
	uint8_t		status;
	uint8_t		flags;		/* bit0 phase */
	uint16_t	sub_qid;
	uint16_t	sq_head_idx;
} __packed;

/* ena_eth_io_defs.h:288-320 — tx_desc len_ctrl / meta_ctrl / addr field bits. */
#define ENA_ETH_IO_TX_DESC_LENGTH_MASK		0x0000ffffU /* GENMASK(15,0) */
#define ENA_ETH_IO_TX_DESC_REQ_ID_HI_SHIFT	16
#define ENA_ETH_IO_TX_DESC_REQ_ID_HI_MASK	0x003f0000U /* GENMASK(21,16) */
#define ENA_ETH_IO_TX_DESC_META_DESC_SHIFT	23
#define ENA_ETH_IO_TX_DESC_META_DESC_MASK	0x00800000U /* BIT(23) */
#define ENA_ETH_IO_TX_DESC_PHASE_SHIFT		24
#define ENA_ETH_IO_TX_DESC_PHASE_MASK		0x01000000U /* BIT(24) */
#define ENA_ETH_IO_TX_DESC_FIRST_SHIFT		26
#define ENA_ETH_IO_TX_DESC_FIRST_MASK		0x04000000U /* BIT(26) */
#define ENA_ETH_IO_TX_DESC_LAST_SHIFT		27
#define ENA_ETH_IO_TX_DESC_LAST_MASK		0x08000000U /* BIT(27) */
#define ENA_ETH_IO_TX_DESC_COMP_REQ_SHIFT	28
#define ENA_ETH_IO_TX_DESC_COMP_REQ_MASK	0x10000000U /* BIT(28) */
#define ENA_ETH_IO_TX_DESC_REQ_ID_LO_SHIFT	22
#define ENA_ETH_IO_TX_DESC_REQ_ID_LO_MASK	0xffc00000U /* GENMASK(31,22) */
#define ENA_ETH_IO_TX_DESC_ADDR_HI_MASK		0x0000ffffU /* GENMASK(15,0) */
#define ENA_ETH_IO_TX_DESC_HEADER_LENGTH_SHIFT	24
#define ENA_ETH_IO_TX_DESC_HEADER_LENGTH_MASK	0xff000000U /* GENMASK(31,24) */

/*
 * ena_eth_io_defs.h:301-315 — tx_desc meta_ctrl checksum-offload field bits.
 * These ride in the FIRST data descriptor's meta_ctrl word (the same word that
 * already carries REQ_ID_LO). L3/L4 proto idx select the protocol the device
 * checksums; L3/L4_CSUM_EN enable the offload; L4_CSUM_PARTIAL=1 selects PART
 * mode (device ADDS the payload sum to the pre-seeded pseudo-header field that
 * the OpenBSD stack already wrote, rather than computing a FULL csum).
 */
#define ENA_ETH_IO_TX_DESC_L3_PROTO_IDX_MASK	0x0000000fU /* GENMASK(3,0) */
#define ENA_ETH_IO_TX_DESC_L4_PROTO_IDX_SHIFT	8
#define ENA_ETH_IO_TX_DESC_L4_PROTO_IDX_MASK	0x00001f00U /* GENMASK(12,8) */
#define ENA_ETH_IO_TX_DESC_L3_CSUM_EN_MASK	0x00002000U /* BIT(13) */
#define ENA_ETH_IO_TX_DESC_L4_CSUM_EN_MASK	0x00004000U /* BIT(14) */
#define ENA_ETH_IO_TX_DESC_L4_CSUM_PARTIAL_MASK	0x00020000U /* BIT(17) */

/*
 * ena_eth_io_defs.h:300-345 — tx_meta_desc (16 bytes, overlays tx_desc) field
 * bits. The META descriptor carries the layer offsets/lengths the device needs
 * to locate L4 for checksum/TSO. For plain csum offload mss=0 (no segmentation)
 * so MSS_HI/MSS_LO are left zero. Mirrors ena_com_create_meta (ena_eth_com.c).
 */
#define ENA_ETH_IO_TX_META_DESC_LEN_CTRL_EXT_VALID_MASK		0x00004000U /* BIT(14) */
#define ENA_ETH_IO_TX_META_DESC_LEN_CTRL_MSS_HI_SHIFT		16
#define ENA_ETH_IO_TX_META_DESC_LEN_CTRL_MSS_HI_MASK		0x000f0000U /* GENMASK(19,16) */
#define ENA_ETH_IO_TX_META_DESC_LEN_CTRL_ETH_META_TYPE_MASK	0x00100000U /* BIT(20) */
#define ENA_ETH_IO_TX_META_DESC_LEN_CTRL_META_STORE_MASK	0x00200000U /* BIT(21) */
#define ENA_ETH_IO_TX_META_DESC_LEN_CTRL_META_DESC_MASK		0x00800000U /* BIT(23) */
#define ENA_ETH_IO_TX_META_DESC_LEN_CTRL_PHASE_SHIFT		24
#define ENA_ETH_IO_TX_META_DESC_LEN_CTRL_PHASE_MASK		0x01000000U /* BIT(24) */
#define ENA_ETH_IO_TX_META_DESC_LEN_CTRL_FIRST_MASK		0x04000000U /* BIT(26) */
#define ENA_ETH_IO_TX_META_DESC_LEN_CTRL_LAST_MASK		0x08000000U /* BIT(27) */
#define ENA_ETH_IO_TX_META_DESC_LEN_CTRL_COMP_REQ_MASK		0x10000000U /* BIT(28) */

#define ENA_ETH_IO_TX_META_DESC_WORD2_L3_HDR_LEN_MASK		0x000000ffU /* GENMASK(7,0) */
#define ENA_ETH_IO_TX_META_DESC_WORD2_L3_HDR_OFF_SHIFT		8
#define ENA_ETH_IO_TX_META_DESC_WORD2_L3_HDR_OFF_MASK		0x0000ff00U /* GENMASK(15,8) */
#define ENA_ETH_IO_TX_META_DESC_WORD2_L4_HDR_LEN_IN_WORDS_SHIFT	16
#define ENA_ETH_IO_TX_META_DESC_WORD2_L4_HDR_LEN_IN_WORDS_MASK	0x003f0000U /* GENMASK(21,16) */
#define ENA_ETH_IO_TX_META_DESC_WORD2_MSS_LO_SHIFT		22
#define ENA_ETH_IO_TX_META_DESC_WORD2_MSS_LO_MASK		0xffc00000U /* GENMASK(31,22) */

/* ena_eth_io_defs.h:350 — tx_cdesc flags. */
#define ENA_ETH_IO_TX_CDESC_PHASE_MASK		0x1	/* BIT(0) */

#endif /* _DEV_PCI_IF_ENAREG_H_ */
