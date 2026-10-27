#pragma once
// Generic game-file format helpers for the plugin runtime (docs/architecture/12-plugins.md 16.3).
// Binary data is std::string (maps 1:1 to Lua strings). No game knowledge, no offsets of specific games.
// Every function treats its input as untrusted: malformed or truncated data yields an error, never a crash.

#include <QString>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace uzume::formats {

struct Result {
    bool ok = false;
    std::string data;
    std::string error;
};

// ---- LZ10 / LZ11 (Nintendo, header byte 0x10 or 0x11 + 24-bit size; LZ11 size 0 = 32-bit size follows) ----
Result lzDecompress(std::string_view in, size_t maxOut = 64u << 20);

// ---- GARC containers (versions 0x0400 and 0x0600: CRAG/FATO/FATB/FIMB) ----
struct GarcEntry {
    uint32_t offset = 0;  // relative to the data region (Garc::dataOffset)
    uint32_t length = 0;
};
struct Garc {
    bool ok = false;
    std::string error;
    uint32_t dataOffset = 0;                     // absolute offset of the data region in the file
    std::vector<std::vector<GarcEntry>> files;   // files[index][sub]
};
Garc garcParse(std::string_view in);
Result garcFile(std::string_view in, size_t index, size_t sub = 0);

// ---- 3DS RomFS inside a decrypted NCCH (.cxi) or NCSD (.3ds/.cci, partition 0) ----
// Files are huge: the ROM is accessed with seeks, never loaded whole.
// Encrypted games fail with "encrypted game: Uzume needs a decrypted game file".
Result romfsRead(const QString& romPath, std::string_view path, size_t maxOut = 256u << 20);  // "/a/b.bin"
struct RomfsEntry {
    std::string name;  // UTF-8
    bool dir = false;
    uint64_t size = 0;
};
std::vector<RomfsEntry> romfsList(const QString& romPath, std::string_view dir, std::string* error = nullptr);
// Drops the cached RomFS index (the game changed or stopped); a changed file is also detected by size and mtime.
void romfsCacheClear();

// RomFS with a LayeredFS mod laid over it (a 3DS game variant, ADR 0031 D2): <modDir>/romfs/<path> wins over the file of the game,
// a listing is the union of both (the mod's size wins). A mod path that tries to leave <modDir>/romfs is ignored. An empty modDir is
// the plain functions above. A file the game cannot give (encrypted game, missing) is still served when the mod has it.
Result romfsOverlayRead(const QString& romPath, const QString& modDir, std::string_view path, size_t maxOut = 256u << 20);
std::vector<RomfsEntry> romfsOverlayList(const QString& romPath, const QString& modDir, std::string_view dir, std::string* error = nullptr);

// ---- 3DS textures ----
// Image: RGBA8888, straight alpha, row-major, top-down.
struct Image {
    bool ok = false;
    int width = 0;
    int height = 0;
    std::string rgba;
    std::string error;
};

// Format numbers of ctrTextureDecode (3DS GPU / PICA200 texture format numbering):
//   0 RGBA8   1 RGB8    2 RGBA5551  3 RGB565  4 RGBA4   5 LA8    6 HILO8
//   7 L8      8 A8      9 LA4      10 L4     11 A4     12 ETC1  13 ETC1A4
// Pixel layout: 8x8 tiles (row-major, left to right, top to bottom), Morton (Z) order inside a tile
// (x bit 0 is the lowest index bit). Little-endian; byte order inside a texel:
//   RGBA8: A,B,G,R   RGB8: B,G,R   LA8: A,L   HILO8: byte0 -> G, byte1 -> R (B=0)
//   L4/A4: low nibble is the first texel; LA4: L in the high nibble, A in the low nibble.
//   A8/A4 give white with that alpha; L formats give gray with alpha 255.
// ETC1/ETC1A4: 4x4 blocks, four per 8x8 tile in Morton order; each 64-bit block is stored byte-reversed
// relative to the standard ETC1 layout; ETC1A4 has a 64-bit alpha block (4 bit/texel, column-major, low
// nibble first) in front of the colour block. Width/height are the visible size (<= 4096); the stored data
// is padded to multiples of 8.
Image ctrTextureDecode(std::string_view in, int width, int height, int format);

// BCLIM: a "CLIM" + "imag" footer (0x28 bytes) at the END of the file, image data in front. BCLIM uses its own
// format numbering, mapped internally to the table above:
//   0 L8  1 A8  2 LA4  3 LA8  4 HILO8  5 RGB565  6 RGB8  7 RGBA5551  8 RGBA4  9 RGBA8  10 ETC1  11 ETC1A4  12 L4  13 A4
// The stored size is padded to a power of two (minimum 8) in each dimension; the result is cropped to the
// width/height of the footer. Only unswizzled (rotation 0) images are supported.
Image bclimDecode(std::string_view in);

// ETC1 (withAlpha=false, 8 bytes per block) / ETC1A4 (withAlpha=true, 16 bytes per block) as plain
// block-row-major data in the 3DS byte order, no 8x8 tiling. width/height are multiples of 4 (<= 4096).
Image etc1Decode(std::string_view in, int width, int height, bool withAlpha);

Result pngEncode(const Image& img);

// ---- Gen 6 Pokémon data (PK6) ----
// 232 bytes (box) or 260 bytes (party). Header u32 EC at 0x00, u16 checksum at 0x06 (sum of the u16 words
// 0x08..0xE7), 4 shuffled blocks of 56 bytes from 0x08. Shuffle index ((EC >> 13) & 31) % 24, LCRNG
// seed = seed * 0x41C64E6D + 0x6073 (start: EC), words XORed with seed >> 16. Party bytes 0xE8..0x103 use a
// fresh LCRNG seeded with EC.
struct Pk6 {
    bool ok = false;
    std::string data;
    bool checksumValid = false;  // decrypt: stored checksum matches; encrypt: stored checksum matches the plain data
    std::string error;
};
Pk6 pk6Decrypt(std::string_view in);  // -> decrypted, blocks in A,B,C,D order
Pk6 pk6Encrypt(std::string_view in);  // inverse; recomputes nothing, the caller keeps the checksum

}  // namespace uzume::formats
