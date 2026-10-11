#pragma once

#include <cstdint>
#include <string>
#include <string_view>

// Game strings are 8-bit Windows-1251 (russian localization); the bridge protocol is UTF-8.
namespace TextCodec {
  std::string toUtf8  (std::string_view cp1251);
  // characters that have no cp1251 code become '?'; ones the game fonts can't draw (ё, —, «») get a lookalike
  std::string fromUtf8(std::string_view utf8);
  // single code point (e.g. from a key event), with the same lookalikes; '\0' if there is no single byte for it
  char        encode(uint32_t codePoint);
  }
