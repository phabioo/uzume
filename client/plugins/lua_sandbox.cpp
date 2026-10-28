#include "lua_sandbox.h"

#include <cstdlib>
#include <cstring>
#include <exception>
#include <stdexcept>

#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"

// Called by the patched pattern matcher of the vendored lstrlib.c every 1024 matcher steps (global C++ symbol, see
// third_party/lua-5.4.7/README.md). A pattern such as ("a"):rep(40000):find(".-b") needs seconds inside one C call, which
// the instruction hook cannot interrupt: the matcher budget aborts it.
void fbMatchBudget(lua_State* L) { uzume::plugins::LuaSandbox::matchStep(L); }

namespace uzume::plugins {

namespace {

constexpr int kHookInterval = 1000;
constexpr lua_Integer kMaxRepBytes = 16 * 1024 * 1024;  // string.rep result (the memory limit alone allows 64 MiB spikes)

// tostring without host addresses (ASLR): objects without __tostring show "<type or __name>: #<id>", the id counts per state.
const char* safeToString(lua_State* L, int idx, size_t* len) {
  idx = lua_absindex(L, idx);
  const int t = lua_type(L, idx);
  if (t != LUA_TTABLE && t != LUA_TFUNCTION && t != LUA_TUSERDATA && t != LUA_TTHREAD && t != LUA_TLIGHTUSERDATA) return luaL_tolstring(L, idx, len);
  if (luaL_getmetafield(L, idx, "__tostring") != LUA_TNIL) {
    lua_pop(L, 1);
    return luaL_tolstring(L, idx, len);
  }
  const int top = lua_gettop(L);
  const char* name = luaL_typename(L, idx);
  if (luaL_getmetafield(L, idx, "__name") == LUA_TSTRING) name = lua_tostring(L, -1);
  if (lua_getfield(L, LUA_REGISTRYINDEX, "uz.ids") != LUA_TTABLE) {
    lua_pop(L, 1);
    lua_createtable(L, 0, 8);
    lua_createtable(L, 0, 1);
    lua_pushstring(L, "k");
    lua_setfield(L, -2, "__mode");
    lua_setmetatable(L, -2);
    lua_pushvalue(L, -1);
    lua_setfield(L, LUA_REGISTRYINDEX, "uz.ids");
  }
  lua_pushvalue(L, idx);
  lua_rawget(L, -2);
  lua_Integer id = lua_tointeger(L, -1);
  lua_pop(L, 1);
  if (id == 0) {
    lua_getfield(L, LUA_REGISTRYINDEX, "uz.idn");
    id = lua_tointeger(L, -1) + 1;
    lua_pop(L, 1);
    lua_pushinteger(L, id);
    lua_setfield(L, LUA_REGISTRYINDEX, "uz.idn");
    lua_pushvalue(L, idx);
    lua_pushinteger(L, id);
    lua_rawset(L, -3);
  }
  const std::string text = std::string(name) + ": #" + std::to_string(id);
  lua_settop(L, top);
  lua_pushlstring(L, text.data(), text.size());
  if (len) *len = text.size();
  return lua_tostring(L, -1);
}

int fbTostring(lua_State* L) {
  luaL_checkany(L, 1);
  safeToString(L, 1, nullptr);
  return 1;
}

// string.format: no "%p" (host pointers), "%s" of tables/functions/userdata goes through safeToString.
int fbFormat(lua_State* L) {
  const int top = lua_gettop(L);
  size_t flen = 0;
  const char* f = luaL_checklstring(L, 1, &flen);
  int arg = 1;
  for (size_t i = 0; i < flen; ++i) {
    if (f[i] != '%') continue;
    if (i + 1 < flen && f[i + 1] == '%') {
      ++i;
      continue;
    }
    size_t j = i + 1;
    while (j < flen && std::strchr("-+ #0123456789.", f[j]) != nullptr) ++j;
    const char conv = j < flen ? f[j] : '\0';
    ++arg;
    if (conv == 'p') return luaL_error(L, "string.format: '%%p' is not available in plugins");
    if (conv == 's' && arg <= top) {
      const int t = lua_type(L, arg);
      if (t == LUA_TTABLE || t == LUA_TFUNCTION || t == LUA_TUSERDATA || t == LUA_TTHREAD || t == LUA_TLIGHTUSERDATA) {
        safeToString(L, arg, nullptr);
        lua_replace(L, arg);
      }
    }
    i = j;
  }
  lua_pushvalue(L, lua_upvalueindex(1));
  lua_insert(L, 1);
  lua_call(L, top, LUA_MULTRET);
  return lua_gettop(L);
}

int fbRep(lua_State* L) {
  size_t l = 0, lsep = 0;
  luaL_checklstring(L, 1, &l);
  const lua_Integer n = luaL_checkinteger(L, 2);
  luaL_optlstring(L, 3, "", &lsep);
  if (n > 1 && static_cast<double>(l + lsep) * static_cast<double>(n) > static_cast<double>(kMaxRepBytes))
    return luaL_error(L, "string.rep: result larger than %d MiB", static_cast<int>(kMaxRepBytes / (1024 * 1024)));
  const int top = lua_gettop(L);
  lua_pushvalue(L, lua_upvalueindex(1));
  lua_insert(L, 1);
  lua_call(L, top, LUA_MULTRET);
  return lua_gettop(L);
}

// setmetatable without __gc: a finalizer would run inside lua_close with no budget (hang on pause/reload/stop).
int fbSetmetatable(lua_State* L) {
  if (lua_type(L, 2) == LUA_TTABLE) {
    lua_pushstring(L, "__gc");
    lua_rawget(L, 2);
    const bool has = !lua_isnil(L, -1);
    lua_pop(L, 1);
    if (has) return luaL_error(L, "setmetatable: __gc metamethods are not allowed in plugins");
  }
  const int top = lua_gettop(L);
  lua_pushvalue(L, lua_upvalueindex(1));
  lua_insert(L, 1);
  lua_call(L, top, LUA_MULTRET);
  return lua_gettop(L);
}

// Replaces table[name] / global name by `fn` with the original as its first upvalue.
void wrapField(lua_State* L, const char* table, const char* name, lua_CFunction fn) {
  if (table) lua_getglobal(L, table); else lua_pushglobaltable(L);
  lua_getfield(L, -1, name);
  lua_pushcclosure(L, fn, 1);
  lua_setfield(L, -2, name);
  lua_pop(L, 1);
}

int panicHandler(lua_State* L) {
  const char* m = lua_tostring(L, -1);
  throw std::runtime_error(std::string("unprotected Lua error: ") + (m ? m : "?"));
}

int luaPrint(lua_State* L) {
  const int n = lua_gettop(L);
  std::string line;
  for (int i = 1; i <= n; ++i) {
    size_t len = 0;
    const char* s = safeToString(L, i, &len);
    if (i > 1) line += '\t';
    line.append(s, len);
    lua_pop(L, 1);
  }
  if (LuaSandbox* sb = LuaSandbox::from(L); sb && sb->printSink()) sb->printSink()(LogLevel::Info, line);
  return 0;
}

int collectgarbageCount(lua_State* L) {
  const char* opt = luaL_optstring(L, 1, "collect");
  if (std::string(opt) == "count") {
    lua_pushnumber(L, static_cast<lua_Number>(lua_gc(L, LUA_GCCOUNT)) + static_cast<lua_Number>(lua_gc(L, LUA_GCCOUNTB)) / 1024.0);
    return 1;
  }
  lua_pushinteger(L, 0);  // every other option is a no-op in the sandbox
  return 1;
}

}  // namespace

LuaSandbox* LuaSandbox::from(lua_State* L) {
  void* ud = nullptr;
  lua_getallocf(L, &ud);
  return static_cast<LuaSandbox*>(ud);
}

void* LuaSandbox::alloc(void* ud, void* ptr, size_t osize, size_t nsize) {
  auto* self = static_cast<LuaSandbox*>(ud);
  if (!ptr) osize = 0;  // for a new block osize encodes the object type
  if (nsize == 0) {
    std::free(ptr);
    self->m_used -= osize;
    return nullptr;
  }
  if (nsize > osize && self->m_used - osize + nsize > self->m_limits.memoryBytes) return nullptr;
  void* p = std::realloc(ptr, nsize);
  if (!p) return nullptr;
  self->m_used = self->m_used - osize + nsize;
  return p;
}

void LuaSandbox::matchStep(lua_State* L) {
  LuaSandbox* self = from(L);
  if (self->m_trip == Trip::None) {
    self->m_matchLeft -= 1024;
    if (self->m_matchLeft > 0 && std::chrono::steady_clock::now() <= self->m_deadline) return;
    self->m_trip = Trip::Time;
  }
  lua_sethook(L, &LuaSandbox::hook, LUA_MASKCOUNT, 1);  // the plugin cannot go on after catching this error
  luaL_error(L, "time budget exceeded (pattern matching budget used up)");
}

void LuaSandbox::hook(lua_State* L, lua_Debug*) {
  LuaSandbox* self = from(L);
  if (self->m_trip == Trip::None) {
    self->m_instrLeft -= kHookInterval;
    if (self->m_instrLeft <= 0) {
      self->m_trip = Trip::Instructions;
    } else if (std::chrono::steady_clock::now() > self->m_deadline) {
      self->m_trip = Trip::Time;
    }
  }
  if (self->m_trip == Trip::None) return;
  // Once tripped, every following instruction raises again so a plugin cannot swallow the error with pcall and go on.
  lua_sethook(L, &LuaSandbox::hook, LUA_MASKCOUNT, 1);
  if (self->m_trip == Trip::Instructions)
    luaL_error(L, "instruction budget exceeded (%I instructions per call)", static_cast<lua_Integer>(self->m_limits.instructions));
  luaL_error(L, "time budget exceeded (%d ms per call)", self->m_limits.wallMs);
}

int LuaSandbox::messageHandler(lua_State* L) {
  const char* msg = lua_tostring(L, 1);
  if (!msg) {
    if (luaL_callmeta(L, 1, "__tostring") && lua_type(L, -1) == LUA_TSTRING) return 1;
    msg = lua_pushfstring(L, "(error object is a %s value)", luaL_typename(L, 1));
  }
  luaL_traceback(L, L, msg, 1);
  return 1;
}

int LuaSandbox::trampoline(lua_State* L) {
  auto* body = static_cast<const std::function<void(lua_State*)>*>(lua_touserdata(L, 1));
  std::string what;
  try {
    (*body)(L);
    return 0;
  } catch (const std::exception& e) {
    what = e.what();
  }
  return luaL_error(L, "%s", what.c_str());
}

LuaSandbox::LuaSandbox(const SandboxLimits& limits) : m_limits(limits) {
  m_L = lua_newstate(&LuaSandbox::alloc, this);
  if (!m_L) return;
  lua_atpanic(m_L, &panicHandler);
  try {
    openLibs();
  } catch (const std::exception&) {
    lua_close(m_L);
    m_L = nullptr;
  }
}

LuaSandbox::~LuaSandbox() {
  if (m_L) lua_close(m_L);
}

int LuaSandbox::openLibsFn(lua_State* L) {
  struct Lib { const char* name; lua_CFunction open; };
  static const Lib libs[] = {{LUA_GNAME, luaopen_base},       {LUA_STRLIBNAME, luaopen_string}, {LUA_TABLIBNAME, luaopen_table},
                             {LUA_MATHLIBNAME, luaopen_math}, {LUA_UTF8LIBNAME, luaopen_utf8},  {LUA_COLIBNAME, luaopen_coroutine}};
  for (const Lib& lib : libs) {
    luaL_requiref(L, lib.name, lib.open, 1);
    lua_pop(L, 1);
  }
  // Remove everything that can load code or touch the host.
  static const char* const removedGlobals[] = {"load", "loadstring", "dofile", "loadfile", "require", "warn"};
  for (const char* n : removedGlobals) {
    lua_pushnil(L);
    lua_setglobal(L, n);
  }
  lua_getglobal(L, "string");
  lua_pushnil(L);
  lua_setfield(L, -2, "dump");
  lua_pop(L, 1);
  lua_pushcfunction(L, collectgarbageCount);
  lua_setglobal(L, "collectgarbage");
  lua_pushcfunction(L, luaPrint);
  lua_setglobal(L, "print");
  lua_pushcfunction(L, fbTostring);
  lua_setglobal(L, "tostring");
  wrapField(L, nullptr, "setmetatable", fbSetmetatable);
  wrapField(L, "string", "format", fbFormat);
  wrapField(L, "string", "rep", fbRep);
  return 0;
}

void LuaSandbox::openLibs() {
  lua_pushcfunction(m_L, &LuaSandbox::openLibsFn);
  if (lua_pcall(m_L, 0, 0, 0) != LUA_OK) {
    const std::string e = lua_tostring(m_L, -1) ? lua_tostring(m_L, -1) : "?";
    throw std::runtime_error(e);
  }
}

bool LuaSandbox::run(const std::function<void(lua_State*)>& body, std::string& err) {
  err.clear();
  if (!m_L) {
    err = "Lua state could not be created";
    return false;
  }
  m_trip = Trip::None;
  m_instrLeft = m_limits.instructions;
  m_matchLeft = m_limits.matchSteps;
  m_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(m_limits.wallMs);
  lua_sethook(m_L, &LuaSandbox::hook, LUA_MASKCOUNT, kHookInterval);

  const int top = lua_gettop(m_L);
  lua_pushcfunction(m_L, &LuaSandbox::messageHandler);
  lua_pushcfunction(m_L, &LuaSandbox::trampoline);
  lua_pushlightuserdata(m_L, const_cast<std::function<void(lua_State*)>*>(&body));
  const int status = lua_pcall(m_L, 1, 0, top + 1);

  lua_sethook(m_L, nullptr, 0, 0);
  bool ok = status == LUA_OK;
  if (!ok) {
    size_t n = 0;
    const char* s = lua_tolstring(m_L, -1, &n);
    err = s ? std::string(s, n) : "unknown error";
    if (status == LUA_ERRMEM) {
      m_trip = Trip::Memory;
      err = "memory limit exceeded (" + std::to_string(m_limits.memoryBytes / (1024 * 1024)) + " MiB)";
    }
  }
  lua_settop(m_L, top);
  if (m_trip == Trip::Instructions || m_trip == Trip::Time) {
    ok = false;
    if (err.empty() || err.find("budget exceeded") == std::string::npos)
      err = (m_trip == Trip::Instructions ? "instruction budget exceeded (" + std::to_string(m_limits.instructions) + " instructions per call)"
                                          : "time budget exceeded (" + std::to_string(m_limits.wallMs) + " ms per call)") +
            "\n" + err;
  }
  return ok;
}

bool LuaSandbox::runChunk(const std::string& code, const std::string& chunkName, std::string& err) {
  return run(
      [&](lua_State* L) {
        // mode "t": precompiled chunks are rejected.
        if (luaL_loadbufferx(L, code.data(), code.size(), chunkName.c_str(), "t") != LUA_OK) lua_error(L);
        lua_call(L, 0, 0);
      },
      err);
}

std::string LuaSandbox::firstLine(const std::string& err) {
  const size_t nl = err.find('\n');
  return nl == std::string::npos ? err : err.substr(0, nl);
}

}  // namespace uzume::plugins
