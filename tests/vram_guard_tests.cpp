// Rever VR's video memory guard (include/xrfg/vram_guard.hpp, KB GVX-060): the poll's pace, the line and the budget the
// runtime's reserve lowered, the caps near and over the budget, at a swapchain recreation, and when they lift.
#include "xrfg/vram_guard.hpp"

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
constexpr std::int64_t kHalf = 500'000'000;
constexpr std::uint64_t kMb = 1024ULL * 1024ULL;
constexpr std::uint64_t kGb = 1024ULL * kMb;
using Event = xrfg::VramGuard::Event;

}  // namespace

int main() {
    // the poll's pace: the first call is due, then every half second
    {
        xrfg::VramGuard guard;
        const std::int64_t start = 7 * kSecond;
        expect(guard.due(start), "the first poll is due");
        expect(!guard.due(start + kHalf - 1), "not before half a second");
        expect(guard.due(start + kHalf), "half a second later");
        expect(!guard.due(start + kHalf + 1), "and then not at once again");
    }

    // configuration: the margin's range, the reserve added back (never more than DXGI's own figure)
    {
        xrfg::VramGuard guard;
        expect(guard.margin_bytes() == 1024 * kMb, "no configure: 1024 MB");
        guard.configure(1536, 0);
        expect(guard.margin_bytes() == 1536 * kMb, "1536 MB");
        guard.configure(100, 0);
        expect(guard.margin_bytes() == 1024 * kMb, "100 MB is under the range: the default");
        guard.configure(9000, 0);
        expect(guard.margin_bytes() == 1024 * kMb, "9000 MB is over the range: the default");
        guard.configure(256, 4096);
        expect(guard.margin_bytes() == 256 * kMb && guard.extra_bytes() == 4 * kGb, "256 MB, a 4 GB reserve");
        // the run of 08:50 (KB GVX-059): 23370 MB for the process, 19274 MB told to the game
        expect(guard.judged_budget(19274 * kMb) == 23370 * kMb, "the reserve added back: Windows' own budget");
        expect(guard.line(19274 * kMb) == (23370 - 256) * kMb, "the line: the judged budget less the margin");
        expect(guard.judged_budget(3 * kGb) == 6 * kGb, "never more than doubling the answered budget");
        guard.configure(1024, 100000);
        expect(guard.extra_bytes() == 65536 * kMb, "the reserve at most 64 GB");
        guard.configure(8192, 0);
        expect(guard.line(4 * kGb) == 0, "a margin above the budget: the line at 0");
    }

    // the run of 08:50 with the guard (no reserve, margin 1536): 18.4 GB of 23.4 is quiet; the spike's 21.3 of 22.5
    // is near the line (21.0): 4X capped to 3X, and 2X once the budget itself is reached
    {
        xrfg::VramGuard guard;
        guard.configure(1536, 0);
        std::int64_t now = 10 * kSecond;
        expect(guard.observe(now, 18387 * kMb, 23370 * kMb, 4) == Event::none, "18.4 of 23.4 GB: quiet");
        expect(guard.cap(now) == 0 && !guard.pressed(), "no cap");
        now += kHalf;
        expect(guard.observe(now, 21301 * kMb, 22481 * kMb, 4) == Event::pressure, "21.3 of 22.5 GB: near the line");
        expect(guard.cap(now) == 3, "4X capped to 3X");
        now += kHalf;
        expect(guard.observe(now, 21500 * kMb, 22481 * kMb, 3) == Event::none, "still near: no new event");
        expect(guard.cap(now) == 3, "the cap stays (no step every poll)");
        now += kHalf;
        expect(guard.observe(now, 22600 * kMb, 22481 * kMb, 3) == Event::over_budget, "over the budget: paging");
        expect(guard.cap(now) == 2, "2X at once");
        now += kHalf;
        expect(guard.observe(now, 22700 * kMb, 22481 * kMb, 2) == Event::none, "over again: no new event");
        expect(guard.observe(now, 21400 * kMb, 22481 * kMb, 2) == Event::none, "back near the line: the cap stays");
        expect(guard.cap(now) == 2, "still 2X");
        // under the line but not 512 MB under it: nothing lifts
        for (int poll = 0; poll < 20; ++poll) {
            now += kHalf;
            expect(guard.observe(now, 20700 * kMb, 22481 * kMb, 2) == Event::none, "within the hysteresis");
        }
        expect(guard.cap(now) == 2, "the hysteresis holds the cap");
        // well under it for 5 s: lifted
        now += kHalf;
        expect(guard.observe(now, 19000 * kMb, 22481 * kMb, 2) == Event::none, "well under: the time starts");
        now += 4 * kSecond;
        expect(guard.observe(now, 19000 * kMb, 22481 * kMb, 2) == Event::none, "4 s: not yet");
        now += kHalf;
        expect(guard.observe(now, 20700 * kMb, 22481 * kMb, 2) == Event::none, "back in the hysteresis: starts over");
        now += kHalf;
        expect(guard.observe(now, 19000 * kMb, 22481 * kMb, 2) == Event::none, "well under again");
        now += 5 * kSecond;
        expect(guard.observe(now, 19000 * kMb, 22481 * kMb, 2) == Event::cleared, "5 s well under: lifted");
        expect(guard.cap(now) == 0 && !guard.pressed(), "no cap");
        expect(guard.pressures() == 2, "two pressure events (near, over)");
        // the window: the highest usage and the least headroom since it was taken
        expect(guard.peak_usage() == 22700 * kMb && guard.least_headroom() == 0, "the window's peak and headroom");
        guard.take_window();
        now += kHalf;
        static_cast<void>(guard.observe(now, 19000 * kMb, 22481 * kMb, 2));
        expect(guard.window_polls() == 1 && guard.peak_usage() == 19000 * kMb &&
                   guard.least_headroom() == 3481 * kMb,
               "a new window");
        // near again at 2X: the cap is 2X (never under it)
        now += kHalf;
        expect(guard.observe(now, 21200 * kMb, 22481 * kMb, 2) == Event::pressure, "near at 2X");
        expect(guard.cap(now) == 2, "never under 2X");
    }

    // the runtime's 4 GB reserve (the user's tuning file): the guard judges Windows' budget, not the lowered one
    {
        xrfg::VramGuard guard;
        guard.configure(1536, 4096);
        const std::int64_t now = 3 * kSecond;
        expect(guard.observe(now, 18387 * kMb, 19274 * kMb, 4) == Event::none,
               "18.4 GB with 19.3 told: 23.4 judged, quiet");
        expect(guard.observe(now + kHalf, 22000 * kMb, 19274 * kMb, 4) == Event::pressure, "22.0 of 23.4: near");
        xrfg::VramGuard plain;
        plain.configure(1536, 0);
        expect(plain.observe(now, 18387 * kMb, 19274 * kMb, 4) == Event::pressure,
               "without the reserve added back the same reading would be near");
    }

    // a swapchain recreation: one below for 5 s, the lower of two caps
    {
        xrfg::VramGuard guard;
        guard.configure(1536, 0);
        std::int64_t now = 20 * kSecond;
        expect(guard.swapchain_recreated(now, 4) == Event::recreation, "recreated at 4X");
        expect(guard.recreating(now) && guard.cap(now) == 3, "3X for a while");
        expect(!guard.take_recreation_end(now + kSecond), "not over after 1 s");
        expect(guard.swapchain_recreated(now + kSecond, 3) == Event::recreation, "another one at 3X");
        expect(guard.cap(now + kSecond) == 2, "the lower cap");
        expect(guard.cap(now + 5 * kSecond) == 2, "the time started again at the second one");
        expect(guard.cap(now + 6 * kSecond) == 0, "over 5 s after the second one");
        expect(guard.take_recreation_end(now + 6 * kSecond), "its end, once");
        expect(!guard.take_recreation_end(now + 7 * kSecond), "only once");
        expect(guard.recreations() == 2, "two recreations");
        // with pressure as well: the lower of the two
        now = 40 * kSecond;
        expect(guard.observe(now, 21500 * kMb, 22481 * kMb, 4) == Event::pressure, "near at 4X: 3X");
        static_cast<void>(guard.swapchain_recreated(now, 2));
        expect(guard.cap(now) == 2, "recreation at 2X: the lower cap");
        expect(guard.cap(now + 6 * kSecond) == 3, "the pressure's cap after the recreation's time");
        // at 2X a recreation caps at 2X
        xrfg::VramGuard low;
        static_cast<void>(low.swapchain_recreated(now, 2));
        expect(low.cap(now) == 2, "2X stays 2X");
    }

    // nothing measured (a failed query): nothing decided
    {
        xrfg::VramGuard guard;
        expect(guard.observe(kSecond, 30 * kGb, 0, 4) == Event::none && guard.cap(kSecond) == 0, "no budget: nothing");
        expect(guard.window_polls() == 0, "and not counted");
    }

    if (failures != 0) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "vram guard: all checks passed\n";
    return 0;
}
