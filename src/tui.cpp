#include "tui.hpp"

#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>

#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/terminal.hpp>

#include "analyze.hpp"
#include "sysinfo.hpp"

using namespace ftxui;

namespace {

struct MemInfo {
  uint64_t total = 0, used = 0;
};

MemInfo read_meminfo() {
  MemInfo m;
  std::istringstream in(read_file("/proc/meminfo"));
  std::string key;
  uint64_t val;
  std::string unit;
  uint64_t avail = 0;
  while (in >> key >> val >> unit) {
    if (key == "MemTotal:") m.total = val * 1024;
    if (key == "MemAvailable:") avail = val * 1024;
  }
  m.used = m.total > avail ? m.total - avail : 0;
  return m;
}

enum class SortBy { Cpu, Mem };

struct Shared {
  std::mutex mu;
  std::vector<ProcInfo> procs;
  std::vector<PortInfo> ports;
  double total_cpu = 0;
  MemInfo mem;
  std::atomic<bool> show_established{false};
  std::atomic<SortBy> sort{SortBy::Cpu};
  std::atomic<bool> running{true};
};

Element cell(const std::string& s, int w, bool right = false) {
  auto t = text(s);
  if (right) t = hbox({filler(), t});
  return t | size(WIDTH, EQUAL, w);
}

Color heat(double frac) {
  if (frac < 0.30) return Color::Green;
  if (frac < 0.65) return Color::Yellow;
  return Color::Red;
}

std::string fixed(double v, int prec) {
  char buf[32];
  snprintf(buf, sizeof buf, "%.*f", prec, v);
  return buf;
}

std::string port_key(const PortInfo& p) {
  return p.proto + ":" + std::to_string(p.local_port) + ":" + std::to_string(p.pid) + ":" +
         p.remote_addr + ":" + std::to_string(p.remote_port);
}

}  // namespace

int run_tui(const LlmConfig& cfg) {
  Shared sh;
  auto screen = ScreenInteractive::Fullscreen();

  // UI state (only touched from the UI thread, except popup fields guarded by popup_mu)
  int focus_pane = 0;  // 0 = processes, 1 = ports
  int sel_pid = -1;
  std::string sel_port;
  int proc_off = 0, port_off = 0;

  std::mutex popup_mu;
  bool popup_open = false, popup_busy = false;
  std::string popup_title;
  Analysis popup_result;
  int popup_scroll = 0;
  std::atomic<int> popup_gen{0};
  std::atomic<int> inflight{0};

  // ---- refresher thread
  std::thread refresher([&] {
    ProcSampler sampler;
    sampler.sample();  // prime CPU deltas
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    int tick = 0;
    const long ncpu = std::max(1L, sysconf(_SC_NPROCESSORS_ONLN));
    while (sh.running) {
      auto procs = sampler.sample();
      double total = 0;
      for (auto& p : procs) total += p.cpu;
      if (sh.sort == SortBy::Mem)
        std::sort(procs.begin(), procs.end(), [](const ProcInfo& a, const ProcInfo& b) {
          if (a.rss_bytes != b.rss_bytes) return a.rss_bytes > b.rss_bytes;
          return a.cpu > b.cpu;
        });
      std::vector<PortInfo> ports;
      bool refresh_ports = (tick % 2 == 0);
      if (refresh_ports) ports = read_ports(sh.show_established);
      MemInfo mem = read_meminfo();
      {
        std::lock_guard<std::mutex> lk(sh.mu);
        sh.procs = std::move(procs);
        if (refresh_ports) sh.ports = std::move(ports);
        sh.total_cpu = std::min(100.0, total / static_cast<double>(ncpu));
        sh.mem = mem;
      }
      screen.PostEvent(Event::Custom);
      ++tick;
      for (int i = 0; i < 10 && sh.running; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  });

  auto start_analysis = [&](bool process) {
    std::string title;
    int pid = -1, port = -1;
    {
      std::lock_guard<std::mutex> lk(sh.mu);
      if (process) {
        pid = sel_pid;
        for (auto& p : sh.procs)
          if (p.pid == pid) title = "Process " + std::to_string(p.pid) + " · " + p.name;
      } else {
        for (auto& p : sh.ports)
          if (port_key(p) == sel_port) {
            port = p.local_port;
            title = "Port " + std::to_string(p.local_port) + " (" + p.proto + ")";
          }
      }
    }
    if (title.empty()) return;
    int gen = ++popup_gen;
    {
      std::lock_guard<std::mutex> lk(popup_mu);
      popup_open = true;
      popup_busy = true;
      popup_title = title;
      popup_result = Analysis{};
      popup_scroll = 0;
    }
    ++inflight;
    std::thread([&, gen, process, pid, port] {
      // analyze_* make no model call when the process is gone / the port is empty / the owner
      // is hidden; they return a plain-language error instead.
      Analysis a = process ? analyze_process(cfg, pid) : analyze_port(cfg, port);
      if (gen == popup_gen) {
        std::lock_guard<std::mutex> lk(popup_mu);
        popup_busy = false;
        popup_result = std::move(a);
      }
      screen.PostEvent(Event::Custom);
      --inflight;
    }).detach();
  };

  // ---- rendering
  auto render = [&]() -> Element {
    std::vector<ProcInfo> procs;
    std::vector<PortInfo> ports;
    double total_cpu;
    MemInfo mem;
    {
      std::lock_guard<std::mutex> lk(sh.mu);
      procs = sh.procs;
      ports = sh.ports;
      total_cpu = sh.total_cpu;
      mem = sh.mem;
    }

    const int H = Terminal::Size().dimy;
    const int avail = std::max(8, H - 2);
    const int proc_h = avail * 6 / 10;
    const int port_h = avail - proc_h;
    const int proc_rows = std::max(1, proc_h - 3);
    const int port_rows = std::max(1, port_h - 3);

    // resolve selection to an index; default to first row
    int proc_idx = 0;
    for (size_t i = 0; i < procs.size(); ++i)
      if (procs[i].pid == sel_pid) proc_idx = static_cast<int>(i);
    if (!procs.empty()) sel_pid = procs[proc_idx].pid;
    int port_idx = 0;
    for (size_t i = 0; i < ports.size(); ++i)
      if (port_key(ports[i]) == sel_port) port_idx = static_cast<int>(i);
    if (!ports.empty()) sel_port = port_key(ports[port_idx]);

    if (proc_idx < proc_off) proc_off = proc_idx;
    if (proc_idx >= proc_off + proc_rows) proc_off = proc_idx - proc_rows + 1;
    if (port_idx < port_off) port_off = port_idx;
    if (port_idx >= port_off + port_rows) port_off = port_idx - port_rows + 1;

    // header
    double memfrac = mem.total ? static_cast<double>(mem.used) / static_cast<double>(mem.total) : 0;
    auto header = hbox({
        text(" sysLens ") | bold | inverted,
        text("  CPU ") | dim,
        gauge(static_cast<float>(total_cpu / 100.0)) | color(heat(total_cpu / 100.0)) |
            size(WIDTH, EQUAL, 14),
        text(" " + fixed(total_cpu, 1) + "%"),
        text("   MEM ") | dim,
        gauge(static_cast<float>(memfrac)) | color(heat(memfrac)) | size(WIDTH, EQUAL, 14),
        text(" " + fmt_bytes(mem.used) + "/" + fmt_bytes(mem.total)),
        text("   Tasks " + std::to_string(procs.size())) | dim,
        filler(),
        text(sh.sort == SortBy::Cpu ? "sort: CPU " : "sort: MEM ") | dim,
    });

    // processes pane
    Elements prow;
    prow.push_back(hbox({cell("PID", 7) | bold, cell("USER", 11) | bold, text("NAME") | flex | bold,
                         cell("CPU%", 7, true) | bold, cell("MEM", 9, true) | bold,
                         cell("UPTIME", 11, true) | bold}) |
                   color(Color::Cyan));
    for (int i = proc_off; i < static_cast<int>(procs.size()) && i < proc_off + proc_rows; ++i) {
      const auto& p = procs[i];
      double memf = mem.total ? static_cast<double>(p.rss_bytes) / static_cast<double>(mem.total) : 0;
      auto row = hbox({cell(std::to_string(p.pid), 7), cell(p.user, 11),
                       text(p.name) | flex,
                       cell(fixed(p.cpu, 1), 7, true) | color(heat(p.cpu / 100.0)),
                       cell(fmt_bytes(p.rss_bytes), 9, true) | color(heat(memf * 4)),
                       cell(fmt_duration(p.uptime_s), 11, true)});
      if (i == proc_idx) row = row | (focus_pane == 0 ? inverted : bold);
      prow.push_back(row);
    }
    auto proc_title = " Processes (" + std::to_string(procs.size()) + ") ";
    auto proc_pane = window(text(proc_title) | bold, vbox(std::move(prow)) | yflex) |
                     size(HEIGHT, EQUAL, proc_h);
    if (focus_pane != 0) proc_pane = proc_pane | dim;

    // ports pane
    Elements qrow;
    qrow.push_back(hbox({cell("PROTO", 7) | bold, cell("LOCAL ADDRESS", 30) | bold,
                         cell("REMOTE", 26) | bold, cell("STATE", 13) | bold, cell("PID", 8) | bold,
                         text("PROCESS") | flex | bold}) |
                   color(Color::Cyan));
    for (int i = port_off; i < static_cast<int>(ports.size()) && i < port_off + port_rows; ++i) {
      const auto& p = ports[i];
      std::string remote = p.remote_port == 0 ? "-" : p.remote_addr + ":" + std::to_string(p.remote_port);
      auto row = hbox({cell(p.proto, 7), cell(p.local_addr + ":" + std::to_string(p.local_port), 30),
                       cell(remote, 26),
                       cell(p.state, 13) | color(p.state == "LISTEN" ? Color(Color::Green) : Color(Color::Default)),
                       cell(p.pid > 0 ? std::to_string(p.pid) : "?", 8),
                       text(p.process.empty() ? "(sudo to see)" : p.process) | flex});
      if (i == port_idx) row = row | (focus_pane == 1 ? inverted : bold);
      qrow.push_back(row);
    }
    auto port_title = std::string(" Ports (") + std::to_string(ports.size()) + ") " +
                      (sh.show_established ? "all sockets " : "listening ");
    auto port_pane = window(text(port_title) | bold, vbox(std::move(qrow)) | yflex) |
                     size(HEIGHT, EQUAL, port_h);
    if (focus_pane != 1) port_pane = port_pane | dim;

    auto footer = hbox({text(" Tab") | bold, text(" switch  ") | dim, text("↑↓") | bold,
                        text(" move  ") | dim, text("a") | bold, text(" AI analyze  ") | dim,
                        text("c/m") | bold, text(" sort  ") | dim, text("e") | bold,
                        text(" all sockets  ") | dim, text("q") | bold, text(" quit") | dim});

    Element base = vbox({header, proc_pane, port_pane, footer});

    bool open, busy;
    std::string title;
    Analysis result;
    int scroll;
    {
      std::lock_guard<std::mutex> lk(popup_mu);
      open = popup_open;
      busy = popup_busy;
      title = popup_title;
      result = popup_result;
      scroll = popup_scroll;
    }
    if (!open) return base;

    // Fixed-width card; text is pre-wrapped so every line renders exactly as laid out.
    const int W = Terminal::Size().dimx;
    const int inner = std::clamp(W - 12, 40, 72);
    std::vector<DocLine> doc;
    if (busy)
      doc.push_back({"  Analyzing…", LineKind::Blank});
    else if (!result.ok)
      doc = layout_error(result.error, inner - 2);
    else
      doc = layout_sections(result.sections, inner - 2);

    const int rows = std::max(3, std::min<int>(static_cast<int>(doc.size()), H - 10));
    const int max_scroll = std::max(0, static_cast<int>(doc.size()) - rows);
    scroll = std::clamp(scroll, 0, max_scroll);
    {
      std::lock_guard<std::mutex> lk(popup_mu);
      popup_scroll = scroll;
    }

    Elements lines;
    for (int i = scroll; i < scroll + rows && i < static_cast<int>(doc.size()); ++i) {
      const auto& dl = doc[i];
      Element e = text(" " + dl.text);
      switch (dl.kind) {
        case LineKind::Heading: e = e | bold | color(Color::Cyan); break;
        case LineKind::Command: e = e | color(Color::Green); break;
        case LineKind::Error: e = e | color(Color::Red); break;
        case LineKind::Blank: e = busy ? e | dim : text(""); break;
        default: break;
      }
      lines.push_back(e);
    }
    while (static_cast<int>(lines.size()) < rows) lines.push_back(text(""));

    std::string hint = " Esc close";
    if (max_scroll > 0) hint += " · ↑↓ scroll (" + std::to_string(scroll + 1) + "/" +
                                std::to_string(max_scroll + 1) + ")";
    if (!result.backend.empty() && result.ok) hint += " · " + result.backend;
    auto popup = vbox({text(" " + title + " ") | bold | color(Color::Cyan), separator(),
                       vbox(std::move(lines)), separator(), text(hint) | dim}) |
                 size(WIDTH, EQUAL, inner) | border | clear_under | center;
    return dbox({base, popup});
  };

  auto root = Renderer(render);

  root = CatchEvent(root, [&](Event e) {
    if (e == Event::Custom) return false;

    bool open;
    {
      std::lock_guard<std::mutex> lk(popup_mu);
      open = popup_open;
    }
    if (open) {
      std::lock_guard<std::mutex> lk(popup_mu);
      if (e == Event::Escape || e == Event::Character('q') || e == Event::Return) {
        popup_open = false;
        ++popup_gen;
      } else if (e == Event::ArrowDown || e == Event::Character('j')) {
        ++popup_scroll;  // clamped at render time
      } else if (e == Event::ArrowUp || e == Event::Character('k')) {
        popup_scroll = std::max(0, popup_scroll - 1);
      }
      return true;
    }

    if (e == Event::Character('q') || e == Event::Escape) {
      sh.running = false;
      screen.Exit();
      return true;
    }
    if (e == Event::Tab) {
      focus_pane = 1 - focus_pane;
      return true;
    }
    if (e == Event::Character('c')) { sh.sort = SortBy::Cpu; return true; }
    if (e == Event::Character('m')) { sh.sort = SortBy::Mem; return true; }
    if (e == Event::Character('e')) { sh.show_established = !sh.show_established; return true; }
    if (e == Event::Character('a') || e == Event::Return) {
      start_analysis(focus_pane == 0);
      return true;
    }

    int delta = 0;
    if (e == Event::ArrowDown || e == Event::Character('j')) delta = 1;
    else if (e == Event::ArrowUp || e == Event::Character('k')) delta = -1;
    else if (e == Event::PageDown) delta = 10;
    else if (e == Event::PageUp) delta = -10;
    else if (e == Event::Home) delta = -1000000;
    else if (e == Event::End) delta = 1000000;
    if (delta != 0) {
      std::lock_guard<std::mutex> lk(sh.mu);
      if (focus_pane == 0 && !sh.procs.empty()) {
        int idx = 0;
        for (size_t i = 0; i < sh.procs.size(); ++i)
          if (sh.procs[i].pid == sel_pid) idx = static_cast<int>(i);
        idx = std::clamp(idx + delta, 0, static_cast<int>(sh.procs.size()) - 1);
        sel_pid = sh.procs[idx].pid;
      } else if (focus_pane == 1 && !sh.ports.empty()) {
        int idx = 0;
        for (size_t i = 0; i < sh.ports.size(); ++i)
          if (port_key(sh.ports[i]) == sel_port) idx = static_cast<int>(i);
        idx = std::clamp(idx + delta, 0, static_cast<int>(sh.ports.size()) - 1);
        sel_port = port_key(sh.ports[idx]);
      }
      return true;
    }
    return false;
  });

  screen.Loop(root);
  sh.running = false;
  ++popup_gen;
  refresher.join();
  if (inflight > 0) {
    // An AI request is still pending and its thread references our locals; the terminal is
    // already restored, so leave immediately instead of waiting for the network timeout.
    fflush(stdout);
    _exit(0);
  }
  return 0;
}
