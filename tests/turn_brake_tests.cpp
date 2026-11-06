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

    if (failures != 0) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "turn brake: all checks passed\n";
    return 0;
}
