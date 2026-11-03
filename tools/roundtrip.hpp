// pex check --roundtrip: decompile a set of scripts, compile the result with
// Bethesda's PapyrusCompiler, compare each result with its original at model
// level, and score. The decompiled set is its own import folder, so a
// dependency never counts against the decompiler because its source was
// missing or stale. docs/ROUNDTRIP.md.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "inputs.hpp"
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
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace pextool {

namespace fs = std::filesystem;

struct RoundtripOptions {
    fs::path tools;                 // the game's "Papyrus Compiler" folder
    fs::path flags;                 // TESV_Papyrus_Flags.flg (found under --import or the game when empty)
    std::vector<fs::path> imports;  // extra import folders, after the decompiled set
    std::vector<Item> resolveItems; // extra compiled scripts the decompiler may consult
    fs::path work;
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
inline const char* kCategories[] = {"identical",     "identical (temps renumbered)", "decompile failed",
                                    "compile error", "missing import",              "structure diff",
                                    "instruction diff", "locals diff",              "debug diff",
                                    "other diff",    "unreadable"};

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
    const fs::path work = o.work.empty() ? fs::temp_directory_path(ec) / ("pex-roundtrip-" + std::to_string(::_getpid())) : o.work;
    const fs::path srcDir = work / "src";
    const fs::path outDir = work / "out";
    fs::remove_all(work, ec);
    fs::create_directories(srcDir, ec);
    fs::create_directories(outDir, ec);
    if (ec) {
        std::fprintf(stderr, "cannot create %s\n", work.string().c_str());
        return 2;
    }
    fs::copy_file(flags, srcDir / flags.filename(), fs::copy_options::overwrite_existing, ec);

    const auto t0 = std::chrono::steady_clock::now();

    ItemResolver resolver;
    resolver.add(o.resolveItems);
    resolver.add(items);  // the set itself has priority over the extra ones

    // Phase 1: decompile everything into the shared source folder.
    struct Entry {
        std::string name;    // object name (file stem)
        std::string category;
        std::string detail;
        bool decompiled = false;
        pex::Script original;
    };
    std::vector<Entry> entries(items.size());
    std::mutex mu;
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
        pex::DecompileOptions dopt;
        dopt.optionalObjectParams = true;  // the set must compile against itself (docs/ROUNDTRIP.md)
        dopt.resolver = &resolver;
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
        std::ofstream f(srcDir / fs::u8path(e.name + ".psc"), std::ios::binary | std::ios::trunc);
        f.write(r.source.data(), static_cast<std::streamsize>(r.source.size()));
        e.decompiled = static_cast<bool>(f);
    });
    const double tDecompile = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    // Phase 2: compile each one and compare.
    std::string importArg = srcDir.string();
    for (const fs::path& i : o.imports) importArg += ";" + i.string();
    const std::string flagsArg = "-f=" + flags.filename().string();

    std::atomic<size_t> done{0};
    for_each_parallel(items, o.threads, [&](size_t i, const Item&, Loader&) {
        Entry& e = entries[i];
        if (!e.decompiled) return;  // unreadable or could not write
        const fs::path myOut = outDir / std::to_string(i);
        std::error_code e2;
        fs::create_directories(myOut, e2);
        const std::string src = (srcDir / fs::u8path(e.name + ".psc")).string();
        const RunResult r = run(compiler, {src, flagsArg, "-i=" + importArg, "-o=" + myOut.string(), "-q"}, srcDir);
        const fs::path produced = myOut / fs::u8path(e.name + ".pex");
        if (!r.started) {
            e.category = "compile error";
            e.detail = "compiler did not start";
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
                e.category = missing ? "missing import" : "compile error";
                e.detail = line.empty() ? "(no message)" : line;
            } else {
                e.detail += " | compile: " + line;
            }
            return;
        }
        pex::Script recompiled;
        std::string err;
        if (pex::read_file(produced, recompiled, &err) != pex::Status::Ok) {
            e.category = "compile error";
            e.detail = "compiler output unreadable: " + err;
            return;
        }
        std::vector<pex::Difference> diffs;
        pex::CompareOptions co;
        if (pex::canonical_equal(e.original, recompiled, &diffs, co)) {
            if (e.category.empty()) e.category = "identical";
            else e.detail += " | recompiles identical anyway";
            if (!o.keep) fs::remove_all(myOut, e2);
            return;
        }
        co.renumberTemps = true;
        std::vector<pex::Difference> diffs2;
        if (pex::canonical_equal(e.original, recompiled, &diffs2, co)) {
            if (e.category.empty()) e.category = "identical (temps renumbered)";
            if (!o.keep) fs::remove_all(myOut, e2);
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
        }
        ++done;
    });
    const double tAll = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    std::map<std::string, size_t> tally;
    size_t identical = 0;
    for (size_t i = 0; i < entries.size(); ++i) {
        const Entry& e = entries[i];
        ++tally[e.category];
        if (e.category == "identical" || e.category == "identical (temps renumbered)") {
            ++identical;
            continue;
        }
        if (!o.quiet) std::fprintf(stdout, "%s: %s: %s\n", items[i].label.c_str(), e.category.c_str(), e.detail.c_str());
    }
    for (const char* c : kCategories) {
        auto it = tally.find(c);
        if (it != tally.end()) std::fprintf(stdout, "  %-30s %zu\n", c, it->second);
    }
    std::fprintf(stdout, "scripts %zu  identical %zu (%.2f%%)  failed functions %zu  decompile %.1fs  total %.1fs\n",
                 items.size(), identical, items.empty() ? 0.0 : 100.0 * double(identical) / double(items.size()),
                 failedFunctions.load(), tDecompile, tAll);
    if (!o.keep) fs::remove_all(work, ec);
    return identical == items.size() ? 0 : 1;
}

}  // namespace pextool
