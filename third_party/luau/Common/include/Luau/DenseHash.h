// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
#pragma once

#include "Luau/HashUtil.h"
#include "Luau/Common.h"

#include <stddef.h>
#include <functional>
#include <type_traits>
#include <utility>
#include <stdint.h>

namespace Luau
{

// Internal implementation of DenseHashSet and DenseHashMap
namespace detail
{

inline size_t countTrailingZeroes(uint64_t word)
{
    // Caller must ensure that word is not 0
    LUAU_ASSERT(word != 0);
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_ctzll(word);
#elif defined(_MSC_VER) && defined(_M_X64)
    unsigned long idx;
    _BitScanForward64(&idx, word);
    return idx;
#else
    size_t n = 0;
    while ((word & 1) == 0)
    {
        word >>= 1;
        ++n;
    }
    return n;
#endif
}

// This is an implementation of a hash table that does not require a tombstone element and supports erase.
// In order to avoid the use of tombstones we use a Dense BitSet to store presence/absence of elements.
// The hashtable uses fibonacci hashing and linear probing to ensure pretty good scattering. The usage of fibonacci hashing
// means we can just use the default std::hash and there is less burden on us to select a good hash function. For degenerate cases, the
// option still remains to supply your own hash.
// Invariants:
// - capacity is a power of two to support fast integer modulo (%)
// - doubling the capacity of the map allows us to amortize the cost of lookups and better scatter elements throughout the map
// - every element can either be reached in the map by computing its hash, or from a linear probe of elements (that wraps around) from the hash
// location
// - when the map gets to 3/4 full(our load factor), we will double the size of the hash.
// Interesting details:
// Rehashing uses the count trailing zeroes intrinsic in order to quickly skip to the elements that have to be rehashed.
// We can do this because the bitset data structure encodes presence/absence in a uint64_t, so we could theoretically
// skip up to 64 empty elements per loop iteration, instead of one element per loop

template<typename Key, typename Item, typename MutableItem, typename ItemInterface, typename Hash, typename Eq>
class DenseHashTable
{

private:
    struct BitSet
    {
        // UsedTable stores an array of uint64_t, where the ith' uint64_t represents the presence/absence of
        // elements at indices [64 * (i - 1) - 64 * i).
        // Each uint64_t is examined from LSB to MSB (right to left).
        // For example, if the 1st uint64 looks like 0'11[61 0's...]1, then we interpret indices 0, 62, and 63 are occupied in
        // the hashtable.

        using BitsetT = uint64_t;
        // These constants are used to make implementing the contains/set methods magic constant agnostic
        static constexpr size_t numElements = sizeof(BitsetT) * 8;
        // Num elements must be a power of 2
        static_assert((numElements & (numElements - 1)) == 0);
        // If you want to change the backing structure from 32 bit unsigned integer to 64 / 8 /... etc, you'll have to update this variable
        static constexpr size_t numElementsLog2 = 6;

        explicit BitSet(size_t capacity = 0)
            : capacity(capacity)
            , count(0)
            , data(nullptr)
        {
            LUAU_ASSERT((capacity & (capacity - 1)) == 0);
            if (capacity != 0u)
            {
                count = capacity < numElements ? 1 : capacity >> numElementsLog2;
                data = static_cast<BitsetT*>(::operator new(sizeof(BitsetT) * count));
                memset(data, 0, sizeof(BitsetT) * count);
            }
        }

        ~BitSet()
        {
            ::operator delete(data);
        }

        BitSet(const BitSet& other)
            : capacity(other.capacity)
            , count(other.count)
            , data(nullptr)
        {
            if (count != 0u)
            {
                data = static_cast<BitsetT*>(::operator new(sizeof(BitsetT) * count));
                memcpy(data, other.data, sizeof(BitsetT) * count);
            }
        }

        BitSet(BitSet&& other) noexcept
            : capacity(other.capacity)
            , count(other.count)
            , data(other.data)
        {
            other.capacity = 0;
            other.count = 0;
            other.data = nullptr;
        }

        BitSet& operator=(const BitSet& other)
        {
            if (this != &other)
            {
                BitSet copy(other);
                *this = std::move(copy);
            }
            return *this;
        }

        BitSet& operator=(BitSet&& other) noexcept
        {
            if (this != &other)
            {
                ::operator delete(data);
                data = other.data;
                capacity = other.capacity;
                count = other.count;
                other.data = nullptr;
                other.capacity = 0;
                other.count = 0;
            }
            return *this;
        }

        bool contains(size_t bucket) const
        {
            size_t whichBitvec = bucket >> numElementsLog2;
            size_t bvOffset = bucket & (numElements - 1);
            return ((data[whichBitvec] >> bvOffset) & 1u) == 1u;
        }

        void clear()
        {
            if (count != 0u)
                memset(data, 0, sizeof(BitsetT) * count);
        }

        void set(size_t bucket, bool v)
        {
            size_t whichBitvec = bucket >> numElementsLog2;
            size_t offset = bucket & (numElements - 1);
            if (v)
                data[whichBitvec] |= (BitsetT(1) << offset);
            else
                data[whichBitvec] &= ~(BitsetT(1) << offset);
        }

        BitsetT wordAt(size_t idx) const
        {
            LUAU_ASSERT(idx < count);
            return data[idx];
        }

        size_t numWords() const
        {
            return count;
        }

        // This iterator produces a stream of indices representing indices in bitset that are on, not each bit in the set
        // Insertions to this set / mutations will invalidate this iterator
        class iterator
        {
        public:
            iterator()
                : data(nullptr)
                , wordCount(0)
                , wordIdx(0)
                , word(0)
            {
            }

            iterator(const BitsetT* data, size_t wordCount, size_t wordIdx)
                : data(data)
                , wordCount(wordCount)
                , wordIdx(wordIdx)
                , word(0)
            {
                // Skip all words that are 0's
                while (this->wordIdx < this->wordCount && (this->word = data[this->wordIdx]) == 0)
                    this->wordIdx++;

                if (this->wordIdx < this->wordCount)
                    currentBucket = this->wordIdx * numElements + countTrailingZeroes(word);
            }

            size_t operator*() const
            {
                return currentBucket;
            }

            iterator& operator++()
            {
                // When you want the `next` occupied bucket, zero out the lowest 1 in the current word in the bitset (since that's where you are
                // currently) If the word is 0, skip to the next one - there aren't anymore elements in the current block of 64 Otherwise, the
                // currentBucket is the index of the word * numElements per word + the trailing zeroes in that word
                word &= word - 1;
                while (word == 0)
                {
                    wordIdx++;
                    if (wordIdx >= wordCount)
                        return *this;
                    word = data[wordIdx];
                }
                currentBucket = wordIdx * numElements + countTrailingZeroes(word);
                return *this;
            }

            bool operator==(const iterator& other) const
            {
                return wordIdx == other.wordIdx && word == other.word;
            }

            bool operator!=(const iterator& other) const
            {
                return wordIdx != other.wordIdx || word != other.word;
            }

        private:
            const BitsetT* data;
            size_t wordCount;
            size_t wordIdx;
            BitsetT word;
            size_t currentBucket = 0;
        };

        iterator begin() const
        {
            return iterator(data, count, 0);
        }

        iterator end() const
        {
            return iterator(data, count, count);
        }

    private:
        size_t capacity;
        size_t count;
        BitsetT* data;
    };

public:
    class const_iterator;
    class iterator;

    explicit DenseHashTable(size_t buckets = 0)
        : data(nullptr)
        , usedTable(buckets)
        , capacity(0)
        , count(0)
        , hashShift(64)
    {
        // buckets has to be power-of-two or zero
        LUAU_ASSERT((buckets & (buckets - 1)) == 0);

        if (buckets != 0u)
        {
            data = static_cast<Item*>(::operator new(sizeof(Item) * buckets));
            capacity = buckets;
            // This cast is fine because count trailing zeroes on a uint64_t returns a number in [0, 64)
            // Easily fits in a uint8_t
            hashShift = 64 - static_cast<uint8_t>(detail::countTrailingZeroes(static_cast<uint64_t>(buckets)));
        }
    }

    ~DenseHashTable()
    {
        if (data)
            destroy();
    }

    DenseHashTable(const DenseHashTable& other)
        : data(nullptr)
        , usedTable(0)
        , capacity(0)
        , count(other.count)
        , hashShift(other.hashShift)
    {
        if (other.capacity)
        {
            data = static_cast<Item*>(::operator new(sizeof(Item) * other.capacity));
            usedTable = BitSet{other.capacity};

            for (size_t bucket : other.usedTable)
            {
                new (&data[bucket]) Item(other.data[bucket]);
                usedTable.set(bucket, true); // if Item copy throws, used table will note the initialized objects for destroy() to clean up
            }

            capacity = other.capacity;
            LUAU_ASSERT((capacity & (capacity - 1)) == 0);
        }
    }

    DenseHashTable(DenseHashTable&& other) noexcept
        : data(other.data)
        , usedTable(std::move(other.usedTable))
        , capacity(other.capacity)
        , count(other.count)
        , hashShift(other.hashShift)
    {
        other.data = nullptr;
        other.capacity = 0;
        other.count = 0;
        other.hashShift = 64;
    }

    DenseHashTable& operator=(DenseHashTable&& other) noexcept
    {
        if (this != &other)
        {
            if (data)
                destroy();

            data = other.data;
            usedTable = std::move(other.usedTable);
            capacity = other.capacity;
            count = other.count;
            hashShift = other.hashShift;

            other.data = nullptr;
            other.capacity = 0;
            other.count = 0;
            other.hashShift = 64;
        }

        return *this;
    }

    DenseHashTable& operator=(const DenseHashTable& other)
    {
        if (this != &other)
        {
            DenseHashTable copy(other);
            *this = std::move(copy);
        }

        return *this;
    }

    void clear(size_t thresholdToDestroy = 32)
    {
        if (count == 0)
            return;

        if (capacity > thresholdToDestroy)
        {
            destroy();
        }
        else
        {
            for (size_t bucket : usedTable)
                data[bucket].~Item();
            usedTable.clear();
        }

        count = 0;
    }

    void destroy()
    {
        for (size_t bucket : usedTable)
            data[bucket].~Item();

        ::operator delete(data);
        data = nullptr;
        usedTable = BitSet{};

        capacity = 0;
        hashShift = 64;
    }

    size_t doHash(const Key& key) const
    {
        // DenseHash2 uses fibonacci hashing for much better scattering
        // https://probablydance.com/2018/06/16/fibonacci-hashing-the-optimization-that-the-world-forgot-or-a-better-alternative-to-integer-modulo/
        // The hashShift is 64(num bits) - log2(capacity).
        // Shifting the hashed value * constant by this much maps the resulting value into the range [0, capacity)
        // For example: if capacity is 4096, we shift right by 52 (64 - 12). This preserves the upper 12 bits of data with the remaining 52 bits being
        // 0 Your value will look like: [ 52 bits of zeroes | 12 bits of 'value'] This 12 bits of value will map you in the range [0, 2^12 -1], which
        // is exactly [0, capacity] One of the reasons for taking the upper order bits is that multiplications shifts information upwards, so those
        // are probably the interesting bits to keep
        // This is nice to do because it means the user of dense hash doesn't have to design a good hash function - e.g. if you're hashing integers
        // you can just return the integer. If you tried to do this with DenseHash(old), you'd end up with a lot of clustering, and it means we can
        // just use std::hash as the default hash everywhere
        return static_cast<size_t>((static_cast<uint64_t>(hasher(key)) * 11400714819323198485ull) >> hashShift);
    }

    struct BucketResult
    {
        size_t bucket;
        bool found;
    };

    void erase(const Key& key)
    {
        if (count == 0)
            return;

        auto [bucket, found] = getBucket(key);
        if (found)
            doErase(bucket);
    }

    Item* insert_unsafe(const Key& key)
    {
        auto [bucket, found] = getBucket(key);

        if (!found)
        {
            usedTable.set(bucket, true);
            ItemInterface::setKey(data[bucket], key);
            count++;
        }

        return &data[bucket];
    }

    const Item* find(const Key& key) const
    {
        if (count == 0)
            return NULL;

        auto [bucket, found] = getBucket(key);
        return found ? &data[bucket] : NULL;
    }

    void grow()
    {
        size_t newsize = capacity == 0 ? 16 : capacity * 2;

        DenseHashTable newtable(newsize);

        // We can leverage the structure of the bitvector here to skip contiguous chunks of empty elements
        for (size_t bucket : usedTable)
        {
            const Key& key = ItemInterface::getKey(data[bucket]);
            // ItemInterface::setKey default constructs the value type. If we use insert_unsafe here, we then pay for one unnecessary construction
            // which is immediately overwritten. Instead, we manually insert these items into the destination table
            auto [dest, found] = newtable.getBucket(key);
            LUAU_ASSERT(!found);
            newtable.usedTable.set(dest, true);
            ++newtable.count;
            new (&newtable.data[dest]) Item(std::move(data[bucket]));
        }

        LUAU_ASSERT(count == newtable.count);

        std::swap(data, newtable.data);
        std::swap(usedTable, newtable.usedTable);
        std::swap(capacity, newtable.capacity);
        std::swap(hashShift, newtable.hashShift);
    }

    void rehash_if_full(const Key& key)
    {
        if (count >= capacity * 3 / 4 && !find(key))
        {
            grow();
        }
    }

    const_iterator begin() const
    {
        return const_iterator(this, usedTable.begin());
    }

    const_iterator end() const
    {
        return const_iterator(this, usedTable.end());
    }

    iterator begin()
    {
        return iterator(this, usedTable.begin());
    }

    iterator end()
    {
        return iterator(this, usedTable.end());
    }

    size_t size() const
    {
        return count;
    }

    class const_iterator
    {
    public:
        using value_type = Item;
        using reference = Item&;
        using pointer = Item*;
        using difference_type = ptrdiff_t;
        using iterator_category = std::forward_iterator_tag;

        const_iterator() = default;

        const_iterator(const DenseHashTable<Key, Item, MutableItem, ItemInterface, Hash, Eq>* set, typename BitSet::iterator bucketIt)
            : set(set)
            , bucketIt(bucketIt)
        {
        }

        const Item& operator*() const
        {
            return set->data[*bucketIt];
        }

        const Item* operator->() const
        {
            return &set->data[*bucketIt];
        }

        bool operator==(const const_iterator& other) const
        {
            return set == other.set && bucketIt == other.bucketIt;
        }

        bool operator!=(const const_iterator& other) const
        {
            return set != other.set || bucketIt != other.bucketIt;
        }

        const_iterator& operator++()
        {
            ++bucketIt;
            return *this;
        }

        const_iterator operator++(int)
        {
            const_iterator res = *this;
            ++*this;
            return res;
        }

    private:
        const DenseHashTable<Key, Item, MutableItem, ItemInterface, Hash, Eq>* set = nullptr;
        typename BitSet::iterator bucketIt;
    };

    class iterator
    {
    public:
        using value_type = MutableItem;
        using reference = MutableItem&;
        using pointer = MutableItem*;
        using difference_type = ptrdiff_t;
        using iterator_category = std::forward_iterator_tag;

        iterator() = default;

        iterator(DenseHashTable<Key, Item, MutableItem, ItemInterface, Hash, Eq>* set, typename BitSet::iterator bucketIt)
            : set(set)
            , bucketIt(bucketIt)
        {
        }

        MutableItem& operator*() const
        {
            return *reinterpret_cast<MutableItem*>(&set->data[*bucketIt]);
        }

        MutableItem* operator->() const
        {
            return reinterpret_cast<MutableItem*>(&set->data[*bucketIt]);
        }

        bool operator==(const iterator& other) const
        {
            return set == other.set && bucketIt == other.bucketIt;
        }

        bool operator!=(const iterator& other) const
        {
            return set != other.set || bucketIt != other.bucketIt;
        }

        iterator& operator++()
        {
            ++bucketIt;
            return *this;
        }

        iterator operator++(int)
        {
            iterator res = *this;
            ++*this;
            return res;
        }

    private:
        DenseHashTable<Key, Item, MutableItem, ItemInterface, Hash, Eq>* set = nullptr;
        typename BitSet::iterator bucketIt;
    };

private:
    // Returns the bucket where `key` was found, or the first empty bucket if not present.
    inline BucketResult getBucket(const Key& key) const
    {
        // Guarantees that this function will terminate - in the worst case, the linear probe
        // is guaranteed to hit an empty slot
        LUAU_ASSERT(count < capacity);
        size_t hashmod = capacity - 1;
        size_t bucket = doHash(key);

        for (;;)
        {
            if (!usedTable.contains(bucket))
            {
                return {bucket, false};
            }

            if (eq(ItemInterface::getKey(data[bucket]), key))
                return {bucket, true};
            bucket = (bucket + 1) & hashmod;
        }

        LUAU_ASSERT(false);
        return {0, false};
    }

    void doErase(size_t bucket)
    {
        // doErase implements algorithm R deletion as described in TAOCP volume 3, 6.4 (although in TAOCP, probes go towards lower indices)
        // This is the implementation described here: https://maskray.me/blog/2026-06-07-recent-llvm-hash-table-improvements
        size_t i = bucket;
        size_t j = bucket;
        size_t hashmod = capacity - 1;
        while (true)
        {
            // Slightly subtle - our capacity is always guaranteed to be a power of two, which is why
            // we can use bitwise & to perform a quick modulo (%) operation. The ensures that the index pointer `j`
            // will always be in range
            j = (j + 1) & hashmod;
            const Item& curr = data[j];
            if (!usedTable.contains(j))
                break;
            size_t r = doHash(ItemInterface::getKey(curr));
            // The invariant here is that every element must be reachable from a contiguous probe sequence.
            // When considering whether to shift the element at j down to the element at i, we look at the original
            // hash position. If the original hash position lies in between i and j, then shifting this element would break
            // the probe sequence. Thus, modulo the size of the hash table, the original hash position must be closer to i than j
            // to avoid breaking the probe sequence.
            size_t left = (i - r) & hashmod;
            size_t right = (j - r) & hashmod;

            if (left < right)
            {
                usedTable.set(i, true);
                usedTable.set(j, false);
                // Slot i currently holds a live element: the just-erased element on the first shift, or an
                // already-moved-from element on later shifts. Destroy it before move-constructing over it,
                // otherwise value types that own resources (e.g. std::unique_ptr) leak the erased element.
                data[i].~Item();
                new (&data[i]) Item(std::move(data[j]));
                i = j;
            }
        }

        usedTable.set(i, false);
        data[i].~Item();
        --count;
    }

    Item* data;
    BitSet usedTable;
    size_t capacity;
    size_t count;
    uint8_t hashShift; // 64 - log2(capacity); used for Fibonacci hashing
    Hash hasher;
    Eq eq;
};

template<typename Key>
struct ItemInterfaceSet2
{
    static const Key& getKey(const Key& item)
    {
        return item;
    }

    static void setKey(Key& item, const Key& key)
    {
        new (&item) Key(key);
    }
};

template<typename Key, typename Value>
struct ItemInterfaceMap2
{
    static const Key& getKey(const std::pair<Key, Value>& item)
    {
        return item.first;
    }

    static void setKey(std::pair<Key, Value>& item, const Key& key)
    {
        new (&item.first) Key(key);
        new (&item.second) Value();
    }
};

} // namespace detail

// This is a faster alternative of unordered_set
template<typename Key, typename Hash = detail::DenseHashDefault<Key>, typename Eq = std::equal_to<Key>>
class DenseHashSet
{
    using Impl = detail::DenseHashTable<Key, Key, Key, detail::ItemInterfaceSet2<Key>, Hash, Eq>;
    Impl impl;

public:
    using const_iterator = typename Impl::const_iterator;
    using iterator = typename Impl::iterator;

    DenseHashSet(size_t buckets = 0)
        : impl(buckets)
    {
    }

    void clear()
    {
        impl.clear();
    }

    const Key& insert(const Key& key)
    {
        impl.rehash_if_full(key);
        return *impl.insert_unsafe(key);
    }

    bool try_insert(const Key& key)
    {
        const size_t oldSize = impl.size();
        insert(key);
        return impl.size() != oldSize;
    }

    const Key* find(const Key& key) const
    {
        return impl.find(key);
    }

    bool contains(const Key& key) const
    {
        return impl.find(key) != 0;
    }

    void erase(const Key& key)
    {
        impl.erase(key);
    }

    size_t size() const
    {
        return impl.size();
    }

    bool empty() const
    {
        return impl.size() == 0;
    }

    const_iterator begin() const
    {
        return impl.begin();
    }

    const_iterator end() const
    {
        return impl.end();
    }

    iterator begin()
    {
        return impl.begin();
    }

    iterator end()
    {
        return impl.end();
    }

    bool operator==(const DenseHashSet<Key, Hash, Eq>& other) const
    {
        if (size() != other.size())
            return false;

        for (const Key& k : *this)
        {
            if (!other.contains(k))
                return false;
        }

        return true;
    }

    bool operator!=(const DenseHashSet<Key, Hash, Eq>& other) const
    {
        return !(*this == other);
    }
};

// This is a faster alternative of unordered_map
template<typename Key, typename Value, typename Hash = detail::DenseHashDefault<Key>, typename Eq = std::equal_to<Key>>
class DenseHashMap
{
    using Impl = detail::DenseHashTable<Key, std::pair<Key, Value>, std::pair<const Key, Value>, detail::ItemInterfaceMap2<Key, Value>, Hash, Eq>;
    Impl impl;

public:
    using const_iterator = typename Impl::const_iterator;
    using iterator = typename Impl::iterator;

    DenseHashMap(size_t buckets = 0)
        : impl(buckets)
    {
    }

    void clear(size_t thresholdToDestroy = 32)
    {
        impl.clear(thresholdToDestroy);
    }

    // Note: this reference is invalidated by any insert operation (i.e. operator[])
    Value& operator[](const Key& key)
    {
        impl.rehash_if_full(key);
        return impl.insert_unsafe(key)->second;
    }

    // Note: this pointer is invalidated by any insert operation (i.e. operator[])
    const Value* find(const Key& key) const
    {
        const std::pair<Key, Value>* result = impl.find(key);

        return result ? &result->second : NULL;
    }

    // Note: this pointer is invalidated by any insert operation (i.e. operator[])
    Value* find(const Key& key)
    {
        const std::pair<Key, Value>* result = impl.find(key);

        return result ? const_cast<Value*>(&result->second) : NULL;
    }

    bool contains(const Key& key) const
    {
        return impl.find(key) != 0;
    }

    void erase(const Key& key)
    {
        impl.erase(key);
    }

    std::pair<Value&, bool> try_insert(const Key& key, const Value& value)
    {
        impl.rehash_if_full(key);

        size_t before = impl.size();
        std::pair<Key, Value>* slot = impl.insert_unsafe(key);

        // Value is fresh if container count has increased
        bool fresh = impl.size() > before;

        if (fresh)
            slot->second = value;

        return std::make_pair(std::ref(slot->second), fresh);
    }

    std::pair<Value&, bool> try_insert(const Key& key, Value&& value)
    {
        impl.rehash_if_full(key);

        size_t before = impl.size();
        std::pair<Key, Value>* slot = impl.insert_unsafe(key);

        // Value is fresh if container count has increased
        bool fresh = impl.size() > before;

        if (fresh)
            slot->second = std::move(value);

        return std::make_pair(std::ref(slot->second), fresh);
    }

    size_t size() const
    {
        return impl.size();
    }

    bool empty() const
    {
        return impl.size() == 0;
    }

    const_iterator begin() const
    {
        return impl.begin();
    }

    const_iterator end() const
    {
        return impl.end();
    }

    iterator begin()
    {
        return impl.begin();
    }

    iterator end()
    {
        return impl.end();
    }
};

} // namespace Luau
