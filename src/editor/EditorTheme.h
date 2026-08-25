#pragma once

#include <windows.h>

namespace qrec::editor_theme {

// Three-level dark surface system. These tokens are intentionally editor-only
// so the selector and recording overlay retain their existing appearance.
inline constexpr COLORREF Canvas = RGB(18, 19, 21);
inline constexpr COLORREF Panel = RGB(25, 27, 30);
inline constexpr COLORREF Control = RGB(36, 39, 43);
inline constexpr COLORREF ControlHover = RGB(44, 48, 53);
inline constexpr COLORREF ControlPressed = RGB(31, 33, 36);
inline constexpr COLORREF ControlDisabled = RGB(30, 32, 35);

inline constexpr COLORREF TextPrimary = RGB(244, 244, 242);
inline constexpr COLORREF TextSecondary = RGB(184, 187, 192);
inline constexpr COLORREF TextDisabled = RGB(116, 120, 126);
inline constexpr COLORREF White = RGB(255, 255, 255);

inline constexpr COLORREF Border = RGB(55, 59, 64);
inline constexpr COLORREF BorderSubtle = RGB(43, 46, 50);
inline constexpr COLORREF BorderHover = RGB(84, 90, 98);
inline constexpr COLORREF BorderStrong = RGB(105, 112, 121);
inline constexpr COLORREF BorderDisabled = RGB(43, 46, 50);

// Green is reserved for the save action, MP4 selection, and success feedback.
inline constexpr COLORREF Save = RGB(47, 116, 66);
inline constexpr COLORREF SaveHover = RGB(52, 125, 72);
inline constexpr COLORREF SavePressed = RGB(40, 95, 56);
inline constexpr COLORREF Mp4Selected = RGB(24, 53, 34);
inline constexpr COLORREF Mp4SelectedHover = RGB(29, 65, 41);
inline constexpr COLORREF Mp4SelectedPressed = RGB(20, 43, 28);
inline constexpr COLORREF Mp4SelectedBorder = RGB(77, 168, 100);
inline constexpr COLORREF Mp4SelectedText = RGB(205, 239, 213);
inline constexpr COLORREF Success = RGB(103, 190, 126);

// Neutral cool selection is used for GIF and timeline editing states.
inline constexpr COLORREF Selection = RGB(48, 58, 70);
inline constexpr COLORREF SelectionHover = RGB(57, 69, 83);
inline constexpr COLORREF SelectionPressed = RGB(39, 47, 57);
inline constexpr COLORREF SelectionBorder = RGB(120, 134, 151);
inline constexpr COLORREF SelectionText = RGB(231, 235, 239);

inline constexpr COLORREF Focus = RGB(101, 177, 220);
inline constexpr COLORREF FocusPressed = RGB(78, 145, 184);
inline constexpr COLORREF Progress = RGB(121, 169, 200);

inline constexpr COLORREF Danger = RGB(199, 47, 45);
inline constexpr COLORREF DangerHover = RGB(176, 39, 37);
inline constexpr COLORREF DangerPressed = RGB(151, 33, 31);
inline constexpr COLORREF DangerText = RGB(255, 180, 175);

inline constexpr COLORREF VideoStage = RGB(6, 7, 8);
inline constexpr COLORREF VideoStageBorder = RGB(47, 50, 55);

inline constexpr COLORREF TimelineTrack = RGB(37, 40, 44);
inline constexpr COLORREF TimelineSelected = RGB(48, 60, 73);
inline constexpr COLORREF TimelineSelectedBorder = RGB(115, 135, 158);
inline constexpr COLORREF TimelineTick = RGB(82, 98, 114);
inline constexpr COLORREF TimelineHandle = RGB(104, 116, 128);
inline constexpr COLORREF TimelineHandleHover = RGB(119, 135, 152);
inline constexpr COLORREF TimelineHandlePressed = RGB(96, 122, 148);
inline constexpr COLORREF TimelineHandleGrip = RGB(18, 19, 21);
inline constexpr COLORREF TimelinePlayhead = RGB(231, 234, 237);
inline constexpr COLORREF TimelinePlayheadHalo = RGB(18, 19, 21);

inline constexpr COLORREF TitleBar = RGB(25, 27, 30);
inline constexpr COLORREF TitleBarText = TextPrimary;
inline constexpr COLORREF TitleBarBorder = BorderSubtle;

}  // namespace qrec::editor_theme
