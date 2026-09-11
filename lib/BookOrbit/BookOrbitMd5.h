#pragma once

#include <string>
#include <string_view>

namespace bookorbit {

// RFC 1321 MD5, returned as 32 lowercase hex characters.
//
// Arduino's MD5Builder (used by KOReaderDocumentId.cpp:24) is unavailable on
// the host, and the BookOrbit annotation identity key md5(datetime|pos0) must
// be computable under the native test suite. Stack use is 64 bytes of state
// plus a 128-byte tail buffer — well inside the 256-byte guideline.
std::string md5Hex(std::string_view data);

}  // namespace bookorbit
