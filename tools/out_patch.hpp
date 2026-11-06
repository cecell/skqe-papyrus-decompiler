// `pex patch --out`: nothing the mod owns is touched. For each script the game
// would load, the one copy that wins gets the FormID changes, and the result is
// written as a loose file to <out>\scripts\, ready to be packaged and installed
// as a mod (docs/INTEGRATION.md).
//
// Which copy wins, highest first: a copy already in the output folder (so runs
// for several plugins build on each other), the loose file in Data\Scripts, the
// copy in the loaded archive with the highest rank. The load order rules are
// spf's (skyrim-plugin-format), the same code `spf build-assets` uses.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "inputs.hpp"
#include "papyrus/analysis.hpp"

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace pextool {

// Every archive at the top of the Data folder: which load, in which order.
struct ArchivePlan {
    struct Loaded {
        std::filesystem::path path;
        int rank = 0;           // 0 = loaded first; higher wins
        std::string loadedBy;   // ini | plugin
        std::string plugin;     // the plugin that loads it; empty for ini
    };
    std::vector<Loaded> loaded;
    std::vector<std::filesystem::path> notLoaded;
    std::string iniSource;  // where the archive lists came from
    size_t plugins = 0;     // plugins in the load order
};

// False with the reason when the load order cannot be established. `iniDirs` are
// searched before Documents\My Games\Skyrim Special Edition; the first folder
// that has a Skyrim.ini or SkyrimCustom.ini is the one used, as the game does.
//
// The load order is the base masters, Skyrim.ccc, then plugins.txt, masters
// first. `allPlugins` keeps plugins.txt's inactive lines too, in their places;
// without it only the active ones count. A plugin plugins.txt does not list is
// ignored either way, so its archives do not load.
bool plan_archives(const std::filesystem::path& data, const std::filesystem::path& pluginsTxt,
                   const std::vector<std::filesystem::path>& iniDirs, bool allPlugins, ArchivePlan& out, std::string& why);

enum class CopyKind { NotLoaded, Archive, Loose, Output };
const char* to_string(CopyKind k) noexcept;

// One copy of one script.
struct ScriptCopy {
    CopyKind kind = CopyKind::NotLoaded;
    int rank = 0;  // for an archive copy: its archive's rank
    Item item;
};

// Index of the copy that wins, or -1 when no copy is live (only archives that
// do not load). Output beats loose beats archive, whatever the load order says;
// among archives the highest rank wins.
int winning_copy(const std::vector<ScriptCopy>& copies);

// Every live-location copy of every script, by lowercased file name. Only
// scripts\<name>.pex counts, in the archives at the top of Data, in Data\Scripts
// and in <out>\scripts: Skyrim loads scripts from nowhere else, so subfolders
// (Data\Scripts\Source) and backups elsewhere under Data are ignored. An empty
// `outFolder` means no output folder.
std::map<std::string, std::vector<ScriptCopy>> collect_script_copies(const std::filesystem::path& data, const std::filesystem::path& outFolder,
                                                                     const ArchivePlan& plan, std::vector<std::string>& warnings);

struct OutOptions {
    std::filesystem::path data;
    std::filesystem::path outFolder;
    std::filesystem::path pluginsTxt;
    std::vector<std::filesystem::path> iniDirs;
    bool allPlugins = false;  // see plan_archives
    std::vector<std::string> extraCallees;
    bool dryRun = false;
    unsigned threads = 0;
    // What the log says produced the pairs: "formid", or "map|<path>".
    std::string source;
};

// What the log says (docs/INTEGRATION.md, format `pex patch output log v1`).
struct OutputLog {
    struct Entry {
        std::string name, source, kind, outFile;
        size_t edits = 0, renames = 0, notInMap = 0, dynamic = 0;
        std::vector<std::string> pairs;
        std::string reason, detail;  // for a skipped script
    };
    struct Lost {
        std::string name, source, kind, winner;  // winner empty: no copy of it loads
        size_t edits = 0;
        std::vector<std::string> pairs;
    };
    bool dryRun = false;
    bool allPlugins = false;
    std::string started, data, outFolder, pluginsTxt, iniSource, plugin, newPlugin, source;
    size_t pairs = 0, archivesLoaded = 0, archivesNotLoaded = 0, scripts = 0;
    std::vector<Entry> patched, skipped;
    std::vector<Lost> lost;
};
std::string render_output_log(const OutputLog& log);

// Beside the output folder, not inside it, so it is not packaged into the mod:
// which pairs each script in the folder already has, per plugin. A copy in the
// output folder is the starting point of the next run, so without this a rerun
// would renumber twice whenever a map's new value is another entry's old value.
std::filesystem::path output_state_path(const std::filesystem::path& outFolder);

// Empty when the two folders are fine; otherwise why they are not. The output
// folder must not be, contain or sit inside the Data folder: the game would see
// it, and the point is that a mod manager installs it.
std::string check_output_folder(const std::filesystem::path& data, const std::filesystem::path& outFolder);

// The run. Appends the stdout lines to `report` and the log text to `logText`.
// Refuses to write while the game runs. Returns the number of failures.
int patch_to_output(const OutOptions& options, const papyrus::pex::FormIdMap& map, std::string& report, std::string& logText);

// The `pex winners` listing: script, kind, source, one line each, header first.
// False with the reason when the load order cannot be established.
bool list_winners(const std::filesystem::path& data, const std::filesystem::path& pluginsTxt,
                  const std::vector<std::filesystem::path>& iniDirs, bool allPlugins, std::string& csv, std::string& why);

}  // namespace pextool
