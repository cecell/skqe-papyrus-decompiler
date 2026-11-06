// In-archive FormID patching (see archive_patch.hpp).
// SPDX-License-Identifier: GPL-3.0-only
#include "archive_patch.hpp"

#include "bsa/reader.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <string_view>
#include <utility>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace pextool {

namespace fs = std::filesystem;
namespace pex = papyrus::pex;

namespace {

// --- a file opened for positioned reads and writes ---------------------------

class RwFile {
public:
    RwFile() = default;
    RwFile(const RwFile&) = delete;
    RwFile& operator=(const RwFile&) = delete;
    ~RwFile() { close(); }

    // On Windows other readers may stay, but the open fails while another
    // program holds the file without sharing writes, rather than changing the
    // bytes underneath it.
    bool open(const fs::path& path, bool create, std::string& why) {
#ifdef _WIN32
        m_h = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                          create ? OPEN_ALWAYS : OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (m_h == INVALID_HANDLE_VALUE) {
            const DWORD e = GetLastError();
            why = "cannot open for writing (Windows error " + std::to_string(e) + ")";
            if (e == ERROR_SHARING_VIOLATION) why += "; another program holds it open, is the game or a manager running?";
            if (e == ERROR_ACCESS_DENIED) why += "; the file is read-only or the folder is protected";
            return false;
        }
#else
        m_fd = ::open(path.c_str(), create ? (O_RDWR | O_CREAT) : O_RDWR, 0644);
        if (m_fd < 0) {
            why = "cannot open for writing (errno " + std::to_string(errno) + ")";
            return false;
        }
#endif
        return true;
    }

    bool size(uint64_t& out) const {
#ifdef _WIN32
        LARGE_INTEGER n;
        if (!GetFileSizeEx(m_h, &n)) return false;
        out = static_cast<uint64_t>(n.QuadPart);
        return true;
#else
        const off_t end = ::lseek(m_fd, 0, SEEK_END);
        if (end < 0) return false;
        out = static_cast<uint64_t>(end);
        return true;
#endif
    }

    bool read_at(uint64_t offset, void* data, size_t n) const {
#ifdef _WIN32
        OVERLAPPED ov{};
        ov.Offset = static_cast<DWORD>(offset);
        ov.OffsetHigh = static_cast<DWORD>(offset >> 32);
        DWORD got = 0;
        return ReadFile(m_h, data, static_cast<DWORD>(n), &got, &ov) && got == n;
#else
        return ::pread(m_fd, data, n, static_cast<off_t>(offset)) == static_cast<ssize_t>(n);
#endif
    }

    bool write_at(uint64_t offset, const void* data, size_t n) {
#ifdef _WIN32
        OVERLAPPED ov{};
        ov.Offset = static_cast<DWORD>(offset);
        ov.OffsetHigh = static_cast<DWORD>(offset >> 32);
        DWORD put = 0;
        return WriteFile(m_h, data, static_cast<DWORD>(n), &put, &ov) && put == n;
#else
        return ::pwrite(m_fd, data, n, static_cast<off_t>(offset)) == static_cast<ssize_t>(n);
#endif
    }

    // To the disk, not just the OS cache.
    bool flush() {
#ifdef _WIN32
        return FlushFileBuffers(m_h) != 0;
#else
        return ::fsync(m_fd) == 0;
#endif
    }

    void close() {
#ifdef _WIN32
        if (m_h != INVALID_HANDLE_VALUE) CloseHandle(m_h);
        m_h = INVALID_HANDLE_VALUE;
#else
        if (m_fd >= 0) ::close(m_fd);
        m_fd = -1;
#endif
    }

private:
#ifdef _WIN32
    HANDLE m_h = INVALID_HANDLE_VALUE;
#else
    int m_fd = -1;
#endif
};

// --- small helpers -------------------------------------------------------------

uint32_t be32(const uint8_t* p) {
    return (uint32_t{p[0]} << 24) | (uint32_t{p[1]} << 16) | (uint32_t{p[2]} << 8) | uint32_t{p[3]};
}

std::string to_hex(const uint8_t* p, size_t n) {
    static const char kDigits[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(n * 2);
    for (size_t i = 0; i < n; ++i) {
        out += kDigits[p[i] >> 4];
        out += kDigits[p[i] & 0xF];
    }
    return out;
}

bool from_hex(std::string_view s, std::vector<uint8_t>& out) {
    if (s.empty() || s.size() % 2 != 0) return false;
    auto digit = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return -1;
    };
    out.clear();
    for (size_t i = 0; i < s.size(); i += 2) {
        const int hi = digit(s[i]), lo = digit(s[i + 1]);
        if (hi < 0 || lo < 0) return false;
        out.push_back(static_cast<uint8_t>((hi << 4) | lo));
    }
    return true;
}

std::string utc_now() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

std::vector<std::string_view> split_bars(std::string_view line) {
    std::vector<std::string_view> out;
    size_t pos = 0;
    for (;;) {
        const size_t bar = line.find('|', pos);
        if (bar == std::string_view::npos) {
            out.push_back(line.substr(pos));
            return out;
        }
        out.push_back(line.substr(pos, bar - pos));
        pos = bar + 1;
    }
}

// A journal field cannot hold the separator or a line break.
bool journal_safe(std::string_view s) {
    return s.find('|') == std::string_view::npos && s.find('\n') == std::string_view::npos &&
           s.find('\r') == std::string_view::npos;
}

// --- the model comparison ----------------------------------------------------

using ValueChange = std::pair<uint32_t, uint32_t>;

bool diff_function(const pex::Function& a, const pex::Function& b, std::vector<ValueChange>& out) {
    if (a.code.size() != b.code.size()) return false;
    for (size_t i = 0; i < a.code.size(); ++i) {
        const pex::Instruction& x = a.code[i];
        const pex::Instruction& y = b.code[i];
        if (x.op != y.op || x.args.size() != y.args.size()) return false;
        for (size_t k = 0; k < x.args.size(); ++k) {
            if (x.args[k].type != y.args[k].type) return false;
            if (x.args[k].bits == y.args[k].bits) continue;
            if (x.args[k].type != pex::ValueType::Integer) return false;
            out.emplace_back(x.args[k].bits, y.args[k].bits);
        }
    }
    return true;
}

// Every integer value that differs between two scripts of the same shape;
// false when anything else differs.
bool diff_integers(const pex::Script& a, const pex::Script& b, std::vector<ValueChange>& out) {
    if (a.objects.size() != b.objects.size()) return false;
    for (size_t o = 0; o < a.objects.size(); ++o) {
        const pex::Object& x = a.objects[o];
        const pex::Object& y = b.objects[o];
        if (x.variables.size() != y.variables.size() || x.properties.size() != y.properties.size() ||
            x.states.size() != y.states.size())
            return false;
        for (size_t v = 0; v < x.variables.size(); ++v) {
            const pex::Value& p = x.variables[v].initial;
            const pex::Value& q = y.variables[v].initial;
            if (p.type != q.type) return false;
            if (p.bits == q.bits) continue;
            if (p.type != pex::ValueType::Integer) return false;
            out.emplace_back(p.bits, q.bits);
        }
        for (size_t p = 0; p < x.properties.size(); ++p) {
            const pex::Property& m = x.properties[p];
            const pex::Property& n = y.properties[p];
            if (m.getter.has_value() != n.getter.has_value() || m.setter.has_value() != n.setter.has_value()) return false;
            if (m.getter && !diff_function(*m.getter, *n.getter, out)) return false;
            if (m.setter && !diff_function(*m.setter, *n.setter, out)) return false;
        }
        for (size_t s = 0; s < x.states.size(); ++s) {
            if (x.states[s].functions.size() != y.states[s].functions.size()) return false;
            for (size_t f = 0; f < x.states[s].functions.size(); ++f) {
                if (!diff_function(x.states[s].functions[f].body, y.states[s].functions[f].body, out)) return false;
            }
        }
    }
    return true;
}

// --- locating members --------------------------------------------------------

// The stored blocks of an archive, to tell whether overwriting one member
// changes another: an archive builder may point identical files at one block.
class BlockIndex {
public:
    explicit BlockIndex(const bsa::Reader& r) {
        const uint32_t n = r.header().fileCount;
        m_blocks.reserve(n);
        for (uint32_t i = 0; i < n; ++i) {
            bsa::Entry e;
            if (r.entry(i, e) == bsa::Status::Ok && e.storedSize != 0) m_blocks.push_back({e.dataOffset, e.dataOffset + e.storedSize, i});
        }
        std::sort(m_blocks.begin(), m_blocks.end(), [](const Block& a, const Block& b) { return a.begin < b.begin; });
        m_maxEnd.reserve(m_blocks.size());
        uint64_t top = 0;
        for (const Block& b : m_blocks) {
            top = std::max(top, b.end);
            m_maxEnd.push_back(top);
        }
    }

    // Another entry whose block overlaps this one; false when none does.
    bool overlap(const bsa::Entry& e, uint32_t& other) const {
        const uint64_t begin = e.dataOffset, end = e.dataOffset + e.storedSize;
        size_t i = static_cast<size_t>(std::lower_bound(m_blocks.begin(), m_blocks.end(), end,
                                                        [](const Block& b, uint64_t x) { return b.begin < x; }) -
                                       m_blocks.begin());
        while (i > 0 && m_maxEnd[i - 1] > begin) {
            --i;
            if (m_blocks[i].end > begin && m_blocks[i].index != e.fileIndex) {
                other = m_blocks[i].index;
                return true;
            }
        }
        return false;
    }

private:
    struct Block {
        uint64_t begin, end;
        uint32_t index;
    };
    std::vector<Block> m_blocks;
    std::vector<uint64_t> m_maxEnd;
};

std::string entry_name(const bsa::Reader& r, uint32_t index) {
    bsa::Entry e;
    std::string_view folder, name;
    if (r.entry(index, e) != bsa::Status::Ok || r.folder_name(e.folderIndex, folder) != bsa::Status::Ok ||
        r.file_name(e, name) != bsa::Status::Ok)
        return "entry " + std::to_string(index);
    return std::string(folder) + "\\" + std::string(name);
}

// Finds where a member's bytes sit; false with the refusal in `why`.
bool locate_member(const bsa::Reader& r, const BlockIndex& blocks, const MemberPatch& m, uint64_t& payload,
                   uint32_t& fileIndex, std::string& why) {
    bsa::Entry e;
    bsa::Status st = r.find(m.entryPath, e);
    if (st != bsa::Status::Ok) {
        why = std::string("cannot look the member up: ") + bsa::to_string(st);
        return false;
    }
    fileIndex = e.fileIndex;
    if (e.compressed) {
        why = (r.header().archiveFlags & bsa::archive_flags::Compressed)
                  ? "stored compressed (the archive's default, this member does not toggle it off)"
                  : "stored compressed (this member's per-file toggle is set)";
        why += "; compressed bytes cannot be overwritten in place; ";
        why += kLooseFallback;
        return false;
    }
    const uint8_t* data = nullptr;
    size_t size = 0;
    st = r.raw(e, data, size);
    if (st != bsa::Status::Ok || size > e.storedSize) {
        why = std::string("bsa-reader cannot give the member's data offset (") + bsa::to_string(st) + "); " + kLooseFallback;
        return false;
    }
    if (size != m.oldBytes.size() || std::memcmp(data, m.oldBytes.data(), size) != 0) {
        why = std::string("the stored block is not the member's bytes; ") + kLooseFallback;
        return false;
    }
    uint32_t other = 0;
    if (blocks.overlap(e, other)) {
        why = "its data block is shared with " + entry_name(r, other) + ", which an overwrite would change too; " + kLooseFallback;
        return false;
    }
    // raw() skips the embedded-name prefix; what it skipped is the difference.
    payload = e.dataOffset + (e.storedSize - size);
    return true;
}

bool edit_took(const std::vector<pex::FormRef>& refs, const pex::PatchEdit& ed) {
    for (const pex::FormRef& r : refs) {
        if (r.ip == ed.ip && r.formId == ed.newValue && r.function == ed.function && r.state == ed.state &&
            r.object == ed.object && r.callee == ed.callee)
            return true;
    }
    return false;
}

// Reads the member back through the normal lookup and checks every edit took.
bool verify_member(const bsa::Reader& r, const MemberPatch& m, const std::vector<std::string>& extra, std::string& why) {
    bsa::Entry e;
    bsa::Status st = r.find(m.entryPath, e);
    if (st != bsa::Status::Ok) {
        why = std::string("the patched member cannot be looked up: ") + bsa::to_string(st);
        return false;
    }
    std::vector<uint8_t> back;
    st = r.read(e, back);
    if (st != bsa::Status::Ok) {
        why = std::string("the patched member cannot be read: ") + bsa::to_string(st);
        return false;
    }
    if (back != m.newBytes) {
        why = "the patched member's bytes are not the bytes planned";
        return false;
    }
    pex::Script s;
    std::string err;
    if (pex::read(back.data(), back.size(), s, &err) != pex::Status::Ok) {
        why = "the patched member does not read back: " + err;
        return false;
    }
    const std::vector<pex::FormRef> refs = pex::find_form_refs(s, extra);
    for (const pex::PatchEdit& ed : m.edits) {
        if (!edit_took(refs, ed)) {
            char buf[64];
            std::snprintf(buf, sizeof buf, "%08X at ip %u", static_cast<unsigned>(ed.newValue), static_cast<unsigned>(ed.ip));
            why = std::string("edit did not take: ") + buf + " in " + ed.function;
            return false;
        }
    }
    return true;
}

void member_error(MemberPatch& m, const std::string& why) {
    m.report += "ERR|" + m.label + "|" + why + "\n";
    m.failed = true;
}

// --- the journal -------------------------------------------------------------

constexpr const char* kJournalHeader =
    "# pex patch journal v1; undo with: pex patch --undo <this file>\n"
    "# RANGE|seq|archive|member|absolute offset|old hex|new hex|map|UTC time\n"
    "# UNDONE|seq|UTC time\n";

struct JournalRange {
    uint64_t seq = 0;
    std::string archive, member;
    uint64_t offset = 0;
    std::vector<uint8_t> oldBytes, newBytes;
};

struct JournalText {
    std::vector<JournalRange> ranges;  // in file order, undone ones removed
    uint64_t lastSeq = 0;
    bool torn = false;                 // the last line has no line break
    uint64_t goodLength = 0;           // bytes up to and including the last complete line
};

bool parse_u64(std::string_view s, uint64_t& out) {
    if (s.empty()) return false;
    uint64_t v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        v = v * 10 + static_cast<uint64_t>(c - '0');
    }
    out = v;
    return true;
}

bool parse_range(const std::vector<std::string_view>& f, JournalRange& r) {
    if (f.size() != 9 || f[0] != "RANGE") return false;
    if (!parse_u64(f[1], r.seq) || !parse_u64(f[4], r.offset)) return false;
    r.archive = std::string(f[2]);
    r.member = std::string(f[3]);
    if (!from_hex(f[5], r.oldBytes) || !from_hex(f[6], r.newBytes) || r.oldBytes.size() != r.newBytes.size()) return false;
    return !r.archive.empty() && !r.member.empty();
}

// A line break ends every journal line, and the file is flushed only after a
// whole batch and before any byte is written, so a last line without one was
// never acted on. Any other unreadable line is a damaged journal: undo refuses.
bool parse_journal(const std::string& text, JournalText& out, std::string& why) {
    std::vector<JournalRange> all;
    std::set<uint64_t> undone;
    size_t pos = 0;
    size_t lineNo = 0;
    while (pos < text.size()) {
        ++lineNo;
        const size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) {
            out.torn = true;
            break;
        }
        std::string_view line(text.data() + pos, nl - pos);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        pos = nl + 1;
        out.goodLength = pos;
        if (line.empty() || line[0] == '#') continue;
        const std::vector<std::string_view> f = split_bars(line);
        JournalRange r;
        uint64_t seq = 0;
        if (parse_range(f, r)) {
            out.lastSeq = std::max(out.lastSeq, r.seq);
            all.push_back(std::move(r));
        } else if (f.size() == 3 && f[0] == "UNDONE" && parse_u64(f[1], seq)) {
            undone.insert(seq);
        } else {
            why = "journal line " + std::to_string(lineNo) + " cannot be read";
            return false;
        }
    }
    for (JournalRange& r : all) {
        if (!undone.count(r.seq)) out.ranges.push_back(std::move(r));
    }
    return true;
}

bool read_whole(const fs::path& p, std::string& out) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return !in.bad();
}

// Appends text to a journal, to the disk. A torn last line is cut off first.
bool append_journal(const fs::path& journal, const std::string& text, std::string& why) {
    std::error_code ec;
    if (fs::exists(journal, ec)) {
        std::string existing;
        JournalText parsed;
        if (read_whole(journal, existing) && parse_journal(existing, parsed, why) && parsed.torn) {
            fs::resize_file(journal, parsed.goodLength, ec);
            if (ec) {
                why = "cannot repair the journal: " + ec.message();
                return false;
            }
        }
    }
    RwFile f;
    if (!f.open(journal, true, why)) return false;
    uint64_t size = 0;
    if (!f.size(size)) {
        why = "cannot size the journal";
        return false;
    }
    std::string all = size == 0 ? std::string(kJournalHeader) : std::string();
    all += text;
    if (!f.write_at(size, all.data(), all.size()) || !f.flush()) {
        why = "cannot write the journal";
        return false;
    }
    return true;
}

uint64_t next_sequence(const fs::path& journal, std::string& why, bool& ok) {
    ok = true;
    std::error_code ec;
    if (!fs::exists(journal, ec)) return 1;
    std::string text;
    if (!read_whole(journal, text)) {
        why = "cannot read the journal";
        ok = false;
        return 0;
    }
    JournalText parsed;
    if (!parse_journal(text, parsed, why)) {
        ok = false;
        return 0;
    }
    return parsed.lastSeq + 1;
}

std::string archive_label(const fs::path& archive, const std::string& member) {
    return archive.filename().string() + ":" + member;
}

std::string patched_counts(const MemberPatch& m) {
    return std::to_string(m.edits.size()) + " edits, 0 renames, in archive, " + std::to_string(m.ranges.size() * 4) + " bytes";
}

}  // namespace

// --- planning ---------------------------------------------------------------

bool compute_operand_ranges(const std::vector<uint8_t>& oldBytes, const std::vector<uint8_t>& newBytes,
                            const pex::Script& oldScript, const pex::Script& newScript,
                            const std::vector<pex::PatchEdit>& edits, std::vector<OperandRange>& out, std::string& why) {
    out.clear();
    if (oldBytes.size() != newBytes.size()) {
        why = "the member's size would change, from " + std::to_string(oldBytes.size()) + " to " + std::to_string(newBytes.size()) + " bytes";
        return false;
    }
    std::set<ValueChange> wanted;
    for (const pex::PatchEdit& ed : edits) wanted.emplace(static_cast<uint32_t>(ed.oldValue), static_cast<uint32_t>(ed.newValue));

    // Walk the differing bytes. The first uncovered one must fall in a 4-byte
    // value whose type tag says integer, holding an edit's old and new values.
    const size_t n = oldBytes.size();
    const uint8_t integerTag = static_cast<uint8_t>(pex::ValueType::Integer);
    std::set<ValueChange> seen;
    std::vector<ValueChange> found;
    for (size_t i = 0; i < n;) {
        if (oldBytes[i] == newBytes[i]) {
            ++i;
            continue;
        }
        bool matched = false;
        for (size_t p = i >= 3 ? i - 3 : 0; p <= i && p + 4 <= n; ++p) {
            if (p == 0 || oldBytes[p - 1] != integerTag || newBytes[p - 1] != integerTag) continue;
            const ValueChange change(be32(&oldBytes[p]), be32(&newBytes[p]));
            if (!wanted.count(change)) continue;
            OperandRange r;
            r.offset = static_cast<uint32_t>(p);
            std::copy_n(&oldBytes[p], 4, r.oldBytes.begin());
            std::copy_n(&newBytes[p], 4, r.newBytes.begin());
            out.push_back(r);
            seen.insert(change);
            found.push_back(change);
            i = p + 4;
            matched = true;
            break;
        }
        if (!matched) {
            why = "byte " + std::to_string(i) + " differs outside every edited integer operand";
            return false;
        }
    }
    if (seen != wanted) {
        why = "an edit has no integer operand in the written bytes";
        return false;
    }
    // The same operands, from the other direction: the models differ in these
    // integer values and in nothing else.
    std::vector<ValueChange> modelled;
    if (!diff_integers(oldScript, newScript, modelled)) {
        why = "the patched model differs in more than integer values";
        return false;
    }
    std::sort(modelled.begin(), modelled.end());
    std::sort(found.begin(), found.end());
    if (modelled != found) {
        why = "the bytes changed " + std::to_string(found.size()) + " integers, the model " + std::to_string(modelled.size());
        return false;
    }
    return true;
}

bool make_member_patch(const std::string& label, const std::string& entryPath, const std::vector<uint8_t>& oldBytes,
                       const pex::Script& patched, const pex::PatchResult& result, MemberPatch& out, std::string& why) {
    if (!result.renamedStrings.empty()) {
        why = "inside an archive and the map renames the plugin, which changes the member's length; ";
        why += kLooseFallback;
        return false;
    }
    out = MemberPatch{};
    out.label = label;
    out.entryPath = entryPath;
    out.oldBytes = oldBytes;
    out.edits = result.edits;
    std::string err;
    if (pex::write(patched, out.newBytes, &err) != pex::Status::Ok) {
        why = "cannot write the patched model: " + err;
        return false;
    }
    if (out.newBytes.size() != oldBytes.size()) {
        why = "the member's size would change, from " + std::to_string(oldBytes.size()) + " to " + std::to_string(out.newBytes.size()) +
              " bytes; " + kLooseFallback;
        return false;
    }
    pex::Script original;
    if (pex::read(oldBytes.data(), oldBytes.size(), original, &err) != pex::Status::Ok) {
        why = "the member does not read: " + err;
        return false;
    }
    if (!compute_operand_ranges(oldBytes, out.newBytes, original, patched, result.edits, out.ranges, why)) {
        why += "; ";
        why += kLooseFallback;
        return false;
    }
    return true;
}

// --- patching ---------------------------------------------------------------

fs::path journal_path(const fs::path& archive) { return fs::path(archive).concat(".pexpatch.journal"); }

void patch_archive_members(const fs::path& archive, std::vector<MemberPatch*> members, const ArchivePatchOptions& options) {
    if (members.empty()) return;
    std::error_code ec;
    const fs::path absolute = fs::absolute(archive, ec);
    const std::string archiveText = (ec ? archive : absolute).u8string();
    const fs::path journal = journal_path(archive);
    std::string archiveLines;
    const auto hardLinks = fs::hard_link_count(archive, ec);
    if (!ec && hardLinks > 1)
        archiveLines += "NOTE|" + archiveText + "|" + std::to_string(hardLinks) +
                        " hard links share these bytes (a mod manager's staging copy?); an overwrite changes them all\n";

    struct Located {
        MemberPatch* member;
        uint64_t payload;
    };
    std::vector<Located> located;
    {
        bsa::Reader r;
        const bsa::Status st = r.open(archive);
        if (st != bsa::Status::Ok) {
            for (MemberPatch* m : members) member_error(*m, std::string("cannot open the archive: ") + bsa::to_string(st));
            return;
        }
        const BlockIndex blocks(r);
        std::set<uint32_t> planned;
        for (MemberPatch* m : members) {
            uint64_t payload = 0;
            uint32_t fileIndex = 0;
            std::string why;
            if (!locate_member(r, blocks, *m, payload, fileIndex, why)) {
                member_error(*m, why);
            } else if (!planned.insert(fileIndex).second) {
                member_error(*m, "the same member was given twice");
            } else {
                located.push_back({m, payload});
            }
        }
    }  // The mapping is gone before anything is written.

    if (options.dryRun) {
        for (const Located& l : located) {
            for (const OperandRange& rg : l.member->ranges) {
                l.member->report += "DRYRUN|" + l.member->label + "|@" + std::to_string(l.payload + rg.offset) + "|" +
                                    to_hex(rg.oldBytes.data(), 4) + "|" + to_hex(rg.newBytes.data(), 4) + "\n";
            }
            l.member->report += "DRYRUN|" + l.member->label + "|" + patched_counts(*l.member) + " at offset " + std::to_string(l.payload) + "\n";
        }
        if (!archiveLines.empty()) members.back()->report += archiveLines;
        return;
    }

    if (options.backup) archiveLines += "NOTE|" + archiveText + "|--backup copies no archive; the journal is the undo\n";
    if (!journal_safe(archiveText) || !journal_safe(options.mapPath)) {
        for (const Located& l : located) member_error(*l.member, std::string("a path cannot go in the journal; ") + kLooseFallback);
        located.clear();
    }

    std::string why;
    RwFile file;
    if (!located.empty() && !file.open(archive, false, why)) {
        for (const Located& l : located) member_error(*l.member, why);
        located.clear();
    }

    // The bytes on disk must still be the ones planned from.
    std::vector<Located> ready;
    for (const Located& l : located) {
        bool same = journal_safe(l.member->entryPath);
        if (!same) {
            member_error(*l.member, std::string("the member's name cannot go in the journal; ") + kLooseFallback);
            continue;
        }
        for (const OperandRange& rg : l.member->ranges) {
            std::array<uint8_t, 4> onDisk{};
            if (!file.read_at(l.payload + rg.offset, onDisk.data(), 4) || onDisk != rg.oldBytes) {
                same = false;
                break;
            }
        }
        if (same) ready.push_back(l);
        else member_error(*l.member, "the archive changed since the member was read; nothing written");
    }

    if (!ready.empty()) {
        bool ok = true;
        uint64_t seq = next_sequence(journal, why, ok);
        std::map<const MemberPatch*, std::vector<uint64_t>> seqs;
        if (ok) {
            const std::string now = utc_now();
            std::string text;
            for (const Located& l : ready) {
                for (const OperandRange& rg : l.member->ranges) {
                    text += "RANGE|" + std::to_string(seq) + "|" + archiveText + "|" + l.member->entryPath + "|" +
                            std::to_string(l.payload + rg.offset) + "|" + to_hex(rg.oldBytes.data(), 4) + "|" +
                            to_hex(rg.newBytes.data(), 4) + "|" + options.mapPath + "|" + now + "\n";
                    seqs[l.member].push_back(seq++);
                }
            }
            ok = append_journal(journal, text, why);
        }
        if (!ok) {
            for (const Located& l : ready) member_error(*l.member, "nothing written: " + why);
            ready.clear();
        }

        // Puts the old bytes back and records that it did.
        auto restore = [&](const std::vector<Located>& which) {
            bool fine = true;
            for (const Located& l : which) {
                for (const OperandRange& rg : l.member->ranges) {
                    if (!file.write_at(l.payload + rg.offset, rg.oldBytes.data(), 4)) fine = false;
                }
            }
            if (!file.flush()) fine = false;
            if (!fine) return false;
            const std::string now = utc_now();
            std::string text;
            for (const Located& l : which) {
                for (uint64_t s : seqs[l.member]) text += "UNDONE|" + std::to_string(s) + "|" + now + "\n";
            }
            std::string err;
            return append_journal(journal, text, err);
        };

        // Write and flush.
        bool wrote = true;
        for (const Located& l : ready) {
            for (const OperandRange& rg : l.member->ranges) {
                if (!file.write_at(l.payload + rg.offset, rg.newBytes.data(), 4)) wrote = false;
            }
        }
        if (!ready.empty() && !file.flush()) wrote = false;
        if (!wrote) {
            const bool back = restore(ready);
            for (const Located& l : ready) {
                member_error(*l.member, back ? "cannot write; rolled back from the journal"
                                             : "cannot write and the roll back failed; run pex patch --undo " + journal.u8string());
            }
            ready.clear();
        }
        file.close();

        if (!ready.empty() && options.afterWriteForTest) options.afterWriteForTest(archive);

        // Verify through a fresh reader and the normal lookup.
        if (!ready.empty()) {
            std::vector<Located> bad;
            std::map<const MemberPatch*, std::string> reasons;
            {
                bsa::Reader r;
                const bsa::Status st = r.open(archive);
                for (const Located& l : ready) {
                    std::string reason;
                    if (st != bsa::Status::Ok) reason = std::string("the patched archive does not open: ") + bsa::to_string(st);
                    else verify_member(r, *l.member, options.extraCallees, reason);
                    if (!reason.empty()) {
                        reasons[l.member] = reason;
                        bad.push_back(l);
                    }
                }
            }
            if (!bad.empty()) {
                std::string err;
                bool back = file.open(archive, false, err) && restore(bad);
                file.close();
                if (back) {
                    // And check the old bytes are what a reader now finds.
                    bsa::Reader r;
                    back = r.open(archive) == bsa::Status::Ok;
                    for (const Located& l : bad) {
                        bsa::Entry e;
                        std::vector<uint8_t> bytes;
                        if (!back || r.find(l.member->entryPath, e) != bsa::Status::Ok || r.read(e, bytes) != bsa::Status::Ok ||
                            bytes != l.member->oldBytes)
                            back = false;
                    }
                }
                for (const Located& l : bad) {
                    member_error(*l.member, reasons[l.member] + (back ? "; rolled back from the journal"
                                                                      : "; the roll back failed, run pex patch --undo " + journal.u8string()));
                }
            }
            size_t goodRanges = 0;
            for (const Located& l : ready) {
                if (reasons.count(l.member)) continue;
                l.member->report += "PATCHED|" + l.member->label + "|" + patched_counts(*l.member) + " written\n";
                goodRanges += l.member->ranges.size();
            }
            archiveLines += "JOURNAL|" + archiveText + "|" + journal.u8string() + "|" + std::to_string(goodRanges) + " ranges\n";
        }
    }
    if (!archiveLines.empty()) members.back()->report += archiveLines;
}

// --- undo ------------------------------------------------------------------

int undo_journal(const fs::path& journalFile, bool dryRun, std::string& report) {
    const std::string journalText = journalFile.u8string();
    std::string text;
    if (!read_whole(journalFile, text)) {
        report += "ERR|" + journalText + "|cannot read the journal\n";
        return 1;
    }
    JournalText parsed;
    std::string why;
    if (!parse_journal(text, parsed, why)) {
        report += "ERR|" + journalText + "|" + why + "; nothing restored\n";
        return 1;
    }
    if (parsed.torn) report += "NOTE|" + journalText + "|the last line is incomplete, from a run that stopped before writing; ignored\n";
    if (parsed.ranges.empty()) {
        report += "UNCHANGED|" + journalText + "|nothing to undo\n";
        return 0;
    }

    std::map<std::string, std::vector<const JournalRange*>> byArchive;
    for (const JournalRange& r : parsed.ranges) byArchive[r.archive].push_back(&r);

    int failures = 0;
    for (const auto& [archiveText, ranges] : byArchive) {
        const fs::path archive = fs::u8path(archiveText);
        RwFile file;
        if (!file.open(archive, false, why)) {
            report += "ERR|" + archiveText + "|" + why + "; nothing restored\n";
            ++failures;
            continue;
        }
        // One position may have been patched more than once, and an earlier
        // undo may have stopped partway. What is on disk must be one of the
        // values the journal gives that position; the earliest old value is
        // the one to restore.
        struct Position {
            std::vector<uint8_t> target;
            std::set<std::vector<uint8_t>> known;
            std::string member;
        };
        std::map<uint64_t, Position> positions;
        for (const JournalRange* r : ranges) {
            Position& p = positions[r->offset];
            if (p.target.empty()) {
                p.target = r->oldBytes;
                p.member = r->member;
            }
            p.known.insert(r->oldBytes);
            p.known.insert(r->newBytes);
        }
        struct Write {
            uint64_t offset;
            const Position* position;
        };
        std::vector<Write> writes;
        std::map<std::string, std::pair<size_t, size_t>> perMember;  // member -> written, already restored
        bool consistent = true;
        for (const auto& [offset, p] : positions) {
            std::vector<uint8_t> cur(p.target.size());
            if (!file.read_at(offset, cur.data(), cur.size())) {
                report += "ERR|" + archiveText + "|cannot read " + std::to_string(cur.size()) + " bytes at offset " + std::to_string(offset) + "\n";
                consistent = false;
                continue;
            }
            if (cur == p.target) {
                ++perMember[p.member].second;
            } else if (p.known.count(cur)) {
                writes.push_back({offset, &p});
                ++perMember[p.member].first;
                if (dryRun) {
                    report += "DRYRUN|" + archive_label(archive, p.member) + "|@" + std::to_string(offset) + "|" +
                              to_hex(cur.data(), cur.size()) + "|" + to_hex(p.target.data(), p.target.size()) + "\n";
                }
            } else {
                report += "ERR|" + archive_label(archive, p.member) + "|offset " + std::to_string(offset) + " holds " +
                          to_hex(cur.data(), cur.size()) + ", a value the journal never gave it\n";
                consistent = false;
            }
        }
        if (!consistent) {
            report += "ERR|" + archiveText + "|the archive changed since the journal was written; nothing restored\n";
            ++failures;
            continue;
        }
        if (dryRun) {
            for (const auto& [member, counts] : perMember) {
                report += "DRYRUN|" + archive_label(archive, member) + "|undo: " + std::to_string(counts.first) + " ranges to restore, " +
                          std::to_string(counts.second) + " already restored\n";
            }
            continue;
        }
        bool wrote = true;
        size_t bytesWritten = 0;
        std::map<std::string, size_t> bytesPerMember;
        for (const Write& w : writes) {
            if (file.write_at(w.offset, w.position->target.data(), w.position->target.size())) {
                bytesWritten += w.position->target.size();
                bytesPerMember[w.position->member] += w.position->target.size();
            } else {
                wrote = false;
            }
        }
        if (!file.flush()) wrote = false;
        // Read every position back from the file.
        for (const auto& [offset, p] : positions) {
            std::vector<uint8_t> cur(p.target.size());
            if (!file.read_at(offset, cur.data(), cur.size()) || cur != p.target) wrote = false;
        }
        file.close();
        if (!wrote) {
            report += "ERR|" + archiveText + "|the restore did not complete after " + std::to_string(bytesWritten) +
                      " bytes; run the undo again\n";
            ++failures;
            continue;
        }
        // Every member must still read as a script through the normal lookup.
        bool readable = true;
        {
            bsa::Reader r;
            const bsa::Status st = r.open(archive);
            if (st != bsa::Status::Ok) {
                report += "ERR|" + archiveText + "|the restored archive does not open: " + std::string(bsa::to_string(st)) + "\n";
                readable = false;
            }
            for (const auto& [member, counts] : perMember) {
                if (!readable) break;
                bsa::Entry e;
                std::vector<uint8_t> bytes;
                pex::Script s;
                std::string err;
                if (r.find(member, e) != bsa::Status::Ok || r.read(e, bytes) != bsa::Status::Ok ||
                    pex::read(bytes.data(), bytes.size(), s, &err) != pex::Status::Ok) {
                    report += "ERR|" + archive_label(archive, member) + "|restored, but does not read as a script: " + err + "\n";
                    readable = false;
                }
            }
        }
        if (!readable) ++failures;
        const std::string now = utc_now();
        std::string marks;
        for (const JournalRange* r : ranges) marks += "UNDONE|" + std::to_string(r->seq) + "|" + now + "\n";
        if (!append_journal(journalFile, marks, why)) {
            report += "ERR|" + journalText + "|restored, but the journal cannot record it: " + why + "\n";
            ++failures;
        }
        for (const auto& [member, counts] : perMember) {
            report += "UNDONE|" + archive_label(archive, member) + "|" + std::to_string(counts.first) + " ranges, " +
                      std::to_string(bytesPerMember[member]) + " bytes restored, " + std::to_string(counts.second) + " already restored\n";
        }
    }
    return failures;
}

}  // namespace pextool
