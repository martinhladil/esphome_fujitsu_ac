# Fujitsu AC UART Protocol

Protocol specification for the serial bus used by Fujitsu air conditioners
between the indoor unit mainboard and an external controller dongle (the
**UTY-TFSXW1** family of CN connectors, also used by the FGLair® dongles).

Throughout this document:

- **Controller** = the device that initiates communication and polls (the role
  you implement when replacing the dongle).
- **Unit** = the AC indoor unit, which only answers requests.

> The controller is the **master** (it polls); the unit is the **slave** (it
> answers). Other controllers (IR remote, UTY-RLRY wall controller) operate on
> the same unit independently; the wall controller is two-way and reflects
> changes made over this bus.

---

## 1. Physical / Electrical Layer

| Property        | Value                                             |
|-----------------|---------------------------------------------------|
| Bus             | Single async UART pair (separate TX and RX lines) |
| Baud rate       | **9600**                                          |
| Frame format    | **8 data bits, no parity, 1 stop bit (8N1)**      |
| Flow control    | None                                              |
| Signal polarity | **Inverted** TX and RX (`UART_SIGNAL_TXD_INV \| UART_SIGNAL_RXD_INV`) |
| Logic level     | AC side needs a logic-level shifter; supply via 12 V → 5 V DC/DC |

The AC connector (4-pin) carries `+12V`, `GND`, and the two data lines (one
controller→unit, one unit→controller).

> ⚠️ The AC data/GND pins are **not galvanically isolated** and not referenced
> to earth GND. Never cross-connect AC GND to a PC/laptop GND.

**Signal inversion is essential** — if your UART peripheral cannot invert TX/RX
in hardware, add an external inverter. Without it the framing will look like
noise.

---

## 2. Frame Structure

All messages (both directions) share one framing format:

```
+--------+--------+--------+--------+--------+============+--------+--------+
| byte 0 | byte 1 | byte 2 | byte 3 | byte 4 |  payload   | CKSUM_H| CKSUM_L|
| CMD    |  0x00  |  0x00  |  0x00  |  LEN   | (LEN bytes)| (high) | (low)  |
+--------+--------+--------+--------+--------+============+--------+--------+
```

| Field      | Offset      | Description                                                   |
|------------|-------------|---------------------------------------------------------------|
| `CMD`      | 0           | Command / message type (see §4)                               |
| reserved   | 1‑3         | Always `0x00 0x00 0x00`                                        |
| `LEN`      | 4           | Number of **payload bytes** that follow (not counting header or checksum) |
| payload    | 5 … 5+LEN‑1 | Command-specific data                                         |
| checksum   | last 2      | 16-bit checksum, **big-endian** (high byte first)             |

**Total frame length = `LEN + 7`** (5 header bytes + `LEN` payload bytes + 2
checksum bytes).

### 2.1 Receive framing rule

The frame is **self-delimiting**: byte 4 (`LEN`) gives the payload size, so a
receiver knows the exact total length (`LEN + 7`) as soon as it has the first 5
bytes, and the trailing checksum (§3) confirms the framing was correct. This is
the only framing mechanism a conforming receiver *must* implement:

- Accumulate incoming bytes. Once more than 4 are buffered, the frame is complete
  when the number of received bytes equals `buffer[4] + 7`.
- Verify the checksum; if it fails, the frame was mis-aligned or corrupt — resync
  and try again (see below).

```c
// length-based completion (pseudo-code)
buffer[index++] = b;
if (index > 4 && index == buffer[4] + 7) {
    handleFrame(buffer, buffer[4] + 7);   // then validate checksum
}
```

Frames are short — the largest in practice is well under 128 bytes — so a
128-byte receive buffer is sufficient.

#### Resynchronization

If a byte is dropped or line noise appears, a length-based parser can latch onto
a wrong `LEN` and get stuck mid-frame. The protocol itself does not mandate a
recovery mechanism; choose one:

- **Checksum-driven sliding (robust, timing-free):** on a checksum failure, drop
  the first buffered byte, shift the rest down, and re-scan for a frame that
  validates. Resynchronizes deterministically without depending on timing.
- **Inter-byte gap reset (simple):** since frames are transmitted as a single
  contiguous burst, a quiet period between bytes marks a frame boundary. If
  **≥ 20 ms** elapse between two received bytes, discard the buffer and restart.
  At 9600 baud a byte is ~1 ms, so 20 ms reliably separates frames. This is
  cheap but couples the parser to wall-clock timing and can misbehave under
  scheduler jitter or batched/buffered UART reads.

---

## 3. Checksum

16-bit additive checksum, transmitted big-endian in the last two bytes.

**Compute** (over every byte except the two checksum bytes):

```c
uint16_t checksum = 0xFFFF;
for (int i = 0; i < size - 2; i++) {
    checksum -= buffer[i];
}
// store big-endian
frame[size - 2] = (checksum >> 8) & 0xFF;  // high
frame[size - 1] =  checksum       & 0xFF;  // low
```

**Verify:**

```c
uint16_t got = (buffer[size-2] << 8) | buffer[size-1];
uint16_t calc = 0xFFFF;
for (int i = 0; i < size - 2; i++) calc -= buffer[i];
bool valid = (got == calc);
```

Equivalently, the checksum is `0xFFFF − (sum of all bytes before the checksum)`,
taken modulo 2¹⁶. It can be accumulated incrementally as each byte is emitted.

---

## 4. Message Types (`CMD`, byte 0)

| `CMD` | Direction            | Meaning                                  |
|-------|----------------------|------------------------------------------|
| `0x00`| controller → unit    | **Init 1** (handshake stage 1)           |
| `0x01`| controller → unit    | **Init 2** (handshake stage 2)           |
| `0x02`| controller → unit    | **Write registers** (set values)         |
| `0x03`| controller → unit    | **Read registers** (request values)      |

The unit **echoes the same `CMD` byte** in its response. For data responses
(`0x02`/`0x03`), payload **byte 5 is a status byte** where `0x01` = OK.

---

## 5. Handshake / Initialization

Before any register access, the controller performs a two-step handshake. Each
step is a fixed request expecting a fixed response. A response that does not
match the expected bytes should be treated as a fault (abort and retry the
handshake from the start).

### Init 1

```
controller → unit:  00 00 00 00 04 00 00 00 00 FF FB
unit → controller:  00 00 00 00 01 01 FF FD          (expected: status 0x01)
```

If the unit has just powered up it may first emit one of these transient
"restarting" frames. They are not the Init 1 response — ignore them and keep
waiting for `00 00 00 00 01 01 FF FD`:

```
FE 00 00 00 01 02 FE FE
FC 00 00 00 01 02 FF 00
```

### Init 2

```
controller → unit:  01 00 00 00 04 00 04 00 01 FF F5
unit → controller:  01 00 00 00 01 01 FF FC          (expected: status 0x01)
```

> **Variant (J-series, e.g. TFSXJ4):** Init 2 differs —
> `01 00 00 00 08 00 01 00 01 00 04 00 00 FF F0`. The TFSXW1 sequence above is
> the confirmed/working one; the J-series register layout is not fully mapped.

After a successful Init 2 the link is established and register polling begins.

---

## 6. Reading Registers (`CMD = 0x03`)

A register is a 16-bit address holding a 16-bit value. Reads are batched: the
request lists addresses, the response returns address+value pairs.

### 6.1 Read request

- `LEN = 2 × N` where `N` = number of registers requested.
- Payload = `N` × `[addrHigh, addrLow]` (big-endian addresses).

```
03 00 00 00 <2N> <addr0_H addr0_L> <addr1_H addr1_L> … <CK_H CK_L>
```

Example (request 2 registers `0x0001`, `0x0101`):

```
03 00 00 00 04 00 01 01 01 <CK_H> <CK_L>
```

### 6.2 Read response

- `LEN = 4 × N + 1` (the `+1` is the leading status byte).
- Payload byte 5 = **status** (`0x01` = OK).
- Then `N` × `[addrHigh, addrLow, valueHigh, valueLow]`.

```
03 00 00 00 <4N+1> 01 <addr0_H addr0_L val0_H val0_L> … <CK_H CK_L>
```

Decoding (number of pairs = `LEN / 4`, integer division drops the status byte):

```c
int count = buffer[4] / 4;
for (int i = 0; i < count; i++) {
    int idx = 6 + i * 4;
    uint16_t addr  = (buffer[idx]   << 8) | buffer[idx+1];
    uint16_t value = (buffer[idx+2] << 8) | buffer[idx+3];
    // store value for addr
}
```

---

## 7. Writing Registers (`CMD = 0x02`)

- `LEN = 4 × N`.
- Payload = `N` × `[addrHigh, addrLow, valueHigh, valueLow]`.

```
02 00 00 00 <4N> <addr0_H addr0_L val0_H val0_L> … <CK_H CK_L>
```

### Write acknowledgement

```
unit → controller:  02 00 00 00 01 01 FF FB           (status 0x01 = OK)
```

After a write, it is good practice to **re-read** those registers (a normal
`0x03` read) to confirm the unit applied them; the unit does not push state
changes on its own, so polling is the only way to observe the result.

> Settings are typically written **one register at a time**. The exception is
> airflow position, which writes two registers in one frame (see §11).

---

## 8. Polling Cycle & Timing

The controller is the sole initiator: it sends one request and waits for the
matching response before sending the next. Requests are not pipelined. The unit
never speaks unprompted, so the controller must poll to observe state.

Suggested timing (values known to work; tune as needed):

| Parameter                    | Value   | Purpose                                  |
|------------------------------|---------|------------------------------------------|
| Inter-request interval       | ~400 ms | spacing between consecutive requests     |
| Response timeout             | ~200 ms | how long to wait before treating a request as unanswered |

On a response timeout, retry the handshake during the init phase, or surface a
"no response" condition once the link is established.

### Request sequence

```
(start) → Init1 → Init2
        → capability reads (once)
        → state poll group A ──► [pending write? → write → read-back]
                → state poll group B → state poll group C → group A → …
```

1. **Capability reads (once, after the handshake):** batch-read the
   capability/feature-flag registers (§9.1) to learn which features the unit
   supports and how many louver positions exist.
2. **State polling (continuous):** repeatedly batch-read the state registers
   (§9.2–9.4) to keep a local mirror of the unit's state. Any grouping of
   addresses into read batches works; §9.5 lists one practical grouping.
3. **Writes:** when a change is requested, send the `0x02` write between polls,
   then read back the written addresses to confirm the new value.

Batching is purely an efficiency choice — a read request may carry any number of
addresses (subject to the frame size limit), so groupings can be merged or split
freely.

---

## 9. Register Map (UTY-TFSXW1)

Addresses are 16-bit. Values are 16-bit. The high nibble of the address roughly
groups registers: `0x00xx`/`0x01xx` = capabilities/feature flags, `0x10xx` =
primary state, `0x11xx` = extended features, `0x14xx`/`0x20xx` = diagnostics.

### 9.1 Capability / feature-flag registers (read once at init)

A value of `0x0001` means **supported**; `0x0000` means not supported. The two
airflow *count* registers are non-boolean: `> 0` means supported. On at least one
unit the value is a **literal position count** — `VerticalAirflowDirectionCount`
read `0x0004` and the unit accepted positions `0x01`–`0x04` while rejecting
`0x05`/`0x06` — but another unit has been observed reporting `0x0015` for
`HorizontalAirflowDirectionCount`, which is not a plausible literal count, so the
encoding is not fully decoded across models. Practical rule: `0x0000` = airflow
unsupported (hide the entity); otherwise treat the value as a position count
**clamped to 1–6** (the defined position codes are `0x01`–`0x06`, see §10.4).

| Address  | Name                              | Meaning of value                          |
|----------|-----------------------------------|-------------------------------------------|
| `0x0001` | Initial0                          | handshake/identity probe                  |
| `0x0101` | Initial1                          | handshake/identity probe                  |
| `0x0110`–`0x0120` | Initial2 … Initial11     | misc identity/capability probes           |
| `0x0130` | VerticalAirflowDirectionCount     | vertical airflow capability code (non-zero = supported; not a literal count — see §9.1 / §10.4) |
| `0x0131` | VerticalSwingSupported            | 1 = vertical swing available              |
| `0x0142` | HorizontalAirflowDirectionCount   | horizontal airflow capability code (non-zero = supported; not a literal count — see §9.1 / §10.4) |
| `0x0143` | HorizontalSwingSupported          | 1 = horizontal swing available            |
| `0x0150` | EconomyModeSupported              | 1 = economy mode available                |
| `0x0151` | MinimumHeatSupported              | 1 = 10 °C minimum-heat available          |
| `0x0152` | HumanSensorSupported              | 1 = human-presence sensor available       |
| `0x0153` | EnergySavingFanSupported          | 1 = energy-saving fan available           |
| `0x0154`–`0x0156` | Initial20 … Initial22    | misc capability probes                    |
| `0x0170` | PowerfulSupported                 | 1 = powerful/boost available              |
| `0x0171` | OutdoorUnitLowNoiseSupported      | 1 = outdoor low-noise available           |
| `0x0193` | CoilDrySupported                  | 1 = coil-dry available                     |

### 9.2 Primary state registers (`0x10xx`)

| Address  | Name                            | Encoding (see §10)                       |
|----------|---------------------------------|------------------------------------------|
| `0x1000` | Power                           | 0 = Off, 1 = On                          |
| `0x1001` | Mode                            | Auto/Cool/Dry/Fan/Heat                   |
| `0x1002` | SetpointTemp                    | temperature × 10 (tenths °C)             |
| `0x1003` | FanSpeed                        | Auto/Quiet/Low/Medium/High               |
| `0x1010` | VerticalAirflowSetterRegistry   | **write** target for vertical position   |
| `0x1011` | VerticalSwing                   | 0 = Off, 1 = On                          |
| `0x10A0` | VerticalAirflow                 | **read-back** of vertical position       |
| `0x1022` | HorizontalAirflowSetterRegistry | **write** target for horizontal position |
| `0x1023` | HorizontalSwing                 | 0 = Off, 1 = On                          |
| `0x10A9` | HorizontalAirflow               | **read-back** of horizontal position     |
| `0x1031` | Register11                      | unknown / internal                       |
| `0x1033` | ActualTemp                      | indoor temperature, offset encoding      |
| `0x1034` | Register13                      | unknown / internal                       |

### 9.3 Extended-feature registers (`0x11xx`, `0x12xx`)

| Address  | Name                  | Encoding             |
|----------|-----------------------|----------------------|
| `0x1100` | EconomyMode           | 0 = Off, 1 = On      |
| `0x1101` | MinimumHeat           | 0 = Off, 1 = On      |
| `0x1102` | HumanSensor           | 0 = Off, 1 = On      |
| `0x1103`–`0x1107` | Register17–21| unknown / internal   |
| `0x1108` | EnergySavingFan       | 0 = Off, 1 = On      |
| `0x1109` | Register23            | unknown / internal   |
| `0x1120` | Powerful              | 0 = Off, 1 = On      |
| `0x1121` | OutdoorUnitLowNoise   | 0 = Off, 1 = On      |
| `0x1144` | CoilDry               | 0 = Off, 1 = On      |
| `0x1141` | Register32            | unknown / internal   |
| `0x1200`–`0x1204` | Register27–31| unknown / internal   |

### 9.4 Diagnostic / misc registers (`0x14xx`, `0x20xx`, `0xF0xx`)

| Address  | Name        | Notes                          |
|----------|-------------|--------------------------------|
| `0x1400`–`0x1406`, `0x140E` | Register33–40 | unknown / internal |
| `0x2000` | Register41  | unknown / internal             |
| `0x2020` | OutdoorTemp | outdoor temperature, offset encoding |
| `0x2021` | Register43  | unknown / internal             |
| `0xF001` | Register44  | unknown / internal             |

### 9.5 Example address groupings

One workable way to split the addresses into read batches. These groupings are
not mandated by the protocol — any addresses may be combined into a single read
request — but reading the known-good sets below avoids requesting addresses a
given unit may not implement.

Capability batches (read once, after the handshake):

- **Caps 1:** `0x0001, 0x0101`
- **Caps 2:** `0x0110, 0x0111, 0x0112, 0x0113, 0x0114, 0x0115,
  0x0117, 0x011A, 0x011D, 0x0120, 0x0130, 0x0131, 0x0142, 0x0143`
- **Caps 3:** `0x0150, 0x0151, 0x0152, 0x0153, 0x0154, 0x0155,
  0x0156, 0x0170, 0x0171, 0x0193`

State batches (read continuously):

- **State A:** `0x1000, 0x1001, 0x1002, 0x1003, 0x1010, 0x1011,
  0x10A0, 0x1022, 0x1023, 0x10A9, 0x1031, 0x1033, 0x1034`
- **State B:** `0x1100, 0x1101, 0x1102, 0x1103, 0x1104, 0x1105,
  0x1106, 0x1107, 0x1108, 0x1109, 0x1120, 0x1121, 0x1144, 0x1200, 0x1201,
  0x1202, 0x1203, 0x1204, 0x1141`
- **State C:** `0x1400, 0x1401, 0x1402, 0x1403, 0x1404, 0x1405,
  0x1406, 0x140E, 0x2000, 0x2020, 0x2021, 0xF001`

---

## 10. Value Encodings

### 10.1 Power (`0x1000`)

| Value    | Meaning |
|----------|---------|
| `0x0000` | Off     |
| `0x0001` | On      |

### 10.2 Mode (`0x1001`)

| Value    | Meaning |
|----------|---------|
| `0x0000` | Auto    |
| `0x0001` | Cool    |
| `0x0002` | Dry     |
| `0x0003` | Fan     |
| `0x0004` | Heat    |

A value of `0x0005` also exists, used internally/transiently by the reference
firmware; it is not a user-selectable mode and its meaning on the bus is
unconfirmed. Decoders should treat it as "unknown" rather than a real mode.

Note that "off" is **not** a mode — on/off is the separate Power register
(`0x1000`). The Mode register retains its value while the unit is off. A climate
UI that has a single OFF/Auto/Cool/… selector therefore maps OFF to
`Power = Off` and uses the Mode register only while powered on.

### 10.3 Fan speed (`0x1003`)

| Value    | Meaning |
|----------|---------|
| `0x0000` | Auto    |
| `0x0002` | Quiet   |
| `0x0005` | Low     |
| `0x0008` | Medium  |
| `0x000B` | High    |

### 10.4 Airflow position (`0x10A0` read / `0x1010` write, and horizontal `0x10A9`/`0x1022`)

| Value    | Meaning     |
|----------|-------------|
| `0x0000` | Closed (reported while the unit is off; the louver closes) |
| `0x0001` | Position 1  |
| `0x0002` | Position 2  |
| `0x0003` | Position 3  |
| `0x0004` | Position 4  |
| `0x0005` | Position 5  |
| `0x0006` | Position 6  |
| `0x0020` | Swing       |

Positions `0x01`…`0x06` plus the dedicated `Swing` code (`0x20`) are the defined
settable values. `0x0000` is reported (not set) while the unit is powered off and
maps to a reported-only **Closed** state. The `…DirectionCount` capability
register only indicates whether airflow control is available (non-zero); it is
**not** a literal position count (§9.1), so cap the number of selectable positions
at **6**.

### 10.5 On/Off feature registers

Swing (`0x1011`, `0x1023`), Powerful, EconomyMode, EnergySavingFan,
OutdoorUnitLowNoise, CoilDry, HumanSensor, MinimumHeat all use:

| Value    | Meaning |
|----------|---------|
| `0x0000` | Off     |
| `0x0001` | On      |

### 10.6 Setpoint temperature (`0x1002`)

Stored as **tenths of a degree Celsius**:

```
°C = value / 10           e.g. 0x00FA (250) = 25.0 °C
value = round(°C × 10)
```

- Resolution is **0.5 °C**, so the raw value is snapped to the nearest multiple
  of 5: `value = ((round(°C·10) + 2) / 5) · 5`.
- Range (clamped): minimum **16.0 °C** in Heat mode, **18.0 °C** otherwise;
  maximum **30.0 °C** (`value` 160/180 … 300).
- In **Fan** mode there is no setpoint; the unit reports `0xFFFF`.

### 10.7 Indoor temperature (`0x1033`, ActualTemp)

Offset-encoded; subtract **5025** then divide by 100:

```
°C = (value − 5025) / 100
```

This register updates frequently; if you forward it to an upstream system you
may want to rate-limit or debounce it (the value is reported in 0.01 °C units).

### 10.8 Outdoor temperature (`0x2020`, OutdoorTemp)

Same offset encoding, but **signed** (can be negative):

```
°C = (value − 5025) / 100      (signed)
```

---

## 11. Setting Behaviour & Business Rules

These are constraints of the unit's behaviour. Observing them avoids writes that
the unit silently ignores or that produce surprising results.

- **Airflow position is a two-register write:** to set a vertical position, write
  **`VerticalSwing (0x1011) = Off`** *and*
  **`VerticalAirflowSetterRegistry (0x1010) = position`** in one `0x02` frame.
  Horizontal is analogous: `HorizontalSwing (0x1023) = Off` +
  `HorizontalAirflowSetterRegistry (0x1022) = position`. The resulting position
  is then reported back in the read-only `0x10A0` / `0x10A9` registers — i.e. you
  write the setter register but read the position from a different address.
- **Capability gating:** a unit only accepts feature/swing/airflow changes for
  features it reports as supported (§9.1). Writing an unsupported feature has no
  effect.
- **Coil-Dry lockout:** while CoilDry is on, the unit ignores changes to mode,
  fan, temperature, airflow, swing, powerful, and economy.
- **Minimum-Heat lockout:** while MinimumHeat is on, the unit ignores changes to
  mode, fan, temperature, powerful, economy, and energy-saving fan.
- **No setpoint in Fan mode:** the unit reports `SetpointTemp = 0xFFFF` and
  ignores temperature writes while in Fan mode.
- **Power and Mode are independent:** changing Mode does not power the unit on.
  To switch a powered-off unit into a mode, write Mode **and** `Power = On`. Note
  the unit may take a moment to power on, so a controller commonly re-asserts
  `Power = On` until a poll confirms it, then stops.

A controller implementation may also choose to serialize writes (one outstanding
write at a time, confirmed by read-back before sending the next) to keep its
local state mirror consistent — this is a controller design choice, not a unit
requirement.

---

## 12. AC-Side (Slave) Behaviour Summary

To emulate the **unit** (useful for a test harness), respond to each command:

| Received `CMD` | Respond with                                                |
|----------------|-------------------------------------------------------------|
| `0x00` (Init1) | `00 00 00 00 01 01 FF FD`                                    |
| `0x01` (Init2) | `01 00 00 00 01 01 FF FC`                                    |
| `0x02` (Write) | apply values, then `02 00 00 00 01 01 FF FB`                |
| `0x03` (Read)  | echo each requested address with its current value (see §6.2): `03 00 00 00 <4N+1> 01 [addr,value]… <CK>` |

For a read response, parse `N = LEN / 2` addresses from the request and emit
`LEN = 4·N + 1` with a leading status byte of `0x01`.

---

## 13. Minimal Re-implementation Checklist

1. Open a UART at **9600 8N1 with TX & RX inverted**.
2. Implement frame TX: `[CMD,0,0,0,LEN, payload…, CK_H, CK_L]` with checksum
   `0xFFFF − Σbytes`.
3. Implement frame RX: complete the frame at `LEN + 7` bytes, verify the
   checksum, and resync on failure (§2.1).
4. Do the Init1 / Init2 handshake; retry on mismatch or timeout (§5).
5. Read the capability registers once (§9.1) to learn which features the unit
   supports.
6. Continuously poll the state registers (§9.2–9.4); decode values per §10 and
   track changes against a local mirror.
7. To change a setting, send a `0x02` write (respecting §11), then read back the
   affected addresses to confirm.

---

## 14. Credits

This protocol description was established through reverse engineering of the
Fujitsu AC serial bus, building on prior community work:

- **[Benas09/FujitsuAC](https://github.com/Benas09/FujitsuAC)** by Benas
  Ragauskas — the primary reference for the UTY-TFSXW1 framing, handshake,
  register map, and value encodings documented here.
- The broader Fujitsu/FGLair reverse-engineering community, whose shared logs and
  discussions ([tested-unit
  reports](https://github.com/Benas09/FujitsuAC/discussions/24) and related
  threads) helped confirm register meanings across models.

Thanks to everyone who captured bus traces and published their findings.

**Scope & caveats:** this covers the **UTY-TFSXW1** protocol variant. Register
names and the "unknown / internal" labels reflect what has been mapped so far;
several `0x12xx`/`0x14xx` registers are exchanged but their meaning is not yet
established, and the J-series variant differs (see §5).

*FGLair® is a trademark of Fujitsu General Limited. This is an independent,
unaffiliated description and is not endorsed by Fujitsu.*
