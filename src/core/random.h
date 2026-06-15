#pragma once

#include <random>

namespace Random {

  inline std::mt19937& rng() {
    static std::mt19937 gen{std::random_device{}()};
    return gen;
  }

  inline float randomFloat(float min, float max) {
    std::uniform_real_distribution<float> dist(min, max);
    return dist(rng());
  }

  template <typename T> inline T randomInt(T min, T max) {
    std::uniform_int_distribution<T> dist(min, max);
    return dist(rng());
  }

} // namespace Random
