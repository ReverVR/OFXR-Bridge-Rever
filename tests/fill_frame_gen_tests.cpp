// Rever VR's fill (xrfg/fill_frame_gen.hpp, 2026-11-16, Assassin's Creed Nexus).
#include "xrfg/fill_frame_gen.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <functional>

namespace {

int g_failures = 0;

void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) {
        ++g_failures;
    }
}

constexpr std::int64_t kPeriod = 8'333'333;  // 120 Hz

// A model of the presenter, in display periods: its runtime wait returns at each whole period (the presenter frame
// count the hold reads), it pops what is queued half a period later - a synthetic, a real frame, or nothing, when it
// repeats the frame before - and the application is admitted with at most one submission queued. The application's wait
// is answered at once with the next period after the presenter's prediction; it renders, ends its frame, and is held
// until the presenter frame count reaches the last release plus `hold` (fill: 1; the other way: the frame's own
// submissions).
struct Outcome {
    int real{};
    int synthetic{};
    int repeats{};
    int alone{};
    int paired{};
    std::size_t deepest{};

    [[nodiscard]] double real_share() const noexcept {
        const int slots = real + synthetic + repeats;
        return slots == 0 ? 0.0 : static_cast<double>(real) / static_cast<double>(slots);
    }
};

constexpr double kPop = 0.5;

Outcome run_model(const std::function<double()>& render, bool hold_for_submissions, std::uint32_t ceiling, int slots) {
    std::deque<bool> queue;  // true: a synthetic
    Outcome out{};
    int next_slot = 0;
    const auto present_until = [&](double time) {
        while (next_slot + kPop <= time && next_slot < slots) {
            if (queue.empty()) {
                ++out.repeats;
            } else {
                (queue.front() ? out.synthetic : out.real) += 1;
                queue.pop_front();
            }
            ++next_slot;
        }
    };
    double released_at = 0.0;
    std::int64_t served = 0;
    std::int64_t previous_handed = -1;
    while (next_slot < slots) {
        const std::int64_t handed = static_cast<std::int64_t>(std::floor(released_at)) + 1;
        double ended = released_at + render();
        present_until(ended);
        while (queue.size() > 1 && next_slot < slots) {
            ended = std::max(ended, next_slot + kPop);
            present_until(next_slot + kPop);
        }
        const std::uint32_t submissions =
            previous_handed < 0 ? 1U : xrfg::fill_submissions((handed - previous_handed) * kPeriod, kPeriod, ceiling);
        for (std::uint32_t synthetic = 1; synthetic < submissions; ++synthetic) {
            queue.push_back(true);
        }
        queue.push_back(false);
        out.deepest = std::max(out.deepest, queue.size());
        (submissions > 1 ? out.paired : out.alone) += 1;
        previous_handed = handed;
        const std::int64_t target = served + (hold_for_submissions ? static_cast<std::int64_t>(submissions) : 1);
        double released = ended;
        if (static_cast<std::int64_t>(std::floor(ended)) < target) {
            released = static_cast<double>(target);
        }
        present_until(released);
        served = static_cast<std::int64_t>(std::floor(released));
        released_at = released;
    }
    return out;
}

// The same model with what the 2026-11-20 lock needs (Half-Life: Alyx at 144 Hz on Rever Streamer's runtime, a real frame
// every five slots for good, KB QPC-095): the admission bound - `admit` submissions queued at most, one in the shallow
// pipeline (3X / 4X), two in the deeper one (2X) - and the readiness hold - a synthetic whose output is not written by its
// pop is kept back and the slot repeats the frame before (V564's 3X / 4X hold where the release counts, and fill's always
// counts). A pair's synthetics are written `ready_base + ready_per_synthetic * (submissions - 1)` periods after it is
// queued: the game's next frame first, then the pair's synthesis. `fill_dynamic` reads the gap on xrfg::fill_interval -
// the interval between the waits capped by the earlier frame's own time. The render callback takes the frame's index.
struct ModelShape {
    std::uint32_t ceiling{2};
    std::size_t admit{1};
    bool readiness_hold{};
    double ready_base{1.1};
    double ready_per_synthetic{0.5};
    bool fill_dynamic{};
    int slots{1440};
    int measure_from{400};
};

struct ModelOutcome {
    int real{};
    int synthetic{};
    int repeats{};
    int alone{};
    int paired{};
    int over_ceiling{};

    [[nodiscard]] double real_share() const noexcept {
        const int slots = real + synthetic + repeats;
        return slots == 0 ? 0.0 : static_cast<double>(real) / static_cast<double>(slots);
    }
};

ModelOutcome run_model_ex(const std::function<double(int)>& render, const ModelShape& shape) {
    struct Queued {
        bool synthetic{};
        double ready_at{};
    };
    std::deque<Queued> queue;
    ModelOutcome out{};
    int next_slot = 0;
    bool shown = false;
    const auto present_until = [&](double time) {
        while (next_slot + kPop <= time && next_slot < shape.slots) {
            const double pop_at = next_slot + kPop;
            const bool counting = next_slot >= shape.measure_from;
            if (queue.empty() ||
                (shape.readiness_hold && shown && queue.front().synthetic && queue.front().ready_at > pop_at)) {
                out.repeats += counting ? 1 : 0;
            } else {
                if (counting) {
                    (queue.front().synthetic ? out.synthetic : out.real) += 1;
                }
                queue.pop_front();
                shown = true;
            }
            ++next_slot;
        }
    };
    double released_at = 0.0;
    std::int64_t served = 0;
    std::int64_t previous_handed = -1;
    double previous_took = 0.0;
    int frame = 0;
    while (next_slot < shape.slots) {
        const std::int64_t handed = static_cast<std::int64_t>(std::floor(released_at)) + 1;
        const double took = render(frame);
        double ended = released_at + took;
        present_until(ended);
        while (queue.size() > shape.admit && next_slot < shape.slots) {
            ended = std::max(ended, next_slot + kPop);
            present_until(next_slot + kPop);
        }
        std::uint32_t submissions = 1;
        std::int64_t gap = 0;
        if (previous_handed >= 0) {
            const std::int64_t interval = xrfg::fill_interval(
                (handed - previous_handed) * kPeriod,
                static_cast<std::int64_t>(previous_took * static_cast<double>(kPeriod)), shape.fill_dynamic);
            gap = xrfg::fill_gap(interval, kPeriod);
            submissions = xrfg::fill_submissions(interval, kPeriod, shape.ceiling);
        }
        const double ready_at =
            ended + shape.ready_base + shape.ready_per_synthetic * static_cast<double>(submissions - 1);
        for (std::uint32_t synthetic = 1; synthetic < submissions; ++synthetic) {
            queue.push_back({true, ready_at});
        }
        queue.push_back({false, ended});
        if (next_slot >= shape.measure_from) {
            (submissions > 1 ? out.paired : out.alone) += 1;
            if (submissions > 1 && gap > static_cast<std::int64_t>(submissions)) {
                ++out.over_ceiling;
            }
        }
        previous_handed = handed;
        previous_took = took;
        double released = ended;
        if (static_cast<std::int64_t>(std::floor(ended)) < served + 1) {
            released = static_cast<double>(served + 1);
        }
        present_until(released);
        served = static_cast<std::int64_t>(std::floor(released));
        released_at = released;
        ++frame;
    }
    return out;
}

// Deterministic, uniform in [low, high).
std::function<double()> uniform(double low, double high, std::uint64_t seed) {
    return [low, high, state = seed]() mutable {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        const double unit = static_cast<double>(state >> 11) / 9007199254740992.0;
        return low + (high - low) * unit;
    };
}

std::function<double()> constant(double value) {
    return [value]() { return value; };
}

}  // namespace

int main() {
    // the gap, rounded to display periods
    {
        check(xrfg::fill_gap(kPeriod, kPeriod) == 1, "one period is a gap of one");
        check(xrfg::fill_gap(kPeriod * 149 / 100, kPeriod) == 1, "1.49 periods round to one");
        check(xrfg::fill_gap(kPeriod * 3 / 2 + 1, kPeriod) == 2, "1.5 periods round to two");
        check(xrfg::fill_gap(2 * kPeriod - 1000, kPeriod) == 2, "just under two periods is two");
        check(xrfg::fill_gap(0, kPeriod) == 0 && xrfg::fill_gap(-kPeriod, kPeriod) == 0, "no forward interval, no gap");
        check(xrfg::fill_gap(kPeriod, 0) == 0, "no period, no gap");
    }
    // the submissions: alone up to a period, then one per period of the gap up to the ceiling
    {
        check(xrfg::fill_submissions(kPeriod, kPeriod, 2) == 1, "kept up: the frame goes alone");
        check(xrfg::fill_submissions(kPeriod / 2, kPeriod, 2) == 1, "two waits in one period: alone");
        check(xrfg::fill_submissions(2 * kPeriod, kPeriod, 2) == 2, "a slot missed at 2X: one synthetic");
        check(xrfg::fill_submissions(3 * kPeriod, kPeriod, 2) == 2, "two slots missed at 2X: still one (the ceiling)");
        check(xrfg::fill_submissions(5 * kPeriod, kPeriod, 2) == 2, "a long gap at 2X: one");
        check(xrfg::fill_submissions(2 * kPeriod, kPeriod, 3) == 2, "a slot missed at 3X: one synthetic");
        check(xrfg::fill_submissions(3 * kPeriod, kPeriod, 3) == 3, "two slots missed at 3X: two");
        check(xrfg::fill_submissions(4 * kPeriod, kPeriod, 3) == 3, "three slots missed at 3X: two (the ceiling)");
        check(xrfg::fill_submissions(2 * kPeriod, kPeriod, 1) == 1, "a ceiling of one makes nothing");
        check(xrfg::fill_submissions(2 * kPeriod, 0, 2) == 1, "no period: alone");
        check(xrfg::fill_submissions(0, kPeriod, 2) == 1, "an interval that does not move forward: alone");
    }
    // the tally
    {
        xrfg::FillTally tally;
        tally.count(1, 1);
        tally.count(1, 0);
        tally.count(2, 2);
        tally.count(2, 4);
        tally.count(3, 3);
        check(tally.primes == 2 && tally.pairs == 3 && tally.synthetics == 4 && tally.over_ceiling == 1,
              "two alone, three paired with four synthetics, one over the ceiling");
    }
    // the model: a game that keeps up makes nothing at all
    {
        const Outcome fast = run_model(uniform(0.6, 0.8, 1), false, 2, 1200);
        check(fast.paired == 0 && fast.synthetic == 0 && fast.real_share() > 0.99,
              "a game of 0.6-0.8 periods: every slot a real frame, nothing invented");
    }
    // a game that misses slots now and then: released every slot it runs at most of the display rate, the synthetics
    // only in the slots it missed; held for its own submissions it locks at half rate - 2X again
    {
        const Outcome released = run_model(uniform(0.85, 1.35, 7), false, 2, 1200);
        const Outcome held = run_model(uniform(0.85, 1.35, 7), true, 2, 1200);
        std::printf("     released every slot: %.2f real, %d paired, %d alone, %d repeats, deepest %zu\n",
                    released.real_share(), released.paired, released.alone, released.repeats, released.deepest);
        std::printf("     held for its submissions: %.2f real, %d paired, %d alone\n", held.real_share(), held.paired,
                    held.alone);
        check(released.real_share() > 0.8, "a game of 0.85-1.35 periods released every slot: over 80 % real");
        check(released.repeats < 12, "and the slots it misses are filled, not repeated");
        check(held.real_share() < 0.6, "held for its own submissions it locks near half rate");
        check(released.deepest <= 3, "the queue stays within the ceiling and the frame admitted");
    }
    // 1.5 periods a frame: alone and paired by turns, two real frames in three slots
    {
        const Outcome middle = run_model(constant(1.5), false, 2, 1200);
        check(std::abs(middle.real_share() - 2.0 / 3.0) < 0.02 && middle.repeats < 4,
              "1.5 periods a frame: two real in three slots, the third filled");
    }
    // a game slower than the ceiling: every frame pairs, the presenter repeats what the ceiling cannot fill
    {
        const Outcome slow = run_model(constant(2.2), false, 2, 1200);
        check(slow.alone <= 1 && slow.repeats > 0, "2.2 periods a frame at 2X: every frame pairs, the rest repeats");
        const Outcome slow_triple = run_model(constant(2.2), false, 3, 1200);
        check(slow_triple.repeats < slow.repeats, "at 3X the second synthetic fills what 2X repeated");
    }
    // Rever VR's fill with the dynamic multiplier (2026-11-20): the switches, off exactly as before
    {
        check(xrfg::fill_in_force(true, false, 4, false) && xrfg::fill_in_force(true, false, 2, false),
              "without fill_dynamic fill is in force at any multiplier, as before");
        check(!xrfg::fill_in_force(false, false, 2, true) && !xrfg::fill_in_force(false, true, 2, true),
              "without fill, never");
        check(xrfg::fill_in_force(true, true, 2, true), "fill_dynamic: in force in its shape at 2X");
        check(!xrfg::fill_in_force(true, true, 2, false), "fill_dynamic: not at 2X outside its shape");
        check(!xrfg::fill_in_force(true, true, 3, true) && !xrfg::fill_in_force(true, true, 4, true),
              "fill_dynamic: never at 3X or 4X");
        check(!xrfg::dynamic_multiplier_steps(true, true, false, true),
              "fill on the presenter without fill_dynamic pins the multiplier, as before");
        check(xrfg::dynamic_multiplier_steps(true, true, false, false) && xrfg::dynamic_multiplier_steps(true, false, false, true),
              "inline, or without fill, the multiplier steps as before");
        check(xrfg::dynamic_multiplier_steps(true, true, true, true), "fill_dynamic keeps the steps on the presenter");
        check(!xrfg::dynamic_multiplier_steps(false, true, true, true), "never without the dynamic multiplier");
        check(xrfg::held_synthetic_counts_late(false, true) && xrfg::held_synthetic_counts_late(false, false),
              "outside fill_dynamic's fill a held synthetic counts late, for age or unwritten, as before");
        check(!xrfg::held_synthetic_counts_late(true, true) && xrfg::held_synthetic_counts_late(true, false),
              "in fill_dynamic's fill a hold for age does not count, one unwritten does");
        check(xrfg::fill_interval(5 * kPeriod, kPeriod * 7 / 10, false) == 5 * kPeriod,
              "without fill_dynamic the interval between the waits, as before");
        check(xrfg::fill_interval(5 * kPeriod, kPeriod * 7 / 10, true) == kPeriod * 7 / 10,
              "fill_dynamic: capped by the earlier frame's own time (the presenter's hold left out)");
        check(xrfg::fill_interval(2 * kPeriod, 5 * kPeriod, true) == 2 * kPeriod,
              "fill_dynamic: a slow earlier frame keeps the interval between the waits");
        check(xrfg::fill_interval(3 * kPeriod, 0, true) == 3 * kPeriod, "fill_dynamic: no own time, the interval");
    }
    // The lock (Half-Life: Alyx at 144 Hz, 2026-11-20): a game of 0.6-0.8 periods, a hitch of two frames of 4.6 (a
    // load), measured well after it. At 4X in the shallow pipeline with the readiness hold - Rever's fichas' ceiling -
    // fill pairs every frame at the ceiling with a repeat: a real frame every five slots, "0 alone, 145 paired (145 pairs
    // over the multiplier 4)" in ofxr_rescue.log. At 2X in the deeper pipeline it alternates a frame alone and a frame
    // paired (Batman's menu, KB QPC-094). With the gap on fill_interval neither locks.
    {
        const auto hitched = [](std::uint64_t seed) {
            return [next = uniform(0.6, 0.8, seed)](int frame) { return frame >= 100 && frame < 102 ? 4.6 : next(); };
        };
        ModelShape quad{};
        quad.ceiling = 4;
        quad.admit = 1;
        quad.readiness_hold = true;
        const ModelOutcome locked = run_model_ex(hitched(11), quad);
        std::printf("     4X shallow after a hitch: %.3f real, %d alone, %d paired, %d over the ceiling, %d repeats\n",
                    locked.real_share(), locked.alone, locked.paired, locked.over_ceiling, locked.repeats);
        check(locked.real_share() < 0.22 && locked.alone == 0 && locked.over_ceiling == locked.paired,
              "4X shallow with the readiness hold: locked at a real frame every five slots, every pair over the ceiling");
        ModelShape quad_fixed = quad;
        quad_fixed.fill_dynamic = true;
        const ModelOutcome unlocked = run_model_ex(hitched(11), quad_fixed);
        check(unlocked.real_share() > 0.95 && unlocked.paired == 0,
              "the same with the gap on fill_interval: back to every slot a real frame");
        ModelShape deep{};
        deep.ceiling = 2;
        deep.admit = 2;
        deep.readiness_hold = true;
        const ModelOutcome alternating = run_model_ex(hitched(11), deep);
        std::printf("     2X deep after a hitch: %.3f real, %d alone, %d paired\n", alternating.real_share(),
                    alternating.alone, alternating.paired);
        check(alternating.real_share() < 0.7 && alternating.alone > 0 && alternating.paired > 0,
              "2X deep: a game that keeps up alternates alone and paired after the hitch");
        ModelShape deep_fixed = deep;
        deep_fixed.fill_dynamic = true;
        const ModelOutcome deep_unlocked = run_model_ex(hitched(11), deep_fixed);
        check(deep_unlocked.real_share() > 0.95 && deep_unlocked.paired == 0,
              "2X deep with the gap on fill_interval: back to every slot a real frame");
        // fill still fills: an isolated frame of 2.2 periods every 40 keeps its synthetic
        const auto spiky = [next = uniform(0.6, 0.8, 5)](int frame) mutable { return frame % 40 == 39 ? 2.2 : next(); };
        const ModelOutcome filled = run_model_ex(spiky, deep_fixed);
        check(filled.paired > 0 && filled.repeats == 0,
              "fill_interval: a slow frame now and then still gets its synthetic, nothing repeated");
    }
    std::printf("%s (%d failures)\n", g_failures == 0 ? "PASS" : "FAILED", g_failures);
    return g_failures == 0 ? 0 : 1;
}
