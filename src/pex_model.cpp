// Model helpers: status names, the opcode table, string lookup, debug lookup,
// and the byte-exact rewrite check.
// SPDX-License-Identifier: GPL-3.0-only
#include "papyrus/pex.hpp"

#include <cstdio>

namespace papyrus::pex {

const char* to_string(Status s) noexcept {
    switch (s) {
    case Status::Ok: return "ok";
    case Status::NotPex: return "not a Papyrus script";
    case Status::NotSkyrim: return "Papyrus script for another game";
    case Status::Truncated: return "truncated";
    case Status::Corrupt: return "corrupt";
    case Status::TooLarge: return "too large";
    case Status::IoError: return "I/O error";
    case Status::BadModel: return "inconsistent model";
    }
    return "?";
}

// Mnemonics as PapyrusAssembler spells them. To be confirmed against the
// compiler's own -keepasm output in the assembly phase (docs/ASM_SYNTAX.md).
static const OpcodeInfo kOpcodes[kMaxOpcode + 1] = {
    {"NOP", 0, false},
    {"IADD", 3, false},
    {"FADD", 3, false},
    {"ISUB", 3, false},
    {"FSUB", 3, false},
    {"IMUL", 3, false},
    {"FMUL", 3, false},
    {"IDIV", 3, false},
    {"FDIV", 3, false},
    {"IMOD", 3, false},
    {"NOT", 2, false},
    {"INEG", 2, false},
    {"FNEG", 2, false},
    {"ASSIGN", 2, false},
    {"CAST", 2, false},
    {"COMPAREEQ", 3, false},
    {"COMPARELT", 3, false},
    {"COMPARELTE", 3, false},
    {"COMPAREGT", 3, false},
    {"COMPAREGTE", 3, false},
    {"JUMP", 1, false},
    {"JUMPT", 2, false},
    {"JUMPF", 2, false},
    {"CALLMETHOD", 3, true},
    {"CALLPARENT", 2, true},
    {"CALLSTATIC", 3, true},
    {"RETURN", 1, false},
    {"STRCAT", 3, false},
    {"PROPGET", 3, false},
    {"PROPSET", 3, false},
    {"ARRAYCREATE", 2, false},
    {"ARRAYLENGTH", 2, false},
    {"ARRAYGETELEMENT", 3, false},
    {"ARRAYSETELEMENT", 3, false},
    {"ARRAYFINDELEMENT", 4, false},
    {"ARRAYRFINDELEMENT", 4, false},
};

const OpcodeInfo* opcode_info(uint8_t op) noexcept {
    return op <= kMaxOpcode ? &kOpcodes[op] : nullptr;
}

size_t Instruction::var_arg_count() const noexcept {
    const OpcodeInfo* info = opcode_info(static_cast<uint8_t>(op));
    if (!info || !info->varArgs || args.size() < info->fixedArgs) return 0;
    return args.size() - info->fixedArgs;
}

const Value* Instruction::var_arg(size_t i) const noexcept {
    const OpcodeInfo* info = opcode_info(static_cast<uint8_t>(op));
    if (!info || !info->varArgs) return nullptr;
    const size_t at = info->fixedArgs + i;
    return at < args.size() ? &args[at] : nullptr;
}

std::string_view Script::str(StrIdx i) const noexcept {
    return i < strings.size() ? std::string_view(strings[i]) : std::string_view();
}

bool Script::find_string(std::string_view s, StrIdx& out) const noexcept {
    for (size_t i = 0; i < strings.size(); ++i) {
        if (strings[i] == s) {
            out = static_cast<StrIdx>(i);
            return true;
        }
    }
    return false;
}

bool Script::intern(std::string_view s, StrIdx& out) {
    if (find_string(s, out)) return true;
    if (strings.size() >= 0xFFFF || s.size() > 0xFFFF) return false;
    strings.emplace_back(s);
    out = static_cast<StrIdx>(strings.size() - 1);
    return true;
}

const FunctionDebug* Script::debug_for(StrIdx object, StrIdx state, StrIdx name, FunctionKind kind) const noexcept {
    if (!debug.present) return nullptr;
    for (const FunctionDebug& f : debug.functions) {
        if (f.object == object && f.state == state && f.name == name && f.kind == kind) return &f;
    }
    return nullptr;
}

Status rewrite_check(const uint8_t* data, size_t size, std::string* error) noexcept {
    Script s;
    Status st = read(data, size, s, error);
    if (st != Status::Ok) return st;
    std::vector<uint8_t> out;
    st = write(s, out, error);
    if (st != Status::Ok) return st;
    if (out.size() == size && std::memcmp(out.data(), data, size) == 0) return Status::Ok;
    if (error) {
        size_t i = 0;
        const size_t n = out.size() < size ? out.size() : size;
        while (i < n && out[i] == data[i]) ++i;
        char buf[160];
        std::snprintf(buf, sizeof buf, "rewrite differs at byte %zu (input %zu bytes, output %zu bytes)", i, size,
                      out.size());
        *error = buf;
    }
    return Status::Corrupt;
}

}  // namespace papyrus::pex
