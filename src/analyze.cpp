#include "analyze.hpp"

#include <algorithm>

#include "sysinfo.hpp"

namespace {

bool valid(const std::vector<Section>& secs, const std::vector<LabelSpec>& specs) {
  for (const auto& sp : specs) {
    if (sp.optional) continue;
    bool found = std::any_of(secs.begin(), secs.end(),
                             [&](const Section& s) { return s.label == sp.name && !s.items.empty(); });
    if (!found) return false;
  }
  return true;
}

// A reply cut off by the token limit may end mid-sentence: drop that last fragment.
void drop_dangling(std::vector<Section>& secs) {
  if (secs.empty() || secs.back().items.empty()) return;
  auto& items = secs.back().items;
  const std::string& t = items.back().text;
  if (items.back().command || t.empty()) return;
  char c = t.back();
  if (c == '.' || c == '!' || c == '?' || c == ')' || c == ':') return;
  items.pop_back();
  if (items.empty()) secs.pop_back();
}

}  // namespace

Analysis ask_structured(const LlmConfig& cfg, const std::string& instructions,
                        const std::string& input, const std::vector<LabelSpec>& specs) {
  LlmConfig c = cfg;
  c.max_tokens = 1024;  // reasoning models spend part of this on thinking; the answer stays short
  c.temperature = 0.1;

  const std::string tail = "\n\nReply now. Your reply must begin with \"" + specs[0].name + ":\".";
  Analysis a;
  LlmResult r;
  for (int attempt = 0; attempt < 2; ++attempt) {
    std::string extra = attempt == 0 ? "" : "\n\nYour previous reply did not follow the format. "
                                             "Reply with ONLY the labeled lines, like the example.";
    r = llm_complete(c, instructions, input + tail + extra);
    a.backend = r.backend_used;
    if (!r.ok) {
      a.error = r.error;
      return a;
    }
    auto secs = parse_sections(r.text, specs);
    if (r.truncated) drop_dangling(secs);
    if (valid(secs, specs)) {
      a.ok = true;
      a.sections = std::move(secs);
      return a;
    }
  }
  a.error = "The model's reply didn't follow the expected format. Try again, or pick another model "
            "with SYSLENS_MODEL (e.g. gemma-3-4b-it).";
  return a;
}

Analysis analyze_process(const LlmConfig& cfg, int pid) {
  if (!proc_exists(pid)) {
    Analysis a;
    a.error = "No process with PID " + std::to_string(pid) + " is running.";
    return a;
  }
  static const std::string instructions =
      "Provide a direct answer without thinking step-by-step. \n\n"
      "You describe one Linux process using only the facts below. Plain text, no markdown.\n\n"
      "Reply with these fields, one line each, each a single short sentence: WHAT (what the "
      "program is), DOING (what it is doing right now; name the script, project path or port if "
      "the facts show one) and, only if something looks unusual or risky, NOTE. Never guess "
      "beyond the facts.\n\n"
      "Example reply for a different process:\n"
      "WHAT: Node.js runtime running a Vite development server.\n"
      "DOING: Serving the project in /home/me/app on port 5173.";
  return ask_structured(cfg, instructions, "Facts about the process:\n" + describe_process(pid),
                        {{"WHAT", false, false}, {"DOING", false, false}, {"NOTE", false, true}});
}

Analysis analyze_port(const LlmConfig& cfg, int port) {
  PortFacts f = inspect_port(port);
  Analysis a;
  if (!f.found) {
    a.error = "Nothing is using port " + std::to_string(port) + ".";
    return a;
  }
  if (!f.owner_known) {
    a.error = "Something is on port " + std::to_string(port) +
              ", but its owner isn't visible. Run sysLens with sudo to see which process it is.";
    return a;
  }
  static const std::string instructions =
      "Provide a direct answer without thinking step-by-step. \n\n"
      "You explain which application uses a network port, using only the facts below. Plain "
      "text, no markdown.\n\n"
      "Reply with these fields, one line each, each a single short sentence: APP (the program and "
      "PID holding the port), WHY (the most likely purpose of that program on this port; say "
      "\"typically\" when relying on port conventions) and, only for a real concern such as a "
      "database reachable from all interfaces, NOTE.\n\n"
      "Example reply for a different port:\n"
      "APP: PostgreSQL server (PID 812).\n"
      "WHY: Typically the default database port; it serves local applications.";
  return ask_structured(cfg, instructions,
                        "Facts about port " + std::to_string(port) + ":\n" + f.text,
                        {{"APP", false, false}, {"WHY", false, false}, {"NOTE", false, true}});
}
