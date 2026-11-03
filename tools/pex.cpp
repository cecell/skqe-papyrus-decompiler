// pex: command line front end of skqe-papyrus. Every command takes a .pex
// file, a directory (loose scripts and every .bsa below it) or one .bsa.
// Exit codes: 0 ok, 1 an input failed or a check found problems, 2 bad
// arguments.
// SPDX-License-Identifier: GPL-3.0-only
#include "inputs.hpp"
#include "papyrus/canon.hpp"
#include "papyrus/decompile.hpp"
#include "papyrus/pex.hpp"
#include "process.hpp"
#include "resolver.hpp"
#include "roundtrip.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

using namespace papyrus;
namespace fs = std::filesystem;

namespace {

const char* kHelp = R"(pex - compiled Papyrus script (.pex) tool, Skyrim LE/SE

usage: pex <command> <input> [options]

  <input> is a .pex file, a directory (every .pex below it, loose and inside
  every .bsa) or a single .bsa.

commands
  info <input>                 header facts and counts (--json)
  json <input> [--out <path>]  the whole model as JSON
  asm <input> [--out <path>]   Papyrus assembly, as PapyrusAssembler reads it
  decompile <input> [--out <path>] [--resolve <dir|bsa>] [--comments]
            [--keep-generated] [--optional-objects]
                               Papyrus source; functions that cannot be
                               structured are reported and emitted as stubs.
                               The decompiler consults the other scripts of
                               the input and of --resolve (usually the Data
                               folder) for parent chains and declarations.
                               --optional-objects declares object parameters
                               "= None" (for import sets that must compile
                               against each other, see docs/ROUNDTRIP.md)
  check --rewrite <input>      read + write every script, compare byte for byte
  check --asm <input> --tools <dir>
                               assemble our listing with PapyrusAssembler.exe
                               from <dir> (the game's "Papyrus Compiler"
                               folder) and compare the result with the
                               original at model level
  check --roundtrip <input> --tools <dir> [--import <dir;dir>] [--flags <flg>]
            [--resolve <dir|bsa>]
                               decompile every script, compile the result
                               with PapyrusCompiler.exe (the decompiled set
                               is its own import folder, then --import) and
                               compare with the original at model level;
                               prints the share that comes back identical
  help

  For json and asm, --out is a file for one script and a directory for many
  (one <name>.json / <name>.pas each); without it one script goes to stdout.

common options
  --threads N                  worker threads (default: all cores)
  --quiet                      only the summary (checks)
  --work <dir>                 scratch folder for checks that run tools
  --keep                       keep the scratch files

exit codes: 0 ok, 1 an input failed or a check found problems, 2 bad arguments
)";

struct Options {
    unsigned threads = 0;
    bool quiet = false;
    bool json = false;
    bool keep = false;
    bool rewrite = false;
    bool asmCheck = false;
    bool roundtrip = false;
    std::string imports;
    std::string flags;
    std::vector<std::string> resolve;
    bool comments = false;
    bool keepGenerated = false;
    bool optionalObjects = false;
    std::string out;
    std::string tools;
    std::string work;
    std::vector<std::string> inputs;
};

bool parse(int argc, char** argv, int from, Options& o, const char* cmd) {
    for (int i = from; i < argc; ++i) {
        const std::string a = argv[i];
        const bool hasNext = i + 1 < argc;
        if (a == "--threads" && hasNext) o.threads = static_cast<unsigned>(std::atoi(argv[++i]));
        else if (a == "--quiet") o.quiet = true;
        else if (a == "--json") o.json = true;
        else if (a == "--keep") o.keep = true;
        else if (a == "--rewrite") o.rewrite = true;
        else if (a == "--asm") o.asmCheck = true;
        else if (a == "--roundtrip") o.roundtrip = true;
        else if (a == "--import" && hasNext) o.imports = argv[++i];
        else if (a == "--flags" && hasNext) o.flags = argv[++i];
        else if (a == "--resolve" && hasNext) o.resolve.push_back(argv[++i]);
        else if (a == "--comments") o.comments = true;
        else if (a == "--keep-generated") o.keepGenerated = true;
        else if (a == "--optional-objects") o.optionalObjects = true;
        else if (a == "--out" && hasNext) o.out = argv[++i];
        else if (a == "--tools" && hasNext) o.tools = argv[++i];
        else if (a == "--work" && hasNext) o.work = argv[++i];
        else if (a.size() > 1 && a[0] == '-') {
            std::fprintf(stderr, "%s: unknown option %s (pex help)\n", cmd, a.c_str());
            return false;
        } else {
            o.inputs.push_back(a);
        }
    }
    return true;
}

bool collect(const Options& o, std::vector<pextool::Item>& items) {
    if (o.inputs.empty()) {
        std::fprintf(stderr, "no input given\n");
        return false;
    }
    std::vector<std::string> warnings;
    for (const std::string& in : o.inputs) {
        if (!pextool::enumerate(fs::u8path(in), items, warnings)) {
            std::fprintf(stderr, "%s: does not exist\n", in.c_str());
            return false;
        }
    }
    for (const std::string& w : warnings) std::fprintf(stderr, "warning: %s\n", w.c_str());
    return true;
}

// The extra compiled scripts named by --resolve.
bool collect_resolve(const Options& o, std::vector<pextool::Item>& items) {
    std::vector<std::string> warnings;
    for (const std::string& in : o.resolve) {
        if (!pextool::enumerate(fs::u8path(in), items, warnings)) {
            std::fprintf(stderr, "--resolve %s: does not exist\n", in.c_str());
            return false;
        }
    }
    for (const std::string& w : warnings) std::fprintf(stderr, "warning: %s\n", w.c_str());
    return true;
}

std::string format_time(uint64_t t) {
    if (t == 0) return "0";
    std::time_t tt = static_cast<std::time_t>(t);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &tt);
#else
    gmtime_r(&tt, &tm);
#endif
    char buf[40];
    std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S UTC", &tm);
    return buf;
}

struct Counts {
    size_t functions = 0, instructions = 0, properties = 0, variables = 0, states = 0;
};

Counts count(const pex::Script& s) {
    Counts c;
    for (const pex::Object& o : s.objects) {
        c.properties += o.properties.size();
        c.variables += o.variables.size();
        c.states += o.states.size();
        for (const pex::Property& p : o.properties) {
            if (p.getter) {
                ++c.functions;
                c.instructions += p.getter->code.size();
            }
            if (p.setter) {
                ++c.functions;
                c.instructions += p.setter->code.size();
            }
        }
        for (const pex::State& st : o.states) {
            c.functions += st.functions.size();
            for (const pex::NamedFunction& f : st.functions) c.instructions += f.body.code.size();
        }
    }
    return c;
}

// Loads and parses one item; on failure prints the reason (under the lock).
bool load_script(const pextool::Item& it, pextool::Loader& ld, pex::Script& s, std::mutex& mu,
                 std::vector<uint8_t>* bytesOut = nullptr) {
    std::vector<uint8_t> bytes;
    std::string err;
    pex::Status st = pex::Status::IoError;
    if (ld.load(it, bytes, err)) st = pex::read(bytes.data(), bytes.size(), s, &err);
    if (st != pex::Status::Ok) {
        std::lock_guard<std::mutex> lk(mu);
        std::fprintf(stderr, "%s: %s: %s\n", it.label.c_str(), pex::to_string(st), err.c_str());
        return false;
    }
    if (bytesOut) *bytesOut = std::move(bytes);
    return true;
}

// --- info -----------------------------------------------------------------

int cmd_info(int argc, char** argv) {
    Options o;
    if (!parse(argc, argv, 2, o, "info")) return 2;
    std::vector<pextool::Item> items;
    if (!collect(o, items)) return 2;

    std::vector<std::string> lines(items.size());
    std::atomic<int> failures{0};
    std::mutex mu;
    const bool single = items.size() == 1;
    pextool::for_each_parallel(items, o.threads, [&](size_t i, const pextool::Item& it, pextool::Loader& ld) {
        pex::Script s;
        if (!load_script(it, ld, s, mu)) {
            ++failures;
            return;
        }
        const Counts n = count(s);
        const pex::Header& h = s.header;
        char buf[512];
        std::string& out = lines[i];
        if (o.json) {
            std::snprintf(buf, sizeof buf,
                          "{\"file\":\"%s\",\"version\":\"%u.%u\",\"gameId\":%u,\"compileTime\":%llu,"
                          "\"source\":\"%s\",\"user\":\"%s\",\"computer\":\"%s\",\"debug\":%s,\"strings\":%zu,"
                          "\"objects\":%zu,\"variables\":%zu,\"properties\":%zu,\"states\":%zu,\"functions\":%zu,"
                          "\"instructions\":%zu,\"notes\":%zu}\n",
                          it.name.c_str(), unsigned(h.major), unsigned(h.minor), unsigned(h.gameId),
                          static_cast<unsigned long long>(h.compileTime), h.sourceName.c_str(), h.userName.c_str(),
                          h.computerName.c_str(), s.debug.present ? "true" : "false", s.strings.size(),
                          s.objects.size(), n.variables, n.properties, n.states, n.functions, n.instructions,
                          s.notes.size());
            out = buf;
            return;
        }
        if (single) {
            std::string text;
            text += "file:         " + it.label + "\n";
            std::snprintf(buf, sizeof buf, "version:      %u.%u  (game id %u)\n", unsigned(h.major),
                          unsigned(h.minor), unsigned(h.gameId));
            text += buf;
            text += "compiled:     " + format_time(h.compileTime) + "\n";
            text += "source:       " + h.sourceName + "\n";
            text += "user:         " + h.userName + "\n";
            text += "computer:     " + h.computerName + "\n";
            text += std::string("debug info:   ") + (s.debug.present ? "yes" : "no");
            if (s.debug.present) text += ", source modified " + format_time(s.debug.modificationTime);
            text += "\n";
            std::snprintf(buf, sizeof buf, "strings:      %zu\n", s.strings.size());
            text += buf;
            for (const pex::Object& ob : s.objects) {
                text += "object:       " + std::string(s.str(ob.name));
                if (!s.str(ob.parent).empty()) text += " extends " + std::string(s.str(ob.parent));
                text += "\n";
                if (!s.str(ob.autoState).empty()) text += "auto state:   " + std::string(s.str(ob.autoState)) + "\n";
            }
            std::snprintf(buf, sizeof buf,
                          "variables:    %zu\nproperties:   %zu\nstates:       %zu\nfunctions:    %zu\n"
                          "instructions: %zu\n",
                          n.variables, n.properties, n.states, n.functions, n.instructions);
            text += buf;
            for (const std::string& note : s.notes) text += "note:         " + note + "\n";
            out = text;
        } else {
            std::snprintf(buf, sizeof buf, "%-50s %u.%u %s %4zu fn %6zu ins  %s\n", it.name.c_str(),
                          unsigned(h.major), unsigned(h.minor), s.debug.present ? "dbg" : "   ", n.functions,
                          n.instructions, it.label.c_str());
            out = buf;
        }
    });
    for (const std::string& l : lines) std::fputs(l.c_str(), stdout);
    return failures ? 1 : 0;
}

// --- json / asm (render a view per script) ----------------------------------

int cmd_render(int argc, char** argv, const char* cmd, const char* ext,
               std::string (*render)(const pex::Script&)) {
    Options o;
    if (!parse(argc, argv, 2, o, cmd)) return 2;
    std::vector<pextool::Item> items;
    if (!collect(o, items)) return 2;
    if (items.size() > 1 && o.out.empty()) {
        std::fprintf(stderr, "%s: %zu scripts; give --out <directory>\n", cmd, items.size());
        return 2;
    }
    std::error_code ec;
    const bool toDir = items.size() > 1 || (!o.out.empty() && fs::is_directory(fs::u8path(o.out), ec));
    if (toDir) {
        fs::create_directories(fs::u8path(o.out), ec);
        items = pextool::effective_scripts(items);
    }
    std::mutex mu;
    std::atomic<int> failures{0};
    pextool::for_each_parallel(items, o.threads, [&](size_t, const pextool::Item& it, pextool::Loader& ld) {
        pex::Script s;
        if (!load_script(it, ld, s, mu)) {
            ++failures;
            return;
        }
        const std::string text = render(s);
        if (o.out.empty()) {
            std::fwrite(text.data(), 1, text.size(), stdout);
            return;
        }
        fs::path target = fs::u8path(o.out);
        if (toDir) target /= fs::u8path(it.name).replace_extension(ext);
        std::ofstream f(target, std::ios::binary | std::ios::trunc);
        if (!f) {
            ++failures;
            std::lock_guard<std::mutex> lk(mu);
            std::fprintf(stderr, "%s: cannot write %s\n", it.label.c_str(), target.string().c_str());
            return;
        }
        f.write(text.data(), static_cast<std::streamsize>(text.size()));
    });
    return failures ? 1 : 0;
}

std::string render_json(const pex::Script& s) { return pex::to_json(s, true); }
std::string render_asm(const pex::Script& s) { return pex::to_asm(s); }

// --- decompile ---------------------------------------------------------------

int cmd_decompile(int argc, char** argv) {
    Options o;
    if (!parse(argc, argv, 2, o, "decompile")) return 2;
    std::vector<pextool::Item> items;
    if (!collect(o, items)) return 2;
    if (items.size() > 1 && o.out.empty()) {
        std::fprintf(stderr, "decompile: %zu scripts; give --out <directory>\n", items.size());
        return 2;
    }
    std::error_code ec;
    const bool toDir = items.size() > 1 || (!o.out.empty() && fs::is_directory(fs::u8path(o.out), ec));
    if (toDir) {
        fs::create_directories(fs::u8path(o.out), ec);
        items = pextool::effective_scripts(items);
    }
    std::vector<pextool::Item> extra;
    if (!collect_resolve(o, extra)) return 2;
    pextool::ItemResolver resolver;
    resolver.add(extra);
    resolver.add(items);
    pex::DecompileOptions dopt;
    dopt.lineComments = o.comments;
    dopt.keepGeneratedFunctions = o.keepGenerated;
    dopt.optionalObjectParams = o.optionalObjects;
    dopt.resolver = &resolver;
    std::mutex mu;
    std::atomic<int> failures{0};
    std::atomic<size_t> functions{0}, failedFunctions{0}, namedTemps{0};
    const auto t0 = std::chrono::steady_clock::now();
    pextool::for_each_parallel(items, o.threads, [&](size_t, const pextool::Item& it, pextool::Loader& ld) {
        pex::Script s;
        if (!load_script(it, ld, s, mu)) {
            ++failures;
            return;
        }
        const pex::DecompileResult r = pex::decompile(s, dopt);
        for (const pex::FunctionReport& f : r.functions) {
            ++functions;
            if (f.status == pex::FnStatus::Failed) {
                ++failedFunctions;
                if (!o.quiet) {
                    std::lock_guard<std::mutex> lk(mu);
                    std::fprintf(stderr, "%s: %s%s%s: %s at instruction %u\n", it.label.c_str(),
                                 f.state.empty() ? "" : f.state.c_str(), f.state.empty() ? "" : ".", f.name.c_str(),
                                 f.reason.c_str(), unsigned(f.ip));
                }
            } else if (f.status == pex::FnStatus::OkMaterialisedTemps) {
                ++namedTemps;
            }
        }
        if (!r.all_ok()) ++failures;
        if (o.out.empty()) {
            std::fwrite(r.source.data(), 1, r.source.size(), stdout);
            return;
        }
        fs::path target = fs::u8path(o.out);
        if (toDir) {
            std::string name = s.objects.empty() ? it.name : std::string(s.str(s.objects[0].name));
            target /= fs::u8path(name + ".psc");
        }
        std::ofstream f(target, std::ios::binary | std::ios::trunc);
        if (!f) {
            ++failures;
            std::lock_guard<std::mutex> lk(mu);
            std::fprintf(stderr, "%s: cannot write %s\n", it.label.c_str(), target.string().c_str());
            return;
        }
        f.write(r.source.data(), static_cast<std::streamsize>(r.source.size()));
    });
    if (items.size() > 1 || o.quiet) {
        const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::fprintf(stderr, "scripts %zu  functions %zu  failed %zu  named-temps %zu  %.1fs\n", items.size(),
                     functions.load(), failedFunctions.load(), namedTemps.load(), secs);
    }
    return failures ? 1 : 0;
}

// --- check ----------------------------------------------------------------

struct Tally {
    std::mutex mu;
    std::map<std::string, size_t> byCategory;
    std::atomic<size_t> ok{0}, bad{0};
    bool quiet = false;

    void pass() { ++ok; }
    void fail(const pextool::Item& it, const std::string& category, const std::string& detail) {
        ++bad;
        std::lock_guard<std::mutex> lk(mu);
        ++byCategory[category];
        if (!quiet) std::fprintf(stdout, "%s: %s: %s\n", it.label.c_str(), category.c_str(), detail.c_str());
    }
    int finish(size_t total, double secs) {
        for (const auto& [k, v] : byCategory) std::fprintf(stdout, "  %-24s %zu\n", k.c_str(), v);
        std::fprintf(stdout, "files %zu  ok %zu  not-ok %zu  %.1fs\n", total, ok.load(), bad.load(), secs);
        return bad ? 1 : 0;
    }
};

std::string first_error_line(const std::string& output) {
    size_t pos = 0;
    while (pos < output.size()) {
        size_t end = output.find('\n', pos);
        if (end == std::string::npos) end = output.size();
        std::string line = output.substr(pos, end - pos);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.find(": error") != std::string::npos || line.find(": fatal") != std::string::npos ||
            line.find("Cannot") != std::string::npos) {
            return line;
        }
        pos = end + 1;
    }
    return output.empty() ? "(no output)" : output.substr(0, 160);
}

std::string difference_text(const std::vector<pex::Difference>& diffs) {
    if (diffs.empty()) return "differs";
    const pex::Difference& d = diffs.front();
    std::string s = std::string(pex::to_string(d.kind)) + " " + d.where + ": " + d.message;
    if (diffs.size() > 1) s += " (+" + std::to_string(diffs.size() - 1) + " more)";
    return s;
}

int check_rewrite(const Options& o, const std::vector<pextool::Item>& items) {
    const auto t0 = std::chrono::steady_clock::now();
    Tally tally;
    tally.quiet = o.quiet;
    pextool::for_each_parallel(items, o.threads, [&](size_t, const pextool::Item& it, pextool::Loader& ld) {
        std::vector<uint8_t> bytes;
        std::string err;
        if (!ld.load(it, bytes, err)) {
            tally.fail(it, "load failed", err);
            return;
        }
        const pex::Status st = pex::rewrite_check(bytes.data(), bytes.size(), &err);
        if (st == pex::Status::Ok) {
            tally.pass();
            return;
        }
        const bool differs = st == pex::Status::Corrupt && err.rfind("rewrite differs", 0) == 0;
        tally.fail(it, differs ? "rewrite differs" : pex::to_string(st), err);
    });
    return tally.finish(items.size(), std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
}

int check_asm(const Options& o, const std::vector<pextool::Item>& items) {
    if (o.tools.empty()) {
        std::fprintf(stderr, "check --asm needs --tools <Papyrus Compiler folder>\n");
        return 2;
    }
    const fs::path assembler = fs::u8path(o.tools) / "PapyrusAssembler.exe";
    std::error_code ec;
    if (!fs::exists(assembler, ec)) {
        std::fprintf(stderr, "%s not found\n", assembler.string().c_str());
        return 2;
    }
    fs::path work = o.work.empty() ? fs::temp_directory_path(ec) / ("pex-check-" + std::to_string(::_getpid()))
                                   : fs::u8path(o.work);
    fs::create_directories(work, ec);
    if (ec) {
        std::fprintf(stderr, "cannot create %s\n", work.string().c_str());
        return 2;
    }

    const auto t0 = std::chrono::steady_clock::now();
    Tally tally;
    tally.quiet = o.quiet;
    std::mutex mu;
    pextool::for_each_parallel(items, o.threads, [&](size_t i, const pextool::Item& it, pextool::Loader& ld) {
        pex::Script original;
        std::vector<uint8_t> bytes;
        if (!load_script(it, ld, original, mu, &bytes)) {
            tally.fail(it, "unreadable", "");
            return;
        }
        if (original.objects.empty()) {
            tally.fail(it, "no object", "");
            return;
        }
        const std::string name = std::string(original.str(original.objects[0].name));
        const fs::path dir = work / std::to_string(i);
        std::error_code e2;
        fs::create_directories(dir, e2);
        const fs::path pas = dir / fs::u8path(name + ".pas");
        {
            const std::string text = pex::to_asm(original);
            std::ofstream f(pas, std::ios::binary | std::ios::trunc);
            f.write(text.data(), static_cast<std::streamsize>(text.size()));
            if (!f) {
                tally.fail(it, "cannot write listing", pas.string());
                return;
            }
        }
        // "Cannot open store" is the assembler failing to open a file that is
        // there; it happens right after the file was written (antivirus
        // scanning holds it) and passes on retry.
        pextool::RunResult r;
        for (int attempt = 0; attempt < 4; ++attempt) {
            if (attempt) std::this_thread::sleep_for(std::chrono::milliseconds(100 * attempt));
            r = pextool::run(assembler, {name, "-Q"}, dir);
            if (!r.started || r.exitCode == 0 || r.output.find("Cannot open store") == std::string::npos) break;
        }
        if (!r.started) {
            tally.fail(it, "assembler not started", assembler.string());
            return;
        }
        const fs::path produced = dir / fs::u8path(name + ".pex");
        if (r.exitCode != 0 || !fs::exists(produced, e2)) {
            tally.fail(it, "assembler rejected", first_error_line(r.output));
            return;
        }
        pex::Script assembled;
        std::string err;
        const pex::Status st = pex::read_file(produced, assembled, &err);
        if (st != pex::Status::Ok) {
            tally.fail(it, "assembler output unreadable", err);
            return;
        }
        std::vector<pex::Difference> diffs;
        pex::CompareOptions co;
        co.compareHeader = false;  // the assembler always writes version 3.2
        if (!pex::canonical_equal(original, assembled, &diffs, co)) {
            tally.fail(it, "model differs", difference_text(diffs));
            return;
        }
        tally.pass();
        if (!o.keep) fs::remove_all(dir, e2);
    });
    if (!o.keep) fs::remove_all(work, ec);
    return tally.finish(items.size(), std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
}

int cmd_check(int argc, char** argv) {
    Options o;
    if (!parse(argc, argv, 2, o, "check")) return 2;
    if (int(o.rewrite) + int(o.asmCheck) + int(o.roundtrip) != 1) {
        std::fprintf(stderr, "check: give exactly one of --rewrite, --asm, --roundtrip (pex help)\n");
        return 2;
    }
    std::vector<pextool::Item> items;
    if (!collect(o, items)) return 2;
    if (o.rewrite) return check_rewrite(o, items);
    if (o.asmCheck) return check_asm(o, items);
    if (o.tools.empty()) {
        std::fprintf(stderr, "check --roundtrip needs --tools <Papyrus Compiler folder>\n");
        return 2;
    }
    pextool::RoundtripOptions ro;
    if (!collect_resolve(o, ro.resolveItems)) return 2;
    items = pextool::effective_scripts(items);
    ro.tools = fs::u8path(o.tools);
    if (!o.flags.empty()) ro.flags = fs::u8path(o.flags);
    if (!o.work.empty()) ro.work = fs::u8path(o.work);
    ro.keep = o.keep;
    ro.quiet = o.quiet;
    ro.threads = o.threads;
    size_t pos = 0;
    while (pos <= o.imports.size() && !o.imports.empty()) {
        size_t semi = o.imports.find(';', pos);
        if (semi == std::string::npos) semi = o.imports.size();
        if (semi > pos) ro.imports.push_back(fs::u8path(o.imports.substr(pos, semi - pos)));
        pos = semi + 1;
    }
    return pextool::run_roundtrip(ro, items);
}

int help() {
    std::fputs(kHelp, stdout);
    return 0;
}

}  // namespace

#ifdef _WIN32
// Arguments arrive as UTF-16; the tool works in UTF-8 throughout.
int wmain(int argc, wchar_t** wargv) {
    std::vector<std::string> storage(static_cast<size_t>(argc));
    std::vector<char*> argv(static_cast<size_t>(argc) + 1, nullptr);
    for (int i = 0; i < argc; ++i) {
        const int n = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, nullptr, 0, nullptr, nullptr);
        storage[i].resize(n > 0 ? static_cast<size_t>(n - 1) : 0);
        if (n > 1) WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, storage[i].data(), n, nullptr, nullptr);
        argv[i] = storage[i].data();
    }
    _setmode(_fileno(stdout), _O_BINARY);
#else
int main(int argc, char** argv_) {
    std::vector<char*> argv(argv_, argv_ + argc + 1);
#endif
    if (argc < 2) return help(), 2;
    const std::string cmd = argv[1];
    if (cmd == "help" || cmd == "--help" || cmd == "-h" || cmd == "/?") return help();
    if (cmd == "--version" || cmd == "-V") {
        std::printf("pex %s\n", pex::kVersion);
        return 0;
    }
    if (cmd == "info") return cmd_info(argc, argv.data());
    if (cmd == "json") return cmd_render(argc, argv.data(), "json", ".json", render_json);
    if (cmd == "asm") return cmd_render(argc, argv.data(), "asm", ".pas", render_asm);
    if (cmd == "decompile") return cmd_decompile(argc, argv.data());
    if (cmd == "check") return cmd_check(argc, argv.data());
    std::fprintf(stderr, "unknown command %s (pex help)\n", cmd.c_str());
    return 2;
}
