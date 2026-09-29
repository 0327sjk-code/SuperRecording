#pragma once
// Pure C++ timing policy; all capture streams share one host-time origin and pause offset.
#include <algorithm>
#include <cmath>
namespace sr {
class CaptureClock final {
public:
    void Start(double host) noexcept { origin_=host; pausedTotal_=0; pauseAt_=-1; resumeAt_=host; }
    void Pause(double host) noexcept { if (origin_>=0 && pauseAt_<0) pauseAt_=host; }
    void Resume(double host) noexcept {
        if (pauseAt_>=0) { pausedTotal_+=std::max(0.0,host-pauseAt_); pauseAt_=-1; resumeAt_=host; }
    }
    bool Started() const noexcept { return origin_>=0; }
    bool Paused() const noexcept { return pauseAt_>=0; }
    double Elapsed(double host) const noexcept {
        return Started() ? std::max(0.0,(Paused()?pauseAt_:host)-origin_-pausedTotal_) : 0;
    }
    double SampleTime(double host) const noexcept {
        return !Started() || Paused() || host<resumeAt_ ? -1 : host-origin_-pausedTotal_;
    }
private:
    double origin_{-1}, pausedTotal_{0}, pauseAt_{-1}, resumeAt_{0};
};
}
