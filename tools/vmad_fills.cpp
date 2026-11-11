// SPDX-License-Identifier: GPL-3.0-only
#include "vmad_fills.hpp"

#include "db_build.h"        // skqe: LoadOrder
#include "pattern_engine.h"  // skqe: PatternCatalog
#include "plugin.h"          // skqe: Plugin

#include <algorithm>
#include <atomic>
#include <cctype>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>
#include <vector>

namespace pextool {
namespace fs = std::filesystem;

namespace {

std::string lowered(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

// A decoded VMAD holds script entries [name, status, property count,
// properties], each property [name, type, status, value]; quest aliases and
// fragment blocks nest more of them. Walk the whole value and take every
// entry's filled property names (a property whose status is Removed is not
// filled).
void collect(const skqe::Value& v, std::map<std::string, std::set<std::string>>& out, size_t& entries) {
    if (v.kind() != skqe::Value::Kind::List) return;
    const auto& it = v.Items();
    static const std::set<std::string> kStatus = {"Local", "Inherited", "Removed", "Inherited and Removed"};
    if (it.size() >= 4 && it[0].kind() == skqe::Value::Kind::String && it[1].kind() == skqe::Value::Kind::String &&
        kStatus.count(it[1].AsString()) && it[3].kind() == skqe::Value::Kind::List) {
        ++entries;
        std::set<std::string>& props = out[lowered(it[0].AsString())];
        for (const skqe::Value& p : it[3].Items()) {
            if (p.kind() != skqe::Value::Kind::List || p.Items().size() < 3) continue;
            const auto& pi = p.Items();
            if (pi[0].kind() != skqe::Value::Kind::String) continue;
            if (pi[2].kind() == skqe::Value::Kind::String && pi[2].AsString().find("Removed") != std::string::npos) continue;
            props.insert(lowered(pi[0].AsString()));
        }
        return;
    }
    for (const skqe::Value& x : it) collect(x, out, entries);
}

}  // namespace

bool load_vmad_fills(const fs::path& data, const fs::path& pluginsTxt, const fs::path& patterns, unsigned threads,
                     papyrus::pex::AuditIndex& index, FillsReport& report, std::string& why) {
    std::error_code ec;
    std::ifstream in(patterns, std::ios::binary);
    if (!in) {
        why = "cannot read the plugin catalog " + patterns.u8string() + " (patterns.json belongs next to pex.exe)";
        return false;
    }
    std::stringstream text;
    text << in.rdbuf();
    skqe::PatternCatalog catalog;
    std::string err;
    if (!catalog.Load(text.str(), &err)) {
        why = "the plugin catalog " + patterns.u8string() + ": " + err;
        return false;
    }
    // The record types that can carry scripts.
    std::set<std::string> vmadTypes;
    for (const std::string& k : catalog.Keys()) {
        const size_t dot = k.find('.');
        if (dot != std::string::npos && k.substr(dot + 1) == "VMAD") vmadTypes.insert(k.substr(0, dot));
    }

    skqe::BuildOptions lo;
    lo.data = data.wstring();
    lo.loadOrderFile = pluginsTxt.wstring();
    const std::vector<std::string> order = skqe::LoadOrder(lo, nullptr, &err);
    if (!err.empty()) {
        why = "the load order: " + err;
        return false;
    }
    if (order.empty()) {
        why = "the load order has no plugin that exists in " + data.u8string();
        return false;
    }

    std::mutex mu;
    std::atomic<size_t> next{0}, readCount{0}, entryCount{0};
    std::vector<std::string> failed;
    auto worker = [&]() {
        std::map<std::string, std::set<std::string>> local;
        size_t entries = 0;
        for (;;) {
            const size_t i = next++;
            if (i >= order.size()) break;
            std::string perr;
            std::unique_ptr<skqe::Plugin> p = skqe::Plugin::Open((data / fs::u8path(order[i])).wstring(), catalog, &perr);
            if (!p) {
                std::lock_guard<std::mutex> lk(mu);
                failed.push_back(order[i] + ": " + perr);
                continue;
            }
            ++readCount;
            const skqe::Plugin::SigFilter wanted = [](const std::string& sig) { return sig == "VMAD"; };
            for (const skqe::Record& r : p->records()) {
                if (r.flags & 0x20) continue;  // deleted
                if (!vmadTypes.count(r.type)) continue;
                for (const skqe::Field& f : p->Fields(r, nullptr, &wanted)) {
                    if (f.sig == "VMAD" && f.decoded) collect(f.value, local, entries);
                }
            }
        }
        std::lock_guard<std::mutex> lk(mu);
        entryCount += entries;
        for (auto& [k, v] : local) index.filled[k].merge(v);
    };
    unsigned n = threads ? threads : std::max(1u, std::thread::hardware_concurrency());
    n = std::min<unsigned>(n, static_cast<unsigned>(order.size()));
    std::vector<std::thread> pool;
    for (unsigned t = 0; t < n; ++t) pool.emplace_back(worker);
    for (std::thread& t : pool) t.join();

    index.fillsKnown = true;
    report.plugins = readCount;
    report.attachments = entryCount;
    report.scripts = index.filled.size();
    if (!failed.empty()) {
        why = std::to_string(failed.size()) + " plugin(s) could not be read and count as filling nothing: " + failed.front();
    }
    return true;
}

}  // namespace pextool
