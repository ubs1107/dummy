/** @file
  ServerBiosDriver.c

  Main entry point and orchestration for the Server BIOS DXE Driver.

  This driver runs during the DXE phase (after hardware initialisation but
  before boot-device selection) and performs the following in order:

  1. Locate required UEFI protocols (SMBus HC, ACPI Table).
  2. Collect a full hardware-health snapshot (temperatures + voltages).
  3. Evaluate the thermal policy – halt on critical temperature.
  4. Install an OEM ACPI table advertising the driver version and sensor count.
  5. Persist the health snapshot to a non-volatile UEFI variable for use by
     the OS and out-of-band management software.

  Copyright (c) 2024, Example Corporation. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "ServerBiosDriver.h"
#include "ServerPlatformLib/ServerPlatformLib.h"

/* ─────────────────────────────────────────────────────────────────────────
 * Module-level driver context
 * ───────────────────────────────────────────────────────────────────────── */

STATIC SERVER_BIOS_DRIVER_CONTEXT  mDriverContext;

/* ─────────────────────────────────────────────────────────────────────────
 * Forward declarations of internal helpers
 * ───────────────────────────────────────────────────────────────────────── */

STATIC
EFI_STATUS
LocateRequiredProtocols (
  OUT SERVER_BIOS_DRIVER_CONTEXT  *Context
  );

STATIC
EFI_STATUS
InstallOemAcpiTable (
  IN OUT SERVER_BIOS_DRIVER_CONTEXT  *Context
  );

/* ─────────────────────────────────────────────────────────────────────────
 * Driver entry point
 * ───────────────────────────────────────────────────────────────────────── */

/**
  Driver entry point – see ServerBiosDriver.h for full specification.
**/
EFI_STATUS
EFIAPI
ServerBiosDriverEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;

  DEBUG ((DEBUG_INFO,
          "[ServerBios] Driver v%u.%u starting\n",
          SERVER_BIOS_DRIVER_VERSION_MAJOR,
          SERVER_BIOS_DRIVER_VERSION_MINOR));

  /* ── Step 0: Initialise the context block ────────────────────────────── */
  ZeroMem (&mDriverContext, sizeof (mDriverContext));
  mDriverContext.Signature = SERVER_BIOS_DRIVER_SIGNATURE;

  /* ── Step 1: Locate required protocols ───────────────────────────────── */
  Status = LocateRequiredProtocols (&mDriverContext);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR,
            "[ServerBios] Protocol location failed: %r\n", Status));
    return Status;
  }

  /* ── Step 2: Collect hardware health snapshot ────────────────────────── */
  Status = CollectHealthSnapshot (mDriverContext.Smbus, &mDriverContext.HealthSnapshot);
  if (EFI_ERROR (Status)) {
    /*
     * Non-fatal: report the error but continue.  The snapshot's SensorsValid
     * flag will be FALSE, so downstream consumers know the data is stale.
     */
    DEBUG ((DEBUG_WARN,
            "[ServerBios] One or more sensors could not be read: %r\n", Status));
  }

  /* ── Step 3: Evaluate thermal / voltage policy ───────────────────────── */
  EvaluateThermalPolicy (&mDriverContext.HealthSnapshot);

  /* ── Step 4: Publish OEM ACPI table ─────────────────────────────────── */
  Status = InstallOemAcpiTable (&mDriverContext);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN,
            "[ServerBios] ACPI table installation failed: %r (non-fatal)\n",
            Status));
    /* Continue – ACPI table is informational only. */
  }

  /* ── Step 5: Persist health snapshot to UEFI variable ───────────────── */
  Status = PersistHealthSnapshot (&mDriverContext.HealthSnapshot);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN,
            "[ServerBios] Failed to persist health snapshot: %r\n", Status));
    /* Continue – variable persistence is best-effort. */
  }

  DEBUG ((DEBUG_INFO, "[ServerBios] Initialisation complete.\n"));
  return EFI_SUCCESS;
}

/* ─────────────────────────────────────────────────────────────────────────
 * Internal helpers
 * ───────────────────────────────────────────────────────────────────────── */

/**
  Locate the SMBus HC and ACPI Table protocols required by this driver.

  @param[out]  Context  Driver context to populate with protocol pointers.

  @retval EFI_SUCCESS    Both protocols found.
  @retval EFI_NOT_FOUND  One or both protocols unavailable.
**/
STATIC
EFI_STATUS
LocateRequiredProtocols (
  OUT SERVER_BIOS_DRIVER_CONTEXT  *Context
  )
{
  EFI_STATUS  Status;

  ASSERT (Context != NULL);

  /* SMBus Host Controller – used to read sensors */
  Status = gBS->LocateProtocol (
                  &gEfiSmbusHcProtocolGuid,
                  NULL,
                  (VOID **)&Context->Smbus
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR,
            "[ServerBios] SMBus HC protocol not found: %r\n", Status));
    return EFI_NOT_FOUND;
  }

  /* ACPI Table – used to publish the OEM table */
  Status = gBS->LocateProtocol (
                  &gEfiAcpiTableProtocolGuid,
                  NULL,
                  (VOID **)&Context->AcpiTable
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR,
            "[ServerBios] ACPI Table protocol not found: %r\n", Status));
    return EFI_NOT_FOUND;
  }

  return EFI_SUCCESS;
}

/* ─────────────────────────────────────────────────────────────────────────── */

/**
  Compute an ACPI table checksum.

  Calculates and writes the 8-bit checksum field so that the sum of all bytes
  in the table is zero (mod 256), as required by the ACPI specification.

  @param[in,out]  TablePtr    Pointer to the ACPI table buffer.
  @param[in]      TableLength Number of bytes in the table.
**/
STATIC
VOID
AcpiChecksumTable (
  IN OUT UINT8   *TablePtr,
  IN     UINT32   TableLength
  )
{
  UINT8   Sum;
  UINT32  Idx;

  ASSERT (TablePtr != NULL);
  ASSERT (TableLength >= sizeof (EFI_ACPI_DESCRIPTION_HEADER));

  /* Zero the existing checksum field (offset 9 in the generic header). */
  TablePtr[9] = 0;

  Sum = 0;
  for (Idx = 0; Idx < TableLength; Idx++) {
    Sum = (UINT8)(Sum + TablePtr[Idx]);
  }

  TablePtr[9] = (UINT8)(0x100u - Sum);
}

/* ─────────────────────────────────────────────────────────────────────────── */

/**
  Build and install the OEM ACPI table.

  The table has a standard ACPI header followed by a small OEM-defined body
  that records the driver version and sensor count.

  @param[in,out]  Context  Driver context (AcpiTable protocol + key output).

  @retval EFI_SUCCESS           Table installed.
  @retval EFI_OUT_OF_RESOURCES  Memory allocation failure.
  @retval Other                 ACPI table protocol error.
**/
STATIC
EFI_STATUS
InstallOemAcpiTable (
  IN OUT SERVER_BIOS_DRIVER_CONTEXT  *Context
  )
{
  EFI_STATUS             Status;
  SERVER_BIOS_ACPI_TABLE *Table;
  UINT32                  TableLength;

  ASSERT (Context != NULL);
  ASSERT (Context->AcpiTable != NULL);

  TableLength = (UINT32)sizeof (SERVER_BIOS_ACPI_TABLE);

  Table = AllocateZeroPool (TableLength);
  if (Table == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  /* Populate the standard ACPI description header */
  Table->Header.Signature = SIGNATURE_32 ('S', 'B', 'I', 'O');
  Table->Header.Length    = TableLength;
  Table->Header.Revision  = 1;

  /* OEM identifiers – 6 and 8 chars, space-padded, NOT NUL-terminated */
  CopyMem (Table->Header.OemId,      ACPI_OEM_ID,       sizeof (Table->Header.OemId));
  CopyMem (Table->Header.OemTableId, ACPI_OEM_TABLE_ID, sizeof (Table->Header.OemTableId));

  Table->Header.OemRevision    = ACPI_OEM_REVISION;
  Table->Header.CreatorId      = SIGNATURE_32 ('E', 'X', 'A', 'M');
  Table->Header.CreatorRevision = 0x00000001;

  /* OEM-defined payload */
  Table->DriverVersion = (UINT32)((SERVER_BIOS_DRIVER_VERSION_MAJOR << 16) |
                                   SERVER_BIOS_DRIVER_VERSION_MINOR);
  Table->SensorCount   = (UINT8)(SENSOR_TEMP_COUNT + SENSOR_VOLT_COUNT);

  AcpiChecksumTable ((UINT8 *)Table, TableLength);

  Status = Context->AcpiTable->InstallAcpiTable (
                                 Context->AcpiTable,
                                 Table,
                                 TableLength,
                                 &Context->AcpiTableKey
                                 );
  FreePool (Table);

  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR,
            "[ServerBios] InstallAcpiTable failed: %r\n", Status));
  } else {
    DEBUG ((DEBUG_INFO,
            "[ServerBios] OEM ACPI table 'SBIO' installed (key=0x%lX)\n",
            (UINT64)Context->AcpiTableKey));
  }

  return Status;
}
