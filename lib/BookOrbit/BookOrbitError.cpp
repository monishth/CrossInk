#include "BookOrbitError.h"

namespace bookorbit {

Error classify(const int httpStatus, const bool transportFailed) {
  if (transportFailed) {
    return {Status::Transport, 0};
  }
  if (httpStatus >= 200 && httpStatus < 300) {
    return {Status::Ok, httpStatus};
  }
  if (httpStatus == 401 || httpStatus == 403) {
    return {Status::Unauthorized, httpStatus};
  }
  if (httpStatus == 404) {
    return {Status::NotFound, httpStatus};
  }
  if (httpStatus >= 500) {
    return {Status::ServerError, httpStatus};
  }
  return {Status::ClientError, httpStatus};
}

bool isAuthError(const Error& error) { return error.status == Status::Unauthorized; }

bool isTransient(const Error& error) {
  return error.status == Status::Transport || error.status == Status::ServerError;
}

}  // namespace bookorbit
