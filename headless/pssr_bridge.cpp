// SPDX-License-Identifier: GPL-3.0-or-later
// PSSR 2 bridge implementation. See pssr_bridge.h.
#include "pssr_bridge.h"

#ifndef ENABLE_PSSR
namespace Eden::PssrBridge {
void PssrSetModuleBase(uint64_t) {}
int InitializePSSR(void*, size_t) { return kPssrDisabled; }
int PssrDispatchCommandSize(size_t*) { return kPssrDisabled; }
int DispatchPSSR(const PssrDispatchDesc&) { return kPssrDisabled; }
void ShutdownPSSR() {}
int InitializePSSRWithArena() { return kPssrDisabled; }
void NoteFrame(uint32_t, uint32_t, uint32_t, uint32_t) {}
int PreparePssrModule() { return kPssrDisabled; }
uint32_t* AcquireCommandSlot() { return nullptr; }
} // namespace Eden::PssrBridge
#else

#include <cstdio>
#include <cstring>
#include <string>

#include "diagnostics.h"
#include "hud.h"
#include "storage_paths.h"
#include <cstdlib>
#include <fstream>

#if defined(__PROSPERO__)
extern "C" {
int sceKernelLoadStartModule(const char* path, size_t args, const void* argp, unsigned flags,
                             const void* opt, int* res);
int sceKernelDlsym(int handle, const char* symbol, void** addr);
int sceSysmoduleLoadModuleInternal(uint32_t id);
int sceKernelAllocateDirectMemory(long searchStart, long searchEnd, size_t length, size_t alignment,
                                  int type, long* physAddrOut);
int sceKernelMapDirectMemory(void** addr, size_t length, int prot, int flags, long directMemoryStart,
                             size_t alignment);
// libScePsml, library libScePsml_mfsr2, imported at link time (tools/stubs/libScePsml.c): the console's
// loader refuses a run-time load of the module and sceKernelDlsym finds nothing. The export names are
// unknown, so each symbol is __nid_<NID> with '+' as "_p" and '-' as "_m".
uint32_t __nid_o_pNM86gEwFE();
uint32_t __nid_1ic5q_mkdOsc();
uint32_t __nid_gMduXCLYrNg();
uint32_t __nid_0GAw7SmkwII();
uint32_t __nid_5z2gBlqxJ_p0();
uint32_t __nid_m9JLPc3wOQw();
uint32_t __nid_Kyy1baXgaVU();
uint32_t __nid_ZLL31lzzxr4();
uint32_t __nid_lrJwpLjKXRc();
}
#endif

namespace Eden::PssrBridge {
namespace {

// NIDs of sce::Psml::Mfsr2 exports (libScePsml_mfsr2) and FW 13.00 offsets in libScePsml.sprx.
struct Sym { const char* nid; uint64_t off; };
constexpr Sym kInit        {"o+NM86gEwFE", 0x99d0};
constexpr Sym kSharedReq   {"1ic5q-kdOsc", 0x9a60};
constexpr Sym kSharedMake  {"gMduXCLYrNg", 0x9ac0};
constexpr Sym kSharedFree  {"0GAw7SmkwII", 0x9b50};
constexpr Sym kCtxReq      {"5z2gBlqxJ+0", 0x9b80};
constexpr Sym kCtxMake     {"m9JLPc3wOQw", 0x9bb0};
constexpr Sym kCtxFree     {"Kyy1baXgaVU", 0x9c00};
constexpr Sym kDispatchSz  {"ZLL31lzzxr4", 0x9c30};
constexpr Sym kDispatch    {"lrJwpLjKXRc", 0x9c50};

constexpr uint32_t kAlreadyInit = 0x8a8a0002;
constexpr size_t kBlock = 0x200000;

// Argument blocks as written by pssr_init.py.
struct SharedCfg {            // 0x80 bytes
    uint8_t  pad0[8];
    uint64_t blocks;          // +0x08 -> array of {addr,size}
    uint64_t block_count;     // +0x10
    uint16_t max_w, max_h;    // +0x18
    uint8_t  pad1[0x80 - 0x1c];
};
struct Requirement { uint64_t block_size, alignment, block_count; uint8_t pad[0x40 - 24]; };
struct ContextParams {        // 0x40 bytes
    uint32_t reserved0, reserved1;   // must be 0
    uint64_t shared;                 // +0x08 sharedResources handle
    uint64_t blocks;                 // +0x10
    uint64_t block_count;            // +0x18
    uint8_t  pad[8];
    uint16_t width, height;          // +0x28
    uint32_t one;                    // +0x2c, must be 1 (>=2 -> 0x8a8a000d)
    uint8_t  pad2[0x40 - 0x30];
};
struct Block { uint64_t addr, size; };
static_assert(sizeof(SharedCfg) == 0x80 && sizeof(Requirement) == 0x40 && sizeof(ContextParams) == 0x40);
static_assert(offsetof(SharedCfg, max_w) == 0x18 && offsetof(ContextParams, width) == 0x28);

using FnInit     = uint32_t (*)();
using FnReq      = uint32_t (*)(Requirement* out, const void* params);
using FnMake     = uint32_t (*)(uint64_t* out_handle, const void* params);
using FnFree     = uint32_t (*)(uint64_t handle);
using FnDispSize = uint32_t (*)(uint64_t ignored, uint32_t* out_dwords);
// dispatch(ctx, &{data, capacityDwords}, params) as read from the disassembly at libScePsml+0x9c50.
using FnDispatch = uint32_t (*)(uint64_t ctx, const PssrCmdStorage* storage, const PssrDispatchParams* params);

struct State {
    uint64_t base = 0;
    int module = -1;
    FnInit init{}; FnReq shared_req{}, ctx_req{}; FnMake shared_make{}, ctx_make{};
    FnFree shared_free{}, ctx_free{};
    FnDispSize dispatch_size{}; FnDispatch dispatch{};
    uint64_t shared = 0, ctx = 0;
};
State g;
int g_prep_id = -1, g_prep_rc[2] = {1, 1};   // 1 = not asked
uint64_t g_base_override = 0;
uint64_t g_arena_virt = 0;
uint32_t* g_cmd_slots = nullptr;
std::atomic<unsigned> g_cmd_next{0};   // CPU address of the arena start; blocks carry a direct-memory OFFSET

void* Resolve(const Sym& s) {
#if defined(__PROSPERO__)
    void* fn = nullptr;
    // The address is taken in code on purpose: a static table of these addresses becomes
    // R_X86_64_64 data relocations, which the console's loader leaves at 0 for an import (only
    // R_X86_64_GLOB_DAT, what code produces, is resolved). Seen: all nine were 0 with a table.
    if (&s == &kInit) fn = reinterpret_cast<void*>(&__nid_o_pNM86gEwFE);
    if (&s == &kSharedReq) fn = reinterpret_cast<void*>(&__nid_1ic5q_mkdOsc);
    if (&s == &kSharedMake) fn = reinterpret_cast<void*>(&__nid_gMduXCLYrNg);
    if (&s == &kSharedFree) fn = reinterpret_cast<void*>(&__nid_0GAw7SmkwII);
    if (&s == &kCtxReq) fn = reinterpret_cast<void*>(&__nid_5z2gBlqxJ_p0);
    if (&s == &kCtxMake) fn = reinterpret_cast<void*>(&__nid_m9JLPc3wOQw);
    if (&s == &kCtxFree) fn = reinterpret_cast<void*>(&__nid_Kyy1baXgaVU);
    if (&s == &kDispatchSz) fn = reinterpret_cast<void*>(&__nid_ZLL31lzzxr4);
    if (&s == &kDispatch) fn = reinterpret_cast<void*>(&__nid_lrJwpLjKXRc);
    if (!fn && g.module >= 0) {
        // The import is 0 when the loader did not load libScePsml for this title: ask for the symbol
        // of the module loaded by id (PreparePssrModule), keyed NID#libraryId#moduleId.
        char name[40];
        std::snprintf(name, sizeof(name), "%s#K#A", s.nid);
        void* p = nullptr;
        if (sceKernelDlsym(g.module, name, &p) == 0 && p) fn = p;
    }
    if (fn) return fn;
#endif
    if (g.base) return reinterpret_cast<void*>(g.base + s.off);   // FW 13.00 fixed offsets
    return nullptr;
}

bool ResolveAll() {
    g.init = reinterpret_cast<FnInit>(Resolve(kInit));
    g.shared_req = reinterpret_cast<FnReq>(Resolve(kSharedReq));
    g.shared_make = reinterpret_cast<FnMake>(Resolve(kSharedMake));
    g.shared_free = reinterpret_cast<FnFree>(Resolve(kSharedFree));
    g.ctx_req = reinterpret_cast<FnReq>(Resolve(kCtxReq));
    g.ctx_make = reinterpret_cast<FnMake>(Resolve(kCtxMake));
    g.ctx_free = reinterpret_cast<FnFree>(Resolve(kCtxFree));
    g.dispatch_size = reinterpret_cast<FnDispSize>(Resolve(kDispatchSz));
    g.dispatch = reinterpret_cast<FnDispatch>(Resolve(kDispatch));
    {
        // One line with every address (0 = the loader left that import unresolved).
        char b[320];
        std::snprintf(b, sizeof(b),
                      "resolve init=%p sharedReq=%p sharedMake=%p sharedFree=%p ctxReq=%p ctxMake=%p ctxFree=%p dispSize=%p dispatch=%p base=%p",
                      reinterpret_cast<void*>(g.init), reinterpret_cast<void*>(g.shared_req),
                      reinterpret_cast<void*>(g.shared_make), reinterpret_cast<void*>(g.shared_free),
                      reinterpret_cast<void*>(g.ctx_req), reinterpret_cast<void*>(g.ctx_make),
                      reinterpret_cast<void*>(g.ctx_free), reinterpret_cast<void*>(g.dispatch_size),
                      reinterpret_cast<void*>(g.dispatch), reinterpret_cast<void*>(g.base));
        Report("pssr", b);
    }
    return g.init && g.shared_req && g.shared_make && g.ctx_req && g.ctx_make && g.dispatch;
}

// Carves n blocks of `size` out of the arena; fills `table` with {addr,size}.
bool Carve(uint8_t*& cur, uint8_t* end, Block* table, uint64_t n, uint64_t size) {
    if (n * size > static_cast<uint64_t>(end - cur)) return false;
    const uint64_t phys = config.arena_phys_base;
    for (uint64_t i = 0; i < n; ++i, cur += size)
        table[i] = {phys ? phys + (reinterpret_cast<uint64_t>(cur) - g_arena_virt) : reinterpret_cast<uint64_t>(cur), size};
    return true;
}

std::string Hex(uint32_t v) {
    char b[16];
    std::snprintf(b, sizeof(b), "0x%08x", v);
    return b;
}
int Fail(const char* step, uint32_t rc, int result = 0) {
    Report("pssr", (std::string(step) + " failed rc=" + Hex(rc)).c_str());
    return result ? result : static_cast<int>(rc);
}

} // namespace

void PssrSetModuleBase(uint64_t base) { g_base_override = base; }

namespace { void SelfTest(); }

int PreparePssrModule() {
#if defined(__PROSPERO__)
    static const uint32_t kIds[] = {0x800000d1, 0x800000d2};
    for (int i = 0; i < 2; ++i) {
        g_prep_rc[i] = sceSysmoduleLoadModuleInternal(kIds[i]);
        if (g_prep_rc[i] >= 0) { g_prep_id = static_cast<int>(kIds[i]); break; }
    }
    return g_prep_rc[0];
#else
    return kPssrDisabled;
#endif
}

int InitializePSSR(void* gpu_mem_ptr, size_t mem_size) {
    if (!config.enabled) { Report("pssr", "disabled by configuration"); return kPssrDisabled; }
    if (!gpu_mem_ptr || mem_size < 2 * kBlock) { Report("pssr", "bad arena arguments"); return kPssrBadArgs; }
    if (ready.load()) return kPssrOk;
    g_arena_virt = reinterpret_cast<uint64_t>(gpu_mem_ptr);
    {
        std::ifstream knob(Eden::AppFile("pssr-test.txt"));
        for (std::string entry; knob >> entry;) {
            if (entry.rfind("cut=", 0) == 0) test_cut.store(std::atoi(entry.c_str() + 4));
            if (entry.rfind("flow=", 0) == 0) {
                const int sign = std::atoi(entry.c_str() + 5);
                flow_sign.store(sign < 0 ? -1 : sign > 0 ? 1 : 0);   // only -1, 0 or 1
            }
        }
        if (test_cut.load() != -1) Report("pssr", ("TEST KNOB cut=" + std::to_string(test_cut.load())).c_str());
        if (flow_sign.load() != 0) Report("pssr", ("TEST KNOB flow=" + std::to_string(flow_sign.load())).c_str());
    }
#if defined(__PROSPERO__)
    // libScePsml is already loaded by the console's loader as an import of this title; the first
    // call below is its first use. A console that is not a PS5 Pro answers through the return codes.
    Report("pssr", "libScePsml imported at link time (libScePsml_mfsr2)");
    {
        // Diagnostics. Control: the address of an import that is known to work (0 would mean that an
        // address taken from an import is 0 here whatever the module).
        char cb[96];
        std::snprintf(cb, sizeof(cb), "control import sceKernelMapDirectMemory=%p",
                      reinterpret_cast<void*>(&sceKernelMapDirectMemory));
        Report("pssr", cb);
        // Bare names: the loader finds a module by bare name when the title imports it. The code says
        // which module (or dependency of it) the loader cannot load.
        static const char* const kBare[] = {"libScePsml.sprx", "libScePsml.prx", "libSceDeci5.sprx",
                                            "libSceDeci5.prx", "libSceRegMgr.sprx", "libSceSysmodule.sprx",
                                            "libSceAgcDriver.sprx"};
        Report("pssr", ("sysmodule load by id: 0x800000d1 -> " + Hex(static_cast<uint32_t>(g_prep_rc[0])) +
                        ", 0x800000d2 -> " + Hex(static_cast<uint32_t>(g_prep_rc[1])) + " (0x1 = not asked)").c_str());
        int psml = -1;
        for (const char* name : kBare) {
            int r = 0;
            const int h = sceKernelLoadStartModule(name, 0, nullptr, 0, nullptr, &r);
            Report("pssr", (std::string("bare load ") + name + " -> " + (h < 0 ? "failed " + Hex(static_cast<uint32_t>(h)) : "handle " + std::to_string(h))).c_str());
            if (h >= 0 && std::strncmp(name, "libScePsml", 10) == 0 && psml < 0) psml = h;
        }
        if (psml >= 0) g.module = psml;
        if (psml >= 0) {
            // sceKernelDlsym keys symbols as NID#libraryId#moduleId (K = libScePsml_mfsr2, A = libScePsml).
            static const char* const kSyms[] = {"o+NM86gEwFE#K#A", "o+NM86gEwFE", "lrJwpLjKXRc#K#A"};
            for (const char* sym : kSyms) {
                void* f = nullptr;
                const int r = sceKernelDlsym(psml, sym, &f);
                char b[160];
                std::snprintf(b, sizeof(b), "dlsym(handle %d, %s) -> rc %s addr %p", psml, sym,
                              Hex(static_cast<uint32_t>(r)).c_str(), f);
                Report("pssr", b);
            }
        }
    }
#endif
    g.base = g_base_override;
    if (!ResolveAll()) { Report("pssr", "Mfsr2 symbols not resolved"); return kPssrSymbolMissing; }
    Report("pssr", "Mfsr2 symbols resolved");

    uint32_t rc = g.init();
    Report("pssr", ("initProfile rc=" + Hex(rc)).c_str());
    if (rc != 0 && rc != kAlreadyInit) return Fail("initProfile", rc);

    // Arena layout: [2 MiB tables][shared blocks][context blocks]. Tables hold the {addr,size} arrays.
    auto* cur = static_cast<uint8_t*>(gpu_mem_ptr);
    auto* end = cur + mem_size;
    auto* tables = cur;
    cur += kBlock;
    std::memset(tables, 0, kBlock);
    // Command slots: 16 x 8 KiB at +0x10000 of the (zeroed) tables block; the tables use only its first 8 KiB.
    g_cmd_slots = reinterpret_cast<uint32_t*>(tables + 0x10000);

    SharedCfg cfg{};
    cfg.max_w = config.output_width; cfg.max_h = config.output_height;
    Requirement req{};
    rc = g.shared_req(&req, &cfg);
    if (rc) return Fail("getSharedResourcesInitRequirement", rc);
    {
        char b[200];
        std::snprintf(b, sizeof(b), "shared requirement: block_size=%llu align=%llu count=%llu; arena virt=%p phys=%llu",
                      static_cast<unsigned long long>(req.block_size), static_cast<unsigned long long>(req.alignment),
                      static_cast<unsigned long long>(req.block_count), gpu_mem_ptr,
                      static_cast<unsigned long long>(config.arena_phys_base));
        Report("pssr", b);
    }
    auto* shared_tbl = reinterpret_cast<Block*>(tables);
    if (req.block_count > 0x1000 / sizeof(Block) || !Carve(cur, end, shared_tbl, req.block_count, req.block_size))
        return Fail("shared arena", 0xFFFFFFFFu, kPssrArenaTooSmall);
    cfg.blocks = reinterpret_cast<uint64_t>(shared_tbl); cfg.block_count = req.block_count;
    {
        char b[200];
        const Block* t = reinterpret_cast<const Block*>(cfg.blocks);
        std::snprintf(b, sizeof(b), "shared table: count=%llu first={addr=%llu size=%llu} cfg max=%ux%u",
                      static_cast<unsigned long long>(cfg.block_count),
                      static_cast<unsigned long long>(cfg.block_count ? t[0].addr : 0),
                      static_cast<unsigned long long>(cfg.block_count ? t[0].size : 0), cfg.max_w, cfg.max_h);
        Report("pssr", b);
    }
    rc = g.shared_make(&g.shared, &cfg);
    if (rc) return Fail("createSharedResources", rc);

    ContextParams cp{};
    cp.shared = g.shared; cp.width = config.output_width; cp.height = config.output_height; cp.one = 1;
    req = Requirement{};
    rc = g.ctx_req(&req, &cp);
    if (rc) return Fail("getContextInitRequirement", rc);
    {
        char b[160];
        std::snprintf(b, sizeof(b), "context requirement: block_size=%llu align=%llu count=%llu",
                      static_cast<unsigned long long>(req.block_size), static_cast<unsigned long long>(req.alignment),
                      static_cast<unsigned long long>(req.block_count));
        Report("pssr", b);
    }
    auto* ctx_tbl = reinterpret_cast<Block*>(tables + 0x1000);
    if (req.block_count > 0x1000 / sizeof(Block) || !Carve(cur, end, ctx_tbl, req.block_count, req.block_size))
        return Fail("context arena", 0xFFFFFFFFu, kPssrArenaTooSmall);
    cp.blocks = reinterpret_cast<uint64_t>(ctx_tbl); cp.block_count = req.block_count;
    rc = g.ctx_make(&g.ctx, &cp);
    if (rc) return Fail("createContext", rc);

    ready.store(true);
    SelfTest();
    Report("pssr", "PSSR 2 context ready (init + shared + context created by libScePsml)");
    return kPssrOk;
}

uint32_t* AcquireCommandSlot() {
    if (!g_cmd_slots) return nullptr;
    return g_cmd_slots + static_cast<size_t>(g_cmd_next.fetch_add(1) % kCommandSlots) * kCommandSlotDwords;
}

int PssrDispatchCommandSize(size_t* out_dwords) {
    if (!ready.load() || !g.dispatch_size) return kPssrNotInitialized;
    if (!out_dwords) return kPssrBadArgs;
    uint32_t dwords = 0;
    const uint32_t rc = g.dispatch_size(g.ctx, &dwords);
    if (rc != 0) return static_cast<int>(rc);
    *out_dwords = dwords;
    return kPssrOk;
}

int DispatchPSSR(const PssrDispatchDesc& d) {
    if (!ready.load()) return kPssrNotInitialized;
    if (!d.cmd_buffer || d.cmd_dwords == 0) return kPssrBadArgs;
    const PssrCmdStorage storage{d.cmd_buffer, d.cmd_dwords, 0};
    const int rc = static_cast<int>(g.dispatch(g.ctx, &storage, &d.params));
    (rc == 0 ? dispatch_ok : dispatch_fail).fetch_add(1);
    return rc;
}

namespace {
// Self-test without the renderer: a dispatch with valid numbers and NULL textures. If the ABI and the
// offsets above are right, libScePsml answers 0x8a8a000a (a required texture is null); another code
// says which field or argument is wrong (0x8a8a000d: a number; 0x8a8a0009: storage/params; ...).
void SelfTest() {
    uint32_t announced = 0;
    const uint32_t src = g.dispatch_size(g.ctx, &announced);
    Report("pssr", ("selftest command size rc=" + Hex(src) + " dwords=" + std::to_string(announced)).c_str());
    static uint32_t cmd[4096];
    PssrDispatchDesc d;
    d.cmd_buffer = cmd;
    d.cmd_dwords = announced && announced <= 4096 ? announced : 2048;
    d.params.pre_exposure = 1.0f;
    d.params.render_w = config.input_width;
    d.params.render_h = config.input_height;
    const PssrCmdStorage storage{d.cmd_buffer, d.cmd_dwords, 0};
    const uint32_t rc = g.dispatch(g.ctx, &storage, &d.params);
    Report("pssr", ("selftest dispatch with null textures rc=" + Hex(rc) +
                    " (0x8a8a000a expected when the layout is right)").c_str());
}
} // namespace

int InitializePSSRWithArena() {
    constexpr size_t kArena = 160ull << 20;  // ~134 MiB needed at 3840x2160 + 2 MiB tables
#if defined(__PROSPERO__)
    long phys = 0;
    int rc = sceKernelAllocateDirectMemory(0, 0x600000000L, kArena, 0x200000, 3, &phys);
    if (rc != 0) { Report("pssr", ("direct memory alloc failed " + Hex(rc)).c_str()); return kPssrArenaTooSmall; }
    void* addr = nullptr;
    rc = sceKernelMapDirectMemory(&addr, kArena, 0x33, 0, phys, 0x200000);
    if (rc != 0 || !addr) { Report("pssr", ("direct memory map failed " + Hex(rc)).c_str()); return kPssrArenaTooSmall; }
    Report("pssr", ("arena mapped, 160 MiB, phys " + std::to_string(phys)).c_str());
    config.arena_phys_base = static_cast<uint64_t>(phys);   // libScePsml maps each block by this offset
    return InitializePSSR(addr, kArena);
#else
    return kPssrDisabled;
#endif
}

void NoteFrame(uint32_t present_w, uint32_t present_h, uint32_t guest_w, uint32_t guest_h) {
    const uint64_t n = frames_seen.fetch_add(1) + 1;
    // The presented frame is already FSR-scaled to the output size, so judge the game's own picture.
    const bool have_guest = guest_w != 0 && guest_h != 0;
    const uint32_t width = have_guest ? guest_w : present_w;
    const uint32_t height = have_guest ? guest_h : present_h;
    const bool up = NeedsUpscale(width, height);
    (up ? frames_upscale : frames_native).fetch_add(1);
    // A change of frame size (or of native/non-native) is logged at once, not only every 600 frames.
    const bool changed = width != last_in_w.exchange(width) || height != last_in_h.exchange(height);
    if (changed && n != 1)
        Report("pssr", ("SCALE DETECT: frame size now " + std::to_string(width) + "x" + std::to_string(height) +
                        (up ? " -> NOT native 4K, PSSR upscale needed" : " -> native 4K, PSSR not needed")).c_str());
    if (n != 1 && n % 600 != 0) return;
    const uint64_t ok = dispatch_ok.load(), fail = dispatch_fail.load();
    const char* verdict;
    if (!config.enabled) verdict = "PSSR RESCALING: NO (disabled by pssr_off.txt)";
    else if (!ready.load()) verdict = "PSSR RESCALING: NO (PSSR 2 context not created, see earlier pssr lines)";
    else if (ok == 0 && fail == 0) verdict = "PSSR RESCALING: NO (context ready, but no upscale dispatch is wired to the Vulkan path)";
    else if (ok == 0) verdict = "PSSR RESCALING: NO (dispatch attempted and failed)";
    else verdict = "PSSR RESCALING: YES (dispatch returned success)";
    Report("pssr", (std::string(verdict) + " | frame " + std::to_string(n) + " game " +
                    std::to_string(width) + "x" + std::to_string(height) + (have_guest ? "" : "(presented, no guest size)") +
                    " presented " + std::to_string(present_w) + "x" + std::to_string(present_h) + ", PSSR target " +
                    std::to_string(config.output_width) + "x" + std::to_string(config.output_height) +
                    ", scale=" + (up ? "UPSCALE-NEEDED" : "NATIVE") +
                    " (upscale_frames=" + std::to_string(frames_upscale.load()) +
                    " native_frames=" + std::to_string(frames_native.load()) + ")" +
                    ", dispatch_ok=" + std::to_string(ok) + " dispatch_fail=" + std::to_string(fail) +
                    " | OVERLAY SAYS: " + std::string(Hud::UpscalerLabel(Hud::upscaler.load())) +
                    " GPU_us=" + std::to_string(Hud::pssr_gpu_us.load()) +
                    " R_milli=" + std::to_string(Hud::pssr_diff_milli.load()))
                       .c_str());
}

void ShutdownPSSR() {
    if (!ready.exchange(false)) return;
    if (g.ctx_free && g.ctx) g.ctx_free(g.ctx);
    if (g.shared_free && g.shared) g.shared_free(g.shared);
    g.ctx = g.shared = 0;
}

} // namespace Eden::PssrBridge
#endif // ENABLE_PSSR
