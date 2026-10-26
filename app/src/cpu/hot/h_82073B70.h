// h_82073B70.h: hand-written native of sub_82073B70, MSVC's std::string::compare(pos r4, n r5, const char* s r6,
// count r7) on the string this r3 (size at +20, capacity at +24, the characters at +4 or, from a capacity of 16, at
// the pointer +4): the first min(min(size - pos, n), count) bytes compared one by one, then the lengths; out_of_range
// (sub_82111428) when pos > size. Reference: app/generated/hot/n_82073B70.h (docs/hand-natives.md,
// docs/optimising-functions.md). The hottest generated function of the emulator profile (out/renut_tests/emu/prof1:
// 205 of the generated code's 1,802 samples, 10 % of the game's main thread; 61 call sites).
//
// Optimisations:
//   - the byte loop compares eight bytes at a time on host memory (one host pointer per string, from the guest address
//     the copy starts at), and finds the first differing byte from the XOR of the two words; the copy loads every byte
//     through the guest memory macros with a loop barrier each iteration;
//   - a range that the guest-to-host mapping does not keep contiguous (one that reaches or crosses 0xE0000000, whose
//     pages are 4 KB higher in host memory, or wraps past 4 GB) and the throw (pos > size) run the copy from its start
//     instead: both are decided before the native's first store.
// Audit (the copy's effects in order and the context it leaves; cr0, cr6 and xer are its function-local flags, r27-r31
// its locals for non-volatile registers: the context keeps the caller's):
//   - lr: the copy sets ctx.lr = 0x82073B78 (its `bl __savegprlr_27`) and leaves it; r12 = the entry lr;
//   - the one store: the back chain, r1 at r1 - 128 (REX_CHAIN_STORE, as the copy does it); r1 = (r1 with its low word
//     less 128) + 128, as the copy computes it;
//   - n = size - pos (64-bit: the 32-bit load less r4) when it is below r5 (32-bit compare), else r5; m = n when it is
//     below r7 (32-bit), else r7, both 64-bit values as the copy moves them;
//   - p = (the pointer at +4 when the capacity, 32-bit, is 16 or more; else this + 4, a 64-bit add) + r4 (64-bit);
//     q = r6; r9 = 0;
//   - m (low word) 0: r7 as it came, r8 = m, r10 = q, r11 = p;
//   - else r8 = p + m and the loop: r9 = byte(p) - byte(q) (64-bit, wraps below 0), r7 = byte(q); a nonzero low word
//     ends it with p and q at that byte; else p + 1, q + 1 (64-bit) until p's low word is r8's (m bytes: the ranges
//     are below 4 GB here, so the low words meet after exactly m steps);
//   - r9's low word nonzero: r3 = r9. Else n < count (32-bit): r9 = -1; else r11 = (count - n has a zero low word),
//     r9 = r11 ^ 1. r3 = r9;
//   - the copy writes back r1, r3, r7-r12 (its limited write-back list); r4-r6 are as they came.
#pragma once

namespace zuma::hot::h_82073B70 {

// The host address of guest [address, address + size) when the mapping keeps it contiguous: below 4 GB and on one
// side of 0xE0000000 (REX_PHYS_HOST_OFFSET is 0 below it and 0x1000 from it on, or 0 everywhere).
inline const uint8_t* ContiguousHost(uint8_t* base, uint64_t address, uint64_t size) {
  const uint64_t end = address + size;
  if (end > 0x100000000ull) return nullptr;
  const bool low = address < 0xE0000000ull, high_end = end > 0xE0000000ull;
  if (low && high_end) return nullptr;
  return base + uint32_t(address) + REX_PHYS_HOST_OFFSET(uint32_t(address));
}

template <bool kLog>
inline void Body(PPCContext& ctx, uint8_t* base) {
  const uint32_t self = ctx.r3.u32;
  const uint64_t r4 = ctx.r4.u64, r5 = ctx.r5.u64, r6 = ctx.r6.u64, r7 = ctx.r7.u64;
  const uint64_t size = REX_LOAD_D_U32(self, 20);
  if (uint32_t(size) < uint32_t(r4)) {
    n_82073B70::Native(ctx, base);  // out_of_range: the copy throws as the original does
    return;
  }
  uint64_t n = size - r4;
  if (!(uint32_t(n) < uint32_t(r5))) n = r5;
  const uint64_t m = uint32_t(n) < uint32_t(r7) ? n : r7;
  const uint64_t capacity = REX_LOAD_D_U32(self, 24);
  const uint64_t data = uint32_t(capacity) < 16 ? uint64_t(ctx.r3.s64 + 4) : uint64_t(REX_LOAD_D_U32(self, 4));
  const uint64_t p = data + r4;
  const uint32_t steps = uint32_t(m);
  const uint8_t* hp = nullptr;
  const uint8_t* hq = nullptr;
  if (steps) {
    hp = ContiguousHost(base, uint32_t(p), steps);
    hq = ContiguousHost(base, uint32_t(r6), steps);
    if (!hp || !hq || (p >> 32) || (r6 >> 32)) {
      n_82073B70::Native(ctx, base);  // a range the mapping splits: the copy, byte by byte
      return;
    }
  }

  // The frame: lr, r12 and the back chain, as the copy's prologue leaves them.
  const uint64_t r12 = ctx.lr;
  ctx.lr = 0x82073B78;
  PPCRegister r1 = ctx.r1;
  const uint32_t ea = uint32_t(-128) + r1.u32;
  if constexpr (kLog) {
    REX_CHAIN_STORE(ZUMA_LOG_STORE_U32(ea, r1.u32));
  } else {
    REX_CHAIN_STORE(REX_STORE_U32(ea, r1.u32));
  }
  r1.u32 = ea;

  uint64_t r8 = m, r9 = 0, r10 = r6, r11 = p, out_r7 = r7;
  if (steps) {
    r8 = p + m;
    uint32_t i = 0;
    while (i + 8 <= steps) {
      uint64_t a, b;
      std::memcpy(&a, hp + i, 8);
      std::memcpy(&b, hq + i, 8);
      if (a != b) {
        i += uint32_t(__builtin_ctzll(a ^ b) >> 3);  // little-endian host: the lowest differing byte is the first
        break;
      }
      i += 8;
    }
    while (i < steps && hp[i] == hq[i]) ++i;
    if (i < steps) {
      r9 = uint64_t(hp[i]) - uint64_t(hq[i]);
      out_r7 = hq[i];
      r10 = r6 + i;
      r11 = p + i;
    } else {
      r9 = 0;
      out_r7 = hq[steps - 1];
      r10 = r6 + steps;
      r11 = p + steps;
    }
  }
  if (uint32_t(r9) == 0) {
    if (uint32_t(n) < uint32_t(r7)) {
      r9 = ~0ull;
    } else {
      r11 = uint32_t(r7 - n) == 0 ? 1 : 0;
      r9 = r11 ^ 1;
    }
  }
  r1.s64 = r1.s64 + 128;
  ctx.r1 = r1;
  ctx.r3.u64 = r9;
  ctx.r7.u64 = out_r7;
  ctx.r8.u64 = r8;
  ctx.r9.u64 = r9;
  ctx.r10.u64 = r10;
  ctx.r11.u64 = r11;
  ctx.r12.u64 = r12;
}

// Verify mode only (zuma_cpu_verify, off in play builds): the copy may call (the throw), so the hook is unguarded and
// the framework cannot check it. Without the throw its one store is the back-chain word, so here every call runs the
// native, puts that word back, runs the copy on a copy of the entry context and compares the registers the copy writes
// back, lr, and the stored word.
inline void Verified(PPCContext& ctx, uint8_t* base) {
  if (uint32_t(REX_LOAD_D_U32(ctx.r3.u32, 20)) < ctx.r4.u32) {
    n_82073B70::Native(ctx, base);  // the throw: the copy itself
    return;
  }
  PPCContext reference = ctx;
  const uint32_t slot = ctx.r1.u32 - 128u;
  uint32_t before, by_native, by_copy;
  std::memcpy(&before, REX_RAW_ADDR(slot), 4);
  Body<false>(ctx, base);
  std::memcpy(&by_native, REX_RAW_ADDR(slot), 4);
  std::memcpy(REX_RAW_ADDR(slot), &before, 4);
  n_82073B70::Native(reference, base);
  std::memcpy(&by_copy, REX_RAW_ADDR(slot), 4);
  ::zuma::enhancements::VerifyCheck();
  const bool same = ctx.r1.u64 == reference.r1.u64 && ctx.r3.u64 == reference.r3.u64 &&
                    ctx.r7.u64 == reference.r7.u64 && ctx.r8.u64 == reference.r8.u64 &&
                    ctx.r9.u64 == reference.r9.u64 && ctx.r10.u64 == reference.r10.u64 &&
                    ctx.r11.u64 == reference.r11.u64 && ctx.r12.u64 == reference.r12.u64 &&
                    ctx.lr == reference.lr && by_native == by_copy;
  if (!same) [[unlikely]] {
    ::zuma::enhancements::VerifyDifference();
    REXLOG_ERROR("[hot] DIFFERENCE: h_82073B70 (string compare) against its copy: r3 {:X}/{:X} r7 {:X}/{:X} "
                 "r8 {:X}/{:X} r9 {:X}/{:X} r10 {:X}/{:X} r11 {:X}/{:X} r12 {:X}/{:X} r1 {:X}/{:X} chain {:X}/{:X}",
                 ctx.r3.u64, reference.r3.u64, ctx.r7.u64, reference.r7.u64, ctx.r8.u64, reference.r8.u64,
                 ctx.r9.u64, reference.r9.u64, ctx.r10.u64, reference.r10.u64, ctx.r11.u64, reference.r11.u64,
                 ctx.r12.u64, reference.r12.u64, ctx.r1.u64, reference.r1.u64, by_native, by_copy);
    ctx = reference;
  }
}

inline void Native(PPCContext& ctx, uint8_t* base) {
  if (::zuma::enhancements::g_verify_mode) [[unlikely]] {
    Verified(ctx, base);
    return;
  }
  Body<false>(ctx, base);
}
inline void NativeLogged(PPCContext& ctx, uint8_t* base) { Body<true>(ctx, base); }

}  // namespace zuma::hot::h_82073B70
