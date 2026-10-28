#import "TGLiveViewController.h"

#import <AVFoundation/AVFoundation.h>

#import "TGDemoSettings.h"
#import "TGLiveSession.h"

#include "TGCapture/ITGCapturer.h"

typedef NS_ENUM(NSInteger, TGLiveState) {
    TGLiveStateIdle,
    TGLiveStateAuthorizing,
    TGLiveStateStarting,
    TGLiveStateLive,
    TGLiveStateStopping,
};

static const NSUInteger kMaxLogLines = 200;

@interface TGLiveViewController () <UITableViewDataSource, UITableViewDelegate, TGLiveSessionDelegate>
@end

@implementation TGLiveViewController {
    TGLiveState _state;
    TGLiveSession* _session;
    NSMutableArray<NSString*>* _logLines;
    NSDate* _liveSince;
    NSTimer* _ticker;

    UILabel* _stateLabel;
    UILabel* _timeLabel;
    UILabel* _statsLabel;
    UIButton* _startButton;
    UISwitch* _muteAudio;
    UISwitch* _muteVideo;
    UITextView* _logView;
    UIView* _motionBar;
    UITableView* _table;
}

- (void)viewDidLoad
{
    [super viewDidLoad];
    self.title = @"TGCapture 开播测试";
    self.view.backgroundColor = UIColor.systemGroupedBackgroundColor;
    _logLines = [NSMutableArray array];
    [self buildUI];
    [self setState:TGLiveStateIdle];
    [self probeEncoders];
}

// 硬编探测会真的建一次 VideoToolbox 会话（几十 ~ 几百 ms），放后台队列；结果进程内缓存
- (void)probeEncoders
{
    __weak TGLiveViewController* weakSelf = self;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
      const std::string video = TGCapture::ITGCapturerMgr::GetAvailableEncoders(true);
      const std::string audio = TGCapture::ITGCapturerMgr::GetAvailableEncoders(false);
      NSString* line = [NSString stringWithFormat:@"本机可用编码器\n  视频: %s\n  音频: %s",
                                                  video.empty() ? "无" : video.c_str(),
                                                  audio.empty() ? "无" : audio.c_str()];
      dispatch_async(dispatch_get_main_queue(), ^{
        [weakSelf appendLog:line];
      });
    });
}

// ------------------------------------------------------------------ UI 搭建
- (void)buildUI
{
    UILabel* version = [UILabel new];
    version.text = [NSString stringWithFormat:@"TGCapture %s", TGCapture::ITGCapturerMgr::GetVersion()];
    version.font = [UIFont systemFontOfSize:12];
    version.textColor = UIColor.secondaryLabelColor;
    [version setContentHuggingPriority:UILayoutPriorityRequired forAxis:UILayoutConstraintAxisHorizontal];

    _stateLabel = [UILabel new];
    _stateLabel.font = [UIFont boldSystemFontOfSize:17];
    UIStackView* row1 = [[UIStackView alloc] initWithArrangedSubviews:@[ _stateLabel, version ]];

    _timeLabel = [UILabel new];
    _timeLabel.font = [UIFont monospacedDigitSystemFontOfSize:14 weight:UIFontWeightRegular];
    _statsLabel = [UILabel new];
    _statsLabel.font = [UIFont monospacedSystemFontOfSize:11 weight:UIFontWeightRegular];
    _statsLabel.textColor = UIColor.secondaryLabelColor;
    _statsLabel.numberOfLines = 2;

    // 部署目标 iOS 14：不用 iOS 15 才有的 UIButtonConfiguration
    _startButton = [UIButton buttonWithType:UIButtonTypeSystem];
    _startButton.titleLabel.font = [UIFont boldSystemFontOfSize:16];
    [_startButton setTitleColor:UIColor.whiteColor forState:UIControlStateNormal];
    [_startButton setTitleColor:[UIColor colorWithWhite:1 alpha:0.5] forState:UIControlStateDisabled];
    _startButton.layer.cornerRadius = 8;
    _startButton.contentEdgeInsets = UIEdgeInsetsMake(8, 20, 8, 20);
    [_startButton addTarget:self action:@selector(onStartStop) forControlEvents:UIControlEventTouchUpInside];
    [_startButton.widthAnchor constraintGreaterThanOrEqualToConstant:96].active = YES;

    _muteAudio = [UISwitch new];
    [_muteAudio addTarget:self action:@selector(onMuteAudio) forControlEvents:UIControlEventValueChanged];
    _muteVideo = [UISwitch new];
    [_muteVideo addTarget:self action:@selector(onMuteVideo) forControlEvents:UIControlEventValueChanged];

    UIButton* clear = [UIButton buttonWithType:UIButtonTypeSystem];
    [clear setTitle:@"清空日志" forState:UIControlStateNormal];
    [clear addTarget:self action:@selector(onClearLog) forControlEvents:UIControlEventTouchUpInside];

    UIStackView* row3 = [[UIStackView alloc] initWithArrangedSubviews:@[
        _startButton, [self caption:@"静音"], _muteAudio, [self caption:@"关画面"], _muteVideo, [UIView new], clear
    ]];
    row3.spacing = 8;
    row3.alignment = UIStackViewAlignmentCenter;

    // 直播中持续移动的色条：ReplayKit 只在画面变化时出帧，保证录屏期间一直有内容变化，
    // 同时肉眼可判断推流端是否卡顿
    UIView* track = [UIView new];
    track.clipsToBounds = YES;
    [track.heightAnchor constraintEqualToConstant:4].active = YES;
    _motionBar = [[UIView alloc] initWithFrame:CGRectMake(-80, 0, 80, 4)];
    _motionBar.backgroundColor = UIColor.systemRedColor;
    [track addSubview:_motionBar];

    _logView = [UITextView new];
    _logView.editable = NO;
    _logView.font = [UIFont monospacedSystemFontOfSize:11 weight:UIFontWeightRegular];
    _logView.backgroundColor = [UIColor colorWithWhite:0.12 alpha:1];
    _logView.textColor = [UIColor colorWithWhite:0.85 alpha:1];
    _logView.layer.cornerRadius = 6;
    [_logView.heightAnchor constraintEqualToConstant:130].active = YES;

    UIStackView* header = [[UIStackView alloc] initWithArrangedSubviews:@[ row1, _timeLabel, _statsLabel, row3, track, _logView ]];
    header.axis = UILayoutConstraintAxisVertical;
    header.spacing = 6;
    header.translatesAutoresizingMaskIntoConstraints = NO;

    _table = [[UITableView alloc] initWithFrame:CGRectZero style:UITableViewStyleInsetGrouped];
    _table.dataSource = self;
    _table.delegate = self;
    _table.translatesAutoresizingMaskIntoConstraints = NO;

    [self.view addSubview:header];
    [self.view addSubview:_table];
    UILayoutGuide* safe = self.view.safeAreaLayoutGuide;
    [NSLayoutConstraint activateConstraints:@[
        [header.topAnchor constraintEqualToAnchor:safe.topAnchor constant:8],
        [header.leadingAnchor constraintEqualToAnchor:safe.leadingAnchor constant:16],
        [header.trailingAnchor constraintEqualToAnchor:safe.trailingAnchor constant:-16],
        [_table.topAnchor constraintEqualToAnchor:header.bottomAnchor constant:4],
        [_table.leadingAnchor constraintEqualToAnchor:safe.leadingAnchor],
        [_table.trailingAnchor constraintEqualToAnchor:safe.trailingAnchor],
        [_table.bottomAnchor constraintEqualToAnchor:self.view.bottomAnchor],
    ]];
}

- (UILabel*)caption:(NSString*)text
{
    UILabel* l = [UILabel new];
    l.text = text;
    l.font = [UIFont systemFontOfSize:14];
    return l;
}

// ------------------------------------------------------------------ 开播流程
- (void)onStartStop
{
    if (_state == TGLiveStateLive) {
        [self setState:TGLiveStateStopping];
        [_session stop];
        return;
    }
    if (_state != TGLiveStateIdle) {
        return;
    }
    const BOOL portrait = self.view.bounds.size.height >= self.view.bounds.size.width;
    TGLiveConfig* cfg = [TGLiveConfig currentWithPortrait:portrait];
    if (!cfg.outputToFile && cfg.publishUrl.length == 0) {
        [self appendLog:@"请先在下方设置推流地址"];
        return;
    }
    [self appendLog:[@"开播配置：\n" stringByAppendingString:cfg.summary]];
    [self setState:TGLiveStateAuthorizing];

    if (!cfg.microphone) {
        [self startWithConfig:cfg];
        return;
    }
    void (^granted)(BOOL) = ^(BOOL ok) {
      dispatch_async(dispatch_get_main_queue(), ^{
        if (!ok) {
            [self appendLog:@"未授予麦克风权限：麦克风采集可能无声（设置 > 隐私 > 麦克风 中开启）"];
        }
        [self startWithConfig:cfg];
      });
    };
    if (@available(iOS 17.0, *)) {
        [AVAudioApplication requestRecordPermissionWithCompletionHandler:granted];
    } else {
        [[AVAudioSession sharedInstance] requestRecordPermission:granted];
    }
}

- (void)startWithConfig:(TGLiveConfig*)cfg
{
    [self setState:TGLiveStateStarting];
    _session = [[TGLiveSession alloc] initWithConfig:cfg delegate:self];
    [_session start];
}

- (void)onMuteAudio
{
    [_session muteAudio:_muteAudio.on];
}

- (void)onMuteVideo
{
    [_session muteVideo:_muteVideo.on];
}

- (void)onClearLog
{
    [_logLines removeAllObjects];
    _logView.text = @"";
}

// ------------------------------------------------------------------ TGLiveSessionDelegate
- (void)liveSession:(TGLiveSession*)session log:(NSString*)line
{
    [self appendLog:line];
}

- (void)liveSessionDidStart:(TGLiveSession*)session
{
    if (session != _session) return;
    _liveSince = [NSDate date];
    [self setState:TGLiveStateLive];
    __weak TGLiveViewController* weakSelf = self;
    _ticker = [NSTimer scheduledTimerWithTimeInterval:1.0 repeats:YES block:^(NSTimer* t) { [weakSelf tick]; }];
    [self tick];
}

- (void)liveSession:(TGLiveSession*)session didFinishWithError:(int)err
{
    if (session != _session) return;
    [self appendLog:err == 0 ? @"已停播" : @"开播失败（详见上方日志）"];
    [_ticker invalidate];
    _ticker = nil;
    _session = nil;
    _muteAudio.on = NO;
    _muteVideo.on = NO;
    [self setState:TGLiveStateIdle];
}

- (void)tick
{
    const int secs = (int)[[NSDate date] timeIntervalSinceDate:_liveSince];
    [_session queryStats:^(float bps, NSString* info) {
      self->_timeLabel.text = [NSString stringWithFormat:@"%02d:%02d   %.1f kbps", secs / 60, secs % 60, bps / 1000.0f];
      self->_statsLabel.text = info;
    }];
}

// ------------------------------------------------------------------ 状态
- (void)setState:(TGLiveState)state
{
    _state = state;
    static NSString* const kNames[] = {@"空闲", @"等待授权…", @"连接中…", @"● 直播中", @"停止中…"};
    _stateLabel.text = kNames[state];
    _stateLabel.textColor = state == TGLiveStateLive ? UIColor.systemRedColor : UIColor.labelColor;

    [_startButton setTitle:state == TGLiveStateIdle ? @"开始" : @"停止" forState:UIControlStateNormal];
    _startButton.backgroundColor = state == TGLiveStateIdle ? UIColor.systemBlueColor : UIColor.systemRedColor;
    _startButton.enabled = state == TGLiveStateIdle || state == TGLiveStateLive;
    _startButton.alpha = _startButton.enabled ? 1.0 : 0.6;

    const BOOL live = state == TGLiveStateLive;
    _muteAudio.enabled = live;
    _muteVideo.enabled = live;
    _table.userInteractionEnabled = state == TGLiveStateIdle;
    _table.alpha = state == TGLiveStateIdle ? 1.0 : 0.5;
    [UIApplication sharedApplication].idleTimerDisabled = state != TGLiveStateIdle;

    [_motionBar.layer removeAllAnimations];
    if (live) {
        CABasicAnimation* move = [CABasicAnimation animationWithKeyPath:@"position.x"];
        move.fromValue = @(-40);
        move.toValue = @(self.view.bounds.size.width + 40);
        move.duration = 2.0;
        move.repeatCount = HUGE_VALF;
        [_motionBar.layer addAnimation:move forKey:@"move"];
    } else {
        _timeLabel.text = @" ";
        _statsLabel.text = @" ";
    }
}

- (void)appendLog:(NSString*)line
{
    static NSDateFormatter* fmt;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
      fmt = [NSDateFormatter new];
      fmt.dateFormat = @"HH:mm:ss";
    });
    [_logLines addObject:[NSString stringWithFormat:@"%@  %@", [fmt stringFromDate:[NSDate date]], line]];
    while (_logLines.count > kMaxLogLines) {
        [_logLines removeObjectAtIndex:0];
    }
    _logView.text = [_logLines componentsJoinedByString:@"\n"];
    [_logView scrollRangeToVisible:NSMakeRange(_logView.text.length, 0)];
}

// ------------------------------------------------------------------ 设置表
- (NSInteger)numberOfSectionsInTableView:(UITableView*)tableView
{
    return (NSInteger)TGDemoSettings.sections.count;
}

- (NSInteger)tableView:(UITableView*)tableView numberOfRowsInSection:(NSInteger)section
{
    return (NSInteger)TGDemoSettings.sections[section].items.count;
}

- (NSString*)tableView:(UITableView*)tableView titleForHeaderInSection:(NSInteger)section
{
    return TGDemoSettings.sections[section].title;
}

- (NSString*)tableView:(UITableView*)tableView titleForFooterInSection:(NSInteger)section
{
    NSMutableArray* notes = [NSMutableArray array];
    for (TGSettingItem* item in TGDemoSettings.sections[section].items) {
        if (item.footnote) [notes addObject:item.footnote];
    }
    return notes.count ? [notes componentsJoinedByString:@"\n"] : nil;
}

- (TGSettingItem*)itemAt:(NSIndexPath*)ip
{
    return TGDemoSettings.sections[ip.section].items[ip.row];
}

- (UITableViewCell*)tableView:(UITableView*)tableView cellForRowAtIndexPath:(NSIndexPath*)ip
{
    TGSettingItem* item = [self itemAt:ip];
    UITableViewCell* cell = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleValue1 reuseIdentifier:nil];
    cell.textLabel.text = item.title;
    if (item.kind == TGSettingKindSwitch) {
        UISwitch* sw = [UISwitch new];
        sw.on = [TGDemoSettings boolForKey:item.key];
        sw.accessibilityIdentifier = item.key;
        [sw addTarget:self action:@selector(onSettingSwitch:) forControlEvents:UIControlEventValueChanged];
        cell.accessoryView = sw;
        cell.selectionStyle = UITableViewCellSelectionStyleNone;
    } else {
        cell.detailTextLabel.text = item.displayValue;
        cell.detailTextLabel.lineBreakMode = NSLineBreakByTruncatingMiddle;
        cell.accessoryType = UITableViewCellAccessoryDisclosureIndicator;
    }
    return cell;
}

- (void)onSettingSwitch:(UISwitch*)sw
{
    [TGDemoSettings setBool:sw.on forKey:sw.accessibilityIdentifier];
}

- (void)tableView:(UITableView*)tableView didSelectRowAtIndexPath:(NSIndexPath*)ip
{
    [tableView deselectRowAtIndexPath:ip animated:YES];
    TGSettingItem* item = [self itemAt:ip];
    if (item.kind == TGSettingKindText) {
        UIAlertController* alert = [UIAlertController alertControllerWithTitle:item.title
                                                                       message:nil
                                                                preferredStyle:UIAlertControllerStyleAlert];
        [alert addTextFieldWithConfigurationHandler:^(UITextField* tf) {
          tf.text = [TGDemoSettings stringForKey:item.key];
          tf.keyboardType = UIKeyboardTypeURL;
          tf.autocapitalizationType = UITextAutocapitalizationTypeNone;
          tf.autocorrectionType = UITextAutocorrectionTypeNo;
          tf.clearButtonMode = UITextFieldViewModeWhileEditing;
        }];
        [alert addAction:[UIAlertAction actionWithTitle:@"取消" style:UIAlertActionStyleCancel handler:nil]];
        [alert addAction:[UIAlertAction actionWithTitle:@"确定"
                                                  style:UIAlertActionStyleDefault
                                                handler:^(UIAlertAction*) {
                                                  [TGDemoSettings setString:alert.textFields.firstObject.text ?: @""
                                                                     forKey:item.key];
                                                  [tableView reloadRowsAtIndexPaths:@[ ip ]
                                                                   withRowAnimation:UITableViewRowAnimationNone];
                                                }]];
        [self presentViewController:alert animated:YES completion:nil];
        return;
    }
    if (item.kind != TGSettingKindChoice) {
        return;
    }
    UIAlertController* sheet = [UIAlertController alertControllerWithTitle:item.title
                                                                   message:nil
                                                            preferredStyle:UIAlertControllerStyleActionSheet];
    NSString* current = [TGDemoSettings stringForKey:item.key];
    [item.values enumerateObjectsUsingBlock:^(NSString* value, NSUInteger idx, BOOL*) {
      NSString* title = [value isEqualToString:current] ? [@"✓ " stringByAppendingString:item.labels[idx]]
                                                        : item.labels[idx];
      [sheet addAction:[UIAlertAction actionWithTitle:title
                                                style:UIAlertActionStyleDefault
                                              handler:^(UIAlertAction*) {
                                                [TGDemoSettings setString:value forKey:item.key];
                                                [tableView reloadRowsAtIndexPaths:@[ ip ]
                                                                 withRowAnimation:UITableViewRowAnimationNone];
                                              }]];
    }];
    [sheet addAction:[UIAlertAction actionWithTitle:@"取消" style:UIAlertActionStyleCancel handler:nil]];
    UITableViewCell* cell = [tableView cellForRowAtIndexPath:ip];
    sheet.popoverPresentationController.sourceView = cell;   // iPad 必须给锚点
    sheet.popoverPresentationController.sourceRect = cell.bounds;
    [self presentViewController:sheet animated:YES completion:nil];
}

@end
