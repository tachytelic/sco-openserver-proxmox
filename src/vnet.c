/*
 * vnet.c -- virtio-net MDI network driver for SCO OpenServer 5.0.7
 *
 * Presents a virtio-net device to SCO's MDI/DLPI network stack.
 *
 * OpenServer is MDI version 1, which is ODDI-shaped: everything happens in
 * init(D2oddi), the boot banner comes from printcfg(D3oddi), and the IRQ is
 * claimed with add_intr_handler(D3oddi) -- NOT Sharegister(), which is
 * specific to SCSI host adapters.
 *
 * Modelled on the `shrk' sample (Matrox NS100 PCI ethernet) from SCO's
 * O5ndsampl package, which is the only worked example of an OpenServer MDI
 * driver. Where this driver and the sample disagree, the sample is probably
 * right.
 *
 * The transport underneath -- PCI discovery, feature negotiation, vrings,
 * the notify doorbell and used-ring reaping -- is virtio.c, shared unchanged
 * with the virtio-scsi driver.
 *
 * BUFFERS. Both directions bounce through physically contiguous memory we
 * allocate ourselves, rather than handing STREAMS buffers to the device:
 *
 *	receive  -- the device fills our buffer; we allocb() an mblk and copy
 *		    the frame in, which is exactly what intr(D2mdi) prescribes
 *	transmit -- we copy the (possibly multi-block) mblk chain into one
 *		    contiguous buffer and post that
 *
 * This costs a copy per frame but avoids needing the physical address of
 * STREAMS memory, which OpenServer gives no reliable way to obtain. At
 * emulated-NIC speeds the copy is far cheaper than the VM exits it replaces.
 *
 * *** PRE-ANSI (K&R) C *** -- see virtio.h. No prototypes, no volatile.
 *
 * Copyright (c) 2026. Released under the MIT license.
 */

#ifndef _INKERNEL
#define _INKERNEL 1
#endif

#include <sys/types.h>
#include <sys/param.h>
#include <sys/immu.h>
#include <sys/sysmacros.h>
#include <sys/pci.h>
#include <sys/kmem.h>
#include <sys/cmn_err.h>
#include <sys/errno.h>
/*
 * MUST precede <sys/stream.h>. stream.h declares its routines as
 *	extern mblk_t *allocb __P((register int, uint));
 * but does not include the header that defines __P, so with the Link Kit
 * compiler every one of those lines is a syntax error. cdefs.h picks
 * __P(protos) -> () when __STDC__ is undefined, which is exactly our case.
 */
#include <sys/cdefs.h>
#include <sys/stream.h>
#include <sys/stropts.h>
#include <sys/strconf.h>
#include <sys/mdi.h>
#include <sys/ci/ciintr.h>	/* driver_info, idistributed, IMODE_*, IROUTE_* */

#include "virtio.h"
#include "virtio_net.h"

/* ------------------------------------------------------------------ */
/* Per-adapter state                                                   */
/* ------------------------------------------------------------------ */

struct vnet_buf {
	caddr_t	vb_kaddr;	/* kernel virtual, direct-mapped */
	paddr_t	vb_paddr;	/* physical, for the descriptor  */
	int	vb_busy;	/* posted to the device          */
};

struct vnet_dev {
	struct virtio_dev  vd_virtio;
	struct virtio_vq   vd_vq[VNET_NUM_VQ];

	struct vnet_buf	   vd_rx[VNET_NRX];
	struct vnet_buf	   vd_tx[VNET_NTX];
	caddr_t		   vd_bufmem;	/* one allocation for all buffers */
	int		   vd_bufpages;

	macaddr_t	   vd_eaddr;	/* our MAC              */
	mctbl_t		   vd_mctbl;	/* multicast table      */
	mac_stats_eth_t	   vd_stats;

	queue_t		  *vd_rq;	/* read queue, set at open  */
	queue_t		  *vd_up;	/* upstream queue when bound */

	int		   vd_flags;
	int		   vd_ready;
};

/* vd_flags */
#define VNET_F_PROMISC		0x01
#define VNET_F_ALLMCA		0x02
#define VNET_F_OPEN		0x04

/*
 * One adapter. The Master file declares a single unit; supporting several
 * would mean an array here and a unit lookup in init.
 */
static struct vnet_dev vnet_dev;

static int vnet_debug = 0;

/*
 * Drop LLC XID/TEST responses addressed to the null SAP instead of passing
 * them up. Not static: an administrator can zero it with scodb/crash, and the
 * drop count can be read the same way. See vnet_rx() for why this exists.
 */
int vnet_llc_filter  = 1;
ulong vnet_llc_dropped = 0;

int vnetinit();
int vnetintr();
int vnetopen();
int vnetclose();
int vnetuwput();

extern void  bcopy();
extern int   splstr();
extern void  splx();
extern void  printcfg();
extern void  idistributed();

/* STREAMS */
extern mblk_t *allocb();
extern void    freemsg();
extern void    putnext();
extern int     putq();
extern mblk_t *getq();
extern void    flushq();
extern void    qreply();

/* MDI library -- mdi.h only declares these under __STDC__, which idcpp
   does not define, so declare what we use ourselves. */
extern int  mdi_addrs_equal();
extern int  mdi_in_mctbl();
extern int  mdi_add_mctbl_entry();
extern int  mdi_del_mctbl_entry();
extern int  mdi_num_mctbl_entries();
extern void mdi_do_loopback();
extern void mdi_macokack();
extern void mdi_macerrorack();

/* ------------------------------------------------------------------ */
/* STREAMS registration                                                */
/*                                                                     */
/* Water marks per the MDI guide: low 0, high 40000, which allows about */
/* 25 frames to queue before flow control. The read side carries        */
/* open/close; the write side carries uwput. MDI drivers must NOT have  */
/* service routines -- transmits are drained at interrupt time.         */
/* ------------------------------------------------------------------ */

static struct module_info vnet_minfo = {
	0, "vnet", VNET_ETH_HDRLEN, VNET_MAXFRAME, 40000, 0
};

static struct qinit vnet_urinit = {
	NULL, NULL, vnetopen, vnetclose, NULL, &vnet_minfo, NULL
};

static struct qinit vnet_uwinit = {
	vnetuwput, NULL, NULL, NULL, NULL, &vnet_minfo, NULL
};

struct streamtab vnetinfo = {
	&vnet_urinit, &vnet_uwinit, NULL, NULL
};

/* ------------------------------------------------------------------ */
/* Buffer pool                                                         */
/* ------------------------------------------------------------------ */

/*
 * All RX and TX buffers come from one physically contiguous allocation so we
 * need only a single getcpages()/freecpages() pair, and so every buffer's
 * physical address is a fixed offset from the base.
 */
static int
vnet_alloc_bufs(dv)
	struct vnet_dev *dv;
{
	ulong  total;
	int    npages, i;
	caddr_t p;

	total  = (ulong)(VNET_NRX + VNET_NTX) * VNET_BUFSZ;
	npages = (int) btoc(total);

	p = (caddr_t) getcpages(npages, MEM_KVMAPPED);
	if (p == (caddr_t)0) {
		cmn_err(CE_WARN, "vnet: getcpages(%d) failed", npages);
		return 0;
	}
	dv->vd_bufmem   = p;
	dv->vd_bufpages = npages;

	for (i = 0; i < VNET_NRX; i++) {
		dv->vd_rx[i].vb_kaddr = p;
		dv->vd_rx[i].vb_paddr = VIRTIO_KVTOPHYS(p);
		dv->vd_rx[i].vb_busy  = 0;
		p += VNET_BUFSZ;
	}
	for (i = 0; i < VNET_NTX; i++) {
		dv->vd_tx[i].vb_kaddr = p;
		dv->vd_tx[i].vb_paddr = VIRTIO_KVTOPHYS(p);
		dv->vd_tx[i].vb_busy  = 0;
		p += VNET_BUFSZ;
	}
	return 1;
}

/*
 * Hand one receive buffer to the device: header and frame together, entirely
 * device-writable, so nout is 0 and nin is 1.
 */
static int
vnet_post_rx(dv, i)
	struct vnet_dev *dv;
	int i;
{
	paddr_t pa[1];
	ulong   len[1];

	pa[0]  = dv->vd_rx[i].vb_paddr;
	len[0] = VNET_BUFSZ;

	if (!virtio_vq_add(&dv->vd_vq[VNET_VQ_RX], pa, len, 0, 1,
			   (void *)&dv->vd_rx[i]))
		return 0;

	dv->vd_rx[i].vb_busy = 1;
	return 1;
}

static void
vnet_fill_rx(dv)
	struct vnet_dev *dv;
{
	int i, posted;

	posted = 0;
	for (i = 0; i < VNET_NRX; i++) {
		if (dv->vd_rx[i].vb_busy)
			continue;
		if (!vnet_post_rx(dv, i))
			break;
		posted++;
	}
	if (posted)
		virtio_vq_notify(&dv->vd_virtio, &dv->vd_vq[VNET_VQ_RX]);
}

static struct vnet_buf *
vnet_get_txbuf(dv)
	struct vnet_dev *dv;
{
	int i;

	for (i = 0; i < VNET_NTX; i++) {
		if (!dv->vd_tx[i].vb_busy) {
			dv->vd_tx[i].vb_busy = 1;
			return &dv->vd_tx[i];
		}
	}
	return (struct vnet_buf *)0;
}

/* ------------------------------------------------------------------ */
/* Transmit                                                            */
/* ------------------------------------------------------------------ */

/*
 * Copy an mblk chain into one contiguous buffer behind a zeroed virtio_net
 * header, and post it. Returns 0 if there was no free buffer or ring space,
 * in which case the caller keeps the message queued.
 */
static int
vnet_xmit(dv, mp)
	struct vnet_dev *dv;
	mblk_t *mp;
{
	struct vnet_buf *b;
	mblk_t  *m;
	unchar  *dst;
	int      len, n, i;
	paddr_t  pa[1];
	ulong    dlen[1];

	b = vnet_get_txbuf(dv);
	if (b == (struct vnet_buf *)0)
		return 0;

	/* Zeroed header: no checksum offload, no GSO -- see virtio_net.h. */
	dst = (unchar *) b->vb_kaddr;
	for (i = 0; i < VNET_HDRLEN; i++)
		dst[i] = 0;
	dst += VNET_HDRLEN;

	len = 0;
	for (m = mp; m != (mblk_t *)0; m = m->b_cont) {
		n = (int)(m->b_wptr - m->b_rptr);
		if (n <= 0)
			continue;
		if (len + n > VNET_MAXFRAME) {
			cmn_err(CE_WARN, "vnet: oversize frame %d", len + n);
			b->vb_busy = 0;
			dv->vd_stats.mac_badlen++;
			return 1;	/* consumed: drop it */
		}
		bcopy((caddr_t)m->b_rptr, (caddr_t)dst, n);
		dst += n;
		len += n;
	}

	/* Ethernet requires a minimum frame; pad with zeros. */
	while (len < VNET_MINFRAME) {
		*dst++ = 0;
		len++;
	}

	pa[0]   = b->vb_paddr;
	dlen[0] = (ulong)(VNET_HDRLEN + len);

	/* Entirely device-readable: nout 1, nin 0. */
	if (!virtio_vq_add(&dv->vd_vq[VNET_VQ_TX], pa, dlen, 1, 0, (void *)b)) {
		b->vb_busy = 0;
		return 0;
	}
	virtio_vq_notify(&dv->vd_virtio, &dv->vd_vq[VNET_VQ_TX]);

	return 1;
}

/* Drain whatever the write queue has, while the device will take it. */
static void
vnet_start(dv, q)
	struct vnet_dev *dv;
	queue_t *q;
{
	mblk_t *mp;

	while ((mp = getq(q)) != (mblk_t *)0) {
		if (!vnet_xmit(dv, mp)) {
			(void) putbq(q, mp);	/* ring full: try later */
			break;
		}
		freemsg(mp);
	}
}

/*
 * M_DATA from above. Loop the frame back ourselves when it is addressed to
 * us, to broadcast, or to a multicast we have joined -- virtio-net does not
 * do it for us, exactly as the shrk sample must at 100Mbps.
 */
static void
vnet_data(q, mp)
	queue_t *q;
	mblk_t  *mp;
{
	struct vnet_dev *dv;
	macaddr_t *ea;
	int s;

	dv = (struct vnet_dev *) q->q_ptr;
	if (dv == (struct vnet_dev *)0 || dv->vd_up == (queue_t *)0) {
		mdi_macerrorack(RD(q), M_DATA, MAC_OUTSTATE);
		freemsg(mp);
		return;
	}

	ea = (macaddr_t *) mp->b_rptr;
	if (((*(unchar *)ea) & 1) ?
		(mdi_in_mctbl(&dv->vd_mctbl, *ea) ||
		 (dv->vd_flags & VNET_F_ALLMCA)) :
		mdi_addrs_equal(dv->vd_eaddr, *ea))
		mdi_do_loopback(q, mp, VNET_MINFRAME);
	else if (dv->vd_flags & VNET_F_PROMISC)
		mdi_do_loopback(q, mp, VNET_MINFRAME);

	s = splstr();
	/*
	 * Queue behind anything already queued so ordering is preserved, and
	 * let the interrupt path drain it. Never putbq() a message that was
	 * not previously on this queue.
	 */
	if (q->q_first != (mblk_t *)0) {
		if (!putq(q, mp))
			freemsg(mp);
		vnet_start(dv, q);
	} else if (!vnet_xmit(dv, mp)) {
		if (!putq(q, mp))
			freemsg(mp);
	} else {
		freemsg(mp);
	}
	splx(s);
}

/* ------------------------------------------------------------------ */
/* Receive                                                             */
/* ------------------------------------------------------------------ */

/*
 * Reap filled receive buffers, filter by destination MAC, copy each frame
 * into a fresh mblk and send it upstream. Called from the interrupt handler.
 */
static int
vnet_rx(dv)
	struct vnet_dev *dv;
{
	struct vnet_buf *b;
	ulong   gotlen;
	unchar *frame;
	macaddr_t *ea;
	mblk_t *mp;
	int     len, n, keep, type, ctrl;

	n = 0;
	for (;;) {
		b = (struct vnet_buf *)
		    virtio_vq_get(&dv->vd_vq[VNET_VQ_RX], &gotlen);
		if (b == (struct vnet_buf *)0)
			break;
		b->vb_busy = 0;
		n++;

		if (gotlen <= (ulong)VNET_HDRLEN)
			continue;
		len = (int)(gotlen - VNET_HDRLEN);
		if (len > VNET_MAXFRAME + 4)
			continue;

		frame = (unchar *) b->vb_kaddr + VNET_HDRLEN;
		ea    = (macaddr_t *) frame;

		/*
		 * intr(D2mdi) requires the driver to filter: accept our own
		 * address, broadcast, and multicasts we have joined.
		 */
		if (dv->vd_flags & VNET_F_PROMISC)
			keep = 1;
		else if ((*(unchar *)ea) & 1)
			keep = (dv->vd_flags & VNET_F_ALLMCA) ||
			       mdi_in_mctbl(&dv->vd_mctbl, *ea);
		else
			keep = mdi_addrs_equal(dv->vd_eaddr, *ea);

		if (!keep)
			continue;

		if (dv->vd_up == (queue_t *)0)
			continue;

		/*
		 * SCO's net0 LLC layer, directly above us, leaks one STREAMS
		 * message block for every unsolicited XID or TEST *response* to
		 * the null SAP it receives (commands it answers and frees; UI
		 * frames and real SAPs are fine). Sonos players broadcast an
		 * XID response every few seconds, which starves the 64-byte
		 * block class within hours: receive allocb() then fails at
		 * interrupt level and frames drop, and getpeername() -- which
		 * does not check allocb() -- panics the kernel with cr2=0xC.
		 * Measured 5 Sep 2026, see NEXT.md. The null SAP carries
		 * nothing but station management, so nothing legitimate is
		 * lost by dropping these here. Every real NIC driver on
		 * OpenServer has the same exposure; this is a workaround for
		 * a SCO bug, not a virtio matter.
		 */
		if (vnet_llc_filter && len >= VNET_LLC_MINLEN) {
			type = ((int)frame[12] << 8) | frame[13];
			if (type <= VNET_LLC_MAXLEN &&
			    frame[VNET_LLC_DSAP] == 0 &&
			    (frame[VNET_LLC_SSAP] & 1) != 0) {
				ctrl = frame[VNET_LLC_CTRL] & ~VNET_LLC_PF;
				if (ctrl == VNET_LLC_XID || ctrl == VNET_LLC_TEST) {
					vnet_llc_dropped++;
					continue;
				}
			}
		}

		mp = allocb(len, BPRI_MED);
		if (mp == (mblk_t *)0) {
			dv->vd_stats.mac_no_resource++;
			continue;
		}
		bcopy((caddr_t)frame, (caddr_t)mp->b_wptr, len);
		mp->b_wptr += len;

		putnext(dv->vd_up, mp);
	}

	if (n)
		vnet_fill_rx(dv);	/* re-post what we consumed */
	return n;
}

/* Reap completed transmits and release their buffers. */
static int
vnet_tx_complete(dv)
	struct vnet_dev *dv;
{
	struct vnet_buf *b;
	ulong gotlen;
	int   n;

	n = 0;
	for (;;) {
		b = (struct vnet_buf *)
		    virtio_vq_get(&dv->vd_vq[VNET_VQ_TX], &gotlen);
		if (b == (struct vnet_buf *)0)
			break;
		b->vb_busy = 0;
		n++;
	}
	return n;
}

/* ------------------------------------------------------------------ */
/* Interrupt                                                           */
/* ------------------------------------------------------------------ */

int
vnetintr()
{
	struct vnet_dev *dv;
	unchar isr;
	int    did;

	dv = &vnet_dev;
	if (!dv->vd_ready)
		return 0;

	isr = virtio_isr_ack(&dv->vd_virtio);
	if ((isr & VIRTIO_PCI_ISR_QUEUE) == 0)
		return 0;

	did  = vnet_rx(dv);
	did += vnet_tx_complete(dv);

	/*
	 * Draining the write queue here is what removes the need for a STREAMS
	 * service routine, and is also what keeps txmon from deciding the
	 * hardware is dead.
	 */
	if (dv->vd_rq != (queue_t *)0)
		vnet_start(dv, WR(dv->vd_rq));

	return did ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/* MDI primitives (M_PROTO)                                            */
/* ------------------------------------------------------------------ */

static void
vnet_macproto(q, mp)
	queue_t *q;
	mblk_t  *mp;
{
	struct vnet_dev *dv;
	mac_prim_t   *prim;
	mac_info_ack_t *ack;
	mblk_t *rmp;

	dv   = (struct vnet_dev *) q->q_ptr;
	prim = (mac_prim_t *) mp->b_rptr;

	switch (prim->mac_primitive) {

	case MAC_INFO_REQ:
		rmp = allocb(MAC_INFO_ACK_SIZE, BPRI_MED);
		if (rmp == (mblk_t *)0) {
			mdi_macerrorack(RD(q), MAC_INFO_REQ, MAC_INITFAILED);
			break;
		}
		rmp->b_datap->db_type = M_PCPROTO;
		ack = (mac_info_ack_t *) rmp->b_wptr;
		ack->mac_primitive      = MAC_INFO_ACK;
		/*
		 * SDU here is the whole ethernet frame including the 14-byte
		 * header but excluding the CRC, i.e. 1514 -- the shrk sample
		 * reports SHRK_TXMAXSZ (SHRK_MAXPACK 1518 - 4) and
		 * SHRK_HEADERSZ. Reporting VNET_MTU (1500) here made the
		 * interface come up at "mtu 1486", because SCO subtracts the
		 * header to get the MTU it advertises.
		 */
		ack->mac_max_sdu        = VNET_MAXFRAME;
		ack->mac_min_sdu        = VNET_ETH_HDRLEN;
		ack->mac_mac_type       = MAC_CSMACD;
		ack->mac_driver_version = MDI_VERSION;
		/*
		 * Advisory only -- virtio has no PHY, no autonegotiation and no
		 * wire, so nothing enforces this. It is what netstat-style tools
		 * report and what a stack choosing between interfaces would weigh.
		 *
		 * Measured 17.5 MB/s (~140 Mbit/s) pulling 50MB over plain HTTP,
		 * so the 100 Mb this used to claim was an understatement. The real
		 * ceiling is host CPU and memory bandwidth; 1 Gb is the honest
		 * "not the bottleneck" answer.
		 */
		ack->mac_if_speed       = 1000000000;	/* 1 Gb, nominal */
		rmp->b_wptr += MAC_INFO_ACK_SIZE;
		qreply(q, rmp);
		break;

	case MAC_BIND_REQ:
		dv->vd_up = RD(q);
		vnet_fill_rx(dv);	/* only now can frames go anywhere */
		mdi_macokack(RD(q), MAC_BIND_REQ);
		break;

	default:
		mdi_macerrorack(RD(q), prim->mac_primitive, MAC_BADPRIM);
		break;
	}
	freemsg(mp);
}

/* ------------------------------------------------------------------ */
/* ioctls                                                              */
/* ------------------------------------------------------------------ */

static void
vnet_ioctl(q, mp)
	queue_t *q;
	mblk_t  *mp;
{
	struct vnet_dev *dv;
	struct iocblk *iocp;
	macaddr_t *ea;
	int ret;

	dv   = (struct vnet_dev *) q->q_ptr;
	iocp = (struct iocblk *) mp->b_rptr;
	ret  = 0;

	switch (iocp->ioc_cmd) {

	case MACIOC_GETADDR:
	case MACIOC_GETRADDR:
		if (mp->b_cont == (mblk_t *)0) { ret = EINVAL; break; }
		bcopy((caddr_t)dv->vd_eaddr,
		      (caddr_t)mp->b_cont->b_rptr, MDI_MACADDRSIZE);
		iocp->ioc_count = MDI_MACADDRSIZE;
		break;

	case MACIOC_GETSTAT:
		if (mp->b_cont == (mblk_t *)0) { ret = EINVAL; break; }
		bcopy((caddr_t)&dv->vd_stats,
		      (caddr_t)mp->b_cont->b_rptr, sizeof(mac_stats_eth_t));
		iocp->ioc_count = sizeof(mac_stats_eth_t);
		break;

	case MACIOC_SETMCA:
		if (mp->b_cont == (mblk_t *)0) { ret = EINVAL; break; }
		ea = (macaddr_t *) mp->b_cont->b_rptr;
		if (!mdi_add_mctbl_entry(&dv->vd_mctbl, *ea))
			ret = ENOSPC;
		break;

	case MACIOC_DELMCA:
		if (mp->b_cont == (mblk_t *)0) { ret = EINVAL; break; }
		ea = (macaddr_t *) mp->b_cont->b_rptr;
		(void) mdi_del_mctbl_entry(&dv->vd_mctbl, *ea);
		break;

	case MACIOC_GETMCSIZ:
		if (mp->b_cont == (mblk_t *)0) { ret = EINVAL; break; }
		*(int *)mp->b_cont->b_rptr = MDI_NMCADDR;
		iocp->ioc_count = sizeof(int);
		break;

	case MACIOC_PROMISC:
		if (mp->b_cont == (mblk_t *)0) { ret = EINVAL; break; }
		if (*(int *)mp->b_cont->b_rptr)
			dv->vd_flags |= VNET_F_PROMISC;
		else
			dv->vd_flags &= ~VNET_F_PROMISC;
		break;

	case MACIOC_CLRSTAT:
		bzero((caddr_t)&dv->vd_stats, sizeof(mac_stats_eth_t));
		break;

	default:
		ret = EINVAL;
		break;
	}

	if (ret) {
		mp->b_datap->db_type = M_IOCNAK;
		iocp->ioc_error = ret;
	} else {
		mp->b_datap->db_type = M_IOCACK;
		iocp->ioc_error = 0;
	}
	qreply(q, mp);
}

/* ------------------------------------------------------------------ */
/* STREAMS entry points                                                */
/* ------------------------------------------------------------------ */

int
vnetuwput(q, mp)
	queue_t *q;
	mblk_t  *mp;
{
	switch (mp->b_datap->db_type) {

	case M_PROTO:
	case M_PCPROTO:
		vnet_macproto(q, mp);
		break;

	case M_DATA:
		vnet_data(q, mp);
		break;

	case M_IOCTL:
		vnet_ioctl(q, mp);
		break;

	case M_FLUSH:
		if (*mp->b_rptr & FLUSHW) {
			flushq(q, FLUSHALL);
			*mp->b_rptr &= ~FLUSHW;
		}
		if (*mp->b_rptr & FLUSHR) {
			flushq(RD(q), FLUSHALL);
			qreply(q, mp);
		} else {
			freemsg(mp);
		}
		break;

	default:
		freemsg(mp);
		break;
	}
	return 0;
}

/* MDI drivers are exclusive-open devices. */
int
vnetopen(q, dev, oflags, sflag)
	queue_t *q;
	int dev, oflags, sflag;
{
	struct vnet_dev *dv;

	dv = &vnet_dev;
	if (!dv->vd_ready)
		return ENXIO;
	if (dv->vd_flags & VNET_F_OPEN)
		return EBUSY;

	dv->vd_flags |= VNET_F_OPEN;
	dv->vd_rq = q;
	q->q_ptr        = (caddr_t) dv;
	WR(q)->q_ptr    = (caddr_t) dv;
	return 0;
}

int
vnetclose(q)
	queue_t *q;
{
	struct vnet_dev *dv;

	dv = (struct vnet_dev *) q->q_ptr;
	if (dv != (struct vnet_dev *)0) {
		dv->vd_up    = (queue_t *)0;
		dv->vd_rq    = (queue_t *)0;
		dv->vd_flags &= ~(VNET_F_OPEN | VNET_F_PROMISC | VNET_F_ALLMCA);
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* Initialisation                                                      */
/* ------------------------------------------------------------------ */

int
vnetinit()
{
	struct vnet_dev *dv;
	ulong host, want;
	int   i;

	dv = &vnet_dev;
	bzero((caddr_t)dv, sizeof(*dv));

	if (!virtio_pci_find(VIRTIO_ID_NET, 0, &dv->vd_virtio)) {
		cmn_err(CE_NOTE, "vnet: no virtio-net device present");
		return 0;
	}

	virtio_reset(&dv->vd_virtio);
	virtio_add_status(&dv->vd_virtio, VIRTIO_CONFIG_S_ACKNOWLEDGE);
	virtio_add_status(&dv->vd_virtio, VIRTIO_CONFIG_S_DRIVER);

	host = virtio_host_features(&dv->vd_virtio);
	want = (ulong) VNET_DRIVER_FEATURES;
	virtio_negotiate(&dv->vd_virtio, want);

	for (i = 0; i < VNET_NUM_VQ; i++) {
		if (!virtio_vq_setup(&dv->vd_virtio, &dv->vd_vq[i], i)) {
			cmn_err(CE_WARN, "vnet: queue %d setup failed", i);
			virtio_set_failed(&dv->vd_virtio);
			return 0;
		}
	}

	if (!vnet_alloc_bufs(dv)) {
		virtio_set_failed(&dv->vd_virtio);
		return 0;
	}

	/*
	 * Read the MAC the device gives us. Without F_MAC we would have to
	 * invent one, which is worse than failing.
	 */
	if (VIRTIO_HAS_FEATURE(&dv->vd_virtio, VIRTIO_NET_F_MAC)) {
		for (i = 0; i < VNET_ETH_ALEN; i++)
			dv->vd_eaddr[i] =
			    virtio_cfg_readb(&dv->vd_virtio, VNET_CFG_MAC + i);
	} else {
		cmn_err(CE_WARN, "vnet: device offers no MAC address");
		virtio_set_failed(&dv->vd_virtio);
		return 0;
	}

	dv->vd_mctbl.cnt = 0;

	/*
	 * Claim the interrupt. A PCI IRQ is shared -- ours is 11, the same line
	 * virtio-scsi is on -- so this MUST ask for a shared mode. An earlier
	 * version called add_intr_handler(irq, handler, 0), which asks for the
	 * line exclusively and failed with
	 *	WARNING: vnet: cannot add interrupt handler (11)
	 * because vscsi already held it.
	 *
	 * idistributed() is what the shrk sample uses and is the network
	 * equivalent of Sharegister() for SCSI. Note the irq field is not a
	 * line number: IDIST_PCI_IRQ() packs the PCI slot and function, and the
	 * kernel resolves the actual line from the device's config space.
	 */
	{
		driver_info idi;

		idi.version		= DRV_INFO_VERS_2;
		idi.name		= "vnet";
		idi.irq			= IDIST_PCI_IRQ(
					     dv->vd_virtio.vd_pci.slotnum,
					     dv->vd_virtio.vd_pci.funcnum);
		idi.bus			= (int) dv->vd_virtio.vd_pci.busnum;
		idi.iosaddr		= 0;		/* unused */
		idi.ioeaddr		= 0;		/* unused */
		idi.weight		= 5;
		idi.intr		= vnetintr;
		idi.ipl			= 5;
		idi.route		= IROUTE_GLOBAL;
		idi.mode		= IMODE_SHARED_CDRIVERIPL;
		/*
		 * 1, NOT ICPU_ANY: restrict this interrupt to the base CPU.
		 *
		 * ICPU_ANY says the handler may be routed to any processor,
		 * which is a promise this driver cannot keep. It protects its
		 * ring and queue state with spl5() alone, and spl "only set[s]
		 * the priority level for the processor on which the code is
		 * executing" (spl(D3oddi)) -- no defence against the other CPU.
		 * The STREAMS side is not declared distributed (no
		 * sdistributed() call here, unlike the shrk sample), so task
		 * level is already funneled to the base CPU; letting the
		 * interrupt roam was the one thing putting the two on different
		 * processors. On a two-CPU MPX kernel that killed networking
		 * outright -- no ping, no ssh, nothing in the log.
		 *
		 * shareg_ex(D4osdi) documents 1 as exactly this: "the driver is
		 * either non-multithreaded or is first level multithreaded ...
		 * this will restrict interrupts to the base cpu". Harmless on a
		 * uniprocessor, where there is only one CPU to route to.
		 *
		 * To lift this, vnet needs what vscsi now has: a lockb5() lock
		 * over the ring and slot state, with callbacks made outside it.
		 */
		idi.processor_mask	= 1;

		idistributed(&idi);
	}

	virtio_add_status(&dv->vd_virtio, VIRTIO_CONFIG_S_DRIVER_OK);
	dv->vd_ready = 1;

	printcfg("vnet", (unsigned) dv->vd_virtio.vd_iobase, 0x3f,
		 (int) dv->vd_virtio.vd_irq, -1,
		 "type=vnet virtio-net");

	if (vnet_debug)
		cmn_err(CE_CONT,
			"vnet: mac %x:%x:%x:%x:%x:%x feat 0x%x\n",
			dv->vd_eaddr[0], dv->vd_eaddr[1], dv->vd_eaddr[2],
			dv->vd_eaddr[3], dv->vd_eaddr[4], dv->vd_eaddr[5],
			dv->vd_virtio.vd_features);

	return 1;
}
