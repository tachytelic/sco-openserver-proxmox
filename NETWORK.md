# Networking

`vnet` is an MDI network driver, so OpenServer's own TCP/IP stack drives a
virtio-net device. `scoadmin` finds it by PCI ID, configures it, and it behaves
like any other adapter afterwards.

## Why it is worth installing

Against the emulated AMD PCnet card OpenServer would otherwise use:

| copying 10 MB over ssh | time | throughput |
|---|---|---|
| **virtio-net** | **1.1 s** | ~9.5 MB/s |
| emulated pcnet | **8 min 54 s** | ~20 KB/s |

That is roughly 480 times faster. The ssh figure is limited by encryption on a
single emulated CPU rather than by the driver. Over plain HTTP the same guest
does 17.5 MB/s, about 140 Mbit/s.

## Set up the VM

Give the VM a virtio NIC. If you are installing OpenServer from scratch, add it
when you create the VM but defer network configuration during the install,
because the installer has no driver for it yet.

```sh
qm set 507 --net0 virtio,bridge=vmbr0
```

## Install the driver

The package is [`vnet-1.0.0.pkg`](download/vnet-1.0.0.pkg), 78 KB. It is also in
`/drivers` on the one-disc installers, which is the easy route for a machine
that has no working network yet:

```sh
mount -r -f HS,lower /dev/cd0 /mnt
pkgadd -d /mnt/drivers/vnet-1.0.0.pkg all
```

## Configure the adapter

```
scoadmin
```

Go to **Network Configuration Manager**, then **Hardware**, then **Add New LAN
Adapter**. It appears as "VirtIO Network Adapter", because SCO's own hardware
scan matches the PCI ID and names it without prompting:

![scoadmin detecting the adapter](screenshots/net-02-scoadmin-detect.png)

Set the address in the same tool. Adding the adapter relinks the kernel itself,
which is the several-minute pause, then reboot when it asks.

That is all there is to it. `ifconfig -a` should show `net0` with your address,
and it is an ordinary SCO network interface from then on.

## Notes

There is no link speed to negotiate. virtio has no PHY and no wire, so the 1 Gb
the driver reports is advisory metadata for tools and routing decisions rather
than a limit. The real ceiling is host CPU and memory bandwidth.

OpenServer 5.0.6 has no `sshd`, because it predates SCO bundling OpenSSH. It has
telnet. The driver itself is identical on both releases.
