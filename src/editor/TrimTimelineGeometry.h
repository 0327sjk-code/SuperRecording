#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>

namespace qrec::timeline_detail {

enum class HitTarget : unsigned char {
    None,
    Start,
    End,
    Playhead,
};

struct TrackGeometry final {
    RECT bounds{};
    int centerY{};
};

struct TimelineGeometry final {
    TrackGeometry track{};
    RECT startHandle{};
    RECT endHandle{};
    int playheadX{};
    int playheadTop{};
    int playheadBottom{};
    RECT playheadMarkerHitZone{};
    RECT playheadLineHitZone{};
    RECT startHandleHitZone{};
    RECT endHandleHitZone{};
};

[[nodiscard]] inline int ScaleDip(const UINT dpi, const int value) noexcept {
    const UINT safeDpi = dpi == 0 ? USER_DEFAULT_SCREEN_DPI : dpi;
    return ::MulDiv(value, static_cast<int>(safeDpi), USER_DEFAULT_SCREEN_DPI);
}

[[nodiscard]] inline TrackGeometry BuildTrackGeometry(
    const int clientWidth,
    const int clientHeight,
    const UINT dpi) noexcept {
    const int trackLeft = ScaleDip(dpi, 24);
    const int trackRight = std::max(trackLeft + 1, clientWidth - ScaleDip(dpi, 24));
    const int trackTop = ScaleDip(dpi, 36);
    const int trackBottom = std::min(
        std::max(trackTop + ScaleDip(dpi, 1), clientHeight - ScaleDip(dpi, 12)),
        trackTop + ScaleDip(dpi, 18));
    return {{trackLeft, trackTop, trackRight, trackBottom}, (trackTop + trackBottom) / 2};
}

[[nodiscard]] inline RECT CenteredRectangle(
    const int centerX,
    const int centerY,
    const int width,
    const int height) noexcept {
    const int safeWidth = std::max(1, width);
    const int safeHeight = std::max(1, height);
    const int left = centerX - safeWidth / 2;
    const int top = centerY - safeHeight / 2;
    return {left, top, left + safeWidth, top + safeHeight};
}

[[nodiscard]] inline RECT ExpandHorizontally(
    const RECT bounds,
    const int amount) noexcept {
    return {bounds.left - amount, bounds.top, bounds.right + amount, bounds.bottom};
}

[[nodiscard]] inline TimelineGeometry BuildTimelineGeometry(
    const TrackGeometry& track,
    const int clientHeight,
    const UINT dpi,
    const int startX,
    const int endX,
    const int playheadX) noexcept {
    TimelineGeometry geometry{};
    geometry.track = track;

    const int trackHeight = std::max(
        1,
        static_cast<int>(geometry.track.bounds.bottom - geometry.track.bounds.top));
    const int availableHandleHeight = std::max(1, clientHeight - ScaleDip(dpi, 24));
    const int handleHeight = std::max(
        1,
        std::min(availableHandleHeight, trackHeight + ScaleDip(dpi, 18)));
    const int handleWidth = std::max(1, ScaleDip(dpi, 14));
    geometry.startHandle = CenteredRectangle(
        startX,
        geometry.track.centerY,
        handleWidth,
        handleHeight);
    geometry.endHandle = CenteredRectangle(
        endX,
        geometry.track.centerY,
        handleWidth,
        handleHeight);

    geometry.playheadX = playheadX;
    geometry.playheadTop = geometry.track.bounds.top - ScaleDip(dpi, 8);
    geometry.playheadBottom = geometry.track.bounds.bottom + ScaleDip(dpi, 6);

    const int markerHalfWidth = std::max(1, ScaleDip(dpi, 10));
    geometry.playheadMarkerHitZone = {
        playheadX - markerHalfWidth,
        geometry.playheadTop - ScaleDip(dpi, 1),
        playheadX + markerHalfWidth,
        geometry.track.bounds.top};

    const int lineHalfWidth = std::max(1, ScaleDip(dpi, 4));
    geometry.playheadLineHitZone = {
        playheadX - lineHalfWidth,
        geometry.playheadTop,
        playheadX + lineHalfWidth,
        geometry.playheadBottom};

    const int handleHorizontalSlop = std::max(1, ScaleDip(dpi, 3));
    geometry.startHandleHitZone = ExpandHorizontally(
        geometry.startHandle,
        handleHorizontalSlop);
    geometry.endHandleHitZone = ExpandHorizontally(
        geometry.endHandle,
        handleHorizontalSlop);
    return geometry;
}

[[nodiscard]] inline TimelineGeometry BuildTimelineGeometry(
    const int clientWidth,
    const int clientHeight,
    const UINT dpi,
    const int startX,
    const int endX,
    const int playheadX) noexcept {
    return BuildTimelineGeometry(
        BuildTrackGeometry(clientWidth, clientHeight, dpi),
        clientHeight,
        dpi,
        startX,
        endX,
        playheadX);
}

[[nodiscard]] inline bool ContainsPointInclusive(
    const RECT bounds,
    const POINT point) noexcept {
    return point.x >= bounds.left && point.x <= bounds.right &&
        point.y >= bounds.top && point.y <= bounds.bottom;
}

[[nodiscard]] inline HitTarget HitTestTimeline(
    const TimelineGeometry& geometry,
    const POINT point) noexcept {
    if (ContainsPointInclusive(geometry.playheadMarkerHitZone, point)) {
        return HitTarget::Playhead;
    }

    const bool hitsStart = ContainsPointInclusive(geometry.startHandleHitZone, point);
    const bool hitsEnd = ContainsPointInclusive(geometry.endHandleHitZone, point);
    if (hitsStart && hitsEnd) {
        const int startDistance = std::abs(point.x -
            (geometry.startHandle.left + geometry.startHandle.right) / 2);
        const int endDistance = std::abs(point.x -
            (geometry.endHandle.left + geometry.endHandle.right) / 2);
        return startDistance <= endDistance ? HitTarget::Start : HitTarget::End;
    }
    if (hitsStart) {
        return HitTarget::Start;
    }
    if (hitsEnd) {
        return HitTarget::End;
    }
    if (ContainsPointInclusive(geometry.playheadLineHitZone, point)) {
        return HitTarget::Playhead;
    }
    return HitTarget::None;
}

}  // namespace qrec::timeline_detail
