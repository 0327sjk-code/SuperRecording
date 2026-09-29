#pragma once
// Shared AppKit styling, time conversion, atomic file delivery and diagnostics.
#import <AppKit/AppKit.h>
#import <AVFoundation/AVFoundation.h>
#include <string>
#include <cmath>
namespace sr {
inline constexpr double MinimumSelection = 24;
inline constexpr double SnapDistance = 12;
inline constexpr int TimeScale = 60000;
inline constexpr double ExportDebounce = 0.30;
inline constexpr double MaximumSpeed = 3.0;
inline constexpr double MinimumSpeed = 0.1;
inline constexpr double DefaultMarkSeconds = 0.5;
inline constexpr NSUInteger BuildNumber = 17001;
NSColor* Color(unsigned rgb, double alpha = 1);
NSError* Error(NSString* message);
NSURL* CacheDirectory();
NSURL* UniqueDirectory(NSString* prefix);
NSString* TimeLabel(double seconds);
CMTime Time(double seconds);
NSButton* Button(NSString* title, NSString* symbol, id target, SEL action);
NSTextField* Label(NSString* value, CGFloat size = 12, bool secondary = false);
void Surface(NSView* view, unsigned rgb, CGFloat cornerRadius = 0);
void Alert(NSWindow* parent, NSString* message);
void Log(NSString* event, NSString* detail);
bool DeliverFile(NSURL* source, NSURL* destination, NSError** error);
std::wstring Wide(NSString* value);
NSString* String(const std::wstring& value);
}
