#include "format.hpp"

#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <iostream>
#include <sstream>

namespace {

std::string trim(const std::string& s) {
  size_t a = s.find_first_not_of(" \t\r\n");
  if (a == std::string::npos) return {};
  size_t b = s.find_last_not_of(" \t\r\n");
  return s.substr(a, b - a + 1);
}

std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
  return s;
}

void replace_all(std::string& s, const std::string& from, const std::string& to) {
  size_t pos = 0;
  while ((pos = s.find(from, pos)) != std::string::npos) {
    s.replace(pos, from.size(), to);
    pos += to.size();
  }
}

// byte offset after the first n UTF-8 code points
size_t utf8_prefix(const std::string& s, int n) {
  size_t i = 0;
  int cp = 0;
  while (i < s.size() && cp < n) {
    ++i;
    while (i < s.size() && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) ++i;
    ++cp;
  }
  return i;
}

std::string strip_markdown(const std::string& raw) {
  std::ostringstream out;
  std::istringstream in(raw);
  std::string line;
  while (std::getline(in, line)) {
    std::string t = trim(line);
    if (t.rfind("```", 0) == 0) continue;  // drop code fence lines
    replace_all(t, "**", "");
    replace_all(t, "__", "");
    replace_all(t, "`", "");
    size_t h = 0;
    while (h < t.size() && t[h] == '#') ++h;
    if (h > 0) t = trim(t.substr(h));
    if (t.rfind("* ", 0) == 0) t = "- " + t.substr(2);
    out << t << "\n";
  }
  return out.str();
}

bool is_chatter(const std::string& lt) {
  static const char* starts[] = {"okay",     "ok,",       "sure",      "certainly", "here's",
                                 "here is",  "here are",  "let me",    "i hope",    "hope this",
                                 "feel free", "i'm happy", "i can ",    "based on the", "this analysis",
                                 "analysis:", "disclaimer", "important:", "note: i "};
  for (auto s : starts)
    if (lt.rfind(s, 0) == 0) return true;
  return false;
}

bool is_closer(const std::string& lt) {
  static const char* starts[] = {"let me know", "i hope", "hope this", "feel free", "if you need",
                                 "if you'd like", "good luck", "happy "};
  for (auto s : starts)
    if (lt.rfind(s, 0) == 0) return true;
  return false;
}

std::string shorten(const std::string& s, size_t max_chars) {
  if (s.size() <= max_chars) return s;
  size_t cut = s.rfind(' ', max_chars);
  if (cut == std::string::npos || cut < max_chars / 2) cut = max_chars;
  return s.substr(0, cut) + "…";
}

void add_line(Section& sec, const std::string& t) {
  if (t[0] == '$') {
    std::string cmd = trim(t.substr(1));
    if (!cmd.empty()) sec.items.push_back({cmd, true});
    return;
  }
  std::string body = t;
  bool marker = false;
  size_t d = 0;
  while (d < body.size() && std::isdigit(static_cast<unsigned char>(body[d]))) ++d;
  if (d > 0 && d < body.size() && (body[d] == '.' || body[d] == ')')) {
    body = trim(body.substr(d + 1));
    marker = true;
  } else if (body.rfind("- ", 0) == 0) {
    body = trim(body.substr(2));
    marker = true;
  } else if (body.rfind("• ", 0) == 0) {
    body = trim(body.substr(std::string("• ").size()));
    marker = true;
  }
  if (body.empty()) return;
  if (!marker && !sec.items.empty() && !sec.items.back().command) {
    sec.items.back().text += " " + body;  // continuation of the previous sentence
  } else {
    sec.items.push_back({body, false});
  }
}

}  // namespace

int display_width(const std::string& s) {
  int n = 0;
  for (unsigned char c : s)
    if ((c & 0xC0) != 0x80) ++n;
  return n;
}

std::string clip_width(const std::string& s, int width) {
  if (display_width(s) <= width) return s;
  return s.substr(0, utf8_prefix(s, std::max(1, width - 1))) + "…";
}

int term_width() {
  winsize ws{};
  int w = 100;
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) w = ws.ws_col;
  return std::clamp(w, 60, 110);
}

std::vector<std::string> wrap_text(const std::string& text, int width) {
  width = std::max(8, width);
  std::vector<std::string> lines;
  std::istringstream in(text);
  std::string word, cur;
  while (in >> word) {
    while (display_width(word) > width) {  // hard-split very long tokens
      if (!cur.empty()) {
        lines.push_back(cur);
        cur.clear();
      }
      size_t k = utf8_prefix(word, width);
      lines.push_back(word.substr(0, k));
      word = word.substr(k);
    }
    if (cur.empty()) {
      cur = word;
    } else if (display_width(cur) + 1 + display_width(word) <= width) {
      cur += " " + word;
    } else {
      lines.push_back(cur);
      cur = word;
    }
  }
  if (!cur.empty()) lines.push_back(cur);
  return lines;
}

// Text that is clearly an echoed instruction/template rather than an answer.
static bool is_placeholder(const std::string& text) {
  std::string t = lower(trim(text));
  if (t.empty()) return true;
  if (t.front() == '<' && t.back() == '>') return true;
  if (t.find("<") != std::string::npos && t.find(">") != std::string::npos &&
      (t.find("max ") != std::string::npos || t.find("words") != std::string::npos))
    return true;
  size_t m = t.find("max ");
  if (m != std::string::npos && m + 4 < t.size() && std::isdigit(static_cast<unsigned char>(t[m + 4])) &&
      t.find("word", m) != std::string::npos)
    return true;
  static const char* echoes[] = {"one sentence", "otherwise omit", "otherwise leave", "no greeting",
                                 "no preamble",  "no markdown",    "constraints",    "short imperative",
                                 "optional second step", "base everything on", "explain the line marked"};
  for (auto e : echoes)
    if (t.find(e) != std::string::npos) return true;
  return false;
}

std::vector<Section> parse_sections(const std::string& raw, const std::vector<LabelSpec>& specs) {
  std::vector<LabelSpec> sorted = specs;  // match longer labels first ("WHAT HAPPENED" before "WHAT")
  std::sort(sorted.begin(), sorted.end(),
            [](const LabelSpec& a, const LabelSpec& b) { return a.name.size() > b.name.size(); });

  std::vector<Section> out;
  bool closed = false;
  int cur = -1;

  std::istringstream in(strip_markdown(raw));
  std::string line;
  while (std::getline(in, line)) {
    std::string t = trim(line);
    if (t.empty()) continue;
    std::string lt = lower(t);

    size_t p = 0;
    while (p < lt.size() && !std::isalnum(static_cast<unsigned char>(lt[p])) && lt[p] != '$') ++p;

    bool matched = false;
    for (const auto& sp : sorted) {
      std::string name = lower(sp.name);
      if (lt.compare(p, name.size(), name) != 0) continue;
      size_t q = p + name.size();
      while (q < lt.size() && lt[q] == ' ') ++q;
      if (q < lt.size() && lt[q] != ':' && lt[q] != '-') continue;
      std::string rest = q < lt.size() ? trim(t.substr(q + 1)) : "";
      // A label seen twice means the model drafted first and answered last: the final
      // answer wins, so throw away everything collected so far.
      for (const auto& s : out)
        if (s.label == sp.name) {
          out.clear();
          break;
        }
      out.push_back({sp.name, {}, sp.numbered});
      cur = static_cast<int>(out.size()) - 1;
      closed = false;
      if (!rest.empty() && !is_placeholder(rest)) add_line(out[cur], rest);
      matched = true;
      break;
    }
    if (matched) continue;

    if (closed || cur < 0) continue;  // text before the first label is never part of the answer
    if (is_closer(lt)) {
      closed = true;
      continue;
    }
    if (is_chatter(lt) || is_placeholder(t)) continue;
    add_line(out[cur], t);
  }

  for (auto& s : out) {
    std::vector<Item> kept;
    int texts = 0;
    for (auto& it : s.items) {
      if (is_placeholder(it.text)) continue;
      if (!it.command) {
        if (!s.numbered && texts >= 1) continue;  // plain fields are a single statement
        if (s.numbered && texts >= 3) continue;   // at most three steps
        ++texts;
      } else if (texts == 0) {
        continue;  // a command must belong to a step
      }
      it.text = shorten(it.text, it.command ? 160 : 200);
      kept.push_back(it);
    }
    s.items = std::move(kept);
  }
  out.erase(std::remove_if(out.begin(), out.end(), [](const Section& s) { return s.items.empty(); }),
            out.end());
  return out;
}

std::vector<DocLine> layout_sections(const std::vector<Section>& sections, int width) {
  std::vector<DocLine> doc;
  for (size_t si = 0; si < sections.size(); ++si) {
    const Section& s = sections[si];
    if (si > 0) doc.push_back({"", LineKind::Blank});
    if (!s.label.empty()) doc.push_back({s.label, LineKind::Heading});
    int n = 0;
    for (const auto& it : s.items) {
      if (it.command) {
        std::string pad(s.numbered ? 5 : 2, ' ');
        auto ls = wrap_text("$ " + it.text, width - static_cast<int>(pad.size()));
        for (auto& l : ls) doc.push_back({pad + l, LineKind::Command});
        continue;
      }
      std::string prefix = s.numbered ? std::to_string(++n) + ". " : "  ";
      if (s.numbered) prefix = " " + prefix;  // " 1. "
      std::string hang(prefix.size(), ' ');
      auto ls = wrap_text(it.text, width - static_cast<int>(prefix.size()));
      for (size_t i = 0; i < ls.size(); ++i)
        doc.push_back({(i == 0 ? prefix : hang) + ls[i], LineKind::Body});
    }
  }
  return doc;
}

std::vector<DocLine> layout_error(const std::string& message, int width) {
  std::vector<DocLine> doc;
  for (auto& l : wrap_text(message, width)) doc.push_back({"  " + l, LineKind::Error});
  return doc;
}

void print_doc(const std::vector<DocLine>& doc, const std::string& indent) {
  bool color = isatty(STDOUT_FILENO);
  for (const auto& l : doc) {
    const char* pre = "";
    const char* post = color ? "\x1b[0m" : "";
    if (color) {
      switch (l.kind) {
        case LineKind::Heading: pre = "\x1b[1;36m"; break;
        case LineKind::Command: pre = "\x1b[32m"; break;
        case LineKind::Error: pre = "\x1b[31m"; break;
        default: post = ""; break;
      }
    }
    if (l.kind == LineKind::Blank)
      std::cout << "\n";
    else
      std::cout << indent << pre << l.text << post << "\n";
  }
}
