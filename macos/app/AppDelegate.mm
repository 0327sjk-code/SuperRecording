#import "app/AppDelegate.h"
#import "app/Preferences.h"
#import "app/HotKey.h"
#import "app/UpdateChecker.h"
#import "capture/RegionSelector.h"
#import "capture/RecordingHUD.h"
#import "capture/Recorder.h"
#import "editor/EditorController.h"
#import "common/Support.h"
#import <ServiceManagement/ServiceManagement.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#import <Carbon/Carbon.h>
@implementation SRAppDelegate {
    NSStatusItem* _statusItem;
    NSMenu* _menu;
    SRPreferences* _preferences;
    SRHotKey* _hotKey;
    SRUpdateChecker* _updater;
    SRRegionSelector* _selector;
    SRRecorder* _recorder;
    SRRecordingHUD* _hud;
    NSTimer* _timer;
    NSMutableArray<SREditorController*>* _editors;
    BOOL _finishing;
}
- (void)applicationDidFinishLaunching:(NSNotification*)notification {
    _preferences=[SRPreferences new]; _updater=[SRUpdateChecker new]; _editors=[NSMutableArray new];
    NSApp.appearance=[NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
    NSMenu* main=[NSMenu new]; NSMenuItem* application=[NSMenuItem new]; [main addItem:application];
    application.submenu=[NSMenu new];
    [application.submenu addItemWithTitle:@"退出 SuperRecording" action:@selector(terminate:) keyEquivalent:@"q"];
    NSMenuItem* edit=[NSMenuItem new]; edit.title=@"编辑"; edit.submenu=[NSMenu new]; [main addItem:edit];
    for (NSArray* entry in @[@[@"剪切",@"cut:",@"x"],@[@"复制",@"copy:",@"c"],@[@"粘贴",@"paste:",@"v"],@[@"全选",@"selectAll:",@"a"]])
        [edit.submenu addItemWithTitle:entry[0] action:NSSelectorFromString(entry[1]) keyEquivalent:entry[2]];
    NSApp.mainMenu=main;
    _statusItem=[NSStatusBar.systemStatusBar statusItemWithLength:NSVariableStatusItemLength];
    _statusItem.button.image=[NSImage imageWithSystemSymbolName:@"record.circle" accessibilityDescription:@"SuperRecording"];
    _statusItem.button.toolTip=@"SuperRecording · 单击录屏，右键设置";
    _statusItem.button.target=self; _statusItem.button.action=@selector(statusClicked:);
    [_statusItem.button sendActionOn:NSEventMaskLeftMouseUp|NSEventMaskRightMouseUp];
    _menu=[NSMenu new]; _menu.delegate=self;
    _hotKey=[SRHotKey new]; __weak SRAppDelegate* weakSelf=self;
    _hotKey.pressed=^{ [weakSelf startRecording:nil]; };
    if (![_hotKey registerCode:(UInt32)_preferences.hotKeyCode modifiers:(UInt32)_preferences.hotKeyModifiers])
        sr::Log(@"hotkey",@"Shortcut in use; change it from the menu bar. F3 can be reserved by Mission Control.");
    if (![NSUserDefaults.standardUserDefaults boolForKey:@"firstLaunchDone"]) {
        [NSUserDefaults.standardUserDefaults setBool:YES forKey:@"firstLaunchDone"];
        if ([NSBundle.mainBundle.bundlePath hasPrefix:@"/Applications/"]) [SMAppService.mainAppService registerAndReturnError:nil];
        NSAlert* welcome=[NSAlert new]; welcome.messageText=@"SuperRecording · Mac 试用版";
        welcome.informativeText=@"入口已放在顶部菜单栏。单击开始框选，右键设置帧率、快捷键和保存目录。\n\n首次录屏需要授权“屏幕录制”；授权后若没有画面，请退出并重新打开。系统声音会单独暂存，导出默认关闭。";
        [welcome addButtonWithTitle:@"开始使用"]; [NSApp activateIgnoringOtherApps:YES]; [welcome runModal];
    }
    sr::Log(@"launch",[NSString stringWithFormat:@"macOS %@ · %@",NSProcessInfo.processInfo.operatingSystemVersionString,
                      NSBundle.mainBundle.bundlePath]);
}
- (void)statusClicked:(id)sender {
    if (NSApp.currentEvent.type==NSEventTypeRightMouseUp) {
        [self menuNeedsUpdate:_menu]; [_statusItem popUpStatusItemMenu:_menu];
    } else [self startRecording:nil];
}
- (NSMenuItem*)item:(NSString*)title action:(SEL)action {
    NSMenuItem* item=[_menu addItemWithTitle:title action:action keyEquivalent:@""]; item.target=self; return item;
}
- (void)menuNeedsUpdate:(NSMenu*)menu {
    [_menu removeAllItems];
    [self item:_recorder?@"结束录制":@"开始录制" action:@selector(startRecording:)];
    if (_recorder) [self item:_recorder.paused?@"继续录制":@"暂停录制" action:@selector(pauseRecording:)];
    [_menu addItem:NSMenuItem.separatorItem];
    for (NSNumber* fps in @[@30,@60]) {
        NSMenuItem* item=[self item:[NSString stringWithFormat:@"%@ FPS",fps] action:@selector(changeFPS:)];
        item.tag=fps.integerValue; item.state=_preferences.fps==fps.integerValue?NSControlStateValueOn:NSControlStateValueOff;
    }
    [self item:[@"录制快捷键：" stringByAppendingString:_preferences.hotKeyLabel] action:@selector(configureHotKey:)];
    NSMenuItem* adjust=[self item:@"录制前调整选区" action:@selector(toggleAdjustment:)]; adjust.state=_preferences.adjustSelection;
    NSMenuItem* keep=[self item:@"复制或保存后保留编辑窗口" action:@selector(toggleKeep:)]; keep.state=_preferences.keepEditor;
    NSMenuItem* login=[self item:@"登录时自动启动" action:@selector(toggleLogin:)];
    login.state=SMAppService.mainAppService.status==SMAppServiceStatusEnabled;
    [self item:@"设置保存目录…" action:@selector(chooseDirectory:)];
    [self item:@"打开录制文件…" action:@selector(openRecording:)];
    [self item:@"打开录制缓存与日志" action:@selector(openCache:)];
    [_menu addItem:NSMenuItem.separatorItem];
    [self item:@"检查 Mac 试用版更新…" action:@selector(checkUpdates:)];
    NSMenuItem* version=[self item:@"Mac 试用版 1.7.0.1" action:nil]; version.enabled=NO;
    [self item:@"退出" action:@selector(quit:)];
}
- (void)startRecording:(id)sender {
    if (_finishing) return;
    if (_recorder) { [self finishRecording]; return; }
    if (_selector) { [_selector close]; _selector=nil; return; }
    if (!CGPreflightScreenCaptureAccess()) {
        BOOL allowed=CGRequestScreenCaptureAccess();
        if (!allowed) { sr::Alert(nil,@"请在“系统设置 → 隐私与安全性 → 屏幕录制”允许 SuperRecording，然后退出并重新打开软件。"); return; }
    }
    _selector=[SRRegionSelector new]; __weak SRAppDelegate* weakSelf=self;
    [_selector beginWithAdjustment:_preferences.adjustSelection completion:^(SRRegionSelection* selection) {
        SRAppDelegate* owner=weakSelf; if (!owner) return;
        if (!selection) { owner->_selector=nil; return; }
        owner->_recorder=[SRRecorder new]; owner->_finishing=YES;
        owner->_recorder.failure=^(NSError* error) { SRAppDelegate* current=weakSelf; if (current && !current->_finishing) [current finishRecording]; };
        [owner->_recorder start:selection fps:owner->_preferences.fps completion:^(NSError* error) {
            SRAppDelegate* current=weakSelf; if (!current) return; current->_finishing=NO;
            if (error) { [current->_selector close]; current->_selector=nil; current->_recorder=nil; sr::Alert(nil,error.localizedDescription); return; }
            current->_hud=[[SRRecordingHUD alloc] initWithSelection:selection];
            current->_hud.pauseAction=^{ [weakSelf pauseRecording:nil]; };
            current->_hud.stopAction=^{ [weakSelf finishRecording]; };
            [current->_hud.window orderFrontRegardless];
            current->_timer=[NSTimer scheduledTimerWithTimeInterval:0.1 repeats:YES block:^(NSTimer* timer) {
                SRAppDelegate* live=weakSelf; if (live) [live->_hud updateTime:live->_recorder.recordedSeconds paused:live->_recorder.paused];
            }];
            current->_statusItem.button.contentTintColor=sr::Color(0xFF595E);
        }];
    }];
}
- (void)pauseRecording:(id)sender { [_recorder togglePause]; }
- (void)finishRecording {
    if (!_recorder || _finishing) return; _finishing=YES; [_hud setFinishing];
    [_timer invalidate]; _timer=nil;
    [_recorder stop:^(SRRecording* recording,NSError* error) {
        [self->_selector close]; self->_selector=nil; [self->_hud close]; self->_hud=nil;
        self->_recorder=nil; self->_finishing=NO; self->_statusItem.button.contentTintColor=nil;
        if (error) sr::Alert(nil,error.localizedDescription); else [self edit:recording];
    }];
}
- (void)edit:(SRRecording*)recording {
    SREditorController* editor=[[SREditorController alloc] initWithRecording:recording preferences:_preferences];
    [_editors addObject:editor]; __weak SRAppDelegate* weakSelf=self; __weak SREditorController* weakEditor=editor;
    editor.closed=^{ SRAppDelegate* owner=weakSelf; if (owner && weakEditor) [owner->_editors removeObject:weakEditor]; };
    [NSApp activateIgnoringOtherApps:YES]; [editor showWindow:nil]; [editor.window makeKeyAndOrderFront:nil];
}
- (void)changeFPS:(NSMenuItem*)sender { _preferences.fps=sender.tag; }
- (void)toggleAdjustment:(id)sender { _preferences.adjustSelection=!_preferences.adjustSelection; }
- (void)toggleKeep:(id)sender { _preferences.keepEditor=!_preferences.keepEditor; }
- (void)toggleLogin:(id)sender {
    NSError* error=nil;
    if (SMAppService.mainAppService.status==SMAppServiceStatusEnabled) [SMAppService.mainAppService unregisterAndReturnError:&error];
    else [SMAppService.mainAppService registerAndReturnError:&error];
    if (error) sr::Alert(nil,[@"请先把软件拖入“应用程序”。登录项设置失败：" stringByAppendingString:error.localizedDescription]);
}
- (void)chooseDirectory:(id)sender {
    NSOpenPanel* panel=[NSOpenPanel openPanel]; panel.canChooseDirectories=YES; panel.canChooseFiles=NO;
    panel.canCreateDirectories=YES; panel.directoryURL=_preferences.saveDirectory; panel.prompt=@"使用此目录";
    if ([panel runModal]==NSModalResponseOK) _preferences.saveDirectory=panel.URL;
}
- (void)openCache:(id)sender { [NSWorkspace.sharedWorkspace openURL:sr::CacheDirectory().URLByDeletingLastPathComponent]; }
- (void)openRecording:(id)sender {
    NSOpenPanel* panel=[NSOpenPanel openPanel]; panel.allowedContentTypes=@[UTTypeMPEG4Movie];
    panel.directoryURL=_preferences.saveDirectory;
    if ([panel runModal]!=NSModalResponseOK) return;
    AVURLAsset* asset=[AVURLAsset URLAssetWithURL:panel.URL options:nil];
    AVAssetTrack* video=[asset tracksWithMediaType:AVMediaTypeVideo].firstObject;
    if (!video) { sr::Alert(nil,@"无法读取此视频。"); return; }
    SRRecording* recording=[SRRecording new]; recording.videoURL=panel.URL; recording.size=video.naturalSize;
    recording.fps=video.nominalFrameRate>45?60:30;
    NSURL* sidecar=[panel.URL.URLByDeletingLastPathComponent URLByAppendingPathComponent:@"system-audio.m4a"];
    if ([NSFileManager.defaultManager fileExistsAtPath:sidecar.path]) recording.audioURL=sidecar;
    else if ([asset tracksWithMediaType:AVMediaTypeAudio].count) recording.audioURL=panel.URL;
    [self edit:recording];
}
- (void)configureHotKey:(id)sender {
    NSAlert* alert=[NSAlert new]; alert.messageText=@"设置录制快捷键";
    alert.informativeText=@"按下新的组合键。建议使用 ⌃⌥R，避开系统的 F3 调度中心。普通字母需要搭配 ⌘ / ⌃ / ⌥。";
    NSTextField* field=sr::Label(_preferences.hotKeyLabel,20); field.frame=NSMakeRect(0,0,320,40); alert.accessoryView=field;
    [alert addButtonWithTitle:@"保存"]; [alert addButtonWithTitle:@"取消"];
    __block UInt32 code=(UInt32)_preferences.hotKeyCode,modifiers=(UInt32)_preferences.hotKeyModifiers;
    __block BOOL valid=YES; __block NSString* name=_preferences.hotKeyLabel;
    id monitor=[NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown handler:^NSEvent*(NSEvent* event) {
        if (event.window!=alert.window || event.keyCode==53) return event;
        modifiers=0; NSMutableString* label=[NSMutableString new];
        if (event.modifierFlags&NSEventModifierFlagControl) { modifiers|=controlKey; [label appendString:@"⌃"]; }
        if (event.modifierFlags&NSEventModifierFlagOption) { modifiers|=optionKey; [label appendString:@"⌥"]; }
        if (event.modifierFlags&NSEventModifierFlagShift) { modifiers|=shiftKey; [label appendString:@"⇧"]; }
        if (event.modifierFlags&NSEventModifierFlagCommand) { modifiers|=cmdKey; [label appendString:@"⌘"]; }
        NSString* character=event.charactersIgnoringModifiers ?: @"";
        unichar c=character.length?[character characterAtIndex:0]:0;
        BOOL function=c>=NSF1FunctionKey && c<=NSF20FunctionKey;
        valid=function || (character.length && (modifiers&(controlKey|optionKey|cmdKey)));
        if (function) [label appendFormat:@"F%d",c-NSF1FunctionKey+1]; else [label appendString:character.uppercaseString];
        name=label; code=event.keyCode; field.stringValue=valid?name:@"请增加组合键"; return nil;
    }];
    NSModalResponse response=[alert runModal]; [NSEvent removeMonitor:monitor];
    if (response!=NSAlertFirstButtonReturn) return;
    if (!valid || ![_hotKey registerCode:code modifiers:modifiers]) { sr::Alert(nil,@"此快捷键不可用或已被占用，原设置未变。"); return; }
    _preferences.hotKeyCode=code; _preferences.hotKeyModifiers=modifiers; _preferences.hotKeyLabel=name;
}
- (void)checkUpdates:(id)sender { [_updater check]; }
- (void)quit:(id)sender { [NSApp terminate:nil]; }
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication*)sender {
    if (_recorder || _finishing) { sr::Alert(nil,@"请先结束当前录制，再退出软件。"); return NSTerminateCancel; }
    if (_editors.count) {
        NSAlert* alert=[NSAlert new]; alert.messageText=@"退出 SuperRecording？";
        alert.informativeText=@"未导出的标注将不会保存。原始录制会保留在缓存目录中。";
        [alert addButtonWithTitle:@"取消"]; [alert addButtonWithTitle:@"退出"];
        if ([alert runModal]!=NSAlertSecondButtonReturn) return NSTerminateCancel;
    }
    return NSTerminateNow;
}
@end
