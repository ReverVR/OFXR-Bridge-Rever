#pragma once

// Rever VR's true display time (OFXR_REVER_TRUE_TIME=1, or [ofxr] true_time=1; 2026-11-16, Assassin's Creed Nexus on
// Rever Streamer's runtime, Steam Frame at 120 / 144 Hz). On the continuous presenter the application's xrWaitFrame is
// answered by the layer: it is handed the presenter's current prediction plus one virtual period. Its real frame goes
// out later than that - its render, the synthesis, the synthetic shown first and whatever is queued ahead of it - and
// the runtime's pose trace showed every frame the presenter submitted carrying a head pose predicted for a time four or
// more display periods before the one it went out at (0.4 - 3.4 degrees on average, 7 at most). With the switch on the
// application is handed the time its real frame is actually shown at, measured, so the runtime predicts its views for
// that time.
//
// The measurement: each time the wait hands a display time out it notes the presenter's prediction at that moment
// (note_handed); each time the presenter pops a real frame it looks up the time the application submitted it for and
// takes the delay from that prediction to the presenter slot the frame goes out in (note_presented). The delay the wait
// adds is the mean of the middle half of the last 16 delays (an occasional late or early frame does not move it), used
// once there are 8, and held between one virtual period - what the layer hands out without the switch - and six display
// periods more, and the wait moves towards it by at most an eighth of a display period a frame (slew_lead), so the
// times handed out keep advancing by whole periods. Half a second without a real frame measured (a menu or a loading
// screen the layer passes through, a pause) and it is not used until one is: what it measured belonged to the frames
// that were generated. Pure arithmetic; the presenter_mutex of the session guards one instance.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>

namespace xrfg {

struct TrueDisplayTime {
    static constexpr std::size_t kHandedRing = 64;
    static constexpr std::size_t kWindow = 16;
    static constexpr std::size_t kMinimumSamples = 8;
    static constexpr std::int64_t kMaximumExtraPeriods = 6;
    // The ceiling's display periods over the virtual one: kMaximumExtraPeriods, or OFXR_REVER_TRUE_TIME_CEILING_PERIODS
    // (1..16) read once per process (2026-11-19, GTA V on the Steam Frame: its delay measured 75-84 ms while six periods
    // handed 58.33, every pose predicted 17-26 ms short - the edge of the picture showing on fast head turns; only Rever's
    // GTA V worker sets it, every other game keeps six).
    static constexpr std::int64_t kLargestExtraPeriods = 16;
    [[nodiscard]] static std::int64_t parse_extra_periods(const char* text) noexcept {
        if (text == nullptr || *text == '\0') {
            return kMaximumExtraPeriods;
        }
        char* end = nullptr;
        const long parsed = std::strtol(text, &end, 10);
        return end != text && parsed >= 1 && parsed <= kLargestExtraPeriods ? static_cast<std::int64_t>(parsed)
                                                                            : kMaximumExtraPeriods;
    }
    [[nodiscard]] static std::int64_t maximum_extra_periods() noexcept {
        static const std::int64_t periods = [] {
#if defined(_MSC_VER)
            char* value = nullptr;
            std::size_t length = 0;
            std::int64_t result = kMaximumExtraPeriods;
            if (_dupenv_s(&value, &length, "OFXR_REVER_TRUE_TIME_CEILING_PERIODS") == 0 && value != nullptr) {
                result = parse_extra_periods(value);
                std::free(value);
            }
            return result;
#else
            return parse_extra_periods(std::getenv("OFXR_REVER_TRUE_TIME_CEILING_PERIODS"));
#endif
        }();
        return periods;
    }
    // A delay outside (0, 1 s) is not a display pipeline: a reset, a stall or a time the application made up.
    static constexpr std::int64_t kLongestDelay = 1'000'000'000;
    // Display time without a measured real frame after which the measurement is not used.
    static constexpr std::int64_t kStaleAfter = 500'000'000;

    struct Handed {
        std::int64_t handed{};
        std::int64_t presenter{};
    };
    std::array<Handed, kHandedRing> handed{};
    std::uint64_t handed_count{};
    std::array<std::int64_t, kWindow> delays{};
    std::uint64_t delay_count{};
    // Real frames whose submitted time matched a handed one, and those that did not (an application that submits a time
    // of its own, or one handed before the ring's reach).
    std::uint64_t matched{};
    std::uint64_t unmatched{};
    std::int64_t last_delay{};
    // The presenter slot the last measured frame went out in.
    std::int64_t last_presented{};

    void reset() noexcept { *this = TrueDisplayTime{}; }

    // The wait handed the application `handed_time` while the presenter's frame was predicted for `presenter_time`.
    void note_handed(std::int64_t handed_time, std::int64_t presenter_time) noexcept {
        if (handed_time <= 0 || presenter_time <= 0) {
            return;
        }
        handed[handed_count % kHandedRing] = {handed_time, presenter_time};
        ++handed_count;
    }

    // The presenter's prediction at the wait that handed out `handed_time`, while the ring still holds it. Fill counts the
    // slots between two real frames on these: the times handed out move with the lead, the presenter's grid does not.
    [[nodiscard]] std::optional<std::int64_t> presenter_time_for(std::int64_t handed_time) const noexcept {
        const std::size_t reach = static_cast<std::size_t>(std::min<std::uint64_t>(handed_count, kHandedRing));
        for (std::size_t back = 0; back < reach; ++back) {
            const Handed& entry = handed[(handed_count - 1 - back) % kHandedRing];
            if (entry.handed == handed_time) {
                return entry.presenter;
            }
        }
        return std::nullopt;
    }

    // A real frame the application submitted for `submitted_time` goes out in the presenter's slot predicted for
    // `presented_time`. True when it was a time this ring handed out and the delay was taken.
    bool note_presented(std::int64_t submitted_time, std::int64_t presented_time) noexcept {
        const std::size_t reach = static_cast<std::size_t>(std::min<std::uint64_t>(handed_count, kHandedRing));
        for (std::size_t back = 0; back < reach; ++back) {
            const Handed& entry = handed[(handed_count - 1 - back) % kHandedRing];
            if (entry.handed != submitted_time) {
                continue;
            }
            const std::int64_t delay = presented_time - entry.presenter;
            if (delay <= 0 || delay >= kLongestDelay) {
                break;
            }
            delays[delay_count % kWindow] = delay;
            ++delay_count;
            ++matched;
            last_delay = delay;
            last_presented = presented_time;
            return true;
        }
        ++unmatched;
        return false;
    }

    // The delay measured: the mean of the middle half of the last 16; none under 8.
    [[nodiscard]] std::optional<std::int64_t> measured() const noexcept {
        const std::size_t count = static_cast<std::size_t>(std::min<std::uint64_t>(delay_count, kWindow));
        if (count < kMinimumSamples) {
            return std::nullopt;
        }
        std::array<std::int64_t, kWindow> sorted{};
        for (std::size_t index = 0; index < count; ++index) {
            sorted[index] = delays[(delay_count - 1 - index) % kWindow];
        }
        std::sort(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(count));
        const std::size_t trim = count / 4;
        std::int64_t sum = 0;
        for (std::size_t index = trim; index < count - trim; ++index) {
            sum += sorted[index];
        }
        return sum / static_cast<std::int64_t>(count - 2 * trim);
    }

    // What the wait adds to the presenter's prediction `presenter_time`: the delay measured, held in [virtual_period,
    // virtual_period + 6 runtime periods]; none while it is unmeasured or stale, or without a period.
    [[nodiscard]] std::optional<std::int64_t> lead(
        std::int64_t presenter_time, std::int64_t virtual_period, std::int64_t runtime_period) const noexcept {
        if (virtual_period <= 0 || runtime_period <= 0 || last_presented <= 0 ||
            presenter_time - last_presented > kStaleAfter) {
            return std::nullopt;
        }
        const auto delay = measured();
        if (!delay) {
            return std::nullopt;
        }
        return std::clamp(*delay, virtual_period, virtual_period + maximum_extra_periods() * runtime_period);
    }

    // The lead the wait applies, moved from `applied` (0: none yet, start from one virtual period) towards `target` (the
    // lead above, or one virtual period without it) by at most an eighth of a runtime period a wait, and kept in [virtual
    // period, virtual period + 6 runtime periods]. The middle of 16 delays a whole period apart steps a whole period at a
    // time; handed out at once, each step would put a period more or less between two frames' display times - a slot
    // missed or doubled in the application's timeline, and under fill a synthetic made for a slot nobody missed.
    [[nodiscard]] static std::int64_t slew_lead(
        std::int64_t applied, std::int64_t target, std::int64_t virtual_period, std::int64_t runtime_period) noexcept {
        if (virtual_period <= 0 || runtime_period <= 0) {
            return target;
        }
        const std::int64_t floor = virtual_period;
        const std::int64_t ceiling = virtual_period + maximum_extra_periods() * runtime_period;
        const std::int64_t from = applied > 0 ? std::clamp(applied, floor, ceiling) : floor;
        const std::int64_t step = std::max<std::int64_t>(1, runtime_period / 8);
        return std::clamp(from + std::clamp(target - from, -step, step), floor, ceiling);
    }
};

}  // namespace xrfg
