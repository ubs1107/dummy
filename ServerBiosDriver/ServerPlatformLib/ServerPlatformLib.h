/** @file
  ServerPlatformLib.h

  Platform support library for the Server BIOS DXE Driver.

  Provides hardware-abstraction helpers for:
    - SMBus temperature and voltage sensor reads
    - IPMI KCS send / receive
    - UEFI variable persistence of the health snapshot

  Copyright (c) 2024, Example Corporation. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef SERVER_PLATFORM_LIB_H_
#define SERVER_PLATFORM_LIB_H_

#include "../ServerBiosDriver.h"

/* ─────────────────────────────────────────────────────────────────────────
 * SMBus sensor helpers
 * ───────────────────────────────────────────────────────────────────────── */

/**
  Read a 16-bit register from a device on the SMBus.

  Issues an SMBus "Read Word" transaction (command code + 2 data bytes).
  The returned value is in host byte order.

  @param[in]  Smbus      Pointer to the SMBus HC protocol.
  @param[in]  Address    7-bit SMBus slave address (bits [6:0]).
  @param[in]  Register   8-bit register / command code.
  @param[out] Value      Receives the 16-bit register value on success.

  @retval EFI_SUCCESS       Transaction completed without errors.
  @retval EFI_DEVICE_ERROR  NACK or bus arbitration failure.
  @retval EFI_INVALID_PARAMETER  Smbus or Value is NULL.
**/
EFI_STATUS
EFIAPI
SmBusReadWord (
  IN  EFI_SMBUS_HC_PROTOCOL  *Smbus,
  IN  UINT8                   Address,
  IN  UINT8                   Register,
  OUT UINT16                 *Value
  );

/**
  Decode a raw LM75 temperature register value to degrees Celsius.

  The LM75 / LM75A temperature register is a 9-bit two's-complement
  value stored in bits [15:7] of the 16-bit word (big-endian on wire,
  converted to host order by SmBusReadWord).  Resolution is 0.5 °C/LSB.

  @param[in]  RawRegister  16-bit value from SMBUS_REG_TEMPERATURE.

  @return  Temperature in whole degrees Celsius (rounded toward zero).
**/
INT16
EFIAPI
Lm75DecodeTemperature (
  IN UINT16  RawRegister
  );

/**
  Decode a raw INA226 bus-voltage register value to millivolts.

  Bits [15:3] hold the bus voltage; LSB = 1.25 mV.

  @param[in]  RawRegister  16-bit value from SMBUS_REG_BUS_VOLTAGE.

  @return  Voltage in millivolts.
**/
UINT16
EFIAPI
Ina226DecodeBusVoltage (
  IN UINT16  RawRegister
  );

/**
  Collect all platform sensor readings into a SERVER_HEALTH_SNAPSHOT.

  Reads all temperature and voltage sensors in order.  If any individual
  read fails the corresponding entry is set to 0 and SensorsValid is
  set to FALSE; the function still attempts all remaining sensors.

  @param[in]  Smbus     Pointer to the SMBus HC protocol.
  @param[out] Snapshot  Receives the populated health snapshot.

  @retval EFI_SUCCESS       All sensors read successfully.
  @retval EFI_DEVICE_ERROR  One or more sensor reads failed.
  @retval EFI_INVALID_PARAMETER  Smbus or Snapshot is NULL.
**/
EFI_STATUS
EFIAPI
CollectHealthSnapshot (
  IN  EFI_SMBUS_HC_PROTOCOL  *Smbus,
  OUT SERVER_HEALTH_SNAPSHOT *Snapshot
  );

/* ─────────────────────────────────────────────────────────────────────────
 * IPMI KCS helpers
 * ───────────────────────────────────────────────────────────────────────── */

/**
  Wait until the KCS Input Buffer Full (IBF) bit is cleared.

  Polls the KCS status register up to IPMI_KCS_MAX_POLL_COUNT times.

  @retval EFI_SUCCESS   IBF cleared within the timeout.
  @retval EFI_TIMEOUT   BMC did not clear IBF in time.
**/
EFI_STATUS
EFIAPI
IpmiKcsWaitForIbfClear (
  VOID
  );

/**
  Wait until the KCS Output Buffer Full (OBF) bit is set.

  @retval EFI_SUCCESS   OBF asserted within the timeout.
  @retval EFI_TIMEOUT   No data appeared within the timeout.
**/
EFI_STATUS
EFIAPI
IpmiKcsWaitForObfSet (
  VOID
  );

/**
  Send an IPMI request and receive the response over the KCS interface.

  Implements the full KCS write-read state machine described in
  IPMI v2.0 specification §9.14.

  @param[in]  Request   Pointer to the IPMI request to transmit.
  @param[out] Response  Receives the BMC response (completion code + data).

  @retval EFI_SUCCESS            Transaction succeeded; check CompletionCode.
  @retval EFI_TIMEOUT            KCS interface timed out.
  @retval EFI_INVALID_PARAMETER  Request or Response is NULL.
  @retval EFI_DEVICE_ERROR       Unexpected KCS state encountered.
**/
EFI_STATUS
EFIAPI
IpmiKcsSendReceive (
  IN  CONST IPMI_REQUEST  *Request,
  OUT IPMI_RESPONSE       *Response
  );

/* ─────────────────────────────────────────────────────────────────────────
 * UEFI variable helpers
 * ───────────────────────────────────────────────────────────────────────── */

/**
  Persist a SERVER_HEALTH_SNAPSHOT to a non-volatile UEFI variable.

  Stores the snapshot under the variable name SERVER_PLATFORM_HEALTH_VAR_NAME
  with GUID SERVER_PLATFORM_VAR_GUID.  Attributes:
    EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS

  @param[in]  Snapshot  Pointer to the snapshot to save.

  @retval EFI_SUCCESS       Variable written.
  @retval EFI_INVALID_PARAMETER  Snapshot is NULL.
  @retval Other             EFI Runtime Services error.
**/
EFI_STATUS
EFIAPI
PersistHealthSnapshot (
  IN CONST SERVER_HEALTH_SNAPSHOT  *Snapshot
  );

/**
  Retrieve a previously persisted SERVER_HEALTH_SNAPSHOT from a UEFI variable.

  @param[out]  Snapshot  Receives the stored snapshot on success.

  @retval EFI_SUCCESS       Variable read successfully.
  @retval EFI_NOT_FOUND     No snapshot variable exists yet.
  @retval EFI_INVALID_PARAMETER  Snapshot is NULL.
  @retval Other             EFI Runtime Services error.
**/
EFI_STATUS
EFIAPI
RetrieveHealthSnapshot (
  OUT SERVER_HEALTH_SNAPSHOT  *Snapshot
  );

/* ─────────────────────────────────────────────────────────────────────────
 * Thermal / voltage policy
 * ───────────────────────────────────────────────────────────────────────── */

/**
  Evaluate a health snapshot against configured thresholds.

  Logs a warning via DEBUG() for sensors exceeding TEMP_WARNING_THRESHOLD_C
  and triggers ASSERT() for sensors exceeding TEMP_CRITICAL_THRESHOLD_C,
  which will halt the system in DEBUG builds.

  @param[in]  Snapshot  Pointer to the snapshot to evaluate.
**/
VOID
EFIAPI
EvaluateThermalPolicy (
  IN CONST SERVER_HEALTH_SNAPSHOT  *Snapshot
  );

#endif /* SERVER_PLATFORM_LIB_H_ */
