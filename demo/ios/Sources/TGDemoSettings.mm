#import "TGDemoSettings.h"

static NSString* const kDefaultUrl = @"rtmp://101.91.22.77/live/test-1";

@implementation TGSettingItem
- (NSString*)displayValue
{
    if (self.kind == TGSettingKindSwitch) {
        return @"";
    }
    NSString* value = [TGDemoSettings stringForKey:self.key];
    if (self.kind == TGSettingKindChoice) {
        NSUInteger idx = [self.values indexOfObject:value];
        return idx == NSNotFound ? value : self.labels[idx];
    }
    return value;
}
@end

@implementation TGSettingSection
@end

static TGSettingItem* Choice(NSString* key, NSString* title, NSArray* values, NSArray* labels, NSString* note)
{
    TGSettingItem* item = [TGSettingItem new];
    item.key = key;
    item.title = title;
    item.kind = TGSettingKindChoice;
    item.values = values;
    item.labels = labels ?: values;
    item.footnote = note;
    return item;
}

static TGSettingItem* Text(NSString* key, NSString* title)
{
    TGSettingItem* item = [TGSettingItem new];
    item.key = key;
    item.title = title;
    item.kind = TGSettingKindText;
    return item;
}

static TGSettingItem* Switch(NSString* key, NSString* title, NSString* note)
{
    TGSettingItem* item = [TGSettingItem new];
    item.key = key;
    item.title = title;
    item.kind = TGSettingKindSwitch;
    item.footnote = note;
    return item;
}

static TGSettingSection* Section(NSString* title, NSArray* items)
{
    TGSettingSection* s = [TGSettingSection new];
    s.title = title;
    s.items = items;
    return s;
}

@implementation TGDemoSettings

+ (void)initialize
{
    if (self != [TGDemoSettings class]) {
        return;
    }
    // 默认值（与 Android demo 对齐；iOS 默认编码器换成系统硬编 videotoolbox / aac_at）
    [[NSUserDefaults standardUserDefaults] registerDefaults:@{
        @"output_mode" : @"publish",
        @"publish_url" : kDefaultUrl,
        @"muxer_format" : @"flv",
        @"video_source" : @"screen",
        @"resolution" : @"720",
        @"orientation" : @"auto",
        @"fps" : @"30",
        @"video_bitrate" : @"3000",
        @"video_codec" : @"h264_videotoolbox",
        @"gop_seconds" : @"2",
        @"perf_mode" : @"1",
        @"audio_mic" : @YES,
        @"audio_tone" : @NO,
        @"audio_codec" : @"aac_at",
        @"audio_bitrate" : @"128",
        @"audio_sample_rate" : @"44100",
        @"audio_channels" : @"2",
        @"av_timeout" : @"3000",
    }];
}

+ (NSArray<TGSettingSection*>*)sections
{
    static NSArray* sections;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
      sections = @[
          Section(@"输出", @[
              Choice(@"output_mode", @"输出方式", @[ @"publish", @"file" ], @[ @"推流（RTMP / SRT）", @"录制到本地文件" ],
                     @"本地文件在 Documents/records，可用「文件」App 取出"),
              Text(@"publish_url", @"推流地址"),
              Choice(@"muxer_format", @"封装格式", @[ @"flv", @"mpegts" ], @[ @"FLV（RTMP）", @"MPEG-TS（SRT / .ts）" ], nil),
          ]),
          Section(@"视频", @[
              Choice(@"video_source", @"视频源", @[ @"screen", @"pattern" ],
                     @[ @"屏幕录制（ReplayKit）", @"测试图案（外部注入 NV12）" ],
                     @"ReplayKit 只能录本 App 画面（系统级录屏需 Broadcast Upload Extension）"),
              Choice(@"resolution", @"分辨率", @[ @"540", @"720", @"1080" ],
                     @[ @"540P（960×540）", @"720P（1280×720）", @"1080P（1920×1080）" ], nil),
              Choice(@"orientation", @"画面方向", @[ @"auto", @"landscape", @"portrait" ], @[ @"跟随屏幕", @"横屏", @"竖屏" ],
                     nil),
              Choice(@"fps", @"帧率", @[ @"15", @"24", @"30", @"60" ], nil, nil),
              Choice(@"video_bitrate", @"视频码率", @[ @"1000", @"1500", @"2000", @"3000", @"4000", @"6000", @"8000" ],
                     @[ @"1000 kbps", @"1500 kbps", @"2000 kbps", @"3000 kbps", @"4000 kbps", @"6000 kbps", @"8000 kbps" ],
                     nil),
              Choice(@"video_codec", @"视频编码器", @[ @"h264_videotoolbox", @"libopenh264" ],
                     @[ @"h264_videotoolbox（硬编）", @"libopenh264（软编）" ], nil),
              Choice(@"gop_seconds", @"关键帧间隔", @[ @"1", @"2", @"4" ], @[ @"1 秒", @"2 秒", @"4 秒" ], nil),
              Choice(@"perf_mode", @"性能模式", @[ @"0", @"1" ], @[ @"CPU（内存帧）", @"GPU（CVPixelBuffer 直通硬编）" ],
                     @"GPU 不可用时（如软编）内核自动回落 CPU"),
          ]),
          Section(@"音频", @[
              Switch(@"audio_mic", @"麦克风", nil),
              Switch(@"audio_tone", @"测试音", @"1kHz 正弦（外部注入），无麦克风时验证音频链路"),
              Choice(@"audio_codec", @"音频编码器", @[ @"aac_at", @"libfdk_aac", @"aac" ],
                     @[ @"aac_at（AudioToolbox）", @"libfdk_aac", @"aac（ffmpeg 内置）" ], nil),
              Choice(@"audio_bitrate", @"音频码率", @[ @"64", @"96", @"128", @"192" ],
                     @[ @"64 kbps", @"96 kbps", @"128 kbps", @"192 kbps" ], nil),
              Choice(@"audio_sample_rate", @"采样率", @[ @"44100", @"48000" ], nil, nil),
              Choice(@"audio_channels", @"声道", @[ @"1", @"2" ], @[ @"单声道", @"立体声" ],
                     @"iOS 不支持系统内录（需 Broadcast Upload Extension）"),
          ]),
          Section(@"高级", @[
              Choice(@"av_timeout", @"断流告警阈值", @[ @"0", @"3000", @"5000", @"10000" ],
                     @[ @"关闭", @"3 秒", @"5 秒", @"10 秒" ], nil),
          ]),
      ];
    });
    return sections;
}

+ (NSString*)stringForKey:(NSString*)key
{
    return [[NSUserDefaults standardUserDefaults] stringForKey:key] ?: @"";
}

+ (BOOL)boolForKey:(NSString*)key
{
    return [[NSUserDefaults standardUserDefaults] boolForKey:key];
}

+ (void)setString:(NSString*)value forKey:(NSString*)key
{
    [[NSUserDefaults standardUserDefaults] setObject:value forKey:key];
}

+ (void)setBool:(BOOL)value forKey:(NSString*)key
{
    [[NSUserDefaults standardUserDefaults] setBool:value forKey:key];
}

@end

@implementation TGLiveConfig

+ (instancetype)currentWithPortrait:(BOOL)portraitScreen
{
    TGLiveConfig* c = [TGLiveConfig new];
    [c loadWithPortrait:portraitScreen];
    return c;
}

- (void)loadWithPortrait:(BOOL)portraitScreen
{
    _outputToFile = [[TGDemoSettings stringForKey:@"output_mode"] isEqualToString:@"file"];
    _publishUrl = [[TGDemoSettings stringForKey:@"publish_url"]
        stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]];
    _muxerFormat = [TGDemoSettings stringForKey:@"muxer_format"];
    _screenCapture = [[TGDemoSettings stringForKey:@"video_source"] isEqualToString:@"screen"];

    NSString* res = [TGDemoSettings stringForKey:@"resolution"];
    int longSide = 1280, shortSide = 720;
    if ([res isEqualToString:@"540"]) {
        longSide = 960, shortSide = 540;
    } else if ([res isEqualToString:@"1080"]) {
        longSide = 1920, shortSide = 1080;
    }
    NSString* orientation = [TGDemoSettings stringForKey:@"orientation"];
    BOOL portrait = [orientation isEqualToString:@"portrait"] ||
                    ([orientation isEqualToString:@"auto"] && portraitScreen);
    _width = portrait ? shortSide : longSide;
    _height = portrait ? longSide : shortSide;

    _fps = [TGDemoSettings stringForKey:@"fps"].intValue;
    _videoBitrateKbps = [TGDemoSettings stringForKey:@"video_bitrate"].intValue;
    _videoCodec = [TGDemoSettings stringForKey:@"video_codec"];
    _gop = _fps * [TGDemoSettings stringForKey:@"gop_seconds"].intValue;
    _gpuMode = [[TGDemoSettings stringForKey:@"perf_mode"] isEqualToString:@"1"];
    _microphone = [TGDemoSettings boolForKey:@"audio_mic"];
    _testTone = [TGDemoSettings boolForKey:@"audio_tone"];
    _audioCodec = [TGDemoSettings stringForKey:@"audio_codec"];
    _audioBitrateKbps = [TGDemoSettings stringForKey:@"audio_bitrate"].intValue;
    _sampleRate = [TGDemoSettings stringForKey:@"audio_sample_rate"].intValue;
    _channels = [TGDemoSettings stringForKey:@"audio_channels"].intValue;
    _avTimeoutMs = [TGDemoSettings stringForKey:@"av_timeout"].intValue;
    _audioEnabled = _microphone || _testTone;
}

- (NSString*)resolvedOutput
{
    if (!self.outputToFile) {
        return self.publishUrl;
    }
    NSString* docs = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject;
    NSString* dir = [docs stringByAppendingPathComponent:@"records"];
    [[NSFileManager defaultManager] createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil];
    NSDateFormatter* fmt = [NSDateFormatter new];
    fmt.dateFormat = @"yyyyMMdd_HHmmss";
    fmt.locale = [NSLocale localeWithLocaleIdentifier:@"en_US_POSIX"];
    NSString* ext = [self.muxerFormat isEqualToString:@"mpegts"] ? @"ts" : @"flv";
    return [dir stringByAppendingPathComponent:[NSString stringWithFormat:@"tgcapture_%@.%@",
                                                                          [fmt stringFromDate:[NSDate date]], ext]];
}

- (NSString*)summary
{
    NSMutableString* s = [NSMutableString string];
    [s appendFormat:@"%@ [%@]\n", self.outputToFile ? @"本地文件" : self.publishUrl, self.muxerFormat];
    [s appendFormat:@"视频 %@ %dx%d@%d %dkbps %@ gop=%d %@\n", self.screenCapture ? @"录屏" : @"测试图案", self.width,
                    self.height, self.fps, self.videoBitrateKbps, self.videoCodec, self.gop,
                    self.gpuMode ? @"GPU" : @"CPU"];
    if (self.audioEnabled) {
        NSMutableArray* src = [NSMutableArray array];
        if (self.microphone) [src addObject:@"麦克风"];
        if (self.testTone) [src addObject:@"测试音"];
        [s appendFormat:@"音频 %@ %dHz %dch %dkbps %@", [src componentsJoinedByString:@"+"], self.sampleRate,
                        self.channels, self.audioBitrateKbps, self.audioCodec];
    } else {
        [s appendString:@"音频 关闭（纯视频）"];
    }
    return s;
}

@end
