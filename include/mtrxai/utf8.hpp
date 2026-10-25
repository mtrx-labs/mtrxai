#pragma once

// Small UTF-8 helpers so the TUI can accept non-ASCII input: encode one code
// point and walk offsets at character (not byte) boundaries. Kept in a header
// so unit tests can cover them without linking the terminal UI.

#include <cstdint>
#include <string>

namespace mtrxai {

// Encode one code point as UTF-8. Invalid code points become U+FFFD so the
// output stream stays decodable.
inline std::string utf8_encode(wchar_t code) {
  const std::uint32_t value = static_cast<std::uint32_t>(code);
  std::string out;
  if (value < 0x80) {
    out.push_back(static_cast<char>(value));
  } else if (value < 0x800) {
    out.push_back(static_cast<char>(0xc0 | (value >> 6)));
    out.push_back(static_cast<char>(0x80 | (value & 0x3f)));
  } else if (value < 0x10000) {
    out.push_back(static_cast<char>(0xe0 | (value >> 12)));
    out.push_back(static_cast<char>(0x80 | ((value >> 6) & 0x3f)));
    out.push_back(static_cast<char>(0x80 | (value & 0x3f)));
  } else if (value <= 0x10ffff) {
    out.push_back(static_cast<char>(0xf0 | (value >> 18)));
    out.push_back(static_cast<char>(0x80 | ((value >> 12) & 0x3f)));
    out.push_back(static_cast<char>(0x80 | ((value >> 6) & 0x3f)));
    out.push_back(static_cast<char>(0x80 | (value & 0x3f)));
  } else {
    out = "\xef\xbf\xbd";
  }
  return out;
}

// Return true when the byte is a UTF-8 continuation byte (10xxxxxx).
inline bool utf8_is_continuation(char byte) {
  return (static_cast<unsigned char>(byte) & 0xc0) == 0x80;
}

// Move a byte offset back to the start of the previous character.
inline std::size_t utf8_prev_char_start(const std::string& text,
                                        std::size_t offset) {
  if (offset == 0) return 0;
  if (offset > text.size()) offset = text.size();
  --offset;
  while (offset > 0 && utf8_is_continuation(text[offset])) --offset;
  return offset;
}

// Move a byte offset forward past the current character.
inline std::size_t utf8_next_char_end(const std::string& text,
                                      std::size_t offset) {
  if (offset >= text.size()) return text.size();
  ++offset;
  while (offset < text.size() && utf8_is_continuation(text[offset])) ++offset;
  return offset;
}

}  // namespace mtrxai
