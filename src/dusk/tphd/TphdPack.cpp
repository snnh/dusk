#include "TphdPack.hpp"

#include <zlib.h>

#include <cstring>
#include <exception>
#include <limits>
#include <new>
#include <stdexcept>

#include "helpers/endian.h"
#include "dusk/io.hpp"
#include "dusk/logging.h"
#include <borealis/log.hpp>

static borealis::Log TphdLog("dusk::tphd");

namespace dusk::tphd {

namespace {

// A TPHD texture sidecar is normally only a few MiB.  Keep corrupt input from
// using the gzip trailer as an allocation oracle while leaving ample room for
// legitimate, high-resolution packs.
constexpr size_t kMaxCompressedPackBytes = 512u * 1024u * 1024u;
constexpr size_t kMaxInflatedPackBytes = 512u * 1024u * 1024u;
constexpr u32 kMaxTmpkEntries = 262144;

u32 read_le_u32(const u8* p) {
    return static_cast<u32>(p[0]) |
           (static_cast<u32>(p[1]) << 8) |
           (static_cast<u32>(p[2]) << 16) |
           (static_cast<u32>(p[3]) << 24);
}

}  // namespace

std::optional<std::vector<u8>> decompressGzip(std::span<const u8> in) {
    if (in.size() < 18) return std::nullopt;
    if (in[0] != 0x1F || in[1] != 0x8B) return std::nullopt;
    if (in.size() > kMaxCompressedPackBytes ||
        in.size() > std::numeric_limits<uInt>::max()) return std::nullopt;

    const u32 isize = read_le_u32(in.data() + in.size() - sizeof(u32));
    if (isize == 0 || isize > kMaxInflatedPackBytes) return std::nullopt;

    std::vector<u8> out;
    try {
        out.resize(isize);
    } catch (const std::bad_alloc&) {
        return std::nullopt;
    } catch (const std::length_error&) {
        return std::nullopt;
    }

    z_stream strm{};
    strm.next_in  = const_cast<Bytef*>(in.data());
    strm.avail_in = static_cast<uInt>(in.size());
    strm.next_out = out.data();
    strm.avail_out = static_cast<uInt>(out.size());

    if (inflateInit2(&strm, 15 + 16) != Z_OK) return std::nullopt;
    int rc = inflate(&strm, Z_FINISH);
    const uLong totalOut = strm.total_out;
    const uInt remainingIn = strm.avail_in;
    inflateEnd(&strm);
    if (rc != Z_STREAM_END || totalOut != out.size() || remainingIn != 0) {
        return std::nullopt;
    }
    return out;
}

std::vector<TmpkEntry> parseTmpk(std::span<const u8> in) {
    std::vector<TmpkEntry> out;
    if (in.size() < sizeof(TmpkRawHeader)) return out;

    const auto* hdr = reinterpret_cast<const TmpkRawHeader*>(in.data());
    if (std::memcmp(hdr->magic, "TMPK", 4) != 0) return out;

    const u32 count = hdr->count;
    if (count == 0 || count > kMaxTmpkEntries ||
        count > (in.size() - sizeof(TmpkRawHeader)) / sizeof(TmpkRawEntry)) {
        return out;
    }

    const auto* entries = reinterpret_cast<const TmpkRawEntry*>(
        in.data() + sizeof(TmpkRawHeader));

    try {
        out.reserve(count);
    } catch (const std::bad_alloc&) {
        return {};
    } catch (const std::length_error&) {
        return {};
    }
    for (u32 i = 0; i < count; ++i) {
        const u32 nameOff  = entries[i].nameOff;
        const u32 dataOff  = entries[i].dataOff;
        const u32 dataSize = entries[i].dataSize;
        const u32 flags    = entries[i].flags;

        if (nameOff >= in.size() || dataOff > in.size() ||
            dataSize > in.size() - dataOff) {
            return {};
        }

        const char* nameStart = reinterpret_cast<const char*>(in.data() + nameOff);
        size_t maxLen = in.size() - nameOff;
        const void* nul = std::memchr(nameStart, 0, maxLen);
        if (nul == nullptr) return {};
        const size_t nameLen = static_cast<size_t>(static_cast<const char*>(nul) - nameStart);
        if (nameLen == 0) return {};

        out.push_back({
            std::string_view(nameStart, nameLen),
            in.subspan(dataOff, dataSize),
            flags,
        });
    }
    return out.size() == count ? out : std::vector<TmpkEntry>{};
}

std::optional<TphdPack> TphdPack::loadFromMemory(std::span<const u8> gzipBytes) {
    auto inflated = decompressGzip(gzipBytes);
    if (!inflated) return std::nullopt;

    TphdPack p;
    p.m_buffer = std::move(*inflated);
    p.m_entries = parseTmpk(std::span<const u8>(p.m_buffer.data(), p.m_buffer.size()));
    if (p.m_entries.empty()) {
        TphdLog.warn("Rejected malformed or empty TMPK pack (buffer size {})", p.m_buffer.size());
        return std::nullopt;
    }
    return p;
}

std::optional<TphdPack> TphdPack::loadFromFile(const std::filesystem::path& path) {
    try {
        return loadFromMemory(io::FileStream::ReadAllBytes(path));
    } catch (const std::exception& e) {
        TphdLog.error("Failed to read {}: {}", io::fs_path_to_string(path), e.what());
        return std::nullopt;
    }
}

const TmpkEntry* TphdPack::find(std::string_view name) const {
    for (const auto& e : m_entries) {
        if (e.name == name) return &e;
    }
    return nullptr;
}

}
