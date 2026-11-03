// SPDX-License-Identifier: GPL-3.0-or-later
// PSSR 2 (libScePsml, sce::Psml::Mfsr2) bridge for the headless renderer.
// Same role as mihawkgen_bridge.h: a small global-state/API surface the Vulkan
// present path (vulkan_pssr.inc, see pssr_bridge.cpp) calls to upscale 1080p -> 2160p.
// Everything is compiled out unless ENABLE_PSSR is defined, so standard builds are untouched.
// API layout derived from D:\Fallo\PSSR2_Inyector\eldenring\pssr_init.py and
// D:\Fallo\Wolverine\pssr_estudio\NOTAS_PSSR.txt (FW 13.00, libScePsml build J03851485).
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace Eden::PssrBridge {

struct PsmlConfig {
    bool enabled = false;       // user setting / main.cpp flag
    uint16_t input_width = 1920;   // render resolution
    uint16_t input_height = 1080;
    uint16_t output_width = 3840;  // display resolution
    uint16_t output_height = 2160;
    bool pro_detected = false;  // sceKernelIsTrinityMode() == 1 (initProfile fails 0x8a8a0012 otherwise)
    // Physical base of the arena if blocks must be passed as direct-memory physical addresses
    // (the python PoC passes physical addresses for the shared/context blocks). 0 = use virtual.
    uint64_t arena_phys_base = 0;
};

// Where one dispatch writes its GPU commands (libScePsml: {uint32_t* data; uint32_t capacityDwords}).
struct PssrCmdStorage {
    uint32_t* data = nullptr;
    uint32_t capacity_dwords = 0;
    uint32_t pad = 0;
};

// Parameters of one upscale dispatch, the game-side DispatchParameters of libScePsml (0x70 bytes,
// read from its validator by disassembly; docs: tmp-psml-scripts/DISPATCH_AUDIT.md). The five texture
// fields point at 32-byte AGC texture descriptors (T#, 8 dwords), NOT at pixels.
struct PssrDispatchParams {
    const void* output_color = nullptr;       // +0x00 required: the 2160p target
    const void* color = nullptr;              // +0x08 required: the low-resolution colour
    const void* motion_vectors = nullptr;     // +0x10 required
    const void* depth = nullptr;              // +0x18 required (inferred)
    const void* exposure = nullptr;           // +0x20 optional
    const void* small_particle_mask = nullptr;// +0x28 optional
    float pre_exposure = 1.0f;                // +0x30 > 0, finite
    uint32_t render_x = 0, render_y = 0;      // +0x34 +0x38 origin of the render rect
    uint32_t render_w = 0, render_h = 0;      // +0x3c +0x40 size of the render rect
    float jitter_x = 0, jitter_y = 0;         // +0x44 +0x48
    float mv_scale_x = 1, mv_scale_y = 1;     // +0x4c +0x50
    float mv_offset_x = 0, mv_offset_y = 0;   // +0x54 +0x58
    float near_z = 0.1f, far_z = 1000.0f;     // +0x5c +0x60 0 < near < far
    uint8_t reset = 0;                        // +0x64 (history reset, assumed)
    uint8_t reserved1 = 0;                    // +0x65 must be 0
    uint8_t perceptual_encoding = 0;          // +0x66 < 4, 0 = linear
    uint8_t small_particle_mask_bits = 0;     // +0x67 0 without a mask
    uint32_t option_flags = 0;                // +0x68 < 0x10: 0x2 AutoExposure, 0x4 DilatedMV, 0x8 Sharpening
    float sharpness = 0;                      // +0x6c
};
static_assert(sizeof(PssrDispatchParams) == 0x70, "DispatchParameters is 0x70 bytes");

struct PssrDispatchDesc {
    PssrDispatchParams params;
    uint32_t* cmd_buffer = nullptr;   // where the commands go (capacity cmd_dwords)
    uint32_t cmd_dwords = 0;
};

// Test knob (file pssr-test.txt in the app folder, "cut=N"): how many dwords of the dispatch stream are
// injected. -1 = all (default), 0 = none (a control: barriers and state reset but no PSSR packets).
inline std::atomic<int> test_cut{-1};
// Test knob (same file, "flow=N"): the sign the optical-flow pass (vulkan_pssr_flow.comp) multiplies the
// motion vectors by. 0 (default) = it writes zeros, the same motion as before the pass existed; 1 or -1 =
// the vectors from the current picture to the previous one, in either sign convention.
inline std::atomic<int> flow_sign{0};

// Global state, set by main.cpp before the renderer starts (like MihawkGenBridge).
inline PsmlConfig config{};
inline std::atomic<bool> ready{false};
// Evidence counters for certification (logged by NoteFrame / DispatchPSSR).
inline std::atomic<uint64_t> frames_seen{0};
inline std::atomic<uint64_t> dispatch_ok{0};
inline std::atomic<uint64_t> dispatch_fail{0};
// Scaling detector: frames whose presented size is below the PSSR target (need upscale) vs at/above it.
inline std::atomic<uint64_t> frames_native{0};
inline std::atomic<uint64_t> frames_upscale{0};
inline std::atomic<uint32_t> last_in_w{0}, last_in_h{0};
// True when a frame of this size is not native output resolution, so PSSR should lift it.
// Compares against config.output_*; dynamic-resolution games are judged frame by frame.
inline bool NeedsUpscale(uint32_t w, uint32_t h) {
    return w < config.output_width || h < config.output_height;
}

enum PssrResult : int {
    kPssrOk = 0,
    kPssrDisabled = -1,        // built without ENABLE_PSSR or config.enabled == false
    kPssrNotPro = -2,
    kPssrModuleLoadFailed = -3,
    kPssrSymbolMissing = -4,
    kPssrArenaTooSmall = -5,
    kPssrNotInitialized = -6,
    kPssrBadArgs = -7,
};

// Optional: module base for the FW 13.00 fixed-offset fallback (when NID dlsym is unavailable).
void PssrSetModuleBase(uint64_t base);

// Loads libScePsml.sprx, calls initProfile, getSharedResourcesInitRequirement/createSharedResources,
// getContextInitRequirement/createContext. gpu_mem_ptr/mem_size: 2 MiB-aligned GPU-visible arena
// (needs ~134 MiB + 2 MiB tables for 3840x2160; 250+64 MiB with the old fakelib).
// Returns a PssrResult value, or the raw 0x8a8axxxx libScePsml error code.
int InitializePSSR(void* gpu_mem_ptr, size_t mem_size);

// Capacity (dwords) libScePsml announces for the command stream of one dispatch (2048 seen).
int PssrDispatchCommandSize(size_t* out_dwords);

// Writes the upscale commands for one frame into desc.cmd_buffer.
int DispatchPSSR(const PssrDispatchDesc& desc);

void ShutdownPSSR();

// Allocates the GPU arena (direct memory on PS5) and runs InitializePSSR, logging every step.
// Returns the same codes as InitializePSSR.
int InitializePSSRWithArena();

// Asks libSceSysmodule for libScePsml by id (0x800000d1, else 0x800000d2), once. Like the PS5 keyboard's
// module (system_keyboard.h) the console refuses it later in the app's life (0x80020063 once the app
// has filesystem access), so main() calls it first. Returns the first id's answer; 0 or more = loaded.
int PreparePssrModule();

// Command buffers for dispatch. libScePsml writes into them packets that carry the ABSOLUTE addresses of
// data inside the same buffer (the texture descriptors), so a buffer must be GPU-visible at the address
// the CPU writes it: these slots are inside the arena (mapped for CPU and GPU). A ring of kCommandSlots.
inline constexpr unsigned kCommandSlots = 16;
inline constexpr uint32_t kCommandSlotDwords = 2048;
// Next slot (round robin), or null before the arena exists. Valid until kCommandSlots more were taken.
uint32_t* AcquireCommandSlot();

// Called once per presented frame: counts it and logs the PSSR state periodically.
// width/height: the presented (already scaled) frame; guest_width/guest_height: the size the game
// itself rendered (Hud::GuestScreen, 0 if unknown). The scaling detector judges the game's own size.
void NoteFrame(uint32_t width, uint32_t height, uint32_t guest_width, uint32_t guest_height);

} // namespace Eden::PssrBridge
