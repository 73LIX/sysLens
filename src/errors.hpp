#pragma once
#include <string>
#include <vector>

#include "llm.hpp"

enum class Severity { Critical, Error, Warning };

struct Finding {
  Severity sev;
  int line_no;
  std::string text;
};

// Local, deterministic triage of a log into critical / error / warning lines.
std::vector<Finding> classify_log(const std::string& log);

// Runs a shell command, streaming its output to the terminal while capturing it
// (stdout+stderr merged). Returns the command's exit status.
int run_and_capture(const std::string& shell_cmd, std::string& captured);

// Quote argv into one shell-safe string.
std::string shell_join(const std::vector<std::string>& argv);

// Prints categorized findings and (if use_ai) an AI explanation with next steps.
// Returns 0 on success.
int report_errors(const std::string& log, const std::string& label, const LlmConfig& cfg,
                  bool use_ai);
