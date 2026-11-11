// The script auditor: static rules over the decompiler's view of a compiled
// script that point out where it can misbehave at run time (loops that cannot
// end, update timers that never stop, calls on values that can be None,
// recursion, arrays used before they exist, states that do not exist).
//
// Each finding names the function, the instruction and, when the file carries
// debug information, the source line, with one sentence of advice an author
// can act on. docs/AUDIT.md explains every rule, what it is grounded in
// (save-game VM state, Papyrus logs) and how often it fires on Bethesda's own
// scripts. Nothing here throws across the API.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "papyrus/decompile.hpp"
#include "papyrus/pex.hpp"

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace papyrus::pex {

enum class Severity { Info, Warning, Error };
const char* to_string(Severity s) noexcept;  // "info", "warning", "error"

struct Finding {
    std::string object;
    std::string state;      // "" for the default state
    std::string function;   // accessors: "Prop.get" / "Prop.set"
    uint32_t ip = 0;        // the instruction the finding is anchored to
    uint16_t line = 0;      // source line from the debug info; 0 when unknown
    Severity severity = Severity::Warning;
    std::string rule;       // stable id, e.g. "loop-no-exit"
    std::string message;    // one sentence for the author
};

struct AuditRule {
    std::string id;
    Severity severity;      // the usual severity; a finding may carry a lower one
    std::string group;      // "threads" (threads that never end or pile up) or "results" (wrong results, log errors)
    std::string summary;    // one line, for `pex audit --rules`
};

// What a whole set of scripts does with properties and states, for the rules
// that need more than one script to decide (property-unused,
// state-unreachable, property-unfilled). Build it with index_script over
// every script the audit should know about (the audited ones and the ones
// they resolve against). Names are lowercased.
struct AuditIndex {
    std::set<std::string> propertiesRead;     // read with PropGet by some script, on any object
    std::set<std::string> propertiesWritten;  // written with PropSet by some script
    std::set<std::string> statesEntered;      // passed as a literal to GotoState on any object
    std::set<std::string> dynamicStateTypes;  // script types something calls GotoState on with a built name
    std::set<std::string> stringLiterals;     // every short string literal any script uses (a state
                                              // name handed to a helper that calls GotoState)
    std::map<std::string, std::string> parentOf;  // script -> the script it extends
    std::map<std::string, std::vector<std::string>> childrenOf;  // filled by finish_index
    // Plugin data, when loaded (fillsKnown): for each script attached
    // somewhere in the load order (VMAD), the properties some attachment
    // fills. Quest aliases and fragments included.
    bool fillsKnown = false;
    std::map<std::string, std::set<std::string>> filled;
};

// Adds one script's facts to the index (instruction level, no decompiling).
void index_script(const Script& script, AuditIndex& index);
// Moves `from` into `into`.
void merge_index(AuditIndex& into, AuditIndex&& from);
// Call once after the last index_script / merge_index, before auditing.
void finish_index(AuditIndex& index);

struct AuditOptions {
    // Other scripts, for inherited states and declared types (optional; see
    // ScriptResolver in decompile.hpp). Not owned.
    ScriptResolver* resolver = nullptr;
    // The whole set (optional, not owned). Without it the property and state
    // rules only see this one script.
    const AuditIndex* index = nullptr;
};

struct AuditResult {
    std::vector<Finding> findings;  // in function order, then by instruction
    size_t functions = 0;           // bodies examined
    size_t skipped = 0;             // bodies the decompiler could not structure (no findings for them)
};

AuditResult audit(const Script& script, const AuditOptions& opt = {});

// Every rule the auditor knows, in the order they are documented.
const std::vector<AuditRule>& audit_rules();

}  // namespace papyrus::pex
