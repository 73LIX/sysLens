#pragma once
#include <string>

struct LlmConfig {
  // "auto": use the Gemma API if a key is set, fall back to local llama-server on failure.
  // "api" or "local" force one backend.
  std::string backend = "auto";
  std::string api_key;
  std::string model = "gemma-3n-e2b-it";  // ~2B effective params; gemma-3-1b-it is smaller
  std::string api_base = "https://generativelanguage.googleapis.com/v1beta";
  std::string local_url = "http://127.0.0.1:8080";
  std::string local_gguf;  // optional: if set and server is down, start llama-server with it
  int max_tokens = 700;
  double temperature = 0.1;
  int timeout_s = 90;

  static LlmConfig from_env();
};

struct LlmResult {
  bool ok = false;
  std::string text;
  bool truncated = false;  // hit the token limit
  std::string error;
  std::string backend_used;
};

// Sends a single prompt (system text is folded into the user turn because Gemma
// models reject separate system instructions on the API).
LlmResult llm_complete(const LlmConfig& cfg, const std::string& system, const std::string& user);
