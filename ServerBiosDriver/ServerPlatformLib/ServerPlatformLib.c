/** @file
  ServerPlatformLib.c

  Platform support library implementation for the Server BIOS DXE Driver.

  Copyright (c) 2024, Example Corporation. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "ServerPlatformLib.h"

/* ─────────────────────────────────────────────────────────────────────────
 * Internal constants
 * ───────────────────────────────────────────────────────────────────────── */

/** GUID placed in the UEFI variable namespace. */
STATIC EFI_GUID  mServerPlatformVarGuid = SERVER_PLATFORM_VAR_GUID;

/* ─────────────────────────────────────────────────────────────────────────
 * SMBus sensor helpers
 * ───────────────────────────────────────────────────────────────────────── */

EFI_STATUS
EFIAPI
SmBusReadWord (
  IN  EFI_SMBUS_HC_PROTOCOL  *Smbus,
  IN  UINT8                   Address,
  IN  UINT8                   Register,
  OUT UINT16                 *Value
  )
{
  EFI_STATUS          Status;
  EFI_SMBUS_DEVICE_ADDRESS  DevAddr;
  EFI_SMBUS_DEVICE_COMMAND  Cmd;
  UINTN               Length;
  UINT16              Data;

  if ((Smbus == NULL) || (Value == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  DevAddr.SmbusDeviceAddress = Address;
  Cmd    = (EFI_SMBUS_DEVICE_COMMAND)Register;
  Length = sizeof (UINT16);
  Data   = 0;

  Status = Smbus->Execute (
                    Smbus,
                    DevAddr,
                    Cmd,
                    EfiSmbusReadWord,
                    FALSE,
                    &Length,
                    &Data
                    );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR,
            "[ServerBios] SmBusReadWord: addr=0x%02X reg=0x%02X error=%r\n",
            Address, Register, Status));
    return EFI_DEVICE_ERROR;
  }

  /* SMBus word data arrives little-endian; protocol driver should already
   * have byte-swapped it, but reinforce host-byte-order here. */
  *Value = Data;
  return EFI_SUCCESS;
}

/* ─────────────────────────────────────────────────────────────────────────── */

INT16
EFIAPI
Lm75DecodeTemperature (
  IN UINT16  RawRegister
  )
{
  /*
   * LM75 temperature register layout (9-bit, big-endian on wire):
   *
   *   Bit 15 (MSB) ... Bit 7 : 9-bit two's-complement temperature
   *   Bits 6:0               : don't-care / reserved
   *
   * After SmBusReadWord the value is in host byte order (little-endian).
   * Shift right by 7 to align the 9-bit field, then sign-extend.
   * Resolution is 0.5 °C per LSB.
   */
  INT16  Signed9Bit;
  INT16  TempCx2;

  /* Arithmetic shift preserves sign on two's-complement machines. */
  Signed9Bit = (INT16)((INT16)RawRegister >> 7);

  /*
   * The field is 9 bits; mask to avoid artefacts from higher garbage bits
   * after shifting.  Then sign-extend from bit 8.
   */
  Signed9Bit = (INT16)(Signed9Bit & 0x01FF);
  if ((Signed9Bit & 0x0100) != 0) {
    Signed9Bit = (INT16)(Signed9Bit | (INT16)0xFF00);  /* sign-extend */
  }

  TempCx2 = Signed9Bit;          /* 0.5 °C units */
  return (INT16)(TempCx2 / 2);   /* return whole degrees, round toward zero */
}

/* ─────────────────────────────────────────────────────────────────────────── */

UINT16
EFIAPI
Ina226DecodeBusVoltage (
  IN UINT16  RawRegister
  )
{
  /*
   * INA226 Bus Voltage Register (Table 3 in INA226 datasheet):
   *   Bits [15:3] : bus voltage value, LSB = 1.25 mV
   *   Bits [2:0]  : reserved
   *
   * Voltage (mV) = (RawRegister >> 3) * 1.25
   *             = (RawRegister >> 3) * 5 / 4
   */
  UINT32 Shifted;

  Shifted = (UINT32)(RawRegister >> 3);
  /* Multiply by 5 then divide by 4 to avoid floating-point. */
  return (UINT16)((Shifted * 5u) / 4u);
}

/* ─────────────────────────────────────────────────────────────────────────── */

EFI_STATUS
EFIAPI
CollectHealthSnapshot (
  IN  EFI_SMBUS_HC_PROTOCOL  *Smbus,
  OUT SERVER_HEALTH_SNAPSHOT *Snapshot
  )
{
  EFI_STATUS  Status;
  EFI_STATUS  OverallStatus;
  UINT16      Raw;
  UINTN       Idx;

  /** Temperature sensor addresses, ordered to match HealthSnapshot.TemperatureC[]. */
  STATIC CONST UINT8 TempAddresses[SENSOR_TEMP_COUNT] = {
    SMBUS_ADDR_TEMP_CPU0,
    SMBUS_ADDR_TEMP_CPU1,
    SMBUS_ADDR_TEMP_INLET,
    SMBUS_ADDR_TEMP_OUTLET,
  };

  /** Voltage sensor addresses, ordered to match HealthSnapshot.VoltageMilliVolts[]. */
  STATIC CONST UINT8 VoltAddresses[SENSOR_VOLT_COUNT] = {
    SMBUS_ADDR_VOLT_VDD_CORE,
    SMBUS_ADDR_VOLT_DIMM_VDD,
  };

  if ((Smbus == NULL) || (Snapshot == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  ZeroMem (Snapshot, sizeof (*Snapshot));
  Snapshot->SensorsValid = TRUE;
  OverallStatus = EFI_SUCCESS;

  /* Read temperature sensors */
  for (Idx = 0; Idx < SENSOR_TEMP_COUNT; Idx++) {
    Status = SmBusReadWord (Smbus, TempAddresses[Idx], SMBUS_REG_TEMPERATURE, &Raw);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_WARN,
              "[ServerBios] Temperature sensor[%u] addr=0x%02X read failed: %r\n",
              (UINT32)Idx, TempAddresses[Idx], Status));
      Snapshot->SensorsValid = FALSE;
      OverallStatus = EFI_DEVICE_ERROR;
    } else {
      Snapshot->TemperatureC[Idx] = Lm75DecodeTemperature (Raw);
      DEBUG ((DEBUG_VERBOSE,
              "[ServerBios] Temp[%u] = %d °C (raw=0x%04X)\n",
              (UINT32)Idx, Snapshot->TemperatureC[Idx], Raw));
    }
  }

  /* Read voltage sensors (bus voltage register) */
  for (Idx = 0; Idx < SENSOR_VOLT_COUNT; Idx++) {
    Status = SmBusReadWord (Smbus, VoltAddresses[Idx], SMBUS_REG_BUS_VOLTAGE, &Raw);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_WARN,
              "[ServerBios] Voltage sensor[%u] addr=0x%02X read failed: %r\n",
              (UINT32)Idx, VoltAddresses[Idx], Status));
      Snapshot->SensorsValid = FALSE;
      OverallStatus = EFI_DEVICE_ERROR;
    } else {
      Snapshot->VoltageMilliVolts[Idx] = Ina226DecodeBusVoltage (Raw);
      DEBUG ((DEBUG_VERBOSE,
              "[ServerBios] Volt[%u] = %u mV (raw=0x%04X)\n",
              (UINT32)Idx, Snapshot->VoltageMilliVolts[Idx], Raw));
    }
  }

  return OverallStatus;
}

/* ─────────────────────────────────────────────────────────────────────────
 * IPMI KCS helpers
 * ───────────────────────────────────────────────────────────────────────── */

EFI_STATUS
EFIAPI
IpmiKcsWaitForIbfClear (
  VOID
  )
{
  UINT32  Poll;
  UINT8   Status;

  for (Poll = 0; Poll < IPMI_KCS_MAX_POLL_COUNT; Poll++) {
    Status = IoRead8 (IPMI_KCS_STATUS_REG);
    if ((Status & IPMI_KCS_STATUS_IBF) == 0) {
      return EFI_SUCCESS;
    }
  }

  DEBUG ((DEBUG_ERROR, "[ServerBios] IPMI KCS: IBF did not clear (timeout)\n"));
  return EFI_TIMEOUT;
}

/* ─────────────────────────────────────────────────────────────────────────── */

EFI_STATUS
EFIAPI
IpmiKcsWaitForObfSet (
  VOID
  )
{
  UINT32  Poll;
  UINT8   Status;

  for (Poll = 0; Poll < IPMI_KCS_MAX_POLL_COUNT; Poll++) {
    Status = IoRead8 (IPMI_KCS_STATUS_REG);
    if ((Status & IPMI_KCS_STATUS_OBF) != 0) {
      return EFI_SUCCESS;
    }
  }

  DEBUG ((DEBUG_ERROR, "[ServerBios] IPMI KCS: OBF did not set (timeout)\n"));
  return EFI_TIMEOUT;
}

/* ─────────────────────────────────────────────────────────────────────────── */

/**
  Issue a KCS "WRITE_START" or "WRITE_END" command.

  @param[in]  IsLast  TRUE when sending the final data byte.
  @param[in]  Data    Data byte to write to the data-in register.

  @retval EFI_SUCCESS       Byte written.
  @retval EFI_TIMEOUT       IBF did not clear.
**/
STATIC
EFI_STATUS
IpmiKcsWriteByte (
  IN BOOLEAN  IsLast,
  IN UINT8    Data
  )
{
  EFI_STATUS  Status;

  Status = IpmiKcsWaitForIbfClear ();
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (!IsLast) {
    /* Signal WRITE_START: write 0x61 to the command/status register. */
    IoWrite8 (IPMI_KCS_STATUS_REG, 0x61);
  } else {
    /* Signal WRITE_END: write 0x62 to the command/status register. */
    IoWrite8 (IPMI_KCS_STATUS_REG, 0x62);
  }

  Status = IpmiKcsWaitForIbfClear ();
  if (EFI_ERROR (Status)) {
    return Status;
  }

  IoWrite8 (IPMI_KCS_DATA_IN_REG, Data);
  return EFI_SUCCESS;
}

/* ─────────────────────────────────────────────────────────────────────────── */

EFI_STATUS
EFIAPI
IpmiKcsSendReceive (
  IN  CONST IPMI_REQUEST  *Request,
  OUT IPMI_RESPONSE       *Response
  )
{
  EFI_STATUS  Status;
  UINT8       Idx;
  UINT8       RxByte;
  UINT8       StatusReg;
  BOOLEAN     CcReceived;

  if ((Request == NULL) || (Response == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  if (Request->DataLength > IPMI_MAX_PAYLOAD_SIZE) {
    return EFI_INVALID_PARAMETER;
  }

  ZeroMem (Response, sizeof (*Response));
  CcReceived = FALSE;

  /* ── Transmit phase ──────────────────────────────────────────────────── */

  /* Byte 0: NetFunction | (LUN << ?)  – send as not-last */
  Status = IpmiKcsWriteByte (FALSE, Request->NetFunction);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "[ServerBios] IPMI KCS: write NetFn failed: %r\n", Status));
    return Status;
  }

  /* Byte 1: Command – if no data follows this is the last byte */
  Status = IpmiKcsWriteByte ((BOOLEAN)(Request->DataLength == 0), Request->Command);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "[ServerBios] IPMI KCS: write Cmd failed: %r\n", Status));
    return Status;
  }

  /* Bytes 2..N: optional request data */
  for (Idx = 0; Idx < Request->DataLength; Idx++) {
    BOOLEAN LastByte = (BOOLEAN)(Idx == (Request->DataLength - 1));
    Status = IpmiKcsWriteByte (LastByte, Request->Data[Idx]);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR,
              "[ServerBios] IPMI KCS: write data[%u] failed: %r\n",
              (UINT32)Idx, Status));
      return Status;
    }
  }

  /* ── Receive phase ───────────────────────────────────────────────────── */

  Response->DataLength = 0;

  while (TRUE) {
    /* Issue a READ command: write 0x68 to the status/command register. */
    Status = IpmiKcsWaitForIbfClear ();
    if (EFI_ERROR (Status)) {
      return Status;
    }
    IoWrite8 (IPMI_KCS_STATUS_REG, 0x68);

    Status = IpmiKcsWaitForObfSet ();
    if (EFI_ERROR (Status)) {
      return Status;
    }

    StatusReg = IoRead8 (IPMI_KCS_STATUS_REG);
    RxByte    = IoRead8 (IPMI_KCS_DATA_OUT_REG);

    /* Check state machine bits S1:S0 */
    if ((StatusReg & (IPMI_KCS_STATUS_S1 | IPMI_KCS_STATUS_S0)) ==
        IPMI_KCS_STATUS_S0)
    {
      /* READ_STATE: more data to follow */
      if (!CcReceived) {
        /* First byte is always the IPMI completion code. */
        Response->CompletionCode = RxByte;
        CcReceived = TRUE;
      } else if (Response->DataLength < IPMI_MAX_PAYLOAD_SIZE) {
        Response->Data[Response->DataLength] = RxByte;
        Response->DataLength++;
      }
    } else if ((StatusReg & (IPMI_KCS_STATUS_S1 | IPMI_KCS_STATUS_S0)) == 0) {
      /* IDLE_STATE: transfer complete */
      break;
    } else {
      /* ERROR_STATE or unexpected */
      DEBUG ((DEBUG_ERROR,
              "[ServerBios] IPMI KCS: unexpected state 0x%02X\n", StatusReg));
      return EFI_DEVICE_ERROR;
    }
  }

  DEBUG ((DEBUG_VERBOSE,
          "[ServerBios] IPMI response: CC=0x%02X len=%u\n",
          Response->CompletionCode, Response->DataLength));
  return EFI_SUCCESS;
}

/* ─────────────────────────────────────────────────────────────────────────
 * UEFI variable helpers
 * ───────────────────────────────────────────────────────────────────────── */

EFI_STATUS
EFIAPI
PersistHealthSnapshot (
  IN CONST SERVER_HEALTH_SNAPSHOT  *Snapshot
  )
{
  if (Snapshot == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  return gRT->SetVariable (
                SERVER_PLATFORM_HEALTH_VAR_NAME,
                &mServerPlatformVarGuid,
                EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS,
                sizeof (SERVER_HEALTH_SNAPSHOT),
                (VOID *)Snapshot
                );
}

/* ─────────────────────────────────────────────────────────────────────────── */

EFI_STATUS
EFIAPI
RetrieveHealthSnapshot (
  OUT SERVER_HEALTH_SNAPSHOT  *Snapshot
  )
{
  UINTN  DataSize;

  if (Snapshot == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  DataSize = sizeof (SERVER_HEALTH_SNAPSHOT);

  return gRT->GetVariable (
                SERVER_PLATFORM_HEALTH_VAR_NAME,
                &mServerPlatformVarGuid,
                NULL,
                &DataSize,
                Snapshot
                );
}

/* ─────────────────────────────────────────────────────────────────────────
 * Thermal / voltage policy
 * ───────────────────────────────────────────────────────────────────────── */

VOID
EFIAPI
EvaluateThermalPolicy (
  IN CONST SERVER_HEALTH_SNAPSHOT  *Snapshot
  )
{
  STATIC CONST CHAR8 *TempSensorNames[SENSOR_TEMP_COUNT] = {
    "CPU0", "CPU1", "Inlet", "Outlet"
  };
  UINTN  Idx;

  if ((Snapshot == NULL) || !Snapshot->SensorsValid) {
    DEBUG ((DEBUG_WARN, "[ServerBios] EvaluateThermalPolicy: snapshot invalid, skipping.\n"));
    return;
  }

  for (Idx = 0; Idx < SENSOR_TEMP_COUNT; Idx++) {
    INT16 T = Snapshot->TemperatureC[Idx];

    if (T >= (INT16)TEMP_CRITICAL_THRESHOLD_C) {
      DEBUG ((DEBUG_ERROR,
              "[ServerBios] CRITICAL: %a temperature = %d °C (threshold %u °C)!\n",
              TempSensorNames[Idx], (INT32)T, TEMP_CRITICAL_THRESHOLD_C));
      /*
       * In a release build this would engage the emergency shutdown path
       * (e.g. via IPMI chassis-control command 0x02 = power-down).
       * In DEBUG builds we assert so the issue is caught immediately.
       */
      ASSERT (T < (INT16)TEMP_CRITICAL_THRESHOLD_C);
    } else if (T >= (INT16)TEMP_WARNING_THRESHOLD_C) {
      DEBUG ((DEBUG_WARN,
              "[ServerBios] WARNING: %a temperature = %d °C (threshold %u °C)\n",
              TempSensorNames[Idx], (INT32)T, TEMP_WARNING_THRESHOLD_C));
    } else {
      DEBUG ((DEBUG_VERBOSE,
              "[ServerBios] %a temperature = %d °C (OK)\n",
              TempSensorNames[Idx], (INT32)T));
    }
  }
}
