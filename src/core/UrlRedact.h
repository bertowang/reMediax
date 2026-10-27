// 推流地址脱敏：日志 / 统计 JSON 里不得出现 streamkey、token、用户名密码。
//
//   rtmp://user:pw@host:1935/live/streamkey?token=abc  ->  rtmp://***@host:1935/live/***?***
//   srt://host:9000?streamid=secret                    ->  srt://host:9000?***
//   /tmp/out.flv  (本地路径，无 scheme)                  ->  原样返回
//
// 规则：保留 scheme、host、port 与路径的前面几段（便于排查连到了哪个服务），
// 路径最后一段（通常就是 streamkey）与整个 query 一律遮蔽。
#ifndef TGCAPTURE_CORE_URL_REDACT_H
#define TGCAPTURE_CORE_URL_REDACT_H

#include <string>

namespace TGCapture::core
{

inline std::string RedactUrl(const std::string& url)
{
    const auto schemePos = url.find("://");
    if (schemePos == std::string::npos) {
        return url;   // 本地路径
    }

    const std::string scheme = url.substr(0, schemePos + 3);
    std::string rest = url.substr(schemePos + 3);

    // query / fragment：整体遮蔽
    bool hasQuery = false;
    const auto queryPos = rest.find_first_of("?#");
    if (queryPos != std::string::npos) {
        hasQuery = true;
        rest.erase(queryPos);
    }

    // authority 与 path 分开
    std::string authority = rest;
    std::string path;
    const auto slashPos = rest.find('/');
    if (slashPos != std::string::npos) {
        authority = rest.substr(0, slashPos);
        path = rest.substr(slashPos);   // 含前导 '/'
    }

    // userinfo（user:pass@）整体遮蔽
    const auto atPos = authority.rfind('@');
    if (atPos != std::string::npos) {
        authority = "***@" + authority.substr(atPos + 1);
    }

    // 路径最后一段遮蔽；只有一段时也遮蔽（/streamkey）
    if (!path.empty()) {
        const auto lastSlash = path.find_last_of('/');
        if (lastSlash + 1 < path.size()) {
            path = path.substr(0, lastSlash + 1) + "***";
        }
    }

    return scheme + authority + path + (hasQuery ? "?***" : "");
}

}  // namespace TGCapture::core

#endif  // TGCAPTURE_CORE_URL_REDACT_H
