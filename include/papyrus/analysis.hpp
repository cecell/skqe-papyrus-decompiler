// Analyses over the model for the plugin tooling:
//   refs     every integer that reaches a FormID-taking call, with the plugin
//            name it is paired with (what ESL compaction must renumber);
//   deps     what a script needs: parent chain, types, calls per type, mod
//            events, and which calls vanilla does not declare;
//   closure  whether everything a script needs to compile is present, as
//            compiled files or sources, with the exact missing pieces;
//   patch    FormID renumbering and plugin renames applied to the model.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "papyrus/decompile.hpp"
#include "papyrus/pex.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace papyrus::pex {

// --- refs ---------------------------------------------------------------------

enum class RefKind {
    Literal,          // the integer is a literal operand of the call
    Propagated,       // a local or temp assigned a literal earlier in the function, unchanged since
    PropertyDefault,  // an auto property / script variable never assigned in this script, with an initial value
    Dynamic,          // computed or passed in; the value is unknown
};
const char* to_string(RefKind k) noexcept;

struct FormRef {
    std::string object;
    std::string state;
    std::string function;    // "Prop.get" / "Prop.set" for accessors
    uint32_t ip = 0;         // instruction of the call
    std::string callee;      // "Game.GetFormFromFile", "PO3_SKSEFunctions.GetFormFromFile", ...
    RefKind kind = RefKind::Dynamic;
    int32_t formId = 0;      // valid unless Dynamic
    std::string plugin;      // the paired plugin string; empty when not a literal
    bool pluginLiteral = false;
    // Where the literal sits, for patching: instruction and operand index of
    // the integer value. Only for Literal (the operand of the call itself) and
    // Propagated / PropertyDefault (the operand of the assignment or the
    // variable's initial value: ip == kNoIp then, and `variable` names it).
    uint32_t literalIp = 0;
    uint32_t literalOperand = 0;
    std::string variable;
};
inline constexpr uint32_t kNoIp = 0xFFFFFFFFu;

// Calls recognised as FormID-taking. The built-in set: any function named
// GetFormFromFile, GetFormEx or GetForm on any class, and any call where an
// integer argument is immediately followed by a string argument that ends in
// .esp, .esm or .esl (wrappers in mods and extenders). Extra names add to it.
std::vector<FormRef> find_form_refs(const Script& s, const std::vector<std::string>& extraCallees = {});

// --- deps -----------------------------------------------------------------------

struct Dependencies {
    std::string name;
    std::vector<std::string> parents;               // nearest first, as far as the resolver knows
    std::set<std::string> types;                    // every script type named (lowercased)
    std::map<std::string, std::set<std::string>> callsByType;   // type -> member function names called
    std::set<std::string> staticClasses;            // classes of CALLSTATIC
    std::set<std::string> modEvents;                // RegisterForModEvent first arguments (literals)
    std::set<std::string> notInVanilla;             // "Type.Function" that the vanilla set does not declare
    bool parentChainComplete = false;
};
// `vanilla` may be null: then notInVanilla stays empty.
Dependencies find_dependencies(const Script& s, ScriptResolver* resolver, ScriptResolver* vanilla);

// --- closure ----------------------------------------------------------------------

enum class DepState { Pex, Source, Missing };
const char* to_string(DepState s) noexcept;

struct ClosureEntry {
    std::string type;        // lowercased script name
    DepState state;
    std::string neededBy;    // the script that first named it
    std::string where;       // file or archive entry, when known
};

struct MissingMember {
    std::string script;      // the script whose code calls it
    std::string type;        // the static type of the object / class
    std::string member;      // function or property name
    std::string reason;      // "no such function in the chain", "chain incomplete at X"
};

struct Closure {
    std::vector<ClosureEntry> entries;  // in discovery order, the root first
    std::vector<MissingMember> missingMembers;
    bool complete() const noexcept {
        for (const ClosureEntry& e : entries) {
            if (e.state == DepState::Missing) return false;
        }
        return missingMembers.empty();
    }
};

// Walks the type dependencies of `s` (parent, property/variable/parameter/
// local types, cast targets, static classes) through the resolver; `hasSource`
// tells whether a .psc for a type exists in the caller's import folders.
// With `declarationClosure` the dependencies (not `s` itself) contribute only
// the types their declarations name (parent, variables, properties,
// signatures): what a compiler needs from an import, and what a declaration
// stub (DecompileOptions::declarationsOnly) contains.
Closure compute_closure(const Script& s, ScriptResolver& resolver,
                        const std::function<bool(const std::string& type, std::string& where)>& hasSource,
                        bool checkMembers = true, bool declarationClosure = false);

// --- patch --------------------------------------------------------------------------

struct FormIdMap {
    std::string plugin;      // the plugin the map is for (file name)
    std::string newPlugin;   // when renamed by the operation (merge), else == plugin
    std::map<uint32_t, uint32_t> oldToNew;  // low 24 bits -> low 24 bits
};
// EslMapLib format: "!KEY=value" header lines (PLUGIN, NEWPLUGIN, ...), rows
// "OLD|NEW[|...]" with the low 6 hex digits as the key.
bool load_formid_map(const std::string& text, FormIdMap& out, std::string* error = nullptr);

struct PatchEdit {
    std::string object, state, function;
    uint32_t ip = 0;
    std::string callee;
    int32_t oldValue = 0, newValue = 0;
    std::string plugin;
};

struct PatchResult {
    std::vector<PatchEdit> edits;
    std::vector<std::string> renamedStrings;  // "Old.esp -> New.esp" per string table entry changed
    std::vector<FormRef> skipped;             // refs that matched the plugin but had no map entry or were dynamic
};

// Applies a FormID map (and the plugin rename when the map renames) to the
// model: literal FormIDs paired with the map's plugin, and literals assigned
// to variables that reach such calls. Returns what it changed; the caller
// writes the model (write) and verifies (read back).
PatchResult apply_formid_map(Script& s, const FormIdMap& map, const std::vector<std::string>& extraCallees = {});

// Renames a plugin everywhere it appears as a string literal (case-insensitive
// match on the whole string). Returns the number of table entries changed.
size_t rename_plugin(Script& s, std::string_view oldName, std::string_view newName);

}  // namespace papyrus::pex
