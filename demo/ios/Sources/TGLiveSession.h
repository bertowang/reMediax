// 一次开播会话：ITGCapturer 的完整生命周期。
// Prepare / Start / Stop 会阻塞（连推流地址、写文件尾），全部在私有串行队列上执行；
// 事件统一回到主线程交给 delegate。
#import <Foundation/Foundation.h>

#import "TGDemoSettings.h"

NS_ASSUME_NONNULL_BEGIN

@class TGLiveSession;

@protocol TGLiveSessionDelegate <NSObject>
- (void)liveSession:(TGLiveSession*)session log:(NSString*)line;
- (void)liveSessionDidStart:(TGLiveSession*)session;
/// err = 0 正常停播；非 0 = 开播失败
- (void)liveSession:(TGLiveSession*)session didFinishWithError:(int)err;
@end

@interface TGLiveSession : NSObject
@property (nonatomic, readonly) TGLiveConfig* config;

- (instancetype)initWithConfig:(TGLiveConfig*)config delegate:(id<TGLiveSessionDelegate>)delegate;
- (void)start;
/// 停播（可重复调用）
- (void)stop;
- (void)muteAudio:(BOOL)mute;
- (void)muteVideo:(BOOL)mute;
/// 当前码率（bps）与码率统计 JSON，主线程回调
- (void)queryStats:(void (^)(float bps, NSString* info))completion;
@end

/// 错误码 / 通知类型的可读文本（对应 include/TGCapture/TGCapturerInterface.h）
NSString* TGErrorName(int code);
NSString* TGNotifyName(int type);

NS_ASSUME_NONNULL_END
