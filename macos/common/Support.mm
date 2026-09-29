#import "common/Support.h"
#include <copyfile.h>
#include <sys/clonefile.h>
#include <cerrno>
#include <cmath>
namespace sr {
NSColor* Color(unsigned rgb, double alpha) {
    return [NSColor colorWithSRGBRed:((rgb>>16)&255)/255.0 green:((rgb>>8)&255)/255.0
                              blue:(rgb&255)/255.0 alpha:alpha];
}
NSError* Error(NSString* message) {
    return [NSError errorWithDomain:@"SuperRecording" code:1
                          userInfo:@{NSLocalizedDescriptionKey:message ?: @"操作失败"}];
}
NSURL* CacheDirectory() {
    NSURL* root = [[NSFileManager defaultManager] URLsForDirectory:NSApplicationSupportDirectory
                                                         inDomains:NSUserDomainMask].firstObject;
    NSURL* url = [root URLByAppendingPathComponent:@"SuperRecording/Recordings" isDirectory:YES];
    [[NSFileManager defaultManager] createDirectoryAtURL:url withIntermediateDirectories:YES attributes:nil error:nil];
    return url;
}
NSURL* UniqueDirectory(NSString* prefix) {
    NSURL* url = [CacheDirectory() URLByAppendingPathComponent:
        [NSString stringWithFormat:@"%@-%@", prefix, NSUUID.UUID.UUIDString] isDirectory:YES];
    NSError* error = nil;
    if (![[NSFileManager defaultManager] createDirectoryAtURL:url withIntermediateDirectories:YES attributes:nil error:&error])
        return nil;
    return url;
}
NSString* TimeLabel(double seconds) {
    if (!std::isfinite(seconds)) seconds=0;
    long ticks=lround(fmax(0,seconds)*100);
    return [NSString stringWithFormat:@"%02ld:%02ld.%02ld",ticks/6000,(ticks/100)%60,ticks%100];
}
CMTime Time(double seconds) { return CMTimeMakeWithSeconds(seconds, TimeScale); }
NSButton* Button(NSString* title, NSString* symbol, id target, SEL action) {
    NSButton* button=[NSButton buttonWithTitle:title target:target action:action];
    button.bezelStyle=NSBezelStyleRounded;
    button.font=[NSFont systemFontOfSize:12 weight:NSFontWeightMedium];
    if (symbol.length) {
        button.image=[NSImage imageWithSystemSymbolName:symbol accessibilityDescription:title];
        button.imagePosition=title.length ? NSImageLeft : NSImageOnly;
    }
    button.toolTip=title;
    [button setAccessibilityLabel:title];
    return button;
}
NSTextField* Label(NSString* value, CGFloat size, bool secondary) {
    NSTextField* label=[NSTextField labelWithString:value];
    label.font=[NSFont systemFontOfSize:size];
    label.textColor=Color(secondary ? 0xB8BBC0 : 0xF4F4F2);
    label.lineBreakMode=NSLineBreakByTruncatingTail;
    return label;
}
void Surface(NSView* view, unsigned rgb, CGFloat cornerRadius) {
    view.wantsLayer=YES; view.layer.backgroundColor=Color(rgb).CGColor;
    view.layer.cornerRadius=cornerRadius;
}
void Alert(NSWindow* parent, NSString* message) {
    NSAlert* alert=[NSAlert new]; alert.messageText=@"SuperRecording";
    alert.informativeText=message; [alert addButtonWithTitle:@"好"];
    if (parent) [alert beginSheetModalForWindow:parent completionHandler:nil];
    else [alert runModal];
}
void Log(NSString* event, NSString* detail) {
    NSLog(@"[SuperRecording] %@: %@",event,detail);
    static dispatch_queue_t queue=dispatch_queue_create("com.superrecording.log",DISPATCH_QUEUE_SERIAL);
    dispatch_async(queue, ^{
        NSURL* url=[CacheDirectory().URLByDeletingLastPathComponent URLByAppendingPathComponent:@"diagnostics.log"];
        NSFileManager* manager=NSFileManager.defaultManager;
        if (![manager fileExistsAtPath:url.path]) [manager createFileAtPath:url.path contents:nil attributes:nil];
        NSFileHandle* file=[NSFileHandle fileHandleForWritingToURL:url error:nil];
        @try {
            [file seekToEndOfFile];
            NSString* line=[NSString stringWithFormat:@"%@ %@: %@\n",NSDate.date,event,detail];
            [file writeData:[line dataUsingEncoding:NSUTF8StringEncoding]]; [file closeFile];
        } @catch (NSException* exception) { (void)exception; }
    });
}
bool DeliverFile(NSURL* source, NSURL* destination, NSError** error) {
    if ([source.path isEqualToString:destination.path]) return true;
    // APFS clone is nearly constant time. Copy fallback is staged, then renamed atomically.
    NSURL* staged=[destination.URLByDeletingLastPathComponent URLByAppendingPathComponent:
        [NSString stringWithFormat:@".sr-%@.partial",NSUUID.UUID.UUIDString]];
    if (clonefile(source.fileSystemRepresentation, staged.fileSystemRepresentation, 0)!=0 &&
        copyfile(source.fileSystemRepresentation, staged.fileSystemRepresentation, nullptr, COPYFILE_DATA|COPYFILE_EXCL)!=0) {
        if (error) *error=[NSError errorWithDomain:NSPOSIXErrorDomain code:errno userInfo:nil];
        [[NSFileManager defaultManager] removeItemAtURL:staged error:nil]; return false;
    }
    if (rename(staged.fileSystemRepresentation,destination.fileSystemRepresentation)!=0) {
        if (error) *error=[NSError errorWithDomain:NSPOSIXErrorDomain code:errno userInfo:nil];
        [[NSFileManager defaultManager] removeItemAtURL:staged error:nil]; return false;
    }
    return true;
}
std::wstring Wide(NSString* value) {
    NSData* bytes=[value dataUsingEncoding:NSUTF32LittleEndianStringEncoding];
    return bytes.length ? std::wstring(static_cast<const wchar_t*>(bytes.bytes),bytes.length/sizeof(wchar_t)) : std::wstring{};
}
NSString* String(const std::wstring& value) {
    return [[NSString alloc] initWithBytes:value.data() length:value.size()*sizeof(wchar_t)
                                encoding:NSUTF32LittleEndianStringEncoding] ?: @"";
}
}
