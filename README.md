# ESPHome Fujitsu AC (UART) Component

An [ESPHome](https://esphome.io) external component that controls Fujitsu air
conditioners over their indoor-unit UART bus (the **UTY-TFSXW1** dongle
protocol), exposing the unit to Home Assistant as a `climate` entity plus
temperature sensors. Built for the **ESP-IDF** framework.

---

## ⚠️ Attribution & Credits — please read first

**This project would not exist without [Benas09/FujitsuAC](https://github.com/Benas09/FujitsuAC)
by Benas Ragauskas.** That project is the original reverse-engineering effort
for the Fujitsu UTY-TFSXW1 serial bus — the framing, two-stage handshake,
register map, and value encodings used here all derive from his work and the
broader Fujitsu/FGLair community that captured and shared bus traces.

This repository is an **independent ESPHome reimplementation** of that protocol.
It is **not** affiliated with, endorsed by, or maintained alongside the original
project. If you want a complete, ready-to-run firmware (with MQTT autodiscovery,
diagnostics, and broad dongle/model support), **use the original project** — it
is more mature and more widely tested than this component.

### 🙏 Please support the original author

If this component is useful to you, the credit and your support belong with
Benas. Please consider:

- ⭐ **Star the original repo:** https://github.com/Benas09/FujitsuAC
- 💚 **Donate (Stripe):** https://donate.stripe.com/8x23cvdbXdht57Zfym1sQ05
- 🔌 **Buy a ready-made dongle (Stripe):** https://buy.stripe.com/6oU28r6Nz3GT8kbbi61sQ06

---

## Features

- **`climate` entity:** power/off, modes (Auto/Cool/Dry/Fan/Heat), fan speed
  (Auto/Quiet/Low/Medium/High), target temperature (16–30 °C, 0.5 °C step),
  vertical/horizontal swing, and current indoor temperature.
- **Presets:** **ECO** (Economy) and **BOOST** (Powerful).
- **`select` entities:** vertical and horizontal vane *position* (Positions 1–6,
  plus Swing and a reported-only Closed state).
- **`switch` entities:** CoilDry, OutdoorLowNoise, MinimumHeat (10 °C),
  EnergySavingFan, and HumanSensor.
- **`sensor` entities:** optional indoor and outdoor temperature.
- **Capability-gated:** the unit reports which features it supports during the
  handshake; climate modes, presets, swing, and each vane's option list are
  enabled accordingly, and writes to an unsupported feature are ignored. The
  optional `switch`/`select` entities are opt-in — just leave out any your model
  doesn't have.
- **Robust I/O:** two-stage handshake, capability discovery, continuous polling,
  and write-then-read-back confirmation — all non-blocking.

Not yet supported: **J-series** units use a different Init2 / register layout.
A handful of `0x12xx`/`0x14xx` registers are exchanged but not yet decoded (see
[docs/PROTOCOL.md](docs/PROTOCOL.md)).

The full bus protocol is documented in [docs/PROTOCOL.md](docs/PROTOCOL.md).

## Wiring

> ⚠️ **Safety:** The AC data/GND pins are **not galvanically isolated** and are
> not referenced to earth ground. **Never** cross-connect the AC GND to a
> PC/laptop GND — you can permanently damage both devices.

See [docs/PROTOCOL.md §1](docs/PROTOCOL.md). Key points:

- 9600 8N1, **TX and RX must be signal-inverted** (`inverted: true` on the pins).
  Without inversion the framing looks like noise.
- Use a 12 V → 5 V DC/DC converter for power and a logic-level shifter on the
  data lines.

> 💡 For detailed wiring guidance — connector pinouts, the parts list (DC/DC
> converter, logic-level shifter, JST/USB connectors), and PCB references — see
> the **[original Benas09/FujitsuAC project](https://github.com/Benas09/FujitsuAC)**,
> which documents the hardware build in much more depth. You can also
> [buy a ready-made dongle](https://buy.stripe.com/6oU28r6Nz3GT8kbbi61sQ06) from
> the author instead of building one.

## Usage

See [example.yaml](example.yaml). Minimal config:

```yaml
external_components:
  - source: github://martinhladil/esphome_fujitsu_ac
    components: [fujitsu_ac]

uart:
  id: ac_uart
  baud_rate: 9600
  tx_pin: { number: GPIO17, inverted: true }
  rx_pin: { number: GPIO16, inverted: true }

climate:
  - platform: fujitsu_ac
    name: "Living Room AC"
    uart_id: ac_uart
    outdoor_temperature: { name: "Outdoor Temperature" }
    indoor_temperature:  { name: "Indoor Temperature" }
```

### Optional entities

Every `switch` and `select` key is optional and only created if you list it. If
your model doesn't support a feature, just omit its key and the entity is never
created. (If you'd rather keep an entity for use in automations but hide it from
the dashboard, set `internal: true` on it — the standard ESPHome entity option.)

Not sure what your unit supports? The component logs a capability summary at
startup (vertical/horizontal swing, economy, powerful, minimum heat, human
sensor, energy-saving fan, outdoor low noise, coil dry, and the vane position
counts). Check the ESPHome logs after boot to see which features your model
reports, then configure only those.

## Development

`example.yaml` pulls the component from GitHub. When working on the component
locally, point `external_components` at the checked-out tree instead:

```yaml
external_components:
  - source: { type: local, path: components }
    components: [fujitsu_ac]
```

Validate / compile:

```sh
esphome compile example.yaml
```

Run the host-side framing/checksum bench test (no hardware, no ESPHome):

```sh
c++ -std=c++17 -I components/fujitsu_ac tests/test_protocol.cpp -o /tmp/t && /tmp/t
```

## Disclaimer

This is an independent, community project provided **as-is**, with no warranty.
It is not affiliated with or endorsed by Fujitsu General Limited. *FGLair® is a
trademark of Fujitsu General Limited.* Use at your own risk — interfacing with
AC mains-powered equipment carries inherent risk to both the hardware and
yourself.
