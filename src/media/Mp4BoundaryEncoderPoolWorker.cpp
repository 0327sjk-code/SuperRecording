#include "media/Mp4BoundaryEncoderPoolInternal.h"

#include "common/Win32Helpers.h"
#include "media/Mp4BoundaryInternal.h"
#include "media/Mp4Writer.h"

#include <mfapi.h>

#include <algorithm>
#include <cwctype>
#include <ranges>
#include <utility>

namespace qrec::detail {
namespace boundary_encoder_pool {

std::optional<std::filesystem::path> NormalizeSourcePath(
    const std::filesystem::path& sourcePath) noexcept {
    try {
        if (sourcePath.empty()) {
            return std::nullopt;
        }
        std::error_code error;
        std::filesystem::path normalized =
            std::filesystem::absolute(sourcePath, error).lexically_normal();
        if (error || normalized.empty()) {
            return std::nullopt;
        }
        return normalized;
    } catch (...) {
        return std::nullopt;
    }
}

std::wstring SourceIdentity(
    const std::filesystem::path& normalizedPath) {
    std::wstring identity = normalizedPath.native();
    std::ranges::transform(
        identity,
        identity.begin(),
        [](const wchar_t character) noexcept {
            return static_cast<wchar_t>(std::towlower(character));
        });
    return identity;
}

bool KeysMatch(
    const Mp4BoundaryEncoderKey& left,
    const Mp4BoundaryEncoderKey& right) noexcept {
    return _wcsicmp(left.sourcePath.c_str(), right.sourcePath.c_str()) == 0 &&
           left.width == right.width && left.height == right.height &&
           left.framesPerSecond == right.framesPerSecond &&
           left.averageBitrate == right.averageBitrate;
}

bool IsCurrentThreadMta() noexcept {
    APTTYPE apartmentType = APTTYPE_CURRENT;
    APTTYPEQUALIFIER qualifier = APTTYPEQUALIFIER_NONE;
    return SUCCEEDED(::CoGetApartmentType(&apartmentType, &qualifier)) &&
           apartmentType == APTTYPE_MTA;
}

void RemoveOwnedPath(const std::filesystem::path& path) noexcept {
    if (path.empty()) {
        return;
    }
    std::error_code ignored;
    static_cast<void>(std::filesystem::remove(path, ignored));
}

PreparedSession::~PreparedSession() {
    writer.reset();
    RemoveOwnedPath(outputPath);
}

namespace {

[[nodiscard]] std::filesystem::path WarmDirectory() {
    return win32::LocalAppDataDirectory() / L"BoundaryWarm";
}

void CleanupStaleWarmFiles() noexcept {
    try {
        const std::filesystem::path directory = WarmDirectory();
        std::error_code error;
        if (!std::filesystem::is_directory(directory, error) || error) {
            return;
        }
        for (std::filesystem::directory_iterator iterator(directory, error), end;
             !error && iterator != end;
             iterator.increment(error)) {
            const std::filesystem::directory_entry& entry = *iterator;
            std::error_code statusError;
            if (!entry.is_regular_file(statusError) || statusError) {
                continue;
            }
            std::wstring extension = entry.path().extension().wstring();
            std::ranges::transform(
                extension,
                extension.begin(),
                [](const wchar_t character) noexcept {
                    return static_cast<wchar_t>(std::towlower(character));
                });
            if (extension == L".mp4" &&
                entry.path().filename().wstring().starts_with(
                    L"boundary_warm_")) {
                std::error_code removeError;
                static_cast<void>(std::filesystem::remove(
                    entry.path(),
                    removeError));
            }
        }
    } catch (...) {
    }
}

[[nodiscard]] std::unique_ptr<PreparedSession> OpenPreparedSession(
    const Mp4BoundaryEncoderKey& key,
    HRESULT* nativeError) {
    if (nativeError == nullptr) {
        return {};
    }
    *nativeError = E_FAIL;

    const std::filesystem::path directory = WarmDirectory();
    std::wstring pathError;
    if (!win32::EnsureDirectory(directory, &pathError)) {
        *nativeError = HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND);
        return {};
    }
    const std::filesystem::path outputPath = win32::MakeUniquePath(
        directory,
        L"boundary_warm",
        L".mp4",
        &pathError);
    if (outputPath.empty()) {
        *nativeError = HRESULT_FROM_WIN32(ERROR_FILE_EXISTS);
        return {};
    }

    auto session = std::make_unique<PreparedSession>();
    session->outputPath = outputPath;
    session->writer = std::make_unique<media::Mp4Writer>();

    media::Mp4WriterConfig config{};
    config.outputPath = outputPath;
    config.width = key.width;
    config.height = key.height;
    config.framesPerSecond = key.framesPerSecond;
    config.averageBitrate = key.averageBitrate;
    config.preferHardwareEncoder = true;

    std::wstring writerError;
    long writerNativeError = 0;
    const auto openStarted = std::chrono::steady_clock::now();
    const bool opened = session->writer->Open(
        config,
        writerError,
        writerNativeError);
    session->openDuration = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - openStarted);
    if (!opened) {
        *nativeError = static_cast<HRESULT>(writerNativeError);
        return {};
    }

    *nativeError = S_OK;
    return session;
}

}  // namespace
}  // namespace boundary_encoder_pool

using boundary_encoder_pool::EntryState;
using boundary_encoder_pool::OpenWork;
using boundary_encoder_pool::PreparedSession;

Mp4BoundaryEncoderLease::Session::~Session() {
    writer.reset();
    if (!outputTransferred) {
        boundary_encoder_pool::RemoveOwnedPath(outputPath);
    }
    if (owner != nullptr) {
        owner->ReleaseLease(key, generation);
    }
}

Mp4BoundaryEncoderPool::Impl::Impl()
    : worker([this](const std::stop_token stopToken) noexcept {
          WorkerLoop(stopToken);
      }) {}

Mp4BoundaryEncoderPool::Impl::~Impl() {
    if (worker.joinable()) {
        worker.request_stop();
        changed.notify_all();
        worker.join();
    }
}

bool Mp4BoundaryEncoderPool::Impl::HasWorkerAction() const noexcept {
    return std::ranges::any_of(
        entries,
        [](const auto& pair) noexcept {
            const boundary_encoder_pool::PoolEntry& entry = pair.second;
            return entry.state == EntryState::Queued ||
                   entry.state == EntryState::Discarded;
        });
}

void Mp4BoundaryEncoderPool::Impl::MarkOpenFailed(
    const std::uint64_t generation) noexcept {
    try {
        std::scoped_lock lock(mutex);
        const auto iterator = entries.find(generation);
        if (iterator != entries.end() &&
            iterator->second.state == EntryState::Opening) {
            iterator->second.state = EntryState::Failed;
        }
        changed.notify_all();
    } catch (...) {
    }
}

void Mp4BoundaryEncoderPool::Impl::PublishProbedKey(
    const std::uint64_t generation,
    const Mp4BoundaryEncoderKey& key) noexcept {
    try {
        std::scoped_lock lock(mutex);
        const auto iterator = entries.find(generation);
        if (iterator != entries.end() &&
            iterator->second.state == EntryState::Opening) {
            iterator->second.key = key;
        }
        changed.notify_all();
    } catch (...) {
    }
}

std::optional<OpenWork> Mp4BoundaryEncoderPool::Impl::SelectWork(
    std::unique_ptr<PreparedSession>* cleanup) {
    if (cleanup == nullptr) {
        return std::nullopt;
    }
    for (auto iterator = entries.begin(); iterator != entries.end();) {
        boundary_encoder_pool::PoolEntry& entry = iterator->second;
        if (entry.state == EntryState::Discarded) {
            if (entry.ready != nullptr) {
                *cleanup = std::move(entry.ready);
            }
            iterator = entries.erase(iterator);
            return std::nullopt;
        }
        if (entry.state == EntryState::Queued) {
            entry.state = EntryState::Opening;
            return OpenWork{
                entry.generation,
                entry.sourcePath,
                entry.key};
        }
        ++iterator;
    }
    return std::nullopt;
}

void Mp4BoundaryEncoderPool::Impl::DrainReadySessions() noexcept {
    for (;;) {
        std::unique_ptr<PreparedSession> cleanup;
        {
            std::scoped_lock lock(mutex);
            auto iterator = std::ranges::find_if(
                entries,
                [](const auto& pair) noexcept {
                    return pair.second.ready != nullptr;
                });
            if (iterator == entries.end()) {
                entries.clear();
                currentBySource.clear();
                break;
            }
            cleanup = std::move(iterator->second.ready);
            entries.erase(iterator);
        }
        cleanup.reset();
    }
}

void Mp4BoundaryEncoderPool::Impl::WorkerLoop(
    const std::stop_token stopToken) noexcept {
    static_cast<void>(::SetThreadPriority(
        ::GetCurrentThread(),
        THREAD_PRIORITY_BELOW_NORMAL));
    const HRESULT apartmentResult =
        ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const HRESULT mediaFoundationResult = SUCCEEDED(apartmentResult)
        ? ::MFStartup(MF_VERSION, MFSTARTUP_FULL)
        : apartmentResult;
    const bool initialized = SUCCEEDED(mediaFoundationResult);
    if (initialized) {
        boundary_encoder_pool::CleanupStaleWarmFiles();
    }

    while (!stopToken.stop_requested()) {
        std::unique_ptr<PreparedSession> cleanup;
        std::optional<OpenWork> work;
        try {
            std::unique_lock lock(mutex);
            changed.wait(
                lock,
                stopToken,
                [this]() noexcept { return HasWorkerAction(); });
            if (stopToken.stop_requested()) {
                break;
            }
            work = SelectWork(&cleanup);
        } catch (...) {
            break;
        }

        if (cleanup != nullptr) {
            cleanup.reset();
            continue;
        }
        if (!work.has_value()) {
            continue;
        }
        if (!initialized) {
            MarkOpenFailed(work->generation);
            continue;
        }

        std::optional<Mp4BoundaryEncoderKey> openKey = work->key;
        if (!openKey.has_value()) {
            Mp4BoundaryEncoderKey probed{};
            const BoundaryStepResult probe = ProbeBoundaryEncoderKey(
                work->sourcePath,
                &probed);
            if (!probe.Succeeded()) {
                MarkOpenFailed(work->generation);
                continue;
            }
            openKey = std::move(probed);
            PublishProbedKey(work->generation, *openKey);
        }

        HRESULT openError = E_FAIL;
        std::unique_ptr<PreparedSession> opened;
        try {
            opened = boundary_encoder_pool::OpenPreparedSession(
                *openKey,
                &openError);
        } catch (...) {
            opened.reset();
            openError = E_OUTOFMEMORY;
        }
        static_cast<void>(openError);

        bool published = false;
        try {
            std::scoped_lock lock(mutex);
            const auto iterator = entries.find(work->generation);
            if (iterator != entries.end() &&
                iterator->second.state == EntryState::Opening) {
                iterator->second.key = openKey;
                if (opened != nullptr) {
                    iterator->second.ready = std::move(opened);
                    iterator->second.state = EntryState::Ready;
                    published = true;
                } else {
                    iterator->second.state = EntryState::Failed;
                }
            }
            changed.notify_all();
        } catch (...) {
        }
        if (!published) {
            opened.reset();
        }
    }

    DrainReadySessions();
    if (SUCCEEDED(mediaFoundationResult)) {
        static_cast<void>(::MFShutdown());
    }
    if (SUCCEEDED(apartmentResult)) {
        ::CoUninitialize();
    }
}

}  // namespace qrec::detail
