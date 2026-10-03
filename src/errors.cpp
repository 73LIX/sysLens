#include "errors.hpp"

#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <iostream>
#include <set>
#include <sstream>

#include "analyze.hpp"
#include "format.hpp"

namespace {

std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
  return s;
}

bool has_any(const std::string& s, std::initializer_list<const char*> needles) {
  for (auto n : needles)
    if (s.find(n) != std::string::npos) return true;
  return false;
}

// strip ANSI escape sequences so colored compiler output is classified correctly
std::string strip_ansi(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '\x1b' && i + 1 < s.size() && s[i + 1] == '[') {
      i += 2;
      while (i < s.size() && !(s[i] >= '@' && s[i] <= '~')) ++i;
      continue;
    }
    out += s[i];
  }
  return out;
}

bool use_color() { return isatty(STDOUT_FILENO); }
const char* col(const char* code) { return use_color() ? code : ""; }
const char* RST() { return col("\x1b[0m"); }
const char* BOLD() { return col("\x1b[1m"); }
const char* RED() { return col("\x1b[1;31m"); }
const char* ORANGE() { return col("\x1b[38;5;208m"); }
const char* YEL() { return col("\x1b[33m"); }
const char* DIM() { return col("\x1b[2m"); }
const char* CYAN() { return col("\x1b[1;36m"); }

std::string trim(const std::string& s) {
  size_t a = s.find_first_not_of(" \t\r\n");
  if (a == std::string::npos) return {};
  size_t b = s.find_last_not_of(" \t\r\n");
  return s.substr(a, b - a + 1);
}

std::vector<std::string> split_lines(const std::string& s) {
  std::vector<std::string> v;
  std::istringstream in(s);
  std::string l;
  while (std::getline(in, l)) v.push_back(l);
  return v;
}

std::string rule(const std::string& title, int width) {
  std::string left = "── " + title + " ";
  int pad = width - display_width(left);
  std::string out = left;
  for (int i = 0; i < pad; ++i) out += "─";
  return out;
}

}  // namespace

std::vector<Finding> classify_log(const std::string& log) {
  std::vector<Finding> out;
  std::set<std::string> seen;
  std::istringstream in(log);
  std::string raw;
  int n = 0;
  while (std::getline(in, raw)) {
    ++n;
    std::string line = trim(strip_ansi(raw));
    if (line.empty()) continue;
    std::string l = lower(line);

    // Summary lines like "0 errors" / "no errors" are not findings.
    if (has_any(l, {"0 errors", "no errors", "0 warnings", "no warnings", "error-free"})) continue;

    Severity sev;
    if (has_any(l, {"panic", "segmentation fault", "segfault", "core dumped", "fatal", "out of memory",
                    "oom-kill", "killed", "stack overflow", "abort", "internal compiler error",
                    "cannot allocate memory", "bus error", "illegal instruction", "sigsegv",
                    "sigabrt", "kernel bug"})) {
      sev = Severity::Critical;
    } else if (has_any(l, {"error", "failed", "failure", "cannot ", "can't ", "could not", "couldn't",
                           "not found", "undefined reference", "no such file", "permission denied",
                           "enoent", "eacces", "eaddrinuse", "traceback", "exception",
                           "unresolved", "denied", "refused", "timed out", "conflict",
                           "unable to", "invalid", "missing"})) {
      sev = Severity::Error;
    } else if (has_any(l, {"warning", "warn[", "warn ", "deprecated", "deprecation"})) {
      sev = Severity::Warning;
    } else {
      continue;
    }
    // dedupe by normalized text (digits collapsed) so repeated lines don't flood the report
    std::string key = l;
    for (auto& ch : key)
      if (ch >= '0' && ch <= '9') ch = '#';
    if (!seen.insert(key).second) continue;
    out.push_back({sev, n, line});
  }
  return out;
}

std::string shell_join(const std::vector<std::string>& argv) {
  std::string out;
  for (size_t i = 0; i < argv.size(); ++i) {
    if (i) out += ' ';
    const std::string& a = argv[i];
    bool plain = !a.empty() && a.find_first_not_of(
                                   "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_@%+=:,./-") ==
                                   std::string::npos;
    if (plain) {
      out += a;
    } else {
      out += '\'';
      for (char c : a) {
        if (c == '\'')
          out += "'\\''";
        else
          out += c;
      }
      out += '\'';
    }
  }
  return out;
}

int run_and_capture(const std::string& shell_cmd, std::string& captured) {
  std::string cmd = "{ " + shell_cmd + "\n} 2>&1";
  FILE* p = popen(cmd.c_str(), "r");
  if (!p) return 127;
  char buf[4096];
  size_t n;
  while ((n = fread(buf, 1, sizeof buf, p)) > 0) {
    fwrite(buf, 1, n, stdout);
    fflush(stdout);
    captured.append(buf, n);
    if (captured.size() > (4u << 20)) captured.erase(0, captured.size() - (2u << 20));  // keep tail
  }
  int st = pclose(p);
  return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + (WIFSIGNALED(st) ? WTERMSIG(st) : 0);
}

static void print_group(const std::vector<Finding>& all, Severity s, const char* name,
                        const char* color, size_t max_show, int width) {
  std::vector<const Finding*> g;
  for (auto& f : all)
    if (f.sev == s) g.push_back(&f);
  if (g.empty()) return;
  std::cout << color << "■ " << name << RST() << DIM() << "  " << g.size() << RST() << "\n";
  for (size_t i = 0; i < g.size() && i < max_show; ++i) {
    std::string tag = "L" + std::to_string(g[i]->line_no);
    std::string pad(std::max<size_t>(1, 6 - tag.size()), ' ');
    std::cout << "  " << DIM() << tag << RST() << pad << clip_width(g[i]->text, width - 10) << "\n";
  }
  if (g.size() > max_show)
    std::cout << "  " << DIM() << "… " << (g.size() - max_show) << " more" << RST() << "\n";
  std::cout << "\n";
}

int report_errors(const std::string& log, const std::string& label, const LlmConfig& cfg,
                  bool use_ai) {
  const int W = term_width();
  auto findings = classify_log(log);
  size_t crit = 0, err = 0, warn = 0;
  for (auto& f : findings) {
    if (f.sev == Severity::Critical) ++crit;
    else if (f.sev == Severity::Error) ++err;
    else ++warn;
  }

  std::cout << "\n" << CYAN() << rule("sysLens · " + clip_width(label, W - 16), W) << RST() << "\n\n";

  if (findings.empty()) {
    std::cout << "No errors or warnings detected.\n";
    return 0;
  }

  std::cout << BOLD() << crit << " critical" << RST() << " · " << BOLD() << err << " error"
            << (err == 1 ? "" : "s") << RST() << " · " << BOLD() << warn << " warning"
            << (warn == 1 ? "" : "s") << RST() << "\n\n";

  print_group(findings, Severity::Critical, "CRITICAL", RED(), 6, W);
  print_group(findings, Severity::Error, "ERRORS", ORANGE(), 8, W);
  print_group(findings, Severity::Warning, "WARNINGS", YEL(), 4, W);

  if (crit + err == 0) {
    std::cout << DIM() << "Only warnings; nothing to fix urgently." << RST() << "\n";
    return 0;
  }

  // The root error is the first critical line, else the first error (later ones are usually
  // fallout from it).
  const Finding* root = nullptr;
  for (auto& f : findings)
    if (f.sev == Severity::Critical) { root = &f; break; }
  if (!root)
    for (auto& f : findings)
      if (f.sev == Severity::Error) { root = &f; break; }

  std::cout << CYAN() << rule("Root error · L" + std::to_string(root->line_no), W) << RST() << "\n";
  for (auto& l : wrap_text(root->text, W - 2)) std::cout << "  " << l << "\n";
  std::cout << "\n";

  if (!use_ai) return 0;

  // ---- build a focused prompt: the root error with surrounding lines, plus the other errors
  auto lines = split_lines(log);
  std::ostringstream ctx;
  int lo = std::max(1, root->line_no - 3), hi = std::min<int>(static_cast<int>(lines.size()), root->line_no + 5);
  for (int i = lo; i <= hi; ++i) {
    std::string t = trim(strip_ansi(lines[i - 1]));
    if (t.empty()) continue;
    ctx << (i == root->line_no ? ">> " : "   ") << clip_width(t, 220) << "\n";
  }
  std::ostringstream others;
  int k = 0;
  for (auto& f : findings) {
    if (&f == root || f.sev == Severity::Warning) continue;
    if (k++ >= 5) break;
    others << "- " << clip_width(f.text, 200) << "\n";
  }

  static const std::string instructions =
      "You diagnose one failed terminal command from its log. Plain text, no markdown.\n\n"
      "Reply with these fields: WHAT HAPPENED (one sentence), CAUSE (one sentence), and FIX (one "
      "to three numbered steps, one per line; a shell command goes on its own line starting "
      "with \"$ \" directly after its step).\n\n"
      "Explain only the line marked >> (the root error); later errors are usually side effects. "
      "Use only what the log shows: never invent file names, packages or flags. If the log is "
      "not enough, write \"Unclear from the log\" as the CAUSE and make a FIX step a command "
      "that would reveal more.\n\n"
      "Example reply for a different log (>> ModuleNotFoundError: No module named 'requests'):\n"
      "WHAT HAPPENED: Python could not import the requests module.\n"
      "CAUSE: The package isn't installed in the active environment.\n"
      "FIX:\n"
      "1. Install it in the active environment.\n"
      "$ pip install requests";

  std::ostringstream user;
  user << "Command: " << label << "\n\nLog excerpt (>> marks the root error):\n" << ctx.str();
  if (k > 0) user << "\nOther errors in the same run:\n" << others.str();

  bool tty = isatty(STDOUT_FILENO);
  if (tty) std::cout << DIM() << "Analyzing…" << RST() << std::flush;
  Analysis a = ask_structured(cfg, instructions, user.str(),
                              {{"WHAT HAPPENED", false, false}, {"CAUSE", false, false}, {"FIX", true, false}});
  if (tty) std::cout << "\r\x1b[2K";

  std::string via = a.backend.empty() ? "" : " · " + a.backend;
  std::cout << CYAN() << rule("Analysis" + via, W) << RST() << "\n";
  if (!a.ok) {
    print_doc(layout_error("AI analysis unavailable: " + a.error, W - 2));
    if (a.error.find("API failed") != std::string::npos || a.error.find("API key") != std::string::npos ||
        a.error.find("unreachable") != std::string::npos)
      std::cout << DIM() << "  Set GEMINI_API_KEY, or run llama-server with a Gemma GGUF (see README)."
                << RST() << "\n";
    return 2;
  }
  print_doc(layout_sections(a.sections, W - 2));
  std::cout << "\n";
  return 0;
}
