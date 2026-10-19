// 导出宏与通用定义
#ifndef TGCAPTURE_EXPORT_H
#define TGCAPTURE_EXPORT_H

#if defined(_WIN32) || defined(_WIN64)
#  if defined(TGCAPTURE_BUILD_SHARED)
#    define TGCAPTURE_API __declspec(dllexport)
#  elif defined(TGCAPTURE_USE_SHARED)
#    define TGCAPTURE_API __declspec(dllimport)
#  else
#    define TGCAPTURE_API
#  endif
#else
#  define TGCAPTURE_API __attribute__((visibility("default")))
#endif

#define TGCAPTURE_NODISCARD [[nodiscard]]

#endif  // TGCAPTURE_EXPORT_H
