/ virtio_fence.asm -- full memory barrier for the SCO virtio transport
/
/ virtio_vq_notify() publishes a new avail index and then reads the device's
/ used->flags to decide whether a notify is needed. x86 keeps stores in
/ order but may execute a later LOAD before an earlier STORE has left the
/ store buffer. Without a barrier between the two, this can happen:
/
/	driver: store avail->idx          (still in the store buffer)
/	driver: load  used->flags         -> sees NO_NOTIFY, skips the kick
/	device: clears NO_NOTIFY, re-checks avail->idx -> sees the OLD index
/	device: goes to sleep
/
/ and the request is never seen. The device side does the mirror-image
/ barrier, so one here closes the window.
/
/ xchg with a memory operand asserts LOCK implicitly on every x86 since the
/ 386, which makes it a full barrier without needing mfence (SSE2 -- absent
/ on the pentium2/pentium3 CPU models these guests run) or a `lock' prefix
/ the Link Kit assembler may not accept. %eax is preserved.
/
/ This is a .asm, not a .s, because build.sh renames the symbol per driver
/ (see its header) and writes the result as a .s, which is a build product.
/ The Link Kit C compiler (idcomp) cannot generate a locked instruction, so
/ this cannot be C.

	.text
	.align	4
	.globl	virtio_fence
virtio_fence:
	pushl	%eax
	xchgl	%eax,(%esp)
	popl	%eax
	ret
