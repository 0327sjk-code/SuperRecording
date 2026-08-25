#include "media/Mp4EditListPatcher.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <new>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace qrec {
namespace {

using Byte = std::byte;
using FourCc = std::uint32_t;

constexpr std::uint64_t kHundredNanosecondsPerSecond = 10'000'000ULL;
constexpr std::uint64_t kMaximumMoovBytes = 256ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kMaximumMediaEndRoundingDrift = 1ULL;

constexpr FourCc MakeFourCc(
    const char first,
    const char second,
    const char third,
    const char fourth) noexcept {
    return (static_cast<FourCc>(static_cast<unsigned char>(first)) << 24U) |
        (static_cast<FourCc>(static_cast<unsigned char>(second)) << 16U) |
        (static_cast<FourCc>(static_cast<unsigned char>(third)) << 8U) |
        static_cast<FourCc>(static_cast<unsigned char>(fourth));
}

constexpr FourCc kFtyp = MakeFourCc('f', 't', 'y', 'p');
constexpr FourCc kMdat = MakeFourCc('m', 'd', 'a', 't');
constexpr FourCc kMoov = MakeFourCc('m', 'o', 'o', 'v');
constexpr FourCc kMoof = MakeFourCc('m', 'o', 'o', 'f');
constexpr FourCc kMvex = MakeFourCc('m', 'v', 'e', 'x');
constexpr FourCc kMvhd = MakeFourCc('m', 'v', 'h', 'd');
constexpr FourCc kTrak = MakeFourCc('t', 'r', 'a', 'k');
constexpr FourCc kTkhd = MakeFourCc('t', 'k', 'h', 'd');
constexpr FourCc kEdts = MakeFourCc('e', 'd', 't', 's');
constexpr FourCc kElst = MakeFourCc('e', 'l', 's', 't');
constexpr FourCc kMdia = MakeFourCc('m', 'd', 'i', 'a');
constexpr FourCc kMdhd = MakeFourCc('m', 'd', 'h', 'd');
constexpr FourCc kHdlr = MakeFourCc('h', 'd', 'l', 'r');
constexpr FourCc kMinf = MakeFourCc('m', 'i', 'n', 'f');
constexpr FourCc kStbl = MakeFourCc('s', 't', 'b', 'l');
constexpr FourCc kStsd = MakeFourCc('s', 't', 's', 'd');
constexpr FourCc kVide = MakeFourCc('v', 'i', 'd', 'e');
constexpr FourCc kAvc1 = MakeFourCc('a', 'v', 'c', '1');
constexpr FourCc kAvc3 = MakeFourCc('a', 'v', 'c', '3');

class UniqueHandle final {
public:
    UniqueHandle() noexcept = default;
    explicit UniqueHandle(const HANDLE handle) noexcept : handle_(handle) {}

    ~UniqueHandle() {
        Reset();
    }

    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;

    UniqueHandle(UniqueHandle&& other) noexcept
        : handle_(std::exchange(other.handle_, INVALID_HANDLE_VALUE)) {}

    UniqueHandle& operator=(UniqueHandle&& other) noexcept {
        if (this != &other) {
            Reset(std::exchange(other.handle_, INVALID_HANDLE_VALUE));
        }
        return *this;
    }

    [[nodiscard]] HANDLE Get() const noexcept { return handle_; }
    [[nodiscard]] bool IsValid() const noexcept {
        return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
    }

    void Reset(const HANDLE replacement = INVALID_HANDLE_VALUE) noexcept {
        if (IsValid()) {
            ::CloseHandle(handle_);
        }
        handle_ = replacement;
    }

private:
    HANDLE handle_{INVALID_HANDLE_VALUE};
};

struct BoxView final {
    FourCc type{};
    std::uint64_t offset{};
    std::uint64_t size{};
    std::uint64_t headerSize{};
    bool extendedSize{};

    [[nodiscard]] std::uint64_t End() const noexcept {
        return offset + size;
    }

    [[nodiscard]] std::uint64_t PayloadOffset() const noexcept {
        return offset + headerSize;
    }

    [[nodiscard]] std::uint64_t PayloadSize() const noexcept {
        return size - headerSize;
    }
};

struct TimeHeader final {
    std::uint8_t version{};
    std::uint32_t timescale{};
    std::uint64_t duration{};
};

struct TrackLayout final {
    BoxView track;
    BoxView trackHeader;
    BoxView mediaHeader;
    FourCc handlerType{};
    TimeHeader mediaTime;
    bool h264{};
};

struct MovieLayout final {
    BoxView movie;
    BoxView movieHeader;
    TrackLayout videoTrack;
    TimeHeader movieTime;
};

enum class FileBoxReadStatus : std::uint8_t {
    Succeeded,
    Malformed,
    IoFailure,
};

[[nodiscard]] std::uint32_t ReadBigEndian32(
    const std::span<const Byte> bytes,
    const std::size_t offset) noexcept {
    return (std::to_integer<std::uint32_t>(bytes[offset]) << 24U) |
        (std::to_integer<std::uint32_t>(bytes[offset + 1]) << 16U) |
        (std::to_integer<std::uint32_t>(bytes[offset + 2]) << 8U) |
        std::to_integer<std::uint32_t>(bytes[offset + 3]);
}

[[nodiscard]] std::uint64_t ReadBigEndian64(
    const std::span<const Byte> bytes,
    const std::size_t offset) noexcept {
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < 8; ++index) {
        value = (value << 8U) |
            std::to_integer<std::uint64_t>(bytes[offset + index]);
    }
    return value;
}

void WriteBigEndian32(
    const std::span<Byte> bytes,
    const std::size_t offset,
    const std::uint32_t value) noexcept {
    bytes[offset] = static_cast<Byte>((value >> 24U) & 0xFFU);
    bytes[offset + 1] = static_cast<Byte>((value >> 16U) & 0xFFU);
    bytes[offset + 2] = static_cast<Byte>((value >> 8U) & 0xFFU);
    bytes[offset + 3] = static_cast<Byte>(value & 0xFFU);
}

void WriteBigEndian64(
    const std::span<Byte> bytes,
    const std::size_t offset,
    const std::uint64_t value) noexcept {
    for (std::size_t index = 0; index < 8; ++index) {
        const std::size_t shift = (7U - index) * 8U;
        bytes[offset + index] = static_cast<Byte>((value >> shift) & 0xFFULL);
    }
}

void AppendBigEndian32(
    std::vector<Byte>* output,
    const std::uint32_t value) {
    const std::size_t offset = output->size();
    output->resize(offset + 4);
    WriteBigEndian32(std::span<Byte>(*output), offset, value);
}

void AppendBigEndian64(
    std::vector<Byte>* output,
    const std::uint64_t value) {
    const std::size_t offset = output->size();
    output->resize(offset + 8);
    WriteBigEndian64(std::span<Byte>(*output), offset, value);
}

void AppendBytes(
    std::vector<Byte>* output,
    const std::span<const Byte> bytes) {
    output->insert(output->end(), bytes.begin(), bytes.end());
}

[[nodiscard]] std::wstring FormatSystemError(const DWORD error) {
    wchar_t* buffer = nullptr;
    const DWORD length = ::FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        error,
        0,
        reinterpret_cast<wchar_t*>(&buffer),
        0,
        nullptr);
    std::wstring message;
    if (length != 0 && buffer != nullptr) {
        message.assign(buffer, length);
        while (!message.empty() &&
               (message.back() == L'\r' || message.back() == L'\n')) {
            message.pop_back();
        }
    } else {
        message = L"Win32 错误 " + std::to_wstring(error);
    }
    if (buffer != nullptr) {
        ::LocalFree(buffer);
    }
    return message;
}

[[nodiscard]] Mp4EditListPatchResult MakeUnsupported(
    std::wstring message,
    const HRESULT nativeError = HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED)) {
    Mp4EditListPatchResult result{};
    result.outcome = Mp4EditListPatchOutcome::Unsupported;
    result.nativeError = nativeError;
    result.errorMessage = std::move(message);
    return result;
}

[[nodiscard]] Mp4EditListPatchResult MakeFailure(
    std::wstring message,
    const HRESULT nativeError) {
    Mp4EditListPatchResult result{};
    result.outcome = Mp4EditListPatchOutcome::Failed;
    result.nativeError = nativeError;
    result.errorMessage = std::move(message);
    return result;
}

[[nodiscard]] Mp4EditListPatchResult MakeWin32Failure(
    const std::wstring_view action,
    const DWORD error) {
    const HRESULT nativeError = error == ERROR_SUCCESS
        ? E_FAIL
        : HRESULT_FROM_WIN32(error);
    return MakeFailure(
        std::wstring(action) + L"：" + FormatSystemError(error),
        nativeError);
}

[[nodiscard]] bool ReadAt(
    const HANDLE file,
    const std::uint64_t offset,
    const std::span<Byte> destination,
    DWORD* nativeError) noexcept {
    if (nativeError == nullptr ||
        offset > static_cast<std::uint64_t>(
            std::numeric_limits<LONGLONG>::max())) {
        if (nativeError != nullptr) {
            *nativeError = ERROR_ARITHMETIC_OVERFLOW;
        }
        return false;
    }
    LARGE_INTEGER position{};
    position.QuadPart = static_cast<LONGLONG>(offset);
    if (::SetFilePointerEx(file, position, nullptr, FILE_BEGIN) == FALSE) {
        *nativeError = ::GetLastError();
        return false;
    }

    std::size_t completed = 0;
    while (completed < destination.size()) {
        const std::size_t remaining = destination.size() - completed;
        const DWORD requested = static_cast<DWORD>(std::min<std::size_t>(
            remaining,
            static_cast<std::size_t>(std::numeric_limits<DWORD>::max())));
        DWORD transferred = 0;
        if (::ReadFile(
                file,
                destination.data() + completed,
                requested,
                &transferred,
                nullptr) == FALSE) {
            *nativeError = ::GetLastError();
            return false;
        }
        if (transferred == 0) {
            *nativeError = ERROR_HANDLE_EOF;
            return false;
        }
        completed += transferred;
    }
    *nativeError = ERROR_SUCCESS;
    return true;
}

[[nodiscard]] FileBoxReadStatus ReadFileBox(
    const HANDLE file,
    const std::uint64_t fileSize,
    const std::uint64_t offset,
    BoxView* output,
    DWORD* nativeError,
    std::wstring* parseError) {
    if (output == nullptr || nativeError == nullptr || parseError == nullptr) {
        return FileBoxReadStatus::Malformed;
    }
    if (offset > fileSize || fileSize - offset < 8) {
        *parseError = L"顶层 MP4 box 头越过文件边界。";
        return FileBoxReadStatus::Malformed;
    }

    std::array<Byte, 16> header{};
    if (!ReadAt(
            file,
            offset,
            std::span<Byte>(header.data(), 8),
            nativeError)) {
        return FileBoxReadStatus::IoFailure;
    }
    const std::span<const Byte> headerBytes(header);
    const std::uint32_t shortSize = ReadBigEndian32(headerBytes, 0);
    const FourCc type = ReadBigEndian32(headerBytes, 4);
    std::uint64_t boxSize = shortSize;
    std::uint64_t headerSize = 8;
    bool extendedSize = false;
    if (shortSize == 1) {
        if (fileSize - offset < 16) {
            *parseError = L"large-size MP4 box 头越过文件边界。";
            return FileBoxReadStatus::Malformed;
        }
        if (!ReadAt(
                file,
                offset + 8,
                std::span<Byte>(header.data() + 8, 8),
                nativeError)) {
            return FileBoxReadStatus::IoFailure;
        }
        boxSize = ReadBigEndian64(headerBytes, 8);
        headerSize = 16;
        extendedSize = true;
    } else if (shortSize == 0) {
        boxSize = fileSize - offset;
    }

    if (boxSize < headerSize || boxSize > fileSize - offset) {
        *parseError = L"顶层 MP4 box size 无效或越过文件边界。";
        return FileBoxReadStatus::Malformed;
    }
    output->type = type;
    output->offset = offset;
    output->size = boxSize;
    output->headerSize = headerSize;
    output->extendedSize = extendedSize;
    return FileBoxReadStatus::Succeeded;
}

[[nodiscard]] bool ParseMemoryBox(
    const std::span<const Byte> bytes,
    const std::size_t offset,
    const std::size_t parentEnd,
    BoxView* output,
    std::wstring* errorMessage) {
    if (output == nullptr || errorMessage == nullptr ||
        parentEnd > bytes.size() || offset > parentEnd ||
        parentEnd - offset < 8) {
        if (errorMessage != nullptr) {
            *errorMessage = L"moov 子 box 头越过父 box 边界。";
        }
        return false;
    }

    const std::uint32_t shortSize = ReadBigEndian32(bytes, offset);
    const FourCc type = ReadBigEndian32(bytes, offset + 4);
    std::uint64_t boxSize = shortSize;
    std::uint64_t headerSize = 8;
    bool extendedSize = false;
    if (shortSize == 1) {
        if (parentEnd - offset < 16) {
            *errorMessage = L"moov 内 large-size box 头越过父 box 边界。";
            return false;
        }
        boxSize = ReadBigEndian64(bytes, offset + 8);
        headerSize = 16;
        extendedSize = true;
    } else if (shortSize == 0) {
        boxSize = parentEnd - offset;
    }

    if (boxSize < headerSize ||
        boxSize > static_cast<std::uint64_t>(parentEnd - offset)) {
        *errorMessage = L"moov 子 box size 无效或越过父 box 边界。";
        return false;
    }
    output->type = type;
    output->offset = offset;
    output->size = boxSize;
    output->headerSize = headerSize;
    output->extendedSize = extendedSize;
    return true;
}

[[nodiscard]] bool ParseChildren(
    const std::span<const Byte> bytes,
    const BoxView& parent,
    std::vector<BoxView>* children,
    std::wstring* errorMessage) {
    if (children == nullptr || errorMessage == nullptr ||
        parent.End() > bytes.size()) {
        if (errorMessage != nullptr) {
            *errorMessage = L"父 MP4 box 范围无效。";
        }
        return false;
    }
    children->clear();
    std::size_t position = static_cast<std::size_t>(parent.PayloadOffset());
    const std::size_t end = static_cast<std::size_t>(parent.End());
    while (position < end) {
        BoxView child{};
        if (!ParseMemoryBox(bytes, position, end, &child, errorMessage)) {
            return false;
        }
        children->push_back(child);
        position = static_cast<std::size_t>(child.End());
    }
    return position == end;
}

[[nodiscard]] std::span<const Byte> BytesForBox(
    const std::span<const Byte> bytes,
    const BoxView& box) noexcept {
    return bytes.subspan(
        static_cast<std::size_t>(box.offset),
        static_cast<std::size_t>(box.size));
}

[[nodiscard]] std::span<const Byte> PayloadForBox(
    const std::span<const Byte> bytes,
    const BoxView& box) noexcept {
    return bytes.subspan(
        static_cast<std::size_t>(box.PayloadOffset()),
        static_cast<std::size_t>(box.PayloadSize()));
}

[[nodiscard]] bool ReadTimeHeader(
    const std::span<const Byte> bytes,
    const BoxView& box,
    TimeHeader* output,
    std::wstring* errorMessage) {
    if (output == nullptr || errorMessage == nullptr) {
        return false;
    }
    const std::span<const Byte> payload = PayloadForBox(bytes, box);
    if (payload.size() < 4) {
        *errorMessage = L"时间头 box 过短。";
        return false;
    }
    const std::uint8_t version = std::to_integer<std::uint8_t>(payload[0]);
    TimeHeader header{};
    header.version = version;
    if (version == 0) {
        if (payload.size() < 20) {
            *errorMessage = L"v0 时间头 box 过短。";
            return false;
        }
        header.timescale = ReadBigEndian32(payload, 12);
        header.duration = ReadBigEndian32(payload, 16);
        if (header.duration == std::numeric_limits<std::uint32_t>::max()) {
            *errorMessage = L"时间头使用未知 duration，无法安全写入 edit list。";
            return false;
        }
    } else if (version == 1) {
        if (payload.size() < 32) {
            *errorMessage = L"v1 时间头 box 过短。";
            return false;
        }
        header.timescale = ReadBigEndian32(payload, 20);
        header.duration = ReadBigEndian64(payload, 24);
        if (header.duration == std::numeric_limits<std::uint64_t>::max()) {
            *errorMessage = L"时间头使用未知 duration，无法安全写入 edit list。";
            return false;
        }
    } else {
        *errorMessage = L"时间头 full-box version 不受支持。";
        return false;
    }
    if (header.timescale == 0) {
        *errorMessage = L"时间头 timescale 为零。";
        return false;
    }
    *output = header;
    return true;
}

[[nodiscard]] bool FindSingleChild(
    const std::vector<BoxView>& children,
    const FourCc type,
    BoxView* output,
    std::wstring* errorMessage,
    const std::wstring_view description) {
    std::size_t count = 0;
    for (const BoxView& child : children) {
        if (child.type == type) {
            *output = child;
            ++count;
        }
    }
    if (count != 1) {
        *errorMessage = std::wstring(description) +
            (count == 0 ? L"缺失。" : L"存在多个实例。" );
        return false;
    }
    return true;
}

[[nodiscard]] bool ValidateH264SampleDescription(
    const std::span<const Byte> bytes,
    const BoxView& mediaInformation,
    std::wstring* errorMessage) {
    std::vector<BoxView> children;
    if (!ParseChildren(bytes, mediaInformation, &children, errorMessage)) {
        return false;
    }
    BoxView sampleTable{};
    if (!FindSingleChild(
            children,
            kStbl,
            &sampleTable,
            errorMessage,
            L"视频轨 stbl ")) {
        return false;
    }
    if (!ParseChildren(bytes, sampleTable, &children, errorMessage)) {
        return false;
    }
    BoxView sampleDescription{};
    if (!FindSingleChild(
            children,
            kStsd,
            &sampleDescription,
            errorMessage,
            L"视频轨 stsd ")) {
        return false;
    }

    const std::span<const Byte> payload = PayloadForBox(bytes, sampleDescription);
    if (payload.size() < 16 ||
        std::to_integer<std::uint8_t>(payload[0]) != 0) {
        *errorMessage = L"视频轨 stsd 版本或长度不受支持。";
        return false;
    }
    const std::uint32_t entryCount = ReadBigEndian32(payload, 4);
    if (entryCount != 1) {
        *errorMessage = L"仅支持单一 H.264 sample description。";
        return false;
    }
    const std::size_t entryOffset =
        static_cast<std::size_t>(sampleDescription.PayloadOffset()) + 8;
    BoxView entry{};
    if (!ParseMemoryBox(
            bytes,
            entryOffset,
            static_cast<std::size_t>(sampleDescription.End()),
            &entry,
            errorMessage) ||
        entry.End() != sampleDescription.End()) {
        if (errorMessage->empty()) {
            *errorMessage = L"视频 sample description 范围无效。";
        }
        return false;
    }
    if (entry.type != kAvc1 && entry.type != kAvc3) {
        *errorMessage = L"视频轨不是可支持的 avc1/avc3 H.264。";
        return false;
    }
    return true;
}

[[nodiscard]] bool ParseTrackLayout(
    const std::span<const Byte> bytes,
    const BoxView& track,
    TrackLayout* output,
    std::wstring* errorMessage) {
    std::vector<BoxView> children;
    if (!ParseChildren(bytes, track, &children, errorMessage)) {
        return false;
    }

    TrackLayout layout{};
    layout.track = track;
    BoxView media{};
    if (!FindSingleChild(
            children,
            kTkhd,
            &layout.trackHeader,
            errorMessage,
            L"trak/tkhd ") ||
        !FindSingleChild(
            children,
            kMdia,
            &media,
            errorMessage,
            L"trak/mdia ")) {
        return false;
    }
    if (!ParseChildren(bytes, media, &children, errorMessage)) {
        return false;
    }

    BoxView handler{};
    BoxView mediaInformation{};
    if (!FindSingleChild(
            children,
            kMdhd,
            &layout.mediaHeader,
            errorMessage,
            L"mdia/mdhd ") ||
        !FindSingleChild(
            children,
            kHdlr,
            &handler,
            errorMessage,
            L"mdia/hdlr ") ||
        !FindSingleChild(
            children,
            kMinf,
            &mediaInformation,
            errorMessage,
            L"mdia/minf ")) {
        return false;
    }

    const std::span<const Byte> handlerPayload = PayloadForBox(bytes, handler);
    if (handlerPayload.size() < 12) {
        *errorMessage = L"hdlr box 过短。";
        return false;
    }
    layout.handlerType = ReadBigEndian32(handlerPayload, 8);
    if (!ReadTimeHeader(
            bytes,
            layout.mediaHeader,
            &layout.mediaTime,
            errorMessage)) {
        return false;
    }
    if (layout.handlerType == kVide) {
        layout.h264 = ValidateH264SampleDescription(
            bytes,
            mediaInformation,
            errorMessage);
        if (!layout.h264) {
            return false;
        }
    }
    *output = layout;
    return true;
}

[[nodiscard]] bool AnalyzeMovie(
    const std::span<const Byte> moovBytes,
    MovieLayout* output,
    std::wstring* errorMessage) {
    BoxView movie{};
    if (!ParseMemoryBox(
            moovBytes,
            0,
            moovBytes.size(),
            &movie,
            errorMessage) ||
        movie.type != kMoov || movie.End() != moovBytes.size()) {
        if (errorMessage->empty()) {
            *errorMessage = L"载入的数据不是单一完整 moov box。";
        }
        return false;
    }

    std::vector<BoxView> children;
    if (!ParseChildren(moovBytes, movie, &children, errorMessage)) {
        return false;
    }
    for (const BoxView& child : children) {
        if (child.type == kMvex) {
            *errorMessage = L"fragmented MP4（mvex）不支持 edit-list 尾部重写。";
            return false;
        }
    }

    MovieLayout layout{};
    layout.movie = movie;
    if (!FindSingleChild(
            children,
            kMvhd,
            &layout.movieHeader,
            errorMessage,
            L"moov/mvhd ") ||
        !ReadTimeHeader(
            moovBytes,
            layout.movieHeader,
            &layout.movieTime,
            errorMessage)) {
        return false;
    }

    std::size_t trackCount = 0;
    std::size_t videoTrackCount = 0;
    for (const BoxView& child : children) {
        if (child.type != kTrak) {
            continue;
        }
        ++trackCount;
        TrackLayout track{};
        if (!ParseTrackLayout(moovBytes, child, &track, errorMessage)) {
            return false;
        }
        if (track.handlerType == kVide) {
            ++videoTrackCount;
            layout.videoTrack = track;
        }
    }
    if (videoTrackCount > 1) {
        *errorMessage = L"MP4 包含多个视频轨，无法确定唯一 edit-list 目标。";
        return false;
    }
    if (videoTrackCount != 1) {
        *errorMessage = L"MP4 不包含唯一的 H.264 vide 轨。";
        return false;
    }
    if (trackCount != 1) {
        *errorMessage = L"当前补丁器仅支持不含音频或其他媒体轨的单视频轨 MP4。";
        return false;
    }
    *output = layout;
    return true;
}

[[nodiscard]] bool ScaleHundredNanoseconds(
    const std::int64_t value,
    const std::uint32_t timescale,
    std::uint64_t* output) noexcept {
    if (output == nullptr || value < 0 || timescale == 0) {
        return false;
    }
    const std::uint64_t unsignedValue = static_cast<std::uint64_t>(value);
    const std::uint64_t whole = unsignedValue / kHundredNanosecondsPerSecond;
    const std::uint64_t remainder = unsignedValue % kHundredNanosecondsPerSecond;
    if (whole > std::numeric_limits<std::uint64_t>::max() / timescale) {
        return false;
    }
    const std::uint64_t integral = whole * timescale;
    const std::uint64_t fractionalNumerator = remainder * timescale +
        kHundredNanosecondsPerSecond / 2;
    const std::uint64_t fractional =
        fractionalNumerator / kHundredNanosecondsPerSecond;
    if (integral > std::numeric_limits<std::uint64_t>::max() - fractional) {
        return false;
    }
    *output = integral + fractional;
    return true;
}

[[nodiscard]] bool ScaleTimeUnits(
    const std::uint64_t value,
    const std::uint32_t sourceTimescale,
    const std::uint32_t destinationTimescale,
    std::uint64_t* output) noexcept {
    if (output == nullptr || sourceTimescale == 0 ||
        destinationTimescale == 0) {
        return false;
    }

    const std::uint64_t whole = value / sourceTimescale;
    const std::uint64_t remainder = value % sourceTimescale;
    if (whole >
        std::numeric_limits<std::uint64_t>::max() / destinationTimescale) {
        return false;
    }
    const std::uint64_t integral = whole * destinationTimescale;
    const std::uint64_t roundingOffset = sourceTimescale / 2ULL;
    if (remainder > 0 &&
        remainder >
            (std::numeric_limits<std::uint64_t>::max() - roundingOffset) /
                destinationTimescale) {
        return false;
    }
    const std::uint64_t fractionalNumerator =
        remainder * destinationTimescale + roundingOffset;
    const std::uint64_t fractional =
        fractionalNumerator / sourceTimescale;
    if (integral > std::numeric_limits<std::uint64_t>::max() - fractional) {
        return false;
    }

    *output = integral + fractional;
    return true;
}

[[nodiscard]] bool BuildBox(
    const FourCc type,
    const std::span<const Byte> payload,
    const bool preserveExtendedSize,
    std::vector<Byte>* output,
    std::wstring* errorMessage) {
    if (output == nullptr || errorMessage == nullptr) {
        return false;
    }
    const bool extended = preserveExtendedSize ||
        payload.size() >
            static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max() - 8U);
    const std::size_t headerSize = extended ? 16U : 8U;
    if (payload.size() > std::numeric_limits<std::size_t>::max() - headerSize) {
        *errorMessage = L"重建 MP4 box 时发生 size 溢出。";
        return false;
    }
    const std::size_t totalSize = headerSize + payload.size();
    output->assign(totalSize, Byte{0});
    std::span<Byte> outputBytes(*output);
    if (extended) {
        WriteBigEndian32(outputBytes, 0, 1);
        WriteBigEndian32(outputBytes, 4, type);
        WriteBigEndian64(outputBytes, 8, totalSize);
    } else {
        WriteBigEndian32(outputBytes, 0, static_cast<std::uint32_t>(totalSize));
        WriteBigEndian32(outputBytes, 4, type);
    }
    std::copy(payload.begin(), payload.end(), output->begin() + headerSize);
    return true;
}

[[nodiscard]] bool BuildEditBox(
    const std::uint64_t mediaStart,
    const std::uint64_t visibleMovieDuration,
    std::vector<Byte>* output,
    bool* usedVersion1,
    std::wstring* errorMessage) {
    if (output == nullptr || usedVersion1 == nullptr || errorMessage == nullptr ||
        visibleMovieDuration == 0 ||
        mediaStart > static_cast<std::uint64_t>(
            std::numeric_limits<std::int64_t>::max())) {
        if (errorMessage != nullptr) {
            *errorMessage = L"edit list 时间范围无效。";
        }
        return false;
    }
    const bool version1 =
        visibleMovieDuration > std::numeric_limits<std::uint32_t>::max() ||
        mediaStart > static_cast<std::uint64_t>(
            std::numeric_limits<std::int32_t>::max());
    std::vector<Byte> editPayload;
    editPayload.reserve(version1 ? 28U : 20U);
    AppendBigEndian32(&editPayload, version1 ? 0x01000000U : 0U);
    AppendBigEndian32(&editPayload, 1U);
    if (version1) {
        AppendBigEndian64(&editPayload, visibleMovieDuration);
        AppendBigEndian64(&editPayload, mediaStart);
    } else {
        AppendBigEndian32(
            &editPayload,
            static_cast<std::uint32_t>(visibleMovieDuration));
        AppendBigEndian32(&editPayload, static_cast<std::uint32_t>(mediaStart));
    }
    AppendBigEndian32(&editPayload, 0x00010000U);

    std::vector<Byte> editList;
    if (!BuildBox(
            kElst,
            editPayload,
            false,
            &editList,
            errorMessage)) {
        return false;
    }
    if (!BuildBox(kEdts, editList, false, output, errorMessage)) {
        return false;
    }
    *usedVersion1 = version1;
    return true;
}

[[nodiscard]] bool PatchMovieHeader(
    const std::span<const Byte> bytes,
    const BoxView& movieHeader,
    const std::uint64_t visibleMovieDuration,
    std::vector<Byte>* output,
    std::wstring* errorMessage) {
    const std::span<const Byte> originalPayload =
        PayloadForBox(bytes, movieHeader);
    if (originalPayload.empty()) {
        *errorMessage = L"mvhd box 为空。";
        return false;
    }
    const std::uint8_t version =
        std::to_integer<std::uint8_t>(originalPayload[0]);
    std::vector<Byte> payload;
    if (version == 0) {
        if (originalPayload.size() < 20) {
            *errorMessage = L"v0 mvhd box 过短。";
            return false;
        }
        if (visibleMovieDuration <= std::numeric_limits<std::uint32_t>::max()) {
            payload.assign(originalPayload.begin(), originalPayload.end());
            WriteBigEndian32(
                std::span<Byte>(payload),
                16,
                static_cast<std::uint32_t>(visibleMovieDuration));
        } else {
            payload.reserve(originalPayload.size() + 12);
            AppendBytes(&payload, originalPayload.first(4));
            payload[0] = static_cast<Byte>(1U);
            AppendBigEndian64(&payload, ReadBigEndian32(originalPayload, 4));
            AppendBigEndian64(&payload, ReadBigEndian32(originalPayload, 8));
            AppendBigEndian32(&payload, ReadBigEndian32(originalPayload, 12));
            AppendBigEndian64(&payload, visibleMovieDuration);
            AppendBytes(&payload, originalPayload.subspan(20));
        }
    } else if (version == 1) {
        if (originalPayload.size() < 32) {
            *errorMessage = L"v1 mvhd box 过短。";
            return false;
        }
        payload.assign(originalPayload.begin(), originalPayload.end());
        WriteBigEndian64(std::span<Byte>(payload), 24, visibleMovieDuration);
    } else {
        *errorMessage = L"mvhd full-box version 不受支持。";
        return false;
    }
    return BuildBox(
        kMvhd,
        payload,
        movieHeader.extendedSize,
        output,
        errorMessage);
}

[[nodiscard]] bool PatchTrackHeader(
    const std::span<const Byte> bytes,
    const BoxView& trackHeader,
    const std::uint64_t visibleMovieDuration,
    std::vector<Byte>* output,
    std::wstring* errorMessage) {
    const std::span<const Byte> originalPayload =
        PayloadForBox(bytes, trackHeader);
    if (originalPayload.empty()) {
        *errorMessage = L"tkhd box 为空。";
        return false;
    }
    const std::uint8_t version =
        std::to_integer<std::uint8_t>(originalPayload[0]);
    std::vector<Byte> payload;
    if (version == 0) {
        if (originalPayload.size() < 24) {
            *errorMessage = L"v0 tkhd box 过短。";
            return false;
        }
        if (visibleMovieDuration <= std::numeric_limits<std::uint32_t>::max()) {
            payload.assign(originalPayload.begin(), originalPayload.end());
            WriteBigEndian32(
                std::span<Byte>(payload),
                20,
                static_cast<std::uint32_t>(visibleMovieDuration));
        } else {
            payload.reserve(originalPayload.size() + 12);
            AppendBytes(&payload, originalPayload.first(4));
            payload[0] = static_cast<Byte>(1U);
            AppendBigEndian64(&payload, ReadBigEndian32(originalPayload, 4));
            AppendBigEndian64(&payload, ReadBigEndian32(originalPayload, 8));
            AppendBigEndian32(&payload, ReadBigEndian32(originalPayload, 12));
            AppendBigEndian32(&payload, ReadBigEndian32(originalPayload, 16));
            AppendBigEndian64(&payload, visibleMovieDuration);
            AppendBytes(&payload, originalPayload.subspan(24));
        }
    } else if (version == 1) {
        if (originalPayload.size() < 36) {
            *errorMessage = L"v1 tkhd box 过短。";
            return false;
        }
        payload.assign(originalPayload.begin(), originalPayload.end());
        WriteBigEndian64(std::span<Byte>(payload), 28, visibleMovieDuration);
    } else {
        *errorMessage = L"tkhd full-box version 不受支持。";
        return false;
    }
    return BuildBox(
        kTkhd,
        payload,
        trackHeader.extendedSize,
        output,
        errorMessage);
}

[[nodiscard]] bool RebuildTrack(
    const std::span<const Byte> bytes,
    const TrackLayout& layout,
    const std::uint64_t visibleMovieDuration,
    const std::span<const Byte> editBox,
    std::vector<Byte>* output,
    std::wstring* errorMessage) {
    std::vector<BoxView> children;
    if (!ParseChildren(bytes, layout.track, &children, errorMessage)) {
        return false;
    }
    std::size_t editCount = 0;
    for (const BoxView& child : children) {
        if (child.type == kEdts) {
            ++editCount;
        }
    }
    if (editCount > 1) {
        *errorMessage = L"视频 trak 包含多个 edts box。";
        return false;
    }

    std::vector<Byte> patchedTrackHeader;
    if (!PatchTrackHeader(
            bytes,
            layout.trackHeader,
            visibleMovieDuration,
            &patchedTrackHeader,
            errorMessage)) {
        return false;
    }

    std::vector<Byte> payload;
    const std::size_t extraCapacity =
        editBox.size() + patchedTrackHeader.size() + 32U;
    if (static_cast<std::uint64_t>(layout.track.PayloadSize()) >
        std::numeric_limits<std::size_t>::max() - extraCapacity) {
        *errorMessage = L"重建 trak 时发生 size 溢出。";
        return false;
    }
    payload.reserve(
        static_cast<std::size_t>(layout.track.PayloadSize()) + extraCapacity);
    bool insertedEdit = false;
    for (const BoxView& child : children) {
        if (child.type == kTkhd) {
            AppendBytes(&payload, patchedTrackHeader);
            if (editCount == 0) {
                AppendBytes(&payload, editBox);
                insertedEdit = true;
            }
        } else if (child.type == kEdts) {
            AppendBytes(&payload, editBox);
            insertedEdit = true;
        } else {
            AppendBytes(&payload, BytesForBox(bytes, child));
        }
    }
    if (!insertedEdit) {
        *errorMessage = L"未能在视频 trak 中插入 edts/elst。";
        return false;
    }
    return BuildBox(
        kTrak,
        payload,
        layout.track.extendedSize,
        output,
        errorMessage);
}

[[nodiscard]] bool RebuildMovie(
    const std::span<const Byte> bytes,
    const MovieLayout& layout,
    const std::uint64_t visibleMovieDuration,
    const std::uint64_t mediaStart,
    std::vector<Byte>* output,
    bool* usedVersion1,
    std::wstring* errorMessage) {
    std::vector<Byte> editBox;
    if (!BuildEditBox(
            mediaStart,
            visibleMovieDuration,
            &editBox,
            usedVersion1,
            errorMessage)) {
        return false;
    }
    std::vector<Byte> patchedMovieHeader;
    if (!PatchMovieHeader(
            bytes,
            layout.movieHeader,
            visibleMovieDuration,
            &patchedMovieHeader,
            errorMessage)) {
        return false;
    }
    std::vector<Byte> patchedTrack;
    if (!RebuildTrack(
            bytes,
            layout.videoTrack,
            visibleMovieDuration,
            editBox,
            &patchedTrack,
            errorMessage)) {
        return false;
    }

    std::vector<BoxView> children;
    if (!ParseChildren(bytes, layout.movie, &children, errorMessage)) {
        return false;
    }
    std::vector<Byte> payload;
    const std::size_t extraCapacity =
        editBox.size() + patchedMovieHeader.size() + patchedTrack.size() + 64U;
    if (static_cast<std::uint64_t>(layout.movie.PayloadSize()) >
        std::numeric_limits<std::size_t>::max() - extraCapacity) {
        *errorMessage = L"重建 moov 时发生 size 溢出。";
        return false;
    }
    payload.reserve(
        static_cast<std::size_t>(layout.movie.PayloadSize()) + extraCapacity);
    std::size_t patchedMovieHeaderCount = 0;
    std::size_t patchedTrackCount = 0;
    for (const BoxView& child : children) {
        if (child.offset == layout.movieHeader.offset) {
            AppendBytes(&payload, patchedMovieHeader);
            ++patchedMovieHeaderCount;
        } else if (child.offset == layout.videoTrack.track.offset) {
            AppendBytes(&payload, patchedTrack);
            ++patchedTrackCount;
        } else {
            AppendBytes(&payload, BytesForBox(bytes, child));
        }
    }
    if (patchedMovieHeaderCount != 1 || patchedTrackCount != 1) {
        *errorMessage = L"重建 moov 时无法唯一定位 mvhd 或视频 trak。";
        return false;
    }
    return BuildBox(
        kMoov,
        payload,
        layout.movie.extendedSize,
        output,
        errorMessage);
}

[[nodiscard]] bool WriteAll(
    const HANDLE file,
    const std::span<const Byte> bytes,
    DWORD* nativeError) noexcept {
    std::size_t completed = 0;
    while (completed < bytes.size()) {
        const std::size_t remaining = bytes.size() - completed;
        const DWORD requested = static_cast<DWORD>(std::min<std::size_t>(
            remaining,
            static_cast<std::size_t>(std::numeric_limits<DWORD>::max())));
        DWORD transferred = 0;
        if (::WriteFile(
                file,
                bytes.data() + completed,
                requested,
                &transferred,
                nullptr) == FALSE) {
            *nativeError = ::GetLastError();
            return false;
        }
        if (transferred == 0) {
            *nativeError = ERROR_WRITE_FAULT;
            return false;
        }
        completed += transferred;
    }
    *nativeError = ERROR_SUCCESS;
    return true;
}

[[nodiscard]] bool RewriteTail(
    const HANDLE file,
    const std::uint64_t offset,
    const std::span<const Byte> bytes,
    DWORD* nativeError) noexcept {
    if (nativeError == nullptr ||
        offset > static_cast<std::uint64_t>(
            std::numeric_limits<LONGLONG>::max())) {
        if (nativeError != nullptr) {
            *nativeError = ERROR_ARITHMETIC_OVERFLOW;
        }
        return false;
    }
    LARGE_INTEGER position{};
    position.QuadPart = static_cast<LONGLONG>(offset);
    if (::SetFilePointerEx(file, position, nullptr, FILE_BEGIN) == FALSE) {
        *nativeError = ::GetLastError();
        return false;
    }
    if (!WriteAll(file, bytes, nativeError)) {
        return false;
    }
    if (::SetEndOfFile(file) == FALSE) {
        *nativeError = ::GetLastError();
        return false;
    }
    if (::FlushFileBuffers(file) == FALSE) {
        *nativeError = ::GetLastError();
        return false;
    }
    *nativeError = ERROR_SUCCESS;
    return true;
}

}  // namespace

Mp4EditListPatchResult Mp4EditListPatcher::Patch(
    const std::filesystem::path& path,
    const std::int64_t mediaStart100Nanoseconds,
    const std::int64_t visibleDuration100Nanoseconds) noexcept {
    try {
        if (path.empty() || mediaStart100Nanoseconds < 0 ||
            visibleDuration100Nanoseconds <= 0 ||
            mediaStart100Nanoseconds >
                std::numeric_limits<std::int64_t>::max() -
                    visibleDuration100Nanoseconds) {
            return MakeFailure(L"edit-list 补丁参数无效。", E_INVALIDARG);
        }

        std::error_code absoluteError;
        const std::filesystem::path absolutePath =
            std::filesystem::absolute(path, absoluteError);
        if (absoluteError) {
            return MakeFailure(
                L"无法规范化 MP4 路径。",
                HRESULT_FROM_WIN32(ERROR_INVALID_NAME));
        }
        const DWORD attributes = ::GetFileAttributesW(absolutePath.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            return MakeWin32Failure(L"MP4 文件不存在", ::GetLastError());
        }
        if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            return MakeWin32Failure(L"MP4 路径指向目录", ERROR_DIRECTORY);
        }

        UniqueHandle source(::CreateFileW(
            absolutePath.c_str(),
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ,
            nullptr,
            OPEN_EXISTING,
            FILE_FLAG_SEQUENTIAL_SCAN,
            nullptr));
        if (!source.IsValid()) {
            return MakeWin32Failure(L"无法打开 MP4 文件", ::GetLastError());
        }
        LARGE_INTEGER signedFileSize{};
        if (::GetFileSizeEx(source.Get(), &signedFileSize) == FALSE) {
            return MakeWin32Failure(L"无法读取 MP4 文件大小", ::GetLastError());
        }
        if (signedFileSize.QuadPart <= 0) {
            return MakeUnsupported(
                L"MP4 文件为空。",
                HRESULT_FROM_WIN32(ERROR_FILE_INVALID));
        }
        const std::uint64_t fileSize =
            static_cast<std::uint64_t>(signedFileSize.QuadPart);

        BoxView movieFileBox{};
        std::size_t movieCount = 0;
        std::size_t mediaDataCount = 0;
        std::uint64_t lastMediaDataEnd = 0;
        bool fragmented = false;
        bool fileTypeFound = false;
        std::uint64_t position = 0;
        while (position < fileSize) {
            BoxView box{};
            DWORD readError = ERROR_SUCCESS;
            std::wstring parseError;
            const FileBoxReadStatus readStatus = ReadFileBox(
                source.Get(),
                fileSize,
                position,
                &box,
                &readError,
                &parseError);
            if (readStatus == FileBoxReadStatus::IoFailure) {
                return MakeWin32Failure(L"读取顶层 MP4 box 失败", readError);
            }
            if (readStatus == FileBoxReadStatus::Malformed) {
                return MakeUnsupported(
                    std::move(parseError),
                    HRESULT_FROM_WIN32(ERROR_FILE_INVALID));
            }
            if (box.type == kFtyp) {
                fileTypeFound = true;
            } else if (box.type == kMoov) {
                movieFileBox = box;
                ++movieCount;
            } else if (box.type == kMdat) {
                ++mediaDataCount;
                lastMediaDataEnd = std::max(lastMediaDataEnd, box.End());
            } else if (box.type == kMoof) {
                fragmented = true;
            }
            position = box.End();
        }
        if (position != fileSize) {
            return MakeUnsupported(
                L"顶层 MP4 box 未覆盖完整文件。",
                HRESULT_FROM_WIN32(ERROR_FILE_INVALID));
        }
        if (!fileTypeFound || movieCount != 1 || mediaDataCount == 0) {
            return MakeUnsupported(
                L"仅支持包含单一 moov、至少一个 mdat 和 ftyp 的 MP4。" );
        }
        if (fragmented) {
            return MakeUnsupported(L"fragmented MP4（moof）不受支持。" );
        }
        if (movieFileBox.End() != fileSize) {
            return MakeUnsupported(L"moov 不是文件尾部 box，拒绝修改。" );
        }
        if (lastMediaDataEnd > movieFileBox.offset) {
            return MakeUnsupported(L"要求所有 mdat 位于尾部 moov 之前。" );
        }
        if (movieFileBox.size > kMaximumMoovBytes ||
            movieFileBox.size > std::numeric_limits<std::size_t>::max()) {
            return MakeUnsupported(L"moov 过大，拒绝将其载入内存。" );
        }

        std::vector<Byte> originalMovie(
            static_cast<std::size_t>(movieFileBox.size));
        DWORD readError = ERROR_SUCCESS;
        if (!ReadAt(
                source.Get(),
                movieFileBox.offset,
                originalMovie,
                &readError)) {
            return MakeWin32Failure(L"读取尾部 moov 失败", readError);
        }

        MovieLayout layout{};
        std::wstring structureError;
        if (!AnalyzeMovie(originalMovie, &layout, &structureError)) {
            return MakeUnsupported(
                std::move(structureError),
                HRESULT_FROM_WIN32(ERROR_FILE_INVALID));
        }

        const std::int64_t visibleEnd100Nanoseconds =
            mediaStart100Nanoseconds + visibleDuration100Nanoseconds;
        std::uint64_t mediaStart = 0;
        std::uint64_t mediaEnd = 0;
        if (!ScaleHundredNanoseconds(
                mediaStart100Nanoseconds,
                layout.videoTrack.mediaTime.timescale,
                &mediaStart) ||
            !ScaleHundredNanoseconds(
                visibleEnd100Nanoseconds,
                layout.videoTrack.mediaTime.timescale,
                &mediaEnd)) {
            return MakeUnsupported(L"时间值换算到 MP4 timescale 时溢出。" );
        }
        if (mediaEnd > layout.videoTrack.mediaTime.duration) {
            const std::uint64_t roundingDrift =
                mediaEnd - layout.videoTrack.mediaTime.duration;
            if (roundingDrift > kMaximumMediaEndRoundingDrift) {
                return MakeUnsupported(
                    L"edit list 的可见终点超过 mdhd 解码时长。" );
            }
            mediaEnd = layout.videoTrack.mediaTime.duration;
        }
        if (mediaEnd <= mediaStart) {
            return MakeUnsupported(L"可见时长小于 MP4 timescale 的一个时间单位。" );
        }

        const std::uint64_t visibleMediaDuration = mediaEnd - mediaStart;
        std::uint64_t visibleMovieDuration = 0;
        if (!ScaleTimeUnits(
                visibleMediaDuration,
                layout.videoTrack.mediaTime.timescale,
                layout.movieTime.timescale,
                &visibleMovieDuration)) {
            return MakeUnsupported(
                L"可见时长从媒体 timescale 换算到电影 timescale 时溢出。" );
        }
        if (visibleMovieDuration == 0) {
            return MakeUnsupported(L"可见时长小于 MP4 timescale 的一个时间单位。" );
        }

        std::vector<Byte> patchedMovie;
        bool usedVersion1 = false;
        std::wstring rebuildError;
        if (!RebuildMovie(
                originalMovie,
                layout,
                visibleMovieDuration,
                mediaStart,
                &patchedMovie,
                &usedVersion1,
                &rebuildError)) {
            return MakeUnsupported(
                std::move(rebuildError),
                HRESULT_FROM_WIN32(ERROR_FILE_INVALID));
        }
        if (patchedMovie.empty() || patchedMovie.size() > kMaximumMoovBytes) {
            return MakeUnsupported(L"重建后的 moov 大小不受支持。" );
        }

        DWORD writeError = ERROR_SUCCESS;
        if (!RewriteTail(
                source.Get(),
                movieFileBox.offset,
                patchedMovie,
                &writeError)) {
            DWORD rollbackError = ERROR_SUCCESS;
            const bool restored = RewriteTail(
                source.Get(),
                movieFileBox.offset,
                originalMovie,
                &rollbackError);
            Mp4EditListPatchResult failure = MakeWin32Failure(
                L"原位写入 edit-list moov 失败",
                writeError);
            if (!restored) {
                failure.errorMessage += L"；旧 moov 回滚也失败：" +
                    FormatSystemError(rollbackError);
            }
            return failure;
        }

        Mp4EditListPatchResult result{};
        result.outcome = Mp4EditListPatchOutcome::Succeeded;
        result.nativeError = S_OK;
        result.usedVersion1EditList = usedVersion1;
        result.originalMoovBytes = movieFileBox.size;
        result.patchedMoovBytes = patchedMovie.size();
        return result;
    } catch (const std::bad_alloc&) {
        return MakeFailure(L"edit-list 补丁内存不足。", E_OUTOFMEMORY);
    } catch (const std::exception& exception) {
        std::wstring message = L"edit-list 补丁发生异常：";
        const std::string detail = exception.what();
        message.append(detail.begin(), detail.end());
        return MakeFailure(std::move(message), E_FAIL);
    } catch (...) {
        return MakeFailure(L"edit-list 补丁发生未知异常。", E_FAIL);
    }
}

}  // namespace qrec
