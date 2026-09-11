#pragma once

#include <cstdint>
#include <string_view>

#include "XPointer.h"

namespace bookorbit {

// Streams a spine item's XHTML and resolves an xpointer's element ancestry to a
// zero-based visible-codepoint offset. Same-name siblings are counted per
// parent, so /div[1]/p[2] means "the second <p> child of the first <div>",
// exactly as crengine means it.
//
// The caller must have removed synthetic boxing steps first
// (stripSyntheticSteps): those elements exist in crengine's DOM but never in
// the source XHTML streamed here.
//
// Non-visible containers (head, style, script, title, rp, rt) contribute no
// codepoints, matching ChapterXPathResolver's existing rule.
bool resolveXPointerToOffset(std::string_view xhtml, const XPointer& target, uint32_t& visibleOffset);

// The inverse: the canonical xpointer for a visible-codepoint offset, with
// spineIndex (0-based) becoming the DocFragment index.
bool resolveOffsetToXPointer(std::string_view xhtml, uint32_t visibleOffset, int spineIndex, XPointer& out);

// Total visible codepoints in the document.
bool visibleTextLength(std::string_view xhtml, uint32_t& length);

}  // namespace bookorbit
