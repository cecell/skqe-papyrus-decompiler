// pex: command line front end of skqe-papyrus. Every command takes a .pex
// file, a directory (loose scripts and every .bsa below it) or one .bsa.
// Exit codes: 0 ok, 1 an input failed or a check found problems, 2 bad
// arguments.
// SPDX-License-Identifier: GPL-3.0-only
#include "inputs.hpp"
#include "papyrus/pex.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#endif

using namespace papyrus;
namespace fs = std::filesystem;

namespace {

const char* kHelp = R"(pex - compiled Papyrus script (.pex) tool, Skyrim LE/SE

usage: pex <command> <input> [options]

  <input> is a .pex file, a directory (every .pex below it, loose and inside
  every .bsa) or a single .bsa.

commands
  info <input>                header facts and counts (--json)
  json <input> [--out <path>] the whole model as JSON (a file for one script,
                              a directory for many)
  check --rewrite <input>     read + write every script and compare byte for byte
  help [command]

common options
  --threads N                 worker threads (default: all cores)
  --quiet                     only the summary

exit codes: 0 ok, 1 an input failed or a check found problems, 2 bad arguments
)";

struct Common {
    unsigned threads = 0;
    bool quiet = false;
    bool json = false;
    std::string out;
    std::vector<std::string> inputs;
};

bool parse_common(int argc, char** argv, int from, Common& c, const char* cmd) {
    for (int i = from; i < argc; ++i) {
        std::string a = argv[i];
        const bool hasNext = i + 1 < argc;
        if (a == "--threads" && hasNext) {
            c.threads = static_cast<unsigned>(std::atoi(argv[++i]));
        } else if (a == "--quiet") {
            c.quiet = true;
        } else if (a == "--json") {
            c.json = true;
        } else if (a == "--out" && hasNext) {
            c.out = argv[++i];
        } else if (a.size() > 1 && a[0] == '-' && a != "--rewrite") {
            std::fprintf(stderr, "%s: unknown option %s (pex help %s)\n", cmd, a.c_str(), cmd);
            return false;
        } else if (a != "--rewrite") {
            c.inputs.push_back(a);
        }
    }
    return true;
}

bool collect(const Common& c, std::vector<pextool::Item>& items) {
    if (c.inputs.empty()) {
        std::fprintf(stderr, "no input given\n");
        return false;
    }
    std::vector<std::string> warnings;
    for (const std::string& in : c.inputs) {
        if (!pextool::enumerate(fs::u8path(in), items, warnings)) {
            std::fprintf(stderr, "%s: does not exist\n", in.c_str());
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

// --- info -----------------------------------------------------------------

int cmd_info(int argc, char** argv) {
    Common c;
    if (!parse_common(argc, argv, 2, c, "info")) return 2;
    std::vector<pextool::Item> items;
    if (!collect(c, items)) return 2;

    std::vector<std::string> lines(items.size());
    std::atomic<int> failures{0};
    const bool single = items.size() == 1;
    pextool::for_each_parallel(items, c.threads, [&](size_t i, const pextool::Item& it, pextool::Loader& ld) {
        std::vector<uint8_t> bytes;
        std::string err;
        pex::Script s;
        pex::Status st = pex::Status::IoError;
        if (ld.load(it, bytes, err)) st = pex::read(bytes.data(), bytes.size(), s, &err);
        std::string& out = lines[i];
        if (st != pex::Status::Ok) {
            ++failures;
            out = it.label + ": " + pex::to_string(st) + ": " + err + "\n";
            return;
        }
        const Counts n = count(s);
        const pex::Header& h = s.header;
        char buf[512];
        if (c.json) {
            // One JSON object per line; a single input gives one object.
            std::snprintf(buf, sizeof buf,
                          "{\"file\":\"%s\",\"version\":\"%u.%u\",\"gameId\":%u,\"compileTime\":%llu,"
                          "\"source\":\"%s\",\"user\":\"%s\",\"computer\":\"%s\",\"debug\":%s,\"strings\":%zu,"
                          "\"objects\":%zu,\"variables\":%zu,\"properties\":%zu,\"states\":%zu,\"functions\":%zu,"
                          "\"instructions\":%zu}\n",
                          it.name.c_str(), unsigned(h.major), unsigned(h.minor), unsigned(h.gameId),
                          static_cast<unsigned long long>(h.compileTime), h.sourceName.c_str(), h.userName.c_str(),
                          h.computerName.c_str(), s.debug.present ? "true" : "false", s.strings.size(),
                          s.objects.size(), n.variables, n.properties, n.states, n.functions, n.instructions);
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
            for (const pex::Object& o : s.objects) {
                text += "object:       " + std::string(s.str(o.name));
                if (!s.str(o.parent).empty()) text += " extends " + std::string(s.str(o.parent));
                text += "\n";
                if (!s.str(o.autoState).empty()) text += "auto state:   " + std::string(s.str(o.autoState)) + "\n";
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

// --- json -----------------------------------------------------------------

int cmd_json(int argc, char** argv) {
    Common c;
    if (!parse_common(argc, argv, 2, c, "json")) return 2;
    std::vector<pextool::Item> items;
    if (!collect(c, items)) return 2;
    if (items.size() > 1 && c.out.empty()) {
        std::fprintf(stderr, "json: %zu scripts; give --out <directory>\n", items.size());
        return 2;
    }
    const bool toDir = items.size() > 1 || (!c.out.empty() && fs::is_directory(fs::u8path(c.out)));
    if (toDir) {
        std::error_code ec;
        fs::create_directories(fs::u8path(c.out), ec);
    }
    std::mutex mu;
    std::atomic<int> failures{0};
    pextool::for_each_parallel(items, c.threads, [&](size_t, const pextool::Item& it, pextool::Loader& ld) {
        std::vector<uint8_t> bytes;
        std::string err;
        pex::Script s;
        pex::Status st = pex::Status::IoError;
        if (ld.load(it, bytes, err)) st = pex::read(bytes.data(), bytes.size(), s, &err);
        if (st != pex::Status::Ok) {
            ++failures;
            std::lock_guard<std::mutex> lk(mu);
            std::fprintf(stderr, "%s: %s: %s\n", it.label.c_str(), pex::to_string(st), err.c_str());
            return;
        }
        const std::string text = pex::to_json(s, true);
        if (c.out.empty()) {
            std::fwrite(text.data(), 1, text.size(), stdout);
            return;
        }
        fs::path target = fs::u8path(c.out);
        if (toDir) target /= fs::u8path(it.name).replace_extension(".json");
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

// --- check ----------------------------------------------------------------

int cmd_check(int argc, char** argv) {
    Common c;
    bool rewrite = false;
    for (int i = 2; i < argc; ++i) {
        if (std::strcmp(argv[i], "--rewrite") == 0) rewrite = true;
    }
    if (!parse_common(argc, argv, 2, c, "check")) return 2;
    if (!rewrite) {
        std::fprintf(stderr, "check: give --rewrite (pex help check)\n");
        return 2;
    }
    std::vector<pextool::Item> items;
    if (!collect(c, items)) return 2;

    const auto t0 = std::chrono::steady_clock::now();
    std::mutex mu;
    std::map<std::string, size_t> tally;
    std::atomic<size_t> ok{0}, bad{0};
    pextool::for_each_parallel(items, c.threads, [&](size_t, const pextool::Item& it, pextool::Loader& ld) {
        std::vector<uint8_t> bytes;
        std::string err;
        std::string category;
        if (!ld.load(it, bytes, err)) {
            category = "load failed";
        } else {
            const pex::Status st = pex::rewrite_check(bytes.data(), bytes.size(), &err);
            if (st == pex::Status::Ok) {
                ++ok;
                return;
            }
            category = st == pex::Status::Corrupt && err.rfind("rewrite differs", 0) == 0 ? "rewrite differs"
                                                                                          : pex::to_string(st);
        }
        ++bad;
        std::lock_guard<std::mutex> lk(mu);
        ++tally[category];
        if (!c.quiet) std::fprintf(stdout, "%s: %s: %s\n", it.label.c_str(), category.c_str(), err.c_str());
    });
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    for (const auto& [k, v] : tally) std::fprintf(stdout, "  %-20s %zu\n", k.c_str(), v);
    std::fprintf(stdout, "files %zu  ok %zu  not-ok %zu  %.1fs\n", items.size(), ok.load(), bad.load(), secs);
    return bad ? 1 : 0;
}

int help(const char* cmd) {
    (void)cmd;
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
    if (argc < 2) return help(nullptr), 2;
    const std::string cmd = argv[1];
    if (cmd == "help" || cmd == "--help" || cmd == "-h" || cmd == "/?") return help(argc > 2 ? argv[2] : nullptr);
    if (cmd == "--version" || cmd == "-V") {
        std::printf("pex %s\n", pex::kVersion);
        return 0;
    }
    if (cmd == "info") return cmd_info(argc, argv.data());
    if (cmd == "json") return cmd_json(argc, argv.data());
    if (cmd == "check") return cmd_check(argc, argv.data());
    std::fprintf(stderr, "unknown command %s (pex help)\n", cmd.c_str());
    return 2;
}
