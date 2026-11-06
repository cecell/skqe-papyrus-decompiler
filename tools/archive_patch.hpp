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

// One member to patch in one archive.
struct MemberPatch {
    std::string label;      // "Archive.bsa:scripts\foo.pex", for the report lines
    std::string entryPath;  // inside the archive
    std::vector<uint8_t> oldBytes, newBytes;
    std::vector<OperandRange> ranges;
    std::vector<papyrus::pex::PatchEdit> edits;
    // Filled by patch_archive_members: the report lines and the outcome.
    std::string report;
    bool failed = false;
};

// Builds the plan for one member from its original bytes and the model that
// apply_formid_map changed: refuses a rename (the length would change), writes
// the model, requires an equal size and computes the ranges. No I/O. Refusals
// and failures come back in `why`, without the "ERR|..." wrapper.
bool make_member_patch(const std::string& label, const std::string& entryPath, const std::vector<uint8_t>& oldBytes,
                       const papyrus::pex::Script& patched, const papyrus::pex::PatchResult& result, MemberPatch& out,
                       std::string& why);

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

// Restores every byte the journal's ranges not yet undone changed, in every
// archive it names. The bytes found must be values the journal says they held;
// on any mismatch in an archive nothing in that archive is written. Appends the
// report lines to `report`. Returns the number of failures.
int undo_journal(const std::filesystem::path& journal, bool dryRun, std::string& report);

}  // namespace pextool
