#pragma once

#include <memory>
#include <string>

class Epub;

/**
 * Reads spine items as raw XHTML, holding one at a time.
 *
 * The BookOrbit position mappers walk source XHTML rather than laid-out pages,
 * so both directions of the annotation exchange need the same bytes, often for
 * the same spine item many times over (a book's highlights cluster in a few
 * chapters). Re-reading and re-inflating a chapter per highlight is the kind of
 * repeated work AGENTS.md rule 2 exists to prevent, so the most recent item is
 * kept and the next request for it costs nothing.
 *
 * One item is held, not a map: a spine item is tens to hundreds of kilobytes,
 * and the C3 has no room to accumulate several.
 */
class SpineTextCache {
 public:
  explicit SpineTextCache(std::shared_ptr<Epub> epub) : epub(std::move(epub)) {}

  /**
   * The spine item's XHTML, or nullptr when it cannot be read. The pointer is
   * valid until the next call with a different index.
   */
  const std::string* get(int spineIndex);

  // Frees the held item. Call before a long operation that needs the heap.
  void release();

 private:
  std::shared_ptr<Epub> epub;
  int cachedIndex = -1;
  std::string cached;
};
