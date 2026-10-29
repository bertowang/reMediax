// 分辨率档位表（ITGCapturerMgr::ApplyResolutionPreset / ITGCapturer::SetResolutionPreset 共用，单一来源）
#ifndef TGCAPTURE_CORE_RESOLUTION_PRESETS_H
#define TGCAPTURE_CORE_RESOLUTION_PRESETS_H

#include <cctype>
#include <cstddef>
#include <string>

namespace TGCapture::core
{

struct ResolutionPreset
{
    const char* name;
    int width;    // 横屏
    int height;
    float fps;
    int bitrate;  // 建议码率 bps（H.264，游戏 / 屏幕内容）
};

// 按分辨率从低到高；同分辨率 30fps 在前
inline constexpr ResolutionPreset kResolutionPresets[] = {
    {"360p", 640, 360, 30.0f, 800000},
    {"480p", 854, 480, 30.0f, 1200000},
    {"540p", 960, 540, 30.0f, 1800000},
    {"720p", 1280, 720, 30.0f, 3000000},
    {"720p60", 1280, 720, 60.0f, 4500000},
    {"1080p", 1920, 1080, 30.0f, 5000000},
    {"1080p60", 1920, 1080, 60.0f, 8000000},
};

// 不区分大小写查找，找不到返回 nullptr
inline const ResolutionPreset* FindResolutionPreset(const std::string& name)
{
    for (const auto& p : kResolutionPresets) {
        const std::string n = p.name;
        if (n.size() != name.size()) {
            continue;
        }
        bool same = true;
        for (size_t i = 0; i < n.size() && same; ++i) {
            same = std::tolower(static_cast<unsigned char>(name[i])) == static_cast<unsigned char>(n[i]);
        }
        if (same) {
            return &p;
        }
    }
    return nullptr;
}

}  // namespace TGCapture::core

#endif  // TGCAPTURE_CORE_RESOLUTION_PRESETS_H
