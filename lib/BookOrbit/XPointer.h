#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace bookorbit {

// One element step below the DocFragment's <body>, always carrying an explicit
// 1-based same-name sibling index.
struct XPointerStep {
  std::string name;
  int index = 1;
};

// A parsed crengine xpointer.
//
// crengine emits two shapes for the same location, and which one you get
// depends on the document's DOM version (see Task 3):
//   >= 20200223: /body[1]/DocFragment[1]/body[1]/div[1]/svg[1].0
//   <  20200223: /body/DocFragment[1]/body/div/svg.0
// Both arrive over the wire from real installs. Parsing tolerates both;
// emitXPointer() always produces the indexed form.
struct XPointer {
  int docFragment = 0;              // 1-based spine fragment; 0 means absent
  std::vector<XPointerStep> steps;  // element ancestry below the fragment body
  int textNodeIndex = 0;            // N from /text()[N]; 0 when no text() step
  long charOffset = -1;             // the trailing .N; -1 when absent
  bool valid = false;
};

// Tolerant parse. Returns false (and leaves out.valid false) when raw is not a
// crengine xpointer at all.
bool parseXPointer(std::string_view raw, XPointer& out);

// Canonical fully-indexed emission.
std::string emitXPointer(const XPointer& p);

// parse + emit. Returns "" when raw does not parse.
std::string normalizeXPointer(std::string_view raw);

// True when both parse and normalize to the same canonical string.
bool sameLocation(std::string_view a, std::string_view b);

// 1-based DocFragment index, or 0 when raw is not an xpointer. Replaces the
// literal "/body/DocFragment[" searches in ProgressMapper.cpp.
int xpointerDocFragmentIndex(std::string_view raw);

// Rewrites the DocFragment index in place, normalizing the string as a side
// effect. Returns false and leaves the string untouched when it does not parse.
bool rewriteDocFragmentIndex(std::string& xpointer, int oneBasedIndex);

// A position at the very start of a chapter: no element steps below the
// fragment body and no non-zero character offset.
bool isChapterStartXPointer(std::string_view raw);

// Builds the canonical string directly. spineIndex is 0-based; the emitted
// DocFragment index is spineIndex + 1.
std::string buildCanonicalXPointer(int spineIndex, const std::vector<XPointerStep>& steps, int textNodeIndex,
                                   long charOffset);

// crengine normalizes xpointers — an explicit index on every step — from this
// DOM version onward. Documents rendered under an older version emit the
// unindexed form. Xpointers are only comparable within a DOM version.
inline constexpr int kNormalizedXPointerDomVersion = 20200223;

// True for elements crengine synthesizes into the DOM that do not exist in the
// source XHTML. The set is the boxing range at
// crengine/include/fb2def.h:40-64 (EL_BOXING_START..EL_BOXING_END) plus
// pseudoElem, which is declared alongside them.
bool isSyntheticCrengineElement(std::string_view name);

bool hasSyntheticSteps(const XPointer& p);

// Removes synthetic steps so the ancestry can be matched against the source
// XHTML, and returns how many were removed.
//
// This is lossy by nature: a legacy "/autoBoxing[2]/img[1]" becomes "/img[1]"
// where the current DOM version would say "/img[2]". Accepting the ambiguity is
// the price of reading pre-2020 xpointers at all; positions the device itself
// writes never contain synthetic steps.
int stripSyntheticSteps(XPointer& p);

// Fixed-size adapter for ProgressMapper's stack-only XPathStep array
// (ProgressMapper.cpp:122-127: char tag[12], MAX_XPATH_DEPTH = 16). Tag names
// longer than 11 characters are truncated; no allocation happens per call
// beyond the parse itself.
bool toXPathSteps(std::string_view raw, char (*tags)[12], int* siblingIndices, int capacity, int& count);
}  // namespace bookorbit
