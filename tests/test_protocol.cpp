// Host-side bench test for the framing/checksum layer (no ESPHome deps).
// Builds the frames documented in PROTOCOL.md §5/§6 and asserts they match
// the spec byte-for-byte. From the repo root:
//   c++ -std=c++17 -I components/fujitsu_ac tests/test_protocol.cpp -o /tmp/t && /tmp/t
#include "protocol.h"
#include <cassert>
#include <cstdio>
#include <vector>

using namespace esphome::fujitsu_ac;

static void expect(const std::vector<uint8_t> &got, const std::vector<uint8_t> &want, const char *name) {
  if (got != want) {
    printf("FAIL %s\n  got: ", name);
    for (auto b : got) printf("%02X ", b);
    printf("\n want: ");
    for (auto b : want) printf("%02X ", b);
    printf("\n");
    exit(1);
  }
  printf("ok   %s\n", name);
}

int main() {
  uint8_t buf[MAX_FRAME_LEN];

  // Init1 (§5): 00 00 00 00 04 00 00 00 00 FF FB
  {
    uint8_t payload[] = {0x00, 0x00, 0x00, 0x00};
    size_t n = build_frame(CMD_INIT1, payload, sizeof(payload), buf);
    expect({buf, buf + n}, {0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFB}, "Init1");
  }

  // Init2 (§5): 01 00 00 00 04 00 04 00 01 FF F5
  {
    uint8_t payload[] = {0x00, 0x04, 0x00, 0x01};
    size_t n = build_frame(CMD_INIT2, payload, sizeof(payload), buf);
    expect({buf, buf + n}, {0x01, 0x00, 0x00, 0x00, 0x04, 0x00, 0x04, 0x00, 0x01, 0xFF, 0xF5}, "Init2");
  }

  // Read request §6.1 example (addrs 0x0001, 0x0101): 03 00 00 00 04 00 01 01 01 CK CK
  {
    uint8_t payload[] = {0x00, 0x01, 0x01, 0x01};
    size_t n = build_frame(CMD_READ, payload, sizeof(payload), buf);
    // checksum = 0xFFFF - (03+04+01+01+01) = 0xFFFF - 0x0A = 0xFFF5
    expect({buf, buf + n}, {0x03, 0x00, 0x00, 0x00, 0x04, 0x00, 0x01, 0x01, 0x01, 0xFF, 0xF5}, "Read req");
  }

  // frame_valid accepts the documented Init responses and rejects a corrupt one.
  {
    uint8_t ok[] = {0x00, 0x00, 0x00, 0x00, 0x01, 0x01, 0xFF, 0xFD};
    assert(frame_valid(ok, sizeof(ok)));
    uint8_t bad[] = {0x00, 0x00, 0x00, 0x00, 0x01, 0x01, 0xFF, 0xFE};
    assert(!frame_valid(bad, sizeof(bad)));
    printf("ok   frame_valid\n");
  }

  // Temperature encodings (§10.6-10.8)
  assert(encode_setpoint(25.0f) == 250);                       // 0x00FA
  assert(encode_setpoint(25.3f) == 255);                       // snaps to 25.5
  assert(decode_setpoint(250) == 25.0f);
  {
    float in = decode_temp_offset(5025 + 2150, false);          // 21.50 °C
    assert(in > 21.49f && in < 21.51f);
    float out = decode_temp_offset(uint16_t(int16_t(5025 - 500)), true);  // -5.00 °C
    assert(out > -5.01f && out < -4.99f);
    printf("ok   temp encodings\n");
  }

  printf("ALL PASS\n");
  return 0;
}
