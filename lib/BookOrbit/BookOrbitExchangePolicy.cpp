#include "BookOrbitExchangePolicy.h"

#include <cstring>

namespace bookorbit {

bool canSkipExchange(const char* storedSignature, const uint32_t exchangedAt, const std::string_view signature,
                     const uint32_t nowUnix) {
  if (storedSignature == nullptr || signature.empty() || exchangedAt == 0) return false;
  if (std::strlen(storedSignature) != signature.size()) return false;
  if (std::memcmp(storedSignature, signature.data(), signature.size()) != 0) return false;
  if (exchangedAt > nowUnix) return false;  // clock went backwards
  // Exclusive bound: a stamp exactly kExchangeMaxAgeSeconds old has expired.
  // (max - 1) may still skip; max must re-exchange.
  return (nowUnix - exchangedAt) < kExchangeMaxAgeSeconds;
}

bool rememberExchanged(char* storedSignature, const size_t capacity, uint32_t& exchangedAt,
                       const std::string_view signature, const uint32_t nowUnix) {
  if (storedSignature == nullptr || signature.empty()) return false;
  if (signature.size() + 1 > capacity) return false;
  std::memcpy(storedSignature, signature.data(), signature.size());
  storedSignature[signature.size()] = '\0';
  exchangedAt = nowUnix;
  return true;
}

}  // namespace bookorbit
