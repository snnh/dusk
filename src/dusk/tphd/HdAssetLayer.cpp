#include "HdAssetLayer.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <exception>
#include <list>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <unordered_map>
#include <vector>

#include <aurora/dvd.h>
#include <aurora/texture.hpp>
#include <dolphin/dvd.h>
#include <SDL3/SDL_error.h>
#include <SDL3/SDL_iostream.h>

#include "JSystem/J3DGraphLoader/J3DModelLoader.h"
#include "JSystem/JKernel/JKRArchive.h"
#include "JSystem/JUtility/JUTTexture.h"
#include "helpers/endian.h"
#include "dusk/io.hpp"
#include "dusk/logging.h"
#include "AddrLib.hpp"
#include "GtxParser.hpp"
#include "LosTable.hpp"
#include "TphdPack.hpp"
#include "tracy/Tracy.hpp"

static aurora::Module HdLog("dusk::tphd::hd");

namespace dusk::tphd {

namespace {

constexpr size_t kMaxHdArchiveBytes = 512u * 1024u * 1024u;
constexpr size_t kMaxDecodedTextureBytes = 256u * 1024u * 1024u;

std::filesystem::path g_contentPath;
uint64_t g_contentGeneration = 0;
std::mutex g_cacheMutex;

// Heap-allocated, never freed — these must outlive g_dComIfG_gameInfo's
// static destructor which holds JKRArchives referencing these bytes.
std::list<std::vector<u8>>& g_mountBuffers() {
    static auto* p = new std::list<std::vector<u8>>{};
    return *p;
}

std::unordered_map<std::string, std::vector<u8>*>& g_mountBuffersByPath() {
    static auto* p = new std::unordered_map<std::string, std::vector<u8>*>{};
    return *p;
}

std::list<std::vector<u8>>& g_textureBuffers() {
    static auto* p = new std::list<std::vector<u8>>{};
    return *p;
}

std::unordered_map<std::string, std::shared_ptr<const TphdPack>>& g_packCache() {
    static auto* p = new std::unordered_map<std::string, std::shared_ptr<const TphdPack>>{};
    return *p;
}

aurora::texture::ReplacementGroup& g_textureReplacementGroup() {
    static auto* p = new aurora::texture::ReplacementGroup{};
    return *p;
}

struct RegisteredTexture {
    std::list<std::vector<u8>>::iterator buffer;
    aurora::texture::ReplacementRegistration registration;
};

std::unordered_map<const void*, RegisteredTexture>& g_registeredTextures() {
    static auto* p = new std::unordered_map<const void*, RegisteredTexture>{};
    return *p;
}

std::unordered_map<std::string, const void*>& g_logicalTexturePointers() {
    static auto* p = new std::unordered_map<std::string, const void*>{};
    return *p;
}

struct HdArcRange {
    const void* begin = nullptr;
    size_t size = 0;
    std::string label;
};

std::vector<HdArcRange>& g_arcRanges() {
    static auto* p = new std::vector<HdArcRange>{};
    return *p;
}

struct HdOverlayEntry {
    std::string dvdPath;
    std::filesystem::path arcPath;
    std::filesystem::path packPath;
    size_t size = 0;
};

std::list<HdOverlayEntry>& g_overlayEntries() {
    static auto* p = new std::list<HdOverlayEntry>{};
    return *p;
}

std::unordered_map<s32, HdOverlayEntry*>& g_entryNumToOverlay() {
    static auto* p = new std::unordered_map<s32, HdOverlayEntry*>{};
    return *p;
}

bool g_overlayCallbacksRegistered = false;

void clear_hd_texture_registrations_locked() {
    aurora::texture::unregister_replacements(g_textureReplacementGroup());
    g_textureReplacementGroup().registrations.clear();
    g_textureBuffers().clear();
    g_registeredTextures().clear();
    g_logicalTexturePointers().clear();
}

void unregister_registered_texture_locked(const void* pixelPtr) {
    const auto existing = g_registeredTextures().find(pixelPtr);
    if (existing == g_registeredTextures().end()) return;
    aurora::texture::unregister_replacement(existing->second.registration);
    auto& registrations = g_textureReplacementGroup().registrations;
    registrations.erase(std::remove_if(registrations.begin(), registrations.end(),
        [&](const aurora::texture::ReplacementRegistration& registration) {
            return registration.id == existing->second.registration.id;
        }), registrations.end());
    g_textureBuffers().erase(existing->second.buffer);
    g_registeredTextures().erase(existing);
    auto& logicalPointers = g_logicalTexturePointers();
    for (auto it = logicalPointers.begin(); it != logicalPointers.end();) {
        if (it->second == pixelPtr) {
            it = logicalPointers.erase(it);
        } else {
            ++it;
        }
    }
}

void register_hd_arc_range_locked(const void* begin, size_t size, std::string_view label) {
    if (begin == nullptr || size == 0) return;
    const auto existing = std::find_if(g_arcRanges().begin(), g_arcRanges().end(),
        [&](const HdArcRange& range) {
            return range.begin == begin && range.size == size;
        });
    if (existing != g_arcRanges().end()) return;
    g_arcRanges().push_back({
        .begin = begin,
        .size = size,
        .label = std::string(label),
    });
}

bool endsWithSuffix(std::string_view s, std::string_view suffix) {
    return s.size() >= suffix.size() &&
           s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool path_exists(const std::filesystem::path& path) {
    std::error_code ec;
    return std::filesystem::is_regular_file(path, ec);
}

struct SDL_IODeleter {
    void operator()(SDL_IOStream* io) const {
        if (io != nullptr) {
            SDL_CloseIO(io);
        }
    }
};

using IOStream = std::unique_ptr<SDL_IOStream, SDL_IODeleter>;

IOStream open_stream(const std::filesystem::path& path) {
    const auto pathString = io::fs_path_to_string(path);
    return IOStream{SDL_IOFromFile(pathString.c_str(), "rb")};
}

std::optional<size_t> get_file_size(const std::filesystem::path& path) {
    auto stream = open_stream(path);
    if (stream == nullptr) {
        return std::nullopt;
    }

    const Sint64 size = SDL_GetIOSize(stream.get());
    if (size < 0 || static_cast<uint64_t>(size) > kMaxHdArchiveBytes ||
        static_cast<uint64_t>(size) > std::numeric_limits<size_t>::max()) {
        return std::nullopt;
    }
    return static_cast<size_t>(size);
}

// On-disk Yaz0 file header.
struct Yaz0Header {
    /* 0x00 */ char magic[4];     // "Yaz0"
    /* 0x04 */ BE(u32) decompressedSize;
    /* 0x08 */ u8 pad[8];
};
static_assert(sizeof(Yaz0Header) == 0x10);

bool isYaz0(std::span<const u8> bytes) {
    return bytes.size() >= sizeof(Yaz0Header) &&
           std::memcmp(bytes.data(), "Yaz0", 4) == 0;
}

// Bounded Yaz0 decoder.  JKRDecomp::decodeSZS only receives an output length,
// so calling it on a truncated file lets it read beyond the input buffer.
std::optional<std::vector<u8>> tryDecodeYaz0(std::span<const u8> bytes) {
    if (bytes.size() < sizeof(Yaz0Header) ||
        std::memcmp(bytes.data(), "Yaz0", 4) != 0) {
        return std::nullopt;
    }
    ZoneScoped;
    const auto* hdr = reinterpret_cast<const Yaz0Header*>(bytes.data());
    const u32 expandedSize = hdr->decompressedSize;
    if (expandedSize == 0 || expandedSize > kMaxHdArchiveBytes) {
        return std::nullopt;
    }

    std::vector<u8> decoded;
    try {
        decoded.resize(expandedSize);
    } catch (const std::bad_alloc&) {
        return std::nullopt;
    } catch (const std::length_error&) {
        return std::nullopt;
    }

    size_t srcPos = sizeof(Yaz0Header);
    size_t dstPos = 0;
    while (dstPos < decoded.size()) {
        if (srcPos >= bytes.size()) return std::nullopt;
        const u8 code = bytes[srcPos++];
        for (u8 bit = 0x80; bit != 0 && dstPos < decoded.size(); bit >>= 1) {
            if (code & bit) {
                if (srcPos >= bytes.size()) return std::nullopt;
                decoded[dstPos++] = bytes[srcPos++];
                continue;
            }

            if (bytes.size() - srcPos < 2) return std::nullopt;
            const u8 first = bytes[srcPos++];
            const u8 second = bytes[srcPos++];
            const size_t distance = (static_cast<size_t>(first & 0x0F) << 8) | second;
            if (distance >= dstPos) return std::nullopt;

            size_t count = first >> 4;
            if (count == 0) {
                if (srcPos >= bytes.size()) return std::nullopt;
                count = static_cast<size_t>(bytes[srcPos++]) + 0x12;
            } else {
                count += 2;
            }
            if (count > decoded.size() - dstPos) return std::nullopt;

            size_t copyPos = dstPos - distance - 1;
            for (size_t i = 0; i < count; ++i) {
                decoded[dstPos++] = decoded[copyPos++];
            }
        }
    }
    return decoded;
}

std::optional<std::vector<u8>> read_file(const std::filesystem::path& path,
                                         size_t maxSize = kMaxHdArchiveBytes) {
    auto stream = open_stream(path);
    if (stream == nullptr) {
        return std::nullopt;
    }
    const Sint64 len = SDL_GetIOSize(stream.get());
    if (len < 0 || static_cast<uint64_t>(len) > maxSize) {
        return std::nullopt;
    }
    std::vector<u8> buf;
    try {
        buf.resize(static_cast<size_t>(len));
    } catch (const std::bad_alloc&) {
        return std::nullopt;
    } catch (const std::length_error&) {
        return std::nullopt;
    }
    size_t total = 0;
    while (total < buf.size()) {
        const size_t got = SDL_ReadIO(stream.get(), buf.data() + total, buf.size() - total);
        if (got == 0) {
            break;
        }
        total += got;
    }
    if (total != buf.size() || SDL_GetIOStatus(stream.get()) == SDL_IO_STATUS_ERROR) {
        return std::nullopt;
    }
    return buf;
}

std::optional<TphdPack> load_pack_from_file(const std::filesystem::path& path) {
    auto raw = read_file(path);
    if (!raw) {
        return std::nullopt;
    }
    return TphdPack::loadFromMemory(*raw);
}

std::shared_ptr<const TphdPack> load_pack_cached(const std::filesystem::path& path) {
    const auto key = io::fs_path_to_string(path);
    {
        std::lock_guard lk{g_cacheMutex};
        const auto it = g_packCache().find(key);
        if (it != g_packCache().end()) {
            return it->second;
        }
    }

    auto loaded = load_pack_from_file(path);
    if (!loaded) {
        return {};
    }

    auto pack = std::make_shared<TphdPack>(std::move(*loaded));
    {
        std::lock_guard lk{g_cacheMutex};
        auto [it, inserted] = g_packCache().emplace(key, pack);
        if (!inserted) {
            return it->second;
        }
    }
    return pack;
}

// Extract the path portion under "res/" from JSystem's absolute path.
// Example: "/arcName/res/Stage/D_SB10/R00_00.arc" -> "res/Stage/D_SB10/R00_00.arc"
std::string_view extractResPath(std::string_view gcPath) {
    auto p = gcPath.find("res/");
    if (p == std::string_view::npos) return {};
    return gcPath.substr(p);
}


// Case-insensitive ASCII suffix match — RARC archives lowercase filenames
// at build time, but our HD pack.gz preserves the original Wii-U authoring
// camelCase. Example: RARC has "coverbg.bti", pack has "coverBG.bti.gtx".
bool endsWithSuffixCI(std::string_view s, std::string_view suffix) {
    if (s.size() < suffix.size()) return false;
    auto toLower = [](unsigned char c) -> unsigned char {
        return (c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c;
    };
    const char* a = s.data() + (s.size() - suffix.size());
    for (size_t i = 0; i < suffix.size(); ++i) {
        if (toLower(a[i]) != toLower(suffix[i])) return false;
    }
    return true;
}

// Match an arc-relative path (e.g. "bmdr/model.bmd") against the Gfx2 entries
// in the HD pack, which look like "tex/.../<arc-rel>.gtx".
const TmpkEntry* findGtxBySuffix(const TphdPack& pack, std::string_view arcRelPath) {
    auto findFirst = [&](std::string_view tail) -> const TmpkEntry* {
        for (const auto& e : pack.entries()) {
            if (e.data.size() < 4 || std::memcmp(e.data.data(), "Gfx2", 4) != 0) continue;
            if (endsWithSuffixCI(e.name, tail)) return &e;
        }
        return nullptr;
    };
    auto findUnique = [&](std::string_view tail) -> const TmpkEntry* {
        const TmpkEntry* found = nullptr;
        for (const auto& e : pack.entries()) {
            if (e.data.size() < 4 || std::memcmp(e.data.data(), "Gfx2", 4) != 0) continue;
            if (!endsWithSuffixCI(e.name, tail)) continue;
            if (found != nullptr) return nullptr;
            found = &e;
        }
        return found;
    };

    const std::string tail = "/" + std::string(arcRelPath) + ".gtx";
    if (const TmpkEntry* gtx = findFirst(tail)) {
        return gtx;
    }

    const auto slash = arcRelPath.find_last_of('/');
    if (slash == std::string_view::npos) {
        return nullptr;
    }

    const std::string basenameTail = "/" + std::string(arcRelPath.substr(slash + 1)) + ".gtx";
    return findUnique(basenameTail);
}

// Post-deswizzle CPU expansions to RGBA8. Used for formats whose HD layout
// can't be directly sampled with a GPU view swizzle (IA4 nibble unpack,
// RGB565 16-bit), and as a fallback if R8_PC/RG8_PC view swizzle isn't
// available. GC sampling semantics: I8 -> (I,I,I,I); IA4/IA8 -> (I,I,I,A).

std::vector<u8> expandR5G6B5toRgba8(std::span<const u8> in, u32 width, u32 height) {
    std::vector<u8> out(static_cast<size_t>(width) * height * 4);
    const size_t pixelCount = static_cast<size_t>(width) * height;
    for (size_t i = 0; i < pixelCount && (i * 2 + 1) < in.size(); ++i) {
        // GX2 stores RGB565 pixel data in GPU-native LE
        u16 px;
        std::memcpy(&px, &in[i * 2], sizeof(px));
        u8 b5 = static_cast<u8>((px >> 11) & 0x1F);
        u8 g6 = static_cast<u8>((px >> 5) & 0x3F);
        u8 r5 = static_cast<u8>(px & 0x1F);
        out[i * 4 + 0] = static_cast<u8>((r5 << 3) | (r5 >> 2));
        out[i * 4 + 1] = static_cast<u8>((g6 << 2) | (g6 >> 4));
        out[i * 4 + 2] = static_cast<u8>((b5 << 3) | (b5 >> 2));
        out[i * 4 + 3] = 0xFF;
    }
    return out;
}

// IA4: high nibble = A, low nibble = I (matches aurora's GC IA4 decoder).
std::vector<u8> expandIA4toRgba8(std::span<const u8> in, u32 width, u32 height) {
    std::vector<u8> out(static_cast<size_t>(width) * height * 4);
    const size_t pixelCount = static_cast<size_t>(width) * height;
    for (size_t i = 0; i < pixelCount && i < in.size(); ++i) {
        u8 b = in[i];
        u8 A = static_cast<u8>((b & 0xF0) | (b >> 4));
        u8 I = static_cast<u8>(((b & 0x0F) << 4) | (b & 0x0F));
        out[i * 4 + 0] = I; out[i * 4 + 1] = I; out[i * 4 + 2] = I; out[i * 4 + 3] = A;
    }
    return out;
}

enum class Expansion {
    None,
    R5G6B5_to_RGBA8,
    IA4_to_RGBA8,
};

struct Gx2FormatMapping {
    u32 gx2Format;       // GX2 surface format
    u8 newGxFormat;      // Aurora PC-target format
    u32 bpp;             // Deswizzle bits-per-pixel (per pixel, or per 4x4 block for BCn)
    bool isBcn;
    Expansion expansion; // Optional post-deswizzle CPU expansion
};

// I8/IA8 pass through as R8_PC/RG8_PC (aurora applies .rrrr/.rrrg view
// swizzle on the GPU side — half / quarter the VRAM of CPU-expanded RGBA8).
// IA4 + RGB565 need CPU expansion (nibble / 16-bit unpack). CMPR stays
// BC1_PC (compressed on the GPU).
constexpr Gx2FormatMapping kFormatMap[] = {
    // gx2 fmt          PC target            bpp  isBcn  expansion
    { 0x01 /* I8     */, 0x41 /* R8_PC    */,  8,  false, Expansion::None },
    { 0x02 /* IA4    */, 0x46 /* RGBA8_PC */,  8,  false, Expansion::IA4_to_RGBA8 },
    { 0x07 /* IA8    */, 0x43 /* RG8_PC   */, 16,  false, Expansion::None },
    { 0x08 /* RGB565 */, 0x46 /* RGBA8_PC */, 16,  false, Expansion::R5G6B5_to_RGBA8 },
    { 0x1A /* RGBA8  */, 0x46 /* RGBA8_PC */, 32,  false, Expansion::None },
    { 0x31 /* CMPR   */, 0x4E /* BC1_PC   */, 64,  true,  Expansion::None },
};

const Gx2FormatMapping* findFormatMapping(u32 gx2Format) {
    for (const auto& m : kFormatMap) {
        if (m.gx2Format == gx2Format) return &m;
    }
    return nullptr;
}

std::vector<u8> applyExpansion(Expansion exp, std::vector<u8> linear, u32 w, u32 h) {
    switch (exp) {
    case Expansion::R5G6B5_to_RGBA8: return expandR5G6B5toRgba8(linear, w, h);
    case Expansion::IA4_to_RGBA8:    return expandIA4toRgba8(linear, w, h);
    case Expansion::None: break;
    }
    return linear;
}

// Per-mip tile-mode + pitch. Demote rule mirrored from decaf-emu's
// R600AddrLib::ComputeSurfaceMipLevelTileMode (MIT, AMD-derived) — see
// AddrLib.cpp header for the full copyright notice.
//
// R700 macro-tile size: 32 × 16 elements (BCN element = 4×4 block).
// Mips below that are demoted to Tiled1DThin1 (microtile-only, 8-element
// align).
struct MipLevelDesc {
    u32 width;
    u32 height;
    u32 pitch;
    addrlib::TileMode tileMode;
};

MipLevelDesc mipLevelDesc(const GtxSurface& s, u32 level, bool isBcn, u32 bpp) {
    MipLevelDesc d{};
    d.width    = std::max(1u, s.width  >> level);
    d.height   = std::max(1u, s.height >> level);

    if (level == 0) {
        d.pitch    = s.pitch;
        d.tileMode = static_cast<addrlib::TileMode>(s.tileMode);
        return d;
    }

    const addrlib::SurfaceInfoIn si{
        .width    = s.width,
        .height   = s.height,
        .bpp      = bpp,
        .mipLevel = level,
        .tileMode = static_cast<addrlib::TileMode>(s.tileMode),
        .isBcn    = isBcn,
    };
    addrlib::SurfaceInfoOut so{};
    addrlib::computeSurfaceInfo(si, so);
    d.pitch    = so.pitch;     // block units for BCN, pixel units for plain.
    d.tileMode = so.tileMode;
    return d;
}

// Slice the bytes for a single mip level. Wii-U quirk: mipOffsets[0] is
// often image_size, not a mipData offset. Level 1
// always starts at 0 in mipData; level >= 2 uses mipOffsets[level - 1].
std::span<const u8> mipLevelData(const GtxSurface& s, u32 level) {
    if (level == 0) return s.baseData;
    if (level >= s.mipCount) return {};

    u32 start = 0;
    if (level >= 2 && level - 1 < s.mipOffsets.size()) {
        start = s.mipOffsets[level - 1];
    }
    if (start >= s.mipData.size()) return {};

    u32 end = static_cast<u32>(s.mipData.size());
    if (level + 1 < s.mipCount && level < s.mipOffsets.size()) {
        const u32 next = s.mipOffsets[level];
        if (next > start && next <= s.mipData.size()) end = next;
    }
    return s.mipData.subspan(start, end - start);
}

struct DeswizzleResult {
    std::vector<u8> bytes;
    u32 mipCount;
};

DeswizzleResult deswizzleAllMips(const Gx2FormatMapping& m, const GtxSurface& s) {
    ZoneScoped;
    try {
        DeswizzleResult out{};
        const u32 maxLevels = std::min(s.mipCount, 13u);
        for (u32 level = 0; level < maxLevels; ++level) {
            const std::span<const u8> slice = mipLevelData(s, level);
            if (slice.empty()) return {};

            const MipLevelDesc lvl = mipLevelDesc(s, level, m.isBcn, m.bpp);
            const addrlib::SurfaceDesc desc{
                .width    = lvl.width,
                .height   = lvl.height,
                .pitch    = lvl.pitch,
                .bpp      = m.bpp,
                .tileMode = lvl.tileMode,
                .swizzle  = s.swizzle,
                .isBcn    = m.isBcn,
                .isDepth  = false,
            };

            auto deswizzled = addrlib::deswizzle(desc, slice);
            if (!deswizzled) return {};
            if (m.expansion != Expansion::None &&
                static_cast<size_t>(lvl.width) * lvl.height > kMaxDecodedTextureBytes / 4) {
                return {};
            }
            auto linear = applyExpansion(m.expansion, std::move(*deswizzled),
                                         lvl.width, lvl.height);
            if (linear.size() > kMaxDecodedTextureBytes - out.bytes.size()) return {};
            out.bytes.insert(out.bytes.end(), linear.begin(), linear.end());
            out.mipCount = level + 1;
        }
        return out;
    } catch (const std::bad_alloc&) {
        return {};
    } catch (const std::length_error&) {
        return {};
    }
}

void registerHdSurface(const Gx2FormatMapping& m, const GtxSurface& s,
                       const void* pixelPtr, std::string_view gtxName,
                       u32 surfaceIdx, bool replaceExistingPointer = false,
                       std::string_view logicalKey = {}) {
    ZoneScoped;
    auto decoded = deswizzleAllMips(m, s);

    HdLog.info("HD reg: ptr={} fmt=0x{:02X} {}x{} mips={}/{} bytes={} gtx={}[{}]",
               pixelPtr, m.newGxFormat, s.width, s.height,
               decoded.mipCount, s.mipCount, decoded.bytes.size(),
               gtxName, surfaceIdx);

    if (decoded.bytes.empty() || pixelPtr == nullptr) {
        return;
    }

    std::lock_guard lk{g_cacheMutex};
    if (!logicalKey.empty()) {
        const auto logical = g_logicalTexturePointers().find(logicalKey);
        if (logical != g_logicalTexturePointers().end() && logical->second != pixelPtr) {
            unregister_registered_texture_locked(logical->second);
        }
    }
    if (g_registeredTextures().contains(pixelPtr)) {
        if (!replaceExistingPointer && logicalKey.empty()) {
            return;
        }
        unregister_registered_texture_locked(pixelPtr);
    }

    try {
        g_textureBuffers().emplace_back(std::move(decoded.bytes));
    } catch (const std::bad_alloc&) {
        return;
    } catch (const std::length_error&) {
        return;
    }
    auto bytesIt = std::prev(g_textureBuffers().end());
    const auto& bytes = *bytesIt;
    const aurora::texture::RawTextureReplacement replacement{
        .bytes = {bytes.data(), bytes.size()},
        .width = s.width,
        .height = s.height,
        .mipCount = std::max(decoded.mipCount, 1u),
        .gxFormat = m.newGxFormat,
        .label = gtxName,
    };

    aurora::texture::ReplacementKey replacementKey{
        aurora::texture::TexturePointerKey{.data = pixelPtr},
    };
    if (replaceExistingPointer) {
        aurora::texture::unregister_replacements(replacementKey);
    }
    auto registration = aurora::texture::register_replacement(std::move(replacementKey), replacement);
    if (registration.id != 0) {
        try {
            g_textureReplacementGroup().registrations.push_back(registration);
            g_registeredTextures().emplace(pixelPtr, RegisteredTexture{
                .buffer = bytesIt,
                .registration = registration,
            });
            if (!logicalKey.empty()) {
                g_logicalTexturePointers()[std::string(logicalKey)] = pixelPtr;
            }
        } catch (const std::bad_alloc&) {
            aurora::texture::unregister_replacement(registration);
            auto& registrations = g_textureReplacementGroup().registrations;
            registrations.erase(std::remove_if(registrations.begin(), registrations.end(),
                [&](const aurora::texture::ReplacementRegistration& item) {
                    return item.id == registration.id;
                }), registrations.end());
            g_registeredTextures().erase(pixelPtr);
            g_textureBuffers().erase(bytesIt);
        } catch (const std::length_error&) {
            aurora::texture::unregister_replacement(registration);
            auto& registrations = g_textureReplacementGroup().registrations;
            registrations.erase(std::remove_if(registrations.begin(), registrations.end(),
                [&](const aurora::texture::ReplacementRegistration& item) {
                    return item.id == registration.id;
                }), registrations.end());
            g_registeredTextures().erase(pixelPtr);
            g_textureBuffers().erase(bytesIt);
        }
    } else {
        g_textureBuffers().erase(bytesIt);
    }
}

bool register_hd_bti_replacement_for_buffer(const TphdPack& pack, std::string_view resourceName,
    void* buffer, size_t resourceSize, bool replaceExistingPointer,
    std::string_view logicalKey = {}) {
    if (buffer == nullptr || resourceSize <= 0x20 || !endsWithSuffixCI(resourceName, ".bti")) {
        return false;
    }

    const TmpkEntry* gtx = findGtxBySuffix(pack, resourceName);
    if (!gtx) {
        return false;
    }

    auto surfaces = parseGtx(gtx->data);
    if (surfaces.empty()) {
        return false;
    }

    const auto& s = surfaces[0];
    if (s.baseData.empty()) {
        return false;
    }

    const Gx2FormatMapping* m = findFormatMapping(s.format);
    if (!m) {
        return false;
    }

    auto* timg = reinterpret_cast<ResTIMG*>(buffer);
    timg->imageOffset = 0x20;
    const u8 hdMips = static_cast<u8>(std::clamp<u32>(s.mipCount, 1u, 11u));
    timg->mipmapCount = hdMips;
    timg->maxLOD = static_cast<s8>((hdMips - 1) * 8);
    registerHdSurface(*m, s, static_cast<u8*>(buffer) + 0x20, gtx->name, 0,
                      replaceExistingPointer, logicalKey);
    return true;
}

// Absolute offset of slot `slotIdx`'s BTI header within a BMD's TEX1 block.
// Returns 0 on failure (the TEX1 table never sits at offset 0, so 0 is a
// safe sentinel).
u32 bmdSlotBtiOffset(std::span<const u8> bmd, u32 slotIdx) {
    constexpr size_t kBlocksOffset = offsetof(J3DModelFileData, mBlocks);  // = 0x20
    if (bmd.size() < kBlocksOffset ||
        std::memcmp(bmd.data(), "J3D2", 4) != 0) return 0;

    const auto* fileData = reinterpret_cast<const J3DModelFileData*>(bmd.data());
    const u32 numSections = fileData->mBlockNum;
    size_t pos = kBlocksOffset;

    for (u32 i = 0; i < numSections &&
         sizeof(J3DModelBlock) <= bmd.size() - pos; ++i) {
        const auto* blk = reinterpret_cast<const J3DModelBlock*>(bmd.data() + pos);
        const u32 blockSize = blk->mBlockSize;
        if (blockSize < sizeof(J3DModelBlock) || blockSize > bmd.size() - pos) return 0;
        if (blk->mBlockType == 'TEX1') {
            if (blockSize < sizeof(J3DTextureBlock)) return 0;
            const auto* tex1 = reinterpret_cast<const J3DTextureBlock*>(bmd.data() + pos);
            const u16 numTex = tex1->mTextureNum;
            if (slotIdx >= numTex) return 0;
            const size_t textureOffset = tex1->mpTextureRes;
            const size_t slotOffset = static_cast<size_t>(slotIdx) * 0x20;
            if (textureOffset > bmd.size() - pos ||
                slotOffset > bmd.size() - pos - textureOffset) return 0;
            const size_t btiAbs = pos + textureOffset + slotOffset;
            if (0x20 > bmd.size() - btiAbs) return 0;
            return static_cast<u32>(btiAbs);
        }
        pos += blockSize;
    }
    return 0;
}

size_t register_hd_bmd_textures_for_buffer(const TphdPack& pack, std::string_view resourceName,
    void* buffer, size_t resourceSize, bool replaceExistingPointer,
    std::string_view logicalKey = {}) {
    if (buffer == nullptr || resourceSize < 0x20) return 0;
    if (!endsWithSuffixCI(resourceName, ".bmd") &&
        !endsWithSuffixCI(resourceName, ".bdl")) return 0;

    const TmpkEntry* gtx = findGtxBySuffix(pack, resourceName);
    if (gtx == nullptr) return 0;

    std::span<u8> bmdBytes(static_cast<u8*>(buffer), resourceSize);
    auto surfaces = parseGtx(gtx->data);
    size_t reg = 0;
    for (u32 i = 0; i < surfaces.size(); ++i) {
        const auto& s = surfaces[i];
        if (s.baseData.empty()) continue;
        const Gx2FormatMapping* m = findFormatMapping(s.format);
        if (!m) continue;

        // HD-stub BMDs collapse every BTI's imageOffset to the same
        // pixel address. Rewrite each to be slot-unique so our pointer
        // map doesn't overwrite.
        const u32 btiAbs = bmdSlotBtiOffset(bmdBytes, i);
        if (btiAbs == 0) continue;
        auto* timg = reinterpret_cast<ResTIMG*>(bmdBytes.data() + btiAbs);
        if (timg->imageOffset == 0) {
            HdLog.debug("Skip cross-arc placeholder slot {} in {}: "
                        "imageOffset==0",
                        i, gtx->name);
            continue;
        }

        const size_t newImgOff = 0x20 + static_cast<size_t>(i) * 0x20;
        if (newImgOff >= resourceSize - btiAbs) continue;
        timg->imageOffset = static_cast<s32>(newImgOff);
        const u8 hdMips = static_cast<u8>(std::clamp<u32>(s.mipCount, 1u, 11u));
        timg->mipmapCount = hdMips;
        timg->maxLOD = static_cast<s8>((hdMips - 1) * 8);
        timg->maxAnisotropy = GX_ANISO_4;
        std::string slotKey;
        if (!logicalKey.empty()) slotKey = std::string(logicalKey) + "/" + std::to_string(i);
        registerHdSurface(*m, s, bmdBytes.data() + btiAbs + newImgOff, gtx->name, i,
                          replaceExistingPointer, slotKey);
        ++reg;
    }
    return reg;
}

// Lightweight RARC walker that returns per-file offsets without copying
// arc bytes — we need absolute pointers into the cached HD arc bytes
// (stable address) to match what the game later passes to GXInitTexObj.
struct ArcFileInfo {
    std::string path;  // e.g. "bmdr/model.bmd"
    u32 dataOffset;    // absolute offset from arc base
    u32 dataSize;
};

bool has_nul_terminated_string(std::span<const u8> table, u32 offset) {
    if (offset >= table.size()) return false;
    return std::memchr(table.data() + offset, 0, table.size() - offset) != nullptr;
}

// JKRMemArchive trusts all of these fields after only checking the RARC
// signature.  Validate the entire table layout before an HD file can replace
// a vanilla archive.
bool is_valid_rarc_bytes(std::span<const u8> arc) {
    constexpr size_t kMetaBase = sizeof(SArcHeader);
    constexpr u32 kMaxRarcNodes = 16384;
    constexpr u32 kMaxRarcFiles = 65536;
    if (arc.size() < kMetaBase + sizeof(SArcDataInfo) ||
        std::memcmp(arc.data(), "RARC", 4) != 0 ||
        reinterpret_cast<uintptr_t>(arc.data()) % alignof(SArcHeader) != 0) {
        return false;
    }

    const auto* hdr = reinterpret_cast<const SArcHeader*>(arc.data());
    const size_t headerLength = hdr->header_length;
    const size_t fileLength = hdr->file_length;
    if (headerLength != kMetaBase || fileLength < headerLength + sizeof(SArcDataInfo) ||
        fileLength > arc.size()) {
        return false;
    }

    const auto* dataInfo = reinterpret_cast<const SArcDataInfo*>(arc.data() + headerLength);
    if (reinterpret_cast<uintptr_t>(dataInfo) % alignof(SArcDataInfo) != 0) return false;
    const u32 nodeCount = dataInfo->num_nodes;
    const u32 fileCount = dataInfo->num_file_entries;
    const u32 stringSize = dataInfo->string_table_length;
    if (nodeCount == 0 || nodeCount > kMaxRarcNodes ||
        fileCount > kMaxRarcFiles || stringSize == 0) {
        return false;
    }

    const size_t nodeTbl = headerLength + static_cast<size_t>(dataInfo->node_offset);
    const size_t fileTbl = headerLength + static_cast<size_t>(dataInfo->file_entry_offset);
    const size_t strTbl = headerLength + static_cast<size_t>(dataInfo->string_table_offset);
    const size_t dataBase = headerLength + static_cast<size_t>(hdr->file_data_offset);
    const size_t dataLength = hdr->file_data_length;
    auto table_fits = [&](size_t offset, size_t count, size_t elementSize) {
        return offset <= fileLength && count <= (fileLength - offset) / elementSize;
    };
    if (!table_fits(nodeTbl, nodeCount, sizeof(JKRArchive::SDIDirEntry)) ||
        !table_fits(fileTbl, fileCount, sizeof(JKRArchive::SDIFileEntry)) ||
        !table_fits(strTbl, stringSize, 1) || dataBase > fileLength ||
        dataLength > fileLength - dataBase ||
        reinterpret_cast<uintptr_t>(arc.data() + nodeTbl) % alignof(JKRArchive::SDIDirEntry) != 0 ||
        reinterpret_cast<uintptr_t>(arc.data() + fileTbl) % alignof(JKRArchive::SDIFileEntry) != 0) {
        return false;
    }

    const auto stringTable = arc.subspan(strTbl, stringSize);
    const auto* nodes = reinterpret_cast<const JKRArchive::SDIDirEntry*>(arc.data() + nodeTbl);
    const auto* files = reinterpret_cast<const JKRArchive::SDIFileEntry*>(arc.data() + fileTbl);
    for (u32 i = 0; i < nodeCount; ++i) {
        const auto& node = nodes[i];
        if (!has_nul_terminated_string(stringTable, node.name_offset) ||
            node.first_file_index > fileCount ||
            node.num_entries > fileCount - node.first_file_index) {
            return false;
        }
    }
    for (u32 i = 0; i < fileCount; ++i) {
        const auto& entry = files[i];
        const u32 typeFlagsAndName = entry.type_flags_and_name_offset;
        if (!has_nul_terminated_string(stringTable, typeFlagsAndName & 0xFFFFFF)) {
            return false;
        }
        // Directory entries use their data fields for directory metadata.  A
        // normal file, however, is later addressed directly by JKRArchive.
        if ((typeFlagsAndName >> 24 & 0x03) == 0x01 &&
            (entry.data_offset > dataLength || entry.data_size > dataLength - entry.data_offset)) {
            return false;
        }
    }
    return true;
}

std::optional<std::vector<ArcFileInfo>> parseRarcFiles(std::span<const u8> arc) {
    if (!is_valid_rarc_bytes(arc)) return std::nullopt;
    try {
    std::vector<ArcFileInfo> out;

    constexpr size_t kMetaBase = sizeof(SArcHeader);  // = 0x20

    const auto* hdr = reinterpret_cast<const SArcHeader*>(arc.data());
    const auto* dataInfo = reinterpret_cast<const SArcDataInfo*>(arc.data() + kMetaBase);

    const u32 nodeCount   = dataInfo->num_nodes;
    const u32 fileCount   = dataInfo->num_file_entries;
    const u32 stringSize  = dataInfo->string_table_length;
    const size_t nodeTbl  = static_cast<size_t>(dataInfo->node_offset) + kMetaBase;
    const size_t fileTbl  = static_cast<size_t>(dataInfo->file_entry_offset) + kMetaBase;
    const size_t strTbl   = static_cast<size_t>(dataInfo->string_table_offset) + kMetaBase;
    const size_t dataBase = kMetaBase + static_cast<size_t>(hdr->file_data_offset);

    auto tableFits = [&](size_t offset, size_t count, size_t elementSize) {
        return offset <= arc.size() && count <= (arc.size() - offset) / elementSize;
    };
    if (!tableFits(nodeTbl, nodeCount, sizeof(JKRArchive::SDIDirEntry)) ||
        !tableFits(fileTbl, fileCount, sizeof(JKRArchive::SDIFileEntry)) ||
        !tableFits(strTbl, stringSize, 1) || dataBase > arc.size()) {
        return std::nullopt;
    }

    auto readStringAt = [&](u32 offset) -> std::string {
        if (offset >= stringSize) return {};
        const u8* start = arc.data() + strTbl + offset;
        const u8* bufferEnd = arc.data() + strTbl + stringSize;

        const void* nul = std::memchr(start, 0,
                                      static_cast<size_t>(bufferEnd - start));
        const u8* terminator = nul ? static_cast<const u8*>(nul) : bufferEnd;
        return std::string(reinterpret_cast<const char*>(start),
                           static_cast<size_t>(terminator - start));
    };

    const auto* nodes = reinterpret_cast<const JKRArchive::SDIDirEntry*>(
        arc.data() + nodeTbl);
    const auto* files = reinterpret_cast<const JKRArchive::SDIFileEntry*>(
        arc.data() + fileTbl);

    constexpr size_t kMaxRarcPathLength = 1024;
    constexpr size_t kMaxRarcPathBytes = 8u * 1024u * 1024u;
    size_t pathBytes = 0;
    out.reserve(std::min<size_t>(fileCount, 4096));
    for (u32 ni = 0; ni < nodeCount; ++ni) {
        const auto& node = nodes[ni];
        const std::string dirName = readStringAt(node.name_offset);
        if (dirName.size() > kMaxRarcPathLength) return std::nullopt;
        const u16 fc       = node.num_entries;
        const u32 firstIdx = node.first_file_index;
        const bool isRoot = (ni == 0);
        if (firstIdx > fileCount || fc > fileCount - firstIdx) continue;

        for (u32 fi = 0; fi < fc; ++fi) {
            const auto& entry = files[firstIdx + fi];
            const u32 typeFlagsAndName = entry.type_flags_and_name_offset;
            const u8  typeFlags = static_cast<u8>(typeFlagsAndName >> 24);
            // Bit 0x01 = file, 0x02 = directory. We only want files.
            if ((typeFlags & 0x03) != 0x01) continue;

            std::string fname = readStringAt(typeFlagsAndName & 0xFFFFFF);
            if (fname.empty() || fname == "." || fname == "..") continue;
            if (fname.size() > kMaxRarcPathLength) return std::nullopt;

            const u32 entryOffset = entry.data_offset;
            const u32 entrySize = entry.data_size;
            if (entryOffset > arc.size() - dataBase ||
                entrySize > arc.size() - dataBase - entryOffset) {
                continue;
            }

            std::string path = (!isRoot && !dirName.empty())
                ? dirName + "/" + fname
                : std::move(fname);
            if (path.size() > kMaxRarcPathLength || out.size() >= kMaxRarcFiles ||
                path.size() > kMaxRarcPathBytes - pathBytes) {
                return std::nullopt;
            }
            pathBytes += path.size();
            out.push_back({
                std::move(path),
                static_cast<u32>(dataBase + entryOffset),
                entrySize,
            });
        }
    }
    return out;
    } catch (const std::bad_alloc&) {
        return std::nullopt;
    } catch (const std::length_error&) {
        return std::nullopt;
    }
}

std::optional<std::vector<u8>> load_valid_hd_rarc(const std::filesystem::path& path) {
    auto bytes = read_file(path);
    if (!bytes) return std::nullopt;
    if (isYaz0(*bytes)) {
        bytes = tryDecodeYaz0(*bytes);
        if (!bytes) return std::nullopt;
    }
    if (!is_valid_rarc_bytes(*bytes)) return std::nullopt;
    if (!parseRarcFiles(std::span<const u8>(bytes->data(), bytes->size()))) return std::nullopt;
    return bytes;
}

u16 read_be_u16(std::span<const u8> bytes, size_t offset) {
    return static_cast<u16>(bytes[offset] << 8 | bytes[offset + 1]);
}

u32 read_be_u32(std::span<const u8> bytes, size_t offset) {
    return static_cast<u32>(bytes[offset]) << 24 |
           static_cast<u32>(bytes[offset + 1]) << 16 |
           static_cast<u32>(bytes[offset + 2]) << 8 |
           static_cast<u32>(bytes[offset + 3]);
}

bool jpc_range_fits(size_t offset, size_t count, size_t size, size_t limit) {
    return offset <= limit && count <= (limit - offset) / size;
}

bool jpc_magic_at(std::span<const u8> bytes, size_t offset, const char (&magic)[5]) {
    return std::memcmp(bytes.data() + offset, magic, 4) == 0;
}

bool validate_jpc_color_table(std::span<const u8> bytes, size_t blockPos, size_t blockSize,
                              s16 tableOffset, u8 keyCount, s16 frameMax) {
    if (tableOffset < 0x34 || keyCount == 0 || frameMax < 0 || frameMax > 4096 ||
        !jpc_range_fits(static_cast<size_t>(tableOffset), keyCount, 6, blockSize)) {
        return false;
    }
    s16 previous = -1;
    for (u8 i = 0; i < keyCount; ++i) {
        const s16 index = static_cast<s16>(read_be_u16(bytes, blockPos + tableOffset + i * 6));
        if (index < 0 || index <= previous || index > frameMax || (i == 0 && index != 0)) {
            return false;
        }
        previous = index;
    }
    // makeColorTable indexes i_data[j] once per frame, including after the
    // last key; its final key must therefore land on the final frame.
    return previous == frameMax;
}

std::optional<size_t> jpc_texture_data_size(u16 width, u16 height, u8 format,
                                            bool hasMips, u8 mipLevels) {
    if (width == 0 || height == 0 || mipLevels == 0 || mipLevels > 11) {
        return std::nullopt;
    }
    u32 shiftX = 0;
    u32 shiftY = 0;
    switch (format) {
    case 0x0: case 0x8: case 0xE: shiftX = 3; shiftY = 3; break;
    case 0x1: case 0x2: case 0x9: shiftX = 3; shiftY = 2; break;
    case 0x3: case 0x4: case 0x5: case 0x6: case 0xA: shiftX = 2; shiftY = 2; break;
    default: return std::nullopt;
    }
    const size_t bytesPerTile = format == 0x6 ? 64 : 32;
    size_t total = 0;
    for (u8 level = 0; level < (hasMips ? mipLevels : 1); ++level) {
        const size_t tilesX = (static_cast<size_t>(width) + (1u << shiftX) - 1) >> shiftX;
        const size_t tilesY = (static_cast<size_t>(height) + (1u << shiftY) - 1) >> shiftY;
        if (tilesX != 0 && tilesY > std::numeric_limits<size_t>::max() / tilesX) {
            return std::nullopt;
        }
        const size_t tiles = tilesX * tilesY;
        if (tiles > (kMaxHdArchiveBytes - total) / bytesPerTile) return std::nullopt;
        total += tiles * bytesPerTile;
        width = std::max<u16>(1, width / 2);
        height = std::max<u16>(1, height / 2);
    }
    return total;
}

bool is_valid_jpc_bytes(std::span<const u8> bytes) {
    // This exactly mirrors the JPAC2-10 layout consumed by JPAResourceLoader.
    // That loader has no input length parameter, so every cursor, count and
    // data-dependent pointer must be proven in-range before it is constructed.
    constexpr size_t kHeaderSize = 0x10;
    constexpr u16 kMaxResources = 8192;
    constexpr u16 kMaxTextures = 4096;
    constexpr u16 kMaxBlocksPerResource = 128;
    if (bytes.size() < kHeaderSize || std::memcmp(bytes.data(), "JPAC2-10", 8) != 0) {
        return false;
    }

    const u16 resourceCount = read_be_u16(bytes, 0x08);
    const u16 textureCount = read_be_u16(bytes, 0x0A);
    const size_t textureTable = read_be_u32(bytes, 0x0C);
    if (resourceCount > kMaxResources || textureCount > kMaxTextures ||
        textureTable < kHeaderSize || textureTable > bytes.size()) {
        return false;
    }

    size_t offset = kHeaderSize;
    for (u16 resource = 0; resource < resourceCount; ++resource) {
        if (!jpc_range_fits(offset, 1, 8, textureTable)) return false;
        const u16 blockCount = read_be_u16(bytes, offset + 2);
        const u8 fieldCount = bytes[offset + 4];
        const u8 keyCount = bytes[offset + 5];
        const u8 textureIndexCount = bytes[offset + 6];
        if (blockCount > kMaxBlocksPerResource) return false;
        offset += 8;

        u16 fields = 0, keys = 0, tdbs = 0;
        bool hasDynamics = false, hasBaseShape = false, hasExtraShape = false;
        bool hasChildShape = false, hasExTexShape = false;
        for (u16 block = 0; block < blockCount; ++block) {
            if (!jpc_range_fits(offset, 1, 8, textureTable)) return false;
            const u32 size = read_be_u32(bytes, offset + 4);
            if (size < 8 || size > textureTable - offset) return false;

            if (jpc_magic_at(bytes, offset, "FLD1")) {
                if (size < 0x41 || fields >= fieldCount || (read_be_u32(bytes, offset + 8) & 0xF) > 8) {
                    return false;
                }
                ++fields;
            } else if (jpc_magic_at(bytes, offset, "KFA1")) {
                const u8 count = bytes[offset + 9];
                if (count == 0 || keys >= keyCount || !jpc_range_fits(0x0C, count, 16, size)) {
                    return false;
                }
                ++keys;
            } else if (jpc_magic_at(bytes, offset, "BEM1")) {
                if (hasDynamics || size < 0x79 || ((read_be_u32(bytes, offset + 8) >> 8) & 7) > 6) {
                    return false;
                }
                hasDynamics = true;
            } else if (jpc_magic_at(bytes, offset, "BSP1")) {
                if (hasBaseShape || size < 0x34) return false;
                const u32 flags = read_be_u32(bytes, offset + 8);
                const u16 blend = read_be_u16(bytes, offset + 0x18);
                const bool texCrdAnim = (flags & 0x01000000) != 0;
                const bool texAnim = (bytes[offset + 0x1E] & 1) != 0;
                const bool prmAnim = (bytes[offset + 0x21] & 2) != 0;
                const bool envAnim = (bytes[offset + 0x21] & 8) != 0;
                const u8 texAnimType = (bytes[offset + 0x1E] >> 2) & 7;
                const u8 colorAnimType = (bytes[offset + 0x21] >> 4) & 7;
                const u8 texKeys = bytes[offset + 0x1F];
                const u8 prmKeys = bytes[offset + 0x22];
                const u8 envKeys = bytes[offset + 0x23];
                const s16 frameMax = static_cast<s16>(read_be_u16(bytes, offset + 0x24));
                size_t variableOffset = 0x34;
                if (texCrdAnim) variableOffset += 0x28;
                if ((flags & 0x0F) > 10 || ((flags >> 4) & 7) > 4 ||
                    ((flags >> 7) & 7) > 4 || ((flags >> 15) & 7) > 5 ||
                    (blend & 3) > 2 || ((blend >> 2) & 0x0F) > 9 ||
                    ((blend >> 6) & 0x0F) > 9 ||
                    bytes[offset + 0x20] >= textureIndexCount ||
                    variableOffset > size ||
                    (texAnim && texAnimType > 4) ||
                    ((prmAnim || envAnim) && colorAnimType > 4) ||
                    (texAnim && (texKeys == 0 || !jpc_range_fits(variableOffset, texKeys, 1, size))) ||
                    (prmAnim && !validate_jpc_color_table(bytes, offset, size,
                        static_cast<s16>(read_be_u16(bytes, offset + 0x0C)), prmKeys, frameMax)) ||
                    (envAnim && !validate_jpc_color_table(bytes, offset, size,
                        static_cast<s16>(read_be_u16(bytes, offset + 0x0E)), envKeys, frameMax))) {
                    return false;
                }
                if (texAnim) {
                    for (u8 i = 0; i < texKeys; ++i) {
                        if (bytes[offset + variableOffset + i] >= textureIndexCount) return false;
                    }
                }
                hasBaseShape = true;
            } else if (jpc_magic_at(bytes, offset, "ESP1")) {
                const u32 flags = size >= 0x60 ? read_be_u32(bytes, offset + 8) : 0;
                if (hasExtraShape || size < 0x60 ||
                    ((flags & 1) && (((flags >> 8) & 3) > 2 ||
                                     ((flags >> 10) & 3) > 2))) return false;
                hasExtraShape = true;
            } else if (jpc_magic_at(bytes, offset, "SSP1")) {
                const u32 flags = size >= 0x48 ? read_be_u32(bytes, offset + 8) : 0;
                if (hasChildShape || size < 0x48 || (flags & 0x0F) > 10 ||
                    ((flags >> 4) & 7) > 4 || ((flags >> 7) & 7) > 4 ||
                    bytes[offset + 0x45] >= textureIndexCount) return false;
                hasChildShape = true;
            } else if (jpc_magic_at(bytes, offset, "ETX1")) {
                const u32 flags = size >= 0x27 ? read_be_u32(bytes, offset + 8) : 0;
                if (hasExTexShape || size < 0x27 ||
                    ((flags & 1) && bytes[offset + 0x25] >= textureIndexCount) ||
                    ((flags & 0x100) && bytes[offset + 0x26] >= textureIndexCount)) return false;
                hasExTexShape = true;
            } else if (jpc_magic_at(bytes, offset, "TDB1")) {
                if (tdbs != 0 || !jpc_range_fits(8, textureIndexCount, 2, size)) return false;
                for (u8 i = 0; i < textureIndexCount; ++i) {
                    if (read_be_u16(bytes, offset + 8 + i * 2) >= textureCount) return false;
                }
                ++tdbs;
            } else {
                return false;
            }
            offset += size;
        }
        if (fields != fieldCount || keys != keyCount || !hasDynamics || !hasBaseShape ||
            (textureIndexCount == 0 ? tdbs != 0 : tdbs != 1)) {
            return false;
        }
    }
    if (offset > textureTable) return false;

    offset = textureTable;
    for (u16 texture = 0; texture < textureCount; ++texture) {
        if (!jpc_range_fits(offset, 1, 0x40, bytes.size()) || !jpc_magic_at(bytes, offset, "TEX1")) {
            return false;
        }
        const size_t size = read_be_u32(bytes, offset + 4);
        if (size < 0x40 || size > bytes.size() - offset ||
            std::memchr(bytes.data() + offset + 0x0C, 0, 0x14) == nullptr) {
            return false;
        }
        const size_t timg = offset + 0x20;
        const u16 width = read_be_u16(bytes, timg + 2);
        const u16 height = read_be_u16(bytes, timg + 4);
        const u8 format = bytes[timg];
        const bool hasMips = bytes[timg + 0x10] != 0;
        const u8 mipCount = bytes[timg + 0x18];
        const s8 maxLod = static_cast<s8>(bytes[timg + 0x17]);
        if (maxLod < 0 || (!hasMips && (mipCount > 1 || maxLod > 0)) ||
            (hasMips && (mipCount == 0 || maxLod > 80))) {
            return false;
        }
        const u8 levels = hasMips ? std::max<u8>(mipCount, static_cast<u8>(maxLod / 8 + 1)) : 1;
        const auto imageSize = jpc_texture_data_size(width, height, format, hasMips, levels);
        const s32 imageOffset = static_cast<s32>(read_be_u32(bytes, timg + 0x1C));
        if (imageOffset < 0) return false;
        const size_t imageRelative = 0x20 + static_cast<size_t>(imageOffset == 0 ? 0x20 : imageOffset);
        if (!imageSize || imageRelative > size || *imageSize > size - imageRelative) {
            return false;
        }
        const u16 paletteEntries = read_be_u16(bytes, timg + 0x0A);
        const s32 paletteOffset = static_cast<s32>(read_be_u32(bytes, timg + 0x0C));
        if (paletteEntries != 0 && (paletteOffset < 0 ||
            !jpc_range_fits(0x20 + static_cast<size_t>(paletteOffset), paletteEntries, 2, size))) {
            return false;
        }
        offset += size;
    }
    return true;
}

// Walk the HD arc, pair BMDs with their pack.gz GTX entries, deswizzle each
// HD surface, and register the decoded bytes with aurora under the absolute
// pointer that GXInitTexObj will later receive.
//
// arcBytes must point at the mounted archive bytes the game will later use;
// aurora's pointer lookups depend on those addresses staying valid.
void register_hd_textures_for_arc(std::span<u8> arcBytes, const std::vector<ArcFileInfo>& files,
    const TphdPack& pack, std::string_view arcLabel) {
    ZoneScoped;
    ZoneText(arcLabel.data(), arcLabel.size());
    size_t bmdReg = 0;
    size_t btiReg = 0;

    // Phase A: per-slot textures inside BMD/BDL models.
    for (const auto& f : files) {
        bmdReg += register_hd_bmd_textures_for_buffer(pack, f.path, arcBytes.data() + f.dataOffset, f.dataSize, false);
    }

    // Phase B: standalone .bti files. Each BTI is its own arc entry; the
    // game loads it via JUTTexture (or similar) which calls GXInitTexObj
    // with `(u8*)resTIMG + imageOffset`. Register that exact pointer.
    for (const auto& f : files) {
        if (register_hd_bti_replacement_for_buffer(pack, f.path, arcBytes.data() + f.dataOffset, f.dataSize, false)) {
            ++btiReg;
        }
    }

    HdLog.info("registerHdTextures[{}]: {} BMD-slot, {} standalone-BTI replacements",
               arcLabel, bmdReg, btiReg);
}

// HD arcs whose Wii-U layouts don't match the GC UI pipeline.
// Mounting the Wii-U RARC here would feed GX2/GTX-format region textures and
// .dat blobs to the GC render path, producing a garbled world/dungeon map.
// Keep the GC archive and let pack.gz replace textures via the vanilla path.
constexpr std::string_view kHdSkipList[] = {
    "res/Object/fileSel.arc",
};

bool is_layout_arc_path(std::string_view resPath) {
    return resPath.starts_with("res/Layout/") ||
           resPath.starts_with("res/LayoutRevo/");
}

bool is_field_map_arc_path(std::string_view resPath) {
    return resPath.starts_with("res/FieldMap/") &&
           endsWithSuffixCI(resPath, ".arc");
}

std::filesystem::path hd_pack_path_for_arc(const std::filesystem::path& contentPath,
                                            std::string_view resPath) {
    std::filesystem::path packPath = contentPath / std::string(resPath);
    packPath.replace_extension(".pack.gz");

    if (!resPath.starts_with("res/Layout/")) {
        return packPath;
    }

    const std::filesystem::path arcPath{std::string(resPath)};
    std::string revoStem = arcPath.stem().string();
    if (!revoStem.empty() && revoStem.back() != 'R') {
        revoStem += 'R';
    }

    const auto revoPackPath = contentPath / "res" / "LayoutRevo" /
                              (revoStem + ".pack.gz");
    if (path_exists(revoPackPath)) {
        return revoPackPath;
    }

    return packPath;
}

bool should_skip_hd_arc_mount(std::string_view resPath) {
    // Layout HD archives do not match the GC UI pipeline, but their pack.gz
    // textures can still be registered against the vanilla archive.
    if (is_layout_arc_path(resPath)) {
        return true;
    }
    if (is_field_map_arc_path(resPath)) {
        return true;
    }
    for (auto skip : kHdSkipList) {
        if (resPath == skip) return true;
    }
    return false;
}

bool should_register_hd_pack_for_vanilla_arc(std::string_view resPath) {
    if (resPath.starts_with("res/Layout/")) {
        return true;
    }
    // FieldMap arcs are mounted as the GC archive (see should_skip_hd_arc_mount),
    // but their pack.gz map textures can still be swapped in against the GC
    // archive's BTI pointers.
    return is_field_map_arc_path(resPath);
}

void* overlay_open(void* userData) {
    auto* entry = static_cast<HdOverlayEntry*>(userData);
    if (entry == nullptr) return nullptr;

    SDL_IOStream* stream = open_stream(entry->arcPath).release();
    if (stream == nullptr) {
        HdLog.warn("HD overlay open failed: {} ({})",
                   entry->arcPath.string(), SDL_GetError());
        return nullptr;
    }

    return stream;
}

void overlay_close(void* handle) {
    auto* stream = static_cast<SDL_IOStream*>(handle);
    if (stream == nullptr) return;
    SDL_CloseIO(stream);
}

int64_t overlay_read(void* handle, uint8_t* buf, size_t len) {
    auto* stream = static_cast<SDL_IOStream*>(handle);
    if (stream == nullptr || buf == nullptr) {
        return -1;
    }
    if (len == 0) {
        return 0;
    }
    const size_t got = SDL_ReadIO(stream, buf, len);
    if (got == 0 && SDL_GetIOStatus(stream) == SDL_IO_STATUS_ERROR) {
        return -1;
    }
    return static_cast<int64_t>(got);
}

int64_t overlay_seek(void* handle, int64_t offset, int32_t whence) {
    auto* stream = static_cast<SDL_IOStream*>(handle);
    if (stream == nullptr) {
        return -1;
    }
    const Sint64 pos = SDL_SeekIO(stream, offset, static_cast<SDL_IOWhence>(whence));
    return pos < 0 ? -1 : static_cast<int64_t>(pos);
}

void ensure_overlay_callbacks_registered() {
    if (g_overlayCallbacksRegistered) {
        return;
    }
    static constexpr AuroraOverlayCallbacks callbacks{
        .open = overlay_open,
        .close = overlay_close,
        .read = overlay_read,
        .seek = overlay_seek,
    };
    aurora_dvd_overlay_callbacks(&callbacks);
    g_overlayCallbacksRegistered = true;
}

void rebuild_hd_overlay_locked() {
    if (g_contentPath.empty()) {
        if (g_overlayCallbacksRegistered) {
            aurora_dvd_overlay_files(nullptr, 0, nullptr);
        }
        return;
    }

    std::error_code ec;
    const auto resRoot = g_contentPath;
    if (!std::filesystem::is_directory(resRoot, ec)) {
        HdLog.warn("HD content path has no res directory: {}", g_contentPath.string());
        return;
    }

    const s32 baseEntryCount = aurora_dvd_base_entry_count();
    if (baseEntryCount <= 0) {
        HdLog.warn("DVD overlay skipped because no base DVD FST is loaded yet");
        return;
    }

    ensure_overlay_callbacks_registered();

    std::vector<AuroraOverlayFile> overlayFiles;
    std::vector<HdOverlayEntry*> overlayEntries;
    for (std::filesystem::recursive_directory_iterator it(resRoot, ec), end;
         !ec && it != end; it.increment(ec)) {
        const bool regularFile = it->is_regular_file(ec);
        if (ec) {
            ec.clear();
            continue;
        }
        if (!regularFile) continue;

        const auto& arcPath = it->path();
        
        const auto rel = arcPath.lexically_relative(g_contentPath);
        const std::string resPath = rel.generic_string();
        if (resPath.empty()) continue;

        if (should_register_hd_pack_for_vanilla_arc(resPath)) {
            auto packPath = hd_pack_path_for_arc(g_contentPath, resPath);
            if (path_exists(packPath)) {
                auto& entry = g_overlayEntries().emplace_back();
                entry.dvdPath = "/" + resPath;
                entry.arcPath = arcPath;
                entry.packPath = std::move(packPath);

                const s32 entryNum = DVDConvertPathToEntrynum(entry.dvdPath.c_str());
                if (entryNum >= 0) {
                    g_entryNumToOverlay()[entryNum] = &entry;
                    HdLog.info("HD texture pack registered for vanilla arc: {} -> {}",
                               entry.dvdPath, entry.packPath.string());
                } else {
                    HdLog.warn("HD texture pack skipped because DVD path was not found: {}",
                               entry.dvdPath);
                }
            }
        }

        if (should_skip_hd_arc_mount(resPath)) continue;

        if (endsWithSuffixCI(resPath, ".arc") && !load_valid_hd_rarc(arcPath)) {
            HdLog.warn("HD overlay arc rejected; keeping original resource: {}", arcPath.string());
            continue;
        }
        if (endsWithSuffixCI(resPath, ".jpc") && !is_valid_hd_jpc_file(arcPath)) {
            HdLog.warn("HD overlay JPC rejected; keeping original resource: {}", arcPath.string());
            continue;
        }

        const auto fileSize = get_file_size(arcPath);
        if (!fileSize.has_value()) {
            HdLog.warn("HD overlay file size failed: {} ({})",
                       arcPath.string(), SDL_GetError());
            continue;
        }

        auto& entry = g_overlayEntries().emplace_back();
        entry.dvdPath = "/" + resPath;
        entry.arcPath = arcPath;
        entry.packPath = hd_pack_path_for_arc(g_contentPath, resPath);
        entry.size = *fileSize;

        overlayFiles.push_back({
            .fileName = entry.dvdPath.c_str(),
            .userData = &entry,
            .size = entry.size,
        });
        overlayEntries.push_back(&entry);
    }

    std::vector overlayEntryNums(overlayFiles.size(), -1);
    aurora_dvd_overlay_files(overlayFiles.data(), overlayFiles.size(), overlayEntryNums.data());

    for (size_t i = 0; i < overlayEntryNums.size() && i < overlayEntries.size(); ++i) {
        auto* entry = overlayEntries[i];
        if (overlayEntryNums[i] < 0) {
            HdLog.warn("HD overlay entry was not accepted by DVD FST: {}", entry->dvdPath);
            continue;
        }
        g_entryNumToOverlay()[overlayEntryNums[i]] = entry;
    }

    HdLog.info("HD DVD overlay registered {} files (arcs, .jpc and Audiores) from {}",
               overlayFiles.size(), g_contentPath.string());
}

}

bool is_valid_hd_rarc_file(const std::filesystem::path& path) {
    return load_valid_hd_rarc(path).has_value();
}

bool is_valid_hd_jpc_file(const std::filesystem::path& path) {
    const auto bytes = read_file(path);
    return bytes.has_value() && is_valid_jpc_bytes(*bytes);
}

void set_hd_content_path(std::filesystem::path contentPath) {
    std::lock_guard lk{g_cacheMutex};
    // Aurora's FST stores HdOverlayEntry::userData.  Detach it before the
    // backing list is cleared so overlay_open can never receive stale data.
    if (g_overlayCallbacksRegistered) {
        aurora_dvd_overlay_files(nullptr, 0, nullptr);
    }
    g_contentPath = std::move(contentPath);
    ++g_contentGeneration;
    clear_hd_texture_registrations_locked();
    // Callers may still be mounting a previously returned buffer.  Keep the
    // stable list storage alive; the path map is cleared below so old content
    // cannot be selected for future requests.
    g_mountBuffersByPath().clear();
    g_packCache().clear();
    g_overlayEntries().clear();
    g_entryNumToOverlay().clear();
    g_arcRanges().clear();
    rebuild_hd_overlay_locked();
    load_los_table(g_contentPath);
    HdLog.info("HD content path set to: {}",
               g_contentPath.empty() ? "(disabled)" : g_contentPath.string());
}

std::optional<std::vector<u8>*> try_load_hd_archive(std::string_view gcPath) {
    std::string_view resPath = extractResPath(gcPath);
    if (resPath.empty()) return std::nullopt;

    if (should_skip_hd_arc_mount(resPath)) return std::nullopt;

    std::filesystem::path contentPath;
    uint64_t contentGeneration = 0;
    const std::string cacheKey(resPath);
    {
        std::lock_guard lk{g_cacheMutex};
        if (g_contentPath.empty()) return std::nullopt;
        contentPath = g_contentPath;
        contentGeneration = g_contentGeneration;
        const auto it = g_mountBuffersByPath().find(cacheKey);
        if (it != g_mountBuffersByPath().end()) {
            return it->second;
        }
    }
    std::filesystem::path hdArcPath = contentPath / std::string(resPath);
    ZoneScoped;
#ifdef TRACY_ENABLE
    {
        auto fn = hdArcPath.filename().string();
        ZoneText(fn.c_str(), fn.size());
    }
#endif

    auto hdBytesOpt = load_valid_hd_rarc(hdArcPath);
    if (!hdBytesOpt) {
        HdLog.warn("HD arc rejected; keeping original resource: {}", hdArcPath.string());
        return std::nullopt;
    }

    auto hdFiles = parseRarcFiles(std::span<const u8>(
        hdBytesOpt->data(), hdBytesOpt->size()));
    if (!hdFiles) {
        HdLog.warn("HD arc file table rejected; keeping original resource: {}", hdArcPath.string());
        return std::nullopt;
    }

    // Sidecar pack.gz holds the HD textures.
    auto hdPackPath = hd_pack_path_for_arc(contentPath, resPath);
    auto hdPack = load_pack_cached(hdPackPath);

    // std::list keeps element addresses stable for aurora's pointer map.
    std::vector<u8>* mountBytes;
    std::string filename = hdArcPath.filename().string();
    {
        std::lock_guard lk{g_cacheMutex};
        if (contentGeneration != g_contentGeneration) {
            return std::nullopt;
        }
        const auto existing = g_mountBuffersByPath().find(cacheKey);
        if (existing != g_mountBuffersByPath().end()) {
            return existing->second;
        }
        g_mountBuffers().emplace_back(std::move(*hdBytesOpt));
        mountBytes = &g_mountBuffers().back();
        g_mountBuffersByPath().emplace(cacheKey, mountBytes);
        register_hd_arc_range_locked(mountBytes->data(), mountBytes->size(), filename);
    }

    HdLog.info("HD arc mount buffer allocated: {} at {} ({} bytes, pack.gz={})",
               filename, static_cast<const void*>(mountBytes->data()),
               mountBytes->size(), hdPack ? "yes" : "no");

    if (hdPack != nullptr) {
        register_hd_textures_for_arc(*mountBytes, *hdFiles, *hdPack, filename);
    }

    return mountBytes;
}

void register_mounted_hd_archive(s32 entryNum, void* arcBytes, size_t arcSize) {
    if (entryNum < 0 || arcBytes == nullptr || arcSize == 0) return;

    std::filesystem::path packPath;
    std::string label;
    {
        std::lock_guard lk{g_cacheMutex};
        auto it = g_entryNumToOverlay().find(entryNum);
        if (it == g_entryNumToOverlay().end()) return;
        packPath = it->second->packPath;
        label = it->second->arcPath.filename().string();
    }

    auto arcSpan = std::span{static_cast<uint8_t*>(arcBytes), arcSize};
    {
        std::lock_guard lk{g_cacheMutex};
        register_hd_arc_range_locked(arcSpan.data(), arcSpan.size(), label);
    }

    auto hdPack = load_pack_cached(packPath);
    if (hdPack == nullptr) {
        return;
    }

    auto hdFiles = parseRarcFiles(std::span<const u8>(arcSpan.data(), arcSpan.size()));
    if (!hdFiles) return;
    register_hd_textures_for_arc(arcSpan, *hdFiles, *hdPack, label);
}

void register_copied_hd_resource(s32 entryNum, std::string_view resourceName, void* buffer,
                            size_t resourceSize) {
    if (entryNum < 0 || buffer == nullptr || resourceSize < 0x20) return;

    const bool isBti = endsWithSuffixCI(resourceName, ".bti");
    const bool isBmd = endsWithSuffixCI(resourceName, ".bmd") ||
                       endsWithSuffixCI(resourceName, ".bdl");
    if (!isBti && !isBmd) return;

    std::filesystem::path packPath;
    {
        std::lock_guard lk{g_cacheMutex};
        auto it = g_entryNumToOverlay().find(entryNum);
        if (it == g_entryNumToOverlay().end()) {
            return;
        }
        packPath = it->second->packPath;
    }

    auto hdPack = load_pack_cached(packPath);
    if (hdPack == nullptr) {
        return;
    }

    std::string logicalKey;
    try {
        logicalKey = std::to_string(entryNum) + ":";
        logicalKey.reserve(logicalKey.size() + resourceName.size());
        for (unsigned char c : resourceName) {
            logicalKey.push_back(static_cast<char>((c >= 'A' && c <= 'Z') ?
                c + ('a' - 'A') : c));
        }
    } catch (const std::bad_alloc&) {
        return;
    } catch (const std::length_error&) {
        return;
    }

    if (isBti) {
        register_hd_bti_replacement_for_buffer(*hdPack, resourceName, buffer, resourceSize, true,
                                               logicalKey);
    } else {
        register_hd_bmd_textures_for_buffer(*hdPack, resourceName, buffer, resourceSize, true,
                                            logicalKey);
    }
}

std::optional<size_t> find_registered_hd_archive_remaining(const void* ptr) {
    if (ptr == nullptr) return std::nullopt;

    const auto p = reinterpret_cast<uintptr_t>(ptr);
    std::lock_guard lk{g_cacheMutex};
    for (const auto& range : g_arcRanges()) {
        const auto begin = reinterpret_cast<uintptr_t>(range.begin);
        const auto end = begin + range.size;
        if (p >= begin && p < end) {
            return end - p;
        }
    }
    return std::nullopt;
}

}
