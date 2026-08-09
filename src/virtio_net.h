/*
 * virtio_net.h -- legacy virtio-net protocol for SCO OpenServer 5.0.7
 *
 * Device-specific definitions only. The transport (PCI discovery, feature
 * negotiation, vrings, notify, reap) is in virtio.h / virtio.c and is shared
 * with the virtio-scsi driver unchanged.
 *
 * *** PRE-ANSI (K&R) C *** -- see virtio.h for the rules.
 *
 * Copyright (c) 2026. Released under the MIT license.
 */

#ifndef _VIRTIO_NET_H
#define _VIRTIO_NET_H

/* ------------------------------------------------------------------ */
/* Virtqueues                                                          */
/*                                                                     */
/* Legacy virtio-net has receive on 0 and transmit on 1. A control     */
/* queue exists at 2 only if VIRTIO_NET_F_CTRL_VQ is negotiated, which  */
/* we do not do -- see the feature discussion below.                    */
/* ------------------------------------------------------------------ */

#define VNET_VQ_RX	0
#define VNET_VQ_TX	1
#define VNET_NUM_VQ	2

/* ------------------------------------------------------------------ */
/* Device-specific configuration space, offsets from                   */
/* VIRTIO_PCI_CONFIG_OFF.                                              */
/* ------------------------------------------------------------------ */

#define VNET_CFG_MAC		0	/* 6 bytes, valid if F_MAC   */
#define VNET_CFG_STATUS		6	/* 16 bits, valid if F_STATUS */

/* Values for VNET_CFG_STATUS */
#define VNET_S_LINK_UP		1

/* ------------------------------------------------------------------ */
/* Feature bits                                                        */
/* ------------------------------------------------------------------ */

#define VIRTIO_NET_F_CSUM		0	/* device handles csum       */
#define VIRTIO_NET_F_GUEST_CSUM		1	/* driver handles csum       */
#define VIRTIO_NET_F_MAC		5	/* device supplies MAC       */
#define VIRTIO_NET_F_GSO		6
#define VIRTIO_NET_F_GUEST_TSO4		7
#define VIRTIO_NET_F_GUEST_TSO6		8
#define VIRTIO_NET_F_GUEST_ECN		9
#define VIRTIO_NET_F_GUEST_UFO		10
#define VIRTIO_NET_F_HOST_TSO4		11
#define VIRTIO_NET_F_HOST_TSO6		12
#define VIRTIO_NET_F_HOST_ECN		13
#define VIRTIO_NET_F_HOST_UFO		14
#define VIRTIO_NET_F_MRG_RXBUF		15	/* mergeable rx buffers      */
#define VIRTIO_NET_F_STATUS		16	/* link status in config     */
#define VIRTIO_NET_F_CTRL_VQ		17	/* control queue present     */
#define VIRTIO_NET_F_CTRL_RX		18	/* rx mode control           */
#define VIRTIO_NET_F_CTRL_VLAN		19

/*
 * What we ask for, and deliberately what we do not.
 *
 * MAC     -- so the device tells us our address instead of inventing one.
 * STATUS  -- link state, cheap to read and useful for diagnostics.
 *
 * Everything else stays off, on purpose:
 *
 *   MRG_RXBUF would make the header 12 bytes instead of 10 and let the device
 *   split one frame across several buffers. Our receive path allocates one
 *   STREAMS message block per frame (as intr(D2mdi) prescribes), so a frame
 *   spanning buffers buys nothing and complicates reassembly.
 *
 *   CSUM/GSO/TSO/UFO are offloads. SCO's stack computes its own checksums and
 *   never hands down an oversized segment to be split, so claiming them would
 *   mean lying about work we do not do.
 *
 *   CTRL_VQ/CTRL_RX would let us program multicast filters into the device.
 *   MDI already requires the driver to filter in software against mctbl_t
 *   (see intr(D2mdi)), so the third queue would be redundant complexity.
 */
#define VNET_DRIVER_FEATURES \
	((1L << VIRTIO_NET_F_MAC) | (1L << VIRTIO_NET_F_STATUS))

/* ------------------------------------------------------------------ */
/* Packet header                                                       */
/*                                                                     */
/* Every buffer in both directions is this header followed by the      */
/* ethernet frame. Without MRG_RXBUF the header is 10 bytes and has no  */
/* num_buffers field -- do NOT use sizeof, which pads it to 12.        */
/* ------------------------------------------------------------------ */

struct virtio_net_hdr {
	unchar	vnh_flags;
	unchar	vnh_gso_type;
	ushort	vnh_hdr_len;
	ushort	vnh_gso_size;
	ushort	vnh_csum_start;
	ushort	vnh_csum_offset;
};

#define VNET_HDRLEN	10	/* wire length; sizeof() would say 12 */

/* vnh_flags */
#define VNET_HDR_F_NEEDS_CSUM	1
#define VNET_HDR_F_DATA_VALID	2

/* vnh_gso_type */
#define VNET_HDR_GSO_NONE	0
#define VNET_HDR_GSO_TCPV4	1
#define VNET_HDR_GSO_UDP	3
#define VNET_HDR_GSO_TCPV6	4
#define VNET_HDR_GSO_ECN	0x80

/* ------------------------------------------------------------------ */
/* Frame sizing                                                        */
/* ------------------------------------------------------------------ */

#define VNET_ETH_ALEN		6		/* MAC address length      */
#define VNET_ETH_HDRLEN		14		/* dst + src + type        */
#define VNET_MTU		1500
#define VNET_MAXFRAME		(VNET_ETH_HDRLEN + VNET_MTU)	/* 1514 */
#define VNET_MINFRAME		60		/* pad shorter than this   */

/*
 * Per-buffer allocation: header plus a full frame plus the 4-byte FCS the
 * device may include. Rounded up so buffers stay aligned.
 */
#define VNET_BUFSZ		(VNET_HDRLEN + VNET_MAXFRAME + 4 + 2)

/* How many buffers we keep posted on each ring. */
#define VNET_NRX		64
#define VNET_NTX		64

#endif /* _VIRTIO_NET_H */
