# Shutdown, reboot and time sync from Proxmox

Out of the box, Proxmox cannot shut an OpenServer guest down. The **Shutdown**
button sends an ACPI power button event, which OpenServer does not understand,
so nothing happens and the VM eventually gets stopped the hard way.

The guest agent fixes that, and once it is installed the **Shutdown button in
the web UI works normally** — Proxmox uses the agent instead of ACPI whenever
one is enabled and answering. From the command line:

```sh
qm shutdown 101             # what the web UI button does
qm agent 101 shutdown       # the agent directly
```

Either way the guest shuts down cleanly and the VM powers off in about fifteen
seconds.

You also get the clock right, and a few things Proxmox can report about the
guest.

| | |
|---|---|
| **Shutdown** | orderly `rc0.d` shutdown, filesystems clean, VM actually powers off |
| **Reboot** | orderly reboot |
| **Time** | the guest clock matches the host, from boot onwards |
| **Info** | hostname, OS version, timezone, IP addresses |

Tested on 5.0.7, including a fresh install that has never had a Maintenance
Pack. Everything ships as one package; there is nothing to compile.

---

## What you need

* OpenServer **5.0.7**.
* About ten minutes, most of it a kernel relink.
* One reboot.

You do **not** need the Development System, a compiler, or a Maintenance Pack.
The agent is written in the Perl that ships with 5.0.7.

You do not need the `vscsi` or `vnet` drivers either — the agent is independent
of them, though most people will want all three.

---

## 1. Configure the VM on the Proxmox host

```sh
qm set 101 --agent 1,freeze-fs=0
qm set 101 --localtime 1
```

Then **stop and start** the VM. A reboot from inside is not enough — QEMU only
adds the new hardware when the VM process is restarted.

```sh
qm stop 101 && qm start 101
```

What these do:

* `--agent 1` adds the guest agent channel. Use the **default** type. Do not set
  `type=isa`.
* `freeze-fs=0` stops Proxmox trying to freeze the filesystems before a backup.
  OpenServer has no such facility, and asking for it would make backups fail.
  Backups still work; they are crash-consistent, exactly as they are today.
* `--localtime 1` presents the virtual clock the way OpenServer expects. Without
  it the guest runs an hour out during British Summer Time, and equivalently
  wrong elsewhere.

---

## 2. Install the package on the guest

Copy [`scoqga-1.0.0.pkg`](download/scoqga-1.0.0.pkg) to the guest and:

```sh
pkgadd -d scoqga-1.0.0.pkg scoqga
```

It prints what it changed. Nothing is live yet — the running kernel is
untouched until you relink.

The package saves a copy of every file it edits alongside the original, named
`*.pre-scoqga`.

---

## 3. Relink and reboot

```sh
/etc/conf/cf.d/link_unix -y
reboot
```

This takes a few minutes. On the way back up you should see, right after
`The system is ready.`:

```
QEMU guest agent 1.0.0 started on /dev/vsio (org.qemu.guest_agent.0)
```

That line is your confirmation: the agent is running and talking to the right
channel.

> If the new kernel does not boot, interrupt the `Boot :` prompt and type
> `hd(40)unix.old` to start the previous one. `link_unix` always keeps it.

---

## 4. Check it works

From the Proxmox host:

```sh
qm agent 101 ping                    # should print nothing and exit 0
qm guest cmd 101 get-osinfo          # should report SCO OpenServer
qm agent 101 shutdown                # the real test
```

The VM should shut down cleanly and stop within about fifteen seconds.

If `ping` fails in the first few seconds after a boot, wait a moment and try
again — the agent takes a little time to settle.

---

## What Proxmox can and cannot do with it

**Works:** the **Shutdown** button in the web UI, `qm shutdown <vmid>`,
`qm agent <vmid> shutdown`, and the guest information on the VM's Summary page.

Proxmox picks the agent automatically: if one is enabled in the VM's config and
responding, `Shutdown` sends `guest-shutdown` to it; otherwise it falls back to
the ACPI power button, which is what fails on OpenServer. So the button works
once this is installed, and goes back to doing nothing if the agent stops
answering.

**Never works, by design:** filesystem freeze for snapshots. OpenServer has no
API for it. This is why `freeze-fs=0` is required.

---

## Removing it

```sh
pkgrm scoqga
```

That stops the agent starting and removes its files. The driver and the
power-management settings stay behind — both are harmless on their own — and the
command prints the steps for undoing those too if you want a completely clean
machine.

---

## What it actually installed

Worth knowing if you maintain the machine:

| | |
|---|---|
| `vsio` | a virtio-serial driver, registered with the Link Kit. This is the channel the agent talks over. |
| `/usr/local/bin/scoqga` | the agent itself |
| `/etc/conf/init.d/scoqga` | starts the agent at boot, and survives future kernel relinks |
| power management | enables the `pwr` and `uapm` drivers and sets run level 0 to remove power |

That last one has a visible side effect: `init 0`, `shutdown -i0` and `haltsys`
now **power the machine off** instead of stopping at a `POWER DOWN` banner.
`reboot` and `init 6` still reboot as normal.

The driver, the agent and the full engineering history live in the
[VirtIO for SCO OpenServer 5](https://github.com/tachytelic/VirtIO-for-SCO-OpenServer-5)
repository — see `docs/GUEST-AGENT.md` there.
