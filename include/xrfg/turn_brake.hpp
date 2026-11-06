#pragma once

// Rever VR's turn brake (2026-11-06, KB ALI-079; the user: "ve haciendo el freno para ambas versiones").
//
// On a fast head turn an interpolated frame misjudges what the turn uncovers ("desencaje" at 144 Hz 3X:
// two of every three frames invented over 21 ms gaps, ALI-073), while the headset's own reprojection
// turns a real frame exactly. So while the head turns faster than a threshold the layer makes no
// synthetic: each real frame is primed (its history kept, nothing invented) and the presenter or the
// runtime shows it again in the synthetic's slots, turned by the headset. Normal play is unchanged.
//
// The speed is the angle between two real frames' view orientations over their display times, lightly
// smoothed; on at the threshold, off below a fraction of it once a minimum hold has passed, so a turn
// that wavers around the threshold does not flap between invented and repeated frames.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>

#include "xrfg/pose.hpp"

namespace xrfg {

// Degrees a second between two orientations shown `elapsed_ns` apart; negative when no speed can be
// told: real frames under 2 ms apart (no headset shows 500 a second: not a display timeline), or a gap so
// long the two frames are not one movement.
[[nodiscard]] inline float angular_speed_deg_per_s(
    Quaternion previous, Quaternion current, std::int64_t elapsed_ns) noexcept {
    if (elapsed_ns < 2'000'000 || elapsed_ns > 200'000'000) {
        return -1.0F;
    }
    const float previous_length = std::sqrt(previous.x * previous.x + previous.y * previous.y +
                                            previous.z * previous.z + previous.w * previous.w);
    const float current_length = std::sqrt(current.x * current.x + current.y * current.y +
                                           current.z * current.z + current.w * current.w);
    if (!(previous_length > 0.0F) || !(current_length > 0.0F)) {
        return -1.0F;
    }
    const float dot = std::abs(previous.x * current.x + previous.y * current.y + previous.z * current.z +
                               previous.w * current.w) /
                      (previous_length * current_length);
    const float angle_deg = 2.0F * std::acos(std::min(1.0F, dot)) * 180.0F / std::numbers::pi_v<float>;
    if (!std::isfinite(angle_deg)) {
        return -1.0F;
    }
    return angle_deg / (static_cast<float>(elapsed_ns) * 1.0e-9F);
}

struct TurnBrake {
    // On at this speed (the live `turn_brake_deg`, 90 by default).
    float threshold_deg_per_s{90.0F};
    // Off below this fraction of it (the live turn_brake_release, 60 % by default)...
    float release_fraction{0.6F};
    // ...and never sooner than this after it came on (the live turn_brake_hold_ms, 150 by default).
    std::int64_t hold_ns{150'000'000};
    float smoothed_deg_per_s{};
    bool engaged{};
    std::int64_t engaged_at_ns{};

    // One real frame's speed at its display time; whether the brake holds that frame.
    bool update(float speed_deg_per_s, std::int64_t display_time_ns) noexcept {
        if (!(speed_deg_per_s >= 0.0F) || !std::isfinite(speed_deg_per_s)) {
            return engaged;  // no speed for this frame: as it was
        }
        smoothed_deg_per_s = smoothed_deg_per_s <= 0.0F
            ? speed_deg_per_s
            : smoothed_deg_per_s * 0.5F + speed_deg_per_s * 0.5F;
        if (!engaged) {
            if (smoothed_deg_per_s >= threshold_deg_per_s) {
                engaged = true;
                engaged_at_ns = display_time_ns;
            }
        } else if (smoothed_deg_per_s < threshold_deg_per_s * release_fraction &&
                   display_time_ns - engaged_at_ns >= hold_ns) {
            engaged = false;
        }
        return engaged;
    }

    void reset() noexcept {
        smoothed_deg_per_s = 0.0F;
        engaged = false;
        engaged_at_ns = 0;
    }
};

}  // namespace xrfg
