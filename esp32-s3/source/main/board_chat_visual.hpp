#pragma once

#include "chat_board.hpp"
#include <cstdio>
#include <cstdint>

namespace board_chat {
struct Glyph { uint8_t rows[7]; };
inline const Glyph &glyph(uint32_t c) {
  if (c >= 'a' && c <= 'z') c -= 32;
  if (c >= 0x430 && c <= 0x44f) c -= 32;
  if (c >= 0x450 && c <= 0x45f) c -= 80;
  if (c == 0x491) c = 0x490;
  switch (c) {
    case 'A': case 0x410: { // AА
      static constexpr Glyph g{{0x0e, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11}}; return g;
    }
    case 'B': case 0x412: { // BВ
      static constexpr Glyph g{{0x1e, 0x11, 0x11, 0x1e, 0x11, 0x11, 0x1e}}; return g;
    }
    case 'C': case 0x421: { // CС
      static constexpr Glyph g{{0x0e, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0e}}; return g;
    }
    case 'D': { // D
      static constexpr Glyph g{{0x1e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1e}}; return g;
    }
    case 'E': case 0x415: { // EЕ
      static constexpr Glyph g{{0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x1f}}; return g;
    }
    case 'F': { // F
      static constexpr Glyph g{{0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x10}}; return g;
    }
    case 'G': { // G
      static constexpr Glyph g{{0x0e, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0f}}; return g;
    }
    case 'H': case 0x41d: { // HН
      static constexpr Glyph g{{0x11, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11}}; return g;
    }
    case 'I': case 0x406: { // IІ
      static constexpr Glyph g{{0x0e, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0e}}; return g;
    }
    case 'J': { // J
      static constexpr Glyph g{{0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0c}}; return g;
    }
    case 'K': case 0x41a: { // KК
      static constexpr Glyph g{{0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}}; return g;
    }
    case 'L': { // L
      static constexpr Glyph g{{0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1f}}; return g;
    }
    case 'M': case 0x41c: { // MМ
      static constexpr Glyph g{{0x11, 0x1b, 0x15, 0x15, 0x11, 0x11, 0x11}}; return g;
    }
    case 'N': { // N
      static constexpr Glyph g{{0x11, 0x19, 0x19, 0x15, 0x13, 0x13, 0x11}}; return g;
    }
    case 'O': case 0x41e: { // OО
      static constexpr Glyph g{{0x0e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e}}; return g;
    }
    case 'P': case 0x420: { // PР
      static constexpr Glyph g{{0x1e, 0x11, 0x11, 0x1e, 0x10, 0x10, 0x10}}; return g;
    }
    case 'Q': { // Q
      static constexpr Glyph g{{0x0e, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0d}}; return g;
    }
    case 'R': { // R
      static constexpr Glyph g{{0x1e, 0x11, 0x11, 0x1e, 0x14, 0x12, 0x11}}; return g;
    }
    case 'S': { // S
      static constexpr Glyph g{{0x0f, 0x10, 0x10, 0x0e, 0x01, 0x01, 0x1e}}; return g;
    }
    case 'T': case 0x422: { // TТ
      static constexpr Glyph g{{0x1f, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}}; return g;
    }
    case 'U': { // U
      static constexpr Glyph g{{0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e}}; return g;
    }
    case 'V': { // V
      static constexpr Glyph g{{0x11, 0x11, 0x11, 0x11, 0x11, 0x0a, 0x04}}; return g;
    }
    case 'W': { // W
      static constexpr Glyph g{{0x11, 0x11, 0x11, 0x15, 0x15, 0x1b, 0x11}}; return g;
    }
    case 'X': case 0x425: { // XХ
      static constexpr Glyph g{{0x11, 0x11, 0x0a, 0x04, 0x0a, 0x11, 0x11}}; return g;
    }
    case 'Y': { // Y
      static constexpr Glyph g{{0x11, 0x11, 0x0a, 0x04, 0x04, 0x04, 0x04}}; return g;
    }
    case 'Z': { // Z
      static constexpr Glyph g{{0x1f, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1f}}; return g;
    }
    case 0x411: { // Б
      static constexpr Glyph g{{0x1f, 0x10, 0x10, 0x1e, 0x11, 0x11, 0x1e}}; return g;
    }
    case 0x413: { // Г
      static constexpr Glyph g{{0x1f, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10}}; return g;
    }
    case 0x490: { // Ґ
      static constexpr Glyph g{{0x01, 0x1f, 0x10, 0x10, 0x10, 0x10, 0x10}}; return g;
    }
    case 0x414: { // Д
      static constexpr Glyph g{{0x06, 0x0a, 0x0a, 0x0a, 0x0a, 0x1f, 0x11}}; return g;
    }
    case 0x404: { // Є
      static constexpr Glyph g{{0x0e, 0x11, 0x10, 0x1e, 0x10, 0x11, 0x0e}}; return g;
    }
    case 0x416: { // Ж
      static constexpr Glyph g{{0x15, 0x15, 0x0e, 0x04, 0x0e, 0x15, 0x15}}; return g;
    }
    case 0x417: { // З
      static constexpr Glyph g{{0x0e, 0x11, 0x01, 0x06, 0x01, 0x11, 0x0e}}; return g;
    }
    case 0x418: { // И
      static constexpr Glyph g{{0x11, 0x11, 0x13, 0x15, 0x19, 0x11, 0x11}}; return g;
    }
    case 0x407: { // Ї
      static constexpr Glyph g{{0x0a, 0x00, 0x0e, 0x04, 0x04, 0x04, 0x0e}}; return g;
    }
    case 0x419: { // Й
      static constexpr Glyph g{{0x0a, 0x04, 0x11, 0x13, 0x15, 0x19, 0x11}}; return g;
    }
    case 0x41b: { // Л
      static constexpr Glyph g{{0x07, 0x09, 0x09, 0x09, 0x09, 0x09, 0x11}}; return g;
    }
    case 0x41f: { // П
      static constexpr Glyph g{{0x1f, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11}}; return g;
    }
    case 0x423: { // У
      static constexpr Glyph g{{0x11, 0x11, 0x11, 0x0f, 0x01, 0x02, 0x1c}}; return g;
    }
    case 0x424: { // Ф
      static constexpr Glyph g{{0x04, 0x0e, 0x15, 0x15, 0x15, 0x0e, 0x04}}; return g;
    }
    case 0x426: { // Ц
      static constexpr Glyph g{{0x12, 0x12, 0x12, 0x12, 0x12, 0x1f, 0x01}}; return g;
    }
    case 0x427: { // Ч
      static constexpr Glyph g{{0x11, 0x11, 0x11, 0x0f, 0x01, 0x01, 0x01}}; return g;
    }
    case 0x428: { // Ш
      static constexpr Glyph g{{0x15, 0x15, 0x15, 0x15, 0x15, 0x15, 0x1f}}; return g;
    }
    case 0x429: { // Щ
      static constexpr Glyph g{{0x15, 0x15, 0x15, 0x15, 0x15, 0x1f, 0x01}}; return g;
    }
    case 0x42c: { // Ь
      static constexpr Glyph g{{0x10, 0x10, 0x10, 0x1e, 0x11, 0x11, 0x1e}}; return g;
    }
    case 0x42e: { // Ю
      static constexpr Glyph g{{0x12, 0x15, 0x15, 0x1d, 0x15, 0x15, 0x12}}; return g;
    }
    case 0x42f: { // Я
      static constexpr Glyph g{{0x0f, 0x11, 0x11, 0x0f, 0x05, 0x09, 0x11}}; return g;
    }
    case '0': { // 0
      static constexpr Glyph g{{0x0e, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0e}}; return g;
    }
    case '1': { // 1
      static constexpr Glyph g{{0x04, 0x0c, 0x04, 0x04, 0x04, 0x04, 0x0e}}; return g;
    }
    case '2': { // 2
      static constexpr Glyph g{{0x0e, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1f}}; return g;
    }
    case '3': { // 3
      static constexpr Glyph g{{0x1e, 0x01, 0x01, 0x0e, 0x01, 0x01, 0x1e}}; return g;
    }
    case '4': { // 4
      static constexpr Glyph g{{0x02, 0x06, 0x0a, 0x12, 0x1f, 0x02, 0x02}}; return g;
    }
    case '5': { // 5
      static constexpr Glyph g{{0x1f, 0x10, 0x10, 0x1e, 0x01, 0x01, 0x1e}}; return g;
    }
    case '6': { // 6
      static constexpr Glyph g{{0x0e, 0x10, 0x10, 0x1e, 0x11, 0x11, 0x0e}}; return g;
    }
    case '7': { // 7
      static constexpr Glyph g{{0x1f, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}}; return g;
    }
    case '8': { // 8
      static constexpr Glyph g{{0x0e, 0x11, 0x11, 0x0e, 0x11, 0x11, 0x0e}}; return g;
    }
    case '9': { // 9
      static constexpr Glyph g{{0x0e, 0x11, 0x11, 0x0f, 0x01, 0x01, 0x0e}}; return g;
    }
    case '.': { // .
      static constexpr Glyph g{{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04}}; return g;
    }
    case ',': { // ,
      static constexpr Glyph g{{0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x08}}; return g;
    }
    case ':': { // :
      static constexpr Glyph g{{0x00, 0x00, 0x04, 0x00, 0x00, 0x04, 0x00}}; return g;
    }
    case '>': {
      static constexpr Glyph g{{0, 16, 8, 4, 8, 16, 0}}; return g;
    }
    case '!': { // !
      static constexpr Glyph g{{0x04, 0x04, 0x04, 0x04, 0x04, 0x00, 0x04}}; return g;
    }
    case 0x27: case 0x2019: case 0x2bc: { // '’ʼ
      static constexpr Glyph g{{0x04, 0x04, 0x08, 0x00, 0x00, 0x00, 0x00}}; return g;
    }
    case '-': case 0x2013: case 0x2014: { // -–—
      static constexpr Glyph g{{0x00, 0x00, 0x00, 0x1f, 0x00, 0x00, 0x00}}; return g;
    }
    case '?': { // ?
      static constexpr Glyph g{{0x0e, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04}}; return g;
    }
    default: {
      static constexpr Glyph blank{};
      return c <= 32 ? blank : glyph('?');
    }
  }
}

// Invalid sequences consume one byte and yield a visible replacement.
inline uint32_t next(const char *&p) {
  const uint8_t lead = uint8_t(*p++);
  if (lead < 128) return lead;
  const unsigned n = lead < 0xc2 || lead > 0xf4 ? 0 :
                     lead < 0xe0 ? 1 : lead < 0xf0 ? 2 : 3;
  if (!n) return '?';
  uint32_t cp = lead & (0x7f >> (n + 1));
  for (unsigned i = 0; i < n; ++i) {
    const uint8_t b = uint8_t(p[i]);
    if ((b & 0xc0) != 0x80) return '?';
    cp = (cp << 6) | (b & 63);
  }
  p += n;
  if ((n == 2 && cp < 0x800) || (n == 3 && cp < 0x10000) ||
      (cp >= 0xd800 && cp <= 0xdfff) || cp > 0x10ffff) return '?';
  return cp;
}

class Scroll {
 public:
  void pause() { ticking_ = false; }
  template<class Pixel>
  bool draw(uint32_t now, int width, Pixel pixel) {
    if (!length_) {
      if (!chatBoardTake(message_)) return false;
      char id[9];
      std::snprintf(id, sizeof(id), "%08X", unsigned(message_.sender));
      append(message_.name[0] ? message_.name : id);
      chars_[length_++] = ':';
      chars_[length_++] = ' ';
      senderEnd_ = length_;
      append(message_.text);
      elapsed_ = 0;
      ticking_ = false;
    }
    if (ticking_) elapsed_ += now - previous_;
    previous_ = now;
    ticking_ = true;
    const int offset = width - 1 - int(elapsed_ / 75);
    if (offset + length_ * 6 <= 0) {
      length_ = 0;
      return false;
    }
    const int first = offset < 0 ? -offset / 6 : 0;
    for (int i = first; i < length_ && offset + i * 6 < width; ++i) {
      const auto &g = glyph(chars_[i]);
      for (int dx = 0; dx < 5; ++dx) {
        const int x = offset + i * 6 + dx;
        if (x < 0 || x >= width) continue;
        for (int y = 0; y < 7; ++y)
          if (g.rows[y] & (16 >> dx)) pixel(x, y, i < senderEnd_);
      }
    }
    return true;
  }
 private:
  void append(const char *p) {
    while (*p) {
      const uint32_t cp = next(p);
      chars_[length_++] = cp <= 0xffff ? uint16_t(cp) : '?';
    }
  }
  ChatBoardMessage message_{};
  uint16_t chars_[sizeof(message_.name) + sizeof(message_.text) + 10]{};
  int length_ = 0, senderEnd_ = 0;
  uint32_t elapsed_ = 0, previous_ = 0;
  bool ticking_ = false;
};
}
