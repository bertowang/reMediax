// 兼容层：把 TGCapture 内核映射回既有 ESMediaX::ESCapturer 接口。
// 老调用方（Windows MFC/DLL、Android JNI、iOS OC 封装）只需把
//   #include "ESMediaXCapturer-Interface.h" / "IESMediaXCapturer.h"
// 换成本头文件，其余代码零改动即可切到新内核。
#ifndef TGCAPTURE_COMPAT_ESMEDIAXCAPTURER_H
#define TGCAPTURE_COMPAT_ESMEDIAXCAPTURER_H

#include "TGCapture/ITGCapturer.h"
#include "TGCapture/TGCapturerInterface.h"

namespace ESMediaX
{
namespace ESCapturer
{
// 枚举 / 结构体
using GameID = TGCapture::GameID;
using AudioCapSourceType = TGCapture::AudioCapSourceType;
using VideoCapSourceType = TGCapture::VideoCapSourceType;
using NotifyMsgType = TGCapture::NotifyMsgType;
using VideoCapMode = TGCapture::VideoCapMode;
using VideoScaleStrategy = TGCapture::VideoScaleStrategy;
using AudioEncoder = TGCapture::AudioEncoder;
using VideoEncdoer = TGCapture::VideoEncdoer;  // 沿用原拼写
using OutputFormat = TGCapture::OutputFormat;
using StatsInfoType = TGCapture::StatsInfoType;
using LogoOpType = TGCapture::LogoOpType;

using IAudioSource = TGCapture::IAudioSource;
using IVideoSource = TGCapture::IVideoSource;

using VideoEncoderParams = TGCapture::VideoEncoderParams;
using AudioEncoderParams = TGCapture::AudioEncoderParams;
using MuxerParams = TGCapture::MuxerParams;
using PauseParams = TGCapture::PauseParams;
using WatermarkParams = TGCapture::WatermarkParams;

// 门面
using IESMediaXCapturer = TGCapture::ITGCapturer;
using IESMediaXCapturerMgr = TGCapture::ITGCapturerMgr;

// 原枚举常量（未加作用域限定，保持原样可访问）
constexpr AudioCapSourceType TypeOfANone = TGCapture::TypeOfANone;
constexpr AudioCapSourceType TypeOfSelfMicphone = TGCapture::TypeOfSelfMicphone;
constexpr AudioCapSourceType TypeOfSpeaker = TGCapture::TypeOfSpeaker;
constexpr VideoCapMode DXGI = TGCapture::DXGI;
constexpr VideoCapMode WGC = TGCapture::WGC;
constexpr VideoEncdoer libopenh264 = TGCapture::libopenh264;
constexpr VideoEncdoer h264_nvenc = TGCapture::h264_nvenc;
constexpr VideoEncdoer h264_mediacodec = TGCapture::h264_mediacodec;
constexpr VideoEncdoer h264_videotoolbox = TGCapture::h264_videotoolbox;

}  // namespace ESCapturer
}  // namespace ESMediaX

#endif  // TGCAPTURE_COMPAT_ESMEDIAXCAPTURER_H
