#pragma once

#include <windows.h>

namespace qrec::theme {

inline constexpr COLORREF Background = RGB(255, 255, 255);
inline constexpr COLORREF Workspace = RGB(246, 246, 245);
inline constexpr COLORREF Surface = RGB(255, 255, 255);
inline constexpr COLORREF SurfaceMuted = RGB(246, 246, 244);
inline constexpr COLORREF SurfaceHover = RGB(242, 242, 240);
inline constexpr COLORREF SurfacePressed = RGB(232, 232, 229);
inline constexpr COLORREF SurfaceDisabled = RGB(244, 244, 242);
inline constexpr COLORREF Ink = RGB(17, 17, 15);
inline constexpr COLORREF Muted = RGB(96, 96, 91);
inline constexpr COLORREF InkDisabled = RGB(153, 153, 147);
inline constexpr COLORREF Primary = RGB(47, 111, 62);
inline constexpr COLORREF PrimaryHover = RGB(40, 96, 52);
inline constexpr COLORREF PrimaryPressed = RGB(34, 82, 44);
inline constexpr COLORREF PrimarySubtle = RGB(233, 241, 234);
inline constexpr COLORREF PrimarySubtleHover = RGB(222, 235, 224);
inline constexpr COLORREF PrimarySubtlePressed = RGB(210, 226, 213);
inline constexpr COLORREF Danger = RGB(199, 47, 45);
inline constexpr COLORREF DangerHover = RGB(176, 39, 37);
inline constexpr COLORREF DangerPressed = RGB(151, 33, 31);
inline constexpr COLORREF DangerSubtle = RGB(249, 236, 234);
inline constexpr COLORREF DangerSubtleHover = RGB(246, 225, 222);
inline constexpr COLORREF DangerMuted = RGB(145, 64, 61);
inline constexpr COLORREF Accent = Danger;
inline constexpr COLORREF VideoStage = RGB(10, 10, 10);
inline constexpr COLORREF VideoStageBorder = RGB(43, 43, 40);
inline constexpr COLORREF Focus = RGB(25, 117, 160);
inline constexpr COLORREF Border = RGB(226, 226, 222);
inline constexpr COLORREF BorderSubtle = RGB(237, 237, 234);
inline constexpr COLORREF BorderHover = RGB(193, 193, 187);
inline constexpr COLORREF BorderStrong = RGB(174, 174, 168);
inline constexpr COLORREF TimelineOutside = RGB(232, 232, 228);
inline constexpr COLORREF TimelineTick = RGB(196, 211, 199);
inline constexpr COLORREF White = RGB(255, 255, 255);
inline constexpr COLORREF OverlayMask = RGB(18, 18, 18);
inline constexpr COLORREF OverlayPanel = RGB(28, 28, 27);
inline constexpr COLORREF OverlayPanelMuted = RGB(216, 216, 213);

inline constexpr int CornerSmall = 8;
inline constexpr int CornerMedium = 10;
inline constexpr int CornerLarge = 12;
inline constexpr int ControlHeight = 40;

// Codex-style native Windows stack: explicit CJK glyph ownership avoids GDI
// font-link ambiguity, while pure Latin/numeric runs keep Segoe UI metrics.
inline constexpr wchar_t FontFamily[] = L"Microsoft YaHei UI";
inline constexpr wchar_t TitleFontFamily[] = L"Microsoft YaHei UI";
inline constexpr wchar_t LatinFontFamily[] = L"Segoe UI";
inline constexpr BYTE FontQuality = CLEARTYPE_QUALITY;

}  // namespace qrec::theme
