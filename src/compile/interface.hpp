// A script's interface: what other scripts need to know about it (parent,
// properties, variables, functions with their parameter defaults, states),
// built from the syntax tree without looking at bodies; and the Program that
// finds interfaces by name through the SourceResolver, parsing each import
// once.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "ast.hpp"
#include "lexer.hpp"
#include "papyrus/compile.hpp"

#include <map>
#include <string>
#include <vector>

namespace papyrus::psc {

struct TypeRef {
    std::string name;  // source spelling; empty = no type (a void function)
    bool isArray = false;
    bool empty() const noexcept { return name.empty(); }
    std::string key() const { return lower(name) + (isArray ? "[]" : ""); }
    static TypeRef of(const TypeName& t) { return TypeRef{t.name, t.isArray}; }
};

struct ParamInfo {
    std::string name;
    TypeRef type;
    bool hasDefault = false;
    Literal def;
};

struct FuncInfo {
    std::string name;
    TypeRef ret;
    std::vector<ParamInfo> params;
    bool isGlobal = false;
    bool isNative = false;
    bool isEvent = false;
    const Function* ast = nullptr;
};

struct PropInfo {
    std::string name;
    TypeRef type;
    bool isAuto = false;
    bool isAutoReadOnly = false;
    bool readable = true;
    bool writable = true;
    const Property* ast = nullptr;
};

struct VarInfo {
    std::string name;
    TypeRef type;
    const Variable* ast = nullptr;
};

struct ScriptInfo {
    std::string name;    // as declared by ScriptName
    std::string parent;  // empty when none
    std::vector<std::string> imports;
    std::vector<VarInfo> vars;
    std::vector<PropInfo> props;
    std::vector<FuncInfo> funcs;  // the default state's, in source order
    std::vector<std::string> states;
    const Script* ast = nullptr;

    const PropInfo* own_property(std::string_view n) const;
    const VarInfo* own_variable(std::string_view n) const;
    const FuncInfo* own_function(std::string_view n) const;
};

ScriptInfo build_interface(const Script& ast);

class Program {
public:
    Program(SourceResolver* resolver, std::vector<Diagnostic>& diagnostics)
        : m_resolver(resolver), m_diags(diagnostics) {}

    // Registers a parsed script (the one being compiled). The Program keeps the tree.
    const ScriptInfo* add(ScriptPtr tree, const std::string& file);
    // The interface of the script called `name`, parsed from its source on
    // first use; nullptr when there is no such source or it does not parse
    // (one diagnostic per missing script).
    const ScriptInfo* find(std::string_view name);
    // Property / function lookups up the parent chain; `owner` receives the
    // script that declares the member.
    const PropInfo* find_property(const ScriptInfo& s, std::string_view name, const ScriptInfo** owner = nullptr);
    const FuncInfo* find_function(const ScriptInfo& s, std::string_view name, const ScriptInfo** owner = nullptr);
    // True when `from` is `to` or derives from it (both script names).
    bool is_subtype(std::string_view from, std::string_view to);

private:
    struct Entry {
        ScriptPtr tree;
        ScriptInfo info;
        bool missing = false;
    };
    SourceResolver* m_resolver;
    std::vector<Diagnostic>& m_diags;
    std::map<std::string, Entry> m_entries;  // by lowercased name
};

}  // namespace papyrus::psc
