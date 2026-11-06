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
#include <utility>
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

// The GOG version keeps its load order and INIs in folders of its own name.
enum class GameStore { Steam, Gog };

// %LOCALAPPDATA%\Skyrim Special Edition\plugins.txt, or Skyrim Special Edition
// GOG for that version. Empty when %LOCALAPPDATA% is not set. Does not check it
// exists: the game reads only its own, so another is never used in its place.
std::filesystem::path default_plugins_txt(GameStore store = GameStore::Steam);

// Documents\My Games\<the same folder name>. Empty when it cannot be found.
std::filesystem::path game_ini_dir(GameStore store);

// Decides on a Data folder from the install folders found: (where it was found,
// install folder). One with no Data folder under it does not count, since a
// record can outlive a moved install. Two different installs are refused rather
// than guessed between. Empty with the reason when none can be chosen.
std::filesystem::path choose_data_folder(const std::vector<std::pair<std::string, std::filesystem::path>>& installs, std::string& why);

struct GameInstall {
    std::filesystem::path data;
    GameStore store = GameStore::Steam;
};

// Steam's records if they name an install, otherwise GOG's. An install of each
// is refused rather than guessed between. False with the reason.
bool pick_game(const std::vector<std::pair<std::string, std::filesystem::path>>& steam,
               const std::vector<std::pair<std::string, std::filesystem::path>>& gog, GameInstall& out, std::string& why);

// Looks in the registry. Steam: Bethesda's key, and Steam's entry for the app
// named Skyrim Special Edition. GOG: its Bethesda key, and GOG's entry for the
// game by name. False with the reason when it cannot settle on one Data folder.
bool find_game(GameInstall& out, std::string& why);

// Which version a Data folder given by hand belongs to: GOG if GOG's records name
// it, otherwise Steam.
GameStore store_of_data(const std::filesystem::path& data);

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
    // A copy in the output folder is rebuilt when the copy the game would load is
    // no longer the one it was built from, and removed if the new winner has
    // nothing for this plugin to change. Otherwise it stays the starting point.
    bool update = false;
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
        bool replaced = false;       // it overwrote an earlier copy in the output folder
    };
    struct Removed {
        std::string name, winner, outFile;
    };
    struct Lost {
        std::string name, source, kind, winner;  // winner empty: no copy of it loads
        size_t edits = 0;
        std::vector<std::string> pairs;
    };
    bool dryRun = false;
    bool allPlugins = false;
    bool update = false;
    std::vector<Removed> removed;
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

// The copy of each script the game would load, as items to read. An output
// folder's copies are not contenders, and neither is an installed copy of one
// (a loose file with the bytes of something written there): once the mod is
// installed its own file would otherwise win, and nothing could ever look
// unchanged. If the output copy was built from a mod's loose file, that file is
// hidden under ours and cannot be seen: it stays the winner, named as recorded,
// with its hash in `knownHashes` (empty for every other item). `outFolder` may be
// empty. False with the reason.
bool winning_items(const std::filesystem::path& data, const std::filesystem::path& pluginsTxt,
                   const std::vector<std::filesystem::path>& iniDirs, bool allPlugins, const std::filesystem::path& outFolder,
                   std::vector<Item>& items, std::vector<std::string>& kinds, std::vector<std::string>& knownHashes, std::string& why);

// The `pex winners` listing: script, kind, source, one line each, header first.
// False with the reason when the load order cannot be established.
bool list_winners(const std::filesystem::path& data, const std::filesystem::path& pluginsTxt,
                  const std::vector<std::filesystem::path>& iniDirs, bool allPlugins, const std::filesystem::path& outFolder,
                  std::string& csv, std::string& why);

}  // namespace pextool
