// Unit test for in-archive FormID patching: a compiled script in a synthetic
// v105 BSA is patched in place, checked through the reader and undone, and the
// cases that must be refused are refused with the archive untouched.
// SPDX-License-Identifier: GPL-3.0-only
#include "archive_patch.hpp"

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
    return make_member_patch(std::string("A.bsa:") + kEntry, kEntry, bytes, s, r, out, why);
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

int main() {
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
    fs::remove_all(dir, ec);
    if (g_failures) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::puts("archive patch tests ok");
    return 0;
}
