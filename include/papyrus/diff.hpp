// The semantic diff: what changed between two versions of a script, in plain
// English, with no AI involved. Three layers over the structural
// differences: declarations (from the canonical comparison in canon.hpp),
// bodies (both versions lifted to the decompiler's statement trees and
// aligned statement by statement, locals matched up by name and by use so a
// rename is a rename, not a change), and the patterns an author cares about,
// read from the trees rather than the text (a literal, a FormID or a plugin
// name that changed, a condition that gained or lost a term, a call, a
// registration or a state change added or removed, a function emptied or
// filled). Every change is one sentence anchored to the new version's source
// line when the file carries debug information. docs/DIFF.md explains the
// sentences and how often each occurs between vanilla and the Unofficial
// Patch. Nothing here throws across the API.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "papyrus/decompile.hpp"
#include "papyrus/pex.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace papyrus::pex {

struct Change {
    std::string object;
    std::string state;      // "" for the default state
    std::string function;   // "" for a script-level change; accessors "Prop.get" / "Prop.set"
    std::string kind;       // stable id: "property-added", "literal-changed", ... (docs/DIFF.md)
    uint16_t line = 0;      // source line in the new version; 0 when unknown or not applicable
    uint16_t oldLine = 0;   // source line in the old version (removed and changed statements)
    std::string message;    // one sentence
};

struct DiffOptions {
    // Other scripts, for the parent chain and callee declarations, so both
    // versions are rendered the same way the decompiler would (optional; see
    // ScriptResolver in decompile.hpp). Not owned.
    ScriptResolver* resolver = nullptr;
    // A run of this many or more consecutive statement changes in one body
    // is reported as one sentence ("N statements changed") plus the calls
    // the run added and removed, instead of one sentence per statement.
    size_t summariseFrom = 6;
};

struct DiffResult {
    bool equal = false;             // the same program (canonically equal); changes is then empty
    std::vector<Change> changes;    // script-level changes first, then by function in the new version's order
    size_t functionsCompared = 0;   // bodies aligned statement by statement
    size_t functionsSkipped = 0;    // bodies the decompiler could not structure on one side (one sentence each)
};

// Compares the old version with the new one.
DiffResult diff(const Script& oldScript, const Script& newScript, const DiffOptions& opt = {});

// The text report: script-level sentences, then one block per function
// ("Function OnUpdate:" and its sentences indented, each with the new
// version's line when known). One sentence per line.
std::string diff_text(const DiffResult& r);

}  // namespace papyrus::pex
