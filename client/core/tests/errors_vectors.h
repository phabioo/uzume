#pragma once
// Contract vectors of the error fingerprint (see errors_test.cpp).
namespace {
const char* kVectors = R"JSON(
[
 {
  "kind": "app_crash",
  "component": "app",
  "message": "",
  "normalized": "",
  "fingerprint": "uz-f896422c0c17"
 },
 {
  "kind": "core_failure",
  "component": "core:azahar",
  "message": "Failed to load core from C:\\Users\\Anna\\AppData\\Local\\Uzume\\cores\\azahar_libretro.dll: error 126",
  "normalized": "failed to load core from <path> error <n>",
  "fingerprint": "uz-4b3666bf8dad"
 },
 {
  "kind": "core_failure",
  "component": "core:melondsds",
  "message": "Could not open /home/anna/.cache/uzume/cores/melondsds_libretro.so (code 2)",
  "normalized": "could not open <path> (code <n>)",
  "fingerprint": "uz-d6f5c5dd6950"
 },
 {
  "kind": "plugin_error",
  "component": "plugin:uzume.soullink",
  "message": "main.lua:1234: attempt to index a nil value (field 'team')",
  "normalized": "main.lua:<n>: attempt to index a nil value (field 'team')",
  "fingerprint": "uz-3b9bd4055feb"
 },
 {
  "kind": "sync_failure",
  "component": "app",
  "message": "upload failed: PUT https://192.168.1.20:8443/api/v1/games/9f1c2d3e-aaaa-bbbb-cccc-0123456789ab/saves/default returned 502",
  "normalized": "upload failed: put <url> returned <n>",
  "fingerprint": "uz-906f4de5704a"
 },
 {
  "kind": "sync_failure",
  "component": "app",
  "message": "connect 10.0.0.5:8443 refused; retry in 30 s, fe80::1:2:3:4 unreachable",
  "normalized": "connect <ip> refused; retry in <n> s, <ip> unreachable",
  "fingerprint": "uz-aaa18e14f55c"
 },
 {
  "kind": "update_failure",
  "component": "app",
  "message": "The downloaded update was rejected: sha256 mismatch deadbeef0123 != 0xCAFEBABE",
  "normalized": "the downloaded update was rejected: sha<n> mismatch <hex> != <hex>",
  "fingerprint": "uz-f321c2ff03b7"
 },
 {
  "kind": "app_crash",
  "component": "app",
  "message": "  Mixed   CASE\tand\nwhitespace  ~/uzume/data/logs/player.log opened ",
  "normalized": "mixed case and whitespace <path> opened",
  "fingerprint": "uz-935f4940bd6e"
 },
 {
  "kind": "plugin_error",
  "component": "plugin:x",
  "message": "\u00fcmlaut \u00f6 \u65e5\u672c\u8a9e \u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9",
  "normalized": "\u00fcmlaut \u00f6 \u65e5\u672c\u8a9e \u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9",
  "fingerprint": "uz-3145d3239bb1"
 },
 {
  "kind": "core_failure",
  "component": "core:mgba",
  "message": "Unable to continue: xstack overflow in handler number one, xstack overflow in handler number one, xstack overflow in handler number one, xstack overflow in handler number one, xstack overflow in handler number one, xstack overflow in handler number one, xstack overflow in handler number one, xstack overflow in handler number one, xstack overflow in handler number one, xstack overflow in handler number one, xstack overflow in handler number one, x and then retry later retry later retry later retry later retry later retry later retry later retry later retry later retry later retry later retry later retry later retry later retry later retry later retry later retry later retry later retry later retry later retry later retry later retry later retry later retry later retry later retry later retry later retry later ",
  "normalized": "unable to continue: xstack overflow in handler number one, xstack overflow in handler number one, xstack overflow in handler number one, xstack overflow in handler number one, xstack overflow in handler number one, xstack overflow in handler number one, xstack overflow in handler number one, xstack ",
  "fingerprint": "uz-befde5700a83"
 },
 {
  "kind": "update_failure",
  "component": "app",
  "message": "download of https://github.com/phabioo/uzume/releases/download/v0.24.0/uzume.msi failed at 0x7ffe12345678 [::1]:8080 and 2001:db8::ff00:42:8329 and ~ and /etc and a/b",
  "normalized": "download of <url> failed at <hex> [::<n>]:<n> and <ip> and ~ and /etc and a/b",
  "fingerprint": "uz-5607adbc4065"
 }
]
)JSON";
}  // namespace
