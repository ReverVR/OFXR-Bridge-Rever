// Rever VR's true display time (xrfg/true_display_time.hpp, 2026-11-16, Assassin's Creed Nexus).
#include "xrfg/true_display_time.hpp"

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
constexpr std::int64_t kVirtual = 2 * kPeriod;  // 2X

// One application frame: its wait at the presenter's prediction `presenter`, handed `presenter + handed_lead`, and its
// real frame popped into the slot `presenter + delay`.
void frame(xrfg::TrueDisplayTime& time, std::int64_t presenter, std::int64_t handed_lead, std::int64_t delay) {
    const std::int64_t handed = presenter + handed_lead;
    time.note_handed(handed, presenter);
    time.note_presented(handed, presenter + delay);
}

}  // namespace

int main() {
    // nothing is used before eight frames are measured
    {
        xrfg::TrueDisplayTime time;
        std::int64_t presenter = 1'000'000'000;
        for (int index = 0; index < 7; ++index) {
            frame(time, presenter, kVirtual, 4 * kPeriod);
            presenter += kVirtual;
        }
        check(!time.measured() && !time.lead(presenter, kVirtual, kPeriod), "unmeasured under eight frames");
        frame(time, presenter, kVirtual, 4 * kPeriod);
        presenter += kVirtual;
        check(time.measured() && *time.measured() == 4 * kPeriod, "eight frames: the delay is measured");
        check(time.lead(presenter, kVirtual, kPeriod) && *time.lead(presenter, kVirtual, kPeriod) == 4 * kPeriod,
              "the wait hands out the delay measured (four periods at 2X, not two)");
        check(time.matched == 8 && time.unmatched == 0, "every frame matched its handed time");
    }
    // the middle half: two late frames among sixteen do not move it
    {
        xrfg::TrueDisplayTime time;
        std::int64_t presenter = 1'000'000'000;
        for (int index = 0; index < 16; ++index) {
            frame(time, presenter, kVirtual, index == 3 || index == 11 ? 10 * kPeriod : 4 * kPeriod);
            presenter += kVirtual;
        }
        check(*time.measured() == 4 * kPeriod, "two late frames in sixteen are trimmed");
        // a split between four and five periods lands between them
        for (int index = 0; index < 16; ++index) {
            frame(time, presenter, kVirtual, (index % 2) == 0 ? 4 * kPeriod : 5 * kPeriod);
            presenter += kVirtual;
        }
        const std::int64_t split = *time.measured();
        check(split > 4 * kPeriod && split < 5 * kPeriod, "a split between four and five periods lands between them");
    }
    // the clamp: never less than one virtual period, never more than six periods past it
    {
        xrfg::TrueDisplayTime early;
        xrfg::TrueDisplayTime late;
        std::int64_t presenter = 1'000'000'000;
        for (int index = 0; index < 16; ++index) {
            frame(early, presenter, kVirtual, kPeriod);
            frame(late, presenter, kVirtual, 12 * kPeriod);
            presenter += kVirtual;
        }
        check(*early.lead(presenter, kVirtual, kPeriod) == kVirtual, "a delay under a virtual period hands one out");
        check(*late.lead(presenter, kVirtual, kPeriod) == kVirtual + 6 * kPeriod,
              "a delay past six periods more is held there");
        check(*early.lead(presenter, kPeriod, kPeriod) == kPeriod, "fill's virtual period is the display's");
        check(!early.lead(presenter, 0, kPeriod) && !early.lead(presenter, kVirtual, 0), "no period, no lead");
    }
    // a time the application made up does not match; a delay that is not a pipeline is not taken
    {
        xrfg::TrueDisplayTime time;
        time.note_handed(2'000'000'000, 1'990'000'000);
        check(!time.note_presented(2'000'000'001, 2'020'000'000) && time.unmatched == 1, "an unknown time is unmatched");
        check(!time.note_presented(2'000'000'000, 1'980'000'000) && time.unmatched == 2,
              "a slot before the prediction is not a delay");
        check(!time.note_presented(2'000'000'000, 3'500'000'000) && time.unmatched == 3,
              "a second and a half is not a delay");
        check(time.note_presented(2'000'000'000, 2'030'000'000) && time.last_delay == 40'000'000,
              "the handed time matches");
    }
    // the ring reaches back 64 handed times
    {
        xrfg::TrueDisplayTime time;
        for (std::int64_t index = 0; index < 100; ++index) {
            time.note_handed(1'000'000'000 + index * kPeriod, 990'000'000 + index * kPeriod);
        }
        check(!time.note_presented(1'000'000'000, 1'050'000'000), "a time handed 100 waits ago is out of reach");
        check(time.note_presented(1'000'000'000 + 40 * kPeriod, 1'050'000'000 + 40 * kPeriod),
              "one handed 60 waits ago is found");
    }
    // stale: half a second without a measured frame (a menu passed through) and the wait goes back to one virtual
    // period until frames are measured again
    {
        xrfg::TrueDisplayTime time;
        std::int64_t presenter = 1'000'000'000;
        for (int index = 0; index < 16; ++index) {
            frame(time, presenter, kVirtual, 4 * kPeriod);
            presenter += kVirtual;
        }
        check(time.lead(presenter + 400'000'000, kVirtual, kPeriod).has_value(), "fresh inside half a second");
        check(!time.lead(presenter + 600'000'000, kVirtual, kPeriod), "stale after half a second");
        presenter += 600'000'000;
        frame(time, presenter, kVirtual, 4 * kPeriod);
        check(time.lead(presenter, kVirtual, kPeriod).has_value(), "one measured frame and it is used again");
        time.reset();
        check(!time.measured() && time.matched == 0 && time.handed_count == 0, "reset forgets everything");
    }
    // the handed time does not feed the measurement: handing four periods out instead of two leaves the delay as is
    {
        xrfg::TrueDisplayTime time;
        std::int64_t presenter = 1'000'000'000;
        for (int index = 0; index < 32; ++index) {
            const auto lead = time.lead(presenter, kVirtual, kPeriod);
            frame(time, presenter, lead ? *lead : kVirtual, 4 * kPeriod);
            presenter += kVirtual;
        }
        check(*time.measured() == 4 * kPeriod, "steady once it hands out what it measured");
    }
    // the presenter's prediction behind a handed time, for fill's gap
    {
        xrfg::TrueDisplayTime time;
        time.note_handed(2'000'000'000, 1'980'000'000);
        time.note_handed(2'040'000'000, 1'990'000'000);
        check(time.presenter_time_for(2'040'000'000) == 1'990'000'000 &&
                  time.presenter_time_for(2'000'000'000) == 1'980'000'000,
              "each handed time keeps the prediction it was handed against");
        check(!time.presenter_time_for(2'020'000'000), "a time never handed out has none");
    }
    // the lead is reached an eighth of a period a wait, inside its bounds
    {
        using Time = xrfg::TrueDisplayTime;
        check(Time::slew_lead(0, 5 * kPeriod, kVirtual, kPeriod) == kVirtual + kPeriod / 8,
              "from nothing it starts at one virtual period and moves an eighth");
        std::int64_t lead = 0;
        int waits = 0;
        while (lead != 4 * kPeriod && waits < 100) {
            lead = Time::slew_lead(lead, 4 * kPeriod, kVirtual, kPeriod);
            ++waits;
        }
        check(lead == 4 * kPeriod && waits >= 16 && waits <= 17, "two periods more take sixteen waits");
        check(Time::slew_lead(4 * kPeriod, kVirtual, kVirtual, kPeriod) == 4 * kPeriod - kPeriod / 8,
              "and it comes back the same way");
        check(Time::slew_lead(4 * kPeriod, 4 * kPeriod + 1000, kVirtual, kPeriod) == 4 * kPeriod + 1000,
              "a small difference is taken at once");
        check(Time::slew_lead(kPeriod, kPeriod, kVirtual, kPeriod) == kVirtual, "never under one virtual period");
        check(Time::slew_lead(20 * kPeriod, 20 * kPeriod, kVirtual, kPeriod) == kVirtual + 6 * kPeriod,
              "never over six periods past it");
        // the times handed out keep moving forward by whole periods, give or take an eighth, while the lead grows
        std::int64_t presenter = 1'000'000'000;
        std::int64_t applied = 0;
        std::int64_t previous_handed = 0;
        bool even = true;
        for (int index = 0; index < 40; ++index) {
            applied = Time::slew_lead(applied, 5 * kPeriod, kPeriod, kPeriod);
            const std::int64_t handed = presenter + applied;
            if (previous_handed != 0) {
                const std::int64_t step = handed - previous_handed;
                even = even && step >= kPeriod && step <= kPeriod + kPeriod / 8;
            }
            previous_handed = handed;
            presenter += kPeriod;
        }
        check(even, "under fill the handed times advance a period and at most an eighth more each frame");
    }
    // the ceiling's periods: six unless OFXR_REVER_TRUE_TIME_CEILING_PERIODS names 1..16 (2026-11-19, GTA V's 75-84 ms)
    {
        using Time = xrfg::TrueDisplayTime;
        check(Time::parse_extra_periods(nullptr) == 6 && Time::parse_extra_periods("") == 6, "unset: six periods");
        check(Time::parse_extra_periods("10") == 10 && Time::parse_extra_periods("16") == 16, "1..16 taken");
        check(Time::parse_extra_periods("0") == 6 && Time::parse_extra_periods("17") == 6 &&
                  Time::parse_extra_periods("x") == 6,
              "anything else: six");
        check(Time::maximum_extra_periods() == 6, "this test's process names none: six");
    }
    std::printf("%s (%d failures)\n", g_failures == 0 ? "PASS" : "FAILED", g_failures);
    return g_failures == 0 ? 0 : 1;
}
