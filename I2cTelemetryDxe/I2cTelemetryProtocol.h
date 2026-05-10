/** @file
  Unified I2C Telemetry Protocol for temperature and voltage sensors.

  This protocol provides production-facing read/write register access and
  normalized telemetry sampling for firmware modules that consume board
  health data over I2C.

  Copyright (c) 2026, UEFI Developer.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef I2C_TELEMETRY_PROTOCOL_H_
#define I2C_TELEMETRY_PROTOCOL_H_

#include <Uefi.h>

///
/// Sensor channel identifiers.
///
typedef enum {
  I2cTelemetrySensorUnknown     = 0,
  I2cTelemetrySensorTemperature = 1,
  I2cTelemetrySensorVoltage     = 2
} I2C_TELEMETRY_SENSOR_TYPE;

///
/// Field validity flags in EFI_I2C_TELEMETRY_SAMPLE.ValidFields.
///
#define I2C_TELEMETRY_VALID_TEMPERATURE  BIT0
#define I2C_TELEMETRY_VALID_VOLTAGE      BIT1

///
/// Normalized telemetry sample.
///
typedef struct {
  UINT32      Revision;
  UINT32      ValidFields;
  INT32       TemperatureMilliCelsius;
  UINT32      VoltageMicroVolts;
  EFI_TIME    SampleTime;
} EFI_I2C_TELEMETRY_SAMPLE;

///
/// Configuration table payload installed by the driver for OS loaders.
///
typedef struct {
  UINT32                     Signature;
  UINT32                     Revision;
  EFI_I2C_TELEMETRY_SAMPLE   Sample;
} EFI_I2C_TELEMETRY_TABLE;

typedef struct _EFI_I2C_TELEMETRY_PROTOCOL EFI_I2C_TELEMETRY_PROTOCOL;

typedef
EFI_STATUS
(EFIAPI *EFI_I2C_TELEMETRY_GET_LATEST_SAMPLE)(
  IN  EFI_I2C_TELEMETRY_PROTOCOL  *This,
  OUT EFI_I2C_TELEMETRY_SAMPLE    *Sample
  );

typedef
EFI_STATUS
(EFIAPI *EFI_I2C_TELEMETRY_REFRESH_SAMPLE)(
  IN EFI_I2C_TELEMETRY_PROTOCOL  *This
  );

typedef
EFI_STATUS
(EFIAPI *EFI_I2C_TELEMETRY_READ_REGISTER)(
  IN EFI_I2C_TELEMETRY_PROTOCOL  *This,
  IN I2C_TELEMETRY_SENSOR_TYPE   SensorType,
  IN UINT8                       Register,
  IN OUT UINTN                   *BufferSize,
  OUT VOID                       *Buffer
  );

typedef
EFI_STATUS
(EFIAPI *EFI_I2C_TELEMETRY_WRITE_REGISTER)(
  IN EFI_I2C_TELEMETRY_PROTOCOL  *This,
  IN I2C_TELEMETRY_SENSOR_TYPE   SensorType,
  IN UINT8                       Register,
  IN UINTN                       BufferSize,
  IN CONST VOID                  *Buffer
  );

///
/// Protocol contract.
///
struct _EFI_I2C_TELEMETRY_PROTOCOL {
  UINT32                             Revision;
  EFI_I2C_TELEMETRY_GET_LATEST_SAMPLE GetLatestSample;
  EFI_I2C_TELEMETRY_REFRESH_SAMPLE    RefreshSample;
  EFI_I2C_TELEMETRY_READ_REGISTER     ReadRegister;
  EFI_I2C_TELEMETRY_WRITE_REGISTER    WriteRegister;
};

#define EFI_I2C_TELEMETRY_PROTOCOL_REVISION  0x00010000U
#define I2C_TELEMETRY_TABLE_REVISION         0x00010000U
#define I2C_TELEMETRY_TABLE_SIGNATURE         SIGNATURE_32 ('I','2','T','M')

#define EFI_I2C_TELEMETRY_PROTOCOL_GUID \
  { 0x7f3e37ca, 0x3b1f, 0x4f79, { 0xb8, 0x11, 0x0f, 0x8b, 0x2d, 0x9f, 0x7e, 0xb4 } }

#define EFI_I2C_TELEMETRY_TABLE_GUID \
  { 0x8cbf80ab, 0xdc9c, 0x4ae8, { 0xa0, 0x4f, 0x4a, 0x2a, 0x66, 0x3d, 0xce, 0x70 } }

#define EFI_I2C_TELEMETRY_VARIABLE_GUID \
  { 0x686e04b1, 0x4d81, 0x4317, { 0xba, 0x74, 0xd5, 0xe2, 0xc3, 0x7a, 0x4f, 0x95 } }

extern EFI_GUID  gEfiI2cTelemetryProtocolGuid;
extern EFI_GUID  gEfiI2cTelemetryTableGuid;
extern EFI_GUID  gEfiI2cTelemetryVariableGuid;

#endif // I2C_TELEMETRY_PROTOCOL_H_
