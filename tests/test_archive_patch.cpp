// Unit test for in-archive FormID patching: a compiled script in a synthetic
// v105 BSA is patched in place, checked through the reader and undone, and the
// cases that must be refused are refused with the archive untouched.
// SPDX-License-Identifier: GPL-3.0-only
#include "archive_patch.hpp"
#include "out_patch.hpp"

#include "bsa/reader.hpp"
#include "papyrus/analysis.hpp"
#include "papyrus/compile.hpp"
#include "papyrus/pex.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

#ifdef _WIN32
#include <share.h>
#endif

using namespace pextool;
namespace pex = papyrus::pex;
namespace psc = papyrus::psc;
namespace fs = std::filesystem;

static int g_failures = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            ++g_failures;                                                        \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                        \
    } while (0)

using Bytes = std::vector<uint8_t>;

// --- the archive builder (uncompressed blocks, or an LZ4 frame of stored blocks) ---

struct TFile {
    std::string folder, name;
    Bytes data;
    bool compress = false;
    int shareWith = -1;  // the record points at another file's block
};

static void put32(Bytes& v, uint32_t x) {
    for (int i = 0; i < 4; ++i) v.push_back(static_cast<uint8_t>(x >> (8 * i)));
}
static void put64(Bytes& v, uint64_t x) {
    put32(v, static_cast<uint32_t>(x));
    put32(v, static_cast<uint32_t>(x >> 32));
}

static Bytes fake_lz4(const Bytes& d) {
    Bytes s = {0x04, 0x22, 0x4D, 0x18, 0x60, 0x40, 0x00};
    for (size_t pos = 0; pos < d.size();) {
        const size_t n = std::min<size_t>(65536, d.size() - pos);
        put32(s, 0x80000000u | static_cast<uint32_t>(n));
        s.insert(s.end(), d.begin() + static_cast<std::ptrdiff_t>(pos), d.begin() + static_cast<std::ptrdiff_t>(pos + n));
        pos += n;
    }
    put32(s, 0);
    return s;
}

static Bytes build_bsa(uint32_t flags, const std::vector<TFile>& files) {
    struct Folder {
        std::string name;
        uint64_t hash;
        std::vector<size_t> idx;
    };
    std::vector<Folder> folders;
    for (size_t i = 0; i < files.size(); ++i) {
        auto it = std::find_if(folders.begin(), folders.end(), [&](const Folder& f) { return f.name == files[i].folder; });
        if (it == folders.end()) {
            folders.push_back({files[i].folder, bsa::hash_folder(files[i].folder, 105), {}});
            it = folders.end() - 1;
        }
        it->idx.push_back(i);
    }
    auto fileHash = [&](size_t i) { return bsa::hash_file(files[i].name, 105); };
    std::sort(folders.begin(), folders.end(), [](const Folder& a, const Folder& b) { return a.hash < b.hash; });
    for (auto& f : folders) std::sort(f.idx.begin(), f.idx.end(), [&](size_t a, size_t b) { return fileHash(a) < fileHash(b); });

    const bool embed = (flags & 0x100) != 0;
    uint32_t folderNamesLength = 0, fileNamesLength = 0;
    for (const auto& f : folders) folderNamesLength += static_cast<uint32_t>(f.name.size() + 1);
    for (const auto& f : files) fileNamesLength += static_cast<uint32_t>(f.name.size() + 1);

    std::vector<Bytes> blocks(files.size());
    for (size_t i = 0; i < files.size(); ++i) {
        if (files[i].shareWith >= 0) continue;
        Bytes& b = blocks[i];
        if (embed) {
            const std::string full = files[i].folder + "\\" + files[i].name;
            b.push_back(static_cast<uint8_t>(full.size()));
            b.insert(b.end(), full.begin(), full.end());
        }
        if (files[i].compress) {
            put32(b, static_cast<uint32_t>(files[i].data.size()));
            const Bytes s = fake_lz4(files[i].data);
            b.insert(b.end(), s.begin(), s.end());
        } else {
            b.insert(b.end(), files[i].data.begin(), files[i].data.end());
        }
    }

    constexpr size_t kRec = 24;
    size_t tables = 36 + folders.size() * kRec;
    for (const auto& f : folders) tables += 2 + f.name.size() + 16 * f.idx.size();
    const size_t dataStart = tables + fileNamesLength;

    std::vector<size_t> offsets(files.size());
    size_t dataPos = dataStart;
    for (const auto& f : folders) {
        for (size_t i : f.idx) {
            offsets[i] = dataPos;
            dataPos += blocks[i].size();
        }
    }
    for (size_t i = 0; i < files.size(); ++i) {
        if (files[i].shareWith >= 0) offsets[i] = offsets[static_cast<size_t>(files[i].shareWith)];
    }

    Bytes out = {'B', 'S', 'A', 0};
    put32(out, 105);
    put32(out, 36);
    put32(out, flags);
    put32(out, static_cast<uint32_t>(folders.size()));
    put32(out, static_cast<uint32_t>(files.size()));
    put32(out, folderNamesLength);
    put32(out, fileNamesLength);
    put32(out, 1);
    size_t block = 36 + folders.size() * kRec;
    for (const auto& f : folders) {
        put64(out, f.hash);
        put32(out, static_cast<uint32_t>(f.idx.size()));
        put32(out, 0);
        put32(out, static_cast<uint32_t>(block + fileNamesLength));
        put32(out, 0);
        block += 2 + f.name.size() + 16 * f.idx.size();
    }
    for (const auto& f : folders) {
        out.push_back(static_cast<uint8_t>(f.name.size() + 1));
        out.insert(out.end(), f.name.begin(), f.name.end());
        out.push_back(0);
        for (size_t i : f.idx) {
            put64(out, fileHash(i));
            const size_t src = files[i].shareWith >= 0 ? static_cast<size_t>(files[i].shareWith) : i;
            uint32_t size = static_cast<uint32_t>(blocks[src].size());
            if (files[i].compress != ((flags & 4) != 0)) size |= 0x40000000u;
            put32(out, size);
            put32(out, static_cast<uint32_t>(offsets[i]));
        }
    }
    for (const auto& f : folders) {
        for (size_t i : f.idx) {
            out.insert(out.end(), files[i].name.begin(), files[i].name.end());
            out.push_back(0);
        }
    }
    for (const auto& f : folders) {
        for (size_t i : f.idx) out.insert(out.end(), blocks[i].begin(), blocks[i].end());
    }
    return out;
}

// --- files ----------------------------------------------------------------------

static Bytes slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return Bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}
static void spit(const fs::path& p, const Bytes& b) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
}
static std::string slurp_text(const fs::path& p) {
    const Bytes b = slurp(p);
    return std::string(b.begin(), b.end());
}
static bool contains(const std::string& s, const char* what) { return s.find(what) != std::string::npos; }
static size_t count_of(const std::string& s, const char* what) {
    size_t n = 0;
    for (size_t p = s.find(what); p != std::string::npos; p = s.find(what, p + 1)) ++n;
    return n;
}

// --- the scripts ---------------------------------------------------------------

struct MapResolver : psc::SourceResolver {
    std::map<std::string, std::string> sources;
    bool find(std::string_view name, psc::SourceFile& out) override {
        std::string key(name);
        for (char& ch : key) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        const auto it = sources.find(key);
        if (it == sources.end()) return false;
        out.name = std::string(name);
        out.path = out.name + ".psc";
        out.text = it->second;
        return true;
    }
};

// A literal, a literal propagated through a local, a script variable's initial
// value, a call whose FormID is dynamic, and a second plugin left alone.
constexpr const char* kPatchMe = R"(Scriptname PatchMe

Int myId = 0x900

Form Function Fetch(Int n)
    Form a = Game.GetFormFromFile(0x801, "Foo.esp")
    Int id = 0x802
    Form b = Game.GetFormFromFile(id, "Foo.esp")
    Form c = Game.GetFormFromFile(myId, "Foo.esp")
    Form d = Game.GetFormFromFile(0x803, "Other.esm")
    Return Game.GetFormFromFile(n, "Foo.esp")
EndFunction
)";

static Bytes compile_script(const char* name, const char* text) {
    MapResolver r;
    r.sources["game"] = "Scriptname Game\nForm Function GetFormFromFile(Int aiFormID, String asFilename) Global Native\n";
    r.sources["form"] = "Scriptname Form\n";
    psc::SourceFile src;
    src.name = name;
    src.path = std::string(name) + ".psc";
    src.text = text;
    psc::CompileOptions opt;
    opt.resolver = &r;
    opt.compileTime = 1;
    pex::Script s;
    std::vector<psc::Diagnostic> diags;
    if (!psc::compile(src, opt, s, diags)) {
        for (const psc::Diagnostic& d : diags) std::fprintf(stderr, "  %s\n", d.text().c_str());
        CHECK(false);
    }
    Bytes out;
    CHECK(pex::write(s, out) == pex::Status::Ok);
    return out;
}

// Bytes of a member through the reader's hash lookup.
static bool read_member(const fs::path& archive, const std::string& path, Bytes& out) {
    bsa::Reader r;
    if (r.open(archive) != bsa::Status::Ok) return false;
    bsa::Entry e;
    return r.find(path, e) == bsa::Status::Ok && r.read(e, out) == bsa::Status::Ok;
}

static pex::FormIdMap load_map(const char* text) {
    pex::FormIdMap map;
    std::string err;
    CHECK(pex::load_formid_map(text, map, &err));
    return map;
}

constexpr const char* kEntry = "scripts\\patchme.pex";

// The plan for one member, as `pex patch` makes it.
static bool plan(const Bytes& bytes, const pex::FormIdMap& map, MemberPatch& out, std::string& why) {
    pex::Script s;
    CHECK(pex::read(bytes.data(), bytes.size(), s) == pex::Status::Ok);
    CHECK(pex::rewrite_check(bytes.data(), bytes.size()) == pex::Status::Ok);
    const pex::PatchResult r = pex::apply_formid_map(s, map);
    return make_member_patch(std::string("A.bsa:") + kEntry, kEntry, fs::path(), bytes, s, r, out, why);
}

static bool plan_loose(const fs::path& file, const Bytes& bytes, const pex::FormIdMap& map, MemberPatch& out, std::string& why) {
    pex::Script s;
    CHECK(pex::read(bytes.data(), bytes.size(), s) == pex::Status::Ok);
    const pex::PatchResult r = pex::apply_formid_map(s, map);
    return make_member_patch(file.u8string(), std::string(), file, bytes, s, r, out, why);
}

static int32_t ref_with_ip_order(const pex::Script& s, size_t index) {
    const std::vector<pex::FormRef> refs = pex::find_form_refs(s);
    return index < refs.size() ? refs[index].formId : -1;
}

// One byte different, two bytes different, three bytes different.
constexpr const char* kMap = "!PLUGIN=Foo.esp\n00000801|00000805\n00000802|00010A02\n00000900|00FFFFFF\n";

// --- patch, read back, undo ----------------------------------------------------

static void test_patch_and_undo(const fs::path& dir, uint32_t flags, const char* fileName) {
    const Bytes script = compile_script("PatchMe", kPatchMe);
    const Bytes other = compile_script("Other", "Scriptname Other\nFunction F()\nEndFunction\n");
    const fs::path archive = dir / fileName;
    const Bytes original = build_bsa(flags, {{"scripts", "patchme.pex", script}, {"scripts", "other.pex", other}});
    spit(archive, original);

    MemberPatch m;
    std::string why;
    CHECK(plan(script, load_map(kMap), m, why));
    CHECK(m.ranges.size() == 3);
    CHECK(m.edits.size() == 3);

    // A dry run says where and writes nothing.
    ArchivePatchOptions dry;
    dry.dryRun = true;
    patch_archive_members(archive, {&m}, dry);
    CHECK(!m.failed);
    CHECK(slurp(archive) == original);
    CHECK(!fs::exists(journal_path(archive)));
    CHECK(count_of(m.report, "DRYRUN|") == 4);
    size_t dataAt = 0;
    {
        // The offsets in the report are real: the old bytes sit there.
        const size_t at = m.report.find("|@");
        CHECK(at != std::string::npos);
        dataAt = static_cast<size_t>(std::strtoull(m.report.c_str() + at + 2, nullptr, 10));
        CHECK(dataAt + 4 <= original.size());
        CHECK(std::memcmp(original.data() + dataAt, m.ranges[0].oldBytes.data(), 4) == 0);
    }

    // The real run: only the operand bytes change in the whole file.
    m.report.clear();
    ArchivePatchOptions real;
    real.mapPath = "the map.txt";
    real.backup = true;
    patch_archive_members(archive, {&m}, real);
    CHECK(!m.failed);
    CHECK(contains(m.report, "PATCHED|"));
    CHECK(contains(m.report, "in archive, 12 bytes written"));
    CHECK(contains(m.report, "JOURNAL|"));
    CHECK(contains(m.report, "--backup copies no archive"));
    const Bytes patched = slurp(archive);
    CHECK(patched.size() == original.size());
    size_t differing = 0;
    for (size_t i = 0; i < original.size(); ++i) differing += original[i] != patched[i];
    CHECK(differing == 1 + 2 + 3);

    Bytes back;
    CHECK(read_member(archive, kEntry, back));
    CHECK(back == m.newBytes);
    pex::Script s;
    CHECK(pex::read(back.data(), back.size(), s) == pex::Status::Ok);
    CHECK(pex::rewrite_check(back.data(), back.size()) == pex::Status::Ok);
    CHECK(ref_with_ip_order(s, 0) == 0x805);
    CHECK(ref_with_ip_order(s, 1) == 0x10A02);
    CHECK(ref_with_ip_order(s, 2) == 0xFFFFFF);
    CHECK(ref_with_ip_order(s, 3) == 0x803);  // the other plugin's literal
    Bytes otherBack;
    CHECK(read_member(archive, "scripts\\other.pex", otherBack));
    CHECK(otherBack == other);

    // The journal has one line per range with what undo needs.
    const std::string journal = slurp_text(journal_path(archive));
    CHECK(count_of(journal, "RANGE|") == 4);  // three, and the header's description
    CHECK(contains(journal, "|the map.txt|"));
    CHECK(contains(journal, kEntry));
    CHECK(contains(journal, "|00000801|00000805|"));

    // A dry undo says what it would restore and writes nothing.
    std::string report;
    CHECK(undo_journal(journal_path(archive), true, report) == 0);
    CHECK(count_of(report, "DRYRUN|") == 4);
    CHECK(slurp(archive) == patched);

    report.clear();
    CHECK(undo_journal(journal_path(archive), false, report) == 0);
    CHECK(contains(report, "UNDONE|"));
    CHECK(contains(report, "12 bytes restored"));
    CHECK(slurp(archive) == original);

    // Undoing again finds nothing to do.
    report.clear();
    CHECK(undo_journal(journal_path(archive), false, report) == 0);
    CHECK(contains(report, "nothing to undo"));
    CHECK(slurp(archive) == original);
}

// Two patches on the same position, then the states a stopped undo can leave.
static void test_layers(const fs::path& dir) {
    const Bytes script = compile_script("PatchMe", kPatchMe);
    const fs::path archive = dir / "layers.bsa";
    const Bytes original = build_bsa(0x3, {{"scripts", "patchme.pex", script}});
    spit(archive, original);

    MemberPatch first, second;
    std::string why;
    CHECK(plan(script, load_map("!PLUGIN=Foo.esp\n00000801|00000A01\n"), first, why));
    patch_archive_members(archive, {&first}, ArchivePatchOptions{});
    CHECK(!first.failed);
    const Bytes layerOne = slurp(archive);
    Bytes member;
    CHECK(read_member(archive, kEntry, member));
    CHECK(plan(member, load_map("!PLUGIN=Foo.esp\n00000A01|00000B01\n00000802|00000B02\n"), second, why));
    patch_archive_members(archive, {&second}, ArchivePatchOptions{});
    CHECK(!second.failed);
    CHECK(slurp(archive) != layerOne);

    // As if an undo stopped with one position at the first patch's value.
    Bytes half = slurp(archive);
    const auto it = std::search(half.begin(), half.end(), second.ranges[0].newBytes.begin(), second.ranges[0].newBytes.end());
    CHECK(it != half.end());
    std::copy(second.ranges[0].oldBytes.begin(), second.ranges[0].oldBytes.end(), it);
    spit(archive, half);

    std::string report;
    CHECK(undo_journal(journal_path(archive), false, report) == 0);
    CHECK(slurp(archive) == original);

    // A later patch on the same journal stands alone: undo takes back only it.
    MemberPatch third;
    CHECK(plan(script, load_map("!PLUGIN=Foo.esp\n00000801|00000C01\n"), third, why));
    patch_archive_members(archive, {&third}, ArchivePatchOptions{});
    CHECK(!third.failed);
    report.clear();
    CHECK(undo_journal(journal_path(archive), false, report) == 0);
    CHECK(contains(report, "1 ranges, 4 bytes restored"));
    CHECK(slurp(archive) == original);
}

static void test_undo_refuses(const fs::path& dir) {
    const Bytes script = compile_script("PatchMe", kPatchMe);
    const fs::path archive = dir / "changed.bsa";
    spit(archive, build_bsa(0x3, {{"scripts", "patchme.pex", script}}));
    MemberPatch m;
    std::string why;
    CHECK(plan(script, load_map(kMap), m, why));
    patch_archive_members(archive, {&m}, ArchivePatchOptions{});
    CHECK(!m.failed);

    // Something else wrote over a patched operand: nothing is restored.
    Bytes changed = slurp(archive);
    const auto it = std::search(changed.begin(), changed.end(), m.ranges[0].newBytes.begin(), m.ranges[0].newBytes.end());
    CHECK(it != changed.end());
    *(it + 3) ^= 0x55;
    spit(archive, changed);
    std::string report;
    CHECK(undo_journal(journal_path(archive), false, report) == 1);
    CHECK(contains(report, "a value the journal never gave it"));
    CHECK(contains(report, "nothing restored"));
    CHECK(slurp(archive) == changed);

    // A damaged journal line refuses; a torn last line is ignored and repaired.
    const fs::path journal = journal_path(archive);
    const std::string text = slurp_text(journal);
    const fs::path bad = dir / "bad.journal";
    {
        std::ofstream f(bad, std::ios::binary);
        f << text << "garbage line\n";
    }
    report.clear();
    CHECK(undo_journal(bad, false, report) == 1);
    CHECK(contains(report, "cannot be read"));

    const fs::path torn = dir / "torn.bsa";
    const Bytes tornOriginal = build_bsa(0x3, {{"scripts", "patchme.pex", script}});
    spit(torn, tornOriginal);
    MemberPatch t;
    CHECK(plan(script, load_map(kMap), t, why));
    patch_archive_members(torn, {&t}, ArchivePatchOptions{});
    CHECK(!t.failed);
    {
        std::ofstream f(journal_path(torn), std::ios::binary | std::ios::app);
        f << "RANGE|99|cut off";
    }
    report.clear();
    CHECK(undo_journal(journal_path(torn), false, report) == 0);
    CHECK(contains(report, "NOTE|"));
    CHECK(slurp(torn) == tornOriginal);
    CHECK(!contains(slurp_text(journal_path(torn)), "cut off"));
}

// --- refusals -----------------------------------------------------------------

static void test_refusals(const fs::path& dir) {
    const Bytes script = compile_script("PatchMe", kPatchMe);
    std::string why;

    // A rename changes the length.
    {
        MemberPatch m;
        CHECK(!plan(script, load_map("!PLUGIN=Foo.esp\n!NEWPLUGIN=LongerName.esp\n00000801|00000A01\n"), m, why));
        CHECK(contains(why, "renames the plugin"));
        CHECK(contains(why, kLooseFallback));
    }

    auto refused = [&](const char* name, uint32_t flags, std::vector<TFile> files, const char* reason) {
        const fs::path archive = dir / name;
        const Bytes original = build_bsa(flags, files);
        spit(archive, original);
        MemberPatch m;
        std::string planWhy;
        CHECK(plan(script, load_map(kMap), m, planWhy));
        patch_archive_members(archive, {&m}, ArchivePatchOptions{});
        CHECK(m.failed);
        CHECK(contains(m.report, "ERR|"));
        CHECK(contains(m.report, reason));
        CHECK(contains(m.report, kLooseFallback));
        CHECK(!contains(m.report, "PATCHED|"));
        CHECK(slurp(archive) == original);
        CHECK(!fs::exists(journal_path(archive)));
    };
    // The archive stores files plain, this member toggles itself compressed.
    refused("toggle.bsa", 0x3, {{"scripts", "patchme.pex", script, true}}, "per-file toggle");
    // The archive's default is compressed and the member does not toggle it off.
    refused("default.bsa", 0x7, {{"scripts", "patchme.pex", script, true}}, "archive's default");
    // Two records on one block: an overwrite would change the other file.
    refused("shared.bsa", 0x3, {{"scripts", "patchme.pex", script}, {"scripts", "twin.pex", script, false, 0}}, "shared with");

    // Compressed by default, but this member toggles it off: that one is fine.
    {
        const fs::path archive = dir / "toggled_off.bsa";
        const Bytes original = build_bsa(0x7, {{"scripts", "patchme.pex", script, false}});
        spit(archive, original);
        MemberPatch m;
        CHECK(plan(script, load_map(kMap), m, why));
        patch_archive_members(archive, {&m}, ArchivePatchOptions{});
        CHECK(!m.failed);
        Bytes back;
        CHECK(read_member(archive, kEntry, back));
        CHECK(back == m.newBytes);
    }

    // The same member twice: patched once, the second refused.
    {
        const fs::path archive = dir / "twice.bsa";
        spit(archive, build_bsa(0x3, {{"scripts", "patchme.pex", script}}));
        MemberPatch a, b;
        CHECK(plan(script, load_map(kMap), a, why));
        CHECK(plan(script, load_map(kMap), b, why));
        patch_archive_members(archive, {&a, &b}, ArchivePatchOptions{});
        CHECK(!a.failed);
        CHECK(b.failed && contains(b.report, "given twice"));
    }

    // An archive that changed since the member was read: no journal, no write.
    {
        const fs::path archive = dir / "stale.bsa";
        const Bytes original = build_bsa(0x3, {{"scripts", "patchme.pex", script}});
        spit(archive, original);
        MemberPatch m;
        CHECK(plan(compile_script("PatchMe", kPatchMe), load_map(kMap), m, why));
        m.oldBytes[m.ranges[0].offset] ^= 0x10;
        patch_archive_members(archive, {&m}, ArchivePatchOptions{});
        CHECK(m.failed);
        CHECK(slurp(archive) == original);
    }

#ifdef _WIN32
    // Another program holds the archive without sharing writes: refused, untouched.
    {
        const fs::path archive = dir / "held.bsa";
        const Bytes original = build_bsa(0x3, {{"scripts", "patchme.pex", script}});
        spit(archive, original);
        std::FILE* holder = _wfsopen(archive.c_str(), L"rb", _SH_DENYWR);
        CHECK(holder != nullptr);
        MemberPatch m;
        CHECK(plan(script, load_map(kMap), m, why));
        patch_archive_members(archive, {&m}, ArchivePatchOptions{});
        if (holder) std::fclose(holder);
        CHECK(m.failed);
        CHECK(contains(m.report, "cannot open for writing"));
        CHECK(contains(m.report, "another program holds it open"));
        CHECK(slurp(archive) == original);
        CHECK(!fs::exists(journal_path(archive)));
    }
#endif

    // Bytes that differ anywhere else than the edited operands are refused.
    {
        pex::Script a, b;
        CHECK(pex::read(script.data(), script.size(), a) == pex::Status::Ok);
        b = a;
        const pex::PatchResult r = pex::apply_formid_map(b, load_map(kMap));
        Bytes written;
        CHECK(pex::write(b, written) == pex::Status::Ok);
        std::vector<OperandRange> ranges;
        CHECK(compute_operand_ranges(script, written, a, b, r.edits, ranges, why));
        CHECK(ranges.size() == 3);
        // The operands really are big-endian integers behind their type tag.
        for (const OperandRange& rg : ranges) CHECK(script[rg.offset - 1] == static_cast<uint8_t>(pex::ValueType::Integer));
        Bytes foreign = written;
        foreign[20] ^= 1;
        CHECK(!compute_operand_ranges(script, foreign, a, b, r.edits, ranges, why));
        CHECK(contains(why, "differs outside"));
        // An edit that has no operand in the bytes.
        std::vector<pex::PatchEdit> extra = r.edits;
        extra.push_back(r.edits[0]);
        extra.back().newValue = 0x123456;
        CHECK(!compute_operand_ranges(script, written, a, b, extra, ranges, why));
        CHECK(contains(why, "no integer operand"));
        // The size differs.
        Bytes longer = written;
        longer.push_back(0);
        CHECK(!compute_operand_ranges(script, longer, a, b, r.edits, ranges, why));
        CHECK(contains(why, "size would change"));
    }
}

// --- the read-back fails ------------------------------------------------------

static void test_rollback(const fs::path& dir) {
    const Bytes script = compile_script("PatchMe", kPatchMe);
    const fs::path archive = dir / "rollback.bsa";
    const Bytes original = build_bsa(0x3, {{"scripts", "patchme.pex", script}});
    spit(archive, original);
    MemberPatch m;
    std::string why;
    CHECK(plan(script, load_map(kMap), m, why));

    // Something damages a patched operand between the write and the read-back.
    ArchivePatchOptions o;
    o.afterWriteForTest = [&](const fs::path& a) {
        Bytes b = slurp(a);
        const auto it = std::search(b.begin(), b.end(), m.ranges[0].newBytes.begin(), m.ranges[0].newBytes.end());
        CHECK(it != b.end());
        *(it + 3) ^= 0x01;
        spit(a, b);
    };
    patch_archive_members(archive, {&m}, o);
    CHECK(m.failed);
    CHECK(contains(m.report, "ERR|"));
    CHECK(contains(m.report, "rolled back from the journal"));
    CHECK(!contains(m.report, "PATCHED|"));
    CHECK(slurp(archive) == original);

    // The journal records the roll back: nothing is left to undo.
    std::string report;
    CHECK(undo_journal(journal_path(archive), false, report) == 0);
    CHECK(contains(report, "nothing to undo"));
    CHECK(slurp(archive) == original);
}

// --- loose files --------------------------------------------------------------

static void test_loose(const fs::path& dir) {
    const fs::path folder = dir / "loose";
    fs::create_directories(folder);
    const Bytes script = compile_script("PatchMe", kPatchMe);
    const fs::path a = folder / "a.pex", b = folder / "b.pex";
    spit(a, script);
    spit(b, script);
    const pex::FormIdMap map = load_map(kMap);
    MemberPatch ma, mb;
    std::string why;
    CHECK(plan_loose(a, script, map, ma, why));
    CHECK(plan_loose(b, script, map, mb, why));

    ArchivePatchOptions dry;
    dry.dryRun = true;
    patch_loose_members(folder, {&ma, &mb}, dry);
    CHECK(!ma.failed && contains(ma.report, "DRYRUN|"));
    CHECK(slurp(a) == script);
    CHECK(!fs::exists(loose_journal_path(folder)));

    ma.report.clear();
    mb.report.clear();
    patch_loose_members(folder, {&ma, &mb}, ArchivePatchOptions{});
    CHECK(!ma.failed && !mb.failed);
    CHECK(contains(ma.report, "PATCHED|") && contains(ma.report, "3 edits, 0 renames"));
    CHECK(contains(mb.report, "JOURNAL|"));  // once, with the folder's last file
    CHECK(slurp(a) == ma.newBytes);
    CHECK(slurp(b) == mb.newBytes);

    std::string report;
    CHECK(undo_journal(loose_journal_path(folder), false, report) == 0);
    CHECK(count_of(report, "UNDONE|") == 2);
    CHECK(slurp(a) == script);
    CHECK(slurp(b) == script);
    report.clear();
    CHECK(undo_journal(loose_journal_path(folder), false, report) == 0);
    CHECK(contains(report, "nothing to undo"));

    // A file that does not verify is rolled back and only that file.
    MemberPatch ra, rb;
    CHECK(plan_loose(a, script, map, ra, why));
    CHECK(plan_loose(b, script, map, rb, why));
    ArchivePatchOptions o;
    o.afterWriteForTest = [&](const fs::path& f) {
        if (f != b) return;
        Bytes bytes = slurp(f);
        bytes[rb.ranges[0].offset + 3] ^= 1;
        spit(f, bytes);
    };
    patch_loose_members(folder, {&ra, &rb}, o);
    CHECK(!ra.failed);
    CHECK(rb.failed && contains(rb.report, "rolled back from the journal"));
    CHECK(slurp(b) == script);
    CHECK(slurp(a) == ra.newBytes);
    report.clear();
    CHECK(undo_journal(loose_journal_path(folder), false, report) == 0);
    CHECK(slurp(a) == script);
}

// --- compressed members become loose files ----------------------------------------

static void test_extract(const fs::path& base) {
    const fs::path dir = base / "extract";
    fs::create_directories(dir);
    const Bytes script = compile_script("PatchMe", kPatchMe);
    const Bytes other = compile_script("Other", "Scriptname Other\nFunction F()\nEndFunction\n");
    const fs::path archive = dir / "c.bsa";
    const Bytes original = build_bsa(0x7, {{"scripts", "patchme.pex", script, true}, {"scripts", "other.pex", other, false}});
    spit(archive, original);
    MemberPatch m, o;
    std::string why;
    CHECK(plan(script, load_map(kMap), m, why));
    o.entryPath = "scripts\\other.pex";

    // The flags come from the archive; the toggled-off member is not compressed.
    std::vector<char> flags;
    CHECK(find_compressed_members(archive, {&m, &o}, flags));
    CHECK(flags.size() == 2 && flags[0] == 1 && flags[1] == 0);
    CHECK(!find_compressed_members(dir / "missing.bsa", {&m}, flags));

    // Scripts\ beside the archive, keeping any subfolder; nothing else qualifies.
    fs::path target;
    CHECK(extract_target(archive, kEntry, target));
    CHECK(target == dir / "Scripts" / "patchme.pex");
    CHECK(extract_target(archive, "SCRIPTS/sub\\x.pex", target));
    CHECK(target == dir / "Scripts" / "sub" / "x.pex");
    CHECK(!extract_target(archive, "meshes\\x.pex", target));
    CHECK(!extract_target(archive, "scripts\\..\\..\\x.pex", target));
    CHECK(!extract_target(archive, "scripts\\", target));

    // A dry run says where and creates nothing.
    CHECK(extract_target(archive, kEntry, m.extractTo));
    ArchivePatchOptions dry;
    dry.dryRun = true;
    extract_members(archive, {&m}, dry);
    CHECK(!m.failed && contains(m.report, "would extract to "));
    CHECK(!fs::exists(dir / "Scripts"));
    CHECK(!fs::exists(journal_path(archive)));

    // The real run: the folder and file appear, the archive does not change.
    m.report.clear();
    ArchivePatchOptions real;
    real.mapPath = "--formid";
    extract_members(archive, {&m}, real);
    CHECK(!m.failed);
    CHECK(contains(m.report, "extracted from A.bsa:"));
    CHECK(contains(m.report, "1 extracted"));
    CHECK(slurp(m.extractTo) == m.newBytes);
    CHECK(!fs::exists(fs::path(m.extractTo).concat(".pextmp")));
    CHECK(slurp(archive) == original);
    const std::string journal = slurp_text(journal_path(archive));
    CHECK(count_of(journal, "\nMKDIR|") == 1);
    CHECK(count_of(journal, "\nEXTRACTED|") == 1);

    // A dry undo deletes nothing; a real one deletes the file and its new folder.
    std::string report;
    CHECK(undo_journal(journal_path(archive), true, report) == 0);
    CHECK(contains(report, "would delete"));
    CHECK(fs::exists(m.extractTo));
    report.clear();
    CHECK(undo_journal(journal_path(archive), false, report) == 0);
    CHECK(contains(report, "extracted file deleted"));
    CHECK(!fs::exists(m.extractTo));
    CHECK(!fs::exists(dir / "Scripts"));
    CHECK(slurp(archive) == original);
    report.clear();
    CHECK(undo_journal(journal_path(archive), false, report) == 0);
    CHECK(contains(report, "nothing to undo"));

    // A folder that was already there stays, and so does a file changed since.
    fs::create_directories(dir / "Scripts");
    MemberPatch n;
    CHECK(plan(script, load_map(kMap), n, why));
    CHECK(extract_target(archive, kEntry, n.extractTo));
    extract_members(archive, {&n}, real);
    CHECK(!n.failed);
    const std::string afterSecond = slurp_text(journal_path(archive));
    CHECK(count_of(afterSecond, "\nMKDIR|") == 1);  // only the first run's
    Bytes edited = slurp(n.extractTo);
    edited[0] ^= 1;
    spit(n.extractTo, edited);
    report.clear();
    CHECK(undo_journal(journal_path(archive), false, report) == 1);
    CHECK(contains(report, "changed since it was extracted"));
    CHECK(slurp(n.extractTo) == edited);
    // Put right, the undo finishes the job and the existing folder stays.
    spit(n.extractTo, n.newBytes);
    report.clear();
    CHECK(undo_journal(journal_path(archive), false, report) == 0);
    CHECK(!fs::exists(n.extractTo));
    CHECK(fs::exists(dir / "Scripts"));

    // A file already at the target is never overwritten.
    const Bytes theirs = {1, 2, 3};
    spit(dir / "Scripts" / "patchme.pex", theirs);
    MemberPatch t;
    CHECK(plan(script, load_map(kMap), t, why));
    CHECK(extract_target(archive, kEntry, t.extractTo));
    extract_members(archive, {&t}, real);
    CHECK(t.failed && contains(t.report, "a file appeared"));
    CHECK(slurp(dir / "Scripts" / "patchme.pex") == theirs);
    fs::remove(dir / "Scripts" / "patchme.pex");

    // A file that does not verify is removed, and the journal says so.
    MemberPatch v;
    CHECK(plan(script, load_map(kMap), v, why));
    CHECK(extract_target(archive, kEntry, v.extractTo));
    ArchivePatchOptions bad;
    bad.afterWriteForTest = [&](const fs::path& f) {
        Bytes b = slurp(f);
        b[v.ranges[0].offset + 3] ^= 1;
        spit(f, b);
    };
    extract_members(archive, {&v}, bad);
    CHECK(v.failed && contains(v.report, "the extracted file was removed"));
    CHECK(!fs::exists(v.extractTo));
    report.clear();
    CHECK(undo_journal(journal_path(archive), false, report) == 0);
    CHECK(contains(report, "nothing to undo"));

    // Other archives beside it: a different copy conflicts, an identical one not.
    const fs::path folder = dir / "copies";
    fs::create_directories(folder);
    const fs::path a = folder / "a.bsa", same = folder / "same.bsa";
    spit(a, build_bsa(0x7, {{"scripts", "patchme.pex", script, true}}));
    spit(same, build_bsa(0x3, {{"scripts", "patchme.pex", script, false}}));
    MemberPatch ca;
    CHECK(plan(script, load_map(kMap), ca, why));
    std::vector<ExtractCandidate> list = {{a, &ca, std::string()}};
    find_other_copies(list);
    CHECK(list[0].conflict.empty());
    const Bytes changed = compile_script("PatchMe", "Scriptname PatchMe\nFunction F()\nEndFunction\n");
    spit(folder / "z_other.bsa", build_bsa(0x3, {{"scripts", "patchme.pex", changed, false}}));
    list = {{a, &ca, std::string()}};
    find_other_copies(list);
    CHECK(list[0].conflict == "z_other.bsa");
    CHECK(skip_reason_code("stored compressed, and there is a different copy in z.bsa: x") == "other-copy");
}

// --- the game must not be running ----------------------------------------------

static void test_game_check(const char* argv0) {
    const std::string self = fs::u8path(argv0).filename().u8string();
    std::vector<std::string> found;
    std::string why;
    // This test's own process is the one process known to be running; the name
    // matches in any case.
    std::string upper = self;
    for (char& c : upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    CHECK(find_running_processes({upper, "no-such-process.exe"}, found, why));
    CHECK(found.size() == 1 && contains(found[0], "(process "));
    CHECK(find_running_processes({"no-such-process.exe"}, found, why));
    CHECK(found.empty());
}

// --- patch --out: one winning copy, in a folder of its own ---------------------------

// A plugin file with only its header: enough for a load order. Test data, built
// here because no real plugin is small enough to commit.
static Bytes tiny_plugin(uint32_t flags) {
    Bytes b = {'T', 'E', 'S', '4'};
    put32(b, 0);
    put32(b, flags);
    put32(b, 0);
    put32(b, 0);
    put32(b, 0);
    return b;
}

static std::string lower_name(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

static std::string script_text(const char* name, int id) {
    return std::string("Scriptname ") + name + "\nForm Function Fetch()\n    Return Game.GetFormFromFile(" + std::to_string(id) + ", \"Foo.esp\")\nEndFunction\n";
}

static int32_t only_ref(const Bytes& bytes) {
    pex::Script s;
    CHECK(pex::read(bytes.data(), bytes.size(), s) == pex::Status::Ok);
    return ref_with_ip_order(s, 0);
}

static void test_out(const fs::path& base) {
    const fs::path root = base / "out";
    const fs::path data = root / "Data", iniDir = root / "ini", output = root / "mod" / "output";
    fs::create_directories(data);
    fs::create_directories(iniDir);

    // Load order: Esp1, then EspM, which is flagged a master though named .esp
    // and so loads first, then Esp2; Off is present and not active.
    spit(data / "Skyrim.esm", tiny_plugin(1));
    spit(data / "Esp1.esp", tiny_plugin(0));
    spit(data / "EspM.esp", tiny_plugin(1));
    spit(data / "Esp2.esp", tiny_plugin(0));
    spit(data / "Off.esp", tiny_plugin(0));
    spit(data / "Unlisted.esp", tiny_plugin(0));  // in Data, not in plugins.txt
    const fs::path plugins = root / "plugins.txt";
    {
        std::ofstream f(plugins, std::ios::binary);
        f << "*Esp1.esp\n*EspM.esp\n*Esp2.esp\nOff.esp\n";
    }
    {
        std::ofstream f(iniDir / "Skyrim.ini", std::ios::binary);
        f << "[Archive]\nsResourceArchiveList=Base.bsa\nsResourceArchiveList2=\n";
    }
    auto copy_of = [](const char* name, int id) { return compile_script(name, script_text(name, id).c_str()); };

    // In one archive that loads: from it, whatever its name.
    spit(data / "Base.bsa", build_bsa(0x3, {{"scripts", "onlybase.pex", copy_of("OnlyBase", 0x801)},
                                            {"scripts", "basevsmod.pex", copy_of("BaseVsMod", 0x802)},
                                            {"scripts\\source", "deep.pex", copy_of("Deep", 0x808)}}));
    // Esp1 loses to Esp2 in plugins.txt order. EspM is listed in between but is a
    // master, so it loads first of the three and loses too.
    spit(data / "Esp1.bsa", build_bsa(0x7, {{"scripts", "basevsmod.pex", copy_of("BaseVsMod", 0x803), true},
                                            {"scripts", "ordered.pex", copy_of("Ordered", 0x804), true}}));
    spit(data / "EspM.bsa", build_bsa(0x3, {{"scripts", "ordered.pex", copy_of("Ordered", 0x807)},
                                            {"scripts", "masterlast.pex", copy_of("MasterLast", 0x809)}}));
    spit(data / "Esp2 - Textures.bsa", build_bsa(0x7, {{"scripts", "ordered.pex", copy_of("Ordered", 0x805), true},
                                                       {"scripts", "masterlast.pex", copy_of("MasterLast", 0x80A), true}}));
    // A loose copy beats every archive.
    spit(data / "Esp2.bsa", build_bsa(0x3, {{"scripts", "looser.pex", copy_of("Looser", 0x806)}}));
    fs::create_directories(data / "Scripts");
    fs::create_directories(data / "Scripts" / "Source");
    spit(data / "Scripts" / "Looser.pex", copy_of("Looser", 0x80B));
    spit(data / "Scripts" / "Source" / "stale.pex", copy_of("Stale", 0x801));
    // An archive that does not load neither wins nor blocks.
    spit(data / "Off.bsa", build_bsa(0x3, {{"scripts", "onlybase.pex", copy_of("OnlyBase", 0x80C)},
                                           {"scripts", "onlyoff.pex", copy_of("OnlyOff", 0x80D)}}));
    // Nor does the archive of a plugin plugins.txt does not list, with
    // --all-plugins or without.
    spit(data / "Unlisted.bsa", build_bsa(0x3, {{"scripts", "ordered.pex", copy_of("Ordered", 0x80F)},
                                                {"scripts", "onlyunlisted.pex", copy_of("OnlyUnlisted", 0x810)}}));
    // A name no plugin or ini claims does not load either.
    spit(data / "Stray0.bsa", build_bsa(0x3, {{"scripts", "onlystray.pex", copy_of("OnlyStray", 0x80E)}}));

    ArchivePlan plan;
    std::string why;
    CHECK(plan_archives(data, plugins, {iniDir}, false, plan, why));
    std::map<std::string, int> rank;
    for (const ArchivePlan::Loaded& l : plan.loaded) rank[l.path.filename().u8string()] = l.rank;
    CHECK(plan.loaded.size() == 5);
    CHECK(rank["Base.bsa"] == 0);
    CHECK(rank["EspM.bsa"] < rank["Esp1.bsa"]);  // the ESM flag beats plugins.txt
    CHECK(rank["Esp1.bsa"] < rank["Esp2.bsa"]);
    CHECK(rank["Esp2.bsa"] < rank["Esp2 - Textures.bsa"]);
    CHECK(plan.notLoaded.size() == 3);

    std::vector<std::string> warnings;
    auto scripts = collect_script_copies(data, fs::path(), plan, warnings);
    auto winner = [&](const char* key) {
        const auto& copies = scripts[key];
        const int w = winning_copy(copies);
        return w < 0 ? std::string() : copies[static_cast<size_t>(w)].item.label;
    };
    CHECK(contains(winner("onlybase.pex"), "Base.bsa:"));     // beats the unloaded Off.bsa
    CHECK(contains(winner("basevsmod.pex"), "Esp1.bsa:"));    // a plugin's archive beats an ini's
    CHECK(contains(winner("ordered.pex"), "Esp2 - Textures.bsa:"));
    CHECK(contains(winner("masterlast.pex"), "Esp2 - Textures.bsa:"));
    CHECK(contains(winner("looser.pex"), "Looser.pex") && !contains(winner("looser.pex"), ".bsa:"));
    CHECK(winner("onlyoff.pex").empty());                     // only in an archive that does not load
    CHECK(winner("onlystray.pex").empty());
    CHECK(winner("onlyunlisted.pex").empty());
    CHECK(scripts.count("deep.pex") == 0);                    // not directly under scripts
    CHECK(scripts.count("stale.pex") == 0);

    // --all-plugins: Off is inactive and listed last, so now it loads, last, and
    // wins. Unlisted still does not, and neither does the name nobody claims.
    {
        ArchivePlan every;
        CHECK(plan_archives(data, plugins, {iniDir}, true, every, why));
        std::map<std::string, int> everyRank;
        for (const ArchivePlan::Loaded& l : every.loaded) everyRank[l.path.filename().u8string()] = l.rank;
        CHECK(every.loaded.size() == 6);
        CHECK(everyRank.count("Off.bsa") == 1);
        CHECK(everyRank["Off.bsa"] > everyRank["Esp2 - Textures.bsa"]);
        CHECK(everyRank["EspM.bsa"] < everyRank["Esp1.bsa"]);  // masters still first
        CHECK(everyRank.count("Unlisted.bsa") == 0);
        CHECK(everyRank.count("Stray0.bsa") == 0);
        CHECK(every.notLoaded.size() == 2);
        std::vector<std::string> w;
        auto all = collect_script_copies(data, fs::path(), every, w);
        auto allWinner = [&](const char* key) {
            const auto& copies = all[key];
            const int i = winning_copy(copies);
            return i < 0 ? std::string() : copies[static_cast<size_t>(i)].item.label;
        };
        CHECK(contains(allWinner("onlybase.pex"), "Off.bsa:"));
        CHECK(contains(allWinner("onlyoff.pex"), "Off.bsa:"));
        CHECK(contains(allWinner("ordered.pex"), "Esp2 - Textures.bsa:"));
        CHECK(allWinner("onlyunlisted.pex").empty());
        CHECK(allWinner("onlystray.pex").empty());
        std::string allList, allWhy;
        CHECK(list_winners(data, plugins, {iniDir}, true, fs::path(), allList, allWhy));
        CHECK(contains(allList, "onlyoff.pex,archive,"));
        CHECK(!contains(allList, "onlyunlisted.pex"));
        OutOptions a;
        a.data = data;
        a.outFolder = root / "mod" / "allplugins";
        a.pluginsTxt = plugins;
        a.iniDirs = {iniDir};
        a.allPlugins = true;
        a.source = "formid";
        std::string aReport, aLog;
        CHECK(patch_to_output(a, load_map("!PLUGIN=Foo.esp\n0000080C|00000A0C\n00000801|00000A01\n"), aReport, aLog) == 0);
        CHECK(contains(aLog, "plugins-counted|all-listed\n"));
        CHECK(contains(aReport, "ORDER|every listed plugin, "));
        CHECK(only_ref(slurp(a.outFolder / "scripts" / "onlybase.pex")) == 0xA0C);  // Off.bsa's copy, not Base.bsa's
    }

    std::string list, listWhy;
    CHECK(list_winners(data, plugins, {iniDir}, false, fs::path(), list, listWhy));
    CHECK(contains(list, "looser.pex,loose,"));
    CHECK(contains(list, "ordered.pex,archive,"));
    CHECK(!contains(list, "onlyoff.pex"));

    // Everything Data holds, to compare afterwards.
    auto snapshot = [&] {
        std::map<std::string, Bytes> all;
        for (const auto& e : fs::recursive_directory_iterator(data)) {
            if (e.is_regular_file()) all[e.path().u8string()] = slurp(e.path());
        }
        return all;
    };
    const auto before = snapshot();

    OutOptions o;
    o.data = data;
    o.outFolder = output;
    o.pluginsTxt = plugins;
    o.iniDirs = {iniDir};
    o.source = "formid";
    pex::FormIdMap all = load_map(
        "!PLUGIN=Foo.esp\n00000801|00000A01\n00000802|00000A02\n00000803|00000A03\n00000804|00000A04\n00000805|00000A05\n00000806|00000A06\n"
        "00000807|00000A07\n00000809|00000A09\n0000080A|00000A0A\n0000080B|00000A0B\n0000080D|00000A0D\n0000080E|00000A0E\n");

    // A dry run writes nothing at all.
    std::string report, text;
    o.dryRun = true;
    CHECK(patch_to_output(o, all, report, text) == 0);
    CHECK(contains(report, "ORDER|"));
    CHECK(contains(report, "DRYRUN|"));
    CHECK(contains(text, "mode|dry-run\n") && contains(text, "\nwould-patch|"));
    CHECK(contains(text, "plugins-counted|active\n"));
    CHECK(contains(report, "ORDER|active plugins, "));
    CHECK(!fs::exists(output));
    CHECK(snapshot() == before);

    // The real run.
    o.dryRun = false;
    report.clear();
    text.clear();
    CHECK(patch_to_output(o, all, report, text) == 0);
    CHECK(snapshot() == before);
    const fs::path out = output / "scripts";
    auto got = [&](const char* name) { return only_ref(slurp(out / name)); };
    CHECK(got("onlybase.pex") == 0xA01);
    CHECK(got("basevsmod.pex") == 0xA03);  // from Esp1.bsa, not Base.bsa's 0x802
    CHECK(got("ordered.pex") == 0xA05);
    CHECK(got("masterlast.pex") == 0xA0A);
    CHECK(got("Looser.pex") == 0xA0B);     // keeps the loose file's spelling
    CHECK(!fs::exists(out / "onlyoff.pex"));
    CHECK(!fs::exists(out / "onlystray.pex"));
    CHECK(!fs::exists(out / "deep.pex"));
    size_t files = 0;
    for (const auto& e : fs::directory_iterator(out)) files += e.is_regular_file() ? 1 : 0;
    CHECK(files == 5);
    // Compressed sources make no difference, and nothing lands beside scripts\.
    size_t top = 0;
    for (const auto& e : fs::directory_iterator(output)) top += e.path().filename() != "scripts" ? 1 : 0;
    CHECK(top == 0);
    CHECK(fs::exists(output_state_path(output)));
    CHECK(output_state_path(output).parent_path() == fs::absolute(output).lexically_normal().parent_path());

    // The log says what won, and what lost and would have changed.
    CHECK(text.rfind("pex patch output log v2\n", 0) == 0);
    CHECK(contains(text, "\nupdate|no\n"));
    CHECK(contains(text, "\npatched|ordered.pex|1|0|00000805->00000A05|0|0|archive|Esp2 - Textures.bsa:"));
    CHECK(contains(text, "\npatched|Looser.pex|1|0|0000080B->00000A0B|0|0|loose|"));
    CHECK(contains(text, "\nlost|ordered.pex|1|00000804->00000A04|archive|Esp1.bsa:"));
    CHECK(contains(text, "\nlost|onlyoff.pex|1|0000080D->00000A0D|not-loaded|Off.bsa:scripts\\onlyoff.pex|\n"));
    CHECK(contains(text, "\n[totals]\nscripts|"));
    CHECK(contains(text, "\npatched|5\nremoved|0\nedits|5\n"));
    CHECK(contains(report, "LOST|Off.bsa:scripts\\onlyoff.pex|1 edits|its archive does not load\n"));

    // The same run again: the output copies are what win, and they do not move.
    const Bytes firstRun = slurp(out / "ordered.pex");
    report.clear();
    text.clear();
    CHECK(patch_to_output(o, all, report, text) == 0);
    CHECK(slurp(out / "ordered.pex") == firstRun);
    CHECK(contains(text, "\npatched|0\n"));

    // A map whose new value is another entry's old value is the case that would
    // renumber twice. Run once it is right; run again it must not move.
    const fs::path chainOut = root / "mod" / "chain";
    OutOptions c = o;
    c.outFolder = chainOut;
    const pex::FormIdMap chain = load_map("!PLUGIN=Foo.esp\n00000805|00000C01\n00000C01|00000C02\n");
    report.clear();
    text.clear();
    CHECK(patch_to_output(c, chain, report, text) == 0);
    CHECK(only_ref(slurp(chainOut / "scripts" / "ordered.pex")) == 0xC01);
    report.clear();
    text.clear();
    CHECK(patch_to_output(c, chain, report, text) == 0);
    CHECK(only_ref(slurp(chainOut / "scripts" / "ordered.pex")) == 0xC01);
    CHECK(contains(report, "already applied"));
    CHECK(contains(text, "\nskipped.already-applied|1\n"));

    // Other pairs for the same plugin on that copy: refused, the copy untouched.
    const pex::FormIdMap other = load_map("!PLUGIN=Foo.esp\n00000C01|00000D01\n");
    report.clear();
    text.clear();
    CHECK(patch_to_output(c, other, report, text) == 1);
    CHECK(contains(report, "already patched for Foo.esp with other pairs"));
    CHECK(only_ref(slurp(chainOut / "scripts" / "ordered.pex")) == 0xC01);

    // Two plugins compose: the second run starts from the first's output.
    const Bytes two = compile_script("TwoPlugins",
                                     "Scriptname TwoPlugins\nFunction F()\n    Game.GetFormFromFile(0x811, \"Foo.esp\")\n    Game.GetFormFromFile(0x812, \"Bar.esp\")\nEndFunction\n");
    spit(data / "Scripts" / "TwoPlugins.pex", two);
    const fs::path both = root / "mod" / "both";
    OutOptions b = o;
    b.outFolder = both;
    CHECK(patch_to_output(b, load_map("!PLUGIN=Foo.esp\n00000811|00000E11\n"), report, text) == 0);
    CHECK(patch_to_output(b, load_map("!PLUGIN=Bar.esp\n00000812|00000E12\n"), report, text) == 0);
    {
        pex::Script s;
        const Bytes bytes = slurp(both / "scripts" / "TwoPlugins.pex");
        CHECK(pex::read(bytes.data(), bytes.size(), s) == pex::Status::Ok);
        CHECK(ref_with_ip_order(s, 0) == 0xE11);
        CHECK(ref_with_ip_order(s, 1) == 0xE12);
    }
    // A new mod replaces the script: its copy is deleted from the output folder and
    // both plugins run again. Neither may count as already applied to the new copy.
    fs::remove(both / "scripts" / "TwoPlugins.pex");
    const Bytes newer = compile_script("TwoPlugins",
                                       "Scriptname TwoPlugins\nFunction F()\n    Game.GetFormFromFile(0x811, \"Foo.esp\")\n    Game.GetFormFromFile(0x812, \"Bar.esp\")\n    Game.GetFormFromFile(0x813, \"Other.esm\")\nEndFunction\n");
    spit(data / "Scripts" / "TwoPlugins.pex", newer);
    CHECK(patch_to_output(b, load_map("!PLUGIN=Foo.esp\n00000811|00000E11\n"), report, text) == 0);
    CHECK(patch_to_output(b, load_map("!PLUGIN=Bar.esp\n00000812|00000E12\n"), report, text) == 0);
    {
        pex::Script s;
        const Bytes bytes = slurp(both / "scripts" / "TwoPlugins.pex");
        CHECK(pex::read(bytes.data(), bytes.size(), s) == pex::Status::Ok);
        CHECK(ref_with_ip_order(s, 0) == 0xE11);
        CHECK(ref_with_ip_order(s, 1) == 0xE12);
        CHECK(bytes.size() > two.size());  // built from the new mod's version
    }
    spit(data / "Scripts" / "TwoPlugins.pex", two);

    // A rename changes the length, which is fine in a loose file.
    const fs::path renamed = root / "mod" / "renamed";
    OutOptions r = o;
    r.outFolder = renamed;
    report.clear();
    CHECK(patch_to_output(r, load_map("!PLUGIN=Foo.esp\n!NEWPLUGIN=MuchLongerName.esp\n00000811|00000F11\n"), report, text) == 0);
    {
        pex::Script s;
        const Bytes bytes = slurp(renamed / "scripts" / "TwoPlugins.pex");
        CHECK(bytes.size() > two.size());
        CHECK(pex::read(bytes.data(), bytes.size(), s) == pex::Status::Ok);
        const auto refs = pex::find_form_refs(s);
        CHECK(!refs.empty() && refs[0].plugin == "MuchLongerName.esp" && refs[0].formId == 0xF11);
    }

    // The output folder must stay away from Data.
    CHECK(!check_output_folder(data, output).empty() == false);
    CHECK(contains(check_output_folder(data, data), "inside"));
    CHECK(contains(check_output_folder(data, data / "Sub"), "inside"));
    CHECK(contains(check_output_folder(data, root), "contains"));
    CHECK(check_output_folder(root / "Data2", data).empty());  // a shared name prefix is not inside

    // A plugin's own ini that changes the lists stops the run rather than guess.
    {
        std::ofstream f(data / "Esp2.ini", std::ios::binary);
        f << "[Archive]\nsResourceArchiveList2=Base.bsa\n";
    }
    ArchivePlan blocked;
    CHECK(!plan_archives(data, plugins, {iniDir}, false, blocked, why));
    CHECK(contains(why, "Esp2.ini"));
    report.clear();
    text.clear();
    CHECK(patch_to_output(o, all, report, text) == 1);
    CHECK(contains(report, "ERR|load-order|"));
    CHECK(text.empty());
    fs::remove(data / "Esp2.ini");

    // No usable load order.
    CHECK(!plan_archives(data, root / "missing.txt", {iniDir}, false, blocked, why));
    CHECK(!plan_archives(root / "nowhere", plugins, {iniDir}, false, blocked, why));
}

// --- --update: rebuild only when the winner changed -----------------------------------

static void test_update(const fs::path& base) {
    const fs::path root = base / "update";
    const fs::path data = root / "Data", iniDir = root / "ini", output = root / "output";
    fs::create_directories(data / "Scripts");
    fs::create_directories(iniDir);
    spit(data / "Skyrim.esm", tiny_plugin(1));
    spit(data / "Mod.esp", tiny_plugin(0));
    spit(data / "Later.esp", tiny_plugin(0));
    spit(data / "Last.esp", tiny_plugin(0));
    const fs::path plugins = root / "plugins.txt";
    {
        std::ofstream f(plugins, std::ios::binary);
        f << "*Mod.esp\n*Later.esp\n*Last.esp\n";
    }
    {
        std::ofstream f(iniDir / "Skyrim.ini", std::ios::binary);
        f << "[Archive]\nsResourceArchiveList=\nsResourceArchiveList2=\n";
    }
    auto source = [](const char* lines) { return std::string("Scriptname Upd\nFunction F()\n") + lines + "EndFunction\n"; };
    const Bytes v1 = compile_script("Upd", source("    Game.GetFormFromFile(0x821, \"Foo.esp\")\n").c_str());
    spit(data / "Mod.bsa", build_bsa(0x3, {{"scripts", "upd.pex", v1}}));

    OutOptions o;
    o.data = data;
    o.outFolder = output;
    o.pluginsTxt = plugins;
    o.iniDirs = {iniDir};
    o.source = "formid";
    OutOptions u = o;
    u.update = true;
    const pex::FormIdMap foo = load_map("!PLUGIN=Foo.esp\n00000821|00000A21\n");
    const pex::FormIdMap bar = load_map("!PLUGIN=Bar.esp\n00000822|00000A22\n");
    const fs::path copy = output / "scripts" / "upd.pex";
    std::string report, text;
    auto patch = [&](const OutOptions& opt, const pex::FormIdMap& map) {
        report.clear();
        text.clear();
        return patch_to_output(opt, map, report, text);
    };
    auto winner = [&](const fs::path& out) {
        std::vector<Item> items;
        std::vector<std::string> kinds, hashes;
        std::string why;
        CHECK(winning_items(data, plugins, {iniDir}, false, out, items, kinds, hashes, why));
        for (const Item& it : items) {
            if (lower_name(it.name) == "upd.pex") return it.label;
        }
        return std::string();
    };

    CHECK(patch(o, foo) == 0);
    const Bytes first = slurp(copy);
    CHECK(only_ref(first) == 0xA21);

    // Once installed, the mod's own file would win. It is not a contender.
    spit(data / "Scripts" / "upd.pex", first);
    CHECK(contains(winner(fs::path()), "Scripts"));
    CHECK(contains(winner(output), "Mod.bsa:"));

    // The winner has not changed: --update changes nothing, however often.
    CHECK(patch(u, foo) == 0);
    CHECK(slurp(copy) == first);
    CHECK(contains(report, "UNCHANGED|"));
    CHECK(!contains(report, "replacing"));
    CHECK(contains(text, "\nupdate|yes\n"));

    // A new patch loads later with its own version. Without --update the copy
    // stays; with it, it is rebuilt from the new winner.
    const Bytes v2 = compile_script("Upd", source("    Game.GetFormFromFile(0x821, \"Foo.esp\")\n    Game.GetFormFromFile(0x822, \"Bar.esp\")\n").c_str());
    spit(data / "Later.bsa", build_bsa(0x3, {{"scripts", "upd.pex", v2}}));
    CHECK(contains(winner(output), "Later.bsa:"));
    CHECK(patch(o, foo) == 0);
    CHECK(slurp(copy) == first);
    u.dryRun = true;
    CHECK(patch(u, foo) == 0);
    CHECK(contains(report, "DRYRUN|") && contains(report, "replacing"));
    CHECK(slurp(copy) == first);
    u.dryRun = false;
    CHECK(patch(u, foo) == 0);
    CHECK(contains(report, "PATCHED|") && contains(report, "replacing the copy built from an earlier winner"));
    CHECK(contains(text, "|replaced\n"));
    const Bytes second = slurp(copy);
    CHECK(second != first);
    CHECK(only_ref(second) == 0xA21);

    // The second plugin builds on that copy instead of rebuilding it, and a
    // further run of either changes nothing.
    CHECK(patch(u, bar) == 0);
    CHECK(!contains(report, "replacing"));
    const Bytes both = slurp(copy);
    {
        pex::Script s;
        CHECK(pex::read(both.data(), both.size(), s) == pex::Status::Ok);
        CHECK(ref_with_ip_order(s, 0) == 0xA21 && ref_with_ip_order(s, 1) == 0xA22);
    }
    CHECK(patch(u, foo) == 0);
    CHECK(patch(u, bar) == 0);
    CHECK(slurp(copy) == both);

    // An earlier installed copy, not reinstalled since, is still ours.
    CHECK(contains(winner(output), "Later.bsa:"));

    // The same archive name with new contents is a new winner too.
    const Bytes v3 = compile_script("Upd", source("    Game.GetFormFromFile(0x821, \"Foo.esp\")\n    Game.GetFormFromFile(0x823, \"Other.esm\")\n").c_str());
    spit(data / "Later.bsa", build_bsa(0x3, {{"scripts", "upd.pex", v3}}));
    CHECK(patch(u, foo) == 0);
    CHECK(contains(report, "replacing"));
    CHECK(slurp(copy) != both);

    // The new winner has nothing for this plugin: the copy would only override
    // it with an old script, so it goes.
    const Bytes none = compile_script("Upd", source("    Game.GetFormFromFile(0x823, \"Other.esm\")\n").c_str());
    spit(data / "Last.bsa", build_bsa(0x3, {{"scripts", "upd.pex", none}}));
    u.dryRun = true;
    CHECK(patch(u, foo) == 0);
    CHECK(contains(report, "would remove"));
    CHECK(fs::exists(copy));
    u.dryRun = false;
    CHECK(patch(u, foo) == 0);
    CHECK(contains(report, "REMOVED|"));
    CHECK(contains(text, "\nremoved|upd.pex|Last.bsa:"));
    CHECK(contains(text, "\nremoved|1\n"));
    CHECK(!fs::exists(copy));

    // A loose file from a new mod is a new winner, and is not mistaken for ours.
    const Bytes loose = compile_script("Upd", source("    Game.GetFormFromFile(0x821, \"Foo.esp\")\n    Game.GetFormFromFile(0x824, \"Other.esm\")\n").c_str());
    spit(data / "Scripts" / "upd.pex", loose);
    CHECK(contains(winner(output), "Scripts"));
    CHECK(patch(u, foo) == 0);
    CHECK(only_ref(slurp(copy)) == 0xA21);
    CHECK(slurp(copy).size() == loose.size());

    // Installing that copy hides the mod's loose file under ours. It stays the
    // winner: falling back to the archive would rebuild from an older script.
    const std::string looseSource = winner(output);
    const Bytes fromLoose = slurp(copy);
    spit(data / "Scripts" / "upd.pex", fromLoose);
    CHECK(winner(output) == looseSource);
    CHECK(patch(u, foo) == 0);
    CHECK(!contains(report, "replacing") && !contains(report, "REMOVED"));
    CHECK(slurp(copy) == fromLoose);
}

// --- finding the game ---------------------------------------------------------------

static void test_find_game(const fs::path& base) {
    const fs::path a = base / "games" / "A", b = base / "games" / "B", none = base / "games" / "NoData";
    fs::create_directories(a / "Data");
    fs::create_directories(b / "Data");
    fs::create_directories(none);
    std::string why;
    // Nothing recorded.
    CHECK(choose_data_folder({}, why).empty());
    CHECK(contains(why, "no record"));
    // One record, and a record with no Data folder, which is not an install.
    why.clear();
    CHECK(choose_data_folder({{"Steam", a}}, why) == (a / "Data").lexically_normal());
    CHECK(choose_data_folder({{"Bethesda", none}, {"Steam", a}}, why) == (a / "Data").lexically_normal());
    CHECK(choose_data_folder({{"Bethesda", none}}, why).empty());
    CHECK(contains(why, "no Data folder"));
    // Two records of one install, however spelled, are one install.
    fs::path upper = a;
    {
        std::string s = a.u8string();
        for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        upper = fs::u8path(s);
    }
    CHECK(choose_data_folder({{"Bethesda", a}, {"Steam", upper}}, why) == (a / "Data").lexically_normal());
    CHECK(choose_data_folder({{"Bethesda", fs::path(a) / ""}, {"Steam", a}}, why) == (a / "Data").lexically_normal());
    // Two installs: not guessed between.
    why.clear();
    CHECK(choose_data_folder({{"Bethesda", a}, {"Steam", b}}, why).empty());
    CHECK(contains(why, "two installs") && contains(why, "Bethesda") && contains(why, "Steam"));

    // Steam if it has an install, otherwise GOG; an install of each is refused.
    GameInstall g;
    why.clear();
    CHECK(pick_game({{"Steam", a}}, {}, g, why) && g.store == GameStore::Steam && g.data == (a / "Data").lexically_normal());
    CHECK(pick_game({}, {{"GOG", b}}, g, why) && g.store == GameStore::Gog && g.data == (b / "Data").lexically_normal());
    // A stale Steam record does not hide a real GOG install.
    CHECK(pick_game({{"Steam", none}}, {{"GOG", b}}, g, why) && g.store == GameStore::Gog);
    why.clear();
    CHECK(!pick_game({{"Steam", a}}, {{"GOG", b}}, g, why));
    CHECK(contains(why, "both are installed"));
    why.clear();
    CHECK(!pick_game({}, {}, g, why));
    CHECK(contains(why, "from Steam") && contains(why, "from GOG"));
    why.clear();
    CHECK(!pick_game({{"Bethesda", a}, {"Steam", b}}, {}, g, why));
    CHECK(contains(why, "two installs"));

    // GOG's folders have a name of their own.
    CHECK(contains(game_ini_dir(GameStore::Gog).u8string(), "Skyrim Special Edition GOG"));
    CHECK(!contains(game_ini_dir(GameStore::Steam).u8string(), " GOG"));

#ifdef _WIN32
    // plugins.txt comes from %LOCALAPPDATA%, whatever that is, and if it is not
    // set there is no default to fall back on.
    const wchar_t* was = _wgetenv(L"LOCALAPPDATA");
    const std::wstring saved = was ? was : L"";
    _wputenv_s(L"LOCALAPPDATA", L"C:\\Somewhere");
    CHECK(default_plugins_txt() == fs::path(L"C:\\Somewhere\\Skyrim Special Edition\\plugins.txt"));
    CHECK(default_plugins_txt(GameStore::Gog) == fs::path(L"C:\\Somewhere\\Skyrim Special Edition GOG\\plugins.txt"));
    _wputenv_s(L"LOCALAPPDATA", L"");
    CHECK(default_plugins_txt().empty());
    _wputenv_s(L"LOCALAPPDATA", saved.c_str());
#endif
}

// --- FormID pairs ---------------------------------------------------------------

static void test_pairs(const fs::path& dir) {
    uint32_t v = 0;
    std::string why;
    CHECK(parse_formid("0x00ABCD", v, why) && v == 0xABCD);
    CHECK(parse_formid("0X0100abcd", v, why) && v == 0x0100ABCD);
    CHECK(parse_formid("2049", v, why) && v == 2049);
    CHECK(parse_formid("16777216", v, why) && v == 16777216);
    CHECK(parse_formid("0", v, why) && v == 0);
    // 0x with the wrong number of digits.
    CHECK(!parse_formid("0x801", v, why));
    CHECK(!parse_formid("0x00000801A", v, why));
    CHECK(!parse_formid("0xGGGGGG", v, why));
    // A hex value missing its prefix is refused, not read as decimal.
    CHECK(!parse_formid("00ABCD", v, why) && contains(why, "0x prefix"));
    CHECK(!parse_formid("000801", v, why) && contains(why, "leading zero"));
    CHECK(!parse_formid("99999999999", v, why));
    CHECK(!parse_formid("", v, why));

    pex::FormIdMap map;
    CHECK(add_formid_pairs("0x000801:0x000805, 2050 : 0x010A02", map, why));
    CHECK(add_formid_pairs("0x0F000900:0x02FFFFFF", map, why));  // high bytes do not count
    CHECK(map.oldToNew.size() == 3);
    CHECK(map.oldToNew[0x801] == 0x805 && map.oldToNew[0x802] == 0x10A02 && map.oldToNew[0x900] == 0xFFFFFF);
    // The same pair again is fine, a different NEW for one OLD is not.
    CHECK(add_formid_pairs("0x000801:0x000805", map, why));
    CHECK(!add_formid_pairs("0x01000801:0x000806", map, why) && contains(why, "mapped twice"));
    CHECK(!add_formid_pairs("0x000801", map, why) && contains(why, "not OLD:NEW"));
    CHECK(!add_formid_pairs("0x000801:", map, why));

    const fs::path file = dir / "pairs.txt";
    {
        std::ofstream f(file, std::ios::binary);
        f << "\xEF\xBB\xBF# a comment\r\n0x000801:0x000805\r\n\r\n  0x000802:0x000806,0x000803:0x000807  \n";
    }
    pex::FormIdMap fromFile;
    CHECK(add_formid_pairs_file(file, fromFile, why));
    CHECK(fromFile.oldToNew.size() == 3 && fromFile.oldToNew[0x803] == 0x807);
    {
        std::ofstream f(file, std::ios::binary | std::ios::trunc);
        f << "0x000801:0x000805\nbad line\n";
    }
    CHECK(!add_formid_pairs_file(file, fromFile, why) && contains(why, "line 2"));
}

// --- the log ---------------------------------------------------------------------

static void test_log() {
    LogRun run;
    run.started = "2026-11-05T12:00:00Z";
    run.inputs = {"C:\\in"};
    run.plugin = "Foo.esp";
    run.source = "formid";
    run.pairs = 2;

    LogContainer archive;
    archive.archive = true;
    archive.path = "C:\\in\\A.bsa";
    archive.journal = "C:\\in\\A.bsa.pexpatch.journal";
    archive.scanned = 5;
    LogFile good;
    good.name = "scripts\\a.pex";
    good.edits = 3;
    good.pairs = {"00000801->00000805", "00000802->00000806"};
    good.dynamic = 1;
    archive.patched.push_back(good);
    LogFile pulled;
    pulled.name = "scripts\\d.pex";
    pulled.edits = 2;
    pulled.pairs = {"00000801->00000805"};
    pulled.extractedTo = "C:\\in\\Scripts\\d.pex";
    archive.extracted.push_back(pulled);
    LogFile compressed;
    compressed.name = "scripts\\b.pex";
    compressed.reason = "compressed";
    compressed.detail = "a|b";
    archive.skipped.push_back(compressed);

    LogContainer loose;
    loose.path = "C:\\in\\Scripts";
    loose.scanned = 2;
    LogFile noEdit;
    noEdit.name = "c.pex";
    noEdit.reason = "no-edit";
    noEdit.detail = "2 not in map, 0 dynamic";
    noEdit.notInMap = 2;
    loose.skipped.push_back(noEdit);

    LogContainer quiet;  // counted, no section
    quiet.path = "C:\\in\\Other";
    quiet.scanned = 7;

    const std::string text = render_patch_log(run, {loose, quiet, archive});
    CHECK(text.rfind("pex patch log v2\nstarted|2026-11-05T12:00:00Z\nmode|write\ninput|C:\\in\nplugin|Foo.esp\nsource|formid\npairs|2\n", 0) == 0);
    CHECK(contains(text, "\n[archive] C:\\in\\A.bsa\njournal|C:\\in\\A.bsa.pexpatch.journal\npatched|scripts\\a.pex|3|00000801->00000805,00000802->00000806|0|1\nextracted|scripts\\d.pex|2|00000801->00000805|0|0|C:\\in\\Scripts\\d.pex\nskipped|scripts\\b.pex|compressed|a/b\nsubtotal|scanned 5|patched 1|extracted 1|edits 5|skipped 1\n"));
    CHECK(contains(text, "\n[loose] C:\\in\\Scripts\nskipped|c.pex|no-edit|2 not in map, 0 dynamic\nsubtotal|scanned 2|patched 0|extracted 0|edits 0|skipped 1\n"));
    CHECK(!contains(text, "C:\\in\\Other"));
    CHECK(text.find("[archive]") < text.find("[loose]"));
    CHECK(contains(text, "\n[totals]\narchives|1\nloose-folders|1\nscanned|14\npatched|1\nextracted|1\nedits|5\nskipped|2\nskipped.compressed|1\nskipped.no-edit|1\nrefs-not-in-map|2\nrefs-dynamic|1\n"));

    run.dryRun = true;
    const std::string dry = render_patch_log(run, {archive});
    CHECK(contains(dry, "mode|dry-run\n"));
    CHECK(contains(dry, "would-patch|scripts\\a.pex|3|"));
    CHECK(contains(dry, "|would-patch 1|would-extract 1|"));
    CHECK(contains(dry, "would-extract|scripts\\d.pex|2|"));
    CHECK(!contains(dry, "\npatched|"));

    CHECK(skip_reason_code("a different copy in z.bsa") == "other-copy");
    CHECK(skip_reason_code("stored compressed (x)") == "compressed");
    CHECK(skip_reason_code("its data block is shared with y") == "shared-block");
    CHECK(skip_reason_code("inside an archive and the map renames the plugin") == "plugin-rename");
    CHECK(skip_reason_code("cannot open") == "failed");
}

int main(int, char** argv) {
    std::error_code ec;
    const fs::path dir = fs::temp_directory_path(ec) / ("skqe_archive_patch_" + std::to_string(static_cast<unsigned long long>(
                                                            fs::file_time_type::clock::now().time_since_epoch().count())));
    fs::create_directories(dir, ec);
    if (ec) {
        std::fprintf(stderr, "cannot create %s\n", dir.string().c_str());
        return 1;
    }
    test_patch_and_undo(dir, 0x3, "plain.bsa");
    test_patch_and_undo(dir, 0x103, "embedded.bsa");  // the data block starts with the member's path
    test_layers(dir);
    test_undo_refuses(dir);
    test_refusals(dir);
    test_rollback(dir);
    test_loose(dir);
    test_extract(dir);
    test_out(dir);
    test_update(dir);
    test_find_game(dir);
    test_game_check(argv[0]);
    test_pairs(dir);
    test_log();
    fs::remove_all(dir, ec);
    if (g_failures) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::puts("archive patch tests ok");
    return 0;
}
