// 文字叠加：时间水印（本地实时时间，含日期与秒）。
//
// 不走 ffmpeg 的 drawtext 滤镜（那需要 libavfilter 滤镜图 + libfreetype，且要把每帧在滤镜图里来回转换），
// 由内核自己把文字渲染成一块 NV12 + alpha，再用 BlendNv12A 混合到帧上 —— 与水印 / 叠加层同一条路径。
//
// 字形来源（光栅化后端）按编译开关二选一：
//   内置点阵  默认。X11 misc-fixed 10x20（公有领域）编进库里，只有 ASCII（0x20 ~ 0x7E），零额外依赖。
//   FreeType  TG_WITH_TEXT=ON 时编入（依赖 FfmpegCompileShell --with-text 产出的 freetype）。
//             用 TrueType / OpenType 字体（宿主指定或系统默认字体）渲染，支持中文等任意 Unicode 字符、
//             抗锯齿边缘；字体加载失败时回落到内置点阵。
//
// 文字每秒才变一次：渲染结果按"文本 + 画布尺寸"缓存，每帧只做一次混合。
#ifndef TGCAPTURE_CORE_TEXT_OVERLAY_H
#define TGCAPTURE_CORE_TEXT_OVERLAY_H

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "TGCapture/TGCapturerInterface.h"
#include "core/Frame.h"
#include "core/Overlay.h"

namespace TGCapture::core
{

// 8 bit 覆盖率蒙版（0 = 透明，255 = 完全覆盖）
struct AlphaMask
{
    int width = 0;
    int height = 0;
    std::vector<uint8_t> a;   // width * height
};

// 单行文字光栅化：行高 = pixelHeight（含上下留白），宽度按字形前进量累加
class TextRasterizer
{
public:
    virtual ~TextRasterizer() = default;
    [[nodiscard]] virtual const char* Name() const = 0;
    // 能否画出该字符（不能的会画成 '?'）
    [[nodiscard]] virtual bool HasGlyph(char32_t ch) const = 0;
    virtual bool Render(const std::u32string& text, int pixelHeight, AlphaMask& out) = 0;
};

std::unique_ptr<TextRasterizer> CreateBitmapRasterizer();
// 本次构建是否编入了 FreeType（TG_WITH_TEXT=ON）
bool HasFreeTypeBackend() noexcept;
// FreeType 后端：fontPath 为空时按平台默认字体列表挑第一个能打开的。
// 未编入 FreeType / 字体打不开时返回 nullptr，why 写原因
std::unique_ptr<TextRasterizer> CreateFreeTypeRasterizer(const std::string& fontPath, std::string* why);
// 各平台的默认字体候选（优先含中文字形的字体）
std::vector<std::string> DefaultFontCandidates();

// ---- 时间格式 ----
// 支持的占位符：%Y %y %m %d %H %I %M %S %p %j %a %F(=%Y-%m-%d) %T(=%H:%M:%S) %%，其余字符原样输出
bool ValidateTimeFormat(const std::string& format, std::string* why);
// wallUs：Unix 纪元起的微秒数；按本机时区换算
std::string FormatLocalTime(const std::string& format, int64_t wallUs);
int64_t WallClockNowUs() noexcept;
// UTF-8 → UTF-32；非法序列返回 false
bool DecodeUtf8(const std::string& in, std::u32string& out);

// ARGB 颜色 → BT.601 limited range 的 Y / U / V
void ArgbToYuv(uint32_t argb, uint8_t& y, uint8_t& u, uint8_t& v) noexcept;

// 时间水印图层（合成器最上层）。线程安全：配置与绘制可在不同线程
class TimestampOverlay
{
public:
    TimestampOverlay();
    ~TimestampOverlay();

    // 校验 + 生效（下一帧起）。返回 TGCaptureError；why 写失败原因
    int Configure(const TimestampOverlayParams& params, std::string* why = nullptr);

    [[nodiscard]] bool Enabled() const;
    [[nodiscard]] TimestampOverlayParams Params() const;
    // 当前使用的光栅化后端："bitmap" / "freetype"
    [[nodiscard]] std::string Backend() const;

    // 画到 NV12 CPU 帧上（planes 必须可写）。wallUs = 该帧对应的墙钟时刻（Unix 微秒）
    void BlendOnto(Frame& frame, int64_t wallUs);

    // 测试 / 排障：最近一次渲染出的文字
    [[nodiscard]] std::string LastText() const;

private:
    struct Rendered
    {
        std::string text;
        int canvasW = 0;
        int canvasH = 0;
        Nv12AImage image;
        int x = 0;
        int y = 0;
    };
    bool RenderLocked(const std::string& text, int canvasW, int canvasH);

    mutable std::mutex m_mutex;
    TimestampOverlayParams m_params;
    std::unique_ptr<TextRasterizer> m_rasterizer;
    Rendered m_cache;
};

}  // namespace TGCapture::core

#endif  // TGCAPTURE_CORE_TEXT_OVERLAY_H
