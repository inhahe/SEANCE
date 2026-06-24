#include "hash_util.h"
#include <cstdio>

namespace SoundShop {

namespace {
uint64_t avalanche(uint64_t z) {
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}
} // namespace

std::string hash128(const uint8_t* d, size_t n) {
    const uint64_t prime = 0x100000001b3ull;
    uint64_t h1 = 0xcbf29ce484222325ull;   // standard FNV-1a 64 basis
    uint64_t h2 = 0x84222325cbf29ce4ull;   // rotated basis for lane 2
    for (size_t i = 0; i < n; ++i) h1 = (h1 ^ d[i]) * prime;       // forward
    for (size_t i = n; i-- > 0;)   h2 = (h2 ^ d[i]) * prime;       // reversed
    h1 = avalanche(h1);
    h2 = avalanche(h2);
    char buf[33];
    std::snprintf(buf, sizeof(buf), "%016llx%016llx",
                  (unsigned long long) h1, (unsigned long long) h2);
    return std::string(buf, 32);
}

std::string hash128(const std::string& s) {
    return hash128(reinterpret_cast<const uint8_t*>(s.data()), s.size());
}

} // namespace SoundShop
