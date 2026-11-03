#pragma once
// What may reach a core's retro_cheat_set (issue #453): codes are normalised and filtered here, on the emulation thread
// side, so no input from a plugin or a cheat file (CRLF, quotes, stray separators, giant text, disabled entries) reaches a
// core's own parser. Header only, no Qt GUI dependency.
#include <QList>
#include <QString>
#include <QStringList>

namespace uzume::emu {

// Longest code text handed to a core. Real codes are far below this; cores with a recursive std::regex parser (melonDS DS on
// MSVC) can overflow their stack on long inputs.
inline constexpr int kMaxCheatCodeChars = 2048;
inline constexpr int kMaxCheatsSent = 512;

// Cleanup only, the same for every format (the line separator and the case stay as given): quotes and control characters
// (tab excepted) are dropped, CR/LF end a line, every line is trimmed, empty lines are dropped, lines are joined with '\n'.
// Returns an empty string for a code that is empty after cleaning or longer than kMaxCheatCodeChars.
inline QString normalizeCheatCode(const QString& format, const QString& code) {
  Q_UNUSED(format);
  QString s;
  s.reserve(code.size());
  for (QChar c : code) {
    const ushort u = c.unicode();
    if (u == '"' || u == 0x201C || u == 0x201D || u == 0xFEFF) continue;
    if (u == '\r' || u == '\n') s += QLatin1Char('\n');
    else if ((u < 0x20 && u != '\t') || u == 0x7F) continue;
    else s += c;
  }
  QStringList lines;
  for (const QString& part : s.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
    const QString line = part.trimmed();
    if (!line.isEmpty()) lines.append(line);
  }
  const QString out = lines.join(QLatin1Char('\n'));
  return out.size() > kMaxCheatCodeChars ? QString() : out;
}

// Sends a whole cheat list: only enabled, non-empty entries after normalisation, with consecutive indices (mGBA and DeSmuME
// ignore the `enabled` flag, so a disabled code would be applied). `coreHasCheats` says whether the core holds cheats from an
// earlier call; when there is nothing to send and nothing to clear, the core is not called at all. Returns the number sent.
// `Entry` has .format, .code, .enabled.
template <class Entry, class ResetFn, class SetFn>
int sendCheatList(const QList<Entry>& list, bool* coreHasCheats, ResetFn&& reset, SetFn&& set) {
  QStringList codes;
  for (const Entry& c : list) {
    if (!c.enabled || codes.size() >= kMaxCheatsSent) continue;
    const QString code = normalizeCheatCode(c.format, c.code);
    if (!code.isEmpty()) codes.append(code);
  }
  if (codes.isEmpty() && !*coreHasCheats) return 0;
  reset();
  unsigned index = 0;
  for (const QString& code : std::as_const(codes)) set(index++, true, code);
  *coreHasCheats = !codes.isEmpty();
  return static_cast<int>(index);
}

}  // namespace uzume::emu
