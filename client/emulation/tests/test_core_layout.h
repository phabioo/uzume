#pragma once
// Frame layout of the Uzume test core (test_core.cpp), shared with the tests that read it back. Header-only, no Qt.
//
// Frame (software path): 256x384 XRGB8888, two NDS-like screens stacked without a gap.
//   Top screen (y 0..191): four quadrants red | green over blue | white, plus an 8x8 cyan square in rows 0..7
//     whose x moves with the frame number (so successive frames differ, deterministically).
//   Bottom screen (y 192..383): magenta backdrop with two input strips and a data row:
//     y 192..207: port 0 joypad, y 208..223: port 1 joypad. Cell i (x = 16*i .. 16*i+15) is black while
//     RETRO_DEVICE_ID_JOYPAD id i is pressed (B Y Select Start Up Down Left Right A X L R L2 R2 L3 R3), else magenta.
//     y 240: data row, pixel i = 0xFF000000 | value (24 bits), see DataIndex.
// OpenGL path (render mode "opengl"): the same quadrants and magenta, scaled by the resolution option, no strips.
// Vulkan path (render mode "vulkan"): the same, plus an 8x8 (times scale) cyan marker in rows 0..7 at vkMarkerX(frame, scale).
#include <cstdint>

namespace uzume::testcore {
constexpr int kWidth = 256;
constexpr int kHeight = 384;
constexpr int kStripY0 = 192;       // port 0; port 1 starts 16 rows lower
constexpr int kStripHeight = 16;
constexpr int kCellWidth = 16;
constexpr int kDataRow = 240;
constexpr uint32_t kMagenta = 0xFFFF00FF;
constexpr uint32_t kPressed = 0xFF000000;
// Left edge of the cyan marker of the Vulkan render mode for the frame number `frame` (inside the red quadrant).
constexpr int vkMarkerX(uint64_t frame, int scale) { return static_cast<int>((frame * 8) % (kWidth / 2 - 8)) * scale; }

enum DataIndex : int {
  DataFrame = 0,     // frames since retro_load_game
  DataPointerX,      // RETRO_DEVICE_ID_POINTER_X + 0x8000
  DataPointerY,      // RETRO_DEVICE_ID_POINTER_Y + 0x8000
  DataPointerDown,   // RETRO_DEVICE_ID_POINTER_PRESSED
  DataFastForward,   // GET_FASTFORWARDING (0/1)
  DataSram0,         // SAVE_RAM byte 0 (0 without SAVE_RAM)
  DataSram1,         // SAVE_RAM byte 1
  DataLoads,         // retro_load_game calls with a game since the library was loaded
  DataResets,        // retro_reset calls
  DataRomSize,       // size of the game file (low 24 bits)
  DataRomSum,        // byte sum of the game file (low 24 bits)
  DataOptionTag,     // (length << 8) | first character of the flavor's tag option (GET_VARIABLE)
  DataDirs,          // bit 0: GET_SYSTEM_DIRECTORY answered, bit 1: GET_SAVE_DIRECTORY answered
  DataBitmask,       // 1 when the joypad was read as one RETRO_DEVICE_ID_JOYPAD_MASK word, 0 per button
  DataVarUpdates,    // GET_VARIABLE_UPDATE answered true this many times
  DataUnknownVar,    // GET_VARIABLE of an unknown key with a stale value pointer: 1 = miss reported and value reset to NULL, 2 = not
  DataCount
};

// Memory map the test core registers with SET_MEMORY_MAPS at retro_load_game (3DS-like virtual addresses), for the
// plugin memory access tests. Two descriptors, each its own zeroed 1 MiB buffer, little-endian values:
//   "heap"   at 0x08000000: +0x00 u32 magic kMagic ("FBMM"), +0x04 u32 frame counter (frames since load, updated in
//                           every retro_run), +0x10 u32 virtual address of the party block (kPartyAddr).
//   "linear" at 0x14000000: party block at 0x14000100: u32 slot count (kPartySlots), then kPartySlots slots of
//                           kPartySlotSize bytes. fillParty() in test_core.cpp writes the block: each slot is a synthetic
//                           encrypted PK6 party structure (makePk6, valid checksum), species kSpeciesBase + slot.
namespace mem {
constexpr uint64_t kHeapAddr = 0x08000000;
constexpr uint64_t kLinearAddr = 0x14000000;
constexpr uint64_t kRegionSize = 0x100000;  // 1 MiB each
constexpr uint32_t kMagic = 0x4D4D4246;     // bytes "FBMM"
constexpr uint64_t kMagicOff = 0x0;
constexpr uint64_t kFrameOff = 0x4;
constexpr uint64_t kCheatCountOff = 0x8;  // u32, number of enabled cheat entries (written every frame)
constexpr uint64_t kPartyPtrOff = 0x10;
constexpr uint32_t kPartyAddr = 0x14000100;
constexpr uint32_t kPartySlots = 6;
constexpr uint32_t kPartySlotSize = 260;
constexpr uint8_t kSlotMarker = 0xA0;  // slot i, byte 0 (low byte of the unencrypted EC) = kSlotMarker + i
constexpr uint32_t kSlotEcBase = 0x5A5A5A00;  // slot i EC = kSlotEcBase | (kSlotMarker + i)
constexpr uint16_t kSpeciesBase = 1;          // slot i species (u16 at decrypted offset 0x08) = kSpeciesBase + i

// Late map: at frame kLateMapFrame (FB_TEST_CORE_LATE_MAP_FRAME overrides, 0 = never) of every retro_run sequence the
// core calls SET_MEMORY_MAPS again from retro_run with the two regions above plus
//   "late"   at 0x0C000000 (kLateSize bytes): +0x00 u32 magic kLateMagic ("LATE").
// Mimics a core that maps heap memory only after the game has started.
constexpr uint64_t kLateAddr = 0x0C000000;
constexpr uint64_t kLateSize = 0x1000;
constexpr uint32_t kLateMagic = 0x4554414C;  // bytes "LATE"
constexpr int kLateMapFrame = 30;
}  // namespace mem

constexpr uint32_t pixel(uint32_t value24) { return 0xFF000000u | (value24 & 0xFFFFFFu); }
}  // namespace uzume::testcore
