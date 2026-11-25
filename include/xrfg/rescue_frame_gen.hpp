#pragma once

// Rever VR's frame generation as a rescue (2026-11-16, Assassin's Creed Nexus; the user: "prepara lo mismo que has
// modificado en SSW para el OFXR del juego ... asi los interpoladores funcionan con su logica. Veo hasta donde llegas y
// relleno lo que falta"). The game alone ran at 119 real frames a second on the user's RTX 4090 at 120 Hz, and OFXR at
// 2X held it at 60 to invent the other half. With the rescue on (OFXR_REVER_RESCUE=1, or [ofxr] rescue=1), while the
// game keeps up with the display by itself OFXR invents nothing: each real frame is primed (its history kept) and goes
// in the game's own slot, as the turn brake's frames do (KB ALI-079) - the inline second cycle that halves the game's
// pace does not run. Once the game cannot keep up, frames pair again (2X). Off unless set: the frames pair as before.
//
// The decision, from the application's own timeline:
// - pairing (2X): the game is held at the multiplier's pace, so its pace says nothing; its own frame time does - from its
//   xrWaitFrame returning to its xrEndFrame, averaged - and once that stays under `try_fraction` of the display period for
//   `try_after_ns`, the rescue tries passing frames through;
// - passing through (1X): the game's pace now is what it can do - the interval between its xrWaitFrame returns, averaged
//   - and once that goes over `fail_fraction` of the period (it misses slots) the frames pair again;
// - a pass-through that fails within `probation_ns` doubles the wait before the next try (from `backoff_min_ns` up to
//   `backoff_max_ns`); one that holds longer brings it back to the minimum. Never two changes within `hold_ns`.

#include <algorithm>
#include <cstdint>

namespace xrfg {

struct RescueFrameGen {
    double try_fraction{0.85};
    double fail_fraction{1.08};
    std::int64_t try_after_ns{2'000'000'000};
    std::int64_t hold_ns{1'000'000'000};
    std::int64_t probation_ns{5'000'000'000};
    std::int64_t backoff_min_ns{3'000'000'000};
    std::int64_t backoff_max_ns{30'000'000'000};

    bool passing{};
    double frame_ns{};     // the game's own frame time, averaged (wait returned -> end frame)
    double interval_ns{};  // its pace while passing through, averaged (wait returned -> wait returned)
    std::int64_t last_wait_ns{};
    std::int64_t fits_since_ns{-1};
    std::int64_t changed_at_ns{-1};
    std::int64_t backoff_ns{3'000'000'000};
    std::int64_t retry_at_ns{};
    std::uint32_t changes{};

    // The game's xrWaitFrame returned at `now_ns`.
    void on_wait(std::int64_t now_ns) noexcept {
        if (last_wait_ns > 0 && now_ns > last_wait_ns) {
            const auto gap = static_cast<double>(now_ns - last_wait_ns);
            if (gap < 500'000'000.0) {
                interval_ns = interval_ns <= 0.0 ? gap : interval_ns * 0.85 + gap * 0.15;
            }
        }
        last_wait_ns = now_ns;
    }

    // The game's xrEndFrame at `now_ns`, its display period `period_ns`: whether this frame passes through (1X). True
    // when the state changed this frame is reported through `changed`.
    bool on_end_frame(std::int64_t now_ns, std::int64_t period_ns, bool& changed) noexcept {
        changed = false;
        if (period_ns <= 0) {
            return passing;
        }
        if (last_wait_ns > 0 && now_ns > last_wait_ns) {
            const auto frame = static_cast<double>(now_ns - last_wait_ns);
            if (frame < 500'000'000.0) {
                frame_ns = frame_ns <= 0.0 ? frame : frame_ns * 0.92 + frame * 0.08;
            }
        }
        const auto period = static_cast<double>(period_ns);
        const bool settled = changed_at_ns < 0 || now_ns - changed_at_ns >= hold_ns;
        if (!passing) {
            const bool fits = frame_ns > 0.0 && frame_ns < try_fraction * period;
            if (!fits) {
                fits_since_ns = -1;
                return false;
            }
            if (fits_since_ns < 0) {
                fits_since_ns = now_ns;
            }
            if (settled && now_ns >= retry_at_ns && now_ns - fits_since_ns >= try_after_ns) {
                passing = true;
                changed = true;
                changed_at_ns = now_ns;
                interval_ns = period;  // its pace starts from the display's
                ++changes;
            }
            return passing;
        }
        if (settled && interval_ns > fail_fraction * period) {
            const bool failed_early = now_ns - changed_at_ns < probation_ns;
            backoff_ns = failed_early ? std::min(backoff_max_ns, std::max(backoff_min_ns, backoff_ns * 2)) : backoff_min_ns;
            retry_at_ns = now_ns + backoff_ns;
            passing = false;
            changed = true;
            changed_at_ns = now_ns;
            fits_since_ns = -1;
            ++changes;
        }
        return passing;
    }
};

}  // namespace xrfg
