#pragma once

#include "air_alert.hpp"
#include <cstring>
#include <initializer_list>

namespace air_alert {
namespace telegram {

struct Page {
  uint32_t newestId = 0, oldestId = 0, eventId = 0;
  int64_t newestTime = 0, eventTime = 0;
  Level level = Level::Unknown;
  unsigned posts = 0;
};

namespace detail {

constexpr size_t kPageLimit = 256 * 1024;
constexpr unsigned kPostLimit = 100, kDepthLimit = 64;
constexpr size_t kTextLimit = 8192;

struct Slice {
  const char *data = nullptr;
  size_t size = 0;
};

inline bool equal(Slice s, const char *text) {
  return s.size == std::strlen(text) && !std::memcmp(s.data, text, s.size);
}
inline char lower(char c) { return c >= 'A' && c <= 'Z' ? char(c + 32) : c; }
inline bool nameEqual(Slice s, const char *text) {
  if (s.size != std::strlen(text)) return false;
  for (size_t i = 0; i < s.size; ++i) if (lower(s.data[i]) != text[i]) return false;
  return true;
}
inline bool space(char c) {
  return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f';
}
inline bool nameChar(char c) {
  return (lower(c) >= 'a' && lower(c) <= 'z') || (c >= '0' && c <= '9') ||
      c == '-' || c == '_' || c == ':';
}

// Strict UTF-8: malformed encodings and numeric references cannot hide text.
inline bool scalar(const char *&p, const char *end, uint32_t &cp) {
  if (p == end) return false;
  const unsigned first = static_cast<unsigned char>(*p++);
  if (first < 0x80) { cp = first; return first != 0; }
  unsigned count;
  uint32_t minimum;
  if (first >= 0xc2 && first <= 0xdf) { count = 1; cp = first & 31; minimum = 0x80; }
  else if (first >= 0xe0 && first <= 0xef) { count = 2; cp = first & 15; minimum = 0x800; }
  else if (first >= 0xf0 && first <= 0xf4) { count = 3; cp = first & 7; minimum = 0x10000; }
  else return false;
  while (count--) {
    if (p == end) return false;
    const unsigned c = static_cast<unsigned char>(*p++);
    if ((c & 0xc0) != 0x80) return false;
    cp = (cp << 6) | (c & 63);
  }
  return cp >= minimum && cp <= 0x10ffff && !(cp >= 0xd800 && cp <= 0xdfff);
}

inline bool entity(const char *&p, const char *end, uint32_t &cp) {
  const char *start = ++p;
  while (p != end && *p != ';' && p - start < 16) ++p;
  if (p == end || *p != ';') return false;
  const Slice value{start, size_t(p++ - start)};
  if (value.size && value.data[0] == '#') {
    size_t i = 1;
    unsigned base = 10;
    if (i < value.size && (value.data[i] == 'x' || value.data[i] == 'X')) { base = 16; ++i; }
    if (i == value.size) return false;
    cp = 0;
    for (; i < value.size; ++i) {
      const char c = lower(value.data[i]);
      const unsigned digit = c >= '0' && c <= '9' ? unsigned(c - '0') :
          c >= 'a' && c <= 'f' ? unsigned(c - 'a' + 10) : 16;
      if (digit >= base || cp > (0x10ffffU - digit) / base) return false;
      cp = cp * base + digit;
    }
    return cp != 0 && cp <= 0x10ffff && !(cp >= 0xd800 && cp <= 0xdfff);
  }
  struct Named { const char *name; uint32_t cp; };
  static constexpr Named names[] = {
      {"amp", 38}, {"lt", 60}, {"gt", 62}, {"quot", 34}, {"apos", 39},
      {"nbsp", 160}, {"ensp", 0x2002}, {"emsp", 0x2003}, {"thinsp", 0x2009},
      {"middot", 0xb7}, {"ndash", 0x2013}, {"mdash", 0x2014}, {"hellip", 0x2026},
      {"laquo", 0xab}, {"raquo", 0xbb}, {"lsquo", 0x2018}, {"rsquo", 0x2019},
      {"ldquo", 0x201c}, {"rdquo", 0x201d}, {"bull", 0x2022}, {"copy", 0xa9},
      {"reg", 0xae}, {"trade", 0x2122}, {"NewLine", 10}, {"Tab", 9}};
  for (const auto &entry : names) if (equal(value, entry.name)) { cp = entry.cp; return true; }
  return false;
}

inline bool classToken(Slice value, const char *wanted, bool &found) {
  // Relevant attributes are decoded too, rather than interpreted as raw substrings.
  const char *p = value.data, *end = p ? p + value.size : p;
  size_t matched = 0;
  bool mismatch = false;
  const size_t length = std::strlen(wanted);
  found = false;
  while (p != end) {
    uint32_t cp;
    if (!(*p == '&' ? entity(p, end, cp) : scalar(p, end, cp))) return false;
    if (cp <= 0x7f && space(char(cp))) {
      if (!mismatch && matched == length) found = true;
      matched = 0; mismatch = false;
    } else {
      if (matched >= length || cp != static_cast<unsigned char>(wanted[matched])) mismatch = true;
      ++matched;
    }
  }
  if (!mismatch && matched == length) found = true;
  return true;
}

inline bool postId(Slice value, uint32_t &id) {
  static constexpr char prefix[] = "vmroficial/";
  constexpr size_t prefixSize = sizeof(prefix) - 1;
  if (value.size <= prefixSize || std::memcmp(value.data, prefix, prefixSize) ||
      value.data[prefixSize] == '0') return false;
  id = 0;
  for (size_t i = prefixSize; i < value.size; ++i) {
    const char c = value.data[i];
    if (c < '0' || c > '9' || id > (UINT32_MAX - unsigned(c - '0')) / 10) return false;
    id = id * 10 + unsigned(c - '0');
  }
  return id != 0;
}

inline bool leap(unsigned year) { return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0); }
inline bool timestamp(Slice s, int64_t &seconds) {
  // Fixed civil bounds, independent of libc time zones, current clock and time_t width.
  if (s.size != 20 && s.size != 25) return false;
  if (s.data[4] != '-' || s.data[7] != '-' || s.data[10] != 'T' ||
      s.data[13] != ':' || s.data[16] != ':' ||
      (s.size == 20 ? s.data[19] != 'Z' : std::memcmp(s.data + 19, "+00:00", 6) != 0)) return false;
  unsigned parts[6] = {};
  static constexpr unsigned starts[] = {0, 5, 8, 11, 14, 17};
  for (unsigned k = 0; k < 6; ++k) {
    for (unsigned i = 0; i < (k == 0 ? 4U : 2U); ++i) {
      const char c = s.data[starts[k] + i];
      if (c < '0' || c > '9') return false;
      parts[k] = parts[k] * 10 + unsigned(c - '0');
    }
  }
  const unsigned year = parts[0], month = parts[1], day = parts[2];
  static constexpr unsigned days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (year < 2013 || year > 2099 || month < 1 || month > 12 || day < 1 ||
      day > days[month - 1] + (month == 2 && leap(year) ? 1U : 0U) ||
      parts[3] > 23 || parts[4] > 59 || parts[5] > 59) return false;
  int64_t total = 0;
  for (unsigned y = 1970; y < year; ++y) total += leap(y) ? 366 : 365;
  for (unsigned m = 1; m < month; ++m) total += days[m - 1] + (m == 2 && leap(year) ? 1U : 0U);
  seconds = ((total + day - 1) * 24 + parts[3]) * 3600 + parts[4] * 60 + parts[5];
  return true;
}

struct Tag {
  Slice name, classes, post, date;
  bool closing = false, empty = false, declaration = false;
};

class Tokenizer {
 public:
  const char *p;
  const char *end;
  Tokenizer(const char *body, size_t length) : p(body), end(body + length) {}

  bool tag(Tag &t) {
    if (p == end || *p++ != '<' || p == end) return false;
    if (end - p >= 3 && !std::memcmp(p, "!--", 3)) {
      p += 3;
      while (end - p >= 3 && std::memcmp(p, "-->", 3)) ++p;
      if (end - p < 3) return false;
      p += 3; t.declaration = true; return true;
    }
    if (*p == '!') {
      const char *start = ++p;
      while (p != end && *p != '>') ++p;
      if (p == end || !nameEqual({start, size_t(p - start)}, "doctype html")) return false;
      ++p; t.declaration = true; return true;
    }
    if (*p == '/') { t.closing = true; ++p; }
    const char *start = p;
    while (p != end && nameChar(*p)) ++p;
    t.name = {start, size_t(p - start)};
    if (!t.name.size || t.name.size > 32 || lower(*start) < 'a' || lower(*start) > 'z') return false;
    unsigned attributes = 0, relevant = 0;
    while (p != end) {
      const bool separated = space(*p);
      while (p != end && space(*p)) ++p;
      if (p == end) return false;
      if (*p == '>') { ++p; return true; }
      if (*p == '/' && !t.closing && end - p >= 2 && p[1] == '>') {
        p += 2; t.empty = true; return true;
      }
      if (t.closing || !separated || ++attributes > 64) return false;
      start = p;
      while (p != end && nameChar(*p)) ++p;
      const Slice name{start, size_t(p - start)};
      if (!name.size) return false;
      unsigned bit = nameEqual(name, "class") ? 1U : nameEqual(name, "data-post") ? 2U :
          nameEqual(name, "datetime") ? 4U : 0U;
      if (bit && (relevant & bit)) return false;
      relevant |= bit;
      const char *afterName = p;
      while (p != end && space(*p)) ++p;
      if (p == end) return false;
      if (*p != '=') {
        if (bit) return false;
        p = afterName; continue;
      }
      ++p;
      while (p != end && space(*p)) ++p;
      if (p == end || (*p != '\'' && *p != '"')) return false;
      const char quote = *p++;
      start = p;
      while (p != end && *p != quote) ++p;
      if (p == end) return false;
      const Slice value{start, size_t(p++ - start)};
      if (bit == 1) t.classes = value;
      if (bit == 2) t.post = value;
      if (bit == 4) t.date = value;
    }
    return false;
  }

  bool raw(Slice name) {
    while (p != end) {
      if (*p == '<' && end - p >= 2 && p[1] == '/') {
        const char *candidate = p;
        p += 2;
        const char *start = p;
        while (p != end && nameChar(*p)) ++p;
        Slice closing{start, size_t(p - start)};
        bool same = closing.size == name.size;
        for (size_t i = 0; same && i < name.size; ++i) same = lower(closing.data[i]) == lower(name.data[i]);
        if (same) {
          p = candidate;
          Tag t;
          return tag(t) && t.closing && !t.declaration;
        }
        p = candidate;
      }
      ++p;
    }
    return false;
  }
};

inline bool voidTag(Slice name) {
  for (const char *s : {"area", "base", "br", "col", "embed", "hr", "img", "input",
                       "link", "meta", "param", "source", "track", "wbr"})
    if (nameEqual(name, s)) return true;
  return false;
}

struct Text {
  char line[128] = {}, tail[96] = {};
  size_t lineSize = 0, tailSize = 0, bytes = 0;
  bool longLine = false, first = true, alert = false, clear = false, conflict = false;
  bool yellow = false, drone = false, red = false, missile = false;

  static bool word(uint32_t cp) {
    return (cp >= 'a' && cp <= 'z') || (cp >= '0' && cp <= '9') ||
        (cp >= 0x400 && cp <= 0x52f);
  }
  bool suffix(const char *s) const {
    const size_t n = std::strlen(s);
    if (n > tailSize || std::memcmp(tail + tailSize - n, s, n)) return false;
    if (n == tailSize) return true;
    const unsigned char before = static_cast<unsigned char>(tail[tailSize - n - 1]);
    return before < 0x80 && !word(before);
  }
  void labels() {
    yellow |= suffix("жовтий рівень"); drone |= suffix("дронова загроза");
    red |= suffix("червоний рівень"); missile |= suffix("ракетна загроза");
  }
  void byte(char c) {
    if (lineSize < sizeof(line) - 1) line[lineSize++] = c;
    else longLine = true;
    if (tailSize == sizeof(tail)) {
      std::memmove(tail, tail + 1, sizeof(tail) - 1); --tailSize;
    }
    tail[tailSize++] = c;
  }
  bool codepoint(uint32_t cp) {
    const size_t width = cp < 0x80 ? 1 : cp < 0x800 ? 2 : cp < 0x10000 ? 3 : 4;
    if (bytes > kTextLimit - width) return false;
    bytes += width;
    if ((cp < 32 && cp != 9 && cp != 10 && cp != 13 && cp != 12) || cp == 127) return false;
    if (cp >= 0x410 && cp <= 0x42f) cp += 32;
    else if (cp == 0x404 || cp == 0x406 || cp == 0x407) cp += 0x50;
    else if (cp == 0x490) cp = 0x491;
    else if (cp >= 'A' && cp <= 'Z') cp += 32;
    // Only known headline decorations are neutral. Quotes and other punctuation remain.
    if (cp == 0xfe0f) return true;
    if (cp == 0x1f534 || cp == 0x1f7e1 || cp == 0x1f7e2 || cp == 0x2757 || cp == '!') cp = ' ';
    if (cp == 160 || (cp >= 0x2000 && cp <= 0x200a) || cp == 0x202f ||
        (cp < 128 && space(char(cp)))) cp = ' ';
    if (!word(cp)) labels();
    if (cp == ' ' && (!lineSize || (!longLine && line[lineSize - 1] == ' ') ||
        (longLine && tailSize && tail[tailSize - 1] == ' '))) return true;
    if (cp < 0x80) byte(char(cp));
    else {
      if (cp >= 0x10000) { byte(char(0xf0 | (cp >> 18))); byte(char(0x80 | ((cp >> 12) & 63))); }
      else if (cp >= 0x800) byte(char(0xe0 | (cp >> 12)));
      else byte(char(0xc0 | (cp >> 6)));
      if (cp >= 0x800) byte(char(0x80 | ((cp >> 6) & 63)));
      byte(char(0x80 | (cp & 63)));
    }
    return true;
  }
  void newline() {
    labels();
    while (lineSize && line[lineSize - 1] == ' ') --lineSize;
    line[lineSize] = 0;
    if (lineSize || longLine) {
      const bool a = !longLine && !std::strcmp(line, "повітряна тривога");
      const bool c = !longLine && !std::strcmp(line, "відбій повітряної тривоги");
      if (first) { alert = a; clear = c; first = false; }
      // All-clear is authoritative only as a standalone headline.
      else if (clear || (alert && (a || c))) conflict = true;
    }
    lineSize = tailSize = 0; longLine = false;
  }
  bool finish(Level &level) {
    newline();
    level = Level::Unknown;
    if (!alert && !clear) return true;
    if (conflict || ((yellow || drone) && (red || missile)) || (clear && (yellow || drone || red || missile))) return false;
    level = clear ? Level::Clear : yellow && drone ? Level::Yellow : red && missile ? Level::Red : Level::Unspecified;
    return true;
  }
};

}  // namespace detail

// Caller-owned storage is available for tasks with exceptionally small stacks.
// No heap, recursion, global mutable storage, clocks, transport or persistence.
class Parser {
 public:
  bool parse(const char *body, size_t length, Page &out) {
    out = Page{};
    reset();
    if (!body || !length || length > detail::kPageLimit || std::memchr(body, 0, length)) return false;
    detail::Tokenizer tokens(body, length);
    while (tokens.p != tokens.end) {
      if (*tokens.p != '<') {
        uint32_t cp;
        if (textDepth_ && !hidden()) {
          if (!(*tokens.p == '&' ? detail::entity(tokens.p, tokens.end, cp) :
              detail::scalar(tokens.p, tokens.end, cp)) || !text_.codepoint(cp)) return false;
        } else {
          if ((!seenHtml_ || closedHtml_) && !detail::space(*tokens.p)) return false;
          ++tokens.p;
        }
        continue;
      }
      detail::Tag tag;
      if (!tokens.tag(tag)) return false;
      if (tag.declaration) continue;
      if (tag.closing) {
        if (!depth_) return false;
        const Frame &frame = stack_[depth_ - 1];
        if (!sameName(body, frame, tag.name)) return false;
        if (depth_ == textDepth_) {
          textValid_ = text_.finish(level_);
          textDepth_ = 0;
        } else if (textDepth_ && !hidden() && block(tag.name)) text_.newline();
        if (depth_ == postDepth_ && !finishPost()) return false;
        if (depth_ == historyDepth_) { historyDepth_ = 0; closedHistory_ = true; }
        if (depth_ == bodyDepth_) {
          if (!closedHistory_) return false;
          bodyDepth_ = 0; closedBody_ = true;
        }
        if (detail::nameEqual(tag.name, "html")) {
          if (depth_ != 1 || !closedBody_) return false;
          closedHtml_ = true;
        }
        if (depth_ == svgDepth_) svgDepth_ = 0;
        --depth_;
        continue;
      }
      if (detail::nameEqual(tag.name, "html")) {
        if (depth_ || seenHtml_ || tag.empty) return false;
        seenHtml_ = true;
      } else if (!seenHtml_ || closedHtml_ || closedBody_) return false;
      if (detail::nameEqual(tag.name, "body")) {
        if (depth_ != 1 || seenBody_ || tag.empty) return false;
        seenBody_ = true; bodyDepth_ = depth_ + 1;
      }
      bool message, text, forwarded, history, historyScript;
      if (!detail::classToken(tag.classes, "tgme_widget_message", message) ||
          !detail::classToken(tag.classes, "tgme_widget_message_text", text) ||
          !detail::classToken(tag.classes, "tgme_widget_message_forwarded_from", forwarded) ||
          !detail::classToken(tag.classes, "tgme_channel_history", history) ||
          !detail::classToken(tag.classes, "js-message_history", historyScript)) return false;
      const bool parentHidden = hidden();
      const bool excluded = parentHidden || exclude(tag.name);
      if (history && !excluded) {
        if (!bodyDepth_ || historyDepth_ || closedHistory_ || !historyScript ||
            !detail::nameEqual(tag.name, "section") || tag.empty) return false;
        historyDepth_ = depth_ + 1;
      }
      if (message && !excluded) {
        if (!historyDepth_ || postDepth_ || !detail::nameEqual(tag.name, "div") || tag.empty ||
            page_.posts >= detail::kPostLimit || !detail::postId(tag.post, id_)) return false;
        for (unsigned i = 0; i < page_.posts; ++i) if (ids_[i] == id_) return false;
        postDepth_ = depth_ + 1;
        hasTime_ = hasText_ = forwarded_ = false; textValid_ = true;
        time_ = 0; level_ = Level::Unknown;
      }
      if (postDepth_ && forwarded && !excluded) forwarded_ = true;
      if (postDepth_ && text && !excluded) {
        if (hasText_ || !detail::nameEqual(tag.name, "div") || tag.empty) return false;
        hasText_ = true; textDepth_ = depth_ + 1; text_ = detail::Text{};
      }
      if (postDepth_ && !textDepth_ && !excluded && detail::nameEqual(tag.name, "time")) {
        if (hasTime_ || !detail::timestamp(tag.date, time_) || tag.empty) return false;
        hasTime_ = true;
      }
      if (textDepth_ && !parentHidden && (block(tag.name) || exclude(tag.name))) text_.newline();
      if (detail::nameEqual(tag.name, "script") || detail::nameEqual(tag.name, "style")) {
        if (tag.empty || !tokens.raw(tag.name)) return false;
        continue;
      }
      if (detail::voidTag(tag.name)) continue;
      // HTML self-closing syntax is only valid for void elements (and SVG children).
      if (tag.empty) {
        if (!svgDepth_) return false;
        continue;
      }
      if (depth_ == detail::kDepthLimit) return false;
      stack_[depth_++] = {uint32_t(tag.name.data - body), uint8_t(tag.name.size), uint8_t(excluded ? 1 : 0)};
      if (detail::nameEqual(tag.name, "svg")) svgDepth_ = depth_;
    }
    if (depth_ || postDepth_ || !closedHtml_ || !closedBody_ || !closedHistory_ || !page_.posts) return false;
    out = page_;
    return true;
  }

 private:
  struct Frame { uint32_t offset; uint8_t length, hidden; };
  Frame stack_[detail::kDepthLimit] = {};
  uint32_t ids_[detail::kPostLimit] = {};
  detail::Text text_;
  Page page_;
  unsigned depth_ = 0, postDepth_ = 0, textDepth_ = 0, svgDepth_ = 0;
  unsigned bodyDepth_ = 0, historyDepth_ = 0;
  uint32_t id_ = 0;
  int64_t time_ = 0;
  Level level_ = Level::Unknown;
  bool hasTime_ = false, hasText_ = false, forwarded_ = false, textValid_ = true;
  bool seenHtml_ = false, closedHtml_ = false, seenBody_ = false, closedBody_ = false, closedHistory_ = false;

  void reset() {
    page_ = Page{};
    depth_ = postDepth_ = textDepth_ = svgDepth_ = 0;
    bodyDepth_ = historyDepth_ = 0;
    id_ = 0; time_ = 0; level_ = Level::Unknown;
    hasTime_ = hasText_ = forwarded_ = false;
    textValid_ = true;
    seenHtml_ = closedHtml_ = seenBody_ = closedBody_ = closedHistory_ = false;
  }
  bool hidden() const { return depth_ && stack_[depth_ - 1].hidden; }
  static bool block(detail::Slice name) {
    return detail::nameEqual(name, "br") || detail::nameEqual(name, "div") || detail::nameEqual(name, "p");
  }
  static bool exclude(detail::Slice name) {
    return detail::nameEqual(name, "blockquote") || detail::nameEqual(name, "script") ||
        detail::nameEqual(name, "style") || detail::nameEqual(name, "template") ||
        detail::nameEqual(name, "noscript") || detail::nameEqual(name, "iframe") ||
        detail::nameEqual(name, "textarea");
  }
  static bool sameName(const char *body, const Frame &frame, detail::Slice name) {
    if (frame.length != name.size) return false;
    for (size_t i = 0; i < name.size; ++i)
      if (detail::lower(body[frame.offset + i]) != detail::lower(name.data[i])) return false;
    return true;
  }
  bool finishPost() {
    if (!hasTime_ || textDepth_ || (!forwarded_ && !textValid_)) return false;
    ids_[page_.posts++] = id_;
    if (id_ > page_.newestId) { page_.newestId = id_; page_.newestTime = time_; }
    if (!page_.oldestId || id_ < page_.oldestId) page_.oldestId = id_;
    if (!forwarded_ && level_ != Level::Unknown && id_ > page_.eventId) {
      page_.eventId = id_; page_.eventTime = time_; page_.level = level_;
    }
    postDepth_ = 0;
    return true;
  }
};

static_assert(sizeof(Parser) <= 1536, "Keep parser scratch below the firmware task stack budget");

inline bool parsePage(const char *body, size_t length, Page &out) {
  Parser parser;
  return parser.parse(body, length, out);
}

}  // namespace telegram
}  // namespace air_alert
