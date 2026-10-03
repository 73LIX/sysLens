#pragma once
#include <chrono>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

struct ProcInfo {
  int pid = 0;
  int ppid = 0;
  std::string name;
  std::string user;
  std::string state;
  double cpu = 0.0;          // percent of one core (can exceed 100 for multithreaded)
  uint64_t rss_bytes = 0;
  uint64_t uptime_s = 0;
  int threads = 0;
};

struct PortInfo {
  std::string proto;  // tcp, tcp6, udp, udp6
  std::string local_addr;
  int local_port = 0;
  std::string remote_addr;
  int remote_port = 0;
  std::string state;
  int pid = -1;
  std::string process;
  uint64_t inode = 0;
};

// Samples /proc. CPU% is computed from the tick delta between two sample() calls.
class ProcSampler {
 public:
  std::vector<ProcInfo> sample();

 private:
  std::unordered_map<int, uint64_t> prev_ticks_;
  std::chrono::steady_clock::time_point prev_time_{};
  bool have_prev_ = false;
  std::unordered_map<unsigned, std::string> user_cache_;
};

// include_established=false -> only LISTEN sockets (TCP) and bound UDP sockets.
std::vector<PortInfo> read_ports(bool include_established);

// Detail helpers used for AI analysis.
std::string read_file(const std::string& path);
std::string proc_cmdline(int pid);
std::string proc_exe(int pid);
std::string proc_cwd(int pid);
bool proc_exists(int pid);
std::string describe_process(int pid);          // multi-line facts about a pid
std::string describe_port(int port);            // multi-line facts about a port

struct PortFacts {
  bool found = false;        // a live socket (not TIME_WAIT/CLOSE) exists on the port
  bool owner_known = false;  // we could map at least one socket to a process
  std::string summary;       // one-line "tcp 0.0.0.0:80 · nginx (PID 12)"
  std::string text;          // detailed facts for the model
};
PortFacts inspect_port(int port);

// Formatting helpers.
std::string fmt_bytes(uint64_t b);
std::string fmt_duration(uint64_t seconds);
