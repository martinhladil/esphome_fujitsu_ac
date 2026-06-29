#include "fujitsu_ac.h"
#include "esphome/core/log.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace esphome {
namespace fujitsu_ac {

static const char *const TAG = "fujitsu_ac";

// Fixed handshake frames (§5). Stored verbatim including checksum.
static const uint8_t INIT1_REQUEST[] = {0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFB};
static const uint8_t INIT1_RESPONSE[] = {0x00, 0x00, 0x00, 0x00, 0x01, 0x01, 0xFF, 0xFD};
static const uint8_t INIT2_REQUEST[] = {0x01, 0x00, 0x00, 0x00, 0x04, 0x00, 0x04, 0x00, 0x01, 0xFF, 0xF5};
static const uint8_t INIT2_RESPONSE[] = {0x01, 0x00, 0x00, 0x00, 0x01, 0x01, 0xFF, 0xFC};

// Capability registers (§9.1). Read once after the handshake; limited to the
// flags the component acts on (mirrors the CapRegister enum in protocol.h).
static const uint16_t CAPS_BATCH[] = {
    CAP_VERTICAL_AIRFLOW_COUNT, CAP_VERTICAL_SWING, CAP_HORIZONTAL_AIRFLOW_COUNT,
    CAP_HORIZONTAL_SWING, CAP_ECONOMY, CAP_MINIMUM_HEAT, CAP_HUMAN_SENSOR,
    CAP_ENERGY_SAVING_FAN, CAP_POWERFUL, CAP_OUTDOOR_LOW_NOISE, CAP_COIL_DRY};

// State registers (§9.2–9.4). Polled continuously; limited to the registers the
// component decodes and publishes (the undecoded 0x12xx/0x14xx range is skipped).
static const uint16_t STATE_BATCH_A[] = {REG_POWER, REG_MODE,    REG_SETPOINT, REG_FAN,       REG_SWING_V,
                                         REG_SWING_H, REG_VANE_V, REG_VANE_H,   REG_INDOOR_TEMP};
static const uint16_t STATE_BATCH_B[] = {REG_ECONOMY,           REG_MINIMUM_HEAT,      REG_HUMAN_SENSOR,
                                         REG_ENERGY_SAVING_FAN, REG_POWERFUL,          REG_OUTDOOR_LOW_NOISE,
                                         REG_COIL_DRY,          REG_OUTDOOR_TEMP};

template<typename T, size_t N> static constexpr size_t array_len(const T (&)[N]) { return N; }

struct Batch {
  const uint16_t *addrs;
  size_t count;
};

static const Batch CAP_BATCHES[] = {
    {CAPS_BATCH, array_len(CAPS_BATCH)},
};
static const Batch STATE_BATCHES[] = {
    {STATE_BATCH_A, array_len(STATE_BATCH_A)},
    {STATE_BATCH_B, array_len(STATE_BATCH_B)},
};

// Airflow position labels (§10.4). Indices align with VANE_CODES.
// "Closed" (0x0000) is the value the unit reports while powered off (the louver
// physically closes); it is a reported-only state, not a settable position.
static const uint16_t VANE_CLOSED_CODE = 0x0000;
static const char *const VANE_OPTIONS[] = {"Position 1", "Position 2", "Position 3", "Position 4",
                                           "Position 5", "Position 6", "Swing", "Closed"};
static const uint16_t VANE_CODES[] = {0x0001, 0x0002, 0x0003, 0x0004,
                                      0x0005, 0x0006, 0x0020, VANE_CLOSED_CODE};
// Indices into VANE_OPTIONS for the non-positional entries, and the protocol's
// hard cap on positional codes (§10.4: 0x01–0x06).
static const uint8_t VANE_MAX_POSITIONS = 6;
static const size_t VANE_SWING_INDEX = 6;
static const size_t VANE_CLOSED_INDEX = 7;

static optional<uint16_t> vane_option_to_code(const std::string &opt) {
  for (size_t i = 0; i < array_len(VANE_OPTIONS); i++)
    if (opt == VANE_OPTIONS[i])
      return VANE_CODES[i];
  return {};
}

static const char *vane_code_to_option(uint16_t code) {
  for (size_t i = 0; i < array_len(VANE_OPTIONS); i++)
    if (code == VANE_CODES[i])
      return VANE_OPTIONS[i];
  return nullptr;
}

// ---------------------------------------------------------------------------
// Component lifecycle
// ---------------------------------------------------------------------------
void FujitsuAC::setup() {
  // Conservative defaults until the real capabilities arrive.
  this->mode = climate::CLIMATE_MODE_OFF;
  this->build_traits_();
  this->start_handshake_();
}

void FujitsuAC::start_handshake_() {
  ESP_LOGD(TAG, "Starting handshake");
  this->state_ = State::HANDSHAKE_INIT1;
  this->waiting_for_response_ = false;
  this->awaiting_readback_ = false;
  this->rx_index_ = 0;
  this->cap_batch_idx_ = 0;
  this->state_batch_idx_ = 0;
  // Send the first request immediately.
  this->last_request_time_ = millis() - INTER_REQUEST_MS;
}

void FujitsuAC::loop() {
  this->read_uart_();

  const uint32_t now = millis();

  if (this->waiting_for_response_) {
    if (now - this->last_request_time_ >= RESPONSE_TIMEOUT_MS)
      this->on_response_timeout_();
    return;
  }

  if (now - this->last_request_time_ >= INTER_REQUEST_MS)
    this->send_next_request_();
}

// ---------------------------------------------------------------------------
// RX path (§2.1, §3)
// ---------------------------------------------------------------------------
void FujitsuAC::read_uart_() {
  const uint32_t now = millis();
  while (this->available()) {
    // Inter-byte gap reset: a quiet period marks a frame boundary (§2.1).
    if (this->rx_index_ > 0 && (now - this->last_byte_time_) >= INTER_BYTE_GAP_MS) {
      ESP_LOGV(TAG, "Inter-byte gap, discarding %u buffered bytes", (unsigned) this->rx_index_);
      this->rx_index_ = 0;
    }
    this->last_byte_time_ = now;

    uint8_t b;
    if (!this->read_byte(&b))
      break;

    if (this->rx_index_ >= MAX_FRAME_LEN) {
      // Bogus LEN or runaway buffer: drop the oldest byte and keep scanning.
      memmove(this->rx_buf_, this->rx_buf_ + 1, MAX_FRAME_LEN - 1);
      this->rx_index_ = MAX_FRAME_LEN - 1;
    }
    this->rx_buf_[this->rx_index_++] = b;

    // Frame complete once we have LEN+7 bytes (§2.1).
    while (this->rx_index_ > LEN_OFFSET) {
      size_t expected = size_t(this->rx_buf_[LEN_OFFSET]) + FRAME_OVERHEAD;
      if (expected > MAX_FRAME_LEN) {
        // Impossible length -> slide one byte and re-scan.
        memmove(this->rx_buf_, this->rx_buf_ + 1, --this->rx_index_);
        continue;
      }
      if (this->rx_index_ < expected)
        break;  // need more bytes

      if (frame_valid(this->rx_buf_, expected)) {
        this->handle_frame_(this->rx_buf_, expected);
        // Drop the consumed frame, keep any trailing bytes.
        size_t remaining = this->rx_index_ - expected;
        if (remaining > 0)
          memmove(this->rx_buf_, this->rx_buf_ + expected, remaining);
        this->rx_index_ = remaining;
      } else {
        // Checksum failure -> slide one byte and re-scan (§2.1 resync).
        memmove(this->rx_buf_, this->rx_buf_ + 1, --this->rx_index_);
      }
    }
  }
}

void FujitsuAC::handle_frame_(const uint8_t *buf, size_t len) {
  const uint8_t cmd = buf[0];

  // Transient "restarting" frames during init (§5) — ignore.
  if ((buf[0] == 0xFE || buf[0] == 0xFC) && this->state_ <= State::HANDSHAKE_INIT2) {
    ESP_LOGV(TAG, "Ignoring transient restart frame 0x%02X", buf[0]);
    return;
  }

  if (!this->waiting_for_response_ || cmd != this->pending_cmd_) {
    ESP_LOGV(TAG, "Unexpected frame CMD=0x%02X (waiting=%d, pending=0x%02X)", cmd,
             this->waiting_for_response_, this->pending_cmd_);
    return;
  }

  this->waiting_for_response_ = false;

  switch (this->state_) {
    case State::HANDSHAKE_INIT1:
      if (len == sizeof(INIT1_RESPONSE) && memcmp(buf, INIT1_RESPONSE, len) == 0) {
        ESP_LOGD(TAG, "Init1 OK");
        this->state_ = State::HANDSHAKE_INIT2;
      } else {
        ESP_LOGW(TAG, "Init1 response mismatch, restarting handshake");
        this->start_handshake_();
      }
      break;

    case State::HANDSHAKE_INIT2:
      if (len == sizeof(INIT2_RESPONSE) && memcmp(buf, INIT2_RESPONSE, len) == 0) {
        ESP_LOGD(TAG, "Init2 OK, link established");
        this->state_ = State::READ_CAPS;
        this->cap_batch_idx_ = 0;
      } else {
        ESP_LOGW(TAG, "Init2 response mismatch, restarting handshake");
        this->start_handshake_();
      }
      break;

    case State::READ_CAPS:
      if (cmd == CMD_READ && buf[STATUS_OFFSET] == STATUS_OK) {
        this->handle_read_response_(buf, len);
        this->cap_batch_idx_++;
        if (this->cap_batch_idx_ >= array_len(CAP_BATCHES)) {
          this->caps_ = this->regs_;  // snapshot capability values
          this->caps_read_ = true;
          this->build_traits_();
          this->apply_capability_visibility_();
          ESP_LOGD(TAG, "Capabilities read, entering POLL");
          this->state_ = State::POLL;
          this->state_batch_idx_ = 0;
        }
      }
      break;

    case State::POLL:
      if (cmd == CMD_READ && buf[STATUS_OFFSET] == STATUS_OK) {
        this->handle_read_response_(buf, len);
        if (this->awaiting_readback_) {
          // Read-back after a write completed.
          this->awaiting_readback_ = false;
        } else {
          this->state_batch_idx_ = (this->state_batch_idx_ + 1) % array_len(STATE_BATCHES);
        }
        this->publish_from_mirror_();
      } else if (cmd == CMD_WRITE) {
        if (buf[STATUS_OFFSET] == STATUS_OK)
          ESP_LOGD(TAG, "Write acknowledged");
        else
          ESP_LOGW(TAG, "Write rejected (status 0x%02X)", buf[STATUS_OFFSET]);
        // A read-back of the written addresses follows on the next request.
      }
      break;
  }
}

void FujitsuAC::handle_read_response_(const uint8_t *buf, size_t len) {
  // §6.2: payload = status byte + N×[addrH, addrL, valH, valL]
  uint8_t payload_len = buf[LEN_OFFSET];
  int count = payload_len / 4;  // integer division drops the leading status byte
  for (int i = 0; i < count; i++) {
    size_t idx = STATUS_OFFSET + 1 + i * 4;
    uint16_t addr = (uint16_t(buf[idx]) << 8) | buf[idx + 1];
    uint16_t value = (uint16_t(buf[idx + 2]) << 8) | buf[idx + 3];
    this->regs_[addr] = value;
  }
}

void FujitsuAC::on_response_timeout_() {
  this->waiting_for_response_ = false;
  if (this->state_ == State::POLL) {
    ESP_LOGW(TAG, "No response from unit");
    // Surface "no response" without tearing down the link; retry next cycle.
  } else {
    ESP_LOGW(TAG, "Handshake timeout, restarting");
    this->start_handshake_();
  }
}

// ---------------------------------------------------------------------------
// TX path (§8 sequence)
// ---------------------------------------------------------------------------
void FujitsuAC::send_frame_(uint8_t cmd, const uint8_t *payload, uint8_t payload_len) {
  uint8_t frame[MAX_FRAME_LEN];
  size_t len = build_frame(cmd, payload, payload_len, frame);
  this->write_array(frame, len);
  this->pending_cmd_ = cmd;
  this->waiting_for_response_ = true;
  this->last_request_time_ = millis();
}

void FujitsuAC::send_read_(const uint16_t *addrs, uint8_t count) {
  uint8_t payload[MAX_FRAME_LEN];
  for (uint8_t i = 0; i < count; i++) {
    payload[i * 2] = (addrs[i] >> 8) & 0xFF;
    payload[i * 2 + 1] = addrs[i] & 0xFF;
  }
  this->send_frame_(CMD_READ, payload, count * 2);
}

void FujitsuAC::send_write_(const std::vector<std::pair<uint16_t, uint16_t>> &regs) {
  uint8_t payload[MAX_FRAME_LEN];
  uint8_t n = 0;
  for (const auto &kv : regs) {
    payload[n * 4] = (kv.first >> 8) & 0xFF;
    payload[n * 4 + 1] = kv.first & 0xFF;
    payload[n * 4 + 2] = (kv.second >> 8) & 0xFF;
    payload[n * 4 + 3] = kv.second & 0xFF;
    n++;
  }
  this->send_frame_(CMD_WRITE, payload, n * 4);
}

void FujitsuAC::send_next_request_() {
  switch (this->state_) {
    case State::HANDSHAKE_INIT1:
      ESP_LOGV(TAG, "-> Init1");
      this->send_frame_(CMD_INIT1, &INIT1_REQUEST[FRAME_HEADER_LEN], INIT1_REQUEST[LEN_OFFSET]);
      break;

    case State::HANDSHAKE_INIT2:
      ESP_LOGV(TAG, "-> Init2");
      this->send_frame_(CMD_INIT2, &INIT2_REQUEST[FRAME_HEADER_LEN], INIT2_REQUEST[LEN_OFFSET]);
      break;

    case State::READ_CAPS: {
      const Batch &b = CAP_BATCHES[this->cap_batch_idx_];
      this->send_read_(b.addrs, b.count);
      break;
    }

    case State::POLL:
      // A pending write takes priority; then read back the written addresses.
      if (!this->write_queue_.empty()) {
        // Each queue entry is one frame: a single register (§7), or the
        // multi-register airflow-position frame (§11).
        std::vector<std::pair<uint16_t, uint16_t>> group = this->write_queue_.front();
        this->write_queue_.erase(this->write_queue_.begin());
        this->readback_addrs_.clear();
        for (const auto &kv : group)
          this->readback_addrs_.push_back(kv.first);
        if (group.size() == 1)
          ESP_LOGD(TAG, "-> Write reg 0x%04X = 0x%04X", group[0].first, group[0].second);
        else
          ESP_LOGD(TAG, "-> Write %u regs (0x%04X = 0x%04X, ...)", (unsigned) group.size(),
                   group[0].first, group[0].second);
        this->send_write_(group);
      } else if (!this->readback_addrs_.empty()) {
        this->awaiting_readback_ = true;
        this->send_read_(this->readback_addrs_.data(), this->readback_addrs_.size());
        this->readback_addrs_.clear();
      } else {
        const Batch &b = STATE_BATCHES[this->state_batch_idx_];
        this->send_read_(b.addrs, b.count);
      }
      break;
  }
}

// ---------------------------------------------------------------------------
// Capabilities / traits
// ---------------------------------------------------------------------------
bool FujitsuAC::cap_supported_(uint16_t cap_addr) const {
  auto it = this->caps_.find(cap_addr);
  return it != this->caps_.end() && it->second != 0;
}

void FujitsuAC::gate_entity_(EntityBase *ent, uint16_t cap_addr) {
  if (ent == nullptr || this->cap_supported_(cap_addr))
    return;
  ESP_LOGD(TAG, "Hiding unsupported entity '%s'", ent->get_name().c_str());
  ent->set_internal(true);
}

void FujitsuAC::configure_vane_(FujitsuACVaneSelect *sel, uint16_t count_addr, uint16_t swing_addr) {
  if (sel == nullptr)
    return;
  auto it = this->caps_.find(count_addr);
  uint16_t raw = (it == this->caps_.end()) ? 0 : it->second;
  if (raw == 0) {  // unit reports no airflow control on this axis
    ESP_LOGD(TAG, "Hiding unsupported entity '%s'", sel->get_name().c_str());
    sel->set_internal(true);
    return;
  }
  // The count register has been observed as a literal position count (e.g. 0x04
  // on a 4-position vertical louver). Treat it as such but clamp to the protocol
  // range 1..6 (§10.4); some units report larger, undecoded values (§9.1).
  uint8_t positions = raw > VANE_MAX_POSITIONS ? VANE_MAX_POSITIONS : static_cast<uint8_t>(raw);
  bool swing = this->cap_supported_(swing_addr);

  FixedVector<const char *> options;
  options.init(positions + (swing ? 1u : 0u) + 1u);  // positions + optional Swing + Closed
  for (uint8_t i = 0; i < positions; i++)
    options.push_back(VANE_OPTIONS[i]);
  if (swing)
    options.push_back(VANE_OPTIONS[VANE_SWING_INDEX]);
  options.push_back(VANE_OPTIONS[VANE_CLOSED_INDEX]);
  sel->traits.set_options(options);
  ESP_LOGD(TAG, "Vane '%s': %u positions%s", sel->get_name().c_str(), positions,
           swing ? " + swing" : "");
}

void FujitsuAC::apply_capability_visibility_() {
  this->gate_entity_(this->coil_dry_switch_, CAP_COIL_DRY);
  this->gate_entity_(this->outdoor_low_noise_switch_, CAP_OUTDOOR_LOW_NOISE);
  this->gate_entity_(this->minimum_heat_switch_, CAP_MINIMUM_HEAT);
  this->gate_entity_(this->energy_saving_fan_switch_, CAP_ENERGY_SAVING_FAN);
  this->gate_entity_(this->human_sensor_switch_, CAP_HUMAN_SENSOR);
  this->configure_vane_(this->vertical_vane_select_, CAP_VERTICAL_AIRFLOW_COUNT, CAP_VERTICAL_SWING);
  this->configure_vane_(this->horizontal_vane_select_, CAP_HORIZONTAL_AIRFLOW_COUNT, CAP_HORIZONTAL_SWING);
}

void FujitsuAC::build_traits_() {
  climate::ClimateTraits t;
  t.add_feature_flags(climate::CLIMATE_SUPPORTS_CURRENT_TEMPERATURE);
  t.set_visual_min_temperature(16.0f);
  t.set_visual_max_temperature(30.0f);
  t.set_visual_temperature_step(0.5f);

  t.set_supported_modes({
      climate::CLIMATE_MODE_OFF,
      climate::CLIMATE_MODE_HEAT_COOL,  // Auto
      climate::CLIMATE_MODE_COOL,
      climate::CLIMATE_MODE_DRY,
      climate::CLIMATE_MODE_FAN_ONLY,
      climate::CLIMATE_MODE_HEAT,
  });

  t.set_supported_fan_modes({
      climate::CLIMATE_FAN_AUTO,
      climate::CLIMATE_FAN_QUIET,
      climate::CLIMATE_FAN_LOW,
      climate::CLIMATE_FAN_MEDIUM,
      climate::CLIMATE_FAN_HIGH,
  });

  // Swing modes gated by capability (default to allowing both before caps arrive).
  bool sv = !this->caps_read_ || this->cap_supported_(CAP_VERTICAL_SWING);
  bool sh = !this->caps_read_ || this->cap_supported_(CAP_HORIZONTAL_SWING);
  climate::ClimateSwingModeMask swing;
  swing.insert(climate::CLIMATE_SWING_OFF);
  if (sv)
    swing.insert(climate::CLIMATE_SWING_VERTICAL);
  if (sh)
    swing.insert(climate::CLIMATE_SWING_HORIZONTAL);
  if (sv && sh)
    swing.insert(climate::CLIMATE_SWING_BOTH);
  t.set_supported_swing_modes(swing);

  // Presets gated by capability.
  climate::ClimatePresetMask presets;
  presets.insert(climate::CLIMATE_PRESET_NONE);
  if (!this->caps_read_ || this->cap_supported_(CAP_ECONOMY))
    presets.insert(climate::CLIMATE_PRESET_ECO);
  if (!this->caps_read_ || this->cap_supported_(CAP_POWERFUL))
    presets.insert(climate::CLIMATE_PRESET_BOOST);
  t.set_supported_presets(presets);

  this->cached_traits_ = t;
  this->traits_ready_ = true;
}

climate::ClimateTraits FujitsuAC::traits() {
  if (this->traits_ready_)
    return this->cached_traits_;
  this->build_traits_();
  return this->cached_traits_;
}

// ---------------------------------------------------------------------------
// State mirror -> climate / sensors (§10)
// ---------------------------------------------------------------------------
bool FujitsuAC::reg_(uint16_t addr, uint16_t &out) const {
  auto it = this->regs_.find(addr);
  if (it == this->regs_.end())
    return false;
  out = it->second;
  return true;
}

void FujitsuAC::publish_from_mirror_() {
  uint16_t v;

  // Snapshot the published fields so we can skip a redundant publish_state()
  // (and its full state log + API push) when nothing actually changed.
  const climate::ClimateMode prev_mode = this->mode;
  const climate::ClimateFanMode prev_fan = this->fan_mode.value_or(climate::CLIMATE_FAN_AUTO);
  const float prev_target = this->target_temperature;
  const float prev_current = this->current_temperature;
  const climate::ClimateSwingMode prev_swing = this->swing_mode;
  const climate::ClimatePreset prev_preset = this->preset.value_or(climate::CLIMATE_PRESET_NONE);

  // Mode / Power (§10.1, §10.2). OFF when power is off.
  uint16_t power = ON_VALUE;
  bool have_power = this->reg_(REG_POWER, power);
  if (have_power && power == OFF_VALUE) {
    this->mode = climate::CLIMATE_MODE_OFF;
  } else if (this->reg_(REG_MODE, v)) {
    switch (v) {
      case MODE_AUTO: this->mode = climate::CLIMATE_MODE_HEAT_COOL; break;
      case MODE_COOL: this->mode = climate::CLIMATE_MODE_COOL; break;
      case MODE_DRY: this->mode = climate::CLIMATE_MODE_DRY; break;
      case MODE_FAN: this->mode = climate::CLIMATE_MODE_FAN_ONLY; break;
      case MODE_HEAT: this->mode = climate::CLIMATE_MODE_HEAT; break;
      default: break;  // 0x05 / unknown — leave as-is (§10.2)
    }
  }

  // Fan speed (§10.3)
  if (this->reg_(REG_FAN, v)) {
    switch (v) {
      case FAN_AUTO: this->fan_mode = climate::CLIMATE_FAN_AUTO; break;
      case FAN_QUIET: this->fan_mode = climate::CLIMATE_FAN_QUIET; break;
      case FAN_LOW: this->fan_mode = climate::CLIMATE_FAN_LOW; break;
      case FAN_MEDIUM: this->fan_mode = climate::CLIMATE_FAN_MEDIUM; break;
      case FAN_HIGH: this->fan_mode = climate::CLIMATE_FAN_HIGH; break;
      default: break;
    }
  }

  // Setpoint (§10.6). 0xFFFF in Fan mode -> no target.
  if (this->reg_(REG_SETPOINT, v) && v != SETPOINT_FAN_MODE)
    this->target_temperature = decode_setpoint(v);

  // Swing (§10.5)
  uint16_t sv = OFF_VALUE, sh = OFF_VALUE;
  bool have_sv = this->reg_(REG_SWING_V, sv);
  bool have_sh = this->reg_(REG_SWING_H, sh);
  if (have_sv || have_sh) {
    bool v_on = have_sv && sv == ON_VALUE;
    bool h_on = have_sh && sh == ON_VALUE;
    if (v_on && h_on)
      this->swing_mode = climate::CLIMATE_SWING_BOTH;
    else if (v_on)
      this->swing_mode = climate::CLIMATE_SWING_VERTICAL;
    else if (h_on)
      this->swing_mode = climate::CLIMATE_SWING_HORIZONTAL;
    else
      this->swing_mode = climate::CLIMATE_SWING_OFF;
  }

  // Presets: Economy -> ECO, Powerful -> BOOST (§9.3)
  uint16_t eco = OFF_VALUE, boost = OFF_VALUE;
  this->reg_(REG_ECONOMY, eco);
  this->reg_(REG_POWERFUL, boost);
  if (boost == ON_VALUE)
    this->preset = climate::CLIMATE_PRESET_BOOST;
  else if (eco == ON_VALUE)
    this->preset = climate::CLIMATE_PRESET_ECO;
  else
    this->preset = climate::CLIMATE_PRESET_NONE;

  // Indoor temperature (§10.7) -> current_temperature + optional sensor
  if (this->reg_(REG_INDOOR_TEMP, v)) {
    float t = decode_temp_offset(v, false);
    this->current_temperature = t;
    if (this->indoor_temperature_sensor_ != nullptr &&
        (!this->indoor_temperature_sensor_->has_state() ||
         std::abs(this->indoor_temperature_sensor_->get_raw_state() - t) >= 0.01f))
      this->indoor_temperature_sensor_->publish_state(t);
  }

  // Outdoor temperature (§10.8, signed)
  if (this->outdoor_temperature_sensor_ != nullptr && this->reg_(REG_OUTDOOR_TEMP, v)) {
    float t = decode_temp_offset(v, true);
    if (!this->outdoor_temperature_sensor_->has_state() ||
        std::abs(this->outdoor_temperature_sensor_->get_raw_state() - t) >= 0.01f)
      this->outdoor_temperature_sensor_->publish_state(t);
  }

  // Optional child entities (§9.3 feature registers, §10.4 airflow position).
  this->publish_switch_(this->coil_dry_switch_, REG_COIL_DRY);
  this->publish_switch_(this->outdoor_low_noise_switch_, REG_OUTDOOR_LOW_NOISE);
  this->publish_switch_(this->minimum_heat_switch_, REG_MINIMUM_HEAT);
  this->publish_switch_(this->energy_saving_fan_switch_, REG_ENERGY_SAVING_FAN);
  this->publish_switch_(this->human_sensor_switch_, REG_HUMAN_SENSOR);
  this->publish_vane_(this->vertical_vane_select_, REG_VANE_V);
  this->publish_vane_(this->horizontal_vane_select_, REG_VANE_H);

  // Only publish when a visible field changed: the unit is polled ~2-3×/s, and
  // publish_state() always emits a full state log and pushes to the API.
  // NaN-safe compare: target/current temperature are NaN when absent (e.g. no
  // setpoint while off), and NaN != NaN would otherwise force a publish every poll.
  auto temp_changed = [](float a, float b) {
    return std::isnan(a) != std::isnan(b) || (!std::isnan(a) && a != b);
  };
  const bool changed = this->mode != prev_mode ||
                       this->fan_mode.value_or(climate::CLIMATE_FAN_AUTO) != prev_fan ||
                       temp_changed(this->target_temperature, prev_target) ||
                       temp_changed(this->current_temperature, prev_current) ||
                       this->swing_mode != prev_swing ||
                       this->preset.value_or(climate::CLIMATE_PRESET_NONE) != prev_preset;
  if (changed)
    this->publish_state();
}

void FujitsuAC::publish_switch_(FujitsuACSwitch *sw, uint16_t addr) {
  uint16_t v;
  if (sw == nullptr || !this->reg_(addr, v))
    return;
  bool on = v == ON_VALUE;
  if (sw->state != on)
    sw->publish_state(on);
}

void FujitsuAC::publish_vane_(FujitsuACVaneSelect *sel, uint16_t addr) {
  uint16_t v;
  if (sel == nullptr || !this->reg_(addr, v))
    return;
  const char *opt = vane_code_to_option(v);
  if (opt == nullptr)
    return;  // unknown/transient position code — leave the last value
  if (sel->state != opt)
    sel->publish_state(opt);
}

// ---------------------------------------------------------------------------
// Child entities (switches / vane selects)
// ---------------------------------------------------------------------------
void FujitsuACSwitch::write_state(bool state) {
  // Publish optimistically only if the write was accepted; if the interlock
  // rejected it, leave the entity as-is (the next poll re-affirms the real value).
  if (this->parent_->queue_register_write(this->write_addr_, state ? ON_VALUE : OFF_VALUE))
    this->publish_state(state);  // optimistic; the read-back confirms the real value
}

void FujitsuACVaneSelect::control(const std::string &value) {
  auto code = vane_option_to_code(value);
  if (!code.has_value())
    return;
  if (*code == VANE_CLOSED_CODE)
    return;  // "Closed" is a reported-only off state, not a settable position
  // Airflow position is a two-register write (§11): clear this axis's Swing and
  // set the position in one frame, matching the unit's expected sequence.
  if (this->parent_->queue_register_writes({{this->swing_addr_, OFF_VALUE}, {this->setter_addr_, *code}}))
    this->publish_state(value);  // optimistic; the read-back confirms
}

// ---------------------------------------------------------------------------
// Control -> write queue (§7/§11)
// ---------------------------------------------------------------------------
bool FujitsuAC::mirror_on_(uint16_t addr) const {
  uint16_t v;
  return this->reg_(addr, v) && v == ON_VALUE;
}

// Coil-dry and minimum-heat are special cycles; while either is active the unit
// should not be sent comfort-setting writes. Mirrors the per-setter guards in
// the reference firmware: coil-dry blocks mode/fan/temp/preset/swing/airflow;
// minimum-heat blocks mode/fan/temp/preset/energy-saving-fan. Power and the
// feature toggles themselves are always allowed.
bool FujitsuAC::write_allowed_(uint16_t addr) const {
  const bool coil_dry = this->mirror_on_(REG_COIL_DRY);
  const bool minimum_heat = this->mirror_on_(REG_MINIMUM_HEAT);
  if (!coil_dry && !minimum_heat)
    return true;
  switch (addr) {
    case REG_MODE:
    case REG_FAN:
    case REG_SETPOINT:
    case REG_ECONOMY:
    case REG_POWERFUL:
      return false;  // blocked by either cycle
    case REG_SWING_V:
    case REG_SWING_H:
    case REG_VANE_V_SET:
    case REG_VANE_H_SET:
      return !coil_dry;  // coil-dry only
    case REG_ENERGY_SAVING_FAN:
      return !minimum_heat;  // minimum-heat only
    default:
      return true;  // power, coil-dry, minimum-heat, low-noise, human sensor
  }
}

bool FujitsuAC::queue_write_(uint16_t addr, uint16_t value) {
  if (!this->write_allowed_(addr)) {
    ESP_LOGD(TAG, "Skipping write 0x%04X = 0x%04X (coil-dry / minimum-heat active)", addr, value);
    return false;
  }
  uint16_t cur;
  if (this->reg_(addr, cur) && cur == value)
    return true;  // already at the requested value — accepted, but nothing to send
  this->write_queue_.push_back({{addr, value}});
  return true;
}

bool FujitsuAC::queue_write_group_(const std::vector<std::pair<uint16_t, uint16_t>> &regs) {
  bool all_unchanged = true;
  for (const auto &kv : regs) {
    if (!this->write_allowed_(kv.first)) {
      ESP_LOGD(TAG, "Skipping grouped write 0x%04X (coil-dry / minimum-heat active)", kv.first);
      return false;  // the frame is atomic (§11) — drop it entirely if any member is blocked
    }
    uint16_t cur;
    if (!this->reg_(kv.first, cur) || cur != kv.second)
      all_unchanged = false;
  }
  if (all_unchanged)
    return true;  // every register already at target — accepted, nothing to send
  this->write_queue_.push_back(regs);
  return true;
}

void FujitsuAC::control(const climate::ClimateCall &call) {
  if (call.get_mode().has_value()) {
    climate::ClimateMode m = *call.get_mode();
    if (m == climate::CLIMATE_MODE_OFF) {
      this->queue_write_(REG_POWER, OFF_VALUE);
    } else {
      uint16_t mode_val = MODE_AUTO;
      switch (m) {
        case climate::CLIMATE_MODE_HEAT_COOL: mode_val = MODE_AUTO; break;
        case climate::CLIMATE_MODE_COOL: mode_val = MODE_COOL; break;
        case climate::CLIMATE_MODE_DRY: mode_val = MODE_DRY; break;
        case climate::CLIMATE_MODE_FAN_ONLY: mode_val = MODE_FAN; break;
        case climate::CLIMATE_MODE_HEAT: mode_val = MODE_HEAT; break;
        default: break;
      }
      // Mode and Power are independent (§11): set the mode and power the unit on.
      // queue_write_ drops the Power=On write when the unit is already running, so
      // a mode change on a powered-on unit doesn't re-issue a redundant power-on.
      this->queue_write_(REG_MODE, mode_val);
      this->queue_write_(REG_POWER, ON_VALUE);
    }
  }

  if (call.get_target_temperature().has_value()) {
    // No setpoint in Fan mode (§11). Use the requested or current mode.
    climate::ClimateMode effective = call.get_mode().value_or(this->mode);
    if (effective != climate::CLIMATE_MODE_FAN_ONLY) {
      float req = *call.get_target_temperature();
      float lo = (effective == climate::CLIMATE_MODE_HEAT) ? 16.0f : 18.0f;
      req = std::max(lo, std::min(30.0f, req));
      this->queue_write_(REG_SETPOINT, encode_setpoint(req));
    }
  }

  if (call.get_fan_mode().has_value()) {
    uint16_t fan_val = FAN_AUTO;
    switch (*call.get_fan_mode()) {
      case climate::CLIMATE_FAN_AUTO: fan_val = FAN_AUTO; break;
      case climate::CLIMATE_FAN_QUIET: fan_val = FAN_QUIET; break;
      case climate::CLIMATE_FAN_LOW: fan_val = FAN_LOW; break;
      case climate::CLIMATE_FAN_MEDIUM: fan_val = FAN_MEDIUM; break;
      case climate::CLIMATE_FAN_HIGH: fan_val = FAN_HIGH; break;
      default: break;
    }
    this->queue_write_(REG_FAN, fan_val);
  }

  if (call.get_swing_mode().has_value()) {
    climate::ClimateSwingMode s = *call.get_swing_mode();
    bool v_on = s == climate::CLIMATE_SWING_VERTICAL || s == climate::CLIMATE_SWING_BOTH;
    bool h_on = s == climate::CLIMATE_SWING_HORIZONTAL || s == climate::CLIMATE_SWING_BOTH;
    if (!this->caps_read_ || this->cap_supported_(CAP_VERTICAL_SWING))
      this->queue_write_(REG_SWING_V, v_on ? ON_VALUE : OFF_VALUE);
    if (!this->caps_read_ || this->cap_supported_(CAP_HORIZONTAL_SWING))
      this->queue_write_(REG_SWING_H, h_on ? ON_VALUE : OFF_VALUE);
  }

  if (call.get_preset().has_value()) {
    climate::ClimatePreset p = *call.get_preset();
    bool eco = p == climate::CLIMATE_PRESET_ECO;
    bool boost = p == climate::CLIMATE_PRESET_BOOST;
    if (!this->caps_read_ || this->cap_supported_(CAP_ECONOMY))
      this->queue_write_(REG_ECONOMY, eco ? ON_VALUE : OFF_VALUE);
    if (!this->caps_read_ || this->cap_supported_(CAP_POWERFUL))
      this->queue_write_(REG_POWERFUL, boost ? ON_VALUE : OFF_VALUE);
  }
}

// ---------------------------------------------------------------------------
void FujitsuAC::dump_config() {
  ESP_LOGCONFIG(TAG, "Fujitsu AC (UART):");
  this->check_uart_settings(9600);
  ESP_LOGCONFIG(TAG, "  Capabilities read: %s", YESNO(this->caps_read_));
  if (this->caps_read_) {
    ESP_LOGCONFIG(TAG, "  Vertical swing:    %s", YESNO(this->cap_supported_(CAP_VERTICAL_SWING)));
    ESP_LOGCONFIG(TAG, "  Horizontal swing:  %s", YESNO(this->cap_supported_(CAP_HORIZONTAL_SWING)));
    ESP_LOGCONFIG(TAG, "  Economy mode:      %s", YESNO(this->cap_supported_(CAP_ECONOMY)));
    ESP_LOGCONFIG(TAG, "  Powerful mode:     %s", YESNO(this->cap_supported_(CAP_POWERFUL)));
    ESP_LOGCONFIG(TAG, "  Minimum heat:      %s", YESNO(this->cap_supported_(CAP_MINIMUM_HEAT)));
    ESP_LOGCONFIG(TAG, "  Human sensor:      %s", YESNO(this->cap_supported_(CAP_HUMAN_SENSOR)));
    ESP_LOGCONFIG(TAG, "  Energy saving fan: %s", YESNO(this->cap_supported_(CAP_ENERGY_SAVING_FAN)));
    ESP_LOGCONFIG(TAG, "  Outdoor low noise: %s", YESNO(this->cap_supported_(CAP_OUTDOOR_LOW_NOISE)));
    ESP_LOGCONFIG(TAG, "  Coil dry:          %s", YESNO(this->cap_supported_(CAP_COIL_DRY)));
    // Raw airflow-count capability codes (§9.1 / §10.4 — encoding not fully
    // decoded; logged to correlate with which vane positions a unit accepts).
    auto cap_raw = [this](uint16_t addr) -> int {
      auto it = this->caps_.find(addr);
      return it == this->caps_.end() ? -1 : it->second;
    };
    ESP_LOGCONFIG(TAG, "  Vertical airflow count (raw):   0x%04X", cap_raw(CAP_VERTICAL_AIRFLOW_COUNT));
    ESP_LOGCONFIG(TAG, "  Horizontal airflow count (raw): 0x%04X", cap_raw(CAP_HORIZONTAL_AIRFLOW_COUNT));
  }
  LOG_CLIMATE("  ", "Climate", this);
  LOG_SENSOR("  ", "Indoor Temperature", this->indoor_temperature_sensor_);
  LOG_SENSOR("  ", "Outdoor Temperature", this->outdoor_temperature_sensor_);
}

}  // namespace fujitsu_ac
}  // namespace esphome
