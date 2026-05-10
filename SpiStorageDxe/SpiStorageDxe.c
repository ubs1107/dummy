/** @file
  SpiStorageDxe - Unified UEFI storage driver for SPI NOR and eMMC.

  This DXE driver discovers SPI NOR and eMMC backends exposed by platform
  firmware, then publishes one protocol that allows production callers to:
    - Enumerate devices
    - Read bytes
    - Write bytes
    - Erase regions (SPI NOR only)

  Copyright (c) 2026, UEFI Developer.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "SpiStorageProtocol.h"

#include <Protocol/BlockIo.h>
#include <Protocol/DevicePath.h>
#include <Protocol/DiskIo.h>
#include <Protocol/SpiNorFlash.h>

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiDriverEntryPoint.h>
#include <Library/UefiLib.h>

#define SPI_STORAGE_MAX_DEVICES  32U

EFI_GUID  gEfiSpiStorageProtocolGuid = EFI_SPI_STORAGE_PROTOCOL_GUID;

typedef struct {
  SPI_STORAGE_BACKEND_TYPE      Type;
  EFI_HANDLE                    Controller;
  EFI_SPI_NOR_FLASH_PROTOCOL    *SpiNor;
  EFI_DISK_IO_PROTOCOL          *DiskIo;
  EFI_BLOCK_IO_PROTOCOL         *BlockIo;
  UINT64                        CapacityBytes;
  UINT32                        BlockSize;
  BOOLEAN                       ReadOnly;
  BOOLEAN                       SupportsErase;
} SPI_STORAGE_DEVICE;

typedef struct {
  EFI_SPI_STORAGE_PROTOCOL      Protocol;
  UINTN                         DeviceCount;
  SPI_STORAGE_DEVICE            Devices[SPI_STORAGE_MAX_DEVICES];
  EFI_HANDLE                    ImageHandle;
} SPI_STORAGE_DRIVER;

STATIC SPI_STORAGE_DRIVER  mDriver;

STATIC
BOOLEAN
IsEmmcDevicePath (
  IN EFI_HANDLE  Controller
  )
{
  EFI_STATUS                Status;
  EFI_DEVICE_PATH_PROTOCOL  *DevicePath;
  CHAR16                    *DevicePathText;
  BOOLEAN                   IsEmmc;

  IsEmmc = FALSE;

  Status = gBS->OpenProtocol (
                  Controller,
                  &gEfiDevicePathProtocolGuid,
                  (VOID **)&DevicePath,
                  mDriver.ImageHandle,
                  NULL,
                  EFI_OPEN_PROTOCOL_GET_PROTOCOL
                  );
  if (EFI_ERROR (Status)) {
    return FALSE;
  }

  DevicePathText = ConvertDevicePathToText (DevicePath, FALSE, FALSE);
  if (DevicePathText == NULL) {
    return FALSE;
  }

  if ((StrStr (DevicePathText, L"Emmc") != NULL) ||
      (StrStr (DevicePathText, L"eMMC") != NULL) ||
      (StrStr (DevicePathText, L"MMC")  != NULL))
  {
    IsEmmc = TRUE;
  }

  FreePool (DevicePathText);
  return IsEmmc;
}

STATIC
EFI_STATUS
GetDevice (
  IN  UINTN               DeviceIndex,
  OUT SPI_STORAGE_DEVICE  **Device
  )
{
  if ((Device == NULL) || (DeviceIndex >= mDriver.DeviceCount)) {
    return EFI_INVALID_PARAMETER;
  }

  *Device = &mDriver.Devices[DeviceIndex];
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
ValidateIoRange (
  IN  UINT64  CapacityBytes,
  IN  UINT64  Offset,
  IN  UINTN   BufferSize
  )
{
  if (BufferSize == 0) {
    return EFI_SUCCESS;
  }

  if ((CapacityBytes > 0) && ((Offset >= CapacityBytes) || ((CapacityBytes - Offset) < BufferSize))) {
    return EFI_BAD_BUFFER_SIZE;
  }

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
SpiStorageGetDeviceCount (
  IN  EFI_SPI_STORAGE_PROTOCOL  *This,
  OUT UINTN                     *DeviceCount
  )
{
  if ((This == NULL) || (DeviceCount == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  *DeviceCount = mDriver.DeviceCount;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
SpiStorageGetDeviceInfo (
  IN  EFI_SPI_STORAGE_PROTOCOL   *This,
  IN  UINTN                      DeviceIndex,
  OUT SPI_STORAGE_DEVICE_INFO    *DeviceInfo
  )
{
  EFI_STATUS          Status;
  SPI_STORAGE_DEVICE  *Device;

  if ((This == NULL) || (DeviceInfo == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  Status = GetDevice (DeviceIndex, &Device);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  ZeroMem (DeviceInfo, sizeof (*DeviceInfo));
  DeviceInfo->Type          = Device->Type;
  DeviceInfo->Controller    = Device->Controller;
  DeviceInfo->CapacityBytes = Device->CapacityBytes;
  DeviceInfo->BlockSize     = Device->BlockSize;
  DeviceInfo->ReadOnly      = Device->ReadOnly;
  DeviceInfo->SupportsErase = Device->SupportsErase;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
SpiStorageRead (
  IN EFI_SPI_STORAGE_PROTOCOL   *This,
  IN UINTN                      DeviceIndex,
  IN UINT64                     Offset,
  IN OUT UINTN                  *BufferSize,
  OUT VOID                      *Buffer
  )
{
  EFI_STATUS          Status;
  SPI_STORAGE_DEVICE  *Device;

  if ((This == NULL) || (BufferSize == NULL) || (Buffer == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  Status = GetDevice (DeviceIndex, &Device);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = ValidateIoRange (Device->CapacityBytes, Offset, *BufferSize);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (*BufferSize == 0) {
    return EFI_SUCCESS;
  }

  if (Device->Type == SpiStorageBackendSpiNor) {
    if (Device->SpiNor == NULL) {
      return EFI_DEVICE_ERROR;
    }

    return Device->SpiNor->ReadData (Device->SpiNor, Offset, *BufferSize, Buffer);
  }

  if (Device->Type == SpiStorageBackendEmmc) {
    if ((Device->DiskIo == NULL) || (Device->BlockIo == NULL) || (Device->BlockIo->Media == NULL)) {
      return EFI_DEVICE_ERROR;
    }

    return Device->DiskIo->ReadDisk (
                             Device->DiskIo,
                             Device->BlockIo->Media->MediaId,
                             Offset,
                             *BufferSize,
                             Buffer
                             );
  }

  return EFI_UNSUPPORTED;
}

STATIC
EFI_STATUS
EFIAPI
SpiStorageWrite (
  IN EFI_SPI_STORAGE_PROTOCOL   *This,
  IN UINTN                      DeviceIndex,
  IN UINT64                     Offset,
  IN OUT UINTN                  *BufferSize,
  IN CONST VOID                 *Buffer
  )
{
  EFI_STATUS          Status;
  SPI_STORAGE_DEVICE  *Device;

  if ((This == NULL) || (BufferSize == NULL) || (Buffer == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  Status = GetDevice (DeviceIndex, &Device);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (Device->ReadOnly) {
    return EFI_WRITE_PROTECTED;
  }

  Status = ValidateIoRange (Device->CapacityBytes, Offset, *BufferSize);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (*BufferSize == 0) {
    return EFI_SUCCESS;
  }

  if (Device->Type == SpiStorageBackendSpiNor) {
    if (Device->SpiNor == NULL) {
      return EFI_DEVICE_ERROR;
    }

    return Device->SpiNor->WriteData (Device->SpiNor, Offset, *BufferSize, (VOID *)Buffer);
  }

  if (Device->Type == SpiStorageBackendEmmc) {
    if ((Device->DiskIo == NULL) || (Device->BlockIo == NULL) || (Device->BlockIo->Media == NULL)) {
      return EFI_DEVICE_ERROR;
    }

    return Device->DiskIo->WriteDisk (
                             Device->DiskIo,
                             Device->BlockIo->Media->MediaId,
                             Offset,
                             *BufferSize,
                             (VOID *)Buffer
                             );
  }

  return EFI_UNSUPPORTED;
}

STATIC
EFI_STATUS
EFIAPI
SpiStorageErase (
  IN EFI_SPI_STORAGE_PROTOCOL   *This,
  IN UINTN                      DeviceIndex,
  IN UINT64                     Offset,
  IN UINTN                      EraseSize
  )
{
  EFI_STATUS          Status;
  SPI_STORAGE_DEVICE  *Device;

  if (This == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  Status = GetDevice (DeviceIndex, &Device);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (EraseSize == 0) {
    return EFI_SUCCESS;
  }

  Status = ValidateIoRange (Device->CapacityBytes, Offset, EraseSize);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (Device->Type == SpiStorageBackendSpiNor) {
    if (Device->SpiNor == NULL) {
      return EFI_DEVICE_ERROR;
    }

    return Device->SpiNor->Erase (Device->SpiNor, Offset, EraseSize);
  }

  return EFI_UNSUPPORTED;
}

STATIC
VOID
RegisterSpiNorBackends (
  VOID
  )
{
  EFI_STATUS                 Status;
  EFI_HANDLE                 *Handles;
  UINTN                      HandleCount;
  UINTN                      Index;
  EFI_SPI_NOR_FLASH_PROTOCOL *SpiNor;
  SPI_STORAGE_DEVICE         *Device;

  Handles     = NULL;
  HandleCount = 0;

  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiSpiNorFlashProtocolGuid,
                  NULL,
                  &HandleCount,
                  &Handles
                  );
  if (EFI_ERROR (Status)) {
    return;
  }

  for (Index = 0; Index < HandleCount; Index++) {
    if (mDriver.DeviceCount >= SPI_STORAGE_MAX_DEVICES) {
      break;
    }

    Status = gBS->OpenProtocol (
                    Handles[Index],
                    &gEfiSpiNorFlashProtocolGuid,
                    (VOID **)&SpiNor,
                    mDriver.ImageHandle,
                    NULL,
                    EFI_OPEN_PROTOCOL_GET_PROTOCOL
                    );
    if (EFI_ERROR (Status)) {
      continue;
    }

    Device = &mDriver.Devices[mDriver.DeviceCount++];
    ZeroMem (Device, sizeof (*Device));
    Device->Type          = SpiStorageBackendSpiNor;
    Device->Controller    = Handles[Index];
    Device->SpiNor        = SpiNor;
    Device->CapacityBytes = 0;
    Device->BlockSize     = 1;
    Device->ReadOnly      = FALSE;
    Device->SupportsErase = TRUE;
  }

  FreePool (Handles);
}

STATIC
VOID
RegisterEmmcBackends (
  VOID
  )
{
  EFI_STATUS           Status;
  EFI_HANDLE           *Handles;
  UINTN                HandleCount;
  UINTN                Index;
  EFI_DISK_IO_PROTOCOL *DiskIo;
  EFI_BLOCK_IO_PROTOCOL *BlockIo;
  SPI_STORAGE_DEVICE   *Device;
  UINT64               CapacityBytes;

  Handles     = NULL;
  HandleCount = 0;

  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiDiskIoProtocolGuid,
                  NULL,
                  &HandleCount,
                  &Handles
                  );
  if (EFI_ERROR (Status)) {
    return;
  }

  for (Index = 0; Index < HandleCount; Index++) {
    if (mDriver.DeviceCount >= SPI_STORAGE_MAX_DEVICES) {
      break;
    }

    if (!IsEmmcDevicePath (Handles[Index])) {
      continue;
    }

    Status = gBS->OpenProtocol (
                    Handles[Index],
                    &gEfiDiskIoProtocolGuid,
                    (VOID **)&DiskIo,
                    mDriver.ImageHandle,
                    NULL,
                    EFI_OPEN_PROTOCOL_GET_PROTOCOL
                    );
    if (EFI_ERROR (Status)) {
      continue;
    }

    Status = gBS->OpenProtocol (
                    Handles[Index],
                    &gEfiBlockIoProtocolGuid,
                    (VOID **)&BlockIo,
                    mDriver.ImageHandle,
                    NULL,
                    EFI_OPEN_PROTOCOL_GET_PROTOCOL
                    );
    if (EFI_ERROR (Status) || (BlockIo == NULL) || (BlockIo->Media == NULL)) {
      continue;
    }

    if (BlockIo->Media->LogicalPartition) {
      continue;
    }

    CapacityBytes = MultU64x32 (BlockIo->Media->LastBlock + 1, BlockIo->Media->BlockSize);

    Device = &mDriver.Devices[mDriver.DeviceCount++];
    ZeroMem (Device, sizeof (*Device));
    Device->Type          = SpiStorageBackendEmmc;
    Device->Controller    = Handles[Index];
    Device->DiskIo        = DiskIo;
    Device->BlockIo       = BlockIo;
    Device->CapacityBytes = CapacityBytes;
    Device->BlockSize     = BlockIo->Media->BlockSize;
    Device->ReadOnly      = BlockIo->Media->ReadOnly;
    Device->SupportsErase = FALSE;
  }

  FreePool (Handles);
}

EFI_STATUS
EFIAPI
SpiStorageDxeEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;

  ZeroMem (&mDriver, sizeof (mDriver));
  mDriver.ImageHandle              = ImageHandle;
  mDriver.Protocol.Revision        = EFI_SPI_STORAGE_PROTOCOL_REVISION;
  mDriver.Protocol.GetDeviceCount  = SpiStorageGetDeviceCount;
  mDriver.Protocol.GetDeviceInfo   = SpiStorageGetDeviceInfo;
  mDriver.Protocol.Read            = SpiStorageRead;
  mDriver.Protocol.Write           = SpiStorageWrite;
  mDriver.Protocol.Erase           = SpiStorageErase;

  RegisterSpiNorBackends ();
  RegisterEmmcBackends ();

  Status = gBS->InstallProtocolInterface (
                  &ImageHandle,
                  &gEfiSpiStorageProtocolGuid,
                  EFI_NATIVE_INTERFACE,
                  &mDriver.Protocol
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  DEBUG ((
    DEBUG_INFO,
    "SpiStorageDxe: installed with %u device(s)\n",
    (UINT32)mDriver.DeviceCount
    ));

  return EFI_SUCCESS;
}
