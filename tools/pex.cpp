// pex: command line front end of skqe-papyrus. Every command takes a .pex
// file, a directory (loose scripts and every .bsa below it) or one .bsa.
// Exit codes: 0 ok, 1 an input failed or a check found problems, 2 bad
// arguments.
// SPDX-License-Identifier: GPL-3.0-only
#include "inputs.hpp"
#include "papyrus/analysis.hpp"
#include "papyrus/assemble.hpp"
#include "papyrus/canon.hpp"
#include "papyrus/compile.hpp"
#include "papyrus/decompile.hpp"
#include "papyrus/pex.hpp"
#include "process.hpp"
#include "resolver.hpp"
#include "roundtrip.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <memory>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <ctime>
#include <iterator>
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
  assemble <listing.pas|dir> [--out <path>]
                               our assembler: the listing (the form asm writes
                               and PapyrusCompiler -keepasm writes) becomes a
                               .pex, <name>.pex next to the listing or at --out
                               (a directory when there are several)
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
  refs <input> [--csv] [--callee Name,...]
                               every integer that reaches a FormID-taking
                               call (GetFormFromFile, GetForm, GetFormEx, any
                               call passing an int then a "x.esp" string, plus
                               --callee names), with its plugin string and
                               whether it is a literal, propagated from an
                               assignment, a property default, or dynamic
  deps <input> [--resolve <dir|bsa>] [--vanilla <dir|bsa>] [--json]
                               parent chain, types, calls per type, mod
                               events; with --vanilla, the calls vanilla does
                               not declare (what the script needs an extender
                               for)
  closure <script.pex> --data <Data> [--import <dir;dir>] [--emit <dir>] [--json]
                               walks every script the script needs, through
                               compiled files in --data; reports each as
                               pex / source (a .psc in --import) / missing,
                               and members the chains do not declare. The
                               closure is what a compiler needs: the script's
                               own types, then what each dependency's
                               declarations name. --emit writes declaration
                               stubs (empty bodies) for the pex ones
  patch <input> --map <FormIDMap> [--callee Name,...] [--dry-run] [--backup]
                               renumbers FormID literals paired with the
                               map's plugin (EslMapLib format), renames the
                               plugin string when the map says so, writes
                               the file and reads it back; loose files only
  check --rewrite <input>      read + write every script, compare byte for byte
  check --asm <input> --tools <dir>
                               assemble our listing with PapyrusAssembler.exe
                               from <dir> (the game's "Papyrus Compiler"
                               folder) and compare the result with the
                               original at model level
  compile <psc|dir> [--import <dir;dir>] [--flags <flg>] [--out <dir>] [--declarations]
            [--resolve <dir|bsa>]
                               our compiler: .psc to .pex (the source's own
                               folder is always an import folder; a dependency
                               without source is taken from its compiled file
                               in --resolve); with --declarations every
                               function body is left empty
  check --parse <SourceDir>    parse every .psc below the folder with the compiler
                               front end; lists the files it cannot parse
  check --compile <SourceDir> --pex <input> [--import <dir;dir>] [--flags <flg>]
            [--declarations] [--tools <dir>]
                               compile every source that has a compiled
                               counterpart in <input> with our compiler and
                               compare with it at model level; prints the share
                               identical (--declarations: declarations only).
                               With --tools, a source whose result differs from
                               the shipped file is also compiled with
                               PapyrusCompiler.exe: "source drift" when ours
                               equals Bethesda's result, "differs from
                               PapyrusCompiler" when not
  check --assemble <input> [--tools <dir>]
                               assemble our listing with our own assembler
                               and compare with the original at model level,
                               and the listing of the result with the
                               listing; with --tools also with what
                               PapyrusAssembler.exe makes of the listing
  check --roundtrip <input> --tools <dir> [--import <dir;dir>] [--flags <flg>]
            [--resolve <dir|bsa>] [--per-script-imports]
                               decompile every script, compile the result
                               with PapyrusCompiler.exe (the decompiled set
                               is its own import folder, then --import) and
                               compare with the original at model level;
                               prints the share that comes back identical.
                               --per-script-imports gives every script its
                               own import folder holding exactly its
                               dependency closure, each dependency taken from
                               the script's own archive first; needed for a
                               mixed set (mods), where one shared folder
                               inherits other mods' overrides and name
                               clashes. Reports incomplete closures before
                               compiling and compiles byte-identical
                               duplicates once.
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
    bool assembleCheck = false;
    bool parseCheck = false;
    bool compileCheck = false;
    bool declarations = false;
    std::string pexInput;
    bool roundtrip = false;
    bool perScript = false;
    std::string imports;
    std::string flags;
    std::vector<std::string> resolve;
    std::string vanilla;
    std::string data;
    std::string emit;
    std::string map;
    std::string callees;
    bool csv = false;
    bool dryRun = false;
    bool backup = false;
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
        else if (a == "--assemble") o.assembleCheck = true;
        else if (a == "--parse") o.parseCheck = true;
        else if (a == "--compile") o.compileCheck = true;
        else if (a == "--declarations") o.declarations = true;
        else if (a == "--pex" && hasNext) o.pexInput = argv[++i];
        else if (a == "--roundtrip") o.roundtrip = true;
        else if (a == "--per-script-imports") o.perScript = true;
        else if (a == "--import" && hasNext) o.imports = argv[++i];
        else if (a == "--flags" && hasNext) o.flags = argv[++i];
        else if (a == "--resolve" && hasNext) o.resolve.push_back(argv[++i]);
        else if (a == "--vanilla" && hasNext) o.vanilla = argv[++i];
        else if (a == "--data" && hasNext) o.data = argv[++i];
        else if (a == "--emit" && hasNext) o.emit = argv[++i];
        else if (a == "--map" && hasNext) o.map = argv[++i];
        else if (a == "--callee" && hasNext) o.callees = argv[++i];
        else if (a == "--csv") o.csv = true;
        else if (a == "--dry-run") o.dryRun = true;
        else if (a == "--backup") o.backup = true;
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

// --- assemble ----------------------------------------------------------------

int cmd_assemble(int argc, char** argv) {
    Options o;
    if (!parse(argc, argv, 2, o, "assemble")) return 2;
    if (o.inputs.empty()) {
        std::fprintf(stderr, "no input given\n");
        return 2;
    }
    std::vector<fs::path> listings;
    std::error_code ec;
    for (const std::string& in : o.inputs) {
        const fs::path p = fs::u8path(in);
        if (fs::is_directory(p, ec)) {
            for (const fs::directory_entry& e : fs::recursive_directory_iterator(p, ec)) {
                if (e.is_regular_file(ec) && e.path().extension() == ".pas") listings.push_back(e.path());
            }
        } else if (fs::is_regular_file(p, ec)) {
            listings.push_back(p);
        } else {
            std::fprintf(stderr, "%s: does not exist\n", in.c_str());
            return 2;
        }
    }
    if (listings.size() > 1 && o.out.empty()) {
        std::fprintf(stderr, "assemble: %zu listings; give --out <directory>\n", listings.size());
        return 2;
    }
    const bool toDir = listings.size() > 1 || (!o.out.empty() && fs::is_directory(fs::u8path(o.out), ec));
    if (toDir) fs::create_directories(fs::u8path(o.out), ec);
    int failures = 0;
    for (const fs::path& in : listings) {
        std::string text;
        {
            std::ifstream f(in, std::ios::binary);
            if (!f) {
                std::fprintf(stderr, "%s: cannot read\n", in.string().c_str());
                ++failures;
                continue;
            }
            text.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
        }
        pex::Script s;
        std::string err;
        const pex::Status st = pex::assemble(text, s, &err);
        if (st != pex::Status::Ok) {
            std::fprintf(stderr, "%s: %s\n", in.string().c_str(), err.c_str());
            ++failures;
            continue;
        }
        fs::path target;
        if (o.out.empty()) target = fs::path(in).replace_extension(".pex");
        else if (toDir) target = fs::u8path(o.out) / in.filename().replace_extension(".pex");
        else target = fs::u8path(o.out);
        if (pex::write_file(s, target, &err) != pex::Status::Ok) {
            std::fprintf(stderr, "%s: cannot write %s: %s\n", in.string().c_str(), target.string().c_str(), err.c_str());
            ++failures;
        }
    }
    return failures ? 1 : 0;
}

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

// Writes `text` as <dir>/<name>.pas, runs PapyrusAssembler on it and reads
// the .pex it produces. On failure `category` and `detail` say what happened.
bool assemble_with_tool(const fs::path& assembler, const fs::path& dir, const std::string& name,
                        const std::string& text, pex::Script& out, std::string& category, std::string& detail) {
    std::error_code e2;
    const fs::path pas = dir / fs::u8path(name + ".pas");
    {
        std::ofstream f(pas, std::ios::binary | std::ios::trunc);
        f.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!f) {
            category = "cannot write listing";
            detail = pas.string();
            return false;
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
        category = "assembler not started";
        detail = assembler.string();
        return false;
    }
    const fs::path produced = dir / fs::u8path(name + ".pex");
    if (r.exitCode != 0 || !fs::exists(produced, e2)) {
        category = "assembler rejected";
        detail = first_error_line(r.output);
        return false;
    }
    std::string err;
    if (pex::read_file(produced, out, &err) != pex::Status::Ok) {
        category = "assembler output unreadable";
        detail = err;
        return false;
    }
    return true;
}

// The PapyrusAssembler.exe of --tools, or an empty path (with a message) when
// it is not there.
fs::path find_tool(const Options& o, const char* exe, const char* forWhat, bool required) {
    if (o.tools.empty()) {
        if (required) std::fprintf(stderr, "%s needs --tools <Papyrus Compiler folder>\n", forWhat);
        return {};
    }
    const fs::path tool = fs::u8path(o.tools) / exe;
    std::error_code ec;
    if (!fs::exists(tool, ec)) {
        std::fprintf(stderr, "%s not found\n", tool.string().c_str());
        return {};
    }
    return tool;
}

fs::path make_work_dir(const Options& o) {
    std::error_code ec;
    fs::path work = o.work.empty() ? fs::temp_directory_path(ec) / ("pex-check-" + std::to_string(::_getpid()))
                                   : fs::u8path(o.work);
    fs::create_directories(work, ec);
    if (ec) {
        std::fprintf(stderr, "cannot create %s\n", work.string().c_str());
        return {};
    }
    return work;
}

int check_asm(const Options& o, const std::vector<pextool::Item>& items) {
    const fs::path assembler = find_tool(o, "PapyrusAssembler.exe", "check --asm", true);
    if (assembler.empty()) return 2;
    const fs::path work = make_work_dir(o);
    if (work.empty()) return 2;
    std::error_code ec;

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
        pex::Script assembled;
        std::string category, detail;
        if (!assemble_with_tool(assembler, dir, name, pex::to_asm(original), assembled, category, detail)) {
            tally.fail(it, category, detail);
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

// Our assembler against the original, and (with --tools) against
// PapyrusAssembler's reading of the same listing.
int check_assemble(const Options& o, const std::vector<pextool::Item>& items) {
    const fs::path assembler = find_tool(o, "PapyrusAssembler.exe", "check --assemble", false);
    if (!o.tools.empty() && assembler.empty()) return 2;
    fs::path work;
    if (!assembler.empty()) {
        work = make_work_dir(o);
        if (work.empty()) return 2;
    }
    const auto t0 = std::chrono::steady_clock::now();
    Tally tally;
    tally.quiet = o.quiet;
    std::mutex mu;
    pextool::for_each_parallel(items, o.threads, [&](size_t i, const pextool::Item& it, pextool::Loader& ld) {
        pex::Script original;
        if (!load_script(it, ld, original, mu)) {
            tally.fail(it, "unreadable", "");
            return;
        }
        if (original.objects.empty()) {
            tally.fail(it, "no object", "");
            return;
        }
        const std::string listing = pex::to_asm(original);
        pex::Script ours;
        std::string err;
        if (pex::assemble(listing, ours, &err) != pex::Status::Ok) {
            tally.fail(it, "assemble error", err);
            return;
        }
        std::vector<pex::Difference> diffs;
        pex::CompareOptions co;
        co.compareHeader = false;  // old mods carry version 3.1; the assembler writes 3.2
        if (!pex::canonical_equal(original, ours, &diffs, co)) {
            tally.fail(it, "model differs", difference_text(diffs));
            return;
        }
        if (pex::to_asm(ours) != listing) {
            tally.fail(it, "listing differs", "the listing of the assembled model is not the input listing");
            return;
        }
        std::vector<uint8_t> bytes;
        pex::Script again;
        if (pex::write(ours, bytes, &err) != pex::Status::Ok || pex::read(bytes.data(), bytes.size(), again, &err) != pex::Status::Ok) {
            tally.fail(it, "write failed", err);
            return;
        }
        if (!assembler.empty()) {
            const std::string name = std::string(original.str(original.objects[0].name));
            const fs::path dir = work / std::to_string(i);
            std::error_code e2;
            fs::create_directories(dir, e2);
            pex::Script theirs;
            std::string category, detail;
            if (!assemble_with_tool(assembler, dir, name, listing, theirs, category, detail)) {
                tally.fail(it, category, detail);
                return;
            }
            diffs.clear();
            if (!pex::canonical_equal(theirs, ours, &diffs, co)) {
                tally.fail(it, "differs from PapyrusAssembler", difference_text(diffs));
                return;
            }
            if (!o.keep) fs::remove_all(dir, e2);
        }
        tally.pass();
    });
    if (!work.empty() && !o.keep) {
        std::error_code ec;
        fs::remove_all(work, ec);
    }
    return tally.finish(items.size(), std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
}

std::vector<std::string> split_list(const std::string& s, char sep);

// .psc files below the inputs (files or folders), sorted.
std::vector<fs::path> source_files(const Options& o, std::vector<fs::path>* folders) {
    std::vector<fs::path> files;
    std::error_code ec;
    for (const std::string& in : o.inputs) {
        const fs::path p = fs::u8path(in);
        if (fs::is_regular_file(p, ec)) {
            files.push_back(p);
            if (folders) folders->push_back(p.parent_path());
            continue;
        }
        if (!fs::is_directory(p, ec)) {
            std::fprintf(stderr, "%s: does not exist\n", in.c_str());
            return {};
        }
        if (folders) folders->push_back(p);
        for (const fs::directory_entry& e : fs::recursive_directory_iterator(p, ec)) {
            if (!e.is_regular_file(ec)) continue;
            std::string ext = e.path().extension().string();
            for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (ext == ".psc") files.push_back(e.path());
        }
    }
    std::sort(files.begin(), files.end());
    return files;
}

std::string lower_ascii(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// The compile options shared by `compile` and `check --compile`: flags file,
// import folders (the sources' own folders first, then --import).
struct CompileSetup {
    std::vector<psc::UserFlagDef> flags;
    std::vector<fs::path> imports;
    bool ok = false;
};

CompileSetup compile_setup(const Options& o, const std::vector<fs::path>& sourceFolders) {
    CompileSetup s;
    if (o.flags.empty()) {
        std::fprintf(stderr, "compile needs --flags <TESV_Papyrus_Flags.flg>\n");
        return s;
    }
    std::string text;
    {
        std::ifstream f(fs::u8path(o.flags), std::ios::binary);
        if (!f) {
            std::fprintf(stderr, "cannot read %s\n", o.flags.c_str());
            return s;
        }
        text.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    }
    std::string err;
    if (!psc::load_flags(text, s.flags, &err)) {
        std::fprintf(stderr, "%s: %s\n", o.flags.c_str(), err.c_str());
        return s;
    }
    for (const fs::path& p : sourceFolders) {
        if (std::find(s.imports.begin(), s.imports.end(), p) == s.imports.end()) s.imports.push_back(p);
    }
    for (const std::string& dir : split_list(o.imports, ';')) s.imports.push_back(fs::u8path(dir));
    s.ok = true;
    return s;
}

// Sources from folders first; a script with no source but a compiled file in
// the given items is served as a declaration stub decompiled from it
// (`DecompileOptions::declarationsOnly`, every parameter with a default, as
// the round trip's import sets are made). Bethesda's compiler cannot do this;
// it lets the oracle cover scripts whose dependencies ship without source.
class CompiledFallbackResolver : public psc::SourceResolver {
public:
    CompiledFallbackResolver(std::unique_ptr<psc::SourceResolver> folders, const std::vector<pextool::Item>& items,
                             const std::map<std::string, size_t>& byStem)
        : m_folders(std::move(folders)), m_items(items), m_byStem(byStem) {}

    bool find(std::string_view scriptName, psc::SourceFile& out) override {
        if (m_folders->find(scriptName, out)) return true;
        auto it = m_byStem.find(lower_ascii(std::string(scriptName)));
        if (it == m_byStem.end()) return false;
        const pextool::Item& item = m_items[it->second];
        std::vector<uint8_t> bytes;
        std::string err;
        pex::Script s;
        if (!m_loader.load(item, bytes, err) || pex::read(bytes.data(), bytes.size(), s, &err) != pex::Status::Ok) return false;
        if (s.objects.empty()) return false;
        pex::DecompileOptions d;
        d.declarationsOnly = true;
        d.optionalObjectParams = true;
        const pex::DecompileResult r = pex::decompile(s, d);
        out.name = std::string(s.str(s.objects[0].name));
        out.path = item.label;
        out.text = r.source;
        out.modified = 0;
        return true;
    }

private:
    std::unique_ptr<psc::SourceResolver> m_folders;
    const std::vector<pextool::Item>& m_items;
    const std::map<std::string, size_t>& m_byStem;
    pextool::Loader m_loader;
};

psc::SourceFile read_source(const fs::path& p) {
    psc::SourceFile src;
    std::ifstream f(p, std::ios::binary);
    src.text.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    src.name = p.stem().string();
    src.path = p.string();
    std::error_code ec;
    const auto t = fs::last_write_time(p, ec);
    if (!ec) {
        const auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
            t - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
        src.modified = static_cast<uint64_t>(std::chrono::system_clock::to_time_t(sctp));
    }
    return src;
}

int cmd_compile(int argc, char** argv) {
    Options o;
    if (!parse(argc, argv, 2, o, "compile")) return 2;
    if (o.inputs.empty()) {
        std::fprintf(stderr, "no input given\n");
        return 2;
    }
    std::vector<fs::path> folders;
    const std::vector<fs::path> files = source_files(o, &folders);
    if (files.empty()) return 2;
    const CompileSetup setup = compile_setup(o, folders);
    if (!setup.ok) return 2;
    std::error_code ec;
    const bool toDir = files.size() > 1 || (!o.out.empty() && fs::is_directory(fs::u8path(o.out), ec));
    if (toDir && o.out.empty()) {
        std::fprintf(stderr, "compile: %zu sources; give --out <directory>\n", files.size());
        return 2;
    }
    if (toDir) fs::create_directories(fs::u8path(o.out), ec);
    std::unique_ptr<psc::SourceResolver> resolver = psc::folder_resolver(setup.imports);
    std::vector<pextool::Item> compiled;
    std::map<std::string, size_t> compiledByStem;
    if (!o.resolve.empty()) {
        if (!collect_resolve(o, compiled)) return 2;
        compiled = pextool::effective_scripts(compiled);
        for (size_t i = 0; i < compiled.size(); ++i) compiledByStem[lower_ascii(fs::path(compiled[i].name).stem().string())] = i;
        resolver = std::make_unique<CompiledFallbackResolver>(std::move(resolver), compiled, compiledByStem);
    }
    psc::CompileOptions co;
    co.flags = setup.flags;
    co.resolver = resolver.get();
    co.declarationsOnly = o.declarations;
    int failures = 0;
    for (const fs::path& in : files) {
        pex::Script s;
        std::vector<psc::Diagnostic> diags;
        const bool ok = psc::compile(read_source(in), co, s, diags);
        for (const psc::Diagnostic& d : diags) std::fprintf(stderr, "%s\n", d.text().c_str());
        if (!ok) {
            ++failures;
            continue;
        }
        fs::path target;
        if (o.out.empty()) target = fs::path(in).replace_extension(".pex");
        else if (toDir) target = fs::u8path(o.out) / in.filename().replace_extension(".pex");
        else target = fs::u8path(o.out);
        std::string err;
        if (pex::write_file(s, target, &err) != pex::Status::Ok) {
            std::fprintf(stderr, "%s: cannot write %s: %s\n", in.string().c_str(), target.string().c_str(), err.c_str());
            ++failures;
        }
    }
    return failures ? 1 : 0;
}

// Compiles one source with PapyrusCompiler.exe into <dir>; reads the result.
bool compile_with_tool(const fs::path& compiler, const fs::path& dir, const fs::path& source, const fs::path& flags,
                       const std::vector<fs::path>& imports, pex::Script& out, std::string& category, std::string& detail) {
    std::string importArg;
    for (const fs::path& p : imports) {
        if (!importArg.empty()) importArg += ';';
        importArg += p.string();
    }
    pextool::RunResult r;
    for (int attempt = 0; attempt < 3; ++attempt) {
        if (attempt) std::this_thread::sleep_for(std::chrono::milliseconds(100 * attempt));
        r = pextool::run(compiler, {source.filename().string(), "-f=" + flags.string(), "-i=" + importArg, "-o=" + dir.string(), "-q"},
                         source.parent_path());
        if (!r.started || r.exitCode == 0 || r.output.find("error while attempting to read") == std::string::npos) break;
    }
    if (!r.started) {
        category = "compiler not started";
        detail = compiler.string();
        return false;
    }
    std::error_code ec;
    const fs::path produced = dir / source.filename().replace_extension(".pex");
    if (r.exitCode != 0 || !fs::exists(produced, ec)) {
        category = "PapyrusCompiler cannot compile";
        detail = first_error_line(r.output);
        return false;
    }
    std::string err;
    if (pex::read_file(produced, out, &err) != pex::Status::Ok) {
        category = "compiler output unreadable";
        detail = err;
        return false;
    }
    return true;
}

// Every source with a compiled counterpart in --pex, compiled with our
// compiler and compared with it canonically.
int check_compile(const Options& o) {
    if (o.pexInput.empty()) {
        std::fprintf(stderr, "check --compile needs --pex <compiled scripts: file, folder or .bsa>\n");
        return 2;
    }
    std::vector<fs::path> folders;
    const std::vector<fs::path> files = source_files(o, &folders);
    if (files.empty()) return 2;
    const CompileSetup setup = compile_setup(o, folders);
    if (!setup.ok) return 2;
    const fs::path compiler = find_tool(o, "PapyrusCompiler.exe", "check --compile", false);
    if (!o.tools.empty() && compiler.empty()) return 2;
    fs::path workDir;
    if (!compiler.empty()) {
        workDir = make_work_dir(o);
        if (workDir.empty()) return 2;
    }
    std::vector<pextool::Item> items;
    std::vector<std::string> warnings;
    if (!pextool::enumerate(fs::u8path(o.pexInput), items, warnings)) {
        std::fprintf(stderr, "--pex %s: does not exist\n", o.pexInput.c_str());
        return 2;
    }
    items = pextool::effective_scripts(items);
    std::map<std::string, size_t> byStem;
    for (size_t i = 0; i < items.size(); ++i) byStem[lower_ascii(fs::path(items[i].name).stem().string())] = i;
    // The sources that have a counterpart.
    std::vector<std::pair<fs::path, size_t>> work;
    for (const fs::path& f : files) {
        auto it = byStem.find(lower_ascii(f.stem().string()));
        if (it != byStem.end()) work.emplace_back(f, it->second);
    }
    const auto t0 = std::chrono::steady_clock::now();
    Tally tally;
    tally.quiet = o.quiet;
    std::atomic<size_t> nextIndex{0};
    std::mutex mu;
    const unsigned threads = o.threads ? o.threads : std::max(1u, std::thread::hardware_concurrency());
    std::vector<std::thread> pool;
    for (unsigned t = 0; t < threads; ++t) {
        pool.emplace_back([&] {
            pextool::Loader ld;
            std::unique_ptr<psc::SourceResolver> resolver =
                std::make_unique<CompiledFallbackResolver>(psc::folder_resolver(setup.imports), items, byStem);
            psc::CompileOptions co;
            co.flags = setup.flags;
            co.resolver = resolver.get();
            co.declarationsOnly = o.declarations;
            for (;;) {
                const size_t i = nextIndex++;
                if (i >= work.size()) return;
                const pextool::Item& it = items[work[i].second];
                pex::Script original;
                if (!load_script(it, ld, original, mu)) {
                    tally.fail(it, "unreadable", "");
                    continue;
                }
                pex::Script ours;
                std::vector<psc::Diagnostic> diags;
                if (!psc::compile(read_source(work[i].first), co, ours, diags)) {
                    tally.fail(it, "compile error", diags.empty() ? "" : diags.front().text());
                    continue;
                }
                std::vector<pex::Difference> diffs;
                pex::CompareOptions cmp;
                cmp.compareHeader = false;
                cmp.declarationsOnly = o.declarations;
                if (!pex::canonical_equal(original, ours, &diffs, cmp)) {
                    if (compiler.empty()) {
                        tally.fail(it, std::string(pex::to_string(diffs.front().kind)) + " diff", difference_text(diffs));
                        continue;
                    }
                    // Bethesda's compiler on the same source: does it agree with us?
                    const fs::path dir = workDir / std::to_string(i);
                    std::error_code e2;
                    fs::create_directories(dir, e2);
                    pex::Script theirs;
                    std::string category, detail;
                    if (!compile_with_tool(compiler, dir, work[i].first, fs::u8path(o.flags), setup.imports, theirs, category, detail)) {
                        tally.fail(it, category, detail);
                    } else {
                        std::vector<pex::Difference> d2;
                        if (pex::canonical_equal(theirs, ours, &d2, cmp)) {
                            tally.fail(it, "source drift (equal to PapyrusCompiler)", difference_text(diffs));
                        } else {
                            tally.fail(it, std::string("differs from PapyrusCompiler: ") + pex::to_string(d2.front().kind),
                                       difference_text(d2));
                        }
                    }
                    if (!o.keep) fs::remove_all(dir, e2);
                    continue;
                }
                tally.pass();
            }
        });
    }
    for (std::thread& t : pool) t.join();
    if (!workDir.empty() && !o.keep) {
        std::error_code ec;
        fs::remove_all(workDir, ec);
    }
    std::fprintf(stdout, "sources %zu  with a compiled counterpart %zu\n", files.size(), work.size());
    return tally.finish(work.size(), std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
}

// Every .psc below the given folders, parsed with the compiler front end.
int check_parse(const Options& o) {
    std::vector<fs::path> files;
    std::error_code ec;
    for (const std::string& in : o.inputs) {
        const fs::path p = fs::u8path(in);
        if (fs::is_regular_file(p, ec)) {
            files.push_back(p);
            continue;
        }
        if (!fs::is_directory(p, ec)) {
            std::fprintf(stderr, "%s: does not exist\n", in.c_str());
            return 2;
        }
        for (const fs::directory_entry& e : fs::recursive_directory_iterator(p, ec)) {
            if (!e.is_regular_file(ec)) continue;
            std::string ext = e.path().extension().string();
            for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (ext == ".psc") files.push_back(e.path());
        }
    }
    std::sort(files.begin(), files.end());
    const auto t0 = std::chrono::steady_clock::now();
    std::atomic<size_t> ok{0}, bad{0};
    std::mutex mu;
    std::map<std::string, size_t> byMessage;
    const unsigned threads = o.threads ? o.threads : std::max(1u, std::thread::hardware_concurrency());
    std::atomic<size_t> nextIndex{0};
    std::vector<std::thread> pool;
    for (unsigned t = 0; t < threads; ++t) {
        pool.emplace_back([&] {
            for (;;) {
                const size_t i = nextIndex++;
                if (i >= files.size()) return;
                std::string text;
                {
                    std::ifstream f(files[i], std::ios::binary);
                    text.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
                }
                psc::ScriptPtr tree;
                psc::Diagnostic d;
                if (psc::parse(text, files[i].filename().string(), tree, d)) {
                    ++ok;
                    continue;
                }
                ++bad;
                std::lock_guard<std::mutex> lk(mu);
                std::string key = d.message;
                if (key.size() > 40) key = key.substr(0, 40);
                ++byMessage[key];
                if (!o.quiet) std::fprintf(stdout, "%s\n", d.text().c_str());
            }
        });
    }
    for (std::thread& t : pool) t.join();
    for (const auto& [k, v] : byMessage) std::fprintf(stdout, "  %-42s %zu\n", k.c_str(), v);
    std::fprintf(stdout, "files %zu  parsed %zu  failed %zu  %.1fs\n", files.size(), ok.load(), bad.load(),
                 std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
    return bad ? 1 : 0;
}

int cmd_check(int argc, char** argv) {
    Options o;
    if (!parse(argc, argv, 2, o, "check")) return 2;
    if (int(o.rewrite) + int(o.asmCheck) + int(o.assembleCheck) + int(o.roundtrip) + int(o.parseCheck) + int(o.compileCheck) != 1) {
        std::fprintf(stderr, "check: give exactly one of --rewrite, --asm, --assemble, --parse, --compile, --roundtrip (pex help)\n");
        return 2;
    }
    if (o.parseCheck || o.compileCheck) {
        if (o.inputs.empty()) {
            std::fprintf(stderr, "no input given\n");
            return 2;
        }
        return o.parseCheck ? check_parse(o) : check_compile(o);
    }
    std::vector<pextool::Item> items;
    if (!collect(o, items)) return 2;
    if (o.rewrite) return check_rewrite(o, items);
    if (o.asmCheck) return check_asm(o, items);
    if (o.assembleCheck) return check_assemble(o, items);
    if (o.tools.empty()) {
        std::fprintf(stderr, "check --roundtrip needs --tools <Papyrus Compiler folder>\n");
        return 2;
    }
    pextool::RoundtripOptions ro;
    if (!collect_resolve(o, ro.resolveItems)) return 2;
    // One shared source folder cannot hold two scripts of one name; per-script
    // folders can, and byte-identical copies are compiled once there.
    if (!o.perScript) items = pextool::effective_scripts(items);
    ro.perScript = o.perScript;
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

// --- refs / deps / closure / patch -------------------------------------------

std::vector<std::string> split_list(const std::string& s, char sep) {
    std::vector<std::string> out;
    size_t pos = 0;
    while (pos <= s.size() && !s.empty()) {
        size_t semi = s.find(sep, pos);
        if (semi == std::string::npos) semi = s.size();
        if (semi > pos) out.push_back(s.substr(pos, semi - pos));
        pos = semi + 1;
    }
    return out;
}

std::string json_str(std::string_view s) {
    std::string out = "\"";
    for (unsigned char c : s) {
        if (c == '"') out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c < 0x20 || c >= 0x7F) {
            char buf[8];
            std::snprintf(buf, sizeof buf, "\\u%04X", unsigned(c));
            out += buf;
        } else out += static_cast<char>(c);
    }
    return out + "\"";
}

std::string csv_str(std::string_view s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '"') out += "\"\"";
        else out += c;
    }
    return out + "\"";
}

int cmd_refs(int argc, char** argv) {
    Options o;
    if (!parse(argc, argv, 2, o, "refs")) return 2;
    std::vector<pextool::Item> items;
    if (!collect(o, items)) return 2;
    const std::vector<std::string> extra = split_list(o.callees, ',');
    std::vector<std::string> rows(items.size());
    std::atomic<int> failures{0};
    std::mutex mu;
    pextool::for_each_parallel(items, o.threads, [&](size_t i, const pextool::Item& it, pextool::Loader& ld) {
        pex::Script s;
        if (!load_script(it, ld, s, mu)) {
            ++failures;
            return;
        }
        std::string text;
        for (const pex::FormRef& r : pex::find_form_refs(s, extra)) {
            char hex[16];
            std::snprintf(hex, sizeof hex, "%08X", unsigned(r.formId));
            if (o.csv) {
                text += csv_str(it.label) + "," + csv_str(r.object) + "," + csv_str(r.state) + "," + csv_str(r.function) + "," +
                        std::to_string(r.ip) + "," + csv_str(r.callee) + "," + pex::to_string(r.kind) + "," +
                        (r.kind == pex::RefKind::Dynamic ? std::string() : std::string(hex)) + "," + csv_str(r.plugin) + "," +
                        (r.pluginLiteral ? "literal" : (r.plugin.empty() ? "" : "variable")) + "," + csv_str(r.variable) + "\n";
            } else {
                text += "{\"file\":" + json_str(it.label) + ",\"object\":" + json_str(r.object) + ",\"state\":" + json_str(r.state) +
                        ",\"function\":" + json_str(r.function) + ",\"ip\":" + std::to_string(r.ip) + ",\"callee\":" + json_str(r.callee) +
                        ",\"kind\":\"" + pex::to_string(r.kind) + "\"" +
                        (r.kind == pex::RefKind::Dynamic ? std::string() : ",\"formId\":" + std::to_string(r.formId) + ",\"formIdHex\":\"" + hex + "\"") +
                        ",\"plugin\":" + json_str(r.plugin) + ",\"pluginLiteral\":" + (r.pluginLiteral ? "true" : "false") +
                        (r.variable.empty() ? std::string() : ",\"variable\":" + json_str(r.variable)) + "}\n";
            }
        }
        rows[i] = std::move(text);
    });
    if (o.csv) std::fputs("file,object,state,function,ip,callee,kind,formIdHex,plugin,pluginKind,variable\n", stdout);
    for (const std::string& r : rows) std::fputs(r.c_str(), stdout);
    return failures ? 1 : 0;
}

int cmd_deps(int argc, char** argv) {
    Options o;
    if (!parse(argc, argv, 2, o, "deps")) return 2;
    std::vector<pextool::Item> items;
    if (!collect(o, items)) return 2;
    std::vector<pextool::Item> extra;
    if (!collect_resolve(o, extra)) return 2;
    pextool::ItemResolver resolver;
    resolver.add(extra);
    resolver.add(items);
    std::unique_ptr<pextool::ItemResolver> vanilla;
    if (!o.vanilla.empty()) {
        std::vector<pextool::Item> v;
        std::vector<std::string> warnings;
        if (!pextool::enumerate(fs::u8path(o.vanilla), v, warnings)) {
            std::fprintf(stderr, "--vanilla %s: does not exist\n", o.vanilla.c_str());
            return 2;
        }
        vanilla = std::make_unique<pextool::ItemResolver>();
        vanilla->add(v);
    }
    std::vector<std::string> rows(items.size());
    std::atomic<int> failures{0};
    std::mutex mu;
    pextool::for_each_parallel(items, o.threads, [&](size_t i, const pextool::Item& it, pextool::Loader& ld) {
        pex::Script s;
        if (!load_script(it, ld, s, mu)) {
            ++failures;
            return;
        }
        const pex::Dependencies d = pex::find_dependencies(s, &resolver, vanilla.get());
        std::string t;
        auto joined = [](const std::set<std::string>& set) {
            std::string out;
            for (const std::string& x : set) out += (out.empty() ? "" : ", ") + x;
            return out;
        };
        if (o.json) {
            t += "{\"file\":" + json_str(it.label) + ",\"name\":" + json_str(d.name) + ",\"parents\":[";
            for (size_t k = 0; k < d.parents.size(); ++k) t += (k ? "," : "") + json_str(d.parents[k]);
            t += "],\"parentChainComplete\":" + std::string(d.parentChainComplete ? "true" : "false") + ",\"types\":[";
            size_t k = 0;
            for (const std::string& x : d.types) t += (k++ ? "," : "") + json_str(x);
            t += "],\"calls\":{";
            k = 0;
            for (const auto& [type, fns] : d.callsByType) {
                t += (k++ ? "," : "") + json_str(type) + ":[";
                size_t m = 0;
                for (const std::string& f : fns) t += (m++ ? "," : "") + json_str(f);
                t += "]";
            }
            t += "},\"modEvents\":[";
            k = 0;
            for (const std::string& x : d.modEvents) t += (k++ ? "," : "") + json_str(x);
            t += "],\"notInVanilla\":[";
            k = 0;
            for (const std::string& x : d.notInVanilla) t += (k++ ? "," : "") + json_str(x);
            t += "]}\n";
        } else {
            t += d.name + "  (" + it.label + ")\n";
            std::string chain;
            for (const std::string& p : d.parents) chain += (chain.empty() ? "" : " -> ") + p;
            t += "  extends:        " + (chain.empty() ? std::string("(nothing)") : chain) + (d.parentChainComplete ? "" : "  [chain incomplete]") + "\n";
            t += "  types:          " + joined(d.types) + "\n";
            for (const auto& [type, fns] : d.callsByType) t += "  calls " + type + ": " + joined(fns) + "\n";
            if (!d.modEvents.empty()) t += "  mod events:     " + joined(d.modEvents) + "\n";
            if (vanilla) t += "  not in vanilla: " + (d.notInVanilla.empty() ? std::string("(none)") : joined(d.notInVanilla)) + "\n";
        }
        rows[i] = std::move(t);
    });
    for (const std::string& r : rows) std::fputs(r.c_str(), stdout);
    return failures ? 1 : 0;
}

int cmd_closure(int argc, char** argv) {
    Options o;
    if (!parse(argc, argv, 2, o, "closure")) return 2;
    if (o.data.empty()) {
        std::fprintf(stderr, "closure needs --data <Data folder>\n");
        return 2;
    }
    std::vector<pextool::Item> items;
    if (!collect(o, items)) return 2;
    std::vector<pextool::Item> dataItems;
    std::vector<std::string> warnings;
    if (!pextool::enumerate(fs::u8path(o.data), dataItems, warnings)) {
        std::fprintf(stderr, "--data %s: does not exist\n", o.data.c_str());
        return 2;
    }
    pextool::ItemResolver resolver;
    resolver.add(dataItems);
    resolver.add(items);
    const std::vector<std::string> imports = split_list(o.imports, ';');
    auto hasSource = [&](const std::string& type, std::string& where) {
        for (const std::string& dir : imports) {
            std::error_code ec;
            const fs::path p = fs::u8path(dir) / fs::u8path(type + ".psc");
            if (fs::exists(p, ec)) {
                where = p.string();
                return true;
            }
        }
        return false;
    };
    int rc = 0;
    for (const pextool::Item& it : items) {
        pextool::Loader ld;
        pex::Script s;
        std::mutex mu;
        if (!load_script(it, ld, s, mu)) {
            rc = 1;
            continue;
        }
        const pex::Closure c = pex::compute_closure(s, resolver, hasSource, true, true);
        size_t nPex = 0, nSrc = 0, nMissing = 0;
        for (const pex::ClosureEntry& e : c.entries) {
            if (e.state == pex::DepState::Pex) ++nPex;
            else if (e.state == pex::DepState::Source) ++nSrc;
            else ++nMissing;
        }
        if (o.json) {
            std::string t = "{\"file\":" + json_str(it.label) + ",\"complete\":" + (c.complete() ? "true" : "false") + ",\"entries\":[";
            for (size_t k = 0; k < c.entries.size(); ++k) {
                const pex::ClosureEntry& e = c.entries[k];
                t += std::string(k ? "," : "") + "{\"type\":" + json_str(e.type) + ",\"state\":\"" + pex::to_string(e.state) + "\",\"neededBy\":" +
                     json_str(e.neededBy) + (e.where.empty() ? "" : ",\"where\":" + json_str(e.where)) + "}";
            }
            t += "],\"missingMembers\":[";
            for (size_t k = 0; k < c.missingMembers.size(); ++k) {
                const pex::MissingMember& m = c.missingMembers[k];
                t += std::string(k ? "," : "") + "{\"type\":" + json_str(m.type) + ",\"member\":" + json_str(m.member) + ",\"reason\":" + json_str(m.reason) + "}";
            }
            t += "]}\n";
            std::fputs(t.c_str(), stdout);
        } else {
            std::printf("%s: %zu scripts in the closure: %zu compiled, %zu source only, %zu missing; %zu members not declared -> %s\n",
                        it.label.c_str(), c.entries.size(), nPex, nSrc, nMissing, c.missingMembers.size(),
                        c.complete() ? "complete" : "INCOMPLETE");
            for (const pex::ClosureEntry& e : c.entries) {
                if (e.state == pex::DepState::Pex && !o.quiet) continue;
                std::printf("  %-8s %s%s%s\n", pex::to_string(e.state), e.type.c_str(),
                            e.neededBy.empty() ? "" : ("  (needed by " + e.neededBy + ")").c_str(), e.where.empty() ? "" : ("  " + e.where).c_str());
            }
            for (const pex::MissingMember& m : c.missingMembers) {
                std::printf("  member   %s.%s: %s\n", m.type.c_str(), m.member.c_str(), m.reason.c_str());
            }
        }
        if (!c.complete()) rc = 1;
        if (!o.emit.empty()) {
            std::error_code ec;
            fs::create_directories(fs::u8path(o.emit), ec);
            pex::DecompileOptions dopt;
            dopt.optionalObjectParams = true;
            dopt.resolver = &resolver;
            for (const pex::ClosureEntry& e : c.entries) {
                if (e.state != pex::DepState::Pex) continue;
                dopt.declarationsOnly = &e != &c.entries.front();  // the script itself in full
                const pex::Script* ps = resolver.find(e.type);
                if (!ps) continue;
                const pex::DecompileResult r = pex::decompile(*ps, dopt);
                const std::string name = ps->objects.empty() ? e.type : std::string(ps->str(ps->objects[0].name));
                std::ofstream f(fs::u8path(o.emit) / fs::u8path(name + ".psc"), std::ios::binary | std::ios::trunc);
                f.write(r.source.data(), static_cast<std::streamsize>(r.source.size()));
            }
        }
    }
    return rc;
}

int cmd_patch(int argc, char** argv) {
    Options o;
    if (!parse(argc, argv, 2, o, "patch")) return 2;
    if (o.map.empty()) {
        std::fprintf(stderr, "patch needs --map <FormIDMap file>\n");
        return 2;
    }
    std::vector<pextool::Item> items;
    if (!collect(o, items)) return 2;
    std::ifstream mf(fs::u8path(o.map), std::ios::binary);
    if (!mf) {
        std::fprintf(stderr, "cannot open %s\n", o.map.c_str());
        return 2;
    }
    const std::string mapText((std::istreambuf_iterator<char>(mf)), std::istreambuf_iterator<char>());
    pex::FormIdMap map;
    std::string err;
    if (!pex::load_formid_map(mapText, map, &err)) {
        std::fprintf(stderr, "%s: %s\n", o.map.c_str(), err.c_str());
        return 2;
    }
    const std::vector<std::string> extra = split_list(o.callees, ',');
    std::printf("MAP|%s|%s|%zu entries\n", map.plugin.c_str(), map.newPlugin.c_str(), map.oldToNew.size());
    std::atomic<int> failures{0};
    std::mutex mu;
    std::vector<std::string> rows(items.size());
    pextool::for_each_parallel(items, o.threads, [&](size_t i, const pextool::Item& it, pextool::Loader& ld) {
        std::string& out = rows[i];
        std::vector<uint8_t> bytes;
        std::string e;
        pex::Script s;
        if (!ld.load(it, bytes, e) || pex::read(bytes.data(), bytes.size(), s, &e) != pex::Status::Ok) {
            out = "ERR|" + it.label + "|" + e + "\n";
            ++failures;
            return;
        }
        // Only a file we can write back byte-exactly is safe to edit.
        if (pex::rewrite_check(bytes.data(), bytes.size(), &e) != pex::Status::Ok) {
            out = "ERR|" + it.label + "|not byte-exact under rewrite: " + e + "\n";
            ++failures;
            return;
        }
        const pex::PatchResult r = pex::apply_formid_map(s, map, extra);
        for (const pex::PatchEdit& ed : r.edits) {
            char buf[256];
            std::snprintf(buf, sizeof buf, "EDIT|%s|%s%s%s|%u|%s|%08X|%08X|%s\n", it.label.c_str(), ed.state.empty() ? "" : ed.state.c_str(),
                          ed.state.empty() ? "" : ".", ed.function.c_str(), unsigned(ed.ip), ed.callee.c_str(), unsigned(ed.oldValue),
                          unsigned(ed.newValue), ed.plugin.c_str());
            out += buf;
        }
        for (const pex::FormRef& sk : r.skipped) {
            char buf[256];
            std::snprintf(buf, sizeof buf, "SKIP|%s|%s%s%s|%u|%s|%s|%s\n", it.label.c_str(), sk.state.empty() ? "" : sk.state.c_str(),
                          sk.state.empty() ? "" : ".", sk.function.c_str(), unsigned(sk.ip), sk.callee.c_str(), pex::to_string(sk.kind),
                          sk.kind == pex::RefKind::Dynamic ? sk.variable.c_str() : "not in map");
            out += buf;
        }
        for (const std::string& rn : r.renamedStrings) out += "RENAME|" + it.label + "|" + rn + "\n";
        if (r.edits.empty() && r.renamedStrings.empty()) {
            out += "UNCHANGED|" + it.label + "\n";
            return;
        }
        if (!it.archive.empty()) {
            out += "ERR|" + it.label + "|inside an archive: cannot write; extract it first\n";
            ++failures;
            return;
        }
        if (o.dryRun) {
            out += "DRYRUN|" + it.label + "|" + std::to_string(r.edits.size()) + " edits, " + std::to_string(r.renamedStrings.size()) + " renames\n";
            return;
        }
        std::vector<uint8_t> newBytes;
        if (pex::write(s, newBytes, &e) != pex::Status::Ok) {
            out += "ERR|" + it.label + "|" + e + "\n";
            ++failures;
            return;
        }
        // Read back and check that every edit took.
        pex::Script check;
        if (pex::read(newBytes.data(), newBytes.size(), check, &e) != pex::Status::Ok) {
            out += "ERR|" + it.label + "|rewritten file does not read back: " + e + "\n";
            ++failures;
            return;
        }
        std::error_code ec;
        if (o.backup) fs::copy_file(it.file, fs::path(it.file).concat(".bak"), fs::copy_options::overwrite_existing, ec);
        std::ofstream f(it.file, std::ios::binary | std::ios::trunc);
        f.write(reinterpret_cast<const char*>(newBytes.data()), static_cast<std::streamsize>(newBytes.size()));
        if (!f) {
            out += "ERR|" + it.label + "|cannot write\n";
            ++failures;
            return;
        }
        out += "PATCHED|" + it.label + "|" + std::to_string(r.edits.size()) + " edits, " + std::to_string(r.renamedStrings.size()) + " renames\n";
    });
    for (const std::string& r : rows) std::fputs(r.c_str(), stdout);
    return failures ? 1 : 0;
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
    if (cmd == "assemble") return cmd_assemble(argc, argv.data());
    if (cmd == "compile") return cmd_compile(argc, argv.data());
    if (cmd == "decompile") return cmd_decompile(argc, argv.data());
    if (cmd == "refs") return cmd_refs(argc, argv.data());
    if (cmd == "deps") return cmd_deps(argc, argv.data());
    if (cmd == "closure") return cmd_closure(argc, argv.data());
    if (cmd == "patch") return cmd_patch(argc, argv.data());
    if (cmd == "check") return cmd_check(argc, argv.data());
    std::fprintf(stderr, "unknown command %s (pex help)\n", cmd.c_str());
    return 2;
}
