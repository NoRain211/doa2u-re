// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef DOA2U_RECOMP_TEXTURE_PACK_H
#define DOA2U_RECOMP_TEXTURE_PACK_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

/* Texture packs stay compatible with DOA2U-XBERecomp: an image in
   <textures>/replacement whose name holds a texture's key replaces it, and
   <textures>/dumps gets each texture once for editing. The name is
   <width>x<height>_<16 hex digits>_<FORMAT>.<png|dds>. */

enum RecompTexturePackFormat : uint32_t {
    RECOMP_TEXTURE_PACK_BGRA8 = 0u,
    RECOMP_TEXTURE_PACK_BC1,
    RECOMP_TEXTURE_PACK_BC2,
    RECOMP_TEXTURE_PACK_BC3,
};

struct RecompTexturePackLevel {
    std::vector<uint8_t> bytes;
    uint32_t pitch = 0u;  // bytes per row, or per block row for the BC formats
    uint32_t width = 0u;
    uint32_t height = 0u;
};

struct RecompTexturePackImage {
    RecompTexturePackFormat format = RECOMP_TEXTURE_PACK_BGRA8;
    uint32_t width = 0u;
    uint32_t height = 0u;
    /* A PNG has level 0 only; the host makes its mip chain. */
    bool generate_mips = false;
    std::vector<RecompTexturePackLevel> levels;  // level 0 first
};

/* Indexes <textures_folder>/replacement once and reads RECOMP_TEXTURE_DUMP
   (png or dds). Call before the presenter's worker starts: the index is
   read-only afterwards. */
void recomp_texture_pack_configure(const std::string &textures_folder);
bool recomp_texture_pack_replacing();
bool recomp_texture_pack_dumping();

/* The key of a guest texture's level 0 bytes, its size and format, and a P8
   texture's 256-colour palette. 0 for a format this pack does not handle. */
uint64_t recomp_texture_pack_key(uint32_t format_byte, uint32_t width, uint32_t height,
    const void *level0, size_t level0_bytes, const void *palette);

/* True and fills out when the key names a replacement image. A file that does
   not load is not tried again this session. */
bool recomp_texture_pack_find(uint64_t key, RecompTexturePackImage &out);

/* Writes the key once this session as PNG or DDS, unless a replacement holds
   it. `guest` is the level 0 bytes then each next level, as the presenter
   received them. */
void recomp_texture_pack_dump(uint64_t key, uint32_t format_byte, uint32_t width,
    uint32_t height, uint32_t levels, const void *guest, size_t guest_bytes,
    const void *palette);

/* Decodes BC1, BC2 or BC3 blocks to rows of B8G8R8A8 texels. */
bool recomp_texture_pack_decode_bc(const void *blocks, size_t bytes, uint32_t format_byte,
    uint32_t width, uint32_t height, std::vector<uint8_t> &bgra);

/* The name a dump uses, and the key in a replacement's name. */
std::string recomp_texture_pack_file_name(uint32_t format_byte, uint32_t width,
    uint32_t height, uint64_t key, const char *extension);
uint64_t recomp_texture_pack_key_in_name(const std::string &name);

#endif
