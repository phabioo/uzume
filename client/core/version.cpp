#include "version.h"

#include "hubprotocol.h"

#ifndef UZUME_VERSION_STR
#define UZUME_VERSION_STR "0.0.0-dev"
#endif
#ifndef UZUME_CHANNEL_STR
#define UZUME_CHANNEL_STR "dev"
#endif
#ifndef UZUME_COMMIT_STR
#define UZUME_COMMIT_STR ""
#endif

namespace uzume {

namespace {
std::string jsonEscape(std::string_view in) {
  std::string out;
  for (const char c : in) {
    if (c == '"' || c == '\\') {
      out += '\\';
      out += c;
    } else if (static_cast<unsigned char>(c) < 0x20) {
      out += ' ';
    } else {
      out += c;
    }
  }
  return out;
}
}  // namespace

std::string_view playerVersion() noexcept { return UZUME_VERSION_STR; }
std::string_view playerChannel() noexcept { return UZUME_CHANNEL_STR; }
std::string_view playerCommit() noexcept { return UZUME_COMMIT_STR; }

std::string displayVersion(std::string_view version) {
  const auto isNumber = [](std::string_view f) {
    if (f.empty()) return false;
    for (const char c : f)
      if (c < '0' || c > '9') return false;
    return true;
  };
  const auto d1 = version.find('.');
  if (d1 == std::string_view::npos) return std::string(version);
  const auto d2 = version.find('.', d1 + 1);
  if (d2 == std::string_view::npos) return std::string(version);
  const auto end = version.find_first_of("-+", d2 + 1);
  const std::string_view third = version.substr(d2 + 1, end == std::string_view::npos ? end : end - d2 - 1);
  if (!isNumber(version.substr(0, d1)) || !isNumber(version.substr(d1 + 1, d2 - d1 - 1)) || third != "0" || d1 > 9 ||
      std::stoi(std::string(version.substr(0, d1))) < 21) {
    return std::string(version);
  }
  return std::string(version.substr(0, d2)) + (end == std::string_view::npos ? std::string() : std::string(version.substr(end)));
}

std::string playerVersionJson() {
  return "{\"product\":\"player\",\"version\":\"" + jsonEscape(playerVersion()) + "\",\"channel\":\"" +
         jsonEscape(playerChannel()) + "\",\"commit\":\"" + jsonEscape(playerCommit()) +
         "\",\"protocol_version\":" + std::to_string(kProtocolVersion) +
         ",\"min_protocol_version\":" + std::to_string(kMinProtocolVersion) + ",\"msi_downgrade\":true}";
}

}  // namespace uzume
