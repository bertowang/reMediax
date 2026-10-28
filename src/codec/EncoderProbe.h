// 编码器探测：判断某个编码器在**本机**能不能用。
//
// "编进了 ffmpeg"不等于"能用"：硬编（MediaCodec / VideoToolbox / NVENC / AMF / QSV / MF）
// 还取决于设备、系统版本与驱动 —— 模拟器、老机型、没有对应显卡的 PC 上
// avcodec_find_encoder 找得到，avcodec_open2 却会失败。
// 这里用一组最小参数真正打开一次来判定，结果按名字进程内缓存（硬编首次探测几十 ~ 几百 ms）。
#ifndef TGCAPTURE_CODEC_ENCODER_PROBE_H
#define TGCAPTURE_CODEC_ENCODER_PROBE_H

#include <string>
#include <vector>

namespace TGCapture::codec
{

// 硬编不可用时的软件兜底（三端都编进了 openh264）
inline constexpr const char* kSoftwareVideoEncoder = "libopenh264";

// 按名字判断是否属于硬件视频编码器族（只看名字，不探测）
bool IsHardwareVideoEncoder(const std::string& name);

// 编码器在本机是否可用：已编进 ffmpeg，且能以最小参数 avcodec_open2 成功。线程安全，结果缓存。
bool ProbeEncoder(const std::string& name);

// TGCapture 支持的 H.264 / AAC 编码器里，本机可用的那些（按推荐顺序：视频硬编在前）
std::vector<std::string> AvailableEncoders(bool video);

}  // namespace TGCapture::codec

#endif  // TGCAPTURE_CODEC_ENCODER_PROBE_H
