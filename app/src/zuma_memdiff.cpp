// Test aid (2026-10-27, touch controls: where Zuma keeps its aim): zuma_diag_memdiff = "f0,f1,f2" game frames. At each
// the game's committed guest memory (the 0x40000000 heap to 0x50000000, and .data 0x82390000-0x82492114) is copied;
// after f2 the log gets the words that changed between f1 and f2 but not between f0 and f1 (a test run moves the stick
// only between f1 and f2), as integers in 0..1024 or floats in -1024..1024, at most 400. Off by default; never in play
// builds' tomls.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

REXCVAR_DEFINE_STRING(zuma_diag_memdump, "", "Zuma", "Test aid: \"address,bytes\" (hex) dumped at zuma_diag_memdiff's f2");
REXCVAR_DEFINE_STRING(zuma_diag_memdiff, "", "Zuma",
                      "Test aid: \"f0,f1,f2\" game frames; logs the guest words that changed between f1 and f2 only");

namespace zuma::diag {

namespace {

struct Region {
  uint32_t base;
  std::vector<uint8_t> bytes;
};

std::vector<Region> Snapshot(const uint8_t* mem) {
  std::vector<Region> out;
  auto* kernel = rex::system::kernel_state();
  auto* memory = kernel ? kernel->memory() : nullptr;
  if (!memory) return out;
  uint32_t address = 0x40000000u;
  while (address < 0x50000000u) {
    auto* heap = memory->LookupHeap(address);
    rex::memory::HeapAllocationInfo info{};
    if (!heap || !heap->QueryRegionInfo(address, &info) || !info.region_size) break;
    const uint32_t end = info.base_address + info.region_size;
    if ((info.state & rex::memory::kMemoryAllocationCommit) && info.protect) {
      Region r;
      r.base = info.base_address;
      r.bytes.assign(mem + info.base_address, mem + std::min<uint64_t>(end, 0x50000000u));
      out.push_back(std::move(r));
    }
    if (end <= address) break;
    address = end;
  }
  Region data;
  data.base = 0x82390000u;
  data.bytes.assign(mem + 0x82390000u, mem + 0x82492114u);
  out.push_back(std::move(data));
  return out;
}

uint32_t Word(const Region& r, size_t o) {
  uint32_t be;
  std::memcpy(&be, r.bytes.data() + o, 4);
  return __builtin_bswap32(be);
}

}  // namespace

void MemDiffFrame(const uint8_t* mem, uint64_t frame) {
  static int64_t frames[3] = {-1, -1, -1};
  static bool parsed = false;
  static std::vector<Region> shots[3];
  if (!parsed) {
    parsed = true;
    const std::string s = REXCVAR_GET(zuma_diag_memdiff);
    long long a = -1, b = -1, c = -1;
    if (std::sscanf(s.c_str(), "%lld,%lld,%lld", &a, &b, &c) == 3) {
      frames[0] = a;
      frames[1] = b;
      frames[2] = c;
    }
  }
  for (int k = 0; k < 3; ++k) {
    if (int64_t(frame) != frames[k]) continue;
    shots[k] = Snapshot(mem);
    size_t total = 0;
    for (const auto& r : shots[k]) total += r.bytes.size();
    REXLOG_INFO("[memdiff] frame {}: {} regions, {} MB", frame, shots[k].size(), total >> 20);
    if (k != 2) return;
    int found = 0;
    for (const auto& r2 : shots[2]) {
      const Region* r0 = nullptr;
      const Region* r1 = nullptr;
      for (const auto& r : shots[0]) if (r.base == r2.base && r.bytes.size() == r2.bytes.size()) r0 = &r;
      for (const auto& r : shots[1]) if (r.base == r2.base && r.bytes.size() == r2.bytes.size()) r1 = &r;
      if (!r0 || !r1) continue;
      for (size_t o = 0; o + 4 <= r2.bytes.size() && found < 400; o += 4) {
        const uint32_t w0 = Word(*r0, o), w1 = Word(*r1, o), w2 = Word(r2, o);
        if (w0 != w1 || w1 == w2) continue;
        float f1, f2;
        std::memcpy(&f1, &w1, 4);
        std::memcpy(&f2, &w2, 4);
        const bool ints = w1 <= 1024 && w2 <= 1024;
        const bool floats = f1 > -1024.0f && f1 < 1024.0f && f2 > -1024.0f && f2 < 1024.0f && (w1 || w2) &&
                            (std::abs(f1) > 1e-6f || f1 == 0.0f) && (std::abs(f2) > 1e-6f || f2 == 0.0f);
        if (!ints && !floats) continue;
        ++found;
        REXLOG_INFO("[memdiff] {:08X}: {} -> {} ({:.3f} -> {:.3f})", r2.base + uint32_t(o), int32_t(w1), int32_t(w2),
                    f1, f2);
      }
    }
    REXLOG_INFO("[memdiff] {} words changed between frames {} and {} only", found, frames[1], frames[2]);
    // zuma_diag_memdump = "address,bytes": that range at f2, as integers and floats.
    unsigned dump_at = 0, dump_len = 0;
    if (std::sscanf(REXCVAR_GET(zuma_diag_memdump).c_str(), "%x,%x", &dump_at, &dump_len) == 2) {
      for (uint32_t o = 0; o < dump_len; o += 32) {
        std::string line;
        for (uint32_t k = 0; k < 32 && o + k < dump_len; k += 4) {
          uint32_t be, w;
          std::memcpy(&be, mem + dump_at + o + k, 4);
          w = __builtin_bswap32(be);
          float f;
          std::memcpy(&f, &w, 4);
          line += fmt::format(" {:X}:{}/{:.2f}", o + k, int32_t(w), f);
        }
        REXLOG_INFO("[memdump] {:08X}:{}", dump_at + o, line);
      }
    }
    for (auto& s : shots) s.clear();
  }
}

}  // namespace zuma::diag
