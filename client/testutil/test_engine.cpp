// uzume_test_engine: a stand-in for the randomizer engine's command line (FVX: `java -Xmx..M -jar UPR-FVX.jar cli -i <in> -o <out>
// -S <settings> -z <seed> [-d] [-l]`). Test code only: synthetic output, deterministic, no game data.
//   - Everything before the word "cli" (the -Xmx option, -jar and the jar name) is ignored.
//   - Without -d: the output file is the input XOR a keystream derived from SHA-256(settings + seed): same input, settings and seed
//     give the same bytes, any other value gives different bytes.
//   - With -d: the output folder holds <TID>/romfs/a/0/0/0 and <TID>/code.bin with synthetic bytes derived the same way.
//   - With -l: <out>.log holds one synthetic line.
//   - UZUME_TEST_ENGINE_ARGS_FILE: the received arguments are written there, one per line.
//   - UZUME_TEST_ENGINE_DELAY_MS sleeps before the output is written (cancel and time-limit tests).
//   - Settings starting with "999" make it fail: it prints ERROR: and WARNING: lines (and a line that must be filtered out) and
//     exits with 3.
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace {

struct Sha256 {
  uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  uint64_t len = 0;
  uint8_t buf[64];
  size_t n = 0;
  static uint32_t rr(uint32_t x, int k) { return (x >> k) | (x << (32 - k)); }
  void block(const uint8_t* p) {
    static const uint32_t K[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be,
        0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa,
        0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85,
        0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
        0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
        0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    uint32_t w[64];
    for (int i = 0; i < 16; ++i) w[i] = (uint32_t(p[i * 4]) << 24) | (uint32_t(p[i * 4 + 1]) << 16) | (uint32_t(p[i * 4 + 2]) << 8) | p[i * 4 + 3];
    for (int i = 16; i < 64; ++i) {
      const uint32_t s0 = rr(w[i - 15], 7) ^ rr(w[i - 15], 18) ^ (w[i - 15] >> 3);
      const uint32_t s1 = rr(w[i - 2], 17) ^ rr(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; ++i) {
      const uint32_t t1 = hh + (rr(e, 6) ^ rr(e, 11) ^ rr(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
      const uint32_t t2 = (rr(a, 2) ^ rr(a, 13) ^ rr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
      hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
  }
  void update(const uint8_t* p, size_t k) {
    len += k;
    while (k--) {
      buf[n++] = *p++;
      if (n == 64) { block(buf); n = 0; }
    }
  }
  void final(uint8_t out[32]) {
    const uint64_t bits = len * 8;
    uint8_t pad = 0x80;
    update(&pad, 1);
    pad = 0;
    while (n != 56) update(&pad, 1);
    for (int i = 7; i >= 0; --i) { uint8_t b = uint8_t(bits >> (i * 8)); update(&b, 1); }
    for (int i = 0; i < 8; ++i) { out[i * 4] = uint8_t(h[i] >> 24); out[i * 4 + 1] = uint8_t(h[i] >> 16); out[i * 4 + 2] = uint8_t(h[i] >> 8); out[i * 4 + 3] = uint8_t(h[i]); }
  }
};

struct Keystream {
  std::string key;
  uint64_t counter = 0;
  uint8_t block[32];
  size_t pos = 32;
  uint8_t next() {
    if (pos == 32) {
      Sha256 s;
      s.update(reinterpret_cast<const uint8_t*>(key.data()), key.size());
      uint8_t c[8];
      for (int i = 0; i < 8; ++i) c[i] = uint8_t(counter >> (i * 8));
      s.update(c, 8);
      s.final(block);
      ++counter;
      pos = 0;
    }
    return block[pos++];
  }
};

}  // namespace

int main(int argc, char** argv) {
  if (const char* af = std::getenv("UZUME_TEST_ENGINE_ARGS_FILE")) {  // tests: the received arguments, one per line
    std::ofstream a(af, std::ios::binary | std::ios::trunc);
    for (int i = 1; i < argc; ++i) a << argv[i] << "\n";
  }
  std::string in, out, settings, seed;
  bool dir = false, log = false, seenCli = false;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (!seenCli) {
      if (a == "cli") seenCli = true;
      continue;
    }
    if (a == "-i" && i + 1 < argc) in = argv[++i];
    else if (a == "-o" && i + 1 < argc) out = argv[++i];
    else if (a == "-S" && i + 1 < argc) settings = argv[++i];
    else if (a == "-z" && i + 1 < argc) seed = argv[++i];
    else if (a == "-d") dir = true;
    else if (a == "-l") log = true;
  }
  if (in.empty() || out.empty()) {
    std::printf("ERROR: missing -i or -o\n");
    return 2;
  }
  if (settings.rfind("999", 0) == 0) {
    std::printf("Randomizing something about SYNTHETICMON\n");
    std::printf("WARNING: synthetic warning\n");
    std::printf("ERROR: synthetic failure while writing\n");
    std::fflush(stdout);
    return 3;
  }
  if (const char* d = std::getenv("UZUME_TEST_ENGINE_DELAY_MS")) {
    const long ms = std::atol(d);
    for (long t = 0; t < ms; t += 20) std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  namespace fs = std::filesystem;
  Keystream ks{settings + "|" + seed};
  if (!dir) {
    std::ifstream f(in, std::ios::binary);
    if (!f) {
      std::printf("ERROR: cannot read the input\n");
      return 2;
    }
    // Like FVX FileNameFunctions.fixFilename: the path gets the default extension of the generation unless it already ends with it
    // (.gb/.gbc -> gbc, .gba -> gba, .nds -> nds); "red.gb" becomes "red.gb.gbc". The log follows the real output name.
    {
      std::string ext = "nds";
      const auto dot = out.rfind('.');
      const std::string e = dot == std::string::npos ? "" : out.substr(dot + 1);
      if (e == "gb" || e == "gbc") ext = "gbc"; else if (e == "gba") ext = "gba";
      if (e != ext) out += "." + ext;
    }
    std::ofstream o(out, std::ios::binary);
    if (!o) {
      std::printf("ERROR: cannot write the output\n");
      return 2;
    }
    std::vector<char> chunk(1 << 16);
    while (f) {
      f.read(chunk.data(), std::streamsize(chunk.size()));
      const std::streamsize got = f.gcount();
      for (std::streamsize i = 0; i < got; ++i) chunk[size_t(i)] = char(uint8_t(chunk[size_t(i)]) ^ ks.next());
      o.write(chunk.data(), got);
    }
  } else {
    const fs::path root = fs::path(out) / "00040000000FB000";
    std::error_code ec;
    fs::create_directories(root / "romfs" / "a" / "0" / "0", ec);
    std::ofstream a(root / "romfs" / "a" / "0" / "0" / "0", std::ios::binary);
    for (int i = 0; i < 256; ++i) a.put(char(ks.next()));
    std::ofstream c(root / "code.bin", std::ios::binary);
    for (int i = 0; i < 64; ++i) c.put(char(ks.next()));
  }
  if (log) {
    std::ofstream l(out + ".log");
    l << "SYNTHETIC SPOILER LOG seed " << seed << "\n";
  }
  std::printf("Randomized successfully!\n");
  return 0;
}
