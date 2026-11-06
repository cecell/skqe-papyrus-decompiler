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
#include <tlhelp32.h>
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

// The bytes read back must be the ones planned, read as a script, and show
// every edit.
bool verify_bytes(const std::vector<uint8_t>& back, const MemberPatch& m, const std::vector<std::string>& extra, std::string& why) {
    if (back != m.newBytes) {
        why = "the patched bytes are not the bytes planned";
        return false;
    }
    pex::Script s;
    std::string err;
    if (pex::read(back.data(), back.size(), s, &err) != pex::Status::Ok) {
        why = "the patched script does not read back: " + err;
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

// Reads the member back through the normal lookup and checks it.
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
    return verify_bytes(back, m, extra, why);
}

void member_error(MemberPatch& m, const std::string& why) {
    m.report += "ERR|" + m.label + "|" + why + "\n";
    m.failed = true;
    m.refusal = why;
}

// --- the journal -------------------------------------------------------------

constexpr const char* kJournalHeader =
    "# pex patch journal v2; undo with: pex patch --undo <this file>\n"
    "# RANGE|seq|archive|member|absolute offset|old hex|new hex|map|UTC time\n"
    "# EXTRACTED|seq|file|archive|member|size|FNV-1a 64 hex|map|UTC time\n"
    "# MKDIR|seq|folder|UTC time\n"
    "# UNDONE|seq|UTC time\n";

struct JournalRange {
    uint64_t seq = 0;
    std::string archive, member;
    uint64_t offset = 0;
    std::vector<uint8_t> oldBytes, newBytes;
};

struct JournalExtract {
    uint64_t seq = 0;
    std::string file, archive, member;
    uint64_t size = 0;
    std::string hash;
};

struct JournalFolder {
    uint64_t seq = 0;
    std::string folder;
};

struct JournalText {
    std::vector<JournalRange> ranges;  // in file order, undone ones removed
    std::vector<JournalExtract> extracts;
    std::vector<JournalFolder> folders;
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
    std::vector<JournalExtract> allExtracts;
    std::vector<JournalFolder> allFolders;
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
        } else if (f.size() == 9 && f[0] == "EXTRACTED" && parse_u64(f[1], seq) && !f[2].empty()) {
            JournalExtract x;
            x.seq = seq;
            x.file = std::string(f[2]);
            x.archive = std::string(f[3]);
            x.member = std::string(f[4]);
            x.hash = std::string(f[6]);
            if (!parse_u64(f[5], x.size)) {
                why = "journal line " + std::to_string(lineNo) + " cannot be read";
                return false;
            }
            out.lastSeq = std::max(out.lastSeq, seq);
            allExtracts.push_back(std::move(x));
        } else if (f.size() == 4 && f[0] == "MKDIR" && parse_u64(f[1], seq) && !f[2].empty()) {
            out.lastSeq = std::max(out.lastSeq, seq);
            allFolders.push_back({seq, std::string(f[2])});
        } else {
            why = "journal line " + std::to_string(lineNo) + " cannot be read";
            return false;
        }
    }
    for (JournalRange& r : all) {
        if (!undone.count(r.seq)) out.ranges.push_back(std::move(r));
    }
    for (JournalExtract& x : allExtracts) {
        if (!undone.count(x.seq)) out.extracts.push_back(std::move(x));
    }
    for (JournalFolder& d : allFolders) {
        if (!undone.count(d.seq)) out.folders.push_back(std::move(d));
    }
    return true;
}

// Identifies a file the journal extracted: an undo deletes only that file.
std::string fnv1a64_hex(const std::vector<uint8_t>& bytes) {
    uint64_t h = 0xCBF29CE484222325ull;
    for (uint8_t b : bytes) {
        h ^= b;
        h *= 0x100000001B3ull;
    }
    char buf[20];
    std::snprintf(buf, sizeof buf, "%016llX", static_cast<unsigned long long>(h));
    return buf;
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

// The member field of a journal line for a loose file. An archive's member path
// always has a folder and a separator, so it cannot be this.
constexpr const char* kLooseMember = "-";

std::string archive_label(const fs::path& archive, const std::string& member) {
    if (member == kLooseMember) return archive.u8string();
    return archive.filename().string() + ":" + member;
}

// Reads a whole file as bytes.
bool read_bytes(const fs::path& p, std::vector<uint8_t>& out) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return !in.bad();
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

bool make_member_patch(const std::string& label, const std::string& entryPath, const fs::path& file,
                       const std::vector<uint8_t>& oldBytes, const pex::Script& patched, const pex::PatchResult& result,
                       MemberPatch& out, std::string& why) {
    const std::string fallback = entryPath.empty() ? std::string("the file was left alone") : std::string(kLooseFallback);
    if (!result.renamedStrings.empty()) {
        why = "inside an archive and the map renames the plugin, which changes the member's length; " + fallback;
        return false;
    }
    out = MemberPatch{};
    out.label = label;
    out.entryPath = entryPath;
    out.file = file;
    out.oldBytes = oldBytes;
    out.edits = result.edits;
    std::string err;
    if (pex::write(patched, out.newBytes, &err) != pex::Status::Ok) {
        why = "cannot write the patched model: " + err;
        return false;
    }
    if (out.newBytes.size() != oldBytes.size()) {
        why = "the member's size would change, from " + std::to_string(oldBytes.size()) + " to " + std::to_string(out.newBytes.size()) +
              " bytes; " + fallback;
        return false;
    }
    pex::Script original;
    if (pex::read(oldBytes.data(), oldBytes.size(), original, &err) != pex::Status::Ok) {
        why = "the member does not read: " + err;
        return false;
    }
    if (!compute_operand_ranges(oldBytes, out.newBytes, original, patched, result.edits, out.ranges, why)) {
        why += "; " + fallback;
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

// --- loose files -------------------------------------------------------------

fs::path loose_journal_path(const fs::path& folder) { return folder / "pexpatch.journal"; }

void patch_loose_members(const fs::path& folder, std::vector<MemberPatch*> members, const ArchivePatchOptions& options) {
    if (members.empty()) return;
    std::error_code ec;
    size_t linked = 0;
    for (const MemberPatch* m : members) {
        const auto links = fs::hard_link_count(m->file, ec);
        if (!ec && links > 1) ++linked;
    }
    const fs::path absoluteFolder = fs::absolute(folder, ec);
    const std::string folderText = (ec ? folder : absoluteFolder).u8string();
    std::string folderLines;
    if (linked)
        folderLines += "NOTE|" + folderText + "|" + std::to_string(linked) +
                       " of these files have other hard links (a mod manager's staging copy?); an overwrite changes them all\n";

    if (options.dryRun) {
        for (MemberPatch* m : members) m->report += "DRYRUN|" + m->label + "|" + std::to_string(m->edits.size()) + " edits, 0 renames\n";
        if (!folderLines.empty()) members.back()->report += folderLines;
        return;
    }

    // The bytes on disk must still be the ones planned from, and writable.
    struct Ready {
        MemberPatch* member;
        std::string fileText;
    };
    std::vector<Ready> ready;
    for (MemberPatch* m : members) {
        const fs::path absoluteFile = fs::absolute(m->file, ec);
        std::string fileText = (ec ? m->file : absoluteFile).u8string();
        std::vector<uint8_t> onDisk;
        std::string why;
        RwFile probe;
        if (!journal_safe(fileText) || !journal_safe(options.mapPath)) {
            member_error(*m, "a path cannot go in the journal; the file was left alone");
        } else if (!read_bytes(m->file, onDisk) || onDisk != m->oldBytes) {
            member_error(*m, "the file changed since it was read; nothing written");
        } else if (!probe.open(m->file, false, why)) {
            member_error(*m, why);
        } else {
            ready.push_back({m, std::move(fileText)});
        }
    }
    if (ready.empty()) {
        if (!folderLines.empty()) members.back()->report += folderLines;
        return;
    }

    const fs::path journal = loose_journal_path(folder);
    std::string why;
    bool ok = true;
    uint64_t seq = next_sequence(journal, why, ok);
    std::map<const MemberPatch*, std::vector<uint64_t>> seqs;
    if (ok) {
        const std::string now = utc_now();
        std::string text;
        for (const Ready& r : ready) {
            for (const OperandRange& rg : r.member->ranges) {
                text += "RANGE|" + std::to_string(seq) + "|" + r.fileText + "|" + kLooseMember + "|" + std::to_string(rg.offset) + "|" +
                        to_hex(rg.oldBytes.data(), 4) + "|" + to_hex(rg.newBytes.data(), 4) + "|" + options.mapPath + "|" + now + "\n";
                seqs[r.member].push_back(seq++);
            }
        }
        ok = append_journal(journal, text, why);
    }
    if (!ok) {
        for (const Ready& r : ready) member_error(*r.member, "nothing written: " + why);
        if (!folderLines.empty()) members.back()->report += folderLines;
        return;
    }

    size_t goodRanges = 0;
    for (const Ready& r : ready) {
        MemberPatch& m = *r.member;
        if (options.backup) fs::copy_file(m.file, fs::path(m.file).concat(".bak"), fs::copy_options::overwrite_existing, ec);

        auto write_ranges = [&](bool forward) {
            RwFile f;
            std::string err;
            if (!f.open(m.file, false, err)) return false;
            bool fine = true;
            for (const OperandRange& rg : m.ranges) {
                if (!f.write_at(rg.offset, forward ? rg.newBytes.data() : rg.oldBytes.data(), 4)) fine = false;
            }
            return f.flush() && fine;
        };
        auto roll_back = [&]() {
            bool back = write_ranges(false);
            std::vector<uint8_t> bytes;
            if (back && (!read_bytes(m.file, bytes) || bytes != m.oldBytes)) back = false;
            if (back) {
                const std::string now = utc_now();
                std::string text;
                for (uint64_t s : seqs[&m]) text += "UNDONE|" + std::to_string(s) + "|" + now + "\n";
                std::string err;
                back = append_journal(journal, text, err);
            }
            return back;
        };

        std::string reason;
        std::vector<uint8_t> back;
        if (!write_ranges(true)) reason = "cannot write";
        else {
            if (options.afterWriteForTest) options.afterWriteForTest(m.file);
            if (!read_bytes(m.file, back)) reason = "the patched file cannot be read";
            else verify_bytes(back, m, options.extraCallees, reason);
        }
        if (!reason.empty()) {
            member_error(m, reason + (roll_back() ? "; rolled back from the journal"
                                                  : "; the roll back failed, run pex patch --undo " + journal.u8string()));
            continue;
        }
        m.report += "PATCHED|" + m.label + "|" + std::to_string(m.edits.size()) + " edits, 0 renames\n";
        goodRanges += m.ranges.size();
    }
    folderLines += "JOURNAL|" + folderText + "|" + journal.u8string() + "|" + std::to_string(goodRanges) + " ranges\n";
    members.back()->report += folderLines;
}

// --- compressed members ---------------------------------------------------------

bool find_compressed_members(const fs::path& archive, const std::vector<MemberPatch*>& members, std::vector<char>& compressed) {
    compressed.assign(members.size(), 0);
    bsa::Reader r;
    if (r.open(archive) != bsa::Status::Ok) return false;
    for (size_t i = 0; i < members.size(); ++i) {
        bsa::Entry e;
        if (r.find(members[i]->entryPath, e) == bsa::Status::Ok) compressed[i] = e.compressed ? 1 : 0;
    }
    return true;
}

bool extract_target(const fs::path& archive, const std::string& entryPath, fs::path& out) {
    constexpr size_t kPrefix = 8;  // "scripts\"
    if (entryPath.size() <= kPrefix) return false;
    std::string head = entryPath.substr(0, kPrefix);
    std::transform(head.begin(), head.end(), head.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    if (head != "scripts\\" && head != "scripts/") return false;
    std::string rest = entryPath.substr(kPrefix);
    if (rest.find("..") != std::string::npos || rest[0] == '\\' || rest[0] == '/') return false;
    std::replace(rest.begin(), rest.end(), '\\', '/');
    out = archive.parent_path() / "Scripts" / fs::u8path(rest);
    return true;
}

void find_other_copies(std::vector<ExtractCandidate>& candidates) {
    std::map<fs::path, std::vector<ExtractCandidate*>> byFolder;
    for (ExtractCandidate& c : candidates) byFolder[c.archive.parent_path()].push_back(&c);
    for (const auto& [folder, list] : byFolder) {
        std::error_code ec;
        for (fs::directory_iterator it(folder.empty() ? fs::path(".") : folder, ec), end; !ec && it != end; it.increment(ec)) {
            std::string ext = it->path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
            if (ext != ".bsa") continue;
            bsa::Reader r;
            if (r.open(it->path()) != bsa::Status::Ok) continue;
            for (ExtractCandidate* c : list) {
                std::error_code same;
                if (!c->conflict.empty() || fs::equivalent(it->path(), c->archive, same)) continue;
                bsa::Entry e;
                std::vector<uint8_t> bytes;
                if (r.find(c->member->entryPath, e) != bsa::Status::Ok) continue;
                // A copy that cannot be read cannot be shown to be the same.
                if (r.read(e, bytes) != bsa::Status::Ok || bytes != c->member->oldBytes) c->conflict = it->path().filename().u8string();
            }
        }
    }
}

void extract_members(const fs::path& archive, std::vector<MemberPatch*> members, const ArchivePatchOptions& options) {
    if (members.empty()) return;
    if (options.dryRun) {
        for (MemberPatch* m : members) {
            m->report += "DRYRUN|" + m->label + "|would extract to " + m->extractTo.u8string() + "|" + std::to_string(m->edits.size()) + " edits, 0 renames\n";
        }
        return;
    }

    std::error_code ec;
    const fs::path absoluteArchive = fs::absolute(archive, ec);
    const std::string archiveText = (ec ? archive : absoluteArchive).u8string();
    const fs::path journal = journal_path(archive);

    struct Ready {
        MemberPatch* member;
        std::string fileText;
        uint64_t seq;
    };
    std::vector<Ready> ready;
    std::string why;
    bool ok = true;
    uint64_t seq = next_sequence(journal, why, ok);
    std::string text;
    std::map<fs::path, uint64_t> newFolders;
    if (ok) {
        const std::string now = utc_now();
        for (MemberPatch* m : members) {
            const fs::path absoluteFile = fs::absolute(m->extractTo, ec);
            std::string fileText = (ec ? m->extractTo : absoluteFile).u8string();
            if (!journal_safe(fileText) || !journal_safe(archiveText) || !journal_safe(m->entryPath) || !journal_safe(options.mapPath)) {
                member_error(*m, "a path cannot go in the journal; nothing written");
                continue;
            }
            if (fs::exists(m->extractTo, ec)) {
                member_error(*m, "a file appeared at " + fileText + "; nothing written");
                continue;
            }
            // Folders to create, outermost first, so an undo can take them away.
            std::vector<fs::path> missing;
            for (fs::path d = fs::absolute(m->extractTo, ec).parent_path(); !d.empty() && !fs::exists(d, ec) && !newFolders.count(d); d = d.parent_path()) {
                missing.push_back(d);
                if (d == d.parent_path()) break;
            }
            for (auto d = missing.rbegin(); d != missing.rend(); ++d) {
                newFolders[*d] = seq;
                text += "MKDIR|" + std::to_string(seq++) + "|" + d->u8string() + "|" + now + "\n";
            }
            text += "EXTRACTED|" + std::to_string(seq) + "|" + fileText + "|" + archiveText + "|" + m->entryPath + "|" +
                    std::to_string(m->newBytes.size()) + "|" + fnv1a64_hex(m->newBytes) + "|" + options.mapPath + "|" + now + "\n";
            ready.push_back({m, std::move(fileText), seq++});
        }
        ok = ready.empty() || append_journal(journal, text, why);
    }
    if (!ok) {
        for (const Ready& r : ready) member_error(*r.member, "nothing written: " + why);
        for (MemberPatch* m : members) {
            if (!m->failed) member_error(*m, "nothing written: " + why);
        }
        return;
    }

    size_t good = 0;
    for (const Ready& r : ready) {
        MemberPatch& m = *r.member;
        std::string reason;
        const fs::path temp = fs::path(m.extractTo).concat(".pextmp");
        fs::create_directories(m.extractTo.parent_path(), ec);
        if (ec) {
            reason = "cannot create the folder: " + ec.message();
        } else {
            fs::remove(temp, ec);
            RwFile out;
            if (!out.open(temp, true, reason)) {
                // reason set
            } else {
                const bool wrote = out.write_at(0, m.newBytes.data(), m.newBytes.size()) && out.flush();
                out.close();
                if (!wrote) reason = "cannot write the extracted file";
                else {
                    fs::rename(temp, m.extractTo, ec);
                    if (ec) reason = "cannot move the extracted file into place: " + ec.message();
                }
            }
        }
        if (reason.empty()) {
            if (options.afterWriteForTest) options.afterWriteForTest(m.extractTo);
            std::vector<uint8_t> back;
            if (!read_bytes(m.extractTo, back)) reason = "the extracted file cannot be read";
            else verify_bytes(back, m, options.extraCallees, reason);
        }
        if (!reason.empty()) {
            std::error_code rm;
            fs::remove(temp, rm);
            fs::remove(m.extractTo, rm);
            const bool gone = !fs::exists(m.extractTo, rm);
            std::string err;
            const bool noted = gone && append_journal(journal, "UNDONE|" + std::to_string(r.seq) + "|" + utc_now() + "\n", err);
            member_error(m, reason + (noted ? "; the extracted file was removed" : "; run pex patch --undo " + journal.u8string()));
            continue;
        }
        m.report += "PATCHED|" + r.fileText + "|" + std::to_string(m.edits.size()) + " edits, 0 renames, extracted from " + m.label + "\n";
        ++good;
    }
    members.back()->report += "JOURNAL|" + archiveText + "|" + journal.u8string() + "|" + std::to_string(good) + " extracted\n";
}

std::string bytes_hash(const std::vector<uint8_t>& bytes) { return fnv1a64_hex(bytes); }

bool write_verified_script(const fs::path& target, const std::vector<uint8_t>& bytes, const std::vector<pex::PatchEdit>& edits,
                           const std::vector<std::string>& extraCallees, std::string& why) {
    std::error_code ec;
    fs::create_directories(target.parent_path(), ec);
    if (ec) {
        why = "cannot create the folder: " + ec.message();
        return false;
    }
    const fs::path temp = fs::path(target).concat(".pextmp");
    fs::remove(temp, ec);
    {
        RwFile out;
        if (!out.open(temp, true, why)) return false;
        const bool wrote = out.write_at(0, bytes.data(), bytes.size()) && out.flush();
        out.close();
        if (!wrote) {
            fs::remove(temp, ec);
            why = "cannot write the file";
            return false;
        }
    }
    MemberPatch check;
    check.newBytes = bytes;
    check.edits = edits;
    std::vector<uint8_t> back;
    if (!read_bytes(temp, back)) why = "the written file cannot be read";
    else verify_bytes(back, check, extraCallees, why);
    if (!why.empty()) {
        fs::remove(temp, ec);
        return false;
    }
    fs::rename(temp, target, ec);
    if (ec) {
        why = "cannot move the file into place: " + ec.message();
        std::error_code rm;
        fs::remove(temp, rm);
        return false;
    }
    return true;
}

// --- the game must not be running --------------------------------------------

bool find_running_processes(const std::vector<std::string>& names, std::vector<std::string>& found, std::string& why) {
    found.clear();
#ifdef _WIN32
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        why = "cannot list the running processes (Windows error " + std::to_string(GetLastError()) + ")";
        return false;
    }
    std::set<std::wstring> wanted;
    for (const std::string& n : names) {
        std::wstring w = fs::u8path(n).wstring();
        std::transform(w.begin(), w.end(), w.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
        wanted.insert(std::move(w));
    }
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof entry;
    bool more = Process32FirstW(snapshot, &entry) != 0;
    if (!more && GetLastError() != ERROR_NO_MORE_FILES) {
        why = "cannot list the running processes (Windows error " + std::to_string(GetLastError()) + ")";
        CloseHandle(snapshot);
        return false;
    }
    for (; more; more = Process32NextW(snapshot, &entry) != 0) {
        std::wstring w = entry.szExeFile;
        std::transform(w.begin(), w.end(), w.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
        if (wanted.count(w)) found.push_back(fs::path(entry.szExeFile).u8string() + " (process " + std::to_string(entry.th32ProcessID) + ")");
    }
    CloseHandle(snapshot);
    return true;
#else
    (void)names;
    why = "listing processes is not implemented on this platform";
    return false;
#endif
}

bool verify_game_not_running(std::string& why) {
    static const std::vector<std::string> kGame = {"SkyrimSE.exe",     "SkyrimVR.exe",     "TESV.exe",
                                                   "skse64_loader.exe", "sksevr_loader.exe", "skse_loader.exe"};
    std::vector<std::string> found;
    std::string err;
    if (!find_running_processes(kGame, found, err)) {
        why = "cannot verify that the game is not running: " + err;
        return false;
    }
    if (!found.empty()) {
        why = "the game is running: ";
        for (size_t i = 0; i < found.size(); ++i) why += (i ? ", " : "") + found[i];
        why += "; close it first";
        return false;
    }
    return true;
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
    if (parsed.ranges.empty() && parsed.extracts.empty() && parsed.folders.empty()) {
        report += "UNCHANGED|" + journalText + "|nothing to undo\n";
        return 0;
    }

    std::map<std::string, std::vector<const JournalRange*>> byArchive;
    for (const JournalRange& r : parsed.ranges) byArchive[r.archive].push_back(&r);

    int failures = 0;
    std::string marks;
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
        if (perMember.count(kLooseMember)) {
            pex::Script s;
            std::string err;
            if (pex::read_file(archive, s, &err) != pex::Status::Ok) {
                report += "ERR|" + archiveText + "|restored, but does not read as a script: " + err + "\n";
                readable = false;
            }
        } else {
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
        for (const JournalRange* r : ranges) marks += "UNDONE|" + std::to_string(r->seq) + "|" + now + "\n";
        for (const auto& [member, counts] : perMember) {
            report += "UNDONE|" + archive_label(archive, member) + "|" + std::to_string(counts.first) + " ranges, " +
                      std::to_string(bytesPerMember[member]) + " bytes restored, " + std::to_string(counts.second) + " already restored\n";
        }
    }
    // Files it extracted: deleted only if they are still what it wrote.
    {
        const std::string now = utc_now();
        bool extractsFailed = false;
        for (const JournalExtract& x : parsed.extracts) {
            const fs::path file = fs::u8path(x.file);
            std::error_code ec;
            if (!fs::exists(file, ec)) {
                if (!dryRun) marks += "UNDONE|" + std::to_string(x.seq) + "|" + now + "\n";
                report += std::string(dryRun ? "DRYRUN|" : "UNDONE|") + x.file + "|extracted file already gone\n";
                continue;
            }
            std::vector<uint8_t> bytes;
            if (!read_bytes(file, bytes) || bytes.size() != x.size || fnv1a64_hex(bytes) != x.hash) {
                report += "ERR|" + x.file + "|changed since it was extracted from " + x.archive + "; left in place\n";
                ++failures;
                extractsFailed = true;
                continue;
            }
            if (dryRun) {
                report += "DRYRUN|" + x.file + "|would delete the extracted file\n";
                continue;
            }
            fs::remove(file, ec);
            if (ec) {
                report += "ERR|" + x.file + "|cannot delete the extracted file: " + ec.message() + "\n";
                ++failures;
                extractsFailed = true;
                continue;
            }
            marks += "UNDONE|" + std::to_string(x.seq) + "|" + now + "\n";
            report += "UNDONE|" + x.file + "|extracted file deleted\n";
        }
        // Folders it created, innermost first, and only when empty. While an
        // extracted file stays, its folder stays and stays in the journal.
        if (!dryRun && !extractsFailed) {
            for (auto d = parsed.folders.rbegin(); d != parsed.folders.rend(); ++d) {
                std::error_code ec;
                const fs::path folder = fs::u8path(d->folder);
                if (fs::exists(folder, ec) && !fs::remove(folder, ec)) report += "NOTE|" + d->folder + "|created by the patch, not empty now; left in place\n";
                marks += "UNDONE|" + std::to_string(d->seq) + "|" + now + "\n";
            }
        }
    }
    // Once, at the end: a folder's journal names one file per script. A stop
    // before this line leaves bytes an undo finds already restored.
    if (!marks.empty() && !append_journal(journalFile, marks, why)) {
        report += "ERR|" + journalText + "|restored, but the journal cannot record it: " + why + "\n";
        ++failures;
    }
    return failures;
}

// --- FormID pairs ------------------------------------------------------------

bool parse_formid(std::string_view text, uint32_t& out, std::string& why) {
    const std::string shown(text);
    if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        std::string_view digits = text.substr(2);
        if (digits.size() != 6 && digits.size() != 8) {
            why = "FormID " + shown + ": a hex FormID has 6 or 8 digits";
            return false;
        }
        uint32_t v = 0;
        for (char c : digits) {
            int d = -1;
            if (c >= '0' && c <= '9') d = c - '0';
            else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
            if (d < 0) {
                why = "FormID " + shown + ": not a hex number";
                return false;
            }
            v = (v << 4) | static_cast<uint32_t>(d);
        }
        out = v;
        return true;
    }
    if (text.empty()) {
        why = "an empty FormID";
        return false;
    }
    uint64_t v = 0;
    for (char c : text) {
        if ((c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')) {
            why = "FormID " + shown + ": hex needs the 0x prefix";
            return false;
        }
        if (c < '0' || c > '9') {
            why = "FormID " + shown + ": neither 0x hex nor decimal";
            return false;
        }
        v = v * 10 + static_cast<uint64_t>(c - '0');
        if (v > 0xFFFFFFFFu) {
            why = "FormID " + shown + ": does not fit 32 bits";
            return false;
        }
    }
    if (text.size() > 1 && text[0] == '0') {
        why = "FormID " + shown + ": a leading zero looks like hex without its prefix; write 0x" + shown + ", or the decimal number without the zero";
        return false;
    }
    out = static_cast<uint32_t>(v);
    return true;
}

bool add_formid_pairs(std::string_view list, pex::FormIdMap& map, std::string& why) {
    auto trim = [](std::string_view s) {
        while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
        while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
        return s;
    };
    size_t pos = 0;
    while (pos <= list.size()) {
        size_t comma = list.find(',', pos);
        if (comma == std::string_view::npos) comma = list.size();
        const std::string_view pair = trim(list.substr(pos, comma - pos));
        pos = comma + 1;
        const size_t colon = pair.find(':');
        if (colon == std::string_view::npos) {
            why = "\"" + std::string(pair) + "\" is not OLD:NEW";
            return false;
        }
        uint32_t oldId = 0, newId = 0;
        if (!parse_formid(trim(pair.substr(0, colon)), oldId, why) || !parse_formid(trim(pair.substr(colon + 1)), newId, why)) return false;
        const uint32_t key = oldId & 0xFFFFFFu, value = newId & 0xFFFFFFu;
        const auto found = map.oldToNew.find(key);
        if (found != map.oldToNew.end() && found->second != value) {
            char buf[96];
            std::snprintf(buf, sizeof buf, "%06X is mapped twice: to %06X and to %06X", static_cast<unsigned>(key),
                          static_cast<unsigned>(found->second), static_cast<unsigned>(value));
            why = buf;
            return false;
        }
        map.oldToNew[key] = value;
    }
    return true;
}

bool add_formid_pairs_file(const fs::path& file, pex::FormIdMap& map, std::string& why) {
    std::string text;
    if (!read_whole(file, text)) {
        why = "cannot read " + file.u8string();
        return false;
    }
    if (text.rfind("\xEF\xBB\xBF", 0) == 0) text.erase(0, 3);
    size_t pos = 0, lineNo = 0;
    while (pos < text.size()) {
        ++lineNo;
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) nl = text.size();
        std::string_view line(text.data() + pos, nl - pos);
        pos = nl + 1;
        while (!line.empty() && std::isspace(static_cast<unsigned char>(line.front()))) line.remove_prefix(1);
        while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back()))) line.remove_suffix(1);
        if (line.empty() || line[0] == '#') continue;
        if (!add_formid_pairs(line, map, why)) {
            why = file.u8string() + " line " + std::to_string(lineNo) + ": " + why;
            return false;
        }
    }
    return true;
}

// --- the log ------------------------------------------------------------------

std::string skip_reason_code(const std::string& why) {
    if (why.find("different copy in") != std::string::npos) return "other-copy";
    if (why.find("stored compressed") != std::string::npos) return "compressed";
    if (why.find("shared with") != std::string::npos) return "shared-block";
    if (why.find("renames the plugin") != std::string::npos) return "plugin-rename";
    if (why.find("not byte-exact") != std::string::npos) return "not-byte-exact";
    return "failed";
}

std::string render_patch_log(const LogRun& run, std::vector<LogContainer> containers) {
    std::sort(containers.begin(), containers.end(), [](const LogContainer& a, const LogContainer& b) {
        return a.archive != b.archive ? a.archive : a.path < b.path;
    });
    const char* patchedWord = run.dryRun ? "would-patch" : "patched";
    const char* extractedWord = run.dryRun ? "would-extract" : "extracted";
    std::string out = "pex patch log v2\n";
    out += "started|" + run.started + "\n";
    out += std::string("mode|") + (run.dryRun ? "dry-run" : "write") + "\n";
    for (const std::string& in : run.inputs) out += "input|" + in + "\n";
    out += "plugin|" + run.plugin + "\n";
    out += "source|" + run.source + "\n";
    out += "pairs|" + std::to_string(run.pairs) + "\n";

    size_t archives = 0, folders = 0, scanned = 0, patched = 0, extracted = 0, edits = 0, skipped = 0, notInMap = 0, dynamic = 0;
    std::map<std::string, size_t> reasons;
    for (const LogContainer& c : containers) {
        scanned += c.scanned;
        if (c.patched.empty() && c.extracted.empty() && c.skipped.empty()) continue;
        ++(c.archive ? archives : folders);
        out += "\n";
        out += std::string(c.archive ? "[archive] " : "[loose] ") + c.path + "\n";
        if (!c.journal.empty()) out += "journal|" + c.journal + "\n";
        size_t localEdits = 0;
        for (const LogFile& f : c.patched) {
            out += std::string(patchedWord) + "|" + f.name + "|" + std::to_string(f.edits) + "|";
            for (size_t i = 0; i < f.pairs.size(); ++i) out += (i ? "," : "") + f.pairs[i];
            out += "|" + std::to_string(f.notInMap) + "|" + std::to_string(f.dynamic) + "\n";
            localEdits += f.edits;
            notInMap += f.notInMap;
            dynamic += f.dynamic;
        }
        for (const LogFile& f : c.extracted) {
            out += std::string(extractedWord) + "|" + f.name + "|" + std::to_string(f.edits) + "|";
            for (size_t i = 0; i < f.pairs.size(); ++i) out += (i ? "," : "") + f.pairs[i];
            out += "|" + std::to_string(f.notInMap) + "|" + std::to_string(f.dynamic) + "|" + f.extractedTo + "\n";
            localEdits += f.edits;
            notInMap += f.notInMap;
            dynamic += f.dynamic;
        }
        for (const LogFile& f : c.skipped) {
            std::string detail = f.detail;
            std::replace(detail.begin(), detail.end(), '|', '/');
            std::replace(detail.begin(), detail.end(), '\n', ' ');
            out += "skipped|" + f.name + "|" + f.reason + "|" + detail + "\n";
            ++reasons[f.reason];
            notInMap += f.notInMap;
            dynamic += f.dynamic;
        }
        out += "subtotal|scanned " + std::to_string(c.scanned) + "|" + patchedWord + " " + std::to_string(c.patched.size()) + "|" + extractedWord + " " +
               std::to_string(c.extracted.size()) + "|edits " + std::to_string(localEdits) + "|skipped " + std::to_string(c.skipped.size()) + "\n";
        patched += c.patched.size();
        extracted += c.extracted.size();
        edits += localEdits;
        skipped += c.skipped.size();
    }
    out += "\n[totals]\n";
    out += "archives|" + std::to_string(archives) + "\n";
    out += "loose-folders|" + std::to_string(folders) + "\n";
    out += "scanned|" + std::to_string(scanned) + "\n";
    out += std::string(patchedWord) + "|" + std::to_string(patched) + "\n";
    out += std::string(extractedWord) + "|" + std::to_string(extracted) + "\n";
    out += "edits|" + std::to_string(edits) + "\n";
    out += "skipped|" + std::to_string(skipped) + "\n";
    for (const auto& [reason, n] : reasons) out += "skipped." + reason + "|" + std::to_string(n) + "\n";
    out += "refs-not-in-map|" + std::to_string(notInMap) + "\n";
    out += "refs-dynamic|" + std::to_string(dynamic) + "\n";
    return out;
}

std::string utc_timestamp() { return utc_now(); }

fs::path default_log_path(const fs::path& firstInput) {
    std::error_code ec;
    fs::path p = fs::absolute(firstInput, ec);
    if (ec) p = firstInput;
    p = p.lexically_normal();
    if (!p.has_filename()) p = p.parent_path();
    const fs::path dir = p.parent_path();
    std::string stamp = utc_now();
    stamp.erase(std::remove_if(stamp.begin(), stamp.end(), [](char c) { return c == '-' || c == ':'; }), stamp.end());
    fs::path candidate = dir / ("pex-patch-" + stamp + ".log");
    for (int n = 2; fs::exists(candidate, ec) && n < 1000; ++n) candidate = dir / ("pex-patch-" + stamp + "-" + std::to_string(n) + ".log");
    return candidate;
}

}  // namespace pextool
