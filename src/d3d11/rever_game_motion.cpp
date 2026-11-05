#include "xrfg/rever_game_motion.hpp"

#include "xrfg/bridge_flight_logger.hpp"
#include "xrfg/dlss_motion_vectors.hpp"

#include <windows.h>
#include <share.h>
#include <tlhelp32.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>

namespace xrfg {

using Microsoft::WRL::ComPtr;

namespace {

// What Rever Inject writes in Local\ReverVR_DLSS_SR_Inputs. All 32-bit,
// little endian. Read-only here; the protocol belongs to the producer.
struct ReverInputsBlock {
    std::uint32_t magic;
    std::uint32_t version;
    std::uint32_t pid;
    volatile LONG generation;
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t flags;
    std::uint32_t reserved;
    struct Eye {
        volatile LONG seq;
        std::uint32_t phase;
        std::uint32_t reserved[2];
    } eyes[2];
};
static_assert(sizeof(ReverInputsBlock) == 64);

constexpr std::uint32_t kReverInputsMagic = 0x49535652;  // "RVSI"
constexpr std::uint32_t kReverInputsVersion = 1;
// Alien's producer (Rever's 32-bit bridge): version 1's layout, flag 2 - each
// eye's textures its whole half of the side-by-side image
constexpr std::uint32_t kReverInputsVersionHalves = 2;
constexpr std::uint32_t kFlagEyeHalves = 2;
constexpr wchar_t kReverInputsName[] = L"Local\\ReverVR_DLSS_SR_Inputs";
constexpr std::size_t kListCount = 4;
// "RVGM" in the high half keeps these stream ids apart from a DLSS
// provider's, which are its own handles.
constexpr std::uint64_t kStreamIdBase = 0x5256474D00000000ULL;
constexpr auto kOpenRetry = std::chrono::seconds(1);
constexpr auto kStatisticsPeriod = std::chrono::seconds(5);

std::atomic<std::uint64_t> g_next_stream_block{1};

// Flight-log result codes of BridgeFlightOperation::rever_game_motion.
enum : std::int64_t {
    kEventChannelOpened = 1,
    kEventLayout = 2,
    kEventFirstPublish = 3,
    kEventStatistics = 4,
    kEventTexturesOpened = 5,
    kEventChannelMissing = 6,
    kEventChannelForeign = 7,
};
// Failure stages, in `a` of a negative (HRESULT) record.
enum : std::uint64_t {
    kStageDevices = 1,
    kStageFence = 2,
    kStageLists = 3,
    kStageOpenTexture = 4,
    kStageTextureShape = 5,
    kStageDestination = 6,
    kStageSubmit = 7,
    kStageBlock = 8,
};

void log_event(std::int64_t result, std::uint64_t a = 0, std::uint64_t b = 0,
               std::uint64_t c = 0) noexcept {
    bridge_flight_logger().event(
        BridgeFlightOperation::rever_game_motion, result, a, b, c);
    static const char* const names[] = {"", "channel opened", "layout", "first publish", "statistics",
                                        "textures opened", "channel missing", "channel foreign"};
    if (result > 0 && result <= kEventChannelForeign) {
        rever_game_motion_note("%s (%llu, %llu, %llu)", names[result], static_cast<unsigned long long>(a),
                               static_cast<unsigned long long>(b), static_cast<unsigned long long>(c));
    } else {
        rever_game_motion_note("failed: HRESULT 0x%08llx at stage %llu (%llu, %llu)",
                               static_cast<unsigned long long>(result) & 0xFFFFFFFFULL,
                               static_cast<unsigned long long>(a), static_cast<unsigned long long>(b),
                               static_cast<unsigned long long>(c));
    }
}

struct UniqueHandle {
    HANDLE value{};
    ~UniqueHandle() {
        if (value != nullptr) CloseHandle(value);
    }
};

bool view_in_bounds(const ReverGameMotionView& view, std::uint32_t width,
                    std::uint32_t height) noexcept {
    return view.x >= 0 && view.y >= 0 && view.width > 0 && view.height > 0 &&
        static_cast<std::uint64_t>(view.x) + static_cast<std::uint64_t>(view.width) <=
            width &&
        static_cast<std::uint64_t>(view.y) + static_cast<std::uint64_t>(view.height) <=
            height;
}

}  // namespace

void rever_game_motion_note(const char* format, ...) noexcept {
    static std::mutex lock;
    static FILE* file = nullptr;
    static bool tried = false;
    std::scoped_lock guard(lock);
    if (!tried) {
        tried = true;
        wchar_t local[MAX_PATH] = {};
        // only in a game Rever's app asked it for (the tests never write the real log)
        if (rever_game_motion_requested() && GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH)) {
            const std::wstring path = std::wstring(local) + L"/ReverVR/ofxr_game_motion.log";
            file = _wfsopen(path.c_str(), L"w", _SH_DENYNO);
        }
    }
    if (!file) return;
    SYSTEMTIME now{};
    GetLocalTime(&now);
    std::fprintf(file, "%02u:%02u:%02u.%03u ", now.wHour, now.wMinute, now.wSecond, now.wMilliseconds);
    va_list args;
    va_start(args, format);
    std::vfprintf(file, format, args);
    va_end(args);
    std::fputc(10, file);  // a line feed
    std::fflush(file);
}

bool rever_game_motion_requested() noexcept {
    wchar_t value[8] = {};
    const DWORD length =
        GetEnvironmentVariableW(L"OFXR_REVER_GAME_MOTION", value, 8);
    return length > 0 && length < 8 && value[0] == L'1';
}

ReverGameMotionPlan plan_rever_game_motion(
    const std::array<ReverGameMotionView, 2>& views,
    std::uint32_t swapchain_width,
    std::uint32_t swapchain_height,
    std::uint32_t swapchain_array_size,
    std::uint32_t render_width,
    std::uint32_t render_height) noexcept {
    ReverGameMotionPlan plan{};
    const ReverGameMotionView& a = views[0];
    const ReverGameMotionView& b = views[1];
    if (render_width == 0 || render_height == 0 || swapchain_width == 0 ||
        swapchain_height == 0 || a.swapchain == 0 || a.swapchain != b.swapchain ||
        !view_in_bounds(a, swapchain_width, swapchain_height) ||
        !view_in_bounds(b, swapchain_width, swapchain_height)) {
        return plan;
    }
    if (swapchain_array_size == 2 && a.array_index < 2 && b.array_index < 2 &&
        a.array_index != b.array_index) {
        plan.layout = ReverGameMotionLayout::array_slices;
        plan.swapchain = a.swapchain;
        plan.stream_count = 2;
        for (std::uint32_t eye = 0; eye < 2; ++eye) {
            const ReverGameMotionView& view = views[eye];
            ReverGameMotionStreamPlan& stream = plan.streams[view.array_index];
            stream.producer_eye = {eye, eye};
            stream.texture_width = render_width;
            stream.texture_height = render_height;
            stream.output_x = static_cast<std::uint32_t>(view.x);
            stream.output_y = static_cast<std::uint32_t>(view.y);
            stream.output_width = static_cast<std::uint32_t>(view.width);
            stream.output_height = static_cast<std::uint32_t>(view.height);
            stream.motion_width = render_width;
            stream.motion_height = render_height;
        }
        return plan;
    }
    if (swapchain_array_size == 1 && a.array_index == 0 && b.array_index == 0 &&
        a.y == b.y && a.width == b.width && a.height == b.height) {
        std::uint32_t left_eye = 0;
        if (static_cast<std::int64_t>(b.x) ==
            static_cast<std::int64_t>(a.x) + a.width) {
            left_eye = 0;
        } else if (static_cast<std::int64_t>(a.x) ==
                   static_cast<std::int64_t>(b.x) + b.width) {
            left_eye = 1;
        } else {
            return plan;
        }
        const ReverGameMotionView& left = views[left_eye];
        if (render_width > 0x7FFFFFFFU) return plan;
        plan.layout = ReverGameMotionLayout::side_by_side;
        plan.swapchain = a.swapchain;
        plan.stream_count = 1;
        ReverGameMotionStreamPlan& stream = plan.streams[0];
        stream.producer_eye = {left_eye, 1U - left_eye};
        stream.texture_width = render_width * 2U;
        stream.texture_height = render_height;
        stream.output_x = static_cast<std::uint32_t>(left.x);
        stream.output_y = static_cast<std::uint32_t>(left.y);
        stream.output_width = static_cast<std::uint32_t>(left.width) * 2U;
        stream.output_height = static_cast<std::uint32_t>(left.height);
        stream.motion_width = render_width * 2U;
        stream.motion_height = render_height;
        return plan;
    }
    return plan;
}

std::array<ReverGameMotionView, 2> rever_game_motion_halves(
    const std::array<ReverGameMotionView, 2>& views,
    std::uint32_t swapchain_width,
    std::uint32_t swapchain_height) noexcept {
    if (swapchain_width < 2 || swapchain_width % 2 != 0 || swapchain_height == 0 ||
        swapchain_width > 0x7FFFFFFFU || swapchain_height > 0x7FFFFFFFU) {
        return views;
    }
    const auto half = static_cast<std::int32_t>(swapchain_width / 2U);
    const auto height = static_cast<std::int32_t>(swapchain_height);
    std::array<ReverGameMotionView, 2> halves = views;
    for (std::uint32_t eye = 0; eye < 2; ++eye) {
        halves[eye].array_index = 0;
        halves[eye].x = static_cast<std::int32_t>(eye) * half;
        halves[eye].y = 0;
        halves[eye].width = half;
        halves[eye].height = height;
    }
    return halves;
}

bool rever_game_motion_ours(std::uint32_t block_pid, std::uint32_t own_pid,
                            std::uint32_t parent_pid) noexcept {
    return block_pid != 0 && (block_pid == own_pid || block_pid == parent_pid);
}

std::uint32_t rever_game_motion_parent_pid() noexcept {
    const DWORD own = GetCurrentProcessId();
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    std::uint32_t parent = 0;
    for (BOOL more = Process32FirstW(snapshot, &entry); more; more = Process32NextW(snapshot, &entry)) {
        if (entry.th32ProcessID == own) {
            parent = entry.th32ParentProcessID;
            break;
        }
    }
    CloseHandle(snapshot);
    return parent;
}

std::uint64_t rever_game_motion_handed_key() noexcept {
    wchar_t value[8] = {};
    const DWORD length =
        GetEnvironmentVariableW(L"OFXR_REVER_GAME_MOTION_KEY", value, 8);
    return length == 1 && value[0] == L'2' ? 2U : 1U;
}

bool same_rever_game_motion_plan(
    const ReverGameMotionPlan& left,
    const ReverGameMotionPlan& right) noexcept {
    if (left.layout != right.layout || left.swapchain != right.swapchain ||
        left.stream_count != right.stream_count) {
        return false;
    }
    for (std::uint32_t index = 0; index < left.stream_count && index < 2; ++index) {
        const auto& a = left.streams[index];
        const auto& b = right.streams[index];
        if (a.producer_eye != b.producer_eye || a.texture_width != b.texture_width ||
            a.texture_height != b.texture_height || a.output_x != b.output_x ||
            a.output_y != b.output_y || a.output_width != b.output_width ||
            a.output_height != b.output_height || a.motion_width != b.motion_width ||
            a.motion_height != b.motion_height) {
            return false;
        }
    }
    return true;
}

struct ReverGameMotionSource::Impl {
    struct List {
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> list;
        std::uint64_t fence_value{};
    };

    std::mutex mutex;
    bool disabled{};
    std::wstring channel_name{kReverInputsName};
    bool foreign_logged{};
    // The key Rever's layer above hands the textures on with (1: none above)
    const std::uint64_t handed_key{rever_game_motion_handed_key()};
    // The process that started this one: Alien's 32-bit game, whose producer
    // writes the channel, when OFXR runs in Rever's 64-bit bridge host
    const std::uint32_t parent_pid{rever_game_motion_parent_pid()};

    // The application's stereo projection, from its xrEndFrame.
    bool views_valid{};
    std::array<ReverGameMotionView, 2> views{};

    // Devices, captured at the first release and fixed for the session.
    ComPtr<ID3D11Device> d3d11_device;
    ComPtr<ID3D11Device1> d3d11_device1;
    ComPtr<ID3D11DeviceContext4> d3d11_context4;
    ComPtr<ID3D12Device> d3d12_device;
    ComPtr<ID3D12CommandQueue> d3d12_queue;
    ComPtr<ID3D12Fence> d3d12_fence;
    ComPtr<ID3D11Fence> d3d11_fence;
    HANDLE fence_event{};
    std::uint64_t next_fence_value{1};
    std::uint64_t last_signalled{};
    // The D3D12 queue's last read of the shared destinations; the D3D11
    // copy into them waits for it.
    std::uint64_t last_read{};
    std::array<List, kListCount> lists{};
    std::size_t next_list{};

    // The producer's channel.
    HANDLE mapping{};
    const ReverInputsBlock* block{};
    std::chrono::steady_clock::time_point next_open_attempt{};
    bool missing_logged{};
    bool textures_open{};
    bool attempted_textures{};
    LONG opened_generation{};
    std::uint32_t opened_pid{};
    std::array<ComPtr<ID3D11Texture2D>, 2> producer_motion;
    std::array<ComPtr<IDXGIKeyedMutex>, 2> producer_mutex;
    // The game's depth per eye: not read here, but handed back with the
    // vectors - Rever Inject offers a frame only once all four of its
    // textures are back (first headset run, 2026-10-30: the depth kept the
    // game's key, one frame was offered and every later one left out).
    std::array<ComPtr<ID3D11Texture2D>, 2> producer_depth;
    std::array<ComPtr<IDXGIKeyedMutex>, 2> depth_mutex;
    std::array<std::uint32_t, 2> producer_width{};
    std::array<std::uint32_t, 2> producer_height{};
    std::array<LONG, 2> last_seq{};
    bool have_last_seq{};

    // What is being published, and where.
    ReverGameMotionPlan plan{};
    bool unsupported_logged{};
    std::array<ComPtr<ID3D12Resource>, 2> destination12;
    std::array<ComPtr<ID3D11Texture2D>, 2> destination11;
    std::array<std::uint64_t, 2> stream_ids{};
    bool reset_next{};

    // Statistics.
    std::uint64_t published{};
    std::uint64_t no_new_frame{};
    std::uint64_t busy{};
    std::uint64_t failures{};
    std::chrono::steady_clock::time_point next_statistics{};

    ~Impl() {
        wait_for_idle();
        retire_streams();
        close_channel();
        if (fence_event != nullptr) CloseHandle(fence_event);
    }

    void wait_for_idle() noexcept {
        if (!d3d12_fence || last_signalled == 0 ||
            d3d12_fence->GetCompletedValue() >= last_signalled) {
            return;
        }
        if (fence_event != nullptr &&
            SUCCEEDED(d3d12_fence->SetEventOnCompletion(last_signalled, fence_event))) {
            WaitForSingleObject(fence_event, 2000);
        }
    }

    void retire_streams() noexcept {
        for (auto& id : stream_ids) {
            if (id != 0) retire_dlss_motion_vector_stream(id);
            id = 0;
        }
    }

    void close_textures() noexcept {
        for (std::size_t eye = 0; eye < 2; ++eye) {
            producer_mutex[eye].Reset();
            producer_motion[eye].Reset();
            depth_mutex[eye].Reset();
            producer_depth[eye].Reset();
            producer_width[eye] = 0;
            producer_height[eye] = 0;
        }
        textures_open = false;
        have_last_seq = false;
    }

    void close_channel() noexcept {
        close_textures();
        if (block != nullptr) UnmapViewOfFile(block);
        if (mapping != nullptr) CloseHandle(mapping);
        block = nullptr;
        mapping = nullptr;
    }

    void fail(std::uint64_t stage, HRESULT result) noexcept {
        ++failures;
        log_event(static_cast<std::int64_t>(result), stage);
    }

    [[nodiscard]] bool ensure_devices(
        ID3D11Device* device,
        ID3D11DeviceContext* context,
        ID3D12Device* device12,
        ID3D12CommandQueue* queue) noexcept {
        if (d3d12_fence) {
            // Fixed for the session; anything else is not ours to touch.
            return device == d3d11_device.Get() && device12 == d3d12_device.Get() &&
                queue == d3d12_queue.Get();
        }
        if (device == nullptr || context == nullptr || device12 == nullptr ||
            queue == nullptr) {
            return false;
        }
        ComPtr<ID3D11Device5> device5;
        HRESULT result = device->QueryInterface(IID_PPV_ARGS(d3d11_device1.GetAddressOf()));
        if (SUCCEEDED(result)) result = device->QueryInterface(IID_PPV_ARGS(device5.GetAddressOf()));
        if (SUCCEEDED(result)) {
            result = context->QueryInterface(IID_PPV_ARGS(d3d11_context4.GetAddressOf()));
        }
        if (FAILED(result)) {
            fail(kStageDevices, result);
            disabled = true;
            return false;
        }
        result = device12->CreateFence(0, D3D12_FENCE_FLAG_SHARED,
                                       IID_PPV_ARGS(d3d12_fence.GetAddressOf()));
        UniqueHandle fence_handle;
        if (SUCCEEDED(result)) {
            result = device12->CreateSharedHandle(d3d12_fence.Get(), nullptr, GENERIC_ALL,
                                                  nullptr, &fence_handle.value);
        }
        if (SUCCEEDED(result)) {
            result = device5->OpenSharedFence(fence_handle.value,
                                              IID_PPV_ARGS(d3d11_fence.GetAddressOf()));
        }
        if (SUCCEEDED(result)) {
            fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (fence_event == nullptr) result = HRESULT_FROM_WIN32(GetLastError());
        }
        if (FAILED(result)) {
            d3d12_fence.Reset();
            fail(kStageFence, result);
            disabled = true;
            return false;
        }
        for (List& list : lists) {
            result = device12->CreateCommandAllocator(
                D3D12_COMMAND_LIST_TYPE_DIRECT,
                IID_PPV_ARGS(list.allocator.GetAddressOf()));
            if (SUCCEEDED(result)) {
                result = device12->CreateCommandList(
                    0, D3D12_COMMAND_LIST_TYPE_DIRECT, list.allocator.Get(), nullptr,
                    IID_PPV_ARGS(list.list.GetAddressOf()));
            }
            if (SUCCEEDED(result)) result = list.list->Close();
            if (FAILED(result)) {
                d3d12_fence.Reset();
                fail(kStageLists, result);
                disabled = true;
                return false;
            }
        }
        d3d11_device = device;
        d3d12_device = device12;
        d3d12_queue = queue;
        return true;
    }

    // Maps the producer's block and opens its textures for the current
    // generation. Retries at most once a second while the game has not made
    // them yet.
    [[nodiscard]] bool ensure_channel() noexcept {
        const auto now = std::chrono::steady_clock::now();
        if (block == nullptr) {
            if (now < next_open_attempt) return false;
            next_open_attempt = now + kOpenRetry;
            mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, channel_name.c_str());
            if (mapping == nullptr) {
                if (!missing_logged) {
                    missing_logged = true;
                    log_event(kEventChannelMissing, GetLastError());
                }
                return false;
            }
            block = static_cast<const ReverInputsBlock*>(
                MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, sizeof(ReverInputsBlock)));
            if (block == nullptr || block->magic != kReverInputsMagic ||
                (block->version != kReverInputsVersion &&
                    block->version != kReverInputsVersionHalves)) {
                const std::uint64_t magic = block != nullptr ? block->magic : 0;
                const std::uint64_t version = block != nullptr ? block->version : 0;
                close_channel();
                fail(kStageBlock, E_UNEXPECTED);
                log_event(kEventChannelMissing, 0, magic, version);
                return false;
            }
            log_event(kEventChannelOpened, block->pid,
                      (static_cast<std::uint64_t>(block->width) << 32) | block->height,
                      static_cast<std::uint64_t>(static_cast<std::uint32_t>(block->generation)));
        }
        const LONG generation = block->generation;
        const std::uint32_t pid = block->pid;
        if (!rever_game_motion_ours(pid, GetCurrentProcessId(), parent_pid)) {
            // The channel's name is per Windows session, not per process: a
            // block from another game is never ours to take frames from.
            if (!foreign_logged) {
                foreign_logged = true;
                log_event(kEventChannelForeign, pid, GetCurrentProcessId(), parent_pid);
            }
            return false;
        }
        if (textures_open && generation == opened_generation && pid == opened_pid) {
            return true;
        }
        // The game made its textures again (or never yet): open the new ones.
        close_textures();
        if (attempted_textures && now < next_open_attempt &&
            opened_generation == generation && opened_pid == pid) {
            return false;
        }
        attempted_textures = true;
        next_open_attempt = now + kOpenRetry;
        opened_generation = generation;
        opened_pid = pid;
        // Vectors from the new textures do not continue the old ones.
        reset_next = true;
        for (std::uint32_t eye = 0; eye < 2; ++eye) {
            wchar_t name[96];
            swprintf_s(name, L"Local\\ReverVR_SR_%u_%ld_%uM", pid, generation, eye);
            HRESULT result = d3d11_device1->OpenSharedResourceByName(
                name, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                IID_PPV_ARGS(producer_motion[eye].GetAddressOf()));
            if (SUCCEEDED(result)) {
                result = producer_motion[eye].As(&producer_mutex[eye]);
            }
            D3D11_TEXTURE2D_DESC description{};
            if (SUCCEEDED(result)) {
                producer_motion[eye]->GetDesc(&description);
                if (description.Format != DXGI_FORMAT_R16G16_FLOAT &&
                    description.Format != DXGI_FORMAT_R16G16_TYPELESS) {
                    result = DXGI_ERROR_UNSUPPORTED;
                }
            }
            if (FAILED(result)) {
                close_textures();
                fail(kStageOpenTexture, result);
                return false;
            }
            producer_width[eye] = description.Width;
            producer_height[eye] = description.Height;
            swprintf_s(name, L"Local\\ReverVR_SR_%u_%ld_%uD", pid, generation, eye);
            result = d3d11_device1->OpenSharedResourceByName(
                name, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                IID_PPV_ARGS(producer_depth[eye].GetAddressOf()));
            if (SUCCEEDED(result)) {
                result = producer_depth[eye].As(&depth_mutex[eye]);
            }
            if (FAILED(result)) {
                close_textures();
                fail(kStageOpenTexture, result);
                return false;
            }
        }
        textures_open = true;
        log_event(kEventTexturesOpened, pid,
                  (static_cast<std::uint64_t>(producer_width[0]) << 32) | producer_height[0],
                  static_cast<std::uint64_t>(static_cast<std::uint32_t>(generation)));
        return true;
    }

    [[nodiscard]] bool create_destinations(const ReverGameMotionPlan& next) noexcept {
        // The previous destinations may still be read by a queued snapshot.
        wait_for_idle();
        retire_streams();
        destination12 = {};
        destination11 = {};
        const std::uint64_t block_id =
            g_next_stream_block.fetch_add(1, std::memory_order_relaxed);
        for (std::uint32_t index = 0; index < next.stream_count; ++index) {
            const ReverGameMotionStreamPlan& stream = next.streams[index];
            D3D12_RESOURCE_DESC description{};
            description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            description.Width = stream.texture_width;
            description.Height = stream.texture_height;
            description.DepthOrArraySize = 1;
            description.MipLevels = 1;
            description.Format = DXGI_FORMAT_R16G16_FLOAT;
            description.SampleDesc.Count = 1;
            description.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
            // As the D3D11 bridge shares its colour textures: both devices
            // use it with no state transitions between them.
            description.Flags = D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS |
                D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            D3D12_HEAP_PROPERTIES heap{};
            heap.Type = D3D12_HEAP_TYPE_DEFAULT;
            heap.CreationNodeMask = 1;
            heap.VisibleNodeMask = 1;
            HRESULT result = d3d12_device->CreateCommittedResource(
                &heap, D3D12_HEAP_FLAG_SHARED, &description,
                D3D12_RESOURCE_STATE_COMMON, nullptr,
                IID_PPV_ARGS(destination12[index].GetAddressOf()));
            UniqueHandle handle;
            if (SUCCEEDED(result)) {
                result = d3d12_device->CreateSharedHandle(
                    destination12[index].Get(), nullptr, GENERIC_ALL, nullptr,
                    &handle.value);
            }
            if (SUCCEEDED(result)) {
                result = d3d11_device1->OpenSharedResource1(
                    handle.value, IID_PPV_ARGS(destination11[index].GetAddressOf()));
            }
            if (FAILED(result)) {
                destination12 = {};
                destination11 = {};
                fail(kStageDestination, result);
                return false;
            }
            stream_ids[index] = kStreamIdBase | (block_id << 2) | index;
        }
        return true;
    }

    void report_statistics(bool force) noexcept {
        const auto now = std::chrono::steady_clock::now();
        if (!force && now < next_statistics) return;
        next_statistics = now + kStatisticsPeriod;
        log_event(kEventStatistics, published, no_new_frame,
                  (std::min<std::uint64_t>(busy, 0xFFFFFFFFULL) << 32) |
                      std::min<std::uint64_t>(failures, 0xFFFFFFFFULL));
    }

    void publish(
        std::uint64_t swapchain,
        ID3D12Resource* output,
        ID3D11Device* device,
        ID3D11DeviceContext* context,
        ID3D12Device* device12,
        ID3D12CommandQueue* queue) noexcept {
        if (disabled || !views_valid || output == nullptr) return;
        if (views[0].swapchain != swapchain) return;
        if (!ensure_devices(device, context, device12, queue)) return;
        if (!ensure_channel()) return;
        report_statistics(false);

        const std::uint32_t render_width = std::min({block->width,
            producer_width[0], producer_width[1]});
        const std::uint32_t render_height = std::min({block->height,
            producer_height[0], producer_height[1]});
        const D3D12_RESOURCE_DESC output_description = output->GetDesc();
        const bool eye_halves = block->version == kReverInputsVersionHalves &&
            (block->flags & kFlagEyeHalves) != 0;
        const ReverGameMotionPlan next = plan_rever_game_motion(
            eye_halves ? rever_game_motion_halves(views,
                         static_cast<std::uint32_t>(output_description.Width),
                         output_description.Height)
                   : views,
            static_cast<std::uint32_t>(output_description.Width),
            output_description.Height, output_description.DepthOrArraySize,
            render_width, render_height);
        if (next.layout == ReverGameMotionLayout::unsupported) {
            if (!unsupported_logged) {
                unsupported_logged = true;
                log_event(kEventLayout, 0, swapchain,
                          (static_cast<std::uint64_t>(output_description.Width) << 32) |
                              output_description.DepthOrArraySize);
            }
            return;
        }
        if (!same_rever_game_motion_plan(next, plan) || !destination12[0]) {
            plan = {};
            if (!create_destinations(next)) {
                disabled = true;
                return;
            }
            plan = next;
            reset_next = true;
            unsupported_logged = false;
            log_event(kEventLayout, static_cast<std::uint64_t>(plan.layout), swapchain,
                      (static_cast<std::uint64_t>(plan.streams[0].output_width) << 32) |
                          plan.streams[0].motion_width);
        }

        // Only a frame both eyes have moved on from is a real new frame.
        const std::array<LONG, 2> seq{block->eyes[0].seq, block->eyes[1].seq};
        if (seq[0] == 0 || seq[1] == 0 ||
            (have_last_seq && (seq[0] == last_seq[0] || seq[1] == last_seq[1]))) {
            ++no_new_frame;
            return;
        }
        List& list = lists[next_list];
        if (list.fence_value != 0 &&
            d3d12_fence->GetCompletedValue() < list.fence_value) {
            ++busy;
            return;
        }
        // all four of the frame's textures, or none (each given back with
        // the game's key, as it left them)
        IDXGIKeyedMutex* const frame_mutexes[4] = {producer_mutex[0].Get(), producer_mutex[1].Get(),
                                                   depth_mutex[0].Get(), depth_mutex[1].Get()};
        // with Rever's layer above, its hand-on key first, then the producer's
        // (a frame the layer did not read)
        int held = 0;
        for (const std::uint64_t key : {handed_key, std::uint64_t{1}}) {
            held = 0;
            while (held < 4 && frame_mutexes[held]->AcquireSync(key, 0) == S_OK) ++held;
            if (held == 4) break;
            for (int index = 0; index < held; ++index) frame_mutexes[index]->ReleaseSync(key);
            if (key == 1) break;
        }
        if (held < 4) {
            ++busy;
            return;
        }
        if (last_read != 0) {
            static_cast<void>(d3d11_context4->Wait(d3d11_fence.Get(), last_read));
        }
        const D3D11_BOX box{0, 0, 0, render_width, render_height, 1};
        for (std::uint32_t index = 0; index < plan.stream_count; ++index) {
            const ReverGameMotionStreamPlan& stream = plan.streams[index];
            const std::uint32_t halves =
                plan.layout == ReverGameMotionLayout::side_by_side ? 2U : 1U;
            for (std::uint32_t half = 0; half < halves; ++half) {
                d3d11_context4->CopySubresourceRegion(
                    destination11[index].Get(), 0, half * render_width, 0, 0,
                    producer_motion[stream.producer_eye[half]].Get(), 0, &box);
            }
        }
        for (IDXGIKeyedMutex* const mutex : frame_mutexes) mutex->ReleaseSync(0);
        last_seq = seq;
        have_last_seq = true;

        // The copies, finished on the game's context, before the layer's
        // queue reads the destinations.
        const std::uint64_t copied = next_fence_value++;
        HRESULT result = d3d11_context4->Signal(d3d11_fence.Get(), copied);
        if (SUCCEEDED(result)) {
            d3d11_context4->Flush1(D3D11_CONTEXT_TYPE_ALL, nullptr);
            result = d3d12_queue->Wait(d3d12_fence.Get(), copied);
        }
        if (SUCCEEDED(result)) result = list.allocator->Reset();
        if (SUCCEEDED(result)) result = list.list->Reset(list.allocator.Get(), nullptr);
        if (FAILED(result)) {
            fail(kStageSubmit, result);
            disabled = true;
            return;
        }
        for (std::uint32_t index = 0; index < plan.stream_count; ++index) {
            const ReverGameMotionStreamPlan& stream = plan.streams[index];
            DlssMotionVectorPublication publication{};
            publication.stream = stream_ids[index];
            publication.output = output;
            publication.motion_vectors = destination12[index].Get();
            publication.producer_queue = d3d12_queue.Get();
            publication.producer_command_list = list.list.Get();
            publication.output_x = stream.output_x;
            publication.output_y = stream.output_y;
            publication.output_width = stream.output_width;
            publication.output_height = stream.output_height;
            publication.motion_x = 0;
            publication.motion_y = 0;
            publication.motion_width = stream.motion_width;
            publication.motion_height = stream.motion_height;
            publication.scale_x = 1.0F;
            publication.scale_y = 1.0F;
            // A simultaneous-access texture: the snapshot copy promotes it
            // from COMMON, so no transition is recorded on it.
            publication.resource_state = D3D12_RESOURCE_STATE_COPY_SOURCE;
            publication.jittered = false;
            publication.reset = reset_next;
            publish_dlss_motion_vectors(publication);
        }
        result = list.list->Close();
        if (SUCCEEDED(result)) {
            ID3D12CommandList* const submitted[] = {list.list.Get()};
            d3d12_queue->ExecuteCommandLists(1, submitted);
            const std::uint64_t read = next_fence_value++;
            result = d3d12_queue->Signal(d3d12_fence.Get(), read);
            if (SUCCEEDED(result)) {
                list.fence_value = read;
                last_read = read;
                last_signalled = read;
            }
        }
        if (FAILED(result)) {
            fail(kStageSubmit, result);
            disabled = true;
            return;
        }
        next_list = (next_list + 1) % kListCount;
        reset_next = false;
        if (++published == 1) {
            const auto registry = dlss_motion_vector_statistics();
            log_event(kEventFirstPublish, plan.stream_count, registry.published,
                      registry.snapshot_failures);
            report_statistics(true);
        }
    }
};

ReverGameMotionSource::ReverGameMotionSource(const wchar_t* channel_name)
    : impl_(std::make_unique<Impl>()) {
    if (channel_name != nullptr) impl_->channel_name = channel_name;
}

ReverGameMotionSource::~ReverGameMotionSource() = default;

void ReverGameMotionSource::set_views(
    bool valid,
    const std::array<ReverGameMotionView, 2>& views) noexcept {
    try {
        std::scoped_lock lock(impl_->mutex);
        impl_->views_valid = valid;
        if (valid) impl_->views = views;
    } catch (...) {
    }
}

void ReverGameMotionSource::publish_for_release(
    std::uint64_t swapchain,
    ID3D12Resource* output,
    ID3D11Device* d3d11_device,
    ID3D11DeviceContext* d3d11_context,
    ID3D12Device* d3d12_device,
    ID3D12CommandQueue* d3d12_queue) noexcept {
    try {
        std::scoped_lock lock(impl_->mutex);
        impl_->publish(swapchain, output, d3d11_device, d3d11_context, d3d12_device,
                       d3d12_queue);
    } catch (...) {
    }
}

}  // namespace xrfg
