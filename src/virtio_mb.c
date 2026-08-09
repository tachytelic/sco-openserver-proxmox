/*
 * virtio_mb.c -- device-memory accessors for the SCO virtio transport
 *
 * *** THIS FILE MUST BE COMPILED AS ITS OWN TRANSLATION UNIT. ***
 *
 * The Link Kit compiler (idcomp) is pre-ANSI and rejects `volatile'. The
 * vring is shared memory: the device writes the used ring and reads the
 * available ring while we are running. Without `volatile' there is nothing
 * stopping a compiler from caching a value in a register across a loop, or
 * from reordering our stores.
 *
 * Putting every such access behind an ordinary function call in a separate
 * file solves both problems, and does so more strongly than `volatile'
 * would have: the compiler cannot see these bodies, so it must genuinely
 * load from memory at each call, and it cannot move stores across the call
 * boundary. There is no link-time optimisation in this toolchain to undo
 * that.
 *
 * The cost is a function call per access. On the ring-management path that
 * is a handful of calls per request -- irrelevant next to the ~98,000
 * VM exits/sec that emulated PIO was costing us.
 *
 * x86 note: the CPU's own store ordering means no fence instruction is
 * needed. The only hazard is the *compiler*, which is what this file
 * addresses.
 *
 * Pre-ANSI (K&R) C -- see virtio.h.
 *
 * Copyright (c) 2026. Released under the MIT license.
 */

#ifndef _INKERNEL
#define _INKERNEL 1
#endif

#include <sys/types.h>
#include <sys/param.h>
#include <sys/pci.h>

#include "virtio.h"

/*
 * Read the used-ring index. The device increments this as it completes
 * requests, so it must be re-read from memory every time.
 */
ushort
virtio_read_used_idx(vq)
	struct virtio_vq *vq;
{
	return vq->vq_used->vu_idx;
}

/*
 * Publish a descriptor chain to the available ring.
 *
 * The ring-slot store must become visible before the index bump, or the
 * device can observe an incremented index pointing at a stale slot. On x86
 * the hardware guarantees store order; this function exists to stop the
 * *compiler* reordering the two.
 */
void
virtio_publish_avail(vq, head)
	struct virtio_vq *vq;
	ushort head;
{
	ushort idx;

	idx = vq->vq_avail->va_idx;
	vq->vq_avail->va_ring[idx % vq->vq_num] = head;
	vq->vq_avail->va_idx = idx + 1;
}

/*
 * Compiler barrier. Calling an opaque function forces anything held in
 * registers to be written back and re-read afterwards.
 */
void
virtio_barrier()
{
}
