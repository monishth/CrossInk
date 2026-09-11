#include "JsonStructure.h"

namespace bookorbit {
namespace {

// Mirrors StreamingJsonParser::MAX_NESTING. Anything deeper would overflow its
// container stack, so reject before handing the body over.
constexpr int kMaxNesting = 32;

bool isSpace(const char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

}  // namespace

bool jsonIsStructurallyComplete(const std::string_view json) {
  char stack[kMaxNesting];
  int depth = 0;
  bool sawRoot = false;
  bool inString = false;
  bool escaped = false;

  for (const char c : json) {
    if (inString) {
      if (escaped) {
        escaped = false;
      } else if (c == '\\') {
        escaped = true;
      } else if (c == '"') {
        inString = false;
      }
      continue;
    }

    switch (c) {
      case '"':
        inString = true;
        break;
      case '{':
      case '[':
        if (depth >= kMaxNesting) return false;
        // A second root container after the first one closed is not one body.
        if (depth == 0 && sawRoot) return false;
        stack[depth++] = c;
        sawRoot = true;
        break;
      case '}':
        if (depth == 0 || stack[depth - 1] != '{') return false;
        depth--;
        break;
      case ']':
        if (depth == 0 || stack[depth - 1] != '[') return false;
        depth--;
        break;
      default:
        // Scalars, separators and whitespace are the parser's concern, except
        // that a scalar outside any container cannot be a response body.
        if (depth == 0 && sawRoot == false && !isSpace(c)) return false;
        break;
    }
  }

  return sawRoot && depth == 0 && !inString && !escaped;
}

}  // namespace bookorbit
