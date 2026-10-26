// h_820E4300.h: hand-written native of sub_820E4300, the 3x3 matrix product out(r3) = a(r4) * b(r5) (row-major floats,
// 36 bytes each): out[i][j] = fmadds(b[2][j], a[i][2], fmadds(b[0][j], a[i][0], fmuls(b[1][j], a[i][1]))), two loops
// (rows, then columns). Reference: app/generated/hot/n_820E4300.h (docs/hand-natives.md). Bejeweled 3's h_822DFF80.h
// is the same product compiled unrolled, with another order of operations; this one keeps Zuma's. The third function
// of the game's main thread in the emulator profile (out/renut_tests/emu/prof1: 51 samples, 2.5 %).
//
// Optimisation: when out's 36 bytes overlap neither input, the 18 inputs are read once, nine big-endian words of each
// matrix together, instead of 54 single loads re-read around every store (the original re-reads because out may be an
// input: an in-place product), the loop barriers go, and the nine results are stored together.
// Audit:
//   - each element is the copy's operations in the copy's order: REX_FMULS(b[1][j], a[i][1]), then
//     double(float(std::fma(b[0][j], a[i][0], that))), then double(float(std::fma(b[2][j], a[i][2], that))), stored as
//     float(that), on the doubles of the loaded floats;
//   - without an overlap no store can change an input, so every load of the copy reads what the first read returns, and
//     the stores' bytes and addresses are the copy's (the copy stores them in the same ascending order, one at a time:
//     nothing reads them in between);
//   - the context as the copy leaves it: the flush mode switched off first, f0 / f13 / f12 = a[2][0..2], f11 = b[1][2],
//     f10 = b[0][2], f8 = b[2][2], f9 / f7 / f6 the last element's three steps, r6 = r5 + 24, r7 = r8 = 0,
//     r9 = r4 + 44, r10 = r3 + 36, r11 = r5 + 36 (64-bit adds, as the copy's);
//   - a block at or above 0xDFFFFFDC (the physical window starts at 0xE0000000, 4 KB higher in host memory, or the
//     block would wrap past 4 GB) and any overlap of out with an input take the copy (n_820E4300::Native /
//     NativeLogged): an in-place product is the copy exactly.
#pragma once

#include <bit>

// n_820E4300.h (the copy) is included just before this file by app/generated/hot/localized_hooks.inc.

namespace zuma::hot::h_820E4300 {

inline bool Overlaps(uint32_t a, uint32_t b) { return a < b + 36u && b < a + 36u; }

template <bool kLog>
inline void Body(PPCContext& ctx, uint8_t* base) {
  const uint32_t o = ctx.r3.u32, pa = ctx.r4.u32, pb = ctx.r5.u32;
  if (Overlaps(o, pa) || Overlaps(o, pb) || o > 0xDFFFFFDBu || pa > 0xDFFFFFDBu ||
      pb > 0xDFFFFFDBu) [[unlikely]] {
    if constexpr (kLog) n_820E4300::NativeLogged(ctx, base); else n_820E4300::Native(ctx, base);
    return;
  }
  ctx.fpscr.disableFlushMode();
  uint32_t wa[9], wb[9];
  std::memcpy(wa, REX_RAW_ADDR(pa), 36);
  std::memcpy(wb, REX_RAW_ADDR(pb), 36);
  double a[9], b[9];
  for (int i = 0; i < 9; ++i) {
    a[i] = double(std::bit_cast<float>(__builtin_bswap32(wa[i])));
    b[i] = double(std::bit_cast<float>(__builtin_bswap32(wb[i])));
  }
  uint32_t wo[9];
  double f9 = 0, f7 = 0, f6 = 0;
  for (int i = 0; i < 3; ++i) {
    for (int j = 0; j < 3; ++j) {
      f9 = REX_FMULS(b[3 + j], a[3 * i + 1]);
      f7 = double(float(std::fma(b[j], a[3 * i], f9)));
      f6 = double(float(std::fma(b[6 + j], a[3 * i + 2], f7)));
      wo[3 * i + j] = __builtin_bswap32(std::bit_cast<uint32_t>(float(f6)));
    }
  }
  if constexpr (kLog) ::zuma::hot::LogBefore(base, o, 36);
  std::memcpy(REX_RAW_ADDR(o), wo, 36);
  ctx.f0.f64 = a[6];
  ctx.f13.f64 = a[7];
  ctx.f12.f64 = a[8];
  ctx.f11.f64 = b[5];
  ctx.f10.f64 = b[2];
  ctx.f8.f64 = b[8];
  ctx.f9.f64 = f9;
  ctx.f7.f64 = f7;
  ctx.f6.f64 = f6;
  ctx.r6.s64 = ctx.r5.s64 + 24;
  ctx.r7.s64 = 0;
  ctx.r8.s64 = 0;
  ctx.r9.s64 = ctx.r4.s64 + 44;
  ctx.r10.u64 = ctx.r3.u64 + 36;
  ctx.r11.s64 = ctx.r5.s64 + 36;
}

inline void Native(PPCContext& ctx, uint8_t* base) { Body<false>(ctx, base); }
inline void NativeLogged(PPCContext& ctx, uint8_t* base) { Body<true>(ctx, base); }

}  // namespace zuma::hot::h_820E4300
