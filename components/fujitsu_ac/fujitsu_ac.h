#pragma once

#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "esphome/components/climate/climate.h"
#include "esphome/components/uart/uart.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/switch/switch.h"
#include "esphome/components/select/select.h"

#include "protocol.h"

#include <map>
#include <vector>

namespace esphome {
namespace fujitsu_ac {

class FujitsuAC;

// A simple on/off feature register (§10.5) exposed as a switch. Writes go
// through the hub's write queue; the real state is corrected by the read-back.
class FujitsuACSwitch : public switch_::Switch, public Parented<FujitsuAC> {
 public:
  void set_register(uint16_t addr) { this->write_addr_ = addr; }

 protected:
  void write_state(bool state) override;
  uint16_t write_addr_{0};
};

// Vertical / horizontal airflow position (§10.4) exposed as a select.
class FujitsuACVaneSelect : public select::Select, public Parented<FujitsuAC> {
 public:
  // Setting a position is a two-register write (§11): the position setter plus
  // the swing register for the same axis, cleared in the same frame.
  void set_registers(uint16_t setter_addr, uint16_t swing_addr) {
    this->setter_addr_ = setter_addr;
    this->swing_addr_ = swing_addr;
  }

 protected:
  void control(const std::string &value) override;
  uint16_t setter_addr_{0};
  uint16_t swing_addr_{0};
};

// Controller state machine (§8 request sequence)
enum class State : uint8_t {
  HANDSHAKE_INIT1,  // send Init1, wait for echo
  HANDSHAKE_INIT2,  // send Init2, wait for echo
  READ_CAPS,        // batch-read capability registers once
  POLL,             // continuously poll state registers
};

// Timing (§8)
static const uint32_t INTER_REQUEST_MS = 400;
static const uint32_t RESPONSE_TIMEOUT_MS = 200;
static const uint32_t INTER_BYTE_GAP_MS = 20;  // §2.1 frame boundary

class FujitsuAC : public climate::Climate, public uart::UARTDevice, public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  void set_outdoor_temperature_sensor(sensor::Sensor *s) { this->outdoor_temperature_sensor_ = s; }
  void set_indoor_temperature_sensor(sensor::Sensor *s) { this->indoor_temperature_sensor_ = s; }

  // Each child entity maps to a fixed register (protocol.h), so the address is
  // assigned here rather than passed in from the Python codegen.
  void set_coil_dry_switch(FujitsuACSwitch *s) {
    this->coil_dry_switch_ = s;
    s->set_register(REG_COIL_DRY);
  }
  void set_outdoor_low_noise_switch(FujitsuACSwitch *s) {
    this->outdoor_low_noise_switch_ = s;
    s->set_register(REG_OUTDOOR_LOW_NOISE);
  }
  void set_minimum_heat_switch(FujitsuACSwitch *s) {
    this->minimum_heat_switch_ = s;
    s->set_register(REG_MINIMUM_HEAT);
  }
  void set_energy_saving_fan_switch(FujitsuACSwitch *s) {
    this->energy_saving_fan_switch_ = s;
    s->set_register(REG_ENERGY_SAVING_FAN);
  }
  void set_human_sensor_switch(FujitsuACSwitch *s) {
    this->human_sensor_switch_ = s;
    s->set_register(REG_HUMAN_SENSOR);
  }
  void set_vertical_vane_select(FujitsuACVaneSelect *s) {
    this->vertical_vane_select_ = s;
    s->set_registers(REG_VANE_V_SET, REG_SWING_V);
  }
  void set_horizontal_vane_select(FujitsuACVaneSelect *s) {
    this->horizontal_vane_select_ = s;
    s->set_registers(REG_VANE_H_SET, REG_SWING_H);
  }

  // Queue a register write from a child entity (§7). Public so the switch /
  // select child classes can reach it. The group variant writes several
  // registers atomically in one frame (§11, e.g. swing-off + vane position).
  // Returns false if the write was rejected by the coil-dry / minimum-heat
  // interlock, so the caller can skip its optimistic publish.
  bool queue_register_write(uint16_t addr, uint16_t value) { return this->queue_write_(addr, value); }
  bool queue_register_writes(const std::vector<std::pair<uint16_t, uint16_t>> &regs) {
    return this->queue_write_group_(regs);
  }

 protected:
  // climate::Climate
  climate::ClimateTraits traits() override;
  void control(const climate::ClimateCall &call) override;

  // --- TX / request dispatch ---
  void send_frame_(uint8_t cmd, const uint8_t *payload, uint8_t payload_len);
  void send_read_(const uint16_t *addrs, uint8_t count);
  void send_write_(const std::vector<std::pair<uint16_t, uint16_t>> &regs);
  void start_handshake_();
  void send_next_request_();

  // --- RX / parsing (§2.1, §3) ---
  void read_uart_();
  void handle_frame_(const uint8_t *buf, size_t len);
  void on_response_timeout_();

  // --- response handlers ---
  void handle_read_response_(const uint8_t *buf, size_t len);

  // --- capabilities / traits ---
  void build_traits_();
  bool cap_supported_(uint16_t cap_addr) const;
  // Narrow each configured vane select's options to the positions this unit
  // actually supports (per its airflow-count capability register). Entities are
  // opt-in via YAML and can't be removed at runtime (Home Assistant reads the
  // entity list before capabilities are known over UART), so a select for an
  // axis the unit lacks is left in place and its writes simply no-op.
  void configure_vanes_();
  void configure_vane_(FujitsuACVaneSelect *sel, uint16_t count_addr, uint16_t swing_addr);

  // --- state mirror -> climate / sensors ---
  void publish_from_mirror_();
  void publish_switch_(FujitsuACSwitch *sw, uint16_t addr);
  void publish_vane_(FujitsuACVaneSelect *sel, uint16_t addr);
  bool reg_(uint16_t addr, uint16_t &out) const;

  // --- write queue (§7/§11) ---
  // Both return false when the coil-dry / minimum-heat interlock rejects the
  // write; they also drop redundant writes whose value already matches the mirror.
  bool queue_write_(uint16_t addr, uint16_t value);
  bool queue_write_group_(const std::vector<std::pair<uint16_t, uint16_t>> &regs);
  bool mirror_on_(uint16_t addr) const;      // true if the mirrored register reads ON
  bool write_allowed_(uint16_t addr) const;  // coil-dry / minimum-heat interlock

  // --- members ---
  sensor::Sensor *outdoor_temperature_sensor_{nullptr};
  sensor::Sensor *indoor_temperature_sensor_{nullptr};

  // Optional child entities (§9.3 feature registers, §10.4 airflow position).
  FujitsuACSwitch *coil_dry_switch_{nullptr};
  FujitsuACSwitch *outdoor_low_noise_switch_{nullptr};
  FujitsuACSwitch *minimum_heat_switch_{nullptr};
  FujitsuACSwitch *energy_saving_fan_switch_{nullptr};
  FujitsuACSwitch *human_sensor_switch_{nullptr};
  FujitsuACVaneSelect *vertical_vane_select_{nullptr};
  FujitsuACVaneSelect *horizontal_vane_select_{nullptr};

  State state_{State::HANDSHAKE_INIT1};
  bool waiting_for_response_{false};
  uint8_t pending_cmd_{0};            // CMD we expect the unit to echo
  uint32_t last_request_time_{0};
  uint32_t last_byte_time_{0};

  // RX accumulation buffer (§2.1)
  uint8_t rx_buf_[MAX_FRAME_LEN];
  size_t rx_index_{0};

  // Register mirror and capability map
  std::map<uint16_t, uint16_t> regs_;
  std::map<uint16_t, uint16_t> caps_;
  bool caps_read_{false};

  // Index into the capability / state batch tables
  uint8_t cap_batch_idx_{0};
  uint8_t state_batch_idx_{0};

  // Pending writes and the addresses to read back after a write (§7). Each
  // queue entry is one frame: usually a single register, but airflow position
  // is a multi-register frame (§11).
  std::vector<std::vector<std::pair<uint16_t, uint16_t>>> write_queue_;
  std::vector<uint16_t> readback_addrs_;
  bool awaiting_readback_{false};

  // Traits are rebuilt once capabilities are known
  climate::ClimateTraits cached_traits_;
  bool traits_ready_{false};
};

}  // namespace fujitsu_ac
}  // namespace esphome
