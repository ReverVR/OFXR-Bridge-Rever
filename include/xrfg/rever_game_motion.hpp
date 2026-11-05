#pragma once

// Rever VR: the game's own per-eye motion vectors as OFXR's game-motion input.
//
// Off unless the process environment carries OFXR_REVER_GAME_MOTION=1. With
// the switch off the layer never creates a ReverGameMotionSource and none of
// this runs.
//
// The producer is Rever Inject inside the game. It describes its inputs in
// the named shared memory Local\ReverVR_DLSS_SR_Inputs and, per eye, shares a
// motion-vector texture by name (R16G16_FLOAT, render pixels, current ->
// previous, the DLSS convention at scale 1) guarded by a keyed mutex: the
// producer releases key 1 once written, a consumer takes key 1 without
// blocking, copies, and gives key 0 back. With Rever's layer above OFXR
// reading them first for its DLSS (Alien Isolation), the layer hands them on
// with key 2 and OFXR takes that one (rever_game_motion_handed_key).
//
// On a D3D11 session (bridged or through the D3D11 interop) the layer holds
// the game's D3D11 device. The source copies each eye's vectors on that
// device into a texture of its own shared with the layer's D3D12 device, joins
// the two on a shared fence, and publishes the D3D12 texture through the
// existing DLSS motion-vector registry on the layer's queue, once per real
// frame and only when both eyes' sequence numbers moved. A frame with nothing
// new publishes nothing, and OFXR measures optical flow for it as before.

#include <d3d11_4.h>
#include <d3d12.h>

#include <array>
#include <cstdint>
#include <memory>

namespace xrfg {

// Read once per session: OFXR_REVER_GAME_MOTION=1 in the environment.
[[nodiscard]] bool rever_game_motion_requested() noexcept;

// One line in %LOCALAPPDATA%\ReverVR\ofxr_game_motion.log (written whenever the game motion is requested, whatever
// the diagnostics setting: Rever's app keeps OFXR's own log off, its every-frame writes cost latency).
void rever_game_motion_note(const char* format, ...) noexcept;

// One projection view as the application submitted it, enough to tell the
// eye layout of the swapchain it names.
struct ReverGameMotionView {
    std::uint64_t swapchain{};
    std::uint32_t array_index{};
    std::int32_t x{};
    std::int32_t y{};
    std::int32_t width{};
    std::int32_t height{};
};

enum class ReverGameMotionLayout : std::uint32_t {
    // Nothing is published: no projection layer seen yet, two swapchains,
    // a gap between side-by-side eyes, or a shape the synthesizer cannot map.
    unsupported = 0,
    // One swapchain, one array slice per eye. One stream per slice, each
    // with its own render-size texture; slice 0's stream is published first
    // so the registry's first-seen order matches the slice order.
    array_slices = 1,
    // One swapchain, array size 1, both eyes side by side as two adjacent
    // image rectangles of equal size (Lone Echo II: 6082x3041, {0,0,3041,3041}
    // and {3041,0,3041,3041}). The synthesizer reads one motion texture per
    // array slice, so both eyes go into one double-width texture whose halves
    // map linearly onto the union of the two rectangles.
    side_by_side = 2,
};

struct ReverGameMotionStreamPlan {
    // Which of the producer's eyes lands at each half of the texture.
    // For array_slices only `producer_eye[0]` is used, at x = 0.
    std::array<std::uint32_t, 2> producer_eye{};
    std::uint32_t texture_width{};
    std::uint32_t texture_height{};
    std::uint32_t output_x{};
    std::uint32_t output_y{};
    std::uint32_t output_width{};
    std::uint32_t output_height{};
    std::uint32_t motion_width{};
    std::uint32_t motion_height{};
};

struct ReverGameMotionPlan {
    ReverGameMotionLayout layout{ReverGameMotionLayout::unsupported};
    std::uint64_t swapchain{};
    // array_slices: 2, stream index = array slice. side_by_side: 1.
    std::uint32_t stream_count{};
    std::array<ReverGameMotionStreamPlan, 2> streams{};
};

// Pure: the eye layout of a stereo projection and what each stream
// publishes, for an eye render size of render_width x render_height. The
// views are in the application's view order (view 0 = the producer's eye 0).
// swapchain_* describe the swapchain both views name.
[[nodiscard]] ReverGameMotionPlan plan_rever_game_motion(
    const std::array<ReverGameMotionView, 2>& views,
    std::uint32_t swapchain_width,
    std::uint32_t swapchain_height,
    std::uint32_t swapchain_array_size,
    std::uint32_t render_width,
    std::uint32_t render_height) noexcept;

[[nodiscard]] bool same_rever_game_motion_plan(
    const ReverGameMotionPlan& left,
    const ReverGameMotionPlan& right) noexcept;

// Rever VR, Alien Isolation (the channel's version 2 with its flag 2, KB
// ALI-050): each eye's vectors cover the whole half of the side-by-side image
// the game drew, not the eye's submitted rectangle - MotherVR's two eyes are a
// few pixels apart in size and cut to their lenses inside their halves. The
// views to plan with: the swapchain's two halves (view 0 the left), as Lone
// Echo II's; the views as they are when the width is odd.
[[nodiscard]] std::array<ReverGameMotionView, 2> rever_game_motion_halves(
    const std::array<ReverGameMotionView, 2>& views,
    std::uint32_t swapchain_width,
    std::uint32_t swapchain_height) noexcept;

// Read when a source is made: OFXR_REVER_GAME_MOTION_KEY=2 - Rever's layer
// above OFXR reads the vectors first (its own DLSS) and hands them on with key
// 2; a frame it did not read still comes with the producer's key 1. 1 when
// absent: the producer's key only.
[[nodiscard]] std::uint64_t rever_game_motion_handed_key() noexcept;

// A producer's block is this process's own (Rever Inject inside the game) or
// its parent's: Alien's producer lives in the 32-bit game, OFXR in the 64-bit
// bridge host the game started (test 34, 2026-11-05: "channel foreign", the
// vectors never taken). Any other process's block is another game's.
[[nodiscard]] bool rever_game_motion_ours(std::uint32_t block_pid, std::uint32_t own_pid,
                                          std::uint32_t parent_pid) noexcept;
// The process that started this one (0 when it cannot be told).
[[nodiscard]] std::uint32_t rever_game_motion_parent_pid() noexcept;

class ReverGameMotionSource final {
public:
    // `channel_name`: null for Rever's Local\ReverVR_DLSS_SR_Inputs; tests
    // pass a private name so they never touch a running game's channel.
    // Only a block whose pid is this process is used either way.
    explicit ReverGameMotionSource(const wchar_t* channel_name = nullptr);
    ~ReverGameMotionSource();
    ReverGameMotionSource(const ReverGameMotionSource&) = delete;
    ReverGameMotionSource& operator=(const ReverGameMotionSource&) = delete;

    // The application's xrEndFrame: its stereo projection, if it has one.
    // `valid` false: no usable stereo projection this frame.
    void set_views(
        bool valid,
        const std::array<ReverGameMotionView, 2>& views) noexcept;

    // At the release of `swapchain`'s image, before the layer resolves DLSS
    // vectors for it and queues its history capture on `d3d12_queue`. Takes,
    // copies and publishes the game's vectors when the swapchain is the
    // projection's and both eyes have new ones. `output` is the D3D12 image
    // the layer resolves against (only its shape is used).
    void publish_for_release(
        std::uint64_t swapchain,
        ID3D12Resource* output,
        ID3D11Device* d3d11_device,
        ID3D11DeviceContext* d3d11_context,
        ID3D12Device* d3d12_device,
        ID3D12CommandQueue* d3d12_queue) noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace xrfg
