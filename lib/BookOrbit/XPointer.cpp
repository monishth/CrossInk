#include "XPointer.h"

#include <cstdlib>
#include <cstring>

namespace bookorbit {
namespace {

constexpr char kTextNode[] = "text()";

std::string_view trim(std::string_view value) {
  const auto first = value.find_first_not_of(" \t\r\n");
  if (first == std::string_view::npos) return {};
  const auto last = value.find_last_not_of(" \t\r\n");
  return value.substr(first, last - first + 1);
}

bool allDigits(const std::string_view value) {
  if (value.empty()) return false;
  for (const char c : value) {
    if (c < '0' || c > '9') return false;
  }
  return true;
}

long toLong(const std::string_view digits) {
  long value = 0;
  for (const char c : digits) {
    value = value * 10 + (c - '0');
  }
  return value;
}

// Splits "name[3]" into ("name", 3). A bare "name" yields index 1, which is
// exactly how crengine reads a pre-normalization step.
bool splitSegment(const std::string_view segment, std::string_view& name, int& index) {
  if (segment.empty()) return false;
  const auto open = segment.find('[');
  if (open == std::string_view::npos) {
    name = segment;
    index = 1;
    return true;
  }
  if (segment.back() != ']') return false;
  const std::string_view digits = segment.substr(open + 1, segment.size() - open - 2);
  if (!allDigits(digits)) return false;
  name = segment.substr(0, open);
  index = static_cast<int>(toLong(digits));
  return !name.empty() && index > 0;
}

}  // namespace

bool parseXPointer(const std::string_view raw, XPointer& out) {
  out = XPointer{};

  std::string_view text = trim(raw);
  if (text.size() < 2 || text.front() != '/') return false;

  // Trailing ".N" character offset. Element names never contain '.', so the
  // last dot followed only by digits is unambiguously the offset.
  const auto dot = text.rfind('.');
  if (dot != std::string_view::npos && allDigits(text.substr(dot + 1))) {
    out.charOffset = toLong(text.substr(dot + 1));
    text = text.substr(0, dot);
  }

  std::vector<std::string_view> segments;
  size_t pos = 0;
  while (pos < text.size()) {
    if (text[pos] != '/') return false;
    const auto next = text.find('/', pos + 1);
    const std::string_view segment =
        (next == std::string_view::npos) ? text.substr(pos + 1) : text.substr(pos + 1, next - pos - 1);
    if (segment.empty()) return false;
    segments.push_back(segment);
    if (next == std::string_view::npos) break;
    pos = next;
  }

  if (segments.size() < 2) return false;

  std::string_view name;
  int index = 1;
  if (!splitSegment(segments[0], name, index) || name != "body") return false;
  if (!splitSegment(segments[1], name, index) || name != "DocFragment") return false;
  out.docFragment = index;

  size_t first = 2;
  if (segments.size() > 2) {
    if (!splitSegment(segments[2], name, index)) return false;
    // The fragment's own <body> is optional in the pre-normalization form.
    if (name == "body") first = 3;
  }

  for (size_t i = first; i < segments.size(); i++) {
    if (segments[i].compare(0, sizeof(kTextNode) - 1, kTextNode) == 0) {
      const std::string_view tail = segments[i].substr(sizeof(kTextNode) - 1);
      if (tail.empty()) {
        out.textNodeIndex = 1;
      } else if (tail.size() >= 3 && tail.front() == '[' && tail.back() == ']' &&
                 allDigits(tail.substr(1, tail.size() - 2))) {
        out.textNodeIndex = static_cast<int>(toLong(tail.substr(1, tail.size() - 2)));
      } else {
        return false;
      }
      if (i + 1 != segments.size()) return false;  // text() is always terminal
      break;
    }
    if (!splitSegment(segments[i], name, index)) return false;
    out.steps.push_back({std::string(name), index});
  }

  out.valid = out.docFragment > 0;
  return out.valid;
}

std::string emitXPointer(const XPointer& p) {
  if (!p.valid || p.docFragment <= 0) return {};

  std::string result = "/body[1]/DocFragment[";
  result += std::to_string(p.docFragment);
  result += "]/body[1]";
  for (const auto& step : p.steps) {
    result += '/';
    result += step.name;
    result += '[';
    result += std::to_string(step.index > 0 ? step.index : 1);
    result += ']';
  }
  if (p.textNodeIndex > 0) {
    result += "/text()[";
    result += std::to_string(p.textNodeIndex);
    result += ']';
  }
  if (p.charOffset >= 0) {
    result += '.';
    result += std::to_string(p.charOffset);
  }
  return result;
}

std::string normalizeXPointer(const std::string_view raw) {
  XPointer parsed;
  if (!parseXPointer(raw, parsed)) return {};
  return emitXPointer(parsed);
}

bool sameLocation(const std::string_view a, const std::string_view b) {
  const std::string left = normalizeXPointer(a);
  if (left.empty()) return false;
  return left == normalizeXPointer(b);
}

int xpointerDocFragmentIndex(const std::string_view raw) {
  XPointer parsed;
  if (!parseXPointer(raw, parsed)) return 0;
  return parsed.docFragment;
}

bool rewriteDocFragmentIndex(std::string& xpointer, const int oneBasedIndex) {
  XPointer parsed;
  if (oneBasedIndex <= 0 || !parseXPointer(xpointer, parsed)) return false;
  parsed.docFragment = oneBasedIndex;
  xpointer = emitXPointer(parsed);
  return true;
}

bool isChapterStartXPointer(const std::string_view raw) {
  XPointer parsed;
  if (!parseXPointer(raw, parsed)) return false;
  return parsed.steps.empty() && parsed.textNodeIndex == 0 && parsed.charOffset <= 0;
}

std::string buildCanonicalXPointer(const int spineIndex, const std::vector<XPointerStep>& steps,
                                   const int textNodeIndex, const long charOffset) {
  XPointer p;
  p.docFragment = spineIndex + 1;
  p.steps = steps;
  p.textNodeIndex = textNodeIndex;
  p.charOffset = charOffset;
  p.valid = p.docFragment > 0;
  return emitXPointer(p);
}

bool isSyntheticCrengineElement(const std::string_view name) {
  return name == "autoBoxing" || name == "tabularBox" || name == "rubyBox" || name == "mathBox" || name == "floatBox" ||
         name == "inlineBox" || name == "pseudoElem";
}

bool hasSyntheticSteps(const XPointer& p) {
  for (const auto& step : p.steps) {
    if (isSyntheticCrengineElement(step.name)) return true;
  }
  return false;
}

int stripSyntheticSteps(XPointer& p) {
  // Bail out before touching anything when there is nothing to remove. This is
  // the common case by a wide margin — the modern DOM version emits no
  // synthetic steps at all — and it keeps the no-op path allocation-free, which
  // matters on the C3. It is also load-bearing for correctness: the loop below
  // moves out of p.steps, so it must only run when its result is committed.
  if (!hasSyntheticSteps(p)) return 0;

  int removed = 0;
  std::vector<XPointerStep> kept;
  kept.reserve(p.steps.size());
  for (auto& step : p.steps) {
    if (isSyntheticCrengineElement(step.name)) {
      removed++;
      continue;
    }
    kept.push_back(std::move(step));
  }
  p.steps = std::move(kept);
  return removed;
}

bool toXPathSteps(const std::string_view raw, char (*tags)[12], int* siblingIndices, const int capacity, int& count) {
  count = 0;
  if (!tags || !siblingIndices || capacity <= 0) return false;

  XPointer parsed;
  if (!parseXPointer(raw, parsed)) return false;

  for (const auto& step : parsed.steps) {
    if (count >= capacity) break;
    const size_t len = step.name.size() < 11 ? step.name.size() : 11;
    std::memcpy(tags[count], step.name.data(), len);
    tags[count][len] = '\0';
    siblingIndices[count] = step.index > 0 ? step.index : 1;
    count++;
  }
  return true;
}
}  // namespace bookorbit
