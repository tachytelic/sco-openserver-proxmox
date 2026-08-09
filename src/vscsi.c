/*
 * vscsi.c -- virtio-scsi host adapter driver for SCO OpenServer 5.0.7
 *
 * Presents a virtio-scsi target to SCO's Sdsk disk driver.
 *
 * The OSR5 HBA interface is undocumented in the public HDK books, but SCO
 * ships a worked example -- the Sram sample driver, installed by the
 * O5hbasamp package to /usr/src/O5hdk/samples/Sram/. This driver follows
 * its conventions, which are not the obvious ones:
 *
 *	- the entry point dispatches on req_type, NOT opcode. `opcode' is
 *	  meaningless for host-adapter calls (it reads as uninitialised
 *	  garbage), which cost a long detour before the sample turned up.
 *	- success returns 0; failure returns -1 with host_sts set.
 *	- SCSI_INIT sub-dispatches on hacmd.
 *	- disk geometry is answered through req_p->ext_p, not data_ptr.
 *	- data_ptr is a PHYSICAL address; ptok() maps it to kernel virtual.
 *	- completion is signalled by calling (*req_p->io_intr)(req_p).
 *
 * Pre-ANSI (K&R) C, built with the in-box Link Kit -- see virtio.h.
 * Keep cmn_err calls to a few arguments; long lists get garbled.
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
#include <sys/scsi.h>
#include <sys/devreg.h>
#include <sys/ci/ciintr.h>	/* IROUTE_*, IMODE_* for Sharegister() */
#include <sys/ci/cilock.h>	/* struct lockb; unlockb() declaration */

#include "virtio.h"
#include "virtio_scsi.h"

/* Wire lengths -- never sizeof, which may include trailing padding. */
#define VSCSI_REQ_HDRLEN	(8 + 8 + 1 + 1 + 1 + VSCSI_CDB_SIZE)	/* 51  */
#define VSCSI_RESP_HDRLEN	(4 + 4 + 2 + 1 + 1 + VSCSI_SENSE_SIZE)	/* 108 */

#define VSCSI_NSLOT	32		/* concurrent commands */
#define VSCSI_SLOTSZ	256		/* per-slot header area */

/*
 * Kernel-true offsets within struct scsi_io_req (sizeof 100): sense_len 66,
 * scsi_sense 68, req_id 72, hacmd 83, rbuf 84, io_intr 88.
 *
 * These exist because scsi.h picks its packing with
 *	#ifdef _M_I386 / #pragma pack(4) / #else / #pragma pack(2) / #endif
 * and /lib/idcpp -- unlike real cc -- does not predefine _M_I386. build.sh
 * now passes -D_M_I386, so plain struct access agrees with these values; they
 * are kept as a belt-and-braces guard against the layout silently drifting
 * again, because the failure mode is nearly undetectable (see RP_CDB below).
 */
#define RP_SENSE_LEN(rp)   (*(unchar *)((char *)(rp) + 66))
#define RP_SCSI_SENSE(rp)  (*(paddr_t *)((char *)(rp) + 68))
/*
 * SIGNED char: hacmd carries SCSI_VER_INFO which is -1. Read as unchar it
 * comes back 255 and never matches the case label.
 */
#define RP_HACMD(rp)       (*(char *)((char *)(rp) + 83))
#define RP_IO_INTR(rp)     (*(int (**)())((char *)(rp) + 88))

/*
 * The CDB itself. union scsi_cdb starts at offset 40 and is 26 bytes:
 * raw[12 + sizeof(struct scsi_sense)], and struct scsi_sense is 14 bytes
 * because lines 402-532 of scsi.h are #pragma pack(1) irrespective of
 * _M_I386.
 *
 * Read the CDB through THIS, never &rp->scsi_cmd. When the packing was wrong
 * that expression resolved four bytes early and returned all zeros -- and an
 * all-zero CDB is a legal TEST UNIT READY, so every command completed
 * rsp=0 sts=0 and looked healthy while INQUIRY and READ CAPACITY silently
 * did nothing. That cost days to find.
 */
#define RP_CDB(rp)         ((unchar *)((char *)(rp) + 40))
#define RP_CDB_MAX	   26
#define VSCSI_SENSE_INLINE 14	/* sizeof(struct scsi_sense), packed */
/*
 * Where sense data goes inside that 26 bytes. The union is declared
 *	unsigned char raw[12+sizeof(struct scsi_sense)];
 * so the command field is a fixed 12 bytes -- the size of its largest member,
 * struct TwelveCmd -- and the sense area starts immediately after it. This is
 * NOT the CDB length: a 6- or 10-byte command still leaves sense at 12.
 */
#define VSCSI_SENSE_OFF	   12

/*
 * BIOS-compatible geometry reported through SCSI_DISK_INFO.
 *
 * This has to agree with what the firmware tells SCO's Stage 1 boot loader,
 * because Stage 1 reads the filesystem through BIOS CHS. Disagree and the
 * install completes, the driver works, the kernel boots from CD -- and then
 * booting from the disk fails with
 *
 *	not a directory
 *	Stage 1 boot failure: error loading hd(40)/boot
 *
 * because Stage 1 computes block addresses from one geometry against a layout
 * written under another, and lands on something that is not the root directory.
 *
 * We reported 128/32, which SeaBIOS does not choose for any disk this driver
 * will realistically see -- for anything over ~4 GB it computes 255/63. That
 * mismatch was papered over by passing lcyls/lheads/lsecs to QEMU by hand,
 * which forced the firmware to agree but also forced the disk to be attached
 * through `args:` instead of as a managed Proxmox disk. That in turn cost
 * snapshots, vzdump and clone -- i.e. every means of recovering the VM.
 *
 * Reporting what the firmware already computes removes the whole problem.
 *
 * The convention the Sram sample documents selects by capacity:
 *	<=1GB 64/32,  <=2GB 128/32,  <=4GB 255/32,  >4GB 255/63
 * We cannot apply it directly: the capacity is not known here. SCSI_DISK_INFO
 * arrives during init, and querying the drive means a polled READ CAPACITY --
 * the sample's own note is "Remember there are no interrupts." Every disk this
 * driver is used with is far above the 4 GB threshold, so 255/63 is the right
 * fixed answer, and a wrong one only for disks small enough that OpenServer
 * would struggle to install on them at all.
 *
 * If you need one of the smaller sets, change these and rebuild; keep the BIOS
 * in step, either by letting the firmware compute it or with explicit
 * lcyls/lheads/lsecs. For a system already installed under a different
 * geometry, dparam(ADM) rewrites the masterboot block to match the driver.
 */
#define VSCSI_HEADS	255
#define VSCSI_SECTORS	63

/*
 * Scatter/gather limits. Sdsk can hand us at most SCSI_MAX_SG segments
 * (NBCLUSTER, 16 on 5.0.7); add the virtio-scsi request and response headers
 * for the descriptor count. Never silently truncate an S/G list -- a short
 * transfer corrupts data with no error anywhere.
 */
#define VSCSI_MAX_SEG	SCSI_MAX_SG
#define VSCSI_MAX_DESC	(VSCSI_MAX_SEG + 2)

struct vscsi_slot {
	struct virtio_scsi_req_cmd  *vs_req;
	struct virtio_scsi_resp_cmd *vs_resp;
	paddr_t   vs_reqpa;
	paddr_t   vs_resppa;
	REQ_IO   *vs_rp;
	int       vs_busy;
};

/*
 * One of these per host adapter. Proxmox's default scsihw,
 * virtio-scsi-single, creates a SEPARATE controller for every disk, so a
 * single-adapter driver silently loses every disk but the first -- the
 * failure documented in README "Disk limitations". The peripheral driver
 * routes requests with rp->ha_num, which mscsi(F) populates per device;
 * everything the request touches must therefore be per-adapter state.
 */
#define VSCSI_MAX_HA	4

struct vscsi_ha {
	struct virtio_dev  ha_dev;
	struct virtio_vq   ha_vq[VSCSI_NUM_VQ];
	struct vscsi_slot  ha_slot[VSCSI_NSLOT];
	/*
	 * ha_lk guards this adapter's mutable I/O state as one domain: the
	 * slot array, both sides of the request virtqueue (the descriptor
	 * free list is shared between add and get), and ha_pending. It is
	 * taken with lockb5(), which also raises to spl5, so it subsumes the
	 * old spl5()-only discipline -- spl alone "only set[s] the priority
	 * level for the processor on which the code is executing"
	 * (spl(D3oddi)) and is no protection against another CPU.
	 *
	 * RULES: never call out of the driver while holding it -- io_intr
	 * callbacks and Unit Attention resubmits happen after it is dropped
	 * (vscsi_reap), because lockb is not recursive and a callback that
	 * submits the next request re-enters vscsi_send(), which takes the
	 * lock afresh. Both of SCO's MP HBAs do the same: slha's callDriver
	 * releases its per-HBA lock around the completion callback, and
	 * dpt's dptintr calls back with no lock held. Zero-initialised
	 * (static storage) is the unlocked state; no init call is needed.
	 */
	struct lockb       ha_lk;
	int    ha_ready;
	int    ha_pending;
	int    ha_ua_retry;	/* per-adapter, so one device's Unit
				   Attention cannot reset another's budget */
	int    ha_max_target;	/* addressing limits from device config */
	int    ha_max_lun;
};

static struct vscsi_ha    vscsi_ha[VSCSI_MAX_HA];
static int                vscsi_nha = 0;
static HAINFO             vscsi_info;

/*
 * Per-I/O tracing. Off by default: at seven cmn_err lines per command, mkfs
 * overran SCO's kernel error log ("WARNING: err: Error log buffer overflow")
 * and every write was gated behind console scrolling. Set to 1 to bring the
 * trace back when diagnosing. Errors are logged regardless of this flag.
 */
static int                vscsi_debug = 0;

/*
 * Command counters, used by the self-limiting scatter/gather trace in
 * vscsi_send_sg(). These answer the question the quiet build cannot: is Sdsk
 * really issuing SCSI_SG, and in what proportion to plain SCSI_SEND?
 */
static ulong              vscsi_sg_count = 0;
static ulong              vscsi_send_count = 0;

int vscsi_entry();
int vscsiintr();
int vscsipoll();

extern void bcopy();
extern int  spl5();
extern void splx();
extern void printcfg();
extern int  Sharegister();
/*
 * The multithreading admission and registration set, per slha's own init
 * sequence (recovered from its disassembly). All four exist in the UP
 * kernel as always-succeed stubs -- scsi_distributed(D3osdi): "On a
 * uniprocessor system, scsi_distributed() always returns 1."
 */
extern int  all_io();
extern int  intrallocs();
extern int  scsi_distributed();
extern int  remap_driver_cpu();
/*
 * Not declared by <sys/ci/cilock.h>, but exported by both the UP and MPX
 * kernels. lockb5() acquires a spin lock and raises to spl5, returning the
 * previous level for unlockb() -- lockb(D3oddi). On the UP kernel the spin
 * can never be contended, so it degenerates to the spl raise.
 */
extern int  lockb5();

/*
 * One SHAREG_EX per adapter: Sharegister(D3osdi) is explicit that each call
 * must pass a unique static structure ("do not reuse the same structure for
 * successive board registration").
 *
 * The layout in <sys/devreg.h> is right -- root_id, in_use, int_vec, route,
 * weight, mode, processor_mask, iosaddr, ioeaddr, bus -- confirmed by
 * reading a live registration out of /dev/mem with crash(ADM) and matching
 * it against what the driver had set. A hand-rolled struct that moved `bus'
 * ahead of `route' shifted every later field by one word, and the MPX
 * kernel said so on the console:
 *
 *	WARNING: idistributed: SCSI adapter: bad route type (0x00000000)
 *
 * -- it had read our bus number where route belongs. Do not rearrange this
 * again without confirming that message stays absent from an MPX boot.
 */
SHAREG_EX vscsi_drvrreg[VSCSI_MAX_HA];
int vscsi_drvr_processor = DRIVER_CPU_DEFAULT;

/* Handed to intrallocs() for the multiprocessor admission check. */
struct lockb vscsi_tab_lk;

/*
 * The mscsi-derived configuration array. The Link Kit generates and links a
 * populated vscsicfg[] whenever mscsi(F) mentions this driver; this tentative
 * definition is the documented fallback ("Adapter drivers should always
 * include the following declaration", shareg_ex(D4osdi)) and stays zero-filled
 * when the driver is absent from mscsi. Sized for every adapter we support so
 * the per-adapter in_use test below never reads out of bounds.
 */
struct scsi_ha_cfg vscsicfg[VSCSI_MAX_HA];

/*
 * Derive CDB length from the SCSI group code in the top 3 bits of the opcode,
 * and direction from the opcode itself.
 *
 * Sdsk DOES populate cmdlen and dir correctly (verified: cmdlen 6/10 and
 * dir 1/2 matching the command). An earlier comment here claimed otherwise --
 * that was the mis-packed struct being read at the wrong offsets, not Sdsk.
 * We still derive rather than trust the fields, because the Sram sample does
 * the same and because it keeps us honest if a peripheral driver omits them.
 */
static int
vscsi_cdblen(op)
	unchar op;
{
	switch (op & SCMD_GROUP_MASK) {
	case SCMD_GROUP_0:	return 6;
	case SCMD_GROUP_1:
	case SCMD_GROUP_2:	return 10;
	case SCMD_GROUP_5:	return 12;
	default:		return 10;
	}
}

/* 0 = no data, SCSI_IN = read, SCSI_OUT = write */
static int
vscsi_dir(op)
	unchar op;
{
	switch (op) {
	case 0x00:			/* TEST UNIT READY */
	case 0x0b:			/* SEEK	           */
	case 0x1b:			/* START/STOP UNIT */
	case 0x2f:			/* VERIFY          */
		return 0;
	case 0x0a:			/* WRITE(6)        */
	case 0x2a:			/* WRITE(10)       */
	case 0x2e:			/* WRITE AND VERIFY*/
	case 0x15:			/* MODE SELECT     */
		return SCSI_OUT;
	default:			/* INQUIRY, READ, READ CAPACITY,
					   REQUEST SENSE, MODE SENSE ... */
		return SCSI_IN;
	}
}

/* ------------------------------------------------------------------ */

static struct vscsi_slot *
vscsi_getslot(hp)
	struct vscsi_ha *hp;
{
	int i;

	for (i = 0; i < VSCSI_NSLOT; i++) {
		if (!hp->ha_slot[i].vs_busy) {
			hp->ha_slot[i].vs_busy = 1;
			return &hp->ha_slot[i];
		}
	}
	return (struct vscsi_slot *)0;
}

/*
 * Resolve the adapter a request is addressed to.
 *
 * rp->ha_num comes from the mscsi(F) `number' field via scsi_dev_cfg, the
 * same per-device record that supplies id and lun -- which were verified to
 * arrive correctly on I/O requests. Trust it only when there is something to
 * choose between: with a single adapter every request goes to it regardless,
 * which keeps the behaviour of every existing installation bit-for-bit and
 * guards against ha_num arriving as garbage the way `id' does on
 * host-adapter calls.
 */
static struct vscsi_ha *
vscsi_findha(rp)
	REQ_IO *rp;
{
	int n;

	if (vscsi_nha <= 1)
		return &vscsi_ha[0];
	n = (int)(unchar) rp->ha_num;
	if (n >= vscsi_nha) {
		cmn_err(CE_WARN, "vscsi: request for ha %d of %d",
			n, vscsi_nha);
		return (struct vscsi_ha *)0;
	}
	return &vscsi_ha[n];
}

/*
 * Address the request at the target/LUN the peripheral driver asked for.
 *
 * This used to hardcode target 0 / LUN 0, justified by `id' reading as -16.
 * That was the mis-packed struct, not Sdsk: id and lun are populated
 * correctly. Honouring them is what lets one adapter carry several drives --
 * add a second disk in the hypervisor, give it an mscsi line with the right
 * ID, and it works with no driver change.
 *
 * Returns 0 if the address is outside what the device advertises.
 */
static int
vscsi_setlun(hp, sl, rp)
	struct vscsi_ha *hp;
	struct vscsi_slot *sl;
	REQ_IO *rp;
{
	int target, lun, cdblun;
	unchar *cdb;

	target = (int)(unchar) rp->id;
	lun    = (int)(unchar) rp->lun;

	/*
	 * OpenServer addresses LUNs the SCSI-2 way: byte 1, bits 7-5 of the
	 * CDB. SAM -- which virtio-scsi uses -- moved the LUN into a separate
	 * address field and made those CDB bits reserved, so the device answers
	 * ILLEGAL REQUEST / INVALID FIELD IN CDB (sense key 05, ASC 24) and the
	 * unit never attaches. Bridging the two schemes is the host adapter's
	 * job. Latent until a second disk was attached at LUN 1, because
	 * everything before it lived at LUN 0 where the bits are zero.
	 *
	 * NOTE, and it is a real constraint on how devices may be attached:
	 * this bridge only works for peripheral drivers that actually populate
	 * one of the two fields. Sdsk sets the CDB bits, so a disk at LUN 1
	 * works. Srom sets NEITHER -- every request it issues arrives with
	 * rp->lun 0 and the CDB LUN bits 0, whatever mscsi(F) says -- so a
	 * CD-ROM at a non-zero LUN is silently addressed to LUN 0 and answered
	 * by whatever lives there. The symptom is a CD-ROM whose INQUIRY reads
	 * "QEMU HARDDISK" and every read returning EIO.
	 *
	 * So: give a CD-ROM its own TARGET ID, never a LUN. Under QEMU that
	 * means an explicit scsi-cd with scsi-id=N,lun=0 rather than Proxmox's
	 * scsiN mapping, which puts everything at scsi-id=0 and varies the LUN.
	 */
	cdb    = RP_CDB(rp);
	cdblun = ((int) cdb[1] >> 5) & 0x07;
	if (lun == 0 && cdblun != 0)
		lun = cdblun;

	if ((hp->ha_max_target && target > hp->ha_max_target) ||
	    (hp->ha_max_lun    && lun    > hp->ha_max_lun)) {
		cmn_err(CE_WARN, "vscsi: target %d lun %d out of range (%d/%d)",
			target, lun, hp->ha_max_target, hp->ha_max_lun);
		return 0;
	}
	/*
	 * INQUIRY only, i.e. probe time, so this costs nothing in steady
	 * state. It is how the Srom LUN behaviour above was established:
	 * turn it on and every peripheral's probe address is one line.
	 */
	if (vscsi_debug && cdb[0] == 0x12)
		cmn_err(CE_CONT,
			"vscsi: INQ id=%d rplun=%d cdblun=%d -> lun=%d\n",
			target, (int)(unchar) rp->lun, cdblun, lun);

	VSCSI_SET_LUN(sl->vs_req->rc_lun, target, lun);
	return 1;
}

/*
 * Kernel-virtual base of a scatter/gather array.
 *
 * The two references disagree. For SCSI_SEND the Sram sample uses
 * ptok(req_p->data_ptr), so data_ptr is physical -- which is why we hand it
 * straight to virtio. But its SCSI_SG path casts data_ptr and link_ptr to
 * pointers and dereferences them WITHOUT ptok, i.e. treats them as kernel
 * virtual, while still applying ptok to each entry. TLS006 does not cover it.
 *
 * Rather than trust that asymmetry in a sample that has already proven
 * fallible, accept either form: at or above KVBASE it is already kernel
 * virtual; below, it is physical and needs ptok().
 */
static caddr_t
vscsi_sgbase(v)
	paddr_t v;
{
	if ((ulong) v >= KVBASE)
		return (caddr_t) v;
	return (caddr_t) ptok(v);
}

/*
 * Issue a scatter/gather command (SCSI_SG / SCSI_SG_NC).
 *
 * Per the Sram sample's own header comment, the request block carries:
 *	data_ptr = kernel address of an array of PHYSICAL segment addresses
 *	link_ptr = kernel address of an array of segment lengths
 *	data_len = TOTAL bytes across all segments, summed
 *	data_blk = starting block
 * There is no segment count -- walk both arrays in lockstep until the
 * lengths sum to data_len.
 *
 * This maps onto virtio almost exactly: a descriptor chain IS an S/G list,
 * so each segment becomes one descriptor rather than needing a bounce buffer.
 */
static int
vscsi_send_sg(hp, rp)
	struct vscsi_ha *hp;
	REQ_IO *rp;
{
	struct vscsi_slot *sl;
	paddr_t  pa[VSCSI_MAX_DESC];
	ulong    len[VSCSI_MAX_DESC];
	paddr_t *d_ptr;
	long    *t_len;
	ulong    bttl;
	int      nseg, nout, nin, i, clen, dir, s;
	unchar  *cdb;

	if (!hp->ha_ready) {
		rp->host_sts = 1;
		return -1;
	}

	cdb  = RP_CDB(rp);
	clen = vscsi_cdblen(cdb[0]);
	dir  = vscsi_dir(cdb[0]);

	/* An S/G command with no data direction is meaningless. */
	if (dir == 0 || rp->data_len == 0) {
		cmn_err(CE_WARN, "vscsi: SG with no data, op=0x%x", cdb[0]);
		rp->host_sts = 1;
		return -1;
	}

	d_ptr = (paddr_t *) vscsi_sgbase(rp->data_ptr);
	t_len = (long *)    vscsi_sgbase(rp->link_ptr);
	if (d_ptr == (paddr_t *)0 || t_len == (long *)0) {
		cmn_err(CE_WARN, "vscsi: SG null list ptr");
		rp->host_sts = 1;
		return -1;
	}

	bttl = 0;
	nseg = 0;
	while (bttl < (ulong) rp->data_len) {
		if (nseg >= VSCSI_MAX_SEG || t_len[nseg] <= 0) {
			cmn_err(CE_WARN,
				"vscsi: bad SG list, seg %d len %d total %d",
				nseg, t_len[nseg], rp->data_len);
			rp->host_sts = 1;
			return -1;
		}
		bttl += (ulong) t_len[nseg];
		nseg++;
	}

	/*
	 * Counters stay live so the ratio is available the moment tracing is
	 * switched on; the output itself is gated. Set vscsi_debug = 1 to see
	 * the first 8 S/G requests and then one line per 512 -- self-limiting,
	 * because unconditional per-I/O logging overran SCO's kernel error
	 * log during mkfs. Measured with it on: S/G passed 1024 requests at
	 * nseg=16 while plain sends stopped at 196, i.e. after setup nearly
	 * all I/O is scatter/gather.
	 */
	vscsi_sg_count++;
	if (vscsi_debug &&
	    (vscsi_sg_count <= 8 || (vscsi_sg_count % 512) == 0))
		cmn_err(CE_CONT,
			"vscsi: SG #%d nseg=%d total=%d op=0x%x (sends %d)\n",
			vscsi_sg_count, nseg, rp->data_len, cdb[0],
			vscsi_send_count);

	s = lockb5(&hp->ha_lk);		/* see vscsi_send() for why */

	sl = vscsi_getslot(hp);
	if (sl == (struct vscsi_slot *)0) {
		unlockb(&hp->ha_lk, s);
		cmn_err(CE_WARN, "vscsi: no free slot (SG)");
		rp->host_sts = 1;
		return -1;
	}
	sl->vs_rp = rp;

	if (!vscsi_setlun(hp, sl, rp)) {
		sl->vs_busy = 0;
		unlockb(&hp->ha_lk, s);
		rp->host_sts = 1;
		return -1;
	}
	sl->vs_req->rc_id_lo     = (ulong)(sl - hp->ha_slot);
	sl->vs_req->rc_id_hi     = 0;
	sl->vs_req->rc_task_attr = VIRTIO_SCSI_S_SIMPLE;
	sl->vs_req->rc_prio      = 0;
	sl->vs_req->rc_crn       = 0;

	for (i = 0; i < VSCSI_CDB_SIZE; i++)
		sl->vs_req->rc_cdb[i] = (i < clen) ? cdb[i] : 0;

	/*
	 * Clear the SCSI-2 LUN field now it has been lifted into the virtio-scsi
	 * address (see vscsi_setlun). Safe unconditionally: OpenServer is a
	 * SCSI-2 initiator so those bits only ever mean LUN, and for LUN 0 they
	 * are already zero. BOTH submit paths need this -- doing only
	 * vscsi_send() lets INQUIRY and READ CAPACITY through, so the unit
	 * attaches with correct geometry while every buffered transfer fails.
	 */
	sl->vs_req->rc_cdb[1] &= 0x1f;

	sl->vs_resp->rs_response  = 0xff;
	sl->vs_resp->rs_status    = 0xff;
	sl->vs_resp->rs_sense_len = 0;

	/* Device-readable first, then device-writable. */
	nout = 0;
	pa[nout] = sl->vs_reqpa;  len[nout] = VSCSI_REQ_HDRLEN;  nout++;
	if (dir == SCSI_OUT)
		for (i = 0; i < nseg; i++) {
			pa[nout]  = d_ptr[i];
			len[nout] = (ulong) t_len[i];
			nout++;
		}

	nin = 0;
	pa[nout + nin]  = sl->vs_resppa;
	len[nout + nin] = VSCSI_RESP_HDRLEN;
	nin++;
	if (dir == SCSI_IN)
		for (i = 0; i < nseg; i++) {
			pa[nout + nin]  = d_ptr[i];
			len[nout + nin] = (ulong) t_len[i];
			nin++;
		}

	if (!virtio_vq_add(&hp->ha_vq[VSCSI_VQ_REQUEST], pa, len, nout, nin,
			   (void *)sl)) {
		sl->vs_busy = 0;
		unlockb(&hp->ha_lk, s);
		cmn_err(CE_WARN, "vscsi: ring full (SG, %d desc)", nout + nin);
		rp->host_sts = 1;
		return -1;
	}
	hp->ha_pending++;
	virtio_vq_notify(&hp->ha_dev, &hp->ha_vq[VSCSI_VQ_REQUEST]);
	unlockb(&hp->ha_lk, s);
	return 0;
}

/*
 * Issue one SCSI command. Returns 0 if queued, -1 on failure -- the entry
 * point's convention, not the usual 1-is-good.
 */
static int
vscsi_send(hp, rp)
	struct vscsi_ha *hp;
	REQ_IO *rp;
{
	struct vscsi_slot *sl;
	paddr_t pa[4];
	ulong   len[4];
	int     nout, nin, i, clen, dir;
	int     s;
	unchar *cdb;

	/*
	 * Use the verified offset, not &rp->scsi_cmd. With -D_M_I386 now set
	 * the two agree, but this one line silently sent an all-zero CDB for
	 * every command when they did not -- and a zero CDB is a legal TEST
	 * UNIT READY, so it completed rsp=0 sts=0 and looked healthy.
	 */
	cdb = RP_CDB(rp);
	if (vscsi_debug) {
		cmn_err(CE_CONT, "vscsi: SEND cdb=%x id=%d ha=%d\n",
			cdb[0], rp->id, rp->ha_num);
		cmn_err(CE_CONT, "vscsi:  len=%d clen=%d dir=%d\n",
			rp->data_len, vscsi_cdblen(cdb[0]), vscsi_dir(cdb[0]));
	}

	if (!hp->ha_ready) {
		rp->host_sts = 1;
		return -1;
	}

	/*
	 * MUST hold ha_lk. This function is called from vscsi_entry() at task
	 * level, but the slot array and the vring it touches are also mutated
	 * by vscsi_reap() from vscsipoll() (clock-interrupt time) and from
	 * vscsiintr() -- and, once the driver is distributed, potentially from
	 * another CPU. lockb5() covers both hazards: it raises to spl5, which
	 * keeps the local clock tick out (a tick landing inside
	 * virtio_vq_add() corrupts the ring -- which is how the box wedged
	 * with the clock handler stuck), and it spins out any other CPU.
	 * Re-entry is safe by construction, not by nesting: when vscsi_reap()
	 * calls back into us (Unit Attention resubmit, or Sdsk's io_intr
	 * starting the next request) it has already dropped the lock.
	 */
	s = lockb5(&hp->ha_lk);

	sl = vscsi_getslot(hp);
	if (sl == (struct vscsi_slot *)0) {
		unlockb(&hp->ha_lk, s);
		cmn_err(CE_WARN, "vscsi: no free slot");
		rp->host_sts = 1;
		return -1;
	}
	sl->vs_rp = rp;

	if (!vscsi_setlun(hp, sl, rp)) {
		sl->vs_busy = 0;
		unlockb(&hp->ha_lk, s);
		rp->host_sts = 1;
		return -1;
	}
	sl->vs_req->rc_id_lo     = (ulong)(sl - hp->ha_slot);
	sl->vs_req->rc_id_hi     = 0;
	sl->vs_req->rc_task_attr = VIRTIO_SCSI_S_SIMPLE;
	sl->vs_req->rc_prio      = 0;
	sl->vs_req->rc_crn       = 0;

	clen = vscsi_cdblen(cdb[0]);
	for (i = 0; i < VSCSI_CDB_SIZE; i++)
		sl->vs_req->rc_cdb[i] = (i < clen) ? cdb[i] : 0;

	/*
	 * Clear the SCSI-2 LUN field now it has been lifted into the virtio-scsi
	 * address (see vscsi_setlun). Safe unconditionally: OpenServer is a
	 * SCSI-2 initiator so those bits only ever mean LUN, and for LUN 0 they
	 * are already zero. BOTH submit paths need this -- doing only
	 * vscsi_send() lets INQUIRY and READ CAPACITY through, so the unit
	 * attaches with correct geometry while every buffered transfer fails.
	 */
	sl->vs_req->rc_cdb[1] &= 0x1f;

	sl->vs_resp->rs_response  = 0xff;
	sl->vs_resp->rs_status    = 0xff;
	sl->vs_resp->rs_sense_len = 0;

	/*
	 * Device-readable segments first, then device-writable. data_ptr is
	 * already physical, which is exactly what a descriptor wants.
	 */
	dir = vscsi_dir(cdb[0]);

	nout = 0;
	pa[nout] = sl->vs_reqpa;  len[nout] = VSCSI_REQ_HDRLEN;  nout++;
	if (dir == SCSI_OUT && rp->data_len) {
		pa[nout] = rp->data_ptr;  len[nout] = rp->data_len;  nout++;
	}

	nin = 0;
	pa[nout + nin]  = sl->vs_resppa;
	len[nout + nin] = VSCSI_RESP_HDRLEN;
	nin++;
	if (dir == SCSI_IN && rp->data_len) {
		pa[nout + nin]  = rp->data_ptr;
		len[nout + nin] = rp->data_len;
		nin++;
	}

	if (!virtio_vq_add(&hp->ha_vq[VSCSI_VQ_REQUEST], pa, len, nout, nin,
			   (void *)sl)) {
		sl->vs_busy = 0;
		unlockb(&hp->ha_lk, s);
		cmn_err(CE_WARN, "vscsi: ring full");
		rp->host_sts = 1;
		return -1;
	}
	hp->ha_pending++;
	vscsi_send_count++;
	virtio_vq_notify(&hp->ha_dev, &hp->ha_vq[VSCSI_VQ_REQUEST]);
	unlockb(&hp->ha_lk, s);
	if (vscsi_debug)
		cmn_err(CE_CONT, "vscsi: sent cdb=%x clen=%d\n", cdb[0], clen);
	return 0;
}

/* ------------------------------------------------------------------ */
/* Adapter entry point                                                 */
/* ------------------------------------------------------------------ */

int
vscsi_entry(rp)
	REQ_IO *rp;
{
	scsi_disk_info       *sdi;
	scsi_max_config_info  lim;

	if (vscsi_debug) {
		cmn_err(CE_CONT, "vscsi: E rt=%d hc=%d ext=0x%x\n",
			rp->req_type, RP_HACMD(rp), rp->ext_p);
		cmn_err(CE_CONT, "vscsi: E  ha=%d id=%d lun=%d\n",
			rp->ha_num, rp->id, rp->lun);
	}

	/*
	 * NOTE: do NOT range-check rp->id here. It is only meaningful for
	 * actual I/O requests; on host-adapter calls (SCSI_INIT etc.) it is
	 * uninitialised and reads as garbage -- we observed -16. The Sram
	 * sample's guard is `id >= 2', which lets negative values through;
	 * a strict `id != 0' rejects every HA call before it is dispatched,
	 * which is exactly the bug this comment exists to prevent.
	 */
	switch (rp->req_type) {

	case SCSI_SEND:
	case SCSI_SEND_NC:
		/*
		 * No id/lun guard here. `id' is unreliable on these calls
		 * (it reads as -16, the same garbage seen on host-adapter
		 * commands), so a range check silently rejects every real
		 * command -- which is exactly what it did, with no log line
		 * to show for it. Let the device reject bad targets.
		 */
		{
			struct vscsi_ha *hp = vscsi_findha(rp);

			if (hp == (struct vscsi_ha *)0) {
				rp->host_sts = 1;
				return -1;
			}
			return vscsi_send(hp, rp);
		}

	case SCSI_SG:
	case SCSI_SG_NC:
		{
			struct vscsi_ha *hp = vscsi_findha(rp);

			if (hp == (struct vscsi_ha *)0) {
				rp->host_sts = 1;
				return -1;
			}
			return vscsi_send_sg(hp, rp);
		}

	case SCSI_INIT:
		switch (RP_HACMD(rp)) {

		case SCSI_VER_INFO:
			if ((ulong) rp->ext_p >= KVBASE) {
				sdi = (scsi_disk_info *) rp->ext_p->data;
				sdi->magic = SCSI_V3_MAGIC;
				sdi->v_maj = SCSI_V3_VERSION;
				sdi->v_min = 0;
			}
			break;

		case SCSI_DISK_INFO:
			/*
			 * Report a BIOS-compatible geometry: data[1] heads,
			 * data[2] sectors/track, per the Sram sample.
			 *
			 * ext_p MUST be validated first. The sample
			 * dereferences it unchecked, but here it arrives as
			 * garbage (observed 0xAB100000) and writing to it
			 * page-faults: PANIC k_trap type 0x0E. Only a
			 * kernel-space pointer is safe to touch.
			 */
			if ((ulong) rp->ext_p >= KVBASE) {
				rp->ext_p->type = SCSI_DISK_INFO;
				rp->ext_p->next = (struct exten *)0;
				rp->ext_p->data[1] = VSCSI_HEADS;
				rp->ext_p->data[2] = VSCSI_SECTORS;
			} else {
				cmn_err(CE_CONT,
					"vscsi: DISK_INFO bad ext_p 0x%x\n",
					rp->ext_p);
			}
			break;

		default:		/* not an error */
			break;
		}
		break;

	case SCSI_INFO:
		/* Tell Sdsk what this adapter can do. */
		bcopy(&vscsi_info, ptok(rp->data_ptr), rp->data_len);
		break;

	case SCSI_GET_LIMITS:
		/* Bound the scan: one bus, one target. */
		lim.magic     = SCSI_MAGIC_LIMITS;
		lim.num_buses = 1;
		lim.num_ids   = 1;
		bcopy(&lim, ptok(rp->data_ptr), rp->data_len);
		break;

	default:
		break;
	}

	return 0;
}

/* ------------------------------------------------------------------ */
/* Interrupt handler -- reap completions                               */
/* ------------------------------------------------------------------ */

/*
 * A completion snapshotted out of its slot while ha_lk is held, so that
 * delivery can happen after the lock is dropped. The sense area is copied
 * in full (the device buffer is always there), vd_slen records how much of
 * it the device declared valid.
 */
struct vscsi_done {
	REQ_IO *vd_rp;
	unchar  vd_status;
	unchar  vd_response;
	unchar  vd_slen;
	unchar  vd_sense[VSCSI_SENSE_INLINE];
};

#define VSCSI_REAP_BATCH 16	/* bounds the stack; the loop drains fully */

/*
 * Reap completions. Called from the interrupt handler and from the clock
 * poll; either way the caller is at spl5, the level Sdsk's io_intr callback
 * expects to run at. (The Sram sample completes from Srampoll rather than
 * relying on a real interrupt; the poll path remains our fallback when no
 * interrupt is delivered, because SCO spins forever waiting for io_intr if
 * nothing reaps.)
 *
 * Two phases per batch. Under ha_lk: pull finished chains off the used
 * ring, snapshot status and sense into done[], free the slots. Lock
 * dropped: classify and deliver. The split is not cosmetic -- a Unit
 * Attention resubmit and Sdsk's own io_intr callback (which typically
 * starts the next request) both re-enter vscsi_send(), which takes ha_lk,
 * and lockb is not recursive. slha and dpt both use this same
 * release-around-the-callback shape.
 */
static int
vscsi_reap(hp)
	struct vscsi_ha *hp;
{
	struct vscsi_done  done[VSCSI_REAP_BATCH];
	struct vscsi_done *d;
	struct vscsi_slot *sl;
	ulong   gotlen;
	unchar *src, *dst;
	int     handled, ndone, more;
	int     i, j, s;

	handled = 0;
	do {
		more  = 0;
		ndone = 0;
		s = lockb5(&hp->ha_lk);
		while (ndone < VSCSI_REAP_BATCH) {
			sl = (struct vscsi_slot *)
			     virtio_vq_get(&hp->ha_vq[VSCSI_VQ_REQUEST],
					   &gotlen);
			if (sl == (struct vscsi_slot *)0)
				break;
			handled++;
			if (sl->vs_rp != (REQ_IO *)0) {
				d = &done[ndone++];
				d->vd_rp       = sl->vs_rp;
				d->vd_status   = sl->vs_resp->rs_status;
				d->vd_response = sl->vs_resp->rs_response;
				d->vd_slen     = sl->vs_resp->rs_sense_len;
				if (d->vd_slen > VSCSI_SENSE_INLINE)
					d->vd_slen = VSCSI_SENSE_INLINE;
				for (i = 0; i < VSCSI_SENSE_INLINE; i++)
					d->vd_sense[i] =
						sl->vs_resp->rs_sense[i];
			}
			sl->vs_rp   = (REQ_IO *)0;
			sl->vs_busy = 0;
			if (hp->ha_pending)
				hp->ha_pending--;
		}
		if (ndone == VSCSI_REAP_BATCH)
			more = 1;	/* the ring may hold more */
		unlockb(&hp->ha_lk, s);

		for (j = 0; j < ndone; j++) {
			d = &done[j];

			/*
			 * CHECK CONDITION with sense key 6 is Unit Attention
			 * -- the device telling us it was reset. It is a
			 * one-shot condition that clears once reported.
			 * Normally the initiator reads the sense and retries,
			 * but SCO hands us no sense buffer at all
			 * (scsi_sense = 0, sense_len = 0), so it cannot clear
			 * it itself and simply stalls. Absorb it here and
			 * re-issue.
			 */
			if (d->vd_status == 2 && d->vd_slen >= 3 &&
			    (d->vd_sense[2] & 0x0f) == 6) {
				if (hp->ha_ua_retry < 4) {
					hp->ha_ua_retry++;
					/*
					 * Gated: a Unit Attention on the
					 * first command after reset is
					 * entirely normal and happens once
					 * per boot. Exhausting the retries
					 * is the interesting case -- warned
					 * about below.
					 */
					if (vscsi_debug)
						cmn_err(CE_CONT,
						"vscsi: unit attention, retry %d\n",
							hp->ha_ua_retry);
					/*
					 * If the resubmit fails the request
					 * must still be completed --
					 * vscsi_send() has set host_sts, but
					 * nothing will ever call io_intr,
					 * and SCO waits on io_intr forever.
					 * Silently dropping it here hung
					 * Sdsk in exactly that way.
					 */
					if (vscsi_send(hp, d->vd_rp) != 0 &&
					    (ulong) RP_IO_INTR(d->vd_rp)
							>= KVBASE)
						(*RP_IO_INTR(d->vd_rp))
							(d->vd_rp);
					continue;
				}
				cmn_err(CE_WARN,
				"vscsi: unit attention persists after %d retries",
					hp->ha_ua_retry);
			}
			hp->ha_ua_retry = 0;

			d->vd_rp->target_sts = d->vd_status;
			d->vd_rp->host_sts =
				(d->vd_response == VIRTIO_SCSI_S_OK) ? 0 : 1;

			/*
			 * Deliver sense data. Two destinations, and both are
			 * required -- delivering to only one of them is what
			 * the certification suite caught.
			 *
			 * First, inline in the request block. This used to be
			 * a copy to RP_SCSI_SENSE() alone, which on the normal
			 * Sdsk I/O path never ran even once: TLS006 section
			 * 4.1 lists scsi_sense and sense_len as "reserved and
			 * should be set to 0", and section 4.5.1 spells out
			 * where sense actually goes --
			 *
			 *   "The host adapter driver must place the sense
			 *    data in the request block after the SCSI
			 *    command that caused the error. Fourteen bytes
			 *    of data must be returned, with the first byte
			 *    following the last byte of the command in the
			 *    SCSI CDB."
			 *
			 * "The command" there is the fixed-size command FIELD,
			 * not the variable-length CDB, and the union settles
			 * it:
			 *
			 *	unsigned char raw[12+sizeof(struct scsi_sense)];
			 *
			 * -- 12 bytes of command (the largest member, struct
			 * TwelveCmd, is exactly 12) followed by a 14-byte
			 * sense area. So sense goes at raw[12] ALWAYS,
			 * whatever the CDB length.
			 *
			 * This was previously written at raw[vscsi_cdblen()],
			 * i.e. raw[10] for a READ(10)/WRITE(10) -- two bytes
			 * early, so a reader picking sense up at raw[12] saw
			 * our sense[2] (the sense key, 0x05) where the
			 * response code 0x70 belongs and rejected the whole
			 * block. That is exactly what SCO's certification
			 * suite reported as "sense data is not valid".
			 */
			if (d->vd_slen) {
				src = d->vd_sense;
				dst = RP_CDB(d->vd_rp) + VSCSI_SENSE_OFF;
				for (i = 0; i < (int)d->vd_slen &&
				     (VSCSI_SENSE_OFF + i) < RP_CDB_MAX; i++)
					dst[i] = src[i];
			}

			/*
			 * Second, into the caller's own sense buffer when it
			 * supplied one. scsi_io_req(D4osdi) defines scsi_sense
			 * as "if non 0 (zero), the address to which the sense
			 * information should be written" -- a CONDITIONAL, and
			 * dropping the copy because the condition never held
			 * was the mistake. It never holds on the normal Sdsk
			 * I/O path, which is all that had ever been watched;
			 * it does hold for the SCSIUSERCMD2 pass-through
			 * ioctl, where <sys/scsicmd.h> ("New SCSIUSERCMD +
			 * sense") carries a user-supplied sense_ptr/sense_len
			 * that scsi_usercmd_fill() copies back to user space.
			 *
			 * Without this, every pass-through command that fails
			 * comes back with no sense at all, so the caller
			 * cannot tell why -- SCO's HBA certification suite
			 * reports it as "sense data is not valid... Maybe
			 * your adapter doesn't support the auto-sense
			 * mechanism" and fails HD_REXTEND_1 / HD_WEXTEND_2,
			 * which read and write past the end of the device.
			 * The device does reject those (sense key 5, ASC 21,
			 * LBA OUT OF RANGE); the failure was never that the
			 * command got through, only that the reason was lost.
			 *
			 * scsi_sense is a paddr_t. The two references disagree
			 * about whether such fields arrive physical or kernel
			 * virtual, so accept either, exactly as the S/G list
			 * pointers do.
			 */
			if (d->vd_slen && RP_SCSI_SENSE(d->vd_rp) != 0 &&
			    RP_SENSE_LEN(d->vd_rp) != 0) {
				int max = (int)(unchar) RP_SENSE_LEN(d->vd_rp);

				src = d->vd_sense;
				dst = (unchar *) vscsi_sgbase(
					RP_SCSI_SENSE(d->vd_rp));
				for (i = 0; i < (int)d->vd_slen && i < max; i++)
					dst[i] = src[i];
			}

			/*
			 * The two fields the sense delivery above turns on.
			 * Gated: on the normal Sdsk path both are 0 and this
			 * says nothing, but on the SCSIUSERCMD2 path it is
			 * how you confirm the caller really did supply a
			 * buffer (observed: slen=14, ssense non-zero).
			 */
			if (vscsi_debug && d->vd_status == 2)
				cmn_err(CE_CONT,
			"vscsi:  CC tgt=%d slen=%d ssense=0x%x cmdlen=%d\n",
					(int)(unchar) d->vd_rp->target_sts,
					(int)(unchar) RP_SENSE_LEN(d->vd_rp),
					RP_SCSI_SENSE(d->vd_rp),
					(int)(unchar) d->vd_rp->cmdlen);

			if (vscsi_debug)
			    cmn_err(CE_CONT, "vscsi: done rsp=%d sts=%d slen=%d\n",
				d->vd_response, d->vd_status, d->vd_slen);
			if (vscsi_debug)
			    cmn_err(CE_CONT, "vscsi:  op=0x%x cdb=%d cmdlen=%d\n",
				RP_CDB(d->vd_rp)[0],
				vscsi_cdblen(RP_CDB(d->vd_rp)[0]),
				d->vd_rp->cmdlen);
			/*
			 * Gated, like the two above it. This used to be
			 * unconditional, which is fine for a disk -- sense is
			 * rare -- and wrong for a CD-ROM, which returns it as
			 * a matter of course: unit attention on every medium
			 * access, not-ready while it spins up, and an illegal
			 * request for each capability the installer probes
			 * for and does not find. The result was sense lines
			 * scrolling over the OpenServer installer. The same
			 * mistake, with the same cause, once overran SCO's
			 * error log during mkfs (see the S/G counter above);
			 * "vscsi: WARNING: err: Error log buffer overflow" in
			 * /usr/adm/messages is what it looks like.
			 *
			 * Nothing is lost by gating it: target_sts carries the
			 * status to the caller either way, and the sense
			 * itself is delivered to both documented destinations.
			 */
			if (vscsi_debug && d->vd_slen >= 3)
				cmn_err(CE_CONT, "vscsi:  sense %x %x %x\n",
					d->vd_sense[0], d->vd_sense[2],
					d->vd_sense[12]);
			/*
			 * Verify our struct layout matches the kernel's
			 * before jumping through this pointer. scsi.h wraps
			 * union scsi_cdb in #pragma pack(1); if idcomp
			 * treats that differently from the compiler that
			 * built the kernel, every field after the CDB shifts
			 * and io_intr is garbage. Real cc puts it at
			 * offset 88.
			 */
			if (vscsi_debug)
				cmn_err(CE_CONT, "vscsi: iointr=0x%x\n",
					RP_IO_INTR(d->vd_rp));
			if ((ulong) RP_IO_INTR(d->vd_rp) >= KVBASE)
				(*RP_IO_INTR(d->vd_rp))(d->vd_rp);
			else
				cmn_err(CE_CONT, "vscsi: BAD io_intr\n");
		}
	} while (more);
	return handled;
}

/*
 * Called on every clock tick. This is what actually completes requests --
 * see vscsi_reap() above.
 */
int
vscsipoll(pps)
	int pps;	/* previous process spl, not the current one */
{
	struct vscsi_ha *hp;
	int s, n;

	if (vscsi_nha == 0)
		return 0;

	/*
	 * Mirror Srampoll exactly. Two things matter here and both were
	 * missing before:
	 *
	 *  - the pps >= 4 guard stops re-entry when we drop the spl below;
	 *  - the completion path MUST run at spl5 (the disk spl). Sdsk's
	 *    io_intr callback manipulates queues that are protected at that
	 *    level; calling it at whatever ipl the clock tick happens to be
	 *    at leaves SCO's state inconsistent and it never continues.
	 *
	 * This spl5 is NOT redundant with ha_lk: vscsi_reap() drops the lock
	 * (and with it lockb5's spl raise) before delivering completions, so
	 * it is this wrapper that keeps the io_intr callbacks at spl5.
	 */
	if (pps >= 4)
		return 0;

	for (n = 0; n < vscsi_nha; n++) {
		hp = &vscsi_ha[n];
		if (hp->ha_ready && hp->ha_pending) {
			s = spl5();
			(void) vscsi_reap(hp);
			splx(s);
		}
	}
	return 0;
}

/*
 * One handler for every adapter: each Sharegister() call names this same
 * function, and PCI IRQs may be shared anyway, so ask every ready adapter
 * whether it interrupted. Reading the ISR register clears it, which
 * deasserts that adapter's INTx.
 */
int
vscsiintr()
{
	struct vscsi_ha *hp;
	unchar isr;
	int    n, handled;

	handled = 0;
	for (n = 0; n < vscsi_nha; n++) {
		hp = &vscsi_ha[n];
		if (!hp->ha_ready)
			continue;
		isr = virtio_isr_ack(&hp->ha_dev);
		if ((isr & VIRTIO_PCI_ISR_QUEUE) == 0)
			continue;
		if (vscsi_debug)
			cmn_err(CE_CONT, "vscsi: INTR ha=%d isr=0x%x\n",
				n, isr);
		if (vscsi_reap(hp))
			handled = 1;
	}
	return handled;
}

/* ------------------------------------------------------------------ */
/* Initialisation                                                      */
/* ------------------------------------------------------------------ */

/*
 * Bring up one adapter: handshake, queues, slot memory, addressing limits.
 * Returns 1 on success. On failure the adapter is marked FAILED and its
 * ha_ready stays 0, but it keeps its position in vscsi_ha[] -- ha numbering
 * must match discovery order or every later adapter's I/O is misrouted.
 */
static int
vscsi_initha(hp)
	struct vscsi_ha *hp;
{
	caddr_t page;
	paddr_t pgpa;
	ulong   off;
	int     i;

	virtio_reset(&hp->ha_dev);
	virtio_add_status(&hp->ha_dev, VIRTIO_CONFIG_S_ACKNOWLEDGE);
	virtio_add_status(&hp->ha_dev, VIRTIO_CONFIG_S_DRIVER);
	virtio_negotiate(&hp->ha_dev, 0L);

	for (i = 0; i < VSCSI_NUM_VQ; i++) {
		if (!virtio_vq_setup(&hp->ha_dev, &hp->ha_vq[i], (ushort)i)) {
			cmn_err(CE_WARN, "vscsi: queue %d setup failed", i);
			virtio_set_failed(&hp->ha_dev);
			return 0;
		}
	}

	page = (caddr_t) getcpages(2, MEM_KVMAPPED);
	if (page == (caddr_t)0) {
		cmn_err(CE_WARN, "vscsi: getcpages failed");
		virtio_set_failed(&hp->ha_dev);
		return 0;
	}
	for (i = 0; i < VSCSI_NSLOT * VSCSI_SLOTSZ; i++)
		page[i] = 0;
	pgpa = VIRTIO_KVTOPHYS(page);

	for (i = 0; i < VSCSI_NSLOT; i++) {
		off = (ulong)i * VSCSI_SLOTSZ;
		hp->ha_slot[i].vs_req =
			(struct virtio_scsi_req_cmd *)(page + off);
		hp->ha_slot[i].vs_resp =
			(struct virtio_scsi_resp_cmd *)(page + off + 64);
		hp->ha_slot[i].vs_reqpa  = pgpa + off;
		hp->ha_slot[i].vs_resppa = pgpa + off + 64;
		hp->ha_slot[i].vs_busy   = 0;
		hp->ha_slot[i].vs_rp     = (REQ_IO *)0;
	}

	/*
	 * Addressing limits, so vscsi_setlun() can reject an out-of-range
	 * target before handing it to the device. QEMU reports max_target 255
	 * and max_lun 16383; a 0 here just means "no limit reported".
	 */
	hp->ha_max_target = (int) virtio_cfg_readw(&hp->ha_dev,
						   VSCSI_CFG_MAX_TARGET);
	hp->ha_max_lun    = (int) virtio_cfg_readl(&hp->ha_dev,
						   VSCSI_CFG_MAX_LUN);

	virtio_add_status(&hp->ha_dev, VIRTIO_CONFIG_S_DRIVER_OK);
	hp->ha_ready = 1;
	return 1;
}

int
vscsiinit()
{
	struct vscsi_ha *hp;
	int     n, i;
	static char cfgstr[VSCSI_MAX_HA][28];
	static char cfgtpl[] = "type=vscsi ha=0 virtio-scsi";

	for (n = 0; n < VSCSI_MAX_HA; n++) {
		hp = &vscsi_ha[n];
		if (!virtio_pci_find(VIRTIO_ID_SCSI, (ushort)n, &hp->ha_dev))
			break;
		vscsi_nha = n + 1;
		(void) vscsi_initha(hp);
	}

	if (vscsi_nha == 0) {
		cmn_err(CE_NOTE, "vscsi: no virtio-scsi device present");
		return 0;
	}

	/*
	 * Capabilities reported for SCSI_INFO. do_sg = 1 now that
	 * vscsi_send_sg() handles SCSI_SG/SCSI_SG_NC: a virtio descriptor
	 * chain is natively a scatter/gather list, so Sdsk can hand us one
	 * multi-segment request instead of splitting it into several
	 * single-buffer ones. do_drive32 says we can DMA anywhere in 32-bit
	 * space, so Sdsk need not bounce buffers below 16MB.
	 */
	vscsi_info.do_sg        = 1;
	/*
	 * do_buffer = 1: "driver buffers commands, no sleep in entry
	 * function" (Sram sample). That describes us exactly -- vscsi_send
	 * queues the request and returns immediately, completing later from
	 * the clock poll. With 0 we were telling Sdsk the entry function
	 * blocks until the work is done, so it waited in a way our
	 * asynchronous completion never satisfied.
	 *
	 * The sample notes this pairs with Sdsk_sleep_option = 0 in
	 * pack.d/Sdsk/space.c -- already 0 on this system, checked.
	 */
	vscsi_info.do_buffer    = 1;
	vscsi_info.do_tagged    = 0;
	vscsi_info.do_resource  = 0;
	vscsi_info.do_xtd_sense = 1;
	vscsi_info.do_drive32   = 1;
	vscsi_info.do_nomaps    = 1;
	vscsi_info.do_reset     = 0;
	vscsi_info.do_abort     = 0;

	/*
	 * Multithreading admission, before any Sharegister() call and exactly
	 * as slha does it: all_io(1) short-circuits on machines where every
	 * CPU can reach the I/O bus; otherwise intrallocs() must grant
	 * multiprocessor access before scsi_distributed() may declare the
	 * entry point multithreaded. On failure the driver still works --
	 * MPX just funnels it to the base CPU, exactly as before.
	 *
	 * This is only safe now that ha_lk exists: distribution means
	 * vscsi_entry(), vscsiintr() and vscsipoll() may genuinely run
	 * concurrently on different CPUs.
	 */
	{
		int mpok = 1;

		if (!all_io(1) &&
		    intrallocs(&vscsi_tab_lk, vscsi_entry, "vscsi") == -1) {
			cmn_err(CE_WARN,
				"vscsi: multiprocessor access denied");
			mpok = 0;
		}
		if (mpok)
			(void) scsi_distributed(vscsi_entry);
	}

	for (n = 0; n < vscsi_nha; n++) {
		SHAREG_EX *r = &vscsi_drvrreg[n];

		hp = &vscsi_ha[n];
		if (!hp->ha_ready)
			continue;

		r->entry          = vscsi_entry;
		r->intr           = vscsiintr;
		r->version        = SHAREG_VERSION2;
		/*
		 * NOT 7. This is the target SCO probes for the root disk, not
		 * the initiator ID -- with 7 the BTLD install scanned target 7,
		 * got VIRTIO_SCSI_S_BAD_TARGET (rsp=3) for TEST UNIT READY and
		 * INQUIRY, and reported "hd: no root disk controller was found".
		 * The Sram sample uses 0. It never showed up on an installed
		 * system because mscsi names the target explicitly there, so
		 * root_id is only consulted during installation.
		 */
		r->root_id        = 0;
		/*
		 * Per shareg_ex(D4osdi): 1 if this adapter is named in
		 * mscsi(F), 0 if not. The spec's rule is to test whether the
		 * Link Kit filled in our cfg entry. Hardcoding 0 here worked
		 * for one adapter -- the first registration is the default
		 * assignment anyway -- but with several adapters the mscsi
		 * `ha' number must bind to the right registration, and that
		 * binding is what in_use declares.
		 */
		r->in_use         = (vscsicfg[n].ha_name != 0 ||
				     vscsicfg[n].adapter_entry != 0) ? 1 : 0;
		r->int_vec        = SHAREG_PCI_INTVEC(hp->ha_dev.vd_pci.slotnum,
						      hp->ha_dev.vd_pci.funcnum);
		r->bus            = (int) hp->ha_dev.vd_pci.busnum;
		/*
		 * These four were all 0, which is why our IRQ was never
		 * delivered and why we fell back to completing from the clock
		 * poll. From <sys/ci/ciintr.h>: IROUTE_NONE is 0 and
		 * IMODE_NONE is 0 -- so we were asking for no routing, no
		 * sharing mode, and (processor_mask 0) no processor allowed to
		 * field it. The Sram sample sets IROUTE_GLOBAL / 5 /
		 * IMODE_EXCLUSIVE / 1; we must share rather than claim
		 * exclusively, because our PCI IRQ 11 is also usb_uhci's.
		 *
		 * Historical caution, still true: registration parameters were
		 * once blamed for MPX receiving no interrupts at all, and six
		 * different sets were built, installed and measured before the
		 * real cause turned out to be PCI topology -- a controller
		 * behind a bridge (scsihw=virtio-scsi-single) whose INTx
		 * swizzle MPX does not follow when it programs the I/O APIC
		 * from the MP table. On the root bus interrupts always worked.
		 * See docs/INSTALL-PROXMOX.md for those measurements.
		 *
		 * The distributed settings below are therefore NOT an attempt
		 * to fix interrupt delivery. They are the second half of real
		 * multithreading -- pointless without ha_lk, meaningful only
		 * with it -- and they follow slha, the one SCO HBA that ships
		 * this way.
		 */
		r->route          = IROUTE_GLOBAL;
		r->weight         = 5;
		/*
		 * IMODE_SHARED_CDRIVERIPL, not IMODE_SHARED_DRIVERIPL. The
		 * distinction only matters with more than one controller:
		 * DRIVERIPL declares that every controller of this driver
		 * shares ONE vector, but with a controller per disk the PCI
		 * IRQs genuinely differ (10 and 11 on the test VM), and the
		 * kernel discards a second registration that contradicts the
		 * declaration -- silently, so mscsi's ha 1 never resolves and
		 * the second disk's probe never reaches the driver. CDRIVERIPL
		 * ("each controller may either share a vector with one of the
		 * other controllers or use a separate vector",
		 * shareg_ex(D4osdi)) matches reality; it is also what SCO's
		 * own slha driver passes (type 4 in its Sharegister block).
		 */
		r->mode           = IMODE_SHARED_CDRIVERIPL;
		/*
		 * The processor set this adapter's interrupt may be routed
		 * to. remap_driver_cpu(D3oddi) turns the space.c-style
		 * tunable (vscsi_drvr_processor, DRIVER_CPU_DEFAULT = any)
		 * into the mask the kernel wants; ICPU_EXANY is the
		 * documented default preference and what slha passes. This
		 * DRIVER_CPU_DEFAULT means no preference; a site can pin the
		 * handler to a CPU by patching vscsi_drvr_processor.
		 */
		r->processor_mask = remap_driver_cpu(vscsi_drvr_processor,
							ICPU_EXANY);
		r->iosaddr        = 0;	/* "Reserved - set to zero" */
		r->ioeaddr        = 0;
		Sharegister(r);

		/*
		 * Patch the adapter number into a per-adapter copy of the
		 * config string -- printcfg() keeps the pointer, so a shared
		 * buffer would leave every line reading the last number.
		 */
		for (i = 0; cfgtpl[i] != '\0'; i++)
			cfgstr[n][i] = cfgtpl[i];
		cfgstr[n][i] = '\0';
		cfgstr[n][14] = (char)('0' + n);

		printcfg("vscsi", (unsigned) hp->ha_dev.vd_iobase, 0x3f,
			 (int) hp->ha_dev.vd_irq, -1, cfgstr[n]);
	}

	return 1;
}
