// Rever VR's frame generation as a rescue (xrfg/rescue_frame_gen.hpp, 2026-11-16, Assassin's Creed Nexus).
#include "xrfg/rescue_frame_gen.hpp"

#include <cstdint>
#include <cstdio>

namespace {

int g_failures = 0;

void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) {
        ++g_failures;
    }
}

constexpr std::int64_t kPeriod = 8'333'333;  // 120 Hz

struct Clock {
    std::int64_t now{1'000'000'000};
};

// `seconds` of game frames, each `frame_ns` of the game's own (wait returned -> end frame). Pairing, the game is held at
// 2X's pace (two periods between its waits); passing through, its pace is `pass_interval_ns` - the display's when it
// keeps up, longer when it misses slots.
bool run(xrfg::RescueFrameGen& rescue, Clock& clock, double seconds, std::int64_t frame_ns, std::int64_t pass_interval_ns,
         std::uint32_t& changes) {
    const std::int64_t end = clock.now + static_cast<std::int64_t>(seconds * 1e9);
    while (clock.now < end) {
        rescue.on_wait(clock.now);
        bool changed = false;
        rescue.on_end_frame(clock.now + frame_ns, kPeriod, changed);
        if (changed) {
            ++changes;
        }
        clock.now += rescue.passing ? pass_interval_ns : 2 * kPeriod;
    }
    return rescue.passing;
}

}  // namespace

int main() {
    // a game that fits (6 ms of its own a frame), held at 2X's pace: passes through after two seconds and, at the
    // display's pace, stays; a heavy stretch that misses slots pairs it again
    {
        xrfg::RescueFrameGen rescue;
        Clock clock;
        std::uint32_t changes = 0;
        check(!run(rescue, clock, 1.0, 6'000'000, kPeriod, changes), "not before two seconds of fitting");
        check(run(rescue, clock, 1.5, 6'000'000, kPeriod, changes) && changes == 1, "then the frames pass through");
        check(run(rescue, clock, 10.0, 6'000'000, kPeriod, changes) && changes == 1, "at the display's pace it stays");
        check(!run(rescue, clock, 2.0, 10'000'000, 10'000'000, changes) && changes == 2, "missing slots, it pairs again");
        check(rescue.backoff_ns == rescue.backoff_min_ns, "after a long pass-through the next try waits the least");
        check(!run(rescue, clock, 10.0, 10'000'000, 10'000'000, changes) && changes == 2,
              "while its own frame stays heavy it keeps pairing");
        check(run(rescue, clock, 3.5, 6'000'000, kPeriod, changes) && changes == 3, "light again: it passes through again");
    }
    // a game that never fits (12 ms of its own a frame): never passes through
    {
        xrfg::RescueFrameGen rescue;
        Clock clock;
        std::uint32_t changes = 0;
        check(!run(rescue, clock, 10.0, 12'000'000, kPeriod, changes) && changes == 0, "a heavy game always pairs");
    }
    // a game whose own frame fits but whose pace cannot hold (11 ms between frames passing through): each failed try
    // doubles the wait before the next (6, 12 s ...)
    {
        xrfg::RescueFrameGen rescue;
        Clock clock;
        std::uint32_t changes = 0;
        check(run(rescue, clock, 2.5, 6'000'000, 11'000'000, changes), "it tries");
        run(rescue, clock, 1.5, 6'000'000, 11'000'000, changes);
        check(!rescue.passing && rescue.backoff_ns == 6'000'000'000, "a failed try: the next waits six seconds");
        const std::int64_t failed_at = rescue.changed_at_ns;
        run(rescue, clock, static_cast<double>(failed_at + 5'900'000'000 - clock.now) / 1e9, 6'000'000, 11'000'000,
            changes);
        check(!rescue.passing, "not within the wait");
        run(rescue, clock, 0.3, 6'000'000, 11'000'000, changes);
        check(rescue.passing && rescue.changed_at_ns - failed_at >= 6'000'000'000, "it tries again after it");
        run(rescue, clock, 1.5, 6'000'000, 11'000'000, changes);
        check(!rescue.passing && rescue.backoff_ns == 12'000'000'000, "failing again: twelve seconds");
        for (int i = 0; i < 6; ++i) {
            run(rescue, clock, 40.0, 6'000'000, 11'000'000, changes);
        }
        check(rescue.backoff_ns == rescue.backoff_max_ns, "never more than thirty seconds");
    }
    // a pass-through that wavers just over the display's period (a short hitch): one change at most a second
    {
        xrfg::RescueFrameGen rescue;
        Clock clock;
        std::uint32_t changes = 0;
        run(rescue, clock, 2.5, 6'000'000, kPeriod, changes);
        const std::uint32_t before = changes;
        run(rescue, clock, 0.5, 6'000'000, 11'000'000, changes);
        check(changes == before && rescue.passing, "no change within a second of the last");
    }
    // no period known: nothing changes
    {
        xrfg::RescueFrameGen rescue;
        bool changed = true;
        rescue.on_wait(1'000'000'000);
        check(!rescue.on_end_frame(1'006'000'000, 0, changed) && !changed, "no period, no change");
    }
    std::printf(g_failures ? "FAILED: %d\n" : "rescue frame gen: all checks passed\n", g_failures);
    return g_failures ? 1 : 0;
}
