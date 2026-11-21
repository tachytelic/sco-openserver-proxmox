# SCO OpenServer on Proxmox

VirtIO drivers that let **SCO OpenServer 5.0.5, 5.0.6 and 5.0.7** run on
Proxmox VE, and on any other QEMU/KVM host, with paravirtualised disk and
network.

You can install OpenServer directly onto a virtio disk, boot from it with no
boot string, and run with no emulated IDE hardware at all.

```
%vscsi    0xE040-0xE07F  10  -  type=vscsi ha=0 virtio-scsi
%cd-rom   -  -  -  type=S ha=0 id=0 lun=0 bus=0 ht=vscsi
%disk     -  -  -  type=S ha=0 id=0 lun=1 bus=0 ht=vscsi
%Sdsk     -  -  -  cyls=1044 hds=255 secs=63 unit=0 fts=sdb
```

## Why

OpenServer has no virtio support of its own. Without it you are stuck with
emulated IDE or an emulated LSI SCSI card. Both are slow, and IDE brings its own
problems on a modern hypervisor.

If you are moving a legacy OpenServer box off ESXi, this is the missing piece.

## What you get

| | |
|---|---|
| **`vscsi`** | virtio-scsi host adapter driver. Boot disk, extra disks, CD-ROM. Up to four controllers. |
| **`vnet`** | virtio-net network adapter driver, configured through `scoadmin` like any other NIC. |
| **`scoqga`** | QEMU guest agent. Lets Proxmox shut the guest down cleanly, and keeps its clock right. See [GUEST-AGENT.md](GUEST-AGENT.md). |

Both drivers work on 5.0.5, 5.0.6 and 5.0.7 from the same binary. The guest
agent has been tested on 5.0.7 only.

## Downloads

Everything is in [`download/`](download/):

| file | what it is |
|---|---|
| [`osr507-virtio-btld-1.0.0.iso`](download/osr507-virtio-btld-1.0.0.iso) (3.2 MB) | boot CD for a **5.0.7** install. Use with your own OpenServer CD. |
| [`osr506-virtio-btld-1.0.0.iso`](download/osr506-virtio-btld-1.0.0.iso) (3.2 MB) | the same for **5.0.6** |
| [`osr505-virtio-btld-1.0.0.iso`](download/osr505-virtio-btld-1.0.0.iso) (3.2 MB) | the same for **5.0.5** |
| [`vscsi-1.0.0.pkg`](download/vscsi-1.0.0.pkg) (72 KB) | disk driver for a system that is already installed |
| [`vnet-1.0.1.pkg`](download/vnet-1.0.1.pkg) (80 KB) | network driver. **1.0.1** stops a kernel panic that a Sonos player (or anything else sending LLC XID broadcasts) on the same LAN causes after some hours — see [NETWORK.md](NETWORK.md#llc-frames-and-a-bug-in-openservers-network-stack). |
| [`vnet-nic.iso`](download/vnet-nic.iso) (444 KB) | the same network driver on a CD, for getting it onto a machine with no network yet |

The `.img` files are the same boot images without the CD wrapper, for writing to
real media. Checksums are in
[`download/SHA256SUMS.txt`](download/SHA256SUMS.txt).

Versions are per file, not per release: `vnet` and the one-disc installers
that bundle it are **1.0.1**, everything else is still **1.0.0**, because only
the network driver changed. The boot-only ISOs and the disk driver are the
same bytes they were.

There are also one-disc installers carrying the boot image and the OpenServer
product media together, so you need nothing else. They are too large for GitHub
and live on downloads.scoexpert.com:

- [`osr507-virtio-install-1.0.1.iso`](https://downloads.scoexpert.com/Proxmox/osr507-virtio-install-1.0.1.iso) (300 MB)
- [`osr506-virtio-install-1.0.1.iso`](https://downloads.scoexpert.com/Proxmox/osr506-virtio-install-1.0.1.iso) (381 MB)
- [`osr505-virtio-install-1.0.1.iso`](https://downloads.scoexpert.com/Proxmox/osr505-virtio-install-1.0.1.iso) (396 MB).
  This one also carries the operating system updates, curl, gzip and OpenSSH:
  see [OpenServer 5.0.5](#openserver-505).

Those are hosted with Xinuos's permission. The drivers are MIT and separate from
that.

## Quick start

Create the VM. Put the CD-ROM on `scsi0` and the disk on `scsi1`. That detail is
what removes the need for any IDE hardware, and it is explained in
[INSTALL.md](INSTALL.md#why-the-cd-rom-goes-on-scsi0).

```sh
qm create 507 --name sco \
  --memory 1024 --cores 1 --sockets 1 \
  --machine pc-i440fx-9.0 --bios seabios --cpu pentium3 \
  --ostype other --vga std \
  --scsihw virtio-scsi-pci \
  --net0 virtio,bridge=vmbr0

qm set 507 --scsi0 local:iso/osr507-virtio-install-1.0.1.iso,media=cdrom
qm set 507 --scsi1 local-lvm:8,cache=writeback
qm set 507 --boot order=scsi0
```

Start it, and at the `Boot :` prompt type:

```
defbootstr link=vscsi btld=fd(64) Sdsk=vscsi(0,0,1) Srom=vscsi(0,0,0) clock.disable_short_timers=1
```

![The SCO boot prompt](screenshots/01-installer-btld-prompt.png)

On 5.0.5 leave `clock.disable_short_timers=1` off the end. That release does not
have the setting.

The driver loads and both devices appear. There is no IDE controller anywhere:

![Disk and CD-ROM on virtio-scsi](screenshots/22-noargs-cd-lun0-disk-lun1.png)

Then follow [INSTALL.md](INSTALL.md). It is a normal OpenServer install with
three things to know about, and the guide flags each one where you meet it, and
where 5.0.5 or 5.0.6 differs.

When it is finished, set `qm set 507 --boot order=scsi1` and it boots from disk
on its own with no boot string:

![Booted from a virtio disk](screenshots/23-noargs-native-boot.png)

## Already have OpenServer running?

You do not need to reinstall. Install the packages, relink, reboot:

```sh
mount -r -f HS,lower /dev/cd0 /mnt
pkgadd -d /mnt/drivers/vscsi-1.0.0.pkg all
pkgadd -d /mnt/drivers/vnet-1.0.1.pkg all
/etc/conf/cf.d/link_unix -y
reboot
```

If your disc is the earlier `-1.0.0` one, its `/drivers` holds `vnet-1.0.0.pkg`;
install [`vnet-1.0.1.pkg`](download/vnet-1.0.1.pkg) from `download/` (or
[`vnet-nic.iso`](download/vnet-nic.iso)) instead, or over the top of it —
`pkgrm vnet` then `pkgadd`, no relink needed until you next add the adapter.

The disk driver does not relink for you. It is the root disk driver, so you pick
the moment. `link_unix` backs the running kernel up to `/stand/unix.old` first,
so a kernel that will not boot is recoverable by typing `hd(40)unix.old` at the
Boot prompt.

There is more detail in [INSTALL.md](INSTALL.md#upgrading-an-existing-system)
and [NETWORK.md](NETWORK.md).

## OpenServer 5.0.5

5.0.5 installs straight onto a virtio disk from its own boot CD, the same way
5.0.6 and 5.0.7 do, and then boots from it with no boot string:

![The 5.0.5 install kernel with the virtio disk and CD-ROM](screenshots/505-installer-hardware-list.png)

The driver packages are the same files. What differs is small, and
[INSTALL.md](INSTALL.md) notes each point where you meet it:

- The boot string has no `clock.disable_short_timers=1`.
- The installer asks for the driver disk at the start, and picks the right
  adapter for the CD-ROM by itself.
- A 5.0.5 licence entered with today's date shows as missing. That has nothing
  to do with the drivers, and it has
  [a one-line fix on Proxmox](INSTALL.md#505-if-it-says-no-user-licences-were-found).

The 5.0.5 one-disc installer carries more than the other two. A 5.0.5 system
needs several of SCO's updates before it can run current software, so the disc
has them on it, in install order, along with curl, gzip and OpenSSH 10.5p1.
[INSTALL.md lists what is there](INSTALL.md#what-else-is-on-the-505-disc), and
[How to Install OpenSSH 10.5 on SCO OpenServer 5.0.5](https://scoexpert.com/guides/openssh-sco-openserver-5-0-5/)
explains each step.

A 5.0.5 system that already exists on emulated IDE does not need reinstalling.
Moving it takes a few more steps than the upgrade above, because an IDE install
leaves the SCSI disk driver switched off.
[INSTALL.md has them in order](INSTALL.md#moving-a-505-system-from-ide-to-virtio).
On the system that was moved, a 71 MB set of files that had been painfully slow
to send by FTP took 6.4 seconds afterwards.

What has been tested on 5.0.5, and what has not:

- A complete install from the one-disc installer onto a virtio disk, followed
  by the five updates, the network driver, curl and OpenSSH, on one virtual
  machine.
- Moving an installed system across from IDE, on another, with RS505A, OSS497C,
  OSS640A, OSS642A and OSS646 applied first.
- Not tested on 5.0.5: the guest agent, the multiprocessor kernel, and the
  network driver on a system with none of the updates applied.

## No IDE at all

Most guides leave the CD-ROM on emulated IDE. That drags SCO's `wd` driver into
your kernel permanently, to serve a device you only needed during the install.
You can avoid it.

Put the CD-ROM on `scsi0` and the disk on `scsi1` and the machine has no IDE
controller. The installer notices, and leaves `wd`, `wdex` and `wdha`
unconfigured on its own. Nothing needs stripping by hand:

```sh
grep '^wd' /etc/conf/cf.d/sdevice
wd	N	2	5	0	0	0	0	0	0
```

`N` means not linked in. On a test install that made the kernel about 240 KB
smaller, and it is one less emulated device on the host.

## Requirements

- Proxmox VE, tested on 9.2.2, or any QEMU/KVM host
- Machine type `pc-i440fx`. q35's PCIe breaks OpenServer.
- SeaBIOS, `virtio-scsi-pci`, and `--cpu pentium3` or similar. Modern CPU models
  advertise features OpenServer chokes on.
- 1 GB RAM is enough for a typical workload. OpenServer tops out near 3 GB:
  assign 4 GB and it reports 3 GB.
- Your own OpenServer 5.0.5, 5.0.6 or 5.0.7 media, unless you use a one-disc
  installer.

If you plan to run the multiprocessor (MPX) kernel, check that
`/sys/module/kvm_intel/parameters/enable_apicv` reads `Y` on the host first.
OpenServer implements `spl()` as a local APIC task priority register write. On a
host without APIC virtualisation every one of those traps and is emulated, and
the guest runs roughly 60 times slower. Anything Haswell-era or newer is fine.

## Building from source

You do not need this to use the drivers. The source is here, and OpenServer can
build it with no compiler licence, because the Link Kit ships a complete
toolchain:

```sh
sh build.sh vscsi     # -> pkg/Driver.o
sh build.sh vnet
```

Each driver bundles its own private copy of the shared virtio transport, so both
can live in one kernel.

## Background

[How these drivers were written, and what went wrong on the way](https://scoexpert.com/guides/sco-openserver-proxmox-virtio-drivers/):
geometry failures, a CD-ROM that identified itself as a hard disk, and an MPX
kernel that was 60 times slower than it should have been.

## Licence

MIT, see [LICENSE](LICENSE). The boot images embed SCO/Xinuos boot code, and the
one-disc installers carry their product media. Those are hosted with Xinuos's
permission and are not covered by the MIT licence.
