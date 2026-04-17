# walkies

Network configuration daemon for [BlueyOS](https://github.com/nzmacgeek/biscuits) — an imaginary Linux-like operating system.

> "Let's go for a walkies!" — Bluey

## Overview

Walkies is the userspace network configuration daemon for BlueyOS. It reads
`/etc/interfaces` and applies the configuration using the `AF_BLUEY_NETCTL`
kernel control plane — bringing interfaces UP, assigning IP addresses, setting
MTU values, and installing default routes.

It integrates with [claw](https://github.com/nzmacgeek/claw) as a oneshot
service in the `claw-network.target` boot stage, and ships as a
[dimsim](https://github.com/nzmacgeek/dimsim) `.dpk` package.

## Building

```bash
# Generate ./configure from configure.ac
./autogen.sh

# Auto-detect musl/sysroot and dimsim tool paths into config.mk
./configure

# Build both binaries for linux/i386 (default, static musl)
make

# Cross-compile with an existing musl-blueyos sysroot
make MUSL_PREFIX=/opt/blueyos-sysroot

# Or let the Makefile clone and build musl automatically
make musl && make
```

Output: `build/walkies`

`./configure` looks for:

- a BlueyOS image sysroot at `/opt/blueyos-sysroot`
- a usable musl sysroot under `SYSROOT` or `/opt/blueyos-sysroot`
- `dpkbuild` and `dimsim` in `PATH`

It writes the detected values to `config.mk`, which the Makefile includes on
subsequent builds. You can override any detected value when configuring:

```bash
MUSL_PREFIX=/path/to/musl SYSROOT=/mnt/blueyos DPKBUILD=/opt/bin/dpkbuild DIMSIM=/opt/bin/dimsim ./configure
./configure --enable-debug
```

## Usage

```
walkies [-c <config>] [-m]

  -c <config>   Path to interfaces config (default: /etc/interfaces)
  -m            Monitor mode: subscribe to all netctl events and log them
```

### One-shot configuration (boot-time)

```bash
walkies
```

Applies `/etc/interfaces`, then exits. This is how claw invokes it.

### Event monitor daemon

```bash
walkies -m
```

Applies `/etc/interfaces`, then subscribes to `NETCTL_GROUP_ALL` multicast
and logs link-state, address, and route changes indefinitely.

## Configuration — `/etc/interfaces`

```
# Loopback interface
auto lo
iface lo inet loopback

# Static IPv4
auto eth0
iface eth0 inet static
  address 192.168.1.100
  netmask 255.255.255.0
  gateway 192.168.1.1
  mtu     1500

# DHCP (interface is brought UP; separate dhcp client handles lease)
auto eth0
iface eth0 inet dhcp
```

## Packaging (dimsim)

```bash
# Requires dpkbuild and dimsim from https://github.com/nzmacgeek/dimsim
make dpk

# Compatibility alias used by biscuits-baker recipes
make package

# Build the .dpk and install it into a mounted BlueyOS rootfs
make install SYSROOT=/mnt/blueyos

# Optional helper script to stamp manifest version/arch and package
./build-dpk.sh 0.1.0
```

Output: `build/dpk/walkies-0.1.0-i386.dpk` (also copied to repository root)

`make install` uses dimsim's offline mode and runs the equivalent of:

```bash
dimsim --root /mnt/blueyos install ./build/dpk/walkies-0.1.0-i386.dpk
```

The target rootfs must already look like a BlueyOS installation, including
`/etc/claw/` and `/bin/bash`, because dimsim validates the sysroot before it
installs the package.

The package installs:

| Path | Purpose |
|------|---------|
| `/sbin/walkies` | Static binary |
| `/etc/interfaces` | Default network config (loopback only) |
| `/etc/claw/services.d/walkies.yml` | Claw oneshot service — applies config at boot |
| `/etc/claw/services.d/walkies-monitor.yml` | Claw simple service — event monitor daemon |
| `/etc/claw/targets.d/claw-network.target.yml` | Claw `claw-network.target` definition |

## Claw integration

Walkies ships two claw service units and one target:

### `walkies.service` (oneshot)

Runs once at boot after `claw-basic.target`. Applies `/etc/interfaces` and
exits. Completion of this service satisfies `claw-network.target`.

```yaml
name: walkies
type: oneshot
start_cmd: /sbin/walkies
after: claw-basic.target
```

### `walkies-monitor.service` (simple daemon)

Stays running after `claw-network.target` is reached. Subscribes to
`NETCTL_GROUP_ALL` and logs events to claw's stdout capture.

```yaml
name: walkies-monitor
type: simple
start_cmd: /sbin/walkies -m
after: claw-network.target
restart: on-failure
```

### `claw-network.target`

Synchronisation point in the boot sequence. Becomes active once `walkies`
(the oneshot) has completed successfully.

```yaml
name: claw-network.target
requires: walkies
after: claw-basic.target
```

## Boot sequence position

```
claw-early.target
  → claw-devices.target
    → claw-rootfs.target
      → claw-basic.target
        → walkies            (oneshot: applies /etc/interfaces)
          → claw-network.target
            → walkies-monitor  (daemon: monitors events)
            → claw-multiuser.target
```

## Control plane API reference

See [`user/walkies.h`](user/walkies.h) for the full set of netctl constants.

| Constant | Value | Description |
|----------|-------|-------------|
| `AF_BLUEY_NETCTL` | 2 | Socket family |
| `SOCK_NETCTL` | 3 | Socket type |
| `NETCTL_MSG_NETDEV_LIST` | 14 | List all network devices |
| `NETCTL_MSG_NETDEV_SET` | 13 | Set device flags / MTU |
| `NETCTL_MSG_ADDR_NEW` | 20 | Add IP address |
| `NETCTL_MSG_ROUTE_NEW` | 30 | Add route |
| `NETCTL_GROUP_ALL` | 7 | All multicast event groups |

## ⚠️ Disclaimer

> Bluey and all related characters are trademarks of Ludo Studio Pty Ltd,
> licensed by BBC Studios. This is an unofficial fan/research project.
> **VIBE CODED RESEARCH PROJECT — NOT FOR PRODUCTION USE.**
