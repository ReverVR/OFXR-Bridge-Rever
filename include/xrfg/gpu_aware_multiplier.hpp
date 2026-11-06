#pragma once

// Rever VR's GPU-aware dynamic multiplier (2026-11-06, KB ALI-079).
//
// The dynamic multiplier steps on the game's own frame time (its xrWaitFrame returning to its
// xrEndFrame), which is the CPU's side. On the Quest 3 at 120 Hz on the presenter it stayed at 2X with
// frame times of 5.9-9.5 ms while the game delivered 50-58 real frames a second instead of 60: 67.6
// synthetic and 2.1 repeats a second, 24 synthetics unwritten in 19 s - the GPU (the game, DLSS inside
// it and OFXR beside it) was full, and 3X (40 real) fitted. So the shape also looks at what reaches the
// headset, a second at a time:
//   - up one when the real frames stay below (display rate / multiplier) x 0.95 for a whole second, or
//     the presenter held 3 or more synthetics (late: 400 for age, 401 unwritten) in it;
//   - the frame-time rule's step down is let through, then on probation: if the next judged seconds do
//     not hold 0.95 of the new target (or late synthetics come back) it goes up again, and the next step
//     down waits twice as long (3 s, 6, 12, 24, at most 30 s).
// The second after a change is never judged (the switch drains the GPU once). Never above the ceiling.

#include <algorithm>
#include <cstdint>

namespace xrfg {

class GpuAwareMultiplier {
public:
    static constexpr std::int64_t kWindowNs = 1'000'000'000;
    static constexpr std::int64_t kProbationNs = 4'000'000'000;  // the discarded second and three judged
    static constexpr std::int64_t kFirstHoldNs = 3'000'000'000;
    static constexpr std::int64_t kLongestHoldNs = 30'000'000'000;
    static constexpr double kRateMargin = 0.95;
    static constexpr std::uint32_t kLateLimit = 3;

    // One real frame (an application xrEndFrame).
    void count_frame() noexcept { ++window_frames_; }
    // Late synthetics the presenter held since the last call.
    void count_late(std::uint32_t late) noexcept { window_late_ += late; }

    // Whether the frame-time rule may step down now.
    [[nodiscard]] bool may_step_down(std::int64_t now_ns) const noexcept {
        return now_ns >= down_blocked_until_ns_;
    }

    // The frame-time rule stepped down from `from`: the new shape is on probation.
    void stepped_down(std::uint32_t from, std::int64_t now_ns) noexcept {
        probation_from_ = from;
        probation_until_ns_ = now_ns + kProbationNs;
    }

    // Every poll: the multiplier the GPU asks for (above `current`), or 0 for none. `display_hz` is the
    // runtime's rate; `ceiling` the ini's.
    [[nodiscard]] std::uint32_t evaluate(
        std::int64_t now_ns, std::uint32_t current, std::uint32_t ceiling, double display_hz) noexcept {
        if (window_multiplier_ != current || window_start_ns_ < 0) {
            // a new shape: its first second is not judged
            start_window(now_ns, current);
            discard_window_ = true;
            return 0;
        }
        const std::int64_t elapsed = now_ns - window_start_ns_;
        if (elapsed < kWindowNs) {
            return 0;
        }
        last_real_fps_ = static_cast<double>(window_frames_) * 1.0e9 / static_cast<double>(elapsed);
        last_late_ = window_late_;
        const bool discarded = discard_window_;
        discard_window_ = false;
        start_window(now_ns, current);
        if (discarded || !(display_hz > 0.0) || current == 0) {
            return 0;
        }
        const double target = display_hz / static_cast<double>(current);
        const bool short_of_target = last_real_fps_ < target * kRateMargin;
        const bool late = last_late_ >= kLateLimit;
        if (probation_from_ != 0) {
            if (short_of_target || late) {
                const std::uint32_t back = std::min(std::max(probation_from_, current + 1U), ceiling);
                probation_from_ = 0;
                down_hold_ns_ = std::min(down_hold_ns_ * 2, kLongestHoldNs);
                down_blocked_until_ns_ = now_ns + down_hold_ns_;
                return back > current ? back : 0U;
            }
            if (now_ns >= probation_until_ns_) {
                probation_from_ = 0;  // passed: the next step down waits as at first
                down_hold_ns_ = kFirstHoldNs;
            }
            return 0;
        }
        if ((short_of_target || late) && current < ceiling) {
            return current + 1U;
        }
        return 0;
    }

    [[nodiscard]] double last_real_fps() const noexcept { return last_real_fps_; }
    [[nodiscard]] std::uint32_t last_late() const noexcept { return last_late_; }
    [[nodiscard]] std::int64_t down_hold_ns() const noexcept { return down_hold_ns_; }
    [[nodiscard]] bool on_probation() const noexcept { return probation_from_ != 0; }

private:
    void start_window(std::int64_t now_ns, std::uint32_t multiplier) noexcept {
        window_start_ns_ = now_ns;
        window_frames_ = 0;
        window_late_ = 0;
        window_multiplier_ = multiplier;
    }

    std::int64_t window_start_ns_{-1};
    std::uint32_t window_frames_{};
    std::uint32_t window_late_{};
    std::uint32_t window_multiplier_{};
    bool discard_window_{};
    double last_real_fps_{};
    std::uint32_t last_late_{};
    std::uint32_t probation_from_{};
    std::int64_t probation_until_ns_{};
    std::int64_t down_blocked_until_ns_{};
    std::int64_t down_hold_ns_{kFirstHoldNs};
};

}  // namespace xrfg
