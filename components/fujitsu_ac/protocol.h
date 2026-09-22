#pragma once

#include <cstdint>
#include <cstddef>

namespace esphome {
namespace fujitsu_ac {

// ---------------------------------------------------------------------------
// Framing (PROTOCOL.md §2)
//   [CMD, 0x00, 0x00, 0x00, LEN, payload(LEN), CK_H, CK_L]
//   Total frame length = LEN + 7.
// ---------------------------------------------------------------------------
static const uint8_t FRAME_HEADER_LEN = 5;   // CMD + 3 reserved + LEN
static const uint8_t FRAME_CKSUM_LEN = 2;    // big-endian 16-bit checksum
static const uint8_t FRAME_OVERHEAD = FRAME_HEADER_LEN + FRAME_CKSUM_LEN;  // 7
static const uint8_t LEN_OFFSET = 4;         // byte index of the LEN field
static const uint8_t STATUS_OFFSET = 5;      // payload byte 0 for 0x02/0x03 responses
static const size_t MAX_FRAME_LEN = 128;     // §2.1: frames are well under 128 bytes

// Command / message types (§4)
enum Command : uint8_t {
  CMD_INIT1 = 0x00,  // handshake stage 1
  CMD_INIT2 = 0x01,  // handshake stage 2
  CMD_WRITE = 0x02,  // write registers
  CMD_READ = 0x03,   // read registers
};

// Response status byte (§4)
static const uint8_t STATUS_OK = 0x01;

// ---------------------------------------------------------------------------
// Register addresses (§9)
// ---------------------------------------------------------------------------
// Capability / feature-flag registers (read once at init, §9.1)
enum CapRegister : uint16_t {
  CAP_VERTICAL_AIRFLOW_COUNT = 0x0130,
  CAP_VERTICAL_SWING = 0x0131,
  CAP_HORIZONTAL_AIRFLOW_COUNT = 0x0142,
  CAP_HORIZONTAL_SWING = 0x0143,
  CAP_ECONOMY = 0x0150,
  CAP_MINIMUM_HEAT = 0x0151,
  CAP_HUMAN_SENSOR = 0x0152,
  CAP_ENERGY_SAVING_FAN = 0x0153,
  CAP_POWERFUL = 0x0170,
  CAP_OUTDOOR_LOW_NOISE = 0x0171,
  CAP_COIL_DRY = 0x0193,
};

// Primary / extended state registers (§9.2–9.4)
enum Register : uint16_t {
  REG_POWER = 0x1000,           // 0=Off, 1=On
  REG_MODE = 0x1001,            // Auto/Cool/Dry/Fan/Heat
  REG_SETPOINT = 0x1002,        // tenths °C (0xFFFF in Fan mode)
  REG_FAN = 0x1003,             // Auto/Quiet/Low/Medium/High
  REG_VANE_V_SET = 0x1010,      // write target for vertical position
  REG_SWING_V = 0x1011,         // 0=Off, 1=On
  REG_VANE_V = 0x10A0,          // read-back of vertical position
  REG_VANE_H_SET = 0x1022,      // write target for horizontal position
  REG_SWING_H = 0x1023,         // 0=Off, 1=On
  REG_VANE_H = 0x10A9,          // read-back of horizontal position
  REG_INDOOR_TEMP = 0x1033,     // offset-encoded (§10.7)
  REG_ECONOMY = 0x1100,         // 0=Off, 1=On
  REG_MINIMUM_HEAT = 0x1101,    // 0=Off, 1=On
  REG_HUMAN_SENSOR = 0x1102,    // 0=Off, 1=On
  REG_ENERGY_SAVING_FAN = 0x1108,
  REG_POWERFUL = 0x1120,        // 0=Off, 1=On
  REG_OUTDOOR_LOW_NOISE = 0x1121,
  REG_COIL_DRY = 0x1144,
  REG_OUTDOOR_TEMP = 0x2020,    // signed offset-encoded (§10.8)
};

// ---------------------------------------------------------------------------
// Value encodings (§10)
// ---------------------------------------------------------------------------
enum ModeValue : uint16_t {
  MODE_AUTO = 0x0000,
  MODE_COOL = 0x0001,
  MODE_DRY = 0x0002,
  MODE_FAN = 0x0003,
  MODE_HEAT = 0x0004,
  // 0x0005 exists transiently in the reference firmware; treat as unknown (§10.2).
};

enum FanValue : uint16_t {
  FAN_AUTO = 0x0000,
  FAN_QUIET = 0x0002,
  FAN_LOW = 0x0005,
  FAN_MEDIUM = 0x0008,
  FAN_HIGH = 0x000B,
};

static const uint16_t ON_VALUE = 0x0001;
static const uint16_t OFF_VALUE = 0x0000;
// "No reading": reported for the setpoint in Fan mode (§10.6), and for either
// temperature register while its sensor has no valid value yet (§10.7, §10.8).
static const uint16_t VALUE_UNAVAILABLE = 0xFFFF;

// Temperature offset encoding (§10.7 / §10.8): °C = (raw - 5025) / 100
static const int32_t TEMP_OFFSET = 5025;

// ---------------------------------------------------------------------------
// Checksum (§3): 16-bit additive, transmitted big-endian.
//   checksum = 0xFFFF - Σ(all bytes before the two checksum bytes)
// ---------------------------------------------------------------------------
inline uint16_t checksum(const uint8_t *buf, size_t len_without_cksum) {
  uint16_t cksum = 0xFFFF;
  for (size_t i = 0; i < len_without_cksum; i++)
    cksum -= buf[i];
  return cksum;
}

// Validate a complete frame of `total_len` bytes (incl. trailing checksum).
inline bool frame_valid(const uint8_t *buf, size_t total_len) {
  if (total_len < FRAME_OVERHEAD)
    return false;
  uint16_t got = (uint16_t(buf[total_len - 2]) << 8) | buf[total_len - 1];
  return got == checksum(buf, total_len - 2);
}

// Build a frame into `out` (must hold at least LEN + 7 bytes). Returns total length.
inline size_t build_frame(uint8_t cmd, const uint8_t *payload, uint8_t payload_len, uint8_t *out) {
  out[0] = cmd;
  out[1] = 0x00;
  out[2] = 0x00;
  out[3] = 0x00;
  out[4] = payload_len;
  for (uint8_t i = 0; i < payload_len; i++)
    out[FRAME_HEADER_LEN + i] = payload[i];
  size_t cksum_pos = FRAME_HEADER_LEN + payload_len;
  uint16_t cksum = checksum(out, cksum_pos);
  out[cksum_pos] = (cksum >> 8) & 0xFF;      // high
  out[cksum_pos + 1] = cksum & 0xFF;         // low
  return cksum_pos + FRAME_CKSUM_LEN;
}

// ---------------------------------------------------------------------------
// Temperature helpers
// ---------------------------------------------------------------------------
inline float decode_temp_offset(uint16_t raw, bool is_signed) {
  int32_t v = is_signed ? int32_t(int16_t(raw)) : int32_t(raw);
  return (v - TEMP_OFFSET) / 100.0f;
}

// Setpoint: tenths of °C, snapped to nearest 0.5 °C (§10.6).
inline uint16_t encode_setpoint(float celsius) {
  int32_t tenths = int32_t(celsius * 10.0f + (celsius >= 0 ? 0.5f : -0.5f));
  // snap to nearest multiple of 5 (0.5 °C resolution)
  tenths = ((tenths + 2) / 5) * 5;
  return uint16_t(tenths);
}

inline float decode_setpoint(uint16_t raw) { return raw / 10.0f; }

}  // namespace fujitsu_ac
}  // namespace esphome
