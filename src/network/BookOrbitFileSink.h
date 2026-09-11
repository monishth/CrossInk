#pragma once
// HalStorage already provides FsFile (aliased to HalFile); including
// SdFat.h as well redeclares FsFile as a class and fails to compile.
#include <HalStorage.h>

#include <string>

#include "IFileSink.h"

/**
 * IFileSink over the HAL storage layer. Holds at most one open FsFile — on
 * real hardware only one reader can hold a file open at a time — and closes it
 * explicitly in every exit path, including the destructor.
 */
class BookOrbitFileSink final : public bookorbit::IFileSink {
 public:
  ~BookOrbitFileSink() override { close(); }

  bool open(std::string_view path) override;
  bool write(const uint8_t* data, size_t len) override;
  bool close() override;
  bool publish(std::string_view from, std::string_view to) override;
  bool remove(std::string_view path) override;
  bool exists(std::string_view path) override;

 private:
  FsFile file_;
  bool open_ = false;
  // string_view::data() is not null-terminated, so every path crossing into the
  // C storage API is materialized here first.
  std::string scratch_;
};
