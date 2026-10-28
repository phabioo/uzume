#pragma once

#include <string>
#include <string_view>

namespace uzume {

// Build identity of the Uzume (CMake cache vars UZUME_VERSION / UZUME_CHANNEL / UZUME_COMMIT).
std::string_view playerVersion() noexcept;  // full SemVer, e.g. "0.3.0-beta.57" or "0.3.0-dev"
std::string_view playerChannel() noexcept;  // compiled default update channel: "stable" | "beta" | "dev"
std::string_view playerCommit() noexcept;   // may be empty

// Display form for people (ADR 0036): drops a third field that is exactly 0, "21.1.0" -> "21.1", "21.1.0-dev.5" -> "21.1-dev.5".
// Anything else ("0.20.15", "dev") is returned unchanged. Comparisons, tags, file names and the index keep the full string.
std::string displayVersion(std::string_view version);

// One JSON object (no trailing newline), for `uzume --version-json`:
// {"product":"player","version":"...","channel":"...","commit":"...","protocol_version":1,"min_protocol_version":1}
std::string playerVersionJson();

}  // namespace uzume
