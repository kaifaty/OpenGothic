#pragma once

#include <cstdint>
#include <string>
#include <string_view>

// Game strings are 8-bit Windows-1251 (russian localization); the bridge protocol is UTF-8.
namespace TextCodec {
  std::string toUtf8  (std::string_view cp1251);
  // characters that have no cp1251 code become '?'
  std::string fromUtf8(std::string_view utf8);
  // single code point (e.g. from a key event); '\0' if it has no cp1251 code
  char        encode(uint32_t codePoint);
  }
