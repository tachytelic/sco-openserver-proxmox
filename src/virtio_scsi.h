/*
 * virtio_scsi.h -- virtio-scsi protocol definitions
 *
 * Layout must match the hypervisor byte-for-byte. Pre-ANSI (K&R) C:
 * no prototypes, no volatile, no ANSI integer suffixes. See virtio.h.
 *
 * Copyright (c) 2026. Released under the MIT license.
 */

#ifndef _VIRTIO_SCSI_H
#define _VIRTIO_SCSI_H

/* ------------------------------------------------------------------ */
/* Queue layout                                                        */
/*                                                                     */
/* virtio-scsi fixes the first two queues and puts request queues      */
/* after them. We use exactly one request queue.                       */
/* ------------------------------------------------------------------ */

#define VSCSI_VQ_CONTROL	0
#define VSCSI_VQ_EVENT		1
#define VSCSI_VQ_REQUEST	2
#define VSCSI_NUM_VQ		3

/* ------------------------------------------------------------------ */
/* Device-specific configuration space (offsets from                   */
/* VIRTIO_PCI_CONFIG_OFF)                                              */
/* ------------------------------------------------------------------ */

#define VSCSI_CFG_NUM_QUEUES		0	/* 32 */
#define VSCSI_CFG_SEG_MAX		4	/* 32 */
#define VSCSI_CFG_MAX_SECTORS		8	/* 32 */
#define VSCSI_CFG_CMD_PER_LUN		12	/* 32 */
#define VSCSI_CFG_EVENT_INFO_SIZE	16	/* 32 */
#define VSCSI_CFG_SENSE_SIZE		20	/* 32 */
#define VSCSI_CFG_CDB_SIZE		24	/* 32 */
#define VSCSI_CFG_MAX_CHANNEL		28	/* 16 */
#define VSCSI_CFG_MAX_TARGET		30	/* 16 */
#define VSCSI_CFG_MAX_LUN		32	/* 32 */

/* Feature bits */
#define VIRTIO_SCSI_F_INOUT		0
#define VIRTIO_SCSI_F_HOTPLUG		1
#define VIRTIO_SCSI_F_CHANGE		2

/*
 * sense_size and cdb_size are writable in the config space, but the
 * defaults are what every implementation uses and what the request layout
 * below is sized for. We do not change them.
 */
#define VSCSI_CDB_SIZE		32
#define VSCSI_SENSE_SIZE	96

/* ------------------------------------------------------------------ */
/* Request / response                                                  */
/*                                                                     */
/* The request is split into a device-READABLE header (this struct),   */
/* then any data-out, then a device-WRITABLE response header, then any */
/* data-in. That maps onto virtio_vq_add()'s nout/nin split directly.  */
/* ------------------------------------------------------------------ */

struct virtio_scsi_req_cmd {
	unchar	rc_lun[8];		/* see VSCSI_SET_LUN below */
	ulong	rc_id_lo;		/* 64-bit request id, split to avoid */
	ulong	rc_id_hi;		/*   long long (not in this compiler) */
	unchar	rc_task_attr;
	unchar	rc_prio;
	unchar	rc_crn;
	unchar	rc_cdb[VSCSI_CDB_SIZE];
};

struct virtio_scsi_resp_cmd {
	ulong	rs_sense_len;
	ulong	rs_residual;
	ushort	rs_status_qualifier;
	unchar	rs_status;		/* SCSI status (0 = GOOD) */
	unchar	rs_response;		/* VIRTIO_SCSI_S_* below */
	unchar	rs_sense[VSCSI_SENSE_SIZE];
};

/* Task attributes */
#define VIRTIO_SCSI_S_SIMPLE		0
#define VIRTIO_SCSI_S_ORDERED		1
#define VIRTIO_SCSI_S_HEAD		2
#define VIRTIO_SCSI_S_ACA		3

/* Response codes (rs_response) */
#define VIRTIO_SCSI_S_OK		0
#define VIRTIO_SCSI_S_OVERRUN		1
#define VIRTIO_SCSI_S_ABORTED		2
#define VIRTIO_SCSI_S_BAD_TARGET	3
#define VIRTIO_SCSI_S_RESET		4
#define VIRTIO_SCSI_S_BUSY		5
#define VIRTIO_SCSI_S_TRANSPORT_FAILURE	6
#define VIRTIO_SCSI_S_TARGET_FAILURE	7
#define VIRTIO_SCSI_S_NEXUS_FAILURE	8
#define VIRTIO_SCSI_S_FAILURE		9

/*
 * Build the 8-byte LUN field. virtio-scsi uses the SAM addressing form:
 * byte 0 is always 1, byte 1 is the target, bytes 2-3 are the LUN with
 * 0x4000 set, and the rest are zero.
 */
#define VSCSI_SET_LUN(l, target, lun)			\
	do {						\
		(l)[0] = 1;				\
		(l)[1] = (unchar)(target);		\
		(l)[2] = (unchar)(0x40 | (((lun) >> 8) & 0x3f)); \
		(l)[3] = (unchar)((lun) & 0xff);	\
		(l)[4] = 0; (l)[5] = 0;			\
		(l)[6] = 0; (l)[7] = 0;			\
	} while (0)

#endif /* _VIRTIO_SCSI_H */
