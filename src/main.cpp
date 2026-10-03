#include <unistd.h>

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

#include "analyze.hpp"
#include "errors.hpp"
#include "format.hpp"
#include "llm.hpp"
#include "sysinfo.hpp"
#include "tui.hpp"

static const char* kUsage = R"(sysLens - htop/btop-style monitor with AI diagnostics

USAGE
  sysLens                          live TUI (processes + ports)
  sysLens --process <PID> [--analyze]
  sysLens --port <PORT>   [--analyze]
  sysLens --run "<command>"        run a command, then triage + explain its errors
  sysLens run -- <cmd> [args...]   same, without shell quoting
  sysLens --errors [FILE]          triage + explain errors from FILE (or stdin if piped)
      e.g.  cargo build 2>&1 | sysLens --errors

OPTIONS
  --no-ai             only classify (critical / error / warning), don't call a model
  --backend <b>       auto (default) | api | local
  --model <name>      Gemma model id for the API (default gemma-3n-e2b-it)
  -h, --help          show this help

ENVIRONMENT
  GEMINI_API_KEY      API key (Google AI Studio) used to reach Gemma
  SYSLENS_MODEL       model id, e.g. gemma-3-1b-it
  SYSLENS_BACKEND     auto | api | local
  SYSLENS_LOCAL_URL   llama-server URL (default http://127.0.0.1:8080)
  SYSLENS_GGUF        path to a Gemma GGUF; llama-server is started automatically if down

TUI KEYS
  Tab switch pane   Up/Down move   a/Enter AI-analyze selection   c/m sort CPU/MEM
  e toggle all sockets   q quit
)";

static std::string slurp(std::istream& in) {
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

int main(int argc, char** argv) {
  LlmConfig cfg = LlmConfig::from_env();
  bool analyze = false, no_ai = false;
  int pid = -1, port = -1;
  bool want_errors = false;
  std::string errors_file, run_cmd;
  std::vector<std::string> run_argv;

  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto next = [&](const char* what) -> std::string {
      if (i + 1 >= argc) {
        std::cerr << "sysLens: " << what << " needs a value\n";
        std::exit(1);
      }
      return argv[++i];
    };
    if (a == "-h" || a == "--help") {
      std::cout << kUsage;
      return 0;
    } else if (a == "--process" || a == "-p") {
      pid = std::atoi(next("--process").c_str());
    } else if (a == "--port") {
      port = std::atoi(next("--port").c_str());
    } else if (a == "--analyze") {
      analyze = true;
    } else if (a == "--no-ai") {
      no_ai = true;
    } else if (a == "--backend") {
      cfg.backend = next("--backend");
    } else if (a == "--model") {
      cfg.model = next("--model");
    } else if (a == "--errors") {
      want_errors = true;
      if (i + 1 < argc && argv[i + 1][0] != '-') errors_file = argv[++i];
    } else if (a == "--run") {
      run_cmd = next("--run");
    } else if (a == "run") {
      for (++i; i < argc; ++i) {
        std::string x = argv[i];
        if (x == "--" && run_argv.empty()) continue;
        run_argv.push_back(x);
      }
    } else {
      std::cerr << "sysLens: unknown argument '" << a << "' (try --help)\n";
      return 1;
    }
  }

  // ---- process / port facts and analysis
  const int W = term_width();
  auto show_analysis = [&](const std::string& heading, const Analysis& an) -> int {
    std::cout << "\n";
    if (isatty(STDOUT_FILENO)) std::cout << "\x1b[1;36m";
    std::cout << heading;
    if (isatty(STDOUT_FILENO)) std::cout << "\x1b[0m";
    std::cout << "\n";
    if (!an.ok) {
      print_doc(layout_error(an.error, W - 2));
      return 2;
    }
    print_doc(layout_sections(an.sections, W - 2));
    if (isatty(STDOUT_FILENO)) std::cout << "\x1b[2m";
    std::cout << "\n  via " << an.backend;
    if (isatty(STDOUT_FILENO)) std::cout << "\x1b[0m";
    std::cout << "\n";
    return 0;
  };

  if (pid > 0) {
    if (!proc_exists(pid)) {
      std::cerr << "No process with PID " << pid << " is running.\n";
      return 1;
    }
    if (!analyze) {
      std::cout << describe_process(pid);
      return 0;
    }
    std::string comm = read_file("/proc/" + std::to_string(pid) + "/comm");
    while (!comm.empty() && comm.back() == '\n') comm.pop_back();
    return show_analysis("Process " + std::to_string(pid) + " · " + comm, analyze_process(cfg, pid));
  }
  if (port > 0) {
    PortFacts pf = inspect_port(port);
    if (!pf.found) {
      std::cerr << "Nothing is using port " << port << ".\n";
      return 1;
    }
    if (!analyze) {
      std::cout << pf.text;
      return 0;
    }
    return show_analysis("Port " + std::to_string(port) + " · " + pf.summary, analyze_port(cfg, port));
  }

  // ---- error triage
  if (!run_argv.empty() || !run_cmd.empty()) {
    std::string cmd = run_argv.empty() ? run_cmd : shell_join(run_argv);
    std::string log;
    int code = run_and_capture(cmd, log);
    if (code == 0) {
      // Succeeded, but still surface warnings (no AI call for warnings alone).
      return report_errors(log, "`" + cmd + "` (exit 0)", cfg, !no_ai) == 2 ? 2 : 0;
    }
    report_errors(log, "`" + cmd + "` (exit " + std::to_string(code) + ")", cfg, !no_ai);
    return code;
  }
  if (want_errors || (!isatty(STDIN_FILENO) && argc > 1)) {
    std::string log;
    std::string label = "stdin";
    if (!errors_file.empty()) {
      std::ifstream f(errors_file);
      if (!f) {
        std::cerr << "sysLens: cannot read " << errors_file << "\n";
        return 1;
      }
      log = slurp(f);
      label = errors_file;
    } else if (!isatty(STDIN_FILENO)) {
      log = slurp(std::cin);
    } else {
      std::cerr << "sysLens: --errors needs a FILE or piped input\n";
      return 1;
    }
    return report_errors(log, label, cfg, !no_ai);
  }

  if (argc > 1) {
    std::cout << kUsage;
    return 1;
  }
  return run_tui(cfg);
}
