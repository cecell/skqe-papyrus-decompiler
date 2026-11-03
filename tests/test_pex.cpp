// Unit tests: a hand-built script (no game data needed), its byte-exact
// rewrite, and damaged variants that must be refused, never crash.
// `--seeds <dir>` writes the hand-built inputs for the fuzzer.
// SPDX-License-Identifier: GPL-3.0-only
#include "papyrus/pex.hpp"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace papyrus::pex;
namespace fs = std::filesystem;

static int g_failures = 0;
#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) {                                                              \
            ++g_failures;                                                           \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
        }                                                                           \
    } while (0)
#define CHECK_STATUS(expr, want)                                                                              \
    do {                                                                                                      \
        const Status got_ = (expr);                                                                           \
        if (got_ != (want)) {                                                                                 \
            ++g_failures;                                                                                     \
            std::fprintf(stderr, "FAIL %s:%d: %s -> %s, wanted %s\n", __FILE__, __LINE__, #expr,              \
                         to_string(got_), to_string(want));                                                   \
        }                                                                                                     \
    } while (0)

// --- byte builder (big-endian) ---------------------------------------------

struct B {
    std::vector<uint8_t> v;
    void u8(unsigned x) { v.push_back(uint8_t(x)); }
    void u16(unsigned x) {
        u8(x >> 8);
        u8(x);
    }
    void u32(uint32_t x) {
        u16(x >> 16);
        u16(x & 0xFFFF);
    }
    void u64(uint64_t x) {
        u32(uint32_t(x >> 32));
        u32(uint32_t(x));
    }
    void str(const char* s) {
        const size_t n = std::strlen(s);
        u16(unsigned(n));
        v.insert(v.end(), s, s + n);
    }
    void ident(unsigned i) {
        u8(1);
        u16(i);
    }
    void lit(unsigned i) {
        u8(2);
        u16(i);
    }
    void integer(int32_t i) {
        u8(3);
        u32(uint32_t(i));
    }
    void flt(uint32_t bits) {
        u8(4);
        u32(bits);
    }
    void boolean(unsigned b) {
        u8(5);
        u8(b);
    }
    void none() { u8(0); }
    size_t mark() const { return v.size(); }
    void patch32(size_t at, uint32_t x) {
        v[at] = uint8_t(x >> 24);
        v[at + 1] = uint8_t(x >> 16);
        v[at + 2] = uint8_t(x >> 8);
        v[at + 3] = uint8_t(x);
    }
};

// String table of the sample:
//  0 ""            1 "Sample"      2 "ScriptObject" 3 "Int"      4 "OnInit"
//  5 "::temp0"     6 "Count"       7 "::Count_var"  8 "Debug"    9 "Trace"
// 10 "hello"      11 "None"       12 "::NoneVar"   13 "Bool"    14 "Ready"
// 15 "String"     16 "Sample.psc" 17 "Float"       18 "hidden"  19 "x"
static B sample(bool withDebug = true) {
    B b;
    b.u32(0xFA57C0DE);
    b.u8(3);
    b.u8(2);
    b.u16(1);
    b.u64(0x5F000000ull);
    b.str("Sample.psc");
    b.str("user");
    b.str("PC");
    const char* strings[] = {"",      "Sample", "ScriptObject", "Int",   "OnInit", "::temp0", "Count",
                             "::Count_var", "Debug", "Trace", "hello", "None",  "::NoneVar", "Bool",
                             "Ready", "String", "Sample.psc", "Float", "hidden", "x"};
    b.u16(20);
    for (const char* s : strings) b.str(s);
    // debug info
    if (withDebug) {
        b.u8(1);
        b.u64(0x5E000000ull);
        b.u16(2);  // two functions: OnInit and Ready's getter
        b.u16(1);  // object Sample
        b.u16(0);  // state ""
        b.u16(4);  // OnInit
        b.u8(0);
        b.u16(4);  // four instructions
        b.u16(10);
        b.u16(11);
        b.u16(11);
        b.u16(12);
        b.u16(1);
        b.u16(0);
        b.u16(14);  // Ready
        b.u8(1);    // getter
        b.u16(1);
        b.u16(20);
    } else {
        b.u8(0);
    }
    // user flags
    b.u16(1);
    b.u16(18);  // "hidden"
    b.u8(0);
    // objects
    b.u16(1);
    b.u16(1);  // name Sample
    const size_t sizeAt = b.mark();  // the object size counts from this field
    b.u32(0);
    b.u16(2);  // parent ScriptObject
    b.u16(0);  // doc
    b.u32(0);  // user flags
    b.u16(0);  // auto state ""
    // variables: ::Count_var Int = 5, x Float = 1.5
    b.u16(2);
    b.u16(7);
    b.u16(3);
    b.u32(0);
    b.integer(5);
    b.u16(19);
    b.u16(17);
    b.u32(1);  // hidden
    b.flt(0x3FC00000);
    // properties: Count (auto), Ready (Bool, read-only getter)
    b.u16(2);
    b.u16(6);
    b.u16(3);
    b.u16(0);
    b.u32(0);
    b.u8(7);   // read|write|autovar
    b.u16(7);  // ::Count_var
    b.u16(14);
    b.u16(13);
    b.u16(0);
    b.u32(0);
    b.u8(1);  // read only -> getter body follows
    //   getter: Bool Get() { return true }
    b.u16(13);  // return type Bool
    b.u16(0);
    b.u32(0);
    b.u8(0);
    b.u16(0);  // params
    b.u16(0);  // locals
    b.u16(1);  // one instruction
    b.u8(0x1A);
    b.boolean(1);
    // states: "" with OnInit
    b.u16(1);
    b.u16(0);
    b.u16(1);
    b.u16(4);   // OnInit
    b.u16(11);  // returns None
    b.u16(0);
    b.u32(0);
    b.u8(0);
    b.u16(0);  // params
    b.u16(1);  // locals: ::temp0 Int
    b.u16(5);
    b.u16(3);
    b.u16(4);  // four instructions
    //   IADD ::temp0 ::Count_var 1
    b.u8(0x01);
    b.ident(5);
    b.ident(7);
    b.integer(1);
    //   ASSIGN ::Count_var ::temp0
    b.u8(0x0D);
    b.ident(7);
    b.ident(5);
    //   CALLSTATIC Debug Trace ::NoneVar 2 "hello" 0
    b.u8(0x19);
    b.ident(8);
    b.ident(9);
    b.ident(12);
    b.integer(2);
    b.lit(10);
    b.integer(0);
    //   RETURN None
    b.u8(0x1A);
    b.none();
    b.patch32(sizeAt, uint32_t(b.mark() - sizeAt));
    return b;
}

static void test_read_sample() {
    const B b = sample();
    Script s;
    std::string err;
    CHECK_STATUS(read(b.v.data(), b.v.size(), s, &err), Status::Ok);
    CHECK(err.empty());
    CHECK(s.header.major == 3 && s.header.minor == 2 && s.header.gameId == 1);
    CHECK(s.header.sourceName == "Sample.psc");
    CHECK(s.notes.empty() && s.trailing.empty() && s.objects[0].canonicalSize);
    CHECK(s.strings.size() == 20);
    CHECK(s.debug.present && s.debug.functions.size() == 2);
    CHECK(s.debug.functions[0].lines.size() == 4 && s.debug.functions[0].lines[3] == 12);
    CHECK(s.debug.functions[1].kind == FunctionKind::Getter);
    CHECK(s.userFlags.size() == 1 && s.str(s.userFlags[0].name) == "hidden");
    CHECK(s.objects.size() == 1);
    const Object& o = s.objects[0];
    CHECK(s.str(o.name) == "Sample" && s.str(o.parent) == "ScriptObject");
    CHECK(o.variables.size() == 2);
    CHECK(o.variables[0].initial.type == ValueType::Integer && o.variables[0].initial.as_int() == 5);
    CHECK(o.variables[1].initial.type == ValueType::Float && o.variables[1].initial.as_float() == 1.5f);
    CHECK(o.properties.size() == 2);
    CHECK(o.properties[0].is_auto() && s.str(o.properties[0].autoVar) == "::Count_var");
    CHECK(!o.properties[1].is_auto() && o.properties[1].getter && !o.properties[1].setter);
    CHECK(o.properties[1].getter->code.size() == 1);
    CHECK(o.states.size() == 1 && o.states[0].functions.size() == 1);
    const Function& f = o.states[0].functions[0].body;
    CHECK(s.str(o.states[0].functions[0].name) == "OnInit");
    CHECK(f.locals.size() == 1 && f.code.size() == 4);
    CHECK(f.code[0].op == Opcode::IAdd && f.code[0].args.size() == 3);
    CHECK(f.code[2].op == Opcode::CallStatic && f.code[2].args.size() == 5);
    CHECK(f.code[2].var_arg_count() == 2);
    CHECK(f.code[2].var_arg(0)->type == ValueType::String && s.str(f.code[2].var_arg(0)->str()) == "hello");
    CHECK(f.code[3].op == Opcode::Return && f.code[3].args[0].type == ValueType::None);
    CHECK(s.debug_for(o.name, o.states[0].name, o.states[0].functions[0].name, FunctionKind::Method) != nullptr);
    CHECK(s.debug_for(o.name, 0, o.properties[1].name, FunctionKind::Getter) != nullptr);
    CHECK(s.debug_for(o.name, 0, o.properties[1].name, FunctionKind::Setter) == nullptr);

    // Byte-exact rewrite.
    std::vector<uint8_t> out;
    CHECK_STATUS(write(s, out, &err), Status::Ok);
    CHECK(out == b.v);
    CHECK_STATUS(rewrite_check(b.v.data(), b.v.size(), &err), Status::Ok);

    // Without debug info.
    const B nd = sample(false);
    CHECK_STATUS(rewrite_check(nd.v.data(), nd.v.size(), &err), Status::Ok);
    Script s2;
    CHECK_STATUS(read(nd.v.data(), nd.v.size(), s2, &err), Status::Ok);
    CHECK(!s2.debug.present && s2.debug_for(1, 0, 4, FunctionKind::Method) == nullptr);

    // Model helpers.
    StrIdx idx = 0;
    CHECK(s.find_string("Trace", idx) && idx == 9);
    CHECK(!s.find_string("trace", idx));
    CHECK(s.intern("NewOne", idx) && idx == 20 && s.strings.size() == 21);
    CHECK(s.intern("NewOne", idx) && idx == 20 && s.strings.size() == 21);
    CHECK(s.str(999).empty());

    // JSON renders without crashing and mentions the pieces.
    const std::string j = to_json(s);
    CHECK(j.find("\"CALLSTATIC\"") != std::string::npos);
    CHECK(j.find("\"autoVar\": \"::Count_var\"") != std::string::npos);
    CHECK(j.find("\"source\": \"Sample.psc\"") != std::string::npos);
}

static void test_truncations() {
    const B b = sample();
    // Every prefix must be refused without crashing.
    for (size_t n = 0; n < b.v.size(); ++n) {
        Script s;
        std::string err;
        const Status st = read(b.v.data(), n, s, &err);
        if (n < 4) {
            CHECK(st == Status::NotPex);
        } else {
            CHECK(st == Status::Truncated || st == Status::Corrupt);
        }
        CHECK(!err.empty());
    }
}

static void test_damage() {
    std::string err;
    Script s;
    {
        // Trailing bytes are kept (the engine ignores them; shipped mods have them).
        B b = sample();
        b.v.push_back(0);
        b.v.push_back(7);
        CHECK_STATUS(read(b.v.data(), b.v.size(), s, &err), Status::Ok);
        CHECK(s.trailing.size() == 2 && s.trailing[1] == 7);
        CHECK(s.notes.size() == 1 && s.notes[0].find("2 trailing bytes") != std::string::npos);
        CHECK_STATUS(rewrite_check(b.v.data(), b.v.size(), &err), Status::Ok);
    }
    {
        // Little-endian magic.
        B b = sample();
        b.v[0] = 0xDE;
        b.v[1] = 0xC0;
        b.v[2] = 0x57;
        b.v[3] = 0xFA;
        CHECK_STATUS(read(b.v.data(), b.v.size(), s, &err), Status::NotSkyrim);
    }
    {
        // Wrong magic.
        B b = sample();
        b.v[0] = 0;
        CHECK_STATUS(read(b.v.data(), b.v.size(), s, &err), Status::NotPex);
    }
    {
        // Object parent string index beyond the table.
        B b = sample();
        // The object name "Sample" (u16 1) follows the user flag table; find it:
        // header ... we locate the pattern of the object header deterministically
        // by rebuilding: simpler to read, mutate the model, and write.
        Script m;
        CHECK_STATUS(read(b.v.data(), b.v.size(), m, &err), Status::Ok);
        m.objects[0].parent = 500;
        std::vector<uint8_t> out;
        CHECK_STATUS(write(m, out, &err), Status::BadModel);
        CHECK(err.find("string 500") != std::string::npos);
    }
    {
        // Bad opcode: patch the IADD (first instruction of OnInit) to 0x24.
        B b = sample();
        bool patched = false;
        // The IADD is the byte 0x01 followed by ident(5) = 01 00 05.
        for (size_t i = 0; i + 3 < b.v.size(); ++i) {
            if (b.v[i] == 0x01 && b.v[i + 1] == 0x01 && b.v[i + 2] == 0x00 && b.v[i + 3] == 0x05) {
                b.v[i] = 0x24;
                patched = true;
                break;
            }
        }
        CHECK(patched);
        CHECK_STATUS(read(b.v.data(), b.v.size(), s, &err), Status::Corrupt);
        CHECK(err.find("opcode 0x24") != std::string::npos);
    }
    {
        // Vararg count larger than the file.
        B b = sample();
        bool patched = false;
        // CALLSTATIC ... integer(2): 19 01 00 08 01 00 09 01 00 0C 03 00 00 00 02
        for (size_t i = 0; i + 14 < b.v.size(); ++i) {
            if (b.v[i] == 0x19 && b.v[i + 10] == 0x03 && b.v[i + 14] == 0x02) {
                b.v[i + 11] = 0x7F;  // 0x7F000002
                patched = true;
                break;
            }
        }
        CHECK(patched);
        CHECK_STATUS(read(b.v.data(), b.v.size(), s, &err), Status::Corrupt);
        CHECK(err.find("argument count") != std::string::npos);
    }
    {
        // Negative vararg count.
        B b = sample();
        for (size_t i = 0; i + 14 < b.v.size(); ++i) {
            if (b.v[i] == 0x19 && b.v[i + 10] == 0x03 && b.v[i + 14] == 0x02) {
                b.v[i + 11] = 0xFF;
                break;
            }
        }
        CHECK_STATUS(read(b.v.data(), b.v.size(), s, &err), Status::Corrupt);
    }
    {
        // Object size with another convention: accepted, noted, reproduced.
        B b = sample();
        Script m;
        CHECK_STATUS(read(b.v.data(), b.v.size(), m, &err), Status::Ok);
        // Locate the size field: it is the u32 right before parent(2)/doc(0)/flags(0)/autostate(0).
        bool patched = false;
        for (size_t i = 0; i + 14 < b.v.size(); ++i) {
            if (b.v[i + 4] == 0 && b.v[i + 5] == 2 && b.v[i + 6] == 0 && b.v[i + 7] == 0 && b.v[i + 8] == 0 &&
                b.v[i + 9] == 0 && b.v[i + 10] == 0 && b.v[i + 11] == 0 && b.v[i + 12] == 0 && b.v[i + 13] == 0 &&
                b.v[i - 2] == 0 && b.v[i - 1] == 1) {
                b.v[i + 3] ^= 1;
                patched = true;
                break;
            }
        }
        CHECK(patched);
        CHECK_STATUS(read(b.v.data(), b.v.size(), s, &err), Status::Ok);
        CHECK(!s.objects[0].canonicalSize);
        CHECK(s.notes.size() == 1 && s.notes[0].find("size field says") != std::string::npos);
        CHECK_STATUS(rewrite_check(b.v.data(), b.v.size(), &err), Status::Ok);
        // Asking for the canonical size changes exactly that field.
        s.objects[0].canonicalSize = true;
        std::vector<uint8_t> out;
        CHECK_STATUS(write(s, out, &err), Status::Ok);
        CHECK(out == sample().v);
        // The little-endian convention is recognised by name.
        Script le;
        B c = sample();
        const uint32_t canon = m.objects[0].declaredSize;
        const uint32_t other = canon - 4;
        for (size_t i = 0; i + 14 < c.v.size(); ++i) {
            if (c.v[i + 4] == 0 && c.v[i + 5] == 2 && c.v[i + 6] == 0 && c.v[i + 7] == 0 && c.v[i + 8] == 0 &&
                c.v[i + 9] == 0 && c.v[i + 10] == 0 && c.v[i + 11] == 0 && c.v[i + 12] == 0 && c.v[i + 13] == 0 &&
                c.v[i - 2] == 0 && c.v[i - 1] == 1) {
                c.v[i] = uint8_t(other);
                c.v[i + 1] = uint8_t(other >> 8);
                c.v[i + 2] = uint8_t(other >> 16);
                c.v[i + 3] = uint8_t(other >> 24);
                break;
            }
        }
        CHECK_STATUS(read(c.v.data(), c.v.size(), le, &err), Status::Ok);
        CHECK(le.notes.size() == 1 && le.notes[0].find("little-endian") != std::string::npos);
        CHECK_STATUS(rewrite_check(c.v.data(), c.v.size(), &err), Status::Ok);
    }
    {
        // Empty and tiny inputs.
        CHECK_STATUS(read(nullptr, 0, s, &err), Status::NotPex);
        const uint8_t magic[] = {0xFA, 0x57, 0xC0, 0xDE};
        CHECK_STATUS(read(magic, 4, s, &err), Status::Truncated);
    }
    {
        // Writer refuses what the file format cannot hold.
        Script m;
        const B b = sample();
        CHECK_STATUS(read(b.v.data(), b.v.size(), m, &err), Status::Ok);
        m.strings.push_back(std::string(70000, 'a'));
        std::vector<uint8_t> out;
        CHECK_STATUS(write(m, out, &err), Status::BadModel);
        m.strings.pop_back();
        m.objects[0].states[0].functions[0].body.code[0].args.push_back(Value::integer(1));  // IADD with 4 args
        CHECK_STATUS(write(m, out, &err), Status::BadModel);
    }
}

static void write_seeds(const fs::path& dir) {
    std::error_code ec;
    fs::create_directories(dir, ec);
    const B a = sample(true);
    const B b = sample(false);
    std::ofstream(dir / "sample_debug.pex", std::ios::binary).write(reinterpret_cast<const char*>(a.v.data()),
                                                                     static_cast<std::streamsize>(a.v.size()));
    std::ofstream(dir / "sample_nodebug.pex", std::ios::binary).write(reinterpret_cast<const char*>(b.v.data()),
                                                                       static_cast<std::streamsize>(b.v.size()));
}

int main(int argc, char** argv) {
    if (argc == 3 && std::strcmp(argv[1], "--seeds") == 0) {
        write_seeds(argv[2]);
        return 0;
    }
    test_read_sample();
    test_truncations();
    test_damage();
    if (g_failures) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::puts("all tests passed");
    return 0;
}
