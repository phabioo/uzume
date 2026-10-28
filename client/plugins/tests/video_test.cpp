// uz.video (permission video.capture): permission check, rectangle validation, rate limit, busy, event delivery with a fake frame.
#include <QTemporaryDir>
#include <QtTest>
#include <atomic>
#include <mutex>

#include "plugin_host.h"
#include "plugin_test_util.h"

using namespace uzume::plugins;
using namespace uzume::plugins::testutil;

namespace {

// Two stacked 8x8 screens in an 8x16 frame; the top one is red, the bottom one blue. Completes at once, or on release().
class FakeVideo : public VideoProvider {
 public:
  bool manual = false;
  std::string failWith;  // non-empty: captures complete with this error
  std::atomic<int> requests{0};

  std::vector<ScreenInfo> screens() const override { return {{"top", 0, 0, 8, 8}, {"bottom", 0, 8, 8, 8}}; }
  void captureFrame(std::function<void(RawFrame)> done) override {
    ++requests;
    std::lock_guard<std::mutex> l(m_);
    if (manual) {
      pending_ = std::move(done);
      return;
    }
    done(frame());
  }
  void release() {
    std::function<void(RawFrame)> d;
    {
      std::lock_guard<std::mutex> l(m_);
      d = std::move(pending_);
      pending_ = nullptr;
    }
    if (d) d(frame());
  }

 private:
  RawFrame frame() const {
    RawFrame f;
    if (!failWith.empty()) {
      f.error = failWith;
      return f;
    }
    f.w = 8;
    f.h = 16;
    f.rgba.resize(8 * 16 * 4);
    for (int y = 0; y < 16; ++y)
      for (int x = 0; x < 8; ++x) {
        char* px = &f.rgba[(y * 8 + x) * 4];
        px[0] = y < 8 ? char(255) : 0;
        px[1] = 0;
        px[2] = y < 8 ? 0 : char(255);
        px[3] = char(255);
      }
    return f;
  }
  std::mutex m_;
  std::function<void(RawFrame)> pending_;
};

bool hasLog(const ConsoleSnapshot& c, const QString& needle) {
  for (const LogLine& l : c.log)
    if (l.text.contains(needle)) return true;
  return false;
}

// Collects every video.captured event as one log line "cap id ok err w h pngmagic ihdrW ihdrH".
const char* kHelpers = R"LUA(
  local function be32(s, i) local a, b, c, d = s:byte(i, i + 3) return ((a * 256 + b) * 256 + c) * 256 + d end
  uz.events.on("uz", "video.captured", function(e)
    local magic = e.png and e.png:sub(1, 8) == "\137PNG\r\n\26\n"
    uz.log.info(string.format("cap id=%d ok=%s err=%s w=%s h=%s png=%s ihdr=%s", e.id, tostring(e.ok), tostring(e.error), tostring(e.w), tostring(e.h),
      tostring(magic), e.png and (be32(e.png, 17) .. "x" .. be32(e.png, 21)) or "-"))
  end)
)LUA";

}  // namespace

class VideoTest : public QObject {
  Q_OBJECT

  QTemporaryDir m_dir;
  PluginHost* m_host = nullptr;
  std::shared_ptr<FakeVideo> m_video;
  std::shared_ptr<FakeMemoryProvider> m_mem;

  void startWith(const QStringList& perms, const QString& lua, bool withVideo = true) {
    PluginSpec s;
    s.permissions = perms;
    s.lua = QString::fromLatin1(kHelpers) + lua;
    GameContext ctx{"nds", "ABCD", "abc123", "Test Game", "", "en"};
    if (withVideo) ctx.video = m_video;
    m_host->start(ctx, m_mem, {infoFor(writePlugin(m_dir.path() + "/src", s), m_dir.path() + "/cache")});
    QTRY_COMPARE_WITH_TIMEOUT(con().status, PluginStatus::Running, 10000);
  }
  ConsoleSnapshot con() const { return m_host->consoleSnapshot("net.test.plugin"); }

 private slots:
  void init() {
    m_video = std::make_shared<FakeVideo>();
    m_mem = std::make_shared<FakeMemoryProvider>();
    m_host = new PluginHost;
    m_host->setTickHz(200);
  }
  void cleanup() {
    delete m_host;
    m_host = nullptr;
    QDir(m_dir.path() + "/src").removeRecursively();
    QDir(m_dir.path() + "/cache").removeRecursively();
  }

  void permissionRequired() {
    startWith({}, R"LUA(
      for _, f in ipairs({ function() return uz.video.screens() end, function() return uz.video.capture("top") end }) do
        local ok, e = pcall(f)
        assert(not ok and e:find("permission video.capture not granted", 1, true), tostring(e))
      end
      uz.log.info("denied ok")
    )LUA");
    QTRY_VERIFY(hasLog(con(), "denied ok"));
    QCOMPARE(m_video->requests.load(), 0);
  }

  void screensAndNamedCapture() {
    startWith({"video.capture"}, R"LUA(
      local s = uz.video.screens()
      assert(#s == 2 and s[1].name == "top" and s[2].name == "bottom" and s[2].y == 8 and s[2].w == 8 and s[2].h == 8)
      local id = uz.video.capture("bottom")
      assert(math.type(id) == "integer")
      uz.log.info("requested " .. id)
    )LUA");
    QTRY_VERIFY(hasLog(con(), "cap id=1 ok=true err=nil w=8 h=8 png=true ihdr=8x8"));
    QVERIFY(hasLog(con(), "requested 1"));
  }

  void rectCaptureAndValidation() {
    startWith({"video.capture"}, R"LUA(
      -- invalid argument types are Lua errors, everything else arrives as an event
      assert(not pcall(uz.video.capture))
      assert(not pcall(uz.video.capture, { x = 0, y = 0, w = 1 }))
      assert(not pcall(uz.video.capture, { x = 0.5, y = 0, w = 1, h = 1 }))
      uz.video.capture({ x = 2, y = 4, w = 3, h = 5 })
    )LUA");
    QTRY_VERIFY(hasLog(con(), "cap id=1 ok=true err=nil w=3 h=5 png=true ihdr=3x5"));
  }

  void rectErrors() {
    startWith({"video.capture"}, R"LUA(
      uz.video.capture({ x = 0, y = 0, w = 0, h = 4 })        -- 1: empty
      uz.video.capture({ x = -1, y = 0, w = 4, h = 4 })       -- 2: negative origin
      uz.video.capture({ x = 0, y = 0, w = 5000, h = 4 })     -- 3: too large
      uz.video.capture("nope")                                -- 4: unknown screen
    )LUA");
    QTRY_VERIFY(hasLog(con(), "cap id=4 ok=false err=unknown_screen"));
    QVERIFY(hasLog(con(), "cap id=1 ok=false err=invalid_rect"));
    QVERIFY(hasLog(con(), "cap id=2 ok=false err=invalid_rect"));
    QVERIFY(hasLog(con(), "cap id=3 ok=false err=invalid_rect"));
    QCOMPARE(m_video->requests.load(), 0);  // none of them reached the frame source
  }

  void rectOutsideFrame() {
    startWith({"video.capture"}, R"LUA(uz.video.capture({ x = 6, y = 0, w = 4, h = 4 }))LUA");
    QTRY_VERIFY(hasLog(con(), "cap id=1 ok=false err=invalid_rect"));
  }

  void busyWhilePending() {
    m_video->manual = true;
    startWith({"video.capture"}, R"LUA(
      uz.video.capture("top")   -- 1: pending
      uz.video.capture("top")   -- 2: busy
    )LUA");
    QTRY_VERIFY(hasLog(con(), "cap id=2 ok=false err=busy"));
    QCOMPARE(m_video->requests.load(), 1);
    m_video->release();
    QTRY_VERIFY(hasLog(con(), "cap id=1 ok=true"));
  }

  void rateLimitOnePerSecond() {
    // The first capture completes at once; asking again from its result is refused (1 per second), after 1.2 s it works.
    startWith({"video.capture"}, R"LUA(
      local t0 = uz.clock()
      local third = false
      uz.events.on("uz", "video.captured", function(e)
        if e.id == 1 then uz.video.capture("top") end            -- id 2: rate_limited
      end)
      uz.on_tick(function()
        if not third and uz.clock() - t0 > 1200 then third = true uz.video.capture("top") end   -- id 3
      end)
      uz.video.capture("top")                                    -- id 1
    )LUA");
    QTRY_VERIFY(hasLog(con(), "cap id=2 ok=false err=rate_limited"));
    QVERIFY(hasLog(con(), "cap id=1 ok=true"));
    QCOMPARE(m_video->requests.load(), 1);
    QTRY_VERIFY_WITH_TIMEOUT((m_mem->snapshot(), hasLog(con(), "cap id=3 ok=true")), 5000);
    QCOMPARE(m_video->requests.load(), 2);
  }

  void errorsFromFrameSource() {
    m_video->failWith = "denied";
    startWith({"video.capture"}, R"LUA(uz.video.capture("top"))LUA");
    QTRY_VERIFY(hasLog(con(), "cap id=1 ok=false err=denied"));
  }

  void noProvider() {
    startWith({"video.capture"}, R"LUA(
      assert(uz.video.screens() == nil)
      uz.video.capture("top")
    )LUA", false);
    QTRY_VERIFY(hasLog(con(), "cap id=1 ok=false err=unsupported"));
  }
};

QTEST_MAIN(VideoTest)
#include "video_test.moc"
