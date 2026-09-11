#include "XPointerResolver.h"

#include <expat.h>

#include <cstring>
#include <string>
#include <vector>

#include "Utf8.h"

namespace bookorbit {
namespace {

std::string stripPrefix(const XML_Char* name) {
  if (!name) return {};
  const char* local = std::strrchr(name, ':');
  return local ? std::string(local + 1) : std::string(name);
}

bool isNonVisibleTag(const std::string& name) {
  return name == "head" || name == "style" || name == "script" || name == "title" || name == "rp" || name == "rt";
}

size_t countUtf8Codepoints(const char* data, const int len) {
  if (!data || len <= 0) return 0;
  size_t count = 0;
  const unsigned char* ptr = reinterpret_cast<const unsigned char*>(data);
  const unsigned char* end = ptr + len;
  while (ptr < end) {
    utf8NextCodepoint(&ptr);
    count++;
  }
  return count;
}

struct NameCounter {
  std::string name;
  int count;
};

// Per-parent same-name sibling counters. One entry is pushed for every open
// element, so counting is always scoped to the immediate parent.
struct ParentState {
  std::vector<NameCounter> children;
  int textNodes = 0;

  int nextIndex(const std::string& name) {
    for (auto& child : children) {
      if (child.name == name) {
        child.count++;
        return child.count;
      }
    }
    children.push_back({name, 1});
    return 1;
  }
};

// One expat pass serving both directions. Which direction is active depends on
// whether target or wantOffset is set.
class AncestryWalker {
 public:
  AncestryWalker() {
    parser = XML_ParserCreate(nullptr);
    if (!parser) return;
    XML_SetUserData(parser, this);
    XML_SetElementHandler(parser, &AncestryWalker::startElement, &AncestryWalker::endElement);
    XML_SetCharacterDataHandler(parser, &AncestryWalker::characterData);
  }

  ~AncestryWalker() {
    if (parser) XML_ParserFree(parser);
  }

  bool run(const std::string_view xhtml) {
    if (!parser) return false;
    if (XML_Parse(parser, xhtml.data(), static_cast<int>(xhtml.size()), XML_TRUE) != XML_STATUS_OK) {
      const enum XML_Error error = XML_GetErrorCode(parser);
      if (error != XML_ERROR_ABORTED) return false;
    }
    return true;
  }

  // Direction 1: find this element ancestry.
  const XPointer* target = nullptr;
  bool targetFound = false;
  uint32_t targetOffset = 0;

  // Direction 2: find the element covering this offset.
  bool wantOffset = false;
  uint32_t wantedOffset = 0;
  bool offsetFound = false;
  std::vector<XPointerStep> offsetPath;
  int offsetTextNode = 0;
  long offsetChar = 0;

  uint32_t visibleChars = 0;

 private:
  static void XMLCALL startElement(void* userData, const XML_Char* name, const XML_Char**) {
    static_cast<AncestryWalker*>(userData)->onStart(stripPrefix(name));
  }
  static void XMLCALL endElement(void* userData, const XML_Char* name) {
    static_cast<AncestryWalker*>(userData)->onEnd(stripPrefix(name));
  }
  static void XMLCALL characterData(void* userData, const XML_Char* data, const int len) {
    static_cast<AncestryWalker*>(userData)->onText(data, len);
  }

  void onStart(const std::string& name) {
    if (stopped) return;

    if (!insideBody) {
      if (name == "body") {
        insideBody = true;
        parents.emplace_back();
        // A target with no element steps addresses a text node directly under
        // <body>. There is no element start to match it against, so arm it here
        // or onText will never consider it.
        if (target && !targetFound && target->steps.empty()) {
          if (target->textNodeIndex <= 0 && target->charOffset <= 0) {
            targetFound = true;
            targetOffset = 0;
            stop();
            return;
          }
          insideTarget = true;
          targetElementStart = 0;
        }
      } else if (isNonVisibleTag(name)) {
        nonVisibleDepth++;
      }
      return;
    }

    const int index = parents.back().nextIndex(name);
    path.push_back({name, index});
    parents.emplace_back();
    if (isNonVisibleTag(name)) nonVisibleDepth++;

    if (target && !targetFound && matchesTarget()) {
      // The element starts here. A text() step or a character offset is applied
      // by onText once the right text node is reached; with neither, the
      // element start is the answer.
      if (target->textNodeIndex <= 0 && target->charOffset <= 0) {
        targetFound = true;
        targetOffset = visibleChars;
        stop();
        return;
      }
      insideTarget = true;
    }
  }

  void onEnd(const std::string& name) {
    if (stopped) return;
    if (isNonVisibleTag(name) && nonVisibleDepth > 0) nonVisibleDepth--;
    if (!insideBody) return;

    if (path.empty()) {
      insideBody = false;
      if (!parents.empty()) parents.pop_back();
      return;
    }

    if (insideTarget && path.size() == target->steps.size()) {
      // The target element closed before its text node was found. Fall back to
      // the element start plus the raw offset, so a stale offset still lands
      // inside the right element rather than failing outright.
      targetFound = true;
      targetOffset = targetElementStart + static_cast<uint32_t>(target->charOffset > 0 ? target->charOffset : 0);
      stop();
      return;
    }

    path.pop_back();
    if (!parents.empty()) parents.pop_back();
  }

  void onText(const char* data, const int len) {
    if (stopped || !insideBody || nonVisibleDepth > 0) return;

    const size_t codepoints = countUtf8Codepoints(data, len);
    if (codepoints == 0) return;

    const int textNode = parents.empty() ? 1 : ++parents.back().textNodes;

    if (insideTarget && path.size() == target->steps.size()) {
      const int wanted = target->textNodeIndex > 0 ? target->textNodeIndex : 1;
      if (textNode == wanted) {
        targetFound = true;
        targetOffset = visibleChars + static_cast<uint32_t>(target->charOffset > 0 ? target->charOffset : 0);
        stop();
        return;
      }
    }

    if (wantOffset && !offsetFound && wantedOffset >= visibleChars &&
        wantedOffset < visibleChars + static_cast<uint32_t>(codepoints)) {
      offsetFound = true;
      offsetPath = path;
      offsetTextNode = textNode;
      offsetChar = static_cast<long>(wantedOffset - visibleChars);
      stop();
      return;
    }

    visibleChars += static_cast<uint32_t>(codepoints);
  }

  bool matchesTarget() {
    if (path.size() != target->steps.size()) return false;
    for (size_t i = 0; i < path.size(); i++) {
      if (path[i].name != target->steps[i].name || path[i].index != target->steps[i].index) return false;
    }
    targetElementStart = visibleChars;
    return true;
  }

  void stop() {
    stopped = true;
    XML_StopParser(parser, XML_FALSE);
  }

  XML_Parser parser = nullptr;
  std::vector<ParentState> parents;
  std::vector<XPointerStep> path;
  bool insideBody = false;
  bool insideTarget = false;
  bool stopped = false;
  int nonVisibleDepth = 0;
  uint32_t targetElementStart = 0;
};

}  // namespace

bool resolveXPointerToOffset(const std::string_view xhtml, const XPointer& target, uint32_t& visibleOffset) {
  if (!target.valid) return false;

  // An empty step list with no text-node selector is the chapter start: offset
  // 0 plus any raw character offset, and no parse is needed. A text()[N] with
  // N > 1 does need the walk, because it names the Nth text node directly under
  // <body>, which is not where the chapter starts.
  if (target.steps.empty() && target.textNodeIndex <= 1) {
    visibleOffset = static_cast<uint32_t>(target.charOffset > 0 ? target.charOffset : 0);
    return true;
  }

  AncestryWalker walker;
  walker.target = &target;
  if (!walker.run(xhtml) || !walker.targetFound) return false;
  visibleOffset = walker.targetOffset;
  return true;
}

bool resolveOffsetToXPointer(const std::string_view xhtml, const uint32_t visibleOffset, const int spineIndex,
                             XPointer& out) {
  out = XPointer{};
  if (spineIndex < 0) return false;

  AncestryWalker walker;
  walker.wantOffset = true;
  walker.wantedOffset = visibleOffset;
  if (!walker.run(xhtml) || !walker.offsetFound) return false;

  out.docFragment = spineIndex + 1;
  out.steps = walker.offsetPath;
  out.textNodeIndex = walker.offsetTextNode;
  out.charOffset = walker.offsetChar;
  out.valid = true;
  return true;
}

bool visibleTextLength(const std::string_view xhtml, uint32_t& length) {
  AncestryWalker walker;
  if (!walker.run(xhtml)) return false;
  length = walker.visibleChars;
  return true;
}

}  // namespace bookorbit
