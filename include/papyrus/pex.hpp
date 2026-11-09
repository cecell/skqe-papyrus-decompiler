// skqe-papyrus: a typed model of compiled Papyrus scripts (.pex) for Skyrim
// LE/SE, with a reader that refuses damaged files and a writer that reproduces
// a read file byte for byte.
//
// The model keeps everything in file order and keeps strings as indices into
// the script's own string table, so that write(read(bytes)) == bytes. Views
// (source, assembly, JSON) and edits (FormID renumbering, string renames) are
// built on top of this model; nothing here interprets the bytecode.
//
// Nothing throws. Every entry point returns a Status and, when asked, a
// one-sentence error naming what was wrong and where.
//
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace papyrus::pex {

inline constexpr const char* kVersion = "0.91";

enum class Status {
    Ok = 0,
    NotPex,      // the file does not start with the Papyrus magic
    NotSkyrim,   // a Papyrus file for another game (little-endian: FO4/FO76/Starfield)
    Truncated,   // the file ends inside a structure it declares
    Corrupt,     // a field has an impossible value (bad string index, opcode, count)
    TooLarge,    // a declared size exceeds what this library will allocate
    IoError,     // the file could not be opened or read
    BadModel,    // write(): the model is inconsistent (string index out of range, ...)
    Syntax,      // assemble(): the listing could not be read (the error names the line)
};
const char* to_string(Status s) noexcept;

// Index into Script::strings. Papyrus files address at most 65,535 strings.
using StrIdx = uint16_t;

// --- values ---------------------------------------------------------------

enum class ValueType : uint8_t {
    None = 0,
    Identifier = 1,  // payload: StrIdx (a variable, type, property or function name)
    String = 2,      // payload: StrIdx (a literal)
    Integer = 3,     // payload: int32
    Float = 4,       // payload: IEEE-754 single
    Bool = 5,        // payload: one byte, any non-zero value is true
};

struct Value {
    ValueType type = ValueType::None;
    // Raw payload: the string index, the int32 bit pattern, the float bit
    // pattern, or the bool byte. Kept raw so unusual encodings (a bool byte of
    // 2, a float NaN payload) survive a rewrite unchanged.
    uint32_t bits = 0;

    static Value none() noexcept { return {}; }
    static Value identifier(StrIdx s) noexcept { return {ValueType::Identifier, s}; }
    static Value string(StrIdx s) noexcept { return {ValueType::String, s}; }
    static Value integer(int32_t v) noexcept { return {ValueType::Integer, static_cast<uint32_t>(v)}; }
    static Value floating(float v) noexcept {
        uint32_t b;
        std::memcpy(&b, &v, 4);
        return {ValueType::Float, b};
    }
    static Value boolean(bool v) noexcept { return {ValueType::Bool, v ? 1u : 0u}; }

    bool is_string_ref() const noexcept { return type == ValueType::Identifier || type == ValueType::String; }
    StrIdx str() const noexcept { return static_cast<StrIdx>(bits); }
    int32_t as_int() const noexcept { return static_cast<int32_t>(bits); }
    float as_float() const noexcept {
        float f;
        std::memcpy(&f, &bits, 4);
        return f;
    }
    bool as_bool() const noexcept { return bits != 0; }
};

// --- bytecode -------------------------------------------------------------

// Skyrim's instruction set (0x00..0x23). Later games add 0x24.. and are not
// accepted by this reader.
enum class Opcode : uint8_t {
    Nop = 0x00,
    IAdd, FAdd, ISub, FSub, IMul, FMul, IDiv, FDiv, IMod,   // 0x01..0x09: dest, a, b
    Not, INeg, FNeg,                                        // 0x0A..0x0C: dest, src
    Assign,                                                 // 0x0D: dest, src
    Cast,                                                   // 0x0E: dest, src
    CmpEq, CmpLt, CmpLe, CmpGt, CmpGe,                      // 0x0F..0x13: dest, a, b
    Jmp,                                                    // 0x14: offset
    JmpT, JmpF,                                             // 0x15, 0x16: cond, offset
    CallMethod,                                             // 0x17: name, object, dest, [args]
    CallParent,                                             // 0x18: name, dest, [args]
    CallStatic,                                             // 0x19: class, name, dest, [args]
    Return,                                                 // 0x1A: value
    StrCat,                                                 // 0x1B: dest, a, b
    PropGet,                                                // 0x1C: name, object, dest
    PropSet,                                                // 0x1D: name, object, value
    ArrayCreate,                                            // 0x1E: dest, size
    ArrayLength,                                            // 0x1F: dest, array
    ArrayGetElement,                                        // 0x20: dest, array, index
    ArraySetElement,                                        // 0x21: array, index, value
    ArrayFindElement,                                       // 0x22: array, dest, value, start
    ArrayRFindElement,                                      // 0x23: array, dest, value, start
};
inline constexpr uint8_t kMaxOpcode = 0x23;

struct OpcodeInfo {
    const char* mnemonic;  // as PapyrusAssembler spells it
    uint8_t fixedArgs;     // operands always present
    bool varArgs;          // followed by an Integer count and that many more operands
};
// Table lookup for 0x00..0x23; nullptr beyond.
const OpcodeInfo* opcode_info(uint8_t op) noexcept;

struct Instruction {
    Opcode op = Opcode::Nop;
    // Fixed operands first, then the variable ones (call arguments). The
    // count value that precedes the variable operands in the file is implied
    // by args.size() - fixedArgs and is regenerated on write.
    std::vector<Value> args;

    size_t var_arg_count() const noexcept;
    const Value* var_arg(size_t i) const noexcept;
};

// --- declarations ---------------------------------------------------------

struct Param {
    StrIdx name = 0;
    StrIdx type = 0;
};

enum FunctionFlags : uint8_t { kFnGlobal = 0x01, kFnNative = 0x02 };

struct Function {
    StrIdx returnType = 0;
    StrIdx docString = 0;
    uint32_t userFlags = 0;
    uint8_t flags = 0;  // FunctionFlags
    std::vector<Param> params;
    std::vector<Param> locals;
    std::vector<Instruction> code;

    bool is_global() const noexcept { return flags & kFnGlobal; }
    bool is_native() const noexcept { return flags & kFnNative; }
};

struct NamedFunction {
    StrIdx name = 0;
    Function body;
};

struct Variable {
    StrIdx name = 0;
    StrIdx type = 0;
    uint32_t userFlags = 0;
    Value initial;
};

enum PropertyFlags : uint8_t { kPropRead = 0x01, kPropWrite = 0x02, kPropAutoVar = 0x04 };

struct Property {
    StrIdx name = 0;
    StrIdx type = 0;
    StrIdx docString = 0;
    uint32_t userFlags = 0;
    uint8_t flags = 0;  // PropertyFlags
    StrIdx autoVar = 0;  // valid when flags & kPropAutoVar
    std::optional<Function> getter;  // present when readable and not an autovar
    std::optional<Function> setter;  // present when writable and not an autovar

    bool is_auto() const noexcept { return flags & kPropAutoVar; }
    bool readable() const noexcept { return flags & kPropRead; }
    bool writable() const noexcept { return flags & kPropWrite; }
};

struct State {
    StrIdx name = 0;  // the empty string names the default state
    std::vector<NamedFunction> functions;
};

struct Object {
    StrIdx name = 0;
    // The size field as found in the file. Bethesda's compiler writes the
    // byte count from the size field itself to the end of the object,
    // big-endian; some other compilers write a different count. The engine
    // ignores the field, so the reader accepts any value, remembers it, and
    // the writer reproduces it unless `canonicalSize` is set, in which case
    // the writer recomputes it the Bethesda way.
    uint32_t declaredSize = 0;
    bool canonicalSize = true;
    StrIdx parent = 0;  // the empty string when the script extends nothing
    StrIdx docString = 0;
    uint32_t userFlags = 0;
    StrIdx autoState = 0;
    std::vector<Variable> variables;
    std::vector<Property> properties;
    std::vector<State> states;
};

// --- debug info -----------------------------------------------------------

enum class FunctionKind : uint8_t { Method = 0, Getter = 1, Setter = 2 };

struct FunctionDebug {
    StrIdx object = 0;
    StrIdx state = 0;
    StrIdx name = 0;
    FunctionKind kind = FunctionKind::Method;
    std::vector<uint16_t> lines;  // one source line per instruction
};

struct DebugInfo {
    bool present = false;
    uint64_t modificationTime = 0;  // time_t of the source when compiled
    std::vector<FunctionDebug> functions;
};

struct UserFlag {
    StrIdx name = 0;
    uint8_t bit = 0;  // mask = 1 << bit
};

struct Header {
    uint8_t major = 3;
    uint8_t minor = 2;
    uint16_t gameId = 1;  // 1 = Skyrim
    uint64_t compileTime = 0;  // time_t
    std::string sourceName;    // "Foo.psc" as given to the compiler
    std::string userName;
    std::string computerName;
};

// --- the script -----------------------------------------------------------

struct Script {
    Header header;
    std::vector<std::string> strings;  // raw bytes, as compiled (no transcoding)
    DebugInfo debug;
    std::vector<UserFlag> userFlags;
    std::vector<Object> objects;  // Skyrim files carry exactly one
    // Bytes after the last object. The engine ignores them; a few shipped
    // mod scripts have some. Reproduced by the writer.
    std::vector<uint8_t> trailing;
    // Oddities that did not prevent reading (a nonstandard object size,
    // trailing bytes). One sentence each.
    std::vector<std::string> notes;

    // The text behind an index; "" and a null pointer for an out-of-range index.
    std::string_view str(StrIdx i) const noexcept;
    // Exact (byte-wise) lookup. Returns false when the table has no such string.
    bool find_string(std::string_view s, StrIdx& out) const noexcept;
    // Exact lookup, appending when absent. Returns false when the table is full.
    bool intern(std::string_view s, StrIdx& out);
    // The debug record of a function, or nullptr when the file has none.
    const FunctionDebug* debug_for(StrIdx object, StrIdx state, StrIdx name, FunctionKind kind) const noexcept;
};

// --- read / write ---------------------------------------------------------

Status read(const uint8_t* data, size_t size, Script& out, std::string* error = nullptr) noexcept;
Status read_file(const std::filesystem::path& path, Script& out, std::string* error = nullptr) noexcept;

// Serializes the model. Fails (BadModel) when an index is out of range, a
// string or table is too long for its length field, or a vararg list is too
// long. On success `out` holds exactly the bytes a reader would accept.
Status write(const Script& script, std::vector<uint8_t>& out, std::string* error = nullptr) noexcept;
Status write_file(const Script& script, const std::filesystem::path& path, std::string* error = nullptr) noexcept;

// Byte-exact identity check: read + write + compare. Returns Ok when the
// rewritten bytes equal the input; Corrupt with a message naming the first
// differing offset otherwise (or the read status when reading fails).
Status rewrite_check(const uint8_t* data, size_t size, std::string* error = nullptr) noexcept;

// JSON rendering of the whole model (strings resolved, bytecode as arrays).
std::string to_json(const Script& script, bool pretty = true);

// Papyrus assembly in the form PapyrusAssembler.exe reads (docs/ASM_SYNTAX.md).
// Jump targets become labels; debug line numbers become ";@line N" comments.
std::string to_asm(const Script& script);

// One instruction as assembly text, jump targets as absolute indices
// ("JUMPF ::temp0 -> 12"). For diagnostics and the decompiler's stubs.
std::string format_instruction(const Script& script, const Instruction& ins, size_t ip);

}  // namespace papyrus::pex
