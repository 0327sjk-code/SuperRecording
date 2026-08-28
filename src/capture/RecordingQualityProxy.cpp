#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "capture/RecordingQualityProxy.h"

#include "media/BgraFrameScaler.h"
#include "media/ExportQuality.h"
#include "media/Mp4Writer.h"

#include <windows.h>
#include <objbase.h>

#include <condition_variable>
#include <cstring>
#include <deque>
#include <future>
#include <limits>
#include <mutex>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace qrec::capture {
namespace {

constexpr std::size_t kMaximumQueuedFrames = 8;
constexpr std::uint32_t kProxyKeyframeIntervalMilliseconds = 100;

struct QueuedProxyFrame final {
    std::vector<std::uint8_t> pixels;
    std::uint32_t stride{};
    std::int64_t timestamp100Nanoseconds{};
    std::int64_t duration100Nanoseconds{};
};

void RemoveFileBestEffort(const std::filesystem::path& path) noexcept {
    if (path.empty()) {
        return;
    }
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

[[nodiscard]] bool IsNonEmptyFile(const std::filesystem::path& path) noexcept {
    std::error_code error;
    return std::filesystem::is_regular_file(path, error) && !error &&
        std::filesystem::file_size(path, error) > 0 && !error;
}

}  // namespace

struct RecordingQualityProxy::Impl final {
    mutable std::mutex mutex;
    std::condition_variable changed;
    std::deque<QueuedProxyFrame> queue;
    std::thread worker;
    RecordingQualityProxyConfig config;
    RecordingQualityProxyResult result;
    bool running{};
    bool accepting{};
    bool finishRequested{};
    bool cancelRequested{};
    bool producerFailure{};
    std::wstring producerFailureMessage;

    void Worker(std::promise<bool> startupPromise) noexcept {
        RecordingQualityProxyResult localResult{};
        localResult.attempted = true;
        localResult.outputPath = config.outputPath;
        localResult.outputWidth = config.outputWidth;
        localResult.outputHeight = config.outputHeight;
        localResult.qualityPercent = config.qualityPercent;

        const HRESULT comResult = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const bool comInitialized = SUCCEEDED(comResult);
        if (!comInitialized) {
            localResult.errorMessage = L"初始化实时画质代理 COM 环境失败。";
            localResult.nativeError = static_cast<long>(comResult);
            startupPromise.set_value(false);
            result = std::move(localResult);
            return;
        }
        struct CoUninitializeGuard final {
            ~CoUninitializeGuard() { ::CoUninitialize(); }
        };
        [[maybe_unused]] CoUninitializeGuard coUninitializeGuard;

        media::BgraFrameScaler scaler;
        HRESULT scalerResult = scaler.Initialize();
        if (FAILED(scalerResult)) {
            localResult.errorMessage = L"初始化实时画质缩放器失败。";
            localResult.nativeError = static_cast<long>(scalerResult);
            startupPromise.set_value(false);
            result = std::move(localResult);
            return;
        }

        media::Mp4Writer writer;
        media::Mp4WriterConfig writerConfig{};
        writerConfig.outputPath = config.outputPath;
        writerConfig.width = config.outputWidth;
        writerConfig.height = config.outputHeight;
        writerConfig.framesPerSecond = config.framesPerSecond;
        writerConfig.averageBitrate = media::ExportQuality::ComputeVideoBitrate(
            config.outputWidth,
            config.outputHeight,
            config.framesPerSecond,
            config.qualityPercent);
        writerConfig.preferHardwareEncoder = true;
        writerConfig.forcedKeyframeIntervalMilliseconds =
            kProxyKeyframeIntervalMilliseconds;
        std::wstring writerError;
        long writerNativeError = 0;
        if (!writer.Open(writerConfig, writerError, writerNativeError)) {
            localResult.errorMessage = writerError.empty()
                ? L"初始化实时画质代理编码器失败。"
                : std::move(writerError);
            localResult.nativeError = writerNativeError;
            startupPromise.set_value(false);
            result = std::move(localResult);
            return;
        }

        {
            std::scoped_lock lock(mutex);
            running = true;
            accepting = true;
        }
        startupPromise.set_value(true);

        std::vector<std::uint8_t> scaledPixels;
        bool failed = false;
        for (;;) {
            QueuedProxyFrame frame;
            {
                std::unique_lock lock(mutex);
                changed.wait(lock, [this] {
                    return finishRequested || cancelRequested ||
                        producerFailure || !queue.empty();
                });
                if (cancelRequested || producerFailure) {
                    failed = true;
                    if (producerFailure) {
                        localResult.errorMessage = producerFailureMessage;
                        localResult.nativeError = HRESULT_FROM_WIN32(ERROR_BUSY);
                    } else {
                        localResult.errorMessage = L"实时画质代理已取消。";
                        localResult.nativeError = HRESULT_FROM_WIN32(ERROR_CANCELLED);
                    }
                    queue.clear();
                    break;
                }
                if (queue.empty()) {
                    if (finishRequested) {
                        break;
                    }
                    continue;
                }
                frame = std::move(queue.front());
                queue.pop_front();
            }

            const HRESULT scaleResult = scaler.Scale(
                frame.pixels,
                config.sourceWidth,
                config.sourceHeight,
                frame.stride,
                config.outputWidth,
                config.outputHeight,
                &scaledPixels);
            if (FAILED(scaleResult)) {
                localResult.errorMessage = L"实时缩放录屏帧失败。";
                localResult.nativeError = static_cast<long>(scaleResult);
                failed = true;
                break;
            }
            if (!writer.WriteBgraFrame(
                    scaledPixels,
                    config.outputWidth * 4U,
                    frame.timestamp100Nanoseconds,
                    frame.duration100Nanoseconds,
                    writerError,
                    writerNativeError)) {
                localResult.errorMessage = writerError.empty()
                    ? L"写入实时画质代理帧失败。"
                    : std::move(writerError);
                localResult.nativeError = writerNativeError;
                failed = true;
                break;
            }
            ++localResult.encodedFrames;
        }

        std::wstring finalizeError;
        long finalizeNativeError = 0;
        const bool finalized = writer.Finalize(finalizeError, finalizeNativeError);
        if (!failed && !finalized) {
            localResult.errorMessage = finalizeError.empty()
                ? L"封装实时画质代理失败。"
                : std::move(finalizeError);
            localResult.nativeError = finalizeNativeError;
            failed = true;
        }
        localResult.success = !failed && localResult.encodedFrames != 0 &&
            IsNonEmptyFile(config.outputPath);
        if (!localResult.success) {
            if (localResult.errorMessage.empty()) {
                localResult.errorMessage = L"实时画质代理没有生成有效视频。";
                localResult.nativeError = E_FAIL;
            }
            RemoveFileBestEffort(config.outputPath);
        }

        {
            std::scoped_lock lock(mutex);
            accepting = false;
            running = false;
            result = std::move(localResult);
        }
        changed.notify_all();
    }
};

RecordingQualityProxy::RecordingQualityProxy()
    : impl_(std::make_unique<Impl>()) {}

RecordingQualityProxy::~RecordingQualityProxy() {
    Cancel();
}

bool RecordingQualityProxy::Start(
    const RecordingQualityProxyConfig& config,
    std::wstring* const errorMessage) noexcept {
    if (errorMessage != nullptr) {
        errorMessage->clear();
    }
    if (config.outputPath.empty() || config.sourceWidth < 16 ||
        config.sourceHeight < 16 || config.outputWidth < 16 ||
        config.outputHeight < 16 || config.outputWidth % 2U != 0 ||
        config.outputHeight % 2U != 0 ||
        (config.framesPerSecond != 30 && config.framesPerSecond != 60) ||
        !media::ExportQuality::IsValid(config.qualityPercent)) {
        if (errorMessage != nullptr) {
            *errorMessage = L"实时画质代理参数无效。";
        }
        return false;
    }

    {
        std::scoped_lock lock(impl_->mutex);
        if (impl_->worker.joinable() || impl_->running || impl_->accepting) {
            if (errorMessage != nullptr) {
                *errorMessage = L"实时画质代理已经运行。";
            }
            return false;
        }
        impl_->config = config;
        impl_->result = {};
        impl_->queue.clear();
        impl_->finishRequested = false;
        impl_->cancelRequested = false;
        impl_->producerFailure = false;
        impl_->producerFailureMessage.clear();
    }
    RemoveFileBestEffort(config.outputPath);

    std::promise<bool> startupPromise;
    std::future<bool> startupFuture = startupPromise.get_future();
    try {
        impl_->worker = std::thread(
            [implementation = impl_.get(),
             promise = std::move(startupPromise)]() mutable {
                implementation->Worker(std::move(promise));
            });
    } catch (...) {
        if (errorMessage != nullptr) {
            *errorMessage = L"创建实时画质代理线程失败。";
        }
        return false;
    }

    bool started = false;
    try {
        started = startupFuture.get();
    } catch (...) {
        started = false;
    }
    if (!started) {
        if (impl_->worker.joinable()) {
            impl_->worker.join();
        }
        if (errorMessage != nullptr) {
            *errorMessage = impl_->result.errorMessage.empty()
                ? L"实时画质代理启动失败。"
                : impl_->result.errorMessage;
        }
    }
    return started;
}

bool RecordingQualityProxy::SubmitFrame(
    const std::span<const std::uint8_t> bgra,
    const std::uint32_t sourceStride,
    const std::int64_t timestamp100Nanoseconds,
    const std::int64_t duration100Nanoseconds) noexcept {
    try {
        const std::uint64_t packedStride =
            static_cast<std::uint64_t>(impl_->config.sourceWidth) * 4U;
        const std::uint64_t minimumBytes =
            static_cast<std::uint64_t>(sourceStride) *
                (impl_->config.sourceHeight - 1U) +
            packedStride;
        if (sourceStride < packedStride || bgra.size() < minimumBytes ||
            packedStride > std::numeric_limits<std::size_t>::max() ||
            packedStride * impl_->config.sourceHeight >
                std::numeric_limits<std::size_t>::max()) {
            return false;
        }

        QueuedProxyFrame frame;
        frame.stride = static_cast<std::uint32_t>(packedStride);
        frame.timestamp100Nanoseconds = timestamp100Nanoseconds;
        frame.duration100Nanoseconds = duration100Nanoseconds;
        frame.pixels.resize(
            static_cast<std::size_t>(packedStride) * impl_->config.sourceHeight);
        for (std::uint32_t row = 0; row < impl_->config.sourceHeight; ++row) {
            std::memcpy(
                frame.pixels.data() +
                    static_cast<std::size_t>(row) * packedStride,
                bgra.data() + static_cast<std::size_t>(row) * sourceStride,
                static_cast<std::size_t>(packedStride));
        }

        {
            std::scoped_lock lock(impl_->mutex);
            if (!impl_->accepting || impl_->finishRequested ||
                impl_->cancelRequested || impl_->producerFailure) {
                return false;
            }
            if (impl_->queue.size() >= kMaximumQueuedFrames) {
                impl_->accepting = false;
                impl_->producerFailure = true;
                impl_->producerFailureMessage =
                    L"实时画质代理队列超时；已保护全画质主录屏并降级为后台生成。";
                impl_->queue.clear();
                impl_->changed.notify_all();
                return false;
            }
            impl_->queue.push_back(std::move(frame));
        }
        impl_->changed.notify_one();
        return true;
    } catch (...) {
        std::scoped_lock lock(impl_->mutex);
        impl_->accepting = false;
        impl_->producerFailure = true;
        impl_->producerFailureMessage =
            L"实时画质代理帧入队失败；已降级为后台生成。";
        impl_->queue.clear();
        impl_->changed.notify_all();
        return false;
    }
}

RecordingQualityProxyResult RecordingQualityProxy::Finish() noexcept {
    {
        std::scoped_lock lock(impl_->mutex);
        impl_->accepting = false;
        impl_->finishRequested = true;
    }
    impl_->changed.notify_all();
    if (impl_->worker.joinable()) {
        impl_->worker.join();
    }
    std::scoped_lock lock(impl_->mutex);
    return impl_->result;
}

void RecordingQualityProxy::Cancel() noexcept {
    {
        std::scoped_lock lock(impl_->mutex);
        if (!impl_->worker.joinable()) {
            return;
        }
        impl_->accepting = false;
        impl_->cancelRequested = true;
        impl_->queue.clear();
    }
    impl_->changed.notify_all();
    impl_->worker.join();
    RemoveFileBestEffort(impl_->config.outputPath);
}

bool RecordingQualityProxy::IsAccepting() const noexcept {
    std::scoped_lock lock(impl_->mutex);
    return impl_->accepting;
}

}  // namespace qrec::capture
