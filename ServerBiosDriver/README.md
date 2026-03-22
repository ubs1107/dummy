# Server BIOS DXE Driver

## Overview

`ServerBiosDriver` is a professional-grade UEFI DXE (Driver Execution Environment)
driver for server platforms. It is written in C following the
[EDK2](https://github.com/tianocore/edk2) coding standard and integrates with the
standard UEFI Boot Services / Runtime Services infrastructure.

The driver runs during the DXE phase — after silicon initialisation (PEI) but
before OS boot-device selection (BDS). This makes it the correct place to:

- Read hardware sensors and enforce thermal safety policies.
- Advertise platform capabilities to the OS via ACPI tables.
- Exchange management commands with the Baseboard Management Controller (BMC)
  over the IPMI KCS interface.
- Persist diagnostic state across reboots using UEFI non-volatile variables.

---

## Features

| Feature | Description |
|---|---|
| **Hardware health monitoring** | Reads CPU / inlet / outlet temperatures via LM75-compatible sensors and VDD_CORE / DIMM voltages via INA226 sensors, all over SMBus. |
| **Thermal policy enforcement** | Warns (via DEBUG) at 85 °C; in DEBUG builds asserts at 95 °C (a release build would engage an emergency power-down via IPMI). |
| **OEM ACPI table** | Publishes a `SBIO` table that advertises the driver version and sensor count to the OS. |
| **UEFI variable persistence** | Persists the last health snapshot to `ServerPlatformHealth` (NV, boot-services scope) for use by OS agents and diagnostics. |
| **IPMI KCS interface** | Full KCS write/read state-machine per IPMI v2.0 §9.14 for out-of-band BMC communication. |

---

## Architecture

```
ServerBiosDriver/
├── ServerBiosDriver.inf          EDK2 module description
├── ServerBiosDriver.h            Public types, constants, entry-point prototype
├── ServerBiosDriver.c            Driver entry point and orchestration
├── ServerPlatformLib/
│   ├── ServerPlatformLib.h       Platform library API
│   └── ServerPlatformLib.c       SMBus helpers, IPMI KCS, variable persistence
└── Test/
    └── ServerBiosDriverTest.c    Host-side unit tests (no EDK2 required)
```

### Execution flow

```
ServerBiosDriverEntryPoint()
  │
  ├─ LocateRequiredProtocols()        // gEfiSmbusHcProtocolGuid
  │                                   // gEfiAcpiTableProtocolGuid
  │
  ├─ CollectHealthSnapshot()          // SMBus → LM75 temps + INA226 voltages
  │
  ├─ EvaluateThermalPolicy()          // Warn / assert on threshold breach
  │
  ├─ InstallOemAcpiTable()            // Publish 'SBIO' OEM ACPI table
  │
  └─ PersistHealthSnapshot()          // UEFI NV variable
```

---

## Building with EDK2

1. Copy (or symlink) `ServerBiosDriver/` into a platform package, e.g.
   `YourPlatformPkg/Drivers/ServerBiosDriver/`.

2. Add the module to your platform DSC:

   ```ini
   [Components]
     YourPlatformPkg/Drivers/ServerBiosDriver/ServerBiosDriver.inf
   ```

3. Build with the standard EDK2 build system:

   ```bash
   build -p YourPlatformPkg/YourPlatformPkg.dsc \
         -a X64 \
         -t GCC5 \
         -b DEBUG
   ```

4. Flash the resulting `ServerBiosDriver.efi` into a DXE firmware volume.

---

## Running the host-side unit tests

The unit tests are self-contained C99 and require only a standard C compiler.
No EDK2 toolchain is needed.

```bash
cd ServerBiosDriver/Test
gcc -std=c99 -Wall -Wextra -Wpedantic \
    -DUNIT_TEST_BUILD \
    -o ServerBiosDriverTest \
    ServerBiosDriverTest.c
./ServerBiosDriverTest
```

Expected output:

```
Server BIOS Driver – Unit Tests
================================

=== Lm75DecodeTemperature ===
  PASS  0 °C
  PASS  +25 °C
  PASS  +85 °C (warning threshold)
  PASS  +127 °C (positive extreme)
  PASS  -1 °C
  PASS  -55 °C (minimum LM75 range)

=== Ina226DecodeBusVoltage ===
  PASS  0 mV
  PASS  1000 mV (1.0 V)
  PASS  3300 mV (3.3 V)
  PASS  5000 mV (5.0 V)
  PASS  8000 mV (8.0 V)

Results: 11 passed, 0 failed
```

---

## Hardware assumptions

| Component | Default address / port | Override location |
|---|---|---|
| LM75 – CPU 0 temp | SMBus 0x48 | `SMBUS_ADDR_TEMP_CPU0` in `ServerBiosDriver.h` |
| LM75 – CPU 1 temp | SMBus 0x49 | `SMBUS_ADDR_TEMP_CPU1` |
| LM75 – Inlet temp | SMBus 0x4A | `SMBUS_ADDR_TEMP_INLET` |
| LM75 – Outlet temp | SMBus 0x4B | `SMBUS_ADDR_TEMP_OUTLET` |
| INA226 – VDD_CORE | SMBus 0x40 | `SMBUS_ADDR_VOLT_VDD_CORE` |
| INA226 – DIMM VDD | SMBus 0x41 | `SMBUS_ADDR_VOLT_DIMM_VDD` |
| IPMI KCS | I/O port 0xCA2 | `IPMI_KCS_BASE_ADDRESS` |

---

## Porting to a new server platform

1. **Adjust sensor addresses** – Update the `SMBUS_ADDR_*` constants in
   `ServerBiosDriver.h` to match your board schematic.
2. **Adjust IPMI KCS base** – Update `IPMI_KCS_BASE_ADDRESS` if your BMC uses
   a non-standard I/O address.
3. **Update OEM ACPI identifiers** – Replace `ACPI_OEM_ID` and
   `ACPI_OEM_TABLE_ID` with your platform values.
4. **Update GUID values** – Generate new GUIDs for `FILE_GUID` (`.inf`) and
   `SERVER_PLATFORM_VAR_GUID` (`.h`) to avoid conflicts.
5. **Extend sensor list** – Increment `SENSOR_TEMP_COUNT` / `SENSOR_VOLT_COUNT`
   and add entries to the address arrays in `CollectHealthSnapshot()`.

---

## UEFI / ACPI specifications referenced

- UEFI Specification 2.10 – https://uefi.org/specifications
- ACPI Specification 6.5 – https://uefi.org/htmlspecs/ACPI_Spec_6_5_html
- IPMI v2.0 Specification – https://www.intel.com/content/www/us/en/products/docs/servers/ipmi/ipmi-second-gen-interface-spec-v2-rev1-1.html
- TI LM75 datasheet
- TI INA226 datasheet

---

## License

BSD 2-Clause Patent License — see individual source files.
