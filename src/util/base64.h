#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace Base64 {

  inline constexpr std::string_view kChars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

  inline std::string encode(const std::uint8_t* data, std::size_t size) {
    std::string out;
    out.reserve(((size + 2) / 3) * 4);
    for (std::size_t i = 0; i < size; i += 3) {
      const std::size_t n = std::min<std::size_t>(3, size - i);
      std::uint32_t chunk = 0;
      for (std::size_t j = 0; j < n; ++j) {
        chunk |= static_cast<std::uint32_t>(data[i + j]) << static_cast<unsigned>(16 - static_cast<int>(j * 8));
      }
      out.push_back(kChars[(chunk >> 18) & 63]);
      out.push_back(kChars[(chunk >> 12) & 63]);
      out.push_back(n > 1 ? kChars[(chunk >> 6) & 63] : '=');
      out.push_back(n > 2 ? kChars[chunk & 63] : '=');
    }
    return out;
  }

  inline std::string encode(std::string_view data) {
    return encode(reinterpret_cast<const std::uint8_t*>(data.data()), data.size());
  }

  inline std::string encode(const std::vector<std::uint8_t>& data) { return encode(data.data(), data.size()); }

  inline std::vector<std::uint8_t> decode(std::string_view in) {
    std::array<int, 256> decodeTable{};
    decodeTable.fill(-1);
    for (int b = 0; b < 64; ++b) {
      decodeTable[static_cast<unsigned char>(kChars[static_cast<std::size_t>(b)])] = b;
    }
    std::vector<std::uint8_t> out;
    out.reserve(in.size() * 3 / 4);
    int val = 0;
    int valb = -8;
    for (char rawc : in) {
      const auto c = static_cast<unsigned char>(rawc);
      if (c == '=') {
        break;
      }
      const int d = decodeTable[c];
      if (d < 0) {
        continue;
      }
      val = (val << 6) + d;
      valb += 6;
      if (valb >= 0) {
        out.push_back(static_cast<std::uint8_t>((val >> valb) & 0xFF));
        valb -= 8;
      }
    }
    return out;
  }

} // namespace Base64
