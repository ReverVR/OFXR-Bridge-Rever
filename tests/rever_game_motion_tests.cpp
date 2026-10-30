#include "xrfg/rever_game_motion.hpp"

#include "xrfg/d3d11_d3d12_interop.hpp"
#include "xrfg/dlss_motion_vectors.hpp"

#include <windows.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

using Views = std::array<xrfg::ReverGameMotionView, 2>;

constexpr std::uint64_t kSwapchain = 0x1234;

// Lone Echo II: one 6082x3041 swapchain, both eyes side by side.
void side_by_side_lone_echo() {
    const Views views{{
        {kSwapchain, 0, 0, 0, 3041, 3041},
        {kSwapchain, 0, 3041, 0, 3041, 3041},
    }};
    const auto plan = xrfg::plan_rever_game_motion(views, 6082, 3041, 1, 2432, 2432);
    require(plan.layout == xrfg::ReverGameMotionLayout::side_by_side,
            "Lone Echo II's swapchain is side by side");
    require(plan.swapchain == kSwapchain && plan.stream_count == 1,
            "side by side publishes one stream");
    const auto& stream = plan.streams[0];
    require(stream.producer_eye[0] == 0 && stream.producer_eye[1] == 1,
            "eye 0 on the left half, eye 1 on the right");
    require(stream.texture_width == 4864 && stream.texture_height == 2432,
            "double-width render-size texture");
    require(stream.output_x == 0 && stream.output_y == 0 &&
                stream.output_width == 6082 && stream.output_height == 3041,
            "output is the union of both eye rectangles");
    require(stream.motion_width == 4864 && stream.motion_height == 2432,
            "motion rectangle is the whole texture");
    // The synthesizer's scale is output / motion per axis; both halves keep
    // the per-eye ratio 3041 / 2432.
    const double scale_x = static_cast<double>(stream.output_width) / stream.motion_width;
    const double scale_y = static_cast<double>(stream.output_height) / stream.motion_height;
    require(scale_x == 3041.0 / 2432.0 && scale_y == 3041.0 / 2432.0,
            "per-eye render-to-swapchain scale preserved");
}

void side_by_side_eyes_swapped() {
    const Views views{{
        {kSwapchain, 0, 1000, 20, 1000, 900},
        {kSwapchain, 0, 0, 20, 1000, 900},
    }};
    const auto plan = xrfg::plan_rever_game_motion(views, 2000, 1000, 1, 800, 720);
    require(plan.layout == xrfg::ReverGameMotionLayout::side_by_side,
            "swapped side by side is still side by side");
    const auto& stream = plan.streams[0];
    require(stream.producer_eye[0] == 1 && stream.producer_eye[1] == 0,
            "the right eye's vectors go to the left half when its rect is left");
    require(stream.output_x == 0 && stream.output_y == 20 &&
                stream.output_width == 2000 && stream.output_height == 900,
            "union starts at the left rectangle");
}

void array_slices() {
    const Views views{{
        {kSwapchain, 0, 0, 0, 2000, 2000},
        {kSwapchain, 1, 0, 0, 2000, 2000},
    }};
    const auto plan = xrfg::plan_rever_game_motion(views, 2000, 2000, 2, 1600, 1600);
    require(plan.layout == xrfg::ReverGameMotionLayout::array_slices &&
                plan.stream_count == 2,
            "array swapchain publishes one stream per slice");
    for (std::uint32_t slice = 0; slice < 2; ++slice) {
        const auto& stream = plan.streams[slice];
        require(stream.producer_eye[0] == slice, "slice order follows view order");
        require(stream.texture_width == 1600 && stream.motion_width == 1600 &&
                    stream.output_width == 2000,
                "per-slice render-size texture onto the eye rect");
    }
    const Views swapped{{
        {kSwapchain, 1, 0, 0, 2000, 2000},
        {kSwapchain, 0, 0, 0, 2000, 2000},
    }};
    const auto reversed = xrfg::plan_rever_game_motion(swapped, 2000, 2000, 2, 1600, 1600);
    require(reversed.streams[0].producer_eye[0] == 1 &&
                reversed.streams[1].producer_eye[0] == 0,
            "a view on slice 1 publishes into stream 1");
}

void unsupported_shapes() {
    const auto unsupported = xrfg::ReverGameMotionLayout::unsupported;
    // Two swapchains, one per eye.
    require(xrfg::plan_rever_game_motion(
                Views{{{1, 0, 0, 0, 100, 100}, {2, 0, 0, 0, 100, 100}}},
                100, 100, 1, 80, 80).layout == unsupported,
            "one swapchain per eye is not mapped");
    // A gap between the eyes.
    require(xrfg::plan_rever_game_motion(
                Views{{{kSwapchain, 0, 0, 0, 100, 100},
                       {kSwapchain, 0, 110, 0, 100, 100}}},
                210, 100, 1, 80, 80).layout == unsupported,
            "non-adjacent side-by-side eyes are not mapped");
    // Different heights.
    require(xrfg::plan_rever_game_motion(
                Views{{{kSwapchain, 0, 0, 0, 100, 100},
                       {kSwapchain, 0, 100, 0, 100, 90}}},
                200, 100, 1, 80, 80).layout == unsupported,
            "unequal eyes are not mapped");
    // Out of bounds.
    require(xrfg::plan_rever_game_motion(
                Views{{{kSwapchain, 0, 0, 0, 100, 100},
                       {kSwapchain, 0, 100, 0, 100, 100}}},
                150, 100, 1, 80, 80).layout == unsupported,
            "a rect outside the swapchain is not mapped");
    // No render size yet.
    require(xrfg::plan_rever_game_motion(
                Views{{{kSwapchain, 0, 0, 0, 100, 100},
                       {kSwapchain, 0, 100, 0, 100, 100}}},
                200, 100, 1, 0, 0).layout == unsupported,
            "no producer size, nothing mapped");
    // Same slice on an array swapchain.
    require(xrfg::plan_rever_game_motion(
                Views{{{kSwapchain, 0, 0, 0, 100, 100},
                       {kSwapchain, 0, 0, 0, 100, 100}}},
                100, 100, 2, 80, 80).layout == unsupported,
            "both views on one slice of an array is not mapped");
}

void plan_comparison() {
    const Views views{{
        {kSwapchain, 0, 0, 0, 3041, 3041},
        {kSwapchain, 0, 3041, 0, 3041, 3041},
    }};
    const auto a = xrfg::plan_rever_game_motion(views, 6082, 3041, 1, 2432, 2432);
    const auto b = xrfg::plan_rever_game_motion(views, 6082, 3041, 1, 2432, 2432);
    const auto c = xrfg::plan_rever_game_motion(views, 6082, 3041, 1, 2400, 2432);
    require(xrfg::same_rever_game_motion_plan(a, b), "identical plans compare equal");
    require(!xrfg::same_rever_game_motion_plan(a, c), "a new render size is a new plan");
}

void environment_switch() {
    SetEnvironmentVariableW(L"OFXR_REVER_GAME_MOTION", nullptr);
    require(!xrfg::rever_game_motion_requested(), "absent: off");
    SetEnvironmentVariableW(L"OFXR_REVER_GAME_MOTION", L"0");
    require(!xrfg::rever_game_motion_requested(), "0: off");
    SetEnvironmentVariableW(L"OFXR_REVER_GAME_MOTION", L"1");
    require(xrfg::rever_game_motion_requested(), "1: on");
    SetEnvironmentVariableW(L"OFXR_REVER_GAME_MOTION", nullptr);
}

using Microsoft::WRL::ComPtr;

void require_hr(HRESULT result, const char* what) {
    if (FAILED(result)) {
        char text[160];
        std::snprintf(text, sizeof(text), "%s failed: 0x%08lX", what,
                      static_cast<unsigned long>(result));
        throw std::runtime_error(text);
    }
}

// The producer's block, as Rever Inject lays it out.
struct ProducerBlock {
    std::uint32_t magic, version, pid;
    volatile LONG generation;
    std::uint32_t width, height, flags, reserved;
    struct Eye {
        volatile LONG seq;
        std::uint32_t phase, reserved[2];
    } eyes[2];
};

// A fake producer on a hardware D3D11 device, the consumer on the same
// device (as in the game), and OFXR's D3D12 device made the way the layer
// makes it. Checks: only a frame whose two eye sequences moved is published,
// a producer that keeps its textures publishes nothing, the side-by-side
// texture holds eye 0 on its left half and eye 1 on its right, and
// consecutive publications chain serials the way valid_game_motion_pair
// expects.
void producer_channel_end_to_end() {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
            D3D11_SDK_VERSION, device.GetAddressOf(), nullptr, context.GetAddressOf()))) {
        std::cout << "SKIP end-to-end: no hardware D3D11 device\n";
        return;
    }
    ComPtr<ID3D12Device> device12;
    ComPtr<ID3D12CommandQueue> queue;
    if (FAILED(xrfg::create_d3d12_device_for_d3d11(
            device.Get(), device12.GetAddressOf(), queue.GetAddressOf()))) {
        std::cout << "SKIP end-to-end: no D3D12 device on the adapter\n";
        return;
    }
    constexpr std::uint32_t kRender = 16;
    constexpr std::uint32_t kEye = 20;
    // A private channel name: a game running Rever Inject in this Windows
    // session owns the real one, and the test must never touch it.
    wchar_t channel[96];
    swprintf_s(channel, L"Local\\ReverVR_DLSS_SR_Inputs_OFXRTest_%lu", GetCurrentProcessId());
    HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
        sizeof(ProducerBlock), channel);
    require(mapping != nullptr, "CreateFileMapping");
    auto* block = static_cast<ProducerBlock*>(
        MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0, sizeof(ProducerBlock)));
    require(block != nullptr, "MapViewOfFile");
    std::memset(block, 0, sizeof(*block));
    block->magic = 0x49535652;
    block->version = 1;
    block->pid = GetCurrentProcessId();
    block->generation = 7;
    block->width = kRender;
    block->height = kRender;

    std::vector<HANDLE> shared_handles;
    std::array<ComPtr<ID3D11Texture2D>, 2> motion;
    std::array<ComPtr<IDXGIKeyedMutex>, 2> keyed;
    std::array<ComPtr<ID3D11Texture2D>, 2> depth;
    std::array<ComPtr<IDXGIKeyedMutex>, 2> depth_keyed;
    std::array<ComPtr<ID3D11RenderTargetView>, 2> target;
    for (std::uint32_t eye = 0; eye < 2; ++eye) {
        D3D11_TEXTURE2D_DESC description{};
        description.Width = kRender;
        description.Height = kRender;
        description.MipLevels = 1;
        description.ArraySize = 1;
        description.Format = DXGI_FORMAT_R16G16_FLOAT;
        description.SampleDesc.Count = 1;
        description.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        description.MiscFlags =
            D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
        require_hr(device->CreateTexture2D(&description, nullptr, motion[eye].GetAddressOf()),
                   "producer texture");
        ComPtr<IDXGIResource1> resource;
        require_hr(motion[eye].As(&resource), "IDXGIResource1");
        wchar_t name[96];
        swprintf_s(name, L"Local\\ReverVR_SR_%u_%ld_%uM", block->pid, block->generation, eye);
        HANDLE handle{};
        require_hr(resource->CreateSharedHandle(nullptr,
                       DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, name, &handle),
                   "named shared handle");
        shared_handles.push_back(handle);
        require_hr(motion[eye].As(&keyed[eye]), "keyed mutex");
        require_hr(device->CreateRenderTargetView(motion[eye].Get(), nullptr,
                       target[eye].GetAddressOf()), "producer view");
        // the depth beside the vectors, as Rever Inject makes it
        description.Format = DXGI_FORMAT_R32_FLOAT;
        require_hr(device->CreateTexture2D(&description, nullptr, depth[eye].GetAddressOf()),
                   "producer depth");
        ComPtr<IDXGIResource1> depth_resource;
        require_hr(depth[eye].As(&depth_resource), "depth IDXGIResource1");
        swprintf_s(name, L"Local\\ReverVR_SR_%u_%ld_%uD", block->pid, block->generation, eye);
        require_hr(depth_resource->CreateSharedHandle(nullptr,
                       DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, name, &handle),
                   "named shared depth handle");
        shared_handles.push_back(handle);
        require_hr(depth[eye].As(&depth_keyed[eye]), "depth keyed mutex");
    }
    float base = 1.0F;
    const auto produce = [&](bool release_to_consumer) {
        for (std::uint32_t eye = 0; eye < 2; ++eye) {
            require(keyed[eye]->AcquireSync(0, 1000) == S_OK, "producer acquire");
            const float value[4] = {base + 2.0F * static_cast<float>(eye),
                                    base + 2.0F * static_cast<float>(eye) + 1.0F, 0.0F, 0.0F};
            context->ClearRenderTargetView(target[eye].Get(), value);
            // Kept (key 0): the sequence moves but the consumer cannot take it.
            require_hr(keyed[eye]->ReleaseSync(release_to_consumer ? 1 : 0),
                       "producer release");
            require(depth_keyed[eye]->AcquireSync(0, 1000) == S_OK, "producer depth acquire");
            require_hr(depth_keyed[eye]->ReleaseSync(release_to_consumer ? 1 : 0),
                       "producer depth release");
            InterlockedIncrement(&block->eyes[eye].seq);
        }
        context->Flush();
    };

    D3D12_RESOURCE_DESC output_description{};
    output_description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    output_description.Width = kEye * 2;
    output_description.Height = kEye;
    output_description.DepthOrArraySize = 1;
    output_description.MipLevels = 1;
    output_description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    output_description.SampleDesc.Count = 1;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    ComPtr<ID3D12Resource> output;
    require_hr(device12->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE,
                   &output_description, D3D12_RESOURCE_STATE_COMMON, nullptr,
                   IID_PPV_ARGS(output.GetAddressOf())), "output");

    xrfg::configure_dlss_motion_vector_tracking(true);
    const auto published = [] { return xrfg::dlss_motion_vector_statistics().published; };
    {
        xrfg::ReverGameMotionSource source(channel);
        constexpr auto eye_size = static_cast<std::int32_t>(kEye);
        source.set_views(true, Views{{
            {kSwapchain, 0, 0, 0, eye_size, eye_size},
            {kSwapchain, 0, eye_size, 0, eye_size, eye_size},
        }});
        const auto release = [&] {
            source.publish_for_release(kSwapchain, output.Get(), device.Get(), context.Get(),
                                       device12.Get(), queue.Get());
        };
        release();
        require(published() == 0, "nothing written yet: nothing published");
        produce(false);
        release();
        require(published() == 0, "producer kept the textures: nothing published");
        const std::uint32_t own_pid = block->pid;
        block->pid = own_pid ^ 0x40000U;
        produce(true);
        release();
        require(published() == 0, "another process's block is never taken");
        block->pid = own_pid;
        source.publish_for_release(0x9999, output.Get(), device.Get(), context.Get(),
                                   device12.Get(), queue.Get());
        require(published() == 0, "another swapchain publishes nothing");
        release();
        require(published() == 1, "a new frame publishes one side-by-side stream");
        // all four handed back: Rever Inject offers its next frame only then
        for (std::uint32_t eye = 0; eye < 2; ++eye) {
            require(keyed[eye]->AcquireSync(0, 0) == S_OK, "the vectors handed back");
            require(depth_keyed[eye]->AcquireSync(0, 0) == S_OK, "the depth handed back");
            require_hr(keyed[eye]->ReleaseSync(0), "vectors kept");
            require_hr(depth_keyed[eye]->ReleaseSync(0), "depth kept");
        }
        release();
        require(published() == 1, "the same frame again publishes nothing");

        auto set = xrfg::resolve_dlss_motion_vectors(output.Get(), queue.Get());
        require(set && set->eye_count == 1 && set->eyes[0], "array-size-1 output resolves one eye");
        const auto first = set->eyes[0];
        require(first->output_x == 0 && first->output_width == kEye * 2 &&
                    first->output_height == kEye && first->motion_width == kRender * 2 &&
                    first->motion_height == kRender && first->serial == 1,
                "rectangles of the side-by-side publication");

        // Read the snapshot back.
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> list;
        require_hr(device12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                       IID_PPV_ARGS(allocator.GetAddressOf())), "allocator");
        require_hr(device12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                       allocator.Get(), nullptr, IID_PPV_ARGS(list.GetAddressOf())), "list");
        const D3D12_RESOURCE_DESC snapshot = first->motion_vectors->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        UINT64 total = 0;
        device12->GetCopyableFootprints(&snapshot, 0, 1, 0, &footprint, nullptr, nullptr, &total);
        D3D12_HEAP_PROPERTIES readback_heap{};
        readback_heap.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC buffer{};
        buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width = total;
        buffer.Height = 1;
        buffer.DepthOrArraySize = 1;
        buffer.MipLevels = 1;
        buffer.SampleDesc.Count = 1;
        buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> readback;
        require_hr(device12->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE,
                       &buffer, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                       IID_PPV_ARGS(readback.GetAddressOf())), "readback");
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = first->motion_vectors.Get();
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = first->resource_state;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        list->ResourceBarrier(1, &barrier);
        D3D12_TEXTURE_COPY_LOCATION to{};
        to.pResource = readback.Get();
        to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        to.PlacedFootprint = footprint;
        D3D12_TEXTURE_COPY_LOCATION from{};
        from.pResource = first->motion_vectors.Get();
        from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
        list->ResourceBarrier(1, &barrier);
        require_hr(list->Close(), "close");
        ID3D12CommandList* lists[] = {list.Get()};
        queue->ExecuteCommandLists(1, lists);
        ComPtr<ID3D12Fence> fence;
        require_hr(device12->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                       IID_PPV_ARGS(fence.GetAddressOf())), "fence");
        require_hr(queue->Signal(fence.Get(), 1), "signal");
        HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        require_hr(fence->SetEventOnCompletion(1, event), "event");
        require(WaitForSingleObject(event, 5000) == WAIT_OBJECT_0, "readback wait");
        CloseHandle(event);
        void* mapped = nullptr;
        require_hr(readback->Map(0, nullptr, &mapped), "map");
        const auto* bytes = static_cast<const std::uint8_t*>(mapped) + footprint.Offset;
        const auto pixel = [&](std::uint32_t x, std::uint32_t y) {
            std::uint16_t value[2];
            std::memcpy(value, bytes + y * footprint.Footprint.RowPitch + x * 4, 4);
            return (static_cast<std::uint32_t>(value[0]) << 16) | value[1];
        };
        // 1.0, 2.0 on the left (eye 0); 3.0, 4.0 on the right (eye 1).
        require(pixel(0, 0) == 0x3C004000U && pixel(kRender - 1, kRender - 1) == 0x3C004000U,
                "left half holds eye 0's vectors");
        require(pixel(kRender, 0) == 0x42004400U &&
                    pixel(kRender * 2 - 1, kRender - 1) == 0x42004400U,
                "right half holds eye 1's vectors");
        readback->Unmap(0, nullptr);

        base = 5.0F;
        produce(true);
        release();
        require(published() == 2, "the next frame publishes again");
        set = xrfg::resolve_dlss_motion_vectors(output.Get(), queue.Get());
        require(set && set->eyes[0]->serial == 2 && set->eyes[0]->previous_serial == 1 &&
                    set->eyes[0]->epoch == first->epoch && !set->eyes[0]->reset,
                "consecutive publications chain for the game-motion pair");
    }
    xrfg::configure_dlss_motion_vector_tracking(false);
    xrfg::retire_disabled_dlss_motion_vector_resources();
    for (HANDLE handle : shared_handles) CloseHandle(handle);
    UnmapViewOfFile(block);
    CloseHandle(mapping);
    std::cout << "end-to-end producer channel passed\n";
}

}  // namespace

int main() {
    // the game motion's text log in the temporary folder, never the user's ReverVR folder
    wchar_t temporary[MAX_PATH] = {};
    if (GetTempPathW(MAX_PATH, temporary)) SetEnvironmentVariableW(L"LOCALAPPDATA", temporary);
    try {
        side_by_side_lone_echo();
        side_by_side_eyes_swapped();
        array_slices();
        unsupported_shapes();
        plan_comparison();
        environment_switch();
        producer_channel_end_to_end();
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    std::cout << "rever_game_motion tests passed\n";
    return 0;
}
