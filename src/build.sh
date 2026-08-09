#!/bin/sh
# Build a virtio driver for SCO with the in-box Link Kit toolchain.
#   sh build.sh [drivername]      (default: vscsi)
#
# Each driver package contains the shared transport plus exactly one
# device driver -- two drivers in one kernel would define the transport
# symbols twice and fail to link.
#
# Guards against two Link Kit traps:
#  1. idcomp writes diagnostics to stderr AND partial assembly to stdout,
#     so gate on exit status, never on whether a .o appeared.
#  2. idinstall consumes EVERY file in the directory it runs from, so the
#     package is assembled in ./pkg with only Driver.o, Master, System.
#
# sh build.sh <driver>
#
# Each driver bundles the shared transport (virtio.o, virtio_mb.o). Linking two
# virtio drivers into one kernel therefore used to fail:
#	Symbol virtio_vq_enable_intr in vnet/Driver.o is multiply defined.
#	First defined in vscsi/Driver.o
# because the Link Kit links every package's Driver.o into a single image.
#
# Rather than make one driver depend on the other, each gets its own privately
# named copy: every exported transport symbol is renamed to <driver>_<symbol>
# at preprocess time. Definitions and uses are both renamed because they all
# pass through cpp with the same -D list, so no source changes are needed.
# The cost is one extra copy of ~23KB of transport code per driver, which is
# cheaper than the coupling.
#
DRV=${1:-vscsi}
CPP=/lib/idcpp
COMP=/lib/idcomp
AS=/bin/idas
LD=/bin/idld
#
# CRITICAL: <sys/scsi.h> selects its structure packing with
#	#ifdef _M_I386 / #pragma pack(4) / #else / #pragma pack(2) / #endif
# Real cc predefines _M_I386; idcpp does not. Without it every struct in
# scsi.h -- including struct scsi_io_req -- is laid out with 2-byte packing
# while the kernel used 4, so every field from req_forw onwards sits 4 bytes
# earlier than the kernel put it. Symptoms that cost days: the CDB read as all
# zeros (a valid TEST UNIT READY, so it looked like it worked), data_len read
# back as 0x0601 (really dir=1,cmdlen=6), id as -16, ext_p as garbage.
#
# _NO_PROTOTYPE is needed by any driver that includes <sys/stream.h> (i.e. the
# MDI network driver). Those headers declare routines as
#	extern mblk_t *allocb __P((register int, uint));
# and <sys/cdefs.h> only expands __P(protos) to () when _NO_PROTOTYPE is
# defined -- otherwise it expands to the prototype itself, which the Link Kit
# compiler cannot parse. Harmless for the SCSI driver.
DEFS="-D_M_I386 -DM_I386 -DM_UNIX -D_NO_PROTOTYPE"

# Exported transport symbols, privatised per driver -- see the header comment.
VSYMS="virtio_pci_find virtio_reset virtio_get_status virtio_add_status \
virtio_set_failed virtio_host_features virtio_negotiate virtio_cfg_readb \
virtio_cfg_readw virtio_cfg_readl virtio_vq_setup virtio_vq_free \
virtio_vq_add virtio_vq_notify virtio_vq_get virtio_isr_ack \
virtio_vq_disable_intr virtio_vq_enable_intr virtio_read_used_idx \
virtio_publish_avail virtio_barrier"
for sym in $VSYMS; do
	DEFS="$DEFS -D$sym=${DRV}_$sym"
done
OBJS=""
fail=0
for f in virtio_mb virtio $DRV; do
	rm -f $f.i $f.s $f.o $f.err
	$CPP $DEFS $f.c > $f.i 2>/dev/null
	if $COMP < $f.i > $f.s 2> $f.err; then
		if $AS $f.s; then echo "OK   $f.o"; else echo "FAIL asm $f"; fail=1; fi
		OBJS="$OBJS $f.o"
	else
		echo "FAIL $f.c:"; cat $f.err; fail=1
	fi
done
[ $fail -ne 0 ] && exit 1
rm -rf pkg; mkdir pkg
if $LD -r -o pkg/Driver.o $OBJS; then
	cp Master.$DRV pkg/Master
	cp System.$DRV pkg/System
	echo "OK   pkg/Driver.o for '$DRV' (`ls -l pkg/Driver.o | awk '{print $5}'` bytes)"
	echo "Now: cd pkg && /etc/conf/bin/idinstall -a $DRV"
else
	echo "FAIL link"; exit 1
fi
exit 0
