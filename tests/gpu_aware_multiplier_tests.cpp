// Rever VR's GPU-aware dynamic multiplier (include/xrfg/gpu_aware_multiplier.hpp, KB ALI-079): the step up
// on a short real-frame rate or late synthetics, and the probation after a step down.
#include "xrfg/gpu_aware_multiplier.hpp"

#include <cstdint>
#include <iostream>
#include <string_view>

namespace {

int failures = 0;

void expect(bool condition, std::string_view description) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << description << '\n';
    }
}

constexpr std::int64_t kSecond = 1'000'000'000;
constexpr std::int64_t kPoll = 250'000'000;

// The poll that first sees a shape: its window starts here (and is not judged).
void start(xrfg::GpuAwareMultiplier& shape, std::int64_t now, std::uint32_t current, std::uint32_t ceiling = 4,
           double hz = 120.0) {
    static_cast<void>(shape.evaluate(now, current, ceiling, hz));
}

// One window: a second at `fps` real frames with `late` late synthetics, polled every 250 ms (the layer's
// pace); what the closing poll asks for.
std::uint32_t window(xrfg::GpuAwareMultiplier& shape, std::int64_t& now, std::uint32_t current, double fps,
                     std::uint32_t late = 0, std::uint32_t ceiling = 4, double hz = 120.0) {
    std::uint32_t asked = 0;
    shape.count_late(late);
    for (int poll = 0; poll < 4; ++poll) {
        const int frames = static_cast<int>(fps / 4.0 + 0.5);
        for (int frame = 0; frame < frames; ++frame) {
            shape.count_frame();
        }
        now += kPoll;
        asked = shape.evaluate(now, current, ceiling, hz);
        if (poll < 3 && asked != 0) {
            return 1000U;  // a window closed early: the test is out of step
        }
    }
    return asked;
}

}  // namespace

int main() {
    // the Quest's case: 120 Hz at 2X, 54 real a second (target 60, 0.95 of it 57): up to 3X
    {
        xrfg::GpuAwareMultiplier shape;
        std::int64_t now = 0;
        start(shape, now, 2);
        expect(window(shape, now, 2, 54.0) == 0, "the first second of a shape is never judged");
        expect(window(shape, now, 2, 54.0) == 3, "54 of 60 real a second: up to 3X");
        expect(shape.last_real_fps() > 50.0 && shape.last_real_fps() < 58.0, "the measured rate is kept");
    }
    // a full rate never moves; late synthetics alone do
    {
        xrfg::GpuAwareMultiplier shape;
        std::int64_t now = 0;
        start(shape, now, 2);
        window(shape, now, 2, 60.0);
        expect(window(shape, now, 2, 60.0) == 0, "60 of 60: stays");
        expect(window(shape, now, 2, 60.0, 2) == 0, "2 late synthetics: stays");
        expect(window(shape, now, 2, 60.0, 3) == 3, "3 late synthetics in a second: up");
        expect(shape.last_late() == 3, "the late count is kept");
    }
    // never above the ceiling
    {
        xrfg::GpuAwareMultiplier shape;
        std::int64_t now = 0;
        start(shape, now, 4, 4);
        window(shape, now, 4, 20.0, 0, 4);
        expect(window(shape, now, 4, 20.0, 0, 4) == 0, "already at the ceiling: no step");
        start(shape, now, 3, 3);
        window(shape, now, 3, 30.0, 0, 3);
        expect(window(shape, now, 3, 30.0, 0, 3) == 0, "a ceiling of 3: no 4X");
    }
    // probation: a step down that the GPU cannot hold goes back up, and the next waits twice as long
    {
        xrfg::GpuAwareMultiplier shape;
        std::int64_t now = 0;
        start(shape, now, 3);
        window(shape, now, 3, 40.0);
        expect(window(shape, now, 3, 40.0) == 0, "3X at 40 of 40: stays");
        expect(shape.may_step_down(now), "no hold yet");
        shape.stepped_down(3, now);  // the frame-time rule: down to 2X
        start(shape, now, 2);
        expect(window(shape, now, 2, 52.0) == 0, "the step's own second is not judged");
        expect(window(shape, now, 2, 52.0) == 3, "52 of 60 on probation: back to 3X");
        expect(!shape.may_step_down(now), "the next step down waits");
        expect(shape.down_hold_ns() == 6 * kSecond, "the hold doubled to 6 s");
        now += 6 * kSecond;
        expect(shape.may_step_down(now), "after the hold it may try again");
        shape.stepped_down(3, now);
        start(shape, now, 2);
        window(shape, now, 2, 50.0);
        expect(window(shape, now, 2, 50.0) == 3, "failed again: up");
        expect(shape.down_hold_ns() == 12 * kSecond, "12 s");
        for (int fail = 0; fail < 4; ++fail) {
            now += 31 * kSecond;
            shape.stepped_down(3, now);
            start(shape, now, 2);
            window(shape, now, 2, 50.0);
            window(shape, now, 2, 50.0);
        }
        expect(shape.down_hold_ns() == 30 * kSecond, "never more than 30 s");
    }
    // probation passed: stays down, and the hold starts over
    {
        xrfg::GpuAwareMultiplier shape;
        std::int64_t now = 0;
        start(shape, now, 3);
        window(shape, now, 3, 40.0);
        shape.stepped_down(3, now);
        start(shape, now, 2);
        expect(window(shape, now, 2, 60.0) == 0, "the step's own second");
        expect(window(shape, now, 2, 59.0) == 0, "59 of 60 holds");
        expect(window(shape, now, 2, 60.0) == 0, "60 of 60 holds");
        expect(window(shape, now, 2, 60.0) == 0, "probation over");
        expect(!shape.on_probation(), "no longer on probation");
        expect(shape.down_hold_ns() == 3 * kSecond, "the hold as at first");
    }
    // no display rate known: nothing
    {
        xrfg::GpuAwareMultiplier shape;
        std::int64_t now = 0;
        start(shape, now, 2, 4, 0.0);
        window(shape, now, 2, 10.0, 0, 4, 0.0);
        expect(window(shape, now, 2, 10.0, 0, 4, 0.0) == 0, "no display rate: no step");
    }

    if (failures != 0) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "gpu-aware multiplier: all checks passed\n";
    return 0;
}
