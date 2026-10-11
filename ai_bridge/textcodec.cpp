#include "textcodec.h"

#include <cstdint>

// Unicode code points of cp1251 bytes 0x80..0xFF, 0 = undefined
static const uint16_t cp1251Upper[128] = {
  0x0402, 0x0403, 0x201A, 0x0453, 0x201E, 0x2026, 0x2020, 0x2021,
  0x20AC, 0x2030, 0x0409, 0x2039, 0x040A, 0x040C, 0x040B, 0x040F,
  0x0452, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
  0x0000, 0x2122, 0x0459, 0x203A, 0x045A, 0x045C, 0x045B, 0x045F,
  0x00A0, 0x040E, 0x045E, 0x0408, 0x00A4, 0x0490, 0x00A6, 0x00A7,
  0x0401, 0x00A9, 0x0404, 0x00AB, 0x00AC, 0x00AD, 0x00AE, 0x0407,
  0x00B0, 0x00B1, 0x0406, 0x0456, 0x0491, 0x00B5, 0x00B6, 0x00B7,
  0x0451, 0x2116, 0x0454, 0x00BB, 0x0458, 0x0405, 0x0455, 0x0457,
  0x0410, 0x0411, 0x0412, 0x0413, 0x0414, 0x0415, 0x0416, 0x0417,
  0x0418, 0x0419, 0x041A, 0x041B, 0x041C, 0x041D, 0x041E, 0x041F,
  0x0420, 0x0421, 0x0422, 0x0423, 0x0424, 0x0425, 0x0426, 0x0427,
  0x0428, 0x0429, 0x042A, 0x042B, 0x042C, 0x042D, 0x042E, 0x042F,
  0x0430, 0x0431, 0x0432, 0x0433, 0x0434, 0x0435, 0x0436, 0x0437,
  0x0438, 0x0439, 0x043A, 0x043B, 0x043C, 0x043D, 0x043E, 0x043F,
  0x0440, 0x0441, 0x0442, 0x0443, 0x0444, 0x0445, 0x0446, 0x0447,
  0x0448, 0x0449, 0x044A, 0x044B, 0x044C, 0x044D, 0x044E, 0x044F,
  };

static void appendUtf8(std::string& out, uint32_t cp) {
  if(cp<0x80) {
    out.push_back(char(cp));
    }
  else if(cp<0x800) {
    out.push_back(char(0xC0 | (cp>>6)));
    out.push_back(char(0x80 | (cp & 0x3F)));
    }
  else {
    out.push_back(char(0xE0 | (cp>>12)));
    out.push_back(char(0x80 | ((cp>>6) & 0x3F)));
    out.push_back(char(0x80 | (cp & 0x3F)));
    }
  }

// The russian game fonts have empty (zero width) glyphs for some cp1251 characters,
// so text using them would lose letters: "Твоё" shows as "Тво". These get a lookalike.
static std::string_view fontFallback(uint32_t cp) {
  switch(cp) {
    case 0x0401: return "\xC5";   // Ё -> Е
    case 0x0451: return "\xE5";   // ё -> е
    case 0x00A0: return " ";      // no-break space
    case 0x2014: return "\x96";   // em dash -> en dash
    case 0x00AB: return "\"";     // «
    case 0x00BB: return "\"";     // »
    case 0x2026: return "...";    // …
    case 0x2116: return "N";      // №
    }
  return {};
  }

static void appendCp1251(std::string& out, uint32_t cp) {
  const auto alt = fontFallback(cp);
  if(!alt.empty()) {
    out.append(alt);
    return;
    }
  const char c = TextCodec::encode(cp);
  out.push_back(c!='\0' ? c : '?');
  }

char TextCodec::encode(uint32_t cp) {
  const auto alt = fontFallback(cp);
  if(!alt.empty())
    return alt.size()==1 ? alt[0] : '\0';
  if(cp<0x80)
    return char(cp);
  for(uint32_t i=0; i<128; ++i)
    if(cp1251Upper[i]==cp)
      return char(0x80+i);
  return '\0';
  }

std::string TextCodec::toUtf8(std::string_view cp1251) {
  std::string out;
  out.reserve(cp1251.size()*2);
  for(char c:cp1251) {
    const auto b = uint8_t(c);
    if(b<0x80) {
      out.push_back(c);
      continue;
      }
    const uint16_t cp = cp1251Upper[b-0x80];
    appendUtf8(out, cp==0 ? uint32_t('?') : cp);
    }
  return out;
  }

std::string TextCodec::fromUtf8(std::string_view utf8) {
  std::string out;
  out.reserve(utf8.size());
  for(size_t i=0; i<utf8.size();) {
    const auto b   = uint8_t(utf8[i]);
    uint32_t   cp  = 0;
    size_t     len = 1;
    if(b<0x80) {
      cp = b;
      }
    else if((b>>5)==0x6) {
      cp = b & 0x1F; len = 2;
      }
    else if((b>>4)==0xE) {
      cp = b & 0x0F; len = 3;
      }
    else if((b>>3)==0x1E) {
      cp = b & 0x07; len = 4;
      }
    else {
      out.push_back('?');
      ++i;
      continue;
      }

    if(i+len>utf8.size()) {
      out.push_back('?');
      break;
      }
    bool valid = true;
    for(size_t r=1; r<len; ++r) {
      const auto cb = uint8_t(utf8[i+r]);
      if((cb>>6)!=0x2) {
        valid = false;
        break;
        }
      cp = (cp<<6) | (cb & 0x3F);
      }
    if(!valid) {
      out.push_back('?');
      ++i;
      continue;
      }
    appendCp1251(out, cp);
    i += len;
    }
  return out;
  }
