/** @file
  ServerBiosDriver.h

  Public header for the Server BIOS DXE Driver.

  Defines data structures, constants, and function prototypes used across
  the server BIOS driver and its platform support library.

  Copyright (c) 2024, Example Corporation. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef SERVER_BIOS_DRIVER_H_
#define SERVER_BIOS_DRIVER_H_

#include <Uefi.h>
#include <Library/UefiLib.h>
#include <Library/DebugLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/IoLib.h>
#include <Library/TimerLib.h>
#include <Library/PrintLib.h>
#include <Protocol/AcpiTable.h>
#include <Protocol/SmbusHc.h>

/* ─────────────────────────────────────────────────────────────────────────
 * Driver identification
 * ───────────────────────────────────────────────────────────────────────── */

#define SERVER_BIOS_DRIVER_VERSION_MAJOR  1
#define SERVER_BIOS_DRIVER_VERSION_MINOR  0
#define SERVER_BIOS_DRIVER_SIGNATURE      SIGNATURE_32 ('S', 'B', 'I', 'O')

/* ─────────────────────────────────────────────────────────────────────────
 * IPMI KCS (Keyboard Controller Style) register offsets
 *
 * The KCS interface is described in the IPMI v2.0 specification, §9.
 * On x86 server platforms the default I/O port base is 0xCA2.
 * ───────────────────────────────────────────────────────────────────────── */

#define IPMI_KCS_BASE_ADDRESS   0x0CA2u
#define IPMI_KCS_DATA_IN_REG    (IPMI_KCS_BASE_ADDRESS + 0u)  ///< Write: data to BMC
#define IPMI_KCS_DATA_OUT_REG   (IPMI_KCS_BASE_ADDRESS + 0u)  ///< Read:  data from BMC
#define IPMI_KCS_STATUS_REG     (IPMI_KCS_BASE_ADDRESS + 1u)  ///< Status / Command register

/** KCS status register bit definitions (IPMI v2.0 Table 9-3). */
#define IPMI_KCS_STATUS_OBF     BIT0  ///< Output Buffer Full  – data ready for host
#define IPMI_KCS_STATUS_IBF     BIT1  ///< Input Buffer Full   – BMC still processing
#define IPMI_KCS_STATUS_SMS_ATN BIT2  ///< SMS Attention
#define IPMI_KCS_STATUS_S0      BIT6  ///< State bits
#define IPMI_KCS_STATUS_S1      BIT7

/** Maximum iterations to poll KCS busy/ready flags. */
#define IPMI_KCS_MAX_POLL_COUNT  0x4000u

/** Largest IPMI request / response payload in bytes. */
#define IPMI_MAX_PAYLOAD_SIZE  64u

/* ─────────────────────────────────────────────────────────────────────────
 * SMBus sensor addresses
 *
 * These follow a typical server board layout where multiple LM75-compatible
 * temperature sensors and INA226 voltage/current monitors sit on SMBus
 * segment 0.  Adjust to match actual board schematic.
 * ───────────────────────────────────────────────────────────────────────── */

#define SMBUS_CHANNEL_PLATFORM        0u   ///< SMBus segment exposed via BIOS

#define SMBUS_ADDR_TEMP_CPU0          0x48u ///< LM75 – CPU 0 package temperature
#define SMBUS_ADDR_TEMP_CPU1          0x49u ///< LM75 – CPU 1 package temperature
#define SMBUS_ADDR_TEMP_INLET         0x4Au ///< LM75 – Chassis inlet temperature
#define SMBUS_ADDR_TEMP_OUTLET        0x4Bu ///< LM75 – Chassis outlet temperature

#define SMBUS_ADDR_VOLT_VDD_CORE      0x40u ///< INA226 – VDD_CORE (CPU core rail)
#define SMBUS_ADDR_VOLT_DIMM_VDD      0x41u ///< INA226 – DIMM VDD rail

#define SMBUS_REG_TEMPERATURE         0x00u ///< LM75: temperature register
#define SMBUS_REG_SHUNT_VOLTAGE       0x01u ///< INA226: shunt voltage register
#define SMBUS_REG_BUS_VOLTAGE         0x02u ///< INA226: bus voltage register

/** Number of temperature sensors managed by this driver. */
#define SENSOR_TEMP_COUNT  4u
/** Number of voltage rails monitored by this driver. */
#define SENSOR_VOLT_COUNT  2u

/* ─────────────────────────────────────────────────────────────────────────
 * Platform variables
 * ───────────────────────────────────────────────────────────────────────── */

/** UEFI variable that persists the last-known-good health snapshot. */
#define SERVER_PLATFORM_HEALTH_VAR_NAME  L"ServerPlatformHealth"

/** UEFI variable GUID – use a project-specific GUID in production. */
#define SERVER_PLATFORM_VAR_GUID \
  { 0xDEADBEEF, 0x1234, 0x5678, { 0x9A, 0xBC, 0xDE, 0xF0, 0x12, 0x34, 0x56, 0x78 } }

/* ─────────────────────────────────────────────────────────────────────────
 * Temperature thresholds (degrees Celsius, raw LM75 units × 0.5 °C)
 * ───────────────────────────────────────────────────────────────────────── */

#define TEMP_WARNING_THRESHOLD_C   85u  ///< Log a warning above this value
#define TEMP_CRITICAL_THRESHOLD_C  95u  ///< Assert and halt above this value

/* ─────────────────────────────────────────────────────────────────────────
 * ACPI OEM table identifiers
 * ───────────────────────────────────────────────────────────────────────── */

#define ACPI_OEM_ID            "EXAMPL"   ///< 6-character OEM identifier
#define ACPI_OEM_TABLE_ID      "SRVBIOS " ///< 8-character OEM table identifier
#define ACPI_OEM_REVISION      0x00000001u

/* ─────────────────────────────────────────────────────────────────────────
 * Data structures
 * ───────────────────────────────────────────────────────────────────────── */

/** Raw health snapshot read from hardware sensors. */
typedef struct {
  INT16    TemperatureC[SENSOR_TEMP_COUNT]; ///< Celsius, signed 16-bit
  UINT16   VoltageMilliVolts[SENSOR_VOLT_COUNT]; ///< mV, unsigned 16-bit
  BOOLEAN  SensorsValid;                    ///< TRUE when readings are trustworthy
} SERVER_HEALTH_SNAPSHOT;

/** IPMI request message (NetFn + Cmd + optional data). */
typedef struct {
  UINT8   NetFunction;           ///< IPMI Net Function (e.g. 0x06 = App)
  UINT8   Command;               ///< IPMI Command code
  UINT8   Data[IPMI_MAX_PAYLOAD_SIZE];
  UINT8   DataLength;            ///< Bytes populated in Data[]
} IPMI_REQUEST;

/** IPMI response message (completion code + optional data). */
typedef struct {
  UINT8   CompletionCode;        ///< 0x00 = normal; see IPMI spec §5.4
  UINT8   Data[IPMI_MAX_PAYLOAD_SIZE];
  UINT8   DataLength;
} IPMI_RESPONSE;

/** Minimal OEM ACPI table layout published by this driver. */
#pragma pack(1)
typedef struct {
  EFI_ACPI_DESCRIPTION_HEADER   Header;         ///< Standard ACPI header
  UINT32                        DriverVersion;  ///< Encoded as (Major << 16 | Minor)
  UINT8                         SensorCount;    ///< Total sensors managed
  UINT8                         Reserved[3];    ///< Pad to DWORD boundary
} SERVER_BIOS_ACPI_TABLE;
#pragma pack()

/* ─────────────────────────────────────────────────────────────────────────
 * Driver private context
 * ───────────────────────────────────────────────────────────────────────── */

/** Internal context block – one instance per driver load. */
typedef struct {
  UINT32                   Signature;       ///< Must equal SERVER_BIOS_DRIVER_SIGNATURE
  EFI_SMBUS_HC_PROTOCOL   *Smbus;           ///< Cached SMBus protocol pointer
  EFI_ACPI_TABLE_PROTOCOL *AcpiTable;       ///< Cached ACPI table protocol pointer
  UINTN                    AcpiTableKey;    ///< Key returned by InstallAcpiTable
  SERVER_HEALTH_SNAPSHOT   HealthSnapshot;  ///< Last successful sensor reading
} SERVER_BIOS_DRIVER_CONTEXT;

/* ─────────────────────────────────────────────────────────────────────────
 * Function prototypes – driver entry / cleanup
 * ───────────────────────────────────────────────────────────────────────── */

/**
  Driver entry point.

  Locates required protocols, performs initial hardware health poll, installs
  the OEM ACPI table, and persists a health snapshot to a UEFI variable.

  @param[in]  ImageHandle  Handle of the loaded image.
  @param[in]  SystemTable  Pointer to the EFI System Table.

  @retval EFI_SUCCESS            Initialisation completed successfully.
  @retval EFI_NOT_FOUND          A required protocol was unavailable.
  @retval EFI_OUT_OF_RESOURCES   Memory allocation failure.
  @retval EFI_DEVICE_ERROR       Hardware sensor read failure.
**/
EFI_STATUS
EFIAPI
ServerBiosDriverEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  );

#endif /* SERVER_BIOS_DRIVER_H_ */
