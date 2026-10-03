#pragma once
#include "llm.hpp"

// Realtime htop/btop-style UI: processes pane + ports pane, with AI analysis popup.
int run_tui(const LlmConfig& cfg);
