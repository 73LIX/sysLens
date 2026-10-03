#include "llm.hpp"

#include <curl/curl.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <nlohmann/json.hpp>
#include <thread>

using json = nlohmann::json;

static const char* env(const char* k) {
  const char* v = std::getenv(k);
  return (v && *v) ? v : nullptr;
}

LlmConfig LlmConfig::from_env() {
  LlmConfig c;
  if (auto v = env("SYSLENS_BACKEND")) c.backend = v;
  if (auto v = env("GEMINI_API_KEY")) c.api_key = v;
  else if (auto v2 = env("GOOGLE_API_KEY")) c.api_key = v2;
  else if (auto v3 = env("GEMMA_API_KEY")) c.api_key = v3;
  if (auto v = env("SYSLENS_MODEL")) c.model = v;
  if (auto v = env("SYSLENS_API_BASE")) c.api_base = v;
  if (auto v = env("SYSLENS_LOCAL_URL")) c.local_url = v;
  if (auto v = env("SYSLENS_GGUF")) c.local_gguf = v;
  return c;
}

static size_t write_cb(char* ptr, size_t sz, size_t n, void* ud) {
  static_cast<std::string*>(ud)->append(ptr, sz * n);
  return sz * n;
}

struct HttpResp {
  long status = 0;
  std::string body;
  std::string err;
};

static HttpResp http(const std::string& url, const std::string& method, const std::string& body,
                     const std::vector<std::string>& headers, int timeout_s) {
  HttpResp r;
  CURL* c = curl_easy_init();
  if (!c) {
    r.err = "curl init failed";
    return r;
  }
  curl_slist* hl = nullptr;
  for (auto& h : headers) hl = curl_slist_append(hl, h.c_str());
  curl_easy_setopt(c, CURLOPT_URL, url.c_str());
  curl_easy_setopt(c, CURLOPT_HTTPHEADER, hl);
  curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_cb);
  curl_easy_setopt(c, CURLOPT_WRITEDATA, &r.body);
  curl_easy_setopt(c, CURLOPT_TIMEOUT, static_cast<long>(timeout_s));
  curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 10L);
  curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
  if (method == "POST") {
    curl_easy_setopt(c, CURLOPT_POST, 1L);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
  }
  CURLcode rc = curl_easy_perform(c);
  if (rc != CURLE_OK)
    r.err = curl_easy_strerror(rc);
  else
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &r.status);
  curl_slist_free_all(hl);
  curl_easy_cleanup(c);
  return r;
}

static std::string full_prompt(const std::string& system, const std::string& user) {
  return system + "\n\n" + user;
}

static LlmResult call_api(const LlmConfig& cfg, const std::string& prompt) {
  LlmResult res;
  res.backend_used = "gemma api (" + cfg.model + ")";
  if (cfg.api_key.empty()) {
    res.error = "no API key (set GEMINI_API_KEY)";
    return res;
  }
  json body = {{"contents", json::array({{{"role", "user"}, {"parts", json::array({{{"text", prompt}}})}}})},
               {"generationConfig", {{"temperature", cfg.temperature}, {"maxOutputTokens", cfg.max_tokens}}}};
  std::string url = cfg.api_base + "/models/" + cfg.model + ":generateContent";
  HttpResp r = http(url, "POST", body.dump(),
                    {"Content-Type: application/json", "x-goog-api-key: " + cfg.api_key},
                    cfg.timeout_s);
  if (!r.err.empty()) {
    res.error = r.err;
    return res;
  }
  json j = json::parse(r.body, nullptr, false);
  if (j.is_discarded()) {
    res.error = "bad response (HTTP " + std::to_string(r.status) + ")";
    return res;
  }
  if (r.status != 200) {
    std::string m = j.contains("error") && j["error"].contains("message")
                        ? j["error"]["message"].get<std::string>()
                        : r.body.substr(0, 200);
    res.error = "HTTP " + std::to_string(r.status) + ": " + m;
    return res;
  }
  try {
    const json& cand = j["candidates"][0];
    std::string out;
    // Reasoning models return their scratch work as parts flagged "thought": skip those, keep
    // only the final answer text.
    for (auto& part : cand["content"]["parts"]) {
      if (part.value("thought", false)) continue;
      if (part.contains("text")) out += part["text"].get<std::string>();
    }
    res.truncated = cand.value("finishReason", "") == "MAX_TOKENS";
    if (out.empty()) {
      res.error = res.truncated
                      ? "The model used its whole token budget thinking and gave no answer. "
                        "Try a non-reasoning model, e.g. SYSLENS_MODEL=gemma-3-4b-it."
                      : "empty response";
      return res;
    }
    res.ok = true;
    res.text = out;
  } catch (...) {
    res.error = "unexpected response shape";
  }
  return res;
}

static bool local_up(const LlmConfig& cfg) {
  HttpResp r = http(cfg.local_url + "/health", "GET", "", {}, 2);
  return r.err.empty() && r.status == 200;
}

static void maybe_start_local(const LlmConfig& cfg) {
  if (local_up(cfg) || cfg.local_gguf.empty()) return;
  std::string port = "8080";
  size_t c = cfg.local_url.rfind(':');
  if (c != std::string::npos) port = cfg.local_url.substr(c + 1);
  std::string cmd = "nohup llama-server -m '" + cfg.local_gguf + "' --port " + port +
                    " -c 4096 >/dev/null 2>&1 &";
  if (std::system(cmd.c_str()) != 0) return;
  for (int i = 0; i < 90; ++i) {  // wait up to ~90s for the model to load
    if (local_up(cfg)) return;
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }
}

static LlmResult call_local(const LlmConfig& cfg, const std::string& prompt) {
  LlmResult res;
  res.backend_used = "llama.cpp (local)";
  maybe_start_local(cfg);
  json body = {{"messages", json::array({{{"role", "user"}, {"content", prompt}}})},
               {"temperature", cfg.temperature},
               {"max_tokens", cfg.max_tokens}};
  HttpResp r = http(cfg.local_url + "/v1/chat/completions", "POST", body.dump(),
                    {"Content-Type: application/json"}, cfg.timeout_s * 3);
  if (!r.err.empty()) {
    res.error = "llama-server unreachable at " + cfg.local_url + " (" + r.err + ")";
    return res;
  }
  json j = json::parse(r.body, nullptr, false);
  if (j.is_discarded() || r.status != 200) {
    res.error = "llama-server HTTP " + std::to_string(r.status);
    return res;
  }
  try {
    res.text = j["choices"][0]["message"]["content"].get<std::string>();
    res.truncated = j["choices"][0].value("finish_reason", "") == "length";
    res.ok = !res.text.empty();
    if (!res.ok) res.error = "empty response";
  } catch (...) {
    res.error = "unexpected llama-server response";
  }
  return res;
}

LlmResult llm_complete(const LlmConfig& cfg, const std::string& system, const std::string& user) {
  static bool curl_ready = (curl_global_init(CURL_GLOBAL_DEFAULT), true);
  (void)curl_ready;
  std::string prompt = full_prompt(system, user);

  if (cfg.backend == "api") return call_api(cfg, prompt);
  if (cfg.backend == "local") return call_local(cfg, prompt);

  // auto
  std::string api_err;
  if (!cfg.api_key.empty()) {
    LlmResult r = call_api(cfg, prompt);
    if (r.ok) return r;
    api_err = r.error;
  } else {
    api_err = "no API key set";
  }
  LlmResult l = call_local(cfg, prompt);
  if (!l.ok) l.error = "API failed (" + api_err + "); local failed (" + l.error + ")";
  return l;
}
