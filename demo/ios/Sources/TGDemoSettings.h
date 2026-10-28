// 开播参数：设置页的数据模型（NSUserDefaults 持久化）+ 开播时的取值快照
#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

typedef NS_ENUM(NSInteger, TGSettingKind) {
    TGSettingKindChoice,   // 多选一（values / labels）
    TGSettingKindText,     // 文本输入
    TGSettingKindSwitch,   // 开关
};

@interface TGSettingItem : NSObject
@property (nonatomic, copy) NSString* key;
@property (nonatomic, copy) NSString* title;
@property (nonatomic) TGSettingKind kind;
@property (nonatomic, copy) NSArray<NSString*>* values;
@property (nonatomic, copy) NSArray<NSString*>* labels;
@property (nonatomic, copy, nullable) NSString* footnote;
/// 当前值的展示文本（choice 显示 label，text 显示原文）
- (NSString*)displayValue;
@end

@interface TGSettingSection : NSObject
@property (nonatomic, copy) NSString* title;
@property (nonatomic, copy) NSArray<TGSettingItem*>* items;
@end

@interface TGDemoSettings : NSObject
+ (NSArray<TGSettingSection*>*)sections;
+ (NSString*)stringForKey:(NSString*)key;
+ (BOOL)boolForKey:(NSString*)key;
+ (void)setString:(NSString*)value forKey:(NSString*)key;
+ (void)setBool:(BOOL)value forKey:(NSString*)key;
@end

/// 开播时读一次的配置快照（直播期间不随设置页变化）
@interface TGLiveConfig : NSObject
@property (nonatomic, readonly) BOOL outputToFile;
@property (nonatomic, readonly, copy) NSString* publishUrl;
@property (nonatomic, readonly, copy) NSString* muxerFormat;
@property (nonatomic, readonly) BOOL screenCapture;   // NO = 测试图案（外部注入）
@property (nonatomic, readonly) int width;
@property (nonatomic, readonly) int height;
@property (nonatomic, readonly) int fps;
@property (nonatomic, readonly) int videoBitrateKbps;
@property (nonatomic, readonly, copy) NSString* videoCodec;
@property (nonatomic, readonly) int gop;
@property (nonatomic, readonly) BOOL gpuMode;
@property (nonatomic, readonly) BOOL microphone;
@property (nonatomic, readonly) BOOL testTone;
@property (nonatomic, readonly, copy) NSString* audioCodec;
@property (nonatomic, readonly) int audioBitrateKbps;
@property (nonatomic, readonly) int sampleRate;
@property (nonatomic, readonly) int channels;
@property (nonatomic, readonly) int avTimeoutMs;
@property (nonatomic, readonly) BOOL audioEnabled;

+ (instancetype)currentWithPortrait:(BOOL)portraitScreen;
/// 推流地址，或 Documents/records/tgcapture_<时间>.<flv|ts>
- (NSString*)resolvedOutput;
- (NSString*)summary;
@end

NS_ASSUME_NONNULL_END
