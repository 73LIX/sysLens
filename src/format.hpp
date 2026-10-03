#pragma once
#include <string>
#include <vector>

// ---- structured model output --------------------------------------------------------------

struct Item {
  std::string text;
  bool command = false;  // a shell command line (rendered as "$ cmd")
};

struct Section {
  std::string label;  // empty = unlabeled body
  std::vector<Item> items;
  bool numbered = false;
};

struct LabelSpec {
  std::string name;  // e.g. "WHAT HAPPENED"
  bool numbered = false;
  bool optional = false;  // may be absent from a valid reply (e.g. NOTE)
};

struct Analysis {
  bool ok = false;
  std::string error;
  std::string backend;
  std::vector<Section> sections;
};

// Cleans raw model text (markdown, preamble, closing chatter) and splits it into the labeled
// sections given by `specs`. Text before the first label and chatty closers are discarded.
std::vector<Section> parse_sections(const std::string& raw, const std::vector<LabelSpec>& specs);

// ---- layout (shared by the CLI and the TUI) ------------------------------------------------

enum class LineKind { Heading, Body, Command, Blank, Error };

struct DocLine {
  std::string text;
  LineKind kind = LineKind::Body;
};

// Word-wraps sections into display lines no wider than `width` columns.
std::vector<DocLine> layout_sections(const std::vector<Section>& sections, int width);
std::vector<DocLine> layout_error(const std::string& message, int width);

std::vector<std::string> wrap_text(const std::string& text, int width);
int display_width(const std::string& s);
std::string clip_width(const std::string& s, int width);  // ellipsis when too wide
int term_width();                                         // clamped to [60, 110]

// Prints a laid-out document to stdout with ANSI colors when attached to a terminal.
void print_doc(const std::vector<DocLine>& doc, const std::string& indent = "");
