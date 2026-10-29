// 时间水印 / 文字渲染：
//   时间格式化（本地时区、各占位符）、格式串校验、UTF-8 解码；
//   内置点阵后端（字号缩放）、FreeType 后端（TG_WITH_TEXT 构建：默认字体、中文字形）；
//   图层：参数校验、位置（相对 / 比例坐标）、底框 / 透明底、每秒换字、画布尺寸跟随；
//   合成器：推流画面与预览画面都带时间、只改动文字所在区域；门面：SetTimestampOverlay / IsSupported
#include <algorithm>
#include <cstring>
#include <ctime>
#include <memory>
#include <mutex>
#include <string>

#include "TGCapturer.h"
#include "core/Bus.h"
#include "core/Clock.h"
#include "core/TextOverlay.h"
#include "nodes/VideoNodes.h"
#include "tg_test.h"

using namespace TGCapture;

namespace
{

core::FramePtr Nv12(int w, int h, uint8_t y)
{
    const size_t ySize = static_cast<size_t>(w) * h;
    auto buffer = std::shared_ptr<uint8_t>(new uint8_t[ySize + ySize / 2], std::default_delete<uint8_t[]>());
    std::fill_n(buffer.get(), ySize, y);
    std::fill_n(buffer.get() + ySize, ySize / 2, uint8_t{128});
    auto f = std::make_shared<core::Frame>();
    f->kind = core::MediaKind::Video;
    f->width = w;
    f->height = h;
    f->pixFmt = AV_PIX_FMT_NV12;
    f->planes[0] = buffer;
    f->strides[0] = w;
    f->planes[1] = std::shared_ptr<uint8_t>(buffer, buffer.get() + ySize);
    f->strides[1] = w;
    f->captureUs = core::MonotonicClock::NowUs();
    return f;
}

int Luma(const core::Frame& f, int x, int y)
{
    return f.planes[0].get()[static_cast<size_t>(y) * f.strides[0] + x];
}

// 与背景亮度不同的像素的包围盒
struct Box
{
    int x0 = 1 << 30, y0 = 1 << 30, x1 = -1, y1 = -1;
    int count = 0;
    bool Empty() const { return count == 0; }
};
Box ChangedBox(const core::Frame& f, uint8_t bg)
{
    Box b;
    for (int y = 0; y < f.height; ++y) {
        for (int x = 0; x < f.width; ++x) {
            if (Luma(f, x, y) != bg) {
                b.x0 = std::min(b.x0, x);
                b.y0 = std::min(b.y0, y);
                b.x1 = std::max(b.x1, x);
                b.y1 = std::max(b.y1, y);
                ++b.count;
            }
        }
    }
    return b;
}

// 本地时区下 2026-10-29 17:05:09 的墙钟（Unix 微秒）
int64_t LocalWallUs(int year, int mon, int day, int h, int m, int s)
{
    std::tm tm{};
    tm.tm_year = year - 1900;
    tm.tm_mon = mon - 1;
    tm.tm_mday = day;
    tm.tm_hour = h;
    tm.tm_min = m;
    tm.tm_sec = s;
    tm.tm_isdst = -1;
    return static_cast<int64_t>(std::mktime(&tm)) * 1000000;
}

TimestampOverlayParams Plain(int position = 0)
{
    TimestampOverlayParams p;
    p.enabled = true;
    p.position = position;
    p.backgroundColor = 0;   // 不画底框
    p.outline = false;
    p.textColor = 0xFFFFFFFF;
    return p;
}

class Sink : public core::BusNode
{
public:
    explicit Sink(std::string name) : core::BusNode(std::move(name)) {}
    bool Submit(core::FramePtr frame) override
    {
        std::lock_guard<std::mutex> lock(mutex);
        last = std::move(frame);
        return true;
    }
    size_t Pending() const override { return 0; }
    std::mutex mutex;
    core::FramePtr last;
};

}  // namespace

// ================================================================== 格式化
TG_TEST(TextOverlay, FormatLocalTime)
{
    const int64_t t = LocalWallUs(2026, 10, 29, 17, 5, 9) + 456789;   // 小数秒不影响显示
    TG_EXPECT_EQ(core::FormatLocalTime("%Y-%m-%d %H:%M:%S", t), std::string("2026-10-29 17:05:09"));
    TG_EXPECT_EQ(core::FormatLocalTime("%F %T", t), std::string("2026-10-29 17:05:09"));
    TG_EXPECT_EQ(core::FormatLocalTime("%y/%m/%d %I:%M:%S %p", t), std::string("26/10/29 05:05:09 PM"));
    TG_EXPECT_EQ(core::FormatLocalTime("%a %j 100%%", t), std::string("Thu 302 100%"));
    TG_EXPECT_EQ(core::FormatLocalTime("时间 %H:%M", t), std::string("时间 17:05"));
    TG_EXPECT_EQ(core::FormatLocalTime("%I %p", LocalWallUs(2026, 1, 1, 0, 0, 0)), std::string("12 AM"));
}

TG_TEST(TextOverlay, ValidateFormatAndUtf8)
{
    std::string why;
    TG_EXPECT(core::ValidateTimeFormat("%Y-%m-%d %H:%M:%S", &why));
    TG_EXPECT(core::ValidateTimeFormat("直播 %F %T", &why));
    TG_EXPECT(!core::ValidateTimeFormat("", &why));
    TG_EXPECT(!core::ValidateTimeFormat("%Q", &why));
    TG_EXPECT(!core::ValidateTimeFormat("abc%", &why));
    TG_EXPECT(!core::ValidateTimeFormat("\xff%H", &why));
    TG_EXPECT(!core::ValidateTimeFormat(std::string(300, 'a'), &why));

    std::u32string u;
    TG_EXPECT(core::DecodeUtf8("a时间", u));
    TG_EXPECT_EQ(u.size(), size_t{3});
    TG_EXPECT(u[1] == U'时');
    TG_EXPECT(!core::DecodeUtf8("\xC0\x80", u));      // 过长编码
    TG_EXPECT(!core::DecodeUtf8("\xED\xA0\x80", u));  // 代理区
    TG_EXPECT(!core::DecodeUtf8("\xE6\x97", u));      // 截断
}

// ================================================================== 光栅化
TG_TEST(TextOverlay, BitmapRasterizerScales)
{
    auto r = core::CreateBitmapRasterizer();
    TG_EXPECT(std::strcmp(r->Name(), "bitmap") == 0);
    TG_EXPECT(r->HasGlyph(U'0'));
    TG_EXPECT(!r->HasGlyph(U'时'));
    core::AlphaMask m;
    TG_ASSERT(r->Render(U"12:34", 20, m));   // 原生 10x20
    TG_EXPECT_EQ(m.width, 50);
    TG_EXPECT_EQ(m.height, 20);
    TG_EXPECT(std::count(m.a.begin(), m.a.end(), uint8_t{255}) > 20);
    TG_ASSERT(r->Render(U"12:34", 40, m));   // 整数倍
    TG_EXPECT_EQ(m.width, 100);
    TG_ASSERT(r->Render(U"12:34", 30, m));   // 非整数倍：box 缩放，边缘有中间值
    TG_EXPECT_EQ(m.width, 75);
    TG_EXPECT(std::any_of(m.a.begin(), m.a.end(), [](uint8_t v) { return v > 0 && v < 255; }));
    TG_ASSERT(r->Render(U"时", 20, m));       // 画不出的字符画成 '?'，不失败
    TG_EXPECT_EQ(m.width, 10);
}

TG_TEST(TextOverlay, FreeTypeBackend)
{
    std::string why;
    if (!core::HasFreeTypeBackend()) {
        // 不带文字库的构建：FreeType 后端不可用，接口给出原因
        TG_EXPECT(core::CreateFreeTypeRasterizer("", &why) == nullptr);
        TG_EXPECT(!why.empty());
        return;
    }
    TG_EXPECT(core::CreateFreeTypeRasterizer("/no/such/font.ttf", &why) == nullptr);
    auto r = core::CreateFreeTypeRasterizer("", &why);
    if (!r) {
        std::printf("    (跳过：本机没有默认字体：%s)\n", why.c_str());
        return;
    }
    TG_EXPECT(std::strcmp(r->Name(), "freetype") == 0);
    core::AlphaMask m;
    TG_ASSERT(r->Render(U"2026-10-29 17:05:09", 32, m));
    TG_EXPECT_EQ(m.height, 32);
    TG_EXPECT(m.width > 32 * 5);
    TG_EXPECT(std::any_of(m.a.begin(), m.a.end(), [](uint8_t v) { return v > 0 && v < 255; }));   // 抗锯齿
#if defined(__APPLE__) || defined(_WIN32)
    TG_EXPECT(r->HasGlyph(U'时'));   // 默认字体优先选含中文的
#endif
    if (r->HasGlyph(U'时')) {
        core::AlphaMask cjk;
        TG_ASSERT(r->Render(U"时间", 32, cjk));
        TG_EXPECT(cjk.width >= 48);   // 全角字形，两个字至少 1.5 倍字高
    }
}

// ================================================================== 图层
TG_TEST(TextOverlay, ConfigureValidation)
{
    core::TimestampOverlay ts;
    TG_EXPECT(!ts.Enabled());
    TimestampOverlayParams p = Plain();
    p.format = "%Q";
    TG_EXPECT_EQ(ts.Configure(p), int{TGErr_InvalidArgument});
    p = Plain(6);
    TG_EXPECT_EQ(ts.Configure(p), int{TGErr_InvalidArgument});
    p = Plain(-1);
    p.x = 1.5f;
    TG_EXPECT_EQ(ts.Configure(p), int{TGErr_InvalidArgument});
    p = Plain();
    p.fontSize = 0.0f;
    TG_EXPECT_EQ(ts.Configure(p), int{TGErr_InvalidArgument});
    p.fontSize = 0.04f;
    p.margin = -0.1f;
    TG_EXPECT_EQ(ts.Configure(p), int{TGErr_InvalidArgument});
    TG_EXPECT(!ts.Enabled());   // 失败保持原设置

    p = Plain();
    p.fontPath = "/no/such/font.ttf";
    TG_EXPECT_EQ(ts.Configure(p),
                 core::HasFreeTypeBackend() ? int{TGErr_InvalidArgument} : int{TGErr_NotSupported});

    TG_EXPECT_EQ(ts.Configure(Plain()), int{TGErr_OK});
    TG_EXPECT(ts.Enabled());
    TimestampOverlayParams off;
    off.enabled = false;
    TG_EXPECT_EQ(ts.Configure(off), int{TGErr_OK});
    TG_EXPECT(!ts.Enabled());
}

TG_TEST(TextOverlay, BlendTopLeftOnlyTouchesTextBox)
{
    core::TimestampOverlay ts;
    TimestampOverlayParams p = Plain(0);
    p.fontSize = 0.1f;    // 240 * 0.1 = 24px
    p.margin = 0.05f;     // 12px
    TG_ASSERT(ts.Configure(p) == TGErr_OK);
    auto f = Nv12(320, 240, 50);
    ts.BlendOnto(*f, LocalWallUs(2026, 10, 29, 17, 5, 9));
    TG_EXPECT_EQ(ts.LastText(), std::string("2026-10-29 17:05:09"));
    const Box b = ChangedBox(*f, 50);
    TG_ASSERT(!b.Empty());
    TG_EXPECT(b.x0 >= 12 && b.y0 >= 12);   // 留出边距
    TG_EXPECT(b.y1 < 12 + 24);             // 只占一行字高
    TG_EXPECT(b.x1 < 320);
    // 19 个字符 × 12px（10x20 按 24px 高缩放）≈ 228px
    TG_EXPECT(b.x1 - b.x0 > 180 && b.x1 - b.x0 < 240);
    int bright = 0;
    for (int y = b.y0; y <= b.y1; ++y)
        for (int x = b.x0; x <= b.x1; ++x) bright += Luma(*f, x, y) > 200 ? 1 : 0;
    TG_EXPECT(bright > 200);   // 白字
    TG_EXPECT_EQ(Luma(*f, 319, 239), 50);
}

TG_TEST(TextOverlay, BackgroundBoxAndOutline)
{
    core::TimestampOverlay ts;
    TimestampOverlayParams p = Plain(0);
    p.margin = 0.0f;
    p.backgroundColor = 0xFF000000;   // 不透明黑底
    TG_ASSERT(ts.Configure(p) == TGErr_OK);
    auto f = Nv12(320, 240, 200);
    ts.BlendOnto(*f, LocalWallUs(2026, 10, 29, 17, 5, 9));
    TG_EXPECT_EQ(Luma(*f, 0, 0), 16);   // 底框左上角 = 黑（limited range）
    TG_EXPECT_EQ(Luma(*f, 319, 239), 200);

    // 无底框 + 描边：白字周围出现黑边（亮画面上也能看清）
    core::TimestampOverlay ts2;
    p = Plain(0);
    p.outline = true;
    p.fontSize = 0.1f;   // 24px：笔画有完整的实心像素（太小的字缩放后多为过渡色）
    TG_ASSERT(ts2.Configure(p) == TGErr_OK);
    auto g = Nv12(320, 240, 128);
    ts2.BlendOnto(*g, LocalWallUs(2026, 10, 29, 17, 5, 9));
    int dark = 0, bright = 0;
    for (int y = 0; y < 60; ++y)
        for (int x = 0; x < 320; ++x) {
            dark += Luma(*g, x, y) < 40 ? 1 : 0;
            bright += Luma(*g, x, y) > 220 ? 1 : 0;
        }
    TG_EXPECT(dark > 50);
    TG_EXPECT(bright > 50);
}

TG_TEST(TextOverlay, PositionsAndCanvasFollow)
{
    const int64_t t = LocalWallUs(2026, 10, 29, 17, 5, 9);
    for (int pos : {1, 2, 3, 4, 5}) {
        core::TimestampOverlay ts;
        TG_ASSERT(ts.Configure(Plain(pos)) == TGErr_OK);
        auto f = Nv12(640, 360, 50);
        ts.BlendOnto(*f, t);
        const Box b = ChangedBox(*f, 50);
        TG_ASSERT(!b.Empty());
        const int cx = (b.x0 + b.x1) / 2;
        const int cy = (b.y0 + b.y1) / 2;
        if (pos == 1 || pos == 3) TG_EXPECT(cx > 320);
        if (pos == 2) TG_EXPECT(cx < 320);
        if (pos == 4 || pos == 5) TG_EXPECT(std::abs(cx - 320) < 16);
        if (pos == 1 || pos == 4) TG_EXPECT(cy < 180);
        if (pos == 2 || pos == 3 || pos == 5) TG_EXPECT(cy > 180);
    }
    // -1：比例坐标
    core::TimestampOverlay ts;
    TimestampOverlayParams p = Plain(-1);
    p.x = 0.5f;
    p.y = 0.5f;
    TG_ASSERT(ts.Configure(p) == TGErr_OK);
    auto f = Nv12(640, 360, 50);
    ts.BlendOnto(*f, t);
    Box b = ChangedBox(*f, 50);
    TG_EXPECT(b.x0 >= 320 && b.y0 >= 180);
    // 画布尺寸变化（运行期切分辨率）：字号按新画布高度重新换算
    auto big = Nv12(1280, 720, 50);
    ts.BlendOnto(*big, t);
    const Box bb = ChangedBox(*big, 50);
    TG_EXPECT(bb.y1 - bb.y0 > (b.y1 - b.y0) * 3 / 2);
    TG_EXPECT(bb.x0 >= 640 && bb.y0 >= 360);
}

TG_TEST(TextOverlay, TextUpdatesEverySecond)
{
    core::TimestampOverlay ts;
    TG_ASSERT(ts.Configure(Plain()) == TGErr_OK);
    const int64_t t = LocalWallUs(2026, 10, 29, 23, 59, 59);
    auto f = Nv12(320, 240, 50);
    ts.BlendOnto(*f, t);
    TG_EXPECT_EQ(ts.LastText(), std::string("2026-10-29 23:59:59"));
    ts.BlendOnto(*f, t + 999999);
    TG_EXPECT_EQ(ts.LastText(), std::string("2026-10-29 23:59:59"));
    ts.BlendOnto(*f, t + 1000000);   // 跨天
    TG_EXPECT_EQ(ts.LastText(), std::string("2026-10-30 00:00:00"));
}

TG_TEST(TextOverlay, NonCpuFrameIgnored)
{
    core::TimestampOverlay ts;
    TG_ASSERT(ts.Configure(Plain()) == TGErr_OK);
    core::Frame hw;
    hw.width = 320;
    hw.height = 240;
    hw.pixFmt = AV_PIX_FMT_NV12;   // 没有 planes：硬件帧
    ts.BlendOnto(hw, core::WallClockNowUs());
    TG_EXPECT(ts.LastText().empty());
}

// ================================================================== 合成器 / 门面
TG_TEST(TextOverlay, CompositorDrawsOnProgramAndPreview)
{
    core::Bus bus;
    auto comp = std::make_shared<nodes::VideoCompositorNode>();
    auto program = std::make_shared<Sink>("program");
    auto preview = std::make_shared<Sink>("preview");
    TG_ASSERT(bus.Begin()
                  .Register("video_compositor", comp)
                  .Register("program", program)
                  .Register("preview", preview)
                  .Link("video_compositor", nodes::VideoCompositorNode::kProgramOutput, "program", 0)
                  .Link("video_compositor", nodes::VideoCompositorNode::kPreviewOutput, "preview", 0)
                  .Commit());
    comp->StartNode();
    auto ts = std::make_shared<core::TimestampOverlay>();
    comp->SetTimestampLayer(ts);

    comp->Submit(Nv12(320, 240, 50));   // 未开启：原样
    TG_EXPECT(ChangedBox(*program->last, 50).Empty());

    TG_ASSERT(ts->Configure(Plain(3)) == TGErr_OK);
    comp->Submit(Nv12(320, 240, 50));
    TG_ASSERT(program->last && preview->last);
    const Box b = ChangedBox(*program->last, 50);
    TG_EXPECT(!b.Empty() && b.x0 > 100 && b.y0 > 120);   // 右下
    TG_EXPECT(!ChangedBox(*preview->last, 50).Empty());
    // 时间是"现在"（采集时刻换算），与本机时钟同一秒（或刚跨秒）
    const std::string now = core::FormatLocalTime("%Y-%m-%d %H:%M:%S", core::WallClockNowUs());
    TG_EXPECT(ts->LastText().substr(0, 16) == now.substr(0, 16));
    TG_EXPECT(comp->DumpState().find(",timestamp") != std::string::npos);
    comp->StopNode();
}

TG_TEST(TextOverlay, FacadeApi)
{
    TGCapturer c(GameID::GAME_ID_DEFAULT);
    TG_EXPECT(c.IsSupported(CaptureFeature::TimestampOverlay));
    TG_EXPECT_EQ(c.IsSupported(CaptureFeature::UnicodeText), core::HasFreeTypeBackend());
    TimestampOverlayParams p;
    p.enabled = true;
    p.format = "%H:%M:%S%";
    TG_EXPECT_EQ(c.SetTimestampOverlay(p), int{TGErr_InvalidArgument});
    p.format = "%Y-%m-%d %H:%M:%S";
    TG_EXPECT_EQ(c.SetTimestampOverlay(p), int{TGErr_OK});
    p.enabled = false;
    TG_EXPECT_EQ(c.SetTimestampOverlay(p), int{TGErr_OK});
}
