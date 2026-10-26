// Game tools of the debug MCP server: state, memory, RomFS and ROM reads, screenshots, input, pause, resume, frame step.
// Memory is read on the emulation thread between frames; nothing here writes memory or saves (ADR 0032 D4).
#include <QFile>
#include <QJsonDocument>
#include <QPointer>
#include <QTimer>
#include <QtConcurrent>
#include <algorithm>
#include <cstring>
#include <memory>

#include "emulator_backend.h"
#include "format_helpers.h"
#include "gamesession.h"
#include "inputmap.h"
#include "mcp_tools.h"
#include "playercontroller.h"
#include "tools_util.h"

namespace uzume::mcp {

using namespace tools;

namespace {

constexpr int kMaxInputFrames = 600;
constexpr int kMaxRomfsEntries = 2000;
constexpr int kMaxMatches = 1000;

QString stateName(ui::GameSession::State s) {
  switch (s) {
    case ui::GameSession::Idle: return QStringLiteral("idle");
    case ui::GameSession::Starting: return QStringLiteral("starting");
    case ui::GameSession::Running: return QStringLiteral("running");
    case ui::GameSession::Paused: return QStringLiteral("paused");
    case ui::GameSession::Failed: return QStringLiteral("failed");
  }
  return QStringLiteral("unknown");
}

QJsonArray sizeJson(const QSize& s) { return QJsonArray{s.width(), s.height()}; }

// Parses "DE AD be ef", "deadbeef", "0xDEADBEEF"; empty on bad input.
QByteArray parseHex(QString s) {
  s.remove(QLatin1Char(' ')).remove(QLatin1Char(':'));
  if (s.startsWith(QLatin1String("0x"), Qt::CaseInsensitive)) s = s.mid(2);
  if (s.isEmpty() || s.size() % 2 != 0) return {};
  const QByteArray out = QByteArray::fromHex(s.toLatin1());
  return out.size() * 2 == s.size() ? out : QByteArray();
}

// Waits for `ready()` to become true on `signal` of `obj`, at most `ms`; then calls `done(ok)` once. GUI thread.
template <class Obj, class Signal, class Ready, class Done>
void waitFor(Obj* obj, Signal signal, Ready ready, int ms, Done done) {
  if (ready()) return done(true);
  auto fired = std::make_shared<bool>(false);
  auto conn = std::make_shared<QMetaObject::Connection>();
  auto timer = new QTimer(obj);
  timer->setSingleShot(true);
  auto finish = [fired, conn, timer, done](bool ok) {
    if (*fired) return;
    *fired = true;
    QObject::disconnect(*conn);
    timer->deleteLater();
    done(ok);
  };
  *conn = QObject::connect(obj, signal, obj, [ready, finish]() {
    if (ready()) finish(true);
  });
  QObject::connect(timer, &QTimer::timeout, timer, [finish]() { finish(false); });
  timer->start(ms);
}

}  // namespace

void installGameTools(McpToolRegistry& registry, ui::PlayerController& player) {
  QPointer<ui::PlayerController> pc(&player);
  const auto session = [pc]() -> ui::GameSession* { return pc ? pc->gameSession() : nullptr; };
  const auto noGame = [](ui::GameSession* gs) { return gs == nullptr || !gs->isActive(); };

  {
    ToolDef t;
    t.name = QStringLiteral("game_state");
    t.description = QStringLiteral(
        "State of the running game: running | paused | starting | failed | idle, the frame number, the frame layout and the chosen screen layout (ui_layout), the screens, the core's render mode and the measurements of the diagnostics overlay (fps, frame times, sizes, audio).");
    t.inputSchema = schema();
    t.handler = [session, pc](const QJsonObject&, ToolReply reply) {
      ui::GameSession* gs = session();
      if (gs == nullptr) return reply(ToolResult::error(QStringLiteral("app_closing")));
      QJsonArray screens;
      for (const emu::ScreenSpec& s : gs->displayProfile().screens)
        screens.append(QJsonObject{{QStringLiteral("id"), s.id}, {QStringLiteral("width"), s.width}, {QStringLiteral("height"), s.height}, {QStringLiteral("touch"), s.touch}});
      QJsonObject o{{QStringLiteral("state"), stateName(gs->state())},
                    {QStringLiteral("active"), gs->isActive()},
                    {QStringLiteral("paused"), gs->isPaused()},
                    {QStringLiteral("title"), gs->title()},
                    {QStringLiteral("error"), gs->errorText()},
                    {QStringLiteral("frame"), static_cast<double>(gs->frameNumber())},
                    // "layout" is the frame arrangement of the core output (single | vertical | horizontal); "ui_layout" is the screen
                    // layout the player chose (stacked | side | large | top | bottom), with "screen_swap".
                    {QStringLiteral("layout"), gs->displayProfile().layout},
                    {QStringLiteral("ui_layout"), pc && pc->sessions() ? pc->sessions()->screenLayout() : QString()},
                    {QStringLiteral("screen_swap"), pc && pc->sessions() ? pc->sessions()->screenSwap() : false},
                    {QStringLiteral("fast_forward_ratio"), gs->fastForwardRatio()},
                    {QStringLiteral("screens"), screens},
                    {QStringLiteral("fast_forward"), gs->fastForward()}};
      const ui::EmulationDiagnostics d = gs->diagnostics();
      if (d.valid) {
        o.insert(QStringLiteral("diagnostics"), QJsonObject{
            {QStringLiteral("core"), d.core}, {QStringLiteral("hw_requested"), d.hwRequested}, {QStringLiteral("hw_active"), d.hwActive},
            {QStringLiteral("api"), d.api}, {QStringLiteral("gpu"), d.gpu}, {QStringLiteral("fallback_reason"), d.fallbackReason},
            {QStringLiteral("frame_size"), sizeJson(d.frameSize)}, {QStringLiteral("readback_size"), sizeJson(d.readbackSize)},
            {QStringLiteral("base_size"), sizeJson(d.baseSize)}, {QStringLiteral("screen_count"), d.screens},
            {QStringLiteral("requested_scale"), d.requestedScale}, {QStringLiteral("cpu_threads"), d.cpuThreads},
            {QStringLiteral("fps"), d.fps}, {QStringLiteral("target_fps"), d.targetFps}, {QStringLiteral("frame_ms"), d.frameMs},
            {QStringLiteral("emu_ms"), d.emuMs}, {QStringLiteral("readback_ms"), d.readbackMs}, {QStringLiteral("zero_copy"), d.zeroCopy},
            {QStringLiteral("audio_active"), d.audioActive}, {QStringLiteral("audio_buffer_ms"), d.audioBufferMs}, {QStringLiteral("underruns"), d.underruns}});
      }
      reply(ToolResult::json(o));
    };
    registry.add(std::move(t));
  }

  {
    ToolDef t;
    t.name = QStringLiteral("memory_regions");
    t.description = QStringLiteral("The memory regions the running core exposes (start address, length, flags, address space).");
    t.inputSchema = schema();
    t.handler = [session, noGame](const QJsonObject&, ToolReply reply) {
      ui::GameSession* gs = session();
      if (noGame(gs)) return reply(ToolResult::error(QStringLiteral("no_game")));
      QJsonArray arr;
      for (const auto& r : gs->memoryRegions())
        arr.append(QJsonObject{{QStringLiteral("start"), hexAddr(r.start)}, {QStringLiteral("length"), static_cast<double>(r.len)},
                               {QStringLiteral("flags"), static_cast<double>(r.flags)}, {QStringLiteral("address_space"), QString::fromStdString(r.addrspace)}});
      reply(ToolResult::json(arr));
    };
    registry.add(std::move(t));
  }

  {
    ToolDef t;
    t.name = QStringLiteral("memory_read");
    t.description = QStringLiteral("Reads live game memory (not a watched snapshot) on the emulation thread: up to 1 MiB per call. Returns base64 (and hex for up to 256 bytes); stops at unmapped memory.");
    t.inputSchema = schema({{QStringLiteral("addr"), typeAny(QStringLiteral("Address: number or \"0x08000000\"."))}, {QStringLiteral("len"), typeInt(QStringLiteral("Bytes, 1 to 1048576."))}},
                           {QStringLiteral("addr"), QStringLiteral("len")});
    t.handler = [session, noGame](const QJsonObject& a, ToolReply reply) {
      ui::GameSession* gs = session();
      if (noGame(gs)) return reply(ToolResult::error(QStringLiteral("no_game")));
      const auto addr = parseNumber(a.value(QStringLiteral("addr")));
      const auto len = parseNumber(a.value(QStringLiteral("len")));
      if (!addr || !len || *len < 1 || static_cast<qint64>(*len) > kMaxMemoryBytes) return reply(ToolResult::error(QStringLiteral("addr and len (1 to 1048576) are required")));
      QByteArray data;
      bool noMap = false;
      const bool ran = gs->runOnEmuThread([&](emu::EmulatorBackend& be) {
        const emu::MemoryMap* map = be.memoryMap();
        if (map == nullptr) {
          noMap = true;
          return;
        }
        quint64 at = *addr, left = *len;
        data.reserve(static_cast<qsizetype>(left));
        while (left > 0) {
          const emu::MemoryMap::Span sp = map->resolve(at);
          if (sp.ptr == nullptr || sp.len == 0) break;
          const quint64 n = (std::min<quint64>)(left, sp.len);
          data.append(reinterpret_cast<const char*>(sp.ptr), static_cast<qsizetype>(n));
          at += n;
          left -= n;
        }
      });
      if (!ran) return reply(ToolResult::error(QStringLiteral("emulation_busy")));
      if (noMap) return reply(ToolResult::error(QStringLiteral("no_memory_map")));
      QJsonObject o{{QStringLiteral("addr"), hexAddr(*addr)}, {QStringLiteral("requested"), static_cast<double>(*len)}, {QStringLiteral("read"), static_cast<double>(data.size())},
                    {QStringLiteral("base64"), QString::fromLatin1(data.toBase64())}};
      if (data.size() <= 256) o.insert(QStringLiteral("hex"), QString::fromLatin1(data.toHex(' ')));
      if (data.isEmpty()) {
        ToolResult r = ToolResult::json(o);
        r.isError = true;
        return reply(std::move(r));
      }
      reply(ToolResult::json(o));
    };
    registry.add(std::move(t));
  }

  {
    ToolDef t;
    t.name = QStringLiteral("memory_search");
    t.description = QStringLiteral(
        "Searches live game memory between start and end (at most 64 MiB scanned per call) for a byte pattern (hex) or a little-endian value of type u8/u16/u32. "
        "Matches that straddle two memory descriptors are not found.");
    t.inputSchema = schema({{QStringLiteral("start"), typeAny(QStringLiteral("First address (number or 0x...)."))},
                            {QStringLiteral("end"), typeAny(QStringLiteral("End address, exclusive."))},
                            {QStringLiteral("pattern"), typeStr(QStringLiteral("Hex bytes, e.g. \"46 42 4D 4D\"."))},
                            {QStringLiteral("value"), typeInt(QStringLiteral("Number to find (with type)."))},
                            {QStringLiteral("type"), typeStr(QStringLiteral("u8 | u16 | u32 (with value)."))},
                            {QStringLiteral("max"), typeInt(QStringLiteral("At most this many matches, 1 to 1000 (default 100)."))}},
                           {QStringLiteral("start"), QStringLiteral("end")});
    t.timeoutMs = 20000;
    t.handler = [session, noGame](const QJsonObject& a, ToolReply reply) {
      ui::GameSession* gs = session();
      if (noGame(gs)) return reply(ToolResult::error(QStringLiteral("no_game")));
      const auto start = parseNumber(a.value(QStringLiteral("start")));
      const auto end = parseNumber(a.value(QStringLiteral("end")));
      if (!start || !end || *end <= *start) return reply(ToolResult::error(QStringLiteral("start and end (end > start) are required")));
      QByteArray pattern;
      if (a.contains(QStringLiteral("pattern"))) {
        pattern = parseHex(a.value(QStringLiteral("pattern")).toString());
        if (pattern.isEmpty()) return reply(ToolResult::error(QStringLiteral("pattern must be hex bytes")));
      } else if (a.contains(QStringLiteral("value"))) {
        const auto v = parseNumber(a.value(QStringLiteral("value")));
        const QString type = a.value(QStringLiteral("type")).toString();
        const int size = type == QLatin1String("u8") ? 1 : type == QLatin1String("u16") ? 2 : type == QLatin1String("u32") ? 4 : 0;
        if (!v || size == 0) return reply(ToolResult::error(QStringLiteral("value needs a non-negative number and type u8, u16 or u32")));
        for (int i = 0; i < size; ++i) pattern.append(static_cast<char>((*v >> (8 * i)) & 0xFF));
      } else {
        return reply(ToolResult::error(QStringLiteral("pattern or value+type is required")));
      }
      const int max = (std::max)(1, (std::min)(kMaxMatches, a.value(QStringLiteral("max")).isDouble() ? a.value(QStringLiteral("max")).toInt(100) : 100));
      QJsonArray matches;
      quint64 scanned = 0;
      bool truncated = false, noMap = false;
      const bool ran = gs->runOnEmuThread(
          [&](emu::EmulatorBackend& be) {
            const emu::MemoryMap* map = be.memoryMap();
            if (map == nullptr) {
              noMap = true;
              return;
            }
            quint64 at = *start;
            const quint64 limit = (std::min<quint64>)(*end, *start + static_cast<quint64>(kMaxScanBytes));
            truncated = *end > limit;
            while (at < limit && matches.size() < max) {
              const emu::MemoryMap::Span sp = map->resolve(at);
              if (sp.ptr == nullptr || sp.len == 0) {  // unmapped: skip on in page steps
                at += 0x1000 - (at & 0xFFF);
                continue;
              }
              const quint64 n = (std::min<quint64>)(limit - at, sp.len);
              const uint8_t* base = sp.ptr;
              const uint8_t* cur = base;
              const uint8_t* stop = base + n;
              while (cur < stop && matches.size() < max) {
                const uint8_t* hit = std::search(cur, stop, pattern.constData(), pattern.constData() + pattern.size(),
                                                 [](uint8_t x, char y) { return x == static_cast<uint8_t>(y); });
                if (hit == stop) break;
                matches.append(hexAddr(at + static_cast<quint64>(hit - base)));
                cur = hit + 1;
              }
              scanned += n;
              at += n;
            }
          },
          15000);
      if (!ran) return reply(ToolResult::error(QStringLiteral("emulation_busy")));
      if (noMap) return reply(ToolResult::error(QStringLiteral("no_memory_map")));
      reply(ToolResult::json(QJsonObject{{QStringLiteral("matches"), matches}, {QStringLiteral("scanned_bytes"), static_cast<double>(scanned)},
                                         {QStringLiteral("truncated"), truncated || matches.size() >= max}}));
    };
    registry.add(std::move(t));
  }

  // ----- game file: RomFS and raw ROM
  const auto romPath = [pc]() -> QString { return pc ? pc->runningGameInfo().value(QStringLiteral("romPath")).toString() : QString(); };

  {
    ToolDef t;
    t.name = QStringLiteral("romfs_list");
    t.description = QStringLiteral("Lists a directory of the RomFS of the running game (3DS). Entries: name, dir, size. Default path: /.");
    t.inputSchema = schema({{QStringLiteral("path"), typeStr(QStringLiteral("Directory, e.g. /a/0."))}});
    t.handler = [romPath](const QJsonObject& a, ToolReply reply) {
      const QString rom = romPath();
      if (rom.isEmpty()) return reply(ToolResult::error(QStringLiteral("no_game")));
      const QString dir = a.value(QStringLiteral("path")).toString(QStringLiteral("/"));
      (void)QtConcurrent::run([rom, dir, reply]() {
        std::string err;
        const auto entries = formats::romfsList(rom, dir.toStdString(), &err);
        if (entries.empty() && !err.empty()) return reply(ToolResult::error(QString::fromStdString(err)));
        QJsonArray arr;
        for (const auto& e : entries) {
          if (arr.size() >= kMaxRomfsEntries) break;
          arr.append(QJsonObject{{QStringLiteral("name"), QString::fromStdString(e.name)}, {QStringLiteral("dir"), e.dir}, {QStringLiteral("size"), static_cast<double>(e.size)}});
        }
        reply(ToolResult::json(QJsonObject{{QStringLiteral("entries"), arr}, {QStringLiteral("truncated"), entries.size() > static_cast<size_t>(kMaxRomfsEntries)}}));
      });
    };
    registry.add(std::move(t));
  }

  {
    ToolDef t;
    t.name = QStringLiteral("romfs_read");
    t.description = QStringLiteral("Reads a file from the RomFS of the running game (3DS), at most 4 MiB per call; use offset to page through larger files. Returns base64 and the file size.");
    t.inputSchema = schema({{QStringLiteral("path"), typeStr(QStringLiteral("File, e.g. /a/0/1/2."))},
                            {QStringLiteral("offset"), typeInt(QStringLiteral("Start offset in the file (default 0)."))},
                            {QStringLiteral("len"), typeInt(QStringLiteral("Bytes, 1 to 4194304 (default 4194304)."))}},
                           {QStringLiteral("path")});
    t.timeoutMs = 30000;
    t.handler = [romPath](const QJsonObject& a, ToolReply reply) {
      const QString rom = romPath();
      if (rom.isEmpty()) return reply(ToolResult::error(QStringLiteral("no_game")));
      const QString path = a.value(QStringLiteral("path")).toString();
      const auto offset = a.contains(QStringLiteral("offset")) ? parseNumber(a.value(QStringLiteral("offset"))) : std::optional<quint64>(0);
      const auto len = a.contains(QStringLiteral("len")) ? parseNumber(a.value(QStringLiteral("len"))) : std::optional<quint64>(kMaxRomfsBytes);
      if (path.isEmpty() || !offset || !len || *len < 1 || static_cast<qint64>(*len) > kMaxRomfsBytes) return reply(ToolResult::error(QStringLiteral("path required; len must be 1 to 4194304")));
      (void)QtConcurrent::run([rom, path, offset = *offset, len = *len, reply]() {
        const formats::Result r = formats::romfsRead(rom, path.toStdString(), 64u << 20);
        if (!r.ok) return reply(ToolResult::error(QString::fromStdString(r.error.empty() ? std::string("read failed") : r.error)));
        const quint64 size = r.data.size();
        const quint64 from = (std::min<quint64>)(offset, size);
        const quint64 n = (std::min<quint64>)(len, size - from);
        reply(ToolResult::json(QJsonObject{{QStringLiteral("path"), path}, {QStringLiteral("size"), static_cast<double>(size)}, {QStringLiteral("offset"), static_cast<double>(from)},
                                           {QStringLiteral("read"), static_cast<double>(n)},
                                           {QStringLiteral("base64"), QString::fromLatin1(QByteArray(r.data.data() + from, static_cast<qsizetype>(n)).toBase64())}}));
      });
    };
    registry.add(std::move(t));
  }

  {
    ToolDef t;
    t.name = QStringLiteral("rom_read");
    t.description = QStringLiteral("Reads bytes of the raw game file of the running game, at most 4 MiB per call. Returns base64 and the file size.");
    t.inputSchema = schema({{QStringLiteral("offset"), typeInt(QStringLiteral("Start offset."))}, {QStringLiteral("len"), typeInt(QStringLiteral("Bytes, 1 to 4194304."))}},
                           {QStringLiteral("offset"), QStringLiteral("len")});
    t.handler = [romPath](const QJsonObject& a, ToolReply reply) {
      const QString rom = romPath();
      if (rom.isEmpty()) return reply(ToolResult::error(QStringLiteral("no_game")));
      const auto offset = parseNumber(a.value(QStringLiteral("offset")));
      const auto len = parseNumber(a.value(QStringLiteral("len")));
      if (!offset || !len || *len < 1 || static_cast<qint64>(*len) > kMaxRomfsBytes) return reply(ToolResult::error(QStringLiteral("offset and len (1 to 4194304) are required")));
      QFile f(rom);
      if (!f.open(QIODevice::ReadOnly)) return reply(ToolResult::error(QStringLiteral("cannot open the game file")));
      const qint64 size = f.size();
      QByteArray data;
      if (static_cast<qint64>(*offset) < size && f.seek(static_cast<qint64>(*offset))) data = f.read(static_cast<qint64>(*len));
      reply(ToolResult::json(QJsonObject{{QStringLiteral("size"), static_cast<double>(size)}, {QStringLiteral("offset"), static_cast<double>(*offset)},
                                         {QStringLiteral("read"), static_cast<double>(data.size())}, {QStringLiteral("base64"), QString::fromLatin1(data.toBase64())}}));
    };
    registry.add(std::move(t));
  }

  // ----- screenshot
  {
    ToolDef t;
    t.name = QStringLiteral("screenshot");
    t.description = QStringLiteral("The game picture at the core's native size as a PNG image: all screens, or only the top or bottom screen of a two-screen system.");
    t.inputSchema = schema({{QStringLiteral("screen"), typeStr(QStringLiteral("all (default) | top | bottom."))}});
    t.timeoutMs = 15000;
    t.handler = [session](const QJsonObject& a, ToolReply reply) {
      ui::GameSession* gs = session();
      if (gs == nullptr) return reply(ToolResult::error(QStringLiteral("app_closing")));
      const QString which = a.value(QStringLiteral("screen")).toString(QStringLiteral("all"));
      if (which != QLatin1String("all") && which != QLatin1String("top") && which != QLatin1String("bottom")) return reply(ToolResult::error(QStringLiteral("screen must be all, top or bottom")));
      QPointer<ui::GameSession> gp(gs);
      gs->requestNativeFrame([gp, which, reply](const QImage& frame, const QString& error) {
        if (!error.isEmpty() || frame.isNull()) return reply(ToolResult::error(error.isEmpty() ? QStringLiteral("no_frame") : error));
        QImage img = frame;
        const emu::DisplayProfile prof = gp ? gp->displayProfile() : emu::DisplayProfile();
        if (which != QLatin1String("all") && prof.screens.size() >= 2) {
          int idx = -1;
          for (int i = 0; i < prof.screens.size(); ++i)
            if (prof.screens.at(i).id == which) idx = i;
          if (idx < 0) idx = which == QLatin1String("top") ? 0 : static_cast<int>(prof.screens.size()) - 1;
          const QSize base = prof.frameSize();
          QRect r = prof.screenRect(idx);
          if (!base.isEmpty() && base != img.size()) {  // the core renders larger than the manifest (internal resolution)
            const double sx = static_cast<double>(img.width()) / base.width(), sy = static_cast<double>(img.height()) / base.height();
            r = QRect(qRound(r.x() * sx), qRound(r.y() * sy), qRound(r.width() * sx), qRound(r.height() * sy));
          }
          img = img.copy(r.intersected(img.rect()));
        }
        const QByteArray png = pngOf(img);
        if (png.isEmpty()) return reply(ToolResult::error(QStringLiteral("no_frame")));
        ToolResult res = ToolResult::json(QJsonObject{{QStringLiteral("width"), img.width()}, {QStringLiteral("height"), img.height()}, {QStringLiteral("screen"), which}});
        res.addImage(png);
        reply(std::move(res));
      });
    };
    registry.add(std::move(t));
  }

  // ----- input, pause, resume, frame step
  {
    ToolDef t;
    t.name = QStringLiteral("game_input");
    t.description = QStringLiteral(
        "Presses buttons of the running game for N frames, then releases them. Button names are the Uzume inputs the system maps (a, b, x, y, l, r, start, select, up, down, left, right ...). "
        "Keyboard and gamepad stay as they are. Optional touch {x, y} (0..1 on the touch screen) is held for the same frames. While paused the frames are stepped.");
    t.inputSchema = schema({{QStringLiteral("buttons"), typeArray(typeStr(QString()), QStringLiteral("Button names."))},
                            {QStringLiteral("frames"), typeInt(QStringLiteral("Frames to hold, 1 to 600 (default 6)."))},
                            {QStringLiteral("touch"), typeObject({{QStringLiteral("x"), typeAny(QStringLiteral("0..1"))}, {QStringLiteral("y"), typeAny(QStringLiteral("0..1"))}}, {QStringLiteral("x"), QStringLiteral("y")},
                                                                 QStringLiteral("Touch screen position, normalized."))}});
    t.timeoutMs = 30000;
    t.handler = [pc, session, noGame](const QJsonObject& a, ToolReply reply) {
      ui::GameSession* gs = session();
      if (noGame(gs)) return reply(ToolResult::error(QStringLiteral("no_game")));
      if (gs->inputBlocked()) return reply(ToolResult::error(QStringLiteral("input_blocked: the game is in the background")));
      const int frames = a.value(QStringLiteral("frames")).isDouble() ? a.value(QStringLiteral("frames")).toInt(6) : 6;
      if (frames < 1 || frames > kMaxInputFrames) return reply(ToolResult::error(QStringLiteral("frames must be 1 to 600")));
      const QString profile = pc ? pc->runningGameInfo().value(QStringLiteral("inputProfile")).toString() : QString();
      quint32 mask = 0;
      for (const QJsonValue& b : a.value(QStringLiteral("buttons")).toArray()) {
        const QString name = b.toString().toLower();
        const quint32 bit = input::inputBit(name);
        const quint32 lib = bit == 0 ? 0 : input::libretroMaskFor(profile, bit);
        if (lib == 0) return reply(ToolResult::error(QStringLiteral("unknown_button: ") + name.left(24)));
        mask |= lib;
      }
      std::optional<QPointF> touch;
      if (a.value(QStringLiteral("touch")).isObject()) {
        const QJsonObject to = a.value(QStringLiteral("touch")).toObject();
        const int ti = gs->displayProfile().touchScreenIndex();
        if (ti < 0) return reply(ToolResult::error(QStringLiteral("no_touch_screen")));
        const double x = to.value(QStringLiteral("x")).toDouble(-1), y = to.value(QStringLiteral("y")).toDouble(-1);
        if (x < 0 || x > 1 || y < 0 || y > 1) return reply(ToolResult::error(QStringLiteral("touch x and y must be between 0 and 1")));
        touch = gs->displayProfile().toFrameNormalized(ti, QPointF(x, y));
      }
      if (mask == 0 && !touch) return reply(ToolResult::error(QStringLiteral("buttons or touch is required")));
      QPointer<ui::GameSession> gp(gs);
      const auto release = [gp, touch]() {
        if (!gp) return;
        gp->setDebugPad(0);
        if (touch) gp->setPointer(QPointF(0, 0), false);
      };
      gs->setDebugPad(mask);
      if (touch) gs->setPointer(*touch, true);
      if (gs->isPaused()) {  // frames do not advance by themselves: step them with the buttons held
        const qint64 n = gs->stepFrames(frames);
        release();
        if (n < 0) return reply(ToolResult::error(QStringLiteral("step_failed")));
        return reply(ToolResult::json(QJsonObject{{QStringLiteral("held_frames"), frames}, {QStringLiteral("stepped"), true}, {QStringLiteral("frame"), static_cast<double>(n)}}));
      }
      const quint64 target = gs->frameNumber() + static_cast<quint64>(frames);
      waitFor(gs, &ui::GameSession::frameChanged, [gp, target]() { return !gp || gp->frameNumber() >= target || !gp->isActive(); }, frames * 100 + 5000,
              [release, reply, frames](bool ok) {
                release();
                if (!ok) return reply(ToolResult::error(QStringLiteral("timeout: the game did not advance (paused or stalled)")));
                reply(ToolResult::json(QJsonObject{{QStringLiteral("held_frames"), frames}, {QStringLiteral("stepped"), false}}));
              });
    };
    registry.add(std::move(t));
  }

  {
    ToolDef t;
    t.name = QStringLiteral("game_pause");
    t.description = QStringLiteral("Pauses the running game (same as the pause button).");
    t.inputSchema = schema();
    t.handler = [session, noGame](const QJsonObject&, ToolReply reply) {
      ui::GameSession* gs = session();
      if (noGame(gs)) return reply(ToolResult::error(QStringLiteral("no_game")));
      QPointer<ui::GameSession> gp(gs);
      gs->pause();
      waitFor(gs, &ui::GameSession::stateChanged, [gp]() { return !gp || gp->isPaused(); }, 5000, [gp, reply](bool ok) {
        if (!ok || !gp) return reply(ToolResult::error(QStringLiteral("timeout")));
        reply(ToolResult::json(QJsonObject{{QStringLiteral("paused"), true}, {QStringLiteral("frame"), static_cast<double>(gp->frameNumber())}}));
      });
    };
    registry.add(std::move(t));
  }

  {
    ToolDef t;
    t.name = QStringLiteral("game_resume");
    t.description = QStringLiteral("Resumes a paused game.");
    t.inputSchema = schema();
    t.handler = [session, noGame](const QJsonObject&, ToolReply reply) {
      ui::GameSession* gs = session();
      if (noGame(gs)) return reply(ToolResult::error(QStringLiteral("no_game")));
      QPointer<ui::GameSession> gp(gs);
      gs->resume();
      waitFor(gs, &ui::GameSession::stateChanged, [gp]() { return !gp || gp->state() == ui::GameSession::Running; }, 5000, [gp, reply](bool ok) {
        if (!ok || !gp) return reply(ToolResult::error(QStringLiteral("timeout")));
        reply(ToolResult::json(QJsonObject{{QStringLiteral("paused"), false}}));
      });
    };
    registry.add(std::move(t));
  }

  {
    ToolDef t;
    t.name = QStringLiteral("game_frame_step");
    t.description = QStringLiteral("Runs exactly N frames of a paused game (1 to 600) and stays paused. Pause first with game_pause.");
    t.inputSchema = schema({{QStringLiteral("frames"), typeInt(QStringLiteral("Frames to run, 1 to 600 (default 1)."))}});
    t.timeoutMs = 30000;
    t.handler = [session, noGame](const QJsonObject& a, ToolReply reply) {
      ui::GameSession* gs = session();
      if (noGame(gs)) return reply(ToolResult::error(QStringLiteral("no_game")));
      if (!gs->isPaused()) return reply(ToolResult::error(QStringLiteral("not_paused: call game_pause first")));
      const int frames = a.value(QStringLiteral("frames")).isDouble() ? a.value(QStringLiteral("frames")).toInt(1) : 1;
      const qint64 n = gs->stepFrames(frames);
      if (n < 0) return reply(ToolResult::error(QStringLiteral("step_failed: frames must be 1 to 600 and the game paused")));
      QPointer<ui::GameSession> gp(gs);
      waitFor(gs, &ui::GameSession::frameChanged, [gp, n]() { return !gp || gp->frameNumber() >= static_cast<quint64>(n); }, 5000, [gp, n, reply](bool) {
        reply(ToolResult::json(QJsonObject{{QStringLiteral("frame"), static_cast<double>(n)}, {QStringLiteral("paused"), gp ? gp->isPaused() : true}}));
      });
    };
    registry.add(std::move(t));
  }
}

}  // namespace uzume::mcp
