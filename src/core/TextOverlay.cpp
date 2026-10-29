#include "core/TextOverlay.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include <libyuv.h>

#include "core/Logger.h"

#if defined(TG_WITH_TEXT)
extern "C" {
#include <libavformat/avio.h>
}
#include <ft2build.h>
#include FT_FREETYPE_H
#endif

namespace TGCapture::core
{

// ================================================================== 内置点阵字体
namespace
{
constexpr int kGlyphW = 10;
constexpr int kGlyphH = 20;
constexpr char32_t kFirstGlyph = 0x20;
constexpr char32_t kLastGlyph = 0x7E;
const uint16_t kFont10x20[kLastGlyph - kFirstGlyph + 1][kGlyphH] = {
#include "core/BitmapFont10x20.inc"
};

class BitmapRasterizer final : public TextRasterizer
{
public:
    const char* Name() const override { return "bitmap"; }
    bool HasGlyph(char32_t ch) const override { return ch >= kFirstGlyph && ch <= kLastGlyph; }

    bool Render(const std::u32string& text, int pixelHeight, AlphaMask& out) override
    {
        if (text.empty() || pixelHeight <= 0) {
            return false;
        }
        // 先按整数倍放大画出（笔画保持锐利），再用 box 滤波缩到目标高度（非整数倍时边缘自然抗锯齿）
        const int k = std::max(1, (pixelHeight + kGlyphH - 1) / kGlyphH);
        const int bigW = static_cast<int>(text.size()) * kGlyphW * k;
        const int bigH = kGlyphH * k;
        std::vector<uint8_t> big(static_cast<size_t>(bigW) * bigH, 0);
        for (size_t i = 0; i < text.size(); ++i) {
            const char32_t ch = HasGlyph(text[i]) ? text[i] : U'?';
            const uint16_t* rows = kFont10x20[ch - kFirstGlyph];
            for (int r = 0; r < kGlyphH; ++r) {
                for (int c = 0; c < kGlyphW; ++c) {
                    if (!(rows[r] & (0x8000u >> c))) {
                        continue;
                    }
                    for (int dy = 0; dy < k; ++dy) {
                        uint8_t* p = big.data() + static_cast<size_t>(r * k + dy) * bigW +
                                     (static_cast<int>(i) * kGlyphW + c) * k;
                        std::fill(p, p + k, uint8_t{255});
                    }
                }
            }
        }
        out.height = pixelHeight;
        out.width = std::max(1, static_cast<int>(std::lround(static_cast<double>(bigW) * pixelHeight / bigH)));
        if (out.width == bigW && out.height == bigH) {
            out.a = std::move(big);
            return true;
        }
        out.a.assign(static_cast<size_t>(out.width) * out.height, 0);
        libyuv::ScalePlane(big.data(), bigW, bigW, bigH, out.a.data(), out.width, out.width, out.height,
                           libyuv::kFilterBox);
        return true;
    }
};

#if defined(TG_WITH_TEXT)
// ================================================================== FreeType
class FreeTypeRasterizer final : public TextRasterizer
{
public:
    ~FreeTypeRasterizer() override
    {
        if (m_face) {
            FT_Done_Face(m_face);
        }
        if (m_lib) {
            FT_Done_FreeType(m_lib);
        }
    }

    bool Open(const std::string& path, std::string* why)
    {
        if (FT_Init_FreeType(&m_lib) != 0) {
            m_lib = nullptr;
            if (why) *why = "FT_Init_FreeType 失败";
            return false;
        }
        // 文件整个读进内存再交给 FreeType：Windows 上 FT_New_Face 走 fopen，不认 UTF-8 的中文路径；
        // 用 ffmpeg 的 avio 读（与图片加载一致，路径按 UTF-8 处理）
        if (!ReadWholeFile(path, m_data)) {
            if (why) *why = "字体文件打不开: " + path;
            return false;
        }
        if (m_data.empty() ||
            FT_New_Memory_Face(m_lib, reinterpret_cast<const FT_Byte*>(m_data.data()),
                               static_cast<FT_Long>(m_data.size()), 0, &m_face) != 0) {
            m_face = nullptr;
            if (why) *why = "不是可识别的字体文件: " + path;
            return false;
        }
        if (FT_Select_Charmap(m_face, FT_ENCODING_UNICODE) != 0 || !FT_IS_SCALABLE(m_face)) {
            if (why) *why = "字体没有 Unicode 字符表或不是矢量字体: " + path;
            return false;
        }
        m_path = path;
        return true;
    }

    const char* Name() const override { return "freetype"; }
    bool HasGlyph(char32_t ch) const override { return FT_Get_Char_Index(m_face, ch) != 0; }

    bool Render(const std::u32string& text, int pixelHeight, AlphaMask& out) override
    {
        if (text.empty() || pixelHeight <= 0 || !SetSize(pixelHeight)) {
            return false;
        }
        struct Glyph
        {
            int x = 0;   // 相对笔位置的左边界
            int y = 0;   // 相对行顶
            int w = 0;
            int h = 0;
            std::vector<uint8_t> a;
        };
        std::vector<Glyph> glyphs;
        glyphs.reserve(text.size());
        int pen = 0;
        int minX = 0;
        int maxX = 0;
        FT_UInt prev = 0;
        const bool kerning = FT_HAS_KERNING(m_face);
        for (const char32_t ch : text) {
            FT_UInt idx = FT_Get_Char_Index(m_face, ch);
            if (idx == 0) {
                idx = FT_Get_Char_Index(m_face, U'?');
            }
            if (kerning && prev && idx) {
                FT_Vector d{};
                if (FT_Get_Kerning(m_face, prev, idx, FT_KERNING_DEFAULT, &d) == 0) {
                    pen += static_cast<int>(d.x >> 6);
                }
            }
            prev = idx;
            // 不用内嵌点阵（宋体等在小字号下带 1bit 点阵，没有抗锯齿）；彩色字形（emoji）不支持
            if (FT_Load_Glyph(m_face, idx, FT_LOAD_RENDER | FT_LOAD_NO_BITMAP | FT_LOAD_TARGET_NORMAL) != 0 &&
                FT_Load_Glyph(m_face, idx, FT_LOAD_RENDER | FT_LOAD_TARGET_NORMAL) != 0) {
                continue;
            }
            const FT_GlyphSlot slot = m_face->glyph;
            const FT_Bitmap& bm = slot->bitmap;
            Glyph g;
            g.x = pen + slot->bitmap_left;
            g.y = m_baseline - slot->bitmap_top;
            g.w = static_cast<int>(bm.width);
            g.h = static_cast<int>(bm.rows);
            if (g.w > 0 && g.h > 0 &&
                (bm.pixel_mode == FT_PIXEL_MODE_GRAY || bm.pixel_mode == FT_PIXEL_MODE_MONO)) {
                g.a.resize(static_cast<size_t>(g.w) * g.h);
                for (int r = 0; r < g.h; ++r) {
                    const uint8_t* src = bm.buffer + static_cast<ptrdiff_t>(r) * bm.pitch;
                    for (int c = 0; c < g.w; ++c) {
                        g.a[static_cast<size_t>(r) * g.w + c] =
                            bm.pixel_mode == FT_PIXEL_MODE_GRAY ? src[c]
                                                                : ((src[c >> 3] & (0x80 >> (c & 7))) ? 255 : 0);
                    }
                }
                minX = std::min(minX, g.x);
                maxX = std::max(maxX, g.x + g.w);
                glyphs.push_back(std::move(g));
            }
            pen += static_cast<int>((slot->advance.x + 32) >> 6);
        }
        maxX = std::max(maxX, pen);
        out.width = std::max(1, maxX - minX);
        out.height = pixelHeight;
        out.a.assign(static_cast<size_t>(out.width) * out.height, 0);
        for (const auto& g : glyphs) {
            for (int r = 0; r < g.h; ++r) {
                const int y = g.y + r;
                if (y < 0 || y >= out.height) {
                    continue;
                }
                for (int c = 0; c < g.w; ++c) {
                    uint8_t& d = out.a[static_cast<size_t>(y) * out.width + (g.x - minX + c)];
                    d = std::max(d, g.a[static_cast<size_t>(r) * g.w + c]);
                }
            }
        }
        return true;
    }

private:
    // 选一个像素字号，使"上伸 + 下伸"恰好放进 pixelHeight，并把基线居中
    bool SetSize(int pixelHeight)
    {
        if (pixelHeight == m_height) {
            return true;
        }
        int px = pixelHeight;
        for (int attempt = 0; attempt < 3; ++attempt) {
            if (FT_Set_Pixel_Sizes(m_face, 0, static_cast<FT_UInt>(std::max(1, px))) != 0) {
                return false;
            }
            const int asc = static_cast<int>((m_face->size->metrics.ascender + 63) >> 6);
            const int desc = static_cast<int>((-m_face->size->metrics.descender + 63) >> 6);
            const int total = std::max(1, asc + desc);
            if (total <= pixelHeight || px <= 1) {
                m_baseline = (pixelHeight - total) / 2 + asc;
                m_height = pixelHeight;
                return true;
            }
            px = std::max(1, px * pixelHeight / total);
        }
        m_baseline = pixelHeight * 4 / 5;
        m_height = pixelHeight;
        return true;
    }

    static bool ReadWholeFile(const std::string& path, std::string& out)
    {
        AVIOContext* io = nullptr;
        if (avio_open(&io, path.c_str(), AVIO_FLAG_READ) < 0 || !io) {
            return false;
        }
        const int64_t size = avio_size(io);
        bool ok = size > 0 && size <= (64LL << 20);   // 字体文件不超过 64MB（CJK 全字库约 20MB）
        if (ok) {
            out.resize(static_cast<size_t>(size));
            ok = avio_read(io, reinterpret_cast<unsigned char*>(out.data()), static_cast<int>(size)) == size;
        }
        avio_closep(&io);
        return ok;
    }

    FT_Library m_lib = nullptr;
    FT_Face m_face = nullptr;
    std::string m_data;
    std::string m_path;
    int m_height = 0;
    int m_baseline = 0;
};
#endif  // TG_WITH_TEXT

}  // namespace

std::unique_ptr<TextRasterizer> CreateBitmapRasterizer()
{
    return std::make_unique<BitmapRasterizer>();
}

bool HasFreeTypeBackend() noexcept
{
#if defined(TG_WITH_TEXT)
    return true;
#else
    return false;
#endif
}

std::vector<std::string> DefaultFontCandidates()
{
    std::vector<std::string> fonts;
#if defined(_WIN32)
    std::string dir = "C:\\Windows\\Fonts\\";
    if (const char* windir = std::getenv("WINDIR"); windir && *windir) {
        dir = std::string(windir) + "\\Fonts\\";
    }
    for (const char* f : {"msyh.ttc", "msyh.ttf", "simhei.ttf", "simsun.ttc", "arial.ttf", "consola.ttf"}) {
        fonts.push_back(dir + f);
    }
#elif defined(__ANDROID__)
    fonts = {"/system/fonts/NotoSansCJK-Regular.ttc", "/system/fonts/NotoSansSC-Regular.otf",
             "/system/fonts/DroidSansFallback.ttf", "/system/fonts/Roboto-Regular.ttf"};
#elif defined(__APPLE__)
    fonts = {"/System/Library/Fonts/PingFang.ttc", "/System/Library/Fonts/STHeiti Medium.ttc",
             "/System/Library/Fonts/Hiragino Sans GB.ttc", "/System/Library/Fonts/Core/PingFang.ttc",
             "/System/Library/Fonts/Supplemental/Arial Unicode.ttf", "/System/Library/Fonts/Helvetica.ttc",
             "/System/Library/Fonts/Menlo.ttc"};
#else
    fonts = {"/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
             "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
             "/usr/share/fonts/google-noto-cjk/NotoSansCJK-Regular.ttc",
             "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc", "/usr/share/fonts/wqy-microhei/wqy-microhei.ttc",
             "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "/usr/share/fonts/dejavu/DejaVuSans.ttf"};
#endif
    return fonts;
}

std::unique_ptr<TextRasterizer> CreateFreeTypeRasterizer(const std::string& fontPath, std::string* why)
{
#if defined(TG_WITH_TEXT)
    if (!fontPath.empty()) {
        auto r = std::make_unique<FreeTypeRasterizer>();
        return r->Open(fontPath, why) ? std::move(r) : nullptr;
    }
    for (const auto& path : DefaultFontCandidates()) {
        auto r = std::make_unique<FreeTypeRasterizer>();
        if (r->Open(path, nullptr)) {
            TG_LOGI("TextOverlay: 默认字体 %s", path.c_str());
            return r;
        }
    }
    if (why) *why = "系统里没有找到可用的默认字体";
    return nullptr;
#else
    (void)fontPath;
    if (why) *why = "本构建未编入 FreeType（TG_WITH_TEXT=OFF）";
    return nullptr;
#endif
}

// ================================================================== 时间 / 编码
int64_t WallClockNowUs() noexcept
{
    using namespace std::chrono;
    return duration_cast<microseconds>(system_clock::now().time_since_epoch()).count();
}

namespace
{
constexpr const char* kTimeDirectives = "YymdHIMSpjaFT%";
const char* const kWeekdays[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};

std::tm LocalTm(int64_t wallUs)
{
    int64_t sec = wallUs / 1000000;
    if (wallUs < 0 && wallUs % 1000000 != 0) {
        --sec;
    }
    const std::time_t t = static_cast<std::time_t>(sec);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    return tm;
}
}  // namespace

bool ValidateTimeFormat(const std::string& format, std::string* why)
{
    if (format.empty()) {
        if (why) *why = "格式串为空";
        return false;
    }
    if (format.size() > 256) {
        if (why) *why = "格式串过长（> 256 字节）";
        return false;
    }
    for (size_t i = 0; i < format.size(); ++i) {
        if (format[i] != '%') {
            continue;
        }
        if (i + 1 >= format.size() || !std::strchr(kTimeDirectives, format[i + 1])) {
            if (why) {
                *why = i + 1 >= format.size() ? "格式串以单个 % 结尾"
                                              : std::string("不支持的占位符 %") + format[i + 1];
            }
            return false;
        }
        ++i;
    }
    std::u32string tmp;
    if (!DecodeUtf8(format, tmp)) {
        if (why) *why = "格式串不是合法的 UTF-8";
        return false;
    }
    return true;
}

std::string FormatLocalTime(const std::string& format, int64_t wallUs)
{
    const std::tm tm = LocalTm(wallUs);
    std::string out;
    out.reserve(format.size() + 16);
    char buf[32];
    for (size_t i = 0; i < format.size(); ++i) {
        if (format[i] != '%' || i + 1 >= format.size()) {
            out += format[i];
            continue;
        }
        const char d = format[++i];
        switch (d) {
            case 'Y': std::snprintf(buf, sizeof(buf), "%04d", tm.tm_year + 1900); break;
            case 'y': std::snprintf(buf, sizeof(buf), "%02d", (tm.tm_year + 1900) % 100); break;
            case 'm': std::snprintf(buf, sizeof(buf), "%02d", tm.tm_mon + 1); break;
            case 'd': std::snprintf(buf, sizeof(buf), "%02d", tm.tm_mday); break;
            case 'H': std::snprintf(buf, sizeof(buf), "%02d", tm.tm_hour); break;
            case 'I': std::snprintf(buf, sizeof(buf), "%02d", tm.tm_hour % 12 == 0 ? 12 : tm.tm_hour % 12); break;
            case 'M': std::snprintf(buf, sizeof(buf), "%02d", tm.tm_min); break;
            case 'S': std::snprintf(buf, sizeof(buf), "%02d", std::min(tm.tm_sec, 59)); break;
            case 'p': std::snprintf(buf, sizeof(buf), "%s", tm.tm_hour < 12 ? "AM" : "PM"); break;
            case 'j': std::snprintf(buf, sizeof(buf), "%03d", tm.tm_yday + 1); break;
            case 'a': std::snprintf(buf, sizeof(buf), "%s", kWeekdays[(tm.tm_wday % 7 + 7) % 7]); break;
            case 'F':
                std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
                break;
            case 'T':
                std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, std::min(tm.tm_sec, 59));
                break;
            case '%': std::snprintf(buf, sizeof(buf), "%%"); break;
            default: std::snprintf(buf, sizeof(buf), "%%%c", d); break;
        }
        out += buf;
    }
    return out;
}

bool DecodeUtf8(const std::string& in, std::u32string& out)
{
    out.clear();
    out.reserve(in.size());
    const auto* s = reinterpret_cast<const unsigned char*>(in.data());
    const size_t n = in.size();
    for (size_t i = 0; i < n;) {
        const unsigned char c = s[i];
        char32_t cp = 0;
        int len = 0;
        if (c < 0x80) {
            cp = c;
            len = 1;
        } else if ((c & 0xE0) == 0xC0) {
            cp = c & 0x1F;
            len = 2;
        } else if ((c & 0xF0) == 0xE0) {
            cp = c & 0x0F;
            len = 3;
        } else if ((c & 0xF8) == 0xF0) {
            cp = c & 0x07;
            len = 4;
        } else {
            return false;
        }
        if (i + len > n) {
            return false;
        }
        for (int k = 1; k < len; ++k) {
            if ((s[i + k] & 0xC0) != 0x80) {
                return false;
            }
            cp = (cp << 6) | (s[i + k] & 0x3F);
        }
        // 过长编码 / 代理区 / 超出 Unicode 范围
        if ((len == 2 && cp < 0x80) || (len == 3 && cp < 0x800) || (len == 4 && cp < 0x10000) || cp > 0x10FFFF ||
            (cp >= 0xD800 && cp <= 0xDFFF)) {
            return false;
        }
        out.push_back(cp);
        i += static_cast<size_t>(len);
    }
    return true;
}

void ArgbToYuv(uint32_t argb, uint8_t& y, uint8_t& u, uint8_t& v) noexcept
{
    const int r = static_cast<int>((argb >> 16) & 0xFF);
    const int g = static_cast<int>((argb >> 8) & 0xFF);
    const int b = static_cast<int>(argb & 0xFF);
    auto clamp8 = [](int x) { return static_cast<uint8_t>(std::clamp(x, 0, 255)); };
    // BT.601 limited range（与 Overlay / Watermark 的图片转换一致）
    y = clamp8((66 * r + 129 * g + 25 * b + 128) / 256 + 16);
    u = clamp8((-38 * r - 74 * g + 112 * b + 128) / 256 + 128);
    v = clamp8((112 * r - 94 * g - 18 * b + 128) / 256 + 128);
}

// ================================================================== TimestampOverlay
TimestampOverlay::TimestampOverlay() = default;
TimestampOverlay::~TimestampOverlay() = default;

namespace
{
bool InRange(float v, float lo, float hi)
{
    return std::isfinite(v) && v >= lo && v <= hi;
}
}  // namespace

int TimestampOverlay::Configure(const TimestampOverlayParams& params, std::string* why)
{
    auto fail = [&](int code, const std::string& msg) {
        if (why) *why = msg;
        return code;
    };
    if (!params.enabled) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_params.enabled = false;
        m_cache = Rendered{};
        return TGErr_OK;
    }
    std::string reason;
    if (!ValidateTimeFormat(params.format, &reason)) {
        return fail(TGErr_InvalidArgument, reason);
    }
    if (params.position < -1 || params.position > 5) {
        return fail(TGErr_InvalidArgument, "position 只能是 -1 ~ 5");
    }
    if (params.position == -1 && (!InRange(params.x, 0.0f, 0.999f) || !InRange(params.y, 0.0f, 0.999f))) {
        return fail(TGErr_InvalidArgument, "x / y 必须在 [0, 1) 内");
    }
    if (!InRange(params.margin, 0.0f, 0.5f)) {
        return fail(TGErr_InvalidArgument, "margin 必须在 [0, 0.5] 内");
    }
    if (!InRange(params.fontSize, 0.0001f, 0.5f)) {
        return fail(TGErr_InvalidArgument, "fontSize 必须在 (0, 0.5] 内");
    }
    if (!params.fontPath.empty() && !HasFreeTypeBackend()) {
        return fail(TGErr_NotSupported, "指定了 fontPath，但本构建未编入 FreeType（需 TG_WITH_TEXT=ON）");
    }

    // 光栅化后端：字体没变就沿用（不重复读字体文件）
    std::unique_ptr<TextRasterizer> rasterizer;
    bool reuse = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        reuse = m_rasterizer && m_params.fontPath == params.fontPath;
    }
    if (!reuse) {
        if (HasFreeTypeBackend()) {
            rasterizer = CreateFreeTypeRasterizer(params.fontPath, &reason);
            if (!rasterizer && !params.fontPath.empty()) {
                return fail(TGErr_InvalidArgument, reason);
            }
            if (!rasterizer) {
                TG_LOGW("TextOverlay: %s，回落到内置点阵字体（仅 ASCII）", reason.c_str());
            }
        }
        if (!rasterizer) {
            rasterizer = CreateBitmapRasterizer();
        }
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    if (rasterizer) {
        m_rasterizer = std::move(rasterizer);
    }
    // 格式里有当前字体画不出的字符（点阵字体只有 ASCII）：提示一次，画成 '?'
    std::u32string u32;
    DecodeUtf8(params.format, u32);
    if (std::any_of(u32.begin(), u32.end(), [&](char32_t c) { return !m_rasterizer->HasGlyph(c); })) {
        TG_LOGW("TextOverlay: 格式串里有 %s 字体画不出的字符，将显示为 '?'%s", m_rasterizer->Name(),
                HasFreeTypeBackend() ? "" : "（非 ASCII 字符需 TG_WITH_TEXT=ON 构建）");
    }
    m_params = params;
    m_cache = Rendered{};
    TG_LOGI("TextOverlay: 时间水印开启 format=\"%s\" position=%d fontSize=%.3f backend=%s", params.format.c_str(),
            params.position, params.fontSize, m_rasterizer->Name());
    return TGErr_OK;
}

bool TimestampOverlay::Enabled() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_params.enabled;
}

TimestampOverlayParams TimestampOverlay::Params() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_params;
}

std::string TimestampOverlay::Backend() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_rasterizer ? m_rasterizer->Name() : (HasFreeTypeBackend() ? "freetype" : "bitmap");
}

std::string TimestampOverlay::LastText() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_cache.text;
}

void TimestampOverlay::BlendOnto(Frame& frame, int64_t wallUs)
{
    if (frame.pixFmt != AV_PIX_FMT_NV12 || !frame.planes[0] || !frame.planes[1] || frame.width < 2 ||
        frame.height < 2) {
        return;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_params.enabled || !m_rasterizer) {
        return;
    }
    const std::string text = FormatLocalTime(m_params.format, wallUs);
    if (text != m_cache.text || frame.width != m_cache.canvasW || frame.height != m_cache.canvasH) {
        if (!RenderLocked(text, frame.width, frame.height)) {
            return;
        }
    }
    const Nv12AImage& img = m_cache.image;
    if (img.width < 2 || img.height < 2) {
        return;
    }
    const Nv12AView view{img.y.data(), img.width, img.uv.data(), img.width, img.a.data(), img.width,
                         img.width, img.height};
    BlendNv12A(frame, m_cache.x, m_cache.y, view, 255);
}

bool TimestampOverlay::RenderLocked(const std::string& text, int canvasW, int canvasH)
{
    m_cache.text = text;
    m_cache.canvasW = canvasW;
    m_cache.canvasH = canvasH;
    m_cache.image = Nv12AImage{};

    std::u32string u32;
    if (!DecodeUtf8(text, u32) || u32.empty()) {
        return false;
    }
    const int textH = std::clamp(static_cast<int>(std::lround(m_params.fontSize * canvasH)), 10, canvasH);
    AlphaMask mask;
    if (!m_rasterizer->Render(u32, textH, mask) || mask.width <= 0) {
        return false;
    }

    // 盒子：文字 + 内边距（有底框时）/ 描边留白
    const int stroke = m_params.outline ? std::max(1, textH / 14) : 0;
    const uint32_t bgA = (m_params.backgroundColor >> 24) & 0xFF;
    const int padX = bgA > 0 ? std::max(stroke, textH / 4) : stroke;
    const int padY = bgA > 0 ? std::max(stroke, textH / 8) : stroke;
    const int boxW = std::min(8192, (mask.width + 2 * padX + 1) & ~1);
    const int boxH = std::min(8192, (textH + 2 * padY + 1) & ~1);
    const size_t n = static_cast<size_t>(boxW) * boxH;

    // 描边蒙版：文字蒙版按 stroke 半径做方形膨胀（先水平后竖直，可分离）
    std::vector<uint8_t> text_a(n, 0);
    for (int r = 0; r < mask.height && r + padY < boxH; ++r) {
        for (int c = 0; c < mask.width && c + padX < boxW; ++c) {
            text_a[static_cast<size_t>(r + padY) * boxW + c + padX] = mask.a[static_cast<size_t>(r) * mask.width + c];
        }
    }
    std::vector<uint8_t> outline_a;
    if (stroke > 0) {
        std::vector<uint8_t> tmp(n, 0);
        outline_a.assign(n, 0);
        for (int r = 0; r < boxH; ++r) {
            for (int c = 0; c < boxW; ++c) {
                uint8_t m = 0;
                for (int k = std::max(0, c - stroke); k <= std::min(boxW - 1, c + stroke); ++k) {
                    m = std::max(m, text_a[static_cast<size_t>(r) * boxW + k]);
                }
                tmp[static_cast<size_t>(r) * boxW + c] = m;
            }
        }
        for (int r = 0; r < boxH; ++r) {
            for (int c = 0; c < boxW; ++c) {
                uint8_t m = 0;
                for (int k = std::max(0, r - stroke); k <= std::min(boxH - 1, r + stroke); ++k) {
                    m = std::max(m, tmp[static_cast<size_t>(k) * boxW + c]);
                }
                outline_a[static_cast<size_t>(r) * boxW + c] = m;
            }
        }
    }

    // 三层自下而上做 "over" 合成：底框 → 描边 → 文字，得到每个像素的 YUV + alpha
    uint8_t ty, tu, tv, by, bu, bv;
    ArgbToYuv(m_params.textColor, ty, tu, tv);
    ArgbToYuv(m_params.backgroundColor, by, bu, bv);
    const uint32_t textA = (m_params.textColor >> 24) & 0xFF;
    const uint8_t oy = ty >= 128 ? 16 : 235;   // 描边：与文字反差大的黑 / 白
    const uint8_t ou = 128;
    const uint8_t ov = 128;

    std::vector<float> fy(n), fu(n), fv(n), fa(n);
    for (size_t i = 0; i < n; ++i) {
        float a = bgA / 255.0f;
        float y = by, u = bu, v = bv;
        auto over = [&](float la, float ly, float lu, float lv) {
            const float oa = la + a * (1.0f - la);
            if (oa > 0.0f) {
                y = (ly * la + y * a * (1.0f - la)) / oa;
                u = (lu * la + u * a * (1.0f - la)) / oa;
                v = (lv * la + v * a * (1.0f - la)) / oa;
            }
            a = oa;
        };
        if (!outline_a.empty() && outline_a[i]) {
            over(outline_a[i] / 255.0f * textA / 255.0f, oy, ou, ov);
        }
        if (text_a[i]) {
            over(text_a[i] / 255.0f * textA / 255.0f, ty, tu, tv);
        }
        fy[i] = y;
        fu[i] = u;
        fv[i] = v;
        fa[i] = a;
    }

    Nv12AImage& img = m_cache.image;
    img.width = boxW;
    img.height = boxH;
    img.y.resize(n);
    img.a.resize(n);
    img.uv.resize(n / 2);
    for (size_t i = 0; i < n; ++i) {
        img.y[i] = static_cast<uint8_t>(std::lround(std::clamp(fy[i], 0.0f, 255.0f)));
        img.a[i] = static_cast<uint8_t>(std::lround(std::clamp(fa[i] * 255.0f, 0.0f, 255.0f)));
    }
    // 色度：2x2 块按 alpha 加权平均；BlendNv12A 取块左上角的 alpha，这里把块内最大 alpha 写回左上角，
    // 避免细笔画的块左上角恰好透明时整块色度被跳过
    for (int r = 0; r < boxH; r += 2) {
        for (int c = 0; c < boxW; c += 2) {
            float su = 0, sv = 0, sa = 0, ma = 0;
            for (int dy = 0; dy < 2; ++dy) {
                for (int dx = 0; dx < 2; ++dx) {
                    const size_t i = static_cast<size_t>(r + dy) * boxW + c + dx;
                    su += fu[i] * fa[i];
                    sv += fv[i] * fa[i];
                    sa += fa[i];
                    ma = std::max(ma, fa[i]);
                }
            }
            const size_t o = static_cast<size_t>(r / 2) * boxW + c;
            img.uv[o] = static_cast<uint8_t>(std::lround(sa > 0 ? su / sa : 128.0f));
            img.uv[o + 1] = static_cast<uint8_t>(std::lround(sa > 0 ? sv / sa : 128.0f));
            const size_t tl = static_cast<size_t>(r) * boxW + c;
            img.a[tl] = std::max(img.a[tl], static_cast<uint8_t>(std::lround(ma * 255.0f)));
        }
    }

    // 位置：与水印一致的 0 ~ 5 相对位置（margin 为距边距离），或 -1 = 比例坐标
    const int margin = static_cast<int>(std::lround(m_params.margin * canvasH));
    int x = 0;
    int y = 0;
    switch (m_params.position) {
        case 0: x = margin; y = margin; break;
        case 1: x = canvasW - boxW - margin; y = margin; break;
        case 2: x = margin; y = canvasH - boxH - margin; break;
        case 3: x = canvasW - boxW - margin; y = canvasH - boxH - margin; break;
        case 4: x = (canvasW - boxW) / 2; y = margin; break;
        case 5: x = (canvasW - boxW) / 2; y = canvasH - boxH - margin; break;
        default:
            x = static_cast<int>(std::lround(m_params.x * canvasW));
            y = static_cast<int>(std::lround(m_params.y * canvasH));
            break;
    }
    // 尽量完整留在画面内（盒子比画布还大时贴左上，超出部分由 BlendNv12A 裁掉）
    m_cache.x = std::max(0, std::min(x, canvasW - boxW));
    m_cache.y = std::max(0, std::min(y, canvasH - boxH));
    return true;
}

}  // namespace TGCapture::core
