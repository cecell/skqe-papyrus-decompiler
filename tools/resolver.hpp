// A ScriptResolver over the tool's enumerated inputs: finds another script's
// compiled file by name (case-insensitive), loose files beating archive
// copies and later items beating earlier ones, loads it on first use and
// keeps it. Thread-safe; loading happens outside the lock so workers do not
// wait for one another's reads (two threads may load the same script at
// once; the first result is kept).
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "inputs.hpp"
#include "papyrus/decompile.hpp"

#include <algorithm>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace pextool {

class ItemResolver : public papyrus::pex::ScriptResolver {
public:
    // Adds items; a later call with the same name overrides an earlier one
    // unless the earlier one is loose and the new one is from an archive.
    void add(const std::vector<Item>& items) {
        for (const Item& it : items) {
            std::string key = it.name;
            if (key.size() > 4) key.resize(key.size() - 4);  // drop .pex
            lower_inplace(key);
            auto found = m_items.find(key);
            if (found != m_items.end() && found->second.archive.empty() && !it.archive.empty()) continue;
            m_items[key] = it;
        }
    }

    const papyrus::pex::Script* find(std::string_view name) override {
        std::string key(name);
        lower_inplace(key);
        const Item* item = nullptr;
        {
            std::lock_guard<std::mutex> lk(m_mu);
            auto cached = m_cache.find(key);
            if (cached != m_cache.end()) return cached->second.get();
            auto it = m_items.find(key);
            if (it == m_items.end()) {
                m_cache.emplace(key, nullptr);  // remember the miss
                return nullptr;
            }
            item = &it->second;
        }
        // Load without the lock; one Loader per thread keeps archive handles.
        thread_local Loader loader;
        std::unique_ptr<papyrus::pex::Script> loaded;
        std::vector<uint8_t> bytes;
        std::string err;
        auto s = std::make_unique<papyrus::pex::Script>();
        if (loader.load(*item, bytes, err) &&
            papyrus::pex::read(bytes.data(), bytes.size(), *s, &err) == papyrus::pex::Status::Ok) {
            loaded = std::move(s);
        }
        std::lock_guard<std::mutex> lk(m_mu);
        auto [pos, inserted] = m_cache.emplace(key, std::move(loaded));
        return pos->second.get();
    }

    // The item a name resolves to, or null when the name is not in the set.
    const Item* item_of(std::string_view name) {
        std::string key(name);
        lower_inplace(key);
        std::lock_guard<std::mutex> lk(m_mu);
        auto it = m_items.find(key);
        return it == m_items.end() ? nullptr : &it->second;
    }

    size_t size() const { return m_items.size(); }

private:
    static void lower_inplace(std::string& s) {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    }

    std::unordered_map<std::string, Item> m_items;
    std::unordered_map<std::string, std::unique_ptr<papyrus::pex::Script>> m_cache;
    std::mutex m_mu;
};

}  // namespace pextool
