/*
 * virtio.c -- legacy virtio-pci transport for SCO OpenServer 5.0.7
 *
 * See virtio.h for the API contract and for why this is pre-ANSI (K&R) C.
 * Builds with the in-box Link Kit toolchain on a stock OpenServer 5.0.7:
 *
 *	/lib/idcpp  virtio.c > virtio.i
 *	/lib/idcomp < virtio.i > virtio.s	(diagnostics on stderr!)
 *	/bin/idas   virtio.s			(writes virtio.o)
 *
 * Copyright (c) 2026. Released under the MIT license.
 */

#ifndef _INKERNEL
#define _INKERNEL 1		/* unlocks the kernel prototypes in pci.h */
#endif

#include <sys/types.h>
#include <sys/param.h>
#include <sys/immu.h>
#include <sys/sysmacros.h>
#include <sys/pci.h>
#include <sys/kmem.h>
#include <sys/cmn_err.h>

#include "virtio.h"

/*
 * Port I/O. SCO declares these old-style in <sys/inline.h>. Argument order
 * for the out* family is (port, value) -- confirmed by that header's
 *	#define outp(addr,value) outb((addr),(value))
 *
 * kmem_zalloc/kmem_free and getcpages/freecpages are already declared by
 * <sys/kmem.h> and <sys/immu.h>; re-declaring them here would conflict.
 */
extern int inb();
extern int inw();
extern int inl();
extern int outb();
extern int outw();
extern int outl();

#define VIRTIO_PAGESZ		4096
#define VIRTIO_MAX_QUEUE	1024	/* cookie array sanity bound */

static void
virtio_bzero(p, n)
	caddr_t p;
	ulong n;
{
	ulong i;

	for (i = 0; i < n; i++)
		p[i] = 0;
}

/* ------------------------------------------------------------------ */
/* Discovery                                                           */
/* ------------------------------------------------------------------ */

int
virtio_pci_find(virtio_type, index, vd)
	ushort virtio_type;
	ushort index;
	struct virtio_dev *vd;
{
	struct pci_devinfo info;
	ushort devid;
	ushort idx;
	ushort subsys;
	ushort seen;
	ushort cmd;
	ulong  bar;
	unchar irq;

	seen = 0;

	/*
	 * 0x1000-0x103F is the transitional/legacy PCI device ID range.
	 * We deliberately ignore 0x1040+ (modern-only devices): those have
	 * no I/O BAR and would need the MMIO capability walk we are not
	 * implementing.
	 */
	for (devid = 0x1000; devid <= 0x103F; devid++) {
		idx = 0;
		while (pci_finddevice(VIRTIO_PCI_VENDOR_ID, devid, idx, &info)) {
			idx++;

			subsys = 0;
			(void) pci_readword(&info, PCI_SUBSYSTEM_ID, &subsys);
			if (subsys != virtio_type)
				continue;

			if (seen++ != index)
				continue;

			bar = 0;
			(void) pci_readdword(&info, PCI_BASE_ADDRESS_0, &bar);
			if ((bar & 1) == 0) {
				/* Memory BAR: a modern-only device. Skip. */
				continue;
			}

			irq = 0;
			(void) pci_readbyte(&info, PCI_INTLINE, &irq);

			vd->vd_pci      = info;
			vd->vd_iobase   = bar & VIRTIO_IOBAR_MASK;
			vd->vd_devid    = devid;
			vd->vd_subsysid = subsys;
			vd->vd_irq      = irq;
			vd->vd_features = 0;

			/*
			 * Enable I/O space decoding (bit 0) and bus
			 * mastering (bit 2). Without the bus-master bit the
			 * device cannot DMA and every queue silently stalls
			 * -- a failure mode that looks like a dead interrupt.
			 */
			cmd = 0;
			(void) pci_readword(&info, PCI_COMMAND, &cmd);
			cmd |= 0x0005;
			(void) pci_writeword(&info, PCI_COMMAND, cmd);

			return 1;
		}
	}

	return 0;
}

/* ------------------------------------------------------------------ */
/* Status and feature handshake                                        */
/* ------------------------------------------------------------------ */

void
virtio_reset(vd)
	struct virtio_dev *vd;
{
	outb(vd->vd_iobase + VIRTIO_PCI_STATUS, 0);
}

unchar
virtio_get_status(vd)
	struct virtio_dev *vd;
{
	return (unchar) inb(vd->vd_iobase + VIRTIO_PCI_STATUS);
}

void
virtio_add_status(vd, bits)
	struct virtio_dev *vd;
	unchar bits;
{
	unchar cur;

	cur = virtio_get_status(vd);
	outb(vd->vd_iobase + VIRTIO_PCI_STATUS, (unchar)(cur | bits));
}

void
virtio_set_failed(vd)
	struct virtio_dev *vd;
{
	virtio_add_status(vd, VIRTIO_CONFIG_S_FAILED);
}

ulong
virtio_host_features(vd)
	struct virtio_dev *vd;
{
	return (ulong) inl(vd->vd_iobase + VIRTIO_PCI_HOST_FEATURES);
}

void
virtio_negotiate(vd, driver_features)
	struct virtio_dev *vd;
	ulong driver_features;
{
	ulong host;

	host = virtio_host_features(vd);
	vd->vd_features = host & driver_features;
	outl(vd->vd_iobase + VIRTIO_PCI_GUEST_FEATURES, vd->vd_features);
}

/* ------------------------------------------------------------------ */
/* Device-specific configuration space                                 */
/* ------------------------------------------------------------------ */

unchar
virtio_cfg_readb(vd, offset)
	struct virtio_dev *vd;
	int offset;
{
	return (unchar) inb(vd->vd_iobase + VIRTIO_PCI_CONFIG_OFF + offset);
}

ushort
virtio_cfg_readw(vd, offset)
	struct virtio_dev *vd;
	int offset;
{
	return (ushort) inw(vd->vd_iobase + VIRTIO_PCI_CONFIG_OFF + offset);
}

ulong
virtio_cfg_readl(vd, offset)
	struct virtio_dev *vd;
	int offset;
{
	return (ulong) inl(vd->vd_iobase + VIRTIO_PCI_CONFIG_OFF + offset);
}

/* ------------------------------------------------------------------ */
/* Virtqueue setup                                                     */
/* ------------------------------------------------------------------ */

int
virtio_vq_setup(vd, vq, index)
	struct virtio_dev *vd;
	struct virtio_vq *vq;
	ushort index;
{
	ulong   base;
	ushort  qnum;
	ulong   descbytes;
	ulong   availbytes;
	ulong   usedoff;
	ulong   usedbytes;
	ulong   ringbytes;
	int     npgs;
	caddr_t mem;
	int     i;

	base = vd->vd_iobase;

	outw(base + VIRTIO_PCI_QUEUE_SEL, index);
	qnum = (ushort) inw(base + VIRTIO_PCI_QUEUE_NUM);

	if (qnum == 0)
		return 0;		/* queue does not exist */

	/*
	 * In the legacy transport QUEUE_NUM is read-only -- the driver
	 * cannot negotiate a smaller ring, so an oversized queue is a hard
	 * failure rather than something we can clamp.
	 */
	if (qnum > VIRTIO_MAX_QUEUE)
		return 0;

	/*
	 * Legacy vring layout, all one contiguous allocation:
	 *
	 *	desc[qnum]			16 bytes each
	 *	avail: flags, idx, ring[qnum], used_event
	 *	--- padded to VIRTIO_PCI_VRING_ALIGN ---
	 *	used:  flags, idx, ring[qnum], avail_event
	 *
	 * The trailing used_event/avail_event fields exist even when
	 * VIRTIO_RING_F_EVENT_IDX is not negotiated; they are part of the
	 * size calculation the device also performs, so they must be
	 * accounted for or the two sides disagree about the layout.
	 */
	descbytes  = (ulong)qnum * 16L;
	availbytes = 2L * (3L + (ulong)qnum);
	usedoff    = (descbytes + availbytes + VIRTIO_PCI_VRING_ALIGN - 1)
		     & ~((ulong)VIRTIO_PCI_VRING_ALIGN - 1);
	usedbytes  = (2L * 3L) + (8L * (ulong)qnum);
	ringbytes  = usedoff + usedbytes;

	npgs = (int)((ringbytes + VIRTIO_PAGESZ - 1) / VIRTIO_PAGESZ);

	/*
	 * MEM_KVMAPPED guarantees a one-to-one (direct) mapping, which is
	 * what makes svtop() valid on the result. Initialization context
	 * only -- this may sleep.
	 */
	mem = (caddr_t) getcpages(npgs, MEM_KVMAPPED);
	if (mem == (caddr_t)0)
		return 0;

	virtio_bzero(mem, (ulong)npgs * VIRTIO_PAGESZ);

	vq->vq_index   = index;
	vq->vq_num     = qnum;
	vq->vq_ringmem = mem;
	vq->vq_npages  = npgs;
	vq->vq_ringpfn = (ulong) svtop(mem);

	vq->vq_desc  = (struct vring_desc *)  mem;
	vq->vq_avail = (struct vring_avail *) (mem + descbytes);
	vq->vq_used  = (struct vring_used *)  (mem + usedoff);

	vq->vq_cookies = (void **) kmem_zalloc(
		(ulong)qnum * sizeof(void *), KM_SLEEP);
	if (vq->vq_cookies == (void **)0) {
		freecpages((pfn_t)vq->vq_ringpfn, npgs);
		vq->vq_ringmem = (caddr_t)0;
		return 0;
	}

	/* Thread every descriptor onto the free list. */
	for (i = 0; i < (int)qnum - 1; i++)
		vq->vq_desc[i].vd_next = (ushort)(i + 1);
	vq->vq_desc[qnum - 1].vd_next = 0xFFFF;

	vq->vq_free_head = 0;
	vq->vq_num_free  = qnum;
	vq->vq_last_used = 0;

	/*
	 * Hand the ring to the device. QUEUE_SEL is still set to `index'
	 * from the size probe above.
	 */
	outl(base + VIRTIO_PCI_QUEUE_PFN, vq->vq_ringpfn);

	return 1;
}

void
virtio_vq_free(vq)
	struct virtio_vq *vq;
{
	if (vq->vq_cookies != (void **)0) {
		kmem_free((void *)vq->vq_cookies,
			  (ulong)vq->vq_num * sizeof(void *));
		vq->vq_cookies = (void **)0;
	}
	if (vq->vq_ringmem != (caddr_t)0) {
		freecpages((pfn_t)vq->vq_ringpfn, vq->vq_npages);
		vq->vq_ringmem = (caddr_t)0;
	}
}

/* ------------------------------------------------------------------ */
/* Queue operation                                                     */
/* ------------------------------------------------------------------ */

int
virtio_vq_add(vq, pa, len, nout, nin, cookie)
	struct virtio_vq *vq;
	paddr_t *pa;
	ulong *len;
	int nout;
	int nin;
	void *cookie;
{
	struct vring_desc *d;
	ushort head;
	ushort desc;
	int total;
	int i;

	total = nout + nin;
	if (total <= 0 || total > VIRTIO_MAX_SG)
		return 0;
	if ((int)vq->vq_num_free < total)
		return 0;

	head = vq->vq_free_head;
	desc = head;

	for (i = 0; i < total; i++) {
		d = &vq->vq_desc[desc];
		d->vd_addr_lo = (ulong) pa[i];
		d->vd_addr_hi = 0;		/* no memory above 4GB here */
		d->vd_len     = len[i];
		/*
		 * Device-readable segments come first, then
		 * device-writable ones. F_WRITE means "device writes".
		 */
		d->vd_flags = (ushort)((i < nout) ? 0 : VRING_DESC_F_WRITE);
		if (i < total - 1)
			d->vd_flags |= VRING_DESC_F_NEXT;
		desc = d->vd_next;
	}

	vq->vq_free_head = desc;
	vq->vq_num_free  = (ushort)(vq->vq_num_free - total);
	vq->vq_cookies[head] = cookie;

	/*
	 * Publish the chain. virtio_publish_avail() lives in virtio_mb.c so
	 * the compiler cannot reorder the descriptor writes above past the
	 * index bump inside it.
	 */
	virtio_barrier();
	virtio_publish_avail(vq, head);

	return 1;
}

void
virtio_vq_notify(vd, vq)
	struct virtio_dev *vd;
	struct virtio_vq *vq;
{
	outw(vd->vd_iobase + VIRTIO_PCI_QUEUE_NOTIFY, vq->vq_index);
}

void *
virtio_vq_get(vq, lenp)
	struct virtio_vq *vq;
	ulong *lenp;
{
	struct vring_used_elem *e;
	void  *cookie;
	ushort head;
	ushort desc;
	int    n;

	if (vq->vq_last_used == virtio_read_used_idx(vq))
		return (void *)0;		/* nothing has completed */

	e = &vq->vq_used->vu_ring[vq->vq_last_used % vq->vq_num];
	head = (ushort) e->vue_id;
	if (lenp != (ulong *)0)
		*lenp = e->vue_len;
	vq->vq_last_used++;

	cookie = vq->vq_cookies[head];
	vq->vq_cookies[head] = (void *)0;

	/*
	 * Walk the chain back onto the free list. The F_NEXT flags are
	 * still intact from virtio_vq_add(), so the chain length is
	 * recoverable without tracking it separately.
	 */
	desc = head;
	n = 1;
	while (vq->vq_desc[desc].vd_flags & VRING_DESC_F_NEXT) {
		desc = vq->vq_desc[desc].vd_next;
		n++;
	}
	vq->vq_desc[desc].vd_next = vq->vq_free_head;
	vq->vq_free_head = head;
	vq->vq_num_free  = (ushort)(vq->vq_num_free + n);

	return cookie;
}

/* ------------------------------------------------------------------ */
/* Interrupt helpers                                                   */
/* ------------------------------------------------------------------ */

unchar
virtio_isr_ack(vd)
	struct virtio_dev *vd;
{
	/*
	 * Reading the ISR clears it. A zero return means the interrupt was
	 * not ours -- essential on SCO, where PCI interrupts are shared.
	 */
	return (unchar) inb(vd->vd_iobase + VIRTIO_PCI_ISR);
}

void
virtio_vq_disable_intr(vq)
	struct virtio_vq *vq;
{
	vq->vq_avail->va_flags |= VRING_AVAIL_F_NO_INTERRUPT;
}

void
virtio_vq_enable_intr(vq)
	struct virtio_vq *vq;
{
	vq->vq_avail->va_flags &= ~VRING_AVAIL_F_NO_INTERRUPT;
}
