// Interfaces, the Program, user flags, the folder resolver and compile().
// SPDX-License-Identifier: GPL-3.0-only
#include "interface.hpp"
#include "lower.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <iterator>

namespace papyrus::psc {

namespace fs = std::filesystem;

// --- interfaces -------------------------------------------------------------

const PropInfo* ScriptInfo::own_property(std::string_view n) const {
    for (const PropInfo& p : props) {
        if (ieq(p.name, n)) return &p;
    }
    return nullptr;
}
const VarInfo* ScriptInfo::own_variable(std::string_view n) const {
    for (const VarInfo& v : vars) {
        if (ieq(v.name, n)) return &v;
    }
    return nullptr;
}
const FuncInfo* ScriptInfo::own_function(std::string_view n) const {
    for (const FuncInfo& f : funcs) {
        if (ieq(f.name, n)) return &f;
    }
    return nullptr;
}

static FuncInfo func_info(const Function& f) {
    FuncInfo fi;
    fi.name = f.name;
    fi.ret = TypeRef::of(f.returnType);
    for (const Param& p : f.params) {
        ParamInfo pi;
        pi.name = p.name;
        pi.type = TypeRef::of(p.type);
        pi.hasDefault = p.hasDefault;
        pi.def = p.def;
        fi.params.push_back(std::move(pi));
    }
    fi.isGlobal = f.isGlobal;
    fi.isNative = f.isNative;
    fi.isEvent = f.isEvent;
    fi.ast = &f;
    return fi;
}

ScriptInfo build_interface(const Script& ast) {
    ScriptInfo s;
    s.name = ast.name;
    s.parent = ast.parent;
    s.imports = ast.imports;
    s.ast = &ast;
    for (const Variable& v : ast.variables) {
        VarInfo vi;
        vi.name = v.name;
        vi.type = TypeRef::of(v.type);
        vi.ast = &v;
        s.vars.push_back(std::move(vi));
    }
    for (const Property& p : ast.properties) {
        PropInfo pi;
        pi.name = p.name;
        pi.type = TypeRef::of(p.type);
        pi.isAuto = p.isAuto;
        pi.isAutoReadOnly = p.isAutoReadOnly;
        if (p.isAuto) {
            pi.readable = pi.writable = true;
        } else if (p.isAutoReadOnly) {
            pi.readable = true;
            pi.writable = false;
        } else {
            pi.readable = p.getter != nullptr;
            pi.writable = p.setter != nullptr;
        }
        pi.ast = &p;
        s.props.push_back(std::move(pi));
    }
    for (const Function& f : ast.functions) s.funcs.push_back(func_info(f));
    for (const State& st : ast.states) s.states.push_back(st.name);
    return s;
}

// --- Program ------------------------------------------------------------------

const ScriptInfo* Program::add(ScriptPtr tree, const std::string& file) {
    (void)file;
    const std::string key = lower(tree->name);
    Entry& e = m_entries[key];
    e.tree = std::move(tree);
    e.info = build_interface(*e.tree);
    e.missing = false;
    return &e.info;
}

const ScriptInfo* Program::find(std::string_view name) {
    const std::string key = lower(name);
    auto it = m_entries.find(key);
    if (it != m_entries.end()) return it->second.missing ? nullptr : &it->second.info;
    Entry& e = m_entries[key];
    SourceFile src;
    if (!m_resolver || !m_resolver->find(name, src)) {
        e.missing = true;
        Diagnostic d;
        d.message = "unable to find script " + std::string(name);
        m_diags.push_back(std::move(d));
        return nullptr;
    }
    Diagnostic err;
    if (!parse(src.text, src.path, e.tree, err)) {
        e.missing = true;
        m_diags.push_back(std::move(err));
        return nullptr;
    }
    e.info = build_interface(*e.tree);
    return &e.info;
}

const PropInfo* Program::find_property(const ScriptInfo& s, std::string_view name, const ScriptInfo** owner) {
    const ScriptInfo* cur = &s;
    for (int depth = 0; cur && depth < 64; ++depth) {
        if (const PropInfo* p = cur->own_property(name)) {
            if (owner) *owner = cur;
            return p;
        }
        if (cur->parent.empty()) break;
        cur = find(cur->parent);
    }
    return nullptr;
}

const FuncInfo* Program::find_function(const ScriptInfo& s, std::string_view name, const ScriptInfo** owner) {
    const ScriptInfo* cur = &s;
    for (int depth = 0; cur && depth < 64; ++depth) {
        if (const FuncInfo* f = cur->own_function(name)) {
            if (owner) *owner = cur;
            return f;
        }
        if (cur->parent.empty()) break;
        cur = find(cur->parent);
    }
    return nullptr;
}

bool Program::is_subtype(std::string_view from, std::string_view to) {
    if (ieq(from, to)) return true;
    const ScriptInfo* cur = find(from);
    for (int depth = 0; cur && depth < 64; ++depth) {
        if (cur->parent.empty()) return false;
        if (ieq(cur->parent, to)) return true;
        cur = find(cur->parent);
    }
    return false;
}

// --- user flags -------------------------------------------------------------

bool load_flags(std::string_view text, std::vector<UserFlagDef>& out, std::string* error) {
    out.clear();
    // Strip comments, then read words.
    std::string clean;
    for (size_t i = 0; i < text.size();) {
        if (text.compare(i, 2, "//") == 0) {
            while (i < text.size() && text[i] != '\n') ++i;
        } else if (text.compare(i, 2, "/*") == 0) {
            const size_t end = text.find("*/", i + 2);
            if (end == std::string_view::npos) {
                if (error) *error = "unterminated comment in flags file";
                return false;
            }
            i = end + 2;
        } else {
            const char c = text[i++];
            if (c == '{' || c == '}') {
                clean += ' ';
                clean += c;
                clean += ' ';
            } else {
                clean += c;
            }
        }
    }
    std::vector<std::string> words;
    {
        std::string w;
        for (char c : clean) {
            if (std::isspace(static_cast<unsigned char>(c))) {
                if (!w.empty()) words.push_back(w);
                w.clear();
            } else {
                w += c;
            }
        }
        if (!w.empty()) words.push_back(w);
    }
    size_t i = 0;
    while (i < words.size()) {
        if (!ieq(words[i], "flag") || i + 2 >= words.size()) {
            if (error) *error = "expected 'Flag <name> <bit>' in flags file, got '" + words[i] + "'";
            return false;
        }
        UserFlagDef d;
        d.name = words[i + 1];
        d.bit = std::atoi(words[i + 2].c_str());
        if (d.bit < 0 || d.bit > 31) {
            if (error) *error = "flag bit out of range for " + d.name;
            return false;
        }
        i += 3;
        if (i < words.size() && words[i] == "{") {
            d.onScript = d.onProperty = d.onVariable = d.onFunction = false;
            ++i;
            while (i < words.size() && words[i] != "}") {
                if (ieq(words[i], "script")) d.onScript = true;
                else if (ieq(words[i], "property")) d.onProperty = true;
                else if (ieq(words[i], "variable")) d.onVariable = true;
                else if (ieq(words[i], "function")) d.onFunction = true;
                ++i;
            }
            if (i >= words.size()) {
                if (error) *error = "missing '}' in flags file";
                return false;
            }
            ++i;
        }
        out.push_back(std::move(d));
    }
    return true;
}

// --- folder resolver ----------------------------------------------------------

namespace {

class FolderResolver : public SourceResolver {
public:
    explicit FolderResolver(std::vector<fs::path> folders) : m_folders(std::move(folders)) {}

    bool find(std::string_view scriptName, SourceFile& out) override {
        const std::string key = lower(scriptName) + ".psc";
        if (m_listings.size() < m_folders.size()) {
            m_listings.resize(m_folders.size());
            m_listed.resize(m_folders.size(), false);
        }
        for (size_t i = 0; i < m_folders.size(); ++i) {
            if (!m_listed[i]) list(i);
            auto it = m_listings[i].find(key);
            if (it == m_listings[i].end()) continue;
            std::ifstream f(it->second, std::ios::binary);
            if (!f) continue;
            out.text.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
            out.name = it->second.stem().string();
            out.path = it->second.string();
            std::error_code ec;
            const auto t = fs::last_write_time(it->second, ec);
            out.modified = ec ? 0 : file_time_to_unix(t);
            return true;
        }
        return false;
    }

    static uint64_t file_time_to_unix(fs::file_time_type t) {
        // Portable enough: assume the file clock's epoch is the system clock's (MSVC: not; see below).
        const auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
            t - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
        return static_cast<uint64_t>(std::chrono::system_clock::to_time_t(sctp));
    }

private:
    void list(size_t i) {
        m_listed[i] = true;
        std::error_code ec;
        for (const fs::directory_entry& e : fs::directory_iterator(m_folders[i], ec)) {
            if (!e.is_regular_file(ec)) continue;
            std::string name = lower(e.path().filename().string());
            if (name.size() > 4 && name.compare(name.size() - 4, 4, ".psc") == 0) m_listings[i].emplace(name, e.path());
        }
    }

    std::vector<fs::path> m_folders;
    std::vector<std::map<std::string, fs::path>> m_listings;
    std::vector<bool> m_listed;
};

}  // namespace

std::unique_ptr<SourceResolver> folder_resolver(const std::vector<fs::path>& folders) {
    return std::make_unique<FolderResolver>(folders);
}

// --- compile ------------------------------------------------------------------

bool compile(const SourceFile& source, const CompileOptions& options, pex::Script& out, std::vector<Diagnostic>& diagnostics) {
    ScriptPtr tree;
    Diagnostic err;
    if (!parse(source.text, source.path.empty() ? source.name + ".psc" : source.path, tree, err)) {
        diagnostics.push_back(std::move(err));
        return false;
    }
    if (!ieq(tree->name, source.name) && !source.name.empty()) {
        Diagnostic d;
        d.file = source.path;
        d.line = tree->line;
        d.message = "script name " + tree->name + " does not match the file name " + source.name;
        diagnostics.push_back(std::move(d));
        return false;
    }
    Program program(options.resolver, diagnostics);
    const ScriptInfo* info = program.add(std::move(tree), source.path);
    const size_t before = diagnostics.size();
    const bool ok = lower_script(*info, program, options, source, out, diagnostics);
    return ok && diagnostics.size() == before;
}

}  // namespace papyrus::psc
