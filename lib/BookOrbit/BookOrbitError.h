#pragma once

namespace bookorbit {

enum class Status {
  Ok,
  Unauthorized,  // 401 / 403 — abort the whole sync
  NotFound,      // 404 — may downgrade a capability
  ClientError,   // other 4xx — definitive "no"
  ServerError,   // 5xx — retry on a later trigger
  Transport,     // socket/TLS/DNS failure — no HTTP status
  BodyTooLarge,  // request exceeded MAX_BODY_BYTES, never sent
  InvalidJson,
};

struct Error {
  Status status = Status::Ok;
  int httpStatus = 0;
};

// Maps an HTTP status (or a transport failure) onto the taxonomy.
// transportFailed takes precedence; httpStatus is then meaningless.
Error classify(int httpStatus, bool transportFailed);

// 401/403 only. These abort the entire sync rather than one phase.
bool isAuthError(const Error& error);

// True when the outcome carries no information about server support:
// any 5xx, or a transport failure. A definitive 4xx is NOT transient.
bool isTransient(const Error& error);

}  // namespace bookorbit
