/*
 * virtio.h -- legacy virtio-pci transport for SCO OpenServer 5.0.7
 *
 * Device-agnostic. Implements the virtio 0.9.5 ("legacy") PCI transport,
 * which is a pure port-I/O interface with a 32-bit page-frame-number queue
 * address -- the reason virtio is tractable at all on this vintage of DDI.
 * No MMIO capability walking, no 64-bit DMA, no MSI-X.
 *
 * *** PRE-ANSI (K&R) C ***
 * This builds with the in-box Link Kit toolchain (idcpp/idcomp/idas) on a
 * stock OpenServer 5.0.7 -- no Development System licence needed. That means:
 *
 *	- old-style function declarations, no prototypes
 *	- old-style (K&R) function definitions
 *	- no `volatile' -- idcomp rejects it. Reads and writes of memory the
 *	  device changes behind our back live in virtio_mb.c, a separate
 *	  translation unit, so the compiler cannot cache across them.
 *	- no // comments
 *
 * `void' and `void *' ARE supported and are used freely.
 *
 * Copyright (c) 2026. Released under the MIT license.
 */

#ifndef _VIRTIO_H
#define _VIRTIO_H

#include <sys/types.h>
#include <sys/pci.h>

/* ------------------------------------------------------------------ */
/* PCI identity                                                        */
/* ------------------------------------------------------------------ */

#define VIRTIO_PCI_VENDOR_ID	0x1AF4

/*
 * I/O BAR address mask. We define our own rather than use
 * PCI_BASE_ADDRESS_IO_MASK from <sys/pci.h>: that is written (~0x03UL),
 * and the `UL' integer suffix is ANSI -- idcomp rejects it outright.
 * ~3L is a long, which the compiler converts to ulong when masking.
 */
#define VIRTIO_IOBAR_MASK	(~3L)

/*
 * Virtio device types. A transitional device carries its type in the PCI
 * subsystem ID, which is a more reliable discriminator than the device ID
 * (console is PCI 0x1003 but type 3; balloon is 0x1002 but type 5 -- the
 * mapping is not an offset). See virtio_pci_find().
 */
#define VIRTIO_ID_NET		1
#define VIRTIO_ID_BLOCK		2
#define VIRTIO_ID_CONSOLE	3
#define VIRTIO_ID_RNG		4
#define VIRTIO_ID_BALLOON	5
#define VIRTIO_ID_SCSI		8

/* ------------------------------------------------------------------ */
/* Legacy virtio-pci register map, relative to BAR0 (I/O space).       */
/* Offsets are valid only while MSI-X is disabled, which we never      */
/* enable -- OpenServer has no MSI-X support to speak of.              */
/* ------------------------------------------------------------------ */

#define VIRTIO_PCI_HOST_FEATURES	0x00	/* 32 r/o  */
#define VIRTIO_PCI_GUEST_FEATURES	0x04	/* 32 r/w  */
#define VIRTIO_PCI_QUEUE_PFN		0x08	/* 32 r/w  */
#define VIRTIO_PCI_QUEUE_NUM		0x0C	/* 16 r/o  */
#define VIRTIO_PCI_QUEUE_SEL		0x0E	/* 16 r/w  */
#define VIRTIO_PCI_QUEUE_NOTIFY		0x10	/* 16 r/w  */
#define VIRTIO_PCI_STATUS		0x12	/*  8 r/w  */
#define VIRTIO_PCI_ISR			0x13	/*  8 r/o, clear-on-read */
#define VIRTIO_PCI_CONFIG_OFF		0x14	/* device-specific config */

/* Device status bits */
#define VIRTIO_CONFIG_S_ACKNOWLEDGE	0x01
#define VIRTIO_CONFIG_S_DRIVER		0x02
#define VIRTIO_CONFIG_S_DRIVER_OK	0x04
#define VIRTIO_CONFIG_S_FAILED		0x80

/* ISR bits */
#define VIRTIO_PCI_ISR_QUEUE		0x01
#define VIRTIO_PCI_ISR_CONFIG		0x02

/* Transport feature bits common to all devices */
#define VIRTIO_F_NOTIFY_ON_EMPTY	24
#define VIRTIO_F_ANY_LAYOUT		27
#define VIRTIO_RING_F_INDIRECT_DESC	28
#define VIRTIO_RING_F_EVENT_IDX		29

/* ------------------------------------------------------------------ */
/* vring layout -- must match the hypervisor's byte-for-byte.          */
/* ------------------------------------------------------------------ */

#define VIRTIO_PCI_VRING_ALIGN	4096

/*
 * The descriptor address is 64-bit in the spec. We split it into two
 * 32-bit halves rather than using long long: this compiler predates C99
 * and the layout is identical either way. vd_addr_hi is always written as
 * zero -- SCO 5.0.7 cannot address memory above 4GB.
 */
struct vring_desc {
	ulong	vd_addr_lo;
	ulong	vd_addr_hi;
	ulong	vd_len;
	ushort	vd_flags;
	ushort	vd_next;
};

#define VRING_DESC_F_NEXT	1	/* chains to vd_next */
#define VRING_DESC_F_WRITE	2	/* device writes (driver reads) */
#define VRING_DESC_F_INDIRECT	4	/* not used in v1 */

struct vring_avail {
	ushort	va_flags;
	ushort	va_idx;
	ushort	va_ring[1];	/* actually [vq_num]; used_event follows */
};

#define VRING_AVAIL_F_NO_INTERRUPT	1

struct vring_used_elem {
	ulong	vue_id;		/* head descriptor index */
	ulong	vue_len;	/* bytes written by the device */
};

struct vring_used {
	ushort			vu_flags;
	ushort			vu_idx;
	struct vring_used_elem	vu_ring[1];	/* actually [vq_num] */
};

#define VRING_USED_F_NO_NOTIFY		1

/* ------------------------------------------------------------------ */
/* Driver-side bookkeeping                                             */
/* ------------------------------------------------------------------ */

/*
 * Max descriptors in one chain. Must cover a full OSR5 scatter/gather list
 * plus the protocol headers: SCSI_MAX_SG is NBCLUSTER (16) on 5.0.7, and
 * virtio-scsi adds a request and a response descriptor, so 18 is the real
 * floor. 34 leaves headroom without approaching the queue size (256) or the
 * device's advertised seg_max (254).
 */
#define VIRTIO_MAX_SG	34	/* max descriptors in one chain */

struct virtio_vq {
	ushort	vq_index;	/* queue selector */
	ushort	vq_num;		/* queue size, from QUEUE_NUM */
	caddr_t	vq_ringmem;	/* base of the contiguous allocation */
	ulong	vq_ringpfn;	/* its physical page frame number */
	int	vq_npages;	/* pages allocated, for freecpages() */

	struct vring_desc  *vq_desc;
	struct vring_avail *vq_avail;
	struct vring_used  *vq_used;

	ushort	vq_free_head;	/* head of the free-descriptor list */
	ushort	vq_num_free;	/* descriptors currently free */
	ushort	vq_last_used;	/* last vu_idx we have consumed */
	void	**vq_cookies;	/* caller token, indexed by head desc */
};

struct virtio_dev {
	struct pci_devinfo vd_pci;
	ulong	vd_iobase;	/* BAR0, I/O space */
	ushort	vd_devid;	/* PCI device ID */
	ushort	vd_subsysid;	/* PCI subsystem ID == virtio device type */
	unchar	vd_irq;		/* PCI interrupt line */
	ulong	vd_features;	/* negotiated feature bits */
};

#define VIRTIO_HAS_FEATURE(vd, bit)	(((vd)->vd_features >> (bit)) & 1)

/*
 * Direct-mapped kernel virtual address -> physical address. Valid only for
 * memory obtained with MEM_KVMAPPED (e.g. from getcpages()), which is
 * guaranteed one-to-one mapped. KVBASE comes from <sys/immu.h>.
 */
#define VIRTIO_KVTOPHYS(v)	((paddr_t)((ulong)(v) - KVBASE))

/*
 * The reverse. Only valid for physical addresses that lie in the direct-
 * mapped range -- which is the case for kernel buffers SCO hands a driver,
 * but is NOT a general-purpose phys-to-virt.
 */
#define VIRTIO_PHYSTOKV(p)	((caddr_t)((ulong)(p) + KVBASE))

/* ------------------------------------------------------------------ */
/* Transport API -- old-style declarations (no prototypes: K&R)        */
/* ------------------------------------------------------------------ */

/*
 * virtio_pci_find(virtio_type, index, vd)
 *	Locate the index'th virtio device of the given type, matching on
 *	subsystem ID. Returns 1 if found, 0 otherwise -- mirroring
 *	pci_finddevice(D3oddi). Initialization context only.
 */
extern int virtio_pci_find();

/* Device status handshake */
extern void   virtio_reset();		/* (vd) */
extern void   virtio_add_status();	/* (vd, bits) */
extern unchar virtio_get_status();	/* (vd) */
extern void   virtio_set_failed();	/* (vd) */

/*
 * virtio_negotiate(vd, driver_features)
 *	Stores the intersection of device and driver features in
 *	vd_features and publishes it to the device.
 */
extern ulong virtio_host_features();	/* (vd) */
extern void  virtio_negotiate();	/* (vd, driver_features) */

/* Device-specific config space (past VIRTIO_PCI_CONFIG_OFF) */
extern unchar virtio_cfg_readb();	/* (vd, offset) */
extern ushort virtio_cfg_readw();	/* (vd, offset) */
extern ulong  virtio_cfg_readl();	/* (vd, offset) */

/*
 * virtio_vq_setup(vd, vq, index)
 *	Allocates physically contiguous ring memory with getcpages() and
 *	hands its PFN to the device. Returns 1 on success, 0 on failure
 *	(queue absent, out of memory, ring too large).
 *	Initialization context only -- getcpages() may sleep.
 */
extern int  virtio_vq_setup();		/* (vd, vq, index) */
extern void virtio_vq_free();		/* (vq) */

/*
 * virtio_vq_add(vq, pa, len, nout, nin, cookie)
 *	Enqueue a descriptor chain: nout device-readable segments followed
 *	by nin device-writable ones. 'cookie' comes back from
 *	virtio_vq_get() on completion. Returns 1, or 0 if the ring is full.
 *	Caller must hold the driver's lock / be at the right spl.
 */
extern int  virtio_vq_add();
extern void virtio_vq_notify();		/* (vd, vq) */

/*
 * virtio_vq_get(vq, lenp)
 *	Reap one completed chain: returns its cookie and stores the byte
 *	count the device reported in *lenp, or NULL if nothing is complete.
 */
extern void *virtio_vq_get();		/* (vq, lenp) */

/* Interrupt-related helpers */
extern unchar virtio_isr_ack();		/* (vd) -- read-to-clear */
extern void   virtio_vq_disable_intr();	/* (vq) */
extern void   virtio_vq_enable_intr();	/* (vq) */

/* ------------------------------------------------------------------ */
/* Device-memory accessors -- defined in virtio_mb.c                   */
/*                                                                     */
/* These exist because idcomp has no `volatile'. Keeping them in a      */
/* separate translation unit means the compiler cannot see their        */
/* bodies, so it cannot cache a value the device has since changed,     */
/* nor reorder stores across the call. That is a stronger guarantee     */
/* than `volatile' would have given us. Do NOT move them into          */
/* virtio.c, and do NOT compile the two files together as one unit.     */
/* ------------------------------------------------------------------ */

extern ushort virtio_read_used_idx();	/* (vq) */
extern void   virtio_publish_avail();	/* (vq, head) */
extern void   virtio_barrier();		/* () */

#endif /* _VIRTIO_H */
