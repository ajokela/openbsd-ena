/*	$OpenBSD$	*/

/*-
 * Driver for the Amazon Elastic Network Adapter (ENA).
 *
 * Phase 0: PCI match/attach/detach skeleton with the BAR0 register window
 * mapped. The admin queue, device bring-up, and IO queues are layered on by
 * later tasks. Structure mirrors the OpenBSD multiqueue-NIC idiom in
 * if_mcx.c (mcx_match/mcx_attach, if_mcx.c:2733/2739); register offsets and
 * PCI device IDs are transcribed from the BSD-licensed ena-com / FreeBSD
 * sources cited in docs/ena-idiom-map.md.
 */

#include "bpfilter.h"

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/sockio.h>
#include <sys/device.h>
#include <sys/endian.h>
#include <sys/malloc.h>
#include <sys/mbuf.h>
#include <sys/timeout.h>
#include <sys/task.h>

#include <machine/bus.h>

#include <net/if.h>
#include <net/if_media.h>
#include <net/toeplitz.h>	/* stoeplitz_to_key (RSS hash key) */

#if NBPFILTER > 0
#include <net/bpf.h>
#endif

#include <netinet/in.h>
#include <netinet/if_ether.h>
#include <netinet/ip.h>

#include <sys/intrmap.h>

#include <dev/pci/pcireg.h>
#include <dev/pci/pcivar.h>
#include <dev/pci/pcidevs.h>

#include <dev/pci/if_enareg.h>
#include <dev/pci/if_enavar.h>

int	ena_match(struct device *, void *, void *);
void	ena_attach(struct device *, struct device *, void *);
int	ena_detach(struct device *, int);

int	ena_dmamem_alloc(struct ena_softc *, struct ena_dma *, bus_size_t,
	    bus_size_t);
void	ena_dmamem_free(struct ena_softc *, struct ena_dma *);

int	ena_reset(struct ena_softc *);
void	ena_mmio_resp_write(struct ena_softc *);
int	ena_admin_init(struct ena_softc *);
int	ena_admin_poll(struct ena_softc *, struct ena_admin_aq_entry *,
	    struct ena_admin_acq_entry *);
int	ena_get_dev_attr(struct ena_softc *);
int	ena_set_host_attr(struct ena_softc *);
void	ena_aenq_register(struct ena_softc *);
int	ena_aenq_init(struct ena_softc *);
void	ena_aenq_intr(struct ena_softc *);

int	ena_intr_mgmt(void *);
int	ena_intr_io_queue(void *);

/* ifnet/ifmedia (Task 5) */
int	ena_ioctl(struct ifnet *, u_long, caddr_t);
void	ena_start(struct ifqueue *);
int	ena_media_change(struct ifnet *);
void	ena_media_status(struct ifnet *, struct ifmediareq *);
int	ena_init(struct ena_softc *);
void	ena_stop(struct ena_softc *);

/* Device reset / recovery (Increment 2) */
void	ena_reset_task(void *);
void	ena_destroy_device(struct ena_softc *, int *);
int	ena_restore_device(struct ena_softc *, int);

/* IO RX path (Task 6) */
void	ena_cq_unmask(struct ena_softc *, bus_size_t);
int	ena_rx_create(struct ena_softc *, unsigned int);
void	ena_rx_destroy(struct ena_softc *, unsigned int);
void	ena_rx_fill(struct ena_rxq *);
int	ena_rxeof(struct ena_rxq *);
int	ena_rx_intr(struct ena_rxq *);
void	ena_rx_refill(void *);

/* IO TX path (Task 7) */
int	ena_tx_llq_map(struct ena_softc *, struct pci_attach_args *);
int	ena_tx_llq_setup(struct ena_softc *);
int	ena_tx_llq_negotiate(struct ena_softc *, struct pci_attach_args *);
int	ena_tx_create(struct ena_softc *, unsigned int);
void	ena_tx_destroy(struct ena_softc *, unsigned int);
int	ena_encap(struct ena_txq *, struct mbuf *);
int	ena_txeof(struct ena_txq *);

/*
 * The ring-slot sizes pinned in if_enavar.h (Task 5) must equal the real
 * ABI struct sizes (if_enareg.h), or every SQ/CQ index arithmetic that uses
 * ENA_ADMIN_*_ENTRY_SIZE would stride the wrong number of bytes across the
 * DMA ring. Catch any __packed/padding surprise at build time (Task 9).
 */
CTASSERT(ENA_ADMIN_AQ_ENTRY_SIZE == sizeof(struct ena_admin_aq_entry));
CTASSERT(ENA_ADMIN_ACQ_ENTRY_SIZE == sizeof(struct ena_admin_acq_entry));

/*
 * The GET_FEATURE command overlays a 64-byte SQ slot and its response a
 * 64-byte CQ slot; the union ABI must match the ring-slot sizes bit-for-bit
 * or the device DMAs into/out of the wrong byte range. Pinned at build time
 * (Task 9). (ena_admin_defs.h:1077 / :1137.)
 */
CTASSERT(ENA_ADMIN_AQ_ENTRY_SIZE == sizeof(struct ena_admin_get_feat_cmd));
CTASSERT(ENA_ADMIN_ACQ_ENTRY_SIZE == sizeof(struct ena_admin_get_feat_resp));

/*
 * The AENQ ring-slot size pinned in if_enavar.h must equal the ABI struct
 * size: ena_aenq_intr strides the ring by ENA_AENQ_ENTRY_SIZE, and the device
 * programs the per-entry size into ENA_REGS_AENQ_CAPS from this same constant.
 * A __packed/padding surprise would desync host and device addressing.
 * (ena_admin_defs.h:1370.)
 */
CTASSERT(ENA_AENQ_ENTRY_SIZE == sizeof(struct ena_admin_aenq_entry));
/* The LINK_CHANGE descriptor overlays an AENQ entry; it must fit a slot. */
CTASSERT(sizeof(struct ena_admin_aenq_link_change_desc) <=
    sizeof(struct ena_admin_aenq_entry));
/* SET_FEATURE shares the 64-byte SQ slot with the other admin commands. */
CTASSERT(ENA_ADMIN_AQ_ENTRY_SIZE == sizeof(struct ena_admin_set_feat_cmd));

/*
 * The IO queue create/destroy commands overlay 64-byte SQ slots and their
 * responses overlay 64-byte CQ slots; a __packed/padding surprise would make
 * the device DMA into/out of the wrong byte range or read garbage cq_idx/
 * doorbell offsets back. Pinned at build time (Task 9).
 * (ena_admin_defs.h:272/335/356/366/394/409.)
 */
CTASSERT(sizeof(struct ena_admin_aq_create_sq_cmd) <= ENA_ADMIN_AQ_ENTRY_SIZE);
CTASSERT(sizeof(struct ena_admin_acq_create_sq_resp_desc) <=
    ENA_ADMIN_ACQ_ENTRY_SIZE);
CTASSERT(sizeof(struct ena_admin_aq_create_cq_cmd) <= ENA_ADMIN_AQ_ENTRY_SIZE);
CTASSERT(sizeof(struct ena_admin_acq_create_cq_resp_desc) <=
    ENA_ADMIN_ACQ_ENTRY_SIZE);
CTASSERT(sizeof(struct ena_admin_aq_destroy_sq_cmd) <= ENA_ADMIN_AQ_ENTRY_SIZE);
CTASSERT(sizeof(struct ena_admin_aq_destroy_cq_cmd) <= ENA_ADMIN_AQ_ENTRY_SIZE);

/*
 * The RX descriptor ring-slot sizes pinned in if_enavar.h must equal the ABI
 * struct sizes: ena_rx_fill and ena_rxeof stride the rings by these constants,
 * and the device programs the CQ entry size (in 32-bit words) from
 * sizeof(ena_eth_io_rx_cdesc_base) in the CREATE_CQ command.
 * (ena_eth_io_defs.h:172/206.)
 */
CTASSERT(ENA_RX_DESC_SIZE == sizeof(struct ena_eth_io_rx_desc));
CTASSERT(ENA_RX_CDESC_SIZE == sizeof(struct ena_eth_io_rx_cdesc_base));

/*
 * The TX descriptor / completion ring-slot sizes pinned in if_enavar.h must
 * equal the ABI struct sizes: ena_encap strides the LLQ window (and the
 * host-mem SQ ring) by ENA_TX_DESC_SIZE, ena_txeof strides the TX CQ by
 * ENA_TX_CDESC_SIZE, and the device programs the TX CQ entry size (in 32-bit
 * words) from sizeof(ena_eth_io_tx_cdesc) in the CREATE_CQ command.
 * (ena_eth_io_defs.h:24/146.)
 */
CTASSERT(ENA_TX_DESC_SIZE == sizeof(struct ena_eth_io_tx_desc));
CTASSERT(ENA_TX_CDESC_SIZE == sizeof(struct ena_eth_io_tx_cdesc));
/* The meta descriptor overlays a tx_desc slot; ena-com asserts equal sizes. */
CTASSERT(sizeof(struct ena_eth_io_tx_meta_desc) ==
    sizeof(struct ena_eth_io_tx_desc));
/* GET/SET_FEATURE(LLQ) overlay the 64-byte admin SQ/CQ ring slots. */
CTASSERT(sizeof(struct ena_admin_get_feat_llq_resp) <= ENA_ADMIN_ACQ_ENTRY_SIZE);
CTASSERT(sizeof(struct ena_admin_set_feat_llq_cmd) <= ENA_ADMIN_AQ_ENTRY_SIZE);
/* One LLQ entry must hold the descriptors-before-header plus an inline header. */
CTASSERT(ENA_LLQ_HEADER_OFFSET < ENA_LLQ_ENTRY_SIZE);

const struct cfattach ena_ca = {
	sizeof(struct ena_softc), ena_match, ena_attach, ena_detach
};

struct cfdriver ena_cd = {
	NULL, "ena", DV_IFNET
};

/*
 * Supported PCI IDs.
 *	PCI_VENDOR_AMAZON			0x1d0f
 *	PCI_PRODUCT_AMAZON_ENA_PF		0x0ec2	(physical function)
 *	PCI_PRODUCT_AMAZON_ENA_PF_RSERV0	0x1ec2	(PF, LLQ)
 *	PCI_PRODUCT_AMAZON_ENA_VF		0xec20	(virtual function)
 *	PCI_PRODUCT_AMAZON_ENA_VF_RSERV0	0xec21	(VF, LLQ)
 * Transcribed from reference/freebsd/sys/dev/ena/ena.h:154-159 (BSD-2-Clause);
 * cross-checked against reference/amzn-drivers/kernel/linux/ena/
 * ena_pci_id_tbl.h:10-26. These products are not yet in OpenBSD's pcidevs;
 * ena.files.fragment adds them (spliced in Task 9).
 */
static const struct pci_matchid ena_devices[] = {
	{ PCI_VENDOR_AMAZON, PCI_PRODUCT_AMAZON_ENA_PF },
	{ PCI_VENDOR_AMAZON, PCI_PRODUCT_AMAZON_ENA_PF_RSERV0 },
	{ PCI_VENDOR_AMAZON, PCI_PRODUCT_AMAZON_ENA_VF },
	{ PCI_VENDOR_AMAZON, PCI_PRODUCT_AMAZON_ENA_VF_RSERV0 },
};

int
ena_match(struct device *parent, void *match, void *aux)
{
	return (pci_matchbyid(aux, ena_devices, nitems(ena_devices)));
}

/*
 * Allocate a single physically contiguous, page-aligned, zeroed DMA buffer
 * and load it so ENA_DMA_DVA() returns the bus address. Mirrors
 * mcx_dmamem_alloc (if_mcx.c:8295) and the brief's skeleton: create map,
 * alloc phys, map to kva, load. BUS_DMA_64BIT lets the ring live anywhere
 * in the (up to 48-bit) DMA window ENA advertises in CAPS.
 *
 * The caller is responsible for the post-alloc bus_dmamap_sync that hands
 * the buffer to the device (see ena_attach).
 */
int
ena_dmamem_alloc(struct ena_softc *sc, struct ena_dma *m, bus_size_t size,
    bus_size_t align)
{
	int nsegs;

	m->edm_size = size;

	if (bus_dmamap_create(sc->sc_dmat, size, 1, size, 0,
	    BUS_DMA_WAITOK | BUS_DMA_ALLOCNOW | BUS_DMA_64BIT,
	    &m->edm_map) != 0)
		return (1);
	if (bus_dmamem_alloc(sc->sc_dmat, size, align, 0, &m->edm_seg, 1,
	    &nsegs, BUS_DMA_WAITOK | BUS_DMA_ZERO | BUS_DMA_64BIT) != 0)
		goto destroy;
	if (bus_dmamem_map(sc->sc_dmat, &m->edm_seg, nsegs, size,
	    &m->edm_kva, BUS_DMA_WAITOK) != 0)
		goto free;
	if (bus_dmamap_load(sc->sc_dmat, m->edm_map, m->edm_kva, size,
	    NULL, BUS_DMA_WAITOK) != 0)
		goto unmap;

	return (0);

unmap:
	bus_dmamem_unmap(sc->sc_dmat, m->edm_kva, size);
free:
	bus_dmamem_free(sc->sc_dmat, &m->edm_seg, 1);
destroy:
	bus_dmamap_destroy(sc->sc_dmat, m->edm_map);
	return (1);
}

void
ena_dmamem_free(struct ena_softc *sc, struct ena_dma *m)
{
	bus_dmamap_unload(sc->sc_dmat, m->edm_map);
	bus_dmamem_unmap(sc->sc_dmat, m->edm_kva, m->edm_size);
	bus_dmamem_free(sc->sc_dmat, &m->edm_seg, 1);
	bus_dmamap_destroy(sc->sc_dmat, m->edm_map);
}

void
ena_attach(struct device *parent, struct device *self, void *aux)
{
	struct ena_softc *sc = (struct ena_softc *)self;
	struct pci_attach_args *pa = aux;
	pci_intr_handle_t ih;
	const char *intrstr_mgmt, *intrstr_io = NULL;
	pcireg_t memtype;
	unsigned int i, nmsix;

	sc->sc_pc = pa->pa_pc;
	sc->sc_tag = pa->pa_tag;
	sc->sc_dmat = pa->pa_dmat;

	/* Map the BAR0 register window. */
	memtype = pci_mapreg_type(pa->pa_pc, pa->pa_tag, ENA_PCI_BAR0);
	if (pci_mapreg_map(pa, ENA_PCI_BAR0, memtype, 0,
	    &sc->sc_memt, &sc->sc_memh, NULL, &sc->sc_mems, 0)) {
		printf(": can't map registers\n");
		return;
	}

	/*
	 * Allocate the admin SQ/CQ DMA rings. Sized from the ena-com admin
	 * geometry (ENA_ADMIN_QUEUE_DEPTH * entry size; see if_enavar.h).
	 * Page-aligned: the device wants the ring base naturally aligned, and
	 * mcx aligns its cmdq ring identically (if_mcx.c:2791).
	 */
	sc->sc_admin_depth = ENA_ADMIN_QUEUE_DEPTH;
	sc->sc_aq_phase = 1;	/* first SQ descriptor is written phase 1 */
	sc->sc_acq_phase = 1;	/* first CQ completion is read at phase 1 */

	if (ena_dmamem_alloc(sc, &sc->sc_aq_dma,
	    ENA_ADMIN_SQ_SIZE(sc->sc_admin_depth), PAGE_SIZE) != 0) {
		printf(": can't allocate admin SQ ring\n");
		goto unmap;
	}
	if (ena_dmamem_alloc(sc, &sc->sc_acq_dma,
	    ENA_ADMIN_CQ_SIZE(sc->sc_admin_depth), PAGE_SIZE) != 0) {
		printf(": can't allocate admin CQ ring\n");
		goto free_aq;
	}

	/*
	 * Allocate the AENQ ring (device->host). Sized from the ena-com AENQ
	 * geometry (ENA_AENQ_DEPTH * entry size; see if_enavar.h). Page-aligned
	 * like the admin rings. The device->host hand-off sync and the BAR
	 * register programming happen in ena_aenq_init, after the device reset.
	 */
	sc->sc_aenq_depth = ENA_AENQ_DEPTH;
	if (ena_dmamem_alloc(sc, &sc->sc_aenq_dma,
	    ENA_AENQ_SIZE(sc->sc_aenq_depth), PAGE_SIZE) != 0) {
		printf(": can't allocate AENQ ring\n");
		goto free_acq;
	}

	/*
	 * Readless MMIO response region. The device tracks this host-memory
	 * mapping (registered via MMIO_RESP_LO/HI) and refuses later admin
	 * operations -- notably CREATE_CQ, which the device silently drops --
	 * if it was never handed a valid region. We read BAR registers directly
	 * (readless disabled), but the region must still be registered. Mirrors
	 * ena_com_mmio_reg_read_request_init (ena_com.c:2050). The address is
	 * (re)written in ena_reset, since reset clears it.
	 */
	if (ena_dmamem_alloc(sc, &sc->sc_mmio_resp_dma,
	    PAGE_SIZE, PAGE_SIZE) != 0) {
		printf(": can't allocate MMIO response region\n");
		goto free_aenq;
	}

	/*
	 * Initial ownership hand-off to the device.
	 *
	 * The rings were allocated BUS_DMA_ZERO, so the CPU has just written
	 * (zeroed) both buffers. Before the device touches them we must flush
	 * those CPU writes out and (for the CQ) prime the read side:
	 *
	 *   - sc_aq_dma (admin SQ): host produces descriptors, device reads
	 *     them. PREWRITE flushes the zeroed/initial ring so the device's
	 *     first read sees host memory, not a stale cache line. Per-submit
	 *     PREWRITE before each doorbell is added in Task 7.
	 *
	 *   - sc_acq_dma (admin CQ): device produces completions, host reads
	 *     them. PREREAD invalidates any stale CPU cache lines so the first
	 *     POSTREAD-synced phase-bit poll (Task 7) observes device writes.
	 *     We also zeroed it, hence the PREWRITE half: flush the zero fill
	 *     (which clears the phase bits) before the device starts writing.
	 *
	 * These run once here because the ring base addresses are programmed
	 * into the BAR (ENA_REGS_A[C]Q_BASE_LO/HI) and the device begins
	 * DMAing during admin_init (Task 7). Doing the hand-off at alloc time
	 * keeps the barrier adjacent to the producing CPU write (the zero
	 * fill) so no window exists where the device could read unsynced data.
	 *
	 * --------------------------------------------------------------------
	 * Per-ring sync audit (Task 5 review checklist; Step 4):
	 *
	 *   sc_aq_dma  (admin submission queue, host->device)
	 *     here (attach): PREWRITE  -- flush initial zeroed ring to device
	 *     Task 7 (submit): PREWRITE before each AQ doorbell write
	 *       (device reads the descriptor we just wrote)
	 *
	 *   sc_acq_dma (admin completion queue, device->host)
	 *     here (attach): PREREAD|PREWRITE -- flush zero fill + prime read
	 *     Task 7 (poll):  POSTREAD before every phase-bit check, re-check
	 *       (device wrote the completion we are about to read)
	 *
	 * Rule applied: PREWRITE before the device reads what we wrote;
	 * POSTREAD after the device writes what we read.
	 * --------------------------------------------------------------------
	 */
	bus_dmamap_sync(sc->sc_dmat, ENA_DMA_MAP(&sc->sc_aq_dma), 0,
	    ENA_DMA_LEN(&sc->sc_aq_dma), BUS_DMASYNC_PREWRITE);
	bus_dmamap_sync(sc->sc_dmat, ENA_DMA_MAP(&sc->sc_acq_dma), 0,
	    ENA_DMA_LEN(&sc->sc_acq_dma),
	    BUS_DMASYNC_PREREAD | BUS_DMASYNC_PREWRITE);

	/*
	 * Allocate two MSI-X vectors. ENA has no INTx fallback; if the
	 * device cannot deliver 2 MSI-X vectors we fall back to a single MSI
	 * for the management path and fail if even that is unavailable.
	 * Phase 1 requires 2 vectors (management + IO); fewer is an error.
	 *
	 * ENA vector assignment convention (ena_netdev.h:100-102):
	 *   vector 0  management — admin completion + AENQ (Task 4)
	 *   vector 1  IO         — RX + TX completion      (Tasks 6/7)
	 *
	 * The IO CQ's MSI-X vector index is delivered to the device via the
	 * CREATE_CQ admin command's msix_vector field (ena_com.c:1470);
	 * that wiring is Tasks 6/7. No separate SET_FEATURE(MSIX) call exists
	 * in the ENA ABI — the per-CQ field is the only mapping mechanism.
	 *
	 * Mirrors if_mcx.c:2895-2908 (admin vector) and :3014-3023
	 * (per-queue vectors).
	 */

	/* Management vector (vector 0: admin + AENQ). */
	if (pci_intr_map_msix(pa, 0, &ih) != 0) {
		/* Single MSI as last-resort mgmt path; IO cannot share it. */
		if (pci_intr_map_msi(pa, &ih) != 0) {
			printf(": couldn't map management interrupt\n");
			goto free_mmio_resp;
		}
	}
	intrstr_mgmt = pci_intr_string(sc->sc_pc, ih);
	sc->sc_mgmt_ih = pci_intr_establish(sc->sc_pc, ih,
	    IPL_NET | IPL_MPSAFE, ena_intr_mgmt, sc, sc->sc_dev.dv_xname);
	if (sc->sc_mgmt_ih == NULL) {
		printf(": couldn't establish management interrupt\n");
		goto free_mmio_resp;
	}

	/*
	 * IO vectors (vector 1..N: one MSI-X per RX/TX queue pair). The MSI-X
	 * table size bounds how many the device offers; reserve vector 0 for
	 * mgmt and let intrmap distribute the rest across CPUs, clamped to
	 * ENA_MAX_IO_QUEUES and the usable CPU count. On a 1-vCPU instance this
	 * resolves to a single queue, behaviourally identical to the
	 * pre-multiqueue driver.
	 */
	nmsix = pci_intr_msix_count(pa);
	if (nmsix < 2) {
		printf(": need at least 2 MSI-X vectors\n");
		goto disestablish_mgmt;
	}
	nmsix--;	/* vector 0 is the management vector */
	sc->sc_intrmap = intrmap_create(&sc->sc_dev, nmsix,
	    MIN(ENA_MAX_IO_QUEUES, IF_MAX_VECTORS), INTRMAP_POWEROF2);
	if (sc->sc_intrmap == NULL) {
		printf(": couldn't create intrmap\n");
		goto disestablish_mgmt;
	}
	sc->sc_nqueues = intrmap_count(sc->sc_intrmap);
	/*
	 * Arrays + IO interrupts are sized to this count NOW; the device's max
	 * queue count is only known after the admin queue is up (ena_get_dev_attr
	 * below), so sc_nqueues may be clamped DOWN there. Keep the allocated count
	 * separately: free/disestablish must use it, not the clamped active count.
	 */
	sc->sc_nqueues_alloc = sc->sc_nqueues;

	sc->sc_rxq = mallocarray(sc->sc_nqueues, sizeof(struct ena_rxq),
	    M_DEVBUF, M_WAITOK | M_ZERO);
	sc->sc_txq = mallocarray(sc->sc_nqueues, sizeof(struct ena_txq),
	    M_DEVBUF, M_WAITOK | M_ZERO);
	sc->sc_queues = mallocarray(sc->sc_nqueues, sizeof(struct ena_queue),
	    M_DEVBUF, M_WAITOK | M_ZERO);

	for (i = 0; i < sc->sc_nqueues; i++) {
		struct ena_queue *q = &sc->sc_queues[i];

		q->sc = sc;
		q->idx = i;
		snprintf(q->name, sizeof(q->name), "%s:%u",
		    sc->sc_dev.dv_xname, i);
		sc->sc_rxq[i].rxq_sc = sc;
		sc->sc_rxq[i].rxq_id = i;
		sc->sc_txq[i].txq_sc = sc;
		sc->sc_txq[i].txq_id = i;

		if (pci_intr_map_msix(pa, i + 1, &ih) != 0) {
			printf(": couldn't map IO interrupt %u\n", i);
			goto disestablish_io;
		}
		if (i == 0)
			intrstr_io = pci_intr_string(sc->sc_pc, ih);
		q->tag = pci_intr_establish_cpu(sc->sc_pc, ih,
		    IPL_NET | IPL_MPSAFE, intrmap_cpu(sc->sc_intrmap, i),
		    ena_intr_io_queue, q, q->name);
		if (q->tag == NULL) {
			printf(": couldn't establish IO interrupt %u\n", i);
			goto disestablish_io;
		}
	}

	sc->sc_nvec = 1 + sc->sc_nqueues;
	printf(": mgmt %s io %s x%u", intrstr_mgmt, intrstr_io, sc->sc_nqueues);

	/*
	 * Device bring-up: reset the device, then program the admin queue.
	 * Both run after the rings + MSI-X are in place (rings are already
	 * handed to the device above). The admin queue is polled in Phase 0,
	 * so no interrupt is on the bring-up critical path.
	 */
	/* Register the readless MMIO response region before reset. */
	ena_mmio_resp_write(sc);
	if (ena_reset(sc) != 0) {
		printf("\n%s: device reset failed\n", sc->sc_dev.dv_xname);
		goto disestablish_io;
	}
	if (ena_admin_init(sc) != 0) {
		printf("\n%s: admin queue init failed\n", sc->sc_dev.dv_xname);
		goto disestablish_io;
	}

	/*
	 * Register the host-info page immediately after admin init, before any
	 * further device interaction. The device requires this to recognize a
	 * real host driver; without it, it faults shortly after bring-up.
	 * Mirrors FreeBSD ena_config_host_info ordering.
	 */
	if (ena_set_host_attr(sc) != 0) {
		printf("\n%s: host attributes config failed\n",
		    sc->sc_dev.dv_xname);
		goto disestablish_io;
	}

	/*
	 * Read and print the device attributes over the admin queue. This is
	 * the Phase 0 milestone payload: the ENA/controller version, max MTU
	 * and MAC address line grepped over the serial console (Task 12).
	 */
	if (ena_get_dev_attr(sc) != 0) {
		printf("\n%s: device attributes query failed\n",
		    sc->sc_dev.dv_xname);
		goto disestablish_io;
	}

	/*
	 * Clamp the active queue count to what the device actually supports
	 * (MAX_QUEUES_EXT, read above). On AWS the VF provisions a queue per vCPU
	 * so this is normally a no-op, but never ask for more IO queues than the
	 * device has -- the surplus arrays/vectors stay allocated (freed via
	 * sc_nqueues_alloc) but unused. Queue creation, RSS, and the data path all
	 * use the clamped sc_nqueues.
	 */
	if (sc->sc_nqueues > sc->sc_max_io_queues) {
		printf("%s: clamping %u IO queues to device max %u\n",
		    sc->sc_dev.dv_xname, sc->sc_nqueues, sc->sc_max_io_queues);
		sc->sc_nqueues = sc->sc_max_io_queues;
	}

	/*
	 * Negotiate the TX descriptor placement (LLQ / push vs host memory).
	 * GET_FEATURE(LLQ) over the (polled) admin queue; if LLQ is supported,
	 * map the BAR2 LLQ memory window and SET_FEATURE(LLQ) to commit the
	 * chosen geometry. The chosen mode is printed for the milestone log.
	 * Runs after admin_init (the admin queue is usable) and before any IO
	 * queue is created (ena_init consumes sc_tx_llq when building the SQ).
	 */
	if (ena_tx_llq_negotiate(sc, pa) != 0) {
		printf("\n%s: TX placement negotiation failed\n",
		    sc->sc_dev.dv_xname);
		goto disestablish_io;
	}

	/*
	 * Bring up the AENQ: program its ring, subscribe LINK_CHANGE +
	 * KEEP_ALIVE, and unmask the management interrupt. After this the device
	 * delivers link-state events through ena_intr_mgmt -> ena_aenq_intr.
	 * Runs after admin init + GET_FEATURE so the admin queue is usable for
	 * the SET_FEATURE(AENQ_CONFIG) command.
	 */
	mtx_init(&sc->sc_aenq_mtx, IPL_NET);	/* before AENQ unmask: the ISR can fire after */
	if (ena_aenq_init(sc) != 0) {
		printf("\n%s: AENQ init failed\n", sc->sc_dev.dv_xname);
		goto disestablish_io;
	}

	/*
	 * Keep-alive watchdog (Increment 1: detection only). The device is now
	 * known-good and sc_aenq_groups reflects the AENQ groups actually
	 * subscribed, so set up the 1 Hz callout. We do NOT timeout_add here:
	 * arming is deferred to ena_init so the tick never runs while the
	 * interface is down (mirrors vio/mcx). Gate the keep-alive staleness
	 * check on whether KEEP_ALIVE was truly subscribed -- otherwise a device
	 * that never beats would be (wrongly) declared dead. sc_keepalive_last is
	 * seeded now so the first tick after up is not instantly stale.
	 */
	timeout_set(&sc->sc_wd_tick, ena_tick, sc);
	task_set(&sc->sc_reset_task, ena_reset_task, sc);
	sc->sc_keepalive_last = getuptime();
	sc->sc_keepalive_timeo =
	    (sc->sc_aenq_groups & (1U << ENA_ADMIN_KEEP_ALIVE)) ?
	    ENA_KEEPALIVE_TIMEOUT_S : 0;
	sc->sc_wd_active = 1;	/* XXX TEST: detection-only (auto-reset gated off below) */

	/*
	 * Wire up the ethernet interface (Task 5).
	 *
	 * The MAC was stashed into sc_ac.ac_enaddr by ena_get_dev_attr above.
	 * Mirror mcx_attach (if_mcx.c:2953-2976):
	 *   1. Fill ifp from sc_ac.ac_if; set softc pointer and xname.
	 *   2. Set flags, ioctl, qstart.
	 *   3. Init ifmedia with IFM_ETHER|IFM_AUTO.
	 *   4. if_attach + ether_ifattach.
	 *
	 * ena_start and ena_init/ena_stop are stubs until Tasks 6/7 wire
	 * the IO queues.
	 */
	{
		struct ifnet *ifp = &sc->sc_ac.ac_if;

		strlcpy(ifp->if_xname, sc->sc_dev.dv_xname, IFNAMSIZ);
		ifp->if_softc = sc;
		ifp->if_flags = IFF_BROADCAST | IFF_SIMPLEX | IFF_MULTICAST;
		ifp->if_ioctl = ena_ioctl;
		ifp->if_xflags = IFXF_MPSAFE;
		ifp->if_qstart = ena_start;
		/*
		 * Advertise the device's maximum MTU so ether_ioctl permits jumbo
		 * SIOCSIFMTU up to it. sc_max_mtu is already an L2-payload MTU (the
		 * device's max_mtu field excludes the Ethernet header), so assign it
		 * directly with no header subtraction. if_mtu stays at the ETHERMTU
		 * default until userland changes it.
		 */
		ifp->if_hardmtu = sc->sc_max_mtu;
		/*
		 * Advertise only the TX checksum offloads the device's OFFLOAD
		 * feature actually reported (sc_tx_csum_l3/l4, read at attach in
		 * ena_get_dev_attr). Advertising IFCAP_CSUM_* is what makes the
		 * IP stack hand ena_encap packets with M_*_CSUM_OUT set and the
		 * L4 field pre-seeded with the pseudo-header sum (so the device's
		 * PART-mode offload completes them). RX csum is handled in
		 * ena_rxeof; this only gates the TX-side capability bits.
		 */
		ifp->if_capabilities = 0;
		/*
		 * TX csum is emitted only on the LLQ push path (ena_encap), so
		 * advertise it only when LLQ was negotiated — otherwise the stack
		 * would delegate a checksum this driver would not complete. RX
		 * csum (ena_rxeof) needs no capability bit and works regardless.
		 */
		if (sc->sc_tx_llq) {
			if (sc->sc_tx_csum_l3)
				ifp->if_capabilities |= IFCAP_CSUM_IPv4;
			if (sc->sc_tx_csum_l4)
				ifp->if_capabilities |= IFCAP_CSUM_TCPv4 |
				    IFCAP_CSUM_UDPv4;
		}

		ifmedia_init(&sc->sc_media, IFM_IMASK, ena_media_change,
		    ena_media_status);
		ifmedia_add(&sc->sc_media, IFM_ETHER | IFM_AUTO, 0, NULL);
		ifmedia_set(&sc->sc_media, IFM_ETHER | IFM_AUTO);

		if_attach(ifp);
		ether_ifattach(ifp);

		/*
		 * Attach one stack send queue + one input queue per IO queue and
		 * bind each to its txq/rxq, so ena_start runs per-ifqueue (recovering
		 * its txq from ifq_softc) and ena_rxeof delivers per-ifiq. On a
		 * 1-vCPU instance sc_nqueues is 1 -- a single ifq/ifiq, as before.
		 */
		if_attach_queues(ifp, sc->sc_nqueues);
		if_attach_iqueues(ifp, sc->sc_nqueues);
		for (i = 0; i < sc->sc_nqueues; i++) {
			struct ifqueue *ifq = ifp->if_ifqs[i];
			struct ifiqueue *ifiq = ifp->if_iqs[i];

			ifq->ifq_softc = &sc->sc_txq[i];
			sc->sc_txq[i].txq_ifq = ifq;
			ifiq->ifiq_softc = &sc->sc_rxq[i];
			sc->sc_rxq[i].rxq_ifiq = ifiq;
		}
	}

	return;

	/*
	 * Attach failure unwind: disestablish in reverse establish order
	 * (IO before management) before freeing DMA memory.
	 */
disestablish_io:
	/* sc_nqueues_alloc: the count arrays/vectors were sized to (>= the clamped
	 * active sc_nqueues), so unwind by it -- not the possibly-clamped count. */
	for (i = 0; i < sc->sc_nqueues_alloc; i++) {
		if (sc->sc_queues[i].tag != NULL)
			pci_intr_disestablish(sc->sc_pc, sc->sc_queues[i].tag);
	}
	free(sc->sc_queues, M_DEVBUF,
	    sc->sc_nqueues_alloc * sizeof(struct ena_queue));
	free(sc->sc_rxq, M_DEVBUF, sc->sc_nqueues_alloc * sizeof(struct ena_rxq));
	free(sc->sc_txq, M_DEVBUF, sc->sc_nqueues_alloc * sizeof(struct ena_txq));
	intrmap_destroy(sc->sc_intrmap);
disestablish_mgmt:
	pci_intr_disestablish(sc->sc_pc, sc->sc_mgmt_ih);
	sc->sc_mgmt_ih = NULL;
	/* host-attr page is allocated late (ena_set_host_attr); free if present. */
	if (sc->sc_host_attr_dma.edm_map != NULL)
		ena_dmamem_free(sc, &sc->sc_host_attr_dma);
free_mmio_resp:
	ena_dmamem_free(sc, &sc->sc_mmio_resp_dma);
free_aenq:
	ena_dmamem_free(sc, &sc->sc_aenq_dma);
free_acq:
	ena_dmamem_free(sc, &sc->sc_acq_dma);
free_aq:
	ena_dmamem_free(sc, &sc->sc_aq_dma);
	if (sc->sc_llq_s != 0) {
		bus_space_unmap(sc->sc_llq_t, sc->sc_llq_h, sc->sc_llq_s);
		sc->sc_llq_s = 0;
	}
unmap:
	bus_space_unmap(sc->sc_memt, sc->sc_memh, sc->sc_mems);
	sc->sc_mems = 0;
}

/*
 * Device reset handshake. Mirrors ena_com_dev_reset (ena_com.c:2566) +
 * wait_for_reset_state (ena_com.c:1036) in OpenBSD idiom.
 *
 * Sequence (all reads via the BAR window — Phase 0 does not use the readless
 * MMIO path, so plain ENA_REG_RD32 is used):
 *   1. Read DEV_STS + CAPS. Gate on DEV_STS_READY (ena_com.c:2582); a device
 *      that is not ready cannot be reset.
 *   2. Derive the reset timeout from CAPS_RESET_TIMEOUT (units of 100ms,
 *      ena_com.c:2587-2592); a zero field is invalid.
 *   3. Write DEV_CTL = DEV_RESET | (NORMAL << RESET_REASON_SHIFT) to trigger
 *      reset (ena_com.c:2595-2618).
 *   4. Poll DEV_STS until RESET_IN_PROGRESS turns ON (ena_com.c:2623).
 *   5. Write DEV_CTL = 0 to clear the trigger (ena_com.c:2631).
 *   6. Poll DEV_STS until RESET_IN_PROGRESS turns OFF (ena_com.c:2632).
 *   7. Cache the admin-command timeout from CAPS_ADMIN_CMD_TO for the poll
 *      loop (ena_com.c:2638-2644).
 *
 * The timeout field is in 100ms units; we bound each poll phase with a
 * DELAY()-counted loop of that many milliseconds. ENA_RESET_POLL_DELAY_US is
 * the per-iteration spin.
 */
#define ENA_RESET_POLL_DELAY_US		1000	/* 1ms per poll iteration */
#define ENA_RESET_TIMEOUT_FALLBACK_MS	1000	/* if CAPS field unreadable */

static int
ena_reset_wait(struct ena_softc *sc, uint32_t timeout_ms, uint32_t exp_state)
{
	uint32_t sts;
	uint32_t i;

	for (i = 0; i < timeout_ms; i++) {
		sts = ENA_REG_RD32(sc, ENA_REGS_DEV_STS_OFF);
		if ((sts & ENA_REGS_DEV_STS_RESET_IN_PROGRESS_MASK) ==
		    exp_state)
			return (0);
		DELAY(ENA_RESET_POLL_DELAY_US);
	}

	return (ETIMEDOUT);
}

/*
 * Register the readless MMIO response region's physical address with the
 * device (MMIO_RESP_LO/HI). The device clears this on reset, so it is written
 * at attach and re-written in ena_reset after the reset trigger. Mirrors
 * ena_com_mmio_reg_read_request_write_dev_addr (ena_com.c:2100).
 */
void
ena_mmio_resp_write(struct ena_softc *sc)
{
	uint64_t pa;

	if (sc->sc_mmio_resp_dma.edm_map == NULL)
		return;
	pa = ENA_DMA_DVA(&sc->sc_mmio_resp_dma);
	ENA_REG_WR32(sc, ENA_REGS_MMIO_RESP_LO_OFF, (uint32_t)pa);
	ENA_REG_WR32(sc, ENA_REGS_MMIO_RESP_HI_OFF, (uint32_t)(pa >> 32));
}

int
ena_reset(struct ena_softc *sc)
{
	uint32_t sts, caps, reset_val, timeout_ms, cmd_to;

	sts = ENA_REG_RD32(sc, ENA_REGS_DEV_STS_OFF);
	if (sts == ENA_MMIO_INVAL) {
		printf("%s: device not responding\n", sc->sc_dev.dv_xname);
		return (ENXIO);
	}
	caps = ENA_REG_RD32(sc, ENA_REGS_CAPS_OFF);
	if (caps == ENA_MMIO_INVAL) {
		printf("%s: device not responding\n", sc->sc_dev.dv_xname);
		return (ENXIO);
	}

	if ((sts & ENA_REGS_DEV_STS_READY_MASK) == 0)
		return (ENXIO);

	/* CAPS reset timeout is in 100ms units (ena_com.c:2587). */
	timeout_ms = ((caps & ENA_REGS_CAPS_RESET_TIMEOUT_MASK) >>
	    ENA_REGS_CAPS_RESET_TIMEOUT_SHIFT) * 100;
	if (timeout_ms == 0)
		timeout_ms = ENA_RESET_TIMEOUT_FALLBACK_MS;

	/* Trigger reset, reason NORMAL in the high nibble. */
	reset_val = ENA_REGS_DEV_CTL_DEV_RESET_MASK;
	reset_val |= (ENA_REGS_RESET_NORMAL <<
	    ENA_REGS_DEV_CTL_RESET_REASON_SHIFT) &
	    ENA_REGS_DEV_CTL_RESET_REASON_MASK;
	ENA_REG_WR32(sc, ENA_REGS_DEV_CTL_OFF, reset_val);

	/* Reset clears the MMIO response region; re-register it (ena_com.c:2620). */
	ena_mmio_resp_write(sc);

	/* Wait for the device to acknowledge: RESET_IN_PROGRESS on. */
	if (ena_reset_wait(sc, timeout_ms,
	    ENA_REGS_DEV_STS_RESET_IN_PROGRESS_MASK) != 0)
		return (ETIMEDOUT);

	/* Clear the trigger and wait for reset to finish: in-progress off. */
	ENA_REG_WR32(sc, ENA_REGS_DEV_CTL_OFF, 0);
	if (ena_reset_wait(sc, timeout_ms, 0) != 0)
		return (ETIMEDOUT);

	/* Cache the admin completion timeout (CAPS, 100ms units). */
	cmd_to = (caps & ENA_REGS_CAPS_ADMIN_CMD_TO_MASK) >>
	    ENA_REGS_CAPS_ADMIN_CMD_TO_SHIFT;
	if (cmd_to != 0)
		sc->sc_admin_cmd_to_us = cmd_to * 100000;
	else
		sc->sc_admin_cmd_to_us = ENA_ADMIN_CMD_TIMEOUT_US;

	return (0);
}

/*
 * Program the admin SQ/CQ into the BAR and initialize the host-side ring
 * state. Mirrors ena_com_admin_init (ena_com.c:2112) for the AQ/ACQ portion
 * (the AENQ is Phase 1+; Phase 0 polls so we do not arm it). The rings were
 * allocated and handed to the device in ena_attach (Task 5), so this only
 * writes base addrs + caps and masks the admin interrupt.
 *
 * Register writes (ena_com.c:2157-2182):
 *   AQ_BASE_LO/HI  = low/high 32 of ENA_DMA_DVA(sc_aq_dma)
 *   ACQ_BASE_LO/HI = low/high 32 of ENA_DMA_DVA(sc_acq_dma)
 *   AQ_CAPS  = depth | (entry_size << ENTRY_SIZE_SHIFT)
 *   ACQ_CAPS = depth | (entry_size << ENTRY_SIZE_SHIFT)
 * Plus INTR_MASK = all-ones: Phase 0 polls the ACQ, so the admin interrupt
 * stays masked (the established MSI-X vector only proves plumbing).
 *
 * Host-side phase/index init mirrors ena_com_admin_init_sq/cq
 * (ena_com.c:143-167): tail=head=0, phase=1.
 */

/*
 * ena_aenq_register -- register the AENQ ring (base + caps) and seed its
 * host-side consumer state. This MUST happen as part of admin-queue init,
 * immediately after the AQ/ACQ base+caps writes, exactly as ena_com does in
 * ena_com_admin_init -> ena_com_admin_init_aenq (ena_com.c:2184-2186). The
 * device initializes its AENQ subsystem during the admin-queue handshake; if
 * the ring is registered only later (after the handshake has completed), the
 * device accepts the register writes but its AENQ is left half-initialized,
 * and the subsequent AENQ_HEAD_DB doorbell that activates the ring trips
 * DEV_STS FATAL_ERROR. Subscribing groups (GET/SET AENQ_CONFIG) and ringing
 * the head doorbell remain in ena_aenq_init, run later in attach.
 */
void
ena_aenq_register(struct ena_softc *sc)
{
	uint64_t aenq_pa;
	uint32_t aenq_caps;

	/*
	 * Host consumer state. head is seeded to depth (the initial head
	 * doorbell value) and phase to 1, exactly as ena_com seeds them
	 * (ena_com.c:153-154). The device DMA-writes the first ring wrap at
	 * phase 1, so the host's first phase-bit comparison matches.
	 */
	sc->sc_aenq_depth = ENA_AENQ_DEPTH;
	sc->sc_aenq_head = ENA_AENQ_DEPTH;
	sc->sc_aenq_phase = 1;
	sc->sc_link_up = 0;

	/*
	 * Initial ownership hand-off (device->host ring): flush the zeroed ring
	 * (clears phase bits) so the device starts from a known state, and prime
	 * the read side so the first POSTREAD-synced poll observes device DMA
	 * writes. Mirrors the admin-CQ hand-off in ena_attach.
	 */
	bus_dmamap_sync(sc->sc_dmat, ENA_DMA_MAP(&sc->sc_aenq_dma), 0,
	    ENA_DMA_LEN(&sc->sc_aenq_dma),
	    BUS_DMASYNC_PREREAD | BUS_DMASYNC_PREWRITE);

	/* Program the AENQ ring base address into the BAR. */
	aenq_pa = ENA_DMA_DVA(&sc->sc_aenq_dma);
	ENA_REG_WR32(sc, ENA_REGS_AENQ_BASE_LO_OFF, (uint32_t)aenq_pa);
	ENA_REG_WR32(sc, ENA_REGS_AENQ_BASE_HI_OFF, (uint32_t)(aenq_pa >> 32));

	/* caps = depth (bits 15:0) | entry size (bits 31:16). */
	aenq_caps = (sc->sc_aenq_depth & ENA_REGS_AENQ_CAPS_AENQ_DEPTH_MASK) |
	    ((sizeof(struct ena_admin_aenq_entry) <<
	    ENA_REGS_AENQ_CAPS_AENQ_ENTRY_SIZE_SHIFT) &
	    ENA_REGS_AENQ_CAPS_AENQ_ENTRY_SIZE_MASK);
	ENA_REG_WR32(sc, ENA_REGS_AENQ_CAPS_OFF, aenq_caps);
}

int
ena_admin_init(struct ena_softc *sc)
{
	uint64_t aq_pa, acq_pa;
	uint32_t sts, aq_caps, acq_caps;

	sts = ENA_REG_RD32(sc, ENA_REGS_DEV_STS_OFF);
	if ((sts & ENA_REGS_DEV_STS_READY_MASK) == 0)
		return (ENXIO);

	/* Host-side ring state: first descriptor/completion is phase 1. */
	sc->sc_aq_tail = 0;
	sc->sc_acq_head = 0;
	sc->sc_aq_phase = 1;
	sc->sc_acq_phase = 1;
	sc->sc_admin_cmd_id = 0;

	aq_pa = ENA_DMA_DVA(&sc->sc_aq_dma);
	acq_pa = ENA_DMA_DVA(&sc->sc_acq_dma);

	ENA_REG_WR32(sc, ENA_REGS_AQ_BASE_LO_OFF, (uint32_t)aq_pa);
	ENA_REG_WR32(sc, ENA_REGS_AQ_BASE_HI_OFF, (uint32_t)(aq_pa >> 32));
	ENA_REG_WR32(sc, ENA_REGS_ACQ_BASE_LO_OFF, (uint32_t)acq_pa);
	ENA_REG_WR32(sc, ENA_REGS_ACQ_BASE_HI_OFF, (uint32_t)(acq_pa >> 32));

	aq_caps = (sc->sc_admin_depth & ENA_REGS_AQ_CAPS_AQ_DEPTH_MASK) |
	    ((sizeof(struct ena_admin_aq_entry) <<
	    ENA_REGS_AQ_CAPS_AQ_ENTRY_SIZE_SHIFT) &
	    ENA_REGS_AQ_CAPS_AQ_ENTRY_SIZE_MASK);
	acq_caps = (sc->sc_admin_depth & ENA_REGS_ACQ_CAPS_ACQ_DEPTH_MASK) |
	    ((sizeof(struct ena_admin_acq_entry) <<
	    ENA_REGS_ACQ_CAPS_ACQ_ENTRY_SIZE_SHIFT) &
	    ENA_REGS_ACQ_CAPS_ACQ_ENTRY_SIZE_MASK);

	ENA_REG_WR32(sc, ENA_REGS_AQ_CAPS_OFF, aq_caps);
	ENA_REG_WR32(sc, ENA_REGS_ACQ_CAPS_OFF, acq_caps);

	/*
	 * Register the AENQ ring as part of the admin-queue handshake, right
	 * after the AQ/ACQ caps, mirroring ena_com_admin_init (ena_com.c:2186).
	 */
	ena_aenq_register(sc);

	/* Phase 0 polls the admin CQ: keep the admin interrupt masked. */
	ENA_REG_WR32(sc, ENA_REGS_INTR_MASK_OFF, 0xffffffff);

	return (0);
}

/*
 * Submit one admin command and poll for its completion. Mirrors the polled
 * path of ena-com: __ena_com_submit_admin_cmd (ena_com.c:255) +
 * ena_com_wait_and_process_admin_cq_polling (ena_com.c:598) +
 * ena_com_handle_admin_completion (ena_com.c:527), collapsed to the
 * single-threaded, single-outstanding-command Phase 0 case.
 *
 * cmd:  caller-filled SQ entry (opcode + body). We stamp command_id + phase.
 * resp: caller buffer; on ENA_ADMIN_SUCCESS we copy the matching CQ entry in.
 * Returns 0 on success, ETIMEDOUT on no completion, or EIO if the device
 * reports a non-SUCCESS completion status.
 *
 * Ring-sync audit (every bus_dmamap_sync, direction, why):
 *   SQ PREWRITE  (before doorbell): we just wrote cmd into the SQ slot; flush
 *                that CPU write so the device's read of the descriptor sees
 *                it. Pairs with the device read triggered by the doorbell.
 *                (ena-com hides this in ENA_DB_SYNC, ena_com.c:308.)
 *   CQ POSTREAD  (before every phase-bit check): the device DMA-wrote the
 *                completion; invalidate/refresh the CPU view so the phase-bit
 *                read observes device memory, not a stale cache line. Re-run
 *                each loop iteration before re-reading the slot, exactly as
 *                mcx_cmdq_poll re-syncs before each ownership-bit test
 *                (if_mcx.c:3148-3151).
 */
int
ena_admin_poll(struct ena_softc *sc, struct ena_admin_aq_entry *cmd,
    struct ena_admin_acq_entry *resp)
{
	struct ena_admin_aq_entry *sq;
	struct ena_admin_acq_entry *cqe;
	uint16_t qmask, sq_idx, cq_idx, cmd_id;
	uint32_t i, timeout_ms;
	uint8_t phase;

	qmask = sc->sc_admin_depth - 1;
	sq = (struct ena_admin_aq_entry *)ENA_DMA_KVA(&sc->sc_aq_dma);
	cqe = (struct ena_admin_acq_entry *)ENA_DMA_KVA(&sc->sc_acq_dma);

	/* --- Submit: write one descriptor into the SQ at the host tail. --- */
	sq_idx = sc->sc_aq_tail & qmask;
	cmd_id = sc->sc_admin_cmd_id;

	/* Stamp command_id and the current SQ producer phase into the cmd. */
	cmd->aq_common_descriptor.command_id |=
	    cmd_id & ENA_ADMIN_AQ_COMMON_DESC_COMMAND_ID_MASK;
	cmd->aq_common_descriptor.flags &= ~ENA_ADMIN_AQ_COMMON_DESC_PHASE_MASK;
	cmd->aq_common_descriptor.flags |=
	    sc->sc_aq_phase & ENA_ADMIN_AQ_COMMON_DESC_PHASE_MASK;

	sq[sq_idx] = *cmd;

	/* Advance host SQ producer state; flip phase on ring wrap. */
	sc->sc_admin_cmd_id = (sc->sc_admin_cmd_id + 1) & qmask;
	sc->sc_aq_tail++;
	if ((sc->sc_aq_tail & qmask) == 0)
		sc->sc_aq_phase ^= 1;

	/*
	 * SQ PREWRITE before ringing: flush the descriptor we just stored so
	 * the device read behind the doorbell sees it. Sync the whole ring
	 * (single-segment, cheap) to match the attach-time hand-off.
	 */
	bus_dmamap_sync(sc->sc_dmat, ENA_DMA_MAP(&sc->sc_aq_dma), 0,
	    ENA_DMA_LEN(&sc->sc_aq_dma), BUS_DMASYNC_PREWRITE);

	/* Ring the SQ doorbell with the new tail (post-write barrier inside). */
	ENA_REG_WR32_DB(sc, ENA_REGS_AQ_DB_OFF, sc->sc_aq_tail);

	/* --- Poll: wait for a completion at the host CQ head. --- */
	cq_idx = sc->sc_acq_head & qmask;
	phase = sc->sc_acq_phase;

	timeout_ms = sc->sc_admin_cmd_to_us / 1000;
	if (timeout_ms == 0)
		timeout_ms = ENA_ADMIN_CMD_TIMEOUT_US / 1000;

	for (i = 0; i < timeout_ms; i++) {
		/*
		 * POSTREAD before reading the phase bit: refresh the CPU view
		 * of the CQ slot the device DMA-wrote. Must precede every
		 * phase-bit check (re-synced each iteration).
		 */
		bus_dmamap_sync(sc->sc_dmat, ENA_DMA_MAP(&sc->sc_acq_dma), 0,
		    ENA_DMA_LEN(&sc->sc_acq_dma), BUS_DMASYNC_POSTREAD);

		if ((cqe[cq_idx].acq_common_descriptor.flags &
		    ENA_ADMIN_ACQ_COMMON_DESC_PHASE_MASK) == phase)
			goto completed;

		DELAY(ENA_RESET_POLL_DELAY_US);
	}

	return (ETIMEDOUT);

completed:
	/*
	 * The phase bit is set; the rest of the completion entry is now valid
	 * (the device writes the body before the phase bit, and our POSTREAD
	 * ordered the whole slot). Advance the host CQ consumer state and flip
	 * phase on wrap, mirroring ena_com_handle_admin_completion.
	 */
	if (resp != NULL)
		*resp = cqe[cq_idx];

	sc->sc_acq_head++;
	if ((sc->sc_acq_head & qmask) == 0)
		sc->sc_acq_phase ^= 1;

	/* ENA descriptors are little-endian; multi-byte fields use letoh16/32. */
	if (cqe[cq_idx].acq_common_descriptor.status != ENA_ADMIN_SUCCESS) {
		printf("%s: admin cmd opcode %u status %u\n", sc->sc_dev.dv_xname,
		    cmd->aq_common_descriptor.opcode,
		    cqe[cq_idx].acq_common_descriptor.status);
		return (EIO);
	}

	return (0);
}

/*
 * Read and print the device attributes — the Phase 0 milestone payload.
 *
 * Two sources are read:
 *   1. ENA version + controller version: plain BAR MMIO reads of
 *      ENA_REGS_VERSION_OFF / ENA_REGS_CONTROLLER_VERSION_OFF, decoded with
 *      the version masks/shifts (ena_regs_defs.h:68-79). No admin command.
 *   2. MAC address + max MTU: a GET_FEATURE(DEVICE_ATTRIBUTES) admin command
 *      whose response the device writes INLINE into the ACQ completion entry.
 *
 * The GET_FEATURE build mirrors ena_com_get_feature_ex (ena_com.c) collapsed
 * to the Phase 0 case:
 *   - opcode = ENA_ADMIN_GET_FEATURE; feat_common.feature_id =
 *     ENA_ADMIN_DEVICE_ATTRIBUTES; feature_version = 0 (current value).
 *   - No control buffer: ena_com_get_feature() calls ena_com_get_feature_ex
 *     with control_buff_size == 0, so aq_common_descriptor.flags = 0 (NOT
 *     CTRL_DATA_INDIRECT) and control_buffer.length = 0 (ena_com.c:1046-1090).
 *     The device returns the dev_attr payload INLINE in the ACQ completion
 *     entry, which ena_com_handle_admin_completion copies into the caller's
 *     get_resp via memcpy(user_cqe, cqe, comp_size) (ena_com.c:1045-1046).
 *   - ena_admin_poll stamps command_id + phase, waits for the completion, and
 *     copies the ACQ entry into the caller's resp; we then reinterpret that
 *     resp as ena_admin_get_feat_resp and read u.dev_attr from it.
 *
 * No separate response-buffer sync is needed: the CQ-ring POSTREAD inside
 * ena_admin_poll (which validated the phase bit) already orders the read of
 * the inline payload out of the same completion slot.
 */
int
ena_get_dev_attr(struct ena_softc *sc)
{
	struct ena_admin_aq_entry cmd;
	struct ena_admin_acq_entry resp;
	struct ena_admin_get_feat_cmd *gf =
	    (struct ena_admin_get_feat_cmd *)&cmd;
	struct ena_admin_get_feat_resp *gr =
	    (struct ena_admin_get_feat_resp *)&resp;
	struct ena_admin_device_attr_feature_desc *attr;
	uint32_t ver, ctrl_ver;
	uint32_t ena_ver_maj, ena_ver_min, ctrl_ver_val;
	uint32_t max_mtu;
	uint8_t mac[ETHER_ADDR_LEN];
	int error;

	/* --- Version registers: plain BAR MMIO reads, no admin command. --- */
	ver = ENA_REG_RD32(sc, ENA_REGS_VERSION_OFF);
	ctrl_ver = ENA_REG_RD32(sc, ENA_REGS_CONTROLLER_VERSION_OFF);

	ena_ver_maj = (ver & ENA_REGS_VERSION_MAJOR_VERSION_MASK) >>
	    ENA_REGS_VERSION_MAJOR_VERSION_SHIFT;
	ena_ver_min = ver & ENA_REGS_VERSION_MINOR_VERSION_MASK;
	ctrl_ver_val = (ctrl_ver &
	    ENA_REGS_CONTROLLER_VERSION_MAJOR_VERSION_MASK) >>
	    ENA_REGS_CONTROLLER_VERSION_MAJOR_VERSION_SHIFT;

	/*
	 * --- DEVICE_ATTRIBUTES via GET_FEATURE, response delivered INLINE. ---
	 *
	 * ena-com fetches DEVICE_ATTRIBUTES with control_buff_size == 0, so the
	 * descriptor carries no indirect control buffer (flags = 0, length = 0)
	 * and the device writes the dev_attr payload INLINE into the ACQ
	 * completion entry (ena_com.c:1046-1090, 1045-1046, 2336-2343). We pass a
	 * real completion entry to ena_admin_poll and read dev_attr back out of
	 * it; using a control buffer here would make the device write the payload
	 * into the ACQ slot we discard, so MAC/MTU would read as zero.
	 */
	memset(&cmd, 0, sizeof(cmd));
	gf->aq_common_descriptor.opcode = ENA_ADMIN_GET_FEATURE;
	gf->aq_common_descriptor.flags = 0;	/* no CTRL_DATA_INDIRECT */

	gf->control_buffer.length = 0;		/* inline response in the ACQ */

	gf->feat_common.feature_id = ENA_ADMIN_DEVICE_ATTRIBUTES;
	gf->feat_common.feature_version = 0;	/* current value */

	error = ena_admin_poll(sc, &cmd, &resp);
	if (error != 0) {
		printf("\n%s: GET_FEATURE(DEVICE_ATTRIBUTES) failed (%d)\n",
		    sc->sc_dev.dv_xname, error);
		return (error);
	}

	/*
	 * The completion entry carries the dev_attr payload inline; ena_admin_poll
	 * copied it into resp and its CQ-ring POSTREAD already ordered this read.
	 * ENA descriptors are little-endian; multi-byte fields must be converted.
	 */
	attr = &gr->u.dev_attr;

	memcpy(mac, attr->mac_addr, ETHER_ADDR_LEN);
	max_mtu = letoh32(attr->max_mtu);

	/*
	 * Store the device's max L2-payload MTU so ena_attach can advertise it as
	 * if_hardmtu and ena_set_mtu can bound jumbo requests. Floor at ETHERMTU
	 * so a bogus/zero read never leaves if_hardmtu below the 1500 default
	 * (which would make ether_ioctl reject every MTU, including 1500).
	 * Re-runs harmlessly on the recovery path.
	 */
	sc->sc_max_mtu = (max_mtu < ETHERMTU) ? ETHERMTU : max_mtu;

	/* Feature bitmap: which SET_FEATUREs the device honours (gates RSS). */
	sc->sc_supported_features = letoh32(attr->supported_features);

	/*
	 * Stash the MAC into sc_ac.ac_enaddr for use by ena_attach (Task 5).
	 * ether_ifattach reads ac_enaddr to fill the ifnet's link-layer address,
	 * so this must happen before if_attach + ether_ifattach are called.
	 */
	memcpy(sc->sc_ac.ac_enaddr, mac, ETHER_ADDR_LEN);

	/*
	 * Milestone line: this exact format is grepped over the serial console
	 * on real hardware (Task 12). Leading newline closes the ": <intrstr>"
	 * fragment ena_attach left open.
	 */
	printf("\n%s: ENA ver %u.%u, ctrl ver %u, MTU max %u, address %s\n",
	    sc->sc_dev.dv_xname, ena_ver_maj, ena_ver_min, ctrl_ver_val,
	    max_mtu, ether_sprintf(mac));

	/*
	 * --- STATELESS_OFFLOAD_CONFIG via GET_FEATURE, response INLINE. ---
	 *
	 * Second GET_FEATURE, same inline pattern as DEVICE_ATTRIBUTES above
	 * (control_buffer.length == 0). Cache which RX/TX checksum offloads the
	 * device advertises. This feature is OPTIONAL: a device that does not
	 * implement feature 11 returns an error, which we swallow (leaving all
	 * four flags 0) so attach does not abort. The DEVICE_ATTRIBUTES result
	 * read above is already stashed, so reusing cmd/resp here is safe.
	 */
	memset(&cmd, 0, sizeof(cmd));
	gf->aq_common_descriptor.opcode = ENA_ADMIN_GET_FEATURE;
	gf->aq_common_descriptor.flags = 0;
	gf->control_buffer.length = 0;		/* inline response, like dev_attr */
	gf->feat_common.feature_id = ENA_ADMIN_STATELESS_OFFLOAD_CONFIG;
	gf->feat_common.feature_version = 0;

	error = ena_admin_poll(sc, &cmd, &resp);
	if (error == 0) {
		uint32_t tx = letoh32(gr->u.offload.tx);
		uint32_t rxs = letoh32(gr->u.offload.rx_supported);

		sc->sc_tx_csum_l3 =
		    !!(tx & ENA_ADMIN_FEATURE_OFFLOAD_DESC_TX_L3_CSUM_IPV4_MASK);
		sc->sc_tx_csum_l4 =
		    !!(tx & ENA_ADMIN_FEATURE_OFFLOAD_DESC_TX_L4_IPV4_CSUM_PART_MASK);
		sc->sc_rx_csum_l3 =
		    !!(rxs & ENA_ADMIN_FEATURE_OFFLOAD_DESC_RX_L3_CSUM_IPV4_MASK);
		sc->sc_rx_csum_l4 =
		    !!(rxs & ENA_ADMIN_FEATURE_OFFLOAD_DESC_RX_L4_IPV4_CSUM_MASK);
	} else {
		/* Device lacks feature 11: leave all four 0. Do NOT fail attach. */
		error = 0;	/* swallow; offload is optional */
	}

	printf("%s: offload tx_l3=%u tx_l4=%u rx_l3=%u rx_l4=%u\n",
	    sc->sc_dev.dv_xname, sc->sc_tx_csum_l3, sc->sc_tx_csum_l4,
	    sc->sc_rx_csum_l3, sc->sc_rx_csum_l4);

	/*
	 * --- MAX_QUEUES_EXT via GET_FEATURE, response INLINE. ---
	 *
	 * The device's per-direction max IO queue counts and ring depths. Used to
	 * bound the host's queue-count (ena_attach) and depth (ena_{rx,tx}_create)
	 * requests so we never ask for more than the VF supports. Default to the
	 * compile-time caps so a device that does not advertise the feature (bit
	 * not set in supported_features) keeps the pre-Task-2 behaviour (no clamp).
	 */
	sc->sc_max_io_queues = ENA_MAX_IO_QUEUES;
	sc->sc_max_rx_depth = ENA_RX_QUEUE_DEPTH;
	sc->sc_max_tx_depth = ENA_TX_QUEUE_DEPTH;
	if (sc->sc_supported_features & (1U << ENA_ADMIN_MAX_QUEUES_EXT)) {
		memset(&cmd, 0, sizeof(cmd));
		gf->aq_common_descriptor.opcode = ENA_ADMIN_GET_FEATURE;
		gf->aq_common_descriptor.flags = 0;
		gf->control_buffer.length = 0;	/* inline response, like dev_attr */
		gf->feat_common.feature_id = ENA_ADMIN_MAX_QUEUES_EXT;
		gf->feat_common.feature_version = 1; /* ENA_FEATURE_MAX_QUEUE_EXT_VER */

		if (ena_admin_poll(sc, &cmd, &resp) == 0) {
			struct ena_admin_queue_ext_feature_fields *q =
			    &gr->u.maxq.f;
			uint32_t ioq, rxd, txd;

			ioq = MIN(MIN(letoh32(q->max_rx_sq_num),
			    letoh32(q->max_rx_cq_num)),
			    MIN(letoh32(q->max_tx_sq_num),
			    letoh32(q->max_tx_cq_num)));
			rxd = MIN(letoh32(q->max_rx_sq_depth),
			    letoh32(q->max_rx_cq_depth));
			txd = MIN(letoh32(q->max_tx_sq_depth),
			    letoh32(q->max_tx_cq_depth));
			if (ioq > 0)
				sc->sc_max_io_queues = ioq;
			if (rxd > 0)
				sc->sc_max_rx_depth = rxd;
			if (txd > 0)
				sc->sc_max_tx_depth = txd;
		} else
			error = 0;	/* optional; keep the defaults above */
	}
	printf("%s: device max %u IO queues, RX depth %u, TX depth %u\n",
	    sc->sc_dev.dv_xname, sc->sc_max_io_queues, sc->sc_max_rx_depth,
	    sc->sc_max_tx_depth);

	return (0);
}

/*
 * Asynchronous Event Notification Queue (AENQ) bring-up. Mirrors
 * ena_com_admin_init_aenq (ena_com.c:137) + ena_com_set_aenq_config
 * (ena_com.c:1609) + ena_com_admin_aenq_enable (ena_com.c:1597), collapsed to
 * the Phase 1 case (subscribe LINK_CHANGE + KEEP_ALIVE).
 *
 * Sequence:
 *   1. Init host consumer state. ena-com seeds head = q_depth and phase = 1
 *      (ena_com.c:153-154): the head doorbell is primed to q_depth to mark all
 *      entries initially available, and the device DMA-writes the first wrap of
 *      entries at phase 1.
 *   2. PREREAD|PREWRITE hand-off: the ring was alloc'd BUS_DMA_ZERO, so the CPU
 *      just wrote zeros (clearing every phase bit). Flush that zero fill so the
 *      device's first write starts from a known state, and prime the read side
 *      so the first POSTREAD-synced phase poll in ena_aenq_intr sees device
 *      writes, not stale cache lines.
 *   3. Program AENQ base lo/hi + caps(depth, entry size) registers.
 *   4. SET_FEATURE(AENQ_CONFIG) over the (still polled) admin queue to
 *      subscribe LINK_CHANGE + KEEP_ALIVE.
 *   5. Write the AENQ head doorbell = depth (ena_com.c:1606).
 *   6. Unmask the management interrupt (INTR_MASK = 0) so AENQ events fire.
 */

/*
 * ena_set_host_attr -- register the 4KB host-info page with the device.
 *
 * FreeBSD and Linux do this unconditionally right after admin-queue init,
 * before reading device attributes (ena_com_set_host_attributes /
 * ena_config_host_info). The device uses the host-info page to recognize a
 * real host driver; if it is never provided, the device's post-bring-up
 * validation trips and it enters DEV_STS FATAL_ERROR, after which it silently
 * drops admin commands such as CREATE_CQ. The page must be physically
 * contiguous (PAGE_SIZE) and is held for the device's lifetime.
 */
int
ena_set_host_attr(struct ena_softc *sc)
{
	struct ena_admin_aq_entry cmd;
	struct ena_admin_set_feat_cmd *sf =
	    (struct ena_admin_set_feat_cmd *)&cmd;
	struct ena_admin_host_info *hi;
	uint64_t pa;
	int error;

	/* Allocate the host-info page once; reused across resets. */
	if (sc->sc_host_attr_dma.edm_map == NULL) {
		if (ena_dmamem_alloc(sc, &sc->sc_host_attr_dma,
		    PAGE_SIZE, PAGE_SIZE) != 0) {
			printf("%s: host-attr page alloc failed\n",
			    sc->sc_dev.dv_xname);
			return (ENOMEM);
		}
	}

	hi = (struct ena_admin_host_info *)ENA_DMA_KVA(&sc->sc_host_attr_dma);
	memset(hi, 0, sizeof(*hi));
	hi->os_type = htole32(ENA_ADMIN_OS_LINUX);
	hi->ena_spec_version = htole16(ENA_ADMIN_HOST_INFO_SPEC_VERSION);
	hi->num_cpus = htole16(ncpus);

	/* Flush the filled page before the device DMA-reads it. */
	bus_dmamap_sync(sc->sc_dmat, ENA_DMA_MAP(&sc->sc_host_attr_dma), 0,
	    PAGE_SIZE, BUS_DMASYNC_PREWRITE);

	memset(&cmd, 0, sizeof(cmd));
	sf->aq_common_descriptor.opcode = ENA_ADMIN_SET_FEATURE;
	sf->aq_common_descriptor.flags = 0;	/* no control buffer */
	sf->feat_common.feature_id = ENA_ADMIN_HOST_ATTR_CONFIG;
	sf->feat_common.feature_version = 0;
	pa = ENA_DMA_DVA(&sc->sc_host_attr_dma);
	sf->u.host_attr.os_info_ba.mem_addr_low = htole32((uint32_t)pa);
	sf->u.host_attr.os_info_ba.mem_addr_high =
	    htole16((uint16_t)(pa >> 32));
	/* debug_ba / debug_area_size left zero: no debug area. */

	error = ena_admin_poll(sc, &cmd, NULL);
	if (error != 0)
		printf("%s: SET_FEATURE(HOST_ATTR_CONFIG) failed (%d)\n",
		    sc->sc_dev.dv_xname, error);
	return (error);
}

/*
 * Program the device's frame-size limit with SET_FEATURE(MTU). This is a pure
 * SET (no GET round-trip, unlike AENQ): the device validates received and
 * transmitted frames against this MTU, so it must be set BEFORE the IO queues
 * are created and kept in agreement with the RX buffer size (see ena_init).
 * The device's mtu field is the L2 PAYLOAD MTU (Ethernet header excluded), so
 * if_mtu is passed straight through with no header math (mirrors FreeBSD
 * ena_com_set_dev_mtu, which passes if_getmtu()/ifr_mtu unchanged).
 */
int
ena_set_mtu(struct ena_softc *sc, uint32_t mtu)
{
	struct ena_admin_aq_entry cmd;
	struct ena_admin_set_feat_cmd *sf =
	    (struct ena_admin_set_feat_cmd *)&cmd;
	int error;

	memset(&cmd, 0, sizeof(cmd));
	sf->aq_common_descriptor.opcode = ENA_ADMIN_SET_FEATURE;
	sf->aq_common_descriptor.flags = 0;	/* no control buffer */
	sf->feat_common.feature_id = ENA_ADMIN_MTU;
	sf->feat_common.feature_version = 0;
	sf->u.mtu.mtu = htole32(mtu);		/* L2 payload MTU (no Ethernet hdr) */

	error = ena_admin_poll(sc, &cmd, NULL);
	if (error != 0)
		printf("%s: SET_FEATURE(MTU=%u) failed (%d)\n",
		    sc->sc_dev.dv_xname, mtu, error);
	return (error);
}

/*
 * Configure receive-side scaling so the device hashes incoming flows across
 * the IO queues (each pinned to its own CPU in ena_attach). Three SET_FEATURE
 * commands, each carrying its bulk payload in the admin control buffer:
 *   - RSS_HASH_FUNCTION: select Toeplitz + the system stoeplitz key
 *   - RSS_HASH_INPUT:    hash on L3 src/dst + L4 src/dst ports
 *   - RSS_INDIRECTION_TABLE: 128 buckets round-robin'd over the queues, each
 *     entry the DEVICE RX CQ index (rxq_cq_idx from CREATE_CQ), NOT the host
 *     queue number.
 * The control-buffer flag (bit 2 of aq flags) survives ena_admin_poll, which
 * only stamps the phase bit. Any failure is non-fatal: leave sc_rss_active=0
 * and run with all RX on queue 0. Called from ena_init after the queues exist.
 */
int
ena_rss_config(struct ena_softc *sc)
{
	struct ena_admin_aq_entry cmd;
	struct ena_admin_set_feat_cmd *sf =
	    (struct ena_admin_set_feat_cmd *)&cmd;
	struct ena_admin_feature_rss_flow_hash_control *key;
	struct ena_admin_feature_rss_hash_control *hctrl;
	struct ena_admin_rss_ind_table_entry *ind;
	const bus_size_t indlen =
	    ENA_RX_RSS_TABLE_SIZE * sizeof(struct ena_admin_rss_ind_table_entry);
	uint64_t pa;
	unsigned int i;
	int n = 0, total = 0;

	/* Lazily allocate the three control-buffer DMA regions (held for life). */
	if (sc->sc_rss_key_dma.edm_map == NULL &&
	    ena_dmamem_alloc(sc, &sc->sc_rss_key_dma, sizeof(*key), 8) != 0)
		return (0);
	if (sc->sc_rss_hashctrl_dma.edm_map == NULL &&
	    ena_dmamem_alloc(sc, &sc->sc_rss_hashctrl_dma, sizeof(*hctrl), 8) != 0)
		return (0);
	if (sc->sc_rss_indtbl_dma.edm_map == NULL &&
	    ena_dmamem_alloc(sc, &sc->sc_rss_indtbl_dma, indlen, 8) != 0)
		return (0);

	/*
	 * Program only the RSS features the device lets the host set (its
	 * supported_features bitmap, read in ena_get_dev_attr). Many ENA VFs
	 * manage the hash function/input themselves -- a SET there returns
	 * UNSUPPORTED_OPCODE -- yet still spread RX with their own default hash;
	 * skip those and push only what is allowed (typically the indirection
	 * table). RX-to-CPU spread works either way.
	 */

	/* 1. Toeplitz hash function + the system RSS key (control buffer). */
	if (sc->sc_supported_features & (1U << ENA_ADMIN_RSS_HASH_FUNCTION)) {
		total++;
		key = (struct ena_admin_feature_rss_flow_hash_control *)
		    ENA_DMA_KVA(&sc->sc_rss_key_dma);
		memset(key, 0, sizeof(*key));
		key->key_parts = htole32(ENA_ADMIN_RSS_KEY_PARTS);
		stoeplitz_to_key(key->key, sizeof(key->key));
		bus_dmamap_sync(sc->sc_dmat, ENA_DMA_MAP(&sc->sc_rss_key_dma), 0,
		    sizeof(*key), BUS_DMASYNC_PREWRITE);

		memset(&cmd, 0, sizeof(cmd));
		sf->aq_common_descriptor.opcode = ENA_ADMIN_SET_FEATURE;
		sf->aq_common_descriptor.flags =
		    ENA_ADMIN_AQ_COMMON_DESC_CTRL_DATA_INDIRECT_MASK;
		sf->feat_common.feature_id = ENA_ADMIN_RSS_HASH_FUNCTION;
		sf->u.flow_hash_func.selected_func =
		    htole32(1U << ENA_ADMIN_TOEPLITZ);
		sf->control_buffer.length = htole32(sizeof(*key));
		pa = ENA_DMA_DVA(&sc->sc_rss_key_dma);
		sf->control_buffer.address.mem_addr_low = htole32((uint32_t)pa);
		sf->control_buffer.address.mem_addr_high =
		    htole16((uint16_t)(pa >> 32));
		if (ena_admin_poll(sc, &cmd, NULL) == 0)
			n++;
		else
			printf("%s: SET RSS hash function failed\n",
			    sc->sc_dev.dv_xname);
	}

	/* 2. Hash input: L3 src/dst + L4 src/dst for TCP/UDP v4/v6 (control buf). */
	if (sc->sc_supported_features & (1U << ENA_ADMIN_RSS_HASH_INPUT)) {
		total++;
		hctrl = (struct ena_admin_feature_rss_hash_control *)
		    ENA_DMA_KVA(&sc->sc_rss_hashctrl_dma);
		memset(hctrl, 0, sizeof(*hctrl));
		for (i = 0; i < ENA_ADMIN_RSS_PROTO_NUM; i++) {
			uint16_t f = 0;

			switch (i) {
			case ENA_ADMIN_RSS_TCP4:
			case ENA_ADMIN_RSS_UDP4:
			case ENA_ADMIN_RSS_TCP6:
			case ENA_ADMIN_RSS_UDP6:
				f = ENA_ADMIN_RSS_L3_SA | ENA_ADMIN_RSS_L3_DA |
				    ENA_ADMIN_RSS_L4_SP | ENA_ADMIN_RSS_L4_DP;
				break;
			case ENA_ADMIN_RSS_IP4:
			case ENA_ADMIN_RSS_IP6:
			case ENA_ADMIN_RSS_IP4_FRAG:
				f = ENA_ADMIN_RSS_L3_SA | ENA_ADMIN_RSS_L3_DA;
				break;
			}
			hctrl->selected_fields[i].fields = htole16(f);
		}
		bus_dmamap_sync(sc->sc_dmat,
		    ENA_DMA_MAP(&sc->sc_rss_hashctrl_dma), 0, sizeof(*hctrl),
		    BUS_DMASYNC_PREWRITE);

		memset(&cmd, 0, sizeof(cmd));
		sf->aq_common_descriptor.opcode = ENA_ADMIN_SET_FEATURE;
		sf->aq_common_descriptor.flags =
		    ENA_ADMIN_AQ_COMMON_DESC_CTRL_DATA_INDIRECT_MASK;
		sf->feat_common.feature_id = ENA_ADMIN_RSS_HASH_INPUT;
		sf->u.flow_hash_input.enabled_input_sort =
		    htole16(ENA_ADMIN_RSS_L3_SORT | ENA_ADMIN_RSS_L4_SORT);
		sf->control_buffer.length = htole32(sizeof(*hctrl));
		pa = ENA_DMA_DVA(&sc->sc_rss_hashctrl_dma);
		sf->control_buffer.address.mem_addr_low = htole32((uint32_t)pa);
		sf->control_buffer.address.mem_addr_high =
		    htole16((uint16_t)(pa >> 32));
		if (ena_admin_poll(sc, &cmd, NULL) == 0)
			n++;
		else
			printf("%s: SET RSS hash input failed\n",
			    sc->sc_dev.dv_xname);
	}

	/*
	 * 3. Indirection table: 128 buckets round-robin'd across the queues.
	 * Despite the field name, ena-com programs each entry's cq_idx with the
	 * RX SQ index (ena_com.c:1389 rss_ind_tbl[i].cq_idx = io_sq->idx, where
	 * io_sq->idx is the CREATE_SQ-returned sq_idx) -- NOT the CQ index. Using
	 * rxq_cq_idx steers ~half the buckets to the wrong queue on devices where
	 * the SQ and CQ indices differ.
	 */
	if (sc->sc_supported_features &
	    (1U << ENA_ADMIN_RSS_INDIRECTION_TABLE_CONFIG)) {
		total++;
		ind = (struct ena_admin_rss_ind_table_entry *)
		    ENA_DMA_KVA(&sc->sc_rss_indtbl_dma);
		for (i = 0; i < ENA_RX_RSS_TABLE_SIZE; i++) {
			ind[i].cq_idx =
			    htole16(sc->sc_rxq[i % sc->sc_nqueues].rxq_sq_idx);
			ind[i].reserved = 0;
		}
		bus_dmamap_sync(sc->sc_dmat,
		    ENA_DMA_MAP(&sc->sc_rss_indtbl_dma), 0, indlen,
		    BUS_DMASYNC_PREWRITE);

		memset(&cmd, 0, sizeof(cmd));
		sf->aq_common_descriptor.opcode = ENA_ADMIN_SET_FEATURE;
		sf->aq_common_descriptor.flags =
		    ENA_ADMIN_AQ_COMMON_DESC_CTRL_DATA_INDIRECT_MASK;
		sf->feat_common.feature_id =
		    ENA_ADMIN_RSS_INDIRECTION_TABLE_CONFIG;
		sf->u.ind_table.size = htole16(ENA_RX_RSS_TABLE_LOG_SIZE);
		sf->u.ind_table.inline_index = htole32(0xffffffff);
		sf->control_buffer.length = htole32((uint32_t)indlen);
		pa = ENA_DMA_DVA(&sc->sc_rss_indtbl_dma);
		sf->control_buffer.address.mem_addr_low = htole32((uint32_t)pa);
		sf->control_buffer.address.mem_addr_high =
		    htole16((uint16_t)(pa >> 32));
		if (ena_admin_poll(sc, &cmd, NULL) == 0)
			n++;
		else
			printf("%s: SET RSS indirection table failed\n",
			    sc->sc_dev.dv_xname);
	}

	if (total == 0)
		printf("%s: RSS managed by device (no host-settable features)\n",
		    sc->sc_dev.dv_xname);
	else
		printf("%s: RSS programmed %d/%d features across %u queues\n",
		    sc->sc_dev.dv_xname, n, total, sc->sc_nqueues);
	return (0);
}

int
ena_aenq_init(struct ena_softc *sc)
{
	struct ena_admin_aq_entry cmd;
	struct ena_admin_set_feat_cmd *sf =
	    (struct ena_admin_set_feat_cmd *)&cmd;
	struct ena_admin_get_feat_cmd *gf =
	    (struct ena_admin_get_feat_cmd *)&cmd;
	struct ena_admin_acq_entry resp;
	struct ena_admin_get_feat_resp *gr =
	    (struct ena_admin_get_feat_resp *)&resp;
	uint32_t groups;
	int error;

	/*
	 * The AENQ ring (base/caps) and host consumer state were already
	 * registered in ena_admin_init via ena_aenq_register(), as part of the
	 * admin-queue handshake (matching ena_com). Here we only subscribe groups
	 * and, at the end, unmask + ring the head doorbell to activate delivery.
	 */

	/*
	 * AENQ group subscription, mirroring ena_com_set_aenq_config
	 * (ena_com.c:1609): first GET_FEATURE(AENQ_CONFIG) to read the device's
	 * supported_groups, then SET only the intersection with the groups we
	 * handle. Enabling a group the device does not advertise makes it reject
	 * the SET with a non-SUCCESS status, so the mask is required, not
	 * optional. The GET response is delivered INLINE in the ACQ (like
	 * DEVICE_ATTRIBUTES), so flags = 0 / control_buffer.length = 0.
	 */
	memset(&cmd, 0, sizeof(cmd));
	gf->aq_common_descriptor.opcode = ENA_ADMIN_GET_FEATURE;
	gf->aq_common_descriptor.flags = 0;	/* no CTRL_DATA_INDIRECT */
	gf->control_buffer.length = 0;		/* inline response in the ACQ */
	gf->feat_common.feature_id = ENA_ADMIN_AENQ_CONFIG;
	gf->feat_common.feature_version = 0;

	error = ena_admin_poll(sc, &cmd, &resp);
	if (error != 0) {
		printf("\n%s: GET_FEATURE(AENQ_CONFIG) failed (%d)\n",
		    sc->sc_dev.dv_xname, error);
		return (error);
	}

	/*
	 * The GET_FEATURE(AENQ_CONFIG) response is an ena_admin_feature_aenq_desc
	 * { u32 supported_groups; u32 enabled_groups; } delivered inline; read the
	 * first word (supported_groups) from the get_feat_resp raw union.
	 */
	groups = letoh32(gr->u.raw[0]) &
	    ((1U << ENA_ADMIN_LINK_CHANGE) | (1U << ENA_ADMIN_KEEP_ALIVE) |
	     (1U << ENA_ADMIN_FATAL_ERROR) | (1U << ENA_ADMIN_WARNING));

	memset(&cmd, 0, sizeof(cmd));
	sf->aq_common_descriptor.opcode = ENA_ADMIN_SET_FEATURE;
	sf->aq_common_descriptor.flags = 0;	/* no control buffer */
	sf->feat_common.feature_id = ENA_ADMIN_AENQ_CONFIG;
	sf->feat_common.feature_version = 0;
	sf->u.aenq.enabled_groups = htole32(groups);

	error = ena_admin_poll(sc, &cmd, NULL);
	if (error != 0) {
		printf("\n%s: SET_FEATURE(AENQ_CONFIG) failed (%d)\n",
		    sc->sc_dev.dv_xname, error);
		return (error);
	}

	/*
	 * Record the groups we actually negotiated/SET (already masked by the
	 * device's supported_groups above). The keep-alive watchdog gates its
	 * staleness check on (sc_aenq_groups & (1U << ENA_ADMIN_KEEP_ALIVE)) so
	 * it never declares a device dead that simply never subscribed to the
	 * KEEP_ALIVE heartbeat.
	 */
	sc->sc_aenq_groups = groups;

	/*
	 * Unmask the management interrupt BEFORE ringing the AENQ head doorbell.
	 * Order matters: ena_com unmasks the admin/AENQ interrupt
	 * (ena_com_set_admin_polling_mode(false) -> INTR_MASK = 0, ena_com.c:1756)
	 * and only THEN rings the head doorbell (ena_com_admin_aenq_enable,
	 * ena_com.c:1597) -- both back to back in
	 * ena_enable_msix_and_set_admin_interrupts (ena_netdev.c:4487-4489).
	 * Ringing the head doorbell while the interrupt is still masked signals
	 * the device that the host is ready to receive AENQ events when it cannot
	 * actually deliver them; the device then enters DEV_STS FATAL_ERROR a few
	 * microseconds later (confirmed: the fault triggers on the head-doorbell
	 * write, and disappears once the unmask precedes it).
	 */
	ENA_REG_WR32(sc, ENA_REGS_INTR_MASK_OFF, 0);

	/*
	 * Prime the AENQ head doorbell to depth: this marks all ring entries as
	 * initially available to the device (ena_com.c:1603-1606). Use the
	 * doorbell accessor so the post-store write barrier orders it after the
	 * register programming above.
	 */
	ENA_REG_WR32_DB(sc, ENA_REGS_AENQ_HEAD_DB_OFF, sc->sc_aenq_head);

	return (0);
}

/*
 * AENQ service routine, called from the management interrupt handler. Mirrors
 * ena_com_aenq_intr_handler (ena_com.c:2444).
 *
 * Walks the ring from the host consumer head, processing every entry whose
 * PHASE flag matches the host's current phase, dispatching by group, then
 * advancing the head and ringing the AENQ head doorbell. Bounded by the ring
 * depth: at most q_depth entries can be owned by the host at once, so a single
 * full pass drains everything the device produced.
 *
 * Per-sync audit (every bus_dmamap_sync, direction, why):
 *   AENQ POSTREAD (before the phase-bit walk): the device DMA-wrote the events;
 *     refresh/invalidate the CPU view so the phase-bit reads and the entry
 *     payloads we read observe device memory, not stale cache lines. The whole
 *     ring is synced once up front (single-segment, cheap) covering every entry
 *     the loop may read this pass. Pairs with the device's event writes.
 *   AENQ PREREAD (after the walk, before the doorbell): re-prime the read side
 *     for the slots we just consumed so the NEXT interrupt's POSTREAD observes
 *     the device's subsequent writes into them. Must precede the head doorbell,
 *     since the doorbell hands those slots back to the device.
 *
 * Phase logic: head is monotonic; masked_head = head & (depth-1) is the ring
 * index. An entry is host-owned when its PHASE flag equals the host phase. On
 * wrap (masked_head reaches depth) the host phase flips, matching the device,
 * which toggles the phase bit each time it laps the ring. The head doorbell is
 * written with the new monotonic head only if at least one entry was processed
 * (ena_com.c:2494-2500).
 */
void
ena_aenq_intr(struct ena_softc *sc)
{
	struct ena_admin_aenq_entry *ring, *e;
	struct ena_admin_aenq_link_change_desc *lc;
	uint16_t qmask, masked_head, processed;
	uint8_t phase;
	uint16_t group;
	int link_changed = 0;

	mtx_enter(&sc->sc_aenq_mtx);

	ring = (struct ena_admin_aenq_entry *)ENA_DMA_KVA(&sc->sc_aenq_dma);
	qmask = sc->sc_aenq_depth - 1;
	masked_head = sc->sc_aenq_head & qmask;
	phase = sc->sc_aenq_phase;
	processed = 0;

	/*
	 * POSTREAD the whole ring once before the walk: order the reads of the
	 * phase bits and entry payloads after the device's DMA writes.
	 */
	bus_dmamap_sync(sc->sc_dmat, ENA_DMA_MAP(&sc->sc_aenq_dma), 0,
	    ENA_DMA_LEN(&sc->sc_aenq_dma), BUS_DMASYNC_POSTREAD);

	e = &ring[masked_head];
	while ((e->aenq_common_desc.flags &
	    ENA_ADMIN_AENQ_COMMON_DESC_PHASE_MASK) == phase) {
		/*
		 * The phase bit matches: the device finished writing this entry
		 * (it writes the body before the phase flag). The up-front
		 * POSTREAD ordered the whole slot, so the payload is valid.
		 * group is little-endian on the wire.
		 */
		group = letoh16(e->aenq_common_desc.group);

		switch (group) {
		case ENA_ADMIN_LINK_CHANGE:
			lc = (struct ena_admin_aenq_link_change_desc *)e;
			sc->sc_link_up =
			    (letoh32(lc->flags) &
			    ENA_ADMIN_AENQ_LINK_CHANGE_DESC_LINK_STATUS_MASK)
			    ? 1 : 0;
			/*
			 * sc_link_up is now cached; the actual ifnet push is
			 * deferred to after the drain loop and funnelled through
			 * ena_link_state(sc) (which sets if_link_state and calls
			 * if_link_state_change). Mirrors vio_link_state
			 * (if_vio.c:963-978).
			 */
			link_changed = 1;
			printf("%s: link %s\n", sc->sc_dev.dv_xname,
			    sc->sc_link_up ? "UP" : "DOWN");
			break;
		case ENA_ADMIN_KEEP_ALIVE:
			/*
			 * Device heartbeat. Stamp the monotonic uptime so the
			 * watchdog tick (ena_tick) can detect staleness. This
			 * runs in the mgmt MSI-X ISR; getuptime() is sleep-free
			 * and the single aligned volatile write needs no lock.
			 */
			sc->sc_keepalive_last = getuptime();
			break;
		case ENA_ADMIN_FATAL_ERROR:
			/*
			 * Device-reported fatal error. Flag a reset; do NOT
			 * task_add here -- the 1 Hz tick (ena_tick) is the
			 * single enqueue point, so it picks this up on its next
			 * pass (this runs in the mgmt MSI-X ISR, which must stay
			 * sleep/reset-free).
			 */
			printf("%s: AENQ FATAL_ERROR event\n",
			    sc->sc_dev.dv_xname);
			SET(sc->sc_flags, ENA_FLAG_TRIGGER_RESET);
			break;
		default:
			/* WARNING / others: log and continue. */
			printf("%s: AENQ event group %u\n",
			    sc->sc_dev.dv_xname, group);
			break;
		}

		/* Advance to the next entry; flip phase on ring wrap. */
		masked_head++;
		processed++;
		if (masked_head == sc->sc_aenq_depth) {
			masked_head = 0;
			phase ^= 1;
		}
		e = &ring[masked_head];
	}

	/* Nothing consumed: leave head/phase and the doorbell untouched. */
	if (processed == 0) {
		mtx_leave(&sc->sc_aenq_mtx);
		return;
	}

	sc->sc_aenq_head += processed;
	sc->sc_aenq_phase = phase;

	/*
	 * PREREAD the ring before handing the consumed slots back: re-prime the
	 * read side so the next interrupt's POSTREAD sees the device's future
	 * writes into these slots. Must precede the head doorbell.
	 */
	bus_dmamap_sync(sc->sc_dmat, ENA_DMA_MAP(&sc->sc_aenq_dma), 0,
	    ENA_DMA_LEN(&sc->sc_aenq_dma), BUS_DMASYNC_PREREAD);

	/* Ring the AENQ head doorbell with the new monotonic head. */
	ENA_REG_WR32_DB(sc, ENA_REGS_AENQ_HEAD_DB_OFF, sc->sc_aenq_head);

	mtx_leave(&sc->sc_aenq_mtx);

	/*
	 * Notify the ifnet layer of a link-state change after the drain loop (and
	 * outside the mutex), funnelled through ena_link_state (which only fires
	 * if_link_state_change -- itself task_add-deferred -- when the state
	 * actually changed).
	 */
	if (link_changed)
		ena_link_state(sc);
}

/*
 * Management interrupt handler (MSI-X vector 0: ENA_MGMNT_IRQ_IDX).
 * Services the Asynchronous Event Notification Queue (AENQ). The admin
 * completion queue is polled in Phase 0/1, so only the AENQ is serviced here.
 *
 * Returns 1 (interrupt claimed) to satisfy the interrupt framework.
 */
int
ena_intr_mgmt(void *xsc)
{
	struct ena_softc *sc = xsc;

	ena_aenq_intr(sc);
	return (1);
}

/*
 * IO interrupt handler (MSI-X vector 1: ENA_IO_IRQ_FIRST_IDX).
 * Services RX and TX completion queues. The IO queues are not yet
 * created in Phase 1; real work is wired in Tasks 6 (RX) and 7 (TX).
 * The msix_vector field in the CREATE_CQ admin command (ena_com.c:1470)
 * binds this vector index to each IO CQ at queue-create time.
 *
 * Returns 1 (interrupt claimed) to satisfy the interrupt framework.
 */
int
ena_intr_io_queue(void *arg)
{
	struct ena_queue *q = arg;
	struct ena_softc *sc = q->sc;
	struct ena_txq *txq = &sc->sc_txq[q->idx];
	struct ena_rxq *rxq = &sc->sc_rxq[q->idx];

	/*
	 * This IO MSI-X vector services exactly one RX/TX queue pair. Drain TX
	 * completions first (reclaim mbufs / free SQ space) so a subsequent
	 * ena_start has room, then service RX. Each helper guards on its own
	 * "created" flag, so a spurious interrupt before bring-up (or after
	 * teardown) touches no freed memory.
	 */
	if (txq->txq_created)
		ena_txeof(txq);
	ena_rx_intr(rxq);
	return (1);
}

int
ena_detach(struct device *self, int flags)
{
	struct ena_softc *sc = (struct ena_softc *)self;
	struct ifnet *ifp = &sc->sc_ac.ac_if;
	unsigned int i;

	/* Nothing mapped: attach failed before the BAR was mapped. */
	if (sc->sc_mems == 0)
		return (0);

	/*
	 * CORRECTION 2: drain the watchdog tick and the reset task BEFORE any
	 * other teardown, and WITHOUT holding NET_LOCK. ena_reset_task takes
	 * NET_LOCK, so holding it across taskq_barrier here would self-deadlock;
	 * config_detach does not hold NET_LOCK and this routine does not take it
	 * (verified: ena_detach calls ena_stop/ena_reset bare, never NET_LOCK),
	 * so the barrier is safe. Setting DYING first makes ena_tick stop
	 * enqueueing and makes any already-running reset task abort early. This
	 * MUST complete before ena_stop/ena_reset/DMA-free below, or a concurrent
	 * reset task would touch freed state.
	 */
	SET(sc->sc_flags, ENA_FLAG_DYING);
	timeout_del_barrier(&sc->sc_wd_tick);	/* cancel + wait for a running tick */
	task_del(systq, &sc->sc_reset_task);	/* dequeue a pending run */
	taskq_barrier(systq);			/* wait for a running reset task */

	/*
	 * Reverse of ena_attach. Ordering matters:
	 *   0. Detach the ethernet interface first: ether_ifdetach + if_detach
	 *      remove the ifnet from the kernel's view and drain any pending
	 *      work before we tear down the device and interrupt handlers.
	 *      ifmedia_delete_instance releases all media entries added by
	 *      ifmedia_add. This must precede the interrupt disestablish so
	 *      the ioctl path cannot race with teardown.
	 *   1. Mask the management interrupt: ena_aenq_init unmasked it so AENQ
	 *      events fire, so close that window before tearing down the AENQ
	 *      ring (the ISR must not service freed memory).
	 *   2. Quiesce the device: reset it so it stops DMAing into the admin
	 *      and AENQ rings before we free them. ena_reset clears the queue
	 *      base addresses' validity (the device drops the rings across reset)
	 *      and leaves the device idle. This MUST precede ena_dmamem_free so no
	 *      DMA is in flight against memory being freed.
	 *   3. Disestablish the interrupt before freeing DMA memory — the ISR
	 *      must not fire against freed buffers.
	 *   4. Free AENQ ring, then admin CQ ring, then admin SQ ring (reverse
	 *      alloc order).
	 *   5. Unmap BAR last.
	 */

	/*
	 * Step 0: detach the ifnet before any hardware teardown. Guard on
	 * IFF_BROADCAST so a partial attach (attach failed before if_attach)
	 * does not call into the ifnet layer with an uninitialised ifnet.
	 * Use the IFF_BROADCAST test as a proxy for "if_attach was called" —
	 * we set IFF_BROADCAST|IFF_SIMPLEX|IFF_MULTICAST unconditionally
	 * in attach, and the kernel zeroes the softc, so this is zero only
	 * if ena_attach failed before the ifnet block.
	 */
	if (ifp->if_flags & IFF_BROADCAST) {
		/*
		 * If the interface is up, take it down first: ena_stop destroys
		 * the IO RX queue (DESTROY_SQ/CQ over the still-live admin path)
		 * and frees its clusters/rings. This MUST precede ena_reset,
		 * which drops the queues device-side, and the admin-ring free.
		 */
		if (ISSET(ifp->if_flags, IFF_RUNNING))
			ena_stop(sc);
		ether_ifdetach(ifp);
		if_detach(ifp);
		ifmedia_delete_instance(&sc->sc_media, IFM_INST_ANY);
	}

	ENA_REG_WR32(sc, ENA_REGS_INTR_MASK_OFF, ENA_REGS_ADMIN_INTR_MASK);
	ena_reset(sc);

	/*
	 * Disestablish in reverse establish order: IO vector first, then
	 * management. Guards on NULL so partial-attach detach is safe.
	 */
	for (i = 0; i < sc->sc_nqueues_alloc; i++) {
		if (sc->sc_queues[i].tag != NULL) {
			pci_intr_disestablish(sc->sc_pc, sc->sc_queues[i].tag);
			sc->sc_queues[i].tag = NULL;
		}
	}
	free(sc->sc_queues, M_DEVBUF,
	    sc->sc_nqueues_alloc * sizeof(struct ena_queue));
	free(sc->sc_rxq, M_DEVBUF, sc->sc_nqueues_alloc * sizeof(struct ena_rxq));
	free(sc->sc_txq, M_DEVBUF, sc->sc_nqueues_alloc * sizeof(struct ena_txq));
	if (sc->sc_intrmap != NULL)
		intrmap_destroy(sc->sc_intrmap);
	if (sc->sc_mgmt_ih != NULL) {
		pci_intr_disestablish(sc->sc_pc, sc->sc_mgmt_ih);
		sc->sc_mgmt_ih = NULL;
	}

	/*
	 * Closing sync before free: assert that any in-flight DMA the device
	 * may have issued is complete from the CPU's perspective before the
	 * memory is returned. Mirrors if_mcx.c teardown ordering. Free in
	 * reverse allocation order: AENQ (allocated last), then admin CQ, then
	 * admin SQ. The AENQ ring was last device->host DMA target; ena_reset
	 * above already quiesced the device, so this closing POSTREAD|POSTWRITE
	 * just orders any final write before the free.
	 */
	bus_dmamap_sync(sc->sc_dmat, ENA_DMA_MAP(&sc->sc_aenq_dma), 0,
	    ENA_DMA_LEN(&sc->sc_aenq_dma),
	    BUS_DMASYNC_POSTREAD | BUS_DMASYNC_POSTWRITE);
	ena_dmamem_free(sc, &sc->sc_aenq_dma);
	ena_dmamem_free(sc, &sc->sc_mmio_resp_dma);
	if (sc->sc_host_attr_dma.edm_map != NULL)
		ena_dmamem_free(sc, &sc->sc_host_attr_dma);
	if (sc->sc_rss_key_dma.edm_map != NULL)
		ena_dmamem_free(sc, &sc->sc_rss_key_dma);
	if (sc->sc_rss_hashctrl_dma.edm_map != NULL)
		ena_dmamem_free(sc, &sc->sc_rss_hashctrl_dma);
	if (sc->sc_rss_indtbl_dma.edm_map != NULL)
		ena_dmamem_free(sc, &sc->sc_rss_indtbl_dma);
	bus_dmamap_sync(sc->sc_dmat, ENA_DMA_MAP(&sc->sc_acq_dma), 0,
	    ENA_DMA_LEN(&sc->sc_acq_dma),
	    BUS_DMASYNC_POSTREAD | BUS_DMASYNC_POSTWRITE);
	ena_dmamem_free(sc, &sc->sc_acq_dma);
	bus_dmamap_sync(sc->sc_dmat, ENA_DMA_MAP(&sc->sc_aq_dma), 0,
	    ENA_DMA_LEN(&sc->sc_aq_dma),
	    BUS_DMASYNC_POSTREAD | BUS_DMASYNC_POSTWRITE);
	ena_dmamem_free(sc, &sc->sc_aq_dma);

	/* Unmap the BAR2 LLQ window (if it was mapped for LLQ TX). */
	if (sc->sc_llq_s != 0) {
		bus_space_unmap(sc->sc_llq_t, sc->sc_llq_h, sc->sc_llq_s);
		sc->sc_llq_s = 0;
	}

	if (sc->sc_mems != 0) {
		bus_space_unmap(sc->sc_memt, sc->sc_memh, sc->sc_mems);
		sc->sc_mems = 0;
	}

	return (0);
}

/*
 * Re-arm an IO completion queue's MSI-X interrupt.
 *
 * ENA masks an IO CQ's MSI-X after it fires; the host re-arms it by writing an
 * intr_reg with INTR_UNMASK set to the per-CQ unmask BAR offset the device
 * returned in the CREATE_CQ response (cq_interrupt_unmask_register_offset).
 * This is NOT the global ENA_REGS_INTR_MASK_OFF register (that is the admin/
 * management mask); each IO CQ has its own unmask register.
 *
 * Phase 1 uses no interrupt moderation, so the RX/TX delay fields are zero and
 * intr_control is just the UNMASK bit. Mirrors ena_com_update_intr_reg
 * (ena_com.h:1351, unmask=true, delays 0) + ena_com_unmask_intr
 * (ena_eth_com.h:91, writes intr_control to io_cq->unmask_reg). The register is
 * little-endian like the rest of the BAR window, so a plain store is correct
 * (no byte swap).
 *
 * The store goes through ENA_REG_WR32_PREDB, not plain ENA_REG_WR32: ena-com's
 * ena_com_unmask_intr writes this register via ENA_REG_WRITE32, which carries a
 * pre-store wmb() (ena_plat.h:403-407). That barrier orders the preceding CQ
 * consume (bus_dmamap_sync POSTREAD in ena_rxeof/ena_txeof) ahead of the unmask
 * store; this is the sole re-arm of the shared IO MSI-X vector and the last
 * device access in the handler, with no trailing read, so the plain (barrierless)
 * accessor left the re-arm unordered against the consume on weakly-ordered arm64.
 */
void
ena_cq_unmask(struct ena_softc *sc, bus_size_t unmask_off)
{
	uint32_t intr_reg;

	/* delays 0 (no moderation) + INTR_UNMASK. */
	intr_reg = ENA_ETH_IO_INTR_REG_INTR_UNMASK_MASK;
	ENA_REG_WR32_PREDB(sc, unmask_off, intr_reg);
}

/*
 * Create the IO RX queue: the RX CQ first, then the RX SQ bound to it.
 *
 * ena-com creates the CQ before the SQ (ena_com_create_io_queue, ena_com.c:
 * 2200-2258: ena_com_create_io_cq then ena_com_create_io_sq), because the SQ
 * create command carries the CQ index it completes into. We mirror that order
 * and the exact command fields:
 *
 *   CREATE_CQ (ena_com_create_io_cq, ena_com.c:1453-1504):
 *     cq_caps_2 = (cdesc_entry_size_in_bytes / 4)  -- 16/4 == 4 words
 *     cq_caps_1 = INTERRUPT_MODE_ENABLED           -- IO CQ raises an interrupt
 *     msix_vector = IO vector index (1: ENA_IO_IRQ_FIRST_IDX, reserved Task 3)
 *     cq_depth = ENA_RX_QUEUE_DEPTH
 *     cq_ba = RX CQ ring physical base
 *   Response: cq_idx, cq_head_db_register_offset (head doorbell BAR offset).
 *
 *   CREATE_SQ (ena_com_create_io_sq, ena_com.c:1302-1369):
 *     sq_identity   = SQ_DIRECTION_RX << DIRECTION_SHIFT
 *     sq_caps_2     = PLACEMENT_POLICY_HOST | (COMPLETION_POLICY_DESC << shift)
 *     sq_caps_3     = IS_PHYSICALLY_CONTIGUOUS
 *     cq_idx        = the CQ index the device just returned
 *     sq_depth      = ENA_RX_QUEUE_DEPTH
 *     sq_ba         = RX SQ ring physical base (host placement)
 *   Response: sq_idx, sq_doorbell_offset (SQ tail doorbell BAR offset).
 *
 * The device-returned doorbell offsets are little-endian; letoh32 before use.
 * Both DMA rings were allocated BUS_DMA_ZERO and are handed to the device with
 * a sync below (see the per-sync audit on each direction).
 */
int
ena_rx_create(struct ena_softc *sc, unsigned int idx)
{
	struct ena_rxq *rxq = &sc->sc_rxq[idx];
	struct ena_admin_aq_entry cmd;
	struct ena_admin_acq_entry resp;
	struct ena_admin_aq_create_cq_cmd *cc =
	    (struct ena_admin_aq_create_cq_cmd *)&cmd;
	struct ena_admin_acq_create_cq_resp_desc *cr =
	    (struct ena_admin_acq_create_cq_resp_desc *)&resp;
	struct ena_admin_aq_create_sq_cmd *sc_cmd =
	    (struct ena_admin_aq_create_sq_cmd *)&cmd;
	struct ena_admin_acq_create_sq_resp_desc *sr =
	    (struct ena_admin_acq_create_sq_resp_desc *)&resp;
	uint64_t cq_pa, sq_pa;
	int error = ENOMEM;
	uint16_t i;

	rxq->rxq_depth = MIN(ENA_RX_QUEUE_DEPTH, sc->sc_max_rx_depth);
	rxq->rxq_sq_tail = 0;
	rxq->rxq_sq_phase = 1;	/* first SQ descriptor is written phase 1 */
	rxq->rxq_cq_head = 0;
	rxq->rxq_cq_phase = 1;	/* first CQ completion is read at phase 1 */

	/* Allocate the RX SQ (host->device) and RX CQ (device->host) rings. */
	if (ena_dmamem_alloc(sc, &rxq->rxq_sq_dma,
	    ENA_RX_SQ_SIZE(rxq->rxq_depth), PAGE_SIZE) != 0) {
		printf("%s: can't allocate RX SQ ring\n", sc->sc_dev.dv_xname);
		return (ENOMEM);
	}
	if (ena_dmamem_alloc(sc, &rxq->rxq_cq_dma,
	    ENA_RX_CQ_SIZE(rxq->rxq_depth), PAGE_SIZE) != 0) {
		printf("%s: can't allocate RX CQ ring\n", sc->sc_dev.dv_xname);
		goto free_sq;
	}

	/* Per-slot bookkeeping + DMA maps for the data clusters. */
	rxq->rxq_slots = mallocarray(rxq->rxq_depth,
	    sizeof(struct ena_rx_slot), M_DEVBUF, M_WAITOK | M_ZERO);
	/*
	 * Size each slot's DMA map for the current MTU's buffer. A map's maxsize
	 * is fixed at create time, so it must cover the largest cluster ena_rx_fill
	 * will load. ena_init sets sc_rx_buf_size (floored at ENA_RX_BUF_SIZE)
	 * before calling ena_rx_create on every bring-up, and ena_stop frees these
	 * maps on every down, so a down/up bounce recreates them at the new size.
	 */
	for (i = 0; i < rxq->rxq_depth; i++) {
		if (bus_dmamap_create(sc->sc_dmat, sc->sc_rx_buf_size, 1,
		    sc->sc_rx_buf_size, 0, BUS_DMA_WAITOK | BUS_DMA_64BIT,
		    &rxq->rxq_slots[i].rxs_map) != 0) {
			printf("%s: can't create RX DMA map\n",
			    sc->sc_dev.dv_xname);
			goto free_maps;
		}
	}

	/*
	 * Initial ownership hand-off to the device, mirroring the admin-ring
	 * hand-off in ena_attach:
	 *   rxq_cq_dma (RX CQ, device->host): PREREAD|PREWRITE -- flush the
	 *     zero fill (clears phase bits) and prime the read side so the first
	 *     POSTREAD-synced phase poll in ena_rxeof sees device writes.
	 *   rxq_sq_dma (RX SQ, host->device): PREWRITE in ena_rx_fill before the
	 *     first doorbell; nothing to flush here yet (descriptors are written
	 *     by ena_rx_fill below).
	 */
	bus_dmamap_sync(sc->sc_dmat, ENA_DMA_MAP(&rxq->rxq_cq_dma), 0,
	    ENA_DMA_LEN(&rxq->rxq_cq_dma),
	    BUS_DMASYNC_PREREAD | BUS_DMASYNC_PREWRITE);

	/* --- CREATE_CQ: must precede CREATE_SQ (SQ references the CQ idx). --- */
	cq_pa = ENA_DMA_DVA(&rxq->rxq_cq_dma);
	memset(&cmd, 0, sizeof(cmd));
	cc->aq_common_descriptor.opcode = ENA_ADMIN_CREATE_CQ;
	cc->cq_caps_2 = (ENA_RX_CDESC_SIZE / 4) &
	    ENA_ADMIN_AQ_CREATE_CQ_CMD_CQ_ENTRY_SIZE_WORDS_MASK;
	cc->cq_caps_1 = ENA_ADMIN_AQ_CREATE_CQ_CMD_INTERRUPT_MODE_ENABLED_MASK;
	cc->msix_vector = htole32(idx + 1);	/* per-queue IO vector (vec 0 = mgmt) */
	cc->cq_depth = htole16(rxq->rxq_depth);
	cc->cq_ba.mem_addr_low = htole32((uint32_t)cq_pa);
	cc->cq_ba.mem_addr_high = htole16((uint16_t)(cq_pa >> 32));

	error = ena_admin_poll(sc, &cmd, &resp);
	if (error != 0) {
		printf("%s: CREATE_CQ failed (%d)\n", sc->sc_dev.dv_xname,
		    error);
		goto free_maps;
	}
	rxq->rxq_cq_idx = letoh16(cr->cq_idx);
	rxq->rxq_cq_db = letoh32(cr->cq_head_db_register_offset);
	rxq->rxq_unmask_off = letoh32(cr->cq_interrupt_unmask_register_offset);

	/* --- CREATE_SQ (RX direction), bound to the CQ just created. --- */
	sq_pa = ENA_DMA_DVA(&rxq->rxq_sq_dma);
	memset(&cmd, 0, sizeof(cmd));
	sc_cmd->aq_common_descriptor.opcode = ENA_ADMIN_CREATE_SQ;
	sc_cmd->sq_identity =
	    (ENA_ADMIN_SQ_DIRECTION_RX <<
	    ENA_ADMIN_AQ_CREATE_SQ_CMD_SQ_DIRECTION_SHIFT) &
	    ENA_ADMIN_AQ_CREATE_SQ_CMD_SQ_DIRECTION_MASK;
	sc_cmd->sq_caps_2 =
	    (ENA_ADMIN_PLACEMENT_POLICY_HOST &
	    ENA_ADMIN_AQ_CREATE_SQ_CMD_PLACEMENT_POLICY_MASK) |
	    ((ENA_ADMIN_COMPLETION_POLICY_DESC <<
	    ENA_ADMIN_AQ_CREATE_SQ_CMD_COMPLETION_POLICY_SHIFT) &
	    ENA_ADMIN_AQ_CREATE_SQ_CMD_COMPLETION_POLICY_MASK);
	sc_cmd->sq_caps_3 =
	    ENA_ADMIN_AQ_CREATE_SQ_CMD_IS_PHYSICALLY_CONTIGUOUS_MASK;
	sc_cmd->cq_idx = htole16(rxq->rxq_cq_idx);
	sc_cmd->sq_depth = htole16(rxq->rxq_depth);
	sc_cmd->sq_ba.mem_addr_low = htole32((uint32_t)sq_pa);
	sc_cmd->sq_ba.mem_addr_high = htole16((uint16_t)(sq_pa >> 32));

	error = ena_admin_poll(sc, &cmd, &resp);
	if (error != 0) {
		printf("%s: CREATE_SQ failed (%d)\n", sc->sc_dev.dv_xname,
		    error);
		goto destroy_cq;
	}
	rxq->rxq_sq_idx = letoh16(sr->sq_idx);
	rxq->rxq_sq_db = letoh32(sr->sq_doorbell_offset);

	/*
	 * Arm the IO CQ interrupt once after create. The device masks the IO
	 * MSI-X after each fire; without this initial unmask the very first RX
	 * interrupt never arrives. Mirrors ena_unmask_interrupt in ena_up
	 * (ena_netdev.c) which unmasks each IO CQ after the queues are created.
	 */
	ena_cq_unmask(sc, rxq->rxq_unmask_off);

	rxq->rxq_created = 1;
	return (0);

destroy_cq:
	{
		struct ena_admin_aq_destroy_cq_cmd *dc =
		    (struct ena_admin_aq_destroy_cq_cmd *)&cmd;

		memset(&cmd, 0, sizeof(cmd));
		dc->aq_common_descriptor.opcode = ENA_ADMIN_DESTROY_CQ;
		dc->cq_idx = htole16(rxq->rxq_cq_idx);
		(void)ena_admin_poll(sc, &cmd, NULL);
	}
free_maps:
	for (i = 0; i < rxq->rxq_depth; i++) {
		if (rxq->rxq_slots[i].rxs_map != NULL)
			bus_dmamap_destroy(sc->sc_dmat,
			    rxq->rxq_slots[i].rxs_map);
	}
	free(rxq->rxq_slots, M_DEVBUF,
	    rxq->rxq_depth * sizeof(struct ena_rx_slot));
	rxq->rxq_slots = NULL;
	ena_dmamem_free(sc, &rxq->rxq_cq_dma);
free_sq:
	ena_dmamem_free(sc, &rxq->rxq_sq_dma);
	return (error);
}

/*
 * Tear down the IO RX queue. Destroy the SQ first, then the CQ (reverse of
 * create), mirroring ena_com_destroy_io_queue (ena_com.c:2260-2278:
 * ena_com_destroy_io_sq then ena_com_destroy_io_cq). The device stops DMAing
 * into the rings once both are destroyed; only then do we free the clusters,
 * maps and rings.
 */
void
ena_rx_destroy(struct ena_softc *sc, unsigned int idx)
{
	struct ena_rxq *rxq = &sc->sc_rxq[idx];
	struct ena_admin_aq_entry cmd;
	struct ena_admin_aq_destroy_sq_cmd *ds =
	    (struct ena_admin_aq_destroy_sq_cmd *)&cmd;
	struct ena_admin_aq_destroy_cq_cmd *dc =
	    (struct ena_admin_aq_destroy_cq_cmd *)&cmd;
	uint16_t i;

	if (!rxq->rxq_created)
		return;

	/* DESTROY_SQ (RX direction) first. */
	memset(&cmd, 0, sizeof(cmd));
	ds->aq_common_descriptor.opcode = ENA_ADMIN_DESTROY_SQ;
	ds->sq.sq_idx = htole16(rxq->rxq_sq_idx);
	ds->sq.sq_identity = (ENA_ADMIN_SQ_DIRECTION_RX <<
	    ENA_ADMIN_SQ_SQ_DIRECTION_SHIFT) & ENA_ADMIN_SQ_SQ_DIRECTION_MASK;
	if (ena_admin_poll(sc, &cmd, NULL) != 0)
		printf("%s: DESTROY_SQ failed\n", sc->sc_dev.dv_xname);

	/* DESTROY_CQ second. */
	memset(&cmd, 0, sizeof(cmd));
	dc->aq_common_descriptor.opcode = ENA_ADMIN_DESTROY_CQ;
	dc->cq_idx = htole16(rxq->rxq_cq_idx);
	if (ena_admin_poll(sc, &cmd, NULL) != 0)
		printf("%s: DESTROY_CQ failed\n", sc->sc_dev.dv_xname);

	rxq->rxq_created = 0;

	/*
	 * The device has dropped both rings; reclaim the in-flight clusters.
	 * Closing sync (POSTREAD) before unload orders any final device write
	 * into the buffer before the CPU frees it.
	 */
	for (i = 0; i < rxq->rxq_depth; i++) {
		struct ena_rx_slot *rxs = &rxq->rxq_slots[i];

		if (rxs->rxs_m != NULL) {
			bus_dmamap_sync(sc->sc_dmat, rxs->rxs_map, 0,
			    rxs->rxs_map->dm_mapsize, BUS_DMASYNC_POSTREAD);
			bus_dmamap_unload(sc->sc_dmat, rxs->rxs_map);
			m_freem(rxs->rxs_m);
			rxs->rxs_m = NULL;
		}
		bus_dmamap_destroy(sc->sc_dmat, rxs->rxs_map);
	}
	free(rxq->rxq_slots, M_DEVBUF,
	    rxq->rxq_depth * sizeof(struct ena_rx_slot));
	rxq->rxq_slots = NULL;

	ena_dmamem_free(sc, &rxq->rxq_cq_dma);
	ena_dmamem_free(sc, &rxq->rxq_sq_dma);
}

/*
 * Fill free RX SQ slots with mbuf clusters and hand them to the device.
 *
 * Mirrors mcx_rx_fill_slots (if_mcx.c:6735) + ena_com_add_single_rx_desc
 * (ena_eth_com.c:646). The if_rxr ring caps how many buffers we keep posted;
 * if_rxr_get returns how many we may add this round.
 *
 * For each slot: allocate a cluster, bus_dmamap_load_mbuf, write the rx_desc
 * (length, req_id = slot index, FIRST|LAST|COMP_REQ|phase, buffer phys addr in
 * little-endian), and PREREAD-sync the data buffer (the device DMA-writes the
 * received frame into it). After the batch: PREWRITE-sync the SQ ring (the
 * device reads the descriptors we just wrote) and ring the SQ doorbell with the
 * monotonic SQ tail (ena_com_write_rx_sq_doorbell writes io_sq->tail,
 * ena_eth_com.h:174-182).
 *
 * Per-sync audit:
 *   data buffer PREREAD (per slot, before posting): device will DMA-write the
 *     frame into this cluster; invalidate the CPU view so a later POSTREAD sees
 *     device data, not stale cache lines.
 *   RX SQ PREWRITE (after batch, before doorbell): flush the descriptors we
 *     just stored so the device's read behind the doorbell sees them.
 */
void
ena_rx_fill(struct ena_rxq *rxq)
{
	struct ena_softc *sc = rxq->rxq_sc;
	struct ena_eth_io_rx_desc *ring, *desc;
	struct ena_rx_slot *rxs;
	struct mbuf *m;
	uint64_t paddr;
	uint16_t qmask, slot;
	u_int slots, filled;

	qmask = rxq->rxq_depth - 1;
	ring = (struct ena_eth_io_rx_desc *)ENA_DMA_KVA(&rxq->rxq_sq_dma);

	slots = if_rxr_get(&rxq->rxq_rxr, rxq->rxq_depth);
	if (slots == 0)
		return;

	for (filled = 0; filled < slots; filled++) {
		slot = rxq->rxq_sq_tail & qmask;
		rxs = &rxq->rxq_slots[slot];
		desc = &ring[slot];

		m = MCLGETL(NULL, M_DONTWAIT, sc->sc_rx_buf_size);
		if (m == NULL)
			break;
		/*
		 * Set m_len to the REQUESTED size, not m_ext.ext_size: MCLGETL rounds
		 * the request up to the next cluster pool (e.g. 9018 -> a 9344-byte
		 * pool), but the per-slot DMA map was created with maxsize ==
		 * sc_rx_buf_size, so loading the full ext_size would exceed the map
		 * and bus_dmamap_load_mbuf would fail -> no RX buffers posted -> RX
		 * wedges. Loading exactly sc_rx_buf_size matches the map and still
		 * holds a full mtu+L2+FCS frame (mirrors if_vmx, which loads JUMBO_LEN
		 * not the cluster's ext_size). desc->length then follows ds_len below.
		 */
		m->m_len = m->m_pkthdr.len = sc->sc_rx_buf_size;

		if (bus_dmamap_load_mbuf(sc->sc_dmat, rxs->rxs_map, m,
		    BUS_DMA_NOWAIT) != 0) {
			m_freem(m);
			break;
		}
		rxs->rxs_m = m;

		/*
		 * Hand the data buffer to the device: invalidate the CPU view
		 * so the eventual POSTREAD in ena_rxeof sees the DMA-written
		 * frame.
		 */
		bus_dmamap_sync(sc->sc_dmat, rxs->rxs_map, 0,
		    rxs->rxs_map->dm_mapsize, BUS_DMASYNC_PREREAD);

		paddr = rxs->rxs_map->dm_segs[0].ds_addr;
		memset(desc, 0, sizeof(*desc));
		desc->length = htole16((uint16_t)rxs->rxs_map->dm_segs[0].ds_len);
		desc->req_id = htole16(slot);
		/* ctrl is a single byte: phase + FIRST|LAST|COMP_REQ. */
		desc->ctrl = (rxq->rxq_sq_phase & ENA_ETH_IO_RX_DESC_PHASE_MASK) |
		    ENA_ETH_IO_RX_DESC_FIRST_MASK |
		    ENA_ETH_IO_RX_DESC_LAST_MASK |
		    ENA_ETH_IO_RX_DESC_COMP_REQ_MASK;
		desc->buff_addr_lo = htole32((uint32_t)paddr);
		desc->buff_addr_hi = htole16((uint16_t)(paddr >> 32));

		/* Advance the SQ producer; flip the producer phase on wrap. */
		rxq->rxq_sq_tail++;
		if ((rxq->rxq_sq_tail & qmask) == 0)
			rxq->rxq_sq_phase ^= 1;
	}

	if_rxr_put(&rxq->rxq_rxr, slots - filled);

	if (filled == 0)
		return;

	/*
	 * Flush the descriptors we just wrote so the device's read behind the
	 * doorbell sees them, then ring the RX SQ doorbell with the monotonic
	 * tail.
	 */
	bus_dmamap_sync(sc->sc_dmat, ENA_DMA_MAP(&rxq->rxq_sq_dma), 0,
	    ENA_DMA_LEN(&rxq->rxq_sq_dma), BUS_DMASYNC_PREWRITE);
	ENA_REG_WR32_DB(sc, rxq->rxq_sq_db, rxq->rxq_sq_tail);
}

/*
 * Deferred RX refill: clusters were temporarily unavailable, retry. Re-arm if
 * the ring is still empty. Mirrors mcx_refill (if_mcx.c:6805).
 */
void
ena_rx_refill(void *arg)
{
	struct ena_rxq *rxq = arg;
	int s;

	s = splnet();
	ena_rx_fill(rxq);
	if (if_rxr_inuse(&rxq->rxq_rxr) == 0)
		timeout_add(&rxq->rxq_refill, 1);
	splx(s);
}

/*
 * Drain completed RX descriptors and hand the frames to the stack.
 *
 * Mirrors ena_com_rx_pkt (ena_eth_com.c:578) + mcx_process_rx
 * (if_mcx.c:6909). Phase 1 RX is single-buffer: the device sets FIRST and LAST
 * on a single cdesc per packet (the cluster is large enough for any frame at
 * the default MTU), so we treat each owned cdesc as one complete packet.
 *
 * Walk: for each cdesc whose PHASE bit matches the host consumer phase, read
 * length + req_id (little-endian); recover the slot via req_id; POSTREAD-sync
 * and unload its data buffer; set the mbuf length and enqueue it; advance the
 * CQ head and flip phase on wrap. Bounded by the ring depth (at most depth
 * completions can be host-owned at once). After the walk, hand the list to the
 * stack, account the freed buffers to if_rxr, and refill.
 *
 * Per-sync audit:
 *   RX CQ POSTREAD (before the phase-bit walk): the device DMA-wrote the
 *     completions; refresh the CPU view so the phase-bit reads and the
 *     length/req_id reads observe device memory. Synced once up front over the
 *     whole ring (single-segment, cheap), covering every entry the loop reads.
 *   data buffer POSTREAD (per packet, before unload): the device DMA-wrote the
 *     frame; order the CPU read of the payload after the device write. Pairs
 *     with the PREREAD ena_rx_fill issued when the buffer was posted.
 *   RX CQ PREREAD (after the walk): re-prime the read side so the next
 *     interrupt's POSTREAD observes the device's future writes into the slots
 *     we consumed.
 *
 * Returns the number of packets enqueued.
 */
int
ena_rxeof(struct ena_rxq *rxq)
{
	struct ena_softc *sc = rxq->rxq_sc;
	struct ena_eth_io_rx_cdesc_base *ring, *cdesc;
	struct ena_rx_slot *rxs;
	struct mbuf_list ml = MBUF_LIST_INITIALIZER();
	struct mbuf *m;
	uint32_t status;
	uint16_t qmask, idx, req_id, len;
	uint8_t phase;
	int rxfree = 0;
	u_int bound;

	ring = (struct ena_eth_io_rx_cdesc_base *)ENA_DMA_KVA(&rxq->rxq_cq_dma);
	qmask = rxq->rxq_depth - 1;
	phase = rxq->rxq_cq_phase;

	/*
	 * POSTREAD the whole CQ ring once before the walk: order the reads of
	 * the phase bits and completion payloads after the device's DMA writes.
	 */
	bus_dmamap_sync(sc->sc_dmat, ENA_DMA_MAP(&rxq->rxq_cq_dma), 0,
	    ENA_DMA_LEN(&rxq->rxq_cq_dma), BUS_DMASYNC_POSTREAD);

	for (bound = 0; bound < rxq->rxq_depth; bound++) {
		idx = rxq->rxq_cq_head & qmask;
		cdesc = &ring[idx];

		status = letoh32(cdesc->status);
		if (((status & ENA_ETH_IO_RX_CDESC_BASE_PHASE_MASK) >>
		    ENA_ETH_IO_RX_CDESC_BASE_PHASE_SHIFT) != phase)
			break;

		len = letoh16(cdesc->length);
		req_id = letoh16(cdesc->req_id);

		/* Advance the CQ consumer; flip phase on ring wrap. */
		rxq->rxq_cq_head++;
		if ((rxq->rxq_cq_head & qmask) == 0)
			phase ^= 1;

		/* Guard a malformed req_id rather than index out of bounds. */
		if (req_id >= rxq->rxq_depth) {
			printf("%s: RX bad req_id %u\n", sc->sc_dev.dv_xname,
			    req_id);
			continue;
		}

		rxs = &rxq->rxq_slots[req_id];
		if (rxs->rxs_m == NULL) {
			printf("%s: RX req_id %u empty slot\n",
			    sc->sc_dev.dv_xname, req_id);
			continue;
		}

		/*
		 * Order the CPU read of the frame after the device write, then
		 * release the mapping so the cluster can be handed to the stack.
		 */
		bus_dmamap_sync(sc->sc_dmat, rxs->rxs_map, 0,
		    rxs->rxs_map->dm_mapsize, BUS_DMASYNC_POSTREAD);
		bus_dmamap_unload(sc->sc_dmat, rxs->rxs_map);

		m = rxs->rxs_m;
		rxs->rxs_m = NULL;
		m->m_pkthdr.len = m->m_len = len;

		/*
		 * With RSS active, hand the stack the device's flow hash so it can
		 * pick a matching TX queue for return traffic. RX-to-CPU spread is
		 * already done by the per-vector ISR this completion arrived on.
		 */
		if (sc->sc_rss_active) {
			m->m_pkthdr.ph_flowid = letoh32(cdesc->hash);
			SET(m->m_pkthdr.csum_flags, M_FLOWID);
		}

		/*
		 * RX checksum-offload decode (never-drop policy): set only the
		 * *_IN_OK flags when the device reports a verified-good checksum.
		 * We never set *_IN_BAD -- a device misreport simply degrades to
		 * software re-verify in the stack, never a drop. L4 OK requires
		 * the device to have actually CHECKED the L4 csum and the packet
		 * not to be an IPv4 fragment.
		 */
		{
			uint32_t l3p = status &
			    ENA_ETH_IO_RX_CDESC_BASE_L3_PROTO_IDX_MASK;
			uint32_t l4p = (status &
			    ENA_ETH_IO_RX_CDESC_BASE_L4_PROTO_IDX_MASK) >>
			    ENA_ETH_IO_RX_CDESC_BASE_L4_PROTO_IDX_SHIFT;

			if (sc->sc_rx_csum_l3 &&
			    l3p == ENA_ETH_IO_L3_PROTO_IPV4 &&
			    !(status & ENA_ETH_IO_RX_CDESC_BASE_L3_CSUM_ERR_MASK))
				m->m_pkthdr.csum_flags |= M_IPV4_CSUM_IN_OK;

			if (sc->sc_rx_csum_l4 &&
			    (status & ENA_ETH_IO_RX_CDESC_BASE_L4_CSUM_CHECKED_MASK) &&
			    !(status & ENA_ETH_IO_RX_CDESC_BASE_IPV4_FRAG_MASK) &&
			    !(status & ENA_ETH_IO_RX_CDESC_BASE_L4_CSUM_ERR_MASK)) {
				if (l4p == ENA_ETH_IO_L4_PROTO_TCP)
					m->m_pkthdr.csum_flags |=
					    M_TCP_CSUM_IN_OK;
				else if (l4p == ENA_ETH_IO_L4_PROTO_UDP)
					m->m_pkthdr.csum_flags |=
					    M_UDP_CSUM_IN_OK;
			}
		}

		ml_enqueue(&ml, m);
		rxfree++;
	}

	rxq->rxq_cq_phase = phase;

	/*
	 * Re-prime the read side for the consumed slots so the next interrupt's
	 * POSTREAD observes the device's subsequent writes into them.
	 */
	bus_dmamap_sync(sc->sc_dmat, ENA_DMA_MAP(&rxq->rxq_cq_dma), 0,
	    ENA_DMA_LEN(&rxq->rxq_cq_dma), BUS_DMASYNC_PREREAD);

	if (rxfree > 0) {
		if_rxr_put(&rxq->rxq_rxr, rxfree);
		if (ifiq_input(rxq->rxq_ifiq, &ml))
			if_rxr_livelocked(&rxq->rxq_rxr);
		ena_rx_fill(rxq);
		if (if_rxr_inuse(&rxq->rxq_rxr) == 0)
			timeout_add(&rxq->rxq_refill, 1);
	}

	/*
	 * Re-arm the IO CQ interrupt: the device masked it when it fired, so we
	 * must unmask after servicing or no further RX interrupts arrive. Done
	 * unconditionally (even on a zero-work poll) to match ena_netdev.c, which
	 * unmasks after every poll completion. Uses the per-CQ unmask offset from
	 * the CREATE_CQ response, not the global ENA_REGS_INTR_MASK_OFF.
	 */
	ena_cq_unmask(sc, rxq->rxq_unmask_off);

	return (rxfree);
}

/*
 * RX interrupt entry, called from ena_intr_io_queue. The IO CQ was created in
 * interrupt mode; service it. Guard on rxq_created so a spurious interrupt
 * before the queue is up (or after teardown) touches no freed memory.
 */
int
ena_rx_intr(struct ena_rxq *rxq)
{
	if (!rxq->rxq_created)
		return (0);

	return (ena_rxeof(rxq));
}

/*
 * Negotiate the TX descriptor placement (LLQ / push vs host memory).
 *
 * Mirrors ena_com_config_dev_mode (ena_com.c:3488-3515) + ena_com_set_llq
 * (ena_com.c:609-642) collapsed to the Phase 1 case. Steps:
 *
 *   1. GET_FEATURE(ENA_ADMIN_LLQ, version 1). The device writes the
 *      ena_admin_feature_llq_desc INLINE into the ACQ completion entry
 *      (ena_com.c:2407-2412); we reinterpret the resp as the llq response.
 *
 *   2. If max_llq_num == 0 the device has no LLQ: select host-memory placement
 *      (ena_com_config_dev_mode early-out). On real Graviton ENA, LLQ is the
 *      supported TX path; the host-memory branch is a documented fallback that
 *      uses a bus_dma TX SQ ring instead of the BAR2 window.
 *
 *   3. Otherwise verify the device supports each bit of our chosen geometry
 *      (INLINE_HEADER, MULTIPLE_DESCS_PER_ENTRY, descs_before_header=2,
 *      128B entry) against the *_supported masks, exactly as
 *      ena_com_config_llq_info does (ena_com.c:644-772). If any chosen bit is
 *      unsupported we fall back to host placement rather than guess.
 *
 *   4. Map the BAR2 LLQ memory window (the device places the push descriptors
 *      there; the per-SQ offset is reported later in the CREATE_SQ response).
 *
 *   5. SET_FEATURE(ENA_ADMIN_LLQ) to commit the chosen geometry, echoing the
 *      *_enabled fields ena_com_set_llq writes.
 *
 * The chosen mode is printed for the milestone log.
 */
/*
 * ena_tx_llq_map -- ONE-TIME map of the BAR2 LLQ memory window.
 *
 * This is the only part of LLQ bring-up that needs the pci_attach_args, so it
 * is split out and called exactly once from ena_attach (via the
 * ena_tx_llq_negotiate wrapper). The device places the push descriptor list in
 * this window; the per-SQ byte offset is reported in the CREATE_SQ response
 * (llq_descriptors_offset) and applied in ena_tx_create. The window persists
 * for the device's lifetime (ena_reset does NOT unmap it), so a mid-life reset
 * recovery re-runs ena_tx_llq_setup() only and never remaps here.
 *
 * Mapping failure is NOT fatal: it leaves sc_llq_s == 0, and ena_tx_llq_setup
 * then treats a missing window as host-memory placement.
 */
int
ena_tx_llq_map(struct ena_softc *sc, struct pci_attach_args *pa)
{
	pcireg_t memtype;

	/* Already mapped (or a previous map failed): nothing to do. */
	if (sc->sc_llq_s != 0)
		return (0);

	memtype = pci_mapreg_type(sc->sc_pc, sc->sc_tag, ENA_PCI_MEM_BAR);
	/*
	 * BUS_SPACE_MAP_PREFETCHABLE maps the LLQ push window write-combining
	 * (Normal-NC on arm64) like FreeBSD ena_enable_wc; it is a perf/ordering
	 * choice, never the cause of a map failure. NOTE: on the native EC2
	 * Graviton VF, BAR2 is unassigned (reads as 0), so this map fails and the
	 * driver uses host-memory TX placement -- the device exposes no LLQ
	 * window there, and host placement is the correct supported mode.
	 */
	if (pci_mapreg_map(pa, ENA_PCI_MEM_BAR, memtype,
	    BUS_SPACE_MAP_PREFETCHABLE,
	    &sc->sc_llq_t, &sc->sc_llq_h, NULL, &sc->sc_llq_s, 0) != 0) {
		printf("%s: can't map LLQ mem BAR, using host TX mode\n",
		    sc->sc_dev.dv_xname);
		sc->sc_llq_s = 0;
		return (0);
	}

	return (0);
}

/*
 * ena_tx_llq_setup -- GET/SET LLQ admin feature commit + geometry.
 *
 * The re-callable half of LLQ negotiation: it runs the GET_FEATURE(LLQ) /
 * SET_FEATURE(LLQ) admin commands and stores the chosen geometry
 * (sc_tx_llq, sc_llq_entry_size, sc_llq_descs_before_hdr, sc_tx_max_header).
 * It needs NO pci_attach_args, so it is safe to call again after a device
 * reset to RE-COMMIT LLQ (a reset returns the NIC to host-memory placement, so
 * recovery must re-issue SET_FEATURE(LLQ) or TX silently breaks).
 *
 * Graceful fallback is preserved: GET failure (e.g. status 3 / unsupported),
 * max_llq_num == 0, an unsupported geometry bit, an unmapped BAR2 window, or a
 * SET rejection all select host-memory placement (sc_tx_llq = 0) and return 0
 * — never a hard error. Unlike the old combined routine, this does NOT own the
 * BAR2 map: it never maps or unmaps it (ena_tx_llq_map / ena_detach own that),
 * so a host-mode fallback simply leaves the (already mapped, unused) window in
 * place.
 *
 * Mirrors ena_com_config_dev_mode (ena_com.c:3488-3515) + ena_com_set_llq
 * (ena_com.c:609-642) collapsed to the Phase 1 case (see the original notes:
 * INLINE_HEADER, MULTIPLE_DESCS_PER_ENTRY, descs_before_header=2, 128B entry).
 */
int
ena_tx_llq_setup(struct ena_softc *sc)
{
	struct ena_admin_aq_entry cmd;
	struct ena_admin_acq_entry resp;
	struct ena_admin_get_feat_cmd *gf =
	    (struct ena_admin_get_feat_cmd *)&cmd;
	struct ena_admin_get_feat_llq_resp *gr =
	    (struct ena_admin_get_feat_llq_resp *)&resp;
	struct ena_admin_set_feat_llq_cmd *sf =
	    (struct ena_admin_set_feat_llq_cmd *)&cmd;
	struct ena_admin_feature_llq_desc *llq;
	uint32_t max_llq_num;
	int error;

	/* Defaults: host-memory placement unless LLQ negotiation succeeds. */
	sc->sc_tx_llq = 0;
	sc->sc_llq_entry_size = ENA_LLQ_ENTRY_SIZE;
	sc->sc_llq_descs_before_hdr = ENA_LLQ_DESCS_BEFORE_HEADER;
	sc->sc_tx_max_header = ENA_TX_MAX_HEADER_SIZE;

	/* No BAR2 window mapped: LLQ is impossible, use host placement. */
	if (sc->sc_llq_s == 0) {
		printf("%s: TX mode host (no LLQ window)\n",
		    sc->sc_dev.dv_xname);
		return (0);
	}

	/* --- GET_FEATURE(LLQ): response delivered INLINE in the ACQ. --- */
	memset(&cmd, 0, sizeof(cmd));
	gf->aq_common_descriptor.opcode = ENA_ADMIN_GET_FEATURE;
	gf->aq_common_descriptor.flags = 0;	/* no control buffer */
	gf->control_buffer.length = 0;		/* inline response */
	gf->feat_common.feature_id = ENA_ADMIN_LLQ;
	gf->feat_common.feature_version = ENA_ADMIN_LLQ_FEATURE_VERSION_1;

	error = ena_admin_poll(sc, &cmd, &resp);
	if (error != 0) {
		/*
		 * GET_FEATURE(LLQ) is not strictly mandatory; ena-com treats
		 * EOPNOTSUPP as "no LLQ" (ena_com.c:2411). Treat any failure as
		 * "no LLQ" and use host-memory placement.
		 */
		printf("%s: GET_FEATURE(LLQ) failed (%d), using host TX mode\n",
		    sc->sc_dev.dv_xname, error);
		return (0);
	}

	llq = &gr->u.llq;
	max_llq_num = letoh32(llq->max_llq_num);
	if (max_llq_num == 0) {
		/* Device has no LLQ: host-memory placement. */
		printf("%s: TX mode host (no LLQ)\n", sc->sc_dev.dv_xname);
		return (0);
	}

	/*
	 * Verify the device supports each chosen geometry bit. The *_supported
	 * fields are bitfields of the enum values; we require our exact picks.
	 * If anything is missing, fall back to host placement (safe, documented)
	 * rather than program an unsupported layout.
	 */
	if ((letoh16(llq->header_location_ctrl_supported) &
	    ENA_ADMIN_LLQ_INLINE_HEADER) == 0 ||
	    (letoh16(llq->entry_size_ctrl_supported) &
	    ENA_ADMIN_LLQ_LIST_ENTRY_SIZE_128B) == 0 ||
	    (letoh16(llq->desc_num_before_header_supported) &
	    ENA_ADMIN_LLQ_NUM_DESCS_BEFORE_HEADER_2) == 0 ||
	    (letoh16(llq->descriptors_stride_ctrl_supported) &
	    ENA_ADMIN_LLQ_MULTIPLE_DESCS_PER_ENTRY) == 0) {
		printf("%s: LLQ geometry unsupported, using host TX mode\n",
		    sc->sc_dev.dv_xname);
		return (0);
	}

	/* --- SET_FEATURE(LLQ): commit the chosen geometry. --- */
	memset(&cmd, 0, sizeof(cmd));
	sf->aq_common_descriptor.opcode = ENA_ADMIN_SET_FEATURE;
	sf->aq_common_descriptor.flags = 0;	/* no control buffer */
	sf->feat_common.feature_id = ENA_ADMIN_LLQ;
	sf->feat_common.feature_version = ENA_ADMIN_LLQ_FEATURE_VERSION_1;
	sf->u.llq.header_location_ctrl_enabled =
	    htole16(ENA_ADMIN_LLQ_INLINE_HEADER);
	sf->u.llq.entry_size_ctrl_enabled =
	    htole16(ENA_ADMIN_LLQ_LIST_ENTRY_SIZE_128B);
	sf->u.llq.desc_num_before_header_enabled =
	    htole16(ENA_ADMIN_LLQ_NUM_DESCS_BEFORE_HEADER_2);
	sf->u.llq.descriptors_stride_ctrl_enabled =
	    htole16(ENA_ADMIN_LLQ_MULTIPLE_DESCS_PER_ENTRY);

	error = ena_admin_poll(sc, &cmd, NULL);
	if (error != 0) {
		/*
		 * Device rejected our layout: use host placement. We do NOT
		 * unmap the BAR2 window here — ena_tx_llq_map / ena_detach own
		 * its lifetime, and a re-commit on a later reset may succeed.
		 */
		printf("%s: SET_FEATURE(LLQ) failed (%d), using host TX mode\n",
		    sc->sc_dev.dv_xname, error);
		return (0);
	}

	sc->sc_tx_llq = 1;
	printf("%s: TX mode LLQ (push, %uB entry, %u hdr descs, %u hdr bytes)\n",
	    sc->sc_dev.dv_xname, sc->sc_llq_entry_size,
	    sc->sc_llq_descs_before_hdr, sc->sc_tx_max_header);
	return (0);
}

/*
 * ena_tx_llq_negotiate -- thin attach-time wrapper: map the BAR2 LLQ window
 * (the one part needing pci_attach_args) then run the GET/SET LLQ admin
 * commit. The ena_attach call site is unchanged. Recovery (ena_restore_device)
 * calls ena_tx_llq_setup() directly, since the window is already mapped and
 * ena_reset does not unmap it.
 */
int
ena_tx_llq_negotiate(struct ena_softc *sc, struct pci_attach_args *pa)
{
	int error;

	error = ena_tx_llq_map(sc, pa);
	if (error != 0)
		return (error);
	return (ena_tx_llq_setup(sc));
}

/*
 * Create the IO TX queue: the TX CQ first, then the TX SQ bound to it.
 *
 * Mirrors ena_com_create_io_queue (ena_com.c:2200-2258: create_io_cq then
 * create_io_sq) and ena_com_create_io_sq (ena_com.c:1302-1369). The CQ is
 * created before the SQ because the SQ create command carries the CQ index it
 * completes into. Command fields:
 *
 *   CREATE_CQ:
 *     cq_caps_2 = sizeof(tx_cdesc)/4 == 2 words   (non-extended TX cdesc)
 *     cq_caps_1 = INTERRUPT_MODE_ENABLED
 *     msix_vector = IO vector index (1: ENA_IO_IRQ_FIRST_IDX)
 *     cq_depth = ENA_TX_QUEUE_DEPTH
 *     cq_ba = TX CQ ring physical base (host memory)
 *   Response: cq_idx, cq_interrupt_unmask_register_offset.
 *
 *   CREATE_SQ (TX direction):
 *     sq_identity = SQ_DIRECTION_TX << DIRECTION_SHIFT
 *     sq_caps_2   = placement (DEV for LLQ / HOST for fallback) |
 *                   (COMPLETION_POLICY_DESC << shift)
 *     sq_caps_3   = IS_PHYSICALLY_CONTIGUOUS
 *     cq_idx      = the CQ index just returned
 *     sq_depth    = ENA_TX_QUEUE_DEPTH
 *     sq_ba       = host SQ ring base (host placement only; MBZ for LLQ, where
 *                   the descriptors live in the BAR2 window the device owns)
 *   Response: sq_idx, sq_doorbell_offset, llq_descriptors_offset (LLQ only).
 *
 * For LLQ placement (ena_com_create_io_sq, ena_com.c:1340-1360) the SQ base
 * address is NOT a host ring: the device returns llq_descriptors_offset, the
 * byte offset within the LLQ mem BAR where this SQ's push window begins. We
 * stash it and write entries at txq_llq_off + index*entry_size.
 */
int
ena_tx_create(struct ena_softc *sc, unsigned int idx)
{
	struct ena_txq *txq = &sc->sc_txq[idx];
	struct ena_admin_aq_entry cmd;
	struct ena_admin_acq_entry resp;
	struct ena_admin_aq_create_cq_cmd *cc =
	    (struct ena_admin_aq_create_cq_cmd *)&cmd;
	struct ena_admin_acq_create_cq_resp_desc *cr =
	    (struct ena_admin_acq_create_cq_resp_desc *)&resp;
	struct ena_admin_aq_create_sq_cmd *sq_cmd =
	    (struct ena_admin_aq_create_sq_cmd *)&cmd;
	struct ena_admin_acq_create_sq_resp_desc *sr =
	    (struct ena_admin_acq_create_sq_resp_desc *)&resp;
	uint64_t cq_pa, sq_pa;
	uint8_t placement;
	int error = ENOMEM;
	uint16_t i;

	txq->txq_depth = MIN(ENA_TX_QUEUE_DEPTH, sc->sc_max_tx_depth);
	txq->txq_sq_tail = 0;
	txq->txq_sq_phase = 1;	/* first SQ descriptor is written phase 1 */
	txq->txq_cq_head = 0;
	txq->txq_cq_phase = 1;	/* first CQ completion is read at phase 1 */
	txq->txq_prod = 0;
	txq->txq_cons = 0;
	/*
	 * Host-placement free SQ descriptor slots: at most depth-1 descriptors
	 * outstanding (one slot kept free to distinguish full from empty), so a
	 * multi-segment packet's descriptors never lap an unreclaimed slot. LLQ
	 * placement does not use this (its tail counts entries, gated by
	 * txq_prod/txq_cons).
	 */
	txq->txq_sq_avail = txq->txq_depth - 1;
	txq->txq_llq = sc->sc_tx_llq;
	txq->txq_llq_t = sc->sc_llq_t;
	txq->txq_llq_h = sc->sc_llq_h;
	txq->txq_llq_off = 0;

	/* Allocate the TX CQ (device->host) ring in host memory. */
	if (ena_dmamem_alloc(sc, &txq->txq_cq_dma,
	    ENA_TX_CQ_SIZE(txq->txq_depth), PAGE_SIZE) != 0) {
		printf("%s: can't allocate TX CQ ring\n", sc->sc_dev.dv_xname);
		return (ENOMEM);
	}

	/*
	 * Host-memory fallback only: allocate the TX SQ descriptor ring. In LLQ
	 * mode the descriptors live in the BAR2 window, so there is no host SQ
	 * ring to allocate.
	 */
	if (!txq->txq_llq) {
		if (ena_dmamem_alloc(sc, &txq->txq_sq_dma,
		    ENA_TX_SQ_SIZE(txq->txq_depth), PAGE_SIZE) != 0) {
			printf("%s: can't allocate TX SQ ring\n",
			    sc->sc_dev.dv_xname);
			goto free_cq;
		}
	}

	/* Per-slot bookkeeping + DMA maps for the packet mbufs. */
	txq->txq_slots = mallocarray(txq->txq_depth,
	    sizeof(struct ena_tx_slot), M_DEVBUF, M_WAITOK | M_ZERO);
	{
		/*
		 * Size the TX maps for the largest frame the device MTU allows so
		 * jumbo packets can be transmitted. ena_encap m_defrags any chain
		 * with more than ENA_TX_MAX_SEGS segments into one contiguous
		 * cluster, so a jumbo packet loads as a single segment up to the
		 * whole frame -- hence maxsegsz must be the full frame, not MCLBYTES
		 * (a 9000-byte segment against a 2KB maxsegsz would fail to load).
		 * Never below the old 2-cluster floor.
		 */
		bus_size_t txbufsz = sc->sc_max_mtu + ETHER_HDR_LEN + ETHER_CRC_LEN;

		if (txbufsz < ENA_TX_MAX_SEGS * MCLBYTES)
			txbufsz = ENA_TX_MAX_SEGS * MCLBYTES;
		for (i = 0; i < txq->txq_depth; i++) {
			if (bus_dmamap_create(sc->sc_dmat, txbufsz,
			    ENA_TX_MAX_SEGS, txbufsz, 0,
			    BUS_DMA_WAITOK | BUS_DMA_64BIT,
			    &txq->txq_slots[i].txs_map) != 0) {
				printf("%s: can't create TX DMA map\n",
				    sc->sc_dev.dv_xname);
				goto free_maps;
			}
		}
	}

	/*
	 * Initial ownership hand-off of the TX CQ (device->host): flush the
	 * zero fill (clears phase bits) and prime the read side so the first
	 * POSTREAD-synced phase poll in ena_txeof sees device writes. Mirrors
	 * the RX CQ hand-off in ena_rx_create.
	 */
	bus_dmamap_sync(sc->sc_dmat, ENA_DMA_MAP(&txq->txq_cq_dma), 0,
	    ENA_DMA_LEN(&txq->txq_cq_dma),
	    BUS_DMASYNC_PREREAD | BUS_DMASYNC_PREWRITE);

	/* --- CREATE_CQ: must precede CREATE_SQ (SQ references the CQ idx). --- */
	cq_pa = ENA_DMA_DVA(&txq->txq_cq_dma);
	memset(&cmd, 0, sizeof(cmd));
	cc->aq_common_descriptor.opcode = ENA_ADMIN_CREATE_CQ;
	cc->cq_caps_2 = (ENA_TX_CDESC_SIZE / 4) &
	    ENA_ADMIN_AQ_CREATE_CQ_CMD_CQ_ENTRY_SIZE_WORDS_MASK;
	cc->cq_caps_1 = ENA_ADMIN_AQ_CREATE_CQ_CMD_INTERRUPT_MODE_ENABLED_MASK;
	cc->msix_vector = htole32(idx + 1);	/* per-queue IO vector (vec 0 = mgmt) */
	cc->cq_depth = htole16(txq->txq_depth);
	cc->cq_ba.mem_addr_low = htole32((uint32_t)cq_pa);
	cc->cq_ba.mem_addr_high = htole16((uint16_t)(cq_pa >> 32));

	error = ena_admin_poll(sc, &cmd, &resp);
	if (error != 0) {
		printf("%s: TX CREATE_CQ failed (%d)\n", sc->sc_dev.dv_xname,
		    error);
		goto free_maps;
	}
	txq->txq_cq_idx = letoh16(cr->cq_idx);
	txq->txq_unmask_off = letoh32(cr->cq_interrupt_unmask_register_offset);

	/* --- CREATE_SQ (TX direction), bound to the CQ just created. --- */
	placement = txq->txq_llq ? ENA_ADMIN_PLACEMENT_POLICY_DEV :
	    ENA_ADMIN_PLACEMENT_POLICY_HOST;
	memset(&cmd, 0, sizeof(cmd));
	sq_cmd->aq_common_descriptor.opcode = ENA_ADMIN_CREATE_SQ;
	sq_cmd->sq_identity =
	    (ENA_ADMIN_SQ_DIRECTION_TX <<
	    ENA_ADMIN_AQ_CREATE_SQ_CMD_SQ_DIRECTION_SHIFT) &
	    ENA_ADMIN_AQ_CREATE_SQ_CMD_SQ_DIRECTION_MASK;
	sq_cmd->sq_caps_2 =
	    (placement & ENA_ADMIN_AQ_CREATE_SQ_CMD_PLACEMENT_POLICY_MASK) |
	    ((ENA_ADMIN_COMPLETION_POLICY_DESC <<
	    ENA_ADMIN_AQ_CREATE_SQ_CMD_COMPLETION_POLICY_SHIFT) &
	    ENA_ADMIN_AQ_CREATE_SQ_CMD_COMPLETION_POLICY_MASK);
	sq_cmd->sq_caps_3 =
	    ENA_ADMIN_AQ_CREATE_SQ_CMD_IS_PHYSICALLY_CONTIGUOUS_MASK;
	sq_cmd->cq_idx = htole16(txq->txq_cq_idx);
	sq_cmd->sq_depth = htole16(txq->txq_depth);
	/*
	 * Host placement programs the SQ ring base; LLQ leaves sq_ba zero (the
	 * descriptor memory is the device's BAR2 window, returned in the resp).
	 */
	if (!txq->txq_llq) {
		sq_pa = ENA_DMA_DVA(&txq->txq_sq_dma);
		sq_cmd->sq_ba.mem_addr_low = htole32((uint32_t)sq_pa);
		sq_cmd->sq_ba.mem_addr_high = htole16((uint16_t)(sq_pa >> 32));
	}

	error = ena_admin_poll(sc, &cmd, &resp);
	if (error != 0) {
		printf("%s: TX CREATE_SQ failed (%d)\n", sc->sc_dev.dv_xname,
		    error);
		goto destroy_cq;
	}
	txq->txq_sq_idx = letoh16(sr->sq_idx);
	txq->txq_sq_db = letoh32(sr->sq_doorbell_offset);
	if (txq->txq_llq) {
		txq->txq_llq_off = letoh32(sr->llq_descriptors_offset);
		/*
		 * Bound-check the window: the device must have placed our whole
		 * SQ (depth entries) inside the mapped BAR2 region.
		 */
		if (txq->txq_llq_off +
		    (bus_size_t)txq->txq_depth * sc->sc_llq_entry_size >
		    sc->sc_llq_s) {
			printf("%s: LLQ window offset out of range\n",
			    sc->sc_dev.dv_xname);
			error = EINVAL;
			goto destroy_sq;
		}
	}

	/*
	 * Arm the TX CQ interrupt once after create. The device masks the IO
	 * MSI-X after each fire; without this initial unmask the first TX
	 * completion interrupt never arrives. The TX CQ has its OWN unmask
	 * register (txq_unmask_off), distinct from the RX CQ's.
	 */
	ena_cq_unmask(sc, txq->txq_unmask_off);

	txq->txq_created = 1;
	return (0);

destroy_sq:
	{
		struct ena_admin_aq_destroy_sq_cmd *dsq =
		    (struct ena_admin_aq_destroy_sq_cmd *)&cmd;

		memset(&cmd, 0, sizeof(cmd));
		dsq->aq_common_descriptor.opcode = ENA_ADMIN_DESTROY_SQ;
		dsq->sq.sq_idx = htole16(txq->txq_sq_idx);
		dsq->sq.sq_identity = (ENA_ADMIN_SQ_DIRECTION_TX <<
		    ENA_ADMIN_SQ_SQ_DIRECTION_SHIFT) &
		    ENA_ADMIN_SQ_SQ_DIRECTION_MASK;
		(void)ena_admin_poll(sc, &cmd, NULL);
	}
destroy_cq:
	{
		struct ena_admin_aq_destroy_cq_cmd *dcq =
		    (struct ena_admin_aq_destroy_cq_cmd *)&cmd;

		memset(&cmd, 0, sizeof(cmd));
		dcq->aq_common_descriptor.opcode = ENA_ADMIN_DESTROY_CQ;
		dcq->cq_idx = htole16(txq->txq_cq_idx);
		(void)ena_admin_poll(sc, &cmd, NULL);
	}
free_maps:
	for (i = 0; i < txq->txq_depth; i++) {
		if (txq->txq_slots[i].txs_map != NULL)
			bus_dmamap_destroy(sc->sc_dmat,
			    txq->txq_slots[i].txs_map);
	}
	free(txq->txq_slots, M_DEVBUF,
	    txq->txq_depth * sizeof(struct ena_tx_slot));
	txq->txq_slots = NULL;
	if (!txq->txq_llq)
		ena_dmamem_free(sc, &txq->txq_sq_dma);
free_cq:
	ena_dmamem_free(sc, &txq->txq_cq_dma);
	return (error);
}

/*
 * Tear down the IO TX queue. Destroy the SQ first, then the CQ (reverse of
 * create), mirroring ena_com_destroy_io_queue (ena_com.c:2260-2278). The device
 * stops DMAing into the CQ (and reading the LLQ window) once both are
 * destroyed; only then do we reclaim in-flight mbufs and free the rings/maps.
 */
void
ena_tx_destroy(struct ena_softc *sc, unsigned int idx)
{
	struct ena_txq *txq = &sc->sc_txq[idx];
	struct ena_admin_aq_entry cmd;
	struct ena_admin_aq_destroy_sq_cmd *ds =
	    (struct ena_admin_aq_destroy_sq_cmd *)&cmd;
	struct ena_admin_aq_destroy_cq_cmd *dc =
	    (struct ena_admin_aq_destroy_cq_cmd *)&cmd;
	uint16_t i;

	if (!txq->txq_created)
		return;

	/* DESTROY_SQ (TX direction) first. */
	memset(&cmd, 0, sizeof(cmd));
	ds->aq_common_descriptor.opcode = ENA_ADMIN_DESTROY_SQ;
	ds->sq.sq_idx = htole16(txq->txq_sq_idx);
	ds->sq.sq_identity = (ENA_ADMIN_SQ_DIRECTION_TX <<
	    ENA_ADMIN_SQ_SQ_DIRECTION_SHIFT) & ENA_ADMIN_SQ_SQ_DIRECTION_MASK;
	if (ena_admin_poll(sc, &cmd, NULL) != 0)
		printf("%s: TX DESTROY_SQ failed\n", sc->sc_dev.dv_xname);

	/* DESTROY_CQ second. */
	memset(&cmd, 0, sizeof(cmd));
	dc->aq_common_descriptor.opcode = ENA_ADMIN_DESTROY_CQ;
	dc->cq_idx = htole16(txq->txq_cq_idx);
	if (ena_admin_poll(sc, &cmd, NULL) != 0)
		printf("%s: TX DESTROY_CQ failed\n", sc->sc_dev.dv_xname);

	txq->txq_created = 0;

	/*
	 * Reclaim any mbufs still in flight. The device has dropped the queues,
	 * so nothing else can complete them. Closing sync (POSTWRITE) before
	 * unload orders any final DMA read of the buffer before the CPU frees
	 * it (TX is a device read, hence POSTWRITE).
	 */
	for (i = 0; i < txq->txq_depth; i++) {
		struct ena_tx_slot *txs = &txq->txq_slots[i];

		if (txs->txs_m != NULL) {
			bus_dmamap_sync(sc->sc_dmat, txs->txs_map, 0,
			    txs->txs_map->dm_mapsize, BUS_DMASYNC_POSTWRITE);
			bus_dmamap_unload(sc->sc_dmat, txs->txs_map);
			m_freem(txs->txs_m);
			txs->txs_m = NULL;
		}
		bus_dmamap_destroy(sc->sc_dmat, txs->txs_map);
	}
	free(txq->txq_slots, M_DEVBUF,
	    txq->txq_depth * sizeof(struct ena_tx_slot));
	txq->txq_slots = NULL;

	ena_dmamem_free(sc, &txq->txq_cq_dma);
	if (!txq->txq_llq)
		ena_dmamem_free(sc, &txq->txq_sq_dma);
}

/*
 * Encapsulate one mbuf into the TX queue and ring the doorbell.
 *
 * Returns 0 on success (mbuf consumed / queued for completion), or non-zero if
 * the ring is full (caller requeues) or the packet is undeliverable (caller
 * frees). Mirrors ena_xmit_mbuf + ena_tx_map_mbuf + ena_com_prepare_tx
 * (reference/freebsd/sys/dev/ena/ena_datapath.c:854-1065, ena_eth_com.c:438-576)
 * in OpenBSD idiom.
 *
 * Phase-1 simplifications (no checksum offload, no TSO, single LLQ entry):
 *   - We m_defrag the packet to at most ENA_TX_MAX_SEGS (2) data segments so it
 *     fits one 128-byte LLQ entry (2 descs before header + inline header), so
 *     the LLQ window write is a single contiguous region write per packet.
 *   - The inline push header is the leading min(pktlen, tx_max_header) bytes;
 *     the remainder of the packet is referenced by the data descriptor(s). On
 *     Graviton ENA the device requires a push header in LLQ mode
 *     (ena_com_prepare_tx, ena_eth_com.c:468-472).
 *
 * LLQ entry layout (INLINE_HEADER, descs_before_header=2, 128B):
 *   bytes  0..15  desc[0]   (first data descriptor; FIRST set, COMP_REQ set)
 *   bytes 16..31  desc[1]   (second data descriptor, if a 2nd segment exists)
 *   bytes 32..127 header    (inline packet header bytes, up to 96)
 *
 * --------------------------------------------------------------------------
 * LLQ window write ordering (the highest-risk sequence in the driver):
 *
 *   1. Assemble the WHOLE 128-byte entry in a host-side stack bounce buffer
 *      (descriptors with htole32 fields + the inline header bytes). Nothing is
 *      written to the device window yet.
 *   2. bus_dmamap_sync(PREWRITE) the packet DATA buffer: the device will DMA-
 *      read the payload the data descriptors point at; flush the CPU's view so
 *      the device read sees current memory. (Pairs with POSTWRITE in txeof.)
 *   3. bus_space_write_region_4() copies the bounce buffer into the BAR2 LLQ
 *      window at txq_llq_off + slot*entry_size. This is the device-memory push.
 *   4. bus_space_barrier(BUS_SPACE_BARRIER_WRITE) over the entry just written:
 *      orders all the window stores BEFORE the doorbell store, exactly the
 *      wmb() ena_com issues in ena_com_write_bounce_buffer_to_dev
 *      (ena_eth_com.c:118-121) before the doorbell.
 *   5. Ring the TX SQ doorbell (ENA_REG_WR32_DB carries its own post-store
 *      BARRIER_WRITE) with the new monotonic tail.
 *
 * The host data-buffer PREWRITE (step 2) and the window write (step 3) are
 * independent memory regions; both must be visible before the doorbell. The
 * doorbell's own barrier plus the explicit window barrier in step 4 ensure the
 * device, when it acts on the doorbell, sees both the descriptors/header (in
 * its BAR2 window) and the payload (in host DMA memory).
 * --------------------------------------------------------------------------
 */
int
ena_encap(struct ena_txq *txq, struct mbuf *m)
{
	struct ena_softc *sc = txq->txq_sc;
	struct ena_tx_slot *txs;
	struct ena_eth_io_tx_desc *desc;
	bus_dmamap_t map;
	/* 8-byte aligned: the LLQ window copy casts this to uint32_t* and the
	 * device entry size is a multiple of 8 (ena_eth_com.c:124). */
	uint64_t entry64[ENA_LLQ_ENTRY_SIZE / sizeof(uint64_t)];
	uint8_t *entry = (uint8_t *)entry64;
	uint8_t hdrbuf[ENA_TX_MAX_HEADER_SIZE];
	uint8_t *hdr_src;
	uint16_t qmask, slot;
	uint16_t header_len, off;
	uint32_t len_ctrl, meta_ctrl, addr_lo, addr_hi_hdr;
	uint64_t paddr;
	int nsegs, ndesc, i, b;
	/*
	 * TX checksum offload (IPv4 only). do_csum is set only when ALL of the
	 * following hold, so the non-csum path below is reached with do_csum==0
	 * and behaves byte-for-byte as before:
	 *   - the stack requested an offload (M_IPV4/TCP/UDP_CSUM_OUT), AND
	 *   - the device's OFFLOAD feature reported the matching capability, AND
	 *   - LLQ (push) placement is in use (csum is scoped to the LLQ path;
	 *     see the host-mem note below), AND
	 *   - the L2/L3/L4 headers parse cleanly: untagged Ethernet (no VLAN),
	 *     IPv4 with ip_hl==5 (the only shape the stack hands us with a
	 *     pre-seeded pseudo-header), and a readable L4 header.
	 * When set, the entry prepends ONE meta descriptor (carrying the layer
	 * offsets/lengths) and the first data descriptor's meta_ctrl word gets
	 * the csum-enable bits; the device completes the L4 sum in PART mode.
	 */
	int do_csum = 0;
	uint8_t csum_l3_en = 0, csum_l4_en = 0, csum_l4_proto = 0;
	uint8_t csum_l3_hdr_len = 0, csum_l3_hdr_off = 0, csum_l4_hdr_words = 0;
	const int csum_flags_out =
	    (m->m_pkthdr.csum_flags &
	    (M_IPV4_CSUM_OUT | M_TCP_CSUM_OUT | M_UDP_CSUM_OUT));

	qmask = txq->txq_depth - 1;

	/* Ring full: at most depth-1 slots in flight (see ena_init cap). */
	if (((txq->txq_prod + 1) & qmask) == (txq->txq_cons & qmask))
		return (ENOBUFS);

	slot = txq->txq_sq_tail & qmask;
	txs = &txq->txq_slots[slot];
	map = txs->txs_map;

	/*
	 * Decide checksum offload BEFORE loading the mbuf for DMA. We must read
	 * the L2/L3/L4 header bytes (via m_copydata, which is safe across any
	 * mbuf layout) and, when offloading, force the packet to coalesce so it
	 * uses at most ONE data descriptor: the LLQ entry has only
	 * descs_before_header (2) descriptor slots, and a csum packet spends one
	 * of them on the meta descriptor, leaving room for a single data desc.
	 */
	if (csum_flags_out != 0 && txq->txq_llq &&
	    (sc->sc_tx_csum_l3 || sc->sc_tx_csum_l4)) {
		uint8_t hp[ETHER_HDR_LEN + 20];		/* L2 + minimal IPv4(20) */
		uint16_t etype;
		uint16_t parselen = m->m_pkthdr.len;
		int l4off;

		if (parselen > sizeof(hp))
			parselen = sizeof(hp);
		/* Need at least L2 + a minimal IPv4 header to classify. */
		if (parselen >= ETHER_HDR_LEN + 20) {
			m_copydata(m, 0, ETHER_HDR_LEN + 20, hp);
			etype = (hp[12] << 8) | hp[13];
			l4off = ETHER_HDR_LEN + 20;
			/*
			 * Only untagged IPv4 with ip_hl==5 is offloadable here.
			 * VLAN-tagged frames (software ETHERTYPE_VLAN header) are
			 * left to the software-csum fallback to stay safe. The
			 * OpenBSD stack only leaves M_*_CSUM_OUT set when ip_hl==5,
			 * so this is the guaranteed-success common case.
			 */
			if (etype == ETHERTYPE_IP &&
			    (hp[ETHER_HDR_LEN] & 0xf0) == 0x40 &&	/* IPv4 */
			    (hp[ETHER_HDR_LEN] & 0x0f) == 5 &&		/* ip_hl==5 */
			    m->m_pkthdr.len > l4off) {	/* L4 header present */
				csum_l3_hdr_off = ETHER_HDR_LEN;
				csum_l3_hdr_len = 20;

				if ((m->m_pkthdr.csum_flags & M_IPV4_CSUM_OUT) &&
				    sc->sc_tx_csum_l3)
					csum_l3_en = 1;

				/*
				 * PART-mode (non-TSO) csum: l4_hdr_len is 0 in the
				 * meta descriptor. This matches the device's own
				 * driver, ena_tx_csum (ena_netdev.c:3185-3188),
				 * which sets ena_meta->l4_hdr_len = 0 and
				 * l4_csum_partial = 1 whenever mss == 0; l4_hdr_len
				 * carries the TCP doff only for TSO. The device
				 * locates L4 from l3_hdr_offset + l3_hdr_len and
				 * adds the payload sum to the pre-seeded
				 * pseudo-header field.
				 */
				if ((m->m_pkthdr.csum_flags & M_TCP_CSUM_OUT) &&
				    sc->sc_tx_csum_l4) {
					csum_l4_hdr_words = 0;
					csum_l4_proto = ENA_ETH_IO_L4_PROTO_TCP;
					csum_l4_en = 1;
				} else if ((m->m_pkthdr.csum_flags &
				    M_UDP_CSUM_OUT) && sc->sc_tx_csum_l4) {
					csum_l4_hdr_words = 0;
					csum_l4_proto = ENA_ETH_IO_L4_PROTO_UDP;
					csum_l4_en = 1;
				}

				if (csum_l3_en || csum_l4_en)
					do_csum = 1;
			}
		}

		/*
		 * Offload spends one of the LLQ entry's two descriptor slots on
		 * the meta descriptor, so the payload must fit in a SINGLE data
		 * descriptor. Coalesce the chain to one DMA segment up front; the
		 * inline-header skip then leaves at most one residual buffer. If
		 * m_defrag fails we cannot offload — drop do_csum and fall through
		 * to the software-checksum path below.
		 */
		if (do_csum && m_defrag(m, M_DONTWAIT) != 0)
			do_csum = 0;
	}

	/*
	 * Hardware csum offload here is honored only on the LLQ push path and
	 * only for untagged IPv4 (ip_hl==5) — the case the OpenBSD stack
	 * guarantees when it delegates (M_*_CSUM_OUT stays set). The capability
	 * is therefore advertised only when LLQ is negotiated (see ena_attach),
	 * so the stack never delegates a packet this path cannot offload. VLAN-
	 * tagged frames never arrive here delegated either: the driver advertises
	 * no IFCAP_VLAN_HWTAGGING/HWOFFLOAD, so vlan(4) (if_vlan.c) gives its
	 * interface no csum capabilities and completes the checksum in software
	 * before handing the frame down.
	 */

	/*
	 * Load the mbuf for DMA. The per-slot map was created with a hard limit
	 * of ENA_TX_MAX_SEGS segments, so bus_dmamap_load_mbuf returns EFBIG for
	 * any packet that scatters into more segments than one LLQ entry can
	 * describe. In that case m_defrag coalesces the chain into a single
	 * cluster (1 segment) and we retry. A packet that still cannot be loaded
	 * after defrag is undeliverable; the caller frees it (mirrors
	 * ena_xmit_mbuf's EFBIG/defrag handling, ena_datapath.c).
	 */
	if (bus_dmamap_load_mbuf(sc->sc_dmat, map, m,
	    BUS_DMA_NOWAIT | BUS_DMA_STREAMING) != 0) {
		if (m_defrag(m, M_DONTWAIT) != 0)
			return (EIO);	/* caller frees m */
		if (bus_dmamap_load_mbuf(sc->sc_dmat, map, m,
		    BUS_DMA_NOWAIT | BUS_DMA_STREAMING) != 0)
			return (EIO);	/* caller frees m */
	}
	nsegs = map->dm_nsegs;

	/*
	 * The inline push header is the leading min(pktlen, tx_max_header)
	 * bytes. If those bytes are contiguous in the first mbuf, point at them
	 * directly; otherwise linearise the header into a stack buffer (mirrors
	 * ena_tx_map_mbuf, ena_datapath.c:892-908).
	 */
	header_len = m->m_pkthdr.len;
	if (header_len > sc->sc_tx_max_header)
		header_len = sc->sc_tx_max_header;

	if (header_len <= m->m_len) {
		hdr_src = mtod(m, uint8_t *);
	} else {
		m_copydata(m, 0, header_len, hdrbuf);
		hdr_src = hdrbuf;
	}

	/*
	 * Build the list of DATA descriptors. In LLQ mode the leading header_len
	 * bytes are carried INLINE in the entry, so they must be SKIPPED from the
	 * data descriptors or the device would see those bytes twice. Walk the
	 * DMA segments, dropping the first header_len bytes, and record the
	 * residual (paddr,len) buffers. Mirrors ena_tx_map_mbuf's offset-skip
	 * (reference/freebsd/sys/dev/ena/ena_datapath.c:914-938). For the host-
	 * memory fallback nothing is pushed inline, so no bytes are skipped.
	 */
	{
		uint64_t bpaddr[ENA_TX_MAX_SEGS];
		uint32_t blen[ENA_TX_MAX_SEGS];
		uint16_t skip = txq->txq_llq ? header_len : 0;
		int ndata = 0;
		/*
		 * Descriptor slot 0 is the meta descriptor when offloading csum;
		 * the data descriptors then start at slot 1. Without csum,
		 * descbase==0 and the layout is byte-for-byte unchanged.
		 */
		int descbase = do_csum ? 1 : 0;

		for (b = 0; b < nsegs; b++) {
			uint64_t sa = map->dm_segs[b].ds_addr;
			uint32_t sl = map->dm_segs[b].ds_len;

			if (skip >= sl) {
				skip -= sl;
				continue;	/* segment fully inside header */
			}
			bpaddr[ndata] = sa + skip;
			blen[ndata] = sl - skip;
			skip = 0;
			ndata++;
		}

		/*
		 * Offload invariant: a csum packet was m_defrag'd up front to a
		 * single DMA segment, so after the inline-header skip it has at
		 * most ONE data buffer (ndata <= 1) and meta(1)+data(1) fit the
		 * two descriptor slots. A standard Ethernet frame always fits a
		 * single cluster, so this is unreachable; if it ever trips, drop
		 * the packet rather than emit a corrupt offload (the caller frees
		 * m). We cannot software-csum here: the DMA buffers are about to
		 * be read by the device.
		 */
		if (do_csum && ndata > 1) {
			bus_dmamap_unload(sc->sc_dmat, map);
			return (EIO);
		}

		/*
		 * Emit the descriptors. When the whole packet fit inside the
		 * inline header (ndata == 0) we still emit ONE descriptor that
		 * carries FIRST|LAST|COMP_REQ + the header length and no buffer,
		 * exactly as ena_com_prepare_tx does for num_bufs == 0 with a
		 * non-zero header (ena_eth_com.c:495-565). Otherwise one
		 * descriptor per residual data buffer.
		 */
		ndesc = (ndata == 0) ? 1 : ndata;

		memset(entry, 0, ENA_LLQ_ENTRY_SIZE);

		/*
		 * Meta descriptor (csum offload only). Occupies entry slot 0 and
		 * carries the L3/L4 layer offsets/lengths the device needs to
		 * locate L4 in PART mode. mss=0 (no TSO), so MSS_HI/MSS_LO stay
		 * zero. Mirrors ena_com_create_meta (ena_eth_com.c:354): it sets
		 * META_DESC|EXT_VALID|ETH_META_TYPE|META_STORE|FIRST|PHASE in
		 * len_ctrl (NO last, NO comp_req) and packs l3_hdr_len /
		 * l3_hdr_offset / l4_hdr_len_in_words into word2. The meta desc
		 * shares the entry's phase and is NOT separately completed by the
		 * device (one completion per packet, on the last data desc).
		 */
		if (do_csum) {
			struct ena_eth_io_tx_meta_desc *meta;
			uint32_t mlen_ctrl, mword2;

			mlen_ctrl =
			    ENA_ETH_IO_TX_META_DESC_LEN_CTRL_META_DESC_MASK |
			    ENA_ETH_IO_TX_META_DESC_LEN_CTRL_EXT_VALID_MASK |
			    ENA_ETH_IO_TX_META_DESC_LEN_CTRL_ETH_META_TYPE_MASK |
			    ENA_ETH_IO_TX_META_DESC_LEN_CTRL_META_STORE_MASK |
			    ENA_ETH_IO_TX_META_DESC_LEN_CTRL_FIRST_MASK |
			    (((uint32_t)txq->txq_sq_phase <<
			    ENA_ETH_IO_TX_META_DESC_LEN_CTRL_PHASE_SHIFT) &
			    ENA_ETH_IO_TX_META_DESC_LEN_CTRL_PHASE_MASK);

			mword2 =
			    ((uint32_t)csum_l3_hdr_len &
			    ENA_ETH_IO_TX_META_DESC_WORD2_L3_HDR_LEN_MASK) |
			    (((uint32_t)csum_l3_hdr_off <<
			    ENA_ETH_IO_TX_META_DESC_WORD2_L3_HDR_OFF_SHIFT) &
			    ENA_ETH_IO_TX_META_DESC_WORD2_L3_HDR_OFF_MASK) |
			    (((uint32_t)csum_l4_hdr_words <<
			    ENA_ETH_IO_TX_META_DESC_WORD2_L4_HDR_LEN_IN_WORDS_SHIFT) &
			    ENA_ETH_IO_TX_META_DESC_WORD2_L4_HDR_LEN_IN_WORDS_MASK);

			meta = (struct ena_eth_io_tx_meta_desc *)entry;
			meta->len_ctrl = htole32(mlen_ctrl);
			meta->word1 = 0;
			meta->word2 = htole32(mword2);
			meta->reserved = 0;
		}

		for (i = 0; i < ndesc; i++) {
			len_ctrl = (txq->txq_sq_phase <<
			    ENA_ETH_IO_TX_DESC_PHASE_SHIFT) &
			    ENA_ETH_IO_TX_DESC_PHASE_MASK;
			meta_ctrl = 0;
			addr_lo = 0;
			addr_hi_hdr = 0;

			if (ndata > 0) {
				paddr = bpaddr[i];
				len_ctrl |= blen[i] &
				    ENA_ETH_IO_TX_DESC_LENGTH_MASK;
				addr_lo = (uint32_t)paddr;
				addr_hi_hdr = (uint32_t)((paddr >> 32) &
				    ENA_ETH_IO_TX_DESC_ADDR_HI_MASK);
			}

			if (i == 0) {
				/*
				 * First data descriptor: COMP_REQ, req_id, and
				 * (LLQ) the inline header length. FIRST is set
				 * here ONLY when there is no meta descriptor;
				 * with csum the meta desc owns FIRST (mirrors
				 * ena_eth_com.c:502 "if (!have_meta) set FIRST").
				 */
				if (!do_csum)
					len_ctrl |= ENA_ETH_IO_TX_DESC_FIRST_MASK;
				len_ctrl |= ENA_ETH_IO_TX_DESC_COMP_REQ_MASK;
				/*
				 * req_id split across two words exactly as
				 * ena_com_prepare_tx encodes it (ena_eth_com.c:
				 * 511, 516): low 10 bits into meta_ctrl
				 * REQ_ID_LO, bits 15:10 into len_ctrl REQ_ID_HI.
				 */
				meta_ctrl |= ((uint32_t)slot <<
				    ENA_ETH_IO_TX_DESC_REQ_ID_LO_SHIFT) &
				    ENA_ETH_IO_TX_DESC_REQ_ID_LO_MASK;
				len_ctrl |= (((uint32_t)slot >> 10) <<
				    ENA_ETH_IO_TX_DESC_REQ_ID_HI_SHIFT) &
				    ENA_ETH_IO_TX_DESC_REQ_ID_HI_MASK;
				if (txq->txq_llq)
					addr_hi_hdr |= ((uint32_t)header_len <<
					ENA_ETH_IO_TX_DESC_HEADER_LENGTH_SHIFT) &
					ENA_ETH_IO_TX_DESC_HEADER_LENGTH_MASK;
				/*
				 * Checksum-enable bits ride in this same meta_ctrl
				 * word (they occupy bits 0-17; REQ_ID_LO is bits
				 * 31:22, no overlap). Mirrors the ena_tx_ctx
				 * meta_valid block (ena_eth_com.c:520-534). L3
				 * proto is set to IPv4 whenever L4 csum is enabled
				 * so the device can locate L4 even if the IPv4
				 * header csum itself was not requested. PART mode
				 * (l4_csum_partial=1) tells the device to ADD the
				 * payload sum to the pre-seeded pseudo-header field.
				 */
				if (csum_l3_en)
					meta_ctrl |=
					    ENA_ETH_IO_TX_DESC_L3_CSUM_EN_MASK;
				if (csum_l4_en) {
					meta_ctrl |= ((uint32_t)
					    ENA_ETH_IO_L3_PROTO_IPV4 &
					    ENA_ETH_IO_TX_DESC_L3_PROTO_IDX_MASK);
					meta_ctrl |= (((uint32_t)csum_l4_proto <<
					    ENA_ETH_IO_TX_DESC_L4_PROTO_IDX_SHIFT) &
					    ENA_ETH_IO_TX_DESC_L4_PROTO_IDX_MASK);
					meta_ctrl |=
					    ENA_ETH_IO_TX_DESC_L4_CSUM_EN_MASK;
					meta_ctrl |=
					    ENA_ETH_IO_TX_DESC_L4_CSUM_PARTIAL_MASK;
				}
			}
			if (i == ndesc - 1)
				len_ctrl |= ENA_ETH_IO_TX_DESC_LAST_MASK;

			desc = (struct ena_eth_io_tx_desc *)
			    (entry + (descbase + i) * ENA_TX_DESC_SIZE);
			desc->len_ctrl = htole32(len_ctrl);
			desc->meta_ctrl = htole32(meta_ctrl);
			desc->buff_addr_lo = htole32(addr_lo);
			desc->buff_addr_hi_hdr_sz = htole32(addr_hi_hdr);
		}
	}

	/* Inline header bytes follow the descriptors-before-header region. */
	if (txq->txq_llq && header_len > 0) {
		off = sc->sc_llq_descs_before_hdr * ENA_TX_DESC_SIZE;
		memcpy(entry + off, hdr_src, header_len);
	}

	/*
	 * Hand the payload buffer to the device: flush the CPU's view so the
	 * device DMA-read (behind the doorbell) sees current memory. TX is a
	 * device read, hence PREWRITE; pairs with POSTWRITE in ena_txeof.
	 */
	bus_dmamap_sync(sc->sc_dmat, map, 0, map->dm_mapsize,
	    BUS_DMASYNC_PREWRITE);

	if (txq->txq_llq) {
		bus_size_t woff = txq->txq_llq_off +
		    (bus_size_t)slot * sc->sc_llq_entry_size;

		/*
		 * Push the whole entry into the device LLQ window, then barrier
		 * those stores before the doorbell. bus_space_write_region_4
		 * writes entry_size/4 little-endian 32-bit words; the device
		 * window is little-endian like the rest of the BAR. (Mirrors
		 * __iowrite64_copy + wmb() in ena_com_write_bounce_buffer_to_dev,
		 * ena_eth_com.c:118-125.)
		 */
		bus_space_write_region_4(txq->txq_llq_t, txq->txq_llq_h, woff,
		    (const uint32_t *)entry, sc->sc_llq_entry_size / 4);
		bus_space_barrier(txq->txq_llq_t, txq->txq_llq_h, woff,
		    sc->sc_llq_entry_size, BUS_SPACE_BARRIER_WRITE);
	} else {
		/*
		 * Host-memory placement: the device's SQ tail counts
		 * DESCRIPTORS, not entries (unlike LLQ). Write each descriptor
		 * into its OWN ring slot (with ring wrap), carrying the PHASE
		 * valid for that slot, and PREWRITE-sync it. The entry holds
		 * ndesc data descriptors (no meta desc: csum offload is LLQ-only,
		 * so do_csum==0 here and descbase==0). The per-descriptor tail
		 * and phase are advanced here; the doorbell below rings that
		 * descriptor tail. Mirrors the host submission in
		 * ena_com_prepare_tx (ena_eth_com.c:241-258): one descriptor per
		 * slot, per-descriptor phase, tail += ndesc.
		 *
		 * (The old fallback memcpy'd ndesc descriptors into one slot and
		 * stepped the tail by 1 — correct only for single-descriptor
		 * packets; a multi-segment packet desynced the SQ tail/phase and
		 * the device raised DEV_STS FATAL.)
		 */
		struct ena_eth_io_tx_desc *ring =
		    (struct ena_eth_io_tx_desc *)ENA_DMA_KVA(&txq->txq_sq_dma);
		uint16_t t = txq->txq_sq_tail;
		uint8_t ph = txq->txq_sq_phase;

		for (i = 0; i < ndesc; i++) {
			struct ena_eth_io_tx_desc *src =
			    (struct ena_eth_io_tx_desc *)
			    (entry + i * ENA_TX_DESC_SIZE);
			uint16_t rs = t & qmask;
			uint32_t lc = letoh32(src->len_ctrl);

			/* Re-stamp PHASE for this descriptor's actual slot. */
			lc &= ~ENA_ETH_IO_TX_DESC_PHASE_MASK;
			lc |= ((uint32_t)ph <<
			    ENA_ETH_IO_TX_DESC_PHASE_SHIFT) &
			    ENA_ETH_IO_TX_DESC_PHASE_MASK;

			ring[rs].len_ctrl = htole32(lc);
			ring[rs].meta_ctrl = src->meta_ctrl;
			ring[rs].buff_addr_lo = src->buff_addr_lo;
			ring[rs].buff_addr_hi_hdr_sz = src->buff_addr_hi_hdr_sz;

			bus_dmamap_sync(sc->sc_dmat,
			    ENA_DMA_MAP(&txq->txq_sq_dma),
			    rs * ENA_TX_DESC_SIZE, ENA_TX_DESC_SIZE,
			    BUS_DMASYNC_PREWRITE);

			t++;
			if ((t & qmask) == 0)
				ph ^= 1;
		}

		/* Per-descriptor producer advance (host placement). */
		txq->txq_sq_tail = t;
		txq->txq_sq_phase = ph;
		txq->txq_sq_avail -= ndesc;	/* freed in ena_txeof */
	}

	/* Stash the mbuf + map for reclaim in ena_txeof, keyed by slot. The
	 * descriptor count lets ena_txeof return the SQ slots to txq_sq_avail. */
	txs->txs_m = m;
	txs->txs_ndesc = ndesc;

	/*
	 * Advance the SQ producer and ring the doorbell.
	 *
	 * LLQ: the device's SQ tail counts ENTRIES (one 128B entry per packet,
	 * regardless of how many 16-byte descriptors it packs). ena_com steps
	 * io_sq->tail once per entry (ena_com_write_bounce_buffer_to_dev,
	 * ena_eth_com.c:127); doorbell = entry tail (ena_eth_com.h:193). Our
	 * single-entry-per-packet invariant => tail steps by 1, phase flips on
	 * entry-ring wrap.
	 *
	 * HOST placement: the SQ tail counts DESCRIPTORS, so the host branch
	 * above already advanced txq_sq_tail by ndesc and stamped each
	 * descriptor's per-slot phase (ena_eth_com.c:241-258); here we only
	 * ring the doorbell with that descriptor tail.
	 *
	 * NOTE: txq_prod/txq_cons cap in-flight PACKETS at depth-1; host
	 * placement still needs a descriptor-occupancy guard before heavy
	 * multi-segment load can fill the SQ ring (TODO: gate accept on ndesc
	 * vs free descriptors). Light traffic (DHCP/SSH) cannot fill it.
	 */
	if (txq->txq_llq) {
		txq->txq_sq_tail++;
		if ((txq->txq_sq_tail & qmask) == 0)
			txq->txq_sq_phase ^= 1;
	}
	txq->txq_prod = (txq->txq_prod + 1) & qmask;

	ENA_REG_WR32_DB(sc, txq->txq_sq_db, txq->txq_sq_tail);

	return (0);
}

/*
 * Drain completed TX descriptors and reclaim their mbufs.
 *
 * Mirrors ena_tx_cleanup (ena_datapath.c:241-315) + ena_com_get_next_tx_cdesc
 * (ena_eth_com.c:685-712). Walk the TX CQ from the host consumer head: for each
 * cdesc whose PHASE bit matches the host phase, read req_id (the slot index we
 * stamped), POSTWRITE-sync and unload that slot's DMA map, free its mbuf, and
 * advance the CQ head (flipping phase on wrap). Bounded by ring depth.
 *
 * After the walk, clear oactive if we freed anything and re-arm the TX CQ
 * interrupt (the device masked it when it fired).
 *
 * Per-sync audit:
 *   TX CQ POSTREAD (before the phase-bit walk): the device DMA-wrote the
 *     completions; refresh the CPU view so the phase-bit + req_id reads observe
 *     device memory. Synced once up front over the whole ring (cheap).
 *   data buffer POSTWRITE (per packet, before unload): the device DMA-READ the
 *     payload; close the mapping that was opened PREWRITE in ena_encap. TX is a
 *     device read, so the closing direction is POSTWRITE.
 *   TX CQ PREREAD (after the walk): re-prime the read side for the consumed
 *     slots so the next interrupt's POSTREAD sees the device's future writes.
 *
 * Returns the number of packets reclaimed.
 */
int
ena_txeof(struct ena_txq *txq)
{
	struct ena_softc *sc = txq->txq_sc;
	struct ena_eth_io_tx_cdesc *ring, *cdesc;
	struct ena_tx_slot *txs;
	uint16_t qmask, idx, req_id;
	uint8_t phase, cphase;
	int txfree = 0;
	u_int bound;

	ring = (struct ena_eth_io_tx_cdesc *)ENA_DMA_KVA(&txq->txq_cq_dma);
	qmask = txq->txq_depth - 1;
	phase = txq->txq_cq_phase;

	/*
	 * POSTREAD the whole CQ ring once before the walk: order the reads of
	 * the phase bits and req_ids after the device's DMA writes.
	 */
	bus_dmamap_sync(sc->sc_dmat, ENA_DMA_MAP(&txq->txq_cq_dma), 0,
	    ENA_DMA_LEN(&txq->txq_cq_dma), BUS_DMASYNC_POSTREAD);

	for (bound = 0; bound < txq->txq_depth; bound++) {
		idx = txq->txq_cq_head & qmask;
		cdesc = &ring[idx];

		cphase = letoh16(cdesc->flags) & ENA_ETH_IO_TX_CDESC_PHASE_MASK;
		if (cphase != phase)
			break;

		req_id = letoh16(cdesc->req_id);

		/* Advance the CQ consumer; flip phase on ring wrap. */
		txq->txq_cq_head++;
		if ((txq->txq_cq_head & qmask) == 0)
			phase ^= 1;

		if (req_id >= txq->txq_depth) {
			printf("%s: TX bad req_id %u\n", sc->sc_dev.dv_xname,
			    req_id);
			continue;
		}

		txs = &txq->txq_slots[req_id];
		if (txs->txs_m == NULL) {
			printf("%s: TX req_id %u empty slot\n",
			    sc->sc_dev.dv_xname, req_id);
			continue;
		}

		/*
		 * Close the mapping the device DMA-read from, then free the
		 * mbuf. POSTWRITE because the device read our buffer.
		 */
		bus_dmamap_sync(sc->sc_dmat, txs->txs_map, 0,
		    txs->txs_map->dm_mapsize, BUS_DMASYNC_POSTWRITE);
		bus_dmamap_unload(sc->sc_dmat, txs->txs_map);
		m_freem(txs->txs_m);
		txs->txs_m = NULL;

		/* Return this packet's SQ descriptor slots (host placement). */
		if (!txq->txq_llq)
			txq->txq_sq_avail += txs->txs_ndesc;

		txq->txq_cons = (txq->txq_cons + 1) & qmask;
		txfree++;
	}

	txq->txq_cq_phase = phase;

	/*
	 * Re-prime the read side for the consumed slots so the next interrupt's
	 * POSTREAD observes the device's subsequent writes into them.
	 */
	bus_dmamap_sync(sc->sc_dmat, ENA_DMA_MAP(&txq->txq_cq_dma), 0,
	    ENA_DMA_LEN(&txq->txq_cq_dma), BUS_DMASYNC_PREREAD);

	if (txfree > 0 && ifq_is_oactive(txq->txq_ifq)) {
		ifq_clr_oactive(txq->txq_ifq);
		ifq_restart(txq->txq_ifq);
	}

	/*
	 * Do NOT re-arm the per-queue IO MSI-X vector here. ena_intr_io_queue drains TX
	 * (this function) then RX (ena_rx_intr -> ena_rxeof), and ena_rxeof issues
	 * a single unmask at the very end of the handler -- mirroring FreeBSD
	 * ena_cleanup. Re-arming mid-handler here too (a second unmask of the same
	 * shared vector) raced the device's per-vector mask and could leave the
	 * vector silent after the first burst (data path dies after ~20 packets).
	 */

	return (txfree);
}

/*
 * ena_init -- bring the interface up (Tasks 6/7: RX + TX queues).
 *
 * Create the IO RX queue (CREATE_CQ then CREATE_SQ), seed the if_rxr ring and
 * fill it with clusters, create the IO TX queue, then mark the interface
 * running. Called from ena_ioctl on IFF_UP && !IFF_RUNNING.
 */
int
ena_init(struct ena_softc *sc)
{
	struct ifnet *ifp = &sc->sc_ac.ac_if;
	unsigned int i;
	int error;

	if (ISSET(ifp->if_flags, IFF_RUNNING))
		return (0);

	/*
	 * Derive the RX cluster size from the active MTU and program the device
	 * MTU, both while the IO queues are still down. The RX buffer must hold
	 * the whole frame (mtu + Ethernet header + FCS) and never drop below the
	 * 2KB default; the device MTU is the plain L2-payload if_mtu. Doing both
	 * here from the same if_mtu, before ena_rx_create, guarantees the posted
	 * RX descriptor length and the device's frame-size limit always agree: a
	 * SET_FEATURE(MTU) larger than the RX buffers would let the device deliver
	 * a frame the host buffer cannot hold (drop/FATAL, no scatter fallback).
	 * Abort the bring-up if the device rejects the MTU.
	 */
	sc->sc_rx_buf_size = ifp->if_mtu + ETHER_HDR_LEN + ETHER_CRC_LEN;
	if (sc->sc_rx_buf_size < ENA_RX_BUF_SIZE)
		sc->sc_rx_buf_size = ENA_RX_BUF_SIZE;
	error = ena_set_mtu(sc, ifp->if_mtu);
	if (error != 0)
		return (error);

	/*
	 * Create each IO queue pair (CREATE_CQ then CREATE_SQ for RX, then TX)
	 * and seed each RX ring. On a 1-vCPU instance sc_nqueues is 1, identical
	 * to the pre-multiqueue path.
	 *
	 * if_rxr_init caps the live RX buffer count (low-water 2, the common
	 * OpenBSD idiom); ena_rx_fill posts as many as the ring allows. Cap the
	 * high-water at depth-1, never depth: ena-com never posts the last SQ
	 * slot, so depth descriptors would mask tail-next_to_comp to 0 and the
	 * device would read the ring as empty. Bound jumbo RX memory by clamping
	 * the budget to 256 when the buffer exceeds MCLBYTES (~9.5KB clusters at
	 * MTU 9000 would otherwise pin ~9.5MB per queue).
	 */
	for (i = 0; i < sc->sc_nqueues; i++) {
		struct ena_rxq *rxq = &sc->sc_rxq[i];
		u_int hi;

		timeout_set(&rxq->rxq_refill, ena_rx_refill, rxq);

		error = ena_rx_create(sc, i);
		if (error != 0)
			goto down;

		hi = rxq->rxq_depth - 1;
		if (sc->sc_rx_buf_size > MCLBYTES && hi > 256)
			hi = 256;
		if_rxr_init(&rxq->rxq_rxr, 2, hi);
		ena_rx_fill(rxq);

		error = ena_tx_create(sc, i);
		if (error != 0)
			goto down;
	}

	/*
	 * With more than one queue, program RSS so the device spreads RX across
	 * them (the indirection table uses the rxq_cq_idx values just filled by
	 * CREATE_CQ). Single-queue needs no hashing. Non-fatal on failure.
	 */
	if (sc->sc_nqueues > 1) {
		/*
		 * Multi-queue: the device hashes RX across the queues (its own
		 * default hash, or the explicit table ena_rss_config programs), so
		 * cdesc->hash is meaningful -- enable the RX flowid. Then best-effort
		 * program whatever RSS features the device lets the host set.
		 */
		sc->sc_rss_active = 1;
		(void)ena_rss_config(sc);
	} else
		sc->sc_rss_active = 0;

	SET(ifp->if_flags, IFF_RUNNING);
	ifq_clr_oactive(&ifp->if_snd);

	/*
	 * Re-assert the cached carrier from sc_link_up: the device sends a
	 * LINK_CHANGE AENQ event only on real transitions, not after a host-side
	 * down/up re-init, so without this an up'd interface would read
	 * "no carrier" even while traffic flows. ena_link_state pushes only if
	 * the state actually differs. THE down/up link-state fix.
	 */
	ena_link_state(sc);

	/*
	 * Arm the watchdog: refresh the keep-alive timestamp so a just-up
	 * interface is not seen as instantly stale, then start the 1 Hz tick
	 * (only when enabled at attach). ena_stop cancels it on the way down.
	 */
	sc->sc_keepalive_last = getuptime();
	if (sc->sc_wd_active)
		timeout_add_sec(&sc->sc_wd_tick, ENA_WATCHDOG_HZ);

	return (0);

down:
	/* Tear down whatever was created so a retried ena_init starts clean. */
	for (i = 0; i < sc->sc_nqueues; i++) {
		timeout_del(&sc->sc_rxq[i].rxq_refill);
		ena_tx_destroy(sc, i);
		ena_rx_destroy(sc, i);
	}
	return (error);
}

/*
 * ena_stop -- take the interface down (Tasks 6/7: RX + TX queues).
 *
 * Clear IFF_RUNNING (and oactive), cancel any deferred refill, and destroy the
 * TX queue then the RX queue. Each destroy issues DESTROY_SQ/DESTROY_CQ which
 * quiesces device DMA before the clusters/rings/mbufs are freed. Called from
 * ena_ioctl on !IFF_UP && IFF_RUNNING.
 */
void
ena_stop(struct ena_softc *sc)
{
	struct ifnet *ifp = &sc->sc_ac.ac_if;
	unsigned int i;

	CLR(ifp->if_flags, IFF_RUNNING);
	sc->sc_rss_active = 0;

	/*
	 * Quiesce each per-queue send ring (clear oactive + barrier) and barrier
	 * its IO interrupt, so no ena_start or ena_intr_io_queue is mid-flight
	 * against a queue about to be torn down. Mirrors igc_stop. On a 1-vCPU
	 * instance sc_nqueues is 1 and if_ifqs[0] is if_snd.
	 */
	for (i = 0; i < sc->sc_nqueues; i++) {
		ifq_clr_oactive(ifp->if_ifqs[i]);
		ifq_barrier(ifp->if_ifqs[i]);
		if (sc->sc_queues[i].tag != NULL)
			intr_barrier(sc->sc_queues[i].tag);
	}

	/*
	 * Cancel the watchdog callout so it stops sampling while the interface
	 * is down (ena_init re-arms on the next up). Plain timeout_del, not
	 * _barrier: ena_tick only reads MMIO/timestamps, sets a flag, and logs --
	 * it never touches the rings being torn down below, so it is safe to let
	 * a final in-flight tick complete.
	 */
	timeout_del(&sc->sc_wd_tick);
	for (i = 0; i < sc->sc_nqueues; i++) {
		timeout_del(&sc->sc_rxq[i].rxq_refill);
		ena_tx_destroy(sc, i);
		ena_rx_destroy(sc, i);
	}
}

/*
 * ena_tick -- watchdog callout (Increment 1: DETECTION ONLY, no reset).
 *
 * Runs in SOFTCLOCK at ENA_WATCHDOG_HZ. It MUST NOT sleep, take NET_LOCK,
 * poll the admin queue, or reset the device. It only: (1) checks keep-alive
 * staleness (when KEEP_ALIVE was subscribed), (2) does a single softclock-legal
 * DEV_STS MMIO read for FATAL_ERROR / loss of READY, sets ENA_FLAG_TRIGGER_RESET
 * and logs on a fault, then (3) always re-arms. Increment 2 will task_add the
 * actual reset off the flag; here detection cannot wedge the device.
 */
void
ena_tick(void *xsc)
{
	struct ena_softc *sc = xsc;
	struct ifnet *ifp = &sc->sc_ac.ac_if;
	uint32_t sts, delta;

	if (!ISSET(ifp->if_flags, IFF_RUNNING))
		return;	/* not armed when down; ena_init re-arms on next up */

	/*
	 * Skip all device access while a reset is mid-rebuild. The reset task
	 * reads/writes device registers through the single readless-MMIO response
	 * region; a concurrent DEV_STS read here would corrupt that shared slot,
	 * and reading the transient FATAL/!READY of a device mid-reset would set
	 * ENA_FLAG_TRIGGER_RESET in a race with the task clearing it -> reset loop.
	 * The reset re-arms the tick (via ena_init) when it brings the queues back.
	 */
	if (ISSET(sc->sc_flags, ENA_FLAG_RESETTING)) {
		timeout_add_sec(&sc->sc_wd_tick, ENA_WATCHDOG_HZ);
		return;
	}

	/*
	 * Drain the AENQ from the tick (FreeBSD ena_timer_service style). On the
	 * native EC2 VF the mgmt MSI-X stops delivering after early boot, so the
	 * keep-alive AENQ would otherwise go unserviced and the device's own
	 * liveness watchdog faults it (DEV_STS FATAL). Draining here every second
	 * keeps keep-alive serviced + the head doorbell rung regardless of the
	 * mgmt interrupt. ena_aenq_intr takes sc_aenq_mtx to serialize vs the ISR.
	 */
	ena_aenq_intr(sc);

	/*
	 * Post-reset cooldown: for ENA_WD_COOLDOWN_S after a recovery the device
	 * re-stabilises and DEV_STS reads transient FATAL/!READY. Watchdog'ing in
	 * that window re-triggers a reset before the device settles -> a reset
	 * storm (which can physically wedge the device). Stay quiet until it ends;
	 * keep-alive timestamps still update from the AENQ ISR meanwhile.
	 */
	if ((uint32_t)getuptime() < sc->sc_wd_cooldown) {
		sc->sc_wd_fatal_count = 0;
		/*
		 * Keep the keep-alive baseline fresh through the cooldown: the device's
		 * keep-alive cadence is jittery for several seconds after a reset, so a
		 * stale timestamp would trip the keep-alive check the instant the
		 * cooldown lifts. The real AENQ keep-alive ISR overwrites this with its
		 * own timestamp as events resume.
		 */
		sc->sc_keepalive_last = getuptime();
		timeout_add_sec(&sc->sc_wd_tick, ENA_WATCHDOG_HZ);
		return;
	}

	/* keep-alive staleness (only if KEEP_ALIVE was negotiated) */
	if (sc->sc_keepalive_timeo != 0) {
		delta = (uint32_t)getuptime() - sc->sc_keepalive_last;
		if (delta > sc->sc_keepalive_timeo) {
			SET(sc->sc_flags, ENA_FLAG_TRIGGER_RESET);
			printf("%s: watchdog: no keep-alive for %us\n",
			    sc->sc_dev.dv_xname, delta);
		}
	}

	/*
	 * Device-fatal: a single softclock-legal MMIO read, DEBOUNCED. A device
	 * is transiently FATAL/!READY for a tick or two right after a reset while
	 * it re-stabilises; reacting to that would start a reset storm (each reset
	 * induces the next). Require ENA_WD_FATAL_THRESH consecutive bad reads --
	 * a genuinely wedged device stays bad, a recovering one clears within one
	 * or two ticks.
	 */
	sts = ENA_REG_RD32(sc, ENA_REGS_DEV_STS_OFF);
	if (sts == 0xffffffff ||
	    (sts & ENA_REGS_DEV_STS_FATAL_ERROR_MASK) ||
	    !(sts & ENA_REGS_DEV_STS_READY_MASK)) {
		if (++sc->sc_wd_fatal_count >= ENA_WD_FATAL_THRESH) {
			SET(sc->sc_flags, ENA_FLAG_TRIGGER_RESET);
			printf("%s: watchdog: device fatal (sts=0x%x)\n",
			    sc->sc_dev.dv_xname, sts);
		}
	} else
		sc->sc_wd_fatal_count = 0;

	/*
	 * INCREMENT 2: enqueue the reset off the trigger flag. Guarded so it
	 * fires once and never while a reset is mid-rebuild (RESETTING) or the
	 * device is detaching (DYING). The reset task clears TRIGGER_RESET when
	 * it runs; the tick keeps running regardless. ena_tick stays softclock-
	 * safe -- it only flags, task_add's, and re-arms (no sleep/NET_LOCK/
	 * admin/reset).
	 */
	if (0 /* XXX TEST: auto-reset gated off — detect+log FATAL, no reset loop */ &&
	    ISSET(sc->sc_flags, ENA_FLAG_TRIGGER_RESET) &&
	    !ISSET(sc->sc_flags, ENA_FLAG_RESETTING) &&
	    !ISSET(sc->sc_flags, ENA_FLAG_DYING))
		task_add(systq, &sc->sc_reset_task);

	timeout_add_sec(&sc->sc_wd_tick, ENA_WATCHDOG_HZ);	/* always re-arm */
}

/*
 * ena_destroy_device -- quiesce the (possibly wedged) device before a rebuild.
 *
 * Process context, NET_LOCK held (called from ena_reset_task). Records whether
 * the interface was up so ena_restore_device knows whether to bring IO back,
 * masks the AENQ/mgmt ISR window (mirroring ena_detach), tears the IO queues
 * down if they were live, then runs the reset primitive. ena_stop tolerates an
 * admin-poll timeout on a wedged device; ena_reset busy-polls up to ~1s, which
 * is legal in process context (and would be illegal in the softclock tick).
 *
 * What it does NOT touch: the admin AQ/ACQ rings, the AENQ ring, the MMIO resp
 * region, the host-attr page, and the BAR2 LLQ window all persist across the
 * reset (allocated/mapped once at attach, freed only at detach). ena_reset
 * does not free or unmap any of them, and ena_restore_device re-programs the
 * device from those same persistent buffers -- so destroy must NOT free them
 * (see ena_restore_device's idempotency note).
 */
void
ena_destroy_device(struct ena_softc *sc, int *was_up)
{
	struct ifnet *ifp = &sc->sc_ac.ac_if;

	*was_up = ISSET(ifp->if_flags, IFF_RUNNING);

	/*
	 * Close the AENQ/mgmt ISR window before resetting, exactly as ena_detach
	 * does (ENA_REGS_ADMIN_INTR_MASK masks the management interrupt). The
	 * device is about to be reset; no AENQ events should be serviced against
	 * a ring the reset is dropping.
	 */
	ENA_REG_WR32(sc, ENA_REGS_INTR_MASK_OFF, ENA_REGS_ADMIN_INTR_MASK);

	/*
	 * If the interface was up, take the IO queues down over the still-live
	 * admin queue (DESTROY_SQ/CQ). On a wedged device the admin polls may
	 * time out; ena_stop tolerates that and still clears IFF_RUNNING and
	 * frees the IO rings/clusters.
	 */
	if (*was_up)
		ena_stop(sc);

	/* The reset primitive; busy-polls up to ~1s (legal in process ctx). */
	ena_reset(sc);
}

/*
 * ena_restore_device -- replay the attach bring-up after a reset.
 *
 * Process context, NET_LOCK held. Re-runs device+admin+AENQ init, re-commits
 * LLQ, refreshes the keep-alive timestamp, and brings IO back if it had been
 * up. Returns 0 on success, 1 on any failure (caller logs "device down").
 *
 * IDEMPOTENCY (the critical correctness point):
 *   ena_admin_init is SAFE TO CALL A SECOND TIME. It allocates NOTHING: it
 *   only resets host-side scalar ring state (sc_aq_tail/sc_acq_head/phases/
 *   cmd_id) and re-programs the AQ/ACQ/AENQ BAR registers from the DMA buffers
 *   ena_attach allocated once (sc_aq_dma, sc_acq_dma, sc_aenq_dma) -- buffers
 *   that ena_reset/ena_stop never free. ena_aenq_register (called inside
 *   ena_admin_init) likewise only re-seeds scalars + re-programs the AENQ ring
 *   base/caps; it allocates nothing. Therefore ena_destroy_device must NOT free
 *   admin/AENQ DMA state (and it does not): if it did, this re-init would
 *   re-program the BAR from freed memory. Re-running it is exactly the intended
 *   "re-init the admin/AENQ rings" behaviour.
 *
 *   ena_set_host_attr re-uses its already-allocated page (alloc guarded on
 *   edm_map == NULL); ena_get_dev_attr is a harmless re-read of MAC/MTU/offload
 *   caps (we do NOT re-run ifmedia/if_attach); ena_tx_llq_setup re-commits LLQ
 *   over the still-mapped BAR2; ena_aenq_init re-subscribes groups, unmasks the
 *   mgmt INTR and rings the AENQ head doorbell, refreshing sc_aenq_groups.
 *
 *   ena_reset already does the MMIO resp-addr write internally (ena_mmio_resp_
 *   write, after the reset trigger), so restore starts at ena_admin_init, not
 *   at the resp write.
 */
int
ena_restore_device(struct ena_softc *sc, int was_up)
{
	if (ena_admin_init(sc) != 0)
		goto fail;
	if (ena_set_host_attr(sc) != 0)
		goto fail;
	if (ena_get_dev_attr(sc) != 0)	/* re-reads MAC/MTU/offload; harmless */
		goto fail;
	if (ena_tx_llq_setup(sc) != 0)	/* CORRECTION 1: re-commit LLQ post-reset */
		goto fail;
	if (ena_aenq_init(sc) != 0)	/* re-subscribe, unmask mgmt INTR, ring DB */
		goto fail;

	/* Fresh stamp so the watchdog does not see the device as instantly stale. */
	sc->sc_keepalive_last = getuptime();

	if (was_up) {
		/*
		 * Re-create the IO queues (CREATE_CQ/SQ), re-assert link state and
		 * re-arm the tick -- the same path a manual down/up takes. NB: the IO
		 * data-path restore after a reset cannot be validated on the nested-VFIO
		 * test harness, where a post-reset boot re-rolls the host-side MSI-X
		 * delivery race that intermittently drops the IO interrupt; confirming
		 * recovery end-to-end needs native ena0 on real EC2.
		 */
		if (ena_init(sc) != 0)	/* re-create RX/TX queues, link state, tick */
			goto fail;
	} else {
		ena_link_state(sc);	/* re-assert carrier even if admin-only */
	}

	/*
	 * Give the freshly-rebuilt device a cooldown before the watchdog reads it
	 * again: DEV_STS reads transient FATAL/!READY for a few seconds post-reset,
	 * and re-triggering in that window storms (and can wedge the device).
	 */
	sc->sc_wd_fatal_count = 0;
	sc->sc_wd_cooldown = (uint32_t)getuptime() + ENA_WD_COOLDOWN_S;
	return (0);
fail:
	return (1);
}

/*
 * ena_reset_task -- process-context device reset/recovery (the watchdog's
 * action). Enqueued on systq by ena_tick when ENA_FLAG_TRIGGER_RESET is set.
 * May sleep; takes NET_LOCK because ena_init/ena_stop are ifnet-path functions
 * that expect it held. Single-entry: RESETTING guards re-entry, DYING aborts a
 * reset racing detach, and the task clears TRIGGER_RESET on the way out.
 */
void
ena_reset_task(void *xsc)
{
	struct ena_softc *sc = xsc;
	int was_up;

	NET_LOCK();
	if (ISSET(sc->sc_flags, ENA_FLAG_DYING) ||
	    !ISSET(sc->sc_flags, ENA_FLAG_TRIGGER_RESET) ||
	    ISSET(sc->sc_flags, ENA_FLAG_RESETTING)) {
		NET_UNLOCK();
		return;
	}
	SET(sc->sc_flags, ENA_FLAG_RESETTING);
	printf("%s: resetting device (recovery)\n", sc->sc_dev.dv_xname);
	ena_destroy_device(sc, &was_up);
	if (ena_restore_device(sc, was_up) != 0) {
		printf("%s: reset recovery failed; device down\n",
		    sc->sc_dev.dv_xname);
		CLR(sc->sc_flags, ENA_FLAG_TRIGGER_RESET | ENA_FLAG_RESETTING);
		NET_UNLOCK();
		return;
	}
	CLR(sc->sc_flags, ENA_FLAG_TRIGGER_RESET | ENA_FLAG_RESETTING);
	NET_UNLOCK();
}

/*
 * ena_start -- drain the send queue onto the TX ring.
 *
 * Mirrors mcx_start (if_mcx.c) / the OpenBSD if_qstart idiom: dequeue mbufs,
 * encap each onto the TX queue, and stop (requeueing the last mbuf and setting
 * oactive) when the ring fills. bpf-taps each transmitted packet. ena_txeof
 * clears oactive and restarts the queue when completions free slots.
 */
void
ena_start(struct ifqueue *ifq)
{
	struct ifnet *ifp = ifq->ifq_if;
	struct ena_txq *txq = ifq->ifq_softc;
	struct mbuf *m;
	uint16_t qmask;
	int error;

	/*
	 * Only drain if the TX queue exists. We intentionally do NOT gate on
	 * sc_link_up: an ARP/packet enqueued before the AENQ LINK_CHANGE event
	 * is observed must still be posted (the device drops frames internally
	 * while the link is down). Gating on link state here would race the
	 * AENQ and could drop the first ARP (the Phase-1 checkpoint packet).
	 */
	if (!txq->txq_created) {
		ifq_purge(ifq);
		return;
	}

	qmask = txq->txq_depth - 1;

	for (;;) {
		/*
		 * Stop before the ring is full (keep one packet slot free). For
		 * host placement also stop when fewer than ENA_TX_MAX_SEGS SQ
		 * descriptor slots remain free, so any packet we dequeue is
		 * guaranteed to fit (a multi-segment packet uses up to
		 * ENA_TX_MAX_SEGS descriptors; LLQ packs one entry per packet and
		 * leaves txq_sq_avail unused).
		 */
		if (((txq->txq_prod + 1) & qmask) == (txq->txq_cons & qmask) ||
		    (!txq->txq_llq && txq->txq_sq_avail < ENA_TX_MAX_SEGS)) {
			ifq_set_oactive(ifq);
			break;
		}

		m = ifq_dequeue(ifq);
		if (m == NULL)
			break;

		/*
		 * bpf-tap before encap: ena_encap may m_defrag (rewriting the
		 * chain) and ultimately hands the mbuf to the device, so tap the
		 * outbound packet while we still own it intact. Matches the
		 * pre-load tap point in mcx_start (if_mcx.c:7900-7911).
		 */
#if NBPFILTER > 0
		if (ifp->if_bpf != NULL)
			bpf_mtap(ifp->if_bpf, m, BPF_DIRECTION_OUT);
#endif

		error = ena_encap(txq, m);
		if (error == ENOBUFS) {
			/*
			 * Ring filled between the pre-check and encap (should not
			 * happen — both gate on the same prod/cons condition —
			 * but handle defensively). Drop rather than risk an
			 * out-of-order requeue; mark oactive and stop.
			 */
			m_freem(m);
			ifp->if_oerrors++;
			ifq_set_oactive(ifq);
			break;
		}
		if (error != 0) {
			/* Undeliverable packet (map/defrag failure): drop it. */
			m_freem(m);
			ifp->if_oerrors++;
			continue;
		}
	}
}

/*
 * ena_link_state -- push the cached sc_link_up to the ifnet layer.
 *
 * Single funnel for carrier changes: it sets if_link_state from sc_link_up
 * (full-duplex when up, down otherwise) and only calls if_link_state_change
 * (which itself defers the heavy work via task_add) when the state actually
 * changed. Called from the AENQ LINK_CHANGE path (after the drain loop) and
 * from ena_init to re-assert the cached carrier after a host re-init (the
 * device does not re-send LINK_CHANGE after a down/up, so without this an
 * up'd interface would read "no carrier" even while traffic flows). Runs in
 * process context (ena_init) or the post-drain mgmt-ISR tail, mirroring
 * vio_link_state (if_vio.c:963-978).
 */
void
ena_link_state(struct ena_softc *sc)
{
	struct ifnet *ifp = &sc->sc_ac.ac_if;
	int nstate = sc->sc_link_up ? LINK_STATE_FULL_DUPLEX : LINK_STATE_DOWN;

	if (ifp->if_link_state != nstate) {
		ifp->if_link_state = nstate;
		if_link_state_change(ifp);
	}
}

/*
 * ena_media_status -- report link state via AENQ sc_link_up.
 *
 * IFM_AVALID is always set (the interface is present). IFM_ACTIVE is OR'd
 * in when the last AENQ LINK_CHANGE event reported the link as up. Mirrors
 * mcx_media_status (if_mcx.c:8080-8085) simplified for the case where the
 * link state is already cached in sc_link_up by the AENQ handler.
 */
void
ena_media_status(struct ifnet *ifp, struct ifmediareq *ifmr)
{
	struct ena_softc *sc = ifp->if_softc;

	ifmr->ifm_status = IFM_AVALID;
	ifmr->ifm_active = IFM_ETHER | IFM_AUTO;
	if (sc->sc_link_up)
		ifmr->ifm_status |= IFM_ACTIVE;
}

/*
 * ena_media_change -- ifmedia change callback.
 *
 * ENA does not expose a user-selectable link mode (speed/duplex are
 * negotiated by the hypervisor). Accept any ifmedia_set call silently.
 * Mirrors vio_media_change (if_vio.c:1020-1023).
 */
int
ena_media_change(struct ifnet *ifp)
{
	return (0);
}

/*
 * ena_ioctl -- interface control entry point.
 *
 * Handles the standard OpenBSD ethernet ioctl set:
 *   SIOCSIFFLAGS: UP&!RUNNING -> ena_init; !UP&RUNNING -> ena_stop.
 *   SIOCGIFMEDIA/SIOCSIFMEDIA: delegated to ifmedia_ioctl.
 *   Everything else: delegated to ether_ioctl.
 *   ENETRESET: no filter/multicast reprogramming until Tasks 6/7; cleared.
 *
 * Mirrors mcx_ioctl (if_mcx.c:7590-7711) and vio_ioctl (if_vio.c:1375-1431),
 * simplified to the minimal set needed before TX/RX queues exist.
 */
int
ena_ioctl(struct ifnet *ifp, u_long cmd, caddr_t data)
{
	struct ena_softc *sc = ifp->if_softc;
	struct ifreq *ifr = (struct ifreq *)data;
	int s, error = 0;

	s = splnet();
	switch (cmd) {
	case SIOCSIFADDR:
		ifp->if_flags |= IFF_UP;
		/* FALLTHROUGH */

	case SIOCSIFFLAGS:
		if (ISSET(ifp->if_flags, IFF_UP)) {
			if (!ISSET(ifp->if_flags, IFF_RUNNING))
				error = ena_init(sc);
		} else {
			if (ISSET(ifp->if_flags, IFF_RUNNING))
				ena_stop(sc);
		}
		break;

	case SIOCGIFMEDIA:
	case SIOCSIFMEDIA:
		error = ifmedia_ioctl(ifp, ifr, &sc->sc_media, cmd);
		break;

	case SIOCSIFMTU:
		/*
		 * OpenBSD ether_ioctl handles SIOCSIFMTU by setting if_mtu and
		 * returning 0 (not ENETRESET), so the driver must act on the change
		 * itself. Bound it against if_hardmtu (= the device max, advertised
		 * at attach) exactly as ether_ioctl would, then, only on an actual
		 * change while running, bounce the interface: ena_init rebuilds the
		 * RX rings/DMA maps at the new buffer size and reprograms the device
		 * MTU, both from the new if_mtu. ena_stop must precede ena_init,
		 * which early-returns while IFF_RUNNING is set; a change while down
		 * just records if_mtu and takes effect on the next IFF_UP.
		 */
		if (ifr->ifr_mtu < ETHERMIN || ifr->ifr_mtu > ifp->if_hardmtu) {
			error = EINVAL;
			break;
		}
		if (ifp->if_mtu != ifr->ifr_mtu) {
			u_int omtu = ifp->if_mtu;

			ifp->if_mtu = ifr->ifr_mtu;
			if (ISSET(ifp->if_flags, IFF_RUNNING)) {
				ena_stop(sc);
				error = ena_init(sc);
				if (error != 0) {
					/*
					 * The device rejected the new MTU, or the
					 * rebuild failed. Roll back to the old MTU and
					 * bring the interface back up so a bad MTU does
					 * not strand a previously-running interface.
					 */
					ifp->if_mtu = omtu;
					(void)ena_init(sc);
				}
			}
		}
		break;

	default:
		error = ether_ioctl(ifp, &sc->sc_ac, cmd, data);
		break;
	}

	/*
	 * ENETRESET: multicast filter or address list changed. Tasks 6/7 will
	 * reprogram the device filter here; for now just clear the error so
	 * the caller does not see ENETRESET propagated.
	 */
	if (error == ENETRESET)
		error = 0;

	splx(s);
	return (error);
}
