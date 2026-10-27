#include "memory_access.h"

#include <algorithm>

namespace uzume::emu::memaccess {

QByteArray read(const MemoryMap& map, quint64 addr, quint64 len) {
  QByteArray data;
  quint64 left = (std::min)(len, kMaxReadBytes);
  quint64 at = addr;
  data.reserve(static_cast<qsizetype>(left));
  while (left > 0) {
    const MemoryMap::Span sp = map.resolve(at);
    if (sp.ptr == nullptr || sp.len == 0) break;
    const quint64 n = (std::min<quint64>)(left, sp.len);
    data.append(reinterpret_cast<const char*>(sp.ptr), static_cast<qsizetype>(n));
    at += n;
    left -= n;
  }
  return data;
}

SearchResult search(const MemoryMap& map, quint64 start, quint64 end, const QByteArray& pattern, int max) {
  SearchResult r;
  if (pattern.isEmpty() || end <= start) return r;
  max = (std::max)(1, (std::min)(kMaxMatches, max));
  quint64 at = start;
  const quint64 limit = (std::min<quint64>)(end, start + kMaxScanBytes);
  r.truncated = end > limit;
  while (at < limit && r.matches.size() < max) {
    const MemoryMap::Span sp = map.resolve(at);
    if (sp.ptr == nullptr || sp.len == 0) {  // unmapped: skip on in page steps
      at += 0x1000 - (at & 0xFFF);
      continue;
    }
    const quint64 n = (std::min<quint64>)(limit - at, sp.len);
    const uint8_t* base = sp.ptr;
    const uint8_t* cur = base;
    const uint8_t* stop = base + n;
    while (cur < stop && r.matches.size() < max) {
      const uint8_t* hit = std::search(cur, stop, pattern.constData(), pattern.constData() + pattern.size(),
                                       [](uint8_t x, char y) { return x == static_cast<uint8_t>(y); });
      if (hit == stop) break;
      r.matches.append(at + static_cast<quint64>(hit - base));
      cur = hit + 1;
    }
    r.scanned += n;
    at += n;
  }
  if (r.matches.size() >= max) r.truncated = true;
  return r;
}

QByteArray parseHex(QString s) {
  s.remove(QLatin1Char(' ')).remove(QLatin1Char(':'));
  if (s.startsWith(QLatin1String("0x"), Qt::CaseInsensitive)) s = s.mid(2);
  if (s.isEmpty() || s.size() % 2 != 0) return {};
  const QByteArray out = QByteArray::fromHex(s.toLatin1());
  return out.size() * 2 == s.size() ? out : QByteArray();
}

QByteArray valueBytes(quint64 value, const QString& type) {
  const int size = type == QLatin1String("u8") ? 1 : type == QLatin1String("u16") ? 2 : type == QLatin1String("u32") ? 4 : 0;
  if (size == 0) return {};
  QByteArray out;
  for (int i = 0; i < size; ++i) out.append(static_cast<char>((value >> (8 * i)) & 0xFF));
  return out;
}

bool parseAddress(const QString& text, quint64* out) {
  const QString t = text.trimmed();
  if (t.isEmpty()) return false;
  bool ok = false;
  quint64 v = 0;
  if (t.startsWith(QLatin1String("0x"), Qt::CaseInsensitive)) v = t.mid(2).toULongLong(&ok, 16);
  else v = t.toULongLong(&ok, 10);
  if (!ok) return false;
  if (out != nullptr) *out = v;
  return true;
}

QString hexAddress(quint64 v) { return QStringLiteral("0x") + QString::number(v, 16).toUpper().rightJustified(8, QLatin1Char('0')); }

QList<HexRow> hexRows(const QByteArray& data, quint64 base, int perRow) {
  QList<HexRow> rows;
  perRow = (std::max)(1, perRow);
  for (qsizetype off = 0; off < data.size(); off += perRow) {
    HexRow row;
    row.addr = base + static_cast<quint64>(off);
    row.addrText = hexAddress(row.addr);
    const QByteArray chunk = data.mid(off, perRow);
    row.hex = QString::fromLatin1(chunk.toHex(' ')).toUpper();
    QString ascii;
    for (char c : chunk) {
      const auto u = static_cast<unsigned char>(c);
      ascii.append(u >= 0x20 && u < 0x7F ? QChar(u) : QLatin1Char('.'));
    }
    row.ascii = ascii;
    rows.append(row);
  }
  return rows;
}

}  // namespace uzume::emu::memaccess
