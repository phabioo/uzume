#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct lua_State;

// Plain-data copy of a Lua value, used to move values between Lua states (events, uz.call, uz.provide results).
// Allowed: nil, boolean, number, string, tables of these. Functions, userdata and threads are rejected.
namespace uzume::plugins {

inline constexpr int kMaxValueDepth = 32;
inline constexpr size_t kMaxValueBytes = 1024 * 1024;

struct Value {
  enum class Type { Nil, Bool, Int, Float, String, Table };
  Type type = Type::Nil;
  bool b = false;
  int64_t i = 0;
  double d = 0;
  std::string s;
  std::vector<Value> keys;  // Table: parallel arrays
  std::vector<Value> vals;
};

// Reads the value at `idx` (deep copy). On failure returns false and sets `err` (e.g. "function values cannot be passed").
bool readValue(lua_State* L, int idx, Value& out, std::string& err, int maxDepth = kMaxValueDepth, size_t maxBytes = kMaxValueBytes);
// Pushes a deep copy of `v` (may raise a Lua memory error).
void pushValue(lua_State* L, const Value& v);
// Approximate host memory of `v` (nodes x sizeof(Value) plus string bytes): what a queued event really costs.
size_t valueBytes(const Value& v);
// Short single-line rendering for the console ("{a=1, b=\"x\"}"), at most `maxLen` characters.
std::string shortText(const Value& v, size_t maxLen = 120);

}  // namespace uzume::plugins
