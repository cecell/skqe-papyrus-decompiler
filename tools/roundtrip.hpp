// pex check --roundtrip: decompile a set of scripts, compile the result with
// Bethesda's PapyrusCompiler, compare each result with its original at model
// level, and score. The decompiled set is its own import folder, so a
// dependency never counts against the decompiler because its source was
// missing or stale. With per-script import sets every script compiles
// against exactly its own dependency closure, each dependency taken from the
// script's own archive first. docs/ROUNDTRIP.md.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "inputs.hpp"
#include "papyrus/analysis.hpp"
#include "papyrus/canon.hpp"
#include "papyrus/decompile.hpp"
#include "papyrus/pex.hpp"
#include "process.hpp"
#include "resolver.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace pextool {

namespace fs = std::filesystem;

struct RoundtripOptions {
    fs::path tools;                 // the game's "Papyrus Compiler" folder
    fs::path flags;                 // TESV_Papyrus_Flags.flg (found under --import or the game when empty)
    std::vector<fs::path> imports;  // extra import folders, after the decompiled set
    std::vector<Item> resolveItems; // extra compiled scripts the decompiler may consult
    fs::path work;
    bool perScript = false;         // one import folder per script, from its closure
    bool keep = false;
    bool quiet = false;
    unsigned threads = 0;
};

inline std::string first_line_matching(const std::string& output, const char* needle) {
    size_t pos = 0;
    while (pos < output.size()) {
        size_t end = output.find('\n', pos);
        if (end == std::string::npos) end = output.size();
        std::string line = output.substr(pos, end - pos);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.find(needle) != std::string::npos) return line;
        pos = end + 1;
    }
    return "";
}

inline std::string lower_copy(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Categories, in the order they are decided for one script.
inline const char* kCategories[] = {"identical",     "identical (temps renumbered)", "duplicate",
                                    "decompile failed", "incomplete closure",       "compile error",
                                    "missing import", "structure diff",             "instruction diff",
                                    "locals diff",   "debug diff",                  "other diff",
                                    "unreadable"};

// Resolves a name inside one origin (the script's own archive, or the loose
// files) before the whole set: a mod's archive is self-consistent even when
// another mod overrides one of its scripts.
class OriginResolver : public papyrus::pex::ScriptResolver {
public:
    OriginResolver(ItemResolver* own, ItemResolver* base) : m_own(own), m_base(base) {}
    const papyrus::pex::Script* find(std::string_view name) override {
        if (m_own) {
            if (const papyrus::pex::Script* s = m_own->find(name)) return s;
        }
        return m_base->find(name);
    }
    const Item* item_of(std::string_view name) {
        if (m_own && m_own->find(name)) return m_own->item_of(name);
        return m_base->item_of(name);
    }

private:
    ItemResolver* m_own;
    ItemResolver* m_base;
};

inline uint64_t fnv1a(const std::vector<uint8_t>& bytes) {
    uint64_t h = 1469598103934665603ULL;
    for (uint8_t b : bytes) h = (h ^ b) * 1099511628211ULL;
    return h;
}

inline std::string origin_key(const Item& it) { return it.archive.empty() ? std::string() : it.archive.string(); }

inline int run_roundtrip(const RoundtripOptions& o, const std::vector<Item>& items) {
    using namespace papyrus;
    std::error_code ec;
    const fs::path compiler = o.tools / "PapyrusCompiler.exe";
    if (!fs::exists(compiler, ec)) {
        std::fprintf(stderr, "%s not found\n", compiler.string().c_str());
        return 2;
    }
    fs::path flags = o.flags;
    if (flags.empty()) {
        // Next to the game (tools = <game>\Papyrus Compiler), then the import folders.
        std::vector<fs::path> candidates = {o.tools.parent_path() / "Data" / "Source" / "Scripts" / "TESV_Papyrus_Flags.flg"};
        for (const fs::path& i : o.imports) candidates.push_back(i / "TESV_Papyrus_Flags.flg");
        for (const fs::path& c : candidates) {
            if (fs::exists(c, ec)) {
                flags = c;
                break;
            }
        }
        if (flags.empty()) {
            std::fprintf(stderr, "TESV_Papyrus_Flags.flg not found; give --flags <file>\n");
            return 2;
        }
    }
    flags = fs::absolute(flags, ec);
    const fs::path work = o.work.empty() ? fs::temp_directory_path(ec) / ("pex-roundtrip-" + std::to_string(::_getpid())) : o.work;
    const fs::path srcDir = work / "src";   // shared mode: the one import folder
    const fs::path depDir = work / "deps";  // per-script mode: one decompiled file per distinct compiled script
    const fs::path impDir = work / "imp";   // per-script mode: one import folder per script
    const fs::path outDir = work / "out";
    fs::remove_all(work, ec);
    fs::create_directories(srcDir, ec);
    fs::create_directories(outDir, ec);
    if (o.perScript) {
        fs::create_directories(depDir, ec);
        fs::create_directories(impDir, ec);
    }
    if (ec) {
        std::fprintf(stderr, "cannot create %s\n", work.string().c_str());
        return 2;
    }
    fs::copy_file(flags, srcDir / flags.filename(), fs::copy_options::overwrite_existing, ec);

    const auto t0 = std::chrono::steady_clock::now();

    // The whole set (extra scripts first, so the set itself wins), and one
    // resolver per origin for the per-script mode.
    ItemResolver base;
    base.add(o.resolveItems);
    base.add(items);
    std::map<std::string, std::unique_ptr<ItemResolver>> origins;
    if (o.perScript) {
        auto group = [&](const std::vector<Item>& v) {
            for (const Item& it : v) {
                std::unique_ptr<ItemResolver>& r = origins[origin_key(it)];
                if (!r) r = std::make_unique<ItemResolver>();
                r->add({it});
            }
        };
        group(o.resolveItems);
        group(items);
    }
    auto origin_of = [&](const Item& it) -> ItemResolver* {
        if (!o.perScript) return nullptr;
        auto f = origins.find(origin_key(it));
        return f == origins.end() ? nullptr : f->second.get();
    };

    // Per-script mode: every distinct compiled file is decompiled once, into
    // its own folder under deps/, and hard-linked into each import folder that
    // needs it.
    struct DepFile {
        std::once_flag once;
        fs::path path;
        bool ok = false;
    };
    std::unordered_map<std::string, std::unique_ptr<DepFile>> depFiles;
    std::mutex depMu;
    auto dep_file = [&](const Item& it, const std::string* source) -> const DepFile& {
        DepFile* df = nullptr;
        size_t index = 0;
        {
            std::lock_guard<std::mutex> lk(depMu);
            std::unique_ptr<DepFile>& slot = depFiles[it.label];
            if (!slot) slot = std::make_unique<DepFile>();
            df = slot.get();
            index = depFiles.size() - 1;
        }
        std::call_once(df->once, [&] {
            std::string text;
            std::string name;
            if (source) {
                text = *source;
            } else {
                std::string key = it.name;
                if (key.size() > 4) key.resize(key.size() - 4);
                OriginResolver res(origin_of(it), &base);
                const pex::Script* ps = res.find(key);
                if (!ps || ps->objects.empty()) return;
                pex::DecompileOptions dopt;
                dopt.optionalObjectParams = true;
                dopt.resolver = &res;
                text = pex::decompile(*ps, dopt).source;
                name = std::string(ps->str(ps->objects[0].name));
            }
            if (name.empty()) {
                name = it.name;
                if (name.size() > 4) name.resize(name.size() - 4);
            }
            std::error_code e2;
            const fs::path dir = depDir / std::to_string(index);
            fs::create_directories(dir, e2);
            df->path = dir / fs::u8path(name + ".psc");
            std::ofstream f(df->path, std::ios::binary | std::ios::trunc);
            f.write(text.data(), static_cast<std::streamsize>(text.size()));
            df->ok = static_cast<bool>(f);
        });
        return *df;
    };

    // Phase 1: decompile everything (shared mode: into the one source folder;
    // per-script mode: into deps/ through the cache, with byte-identical
    // copies of the same script compiled once).
    struct Entry {
        std::string name;    // object name (file stem)
        std::string category;
        std::string detail;
        bool decompiled = false;
        pex::Script original;
    };
    std::vector<Entry> entries(items.size());
    std::unordered_map<uint64_t, size_t> firstByHash;
    std::mutex hashMu;
    std::atomic<size_t> failedFunctions{0};
    for_each_parallel(items, o.threads, [&](size_t i, const Item& it, Loader& ld) {
        Entry& e = entries[i];
        std::vector<uint8_t> bytes;
        std::string err;
        if (!ld.load(it, bytes, err) || pex::read(bytes.data(), bytes.size(), e.original, &err) != pex::Status::Ok) {
            e.category = "unreadable";
            e.detail = err;
            return;
        }
        if (e.original.objects.empty()) {
            e.category = "unreadable";
            e.detail = "no object";
            return;
        }
        e.name = std::string(e.original.str(e.original.objects[0].name));
        if (o.perScript) {
            const uint64_t h = fnv1a(bytes);
            std::lock_guard<std::mutex> lk(hashMu);
            auto [pos, inserted] = firstByHash.emplace(h, i);
            if (!inserted) {
                e.category = "duplicate";
                e.detail = "same bytes as " + items[pos->second].label;
                e.original = pex::Script();
                return;
            }
        }
        OriginResolver res(origin_of(it), &base);
        pex::DecompileOptions dopt;
        dopt.optionalObjectParams = true;  // the set must compile against itself (docs/ROUNDTRIP.md)
        dopt.resolver = &res;
        const pex::DecompileResult r = pex::decompile(e.original, dopt);
        for (const pex::FunctionReport& f : r.functions) {
            if (f.status == pex::FnStatus::Failed) {
                ++failedFunctions;
                if (e.category.empty()) {
                    e.category = "decompile failed";
                    e.detail = (f.state.empty() ? "" : f.state + ".") + f.name + ": " + f.reason + " at " +
                               std::to_string(f.ip);
                }
            }
        }
        if (o.perScript) {
            e.decompiled = dep_file(it, &r.source).ok;
        } else {
            std::ofstream f(srcDir / fs::u8path(e.name + ".psc"), std::ios::binary | std::ios::trunc);
            f.write(r.source.data(), static_cast<std::streamsize>(r.source.size()));
            e.decompiled = static_cast<bool>(f);
        }
    });
    const double tDecompile = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    // Phase 2: compile each one and compare.
    std::string extraImports;
    for (const fs::path& i : o.imports) extraImports += ";" + i.string();
    const std::string sharedImportArg = srcDir.string() + extraImports;
    const std::string flagsArg = o.perScript ? "-f=" + flags.string() : "-f=" + flags.filename().string();
    auto hasSource = [&](const std::string& type, std::string& where) {
        for (const fs::path& dir : o.imports) {
            std::error_code e2;
            const fs::path p = dir / fs::u8path(type + ".psc");
            if (fs::exists(p, e2)) {
                where = p.string();
                return true;
            }
        }
        return false;
    };

    std::atomic<size_t> progress{0};
    const size_t progressStep = items.size() >= 4000 ? 1000 : 0;
    for_each_parallel(items, o.threads, [&](size_t i, const Item& it, Loader&) {
        Entry& e = entries[i];
        struct Progress {
            std::atomic<size_t>& p;
            size_t step, total;
            std::chrono::steady_clock::time_point t0;
            ~Progress() {
                const size_t n = ++p;
                if (step && n % step == 0) {
                    std::fprintf(stderr, "  %zu / %zu  %.0f s\n", n, total,
                                 std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
                }
            }
        } tick{progress, progressStep, items.size(), t0};
        if (!e.decompiled) return;  // unreadable, duplicate, or could not write
        const fs::path myOut = outDir / std::to_string(i);
        std::error_code e2;
        fs::create_directories(myOut, e2);
        std::string src, importArg;
        fs::path cwd;
        std::string memberNote;
        if (o.perScript) {
            OriginResolver res(origin_of(it), &base);
            const pex::Closure c = pex::compute_closure(e.original, res, hasSource, true);
            for (const pex::ClosureEntry& ce : c.entries) {
                if (ce.state != pex::DepState::Missing) continue;
                if (e.category.empty()) e.category = "incomplete closure";
                e.detail += (e.detail.empty() ? "" : " | ") + std::string("missing ") + ce.type +
                            (ce.neededBy.empty() ? "" : " (needed by " + ce.neededBy + ")");
                if (!o.keep) fs::remove_all(myOut, e2);
                return;
            }
            if (!c.missingMembers.empty()) {
                const pex::MissingMember& m = c.missingMembers.front();
                memberNote = "member " + m.type + "." + m.member + ": " + m.reason;
                if (c.missingMembers.size() > 1) memberNote += " (+" + std::to_string(c.missingMembers.size() - 1) + " more)";
            }
            const fs::path myImp = impDir / std::to_string(i);
            fs::create_directories(myImp, e2);
            for (const pex::ClosureEntry& ce : c.entries) {
                if (ce.state != pex::DepState::Pex) continue;
                const bool root = &ce == &c.entries.front();
                const Item* di = root ? &it : res.item_of(ce.type);
                if (!di) continue;
                const DepFile& df = dep_file(*di, nullptr);
                if (!df.ok) continue;
                const fs::path link = myImp / df.path.filename();
                fs::create_hard_link(df.path, link, e2);
                if (e2) {
                    e2.clear();
                    fs::copy_file(df.path, link, fs::copy_options::overwrite_existing, e2);
                }
                if (root) src = link.string();
            }
            importArg = myImp.string() + extraImports;
            cwd = myImp;
        } else {
            src = (srcDir / fs::u8path(e.name + ".psc")).string();
            importArg = sharedImportArg;
            cwd = srcDir;
        }
        const RunResult r = run(compiler, {src, flagsArg, "-i=" + importArg, "-o=" + myOut.string(), "-q"}, cwd);
        const fs::path produced = myOut / fs::u8path(e.name + ".pex");
        auto cleanup = [&] {
            if (o.keep) return;
            fs::remove_all(myOut, e2);
            if (o.perScript) fs::remove_all(impDir / std::to_string(i), e2);
        };
        if (!r.started) {
            e.category = "compile error";
            e.detail = "compiler did not start";
            cleanup();
            return;
        }
        if (r.exitCode != 0 || !fs::exists(produced, e2)) {
            std::string line = first_line_matching(r.output, "): ");
            if (line.empty()) line = first_line_matching(r.output, "error");
            const std::string low = lower_copy(line);
            const bool missing = low.find("unknown type") != std::string::npos ||
                                 low.find("is not a known") != std::string::npos ||
                                 low.find("unable to find") != std::string::npos ||
                                 low.find("cannot be found") != std::string::npos ||
                                 low.find("could not be found") != std::string::npos ||
                                 low.find("no viable alternative") == std::string::npos && low.find("not a function or does not exist") != std::string::npos;
            if (e.category.empty()) {
                e.category = !memberNote.empty() ? "incomplete closure" : missing ? "missing import" : "compile error";
                e.detail = (memberNote.empty() ? "" : memberNote + " | compile: ") + (line.empty() ? "(no message)" : line);
            } else {
                e.detail += " | compile: " + line;
            }
            cleanup();
            return;
        }
        pex::Script recompiled;
        std::string err;
        if (pex::read_file(produced, recompiled, &err) != pex::Status::Ok) {
            e.category = "compile error";
            e.detail = "compiler output unreadable: " + err;
            cleanup();
            return;
        }
        std::vector<pex::Difference> diffs;
        pex::CompareOptions co;
        co.compareHeader = false;  // a 3.1 original (LE compiler) recompiles as 3.2; the body is what is scored
        if (pex::canonical_equal(e.original, recompiled, &diffs, co)) {
            if (e.category.empty()) e.category = "identical";
            else e.detail += " | recompiles identical anyway";
            cleanup();
            return;
        }
        co.renumberTemps = true;
        std::vector<pex::Difference> diffs2;
        if (pex::canonical_equal(e.original, recompiled, &diffs2, co)) {
            if (e.category.empty()) e.category = "identical (temps renumbered)";
            cleanup();
            return;
        }
        if (e.category.empty()) {
            const pex::Difference& d = diffs2.empty() ? diffs.front() : diffs2.front();
            switch (d.kind) {
            case pex::DiffKind::Instruction: e.category = "instruction diff"; break;
            case pex::DiffKind::Locals: e.category = "locals diff"; break;
            case pex::DiffKind::DebugInfo: e.category = "debug diff"; break;
            case pex::DiffKind::Object:
            case pex::DiffKind::Variable:
            case pex::DiffKind::Property:
            case pex::DiffKind::Function: e.category = "structure diff"; break;
            default: e.category = "other diff"; break;
            }
            e.detail = std::string(pex::to_string(d.kind)) + " " + d.where + ": " + d.message;
            if (diffs2.size() > 1) e.detail += " (+" + std::to_string(diffs2.size() - 1) + " more)";
            if (!memberNote.empty()) e.detail += " | " + memberNote;
        }
        if (!o.keep && o.perScript) fs::remove_all(impDir / std::to_string(i), e2);
    });
    const double tAll = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    std::map<std::string, size_t> tally;
    size_t identical = 0, duplicates = 0;
    for (size_t i = 0; i < entries.size(); ++i) {
        const Entry& e = entries[i];
        ++tally[e.category];
        if (e.category == "identical" || e.category == "identical (temps renumbered)") {
            ++identical;
            continue;
        }
        if (e.category == "duplicate") {
            ++duplicates;
            continue;
        }
        if (!o.quiet) std::fprintf(stdout, "%s: %s: %s\n", items[i].label.c_str(), e.category.c_str(), e.detail.c_str());
    }
    for (const char* c : kCategories) {
        auto it = tally.find(c);
        if (it != tally.end()) std::fprintf(stdout, "  %-30s %zu\n", c, it->second);
    }
    const size_t scored = items.size() - duplicates;
    std::fprintf(stdout, "scripts %zu%s  identical %zu (%.2f%%)  failed functions %zu  decompile %.1fs  total %.1fs\n",
                 scored, duplicates ? (" (+" + std::to_string(duplicates) + " duplicates)").c_str() : "", identical,
                 scored == 0 ? 0.0 : 100.0 * double(identical) / double(scored), failedFunctions.load(), tDecompile, tAll);
    if (!o.keep) fs::remove_all(work, ec);
    return identical == scored ? 0 : 1;
}

}  // namespace pextool
