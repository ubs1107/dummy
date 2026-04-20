# SpiStorageDxe - UEFI SPI NOR + eMMC Storage Driver

`SpiStorageDxe` is a DXE driver that publishes a single protocol to read/write
storage across two backend types:

- **SPI NOR** (via `EFI_SPI_NOR_FLASH_PROTOCOL`)
- **eMMC** (via `EFI_DISK_IO_PROTOCOL` + `EFI_BLOCK_IO_PROTOCOL`)

It is intended for production firmware that needs one stable API for
bootloader updates, capsule staging, provisioning data, and recovery flows.

---

## What this driver exposes

The driver installs:

- `gEfiSpiStorageProtocolGuid`
- Interface definition in `SpiStorageProtocol.h`

Core APIs:

1. `GetDeviceCount()`
2. `GetDeviceInfo(DeviceIndex, ...)`
3. `Read(DeviceIndex, Offset, BufferSize, Buffer)`
4. `Write(DeviceIndex, Offset, BufferSize, Buffer)`
5. `Erase(DeviceIndex, Offset, EraseSize)` (**SPI NOR only**)

---

## Interface details

### 1) `GetDeviceCount`
Returns how many storage backends were discovered at DXE start.

### 2) `GetDeviceInfo`
Returns `SPI_STORAGE_DEVICE_INFO`:
- backend type (`SpiStorageBackendSpiNor` / `SpiStorageBackendEmmc`)
- controller handle
- capacity (if known)
- block size
- read-only flag
- erase support flag

### 3) `Read`
- SPI NOR path: calls backend `ReadData()`
- eMMC path: calls backend `ReadDisk()`
- validates index, pointers, and range before issuing I/O

### 4) `Write`
- SPI NOR path: calls backend `WriteData()`
- eMMC path: calls backend `WriteDisk()`
- returns `EFI_WRITE_PROTECTED` for read-only media

### 5) `Erase`
- SPI NOR path: calls backend `Erase()`
- eMMC path: returns `EFI_UNSUPPORTED`

---

## Discovery model

At entry:

1. Enumerate all handles with `gEfiSpiNorFlashProtocolGuid`
2. Enumerate all handles with `gEfiDiskIoProtocolGuid`
3. For candidate DiskIo handles, check DevicePath text for `Emmc/eMMC/MMC`
4. Open `EFI_BLOCK_IO_PROTOCOL` to obtain media metadata
5. Skip logical partitions; register full media devices only

All accepted devices are stored in an internal fixed-size table.

---

## Error model

Common return statuses:

- `EFI_INVALID_PARAMETER`: bad pointers/index
- `EFI_BAD_BUFFER_SIZE`: read/write/erase beyond known capacity
- `EFI_WRITE_PROTECTED`: media is read-only
- `EFI_UNSUPPORTED`: operation not implemented for backend type
- `EFI_DEVICE_ERROR`: backend protocol missing/inconsistent

---

## Production notes

- This driver centralizes backend dispatch but still relies on platform DXE
  stack quality (SPI controller driver, eMMC host controller, block stack).
- eMMC detection uses DevicePath text heuristics; for strict products, replace
  this with board-specific identification (controller path match, ACPI _HID,
  or protocol tagging).
- Add policy hooks (allowlist regions, anti-rollback, signed payload checks)
  before exposing write access to untrusted callers.

---

## Integration (DSC/FDF)

Add module:

```ini
[Components]
  path/to/SpiStorageDxe/SpiStorageDxe.inf
```

Ensure dependent platform drivers are already present:
- SPI controller + SPI NOR protocol producer
- eMMC host controller + Block/Disk I/O stack

---

## Consumer example flow

1. Locate `gEfiSpiStorageProtocolGuid`
2. `GetDeviceCount()`
3. Iterate `GetDeviceInfo()`
4. Choose device by type/capacity/policy
5. Read/write via unified offsets
6. For NOR update flows, call `Erase()` then `Write()`
