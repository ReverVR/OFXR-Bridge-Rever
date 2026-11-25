#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace xrfg::implicit_layer {

// One manual-reset kernel event per arm. Old readers retain their own event;
// re-arming cannot accidentally reactivate a session that was already stopped.
[[nodiscard]] std::wstring arm_signal_name(const std::filesystem::path& manifest);
[[nodiscard]] void* create_arm_signal(const std::filesystem::path& manifest,
                                      std::wstring* error = nullptr) noexcept;
[[nodiscard]] bool signal_arm_stop(const std::filesystem::path& manifest,
                                    std::wstring* error = nullptr) noexcept;
// The tray's "Pause frame generation" switch: a second manual-reset event
// per arm, set while paused and reset on resume. Unlike the stop it goes both
// ways, and a session that cannot open it is simply never paused. The name
// is the arm signal's with another prefix, so the layer finds it from the
// same `control_event` and nothing else has to travel through the ini.
[[nodiscard]] std::wstring pause_signal_name(const std::filesystem::path& manifest);
[[nodiscard]] void* create_pause_signal(const std::filesystem::path& manifest,
                                        std::wstring* error = nullptr) noexcept;
// Wine's ntdll exports wine_get_version; Windows' does not.
[[nodiscard]] bool running_under_wine() noexcept;
// How a session found the tray's control. The flight log's arm_control
// record carries it.
enum class ArmControlState : std::int64_t {
    // No control_event in the ini: a layer the tray does not manage.
    unmanaged = 0,
    tray_control = 1,
    // Managed, but the control is gone: the tray quit or was killed, so
    // the layer stays Off.
    control_missing = 2,
    // The same under Wine, where the layer runs. Proton's tray cannot run
    // beside the game (separate wineservers deadlock the start-up, and their
    // named objects are not shared anyway), so the control is always missing
    // there; failing closed would make the layer unusable.
    control_missing_wine = 3,
};
class ManualArmControl {
public:
    explicit ManualArmControl(const std::filesystem::path& module_directory,
                              bool under_wine = running_under_wine()) noexcept;
    ~ManualArmControl();
    ManualArmControl(const ManualArmControl&) = delete;
    ManualArmControl& operator=(const ManualArmControl&) = delete;
    [[nodiscard]] bool stop_requested() const noexcept;
    [[nodiscard]] bool pause_requested() const noexcept;
    [[nodiscard]] ArmControlState state() const noexcept { return state_; }
private:
    void* event_{};
    void* pause_event_{};
    bool managed_{};
    ArmControlState state_{ArmControlState::unmanaged};
};

inline constexpr wchar_t kRegistrySubkey[] =
    L"SOFTWARE\\Khronos\\OpenXR\\1\\ApiLayers\\Implicit";
inline constexpr wchar_t kManifestPrefix[] =
    L"XR_APILAYER_XRFrameBridge_manual-";
inline constexpr wchar_t kManifestSuffix[] = L".json";
// The Vulkan implicit layer that serialises queue submissions, registered
// beside the OpenXR one while the bridge is armed with Vulkan support on.
// Same value scheme: the manifest path as the value name, DWORD 0.
inline constexpr wchar_t kVulkanRegistrySubkey[] =
    L"SOFTWARE\\Khronos\\Vulkan\\ImplicitLayers";
inline constexpr wchar_t kVulkanManifestPrefix[] =
    L"OFXR_vulkan_queue_manual-";
inline constexpr wchar_t kVulkanLayerName[] = L"VK_LAYER_OFXR_queue_serialize";
inline constexpr wchar_t kVulkanLayerDll[] = L"OFXR_vulkan_queue_layer.dll";
// V576: the 32-bit layer, for 32-bit games, armed beside the 64-bit one.
// The 32-bit loader reads the same per-user key as the 64-bit one, and a
// loader fails xrCreateInstance only when no layer it lists loads, so the
// two are registered together and each loader skips the DLL it cannot load.
// Its manifest is the arm's, same process and arm id, under its own prefix;
// its DLL sits beside the 64-bit one under its own name, and so shares that
// one's ini, controls and log folder. Bundled in kX86BundleDirectory beside
// the tray's ofxr folder, under the 64-bit DLL's name.
inline constexpr wchar_t kX86ManifestPrefix[] = L"XR_APILAYER_XRFrameBridge_x86-";
inline constexpr wchar_t kX86LayerDllName[] = L"XR_APILAYER_XRFrameBridge_x86.dll";
inline constexpr wchar_t kX86BundleDirectory[] = L"ofxr_x86";
// The 32-bit manifest of the arm whose 64-bit manifest is `manifest`; empty
// when `manifest` is not a tray manifest's name.
[[nodiscard]] std::filesystem::path x86_manifest_for(
    const std::filesystem::path& manifest);

enum class RegistryScope {
    current_user,
    local_machine,
};

// V572: the layer for games run as administrator. The loader reads no
// per-user layer in an elevated process, so the tray's arm never reaches one.
// An opt-in machine-wide copy does: the same DLL, installed by an elevated
// tray helper into a folder only administrators can write, registered in
// HKLM under its own name. It loads only in an elevated process, and only
// while a per-user arm with a byte-identical DLL exists, whose folder it then
// takes its settings from as the per-user layer would; anywhere else it
// declines at negotiation and the loader goes on without it.
inline constexpr wchar_t kElevatedLayerName[] = L"XR_APILAYER_XRFrameBridge_elevated";
inline constexpr wchar_t kElevatedManifestName[] = L"XR_APILAYER_XRFrameBridge_elevated.json";
inline constexpr wchar_t kLayerDllName[] = L"XR_APILAYER_XRFrameBridge_diagnostic.dll";
// The Vulkan queue layer's copy beside it, registered in HKLM's Vulkan key
// under its own name, whose manifest names a negotiation entry point that
// makes the same checks (OFXR_vkNegotiateLoaderLayerInterfaceVersionElevated).
inline constexpr wchar_t kElevatedVulkanManifestName[] = L"OFXR_vulkan_queue_elevated.json";
// Whether this process runs with an elevated token.
[[nodiscard]] bool process_is_elevated() noexcept;
// Byte for byte, read as data.
[[nodiscard]] bool files_identical(
    const std::filesystem::path& first,
    const std::filesystem::path& second) noexcept;
// The folder of an enabled per-user registration of one of the tray's
// manifests (HKCU, `prefix`: the OpenXR layer's or the Vulkan layer's) that
// holds a byte-identical copy of `layer_dll`, under the same file name: the
// arm an elevated copy of that DLL belongs to. Empty without one.
[[nodiscard]] std::filesystem::path armed_directory_matching(
    const std::filesystem::path& layer_dll,
    std::wstring_view registry_subkey = kRegistrySubkey,
    std::wstring_view prefix = kManifestPrefix) noexcept;

[[nodiscard]] RegistryScope registry_scope_for_integrity_rid(
    std::uint32_t integrity_rid) noexcept;
[[nodiscard]] RegistryScope preferred_registry_scope() noexcept;
[[nodiscard]] std::wstring_view registry_scope_name(RegistryScope scope) noexcept;

enum class ConfiguredFlowBackend {
    fidelity_fx,
    nvidia,
};

enum class ConfiguredNvidiaPerformancePreset {
    slow,
    medium,
    fast,
};

enum class ConfiguredNvidiaInputScale {
    full,
    three_quarter,
    half,
    quarter,
};

struct ConfiguredNvidiaOptions {
    ConfiguredNvidiaPerformancePreset preset{
        ConfiguredNvidiaPerformancePreset::medium};
    ConfiguredNvidiaInputScale input_scale{
        ConfiguredNvidiaInputScale::half};
    bool bidirectional{};
};

[[nodiscard]] ConfiguredFlowBackend read_flow_backend(
    const std::filesystem::path& module_directory) noexcept;

// `[ofxr] deep_pipeline`: one display period of extra depth, bought with one
// display period of latency. On unless set to 0. Read once per session, at
// xrCreateSession; the tray calls it "Prefer FPS over latency".
[[nodiscard]] bool read_deep_pipeline(
    const std::filesystem::path& module_directory) noexcept;

// `[ofxr] triple_frame_gen`: two synthetic frames per application frame
// instead of one, for an application running at a third of the display rate.
// Off unless set to 1. Read at xrCreateSession and then followed while the
// session runs, where the session can take either shape; it takes the
// shallow pipeline whatever deep_pipeline says. The tray calls it
// "3X Frame Gen".
[[nodiscard]] bool read_triple_frame_gen(
    const std::filesystem::path& module_directory) noexcept;

// `[ofxr] frame_multiplier` (V558): frames shown per application frame, 2,
// 3 or 4 - 4 is three synthetics per application frame ("4X"). Absent or
// out of range it is what triple_frame_gen says: 3 when set, 2 otherwise.
// Read at xrCreateSession and followed while the session runs, as far as
// the synthetic ring the session was made with allows (a ring of n slots
// fits every multiplier up to n + 1). The tray calls it "Frame Gen
// multiplier".
[[nodiscard]] std::uint32_t read_frame_multiplier(
    const std::filesystem::path& module_directory) noexcept;

// `[ofxr] edge_ghosting_reduction`: the composition's refinement, off unless
// set to 1; followed while the session runs. (`repeating_pattern_lock` left
// with V507.)
[[nodiscard]] bool read_edge_ghosting_reduction(
    const std::filesystem::path& module_directory) noexcept;

// `[ofxr] vulkan_session_bridge`: a Vulkan game's session is handed to the
// runtime as a D3D12 one on the layer's device, the game rendering into
// Vulkan imports of the layer's shared textures, so every path downstream is
// the native D3D12 one and nothing is mirrored. On unless set to 0; needs
// vulkan_bridge (Vulkan support) on as well. Read at xrCreateInstance.
[[nodiscard]] bool read_vulkan_session_bridge(
    const std::filesystem::path& module_directory) noexcept;

// `[ofxr] single_swapchain_rings`: one private swapchain per output with
// staging textures instead of two per output, on the paths where the
// synthesizer writes D3D12 images directly. On unless set to 0. Read at
// xrCreateSession.
[[nodiscard]] bool read_single_swapchain_rings(
    const std::filesystem::path& module_directory) noexcept;

// `[ofxr] capture_at_end_frame`: on a native D3D12 session the history
// capture of an application image is queued at the application's xrEndFrame
// rather than at its xrReleaseSwapchainImage. A release only promises that
// the image's rendering has been *submitted*; an application whose
// submission thread trails the one that releases can release first, and a
// capture queued then holds the previous frame for that image. The runtime
// only reads at xrEndFrame and never sees that. On the D3D11 bridge the
// whole release waits for xrEndFrame instead - the copy into the runtime's
// image and the capture together, since the copy is what the runtime shows.
// On unless set to 0. Read at xrCreateSession.
[[nodiscard]] bool read_capture_at_end_frame(
    const std::filesystem::path& module_directory) noexcept;

// Rever VR's turn brake (xrfg/turn_brake.hpp, KB ALI-079): `[ofxr] turn_brake`, on unless set to 0
// (OFXR_REVER_TURN_BRAKE=0 / 1 in the game's environment wins), and `[ofxr] turn_brake_deg`, the head's
// speed in degrees a second that engages it (90 by default, 20 to 720; OFXR_REVER_TURN_BRAKE_DEG wins).
// Read live, every quarter second: the tray rewrites the ini without them at each arm, which is the
// default, so an A/B edits the running ini.
[[nodiscard]] bool read_turn_brake(
    const std::filesystem::path& module_directory) noexcept;
[[nodiscard]] std::uint32_t read_turn_brake_deg(
    const std::filesystem::path& module_directory) noexcept;
// The brake's tuning (KB ALI-082; the Steam Frame's launch sets 450 / 40, the Quest none): `[ofxr]
// turn_brake_hold_ms`, the least it stays on once engaged (150 by default, 50 to 2000), and `[ofxr]
// turn_brake_release`, the speed it lets go below as a percent of turn_brake_deg (60 by default, 10 to
// 95). OFXR_REVER_TURN_BRAKE_HOLD_MS / OFXR_REVER_TURN_BRAKE_RELEASE in the game's environment win when
// inside the range; out of range, the ini's, else the default. Read live with turn_brake.
[[nodiscard]] std::uint32_t read_turn_brake_hold_ms(
    const std::filesystem::path& module_directory) noexcept;
[[nodiscard]] std::uint32_t read_turn_brake_release(
    const std::filesystem::path& module_directory) noexcept;

// Rever VR's GPU-aware dynamic multiplier (xrfg/gpu_aware_multiplier.hpp, KB ALI-079): `[ofxr] gpu_aware`,
// on unless set to 0 (OFXR_REVER_GPU_AWARE=0 / 1 in the game's environment wins). Read live, like
// turn_brake.
[[nodiscard]] bool read_gpu_aware(
    const std::filesystem::path& module_directory) noexcept;
// `[ofxr] gpu_margin` (KB ALI-082): the share of the shape's real-frame rate below which the GPU-aware
// rule steps up, in percent (95 by default, 70 to 99; the Steam Frame's launch sets 90).
// OFXR_REVER_GPU_MARGIN wins as the two above.
[[nodiscard]] std::uint32_t read_gpu_margin(
    const std::filesystem::path& module_directory) noexcept;

// `[ofxr] binding_queue` (Rever VR): give the runtime the layer's own binding queue on
// every runtime, not only SteamVR. For an API layer above OFXR that finishes the
// application's frame on the GPU after its release (Rever's DLSS-NR layer, ~10 ms later):
// with the application's queue, each downstream xrEndFrame on Virtual Desktop - repeats
// too - waited behind that work. Off unless set to 1, or OFXR_BINDING_QUEUE=1 in the
// application's environment. Read at xrCreateSession.
[[nodiscard]] bool read_binding_queue(
    const std::filesystem::path& module_directory) noexcept;

// `[ofxr] rever_binding_queue_high` (Rever VR, KB GVX-061, 2026-11-24): the binding queue above made at
// D3D12_COMMAND_QUEUE_PRIORITY_HIGH, as VectorXR makes the queue it gives the runtime
// (framegen_runtime_queue.cpp): a generated frame's copy into the runtime's image and its release go ahead of
// the game's queued work, not only beside it. Only where a binding queue is made; off unless set to 1, or
// OFXR_REVER_BINDING_QUEUE_HIGH=1 in the application's environment (without it: NORMAL, exactly as before).
// Read at xrCreateSession.
[[nodiscard]] bool read_rever_binding_queue_high(
    const std::filesystem::path& module_directory) noexcept;

// `[ofxr] presenter_from_start` (Rever VR): request the continuous presenter after the first
// completed inline pair instead of waiting for a pacing signal. On Titanfall 2 + DLSS-NR the
// signal only came after a level reload: until then the session stayed inline, the game held
// at 36 fps with one frame shown per frame. Off unless set to 1. Read at xrCreateSession.
[[nodiscard]] bool read_presenter_from_start(
    const std::filesystem::path& module_directory) noexcept;

// Rever VR: `[ofxr] dynamic_frame_gen` (or OFXR_DYNAMIC_FRAME_GEN=1 in the
// game's environment): on a session that can take several shapes, the
// multiplier is chosen from how long the application takes for a frame
// instead of from frame_multiplier - one more frame between game frames
// when it cannot make its periods, one fewer once it fits well in them.
// frame_multiplier is then the ceiling, never below 3: 2X / 3X as before,
// 2X / 3X / 4X when it says 4. Off unless set to 1. Read at xrCreateSession.
[[nodiscard]] bool read_dynamic_frame_gen(
    const std::filesystem::path& module_directory) noexcept;

// Rever VR's frame generation as a rescue (OFXR_REVER_RESCUE=1 in the game's environment, or [ofxr] rescue=1;
// xrfg/rescue_frame_gen.hpp, 2026-11-16): on an inline session, frames pass through (nothing invented, the game at the
// display's pace) while the game keeps up by itself, and pair again once it cannot. Off unless set. Read at
// xrCreateSession.
[[nodiscard]] bool read_rescue_frame_gen(
    const std::filesystem::path& module_directory) noexcept;

// Rever VR's true display time (OFXR_REVER_TRUE_TIME=1 in the game's environment, or [ofxr] true_time=1;
// xrfg/true_display_time.hpp, 2026-11-16): on a presenter session the application is handed the display time its real
// frame is measured to go out at, not the presenter's prediction plus one virtual period. Off unless set. Read at
// xrCreateSession.
[[nodiscard]] bool read_true_time(
    const std::filesystem::path& module_directory) noexcept;

// Rever VR's continuous presenter on Rever Streamer's runtime (OFXR_REVER_PRESENTER=1 in the game's environment, or
// [ofxr] rever_presenter=1 in the armed layer's ini; KB GVX-058, 2026-11-24): a game its store starts again without
// Rever's environment (Microsoft Flight Simulator through Steam) gets it from the ini Rever's app writes at each arming.
// Only the route changes: the presenter still needs the session the D3D11 bridge lifted to D3D12 (no D3D11 device) and
// a one-thread frame loop (frame_loop_takes_presenter). Off unless set. Read at xrCreateInstance.
[[nodiscard]] bool read_rever_presenter(
    const std::filesystem::path& module_directory) noexcept;

// Rever VR's fill (OFXR_REVER_FILL=1 in the game's environment, or [ofxr] fill=1; xrfg/fill_frame_gen.hpp,
// 2026-11-16): on a presenter session the application is handed the display's period and runs at up to the display
// rate; a frame pairs only when it came a slot or more late, with as many synthetics as slots it missed up to the
// multiplier. Inline sessions are left as they are. Off unless set. Read at xrCreateSession.
[[nodiscard]] bool read_fill_frame_gen(
    const std::filesystem::path& module_directory) noexcept;

// Rever VR's fill with the dynamic multiplier (OFXR_REVER_FILL_DYNAMIC=1 in the game's environment, or [ofxr]
// fill_dynamic=1; xrfg/fill_frame_gen.hpp fill_in_force, 2026-11-20): under fill on a presenter session the multiplier
// keeps its dynamic steps and fill is in force only at 2X; at 3X and 4X frames pair as without fill. Off unless set:
// fill as before. Read at xrCreateSession.
[[nodiscard]] bool read_fill_dynamic(
    const std::filesystem::path& module_directory) noexcept;

// Rever VR's video memory guard (xrfg/vram_guard.hpp, KB GVX-060, 2026-11-24): OFXR_REVER_VRAM_GUARD=1 in the game's
// environment, or [ofxr] rever_vram_guard=1 in the armed layer's ini (Rever's app writes it for Microsoft Flight
// Simulator, which Steam starts again without Rever's environment). Off unless set: the layer exactly as before. Read
// at xrCreateSession.
[[nodiscard]] bool read_rever_vram_guard(
    const std::filesystem::path& module_directory) noexcept;
// `[ofxr] rever_vram_margin_mb`: how far under the budget the guard acts, in MB (1024 by default, 256 to 8192;
// OFXR_REVER_VRAM_MARGIN_MB in the game's environment wins, as gpu_margin's).
[[nodiscard]] std::uint32_t read_rever_vram_margin_mb(
    const std::filesystem::path& module_directory) noexcept;
// `[ofxr] rever_vram_budget_extra_mb`: the MB Rever's runtime takes off the budget every DXGI caller of the process is
// told (REVER_XR_VRAM_RESERVE_GB, KB GVX-058), added back by the guard so it judges Windows' own budget (0 by default,
// 0 to 65536; OFXR_REVER_VRAM_BUDGET_EXTRA_MB wins).
[[nodiscard]] std::uint32_t read_rever_vram_budget_extra_mb(
    const std::filesystem::path& module_directory) noexcept;

// `[ofxr] excluded_processes`: executable names, separated by ';', in which
// the layer declines to load at negotiation. Absent, it is Pimax Home: a
// home environment is an OpenXR application like any other, takes the
// headset the moment a game leaves VR, and would otherwise carry the
// layer's buffers at the headset's full resolution - about 4 GB at a
// Crystal's - for as long as the game is out of VR, on top of the game's
// own. Written out explicitly empty, it excludes nothing.
inline constexpr wchar_t kDefaultExcludedProcesses[] = L"PimaxHome-Win64-Shipping.exe";
[[nodiscard]] std::vector<std::wstring> read_excluded_processes(
    const std::filesystem::path& module_directory) noexcept;

// Whether an executable's file name is in the list, compared without case.
[[nodiscard]] bool executable_is_excluded(
    std::wstring_view executable,
    const std::vector<std::wstring>& excluded) noexcept;

// This process's executable file name, without its directory.
[[nodiscard]] std::wstring current_executable_name() noexcept;

// Held by every running session that cannot follow the "3X Frame Gen"
// switch live - one started with "Prefer FPS over latency" off, or one the
// runtime's swapchain limit pushed back to a single synthetic. The tray asks
// whether any exists to tell the user a game restart is needed. A named
// event, so the handle count is the session count and nothing outlives the
// last session.
class FixedFrameMultiplierMarker {
public:
    FixedFrameMultiplierMarker() noexcept = default;
    ~FixedFrameMultiplierMarker();
    FixedFrameMultiplierMarker(const FixedFrameMultiplierMarker&) = delete;
    FixedFrameMultiplierMarker& operator=(const FixedFrameMultiplierMarker&) = delete;
    void hold() noexcept;
    void release() noexcept;

private:
    void* event_{};
};

// Whether a running game holds the marker above.
[[nodiscard]] bool running_session_needs_restart_for_3x() noexcept;

// `[ofxr] vulkan_bridge`: whether the layer generates for Vulkan sessions.
// On unless the key says 0. A Vulkan session needs the queue-serialising
// Vulkan layer registered beside this one, which the tray does at every arm
// unless tray.ini says vulkan_bridge=0; without it the presenter's
// submissions race the game's on its queue.
// [ofxr] d3d11_bridge: a D3D11 session is given to the runtime as a D3D12
// one, with the application's images bridged through shared textures. On
// unless the key says 0; kept as a switch for diagnosing a title against the
// direct D3D11 path.
[[nodiscard]] bool read_d3d11_bridge(
    const std::filesystem::path& module_directory) noexcept;
[[nodiscard]] bool read_vulkan_support(
    const std::filesystem::path& module_directory) noexcept;

[[nodiscard]] ConfiguredNvidiaOptions read_nvidia_options(
    const std::filesystem::path& module_directory) noexcept;

[[nodiscard]] bool register_manifest(
    const std::filesystem::path& manifest,
    RegistryScope scope,
    std::wstring* error = nullptr,
    std::wstring_view registry_subkey = kRegistrySubkey) noexcept;

[[nodiscard]] bool unregister_manifest(
    const std::filesystem::path& manifest,
    RegistryScope scope,
    std::wstring* error = nullptr,
    std::wstring_view registry_subkey = kRegistrySubkey) noexcept;

[[nodiscard]] bool manifest_registered(
    const std::filesystem::path& manifest,
    RegistryScope scope,
    std::wstring_view registry_subkey = kRegistrySubkey) noexcept;

// prefix says which of the tray's manifests: the OpenXR layer's
// (kManifestPrefix) or the Vulkan layer's (kVulkanManifestPrefix).
[[nodiscard]] bool owned_manifest_path(
    const std::filesystem::path& manifest,
    const std::filesystem::path& runtime_directory,
    std::wstring_view prefix = kManifestPrefix) noexcept;

// Recognizes only tray-owned manual manifests in RuntimeLayer (legacy) or
// RuntimeLayer/vNNN. Does not require the manifest or DLL to still exist.
[[nodiscard]] bool owned_registration_path(
    const std::filesystem::path& manifest,
    const std::filesystem::path& local_directory,
    std::wstring_view prefix = kManifestPrefix) noexcept;

// Disable before deleting; read back registry state, and retain the JSON when
// deregistration fails. The caller must report a false result, never claim Off.
[[nodiscard]] bool retire_manifest(
    const std::filesystem::path& manifest,
    RegistryScope scope,
    std::wstring* error = nullptr,
    std::wstring_view registry_subkey = kRegistrySubkey) noexcept;

// Registry-first recovery across ALL versions, including missing JSON files.
// Removes only owned registration values and owned JSONs, never DLLs/settings.
[[nodiscard]] bool cleanup_owned_registrations(
    const std::filesystem::path& local_directory,
    RegistryScope scope,
    std::wstring* error = nullptr,
    std::wstring_view registry_subkey = kRegistrySubkey,
    std::wstring_view prefix = kManifestPrefix) noexcept;

} // namespace xrfg::implicit_layer
