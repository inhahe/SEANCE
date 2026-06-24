#pragma once
#include <cstdint>
#include <cstddef>
#include <string>

namespace SoundShop {

// ============================================================================
// hash128 - 128-bit content hash (two decorrelated FNV-1a lanes + avalanche).
//
// Not cryptographic - this is for dedup / content-addressing only. A collision
// would alias two distinct payloads, so we use 128 bits: birthday-safe far past
// any realistic item count in a project. Deterministic across runs/platforms
// (fixed constants, byte-wise), which is exactly what content addressing needs.
//
// This was originally a private helper inside content_store.cpp. It is promoted
// here as the single source of truth so the asset library (and any future
// content-addressed store) hashes payloads identically - no second copy to drift.
// ============================================================================
std::string hash128(const uint8_t* d, size_t n);
std::string hash128(const std::string& s);

} // namespace SoundShop
