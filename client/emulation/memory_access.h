#pragma once
// Read-only memory access shared by the debug MCP tools (memory_read, memory_search) and the memory viewer of the developer
// console (Settings > Plugins > Developer console, 0.22). Pure functions over a MemoryMap; the caller runs them on the
// emulation thread between frames (GameSession::runOnEmuThread). Nothing here writes memory.
#include <QByteArray>
#include <QList>
#include <QString>
#include <cstdint>

#include "memory_map.h"

namespace uzume::emu::memaccess {

inline constexpr quint64 kMaxReadBytes = 1024 * 1024;           // one read
inline constexpr quint64 kMaxScanBytes = 64ull * 1024 * 1024;  // one search
inline constexpr int kMaxMatches = 1000;

// Bytes from `addr`, up to `len` (at most kMaxReadBytes); stops at the first unmapped byte, so the result can be shorter.
QByteArray read(const MemoryMap& map, quint64 addr, quint64 len);

struct SearchResult {
  QList<quint64> matches;
  quint64 scanned = 0;
  bool truncated = false;  // the range was longer than kMaxScanBytes or `max` matches were found
};
// Finds `pattern` between start and end (exclusive). Matches that straddle two memory descriptors are not found.
SearchResult search(const MemoryMap& map, quint64 start, quint64 end, const QByteArray& pattern, int max);

// "DE AD be ef", "deadbeef", "0xDEADBEEF", "de:ad" -> bytes; empty on bad input (odd digit count, non-hex).
QByteArray parseHex(QString text);
// Little-endian bytes of `value` for a type "u8" | "u16" | "u32"; empty for another type (a wider value is cut to the type's width).
QByteArray valueBytes(quint64 value, const QString& type);
// "0x08000000", "08000000h"-free: decimal or 0x-prefixed hex, optional leading/trailing blanks; false on anything else.
bool parseAddress(const QString& text, quint64* out);

// One row of the hex view: address text, 16 hex bytes ("00 11 .."), ASCII ('.' for non-printable).
struct HexRow {
  quint64 addr = 0;
  QString addrText;
  QString hex;
  QString ascii;
};
QList<HexRow> hexRows(const QByteArray& data, quint64 base, int perRow = 16);
QString hexAddress(quint64 v);  // "0x08001A10" (at least 8 digits)

}  // namespace uzume::emu::memaccess
