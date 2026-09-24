#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

constexpr uint32_t SPECTRUM_TRANSPORT_MAGIC = 0x534C5A31U; // "SLZ1"
constexpr uint16_t SPECTRUM_TRANSPORT_VERSION = 1;

#pragma pack(push, 1)
struct spectrum_transport_header {
  uint32_t magic = SPECTRUM_TRANSPORT_MAGIC;
  uint16_t version = SPECTRUM_TRANSPORT_VERSION;
  uint16_t server_id = 0;
  uint64_t sequence = 0;
  uint32_t message_bytes = 0;
  uint32_t crc32c = 0;
};
#pragma pack(pop)

static_assert(sizeof(spectrum_transport_header) == 24,
              "spectrum transport header must remain 24 bytes");

inline uint32_t spectrum_crc32c(const void *data, size_t length) {
  static const std::array<uint32_t, 256> table = [] {
    std::array<uint32_t, 256> result{};
    for (uint32_t i = 0; i < result.size(); ++i) {
      uint32_t crc = i;
      for (unsigned bit = 0; bit < 8; ++bit)
        crc = (crc >> 1) ^ ((crc & 1U) ? 0x82F63B78U : 0U);
      result[i] = crc;
    }
    return result;
  }();

  uint32_t crc = 0xffffffffU;
  const auto *bytes = static_cast<const uint8_t *>(data);
  for (size_t i = 0; i < length; ++i)
    crc = table[(crc ^ bytes[i]) & 0xffU] ^ (crc >> 8);
  return ~crc;
}
