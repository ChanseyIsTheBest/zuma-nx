// D3D call trace (PC test aid, 2026-10-26: mapping Renut's hooks to Zuma's XDK 2135 Direct3D, docs/zuma-notes.md):
// every function of the Direct3D library (0x82121000-0x8213B000) that code outside it calls (81, out/zuma_d3d_api.txt)
// is hooked, but Present sub_821221C0 (the frame hook has it). With zuma_d3d_trace = <game frame>, the log gets, per
// function: its calls up to that frame, whether r3 was the device (the word Direct3D_CreateDevice sub_82121838 fills
// through its r8: a field of the game's renderer, +52, not a global), its first arguments and caller; and the order of
// the calls during that frame. Off by default; the PC build only. (feedingfrenzy2-nx's trace with Zuma's addresses.)

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "zuma_capture.h"

REXCVAR_DEFINE_INT32(zuma_d3d_trace, 0, "Zuma",
                     "PC test aid: log the Direct3D API calls up to and during this game frame (0 = off)");

namespace {

constexpr uint32_t kCreateDevice = 0x82121838;
uint32_t g_device_slot = 0;  // Direct3D_CreateDevice's r8
struct Stat {
  uint32_t addr = 0;
  uint64_t calls = 0, device_calls = 0;
  uint32_t first[3][6] = {};
};
std::mutex g_mutex;
std::vector<Stat> g_stats;
std::vector<std::string> g_frame_calls;
bool g_dumped = false;

uint32_t Load32(const uint8_t* base, uint32_t a) {
  uint32_t be;
  std::memcpy(&be, base + a, 4);
  return __builtin_bswap32(be);
}

void Trace(uint32_t addr, PPCContext& ctx, uint8_t* base) {
  const int32_t at = REXCVAR_GET(zuma_d3d_trace);
  if (at <= 0 || g_dumped) return;
  const uint64_t frame = g_zuma_game_frames.load(std::memory_order_relaxed);
  std::lock_guard<std::mutex> lock(g_mutex);
  if (addr == kCreateDevice) g_device_slot = ctx.r8.u32;
  const uint32_t device = g_device_slot ? Load32(base, g_device_slot) : 0;
  Stat* s = nullptr;
  for (auto& x : g_stats) if (x.addr == addr) s = &x;
  if (!s) {
    g_stats.push_back({});
    s = &g_stats.back();
    s->addr = addr;
  }
  const bool is_dev = device && ctx.r3.u32 == device;
  if (s->calls < 3) {
    uint32_t* f = s->first[s->calls];
    f[0] = ctx.r3.u32; f[1] = ctx.r4.u32; f[2] = ctx.r5.u32; f[3] = ctx.r6.u32; f[4] = ctx.r7.u32;
    f[5] = uint32_t(ctx.lr);
  }
  // The shader containers (CreatePixelShader 82125AC8, CreateVertexShader 82125BB8) and the vertex elements
  // (CreateVertexDeclaration 82124AB8): their first 96 bytes, on the first three calls.
  if (s->calls < 3 && (addr == 0x82125AC8u || addr == 0x82125BB8u || addr == 0x82124AB8u) && ctx.r3.u32) {
    std::string hex;
    for (uint32_t o = 0; o < 96; o += 4) hex += fmt::format(" {:08X}", Load32(base, ctx.r3.u32 + o));
    REXLOG_INFO("[d3dtrace] {:08X} r3 {:08X}:{}", addr, ctx.r3.u32, hex);
  }
  // Every shader container created (virtual part + physical part) to out/ff_shaders/<ps|vs>_<n>.bin, for the
  // container-version study (XDK 2571 writes version 0x0E; Renut and XenosRecomp read 0x11).
  if ((addr == 0x82125AC8u || addr == 0x82125BB8u) && ctx.r3.u32) {
    const uint32_t c = ctx.r3.u32;
    const uint32_t size = Load32(base, c + 4) + Load32(base, c + 8);
    if (size > 0 && size < 0x40000) {
      static int n = 0;
      const std::string path = fmt::format("out/ff_shaders/{}_{:02}.bin", addr == 0x82125AC8u ? "ps" : "vs", n++);
      std::filesystem::create_directories("out/ff_shaders");
      if (FILE* f = std::fopen(path.c_str(), "wb")) {
        std::fwrite(base + c, 1, size, f);
        std::fclose(f);
      }
    }
  }
  ++s->calls;
  if (is_dev) ++s->device_calls;
  if (frame == uint64_t(at) && g_frame_calls.size() < 4000) {
    g_frame_calls.push_back(fmt::format("{:08X}{} r4={:X} r5={:X} r6={:X} lr={:08X}", addr, is_dev ? " dev" : "",
                                        ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, uint32_t(ctx.lr)));
  }
  if (frame > uint64_t(at)) {
    g_dumped = true;
    REXLOG_INFO("[d3dtrace] device 0x{:08X}; {} functions called up to frame {}", device, g_stats.size(), at);
    for (const auto& x : g_stats) {
      std::string first;
      for (uint64_t k = 0; k < x.calls && k < 3; ++k)
        first += fmt::format(" [{:X} {:X} {:X} {:X} {:X} lr {:08X}]", x.first[k][0], x.first[k][1], x.first[k][2],
                             x.first[k][3], x.first[k][4], x.first[k][5]);
      REXLOG_INFO("[d3dtrace] {:08X} calls {} (r3 = device {}):{}", x.addr, x.calls, x.device_calls, first);
    }
    REXLOG_INFO("[d3dtrace] frame {}: {} calls", at, g_frame_calls.size());
    for (const auto& c : g_frame_calls) REXLOG_INFO("[d3dtrace]   {}", c);
  }
}

}  // namespace

#define FF_D3D_TRACE(ADDR)                            extern "C" REX_FUNC(__imp__sub_##ADDR);            REX_HOOK_RAW(sub_##ADDR) {                            Trace(0x##ADDR##u, ctx, base);                     __imp__sub_##ADDR(ctx, base);                    }

FF_D3D_TRACE(82121170)
FF_D3D_TRACE(82121308)
FF_D3D_TRACE(82121390)
FF_D3D_TRACE(821213F8)
FF_D3D_TRACE(821217E0)
FF_D3D_TRACE(82121838)
FF_D3D_TRACE(82122300)
FF_D3D_TRACE(82122368)
FF_D3D_TRACE(82122880)
FF_D3D_TRACE(82122900)
FF_D3D_TRACE(82122968)
FF_D3D_TRACE(82122A18)
FF_D3D_TRACE(82122A88)
FF_D3D_TRACE(82123F08)
FF_D3D_TRACE(82124140)
FF_D3D_TRACE(82124288)
FF_D3D_TRACE(821243D0)
FF_D3D_TRACE(82124428)
FF_D3D_TRACE(82124480)
FF_D3D_TRACE(821244D0)
FF_D3D_TRACE(82124690)
FF_D3D_TRACE(821246E0)
FF_D3D_TRACE(82124A50)
FF_D3D_TRACE(82124AB8)
FF_D3D_TRACE(82124C68)
FF_D3D_TRACE(82124C70)
FF_D3D_TRACE(82124D70)
FF_D3D_TRACE(82125AC8)
FF_D3D_TRACE(82125BB8)
FF_D3D_TRACE(82125C90)
FF_D3D_TRACE(82125D10)
FF_D3D_TRACE(82125D18)
FF_D3D_TRACE(82125D20)
FF_D3D_TRACE(82125D48)
FF_D3D_TRACE(82125D50)
FF_D3D_TRACE(82125D58)
FF_D3D_TRACE(82126308)
FF_D3D_TRACE(821263E0)
FF_D3D_TRACE(82126450)
FF_D3D_TRACE(82126ED0)
FF_D3D_TRACE(82126ED8)
FF_D3D_TRACE(82127830)
FF_D3D_TRACE(82127838)
FF_D3D_TRACE(82127840)
FF_D3D_TRACE(82127848)
FF_D3D_TRACE(82127890)
FF_D3D_TRACE(82127AF8)
FF_D3D_TRACE(82127B00)
FF_D3D_TRACE(82127B08)
FF_D3D_TRACE(82127B10)
FF_D3D_TRACE(82127C90)
FF_D3D_TRACE(82127C98)
FF_D3D_TRACE(82127CA0)
FF_D3D_TRACE(821287F8)
FF_D3D_TRACE(821288A8)
FF_D3D_TRACE(82128940)
FF_D3D_TRACE(82128958)
FF_D3D_TRACE(821289E0)
FF_D3D_TRACE(82128A78)
FF_D3D_TRACE(82128B88)
FF_D3D_TRACE(821291D0)
FF_D3D_TRACE(8212A1C0)
FF_D3D_TRACE(8212A1D0)
FF_D3D_TRACE(8212A1D8)
FF_D3D_TRACE(8212A280)
FF_D3D_TRACE(8212A298)
FF_D3D_TRACE(8212A340)
FF_D3D_TRACE(8212A348)
FF_D3D_TRACE(8212A3F0)
FF_D3D_TRACE(8212A818)
FF_D3D_TRACE(8212A870)
FF_D3D_TRACE(8212A898)
FF_D3D_TRACE(8212A8E0)
FF_D3D_TRACE(8212A900)
FF_D3D_TRACE(8212AA88)
FF_D3D_TRACE(8212D4D0)
FF_D3D_TRACE(821339B8)
FF_D3D_TRACE(82136DB8)
FF_D3D_TRACE(8213AE00)
FF_D3D_TRACE(8213AEB0)
FF_D3D_TRACE(8213AF98)
