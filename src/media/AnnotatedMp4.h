#pragma once
// Incremental annotation export: encode affected GOPs, copy all untouched H.264 samples.
#include "common/Types.h"
#include "media/Mp4BoundaryTrimmer.h"
#include <functional>
namespace qrec {
struct AnnotatedMp4Result final {
    Mp4BoundaryTrimOutcome outcome{Mp4BoundaryTrimOutcome::Unsupported};
    HRESULT nativeError{E_FAIL};
    std::wstring error;
    std::uint64_t encodedFrames{},copiedSamples{},cachedSegments{},encodedSegments{};
};
[[nodiscard]] AnnotatedMp4Result ExportAnnotatedMp4(const ExportRequest& request,
    const std::filesystem::path& output,std::stop_token stop,
    const std::function<void(const ExportProgress&)>& progress);
}
