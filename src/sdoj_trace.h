// sdoj_trace.h - recompile-level hooks for finding the DEATH trigger and the
// bomb/life counter in Arrange (CA022110) mode. Discovery build v4.
//
// Known facts this design is built on:
//  * 0x8887D900-0x8887DCFF is a generic timer-callback queue
//    (sub_880358C0 = per-frame tick, sub_88035978 = scheduler); the wider
//    0x8887D000-0x8887E000 block is ALL per-frame noise (v3: 37k WRT lines
//    in 8 seconds from ~30 hot addresses) - store-hooking it is hopeless.
//  * Death moment (S~1960-1990) flips these bytes 1->0 (old byte-diff run):
//    0x88619247, 0x88619C54, 0x88619C5B, 0x8861B117 (0x8861B123 dropped:
//    toggles every frame via sub_880AE788). These 4 stayed quiet the entire
//    v3 run. Their writers = death-path functions -> captured by the DIE hook.
//  * 0x88614947 = low byte of the object-destruction-queue count (NOT the hit
//    flag): sub_880E9C80 appends (count 1,2,3...), sub_880E9B68 flushes to 0.
//    Its dense clusters still mark hit events on the timeline (HIT hook).
//
// Hooks (all shadow-gated so per-frame noise can't flood):
//   DIE - stores covering the object region 0x88619200-0x8861B200, logged only
//         when one of the 4 known death-flip bytes actually changes value.
//   HIT - stores covering 0x88614944 word, logged only when the count's low
//         byte actually changes (hit/bullet-clear timeline markers).
//   CHG - per-frame byte-diff of the death block AND 0x88610000-0x88620000
//         (object region, bomb counter hunt), 15-frame cooldown per address.
//
// Fast path: each REX_STORE_* macro adds only a few integer compares per
// store (no function call on the common path). Keep it that way.
#pragma once

#include "sdoj_patch_flags.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <unordered_map>

namespace sdoj_trace {

// The 0x8887D000-0x8887E000 block (timer queue + per-frame worker state) was
// dropped from the store hook: v3 showed it is ALL per-frame noise
// (sub_88036C10 writes 0x8887DCA0 ~15x/frame; ArrangeGameWorker ~25x/frame;
// BuildInputMasks, sub_88080EB8 1x/frame) - 37k lines in 8s, pure flood.
// Object-destruction-queue count word (low byte 0x88614947 = HIT timeline).
inline constexpr std::uint32_t kHitLo = 0x88614940u;
inline constexpr std::uint32_t kHitHi = 0x88614950u;  // exclusive
inline constexpr std::uint32_t kHitByte = 0x88614947u;
// v5: decision-flag watch + wider object scan.
// The hit-decision state block base is 0x8860C828 (lis -30612 / addi -14296),
// consumed in sub_880BB920 (recomp.4.cpp):
//   [+1252] 0x8860CD0C = DEATH flag  (set -> caller clears & calls death)
//   [+1256] 0x8860CD10 = alt path    (set -> cleanup + sub_88093BC0)
//   [+1264] 0x8860CD14 = AUTO-BOMB flag (set -> cancel timers, effects,
//                        increments [+1268], sub_8808F728 cancel-all)
//   [+1272] 0x8860CD1C = alt path 2  (set -> sub_88070988 + effects)
// The bomb counter itself likely lives in this block, which sits BELOW the
// previously scanned 0x88610000 window (why it was never seen). 32-bit flags
// are watched via their big-endian LOW byte (addr+3).
inline constexpr std::uint32_t kDeathByteLo = 0x8860C000u;
inline constexpr std::uint32_t kDeathByteHi = 0x8861B200u;  // exclusive
inline constexpr std::uint32_t kDeathBytes[] = {
    0x88619247u, 0x88619C54u, 0x88619C5Bu, 0x8861B117u,  // death state bytes
    0x8860CD0Fu, 0x8860CD13u, 0x8860CD17u, 0x8860CD1Fu,  // decision flags (low)
};
inline constexpr std::size_t kDeathByteCount = 8;
// Byte-diff regions. Region 1 (the 0x8887Dxxx timer/noise block) was dropped
// in v5; region 3 covers the hit-decision state block at 0x8860C828.
inline constexpr std::uint32_t kScan2Lo = 0x88610000u;
inline constexpr std::uint32_t kScan2Hi = 0x88620000u;  // exclusive
inline constexpr std::size_t kScan2Size =
    static_cast<std::size_t>(kScan2Hi - kScan2Lo);  // 64 KiB
inline constexpr std::uint32_t kScan3Lo = 0x8860C000u;
inline constexpr std::uint32_t kScan3Hi = 0x88610000u;  // exclusive
inline constexpr std::size_t kScan3Size =
    static_cast<std::size_t>(kScan3Hi - kScan3Lo);  // 16 KiB
inline constexpr std::uint32_t kLineCap = 40000u;

inline std::FILE* death_log_ = nullptr;
inline std::uint32_t line_cap_count_ = 0;
inline std::uint32_t wrt_seq_ = 0;
inline std::unordered_map<std::uint32_t, std::uint32_t> scan_last_;
inline std::uint8_t scan_prev2_[kScan2Size];
inline std::uint8_t scan_prev3_[kScan3Size];
inline bool scan_primed_ = false;
inline std::uint32_t scan_sample_ = 0;
inline std::uint8_t hit_prev_ = 0xFF;
inline std::uint8_t death_prev_[kDeathByteCount] = {0xFF, 0xFF, 0xFF, 0xFF,
                                                    0xFF, 0xFF, 0xFF, 0xFF};

inline std::FILE* Log() {
  if (!death_log_)
    death_log_ = std::fopen("sdoj_death.log", "w");
  return death_log_;
}

// Destruction-queue count hook (hit timeline): called for every store
// overlapping the count word, but only logs when the low byte changed.
// All tracing is gated behind the sdoj_trace cvar (default off).
inline void CaptureHitWriter(std::uint8_t* base, const char* fn) {
  if (!sdoj_patch_flags::trace_enabled())
    return;
  const std::uint8_t cur = base[kHitByte];
  if (cur == hit_prev_)
    return;
  hit_prev_ = cur;
  if (!Log())
    return;
  if (line_cap_count_ < kLineCap) {
    std::fprintf(death_log_, "HIT %u %s byte=%u S%u\n", wrt_seq_++, fn,
                 static_cast<unsigned>(cur), scan_sample_);
    std::fflush(death_log_);
    ++line_cap_count_;
  }
}

// Death-flip byte hook: called for stores covering the object region; logs
// only when one of the 5 known death-time bytes actually changes. The writer
// of the 1->0 flip is a death-path function.
inline void CaptureDeathBytes(std::uint8_t* base, const char* fn) {
  if (!sdoj_patch_flags::trace_enabled())
    return;
  for (std::size_t i = 0; i < kDeathByteCount; ++i) {
    const std::uint8_t cur = base[kDeathBytes[i]];
    if (cur == death_prev_[i])
      continue;
    const std::uint8_t old = death_prev_[i];
    death_prev_[i] = cur;
    if (!Log())
      return;
    if (line_cap_count_ < kLineCap) {
      std::fprintf(death_log_, "DIE %u %s 0x%08X o%u n%u S%u\n", wrt_seq_++,
                   fn, kDeathBytes[i], static_cast<unsigned>(old),
                   static_cast<unsigned>(cur), scan_sample_);
      std::fflush(death_log_);
      ++line_cap_count_;
    }
  }
}

// Per-frame byte-diff. Region 2 = object region, region 3 = hit-decision
// state block (bomb counter hunt). 15-frame per-address cooldown suppresses
// per-frame animation so real counters stand out.
inline void ScanRegion(std::uint8_t* live, std::uint8_t* prev,
                       std::size_t size, std::uint32_t addr_lo) {
  for (std::size_t i = 0; i < size; ++i) {
    const std::uint8_t o = prev[i];
    const std::uint8_t n = live[i];
    prev[i] = n;
    if (o == n)
      continue;
    const std::uint32_t addr = addr_lo + static_cast<std::uint32_t>(i);
    auto it = scan_last_.find(addr);
    const std::uint32_t last = (it != scan_last_.end()) ? it->second : 0;
    scan_last_[addr] = scan_sample_;
    if (scan_sample_ - last <= 15)
      continue;  // suppress per-frame animation
    if (line_cap_count_ >= kLineCap)
      return;
    std::fprintf(death_log_, "CHG 0x%08X o%u n%u S%u\n", addr,
                 static_cast<unsigned>(o), static_cast<unsigned>(n),
                 scan_sample_);
    std::fflush(death_log_);
    ++line_cap_count_;
  }
}

inline void DecrementScan(std::uint8_t* base) {
  if (!sdoj_patch_flags::trace_enabled())
    return;
  if (!Log())
    return;
  if (!scan_primed_) {
    std::memcpy(scan_prev2_, base + kScan2Lo, kScan2Size);
    std::memcpy(scan_prev3_, base + kScan3Lo, kScan3Size);
    scan_primed_ = true;
    return;
  }
  ++scan_sample_;
  ScanRegion(base + kScan2Lo, scan_prev2_, kScan2Size, kScan2Lo);
  ScanRegion(base + kScan3Lo, scan_prev3_, kScan3Size, kScan3Lo);
}

}  // namespace sdoj_trace

#undef REX_STORE_U8
#undef REX_STORE_U16
#undef REX_STORE_U32
#undef REX_STORE_U64
#define REX_STORE_U8(x, y) do { \
  *(volatile std::uint8_t*)(base + (std::uint32_t)(x) + \
     (((std::uint32_t)(x) >= 0xE0000000u) ? 0x1000u : 0u)) = (std::uint8_t)(y); \
  { std::uint32_t sdoj_a = (std::uint32_t)(x); \
    if (sdoj_a + 1 >= sdoj_trace::kHitLo && sdoj_a < sdoj_trace::kHitHi) \
      sdoj_trace::CaptureHitWriter(base, __func__); \
    if (sdoj_a + 1 > sdoj_trace::kDeathByteLo && sdoj_a < sdoj_trace::kDeathByteHi) \
      sdoj_trace::CaptureDeathBytes(base, __func__); } \
} while (0)
#define REX_STORE_U16(x, y) do { \
  *(volatile std::uint16_t*)(base + (std::uint32_t)(x) + \
     (((std::uint32_t)(x) >= 0xE0000000u) ? 0x1000u : 0u)) = \
      __builtin_bswap16((std::uint16_t)(y)); \
  { std::uint32_t sdoj_a = (std::uint32_t)(x); \
    if (sdoj_a + 2 >= sdoj_trace::kHitLo && sdoj_a < sdoj_trace::kHitHi) \
      sdoj_trace::CaptureHitWriter(base, __func__); \
    if (sdoj_a + 2 > sdoj_trace::kDeathByteLo && sdoj_a < sdoj_trace::kDeathByteHi) \
      sdoj_trace::CaptureDeathBytes(base, __func__); } \
} while (0)
#define REX_STORE_U32(x, y) do { \
  *(volatile std::uint32_t*)(base + (std::uint32_t)(x) + \
     (((std::uint32_t)(x) >= 0xE0000000u) ? 0x1000u : 0u)) = \
      __builtin_bswap32((std::uint32_t)(y)); \
  { std::uint32_t sdoj_a = (std::uint32_t)(x); \
    if (sdoj_a + 4 >= sdoj_trace::kHitLo && sdoj_a < sdoj_trace::kHitHi) \
      sdoj_trace::CaptureHitWriter(base, __func__); \
    if (sdoj_a + 4 > sdoj_trace::kDeathByteLo && sdoj_a < sdoj_trace::kDeathByteHi) \
      sdoj_trace::CaptureDeathBytes(base, __func__); } \
} while (0)
#define REX_STORE_U64(x, y) do { \
  *(volatile std::uint64_t*)(base + (std::uint32_t)(x) + \
     (((std::uint32_t)(x) >= 0xE0000000u) ? 0x1000u : 0u)) = \
      __builtin_bswap64((std::uint64_t)(y)); \
  { std::uint32_t sdoj_a = (std::uint32_t)(x); \
    if (sdoj_a + 8 >= sdoj_trace::kHitLo && sdoj_a < sdoj_trace::kHitHi) \
      sdoj_trace::CaptureHitWriter(base, __func__); \
    if (sdoj_a + 8 > sdoj_trace::kDeathByteLo && sdoj_a < sdoj_trace::kDeathByteHi) \
      sdoj_trace::CaptureDeathBytes(base, __func__); } \
} while (0)
