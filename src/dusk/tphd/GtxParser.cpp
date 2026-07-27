#include "GtxParser.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>

#include "helpers/endian.h"

namespace dusk::tphd {

namespace {

constexpr u32 kBlockTypeEOF      = 0x01;
constexpr u32 kBlockTypeSurface  = 0x0B;
constexpr u32 kBlockTypeImage    = 0x0C;
constexpr u32 kBlockTypeMipChain = 0x0D;

constexpr u32 kMaxTextureDimension = 16384;
constexpr u32 kMaxTextureSurfaces = 256;
constexpr u32 kMaxMipLevels = 13;
constexpr size_t kMaxTextureDataBytes = 256u * 1024u * 1024u;

bool is_valid_surface(const GtxSurface& s) {
    return s.width != 0 && s.width <= kMaxTextureDimension &&
           s.height != 0 && s.height <= kMaxTextureDimension &&
           s.depth == 1 && s.mipCount != 0 && s.mipCount <= kMaxMipLevels &&
           s.pitch != 0 && s.pitch <= kMaxTextureDimension &&
           s.tileMode <= 15 &&
           s.imgSize != 0 && s.imgSize <= kMaxTextureDataBytes &&
           s.mipSize <= kMaxTextureDataBytes;
}

}

std::vector<GtxSurface> parseGtx(std::span<const u8> gtx) {
    std::vector<GtxSurface> out;

    if (gtx.size() < sizeof(Gfx2Header) ||
        std::memcmp(gtx.data(), "Gfx2", 4) != 0 ||
        reinterpret_cast<uintptr_t>(gtx.data()) % alignof(Gfx2Header) != 0) {
        return out;
    }
    const auto* fileHdr = reinterpret_cast<const Gfx2Header*>(gtx.data());
    const u32 headerSize = fileHdr->headerSize;
    if (headerSize < sizeof(Gfx2Header) || headerSize > gtx.size()) {
        return out;
    }

    GtxSurface* current = nullptr;
    size_t off = headerSize;

    while (off <= gtx.size() && sizeof(Gfx2BlockHeader) <= gtx.size() - off) {
        if (reinterpret_cast<uintptr_t>(gtx.data() + off) % alignof(Gfx2BlockHeader) != 0) break;
        const auto* blk = reinterpret_cast<const Gfx2BlockHeader*>(gtx.data() + off);
        if (std::memcmp(blk->magic, "BLK{", 4) != 0) {
            break;
        }
        const u32 blockHdrSize = blk->headerSize;
        const u32 blockType    = blk->blockType;
        const u32 blockDataSz  = blk->blockDataSize;

        if (blockHdrSize < sizeof(Gfx2BlockHeader) ||
            blockHdrSize > gtx.size() - off ||
            blockDataSz > gtx.size() - off - blockHdrSize) {
            break;
        }
        const u8* body = gtx.data() + off + blockHdrSize;

        switch (blockType) {
        case kBlockTypeSurface: {
            if (blockDataSz < sizeof(Gx2SurfaceBody) ||
                reinterpret_cast<uintptr_t>(body) % alignof(Gx2SurfaceBody) != 0) break;
            const auto* sb = reinterpret_cast<const Gx2SurfaceBody*>(body);
            GtxSurface s{};
            s.format   = sb->format;
            s.width    = sb->width;
            s.height   = sb->height;
            s.depth    = sb->depth;
            s.mipCount = sb->mipCount;
            s.aa       = sb->aa;
            s.use      = sb->use;
            s.imgSize  = sb->imgSize;
            s.mipSize  = sb->mipSize;
            s.tileMode = sb->tileMode;
            s.swizzle  = sb->swizzle;
            s.pitch    = sb->pitch;
            for (u32 i = 0; i < 13; ++i) {
                s.mipOffsets[i] = sb->mipOffsets[i];
            }
            if (!is_valid_surface(s) || out.size() >= kMaxTextureSurfaces) {
                current = nullptr;
                break;
            }
            out.push_back(s);
            current = &out.back();
            break;
        }
        case kBlockTypeImage:
            if (current && current->baseData.empty() &&
                blockDataSz >= current->imgSize) {
                current->baseData = gtx.subspan(off + blockHdrSize, current->imgSize);
            }
            break;
        case kBlockTypeMipChain:
            if (current && current->mipData.empty() &&
                blockDataSz >= current->mipSize) {
                current->mipData = gtx.subspan(off + blockHdrSize, current->mipSize);
            }
            break;
        case kBlockTypeEOF:
            off = gtx.size();
            break;
        default:
            break;
        }
        if (off == gtx.size()) break;
        off += blockHdrSize + blockDataSz;
    }

    out.erase(std::remove_if(out.begin(), out.end(), [](const GtxSurface& s) {
        return s.baseData.size() != s.imgSize ||
               (s.mipCount > 1 && s.mipData.size() != s.mipSize);
    }), out.end());
    return out;
}

}
