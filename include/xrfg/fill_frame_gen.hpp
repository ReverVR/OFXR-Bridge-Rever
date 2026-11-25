#pragma once

// Rever VR's fill (OFXR_REVER_FILL=1, or [ofxr] fill=1; 2026-11-16, Assassin's Creed Nexus on Rever Streamer's runtime,
// Steam Frame at 120 Hz: the game alone ran at ~100-119 real frames a second and OFXR at 2X held it at 60). On the
// continuous presenter the application is handed the display's own period and released once a display period, so it
// runs at up to the display rate; a slot gets a synthetic only when the game did not bring a new frame for it, as the
// runtime's own SSW does. The multiplier (2 today) becomes the ceiling: at most multiplier - 1 synthetics a frame.
//
// The decision, per real frame at its xrEndFrame: the gap to the previous real frame in display periods, rounded, from
// the presenter's predictions at the two frames' waits (where the true display time's ring still has them; else from
// the display times the application was handed - with the true display time on these also carry its lead, which moves,
// and a lead grown by a period would read as a missed slot). One period or less - the game
// kept up - and the frame goes alone, primed (its history kept, nothing invented, in its own slot), as the turn brake's
// frames do. Two or more and it pairs, with min(gap, ceiling) - 1 synthetics shown in the slots before it, each at the
// instant of the slot it is shown in (synthetic_interpolation_fraction with that many submissions: (i + 1) / gap when
// every slot of the gap is filled).
//
// The application is released once a presenter frame whatever this frame made (wait_for_presenter_pair). Holding it for
// the slots its own frame fills - two for a pair - makes the next frame's gap two, which pairs it, which holds it for
// two: one frame that spans two slots locks the game at half rate for good, which is 2X again (the model of the
// presenter in tests/fill_frame_gen_tests.cpp: a game of 0.85-1.35 periods a frame shows 50 % real frames held that way,
// 90 % released every slot). Released every slot, the synthetics a frame makes are the slots the frame before it took,
// so the queue does not grow - nor shrink: nothing drains it, and a game that keeps up keeps the depth it had, at most
// the presenter's admission bound (the paced test runtime at 100 Hz, a game of 3 ms: 100 real frames a second against
// 2X's 50, each shown four presenter frames after its wait in the deeper pipeline, three in the shallow one - as at 2X).

#include <algorithm>
#include <cstdint>

namespace xrfg {

// The gap from the previous real frame to this one in display periods, rounded; 0 without a period or a forward
// interval.
[[nodiscard]] constexpr std::int64_t fill_gap(std::int64_t interval_ns, std::int64_t period_ns) noexcept {
    if (period_ns <= 0 || interval_ns <= 0) {
        return 0;
    }
    return (interval_ns + period_ns / 2) / period_ns;
}

// The submissions a real frame takes under fill: 1 (the frame alone) when it follows the previous real frame by one
// display period or less, else one per period of the gap up to the ceiling - the synthetics and the frame.
[[nodiscard]] constexpr std::uint32_t fill_submissions(
    std::int64_t interval_ns, std::int64_t period_ns, std::uint32_t ceiling) noexcept {
    const std::int64_t gap = fill_gap(interval_ns, period_ns);
    if (gap <= 1 || ceiling < 2) {
        return 1;
    }
    return static_cast<std::uint32_t>(std::min<std::int64_t>(gap, static_cast<std::int64_t>(ceiling)));
}

// Rever VR's fill with the dynamic multiplier (OFXR_REVER_FILL_DYNAMIC=1, or [ofxr] fill_dynamic=1; 2026-11-20,
// Half-Life: Alyx on Rever Streamer's runtime, Steam Frame at 144 Hz, ofxr_rescue.log: "0 alone, 145 paired, 435
// synthetics (145 pairs over the multiplier 4)" every five seconds - 28.8 real frames a second, 24.0 at 120 Hz, from a
// game that ran 108-144 by itself on the same headset). Without the switch fill pins the multiplier to the ini's (Rever's
// fichas write their ceiling there, 4) and switches the dynamic steps off. At 3X and 4X the presenter runs the shallow
// pipeline: a frame is admitted once the previous pair's synthetics have retired, the application is released by a count
// of presenter frames, and that takes the V564 readiness hold - the pair's first synthetic, its synthesis queued behind
// the game's next frame, is not written when its slot comes and the slot repeats the frame before. So a frame that paired
// at the ceiling makes the next one's gap the ceiling plus that slot, and it pairs at the ceiling again: one heavy moment
// (a load) locks the game at a real frame every ceiling + 1 slots for the rest of the session. The deeper pipeline at 2X
// has its own form of it (tests/fill_frame_gen_tests.cpp): the gap between two waits holds the time the presenter kept
// the earlier frame in its xrEndFrame, so after a hitch a game that keeps up alternates a frame alone and a frame paired
// for good - Batman's menu, KB QPC-094.
//
// With the switch:
// - the multiplier keeps its dynamic steps (the game's own frame time, 701; the GPU-aware rule, 705), 2X up to the ini's;
// - fill is in force only in its own shape at 2X (the deeper pipeline it was built on), taken while the game keeps up
//   with the display by itself and left once it does not, by the rescue's rule (xrfg/rescue_frame_gen.hpp: its own frame
//   under 85 % of the period for 2 s; its pace over 108 %; a try that fails early doubles the wait, 3 s to 30 s);
// - elsewhere frames pair as without fill: handed the multiplier's period, released at their pop - Alien's 2X at 120 Hz
//   and 3X at 144 Hz, Assassin's 2X at 144 Hz (KB ALI-072 / ALI-073, QPC-079);
// - in fill's shape a frame's gap is read on fill_interval below, so the presenter's own hold is never a missed slot.
[[nodiscard]] constexpr bool fill_in_force(
    bool fill_enabled, bool fill_dynamic, std::uint32_t multiplier, bool fill_shape) noexcept {
    return fill_enabled && (!fill_dynamic || (multiplier <= 2U && fill_shape));
}

// The interval fill rounds into a gap. As before, the one between the two frames' waits on the presenter's grid. Under
// fill_dynamic never more than the earlier frame's own time (its xrWaitFrame returning to its xrEndFrame): that interval
// is the earlier frame's own time plus the time the presenter held it in xrEndFrame - the admission bound and the hold,
// the presenter's own backlog - which is not a slot the game missed.
[[nodiscard]] constexpr std::int64_t fill_interval(
    std::int64_t between_waits_ns, std::int64_t previous_own_ns, bool fill_dynamic) noexcept {
    if (!fill_dynamic || previous_own_ns <= 0 || between_waits_ns <= 0) {
        return between_waits_ns;
    }
    return std::min(between_waits_ns, previous_own_ns);
}

// Whether the dynamic multiplier takes its steps: everywhere it is on, except under fill on the presenter without
// fill_dynamic, where fill pins it to the ini's.
[[nodiscard]] constexpr bool dynamic_multiplier_steps(
    bool dynamic_frame_gen, bool fill_enabled, bool fill_dynamic, bool presenter) noexcept {
    return dynamic_frame_gen && !(fill_enabled && presenter && !fill_dynamic);
}

// Whether a synthetic the presenter held back counts as late for the GPU-aware rule (ALI-079). In fill_dynamic's fill
// shape a synthetic held for its age (400) is the deeper pipeline re-phasing after frames that went alone, by design, not
// the GPU falling behind; one held unwritten (401) still counts. Everywhere else, as before: both count.
[[nodiscard]] constexpr bool held_synthetic_counts_late(bool fill_dynamic_in_force, bool held_for_age) noexcept {
    return !(fill_dynamic_in_force && held_for_age);
}

// A second of fill's decisions, for the flight log (881) and Rever's note.
struct FillTally {
    std::uint32_t primes{};
    std::uint32_t pairs{};
    std::uint32_t synthetics{};
    // Pairs whose gap was over the ceiling: slots the presenter repeated the frame before instead.
    std::uint32_t over_ceiling{};

    void count(std::uint32_t submissions, std::int64_t gap) noexcept {
        if (submissions <= 1) {
            ++primes;
            return;
        }
        ++pairs;
        synthetics += submissions - 1;
        if (gap > static_cast<std::int64_t>(submissions)) {
            ++over_ceiling;
        }
    }
};

}  // namespace xrfg
