// A ScriptResolver over the tool's enumerated inputs: finds another script's
// compiled file by name (case-insensitive), loose files beating archive
// copies and later items beating earlier ones, loads it on first use and
// keeps it. Thread-safe.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "inputs.hpp"
#include "papyrus/decompile.hpp"

#include <algorithm>
#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace pextool {

class ItemResolver : public papyrus::pex::ScriptResolver {
public:
    // Adds items; a later call with the same name overrides an earlier one
    // unless the earlier one is loose and the new one is from an archive.
    void add(const std::vector<Item>& items) {
        for (const Item& it : items) {
            std::string key = it.name;
            if (key.size() > 4) key.resize(key.size() - 4);  // drop .pex
            std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return char(std::tolower(c)); });
            auto found = m_items.find(key);
            if (found != m_items.end() && found->second.archive.empty() && !it.archive.empty()) continue;
            m_items[key] = it;
        }
    }

    const papyrus::pex::Script* find(std::string_view name) override {
        std::string key(name);
        std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        std::lock_guard<std::mutex> lk(m_mu);
        auto cached = m_cache.find(key);
        if (cached != m_cache.end()) return cached->second.get();
        auto it = m_items.find(key);
        std::unique_ptr<papyrus::pex::Script> loaded;
        if (it != m_items.end()) {
            std::vector<uint8_t> bytes;
            std::string err;
            auto s = std::make_unique<papyrus::pex::Script>();
            if (m_loader.load(it->second, bytes, err) &&
                papyrus::pex::read(bytes.data(), bytes.size(), *s, &err) == papyrus::pex::Status::Ok) {
                loaded = std::move(s);
            }
        }
        const papyrus::pex::Script* result = loaded.get();
        m_cache[key] = std::move(loaded);  // a null entry remembers a miss
        return result;
    }

    size_t size() const { return m_items.size(); }

private:
    std::map<std::string, Item> m_items;
    std::map<std::string, std::unique_ptr<papyrus::pex::Script>> m_cache;
    Loader m_loader;
    std::mutex m_mu;
};

}  // namespace pextool
