#import "app/UpdateChecker.h"
#import "common/Support.h"
@implementation SRUpdateChecker {
    BOOL _busy;
}
- (void)check {
    if (_busy) return; _busy=YES;
    NSURL* url=[NSURL URLWithString:@"https://api.github.com/repos/0327sjk-code/SuperRecording/releases?per_page=30"];
    NSMutableURLRequest* request=[NSMutableURLRequest requestWithURL:url]; request.timeoutInterval=20;
    [request setValue:@"SuperRecording-macOS" forHTTPHeaderField:@"User-Agent"];
    [[[NSURLSession sharedSession] dataTaskWithRequest:request completionHandler:^(NSData* data,NSURLResponse* response,NSError* error) {
        NSArray* releases=data?[NSJSONSerialization JSONObjectWithData:data options:0 error:nil]:nil;
        NSDictionary* newest=nil; NSString* version=nil;
        if ([releases isKindOfClass:NSArray.class]) for (NSDictionary* release in releases) {
            NSString* tag=release[@"tag_name"];
            if (![tag hasPrefix:@"mac-v"] || [release[@"draft"] boolValue]) continue;
            NSString* candidate=[tag substringFromIndex:5];
            if ([candidate compare:@"1.7.0.1" options:NSNumericSearch]!=NSOrderedDescending) continue;
            for (NSDictionary* asset in release[@"assets"]) if ([asset[@"name"] isEqualToString:@"SuperRecording-macOS-universal.dmg"] &&
                (!version || [candidate compare:version options:NSNumericSearch]==NSOrderedDescending)) { newest=asset; version=candidate; }
        }
        dispatch_async(dispatch_get_main_queue(),^{
            self->_busy=NO;
            if (error || [(NSHTTPURLResponse*)response statusCode]!=200 || ![releases isKindOfClass:NSArray.class]) {
                sr::Alert(nil,@"暂时无法连接 GitHub，请稍后重试。"); return;
            }
            if (!newest) { sr::Alert(nil,@"目前没有更新的 Mac 试用包。Windows 版更新不会安装到此 Mac 上。"); return; }
            NSAlert* alert=[NSAlert new]; alert.messageText=[@"发现 Mac 新版本 " stringByAppendingString:version];
            alert.informativeText=@"下载后将打开安装包。退出旧版，再把新版本拖入“应用程序”替换即可。";
            [alert addButtonWithTitle:@"下载安装包"]; [alert addButtonWithTitle:@"取消"];
            if ([alert runModal]!=NSAlertFirstButtonReturn) return;
            NSURL* download=[NSURL URLWithString:newest[@"browser_download_url"]];
            if (![download.scheme isEqualToString:@"https"] || ![download.host isEqualToString:@"github.com"]) return;
            [self download:download];
        });
    }] resume];
}
- (void)download:(NSURL*)url {
    _busy=YES;
    [[[NSURLSession sharedSession] downloadTaskWithURL:url completionHandler:^(NSURL* temporary,NSURLResponse* response,NSError* error) {
        NSError* resultError=error;
        NSURL* directory=[NSFileManager.defaultManager URLsForDirectory:NSDownloadsDirectory inDomains:NSUserDomainMask].firstObject;
        NSURL* target=[directory URLByAppendingPathComponent:[NSString stringWithFormat:@"SuperRecording-Mac-%@.dmg",NSUUID.UUID.UUIDString]];
        if (!error && temporary && [(NSHTTPURLResponse*)response statusCode]==200)
            [NSFileManager.defaultManager moveItemAtURL:temporary toURL:target error:&resultError];
        else if (!error) resultError=sr::Error(@"下载失败，请重试。");
        dispatch_async(dispatch_get_main_queue(),^{
            self->_busy=NO;
            if (resultError) sr::Alert(nil,resultError.localizedDescription); else [NSWorkspace.sharedWorkspace openURL:target];
        });
    }] resume];
}
@end
