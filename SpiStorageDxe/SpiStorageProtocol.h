/** @file
  Unified SPI Storage Protocol for SPI NOR and eMMC backends.

  This protocol provides a production-oriented storage access API for
  firmware components that need direct read/write access to SPI NOR flash
  and eMMC media through one stable interface.

  Copyright (c) 2026, UEFI Developer.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef SPI_STORAGE_PROTOCOL_H_
#define SPI_STORAGE_PROTOCOL_H_

#include <Uefi.h>

///
/// Backend type.
///
typedef enum {
  SpiStorageBackendUnknown = 0,
  SpiStorageBackendSpiNor  = 1,
  SpiStorageBackendEmmc    = 2
} SPI_STORAGE_BACKEND_TYPE;

///
/// Device information returned by GetDeviceInfo().
///
typedef struct {
  SPI_STORAGE_BACKEND_TYPE    Type;
  EFI_HANDLE                  Controller;
  UINT64                      CapacityBytes;
  UINT32                      BlockSize;
  BOOLEAN                     ReadOnly;
  BOOLEAN                     SupportsErase;
} SPI_STORAGE_DEVICE_INFO;

///
/// Forward declaration.
///
typedef struct _EFI_SPI_STORAGE_PROTOCOL EFI_SPI_STORAGE_PROTOCOL;

typedef
EFI_STATUS
(EFIAPI *EFI_SPI_STORAGE_GET_DEVICE_COUNT)(
  IN  EFI_SPI_STORAGE_PROTOCOL  *This,
  OUT UINTN                     *DeviceCount
  );

typedef
EFI_STATUS
(EFIAPI *EFI_SPI_STORAGE_GET_DEVICE_INFO)(
  IN  EFI_SPI_STORAGE_PROTOCOL   *This,
  IN  UINTN                      DeviceIndex,
  OUT SPI_STORAGE_DEVICE_INFO    *DeviceInfo
  );

typedef
EFI_STATUS
(EFIAPI *EFI_SPI_STORAGE_READ)(
  IN EFI_SPI_STORAGE_PROTOCOL   *This,
  IN UINTN                      DeviceIndex,
  IN UINT64                     Offset,
  IN OUT UINTN                  *BufferSize,
  OUT VOID                      *Buffer
  );

typedef
EFI_STATUS
(EFIAPI *EFI_SPI_STORAGE_WRITE)(
  IN EFI_SPI_STORAGE_PROTOCOL   *This,
  IN UINTN                      DeviceIndex,
  IN UINT64                     Offset,
  IN OUT UINTN                  *BufferSize,
  IN CONST VOID                 *Buffer
  );

typedef
EFI_STATUS
(EFIAPI *EFI_SPI_STORAGE_ERASE)(
  IN EFI_SPI_STORAGE_PROTOCOL   *This,
  IN UINTN                      DeviceIndex,
  IN UINT64                     Offset,
  IN UINTN                      EraseSize
  );

///
/// Unified protocol surface for SPI NOR + eMMC.
///
struct _EFI_SPI_STORAGE_PROTOCOL {
  UINT32                               Revision;
  EFI_SPI_STORAGE_GET_DEVICE_COUNT     GetDeviceCount;
  EFI_SPI_STORAGE_GET_DEVICE_INFO      GetDeviceInfo;
  EFI_SPI_STORAGE_READ                 Read;
  EFI_SPI_STORAGE_WRITE                Write;
  EFI_SPI_STORAGE_ERASE                Erase;
};

#define EFI_SPI_STORAGE_PROTOCOL_REVISION  0x00010000U

#define EFI_SPI_STORAGE_PROTOCOL_GUID \
  { 0x98a8f5f4, 0x2df4, 0x4a86, { 0x8b, 0xa9, 0x87, 0x20, 0xfa, 0xde, 0x67, 0x31 } }

extern EFI_GUID  gEfiSpiStorageProtocolGuid;

#endif // SPI_STORAGE_PROTOCOL_H_
