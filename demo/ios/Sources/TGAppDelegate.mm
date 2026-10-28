#import "TGAppDelegate.h"

#import "TGLiveViewController.h"

#include "TGCapture/ITGCapturer.h"

@implementation TGAppDelegate

- (BOOL)application:(UIApplication*)application didFinishLaunchingWithOptions:(NSDictionary*)launchOptions
{
    // 进程内一次。printLog=true：内核日志同时打到 Xcode 控制台；
    // 落盘目录 Documents/logs 可在「文件」App 或 Finder 设备页取出
    NSString* docs = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject;
    NSString* logDir = [docs stringByAppendingPathComponent:@"logs"];
    [[NSFileManager defaultManager] createDirectoryAtPath:logDir withIntermediateDirectories:YES attributes:nil error:nil];
    TGCapture::ITGCapturerMgr::Init(1, logDir.UTF8String, true);

    self.window = [[UIWindow alloc] initWithFrame:UIScreen.mainScreen.bounds];
    self.window.rootViewController =
        [[UINavigationController alloc] initWithRootViewController:[TGLiveViewController new]];
    [self.window makeKeyAndVisible];
    return YES;
}

@end
