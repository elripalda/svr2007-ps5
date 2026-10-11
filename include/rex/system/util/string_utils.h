/**
 * @file        kernel/util/string_utils.h
 * @brief       String utility functions for kernel operations
 *
 * @copyright   Copyright 2022 Ben Vanik. All rights reserved. (Xenia Project)
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#pragma once

#include <string>
#include <string_view>

#include <rex/memory.h>
#include <rex/string.h>
#include <rex/system/xio.h>  // X_ANSI_STRING, X_UNICODE_STRING
#include <rex/system/xtypes.h>

namespace rex {
namespace system {
namespace util {

inline std::string_view TranslateAnsiString(const memory::Memory* memory,
                                            const X_ANSI_STRING* ansi_string) {
  if (!ansi_string || !ansi_string->length) {
    return "";
  }
  return std::string_view(memory->TranslateVirtual<const char*>(ansi_string->pointer),
                          ansi_string->length);
}

// [svr2011] Guest strings are 8-bit ANSI; the runtime's string code expects
// UTF-8 and throws on anything else. Valid UTF-8 (all ASCII included) is kept;
// otherwise each byte is taken as Latin-1 (e.g. a content root "p0ú0",
// 4D5307E6-style) so the same guest bytes always give the same host string.
inline std::string AnsiToUtf8(std::string_view s) {
  size_t i = 0;
  bool valid = true;
  while (i < s.size() && valid) {
    const uint8_t c = uint8_t(s[i]);
    size_t n = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 0;
    if (!n || i + n > s.size()) {
      valid = false;
      break;
    }
    for (size_t k = 1; k < n; ++k) {
      if ((uint8_t(s[i + k]) >> 6) != 0x2) valid = false;
    }
    i += n;
  }
  if (valid) return std::string(s);
  std::string out;
  out.reserve(s.size() * 2);
  for (char ch : s) {
    const uint8_t c = uint8_t(ch);
    if (c < 0x80) {
      out += ch;
    } else {
      out += char(0xC0 | (c >> 6));
      out += char(0x80 | (c & 0x3F));
    }
  }
  return out;
}

inline std::string TranslateAnsiPath(const memory::Memory* memory,
                                     const X_ANSI_STRING* ansi_string) {
  return rex::string::trim_string(AnsiToUtf8(TranslateAnsiString(memory, ansi_string)));
}

inline std::string_view TranslateAnsiStringAddress(const memory::Memory* memory,
                                                   uint32_t guest_address) {
  if (!guest_address) {
    return "";
  }
  return TranslateAnsiString(memory, memory->TranslateVirtual<const X_ANSI_STRING*>(guest_address));
}

inline std::u16string TranslateUnicodeString(const memory::Memory* memory,
                                             const X_UNICODE_STRING* unicode_string) {
  if (!unicode_string) {
    return u"";
  }
  uint16_t length = unicode_string->length;
  if (!length) {
    return u"";
  }
  auto src = memory->TranslateVirtual<const uint16_t*>(unicode_string->pointer);
  std::u16string result;
  result.reserve(length);
  for (uint16_t i = 0; i < length / 2; ++i) {
    uint16_t c = rex::byte_swap(src[i]);
    result.push_back(c);
  }
  return result;
}

}  // namespace util
}  // namespace system
}  // namespace rex
