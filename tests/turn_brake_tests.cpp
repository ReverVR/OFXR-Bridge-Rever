// Rever VR's turn brake (include/xrfg/turn_brake.hpp, KB ALI-079): the speed between two real frames'
// orientations and the hysteresis that holds it.
#include "xrfg/turn_brake.hpp"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <numbers>
#include <string_view>

namespace {

int failures = 0;

void expect(bool condition, std::string_view description) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << description << '\n';
    }
}

[[nodiscard]] xrfg::Quaternion yaw(float degrees) {
    const float half_angle = degrees * std::numbers::pi_v<float> / 360.0F;
    return {0.0F, std::sin(half_angle), 0.0F, std::cos(half_angle)};
}

constexpr std::int64_t kFrame = 20'833'333;  // 48 real frames a second (144 Hz at 3X)

}  // namespace

int main() {
    // speed: 5 degrees in 50 ms is 100 deg/s; the quaternion's sign never matters
    const float speed = xrfg::angular_speed_deg_per_s(yaw(0.0F), yaw(5.0F), 50'000'000);
    expect(std::abs(speed - 100.0F) < 0.5F, "5 deg in 50 ms is 100 deg/s");
    const auto flipped = yaw(5.0F);
    const float flipped_speed = xrfg::angular_speed_deg_per_s(
        yaw(0.0F), {-flipped.x, -flipped.y, -flipped.z, -flipped.w}, 50'000'000);
    expect(std::abs(flipped_speed - 100.0F) < 0.5F, "the other sign of one rotation, the same speed");
    expect(xrfg::angular_speed_deg_per_s(yaw(0.0F), yaw(5.0F), 0) < 0.0F, "no time between: no speed");
    expect(xrfg::angular_speed_deg_per_s(yaw(0.0F), yaw(5.0F), 100) < 0.0F, "100 ns apart: not a display timeline");
    expect(xrfg::angular_speed_deg_per_s(yaw(0.0F), yaw(5.0F), 500'000'000) < 0.0F, "a long gap: no speed");
    expect(xrfg::angular_speed_deg_per_s(yaw(0.0F), {0.0F, 0.0F, 0.0F, 0.0F}, kFrame) < 0.0F,
           "a zero quaternion: no speed");

    // still and slow turns never engage
    xrfg::TurnBrake brake{};
    std::int64_t time = 0;
    for (int frame = 0; frame < 48; ++frame) {
        time += kFrame;
        expect(!brake.update(40.0F, time), "40 deg/s stays off");
    }
    // a fast turn engages at the threshold (the light smoothing: two frames at 150 from 40)
    time += kFrame;
    brake.update(150.0F, time);
    time += kFrame;
    expect(brake.update(150.0F, time), "150 deg/s engages");
    const std::int64_t engaged_at = brake.engaged_at_ns;
    // wavering around the threshold, above its release, holds
    for (int frame = 0; frame < 10; ++frame) {
        time += kFrame;
        expect(brake.update(frame % 2 ? 85.0F : 60.0F, time), "60-85 deg/s (above 54) holds");
    }
    // stopping dead releases only after the hold (150 ms) from engaging
    xrfg::TurnBrake quick{};
    quick.update(200.0F, kFrame);
    expect(quick.engaged, "200 deg/s engages at once");
    // the smoothing halves 200 -> 100 -> 50: still above the release (54) on the first still frame...
    expect(quick.update(0.0F, kFrame + 2 * kFrame), "100 deg/s smoothed after one still frame: on");
    // ...below it on the second, but 62 ms after engaging: the hold keeps it
    expect(quick.update(0.0F, kFrame + 3 * kFrame), "within 150 ms of engaging it holds");
    expect(!quick.update(0.0F, kFrame + 8 * kFrame), "after the hold, still, it releases");
    expect(engaged_at > 0, "the engaging frame's time is kept");
    // a frame without a speed keeps the state
    xrfg::TurnBrake keep{};
    keep.update(200.0F, kFrame);
    expect(keep.update(-1.0F, 2 * kFrame), "no speed: still engaged");
    keep.reset();
    expect(!keep.engaged && keep.smoothed_deg_per_s == 0.0F, "reset clears it");
    // the threshold is live
    xrfg::TurnBrake strict{};
    strict.threshold_deg_per_s = 300.0F;
    expect(!strict.update(200.0F, kFrame), "200 deg/s with a 300 threshold stays off");

    // KB ALI-082: the defaults are cb142461's (the Quest's), and the Steam Frame's tuning (450 ms, 40 %)
    {
        const xrfg::TurnBrake defaults{};
        expect(defaults.threshold_deg_per_s == 90.0F && defaults.release_fraction == 0.6F &&
                   defaults.hold_ns == 150'000'000,
               "the defaults: 90 deg/s, 60 %, 150 ms");
        expect(static_cast<float>(60U) / 100.0F == 0.6F, "60 % read live is the very 0.6F of before");
        // the Frame's 21:17 turns (on and off every 0.2-0.3 s): a quick turn, then ~0.2 s slower
        const float wobble[] = {130.0F, 130.0F, 40.0F, 40.0F, 40.0F, 40.0F, 40.0F, 40.0F, 40.0F, 40.0F};
        xrfg::TurnBrake today{};
        xrfg::TurnBrake frame{};
        frame.release_fraction = static_cast<float>(40U) / 100.0F;
        frame.hold_ns = 450'000'000;
        int today_changes = 0;
        int frame_changes = 0;
        bool today_was = false;
        bool frame_was = false;
        std::int64_t at = 0;
        for (int lap = 0; lap < 4; ++lap) {
            for (const float turn : wobble) {
                at += kFrame;
                const bool today_on = today.update(turn, at);
                const bool frame_on = frame.update(turn, at);
                today_changes += today_on != today_was ? 1 : 0;
                frame_changes += frame_on != frame_was ? 1 : 0;
                today_was = today_on;
                frame_was = frame_on;
            }
        }
        expect(today_changes >= 4, "the defaults flap on the 21:17 turns (released below 54 deg/s)");
        expect(frame_changes <= 1, "450 ms / 40 % stay on through them");
        // the release: 45 deg/s after the hold lets go at 60 % (54), not at 40 % (36)
        xrfg::TurnBrake release60{};
        xrfg::TurnBrake release40{};
        release40.release_fraction = 0.4F;
        release60.update(200.0F, kFrame);
        release40.update(200.0F, kFrame);
        for (int frame_index = 2; frame_index < 20; ++frame_index) {
            release60.update(45.0F, frame_index * kFrame);
            release40.update(45.0F, frame_index * kFrame);
        }
        expect(!release60.engaged, "45 deg/s: released at 60 %");
        expect(release40.engaged, "45 deg/s: held at 40 %");
        // the hold: still after engaging, 150 ms lets go by 200 ms, 450 ms not before 450
        xrfg::TurnBrake hold150{};
        xrfg::TurnBrake hold450{};
        hold450.hold_ns = 450'000'000;
        hold150.update(200.0F, kFrame);
        hold450.update(200.0F, kFrame);
        for (int frame_index = 2; frame_index <= 11; ++frame_index) {  // ~208 ms later
            hold150.update(0.0F, frame_index * kFrame);
            hold450.update(0.0F, frame_index * kFrame);
        }
        expect(!hold150.engaged, "still for 200 ms: 150 ms let go");
        expect(hold450.engaged, "still for 200 ms: 450 ms holds");
        for (int frame_index = 12; frame_index <= 24; ++frame_index) {  // ~480 ms after engaging
            hold450.update(0.0F, frame_index * kFrame);
        }
        expect(!hold450.engaged, "after 450 ms it lets go");
    }

    if (failures != 0) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "turn brake: all checks passed\n";
    return 0;
}
