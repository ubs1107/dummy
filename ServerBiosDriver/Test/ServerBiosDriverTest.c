/** @file
  ServerBiosDriverTest.c

  Host-side unit tests for the Server BIOS DXE Driver.

  These tests exercise the pure-computation helpers in ServerPlatformLib that
  do *not* require live hardware or a UEFI environment:
    - Lm75DecodeTemperature  (temperature register decoder)
    - Ina226DecodeBusVoltage (voltage register decoder)

  Build & run (requires a C99-capable compiler, no EDK2 toolchain needed):

      gcc -std=c99 -Wall -Wextra -Wpedantic \
          -I.. \
          -DUNIT_TEST_BUILD \
          ServerBiosDriverTest.c \
          -o ServerBiosDriverTest
      ./ServerBiosDriverTest

  Copyright (c) 2024, Example Corporation. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

/* ─────────────────────────────────────────────────────────────────────────
 * Minimal EDK2 type / macro stubs for host-side compilation
 * ───────────────────────────────────────────────────────────────────────── */

#ifdef UNIT_TEST_BUILD

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* Basic EDK2-compatible type aliases */
typedef uint8_t   UINT8;
typedef uint16_t  UINT16;
typedef uint32_t  UINT32;
typedef int16_t   INT16;
typedef int32_t   INT32;
typedef uint8_t   BOOLEAN;
typedef void      VOID;
typedef uintptr_t UINTN;

#define TRUE   ((BOOLEAN)1)
#define FALSE  ((BOOLEAN)0)

/* Silence EDK2 macros used in headers */
#define DEBUG(x)          ((void)0)
#define ASSERT(x)         ((void)0)
#define ZeroMem(p, n)     memset((p), 0, (n))
#define CopyMem(d, s, n)  memcpy((d), (s), (n))
#define STATIC            static
#define EFIAPI
#define IN
#define OUT
#define CONST const

/* Minimal EFI_STATUS and return values */
typedef uintptr_t EFI_STATUS;
#define EFI_SUCCESS            ((EFI_STATUS)0)
#define EFI_INVALID_PARAMETER  ((EFI_STATUS)2)
#define EFI_DEVICE_ERROR       ((EFI_STATUS)7)
#define EFI_NOT_FOUND          ((EFI_STATUS)14)
#define EFI_TIMEOUT            ((EFI_STATUS)16)
#define EFI_ERROR(s)           ((s) != EFI_SUCCESS)

/* BIT macros */
#define BIT0  0x01u
#define BIT1  0x02u
#define BIT2  0x04u
#define BIT6  0x40u
#define BIT7  0x80u

/* Stub away SIGNATURE_32 */
#define SIGNATURE_32(a, b, c, d) \
    ((UINT32)((a) | ((b) << 8) | ((c) << 16) | ((d) << 24)))

/* ─────────────────────────────────────────────────────────────────────────
 * Pull in only the pure-computation functions under test.
 * We stub out everything that touches hardware or UEFI services.
 * ───────────────────────────────────────────────────────────────────────── */

/* Provide just the constants and signatures that the .c files need. */
#define SENSOR_TEMP_COUNT  4u
#define SENSOR_VOLT_COUNT  2u
#define TEMP_WARNING_THRESHOLD_C  85u
#define TEMP_CRITICAL_THRESHOLD_C 95u
#define SMBUS_ADDR_TEMP_CPU0   0x48u
#define SMBUS_ADDR_TEMP_CPU1   0x49u
#define SMBUS_ADDR_TEMP_INLET  0x4Au
#define SMBUS_ADDR_TEMP_OUTLET 0x4Bu
#define SMBUS_ADDR_VOLT_VDD_CORE 0x40u
#define SMBUS_ADDR_VOLT_DIMM_VDD 0x41u
#define SMBUS_REG_TEMPERATURE    0x00u
#define SMBUS_REG_BUS_VOLTAGE    0x02u
#define IPMI_KCS_MAX_POLL_COUNT  0x4000u
#define IPMI_MAX_PAYLOAD_SIZE    64u
#define IPMI_KCS_STATUS_OBF  BIT0
#define IPMI_KCS_STATUS_IBF  BIT1
#define IPMI_KCS_STATUS_S0   BIT6
#define IPMI_KCS_STATUS_S1   BIT7
#define IPMI_KCS_BASE_ADDRESS  0x0CA2u
#define IPMI_KCS_STATUS_REG    (IPMI_KCS_BASE_ADDRESS + 1u)
#define IPMI_KCS_DATA_IN_REG   (IPMI_KCS_BASE_ADDRESS + 0u)
#define IPMI_KCS_DATA_OUT_REG  (IPMI_KCS_BASE_ADDRESS + 0u)

typedef struct { BOOLEAN SensorsValid; INT16 TemperatureC[4]; UINT16 VoltageMilliVolts[2]; } SERVER_HEALTH_SNAPSHOT;

/* Function prototypes we are testing */
INT16  Lm75DecodeTemperature  (IN UINT16 RawRegister);
UINT16 Ina226DecodeBusVoltage (IN UINT16 RawRegister);

/* Inline the pure functions here so we can compile without EDK2 */
INT16
Lm75DecodeTemperature (
  IN UINT16  RawRegister
  )
{
  INT16  Signed9Bit;
  INT16  TempCx2;

  Signed9Bit = (INT16)((INT16)RawRegister >> 7);
  Signed9Bit = (INT16)(Signed9Bit & 0x01FF);
  if ((Signed9Bit & 0x0100) != 0) {
    Signed9Bit = (INT16)(Signed9Bit | (INT16)(INT16)0xFF00);
  }
  TempCx2 = Signed9Bit;
  return (INT16)(TempCx2 / 2);
}

UINT16
Ina226DecodeBusVoltage (
  IN UINT16  RawRegister
  )
{
  UINT32 Shifted = (UINT32)(RawRegister >> 3);
  return (UINT16)((Shifted * 5u) / 4u);
}

/* ─────────────────────────────────────────────────────────────────────────
 * Minimal test framework
 * ───────────────────────────────────────────────────────────────────────── */

static int g_pass = 0;
static int g_fail = 0;

#define CHECK_EQ(desc, actual, expected)                                   \
  do {                                                                     \
    if ((actual) == (expected)) {                                          \
      printf ("  PASS  %s\n", (desc));                                     \
      g_pass++;                                                            \
    } else {                                                               \
      printf ("  FAIL  %s  (got %ld, expected %ld)\n",                    \
              (desc), (long)(actual), (long)(expected));                   \
      g_fail++;                                                            \
    }                                                                      \
  } while (0)

/* ─────────────────────────────────────────────────────────────────────────
 * Lm75DecodeTemperature tests
 * ───────────────────────────────────────────────────────────────────────── */

static void
TestLm75DecodeTemperature (void)
{
  printf ("=== Lm75DecodeTemperature ===\n");

  /*
   * LM75 register layout (raw 16-bit after host-byte-order read):
   *   Bits [15:7] = 9-bit two's-complement temperature (0.5 °C / LSB)
   *   Bits [6:0]  = don't-care
   *
   * To encode:  raw = (temp_in_half_degrees & 0x1FF) << 7
   */

  /* 0 °C  → 0 half-degrees → raw = 0x0000 */
  CHECK_EQ ("0 °C",
            Lm75DecodeTemperature (0x0000),
            (INT16)0);

  /* +25 °C = 50 half-degrees = 0x32 → raw = 0x32 << 7 = 0x1900 */
  CHECK_EQ ("+25 °C",
            Lm75DecodeTemperature (0x1900),
            (INT16)25);

  /* +85 °C = 170 half-degrees = 0xAA → raw = 0xAA << 7 = 0x5500 */
  CHECK_EQ ("+85 °C (warning threshold)",
            Lm75DecodeTemperature (0x5500),
            (INT16)85);

  /* +127.5 °C → 255 half-degrees = 0xFF → raw = 0xFF << 7 = 0x7F80
   * Integer result truncated toward zero: 127 °C */
  CHECK_EQ ("+127 °C (positive extreme)",
            Lm75DecodeTemperature (0x7F80),
            (INT16)127);

  /*
   * -1 °C = -2 half-degrees.
   * Two's-complement 9-bit representation of -2 = 0x1FE
   * raw = (0x1FE & 0x1FF) << 7 = 0x1FE << 7 = 0xFF00
   */
  CHECK_EQ ("-1 °C",
            Lm75DecodeTemperature ((UINT16)0xFF00),
            (INT16)-1);

  /* -55 °C = -110 half-degrees.
   * 9-bit two's complement of -110 = 0x1FF - 110 + 1 = 0x192
   * raw = 0x192 << 7 = 0xC900 */
  CHECK_EQ ("-55 °C (minimum LM75 range)",
            Lm75DecodeTemperature ((UINT16)0xC900),
            (INT16)-55);

  printf ("\n");
}

/* ─────────────────────────────────────────────────────────────────────────
 * Ina226DecodeBusVoltage tests
 * ───────────────────────────────────────────────────────────────────────── */

static void
TestIna226DecodeBusVoltage (void)
{
  printf ("=== Ina226DecodeBusVoltage ===\n");

  /*
   * INA226 Bus Voltage Register:
   *   Bits [15:3] = voltage reading, LSB = 1.25 mV
   *   Bits [2:0]  = reserved / flags (ignored)
   *
   * To encode N mV (when N is a multiple of 1.25):
   *   raw_field = N / 1.25 = N * 4 / 5
   *   raw_reg   = raw_field << 3
   */

  /* 0 mV */
  CHECK_EQ ("0 mV",
            Ina226DecodeBusVoltage (0x0000),
            (UINT16)0);

  /* 1000 mV (1 V): field = 1000/1.25 = 800 = 0x320, raw = 0x320 << 3 = 0x1900 */
  CHECK_EQ ("1000 mV (1.0 V)",
            Ina226DecodeBusVoltage (0x1900),
            (UINT16)1000);

  /* 3300 mV (3.3 V): field = 3300/1.25 = 2640 = 0xA50, raw = 0xA50 << 3 = 0x5280 */
  CHECK_EQ ("3300 mV (3.3 V)",
            Ina226DecodeBusVoltage (0x5280),
            (UINT16)3300);

  /* 5000 mV (5 V): field = 5000/1.25 = 4000 = 0xFA0, raw = 0xFA0 << 3 = 0x7D00 */
  CHECK_EQ ("5000 mV (5.0 V)",
            Ina226DecodeBusVoltage (0x7D00),
            (UINT16)5000);

  /* 12000 mV (12 V): field = 12000/1.25 = 9600 = 0x2580, raw = 0x2580 << 3 = 0x12C00
   * Note: 0x12C00 is 17-bit; truncated to 16-bit = 0x2C00 (4 mV / 1.25 = 3.2, field=3, V=3.75)
   * so this tests that 12 V fits in an unsigned 16-bit result as millivolts. */
  /* field = 9600; raw = 9600 << 3 = 76800 = 0x12C00 -> 16-bit truncation to 0x2C00
   * 0x2C00 >> 3 = 0x580 = 1408; * 5 / 4 = 1760 mV – not 12000.
   * INA226 supports up to 36 V but millivolt result fits in UINT16 only up to 65535 mV.
   * Let's use a realistic 12 V test with a valid 16-bit raw register. */
  /* field(12000 mV) = 12000*4/5 = 9600 = 0x2580; raw16 = 9600 << 3 = 76800 overflows uint16.
   * Instead test with 8000 mV (8 V): field = 8000*4/5 = 6400 = 0x1900, raw = 0x1900<<3 = 0xC800 */
  CHECK_EQ ("8000 mV (8.0 V)",
            Ina226DecodeBusVoltage (0xC800),
            (UINT16)8000);

  printf ("\n");
}

/* ─────────────────────────────────────────────────────────────────────────
 * Main
 * ───────────────────────────────────────────────────────────────────────── */

int
main (void)
{
  printf ("Server BIOS Driver – Unit Tests\n");
  printf ("================================\n\n");

  TestLm75DecodeTemperature ();
  TestIna226DecodeBusVoltage ();

  printf ("Results: %d passed, %d failed\n", g_pass, g_fail);
  return (g_fail == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}

#endif /* UNIT_TEST_BUILD */
