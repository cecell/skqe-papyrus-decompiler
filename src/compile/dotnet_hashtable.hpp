// The order in which Bethesda's compiler writes state blocks into its
// listing: the enumeration order of a .NET 2.0 System.Collections.Hashtable
// (64-bit runtime) keyed by the lowercased state names, inserted in source
// order. That order is the table's bucket order, which depends on the keys' hash
// codes, the insertion order and the table's growth. That order decides the
// spelling of every string a state block shares with earlier text (the
// assembler keeps the first spelling it meets) and the order of the debug
// records. Validated against the real runtime on random key sets and on all
// 253 vanilla scripts with two or more states (docs/COMPILER_PATTERNS.md).
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace papyrus::dotnet {

// String.GetHashCode of the 64-bit .NET Framework CLR (2.0 through 4.x,
// unrandomized): two characters per round into two accumulators. Papyrus
// identifiers are ASCII, so bytes stand in for UTF-16 code units.
inline int32_t string_hash_64(std::string_view s) {
    uint32_t h1 = 5381, h2 = 5381;
    size_t i = 0;
    while (i < s.size()) {
        h1 = ((h1 << 5) + h1) ^ static_cast<unsigned char>(s[i]);
        if (i + 1 >= s.size()) break;
        h2 = ((h2 << 5) + h2) ^ static_cast<unsigned char>(s[i + 1]);
        i += 2;
    }
    return static_cast<int32_t>(h1 + h2 * 1566083941u);
}

inline bool is_prime_(size_t n) {
    if (n < 2) return false;
    for (size_t d = 2; d * d <= n; ++d) {
        if (n % d == 0) return false;
    }
    return true;
}

// HashHelpers.GetPrime of .NET 2.0: its table starts at 11 and is dense
// from there, so a new Hashtable() has 11 buckets and grows 11, 23, 47, 97...
inline size_t prime_at_least_(size_t minimum) {
    size_t p = minimum < 11 ? 11 : minimum;
    while (!is_prime_(p)) ++p;
    return p;
}

// Enumeration order of a Hashtable's keys after inserting `keys` in order
// (lowercased; a repeated key is an assignment and keeps its bucket).
// Load factor 0.72, double hashing with incr = 1 + ((hash >> 5) + 1) % (n - 1),
// expansion before an insert once count reaches (int)(0.72f * n), rehash in
// ascending bucket order, enumeration in descending bucket order.
inline std::vector<std::string> hashtable20_key_order(const std::vector<std::string>& keys) {
    struct Bucket {
        std::string key;
        uint32_t hash = 0;
        bool used = false;
    };
    auto put = [](std::vector<Bucket>& b, const std::string& key, uint32_t hash) {
        const size_t n = b.size();
        const uint32_t incr = 1 + (((hash >> 5) + 1) % static_cast<uint32_t>(n - 1));
        size_t idx = hash % n;
        for (size_t tries = 0; tries <= n; ++tries) {
            if (!b[idx].used || b[idx].key == key) {
                b[idx] = Bucket{key, hash, true};
                return;
            }
            idx = (idx + incr) % n;
        }
    };
    std::vector<Bucket> buckets(prime_at_least_(0));
    size_t loadsize = static_cast<size_t>(0.72f * static_cast<float>(buckets.size()));
    size_t count = 0;
    for (const std::string& raw : keys) {
        std::string key = raw;
        for (char& c : key) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        bool exists = false;
        for (const Bucket& b : buckets) exists = exists || (b.used && b.key == key);
        if (count >= loadsize) {
            std::vector<Bucket> grown(prime_at_least_(buckets.size() * 2));
            for (const Bucket& b : buckets) {
                if (b.used) put(grown, b.key, b.hash);
            }
            buckets.swap(grown);
            loadsize = static_cast<size_t>(0.72f * static_cast<float>(buckets.size()));
        }
        put(buckets, key, static_cast<uint32_t>(string_hash_64(key)) & 0x7FFFFFFFu);
        if (!exists) ++count;
    }
    std::vector<std::string> order;
    for (size_t i = buckets.size(); i-- > 0;) {
        if (buckets[i].used) order.push_back(buckets[i].key);
    }
    return order;
}

}  // namespace papyrus::dotnet
