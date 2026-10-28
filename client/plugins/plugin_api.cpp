// The `fb` Lua API of a plugin (docs/guides/plugin-api.md). Every function resolves its plugin through the sandbox owner.
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <algorithm>
#include <atomic>
#include <filesystem>
#include <map>

#include "format_helpers.h"
#include "lauxlib.h"
#include "lua.h"
#include "plugin_engine.h"
#include "plugin_ui.h"
#include "run_bridge.h"

namespace uzume::plugins {

namespace {

constexpr uint32_t kMaxWatchRange = 4u * 1024 * 1024;
constexpr uint64_t kMaxWatchTotal = 16ull * 1024 * 1024;
constexpr size_t kMaxWatchKeys = 256;
constexpr size_t kMaxSubs = 64;
constexpr lua_Integer kMaxRomRead = 16ll * 1024 * 1024;
constexpr qint64 kCacheQuota = 256ll * 1024 * 1024;
constexpr int kMaxCacheFiles = 2000;  // bytes alone let a plugin create millions of tiny files (inode/MFT exhaustion, slow walks)
constexpr qint64 kMaxCacheRead = 32ll * 1024 * 1024;

Plugin* self(lua_State* L) { return static_cast<Plugin*>(LuaSandbox::from(L)->owner()); }

void need(lua_State* L, const char* perm) {
  Plugin* p = self(L);
  if (!p->info.manifest.hasPermission(QLatin1String(perm))) {
    p->engine->store.noteDenied(p->id(), QLatin1String(perm));  // shown as "Not granted" in the developer console
    luaL_error(L, "permission %s not granted", perm);
  }
}

template <class F>
int guard(lua_State* L, F f) {
  std::string msg;
  try {
    return f();
  } catch (const std::exception& e) {
    msg = e.what();
  }
  return luaL_error(L, "%s", msg.c_str());
}

uint64_t checkAddr(lua_State* L, int idx) { return static_cast<uint64_t>(luaL_checkinteger(L, idx)); }

std::string checkStr(lua_State* L, int idx) {
  size_t n = 0;
  const char* s = luaL_checklstring(L, idx, &n);
  return std::string(s, n);
}

// ------------------------------------------------------------------ uz.log

int logFn(lua_State* L) {
  const auto level = static_cast<LogLevel>(lua_tointeger(L, lua_upvalueindex(1)));
  std::string line;
  const int n = lua_gettop(L);
  for (int i = 1; i <= n; ++i) {
    size_t len = 0;
    const char* s = luaL_tolstring(L, i, &len);
    if (i > 1) line += '\t';
    line.append(s, len);
    lua_pop(L, 1);
  }
  Plugin* p = self(L);
  p->engine->log(*p, level, line);
  return 0;
}

// ------------------------------------------------------------------ uz.memory

uint64_t watchedTotal(const Plugin& p, const std::string& exceptKey) {
  uint64_t total = 0;
  for (const auto& kv : p.watches)
    if (kv.first != exceptKey) total += kv.second.len;
  return total;
}

void addWatch(lua_State* L, Plugin& p, const std::string& key, uint64_t addr, lua_Integer len) {
  if (len <= 0) luaL_error(L, "watch length must be at least 1");
  if (len > static_cast<lua_Integer>(kMaxWatchRange)) luaL_error(L, "watch range too large (max 4 MiB per range)");
  if (!p.watches.count(key) && p.watches.size() >= kMaxWatchKeys) luaL_error(L, "too many watched ranges (max %d)", static_cast<int>(kMaxWatchKeys));
  if (watchedTotal(p, key) + static_cast<uint64_t>(len) > kMaxWatchTotal) luaL_error(L, "watched memory limit exceeded (max 16 MiB in total)");
  p.watches[key] = WatchRange{addr, static_cast<uint32_t>(len)};
  p.engine->syncWatches(p);
}

int memWatch(lua_State* L) {
  need(L, "memory.read");
  const std::string key = checkStr(L, 1);
  const uint64_t addr = checkAddr(L, 2);
  const lua_Integer len = luaL_checkinteger(L, 3);
  addWatch(L, *self(L), "m:" + key, addr, len);
  return 0;
}

int memUnwatch(lua_State* L) {
  need(L, "memory.read");
  Plugin* p = self(L);
  if (p->watches.erase("m:" + checkStr(L, 1))) p->engine->syncWatches(*p);
  return 0;
}

// upvalue 1: size | (signed << 8)
int memNum(lua_State* L) {
  need(L, "memory.read");
  const lua_Integer spec = lua_tointeger(L, lua_upvalueindex(1));
  const int size = static_cast<int>(spec & 0xFF);
  const bool sign = (spec >> 8) != 0;
  const uint64_t addr = checkAddr(L, 1);
  Plugin* p = self(L);
  unsigned char buf[8] = {};
  if (!p->engine->provider || !p->engine->provider->read(addr, buf, static_cast<size_t>(size))) {
    lua_pushnil(L);
    return 1;
  }
  uint64_t v = 0;
  for (int i = 0; i < size; ++i) v |= static_cast<uint64_t>(buf[i]) << (8 * i);
  if (sign && size < 8 && (v & (1ull << (size * 8 - 1)))) v |= ~0ull << (size * 8);
  lua_pushinteger(L, static_cast<lua_Integer>(v));
  return 1;
}

int memBytes(lua_State* L) {
  need(L, "memory.read");
  const uint64_t addr = checkAddr(L, 1);
  const lua_Integer len = luaL_checkinteger(L, 2);
  if (len < 0 || len > static_cast<lua_Integer>(kMaxWatchRange)) luaL_error(L, "length must be between 0 and 4 MiB");
  Plugin* p = self(L);
  std::string buf(static_cast<size_t>(len), '\0');
  if (!p->engine->provider || (len > 0 && !p->engine->provider->read(addr, buf.data(), buf.size()))) {
    lua_pushnil(L);
    return 1;
  }
  lua_pushlstring(L, buf.data(), buf.size());
  return 1;
}

int memFrame(lua_State* L) {
  need(L, "memory.read");
  Plugin* p = self(L);
  const std::optional<uint64_t> f = p->engine->provider ? p->engine->provider->frame() : std::nullopt;
  if (f) lua_pushinteger(L, static_cast<lua_Integer>(*f));
  else lua_pushnil(L);
  return 1;
}

// ------------------------------------------------------------------ uz.on_tick, uz.events

int onTick(lua_State* L) {
  Plugin* p = self(L);
  if (lua_isnil(L, 1)) {
    if (p->tickRef != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, p->tickRef);
    p->tickRef = LUA_NOREF;
    return 0;
  }
  luaL_checktype(L, 1, LUA_TFUNCTION);
  if (p->tickRef != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, p->tickRef);
  lua_pushvalue(L, 1);
  p->tickRef = luaL_ref(L, LUA_REGISTRYINDEX);
  return 0;
}

int eventsEmit(lua_State* L) {
  Plugin* p = self(L);
  const std::string name = checkStr(L, 1);
  if (name.empty() || name.size() > 128) luaL_error(L, "event name must be 1 to 128 characters");
  Value v;
  std::string err;
  if (lua_gettop(L) >= 2 && !readValue(L, 2, v, err)) luaL_error(L, "uz.events.emit: %s", err.c_str());
  if (!p->engine->eventFits(p->id(), valueBytes(v) + name.size() + sizeof(QueuedEvent))) luaL_error(L, "event queue is full");
  p->engine->emitEvent(*p, name, std::move(v));
  return 0;
}

int eventsOn(lua_State* L) {
  Plugin* p = self(L);
  std::string from = checkStr(L, 1);
  if (from == "fb") from = "uz";  // host events: "fb" is the old source name, kept for plugins written for older apps
  const std::string name = checkStr(L, 2);
  luaL_checktype(L, 3, LUA_TFUNCTION);
  if (p->subs.size() >= kMaxSubs) luaL_error(L, "too many event subscriptions (max %d)", static_cast<int>(kMaxSubs));
  lua_pushvalue(L, 3);
  const int ref = luaL_ref(L, LUA_REGISTRYINDEX);
  p->subs.push_back({QString::fromUtf8(from.c_str()), QString::fromUtf8(name.c_str()), ref});
  return 0;
}

// ------------------------------------------------------------------ uz.provide / uz.call

int provide(lua_State* L) {
  Plugin* p = self(L);
  const std::string name = checkStr(L, 1);
  luaL_checktype(L, 2, LUA_TTABLE);
  if (!p->info.manifest.provides.contains(QString::fromUtf8(name.c_str())))
    luaL_error(L, "interface '%s' is not listed in the manifest's \"provides\"", name.c_str());
  auto it = p->provided.find(name);
  if (it != p->provided.end()) luaL_unref(L, LUA_REGISTRYINDEX, it->second);
  lua_pushvalue(L, 2);
  p->provided[name] = luaL_ref(L, LUA_REGISTRYINDEX);
  return 0;
}

int call(lua_State* L) {
  Plugin* me = self(L);
  PluginEngine& eng = *me->engine;
  const std::string name = checkStr(L, 1);
  const std::string fn = checkStr(L, 2);
  const int top = lua_gettop(L);
  std::vector<Value> args;
  for (int i = 3; i <= top; ++i) {
    Value v;
    std::string err;
    if (!readValue(L, i, v, err)) luaL_error(L, "uz.call argument %d: %s", i - 2, err.c_str());
    args.push_back(std::move(v));
  }
  Plugin* callee = nullptr;
  const QString qname = QString::fromUtf8(name.c_str());
  for (const Dependency& d : std::as_const(me->info.manifest.dependencies)) {
    Plugin* c = eng.find(d.id);
    if (c && c->info.manifest.provides.contains(qname)) {
      callee = c;
      break;
    }
  }
  if (!callee) luaL_error(L, "no required plugin provides '%s' (add its plugin to \"requires\")", name.c_str());
  if (callee->status != PluginStatus::Running || !callee->lua)
    luaL_error(L, "plugin %s is not running", callee->id().toUtf8().constData());
  auto it = callee->provided.find(name);
  if (it == callee->provided.end())
    luaL_error(L, "plugin %s has not provided '%s' yet", callee->id().toUtf8().constData(), name.c_str());
  const int ref = it->second;

  std::vector<Value> results;
  std::string cerr;
  const bool ok = callee->lua->run(
      [&](lua_State* CL) {
        lua_rawgeti(CL, LUA_REGISTRYINDEX, ref);
        lua_getfield(CL, -1, fn.c_str());
        if (!lua_isfunction(CL, -1)) luaL_error(CL, "'%s.%s' is not a function", name.c_str(), fn.c_str());
        const int fidx = lua_gettop(CL);
        luaL_checkstack(CL, static_cast<int>(args.size()) + 4, "too many arguments");
        for (const Value& a : args) pushValue(CL, a);
        lua_call(CL, static_cast<int>(args.size()), LUA_MULTRET);
        const int nres = lua_gettop(CL) - fidx + 1;
        for (int i = 0; i < nres; ++i) {
          Value v;
          std::string e;
          if (!readValue(CL, fidx + i, v, e)) luaL_error(CL, "result %d: %s", i + 1, e.c_str());
          results.push_back(std::move(v));
        }
      },
      cerr);
  if (!ok) {
    const std::string first = LuaSandbox::firstLine(cerr);
    if (callee->lua->trip() != LuaSandbox::Trip::None) {
      eng.pause(*callee, cerr);
      luaL_error(L, "plugin %s was paused: %s", callee->id().toUtf8().constData(), first.c_str());
    }
    eng.log(*callee, LogLevel::Error, "Call " + name + "." + fn + " from " + me->id().toStdString() + " failed: " + first);
    luaL_error(L, "uz.call %s.%s failed: %s", name.c_str(), fn.c_str(), first.c_str());
  }
  luaL_checkstack(L, static_cast<int>(results.size()) + 4, "too many results");
  for (const Value& v : results) pushValue(L, v);
  return static_cast<int>(results.size());
}

// ------------------------------------------------------------------ uz.dev

int devWatch(lua_State* L) {
  need(L, "memory.read");
  Plugin* p = self(L);
  const std::string label = checkStr(L, 1);
  const uint64_t addr = checkAddr(L, 2);
  const std::string kind = checkStr(L, 3);
  DevWatch w;
  w.label = QString::fromUtf8(label.c_str());
  w.addr = addr;
  if (kind == "u8") { w.kind = QStringLiteral("u8"); w.size = 1; }
  else if (kind == "u16") { w.kind = QStringLiteral("u16"); w.size = 2; }
  else if (kind == "u32") { w.kind = QStringLiteral("u32"); w.size = 4; }
  else if (kind == "s32") { w.kind = QStringLiteral("s32"); w.size = 4; }
  else if (kind.rfind("hex:", 0) == 0) {
    bool okN = false;
    const int n = QString::fromStdString(kind.substr(4)).toInt(&okN);
    if (!okN || n < 1 || n > 64) luaL_error(L, "hex length must be 1 to 64");
    w.kind = QStringLiteral("hex");
    w.size = static_cast<uint32_t>(n);
  } else {
    luaL_error(L, "unknown watch kind '%s' (use u8, u16, u32, s32 or hex:<len>)", kind.c_str());
  }
  addWatch(L, *p, "d:" + label, addr, w.size);
  for (DevWatch& e : p->devWatches)
    if (e.label == w.label) {
      e = w;
      return 0;
    }
  p->devWatches.push_back(w);
  return 0;
}

int devUnwatch(lua_State* L) {
  need(L, "memory.read");
  Plugin* p = self(L);
  const std::string label = checkStr(L, 1);
  const QString ql = QString::fromUtf8(label.c_str());
  for (auto it = p->devWatches.begin(); it != p->devWatches.end(); ++it)
    if (it->label == ql) {
      p->devWatches.erase(it);
      break;
    }
  if (p->watches.erase("d:" + label)) p->engine->syncWatches(*p);
  return 0;
}

// ------------------------------------------------------------------ uz.video

// uz.video.screens() -> { { name, x, y, w, h }, ... } or nil when no game runs.
int videoScreens(lua_State* L) {
  need(L, "video.capture");
  Plugin* p = self(L);
  const auto& video = p->engine->ctx.video;
  const std::vector<ScreenInfo> screens = video ? video->screens() : std::vector<ScreenInfo>();
  if (screens.empty()) {
    lua_pushnil(L);
    return 1;
  }
  lua_createtable(L, static_cast<int>(screens.size()), 0);
  int i = 1;
  for (const ScreenInfo& s : screens) {
    lua_createtable(L, 0, 5);
    lua_pushlstring(L, s.name.data(), s.name.size());
    lua_setfield(L, -2, "name");
    lua_pushinteger(L, s.x);
    lua_setfield(L, -2, "x");
    lua_pushinteger(L, s.y);
    lua_setfield(L, -2, "y");
    lua_pushinteger(L, s.w);
    lua_setfield(L, -2, "w");
    lua_pushinteger(L, s.h);
    lua_setfield(L, -2, "h");
    lua_rawseti(L, -2, i++);
  }
  return 1;
}

lua_Integer rectField(lua_State* L, int idx, const char* key) {
  lua_getfield(L, idx, key);
  if (!lua_isinteger(L, -1)) luaL_error(L, "capture rectangle needs integer x, y, w and h");
  const lua_Integer v = lua_tointeger(L, -1);
  lua_pop(L, 1);
  return v;
}

// uz.video.capture(name | {x, y, w, h}) -> request id. Everything that can fail depending on the game or timing
// (busy, rate limit, no frame, ...) is reported in the "video.captured" event, never as a Lua error.
int videoCapture(lua_State* L) {
  need(L, "video.capture");
  Plugin* p = self(L);
  uint64_t id = 0;
  if (lua_type(L, 1) == LUA_TSTRING) {
    const std::string name = checkStr(L, 1);
    id = p->engine->requestCapture(*p, name.empty() ? std::string("?") : name, 0, 0, 0, 0);
  } else if (lua_type(L, 1) == LUA_TTABLE) {
    const lua_Integer x = rectField(L, 1, "x"), y = rectField(L, 1, "y"), w = rectField(L, 1, "w"), h = rectField(L, 1, "h");
    const lua_Integer lim = 1 << 20;  // keeps the int conversion safe; the real range check is in the engine
    id = p->engine->requestCapture(*p, std::string(), static_cast<int>(std::clamp<lua_Integer>(x, -1, lim)),
                                   static_cast<int>(std::clamp<lua_Integer>(y, -1, lim)), static_cast<int>(std::clamp<lua_Integer>(w, 0, lim)),
                                   static_cast<int>(std::clamp<lua_Integer>(h, 0, lim)));
  } else {
    luaL_error(L, "capture expects a screen name or a rectangle {x, y, w, h}");
  }
  lua_pushinteger(L, static_cast<lua_Integer>(id));
  return 1;
}

// ------------------------------------------------------------------ uz.rom

QFile* openRom(lua_State* L, Plugin& p) {
  if (p.engine->ctx.romPath.isEmpty()) return nullptr;
  if (!p.rom) {
    auto f = std::make_unique<QFile>(p.engine->ctx.romPath);
    if (!f->open(QIODevice::ReadOnly)) luaL_error(L, "cannot open the game file");
    p.rom = std::move(f);
  }
  return p.rom.get();
}

int romSize(lua_State* L) {
  need(L, "rom.read");
  QFile* f = openRom(L, *self(L));
  if (!f) lua_pushnil(L);
  else lua_pushinteger(L, static_cast<lua_Integer>(f->size()));
  return 1;
}

int romRead(lua_State* L) {
  need(L, "rom.read");
  const lua_Integer off = luaL_checkinteger(L, 1);
  const lua_Integer len = luaL_checkinteger(L, 2);
  if (off < 0 || len < 0 || len > kMaxRomRead) luaL_error(L, "invalid range (offset >= 0, length 0 to 16 MiB)");
  QFile* f = openRom(L, *self(L));
  if (!f || off >= f->size()) {
    lua_pushnil(L);
    return 1;
  }
  QByteArray data;
  if (f->seek(off)) data = f->read(len);
  lua_pushlstring(L, data.constData(), static_cast<size_t>(data.size()));
  return 1;
}

// ------------------------------------------------------------------ uz.codec and uz.rom.romfs
// Pure functions over strings (client/formats). Failures return nil, message (never a Lua error); only wrong argument
// types raise. Indexes are 1-based in Lua.

int pushFail(lua_State* L, const std::string& msg) {
  lua_pushnil(L);
  lua_pushlstring(L, msg.data(), msg.size());
  return 2;
}

int pushResult(lua_State* L, const formats::Result& r) {
  if (!r.ok) return pushFail(L, r.error.empty() ? std::string("failed") : r.error);
  lua_pushlstring(L, r.data.data(), r.data.size());
  return 1;
}

std::string_view checkView(lua_State* L, int idx) {
  size_t n = 0;
  const char* s = luaL_checklstring(L, idx, &n);
  return std::string_view(s, n);
}

int pushImage(lua_State* L, const formats::Image& img) {
  if (!img.ok) return pushFail(L, img.error.empty() ? std::string("failed") : img.error);
  lua_createtable(L, 0, 3);
  lua_pushinteger(L, img.width);
  lua_setfield(L, -2, "width");
  lua_pushinteger(L, img.height);
  lua_setfield(L, -2, "height");
  lua_pushlstring(L, img.rgba.data(), img.rgba.size());
  lua_setfield(L, -2, "rgba");
  return 1;
}

int codecLz(lua_State* L) { return pushResult(L, formats::lzDecompress(checkView(L, 1))); }

int codecGarc(lua_State* L) {
  const formats::Garc g = formats::garcParse(checkView(L, 1));
  if (!g.ok) return pushFail(L, g.error.empty() ? std::string("invalid GARC") : g.error);
  lua_createtable(L, 0, 3);
  lua_pushinteger(L, static_cast<lua_Integer>(g.files.size()));
  lua_setfield(L, -2, "count");
  lua_pushinteger(L, static_cast<lua_Integer>(g.dataOffset));
  lua_setfield(L, -2, "data_offset");  // absolute offset of the data region; entry offsets are relative to it
  lua_createtable(L, static_cast<int>(g.files.size()), 0);
  for (size_t i = 0; i < g.files.size(); ++i) {
    lua_createtable(L, static_cast<int>(g.files[i].size()), 0);
    for (size_t j = 0; j < g.files[i].size(); ++j) {
      lua_createtable(L, 0, 2);
      lua_pushinteger(L, static_cast<lua_Integer>(g.files[i][j].offset));
      lua_setfield(L, -2, "offset");
      lua_pushinteger(L, static_cast<lua_Integer>(g.files[i][j].length));
      lua_setfield(L, -2, "length");
      lua_rawseti(L, -2, static_cast<lua_Integer>(j + 1));
    }
    lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
  }
  lua_setfield(L, -2, "files");
  return 1;
}

int codecGarcFile(lua_State* L) {
  const std::string_view in = checkView(L, 1);
  const lua_Integer i = luaL_checkinteger(L, 2);
  const lua_Integer sub = luaL_optinteger(L, 3, 1);
  if (i < 1 || sub < 1) return pushFail(L, "GARC indexes start at 1");
  return pushResult(L, formats::garcFile(in, static_cast<size_t>(i - 1), static_cast<size_t>(sub - 1)));
}

int checkDim(lua_State* L, int idx) {
  const lua_Integer v = luaL_checkinteger(L, idx);
  return v < 0 || v > 100000 ? -1 : static_cast<int>(v);  // the C++ side enforces the real limit (4096)
}

int codecCtrTexture(lua_State* L) {
  const std::string_view in = checkView(L, 1);
  const int w = checkDim(L, 2), h = checkDim(L, 3);
  const lua_Integer fmt = luaL_checkinteger(L, 4);
  if (w < 0 || h < 0 || fmt < 0 || fmt > 100) return pushFail(L, "invalid texture size or format");
  return pushImage(L, formats::ctrTextureDecode(in, w, h, static_cast<int>(fmt)));
}

int codecBclim(lua_State* L) { return pushImage(L, formats::bclimDecode(checkView(L, 1))); }

int codecEtc1(lua_State* L) {
  const std::string_view in = checkView(L, 1);
  const int w = checkDim(L, 2), h = checkDim(L, 3);
  const bool alpha = lua_toboolean(L, 4) != 0;
  if (w < 0 || h < 0) return pushFail(L, "invalid texture size");
  return pushImage(L, formats::etc1Decode(in, w, h, alpha));
}

int codecPng(lua_State* L) {
  luaL_checktype(L, 1, LUA_TTABLE);
  formats::Image img;
  lua_getfield(L, 1, "width");
  lua_getfield(L, 1, "height");
  lua_getfield(L, 1, "rgba");
  const bool okTypes = lua_isinteger(L, -3) && lua_isinteger(L, -2) && lua_type(L, -1) == LUA_TSTRING;
  if (!okTypes) {
    lua_pop(L, 3);
    return pushFail(L, "png expects {width, height, rgba}");
  }
  const lua_Integer w = lua_tointeger(L, -3), h = lua_tointeger(L, -2);
  size_t n = 0;
  const char* d = lua_tolstring(L, -1, &n);
  if (w < 1 || h < 1 || w > 4096 || h > 4096) {
    lua_pop(L, 3);
    return pushFail(L, "invalid image size (1 to 4096)");
  }
  img.ok = true;
  img.width = static_cast<int>(w);
  img.height = static_cast<int>(h);
  img.rgba.assign(d, n);
  lua_pop(L, 3);
  return pushResult(L, formats::pngEncode(img));
}

int pushPk6(lua_State* L, const formats::Pk6& r) {
  if (!r.ok) return pushFail(L, r.error.empty() ? std::string("failed") : r.error);
  lua_pushlstring(L, r.data.data(), r.data.size());
  lua_pushboolean(L, r.checksumValid);
  return 2;
}

int codecPk6Decrypt(lua_State* L) { return pushPk6(L, formats::pk6Decrypt(checkView(L, 1))); }
int codecPk6Encrypt(lua_State* L) { return pushPk6(L, formats::pk6Encrypt(checkView(L, 1))); }

int romRomfs(lua_State* L) {
  need(L, "rom.read");
  const std::string_view path = checkView(L, 1);
  const Plugin* p = self(L);
  if (p->engine->ctx.romPath.isEmpty()) return pushFail(L, "no game file");
  return pushResult(L, formats::romfsOverlayRead(p->engine->ctx.romPath, p->engine->ctx.modDir, path));
}

int romRomfsList(lua_State* L) {
  need(L, "rom.read");
  const std::string_view dir = checkView(L, 1);
  const Plugin* p = self(L);
  if (p->engine->ctx.romPath.isEmpty()) return pushFail(L, "no game file");
  std::string err;
  const auto entries = formats::romfsOverlayList(p->engine->ctx.romPath, p->engine->ctx.modDir, dir, &err);
  if (entries.empty() && !err.empty()) return pushFail(L, err);
  lua_createtable(L, static_cast<int>(entries.size()), 0);
  lua_Integer i = 1;
  for (const auto& e : entries) {
    lua_createtable(L, 0, 3);
    lua_pushlstring(L, e.name.data(), e.name.size());
    lua_setfield(L, -2, "name");
    lua_pushboolean(L, e.dir);
    lua_setfield(L, -2, "dir");
    lua_pushinteger(L, static_cast<lua_Integer>(e.size));
    lua_setfield(L, -2, "size");
    lua_rawseti(L, -2, i++);
  }
  return 1;
}

// ------------------------------------------------------------------ uz.variants

void setStr(lua_State* L, const char* k, const QString& v);

// Reads field `key` of the table at `idx` as a string; nil/absent gives `def`, any other type is an error.
QString varStr(lua_State* L, int idx, const char* key, const QString& def, int maxLen) {
  lua_getfield(L, idx, key);
  QString out = def;
  if (lua_isnil(L, -1)) {
    lua_pop(L, 1);
    return out;
  }
  if (lua_type(L, -1) != LUA_TSTRING) luaL_error(L, "%s must be a string", key);
  size_t n = 0;
  const char* c = lua_tolstring(L, -1, &n);
  if (static_cast<int>(n) > maxLen) luaL_error(L, "%s is too long (max %d)", key, maxLen);
  out = QString::fromUtf8(c, static_cast<qsizetype>(n));
  lua_pop(L, 1);
  return out;
}

bool validPresetId(const QString& id) {
  static const QRegularExpression re(QStringLiteral("^[a-z0-9_-]{1,32}$"));
  return re.match(id).hasMatch() && id != QLatin1String("custom");  // "custom" is Uzume's own entry
}

std::shared_ptr<VariantsProvider> variantsProvider(lua_State* L) { return self(L)->engine->ctx.variants; }

// uz.variants.register{engine, presets = {{id, label, hint, settings = {["428"] = "..."}}}, games = {{system, codes = {...}}}}
int variantsRegister(lua_State* L) {
  need(L, "variants.build");
  luaL_checktype(L, 1, LUA_TTABLE);
  const auto prov = variantsProvider(L);
  if (!prov) luaL_error(L, "variants are not available");
  VariantRegistration reg;
  reg.pluginId = self(L)->id();
  reg.engine = varStr(L, 1, "engine", QStringLiteral("randomizer"), 64);
  if (reg.engine != QLatin1String("randomizer")) luaL_error(L, "unknown engine '%s'", reg.engine.toUtf8().constData());
  lua_getfield(L, 1, "presets");
  if (lua_type(L, -1) == LUA_TTABLE) {
    const lua_Integer n = luaL_len(L, -1);
    if (n > 32) luaL_error(L, "too many presets (max 32)");
    for (lua_Integer i = 1; i <= n; ++i) {
      lua_rawgeti(L, -1, i);
      if (lua_type(L, -1) != LUA_TTABLE) luaL_error(L, "presets[%d] must be a table", static_cast<int>(i));
      VariantPreset pr;
      pr.id = varStr(L, lua_gettop(L), "id", QString(), 32);
      if (!validPresetId(pr.id)) luaL_error(L, "presets[%d].id must be 1 to 32 characters of a-z 0-9 _ - and not 'custom'", static_cast<int>(i));
      pr.label = varStr(L, lua_gettop(L), "label", pr.id, 60);
      pr.hint = varStr(L, lua_gettop(L), "hint", QString(), 200);
      lua_getfield(L, -1, "settings");
      if (lua_type(L, -1) != LUA_TTABLE) luaL_error(L, "presets[%d].settings must be a table {[\"428\"] = \"...\"}", static_cast<int>(i));
      lua_pushnil(L);
      int count = 0;
      while (lua_next(L, -2) != 0) {
        if (lua_type(L, -2) != LUA_TSTRING || lua_type(L, -1) != LUA_TSTRING) luaL_error(L, "settings entries must map a version string to a settings string");
        size_t kn = 0, vn = 0;
        const char* k = lua_tolstring(L, -2, &kn);
        const char* v = lua_tolstring(L, -1, &vn);
        const QString key = QString::fromUtf8(k, static_cast<qsizetype>(kn));
        static const QRegularExpression ver(QStringLiteral("^[0-9]{3}$"));
        if (!ver.match(key).hasMatch()) luaL_error(L, "settings version must be 3 digits");
        if (vn == 0 || vn > 4096) luaL_error(L, "a settings string must be 1 to 4096 characters");
        if (++count > 8) luaL_error(L, "too many settings versions in one preset (max 8)");
        pr.settings.insert(key, QString::fromUtf8(v, static_cast<qsizetype>(vn)));
        lua_pop(L, 1);
      }
      lua_pop(L, 1);  // settings
      if (pr.settings.isEmpty()) luaL_error(L, "presets[%d] has no settings", static_cast<int>(i));
      reg.presets.append(pr);
      lua_pop(L, 1);  // the preset
    }
  }
  lua_pop(L, 1);
  lua_getfield(L, 1, "games");
  if (lua_type(L, -1) == LUA_TTABLE) {
    const lua_Integer n = luaL_len(L, -1);
    if (n > 64) luaL_error(L, "too many games entries (max 64)");
    for (lua_Integer i = 1; i <= n; ++i) {
      lua_rawgeti(L, -1, i);
      if (lua_type(L, -1) != LUA_TTABLE) luaL_error(L, "games[%d] must be a table", static_cast<int>(i));
      VariantGameSupport g;
      g.system = varStr(L, lua_gettop(L), "system", QString(), 16);
      if (g.system.isEmpty()) luaL_error(L, "games[%d].system is required", static_cast<int>(i));
      lua_getfield(L, -1, "codes");
      if (lua_type(L, -1) == LUA_TTABLE) {
        const lua_Integer cn = luaL_len(L, -1);
        if (cn > 512) luaL_error(L, "too many codes in games[%d] (max 512)", static_cast<int>(i));
        for (lua_Integer k = 1; k <= cn; ++k) {
          lua_rawgeti(L, -1, k);
          if (lua_type(L, -1) != LUA_TSTRING) luaL_error(L, "codes must be strings");
          size_t len = 0;
          const char* c = lua_tolstring(L, -1, &len);
          if (len == 0 || len > 32) luaL_error(L, "a code must be 1 to 32 characters");
          g.codes.append(QString::fromUtf8(c, static_cast<qsizetype>(len)));
          lua_pop(L, 1);
        }
      }
      lua_pop(L, 1);  // codes
      reg.games.append(g);
      lua_pop(L, 1);  // the entry
    }
  }
  lua_pop(L, 1);
  prov->registry()->set(reg);
  lua_pushboolean(L, 1);
  return 1;
}

// uz.variants.engine() -> {offered = bool, version = string | nil, on_pc = bool}
int variantsEngine(lua_State* L) {
  need(L, "variants.build");
  const auto prov = variantsProvider(L);
  VariantEngineInfo e;
  if (prov) e = prov->engine();
  lua_createtable(L, 0, 3);
  lua_pushboolean(L, e.offered);
  lua_setfield(L, -2, "offered");
  if (!e.version.isEmpty()) setStr(L, "version", e.version);
  lua_pushboolean(L, e.onPc);
  lua_setfield(L, -2, "on_pc");
  return 1;
}

// uz.variants.list([game_id]) -> {{id, name, slot, preset_id, state, tool_version}}; the variants of the game in the context
// (in the shell context, which has no game: of `game_id`). Never the seed or the settings.
int variantsList(lua_State* L) {
  need(L, "variants.build");
  const auto prov = variantsProvider(L);
  Plugin* p = self(L);
  QString gameId = p->engine->ctx.gameId;
  if (p->engine->ctx.shell && lua_type(L, 1) == LUA_TSTRING) gameId = QString::fromStdString(checkStr(L, 1));
  const QList<VariantRow> rows = prov && !gameId.isEmpty() ? prov->list(gameId) : QList<VariantRow>();
  lua_createtable(L, static_cast<int>(rows.size()), 0);
  lua_Integer i = 1;
  for (const VariantRow& r : rows) {
    lua_createtable(L, 0, 6);
    setStr(L, "id", r.id);
    setStr(L, "name", r.name);
    setStr(L, "slot", r.slot);
    if (!r.presetId.isEmpty()) setStr(L, "preset_id", r.presetId);
    setStr(L, "state", r.state);
    if (!r.toolVersion.isEmpty()) setStr(L, "tool_version", r.toolVersion);
    lua_rawseti(L, -2, i++);
  }
  return 1;
}

// uz.variants.build{name, preset_id | settings, seed, spoiler_log, game_id} -> request id. Progress and the result arrive as the
// host event "variants.state" {id, state, progress, error}; failures that depend on the PC or the server are reported there, not as
// Lua errors.
int variantsBuild(lua_State* L) {
  need(L, "variants.build");
  luaL_checktype(L, 1, LUA_TTABLE);
  const auto prov = variantsProvider(L);
  if (!prov) luaL_error(L, "variants are not available");
  Plugin* p = self(L);
  VariantBuildRequest req;
  req.name = varStr(L, 1, "name", QString(), 60).trimmed();
  if (req.name.isEmpty()) luaL_error(L, "name is required (1 to 60 characters)");
  req.presetId = varStr(L, 1, "preset_id", QString(), 32);
  req.settings = varStr(L, 1, "settings", QString(), 4096);
  if (req.presetId.isEmpty() == req.settings.isEmpty()) luaL_error(L, "give either preset_id or settings");
  lua_getfield(L, 1, "seed");
  if (lua_type(L, -1) == LUA_TNUMBER && lua_isinteger(L, -1)) req.seed = QString::number(static_cast<qlonglong>(lua_tointeger(L, -1)));
  else if (lua_type(L, -1) == LUA_TSTRING) req.seed = QString::fromStdString(checkStr(L, lua_gettop(L)));
  else if (!lua_isnil(L, -1)) luaL_error(L, "seed must be an integer or a string of digits");
  lua_pop(L, 1);
  if (!req.seed.isEmpty()) {
    bool ok = false;
    req.seed.toLongLong(&ok);
    if (!ok || req.seed.startsWith(QLatin1Char('-'))) luaL_error(L, "seed must be a non-negative integer below 2^63");
  }
  lua_getfield(L, 1, "spoiler_log");
  req.spoilerLog = lua_toboolean(L, -1) != 0;
  lua_pop(L, 1);
  req.gameId = p->engine->ctx.gameId;
  if (p->engine->ctx.shell) req.gameId = varStr(L, 1, "game_id", QString(), 128);
  if (req.gameId.isEmpty()) luaL_error(L, "no game: pass game_id in the shell context");
  lua_pushinteger(L, static_cast<lua_Integer>(prov->build(p->id(), req)));
  return 1;
}

// ------------------------------------------------------------------ uz.cache

}  // namespace

bool validCacheName(const std::string& n) {
  if (n.empty() || n.size() > 200 || n.front() == '/' || n.back() == '/' || n.find("..") != std::string::npos || n.find("//") != std::string::npos)
    return false;
  for (char c : n)
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '/' || c == '-')) return false;
  // Windows: a segment named like a device (CON, NUL, COM1, "aux.png" ...) opens the device, and a trailing dot is dropped (PI-14).
  size_t start = 0;
  while (start <= n.size()) {
    size_t end = n.find('/', start);
    if (end == std::string::npos) end = n.size();
    const std::string seg = n.substr(start, end - start);
    if (!seg.empty() && seg.back() == '.') return false;
    std::string stem = seg.substr(0, seg.find('.'));
    for (char& c : stem) c = static_cast<char>(c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c);
    if (stem == "CON" || stem == "PRN" || stem == "AUX" || stem == "NUL") return false;
    if (stem.size() == 4 && (stem.rfind("COM", 0) == 0 || stem.rfind("LPT", 0) == 0) && stem[3] >= '1' && stem[3] <= '9') return false;
    start = end + 1;
  }
  return true;
}

namespace {

QString cacheFile(lua_State* L, Plugin& p, const std::string& name) {
  if (!validCacheName(name)) luaL_error(L, "invalid cache name '%s' (use A-Z a-z 0-9 . _ - and /, no '..')", name.c_str());
  const QString base = QDir::cleanPath(p.info.cacheDir);
  const QString path = QDir::cleanPath(base + QLatin1Char('/') + QString::fromUtf8(name.c_str()));
  if (!path.startsWith(base + QLatin1Char('/'))) luaL_error(L, "invalid cache name '%s'", name.c_str());
  return path;
}

qint64 cacheUsage(Plugin& p) {
  if (p.cacheBytes >= 0) return p.cacheBytes;
  qint64 total = 0;
  int files = 0;
  QDirIterator it(p.info.cacheDir, QDir::Files, QDirIterator::Subdirectories);
  while (it.hasNext()) {
    it.next();
    if (it.fileName().contains(QLatin1Char('~'))) continue;  // leftover temp file of an interrupted write
    total += it.fileInfo().size();
    ++files;
  }
  p.cacheFiles = files;
  return p.cacheBytes = total;
}

int cacheRead(lua_State* L) {
  return guard(L, [&] {
    Plugin* p = self(L);
    const QString path = cacheFile(L, *p, checkStr(L, 1));
    QFileInfo fi(path);
    if (!fi.isFile() || fi.isSymLink()) {
      lua_pushnil(L);
      return 1;
    }
    if (fi.size() > kMaxCacheRead) return luaL_error(L, "cache file is larger than 32 MiB");
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
      lua_pushnil(L);
      return 1;
    }
    const QByteArray data = f.readAll();
    lua_pushlstring(L, data.constData(), static_cast<size_t>(data.size()));
    return 1;
  });
}

int cacheExists(lua_State* L) {
  Plugin* p = self(L);
  const QFileInfo fi(cacheFile(L, *p, checkStr(L, 1)));
  lua_pushboolean(L, fi.isFile() && !fi.isSymLink());
  return 1;
}

int cacheWrite(lua_State* L) {
  return guard(L, [&] {
    Plugin* p = self(L);
    const QString path = cacheFile(L, *p, checkStr(L, 1));
    size_t n = 0;
    const char* data = luaL_checklstring(L, 2, &n);
    const QFileInfo fi(path);
    if (fi.isSymLink() || fi.isDir()) return luaL_error(L, "cannot write cache file here");
    const bool existed = fi.isFile();
    const qint64 old = existed ? fi.size() : 0;
    const qint64 used = cacheUsage(*p);
    if (used - old + static_cast<qint64>(n) > kCacheQuota) return luaL_error(L, "cache quota exceeded (256 MiB per plugin)");
    if (!existed && p->cacheFiles >= kMaxCacheFiles) return luaL_error(L, "cache file limit exceeded (%d files per plugin)", kMaxCacheFiles);
    if (!QDir().mkpath(fi.absolutePath())) return luaL_error(L, "cannot create the cache folder");
    // Atomic without a flush (a cache needs no durability, only no torn files; QSaveFile would fsync every write, very slow
    // on CI disks): write a temp sibling whose name no cache name can have ('~'), then rename over the target.
    static std::atomic<unsigned> tmpSeq{0};
    const QString tmp = path + QStringLiteral(".%1~").arg(tmpSeq.fetch_add(1));
    bool ok = false;
    {
      QFile f(tmp);
      ok = f.open(QIODevice::WriteOnly | QIODevice::Truncate) && f.write(data, static_cast<qint64>(n)) == static_cast<qint64>(n);
      if (ok) {
        f.close();
        ok = f.error() == QFileDevice::NoError;
      }
    }
    if (ok) {
      std::error_code ec;  // replaces an existing target atomically (MoveFileExW / rename(2))
      std::filesystem::rename(std::filesystem::path(tmp.toStdWString()), std::filesystem::path(path.toStdWString()), ec);
      ok = !ec;
    }
    if (!ok) {
      QFile::remove(tmp);
      return luaL_error(L, "cannot write the cache file");
    }
    p->cacheBytes = used - old + static_cast<qint64>(n);
    if (!existed) ++p->cacheFiles;
    lua_pushboolean(L, 1);
    return 1;
  });
}

// ------------------------------------------------------------------ uz.ui (needs permission `ui`)

QString checkSlot(lua_State* L, int idx) {
  need(L, "ui");
  const QString slot = QString::fromStdString(checkStr(L, idx));
  if (!self(L)->info.manifest.ui.declaresSlot(slot)) luaL_error(L, "slot %s not declared in manifest", slot.toUtf8().constData());
  return slot;
}

Value readUiArg(lua_State* L, int idx) {
  Value v;
  std::string err;
  if (!readValue(L, idx, v, err, kUiReadDepth, kUiMaxBytes)) luaL_error(L, "ui tree: %s", err.c_str());
  return v;
}

int uiSet(lua_State* L) {
  const QString slot = checkSlot(L, 1);
  luaL_checktype(L, 2, LUA_TTABLE);
  const Value v = readUiArg(L, 2);
  QVariantMap tree;
  QString e;
  if (!convertUiTree(v, tree, e)) luaL_error(L, "ui tree: %s", e.toUtf8().constData());
  Plugin* p = self(L);
  p->uiTrees[slot] = tree;
  p->engine->store.ui.setTree(p->id(), slot, tree);
  return 0;
}

int uiPatch(lua_State* L) {
  const QString slot = checkSlot(L, 1);
  const QString id = QString::fromStdString(checkStr(L, 2));
  luaL_checktype(L, 3, LUA_TTABLE);
  const Value props = readUiArg(L, 3);
  Plugin* p = self(L);
  auto it = p->uiTrees.find(slot);
  if (it == p->uiTrees.end()) luaL_error(L, "ui tree: slot %s has no tree to patch (call uz.ui.set first)", slot.toUtf8().constData());
  QVariantMap tree = it->second;
  QString e;
  if (!patchUiTree(tree, id, props, e)) luaL_error(L, "ui tree: %s", e.toUtf8().constData());
  it->second = tree;
  p->engine->store.ui.setTree(p->id(), slot, tree);
  return 0;
}

int uiClear(lua_State* L) {
  const QString slot = checkSlot(L, 1);
  Plugin* p = self(L);
  p->uiTrees.erase(slot);
  p->engine->store.ui.clearSlot(p->id(), slot);
  return 0;
}

int uiTitle(lua_State* L) {
  const QString slot = checkSlot(L, 1);
  if (slot != QLatin1String("panel") && slot != QLatin1String("screen")) luaL_error(L, "uz.ui.title only supports the slots panel and screen");
  QString title;
  if (!lua_isnil(L, 2)) {
    title = QString::fromStdString(checkStr(L, 2));
    if (title.isEmpty() || title.size() > 40) luaL_error(L, "title must be 1 to 40 characters");
  }
  Plugin* p = self(L);
  p->engine->store.ui.setTitle(p->id(), slot, title);
  return 0;
}

int uiBadge(lua_State* L) {
  need(L, "ui");
  int n = 0;
  if (!lua_isnil(L, 1)) {
    const lua_Number d = luaL_checknumber(L, 1);
    n = d != d ? 0 : static_cast<int>((std::max)(0.0, (std::min)(999.0, d)));
  }
  Plugin* p = self(L);
  p->engine->store.ui.setBadge(p->id(), n);
  return 0;
}

int uiDot(lua_State* L) {
  need(L, "ui");
  QString kind;
  if (!lua_isnil(L, 1)) {
    kind = QString::fromStdString(checkStr(L, 1));
    if (kind != QLatin1String("news")) luaL_error(L, "unknown dot kind '%s' (use \"news\" or nil)", kind.toUtf8().constData());
  }
  Plugin* p = self(L);
  p->engine->store.ui.setDot(p->id(), kind);
  return 0;
}

int uiNotify(lua_State* L) {
  need(L, "ui");
  luaL_checktype(L, 1, LUA_TTABLE);
  const Value v = readUiArg(L, 1);
  QVariantMap toast;
  QString e;
  if (!convertToast(v, toast, e)) luaL_error(L, "uz.ui.notify: %s", e.toUtf8().constData());
  Plugin* p = self(L);
  const auto now = std::chrono::steady_clock::now();
  p->toastTokens = (std::min)(3.0, p->toastTokens + std::chrono::duration<double>(now - p->toastLast).count());
  p->toastLast = now;
  if (p->toastTokens < 1.0) {
    p->engine->log(*p, LogLevel::Warn, "uz.ui.notify: dropped (at most one notification per second)");
    lua_pushnil(L);
    return 1;
  }
  p->toastTokens -= 1.0;
  lua_pushinteger(L, p->engine->store.ui.addToast(p->id(), toast));
  return 1;
}

int uiOpen(lua_State* L) {
  const QString slot = checkSlot(L, 1);
  if (slot != QLatin1String("panel") && slot != QLatin1String("screen")) luaL_error(L, "uz.ui.open only supports the slots screen and panel");
  Plugin* p = self(L);
  p->engine->store.ui.addOpenRequest(p->id(), slot);
  return 0;
}

// uz.ui.close(slot): closes the screen sheet / panel tab that uz.ui.open showed.
int uiClose(lua_State* L) {
  const QString slot = checkSlot(L, 1);
  if (slot != QLatin1String("panel") && slot != QLatin1String("screen")) luaL_error(L, "uz.ui.close only supports the slots screen and panel");
  Plugin* p = self(L);
  p->engine->store.ui.addCloseRequest(p->id(), slot);
  return 0;
}

int uiOnAction(lua_State* L) {
  need(L, "ui");
  Plugin* p = self(L);
  if (p->actionRef != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, p->actionRef);
  p->actionRef = LUA_NOREF;
  if (lua_isnil(L, 1)) return 0;
  luaL_checktype(L, 1, LUA_TFUNCTION);
  lua_pushvalue(L, 1);
  p->actionRef = luaL_ref(L, LUA_REGISTRYINDEX);
  return 0;
}


// ------------------------------------------------------------------ uz.memory.write (permission memory.write)

constexpr lua_Integer kMaxWriteBytes = 64 * 1024;

void setStr(lua_State* L, const char* k, const QString& v);
void setFn(lua_State* L, const char* k, lua_CFunction f);

int pushNilMsg(lua_State* L, const QString& msg) {
  lua_pushnil(L);
  const QByteArray u = msg.toUtf8();
  lua_pushlstring(L, u.constData(), static_cast<size_t>(u.size()));
  return 2;
}

int pushWriteResult(lua_State* L, const QString& err) {
  if (!err.isEmpty()) return pushNilMsg(L, err);
  lua_pushboolean(L, 1);
  return 1;
}

int doWrite(lua_State* L, uint64_t addr, const QByteArray& bytes) {
  Plugin* p = self(L);
  const auto& emu = p->engine->ctx.emulation;
  if (!emu) return pushNilMsg(L, QStringLiteral("no_game"));
  return pushWriteResult(L, emu->writeMemory(p->id(), addr, bytes));
}

int memWrite(lua_State* L) {
  need(L, "memory.write");
  const uint64_t addr = checkAddr(L, 1);
  size_t n = 0;
  const char* s = luaL_checklstring(L, 2, &n);
  if (n == 0 || n > static_cast<size_t>(kMaxWriteBytes)) luaL_error(L, "write length must be between 1 and 65536 bytes");
  return doWrite(L, addr, QByteArray(s, static_cast<qsizetype>(n)));
}

// upvalue 1: size in bytes (little endian, the byte order of every supported system)
int memWriteNum(lua_State* L) {
  need(L, "memory.write");
  const int size = static_cast<int>(lua_tointeger(L, lua_upvalueindex(1)));
  const uint64_t addr = checkAddr(L, 1);
  const lua_Integer v = luaL_checkinteger(L, 2);
  const lua_Integer lo = size == 4 ? -(1ll << 31) : size == 2 ? -(1ll << 15) : -(1ll << 7);
  const lua_Integer hi = size == 4 ? (1ll << 32) - 1 : size == 2 ? (1ll << 16) - 1 : (1ll << 8) - 1;
  if (v < lo || v > hi) luaL_error(L, "value does not fit in %d byte%s", size, size == 1 ? "" : "s");
  QByteArray b(size, '\0');
  for (int i = 0; i < size; ++i) b[i] = static_cast<char>((static_cast<uint64_t>(v) >> (8 * i)) & 0xFF);
  return doWrite(L, addr, b);
}

// ------------------------------------------------------------------ uz.cheats (permission cheats)

constexpr size_t kMaxCheatEntries = 2000;

QString tableString(lua_State* L, int idx, const char* key, bool required, size_t maxLen) {
  lua_getfield(L, idx, key);
  if (lua_isnil(L, -1) && !required) {
    lua_pop(L, 1);
    return {};
  }
  if (lua_type(L, -1) != LUA_TSTRING) luaL_error(L, "cheat entry: %s must be a string", key);
  size_t n = 0;
  const char* s = lua_tolstring(L, -1, &n);
  if (n > maxLen) luaL_error(L, "cheat entry: %s is longer than %d characters", key, static_cast<int>(maxLen));
  const QString out = QString::fromUtf8(s, static_cast<qsizetype>(n));
  lua_pop(L, 1);
  return out;
}

int cheatsFormats(lua_State* L) {
  need(L, "cheats");
  const auto& emu = self(L)->engine->ctx.emulation;
  const QStringList f = emu ? emu->cheatFormats() : QStringList();
  lua_createtable(L, static_cast<int>(f.size()), 0);
  int i = 1;
  for (const QString& s : f) {
    const QByteArray u = s.toUtf8();
    lua_pushlstring(L, u.constData(), static_cast<size_t>(u.size()));
    lua_rawseti(L, -2, i++);
  }
  const QString core = emu ? emu->coreName() : QString();
  if (core.isEmpty()) return 1;
  const QByteArray c = core.toUtf8();
  lua_pushlstring(L, c.constData(), static_cast<size_t>(c.size()));
  return 2;
}

// uz.cheats.allowed() -> true | false, reason ("run_rule"): whether the run's rule lets cheats be turned on.
int cheatsAllowed(lua_State* L) {
  need(L, "cheats");
  const auto& emu = self(L)->engine->ctx.emulation;
  const QString why = emu ? emu->cheatsBlockedReason() : QString();
  lua_pushboolean(L, why.isEmpty());
  if (why.isEmpty()) return 1;
  const QByteArray u = why.toUtf8();
  lua_pushlstring(L, u.constData(), static_cast<size_t>(u.size()));
  return 2;
}

int cheatsApply(lua_State* L) {
  need(L, "cheats");
  luaL_checktype(L, 1, LUA_TTABLE);
  QList<CheatItem> list;
  const lua_Integer n = luaL_len(L, 1);
  if (static_cast<size_t>(n) > kMaxCheatEntries) luaL_error(L, "too many cheats (max %d)", static_cast<int>(kMaxCheatEntries));
  for (lua_Integer i = 1; i <= n; ++i) {
    lua_rawgeti(L, 1, i);
    if (!lua_istable(L, -1)) luaL_error(L, "cheat entry %d must be a table {id, name, format, code, enabled}", static_cast<int>(i));
    CheatItem c;
    const int t = lua_gettop(L);
    c.id = tableString(L, t, "id", true, 64);
    c.name = tableString(L, t, "name", false, 120);
    c.format = tableString(L, t, "format", true, 32);
    c.code = tableString(L, t, "code", true, 4096);
    lua_getfield(L, t, "enabled");
    c.enabled = lua_isnil(L, -1) ? true : lua_toboolean(L, -1) != 0;
    lua_pop(L, 2);
    list.append(c);
  }
  Plugin* p = self(L);
  const auto& emu = p->engine->ctx.emulation;
  if (!emu) return pushNilMsg(L, QStringLiteral("no_game"));
  const CheatApplyResult r = emu->applyCheats(p->id(), list);
  if (!r.error.isEmpty()) return pushNilMsg(L, r.error);
  lua_createtable(L, 0, 2);
  lua_pushinteger(L, r.applied);
  lua_setfield(L, -2, "applied");
  lua_createtable(L, static_cast<int>(r.rejected.size()), 0);
  int i = 1;
  for (const CheatReject& rj : r.rejected) {
    lua_createtable(L, 0, 2);
    setStr(L, "id", rj.id);
    setStr(L, "reason", rj.reason);
    lua_rawseti(L, -2, i++);
  }
  lua_setfield(L, -2, "rejected");
  return 1;
}

int cheatsClear(lua_State* L) {
  need(L, "cheats");
  Plugin* p = self(L);
  const auto& emu = p->engine->ctx.emulation;
  if (!emu) return pushNilMsg(L, QStringLiteral("no_game"));
  return pushWriteResult(L, emu->clearCheats(p->id()));
}

int cheatsDatabase(lua_State* L) {
  need(L, "cheats");
  const auto& emu = self(L)->engine->ctx.emulation;
  if (!emu) return pushNilMsg(L, QStringLiteral("no_game"));
  const CheatDatabaseResult r = emu->cheatDatabase();
  if (!r.error.isEmpty()) return pushNilMsg(L, r.error);
  pushValue(L, valueFromJson(r.data));
  return 1;
}

// ------------------------------------------------------------------ uz.save.snapshot (no permission)

constexpr int kMaxSnapshotReason = 80;

int saveSnapshot(lua_State* L) {
  size_t n = 0;
  const char* s = luaL_checklstring(L, 1, &n);
  const QString reason = QString::fromUtf8(s, static_cast<qsizetype>(n)).trimmed();
  Plugin* p = self(L);
  if (reason.isEmpty()) return pushNilMsg(L, QStringLiteral("reason is empty"));
  if (reason.size() > kMaxSnapshotReason) return pushNilMsg(L, QStringLiteral("reason is longer than 80 characters"));
  const auto& emu = p->engine->ctx.emulation;
  if (!emu) return pushNilMsg(L, QStringLiteral("no_game"));
  const auto now = std::chrono::steady_clock::now();
  if (p->snapshotLast && now - *p->snapshotLast < std::chrono::minutes(1)) return pushNilMsg(L, QStringLiteral("rate_limited"));
  const QString err = emu->snapshot(p->id(), p->info.manifest.name, reason);
  if (!err.isEmpty()) return pushNilMsg(L, err);
  p->snapshotLast = now;
  lua_pushboolean(L, 1);
  return 1;
}

// ------------------------------------------------------------------ uz.store (permission store)

QString storeKey(lua_State* L, int idx) {
  const QString key = QString::fromStdString(checkStr(L, idx));
  if (!PluginStore::validKey(key)) luaL_error(L, "invalid store key '%s' (use a-z 0-9 _ . - , 1 to 64 characters)", key.toUtf8().constData());
  return key;
}

PluginStore* storeOf(lua_State* L) {
  need(L, "store");
  return self(L)->engine->ctx.store.get();
}

int storeGet(lua_State* L) {
  PluginStore* st = storeOf(L);
  const QString key = storeKey(L, 1);
  if (!st) return pushNilMsg(L, QStringLiteral("unavailable"));
  const PluginStore::Entry e = st->get(self(L)->id(), key);
  if (!e.found) {
    lua_pushnil(L);
    lua_pushinteger(L, 0);
    return 2;
  }
  pushValue(L, valueFromJson(e.value));
  lua_pushinteger(L, static_cast<lua_Integer>(e.revision));
  return 2;
}

int storePut(lua_State* L) {
  PluginStore* st = storeOf(L);
  const QString key = storeKey(L, 1);
  if (lua_isnoneornil(L, 2)) luaL_error(L, "uz.store.put: value must not be nil");
  Value v;
  std::string err;
  if (!readValue(L, 2, v, err)) luaL_error(L, "uz.store.put: %s", err.c_str());
  QJsonValue json;
  QString e;
  if (!jsonFromValue(v, json, e)) luaL_error(L, "uz.store.put: %s", e.toUtf8().constData());
  std::optional<qint64> base;
  if (!lua_isnoneornil(L, 3)) base = static_cast<qint64>(luaL_checkinteger(L, 3));
  if (!st) return pushNilMsg(L, QStringLiteral("unavailable"));
  using K = PluginStore::PutResult::Kind;
  const PluginStore::PutResult r = st->put(self(L)->id(), key, json, base);
  switch (r.kind) {
    case K::Ok:
      lua_pushboolean(L, 1);
      lua_pushinteger(L, static_cast<lua_Integer>(r.revision));
      return 2;
    case K::Conflict: return pushNilMsg(L, QStringLiteral("conflict"));
    case K::TooLarge: return pushNilMsg(L, QStringLiteral("too_large"));
    case K::Quota: return pushNilMsg(L, QStringLiteral("quota_exceeded"));
    case K::Invalid: break;
  }
  return pushNilMsg(L, QStringLiteral("invalid"));
}

int storeConflicts(lua_State* L) {
  PluginStore* st = storeOf(L);
  const QString key = storeKey(L, 1);
  lua_newtable(L);
  if (!st) return 1;
  int i = 1;
  for (const PluginStore::Conflict& c : st->conflicts(self(L)->id(), key)) {
    lua_createtable(L, 0, 3);
    pushValue(L, valueFromJson(c.value));
    lua_setfield(L, -2, "value");
    lua_pushinteger(L, static_cast<lua_Integer>(c.revision));
    lua_setfield(L, -2, "revision");
    setStr(L, "source", c.source);
    lua_rawseti(L, -2, i++);
  }
  return 1;
}

// ------------------------------------------------------------------ uz.sound (permission sound)

constexpr size_t kMaxSoundClips = 8;

// Path inside the plugin package: relative, no "..", no backslash or drive letter.
bool soundPathOk(const QString& rel) {
  if (rel.isEmpty() || rel.startsWith(QLatin1Char('/')) || rel.contains(QLatin1Char('\\')) || rel.contains(QLatin1Char(':')) ||
      rel.contains(QLatin1Char('\0')))
    return false;
  for (const QString& seg : rel.split(QLatin1Char('/')))
    if (seg == QLatin1String("..")) return false;
  return true;
}

// The file below the plugin folder: a regular file that is not a link and stays inside the package; empty otherwise.
QString soundFile(const Plugin& p, const QString& rel) {
  const QString root = QFileInfo(p.info.path).canonicalFilePath();
  const QFileInfo fi(QDir(p.info.path).filePath(rel));
  if (root.isEmpty() || fi.isSymLink() || !fi.isFile()) return {};
  const QString real = fi.canonicalFilePath();
  return real.startsWith(root + QLatin1Char('/')) ? real : QString();
}

int soundPlay(lua_State* L) {
  need(L, "sound");
  Plugin* p = self(L);
  const QString rel = QString::fromStdString(checkStr(L, 1));
  double volume = 1.0;
  if (!lua_isnoneornil(L, 2)) {
    luaL_checktype(L, 2, LUA_TTABLE);
    lua_getfield(L, 2, "volume");
    if (!lua_isnil(L, -1)) {
      if (!lua_isnumber(L, -1)) luaL_error(L, "uz.sound.play: volume must be a number between 0 and 1");
      volume = lua_tonumber(L, -1);
      if (!(volume >= 0.0 && volume <= 1.0)) luaL_error(L, "uz.sound.play: volume must be between 0 and 1");
    }
    lua_pop(L, 1);
  }
  const auto& sink = p->engine->ctx.sound;
  if (!sink) return pushNilMsg(L, QStringLiteral("unavailable"));
  if (!soundPathOk(rel)) return pushNilMsg(L, QStringLiteral("invalid_path"));
  const QString path = soundFile(*p, rel);
  if (path.isEmpty()) return pushNilMsg(L, QStringLiteral("not_found"));
  if (sink->activeVoices(p->id()) >= kMaxSoundVoices) return pushNilMsg(L, QStringLiteral("too_many_voices"));
  std::shared_ptr<const SoundClip> clip;
  if (auto it = p->soundClips.find(path); it != p->soundClips.end()) {
    clip = it->second;
  } else {
    if (QFileInfo(path).size() > kMaxSoundFileBytes) return pushNilMsg(L, QStringLiteral("too_large"));
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return pushNilMsg(L, QStringLiteral("not_found"));
    QString err;
    clip = decodeWav(f.readAll(), &err);
    if (!clip) return pushNilMsg(L, err);
    if (p->soundClips.size() >= kMaxSoundClips) p->soundClips.clear();
    p->soundClips[path] = clip;
  }
  const quint64 h = sink->play(p->id(), clip, volume);
  if (h == 0) return pushNilMsg(L, QStringLiteral("unavailable"));
  lua_pushinteger(L, static_cast<lua_Integer>(h));
  return 1;
}

int soundStopAll(lua_State* L) {
  need(L, "sound");
  if (const auto& sink = self(L)->engine->ctx.sound) sink->stopAll(self(L)->id());
  lua_pushboolean(L, 1);
  return 1;
}

// ------------------------------------------------------------------ uz.ui.pick_file (permission ui)

constexpr lua_Integer kPickDefaultMax = 1024 * 1024;
constexpr lua_Integer kPickMaxMax = 8 * 1024 * 1024;

// uz.ui.pick_file{title=, filters={"*.cht"}, max_bytes=} , callback: opens a file dialog; callback(content, {name=, size=}) on
// success, callback(nil) when cancelled, callback(nil, "too_large" | "read_failed") otherwise. The plugin never learns the path.
int uiPickFile(lua_State* L) {
  need(L, "ui");
  luaL_checktype(L, 1, LUA_TTABLE);
  int cbIdx = 0;
  if (lua_isfunction(L, 2)) {
    cbIdx = 2;
  } else {
    lua_getfield(L, 1, "callback");
    if (lua_isfunction(L, -1)) cbIdx = lua_gettop(L);
    else lua_pop(L, 1);
  }
  if (cbIdx == 0) luaL_error(L, "uz.ui.pick_file: a callback function is required");
  PickRequest req;
  req.title = tableString(L, 1, "title", false, 100);
  lua_getfield(L, 1, "filters");
  if (!lua_isnil(L, -1)) {
    if (!lua_istable(L, -1)) luaL_error(L, "uz.ui.pick_file: filters must be a list of patterns like \"*.cht\"");
    const lua_Integer n = luaL_len(L, -1);
    if (n > 12) luaL_error(L, "uz.ui.pick_file: at most 12 filters");
    for (lua_Integer i = 1; i <= n; ++i) {
      lua_rawgeti(L, -1, i);
      if (lua_type(L, -1) != LUA_TSTRING) luaL_error(L, "uz.ui.pick_file: filters must be strings");
      const QString f = QString::fromUtf8(lua_tostring(L, -1));
      static const QRegularExpression ok(QStringLiteral("^\\*\\.[A-Za-z0-9]{1,10}$"));
      if (!ok.match(f).hasMatch()) luaL_error(L, "uz.ui.pick_file: filter '%s' must look like \"*.cht\"", f.toUtf8().constData());
      req.filters << f;
      lua_pop(L, 1);
    }
  }
  lua_pop(L, 1);
  lua_getfield(L, 1, "max_bytes");
  lua_Integer maxBytes = kPickDefaultMax;
  if (!lua_isnil(L, -1)) {
    if (!lua_isinteger(L, -1)) luaL_error(L, "uz.ui.pick_file: max_bytes must be an integer");
    maxBytes = lua_tointeger(L, -1);
    if (maxBytes < 1 || maxBytes > kPickMaxMax) luaL_error(L, "uz.ui.pick_file: max_bytes must be between 1 and 8388608");
  }
  lua_pop(L, 1);
  req.maxBytes = maxBytes;
  Plugin* p = self(L);
  if (p->pickRefs.size() >= 1) {
    lua_pushnil(L);
    lua_pushliteral(L, "busy");
    return 2;
  }
  lua_pushvalue(L, cbIdx);
  req.id = p->pickNextId++;
  req.pluginId = p->id();
  p->pickRefs[req.id] = luaL_ref(L, LUA_REGISTRYINDEX);
  p->engine->store.ui.addPickRequest(req);
  lua_pushboolean(L, 1);
  return 1;
}


// ------------------------------------------------------------------ uz.run (permission run.state)

RunBridge* runBridge(lua_State* L) {
  need(L, "run.state");
  return self(L)->engine->ctx.run.get();
}

Value fieldOf(const Value& t, const char* name) {
  for (size_t i = 0; i < t.keys.size(); ++i)
    if (t.keys[i].type == Value::Type::String && t.keys[i].s == name) return t.vals[i];
  return Value();
}

int pushRefusal(lua_State* L, const QString& why) {
  lua_pushnil(L);
  const QByteArray u = why.toUtf8();
  lua_pushlstring(L, u.constData(), static_cast<size_t>(u.size()));
  return 2;
}

int runCurrent(lua_State* L) {
  const QJsonObject c = runBridge(L)->current();
  if (c.isEmpty()) {
    lua_pushnil(L);
    return 1;
  }
  pushValue(L, valueFromJson(c));
  return 1;
}

// uz.run.state([user_id [, plugin_id]]): defaults are the own user and the calling plugin.
int runState(lua_State* L) {
  RunBridge* b = runBridge(L);
  const QString user = lua_isnoneornil(L, 1) ? b->current().value(QStringLiteral("me")).toString() : QString::fromStdString(checkStr(L, 1));
  const QString plugin = lua_isnoneornil(L, 2) ? self(L)->id() : QString::fromStdString(checkStr(L, 2));
  bool found = false;
  const QJsonObject st = b->stateOf(user, plugin, &found);
  if (!found) {
    lua_pushnil(L);
    return 1;
  }
  pushValue(L, valueFromJson(st));
  return 1;
}

// uz.run.states([plugin_id]): {user_id = state} of that plugin (default: the calling plugin).
int runStates(lua_State* L) {
  RunBridge* b = runBridge(L);
  const QString plugin = lua_isnoneornil(L, 1) ? self(L)->id() : QString::fromStdString(checkStr(L, 1));
  pushValue(L, valueFromJson(b->states(plugin)));
  return 1;
}

// uz.run.setState(tbl): replaces the calling plugin's state of the own participant. Coalesced per plugin: the bridge keeps
// the latest value and the RunClient sends at most 4 writes per second for each plugin.
int runSetState(lua_State* L) {
  RunBridge* b = runBridge(L);
  luaL_checktype(L, 1, LUA_TTABLE);
  Value v;
  std::string err;
  if (!readValue(L, 1, v, err)) luaL_error(L, "uz.run.setState: %s", err.c_str());
  QJsonObject o;
  QString e;
  if (!jsonObjectFromValue(v, o, e)) luaL_error(L, "uz.run.setState: %s", e.toUtf8().constData());
  e = b->setState(self(L)->id(), o);
  if (!e.isEmpty()) return pushRefusal(L, e);
  lua_pushboolean(L, 1);
  return 1;
}

int runEvent(lua_State* L) {
  RunBridge* b = runBridge(L);
  luaL_checktype(L, 1, LUA_TTABLE);
  Value v;
  std::string err;
  if (!readValue(L, 1, v, err)) luaL_error(L, "uz.run.event: %s", err.c_str());
  const Value type = fieldOf(v, "type");
  const Value text = fieldOf(v, "text");
  const Value payload = fieldOf(v, "payload");
  const Value source = fieldOf(v, "source");
  if (type.type != Value::Type::String) luaL_error(L, "uz.run.event: type must be a string");
  if (text.type != Value::Type::Nil && text.type != Value::Type::String) luaL_error(L, "uz.run.event: text must be a string");
  if (source.type != Value::Type::Nil && source.type != Value::Type::String) luaL_error(L, "uz.run.event: source must be \"automatic\" or \"manual\"");
  QJsonObject po;
  QString e;
  if (payload.type != Value::Type::Nil && !jsonObjectFromValue(payload, po, e)) luaL_error(L, "uz.run.event: payload: %s", e.toUtf8().constData());
  QString clientId;
  e = b->postEvent(self(L)->id(), source.type == Value::Type::Nil ? QStringLiteral("automatic") : QString::fromStdString(source.s),
                   QString::fromStdString(type.s), QString::fromStdString(text.s), po, &clientId);
  if (!e.isEmpty()) return pushRefusal(L, e);
  const QByteArray u = clientId.toUtf8();
  lua_pushlstring(L, u.constData(), static_cast<size_t>(u.size()));
  return 1;
}

int runEvents(lua_State* L) {
  RunBridge* b = runBridge(L);
  const lua_Integer after = luaL_optinteger(L, 1, 0);
  const lua_Integer limit = luaL_optinteger(L, 2, 100);
  if (limit < 1 || limit > 500) luaL_error(L, "uz.run.events: limit must be 1 to 500");
  pushValue(L, valueFromJson(b->events(static_cast<qint64>(after), static_cast<int>(limit))));
  return 1;
}

int runOn(lua_State* L) {
  runBridge(L);
  Plugin* p = self(L);
  const std::string kind = checkStr(L, 1);
  if (kind != "event" && kind != "state" && kind != "status" && kind != "participants")
    luaL_error(L, "uz.run.on: unknown kind '%s' (use \"event\", \"state\", \"status\" or \"participants\")", kind.c_str());
  luaL_checktype(L, 2, LUA_TFUNCTION);
  if (p->runSubs.size() >= 16) luaL_error(L, "too many uz.run.on callbacks (max 16)");
  lua_pushvalue(L, 2);
  p->runSubs.push_back({kind, luaL_ref(L, LUA_REGISTRYINDEX)});
  return 0;
}

int runSetBoard(lua_State* L) {
  RunBridge* b = runBridge(L);
  luaL_checktype(L, 1, LUA_TTABLE);
  const Value v = readUiArg(L, 1);
  QVariantMap tree;
  QString e;
  if (!convertUiTree(v, tree, e)) luaL_error(L, "ui tree: %s", e.toUtf8().constData());
  const QJsonObject json = QJsonObject::fromVariantMap(tree);
  Plugin* p = self(L);
  const auto now = std::chrono::steady_clock::now();
  if (now - p->boardLast >= std::chrono::milliseconds(1000)) {
    p->boardLast = now;
    p->hasBoardPending = false;
    e = b->setBoard(p->id(), json);
    if (!e.isEmpty()) return pushRefusal(L, e);
  } else {
    p->boardPending = json;  // at most one board per second: the latest tree goes out when the second is over
    p->hasBoardPending = true;
  }
  lua_pushboolean(L, 1);
  return 1;
}

// ------------------------------------------------------------------ uz.i18n

void flatten(const QString& prefix, const QJsonObject& o, QHash<QString, QString>& out) {
  for (auto it = o.begin(); it != o.end(); ++it) {
    if (it.value().isString()) out.insert(prefix + it.key(), it.value().toString());
    else if (it.value().isObject()) flatten(prefix + it.key() + QLatin1Char('.'), it.value().toObject(), out);
  }
}

const QHash<QString, QString>* i18nFile(Plugin& p, const QString& lang) {
  auto it = p.i18n.find(lang);
  if (it == p.i18n.end()) {
    std::optional<QHash<QString, QString>> loaded;
    static const QRegularExpression ok(QStringLiteral("^[A-Za-z0-9_-]{1,16}$"));
    if (ok.match(lang).hasMatch()) {
      QFile f(QDir(p.info.path).filePath(QStringLiteral("i18n/%1.json").arg(lang)));
      if (f.open(QIODevice::ReadOnly) && f.size() < 4 * 1024 * 1024) {
        const QJsonDocument d = QJsonDocument::fromJson(f.readAll());
        if (d.isObject()) {
          QHash<QString, QString> h;
          flatten(QString(), d.object(), h);
          loaded = std::move(h);
        }
      }
    }
    it = p.i18n.insert(lang, std::move(loaded));
  }
  return it.value() ? &*it.value() : nullptr;
}

int i18nT(lua_State* L) {
  return guard(L, [&] {
    Plugin* p = self(L);
    const QString key = QString::fromStdString(checkStr(L, 1));
    std::map<QString, QString> vars;
    if (lua_istable(L, 2)) {
      lua_pushnil(L);
      while (lua_next(L, 2) != 0) {
        if (lua_type(L, -2) == LUA_TSTRING) {
          size_t n = 0;
          const char* s = luaL_tolstring(L, -1, &n);
          vars[QString::fromUtf8(lua_tostring(L, -3))] = QString::fromUtf8(s, static_cast<qsizetype>(n));
          lua_pop(L, 1);
        }
        lua_pop(L, 1);
      }
    }
    QStringList langs;
    const QString given = p->engine->ctx.language.trimmed();
    if (!given.isEmpty()) {
      langs << given;
      const QString norm = QString(given).replace(QLatin1Char('_'), QLatin1Char('-'));
      langs << norm << norm.section(QLatin1Char('-'), 0, 0).toLower();
    }
    langs << QStringLiteral("en");
    langs.removeDuplicates();
    QString text = key;
    for (const QString& lang : std::as_const(langs)) {
      const QHash<QString, QString>* h = i18nFile(*p, lang);
      if (h && h->contains(key)) {
        text = h->value(key);
        break;
      }
    }
    QString out;
    for (qsizetype i = 0; i < text.size(); ++i) {
      if (text[i] == QLatin1Char('{')) {
        const qsizetype close = text.indexOf(QLatin1Char('}'), i);
        if (close > i) {
          const QString name = text.mid(i + 1, close - i - 1);
          const auto v = vars.find(name);
          if (v != vars.end()) {
            out += v->second;
            i = close;
            continue;
          }
        }
      }
      out += text[i];
    }
    const QByteArray u = out.toUtf8();
    lua_pushlstring(L, u.constData(), static_cast<size_t>(u.size()));
    return 1;
  });
}

// uz.game.hash(): SHA-256 of the game file (hex) for the cheat database match, nil without a game.
int gameHash(lua_State* L) {
  const QString h = self(L)->engine->ctx.sha256;
  if (h.isEmpty()) {
    lua_pushnil(L);
    return 1;
  }
  const QByteArray u = h.toUtf8();
  lua_pushlstring(L, u.constData(), static_cast<size_t>(u.size()));
  return 1;
}

int clockFn(lua_State* L) {
  using namespace std::chrono;
  lua_pushinteger(L, static_cast<lua_Integer>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count()));
  return 1;
}

void setStr(lua_State* L, const char* k, const QString& v) {
  if (v.isEmpty()) return;
  const QByteArray u = v.toUtf8();
  lua_pushlstring(L, u.constData(), static_cast<size_t>(u.size()));
  lua_setfield(L, -2, k);
}

void setFn(lua_State* L, const char* k, lua_CFunction f) {
  lua_pushcfunction(L, f);
  lua_setfield(L, -2, k);
}

}  // namespace

void registerFbApi(PluginEngine& engine, Plugin& p) {
  lua_State* L = p.lua->L();
  const GameContext& g = engine.ctx;
  lua_newtable(L);  // fb
  lua_pushinteger(L, kPluginApiVersion);
  lua_setfield(L, -2, "api");

  lua_newtable(L);  // uz.plugin
  setStr(L, "id", p.info.manifest.id);
  setStr(L, "version", p.info.manifest.version.toString());
  lua_setfield(L, -2, "plugin");

  lua_newtable(L);  // uz.game
  setStr(L, "system", g.system);
  setStr(L, "code", g.code);
  setStr(L, "sha256", g.sha256);
  setStr(L, "title", g.title);
  if (!g.shell) setStr(L, "language", g.language);
  if (!g.shell) setFn(L, "hash", gameHash);  // the shell has no game: uz.game stays empty
  lua_setfield(L, -2, "game");

  lua_newtable(L);  // uz.log
  static const struct { const char* name; LogLevel level; } levels[] = {
      {"debug", LogLevel::Debug}, {"info", LogLevel::Info}, {"warn", LogLevel::Warn}, {"error", LogLevel::Error}};
  for (const auto& lv : levels) {
    lua_pushinteger(L, static_cast<lua_Integer>(lv.level));
    lua_pushcclosure(L, logFn, 1);
    lua_setfield(L, -2, lv.name);
  }
  lua_setfield(L, -2, "log");

  lua_newtable(L);  // uz.memory
  setFn(L, "watch", memWatch);
  setFn(L, "unwatch", memUnwatch);
  setFn(L, "bytes", memBytes);
  setFn(L, "frame", memFrame);
  setFn(L, "write", memWrite);
  static const struct { const char* name; int size; } wnums[] = {{"write_u8", 1}, {"write_u16", 2}, {"write_u32", 4}};
  for (const auto& n : wnums) {
    lua_pushinteger(L, n.size);
    lua_pushcclosure(L, memWriteNum, 1);
    lua_setfield(L, -2, n.name);
  }
  static const struct { const char* name; int size; bool sign; } nums[] = {
      {"u8", 1, false}, {"u16", 2, false}, {"u32", 4, false}, {"u64", 8, false}, {"s8", 1, true}, {"s16", 2, true}, {"s32", 4, true}};
  for (const auto& n : nums) {
    lua_pushinteger(L, n.size | (n.sign ? 0x100 : 0));
    lua_pushcclosure(L, memNum, 1);
    lua_setfield(L, -2, n.name);
  }
  lua_setfield(L, -2, "memory");

  setFn(L, "on_tick", onTick);

  lua_newtable(L);  // uz.events
  setFn(L, "emit", eventsEmit);
  setFn(L, "on", eventsOn);
  lua_setfield(L, -2, "events");

  setFn(L, "provide", provide);
  setFn(L, "call", call);

  lua_newtable(L);  // uz.dev
  setFn(L, "watch", devWatch);
  setFn(L, "unwatch", devUnwatch);
  lua_setfield(L, -2, "dev");

  lua_newtable(L);  // uz.rom
  setFn(L, "size", romSize);
  setFn(L, "read", romRead);
  setFn(L, "romfs", romRomfs);
  setFn(L, "romfs_list", romRomfsList);
  lua_setfield(L, -2, "rom");

  lua_newtable(L);  // uz.video
  setFn(L, "screens", videoScreens);
  setFn(L, "capture", videoCapture);
  lua_setfield(L, -2, "video");

  if (p.info.manifest.hasPermission(QStringLiteral("variants.build"))) {
    lua_newtable(L);  // uz.variants (only with the permission variants.build)
    setFn(L, "register", variantsRegister);
    setFn(L, "engine", variantsEngine);
    setFn(L, "list", variantsList);
    setFn(L, "build", variantsBuild);
    lua_setfield(L, -2, "variants");
  }
  lua_newtable(L);  // uz.cheats (permission cheats)
  setFn(L, "formats", cheatsFormats);
  setFn(L, "allowed", cheatsAllowed);
  setFn(L, "apply", cheatsApply);
  setFn(L, "clear", cheatsClear);
  setFn(L, "database", cheatsDatabase);
  lua_setfield(L, -2, "cheats");

  lua_newtable(L);  // uz.save (no permission)
  setFn(L, "snapshot", saveSnapshot);
  lua_setfield(L, -2, "save");

  lua_newtable(L);  // uz.store (permission store)
  setFn(L, "get", storeGet);
  setFn(L, "put", storePut);
  setFn(L, "conflicts", storeConflicts);
  lua_setfield(L, -2, "store");

  lua_newtable(L);  // uz.sound (permission sound)
  setFn(L, "play", soundPlay);
  setFn(L, "stop_all", soundStopAll);
  lua_setfield(L, -2, "sound");

  lua_newtable(L);  // uz.codec (pure functions, no permission)
  setFn(L, "lz_decompress", codecLz);
  setFn(L, "garc", codecGarc);
  setFn(L, "garc_file", codecGarcFile);
  setFn(L, "ctr_texture", codecCtrTexture);
  setFn(L, "bclim", codecBclim);
  setFn(L, "etc1", codecEtc1);
  setFn(L, "png", codecPng);
  setFn(L, "pk6_decrypt", codecPk6Decrypt);
  setFn(L, "pk6_encrypt", codecPk6Encrypt);
  lua_setfield(L, -2, "codec");

  lua_newtable(L);  // uz.cache
  setFn(L, "read", cacheRead);
  setFn(L, "write", cacheWrite);
  setFn(L, "exists", cacheExists);
  lua_setfield(L, -2, "cache");

  lua_newtable(L);  // uz.ui
  setFn(L, "set", uiSet);
  setFn(L, "patch", uiPatch);
  setFn(L, "clear", uiClear);
  setFn(L, "title", uiTitle);
  setFn(L, "badge", uiBadge);
  setFn(L, "dot", uiDot);
  setFn(L, "notify", uiNotify);
  setFn(L, "open", uiOpen);
  setFn(L, "on_action", uiOnAction);
  setFn(L, "pick_file", uiPickFile);
  setFn(L, "close", uiClose);
  lua_setfield(L, -2, "ui");

  lua_newtable(L);  // uz.i18n
  setFn(L, "t", i18nT);
  lua_setfield(L, -2, "i18n");

  if (g.run) {
    lua_newtable(L);  // uz.run (only when the game was started in a run)
    setFn(L, "current", runCurrent);
    setFn(L, "state", runState);
    setFn(L, "states", runStates);
    setFn(L, "setState", runSetState);
    setFn(L, "event", runEvent);
    setFn(L, "events", runEvents);
    setFn(L, "on", runOn);
    setFn(L, "setBoard", runSetBoard);
    lua_setfield(L, -2, "run");
  }

  setFn(L, "clock", clockFn);
  // Namespace `uz` (Uzume 0.22); `fb` stays as an alias of the same table for plugins written for older apps.
  lua_pushvalue(L, -1);
  lua_setglobal(L, "fb");
  lua_setglobal(L, "uz");
}

}  // namespace uzume::plugins
