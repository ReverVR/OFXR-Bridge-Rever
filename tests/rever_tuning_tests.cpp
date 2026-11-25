// Rever VR's live tuning of the turn brake and the GPU-aware shape (KB ALI-082): the readers' defaults,
// ranges, and the game's environment winning over the ini.
#include <windows.h>

#include "xrfg/implicit_layer.hpp"

#include <filesystem>
#include <fstream>
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

void write_ini(const std::filesystem::path& directory, const char* body) {
    std::ofstream stream(directory / L"ofxr_bridge.ini", std::ios::binary | std::ios::trunc);
    stream << "[ofxr]\r\n" << body;
}

void clear_environment() {
    for (const wchar_t* name : {L"OFXR_REVER_TURN_BRAKE", L"OFXR_REVER_TURN_BRAKE_DEG",
                                L"OFXR_REVER_TURN_BRAKE_HOLD_MS", L"OFXR_REVER_TURN_BRAKE_RELEASE",
                                L"OFXR_REVER_GPU_AWARE", L"OFXR_REVER_GPU_MARGIN"}) {
        SetEnvironmentVariableW(name, nullptr);
    }
}

}  // namespace

int main() {
    namespace layer = xrfg::implicit_layer;
    clear_environment();
    std::error_code code;
    const auto directory = std::filesystem::temp_directory_path(code) /
        (L"xrfg_rever_tuning_" + std::to_wstring(GetCurrentProcessId()));
    std::filesystem::create_directories(directory, code);

    // the tray's ini, without the keys: cb142461's values (the Quest's)
    write_ini(directory, "frame_multiplier=3\r\n");
    expect(layer::read_turn_brake(directory), "turn_brake: on");
    expect(layer::read_turn_brake_deg(directory) == 90U, "turn_brake_deg: 90");
    expect(layer::read_turn_brake_hold_ms(directory) == 150U, "turn_brake_hold_ms: 150");
    expect(layer::read_turn_brake_release(directory) == 60U, "turn_brake_release: 60 %");
    expect(layer::read_gpu_aware(directory), "gpu_aware: on");
    expect(layer::read_gpu_margin(directory) == 95U, "gpu_margin: 95 %");
    expect(layer::read_turn_brake_hold_ms({}) == 150U && layer::read_turn_brake_release({}) == 60U &&
               layer::read_gpu_margin({}) == 95U,
           "no folder: the defaults");

    // the ini's own values, inside their ranges
    write_ini(directory, "turn_brake_hold_ms=450\r\nturn_brake_release=40\r\ngpu_margin=90\r\n");
    expect(layer::read_turn_brake_hold_ms(directory) == 450U, "ini hold 450");
    expect(layer::read_turn_brake_release(directory) == 40U, "ini release 40");
    expect(layer::read_gpu_margin(directory) == 90U, "ini margin 90");

    // out of range: the default
    write_ini(directory, "turn_brake_hold_ms=20\r\nturn_brake_release=99\r\ngpu_margin=50\r\n");
    expect(layer::read_turn_brake_hold_ms(directory) == 150U, "hold 20 (under 50): the default");
    expect(layer::read_turn_brake_release(directory) == 60U, "release 99 (over 95): the default");
    expect(layer::read_gpu_margin(directory) == 95U, "margin 50 (under 70): the default");
    write_ini(directory, "turn_brake_hold_ms=2001\r\nturn_brake_release=9\r\ngpu_margin=100\r\n");
    expect(layer::read_turn_brake_hold_ms(directory) == 150U, "hold 2001: the default");
    expect(layer::read_turn_brake_release(directory) == 60U, "release 9: the default");
    expect(layer::read_gpu_margin(directory) == 95U, "margin 100: the default");
    write_ini(directory, "turn_brake_hold_ms=2000\r\nturn_brake_release=10\r\ngpu_margin=99\r\n");
    expect(layer::read_turn_brake_hold_ms(directory) == 2000U, "hold 2000: the top of its range");
    expect(layer::read_turn_brake_release(directory) == 10U, "release 10: the bottom of its range");
    expect(layer::read_gpu_margin(directory) == 99U, "margin 99: the top of its range");

    // the game's environment wins (the Frame's launch: 450 / 40 / 90)
    write_ini(directory, "turn_brake_hold_ms=200\r\nturn_brake_release=70\r\ngpu_margin=80\r\n");
    SetEnvironmentVariableW(L"OFXR_REVER_TURN_BRAKE_HOLD_MS", L"450");
    SetEnvironmentVariableW(L"OFXR_REVER_TURN_BRAKE_RELEASE", L"40");
    SetEnvironmentVariableW(L"OFXR_REVER_GPU_MARGIN", L"90");
    expect(layer::read_turn_brake_hold_ms(directory) == 450U, "environment hold 450 over the ini's 200");
    expect(layer::read_turn_brake_release(directory) == 40U, "environment release 40 over the ini's 70");
    expect(layer::read_gpu_margin(directory) == 90U, "environment margin 90 over the ini's 80");
    // an environment value out of range or not a number: the ini's
    SetEnvironmentVariableW(L"OFXR_REVER_TURN_BRAKE_HOLD_MS", L"5000");
    SetEnvironmentVariableW(L"OFXR_REVER_TURN_BRAKE_RELEASE", L"x");
    SetEnvironmentVariableW(L"OFXR_REVER_GPU_MARGIN", L"");
    expect(layer::read_turn_brake_hold_ms(directory) == 200U, "environment hold 5000: the ini's");
    expect(layer::read_turn_brake_release(directory) == 70U, "environment release x: the ini's");
    expect(layer::read_gpu_margin(directory) == 80U, "an empty environment margin: the ini's");

    clear_environment();

    // Rever's presenter (KB GVX-058): the environment as before, or [ofxr] rever_presenter=1 in the armed layer's ini
    // for a game Steam starts again without Rever's environment
    SetEnvironmentVariableW(L"OFXR_REVER_PRESENTER", nullptr);
    write_ini(directory, "frame_multiplier=4\r\ndynamic_frame_gen=1\r\n");
    expect(!layer::read_rever_presenter(directory), "presenter: off without the key (as before)");
    expect(!layer::read_rever_presenter({}), "presenter: off with no folder");
    write_ini(directory, "dynamic_frame_gen=1\r\nrever_presenter=1\r\n");
    expect(layer::read_rever_presenter(directory), "presenter: on from the ini");
    write_ini(directory, "rever_presenter=0\r\n");
    expect(!layer::read_rever_presenter(directory), "presenter: rever_presenter=0 off");
    SetEnvironmentVariableW(L"OFXR_REVER_PRESENTER", L"1");
    expect(layer::read_rever_presenter(directory), "presenter: the environment's 1 over the ini's 0");
    expect(layer::read_rever_presenter({}), "presenter: the environment's 1 with no folder");
    SetEnvironmentVariableW(L"OFXR_REVER_PRESENTER", L"0");
    write_ini(directory, "rever_presenter=1\r\n");
    expect(layer::read_rever_presenter(directory), "presenter: an environment 0 only never switches the ini's 1 off");
    SetEnvironmentVariableW(L"OFXR_REVER_PRESENTER", nullptr);

    // Rever's video memory guard (KB GVX-060): off without its key (the layer exactly as before), the ini Rever's app
    // writes for Microsoft Flight Simulator, the margin's and the runtime reserve's ranges, the environment first
    for (const wchar_t* name : {L"OFXR_REVER_VRAM_GUARD", L"OFXR_REVER_VRAM_MARGIN_MB", L"OFXR_REVER_VRAM_BUDGET_EXTRA_MB"}) {
        SetEnvironmentVariableW(name, nullptr);
    }
    write_ini(directory, "frame_multiplier=4\r\ndynamic_frame_gen=1\r\nrever_presenter=1\r\n");
    expect(!layer::read_rever_vram_guard(directory), "vram guard: off without the key (as before)");
    expect(!layer::read_rever_vram_guard({}), "vram guard: off with no folder");
    expect(layer::read_rever_vram_margin_mb(directory) == 1024U, "vram margin: 1024 MB without the key");
    expect(layer::read_rever_vram_budget_extra_mb(directory) == 0U, "vram extra: 0 without the key");
    expect(layer::read_rever_vram_margin_mb({}) == 1024U && layer::read_rever_vram_budget_extra_mb({}) == 0U,
           "vram: no folder, the defaults");
    write_ini(directory, "dynamic_frame_gen=1\r\nrever_vram_guard=1\r\nrever_vram_margin_mb=1536\r\n"
                         "rever_vram_budget_extra_mb=4096\r\n");
    expect(layer::read_rever_vram_guard(directory), "vram guard: on from the ini");
    expect(layer::read_rever_vram_margin_mb(directory) == 1536U, "vram margin: the ini's 1536");
    expect(layer::read_rever_vram_budget_extra_mb(directory) == 4096U, "vram extra: the ini's 4096");
    write_ini(directory, "rever_vram_guard=0\r\nrever_vram_margin_mb=255\r\nrever_vram_budget_extra_mb=65537\r\n");
    expect(!layer::read_rever_vram_guard(directory), "vram guard: rever_vram_guard=0 off");
    expect(layer::read_rever_vram_margin_mb(directory) == 1024U, "vram margin 255 (under 256): the default");
    expect(layer::read_rever_vram_budget_extra_mb(directory) == 0U, "vram extra 65537 (over 65536): the default");
    write_ini(directory, "rever_vram_margin_mb=8192\r\nrever_vram_budget_extra_mb=65536\r\n");
    expect(layer::read_rever_vram_margin_mb(directory) == 8192U, "vram margin 8192: the top of its range");
    expect(layer::read_rever_vram_budget_extra_mb(directory) == 65536U, "vram extra 65536: the top of its range");
    write_ini(directory, "rever_vram_guard=0\r\nrever_vram_margin_mb=2048\r\nrever_vram_budget_extra_mb=2048\r\n");
    SetEnvironmentVariableW(L"OFXR_REVER_VRAM_GUARD", L"1");
    SetEnvironmentVariableW(L"OFXR_REVER_VRAM_MARGIN_MB", L"512");
    SetEnvironmentVariableW(L"OFXR_REVER_VRAM_BUDGET_EXTRA_MB", L"0");
    expect(layer::read_rever_vram_guard(directory), "vram guard: the environment's 1 over the ini's 0");
    expect(layer::read_rever_vram_margin_mb(directory) == 512U, "vram margin: the environment's 512 over the ini's");
    expect(layer::read_rever_vram_budget_extra_mb(directory) == 0U, "vram extra: the environment's 0 over the ini's");
    SetEnvironmentVariableW(L"OFXR_REVER_VRAM_GUARD", L"0");
    SetEnvironmentVariableW(L"OFXR_REVER_VRAM_MARGIN_MB", L"x");
    write_ini(directory, "rever_vram_guard=1\r\nrever_vram_margin_mb=2048\r\n");
    expect(layer::read_rever_vram_guard(directory), "vram guard: an environment 0 never switches the ini's 1 off");
    expect(layer::read_rever_vram_margin_mb(directory) == 2048U, "vram margin: an environment x, the ini's");
    for (const wchar_t* name : {L"OFXR_REVER_VRAM_GUARD", L"OFXR_REVER_VRAM_MARGIN_MB", L"OFXR_REVER_VRAM_BUDGET_EXTRA_MB"}) {
        SetEnvironmentVariableW(name, nullptr);
    }

    // The binding queue at high priority (KB GVX-061): off without its key (NORMAL, exactly as before), the ini Rever's
    // app writes for Microsoft Flight Simulator with binding_queue=1, the environment's 1 first; and binding_queue
    // itself still read as before
    SetEnvironmentVariableW(L"OFXR_REVER_BINDING_QUEUE_HIGH", nullptr);
    SetEnvironmentVariableW(L"OFXR_BINDING_QUEUE", nullptr);
    write_ini(directory, "frame_multiplier=4\r\ndynamic_frame_gen=1\r\nrever_presenter=1\r\nrever_vram_guard=1\r\n");
    expect(!layer::read_rever_binding_queue_high(directory), "binding queue high: off without the key (as before)");
    expect(!layer::read_rever_binding_queue_high({}), "binding queue high: off with no folder");
    expect(!layer::read_binding_queue(directory), "binding queue: off without its key (as before)");
    write_ini(directory, "dynamic_frame_gen=1\r\nbinding_queue=1\r\nrever_binding_queue_high=1\r\n");
    expect(layer::read_rever_binding_queue_high(directory), "binding queue high: on from the ini");
    expect(layer::read_binding_queue(directory), "binding queue: on from the ini");
    write_ini(directory, "binding_queue=1\r\nrever_binding_queue_high=0\r\n");
    expect(!layer::read_rever_binding_queue_high(directory), "binding queue high: rever_binding_queue_high=0 off");
    SetEnvironmentVariableW(L"OFXR_REVER_BINDING_QUEUE_HIGH", L"1");
    expect(layer::read_rever_binding_queue_high(directory), "binding queue high: the environment's 1 over the ini's 0");
    expect(layer::read_rever_binding_queue_high({}), "binding queue high: the environment's 1 with no folder");
    SetEnvironmentVariableW(L"OFXR_REVER_BINDING_QUEUE_HIGH", L"0");
    write_ini(directory, "rever_binding_queue_high=1\r\n");
    expect(layer::read_rever_binding_queue_high(directory),
           "binding queue high: an environment 0 never switches the ini's 1 off");
    SetEnvironmentVariableW(L"OFXR_REVER_BINDING_QUEUE_HIGH", nullptr);

    std::filesystem::remove_all(directory, code);
    if (failures != 0) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "rever tuning: all checks passed\n";
    return 0;
}
