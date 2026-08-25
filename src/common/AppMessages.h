#pragma once

#include <windows.h>

namespace qrec::messages {

inline constexpr UINT TrayIcon = WM_APP + 1;
inline constexpr UINT CaptureStats = WM_APP + 2;
inline constexpr UINT CaptureStopped = WM_APP + 3;
inline constexpr UINT CaptureFailed = WM_APP + 4;
inline constexpr UINT EditorClosed = WM_APP + 5;
inline constexpr UINT ExportFinished = WM_APP + 6;
inline constexpr UINT ExistingInstance = WM_APP + 7;
inline constexpr UINT RequestExit = WM_APP + 8;
inline constexpr UINT CloseEditorAfterExport = WM_APP + 9;
inline constexpr UINT UpdateStatusChanged = WM_APP + 10;

}  // namespace qrec::messages
