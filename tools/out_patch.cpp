// `pex patch --out` (see out_patch.hpp).
// SPDX-License-Identifier: GPL-3.0-only
#include "out_patch.hpp"

#include "archive_patch.hpp"
#include "papyrus/pex.hpp"

#include "asset_build.h"  // spf: PlanArchives
#include "db_build.h"     // spf: LoadOrder

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <set>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <knownfolders.h>
#include <shlobj.h>
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "advapi32.lib")
#endif

namespace pextool {

namespace fs = std::filesystem;
namespace pex = papyrus::pex;

namespace {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

// The vanilla lists, used when no ini sets them; the same as spf's and the
// Skyrim_Default.ini the game ships.
const char* const kDefaultList1 =
    "Skyrim - Misc.bsa, Skyrim - Shaders.bsa, Skyrim - Interface.bsa, Skyrim - Animations.bsa, Skyrim - Meshes0.bsa, "
    "Skyrim - Meshes1.bsa, Skyrim - Sounds.bsa";
const char* const kDefaultList2 =
    "Skyrim - Voices_en0.bsa, Skyrim - Textures0.bsa, Skyrim - Textures1.bsa, Skyrim - Textures2.bsa, Skyrim - "
    "Textures3.bsa, Skyrim - Textures4.bsa, Skyrim - Textures5.bsa, Skyrim - Textures6.bsa, Skyrim - Textures7.bsa, "
    "Skyrim - Textures8.bsa, Skyrim - Patch.bsa";

std::string read_text(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// key's value in [section]; false when absent. The last occurrence wins.
bool ini_value(const std::string& text, const char* section, const char* key, std::string& value) {
    std::string current;
    bool found = false;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) nl = text.size();
        std::string line = text.substr(pos, nl - pos);
        pos = nl + 1;
        const size_t a = line.find_first_not_of(" \t\r");
        if (a == std::string::npos || line[a] == ';' || line[a] == '#') continue;
        line = line.substr(a);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
        if (line[0] == '[') {
            const size_t close = line.find(']');
            current = lower(line.substr(1, close == std::string::npos ? std::string::npos : close - 1));
            continue;
        }
        const size_t eq = line.find('=');
        if (eq == std::string::npos || current != lower(section)) continue;
        std::string k = line.substr(0, eq);
        while (!k.empty() && (k.back() == ' ' || k.back() == '\t')) k.pop_back();
        if (lower(k) != lower(key)) continue;
        const size_t b = line.find_first_not_of(" \t", eq + 1);
        value = b == std::string::npos ? std::string() : line.substr(b);
        found = true;
    }
    return found;
}

std::vector<std::string> split_archive_list(const std::string& list) {
    std::vector<std::string> out;
    size_t pos = 0;
    while (pos <= list.size()) {
        size_t comma = list.find(',', pos);
        if (comma == std::string::npos) comma = list.size();
        std::string name = list.substr(pos, comma - pos);
        pos = comma + 1;
        const size_t a = name.find_first_not_of(" \t");
        if (a == std::string::npos) continue;
        const size_t b = name.find_last_not_of(" \t");
        out.push_back(name.substr(a, b - a + 1));
    }
    return out;
}

std::vector<fs::path> ini_search_dirs(const std::vector<fs::path>& given) {
    std::vector<fs::path> dirs = given;
#ifdef _WIN32
    PWSTR documents = nullptr;
    if (SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &documents) == S_OK) {
        dirs.push_back(fs::path(documents) / "My Games" / "Skyrim Special Edition");
    }
    CoTaskMemFree(documents);
#endif
    return dirs;
}

std::vector<std::string> ini_archives(const std::vector<fs::path>& iniDirs, std::string& source) {
    std::string list1 = kDefaultList1, list2 = kDefaultList2;
    std::string from;
    for (const fs::path& dir : ini_search_dirs(iniDirs)) {
        bool any = false;
        for (const char* name : {"Skyrim.ini", "SkyrimCustom.ini"}) {
            const fs::path file = dir / name;
            std::error_code ec;
            if (!fs::exists(file, ec)) continue;
            any = true;
            const std::string text = read_text(file);
            std::string v;
            bool used = false;
            if (ini_value(text, "Archive", "sResourceArchiveList", v)) {
                list1 = v;
                used = true;
            }
            if (ini_value(text, "Archive", "sResourceArchiveList2", v)) {
                list2 = v;
                used = true;
            }
            if (used) from += (from.empty() ? "" : " + ") + file.u8string();
        }
        if (any) break;  // the first folder that has inis is the one the game reads
    }
    source = from.empty() ? "the vanilla lists (no Skyrim.ini or SkyrimCustom.ini sets them)" : from;
    std::vector<std::string> out = split_archive_list(list1);
    for (std::string& n : split_archive_list(list2)) out.push_back(std::move(n));
    return out;
}

// Finds a folder by name in any case; empty when there is none.
fs::path find_scripts_folder(const fs::path& parent) {
    std::error_code ec;
    for (fs::directory_iterator it(parent, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->is_directory(ec) && lower(it->path().filename().u8string()) == "scripts") return it->path();
    }
    return fs::path();
}

void add_loose_copies(const fs::path& parent, CopyKind kind, std::map<std::string, std::vector<ScriptCopy>>& out) {
    const fs::path folder = find_scripts_folder(parent);
    if (folder.empty()) return;
    std::error_code ec;
    for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec) || !iequals_ext(it->path(), ".pex")) continue;
        ScriptCopy c;
        c.kind = kind;
        c.item.file = it->path();
        c.item.name = it->path().filename().u8string();
        c.item.label = it->path().u8string();
        out[lower(c.item.name)].push_back(std::move(c));
    }
}

// scripts\name.pex, and not deeper.
bool is_live_member(const std::string& entryPath) {
    const std::string p = lower(entryPath);
    if (p.rfind("scripts\\", 0) != 0 && p.rfind("scripts/", 0) != 0) return false;
    return p.find_first_of("\\/", 8) == std::string::npos;
}

// What one copy comes to under the map.
struct CopyResult {
    bool read = false;
    std::string readError;
    std::vector<pex::PatchEdit> edits;
    std::vector<std::string> pairs;
    size_t renames = 0, notInMap = 0, dynamic = 0;
    std::string lines;  // EDIT, SKIP, RENAME
    std::vector<uint8_t> newBytes;
    std::string refusal, refusalCode;
    std::string sourceHash;  // of the winning copy's own bytes
};

// Identifies a set of pairs: its entries are in key order, so equal sets match.
std::string pairs_hash(const pex::FormIdMap& map) {
    uint64_t h = 0xCBF29CE484222325ull;
    auto add = [&](uint32_t v) {
        for (int i = 0; i < 4; ++i) {
            h ^= static_cast<uint8_t>(v >> (8 * i));
            h *= 0x100000001B3ull;
        }
    };
    for (const auto& [oldId, newId] : map.oldToNew) {
        add(oldId);
        add(newId);
    }
    char buf[20];
    std::snprintf(buf, sizeof buf, "%016llX", static_cast<unsigned long long>(h));
    return buf;
}

// In the state file's plugin field: the output copy was rebuilt from a copy
// outside the folder, so what earlier lines say about that script no longer holds.
constexpr const char* kNewCopy = "*new-copy*";

constexpr const char* kSource = "*source*";  // the copy an output copy was built from
constexpr const char* kWrote = "*wrote*";    // the bytes of a copy written there

struct OutputState {
    std::map<std::pair<std::string, std::string>, std::string> applied;  // (script, plugin) -> hash of the pairs
    std::map<std::string, std::string> source;                           // script -> "<label>#<hash>"
    std::map<std::string, std::set<std::string>> wrote;                  // script -> hashes of every copy written
};

OutputState read_output_state(const fs::path& file) {
    OutputState out;
    const std::string text = read_text(file);
    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) break;  // a torn last line was never acted on
        const std::string line = text.substr(pos, nl - pos);
        pos = nl + 1;
        if (line.empty() || line[0] == '#') continue;
        const size_t a = line.find('|'), b = a == std::string::npos ? a : line.find('|', a + 1);
        if (b == std::string::npos) continue;
        const std::string script = line.substr(0, a), plugin = line.substr(a + 1, b - a - 1);
        if (plugin == kNewCopy) {
            for (auto it = out.applied.lower_bound({script, std::string()}); it != out.applied.end() && it->first.first == script;) it = out.applied.erase(it);
            out.source.erase(script);
        } else if (plugin == kSource) {
            out.source[script] = line.substr(b + 1);
        } else if (plugin == kWrote) {
            out.wrote[script].insert(line.substr(b + 1));
        } else {
            out.applied[{script, plugin}] = line.substr(b + 1);
        }
    }
    return out;
}

// Takes out a loose copy that is really this output folder's own file, installed.
// There is only one loose file, so if the copy was built from another loose file,
// a mod's, that one is now hidden under ours and cannot be seen. It is still the
// source, or every install would look like a change back to some older archive.
// Those are returned: script -> our installed file.
std::map<std::string, fs::path> drop_our_installed_copies(std::map<std::string, std::vector<ScriptCopy>>& scripts, const OutputState& state, const fs::path& outFolder) {
    std::map<std::string, fs::path> hiddenSources;
    if (outFolder.empty()) return hiddenSources;
    const fs::path ours = find_scripts_folder(outFolder);
    for (auto& [key, copies] : scripts) {
        for (size_t i = 0; i < copies.size(); ++i) {
            if (copies[i].kind != CopyKind::Loose) continue;
            const auto wrote = state.wrote.find(key);
            std::error_code ec;
            const fs::path mine = ours.empty() ? fs::path() : ours / fs::u8path(copies[i].item.name);
            const bool outCopy = !mine.empty() && fs::is_regular_file(mine, ec);
            if (wrote == state.wrote.end() && !outCopy) continue;
            const std::string text = read_text(copies[i].item.file);
            const std::string hash = bytes_hash(std::vector<uint8_t>(text.begin(), text.end()));
            bool isOurs = wrote != state.wrote.end() && wrote->second.count(hash) != 0;
            if (!isOurs && outCopy) isOurs = read_text(mine) == text;
            if (!isOurs) continue;
            const auto source = state.source.find(key);
            if (source != state.source.end() && lower(source->second).find(".bsa:") == std::string::npos) hiddenSources[key] = copies[i].item.file;
            copies.erase(copies.begin() + static_cast<std::ptrdiff_t>(i--));
        }
    }
    return hiddenSources;
}

// The copy that would win if the output folder were not there.
int winning_source_copy(const std::vector<ScriptCopy>& copies) {
    int best = -1;
    for (size_t i = 0; i < copies.size(); ++i) {
        const ScriptCopy& c = copies[i];
        if (c.kind == CopyKind::NotLoaded || c.kind == CopyKind::Output) continue;
        if (best < 0 || c.kind > copies[best].kind || (c.kind == copies[best].kind && c.rank > copies[best].rank)) best = static_cast<int>(i);
    }
    return best;
}

void join_pairs(const std::vector<std::string>& pairs, std::string& out) {
    for (size_t i = 0; i < pairs.size(); ++i) out += (i ? "," : "") + pairs[i];
}

}  // namespace

std::string render_output_log(const OutputLog& log) {
    const char* patchedWord = log.dryRun ? "would-patch" : "patched";
    std::string out = "pex patch output log v2\n";
    out += "started|" + log.started + "\n";
    out += std::string("mode|") + (log.dryRun ? "dry-run" : "write") + "\n";
    out += std::string("update|") + (log.update ? "yes" : "no") + "\n";
    out += "data|" + log.data + "\n";
    out += "output|" + log.outFolder + "\n";
    out += "load-order|" + log.pluginsTxt + "\n";
    out += std::string("plugins-counted|") + (log.allPlugins ? "all-listed" : "active") + "\n";
    out += "archive-lists|" + log.iniSource + "\n";
    out += "plugin|" + log.plugin + "\n";
    out += "new-plugin|" + log.newPlugin + "\n";
    out += "source|" + log.source + "\n";
    out += "pairs|" + std::to_string(log.pairs) + "\n";
    out += "archives-loaded|" + std::to_string(log.archivesLoaded) + "\n";
    out += "archives-not-loaded|" + std::to_string(log.archivesNotLoaded) + "\n";

    size_t edits = 0, notInMap = 0, dynamic = 0, lostNotLoaded = 0;
    std::map<std::string, size_t> reasons;
    out += "\n[output]\n";
    for (const OutputLog::Entry& e : log.patched) {
        out += std::string(patchedWord) + "|" + e.name + "|" + std::to_string(e.edits) + "|" + std::to_string(e.renames) + "|";
        join_pairs(e.pairs, out);
        out += "|" + std::to_string(e.notInMap) + "|" + std::to_string(e.dynamic) + "|" + e.kind + "|" + e.source + "|" + e.outFile + "|" + (e.replaced ? "replaced" : "new") + "\n";
        edits += e.edits;
        notInMap += e.notInMap;
        dynamic += e.dynamic;
    }
    for (const OutputLog::Removed& r : log.removed) out += "removed|" + r.name + "|" + r.winner + "|" + r.outFile + "\n";
    out += "\n[skipped]\n";
    for (const OutputLog::Entry& e : log.skipped) {
        std::string detail = e.detail;
        std::replace(detail.begin(), detail.end(), '|', '/');
        std::replace(detail.begin(), detail.end(), '\n', ' ');
        out += "skipped|" + e.name + "|" + e.reason + "|" + e.source + "|" + detail + "\n";
        ++reasons[e.reason];
        notInMap += e.notInMap;
        dynamic += e.dynamic;
    }
    out += "\n[losing copies]\n";
    for (const OutputLog::Lost& l : log.lost) {
        out += "lost|" + l.name + "|" + std::to_string(l.edits) + "|";
        join_pairs(l.pairs, out);
        out += "|" + l.kind + "|" + l.source + "|" + l.winner + "\n";
        if (l.winner.empty()) ++lostNotLoaded;
    }
    out += "\n[totals]\n";
    out += "scripts|" + std::to_string(log.scripts) + "\n";
    out += std::string(patchedWord) + "|" + std::to_string(log.patched.size()) + "\n";
    out += "removed|" + std::to_string(log.removed.size()) + "\n";
    out += "edits|" + std::to_string(edits) + "\n";
    out += "skipped|" + std::to_string(log.skipped.size()) + "\n";
    for (const auto& [reason, n] : reasons) out += "skipped." + reason + "|" + std::to_string(n) + "\n";
    out += "lost|" + std::to_string(log.lost.size()) + "\n";
    out += "lost.no-copy-loads|" + std::to_string(lostNotLoaded) + "\n";
    out += "refs-not-in-map|" + std::to_string(notInMap) + "\n";
    out += "refs-dynamic|" + std::to_string(dynamic) + "\n";
    return out;
}

fs::path output_state_path(const fs::path& outFolder) {
    std::error_code ec;
    fs::path p = fs::absolute(outFolder, ec);
    if (ec) p = outFolder;
    p = p.lexically_normal();
    if (!p.has_filename()) p = p.parent_path();
    return fs::path(p).concat(".pexpatch.state");
}

// --- finding the game --------------------------------------------------------------

namespace {

const char* game_folder_name(GameStore store) { return store == GameStore::Gog ? "Skyrim Special Edition GOG" : "Skyrim Special Edition"; }

}  // namespace

fs::path default_plugins_txt(GameStore store) {
#ifdef _WIN32
    const wchar_t* local = _wgetenv(L"LOCALAPPDATA");
    if (!local || !*local) return fs::path();
    return fs::path(local) / game_folder_name(store) / "plugins.txt";
#else
    (void)store;
    return fs::path();
#endif
}

fs::path game_ini_dir(GameStore store) {
    fs::path out;
#ifdef _WIN32
    PWSTR documents = nullptr;
    if (SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &documents) == S_OK) out = fs::path(documents) / "My Games" / game_folder_name(store);
    CoTaskMemFree(documents);
#else
    (void)store;
#endif
    return out;
}

fs::path choose_data_folder(const std::vector<std::pair<std::string, fs::path>>& installs, std::string& why) {
    std::vector<std::pair<std::string, fs::path>> found;
    std::string looked;
    for (const auto& [where, install] : installs) {
        looked += (looked.empty() ? "" : "; ") + where + ": " + install.u8string();
        std::error_code ec;
        const fs::path data = (install / "Data").lexically_normal();
        if (fs::is_directory(data, ec)) found.emplace_back(where, data);
    }
    if (found.empty()) {
        why = looked.empty() ? "the registry has no record of it" : "the registry names an install with no Data folder (" + looked + ")";
        return fs::path();
    }
    for (const auto& f : found) {
        if (lower(f.second.u8string()) != lower(found[0].second.u8string())) {
            why = "the registry names two installs: " + found[0].first + " has " + found[0].second.u8string() + ", " + f.first + " has " + f.second.u8string();
            return fs::path();
        }
    }
    return found[0].second;
}

bool pick_game(const std::vector<std::pair<std::string, fs::path>>& steam, const std::vector<std::pair<std::string, fs::path>>& gog, GameInstall& out,
               std::string& why) {
    std::string steamWhy, gogWhy;
    const fs::path s = choose_data_folder(steam, steamWhy);
    const fs::path g = choose_data_folder(gog, gogWhy);
    for (const std::string* w : {&steamWhy, &gogWhy}) {
        if (w->find("two installs") != std::string::npos) {
            why = *w;
            return false;
        }
    }
    if (!s.empty() && !g.empty() && lower(s.u8string()) != lower(g.u8string())) {
        why = "both are installed, Steam at " + s.u8string() + " and GOG at " + g.u8string();
        return false;
    }
    if (!s.empty()) {
        out = {s, GameStore::Steam};
        return true;
    }
    if (!g.empty()) {
        out = {g, GameStore::Gog};
        return true;
    }
    why = "looked for Skyrim Special Edition from Steam (" + steamWhy + ") and from GOG (" + gogWhy + ")";
    return false;
}

namespace {

using InstallList = std::vector<std::pair<std::string, fs::path>>;

#ifdef _WIN32
bool registry_text(HKEY root, const wchar_t* key, const wchar_t* name, DWORD view, std::wstring& out) {
    wchar_t buf[2048];
    DWORD size = sizeof buf;
    if (RegGetValueW(root, key, name, RRF_RT_REG_SZ | view, nullptr, buf, &size) != ERROR_SUCCESS) return false;
    out = buf;
    return !out.empty();
}
#endif

void read_game_records(InstallList& steam, InstallList& gog) {
#ifdef _WIN32
    std::wstring path, name;
    if (registry_text(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Bethesda Softworks\\Skyrim Special Edition", L"Installed Path", RRF_SUBKEY_WOW6432KEY, path)) {
        steam.emplace_back("Bethesda's registry key", fs::path(path));
    }
    // Steam's entry for the app, by its app ID; the name is checked so that an
    // entry for something else is not taken for the game.
    const wchar_t* const steamKey = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Steam App 489830";
    if (registry_text(HKEY_LOCAL_MACHINE, steamKey, L"DisplayName", RRF_SUBKEY_WOW6464KEY, name) && name.find(L"Skyrim Special Edition") != std::wstring::npos &&
        registry_text(HKEY_LOCAL_MACHINE, steamKey, L"InstallLocation", RRF_SUBKEY_WOW6464KEY, path)) {
        steam.emplace_back("Steam's app entry", fs::path(path));
    }

    if (registry_text(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Bethesda Softworks\\Skyrim Special Edition GOG", L"Installed Path", RRF_SUBKEY_WOW6432KEY, path)) {
        gog.emplace_back("Bethesda's GOG registry key", fs::path(path));
    }
    // GOG lists each game under its product ID; it is found by name, because the
    // Special and Anniversary editions have IDs of their own.
    HKEY games = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\GOG.com\\Games", 0, KEY_READ | KEY_WOW64_32KEY, &games) == ERROR_SUCCESS) {
        for (DWORD i = 0;; ++i) {
            wchar_t id[256];
            DWORD idSize = 256;
            if (RegEnumKeyExW(games, i, id, &idSize, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
            if (!registry_text(games, id, L"gameName", 0, name) || name.find(L"Skyrim") == std::wstring::npos) continue;
            if (name.find(L"Special Edition") == std::wstring::npos && name.find(L"Anniversary") == std::wstring::npos) continue;
            if (registry_text(games, id, L"path", 0, path)) gog.emplace_back("GOG's game entry", fs::path(path));
        }
        RegCloseKey(games);
    }
#else
    (void)steam;
    (void)gog;
#endif
}

}  // namespace

bool find_game(GameInstall& out, std::string& why) {
    InstallList steam, gog;
    read_game_records(steam, gog);
    return pick_game(steam, gog, out, why);
}

GameStore store_of_data(const fs::path& data) {
    InstallList steam, gog;
    read_game_records(steam, gog);
    std::error_code ec;
    const fs::path a = fs::absolute(data, ec);
    std::string given = lower((ec ? data : a).lexically_normal().u8string());
    while (!given.empty() && (given.back() == '\\' || given.back() == '/')) given.pop_back();
    for (const auto& [where, install] : gog) {
        if (lower((install / "Data").lexically_normal().u8string()) == given) return GameStore::Gog;
    }
    return GameStore::Steam;
}

// --- the load order ---------------------------------------------------------------

namespace {

// One name per line of a plugins.txt or Skyrim.ccc: no BOM, no line ending, no
// comment or blank line, and no active marker, whose meaning is the caller's.
std::vector<std::string> listed_plugin_names(const fs::path& file) {
    std::vector<std::string> out;
    std::string text = read_text(file);
    if (text.rfind("\xEF\xBB\xBF", 0) == 0) text.erase(0, 3);
    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) nl = text.size();
        std::string line = text.substr(pos, nl - pos);
        pos = nl + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        if (line[0] == '*') line.erase(0, 1);
        if (!line.empty()) out.push_back(std::move(line));
    }
    return out;
}

// The same order spf builds from plugins.txt's active lines, with the inactive
// lines kept: spf takes this list as given and only moves masters first.
std::vector<std::string> every_listed_plugin(const fs::path& data, const fs::path& pluginsTxt) {
    std::vector<std::string> names = {"Skyrim.esm", "Update.esm", "Dawnguard.esm", "HearthFires.esm", "Dragonborn.esm"};
    std::error_code ec;
    const fs::path parent = fs::absolute(data, ec).lexically_normal().parent_path();
    const fs::path game = parent.has_filename() ? parent : parent.parent_path();
    for (std::string& n : listed_plugin_names(game / "Skyrim.ccc")) names.push_back(std::move(n));
    for (std::string& n : listed_plugin_names(pluginsTxt)) names.push_back(std::move(n));
    std::map<std::string, std::string> onDisk;  // lower -> as spelled in Data
    for (fs::directory_iterator it(data, ec), end; !ec && it != end; it.increment(ec)) {
        const fs::path& p = it->path();
        if (iequals_ext(p, ".esm") || iequals_ext(p, ".esp") || iequals_ext(p, ".esl")) {
            const std::string name = p.filename().u8string();
            onDisk[lower(name)] = name;
        }
    }
    std::vector<std::string> out;
    std::set<std::string> seen;
    for (const std::string& n : names) {
        const auto found = onDisk.find(lower(n));
        if (found != onDisk.end() && seen.insert(found->first).second) out.push_back(found->second);
    }
    return out;
}

}  // namespace

bool plan_archives(const fs::path& data, const fs::path& pluginsTxt, const std::vector<fs::path>& iniDirs, bool allPlugins, ArchivePlan& out,
                   std::string& why) {
    std::error_code ec;
    if (!fs::is_directory(data, ec)) {
        why = data.u8string() + " is not a folder";
        return false;
    }
    if (!fs::is_regular_file(pluginsTxt, ec)) {
        why = "cannot read the load order " + pluginsTxt.u8string();
        return false;
    }
    spf::BuildOptions lo;
    lo.data = data.wstring();
    lo.loadOrderFile = pluginsTxt.wstring();
    if (allPlugins) lo.plugins = every_listed_plugin(data, pluginsTxt);
    std::string err;
    const std::vector<std::string> order = spf::LoadOrder(lo, nullptr, &err);
    if (!err.empty()) {
        why = "the load order: " + err;
        return false;
    }
    if (order.empty()) {
        why = "the load order has no plugin that exists in " + data.u8string();
        return false;
    }
    out.plugins = order.size();

    // A plugin's own ini could change the archive lists, which the rules here
    // do not model: stop rather than name a winner that may be wrong.
    for (const std::string& plugin : order) {
        const size_t dot = plugin.rfind('.');
        const fs::path ini = data / fs::u8path(plugin.substr(0, dot) + ".ini");
        if (fs::exists(ini, ec) && lower(read_text(ini)).find("sresourcearchive") != std::string::npos) {
            why = ini.filename().u8string() + " changes the archive lists, so the winning archives cannot be established";
            return false;
        }
    }

    std::vector<std::string> names;
    for (fs::directory_iterator it(data, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->is_regular_file(ec) && iequals_ext(it->path(), ".bsa")) names.push_back(it->path().filename().u8string());
    }
    std::sort(names.begin(), names.end());
    const std::vector<spf::assets::ArchiveLoad> plan = spf::assets::PlanArchives(names, ini_archives(iniDirs, out.iniSource), order);
    std::set<std::string> loadedNames;
    for (const spf::assets::ArchiveLoad& a : plan) {
        ArchivePlan::Loaded l;
        l.path = data / fs::u8path(a.name);
        l.rank = a.rank;
        l.loadedBy = a.loadedBy;
        l.plugin = a.plugin;
        out.loaded.push_back(std::move(l));
        loadedNames.insert(a.name);
    }
    for (const std::string& n : names) {
        if (!loadedNames.count(n)) out.notLoaded.push_back(data / fs::u8path(n));
    }
    return true;
}

const char* to_string(CopyKind k) noexcept {
    switch (k) {
        case CopyKind::Output: return "output";
        case CopyKind::Loose: return "loose";
        case CopyKind::Archive: return "archive";
        case CopyKind::NotLoaded: return "not-loaded";
    }
    return "";
}

int winning_copy(const std::vector<ScriptCopy>& copies) {
    int best = -1;
    for (size_t i = 0; i < copies.size(); ++i) {
        const ScriptCopy& c = copies[i];
        if (c.kind == CopyKind::NotLoaded) continue;
        if (best < 0 || c.kind > copies[best].kind || (c.kind == copies[best].kind && c.rank > copies[best].rank)) best = static_cast<int>(i);
    }
    return best;
}

std::map<std::string, std::vector<ScriptCopy>> collect_script_copies(const fs::path& data, const fs::path& outFolder, const ArchivePlan& plan,
                                                                     std::vector<std::string>& warnings) {
    std::map<std::string, std::vector<ScriptCopy>> out;
    auto add_archive = [&](const fs::path& archive, CopyKind kind, int rank) {
        std::vector<Item> items;
        enumerate(archive, items, warnings);
        for (Item& it : items) {
            if (!is_live_member(it.entryPath)) continue;
            ScriptCopy c;
            c.kind = kind;
            c.rank = rank;
            const std::string key = lower(it.name);
            c.item = std::move(it);
            out[key].push_back(std::move(c));
        }
    };
    for (const ArchivePlan::Loaded& l : plan.loaded) add_archive(l.path, CopyKind::Archive, l.rank);
    for (const fs::path& p : plan.notLoaded) add_archive(p, CopyKind::NotLoaded, 0);
    add_loose_copies(data, CopyKind::Loose, out);
    if (!outFolder.empty()) add_loose_copies(outFolder, CopyKind::Output, out);
    return out;
}

std::string check_output_folder(const fs::path& data, const fs::path& outFolder) {
    std::error_code ec;
    auto normal = [&](const fs::path& p) {
        const fs::path a = fs::absolute(p, ec);
        std::string s = lower((ec ? p : a).lexically_normal().u8string());
        while (s.size() > 1 && (s.back() == '\\' || s.back() == '/')) s.pop_back();
        return s + static_cast<char>(fs::path::preferred_separator);
    };
    const std::string d = normal(data), o = normal(outFolder);
    if (o.rfind(d, 0) == 0) return "the output folder is inside the Data folder, where the game would see it; give a folder outside it";
    if (d.rfind(o, 0) == 0) return "the output folder contains the Data folder; give a separate folder";
    return std::string();
}

// --- the run ------------------------------------------------------------------------

int patch_to_output(const OutOptions& options, const pex::FormIdMap& map, std::string& report, std::string& logText) {
    std::string gameWhy;
    if (!options.dryRun && !verify_game_not_running(gameWhy)) {
        report += "ERR|game|" + gameWhy + "; nothing written\n";
        return 1;
    }
    ArchivePlan plan;
    std::string why;
    if (!plan_archives(options.data, options.pluginsTxt, options.iniDirs, options.allPlugins, plan, why)) {
        report += "ERR|load-order|" + why + "; nothing written\n";
        return 1;
    }
    std::vector<std::string> warnings;
    std::map<std::string, std::vector<ScriptCopy>> scripts = collect_script_copies(options.data, options.outFolder, plan, warnings);
    const fs::path stateFile = output_state_path(options.outFolder);
    const OutputState state = read_output_state(stateFile);
    const std::map<std::string, fs::path> hiddenSources = drop_our_installed_copies(scripts, state, options.outFolder);
    for (const std::string& w : warnings) report += "NOTE|" + w + "\n";
    report += std::string("ORDER|") + (options.allPlugins ? "every listed plugin, " : "active plugins, ") + std::to_string(plan.plugins) + " plugins, " + std::to_string(plan.loaded.size()) + " archives load, " +
              std::to_string(plan.notLoaded.size()) + " do not|archive lists from " + plan.iniSource + "\n";

    // Flatten for the workers: a copy's bytes are read once, on any thread.
    struct Slot {
        const ScriptCopy* copy;
        bool winner;
        const std::string* key;
    };
    std::vector<Item> items;
    std::vector<Slot> slots;
    std::map<std::string, int> winners;
    std::set<std::string> rebuilding;  // --update: the output copy's source is no longer the winner
    Loader sourceLoader;
    for (const auto& [key, copies] : scripts) {
        int w = winning_copy(copies);
        if (options.update && w >= 0 && copies[static_cast<size_t>(w)].kind == CopyKind::Output && !hiddenSources.count(key)) {
            const int t = winning_source_copy(copies);
            if (t >= 0) {
                const Item& it = copies[static_cast<size_t>(t)].item;
                std::vector<uint8_t> bytes;
                std::string e;
                const std::string identity = it.label + "#" + (sourceLoader.load(it, bytes, e) ? bytes_hash(bytes) : std::string());
                const auto known = state.source.find(key);
                if (known == state.source.end() || known->second != identity) {
                    w = t;
                    rebuilding.insert(key);
                }
            }
        }
        winners[key] = w;
        for (size_t i = 0; i < copies.size(); ++i) {
            items.push_back(copies[i].item);
            slots.push_back({&copies[i], static_cast<int>(i) == w, &key});
        }
    }
    sourceLoader = Loader();  // no mapping is kept open past this point
    std::vector<CopyResult> results(items.size());
    for_each_parallel(items, options.threads, [&](size_t i, const Item& it, Loader& ld) {
        CopyResult& res = results[i];
        std::vector<uint8_t> bytes;
        std::string e;
        pex::Script s;
        if (!ld.load(it, bytes, e) || pex::read(bytes.data(), bytes.size(), s, &e) != pex::Status::Ok) {
            res.readError = e;
            return;
        }
        res.read = true;
        if (slots[i].winner) res.sourceHash = bytes_hash(bytes);
        const pex::PatchResult r = pex::apply_formid_map(s, map, options.extraCallees);
        res.edits = r.edits;
        res.renames = r.renamedStrings.size();
        for (const pex::FormRef& sk : r.skipped) ++(sk.kind == pex::RefKind::Dynamic ? res.dynamic : res.notInMap);
        if (r.edits.empty() && r.renamedStrings.empty()) return;
        for (const pex::PatchEdit& ed : r.edits) {
            char pair[32];
            std::snprintf(pair, sizeof pair, "%08X->%08X", unsigned(ed.oldValue), unsigned(ed.newValue));
            if (std::find(res.pairs.begin(), res.pairs.end(), pair) == res.pairs.end()) res.pairs.push_back(pair);
        }
        if (!slots[i].winner) return;  // a losing copy is only counted
        for (const pex::PatchEdit& ed : r.edits) {
            char buf[256];
            std::snprintf(buf, sizeof buf, "%s%s%s|%u|%s|%08X|%08X|%s\n", ed.state.empty() ? "" : ed.state.c_str(), ed.state.empty() ? "" : ".",
                          ed.function.c_str(), unsigned(ed.ip), ed.callee.c_str(), unsigned(ed.oldValue), unsigned(ed.newValue), ed.plugin.c_str());
            res.lines += "EDIT|" + it.label + "|" + buf;
        }
        for (const pex::FormRef& sk : r.skipped) {
            char buf[256];
            std::snprintf(buf, sizeof buf, "%s%s%s|%u|%s|%s|%s\n", sk.state.empty() ? "" : sk.state.c_str(), sk.state.empty() ? "" : ".",
                          sk.function.c_str(), unsigned(sk.ip), sk.callee.c_str(), pex::to_string(sk.kind),
                          sk.kind == pex::RefKind::Dynamic ? sk.variable.c_str() : "not in map");
            res.lines += "SKIP|" + it.label + "|" + buf;
        }
        for (const std::string& rn : r.renamedStrings) res.lines += "RENAME|" + it.label + "|" + rn + "\n";
        // Only a file the writer reproduces exactly is safe to edit.
        if (pex::rewrite_check(bytes.data(), bytes.size(), &e) != pex::Status::Ok) {
            res.refusalCode = "not-byte-exact";
            res.refusal = "not byte-exact under rewrite: " + e;
            return;
        }
        if (pex::write(s, res.newBytes, &e) != pex::Status::Ok) {
            res.refusalCode = "failed";
            res.refusal = "cannot write the patched model: " + e;
        }
    });

    // The scan can take seconds: look again before anything is written.
    if (!options.dryRun && !verify_game_not_running(gameWhy)) {
        report += "ERR|game|" + gameWhy + "; nothing written\n";
        return 1;
    }

    OutputLog log;
    log.dryRun = options.dryRun;
    log.allPlugins = options.allPlugins;
    log.update = options.update;
    log.started = utc_timestamp();
    log.data = options.data.u8string();
    log.outFolder = options.outFolder.u8string();
    log.pluginsTxt = options.pluginsTxt.u8string();
    log.iniSource = plan.iniSource;
    log.plugin = map.plugin;
    log.newPlugin = map.newPlugin;
    log.source = options.source;
    log.pairs = map.oldToNew.size();
    log.archivesLoaded = plan.loaded.size();
    log.archivesNotLoaded = plan.notLoaded.size();
    log.scripts = scripts.size();

    int failures = 0;
    const auto& applied = state.applied;
    const std::string thisHash = pairs_hash(map);
    const std::string pluginKey = lower(map.plugin);
    const fs::path scriptsFolder = [&] {
        const fs::path existing = find_scripts_folder(options.outFolder);
        return existing.empty() ? options.outFolder / "scripts" : existing;
    }();

    for (size_t i = 0; i < items.size(); ++i) {
        const Slot& slot = slots[i];
        const CopyResult& res = results[i];
        const std::vector<ScriptCopy>& copies = scripts[*slot.key];
        const int w = winners[*slot.key];
        if (!slot.winner) {
            // A copy the game does not use, that would have changed: worth saying.
            // The output copy passed over by --update is about to be replaced.
            if (slot.copy->kind == CopyKind::Output || (res.edits.empty() && res.renames == 0)) continue;
            OutputLog::Lost lost;
            lost.name = items[i].name;
            lost.source = items[i].label;
            lost.edits = res.edits.size();
            lost.pairs = res.pairs;
            lost.kind = to_string(slot.copy->kind);
            lost.winner = w >= 0 ? copies[static_cast<size_t>(w)].item.label : std::string();
            report += "LOST|" + lost.source + "|" + std::to_string(lost.edits) + " edits|" +
                      (w >= 0 ? "the game loads " + lost.winner : std::string("its archive does not load")) + "\n";
            log.lost.push_back(std::move(lost));
            continue;
        }

        report += res.lines;
        const std::string kind = to_string(slot.copy->kind);
        OutputLog::Entry entry;
        entry.name = items[i].name;
        entry.source = items[i].label;
        entry.kind = kind;
        entry.edits = res.edits.size();
        entry.renames = res.renames;
        entry.pairs = res.pairs;
        entry.notInMap = res.notInMap;
        entry.dynamic = res.dynamic;

        if (!res.read) {
            report += "ERR|" + items[i].label + "|" + res.readError + "\n";
            entry.reason = "unreadable";
            entry.detail = res.readError;
            log.skipped.push_back(std::move(entry));
            ++failures;
            continue;
        }
        if (!res.refusal.empty()) {
            report += "ERR|" + items[i].label + "|" + res.refusal + "\n";
            entry.reason = res.refusalCode;
            entry.detail = res.refusal;
            log.skipped.push_back(std::move(entry));
            ++failures;
            continue;
        }
        const bool replacing = rebuilding.count(*slot.key) != 0;
        if (res.edits.empty() && res.renames == 0 && replacing && applied.count({*slot.key, pluginKey})) {
            // The new winner has nothing for this plugin, so the old copy would
            // only override it with an out-of-date script.
            const fs::path stale = scriptsFolder / fs::u8path(items[i].name);
            const std::string counts = "the game now loads " + items[i].label + ", with nothing to change for " + map.plugin;
            OutputLog::Removed removed;
            removed.name = items[i].name;
            removed.winner = items[i].label;
            removed.outFile = stale.u8string();
            if (options.dryRun) {
                report += "DRYRUN|" + removed.outFile + "|would remove: " + counts + "\n";
            } else {
                std::error_code removeEc;
                fs::remove(stale, removeEc);
                if (removeEc) {
                    report += "ERR|" + removed.outFile + "|cannot remove it: " + removeEc.message() + "\n";
                    ++failures;
                    continue;
                }
                std::ofstream gone(stateFile, std::ios::binary | std::ios::app);
                gone << *slot.key << "|" << kNewCopy << "|\n";
                report += "REMOVED|" + removed.outFile + "|" + counts + "\n";
            }
            log.removed.push_back(std::move(removed));
            continue;
        }
        if (res.edits.empty() && res.renames == 0) {
            report += "UNCHANGED|" + items[i].label + "\n";
            if (res.notInMap || res.dynamic) {
                entry.reason = "no-edit";
                entry.detail = std::to_string(res.notInMap) + " not in map, " + std::to_string(res.dynamic) + " dynamic";
                log.skipped.push_back(std::move(entry));
            }
            continue;
        }
        // The output folder's copy is already patched. The same pairs again would
        // renumber what they renumbered; other pairs for the plugin cannot be
        // composed with them, so the folder has to start over.
        if (slot.copy->kind == CopyKind::Output) {
            const auto done = applied.find({*slot.key, pluginKey});
            if (done != applied.end() && done->second == thisHash) {
                report += "UNCHANGED|" + items[i].label + "|these pairs are already applied\n";
                entry.reason = "already-applied";
                entry.detail = "these pairs are already applied";
                log.skipped.push_back(std::move(entry));
                continue;
            }
            if (done != applied.end()) {
                const std::string text = "already patched for " + map.plugin + " with other pairs; delete it from the output folder and run again";
                report += "ERR|" + items[i].label + "|" + text + "\n";
                entry.reason = "already-patched";
                entry.detail = text;
                log.skipped.push_back(std::move(entry));
                ++failures;
                continue;
            }
        }
        const fs::path target = scriptsFolder / fs::u8path(items[i].name);
        entry.outFile = target.u8string();
        entry.replaced = replacing || slot.copy->kind == CopyKind::Output;
        const std::string counts = std::to_string(res.edits.size()) + " edits, " + std::to_string(res.renames) + " renames, from " + items[i].label +
                                   (replacing ? ", replacing the copy built from an earlier winner" : "");
        if (options.dryRun) {
            report += "DRYRUN|" + entry.outFile + "|" + counts + "\n";
            log.patched.push_back(std::move(entry));
            continue;
        }
        std::string err;
        if (!write_verified_script(target, res.newBytes, res.edits, options.extraCallees, err)) {
            report += "ERR|" + items[i].label + "|" + err + "\n";
            entry.reason = "failed";
            entry.detail = err;
            log.skipped.push_back(std::move(entry));
            ++failures;
            continue;
        }
        std::ofstream record(stateFile, std::ios::binary | std::ios::app);
        if (slot.copy->kind != CopyKind::Output) {
            record << *slot.key << "|" << kNewCopy << "|\n";
            record << *slot.key << "|" << kSource << "|" << items[i].label << "#" << res.sourceHash << "\n";
        }
        record << *slot.key << "|" << pluginKey << "|" << thisHash << "\n";
        record << *slot.key << "|" << kWrote << "|" << bytes_hash(res.newBytes) << "\n";
        record.close();
        if (!record) {
            report += "ERR|" + stateFile.u8string() + "|written, but cannot record it; rerunning this map could renumber twice\n";
            ++failures;
        }
        report += "PATCHED|" + entry.outFile + "|" + counts + "\n";
        log.patched.push_back(std::move(entry));
    }
    logText = render_output_log(log);
    return failures;
}

bool winning_items(const fs::path& data, const fs::path& pluginsTxt, const std::vector<fs::path>& iniDirs, bool allPlugins, const fs::path& outFolder,
                   std::vector<Item>& items, std::vector<std::string>& kinds, std::vector<std::string>& knownHashes, std::string& why) {
    ArchivePlan plan;
    if (!plan_archives(data, pluginsTxt, iniDirs, allPlugins, plan, why)) return false;
    std::vector<std::string> warnings;
    auto scripts = collect_script_copies(data, fs::path(), plan, warnings);
    OutputState state;
    if (!outFolder.empty()) state = read_output_state(output_state_path(outFolder));
    const auto hiddenSources = drop_our_installed_copies(scripts, state, outFolder);
    for (const auto& [key, copies] : scripts) {
        const auto hidden = hiddenSources.find(key);
        if (hidden != hiddenSources.end()) {
            // Its bytes cannot be read, being under ours: named as recorded.
            const std::string& identity = state.source.at(key);
            const size_t mark = identity.rfind('#');
            Item item;
            item.file = hidden->second;
            item.name = hidden->second.filename().u8string();
            item.label = identity.substr(0, mark);
            items.push_back(std::move(item));
            kinds.push_back(to_string(CopyKind::Loose));
            knownHashes.push_back(mark == std::string::npos ? std::string() : identity.substr(mark + 1));
            continue;
        }
        const int w = winning_copy(copies);
        if (w < 0) continue;
        const ScriptCopy& c = copies[static_cast<size_t>(w)];
        items.push_back(c.item);
        kinds.push_back(to_string(c.kind));
        knownHashes.emplace_back();
    }
    return true;
}

bool list_winners(const fs::path& data, const fs::path& pluginsTxt, const std::vector<fs::path>& iniDirs, bool allPlugins, const fs::path& outFolder,
                  std::string& csv, std::string& why) {
    std::vector<Item> items;
    std::vector<std::string> kinds, knownHashes;
    if (!winning_items(data, pluginsTxt, iniDirs, allPlugins, outFolder, items, kinds, knownHashes, why)) return false;
    csv = "script,kind,source\n";
    for (size_t i = 0; i < items.size(); ++i) csv += lower(items[i].name) + "," + kinds[i] + ",\"" + items[i].label + "\"\n";
    return true;
}

}  // namespace pextool
