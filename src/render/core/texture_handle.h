#pragma once

#include <cstdint>

class TextureId {
public:
  constexpr TextureId() noexcept = default;
  explicit constexpr TextureId(std::uint32_t value) noexcept : m_value(value) {}

  [[nodiscard]] constexpr std::uint32_t value() const noexcept { return m_value; }
  [[nodiscard]] constexpr bool valid() const noexcept { return m_value != 0; }
  [[nodiscard]] explicit constexpr operator bool() const noexcept { return valid(); }

  friend constexpr bool operator==(TextureId lhs, TextureId rhs) noexcept = default;
  friend constexpr bool operator==(TextureId lhs, std::uint32_t rhs) noexcept { return lhs.m_value == rhs; }
  friend constexpr bool operator==(std::uint32_t lhs, TextureId rhs) noexcept { return lhs == rhs.m_value; }

private:
  std::uint32_t m_value = 0;
};

struct TextureHandle {
  TextureId id;
  int width = 0;
  int height = 0;
  std::uint64_t generation = 0;

  [[nodiscard]] constexpr bool valid() const noexcept { return id.valid(); }
};
