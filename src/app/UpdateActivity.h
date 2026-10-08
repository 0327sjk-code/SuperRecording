#pragma once

// One conservative activity gate shared by timer polling and final install hand-off.
#include "common/Types.h"

namespace qrec {
struct UpdateActivity final {
    RecordingState recordingState{RecordingState::Idle};
    bool editorPresent{};
    bool settingsOrMenuOpen{};
    bool exiting{};
    bool messageWindowAvailable{};

    [[nodiscard]] bool IsIdle() const noexcept {
        return recordingState == RecordingState::Idle && !editorPresent &&
            !settingsOrMenuOpen && !exiting && messageWindowAvailable;
    }
};
}  // namespace qrec
