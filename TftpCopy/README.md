# TftpCopy – UEFI TFTP File Transfer Utility

`TftpCopy` is a production-grade UEFI Shell application that downloads a
file from a TFTP server and writes it directly to a physical memory address.
It is designed for embedded and firmware-development workflows where a
kernel, device tree blob, or ramdisk must be placed at a precise address
before execution (e.g. via a `bootefi` or `chainload` command).

---

## Features

| Feature | Details |
|---------|---------|
| Direct memory write | File data is written straight to the caller-specified address—no intermediate allocation needed. |
| RFC 2348 `blksize` | Negotiates a 1468-byte block size (MTU 1500 − 20 IP − 8 UDP − 4 TFTP) to saturate standard Ethernet links. |
| RFC 2349 `tsize` | Requests the file size from the server; renders a percentage progress bar when available. |
| Spinner fallback | Displays a spinning cursor + byte counter if `tsize` is unsupported by the server. |
| Buffer-overflow guard | Aborts the transfer and prints a diagnostic if the file would exceed `MaxSize`. |
| Configurable retries | 6 retransmissions with a 3-second per-packet timeout (compile-time constants). |
| Dual-stack NIC scan | Iterates all handles that expose `EFI_MTFTP4_SERVICE_BINDING_PROTOCOL` and uses the first available one. |

---

## Prerequisites

* **EDK2** (any recent release; tested against edk2-stable202311 and later).
* The following packages must be present in your workspace:
  * `MdePkg`
  * `MdeModulePkg`
  * `ShellPkg`
* A network interface whose MTFTP4 stack has been initialised (i.e. the
  platform's Network Stack DXE drivers are running and an IP address has
  been assigned via DHCP or configured statically).

---

## Building

### 1 – Add TftpCopy to a platform DSC

Open your platform's `.dsc` file and add the module under
`[Components]`:

```ini
[Components]
  path/to/TftpCopy/TftpCopy.inf
```

Ensure the library class mappings below are present (they are usually
already present in any platform that includes `ShellPkg`):

```ini
[LibraryClasses]
  ShellCEntryLib|ShellPkg/Library/UefiShellCEntryLib/UefiShellCEntryLib.inf
  ShellLib|ShellPkg/Library/UefiShellLib/UefiShellLib.inf
  UefiLib|MdePkg/Library/UefiLib/UefiLib.inf
  BaseLib|MdePkg/Library/BaseLib/BaseLib.inf
  BaseMemoryLib|MdePkg/Library/BaseMemoryLib/BaseMemoryLib.inf
  MemoryAllocationLib|MdePkg/Library/UefiMemoryAllocationLib/UefiMemoryAllocationLib.inf
  UefiBootServicesTableLib|MdePkg/Library/UefiBootServicesTableLib/UefiBootServicesTableLib.inf
  DebugLib|MdePkg/Library/BaseDebugLibNull/BaseDebugLibNull.inf
  PrintLib|MdePkg/Library/BasePrintLib/BasePrintLib.inf
```

### 2 – Build

```bash
build -p Platform/YourPlatform.dsc          \
      -a X64                                 \
      -t GCC5                                \
      -m path/to/TftpCopy/TftpCopy.inf
```

The resulting `TftpCopy.efi` binary is placed under
`Build/<Platform>/<TARGET>_<TOOLCHAIN>/X64/`.

---

## Installation

Copy `TftpCopy.efi` to your UEFI Shell environment:

```
# via a USB drive or PXE-served filesystem:
Shell> cp FS0:\TftpCopy.efi FS0:\EFI\Tools\TftpCopy.efi
```

Or place it anywhere on a filesystem visible to the shell and invoke it
by full path.

---

## Usage

```
TftpCopy <RemoteFile> <DestAddress> [MaxSize]
```

### Arguments

| Argument | Required | Description |
|----------|----------|-------------|
| `RemoteFile` | Yes | Path of the file on the TFTP server. |
| `DestAddress` | Yes | Destination physical memory address in hexadecimal (e.g. `0x80000000`). Must be non-zero. |
| `MaxSize` | No | Maximum bytes to receive, hex or decimal. Default: **256 MiB**. The destination memory region must be at least this size and must be mapped writable. |

### Environment Variables

| Variable | Required | Description |
|----------|----------|-------------|
| `serverip` | **Yes** | IPv4 address of the TFTP server, dotted-decimal (e.g. `192.168.1.10`). |

### Exit Codes

| Code | Meaning |
|------|---------|
| `0` | Transfer succeeded. |
| `1` | Bad arguments or invalid / missing `serverip` variable. |
| `2` | Network initialisation or TFTP transfer error. |

---

## Examples

```shell
# Set the server address, then load a Linux kernel to 0x80000000
Shell> set serverip 192.168.1.10
Shell> TftpCopy Image 0x80000000

# Load a compressed ramdisk with an explicit 128 MiB ceiling
Shell> TftpCopy initramfs.cpio.gz 0x90000000 0x8000000

# Load a device tree blob
Shell> TftpCopy board.dtb 0xBF000000 0x100000
```

### Sample session output

```
TftpCopy v1.0

  Server IP   : 192.168.1.10 (192.168.1.10)
  Remote file : Image
  Destination : 0x0000000080000000
  Max size    : 0x10000000 (268435456 B)

  Downloading 'Image' from 192.168.1.10 ...
  [##################################################]  100%  26214400 / 26214400 bytes

  SUCCESS: 26214400 bytes loaded to 0x0000000080000000 - 0x00000000818FFFFF
```

---

## Notes and Caveats

### Memory region requirements

`TftpCopy` writes file data **directly** to the address you provide.
Before calling the utility:

1. Confirm the target region is within the UEFI memory map and marked
   writable (e.g. `EfiConventionalMemory` or `EfiLoaderData`).
2. Ensure the region is large enough for `MaxSize` bytes.  The utility
   aborts safely if the file would overflow, but the first few packets
   may already have been written.
3. On platforms with UEFI memory protection (e.g. memory attributes table
   enforced via `EFI_MEMORY_XP`/`EFI_MEMORY_RO`) you may need to call
   `gDS->SetMemorySpaceAttributes()` first to mark the range writable.

### TFTP option support

`TftpCopy` requests the `blksize` and `tsize` options (RFC 2347–2349).
Most modern TFTP servers (tftpd-hpa, atftpd, dnsmasq) support these.
If your server does not:

* It may silently ignore the options and respond with the default
  512-byte block size—the transfer will still succeed, just slower.
* A strict server may respond with TFTP error code 8 ("illegal options"),
  causing the transfer to fail.  In that case, rebuild with
  `Token.OptionCount = 0` (remove the `Options` setup in `DownloadFile`)
  to use vanilla TFTP.

### IPv4 only

This utility uses `EFI_MTFTP4_PROTOCOL` (IPv4).  For IPv6 networks,
adapt the code to use `EFI_MTFTP6_PROTOCOL` instead.

---

## Source File Overview

| File | Purpose |
|------|---------|
| `TftpCopy.c` | Full implementation: argument parsing, IP parsing, MTFTP4 setup, transfer loop, progress display. |
| `TftpCopy.h` | Constants, the `TFTP_COPY_CONTEXT` structure, and all function prototypes. |
| `TftpCopy.inf` | EDK2 module descriptor: sources, packages, library classes, and consumed protocols. |

---

## License

BSD-2-Clause-Patent – see `TftpCopy.c` file header.
