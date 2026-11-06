// In-archive FormID patching for `pex patch`: a FormID literal is a fixed
// 4-byte big-endian integer operand, so a map that only renumbers never changes
// a member's length, and a BSA has no checksum over file contents. The changed
// bytes can therefore be overwritten in place, with a journal that undoes them
// (docs/INTEGRATION.md).
//
// The order is always: plan (no I/O), locate through bsa-reader, re-read the
// bytes on disk, journal and flush, write and flush, close, reopen through the
// normal hash lookup, verify. A member that does not verify is rolled back.
// Nothing here throws.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "papyrus/analysis.hpp"
#include "papyrus/pex.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace pextool {

// Every refusal ends with this, so the caller knows what to do instead.
inline constexpr const char* kLooseFallback = "extract and patch loose; a loose file overrides the archive";

// One integer operand to overwrite. `offset` is from the start of the member.
struct OperandRange {
    uint32_t offset = 0;
    std::array<uint8_t, 4> oldBytes{};
    std::array<uint8_t, 4> newBytes{};
};

// Compares a member before and after apply_formid_map. Succeeds only when the
// sizes match and every differing byte lies in a 4-byte integer operand holding
// exactly the old value of an edit before and its new value after, every edit
// has such an operand, and the model comparison finds those same operands and
// nothing else. `why` says what failed.
bool compute_operand_ranges(const std::vector<uint8_t>& oldBytes, const std::vector<uint8_t>& newBytes,
                            const papyrus::pex::Script& oldScript, const papyrus::pex::Script& newScript,
                            const std::vector<papyrus::pex::PatchEdit>& edits, std::vector<OperandRange>& out,
                            std::string& why);

// One script to patch: a member of one archive, or one loose file.
struct MemberPatch {
    std::string label;      // "Archive.bsa:scripts\foo.pex" or the file's path, for the report lines
    std::string entryPath;  // inside the archive; empty for a loose file
    std::filesystem::path file;  // the loose file; empty for a member
    std::vector<uint8_t> oldBytes, newBytes;
    std::vector<OperandRange> ranges;
    std::vector<papyrus::pex::PatchEdit> edits;
    // A compressed member: the loose file it becomes (see extract_members).
    std::filesystem::path extractTo;
    // Filled when it is applied: the report lines and the outcome.
    std::string report;
    bool failed = false;
    std::string refusal;  // the reason of the ERR line
};

// Builds the plan from the script's original bytes and the model that
// apply_formid_map changed: refuses a rename (the length would change), writes
// the model, requires an equal size and computes the ranges. No I/O. Refusals
// and failures come back in `why`, without the "ERR|..." wrapper. With an empty
// `entryPath` it is a plan for the loose file `file`.
bool make_member_patch(const std::string& label, const std::string& entryPath, const std::filesystem::path& file,
                       const std::vector<uint8_t>& oldBytes, const papyrus::pex::Script& patched,
                       const papyrus::pex::PatchResult& result, MemberPatch& out, std::string& why);

struct ArchivePatchOptions {
    bool dryRun = false;
    bool backup = false;      // archives copy nothing; a NOTE line says so
    std::string mapPath;      // recorded in the journal
    std::vector<std::string> extraCallees;
    // For the tests: runs after the writes are flushed and closed, before the
    // read-back, to simulate an archive that does not verify.
    std::function<void(const std::filesystem::path& archive)> afterWriteForTest;
};

std::filesystem::path journal_path(const std::filesystem::path& archive);

// Patches the members of one archive, setting each member's report and failed.
// Archive-level lines (JOURNAL, NOTE) go to the last member's report. No reader
// of this archive may be open in this process while it runs.
void patch_archive_members(const std::filesystem::path& archive, std::vector<MemberPatch*> members,
                           const ArchivePatchOptions& options);

// `pexpatch.journal` in the folder: the loose files patched there.
std::filesystem::path loose_journal_path(const std::filesystem::path& folder);

// Patches loose files of one folder the same way: journal first, overwrite the
// ranges, read back, roll back on failure. Sets each member's report and failed.
void patch_loose_members(const std::filesystem::path& folder, std::vector<MemberPatch*> members,
                         const ArchivePatchOptions& options);

// --- compressed members become loose files ---------------------------------------

// Which of the members are stored compressed, one flag per member. False when
// the archive cannot be opened; patch_archive_members reports that.
bool find_compressed_members(const std::filesystem::path& archive, const std::vector<MemberPatch*>& members,
                             std::vector<char>& compressed);

// Where a member's loose file goes: Scripts\<the rest of its path> in the
// archive's folder, the place the game looks for it. False when the member is
// not under scripts\: the game would not load it as a script from anywhere.
bool extract_target(const std::filesystem::path& archive, const std::string& entryPath, std::filesystem::path& out);

// One member about to become a loose file.
struct ExtractCandidate {
    std::filesystem::path archive;
    MemberPatch* member = nullptr;
    std::string conflict;  // filled by find_other_copies: the archive with different bytes
};

// A loose file beats every archive, so extracting one copy of a script would
// also beat another archive's copy that load order may have been choosing. Looks
// in every .bsa in each candidate's folder for the same path and records the
// first copy with different bytes. Identical copies are fine.
void find_other_copies(std::vector<ExtractCandidate>& candidates);

// Writes each member's patched bytes to its extractTo, journaled in the archive's
// journal first, written through a temporary name so no partial script is ever
// loadable, then read back. A file that does not verify is removed. Sets each
// member's report and failed. The archive itself is not touched.
void extract_members(const std::filesystem::path& archive, std::vector<MemberPatch*> members, const ArchivePatchOptions& options);

// Restores every byte the journal's ranges not yet undone changed, in every
// archive or loose file it names, and deletes the files it extracted. The bytes
// found must be values the journal says they held; on any mismatch in a file
// nothing in that file is written, and an extracted file that has changed since
// is left in place. Appends the report lines to `report`. Returns the number of
// failures.
int undo_journal(const std::filesystem::path& journal, bool dryRun, std::string& report);

// Writes a patched script to `target` under a temporary name, reads that back,
// checks it, and only then moves it into place, replacing what was there. A
// failure leaves `target` as it was. False with the reason.
bool write_verified_script(const std::filesystem::path& target, const std::vector<uint8_t>& bytes,
                           const std::vector<papyrus::pex::PatchEdit>& edits, const std::vector<std::string>& extraCallees, std::string& why);

// --- the game must not be running --------------------------------------------

// False, with the reason, when a Skyrim or SKSE loader process is running, or
// when that cannot be established: an unverified check counts as running.
bool verify_game_not_running(std::string& why);

// The running processes with one of these names (any case). False when the
// process list cannot be read.
bool find_running_processes(const std::vector<std::string>& names, std::vector<std::string>& found, std::string& why);

// --- FormID pairs on the command line -----------------------------------------

// One FormID: 0x plus 6 or 8 hex digits, or a decimal number. A bare number with
// a leading zero or a hex letter is refused rather than guessed at: it is a hex
// value missing its prefix, and read as decimal it would renumber the wrong form.
bool parse_formid(std::string_view text, uint32_t& out, std::string& why);

// Adds "OLD:NEW[,OLD:NEW...]" to the map; the low 24 bits are the key and the
// value. The same OLD with two different NEWs is refused.
bool add_formid_pairs(std::string_view list, papyrus::pex::FormIdMap& map, std::string& why);

// One OLD:NEW per line (commas allowed); blank lines and # comments are ignored.
bool add_formid_pairs_file(const std::filesystem::path& file, papyrus::pex::FormIdMap& map, std::string& why);

// --- the log ------------------------------------------------------------------

// The stable code of a refusal, from its text: compressed, shared-block,
// plugin-rename, not-byte-exact, other-copy, or failed.
std::string skip_reason_code(const std::string& why);

struct LogFile {
    std::string name;  // the member's path in the archive, or the file name
    size_t edits = 0;
    std::vector<std::string> pairs;  // "0100ABCD->0100ABCE", distinct, in file order
    size_t notInMap = 0, dynamic = 0;  // references to the plugin left as they are
    std::string reason, detail;  // for a skipped file: its code and text
    std::string extractedTo;     // for an extracted member: the loose file
};

struct LogContainer {
    bool archive = false;
    std::string path;
    std::string journal;  // empty when nothing was written
    size_t scanned = 0;
    std::vector<LogFile> patched, extracted, skipped;
};

struct LogRun {
    std::string started;  // UTC
    bool dryRun = false;
    std::vector<std::string> inputs;
    std::string plugin;
    std::string source;  // "formid", or "map|<path>"
    size_t pairs = 0;
};

// The text of the log (docs/INTEGRATION.md). Containers with neither a patched
// nor a skipped file count in the totals but get no section.
std::string render_patch_log(const LogRun& run, std::vector<LogContainer> containers);

std::string utc_timestamp();

// Where the log goes without --log: pex-patch-<UTC time>.log next to the input,
// with a counter when that name exists.
std::filesystem::path default_log_path(const std::filesystem::path& firstInput);

}  // namespace pextool
