// Input enumeration for the pex tool: a loose file, a directory (every .pex
// under it plus every .pex inside every .bsa under it) or a single .bsa.
// Items are processed on worker threads; results are reported in input order.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "bsa/reader.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace pextool {

namespace fs = std::filesystem;

struct Item {
    std::string label;  // "C:\...\Foo.pex" or "Archive.bsa:scripts\foo.pex"
    std::string name;   // "foo.pex" (file name only)
    fs::path archive;   // empty for a loose file
    fs::path file;      // loose file path, or the archive path
    std::string entryPath;  // inside the archive
};

inline bool iequals_ext(const fs::path& p, const char* ext) {
    std::string e = p.extension().string();
    std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return e == ext;
}

// Enumerates the items of one input argument. Returns false when the input
// does not exist.
inline bool enumerate(const fs::path& input, std::vector<Item>& items, std::vector<std::string>& warnings) {
    std::error_code ec;
    if (!fs::exists(input, ec)) return false;

    auto add_archive = [&](const fs::path& bsa) {
        bsa::Reader r;
        const bsa::Status st = r.open(bsa);
        if (st != bsa::Status::Ok) {
            warnings.push_back(bsa.string() + ": " + bsa::to_string(st));
            return;
        }
        const uint32_t n = r.header().fileCount;
        for (uint32_t i = 0; i < n; ++i) {
            bsa::Entry e;
            if (r.entry(i, e) != bsa::Status::Ok) continue;
            std::string_view name, folder;
            if (r.file_name(e, name) != bsa::Status::Ok) continue;
            if (name.size() < 4) continue;
            std::string lower(name);
            std::transform(lower.begin(), lower.end(), lower.begin(),
                           [](unsigned char c) { return char(std::tolower(c)); });
            if (lower.compare(lower.size() - 4, 4, ".pex") != 0) continue;
            r.folder_name(e.folderIndex, folder);
            Item it;
            it.archive = bsa;
            it.file = bsa;
            it.entryPath = std::string(folder) + "\\" + std::string(name);
            it.name = std::string(name);
            it.label = bsa.filename().string() + ":" + it.entryPath;
            items.push_back(std::move(it));
        }
    };

    if (fs::is_directory(input, ec)) {
        std::vector<fs::path> archives;
        for (fs::recursive_directory_iterator it(input, fs::directory_options::skip_permission_denied, ec), end;
             it != end; it.increment(ec)) {
            if (ec) break;
            if (!it->is_regular_file(ec)) continue;
            const fs::path& p = it->path();
            if (iequals_ext(p, ".pex")) {
                Item item;
                item.file = p;
                item.name = p.filename().string();
                item.label = p.string();
                items.push_back(std::move(item));
            } else if (iequals_ext(p, ".bsa")) {
                archives.push_back(p);
            }
        }
        std::sort(archives.begin(), archives.end());
        for (const fs::path& a : archives) add_archive(a);
        return true;
    }
    if (iequals_ext(input, ".bsa")) {
        add_archive(input);
        return true;
    }
    Item item;
    item.file = input;
    item.name = input.filename().string();
    item.label = input.string();
    items.push_back(std::move(item));
    return true;
}

// Keeps one item per script name: a loose file beats an archive copy (the
// game's rule), and among equals the later one wins. For output folders and
// import sets, where two files cannot share a name.
inline std::vector<Item> effective_scripts(const std::vector<Item>& items) {
    std::map<std::string, size_t> byName;
    std::vector<Item> out;
    for (const Item& it : items) {
        std::string key = it.name;
        std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        auto f = byName.find(key);
        if (f == byName.end()) {
            byName[key] = out.size();
            out.push_back(it);
        } else if (!(out[f->second].archive.empty() && !it.archive.empty())) {
            out[f->second] = it;
        }
    }
    return out;
}

// Loads an item's bytes. Archive readers are opened per thread and cached by
// path, so a 14,000-script archive is opened once per worker, not per file.
class Loader {
public:
    bool load(const Item& it, std::vector<uint8_t>& out, std::string& error) {
        if (it.archive.empty()) {
            std::ifstream in(it.file, std::ios::binary);
            if (!in) {
                error = "cannot open";
                return false;
            }
            out.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            return true;
        }
        if (m_openPath != it.archive) {
            m_reader.close();
            const bsa::Status st = m_reader.open(it.archive);
            if (st != bsa::Status::Ok) {
                error = bsa::to_string(st);
                m_openPath.clear();
                return false;
            }
            m_openPath = it.archive;
        }
        bsa::Entry e;
        bsa::Status st = m_reader.find(it.entryPath, e);
        if (st != bsa::Status::Ok) {
            error = bsa::to_string(st);
            return false;
        }
        st = m_reader.read(e, out);
        if (st != bsa::Status::Ok) {
            error = bsa::to_string(st);
            return false;
        }
        return true;
    }

private:
    bsa::Reader m_reader;
    fs::path m_openPath;
};

// Runs fn over every item on `threads` workers. fn receives the item index,
// the item and its loader; it must be thread-safe in what it touches.
inline void for_each_parallel(const std::vector<Item>& items, unsigned threads,
                              const std::function<void(size_t, const Item&, Loader&)>& fn) {
    if (threads == 0) threads = std::max(1u, std::thread::hardware_concurrency());
    threads = std::min<unsigned>(threads, static_cast<unsigned>(std::max<size_t>(1, items.size())));
    std::atomic<size_t> next{0};
    auto worker = [&] {
        Loader loader;
        for (;;) {
            const size_t i = next.fetch_add(1);
            if (i >= items.size()) break;
            fn(i, items[i], loader);
        }
    };
    if (threads == 1) {
        worker();
        return;
    }
    std::vector<std::thread> pool;
    for (unsigned t = 0; t < threads; ++t) pool.emplace_back(worker);
    for (std::thread& t : pool) t.join();
}

}  // namespace pextool
