#pragma once

#include <string_view>

namespace bookorbit {

// Reports whether a response body is a structurally complete JSON container.
//
// StreamingJsonParser is deliberately lenient: it flags tokens it chokes on via
// hasError(), but it never complains about input that simply stops inside an
// open container, so a truncated body decodes as a success with partial data.
// Every BookOrbit decoder therefore gates on this first.
//
// This is a structural scan, not a validator: it checks container balance,
// string termination, and nesting depth. Grammar errors inside a well-balanced
// body are still the parser's business.
//
// Returns false for an empty body, a bare scalar (BookOrbit responses are
// always an object or array), unbalanced or mismatched containers, an
// unterminated string, or nesting deeper than StreamingJsonParser::MAX_NESTING.
bool jsonIsStructurallyComplete(std::string_view json);

}  // namespace bookorbit
