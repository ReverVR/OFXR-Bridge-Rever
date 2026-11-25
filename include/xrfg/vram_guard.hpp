#pragma once

// Rever VR's video memory guard (KB GVX-060, 2026-11-24; user: "necesito una salida para hacer factible OFXR").
//
// Microsoft Flight Simulator 2024 on an RTX 4090 flew at 15-18 GB of the 22.5-23.4 GB budget DXGI gives its process,
// and OFXR's generation lives in that process and counts against the same budget. At 08:51:39 the game made two new
// projection swapchains mid-flight, the process reached 21.3 GB of 22.5 and the frame loop stopped for a second. With
// [ofxr] rever_vram_guard=1 the layer reads the process's use of the adapter's local memory twice a second
// (IDXGIAdapter3::QueryVideoMemoryInfo) and decides here:
//   - near the budget - usage at or above the budget less the margin (rever_vram_margin_mb, 1024 MB by default): the
//     multiplier capped one below where it is, never under 2X. Paging makes the game late, and the GPU-aware rule reads
//     a late game as a reason to step up - more synthesis work on a card that is already paging - so the cap also holds
//     every step up off;
//   - over the budget itself: 2X at once;
//   - the game making a projection swapchain again (the layer tells it): one below for 5 s, while the replacement's
//     history and generation images are made;
//   - the pressure's cap lifts once usage has stayed 512 MB under the line for 5 s.
// The budget judged is the one DXGI answers plus rever_vram_budget_extra_mb: Rever's runtime can lower the budget every
// DXGI caller of the process is told (REVER_XR_VRAM_RESERVE_GB, KB GVX-058), and the guard adds that back - never more
// than the answered figure itself, as the runtime never lowers it by more than half. Decisions only, no Windows call:
// the layer polls DXGI, applies the cap to its multiplier and logs each event in ofxr_rescue.log.

#include <algorithm>
#include <cstdint>

namespace xrfg {

class VramGuard {
public:
    static constexpr std::int64_t kPollNs = 500'000'000;  // twice a second
    static constexpr std::int64_t kClearNs = 5'000'000'000;
    static constexpr std::int64_t kRecreationNs = 5'000'000'000;
    static constexpr std::uint64_t kMegabyte = 1024ULL * 1024ULL;
    static constexpr std::uint64_t kHysteresisBytes = 512ULL * kMegabyte;
    static constexpr std::uint32_t kFloor = 2;
    static constexpr std::uint32_t kDefaultMarginMb = 1024;
    static constexpr std::uint32_t kLowestMarginMb = 256;
    static constexpr std::uint32_t kHighestMarginMb = 8192;
    static constexpr std::uint32_t kHighestExtraMb = 65536;

    enum class Event : std::uint8_t {
        none,
        pressure,     // usage reached the line: capped one below
        over_budget,  // usage reached the budget itself: 2X
        cleared,      // well under the line long enough: the pressure's cap lifted
        recreation,   // the game made a projection swapchain again: capped one below for a while
    };

    // The margin in MB (out of range: the default) and the runtime's reserve to add back, in MB (at most 64 GB).
    void configure(std::uint32_t margin_mb, std::uint32_t budget_extra_mb) noexcept {
        const std::uint32_t margin =
            margin_mb >= kLowestMarginMb && margin_mb <= kHighestMarginMb ? margin_mb : kDefaultMarginMb;
        margin_bytes_ = static_cast<std::uint64_t>(margin) * kMegabyte;
        extra_bytes_ = static_cast<std::uint64_t>(std::min(budget_extra_mb, kHighestExtraMb)) * kMegabyte;
    }
    [[nodiscard]] std::uint64_t margin_bytes() const noexcept { return margin_bytes_; }
    [[nodiscard]] std::uint64_t extra_bytes() const noexcept { return extra_bytes_; }

    // Whether a poll is due now; when it is, the next one is half a second later.
    [[nodiscard]] bool due(std::int64_t now_ns) noexcept {
        if (polled_ && now_ns < next_poll_ns_) {
            return false;
        }
        polled_ = true;
        next_poll_ns_ = now_ns + kPollNs;
        return true;
    }

    // The budget judged: DXGI's answer plus the runtime's reserve, never more than doubling it.
    [[nodiscard]] std::uint64_t judged_budget(std::uint64_t budget) const noexcept {
        return budget + std::min(extra_bytes_, budget);
    }
    // The line: the judged budget less the margin.
    [[nodiscard]] std::uint64_t line(std::uint64_t budget) const noexcept {
        const std::uint64_t judged = judged_budget(budget);
        return judged > margin_bytes_ ? judged - margin_bytes_ : 0;
    }

    // One poll: the process's usage and DXGI's budget, and the multiplier the session runs now.
    Event observe(std::int64_t now_ns, std::uint64_t usage, std::uint64_t budget, std::uint32_t current) noexcept {
        if (budget == 0) {
            return Event::none;  // nothing measured
        }
        last_usage_ = usage;
        last_budget_ = budget;
        const std::uint64_t judged = judged_budget(budget);
        const std::uint64_t headroom = judged > usage ? judged - usage : 0;
        peak_usage_ = window_polls_ == 0 ? usage : std::max(peak_usage_, usage);
        least_headroom_ = window_polls_ == 0 ? headroom : std::min(least_headroom_, headroom);
        ++window_polls_;
        const std::uint64_t threshold = line(budget);
        if (usage >= judged) {
            clear_since_ns_ = -1;
            const bool fresh = !pressed_ || pressure_cap_ != kFloor;
            pressed_ = true;
            pressure_cap_ = kFloor;
            if (fresh) {
                ++pressures_;
                return Event::over_budget;
            }
            return Event::none;
        }
        if (usage >= threshold) {
            clear_since_ns_ = -1;
            if (pressed_) {
                return Event::none;  // the cap stays where it is
            }
            pressed_ = true;
            pressure_cap_ = one_below(current);
            ++pressures_;
            return Event::pressure;
        }
        if (!pressed_) {
            return Event::none;
        }
        if (usage + kHysteresisBytes > threshold) {
            clear_since_ns_ = -1;  // under the line, not well under it
            return Event::none;
        }
        if (clear_since_ns_ < 0) {
            clear_since_ns_ = now_ns;
            return Event::none;
        }
        if (now_ns - clear_since_ns_ < kClearNs) {
            return Event::none;
        }
        pressed_ = false;
        pressure_cap_ = 0;
        clear_since_ns_ = -1;
        return Event::cleared;
    }

    // The game made a projection swapchain again: one below the multiplier it runs now for kRecreationNs (a second
    // one inside that time keeps the lower cap and starts the time again).
    Event swapchain_recreated(std::int64_t now_ns, std::uint32_t current) noexcept {
        const std::uint32_t cap = one_below(current);
        recreation_cap_ = recreating(now_ns) ? std::min(recreation_cap_, cap) : cap;
        recreation_until_ns_ = now_ns + kRecreationNs;
        ++recreations_;
        return Event::recreation;
    }
    [[nodiscard]] bool recreating(std::int64_t now_ns) const noexcept {
        return recreation_until_ns_ != 0 && now_ns < recreation_until_ns_;
    }
    // True once, at the first call after a recreation's time is over.
    [[nodiscard]] bool take_recreation_end(std::int64_t now_ns) noexcept {
        if (recreation_until_ns_ != 0 && now_ns >= recreation_until_ns_) {
            recreation_until_ns_ = 0;
            recreation_cap_ = 0;
            return true;
        }
        return false;
    }

    // The highest multiplier allowed now; 0 for none.
    [[nodiscard]] std::uint32_t cap(std::int64_t now_ns) const noexcept {
        std::uint32_t result = pressed_ ? pressure_cap_ : 0U;
        if (recreating(now_ns)) {
            result = result == 0 ? recreation_cap_ : std::min(result, recreation_cap_);
        }
        return result;
    }
    [[nodiscard]] bool pressed() const noexcept { return pressed_; }

    // What the polls saw since the last take_window: the highest usage, the least headroom under the judged budget.
    [[nodiscard]] std::uint32_t window_polls() const noexcept { return window_polls_; }
    [[nodiscard]] std::uint64_t peak_usage() const noexcept { return peak_usage_; }
    [[nodiscard]] std::uint64_t least_headroom() const noexcept { return least_headroom_; }
    void take_window() noexcept { window_polls_ = 0; }
    [[nodiscard]] std::uint64_t last_usage() const noexcept { return last_usage_; }
    [[nodiscard]] std::uint64_t last_budget() const noexcept { return last_budget_; }
    [[nodiscard]] std::uint32_t pressures() const noexcept { return pressures_; }
    [[nodiscard]] std::uint32_t recreations() const noexcept { return recreations_; }

private:
    [[nodiscard]] static std::uint32_t one_below(std::uint32_t current) noexcept {
        return current > kFloor + 1U ? current - 1U : kFloor;
    }

    std::uint64_t margin_bytes_{static_cast<std::uint64_t>(kDefaultMarginMb) * kMegabyte};
    std::uint64_t extra_bytes_{};
    bool polled_{};
    std::int64_t next_poll_ns_{};
    bool pressed_{};
    std::uint32_t pressure_cap_{};
    std::int64_t clear_since_ns_{-1};
    std::int64_t recreation_until_ns_{};
    std::uint32_t recreation_cap_{};
    std::uint64_t last_usage_{};
    std::uint64_t last_budget_{};
    std::uint32_t window_polls_{};
    std::uint64_t peak_usage_{};
    std::uint64_t least_headroom_{};
    std::uint32_t pressures_{};
    std::uint32_t recreations_{};
};

}  // namespace xrfg
