// Unit tests: a hand-built script (no game data needed), its byte-exact
// rewrite, and damaged variants that must be refused, never crash.
// `--seeds <dir>` writes the hand-built inputs for the fuzzer.
// SPDX-License-Identifier: GPL-3.0-only
#include "papyrus/analysis.hpp"
#include "papyrus/assemble.hpp"
#include "papyrus/audit.hpp"
#include "papyrus/canon.hpp"
#include "papyrus/compile.hpp"
#include "papyrus/decompile.hpp"
#include "papyrus/pex.hpp"
#include "compile/dotnet_hashtable.hpp"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <map>
#include <filesystem>
#include <fstream>
#include <iterator>
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

static void test_asm_and_canon() {
    const B b = sample();
    Script s;
    std::string err;
    CHECK_STATUS(read(b.v.data(), b.v.size(), s, &err), Status::Ok);
    const std::string a = to_asm(s);
    CHECK(a.find(".object Sample ScriptObject\n") != std::string::npos);
    CHECK(a.find(".property Count Int auto\n") != std::string::npos);
    CHECK(a.find(".autoVar ::Count_var\n") != std::string::npos);
    CHECK(a.find(".property Ready Bool\n") != std::string::npos);
    CHECK(a.find(".function get\n") != std::string::npos);
    CHECK(a.find("RETURN True ;@line 20\n") != std::string::npos);
    CHECK(a.find("IADD ::temp0 ::Count_var 1 ;@line 10\n") != std::string::npos);
    CHECK(a.find("CALLSTATIC Debug Trace ::NoneVar \"hello\" 0 ;@line 11\n") != std::string::npos);
    CHECK(a.find(".initialValue 1.5\n") != std::string::npos);
    CHECK(a.find(".variable x Float\n") != std::string::npos);
    CHECK(a.find(".flag hidden 0\n") != std::string::npos);

    // Canonical comparison: equal to itself, insensitive to table order and
    // identifier case, sensitive to an instruction change.
    Script t = s;
    CHECK(canonical_equal(s, t));
    std::swap(t.objects[0].variables[0], t.objects[0].variables[1]);
    std::swap(t.objects[0].properties[0], t.objects[0].properties[1]);
    t.strings[9] = "TRACE";
    CHECK(canonical_equal(s, t));
    t.objects[0].states[0].functions[0].body.code[0].op = Opcode::ISub;
    std::vector<Difference> diffs;
    CHECK(!canonical_equal(s, t, &diffs));
    CHECK(diffs.size() == 1 && diffs[0].kind == DiffKind::Instruction && diffs[0].where == "sample.oninit");
    CHECK(diffs[0].message.find("ip 0") != std::string::npos);
    // A temp renamed: unequal by default, equal with renumbering.
    Script u = s;
    u.strings[5] = "::temp7";
    CHECK(!canonical_equal(s, u));
    CompareOptions opt;
    opt.renumberTemps = true;
    CHECK(canonical_equal(s, u, nullptr, opt));
    // Debug line counts matter, line numbers do not.
    Script v = s;
    v.debug.functions[0].lines[0] = 99;
    CHECK(canonical_equal(s, v));
    v.debug.functions[0].lines.pop_back();
    CHECK(!canonical_equal(s, v));
}

static void test_decompile() {
    const B b = sample();
    Script s;
    std::string err;
    CHECK_STATUS(read(b.v.data(), b.v.size(), s, &err), Status::Ok);
    const DecompileResult r = decompile(s);
    CHECK(r.all_ok());
    CHECK(r.functions.size() == 2);
    const std::string& p = r.source;
    CHECK(p.rfind("\xEF\xBB\xBFScriptname Sample extends ScriptObject\n", 0) == 0);
    CHECK(p.find("Float x = 1.5 Hidden\n") != std::string::npos);
    CHECK(p.find("Int Property Count = 5 Auto\n") != std::string::npos);
    CHECK(p.find("Bool Property Ready = True AutoReadOnly\n") != std::string::npos);
    CHECK(p.find("Event OnInit()\n") != std::string::npos);
    CHECK(p.find("    Count += 1\n") != std::string::npos);
    CHECK(p.find("    Debug.Trace(\"hello\", 0)\n") != std::string::npos);
    CHECK(p.find("EndEvent\n") != std::string::npos);
    CHECK(p.find("::") == std::string::npos);
    // Declaration stub: same declarations, no statements.
    DecompileOptions stub;
    stub.declarationsOnly = true;
    const std::string q = decompile(s, stub).source;
    CHECK(q.find("Int Property Count = 5 Auto\n") != std::string::npos);
    CHECK(q.find("Event OnInit()\n") != std::string::npos);
    CHECK(q.find("EndEvent\n") != std::string::npos);
    CHECK(q.find("Count += 1") == std::string::npos);
    CHECK(q.find("Debug.Trace") == std::string::npos);
}

// A script with FormID calls: literal, propagated through a local, property
// default, and dynamic.
//  0 ""  1 "Refs"  2 "ScriptObject"  3 "Int"  4 "Form"  5 "OnInit"  6 "game"
//  7 "GetFormFromFile"  8 "::temp0"  9 "Foo.esp"  10 "id"  11 "::MyId_var"
// 12 "MyId"  13 "None"  14 "n"  15 "Other.esm"  16 "Refs.psc"
static B refs_sample() {
    B b;
    b.u32(0xFA57C0DE);
    b.u8(3); b.u8(2); b.u16(1); b.u64(1);
    b.str("Refs.psc"); b.str("u"); b.str("c");
    const char* strings[] = {"", "Refs", "ScriptObject", "Int", "Form", "OnInit", "game", "GetFormFromFile", "::temp0",
                             "Foo.esp", "id", "::MyId_var", "MyId", "None", "n", "Other.esm", "Refs.psc"};
    b.u16(17);
    for (const char* s : strings) b.str(s);
    b.u8(0);  // no debug
    b.u16(0); // no user flags
    b.u16(1);
    b.u16(1);  // object Refs
    const size_t sizeAt = b.mark();
    b.u32(0);
    b.u16(2); b.u16(0); b.u32(0); b.u16(0);
    // variables: ::MyId_var Int = 0x900
    b.u16(1);
    b.u16(11); b.u16(3); b.u32(0); b.integer(0x900);
    // properties: MyId auto
    b.u16(1);
    b.u16(12); b.u16(3); b.u16(0); b.u32(0); b.u8(7); b.u16(11);
    // state "" with OnInit(Int n)
    b.u16(1);
    b.u16(0);
    b.u16(1);
    b.u16(5);
    b.u16(13); b.u16(0); b.u32(0); b.u8(0);
    b.u16(1); b.u16(14); b.u16(3);        // param n Int
    b.u16(2); b.u16(8); b.u16(4); b.u16(10); b.u16(3);  // locals ::temp0 Form, id Int
    b.u16(5);
    // 0: CALLSTATIC game GetFormFromFile ::temp0 2 0x801 "Foo.esp"      literal
    b.u8(0x19); b.ident(6); b.ident(7); b.ident(8); b.integer(2); b.integer(0x801); b.lit(9);
    // 1: ASSIGN id 0x802
    b.u8(0x0D); b.ident(10); b.integer(0x802);
    // 2: CALLSTATIC game GetFormFromFile ::temp0 2 id "Foo.esp"         propagated
    b.u8(0x19); b.ident(6); b.ident(7); b.ident(8); b.integer(2); b.ident(10); b.lit(9);
    // 3: CALLSTATIC game GetFormFromFile ::temp0 2 ::MyId_var "Other.esm"  property default
    b.u8(0x19); b.ident(6); b.ident(7); b.ident(8); b.integer(2); b.ident(11); b.lit(15);
    // 4: CALLSTATIC game GetFormFromFile ::temp0 2 n "Foo.esp"          dynamic
    b.u8(0x19); b.ident(6); b.ident(7); b.ident(8); b.integer(2); b.ident(14); b.lit(9);
    b.patch32(sizeAt, uint32_t(b.mark() - sizeAt));
    return b;
}

static void test_refs_and_patch() {
    const B b = refs_sample();
    Script s;
    std::string err;
    CHECK_STATUS(read(b.v.data(), b.v.size(), s, &err), Status::Ok);
    CHECK_STATUS(rewrite_check(b.v.data(), b.v.size(), &err), Status::Ok);
    const std::vector<FormRef> refs = find_form_refs(s);
    CHECK(refs.size() == 4);
    if (refs.size() == 4) {
        CHECK(refs[0].kind == RefKind::Literal && refs[0].formId == 0x801 && refs[0].plugin == "Foo.esp" && refs[0].pluginLiteral);
        CHECK(refs[1].kind == RefKind::Propagated && refs[1].formId == 0x802 && refs[1].literalIp == 1 && refs[1].variable == "id");
        CHECK(refs[2].kind == RefKind::PropertyDefault && refs[2].formId == 0x900 && refs[2].plugin == "Other.esm");
        CHECK(refs[3].kind == RefKind::Dynamic && refs[3].variable == "n");
        CHECK(refs[0].callee == "game.GetFormFromFile" && refs[0].function == "OnInit");
    }
    FormIdMap map;
    CHECK(load_formid_map("!PLUGIN=Foo.esp\n!NEWPLUGIN=Bar.esp\n00000801|00000A01\n00000802|00000A02\n# comment\n", map, &err));
    CHECK(map.plugin == "Foo.esp" && map.newPlugin == "Bar.esp" && map.oldToNew.size() == 2);
    const PatchResult pr = apply_formid_map(s, map);
    CHECK(pr.edits.size() == 2);
    CHECK(pr.skipped.size() == 1 && pr.skipped[0].kind == RefKind::Dynamic);
    CHECK(pr.renamedStrings.size() == 1);
    CHECK(s.objects[0].states[0].functions[0].body.code[0].args[3].as_int() == 0xA01);
    CHECK(s.objects[0].states[0].functions[0].body.code[1].args[1].as_int() == 0xA02);
    CHECK(s.str(9) == "Bar.esp");
    // Other.esm untouched.
    CHECK(s.objects[0].variables[0].initial.as_int() == 0x900);
    std::vector<uint8_t> out;
    CHECK_STATUS(write(s, out, &err), Status::Ok);
    Script again;
    CHECK_STATUS(read(out.data(), out.size(), again, &err), Status::Ok);
    const std::vector<FormRef> refs2 = find_form_refs(again);
    CHECK(refs2.size() == 4 && refs2[0].formId == 0xA01 && refs2[0].plugin == "Bar.esp");
    // Decompiles with the renamed plugin.
    const DecompileResult d = decompile(again);
    CHECK(d.all_ok());
    CHECK(d.source.find("Game.GetFormFromFile(0x000A01, \"Bar.esp\")") != std::string::npos);
    CHECK(d.source.find("Int id = 2562") != std::string::npos);
}

// --- the assembler ----------------------------------------------------------

static void test_assemble() {
    for (const bool withDebug : {true, false}) {
        const B b = sample(withDebug);
        Script s;
        std::string err;
        CHECK_STATUS(read(b.v.data(), b.v.size(), s, &err), Status::Ok);
        const std::string listing = to_asm(s);
        Script a;
        CHECK_STATUS(assemble(listing, a, &err), Status::Ok);
        CHECK(err.empty());
        // Like PapyrusAssembler, the result always carries debug info; for
        // a listing without line comments every record is empty.
        CHECK(a.debug.present);
        if (!withDebug) {
            CHECK(a.debug.functions.size() == 2 && a.debug.functions[0].lines.empty() && a.debug.functions[1].lines.empty());
            a.debug = s.debug;
        }
        CHECK(canonical_equal(s, a));
        CHECK(to_asm(a) == listing);
        if (withDebug) {
            CHECK(a.debug.modificationTime == 0x5E000000ull);
            // Records in the order the listing meets the functions: the
            // getter first (property table), then OnInit.
            CHECK(a.debug.functions.size() == 2 && a.debug.functions[0].kind == FunctionKind::Getter &&
                  a.debug.functions[1].lines.size() == 4 && a.debug.functions[1].lines[3] == 12);
        }
        CHECK(a.header.sourceName == "Sample.psc" && a.header.userName == "user" && a.header.compileTime == 0x5F000000ull);
        std::vector<uint8_t> out;
        CHECK_STATUS(write(a, out, &err), Status::Ok);
        Script again;
        CHECK_STATUS(read(out.data(), out.size(), again, &err), Status::Ok);
        CHECK(canonical_equal(s, again));
    }

    // Operand typing, escapes, labels, the compiler's own spellings (tabs,
    // trailing spaces, lowercase none/false, numeric jump offsets).
    const char* text =
        ".info\n"
        "\t.source \"T.psc\"\n"
        "  .modifyTime 5\n"
        "  .compileTime 6\n"
        "  .user \"u\"\n"
        "  .computer \"c\"\n"
        ".endInfo\n"
        ".userFlagsRef\n"
        "  .flag hidden 0\n"
        ".endUserFlagsRef\n"
        ".objectTable\n"
        "  .object T ScriptObject\n"
        "    .userFlags 0\n"
        "    .docString \"quote \\\" backslash \\\\ nl \\n tab \\t\"\n"
        "    .autoState \n"
        "    .variableTable\n"
        "      .variable ::X_var int\n"
        "        .userFlags 1\n"
        "        .initialValue -7\n"
        "      .endVariable\n"
        "      .variable f float\n"
        "        .userFlags 0\n"
        "        .initialValue 1.5\n"
        "      .endVariable\n"
        "      .variable n string\n"
        "        .userFlags 0\n"
        "        .initialValue \"s\"\n"
        "      .endVariable\n"
        "    .endVariableTable\n"
        "    .propertyTable\n"
        "      .property X Int auto\n"
        "        .userFlags 0\n"
        "        .docString \"\"\n"
        "        .autoVar ::X_var\n"
        "      .endProperty\n"
        "      .property R Bool\n"
        "        .userFlags 0\n"
        "        .docString \"\"\n"
        "        .function get \n"
        "          .userFlags 0\n"
        "          .docString \"\"\n"
        "          .return bool\n"
        "          .paramTable\n"
        "          .endParamTable\n"
        "          .localTable\n"
        "          .endLocalTable\n"
        "          .code\n"
        "            RETURN true ;@line 3\n"
        "          .endCode\n"
        "        .endFunction\n"
        "      .endProperty\n"
        "    .endPropertyTable\n"
        "    .stateTable\n"
        "      .state\n"
        "        .function Go static\n"
        "          .userFlags 0\n"
        "          .docString \"\"\n"
        "          .return NONE\n"
        "          .paramTable\n"
        "            .param a Int\n"
        "          .endParamTable\n"
        "          .localTable\n"
        "            .local ::temp0 bool\n"
        "            .local ::nonevar none\n"
        "          .endLocalTable\n"
        "          .code\n"
        "            COMPAREEQ ::temp0 a 0 ;@line 10\n"
        "            JUMPF ::temp0 label1 ;@line 10\n"
        "            CALLSTATIC debug Trace ::nonevar \"zero\" 0 ;@line 11\n"
        "            JUMP label0\n"
        "            label1:\n"
        "            CallMethod Foo self ::NoneVar false None 2.0 ; a comment\n"
        "            JUMP 1\n"
        "            label0:\n"
        "          .endCode\n"
        "        .endFunction\n"
        "        .function Nat native\n"
        "          .userFlags 0\n"
        "          .docString \"\"\n"
        "          .return Int\n"
        "          .paramTable\n"
        "          .endParamTable\n"
        "        .endFunction\n"
        "      .endState\n"
        "      .state busy\n"
        "        .function Go\n"
        "          .userFlags 0\n"
        "          .docString \"\"\n"
        "          .return None\n"
        "          .paramTable\n"
        "            .param a Int\n"
        "          .endParamTable\n"
        "          .localTable\n"
        "          .endLocalTable\n"
        "          .code\n"
        "            RETURN None ;@line 20\n"
        "          .endCode\n"
        "        .endFunction\n"
        "        .function Other\n"
        "          .userFlags 0\n"
        "          .docString \"\"\n"
        "          .return None\n"
        "          .paramTable\n"
        "          .endParamTable\n"
        "          .localTable\n"
        "          .endLocalTable\n"
        "          .code\n"
        "            RETURN None ;@line 22\n"
        "          .endCode\n"
        "        .endFunction\n"
        "      .endState\n"
        "    .endStateTable\n"
        "  .endObject\n"
        ".endObjectTable\n";
    Script t;
    std::string err;
    CHECK_STATUS(assemble(text, t, &err), Status::Ok);
    CHECK(err.empty());
    CHECK(t.objects.size() == 1);
    if (t.objects.size() == 1) {
        const Object& o = t.objects[0];
        CHECK(t.str(o.docString) == "quote \" backslash \\ nl \n tab \t");
        CHECK(t.str(o.autoState).empty());
        CHECK(o.variables.size() == 3);
        CHECK(o.variables[0].initial.type == ValueType::Integer && o.variables[0].initial.as_int() == -7 && o.variables[0].userFlags == 1);
        CHECK(o.variables[1].initial.type == ValueType::Float && o.variables[1].initial.as_float() == 1.5f);
        CHECK(o.variables[2].initial.type == ValueType::String && t.str(o.variables[2].initial.str()) == "s");
        CHECK(o.properties.size() == 2 && o.properties[0].flags == (kPropRead | kPropWrite | kPropAutoVar));
        CHECK(o.properties[1].flags == kPropRead && o.properties[1].getter && !o.properties[1].setter);
        CHECK(o.properties[1].getter->code.size() == 1 && o.properties[1].getter->code[0].args[0].type == ValueType::Bool);
        CHECK(o.states.size() == 2 && o.states[0].functions.size() == 2);
        const Function& go = o.states[0].functions[0].body;
        CHECK(go.is_global() && !go.is_native() && t.str(go.returnType) == "None");  // built-in names get the reference spelling
        CHECK(go.params.size() == 1 && go.locals.size() == 2);
        CHECK(go.code.size() == 6);
        if (go.code.size() == 6) {
            CHECK(go.code[1].op == Opcode::JmpF && go.code[1].args[1].type == ValueType::Integer && go.code[1].args[1].as_int() == 3);
            CHECK(go.code[3].op == Opcode::Jmp && go.code[3].args[0].as_int() == 3);
            CHECK(go.code[4].op == Opcode::CallMethod && go.code[4].args.size() == 6);
            CHECK(go.code[4].args[3].type == ValueType::Bool && !go.code[4].args[3].as_bool());
            CHECK(go.code[4].args[4].type == ValueType::None);
            CHECK(go.code[4].args[5].type == ValueType::Float && go.code[4].args[5].as_float() == 2.0f);
            CHECK(go.code[5].op == Opcode::Jmp && go.code[5].args[0].as_int() == 1);
        }
        const Function& nat = o.states[0].functions[1].body;
        CHECK(nat.is_native() && nat.code.empty() && nat.locals.empty());
        // ::nonevar and ::NoneVar are one string, first spelling kept; the
        // types int / bool are stored as Int / Bool.
        StrIdx idx;
        CHECK(t.find_string("::nonevar", idx) && !t.find_string("::NoneVar", idx));
        CHECK(t.find_string("Int", idx) && !t.find_string("int", idx) && t.find_string("Bool", idx));
    }
    // Debug records: one per function; Nat (no ;@line) has none of its own
    // lines; busy's Go is the first of its block and keeps its label; Other,
    // the second, is labelled with the default state.
    CHECK(t.debug.present && t.debug.functions.size() == 5);
    if (t.debug.functions.size() == 5) {
        CHECK(t.debug.functions[0].kind == FunctionKind::Getter && t.debug.functions[0].lines.size() == 1);
        CHECK(t.str(t.debug.functions[1].name) == "Go" && t.str(t.debug.functions[1].state).empty());
        CHECK(t.debug.functions[1].lines.size() == 6 && t.debug.functions[1].lines[3] == 11 && t.debug.functions[1].lines[5] == 11);
        CHECK(t.str(t.debug.functions[2].name) == "Nat" && t.debug.functions[2].lines.empty());
        CHECK(t.str(t.debug.functions[3].name) == "Go" && t.str(t.debug.functions[3].state) == "busy");
        CHECK(t.debug.functions[3].lines.size() == 1 && t.debug.functions[3].lines[0] == 20);
        CHECK(t.str(t.debug.functions[4].name) == "Other" && t.str(t.debug.functions[4].state).empty());
    }
    // It writes, and the listing of the result assembles to the same model.
    std::vector<uint8_t> out;
    CHECK_STATUS(write(t, out, &err), Status::Ok);
    Script t2;
    CHECK_STATUS(assemble(to_asm(t), t2, &err), Status::Ok);
    CHECK(canonical_equal(t, t2));

    // Errors name the line.
    struct Bad {
        const char* text;
        const char* message;
        int line;
    };
    const Bad bad[] = {
        {".info\n.endInfo\n.bogus\n", "expected .info", 3},
        {".info\n.endInfo\n", "no .objectTable", 2},
        {".objectTable\n.object A\n.stateTable\n.state\n.function F\n.code\nIADD a b\n.endCode\n.endFunction\n.endState\n.endStateTable\n.endObject\n.endObjectTable\n", "IADD takes 3", 7},
        {".objectTable\n.object A\n.stateTable\n.state\n.function F\n.code\nJUMP nowhere\n.endCode\n.endFunction\n.endState\n.endStateTable\n.endObject\n.endObjectTable\n", "undefined label nowhere", 7},
        {".objectTable\n.object A\n.docString \"open\n.endObject\n.endObjectTable\n", "unterminated string", 3},
        {".objectTable\n.object A\n.stateTable\n.state\n.function F native\n.code\n.endCode\n.endFunction\n.endState\n.endStateTable\n.endObject\n.endObjectTable\n", "native function has no .code", 6},
        {".objectTable\n.object A\n.stateTable\n.state\n.function F\n.code\nFROB a\n.endCode\n.endFunction\n.endState\n.endStateTable\n.endObject\n.endObjectTable\n", "unknown mnemonic FROB", 7},
        {".objectTable\n.object A\n", "missing .endObject", 2},
    };
    for (const Bad& x : bad) {
        Script z;
        err.clear();
        CHECK_STATUS(assemble(x.text, z, &err), Status::Syntax);
        CHECK(err.find(x.message) != std::string::npos);
        CHECK(err.rfind("line " + std::to_string(x.line) + ":", 0) == 0);
        if (err.find(x.message) == std::string::npos || err.rfind("line " + std::to_string(x.line) + ":", 0) != 0) {
            std::fprintf(stderr, "  got: %s\n", err.c_str());
        }
    }

    // The compiler's own listing of Probe01, when the test runs in the repo.
    for (const char* rel : {"probes/Probe01_AllOps.pas.txt", "../probes/Probe01_AllOps.pas.txt"}) {
        std::ifstream f(rel, std::ios::binary);
        if (!f) continue;
        const std::string probe((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        Script p;
        err.clear();
        CHECK_STATUS(assemble(probe, p, &err), Status::Ok);
        if (!err.empty()) std::fprintf(stderr, "  %s\n", err.c_str());
        CHECK(p.objects.size() == 1 && p.objects[0].states.size() == 3 && p.objects[0].properties.size() == 6);
        CHECK(p.debug.present && p.debug.modificationTime == 1793743743ull);
        std::vector<uint8_t> out2;
        CHECK_STATUS(write(p, out2, &err), Status::Ok);
        Script p2;
        CHECK_STATUS(assemble(to_asm(p), p2, &err), Status::Ok);
        CHECK(canonical_equal(p, p2));
        break;
    }
}

// --- ElseIf against a nested If, from the temps (docs/COMPILER_PATTERNS.md) ---

// A listing with Probe02's Chain (an ElseIf chain) and Nested (the same
// written as nested Else / If), as the compiler emitted them.
static const char* kElseIfListing =
    ".info\n  .source \"P.psc\"\n  .modifyTime 1\n  .compileTime 2\n  .user \"u\"\n  .computer \"c\"\n.endInfo\n"
    ".userFlagsRef\n.endUserFlagsRef\n"
    ".objectTable\n  .object P ObjectReference\n    .userFlags 0\n    .docString \"\"\n    .autoState\n"
    "    .variableTable\n    .endVariableTable\n    .propertyTable\n    .endPropertyTable\n"
    "    .stateTable\n      .state\n"
    "        .function Chain\n          .userFlags 0\n          .docString \"\"\n          .return None\n"
    "          .paramTable\n            .param a Int\n            .param b Int\n            .param c Int\n          .endParamTable\n"
    "          .localTable\n            .local ::temp3 bool\n            .local ::temp4 bool\n            .local ::nonevar none\n"
    "            .local bx Bool\n            .local ::temp5 bool\n            .local by Bool\n            .local by2 Bool\n"
    "            .local bz Bool\n            .local bw Bool\n            .local bw2 Bool\n          .endLocalTable\n"
    "          .code\n"
    "            COMPAREEQ ::temp3 a b ;@line 27\n            JUMPF ::temp3 label3 ;@line 27\n"
    "            COMPAREEQ ::temp4 c a ;@line 28\n            ASSIGN bx ::temp4 ;@line 28\n"
    "            CALLMETHOD Take self ::nonevar 1 1 1 ;@line 29\n            JUMP label0\n            label3:\n"
    "            COMPAREEQ ::temp4 a c ;@line 30\n            JUMPF ::temp4 label1 ;@line 30\n"
    "            COMPAREEQ ::temp5 b c ;@line 31\n            ASSIGN by ::temp5 ;@line 31\n"
    "            CALLMETHOD Take self ::nonevar 2 2 2 ;@line 32\n            JUMP label0\n            label1:\n"
    "            COMPAREEQ ::temp5 b c ;@line 33\n            JUMPF ::temp5 label2 ;@line 33\n"
    "            COMPAREEQ ::temp4 a 1 ;@line 34\n            ASSIGN by2 ::temp4 ;@line 34\n"
    "            CALLMETHOD Take self ::nonevar 3 3 3 ;@line 35\n            JUMP label0\n            label2:\n"
    "            COMPAREEQ ::temp4 a 2 ;@line 37\n            ASSIGN bz ::temp4 ;@line 37\n"
    "            CALLMETHOD Take self ::nonevar 4 4 4 ;@line 38\n            label0:\n"
    "            COMPAREEQ ::temp5 a 3 ;@line 40\n            ASSIGN bw ::temp5 ;@line 40\n"
    "            COMPAREEQ ::temp4 a 4 ;@line 41\n            ASSIGN bw2 ::temp4 ;@line 41\n"
    "          .endCode\n        .endFunction\n"
    "        .function Nested\n          .userFlags 0\n          .docString \"\"\n          .return None\n"
    "          .paramTable\n            .param a Int\n            .param b Int\n            .param c Int\n          .endParamTable\n"
    "          .localTable\n            .local ::temp6 bool\n            .local ::temp7 bool\n            .local ::nonevar none\n"
    "            .local bx Bool\n            .local ::temp8 bool\n            .local by Bool\n            .local ::temp9 bool\n"
    "            .local by2 Bool\n            .local bz Bool\n            .local bw Bool\n            .local bw2 Bool\n          .endLocalTable\n"
    "          .code\n"
    "            COMPAREEQ ::temp6 a b ;@line 46\n            JUMPF ::temp6 label9 ;@line 46\n"
    "            COMPAREEQ ::temp7 c a ;@line 47\n            ASSIGN bx ::temp7 ;@line 47\n"
    "            CALLMETHOD Take self ::nonevar 1 1 1 ;@line 48\n            JUMP label4\n            label9:\n"
    "            COMPAREEQ ::temp7 a c ;@line 50\n            JUMPF ::temp7 label8 ;@line 50\n"
    "            COMPAREEQ ::temp8 b c ;@line 51\n            ASSIGN by ::temp8 ;@line 51\n"
    "            CALLMETHOD Take self ::nonevar 2 2 2 ;@line 52\n            JUMP label5\n            label8:\n"
    "            COMPAREEQ ::temp8 b c ;@line 54\n            JUMPF ::temp8 label7 ;@line 54\n"
    "            COMPAREEQ ::temp9 a 1 ;@line 55\n            ASSIGN by2 ::temp9 ;@line 55\n"
    "            CALLMETHOD Take self ::nonevar 3 3 3 ;@line 56\n            JUMP label6\n            label7:\n"
    "            COMPAREEQ ::temp9 a 2 ;@line 58\n            ASSIGN bz ::temp9 ;@line 58\n"
    "            CALLMETHOD Take self ::nonevar 4 4 4 ;@line 59\n            label6:\n            label5:\n            label4:\n"
    "            COMPAREEQ ::temp9 a 3 ;@line 63\n            ASSIGN bw ::temp9 ;@line 63\n"
    "            COMPAREEQ ::temp8 a 4 ;@line 64\n            ASSIGN bw2 ::temp8 ;@line 64\n"
    "          .endCode\n        .endFunction\n"
    "      .endState\n    .endStateTable\n  .endObject\n.endObjectTable\n";

// The compiler's state-block order is a .NET 2.0 Hashtable's: these are the
// orders of vanilla scripts' debug records (docs/COMPILER_PATTERNS.md).
static void test_dotnet_hashtable_order() {
    auto joined = [](std::vector<std::string> keys) {
        std::string out;
        for (const std::string& k : papyrus::dotnet::hashtable20_key_order(keys)) out += (out.empty() ? "" : " ") + k;
        return out;
    };
    CHECK(joined({"Position01", "Position02", "Position03", "Position04", "Position05", "Position06", "Position07", "Position08"}) ==
          "position07 position05 position06 position01 position02 position08 position03 position04");
    CHECK(joined({"busy", "ready", "fired"}) == "busy fired ready");
    CHECK(joined({"empty", "loadedClosed", "loadedOpened", "loadedInscribed", "emptyDone", "busy"}) ==
          "loadedclosed emptydone loadedopened loadedinscribed busy empty");
    CHECK(joined({"waitingForActor", "hasBeenTriggered"}) == "waitingforactor hasbeentriggered");
    CHECK(papyrus::dotnet::string_hash_64("a") == 372029373);
    CHECK(papyrus::dotnet::string_hash_64("abcd") == -1664220650);
    CHECK(papyrus::dotnet::string_hash_64("waitingforactor") == 847524731);
}

static void test_elseif_rule() {
    Script s;
    std::string err;
    CHECK_STATUS(assemble(kElseIfListing, s, &err), Status::Ok);
    const DecompileResult r = decompile(s);
    CHECK(r.all_ok());
    const std::string& src = r.source;
    const size_t chain = src.find("Function Chain(");
    const size_t nested = src.find("Function Nested(");
    CHECK(chain != std::string::npos && nested != std::string::npos && chain < nested);
    if (chain == std::string::npos || nested == std::string::npos) return;
    const std::string chainText = src.substr(chain, nested - chain);
    const std::string nestedText = src.substr(nested);
    // Chain: the ElseIf temps were reused, so the chain comes back flat.
    CHECK(chainText.find("    ElseIf a == c\n") != std::string::npos);
    CHECK(chainText.find("    ElseIf b == c\n") != std::string::npos);
    CHECK(chainText.find("        If a == c\n") == std::string::npos);
    // Nested: each inner If allocated a fresh temp while its outer's was
    // held, so the else parts stay nested.
    CHECK(nestedText.find("    Else\n        If a == c\n") != std::string::npos);
    CHECK(nestedText.find("        Else\n            If b == c\n") != std::string::npos);
    CHECK(nestedText.find("ElseIf") == std::string::npos);
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
    Script s;
    if (read(a.v.data(), a.v.size(), s) == Status::Ok) {
        const std::string listing = to_asm(s);
        std::ofstream(dir / "sample.pas", std::ios::binary).write(listing.data(), static_cast<std::streamsize>(listing.size()));
    }
}

// --- audit --------------------------------------------------------------------
// Each rule on a hand-written Papyrus source compiled with our own compiler
// against stub declarations, so the test reads like the author's script.

namespace {
struct MapResolver : papyrus::psc::SourceResolver {
    std::map<std::string, std::string> sources;  // lowercased name -> text
    bool find(std::string_view name, papyrus::psc::SourceFile& out) override {
        std::string key(name);
        for (char& ch : key) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        auto it = sources.find(key);
        if (it == sources.end()) return false;
        out.name = std::string(name);
        out.path = out.name + ".psc";
        out.text = it->second;
        return true;
    }
};

bool compile_sample(MapResolver& r, const std::string& name, Script& out) {
    papyrus::psc::SourceFile src;
    CHECK(r.find(name, src));
    papyrus::psc::CompileOptions opt;
    opt.resolver = &r;
    std::vector<papyrus::psc::Diagnostic> diags;
    const bool ok = papyrus::psc::compile(src, opt, out, diags);
    if (!ok) {
        for (const papyrus::psc::Diagnostic& d : diags) std::fprintf(stderr, "  %s\n", d.text().c_str());
    }
    return ok;
}
}  // namespace

static void test_audit() {
    MapResolver r;
    r.sources["scriptobject"] = R"(Scriptname ScriptObject
Function RegisterForUpdate(float afInterval) native
Function RegisterForSingleUpdate(float afInterval) native
Function UnregisterForUpdate() native
Function RegisterForUpdateGameTime(float afInterval) native
Function UnregisterForUpdateGameTime() native
Function GotoState(string asNewState) native
string Function GetState() native
Event OnUpdate()
EndEvent
Event OnInit()
EndEvent
)";
    r.sources["form"] = "Scriptname Form extends ScriptObject\n";
    r.sources["objectreference"] = R"(Scriptname ObjectReference extends Form
Location Function GetCurrentLocation() native
Event OnActivate(ObjectReference akActionRef)
EndEvent
)";
    r.sources["location"] = "Scriptname Location extends Form\nbool Function IsCleared() native\n";
    r.sources["quest"] = "Scriptname Quest extends Form\nbool Function Start() native\nFunction Stop() native\n";
    r.sources["formlist"] = "Scriptname FormList extends Form\nFunction AddForm(Form apForm) native\n";
    r.sources["utility"] = "Scriptname Utility\nFunction Wait(float afSeconds) native global\n";
    r.sources["game"] = "Scriptname Game\nForm Function GetFormFromFile(int aiFormID, string asFilename) native global\nint Function GetModByName(string asName) native global\n";
    r.sources["auditsample"] = R"(Scriptname AuditSample extends ObjectReference

Int[] neverCreated
Form[] created
Bool done
Int counter

Function LoopNoExit(Int n)
    While n > 0
        counter += 1
    EndWhile
EndFunction

Function LoopCounted(Int n)
    While n > 0
        n -= 1
    EndWhile
EndFunction

Function LoopPolls()
    While !done
    EndWhile
EndFunction

Function LoopPollsWithWait()
    While !done
        Utility.Wait(1.0)
    EndWhile
EndFunction

Function Forever()
    While True
        Utility.Wait(1.0)
    EndWhile
EndFunction

Event OnInit()
    RegisterForUpdate(0.05)
EndEvent

Event OnUpdate()
    Utility.Wait(0.5)
    RegisterForSingleUpdate(1.0)
EndEvent

Function NoneDeref()
    Quest q = Game.GetFormFromFile(0x800, "Other.esp") as Quest
    q.Start()
EndFunction

Function NoneDerefChecked()
    Quest q = Game.GetFormFromFile(0x800, "Other.esp") as Quest
    If q
        q.Start()
    EndIf
EndFunction

Function NoneDerefModChecked()
    If Game.GetModByName("Other.esp") != 255
        Quest q = Game.GetFormFromFile(0x800, "Other.esp") as Quest
        q.Start()
    EndIf
EndFunction

Function NoneArg(FormList list)
    list.AddForm(Game.GetFormFromFile(0x801, "Other.esp"))
EndFunction

Function NoneLocation()
    Location here = GetCurrentLocation()
    done = here.IsCleared()
EndFunction

Function Recurse(Int n)
    Recurse(n + 1)
EndFunction

Function RecurseGuarded(Int n)
    If n <= 0
        Return
    EndIf
    RecurseGuarded(n - 1)
EndFunction

Function OffByOne(Int[] a)
    Int i = 0
    While i <= a.Length
        counter += a[i]
        i += 1
    EndWhile
EndFunction

Function ArrayNoneLocal()
    Form[] items
    If items.Length > 0
        done = True
    EndIf
EndFunction

Function ArrayNoneVar()
    counter = neverCreated[0]
    created = new Form[2]
    counter += created.Length
EndFunction
)";
    r.sources["auditstates"] = R"(Scriptname AuditStates extends ObjectReference

Event OnActivate(ObjectReference akActionRef)
    GotoState("Busy")
    OnActivate(akActionRef)
    GotoState("Bussy")
    GotoState("None")
    If GetState() == "Flag"
        GotoState("Flag")
    EndIf
EndEvent

State Busy
    Event OnActivate(ObjectReference akActionRef)
        GotoState("")
    EndEvent
EndState

Event OnUpdate()
    RegisterForSingleUpdate(1.0)
EndEvent
)";

    struct Resolver : ScriptResolver {
        std::map<std::string, const Script*> scripts;
        const Script* find(std::string_view name) override {
            std::string key(name);
            for (char& ch : key) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            auto it = scripts.find(key);
            return it == scripts.end() ? nullptr : it->second;
        }
    } resolver;
    Script sample, states, objectReference, form, scriptObject;
    CHECK(compile_sample(r, "AuditSample", sample));
    CHECK(compile_sample(r, "AuditStates", states));
    CHECK(compile_sample(r, "ObjectReference", objectReference));
    CHECK(compile_sample(r, "Form", form));
    CHECK(compile_sample(r, "ScriptObject", scriptObject));
    resolver.scripts["objectreference"] = &objectReference;
    resolver.scripts["form"] = &form;
    resolver.scripts["scriptobject"] = &scriptObject;
    AuditOptions opt;
    opt.resolver = &resolver;

    auto has = [](const AuditResult& res, const char* fn, const char* rule) {
        for (const Finding& f : res.findings) {
            if (f.function == fn && f.rule == rule) return true;
        }
        return false;
    };
    auto count = [](const AuditResult& res, const char* fn) {
        size_t n = 0;
        for (const Finding& f : res.findings) n += f.function == fn;
        return n;
    };

    const AuditResult a = audit(sample, opt);
    CHECK(a.skipped == 0);
    CHECK(has(a, "LoopNoExit", "loop-no-exit"));
    CHECK(count(a, "LoopCounted") == 0);
    CHECK(has(a, "LoopPolls", "loop-polls-without-wait"));
    CHECK(count(a, "LoopPollsWithWait") == 0);
    CHECK(count(a, "Forever") == 0);
    CHECK(has(a, "OnInit", "update-interval-too-short"));
    CHECK(has(a, "OnInit", "update-never-unregistered"));
    CHECK(has(a, "OnUpdate", "wait-in-onupdate"));
    CHECK(has(a, "OnUpdate", "onupdate-reregisters-unconditionally"));
    CHECK(has(a, "NoneDeref", "none-deref-unchecked"));
    CHECK(count(a, "NoneDerefChecked") == 0);
    CHECK(count(a, "NoneDerefModChecked") == 0);
    CHECK(has(a, "NoneArg", "none-deref-unchecked"));
    CHECK(has(a, "NoneLocation", "none-deref-unchecked"));
    CHECK(has(a, "Recurse", "recursion-unconditional"));
    CHECK(has(a, "RecurseGuarded", "recursion"));
    CHECK(!has(a, "RecurseGuarded", "recursion-unconditional"));
    CHECK(has(a, "OffByOne", "array-index-off-by-one"));
    CHECK(has(a, "ArrayNoneLocal", "array-none"));
    CHECK(has(a, "ArrayNoneVar", "array-none"));
    CHECK(count(a, "ArrayNoneVar") == 1);
    // Our compiler writes debug info: every finding has a source line.
    for (const Finding& f : a.findings) CHECK(f.line > 0);

    const AuditResult b = audit(states, opt);
    CHECK(b.skipped == 0);
    CHECK(has(b, "OnActivate", "gotostate-unknown"));
    CHECK(!has(b, "OnActivate", "recursion"));
    CHECK(!has(b, "OnActivate", "recursion-unconditional"));
    size_t unknownStates = 0;
    for (const Finding& f : b.findings) unknownStates += f.rule == "gotostate-unknown";
    CHECK(unknownStates == 1);  // "Bussy" only: "None" and the GetState flag "Flag" are not reported
    CHECK(count(b, "OnUpdate") == 0);  // the script has GotoState, so the re-arm has an exit
    CHECK(audit_rules().size() == 12);
}

int main(int argc, char** argv) {
    if (argc == 3 && std::strcmp(argv[1], "--seeds") == 0) {
        write_seeds(argv[2]);
        return 0;
    }
    test_read_sample();
    test_truncations();
    test_damage();
    test_asm_and_canon();
    test_decompile();
    test_refs_and_patch();
    test_assemble();
    test_elseif_rule();
    test_dotnet_hashtable_order();
    test_audit();
    if (g_failures) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::puts("all tests passed");
    return 0;
}
