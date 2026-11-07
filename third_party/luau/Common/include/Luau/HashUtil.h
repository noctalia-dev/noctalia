#pragma once

#include <stddef.h>
#include <stdint.h>

#include <functional>
#include <type_traits>
#include <utility>

namespace Luau
{

struct DenseHashPointer
{
    size_t operator()(const void* key) const
    {
        // The idea to use this hash function was suggested here originally: https://maskray.me/blog/2026-06-07-recent-llvm-hash-table-improvements
        // Hash function implementation is detailed here: https://github.com/MaskRay/llvm-project/blob/main/llvm/include/llvm/ADT/DenseMapInfo.h
        // This hash produces better scattering for arena allocated types, because the pointers usually share the higher order bits.
        // When inserting lots of keys, quadratic probing is not enough to save DenseHash, although it usually takes many more elements,
        // before it becomes a problem
        uint64_t u = static_cast<uint64_t>(uintptr_t(key));
        u *= 0xbf58476d1ce4e5b9u;
        u ^= u >> 31;
        // On 32-bit platforms uint64_t to size_t is a narrowing, so we need
        // to static cast here.
        return static_cast<size_t>(u);
    }
};

// std::hash can't be specialized for void*, so void* keys are hashed by address; DenseHash's Fibonacci hashing mixes the bits.
struct DenseHashAddress
{
    size_t operator()(const void* key) const noexcept
    {
        return uintptr_t(key);
    }
};

namespace detail
{

template<typename T>
using DenseHashDefault = std::conditional_t<std::is_same_v<T, void*> || std::is_same_v<T, const void*>, DenseHashAddress, std::hash<T>>;

} // namespace detail

inline void hashCombine(size_t& seed, size_t hash)
{
    // Golden Ratio constant used for better hash scattering
    // See https://softwareengineering.stackexchange.com/a/402543
    seed ^= hash + 0x9e3779b9 + (seed << 6) + (seed >> 2);
}

template<typename T1, typename T2, typename H1 = detail::DenseHashDefault<T1>, typename H2 = detail::DenseHashDefault<T2>>
struct PairHash
{
    std::size_t operator()(const std::pair<T1, T2>& p) const noexcept
    {
        std::size_t seed = 0;
        hashCombine(seed, h1(p.first));
        hashCombine(seed, h2(p.second));
        return seed;
    }

private:
    H1 h1;
    H2 h2;
};

} // namespace Luau
