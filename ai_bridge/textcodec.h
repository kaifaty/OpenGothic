#pragma once

#include <string>
#include <string_view>

// Game strings are 8-bit Windows-1251 (russian localization); the bridge protocol is UTF-8.
namespace TextCodec {
  std::string toUtf8  (std::string_view cp1251);
  // characters that have no cp1251 code become '?'
  std::string fromUtf8(std::string_view utf8);
  }
