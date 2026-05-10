/** @file
  I2cTelemetryDxe - UEFI I2C telemetry driver for temperature and voltage.

  This driver discovers I2C devices, provides raw register read/write APIs,
  converts sensor raw values into normalized units, and publishes telemetry
  to OS loaders via an EFI configuration table and runtime variable.

  Copyright (c) 2026, UEFI Developer.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "I2cTelemetryProtocol.h"

#include <Protocol/DevicePath.h>
#include <Protocol/I2cIo.h>

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiDriverEntryPoint.h>
#include <Library/UefiLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>

#define I2C_TELEMETRY_MAX_I2C_HANDLES  32U

//
// Default example sensor register map:
// - Temperature: TMP102-compatible register layout
// - Voltage: INA219-compatible register layout
//
#define TEMP_REG_VALUE               0x00U
#define VOLTAGE_REG_BUS              0x02U
#define I2C_MAX_RW_SIZE              16U
#define I2C_SLAVE_ADDRESS_INDEX      0U

EFI_GUID  gEfiI2cTelemetryProtocolGuid = EFI_I2C_TELEMETRY_PROTOCOL_GUID;
EFI_GUID  gEfiI2cTelemetryTableGuid    = EFI_I2C_TELEMETRY_TABLE_GUID;
EFI_GUID  gEfiI2cTelemetryVariableGuid = EFI_I2C_TELEMETRY_VARIABLE_GUID;

typedef struct {
  EFI_HANDLE            Handle;
  EFI_I2C_IO_PROTOCOL   *I2cIo;
} I2C_TELEMETRY_CHANNEL;

typedef struct {
  EFI_I2C_TELEMETRY_PROTOCOL   Protocol;
  EFI_HANDLE                   ImageHandle;
  I2C_TELEMETRY_CHANNEL        TempChannel;
  I2C_TELEMETRY_CHANNEL        VoltageChannel;
  EFI_I2C_TELEMETRY_TABLE      Table;
  EFI_I2C_TELEMETRY_SAMPLE     SampleCache;
} I2C_TELEMETRY_DRIVER;

STATIC I2C_TELEMETRY_DRIVER  mDriver;

STATIC
BOOLEAN
ContainsAnyToken (
  IN CONST CHAR16  *Text,
  IN CONST CHAR16  *TokenA,
  IN CONST CHAR16  *TokenB,
  IN CONST CHAR16  *TokenC
  )
{
  if (Text == NULL) {
    return FALSE;
  }

  if ((TokenA != NULL) && (StrStr (Text, TokenA) != NULL)) {
    return TRUE;
  }

  if ((TokenB != NULL) && (StrStr (Text, TokenB) != NULL)) {
    return TRUE;
  }

  if ((TokenC != NULL) && (StrStr (Text, TokenC) != NULL)) {
    return TRUE;
  }

  return FALSE;
}

STATIC
VOID
DiscoverI2cTelemetryChannels (
  VOID
  )
{
  EFI_STATUS                 Status;
  EFI_HANDLE                 *Handles;
  UINTN                      HandleCount;
  UINTN                      Index;
  EFI_I2C_IO_PROTOCOL        *I2cIo;
  EFI_DEVICE_PATH_PROTOCOL   *DevicePath;
  CHAR16                     *DevicePathText;

  Handles        = NULL;
  HandleCount    = 0;
  DevicePathText = NULL;

  ZeroMem (&mDriver.TempChannel, sizeof (mDriver.TempChannel));
  ZeroMem (&mDriver.VoltageChannel, sizeof (mDriver.VoltageChannel));

  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiI2cIoProtocolGuid,
                  NULL,
                  &HandleCount,
                  &Handles
                  );
  if (EFI_ERROR (Status)) {
    return;
  }

  if (HandleCount > I2C_TELEMETRY_MAX_I2C_HANDLES) {
    HandleCount = I2C_TELEMETRY_MAX_I2C_HANDLES;
  }

  for (Index = 0; Index < HandleCount; Index++) {
    Status = gBS->OpenProtocol (
                    Handles[Index],
                    &gEfiI2cIoProtocolGuid,
                    (VOID **)&I2cIo,
                    mDriver.ImageHandle,
                    NULL,
                    EFI_OPEN_PROTOCOL_GET_PROTOCOL
                    );
    if (EFI_ERROR (Status)) {
      continue;
    }

    DevicePath = NULL;
    Status = gBS->OpenProtocol (
                    Handles[Index],
                    &gEfiDevicePathProtocolGuid,
                    (VOID **)&DevicePath,
                    mDriver.ImageHandle,
                    NULL,
                    EFI_OPEN_PROTOCOL_GET_PROTOCOL
                    );
    if (!EFI_ERROR (Status) && (DevicePath != NULL)) {
      DevicePathText = ConvertDevicePathToText (DevicePath, FALSE, FALSE);
    } else {
      DevicePathText = NULL;
    }

    if ((mDriver.TempChannel.I2cIo == NULL) &&
        ContainsAnyToken (DevicePathText, L"Temp", L"TMP", L"Thermal"))
    {
      mDriver.TempChannel.Handle = Handles[Index];
      mDriver.TempChannel.I2cIo  = I2cIo;
    }

    if ((mDriver.VoltageChannel.I2cIo == NULL) &&
        ContainsAnyToken (DevicePathText, L"Volt", L"INA", L"PMIC"))
    {
      mDriver.VoltageChannel.Handle = Handles[Index];
      mDriver.VoltageChannel.I2cIo  = I2cIo;
    }

    if (DevicePathText != NULL) {
      FreePool (DevicePathText);
      DevicePathText = NULL;
    }
  }

  //
  // Fallback assignment when path tokens are unavailable.
  //
  if ((mDriver.TempChannel.I2cIo == NULL) && (HandleCount > 0)) {
    Status = gBS->OpenProtocol (
                    Handles[0],
                    &gEfiI2cIoProtocolGuid,
                    (VOID **)&mDriver.TempChannel.I2cIo,
                    mDriver.ImageHandle,
                    NULL,
                    EFI_OPEN_PROTOCOL_GET_PROTOCOL
                    );
    if (!EFI_ERROR (Status)) {
      mDriver.TempChannel.Handle = Handles[0];
    }
  }

  if ((mDriver.VoltageChannel.I2cIo == NULL) && (HandleCount > 1)) {
    Status = gBS->OpenProtocol (
                    Handles[1],
                    &gEfiI2cIoProtocolGuid,
                    (VOID **)&mDriver.VoltageChannel.I2cIo,
                    mDriver.ImageHandle,
                    NULL,
                    EFI_OPEN_PROTOCOL_GET_PROTOCOL
                    );
    if (!EFI_ERROR (Status)) {
      mDriver.VoltageChannel.Handle = Handles[1];
    }
  }

  if ((mDriver.VoltageChannel.I2cIo == NULL) && (mDriver.TempChannel.I2cIo != NULL)) {
    //
    // Some boards host both values on one telemetry IC.
    //
    mDriver.VoltageChannel = mDriver.TempChannel;
  }

  FreePool (Handles);
}

STATIC
EFI_STATUS
I2cReadRegisterRaw (
  IN  EFI_I2C_IO_PROTOCOL  *I2cIo,
  IN  UINT8                Register,
  IN OUT UINTN             *BufferSize,
  OUT VOID                 *Buffer
  )
{
  EFI_STATUS               Status;
  UINT8                    RegisterAddress[1];
  UINTN                    RequestSize;
  EFI_I2C_REQUEST_PACKET   *Request;
  EFI_STATUS               I2cStatus;

  if ((I2cIo == NULL) || (BufferSize == NULL) || (Buffer == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  if ((*BufferSize == 0) || (*BufferSize > I2C_MAX_RW_SIZE)) {
    return EFI_BAD_BUFFER_SIZE;
  }

  RequestSize = sizeof (EFI_I2C_REQUEST_PACKET) + sizeof (EFI_I2C_OPERATION);
  Request = AllocateZeroPool (RequestSize);
  if (Request == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  RegisterAddress[0]                     = Register;
  Request->OperationCount                = 2;
  Request->Operation[0].Flags            = 0;
  Request->Operation[0].LengthInBytes    = sizeof (RegisterAddress);
  Request->Operation[0].Buffer           = RegisterAddress;
  Request->Operation[1].Flags            = I2C_FLAG_READ;
  Request->Operation[1].LengthInBytes    = *BufferSize;
  Request->Operation[1].Buffer           = Buffer;

  Status = I2cIo->QueueRequest (
                    I2cIo,
                    I2C_SLAVE_ADDRESS_INDEX,
                    NULL,
                    Request,
                    &I2cStatus
                    );

  FreePool (Request);

  if (EFI_ERROR (Status)) {
    return Status;
  }

  return I2cStatus;
}

STATIC
EFI_STATUS
I2cWriteRegisterRaw (
  IN EFI_I2C_IO_PROTOCOL  *I2cIo,
  IN UINT8                Register,
  IN UINTN                BufferSize,
  IN CONST VOID           *Buffer
  )
{
  EFI_STATUS               Status;
  UINTN                    RequestSize;
  EFI_I2C_REQUEST_PACKET   *Request;
  UINT8                    *WriteBuffer;
  EFI_STATUS               I2cStatus;

  if ((I2cIo == NULL) || (Buffer == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  if ((BufferSize == 0) || (BufferSize > I2C_MAX_RW_SIZE)) {
    return EFI_BAD_BUFFER_SIZE;
  }

  RequestSize = sizeof (EFI_I2C_REQUEST_PACKET);
  Request = AllocateZeroPool (RequestSize);
  if (Request == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  WriteBuffer = AllocatePool (BufferSize + 1);
  if (WriteBuffer == NULL) {
    FreePool (Request);
    return EFI_OUT_OF_RESOURCES;
  }

  WriteBuffer[0] = Register;
  CopyMem (&WriteBuffer[1], Buffer, BufferSize);

  Request->OperationCount             = 1;
  Request->Operation[0].Flags         = 0;
  Request->Operation[0].LengthInBytes = BufferSize + 1;
  Request->Operation[0].Buffer        = WriteBuffer;

  Status = I2cIo->QueueRequest (
                    I2cIo,
                    I2C_SLAVE_ADDRESS_INDEX,
                    NULL,
                    Request,
                    &I2cStatus
                    );

  FreePool (WriteBuffer);
  FreePool (Request);

  if (EFI_ERROR (Status)) {
    return Status;
  }

  return I2cStatus;
}

STATIC
EFI_STATUS
GetSensorChannel (
  IN  I2C_TELEMETRY_SENSOR_TYPE  SensorType,
  OUT I2C_TELEMETRY_CHANNEL      **Channel
  )
{
  if (Channel == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  if (SensorType == I2cTelemetrySensorTemperature) {
    if (mDriver.TempChannel.I2cIo == NULL) {
      return EFI_NOT_FOUND;
    }

    *Channel = &mDriver.TempChannel;
    return EFI_SUCCESS;
  }

  if (SensorType == I2cTelemetrySensorVoltage) {
    if (mDriver.VoltageChannel.I2cIo == NULL) {
      return EFI_NOT_FOUND;
    }

    *Channel = &mDriver.VoltageChannel;
    return EFI_SUCCESS;
  }

  return EFI_INVALID_PARAMETER;
}

STATIC
EFI_STATUS
ReadTemperatureMilliC (
  OUT INT32  *TemperatureMilliC
  )
{
  EFI_STATUS             Status;
  I2C_TELEMETRY_CHANNEL  *Channel;
  UINTN                  Size;
  UINT8                  Raw[2];
  INT16                  TempRaw;
  INT32                  TempMilli;

  if (TemperatureMilliC == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  Status = GetSensorChannel (I2cTelemetrySensorTemperature, &Channel);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Size = sizeof (Raw);
  Status = I2cReadRegisterRaw (Channel->I2cIo, TEMP_REG_VALUE, &Size, Raw);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  //
  // TMP102 style: signed 12-bit value in bits [15:4], 0.0625 C / LSB.
  //
  TempRaw = (INT16)((Raw[0] << 8) | Raw[1]);
  TempRaw = (INT16)(TempRaw >> 4);
  if ((TempRaw & BIT11) != 0) {
    TempRaw = (INT16)(TempRaw | 0xF000);
  }

  TempMilli = (INT32)(((INT64)TempRaw * 625) / 10);
  *TemperatureMilliC = TempMilli;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
ReadVoltageMicroV (
  OUT UINT32  *VoltageMicroVolts
  )
{
  EFI_STATUS             Status;
  I2C_TELEMETRY_CHANNEL  *Channel;
  UINTN                  Size;
  UINT8                  Raw[2];
  UINT16                 RawReg;
  UINT32                 Voltage;

  if (VoltageMicroVolts == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  Status = GetSensorChannel (I2cTelemetrySensorVoltage, &Channel);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Size = sizeof (Raw);
  Status = I2cReadRegisterRaw (Channel->I2cIo, VOLTAGE_REG_BUS, &Size, Raw);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  //
  // INA219 style: bus voltage in bits [15:3], 4mV / LSB.
  //
  RawReg  = (UINT16)((Raw[0] << 8) | Raw[1]);
  Voltage = (UINT32)((RawReg >> 3) * 4000U);
  *VoltageMicroVolts = Voltage;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
PublishTelemetryToOs (
  VOID
  )
{
  EFI_STATUS  Status;
  UINT32      Attributes;

  mDriver.Table.Signature = I2C_TELEMETRY_TABLE_SIGNATURE;
  mDriver.Table.Revision  = I2C_TELEMETRY_TABLE_REVISION;
  CopyMem (&mDriver.Table.Sample, &mDriver.SampleCache, sizeof (mDriver.SampleCache));

  Status = gBS->InstallConfigurationTable (&gEfiI2cTelemetryTableGuid, &mDriver.Table);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Attributes = EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS;
  Status = gRT->SetVariable (
                  L"I2cTelemetrySample",
                  &gEfiI2cTelemetryVariableGuid,
                  Attributes,
                  sizeof (mDriver.SampleCache),
                  &mDriver.SampleCache
                  );
  return Status;
}

STATIC
EFI_STATUS
EFIAPI
I2cTelemetryGetLatestSample (
  IN  EFI_I2C_TELEMETRY_PROTOCOL  *This,
  OUT EFI_I2C_TELEMETRY_SAMPLE    *Sample
  )
{
  if ((This == NULL) || (Sample == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  CopyMem (Sample, &mDriver.SampleCache, sizeof (*Sample));
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
I2cTelemetryRefreshSample (
  IN EFI_I2C_TELEMETRY_PROTOCOL  *This
  )
{
  EFI_STATUS  Status;
  EFI_STATUS  TempStatus;
  EFI_STATUS  VoltStatus;
  INT32       TemperatureMilliC;
  UINT32      VoltageMicroV;

  if (This == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  mDriver.SampleCache.ValidFields = 0;
  mDriver.SampleCache.Revision    = EFI_I2C_TELEMETRY_PROTOCOL_REVISION;
  ZeroMem (&mDriver.SampleCache.SampleTime, sizeof (mDriver.SampleCache.SampleTime));
  Status = gRT->GetTime (&mDriver.SampleCache.SampleTime, NULL);
  if (EFI_ERROR (Status)) {
    ZeroMem (&mDriver.SampleCache.SampleTime, sizeof (mDriver.SampleCache.SampleTime));
  }

  TempStatus = ReadTemperatureMilliC (&TemperatureMilliC);
  if (!EFI_ERROR (TempStatus)) {
    mDriver.SampleCache.TemperatureMilliCelsius = TemperatureMilliC;
    mDriver.SampleCache.ValidFields |= I2C_TELEMETRY_VALID_TEMPERATURE;
  }

  VoltStatus = ReadVoltageMicroV (&VoltageMicroV);
  if (!EFI_ERROR (VoltStatus)) {
    mDriver.SampleCache.VoltageMicroVolts = VoltageMicroV;
    mDriver.SampleCache.ValidFields |= I2C_TELEMETRY_VALID_VOLTAGE;
  }

  if (mDriver.SampleCache.ValidFields == 0) {
    if (EFI_ERROR (TempStatus)) {
      return TempStatus;
    }

    return VoltStatus;
  }

  return PublishTelemetryToOs ();
}

STATIC
EFI_STATUS
EFIAPI
I2cTelemetryReadRegister (
  IN EFI_I2C_TELEMETRY_PROTOCOL  *This,
  IN I2C_TELEMETRY_SENSOR_TYPE   SensorType,
  IN UINT8                       Register,
  IN OUT UINTN                   *BufferSize,
  OUT VOID                       *Buffer
  )
{
  EFI_STATUS             Status;
  I2C_TELEMETRY_CHANNEL  *Channel;

  if ((This == NULL) || (BufferSize == NULL) || (Buffer == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  Status = GetSensorChannel (SensorType, &Channel);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  return I2cReadRegisterRaw (Channel->I2cIo, Register, BufferSize, Buffer);
}

STATIC
EFI_STATUS
EFIAPI
I2cTelemetryWriteRegister (
  IN EFI_I2C_TELEMETRY_PROTOCOL  *This,
  IN I2C_TELEMETRY_SENSOR_TYPE   SensorType,
  IN UINT8                       Register,
  IN UINTN                       BufferSize,
  IN CONST VOID                  *Buffer
  )
{
  EFI_STATUS             Status;
  I2C_TELEMETRY_CHANNEL  *Channel;

  if ((This == NULL) || (Buffer == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  Status = GetSensorChannel (SensorType, &Channel);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  return I2cWriteRegisterRaw (Channel->I2cIo, Register, BufferSize, Buffer);
}

EFI_STATUS
EFIAPI
I2cTelemetryDxeEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;

  (VOID)SystemTable;

  ZeroMem (&mDriver, sizeof (mDriver));
  mDriver.ImageHandle                   = ImageHandle;
  mDriver.Protocol.Revision             = EFI_I2C_TELEMETRY_PROTOCOL_REVISION;
  mDriver.Protocol.GetLatestSample      = I2cTelemetryGetLatestSample;
  mDriver.Protocol.RefreshSample        = I2cTelemetryRefreshSample;
  mDriver.Protocol.ReadRegister         = I2cTelemetryReadRegister;
  mDriver.Protocol.WriteRegister        = I2cTelemetryWriteRegister;

  DiscoverI2cTelemetryChannels ();

  Status = gBS->InstallProtocolInterface (
                  &ImageHandle,
                  &gEfiI2cTelemetryProtocolGuid,
                  EFI_NATIVE_INTERFACE,
                  &mDriver.Protocol
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = I2cTelemetryRefreshSample (&mDriver.Protocol);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "I2cTelemetryDxe: initial sample refresh failed: %r\n", Status));
  }

  DEBUG ((DEBUG_INFO, "I2cTelemetryDxe: protocol installed\n"));
  return EFI_SUCCESS;
}
