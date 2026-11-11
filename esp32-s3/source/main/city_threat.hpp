#pragma once

#include "telegram_alert.hpp"

namespace air_alert {
namespace city_threat {

// An anchored, whole-message grammar deliberately rejects directions, regional
// reports, negation and additional clauses. Extend accepted wording explicitly.
struct CityText {
  char text[512] = {};
  uint16_t used = 0;
  uint16_t bytes = 0;
  bool overflow = false;

  bool codepoint(uint32_t cp) {
    const size_t inputWidth = cp < 0x80 ? 1 : cp < 0x800 ? 2 : cp < 0x10000 ? 3 : 4;
    if (bytes > telegram::detail::kTextLimit - inputWidth) return false;
    bytes = uint16_t(bytes + inputWidth);
    if (cp == 0xfe0f) return true;
    if (cp == 0x1f4cd || cp == 0x26a0 || cp == 0x1f4e2 ||
        cp == 0x1f6f5 || cp == 0x1f3cd || cp == 0x1f680 || cp == 0x1f6e9) cp = ' ';
    if (cp >= 0x410 && cp <= 0x42f) cp += 32;
    else if (cp == 0x404 || cp == 0x406 || cp == 0x407) cp += 0x50;
    else if (cp == 0x490) cp = 0x491;
    else if (cp >= 'A' && cp <= 'Z') cp += 32;
    if (cp == 160 || (cp >= 0x2000 && cp <= 0x200a) || cp == 0x202f ||
        (cp < 128 && telegram::detail::space(char(cp)))) cp = ' ';
    if (cp == ' ' && (!used || text[used - 1] == ' ')) return true;
    if (cp < 32 || cp == 127 || cp > 0x10ffff ||
        (cp >= 0xd800 && cp <= 0xdfff)) return false;
    if (overflow) return true;
    // The accepted vocabulary is entirely ASCII and two-byte Cyrillic UTF-8.
    // Other scalars remain a non-match without truncating the message.
    if (cp >= 0x800) cp = '?';
    const size_t width = cp < 128 ? 1 : 2;
    if (used + width >= sizeof(text)) { overflow = true; return true; }
    if (width == 2) {
      text[used++] = char(0xc0 | (cp >> 6));
      text[used++] = char(0x80 | (cp & 63));
    } else text[used++] = char(cp);
    text[used] = 0;
    return true;
  }

  void newline() {
    // Preserve a boundary even when a block ends exactly at capacity.
    if (used && text[used - 1] != ' ') {
      if (size_t(used) + 1 < sizeof(text)) { text[used++] = ' '; text[used] = 0; }
      else overflow = true;
    }
  }

  bool finish(Level &level) {
    level = Level::Unknown;
    if (overflow) return true;
    while (used && text[used - 1] == ' ') text[--used] = 0;
    const char *p = text;
    const char city[] = "вінниця";
    const size_t citySize = sizeof(city) - 1;
    if (used <= citySize || std::memcmp(p, city, citySize)) return true;
    p += citySize;
    if (*p == ':') ++p;
    else if (*p != ' ') return true;
    while (*p == ' ') ++p;
    struct Threat { const char *name; Level level; };
    static constexpr Threat threats[] = {
        {"реактивний бпла", Level::Yellow}, {"бпла", Level::Yellow},
        {"дрон", Level::Yellow}, {"дрони", Level::Yellow},
        {"шахед", Level::Yellow}, {"шахеди", Level::Yellow},
        {"ракета", Level::Red}, {"ракети", Level::Red}};
    for (const auto &threat : threats) {
      const size_t size = std::strlen(threat.name);
      if (std::strncmp(p, threat.name, size)) continue;
      const char *location = p + size;
      for (const char *phrase : {" над містом", " у місті", " в місті",
                                 " у межах міста", " в межах міста"}) {
        const size_t length = std::strlen(phrase);
        if (std::strncmp(location, phrase, length)) continue;
        const char *tail = location + length;
        if (tail[0] == ' ' && tail[1] == '(' && !locality(tail)) continue;
        if (*tail == '.' || *tail == '!') ++tail;
        if (!*tail) level = threat.level;
      }
    }
    return true;
  }

 private:
  static bool locality(const char *&tail) {
    const char *p = tail + 2;
    // Reject direction, regional and negative qualifiers even inside parentheses.
    for (const char *bad : {"курс", "напрям", "повз", "район", "област", "вінниччин",
                            "північ", "півден", "південь", "захід", "схід", "на ",
                            "нема", "відсут", "відбій", "не ", "без ", "не)"})
      if (std::strstr(p, bad)) return false;
    const char *end = p + std::strlen(p);
    unsigned count = 0, letters = 0;
    while (p != end && *p != ')') {
      uint32_t cp;
      if (++count > 48 || !telegram::detail::scalar(p, end, cp)) return false;
      const bool alpha = (cp >= 'a' && cp <= 'z') || (cp >= 0x430 && cp <= 0x44f) ||
          cp == 0x454 || cp == 0x456 || cp == 0x457 || cp == 0x491;
      if (!alpha && cp != ' ' && cp != '-') return false;
      if (alpha) ++letters;
    }
    if (!letters || p == end || *p != ')') return false;
    tail = p + 1;
    return true;
  }
};

using Page = telegram::Page;
using Diagnostics = telegram::Diagnostics;
using Parser = telegram::BasicParser<CityText>;
static_assert(sizeof(Parser) <= 1536, "City parser must fit the poll task budget");

// Structural validity and an event are separate. A valid page with eventId == 0
// carries no city threat. The caller enforces publication TTL and ID replay rules.
inline bool parsePage(const char *body, size_t length, Page &out,
                      Diagnostics *diagnostics = nullptr) {
  Parser parser;
  return parser.parse(body, length, out, diagnostics, "kpszsu/");
}

}  // namespace city_threat
}  // namespace air_alert
