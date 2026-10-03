#pragma once
#include "format.hpp"
#include "llm.hpp"

// Gather facts from /proc and ask the model to explain a process / port.
// Returns ok=false with a plain-language `error` (and makes NO model call) when the process
// doesn't exist, nothing is listening on the port, or the port's owner can't be seen.
Analysis analyze_process(const LlmConfig& cfg, int pid);
Analysis analyze_port(const LlmConfig& cfg, int port);

// Asks the model and returns validated, structured sections. A reply that doesn't contain every
// required label is retried once; if it still doesn't comply the result is an error, never raw text.
Analysis ask_structured(const LlmConfig& cfg, const std::string& instructions,
                        const std::string& input, const std::vector<LabelSpec>& specs);
