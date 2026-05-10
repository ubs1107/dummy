# I2cTelemetryDxe - UEFI I2C Temperature/Voltage Driver

`I2cTelemetryDxe` is a DXE driver that provides production-oriented I2C access
for board telemetry and exposes:

- Raw **I2C register read/write** APIs
- Normalized **temperature** and **voltage** sample APIs
- OS handoff via **EFI Configuration Table** and **UEFI runtime variable**

---

## Interfaces exposed by the driver

Header: `I2cTelemetryProtocol.h`  
Protocol GUID: `gEfiI2cTelemetryProtocolGuid`

### 1) `GetLatestSample()`
Returns the most recent cached telemetry sample:
- `TemperatureMilliCelsius`
- `VoltageMicroVolts`
- `SampleTime`
- `ValidFields` bitmask

### 2) `RefreshSample()`
Performs fresh I2C reads from temperature and voltage channels, updates cache,
and republishes telemetry for OS consumption.

### 3) `ReadRegister(SensorType, Register, BufferSize, Buffer)`
Reads an arbitrary register from selected sensor channel over I2C.

### 4) `WriteRegister(SensorType, Register, BufferSize, Buffer)`
Writes an arbitrary register on selected sensor channel over I2C.

---

## Sensor read/write flow

The driver uses `EFI_I2C_IO_PROTOCOL::QueueRequest()`:

- **Read register**
  1. write register address
  2. repeated-start + read payload bytes
- **Write register**
  1. write register address + payload

Implemented with standard `EFI_I2C_REQUEST_PACKET` operations.

---

## Temperature and voltage conversion

Current defaults in the sample implementation:

- Temperature: TMP102-style register format  
  - source register: `0x00`
  - output unit: **milli-Celsius**
- Voltage: INA219-style bus voltage register format  
  - source register: `0x02`
  - output unit: **micro-volts**

These defaults are easy to replace with your platform sensor map.

---

## How values are passed from UEFI BIOS to OS

The driver publishes telemetry through two OS-visible handoff paths:

1. **EFI Configuration Table**  
   - GUID: `gEfiI2cTelemetryTableGuid`
   - payload type: `EFI_I2C_TELEMETRY_TABLE`
   - Intended for OS loaders/runtime components that parse configuration tables
     before `ExitBootServices()`.

2. **UEFI Runtime Variable**  
   - Vendor GUID: `gEfiI2cTelemetryVariableGuid`
   - Variable name: `L"I2cTelemetrySample"`
   - Attributes: `EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS`
   - Intended for OS/runtime agents that retrieve telemetry via UEFI variable
     services.

### Typical OS boot flow

1. Bootloader finds config table entry with `gEfiI2cTelemetryTableGuid`
2. Bootloader copies sample into OS handoff block / DT / ACPI extension
3. OS telemetry/health subsystem consumes sample on early boot
4. Optional: OS reads `I2cTelemetrySample` runtime variable as fallback path

---

## Integration

Add module to platform DSC:

```ini
[Components]
  path/to/I2cTelemetryDxe/I2cTelemetryDxe.inf
```

Ensure board firmware already provides:
- I2C host/controller stack
- `EFI_I2C_IO_PROTOCOL` producers for telemetry sensors

---

## Production hardening recommendations

- Replace DevicePath string heuristics with board-specific sensor binding
- Add sensor presence/ID probing and whitelist validation
- Add policy checks before allowing register writes
- Add periodic refresh scheduling (timer event) if continuous telemetry is needed
- Map final handoff to platform ACPI objects (`_TMP`, power/voltage methods) for
  native OS thermal/power frameworks
