#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace bookorbit {

// One acknowledged add/edit. key/datetime/pos0 report the LOCAL identity the
// entry ended up with, so the server can address it in a later delete.
struct AppliedAck {
  std::string serverId;
  uint32_t version = 0;
  std::string key;
  std::string datetime;
  std::string pos0;
  bool failed = false;
};

struct DeletedAck {
  std::string serverId;
  bool failed = false;
};

// Leg 3 of the exchange. The server marks entries delivered only when this
// request succeeds, so an exchange whose ack never lands is re-delivered.
std::string encodeExchangeAck(std::string_view hash, const std::vector<AppliedAck>& applied,
                              const std::vector<DeletedAck>& deleted);

}  // namespace bookorbit
