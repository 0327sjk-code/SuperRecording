#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "common/Types.h"

#include <windows.h>

namespace qrec {

enum class EditorStatusTone : unsigned char {
    Neutral,
    Progress,
    Success,
    Error,
};

enum class EditorButtonRole : unsigned char {
    Play,
    TrimStart,
    TrimEnd,
    SegmentLeft,
    SegmentRight,
    Primary,
    Secondary,
};

struct EditorButtonPaintState final {
    EditorButtonRole role{EditorButtonRole::Secondary};
    bool selected{};
    bool playing{};
    bool busy{};
};

struct EditorChromeLayout final {
    RECT headerTitle{};
    RECT headerSubtitle{};
    RECT previewStage{};
    RECT preview{};
    RECT rangeLabel{};
    RECT speedControl{};
    RECT trimStartButton{};
    RECT trimEndButton{};
    RECT timeline{};
    RECT playButton{};
    RECT timeLabel{};
    RECT audioToggle{};
    RECT formatLabel{};
    RECT mp4Button{};
    RECT gifButton{};
    RECT copyButton{};
    RECT saveButton{};
    RECT statusLabel{};
    POINT statusDotCenter{};
    int editorPanelTop{};
    int previewRadius{};
};

class EditorChrome final {
public:
    EditorChrome() = default;
    ~EditorChrome();

    EditorChrome(const EditorChrome&) = delete;
    EditorChrome& operator=(const EditorChrome&) = delete;

    [[nodiscard]] bool Initialize(HWND window);
    void RecreateFonts(HWND window);
    void Destroy() noexcept;
    void AttachButton(HWND button);

    [[nodiscard]] EditorChromeLayout CalculateLayout(
        HWND window,
        int width,
        int height,
        const RecordingResult& recording) const;
    void ApplyPreviewRegion(HWND preview, const EditorChromeLayout& layout) const;
    void PaintWindow(
        HWND window,
        int editorPanelTop,
        const RECT& previewStage,
        POINT statusDotCenter,
        EditorStatusTone statusTone);
    [[nodiscard]] bool DrawButton(
        HWND owner,
        const DRAWITEMSTRUCT* item,
        const EditorButtonPaintState& state) const;

    [[nodiscard]] COLORREF StatusColor(EditorStatusTone tone) const noexcept;
    [[nodiscard]] HBRUSH PanelBrush() const noexcept { return panelBrush_; }
    [[nodiscard]] HBRUSH VideoBrush() const noexcept { return videoBrush_; }
    [[nodiscard]] HFONT RegularFont() const noexcept { return regularFont_; }
    [[nodiscard]] HFONT StrongFont() const noexcept { return strongFont_; }
    [[nodiscard]] HFONT TitleFont() const noexcept { return titleFont_; }
    [[nodiscard]] HFONT CaptionFont() const noexcept { return captionFont_; }
    [[nodiscard]] HFONT TimeFont() const noexcept { return timeFont_; }

private:
    static LRESULT CALLBACK ButtonSubclassProc(
        HWND control,
        UINT message,
        WPARAM wParam,
        LPARAM lParam,
        UINT_PTR subclassId,
        DWORD_PTR referenceData);
    static int Scale(HWND window, int value) noexcept;
    void DeleteFonts() noexcept;
    [[nodiscard]] bool EnsureBackBuffer(HDC target, int width, int height) noexcept;
    void DeleteBackBuffer() noexcept;

    HFONT regularFont_{};
    HFONT strongFont_{};
    HFONT titleFont_{};
    HFONT captionFont_{};
    HFONT timeFont_{};
    HBRUSH workspaceBrush_{};
    HBRUSH panelBrush_{};
    HBRUSH videoBrush_{};
    HDC backBufferDc_{};
    HBITMAP backBufferBitmap_{};
    HGDIOBJ backBufferPreviousBitmap_{};
    int backBufferWidth_{};
    int backBufferHeight_{};
};

}  // namespace qrec
