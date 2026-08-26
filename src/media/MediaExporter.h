#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "common/Types.h"
#include "media/ExportArtifactCache.h"

#include <windows.h>

#include <atomic>
#include <filesystem>
#include <functional>
#include <mutex>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>

namespace qrec {

enum class MediaExportDisposition : std::uint8_t {
    None,
    Transcoded,
    AudioMuxed,
    CompressedRetimedPassthrough,
    BoundaryTrimmedHybrid,
    SmartTrimmedPassthrough,
    HardLinkedPassthrough,
    CopiedPassthrough,
    OriginalPassthrough,
    CachedArtifact,
};

struct MediaExportResult final {
    bool success{};
    bool cancelled{};
    std::filesystem::path outputPath;
    std::wstring errorMessage;
    MediaExportDisposition disposition{MediaExportDisposition::None};
    std::chrono::milliseconds elapsed{};
    bool cacheHit{};
    bool waitedForCacheBuilder{};
    MediaArtifactDelivery delivery{MediaArtifactDelivery::None};
    std::chrono::milliseconds cacheBuilderWait{};
    std::chrono::milliseconds cacheGeneration{};
    std::chrono::milliseconds deliveryElapsed{};
    std::wstring cacheKey;
    std::wstring diagnosticSummary;
    std::uint64_t sourceBytes{};
    std::uint64_t outputBytes{};
    HRESULT nativeError{E_FAIL};
};

class MediaExporter final {
public:
    using ProgressCallback = std::function<void(const ExportProgress&)>;
    using CompletionCallback = std::function<void(const MediaExportResult&)>;

    MediaExporter() = default;
    ~MediaExporter();

    MediaExporter(const MediaExporter&) = delete;
    MediaExporter& operator=(const MediaExporter&) = delete;

    // 回调从工作线程发出；调用方必须自行切换到 UI 线程。
    [[nodiscard]] bool StartExport(
        ExportRequest request,
        ProgressCallback progress,
        CompletionCallback completed);
    void Cancel() noexcept;
    void CancelAndWait() noexcept;
    [[nodiscard]] bool IsRunning() const noexcept;

    // Synchronously prepares a trimmed MP4 or GIF in the shared artifact
    // cache. Call this from a cancellable background thread after the editor's
    // trim/format selection has stabilized. destinationPath is ignored.
    [[nodiscard]] static MediaExportResult WarmCache(
        const ExportRequest& request,
        const ProgressCallback& progress,
        std::stop_token stopToken);

    [[nodiscard]] static std::wstring_view DispositionName(
        MediaExportDisposition disposition) noexcept;
    [[nodiscard]] static std::wstring_view DeliveryName(
        MediaArtifactDelivery delivery) noexcept;

    // Whole-range MP4 can be handed off byte-for-byte. Clipboard callers may
    // put recording.sourcePath on CF_HDROP immediately; StartExport creates an
    // O(1) hard link when source and destination share a volume and otherwise
    // falls back to an asynchronous file copy.
    [[nodiscard]] static bool CanUsePassthrough(
        const ExportRequest& request) noexcept;

    // 使用 CF_HDROP 复制实际导出文件，资源管理器和聊天软件可直接粘贴。
    [[nodiscard]] static bool CopyFileToClipboard(
        HWND owner,
        const std::filesystem::path& filePath,
        std::wstring* errorMessage = nullptr);

private:
    static MediaExportResult RunExport(
        const ExportRequest& request,
        const ProgressCallback& progress,
        std::stop_token stopToken);

    mutable std::mutex workerMutex_;
    std::jthread worker_;
    std::atomic_bool running_{false};
    std::atomic_uint64_t workerGeneration_{0};
};

}  // namespace qrec
