#include "sysinfo.hpp"

#include <arpa/inet.h>
#include <dirent.h>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <set>
#include <sstream>

namespace fs_ = std;  // (no <filesystem> needed; we use dirent for speed)

std::string read_file(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return {};
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

static std::string read_link(const std::string& path) {
  char buf[4096];
  ssize_t n = readlink(path.c_str(), buf, sizeof(buf) - 1);
  if (n <= 0) return {};
  return std::string(buf, static_cast<size_t>(n));
}

static std::vector<int> list_pids() {
  std::vector<int> pids;
  DIR* d = opendir("/proc");
  if (!d) return pids;
  while (dirent* e = readdir(d)) {
    if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
    pids.push_back(atoi(e->d_name));
  }
  closedir(d);
  return pids;
}

std::string proc_cmdline(int pid) {
  std::string s = read_file("/proc/" + std::to_string(pid) + "/cmdline");
  for (auto& c : s)
    if (c == '\0') c = ' ';
  while (!s.empty() && s.back() == ' ') s.pop_back();
  return s;
}
std::string proc_exe(int pid) { return read_link("/proc/" + std::to_string(pid) + "/exe"); }
std::string proc_cwd(int pid) { return read_link("/proc/" + std::to_string(pid) + "/cwd"); }
bool proc_exists(int pid) {
  struct stat st;
  return stat(("/proc/" + std::to_string(pid)).c_str(), &st) == 0;
}

std::string fmt_bytes(uint64_t b) {
  const char* units[] = {"B", "K", "M", "G", "T"};
  double v = static_cast<double>(b);
  int u = 0;
  while (v >= 1024.0 && u < 4) {
    v /= 1024.0;
    ++u;
  }
  char buf[32];
  if (u == 0)
    snprintf(buf, sizeof buf, "%d%s", static_cast<int>(v), units[u]);
  else
    snprintf(buf, sizeof buf, "%.1f%s", v, units[u]);
  return buf;
}

std::string fmt_duration(uint64_t s) {
  uint64_t d = s / 86400, h = (s % 86400) / 3600, m = (s % 3600) / 60, sec = s % 60;
  char buf[48];
  if (d > 0)
    snprintf(buf, sizeof buf, "%lud %02luh", (unsigned long)d, (unsigned long)h);
  else if (h > 0)
    snprintf(buf, sizeof buf, "%luh %02lum", (unsigned long)h, (unsigned long)m);
  else
    snprintf(buf, sizeof buf, "%lum %02lus", (unsigned long)m, (unsigned long)sec);
  return buf;
}

// ---------------------------------------------------------------- processes

std::vector<ProcInfo> ProcSampler::sample() {
  const long clk = sysconf(_SC_CLK_TCK);
  const long page = sysconf(_SC_PAGESIZE);
  auto now = std::chrono::steady_clock::now();
  double dt = have_prev_ ? std::chrono::duration<double>(now - prev_time_).count() : 0.0;

  double sys_up = 0;
  {
    std::istringstream us(read_file("/proc/uptime"));
    us >> sys_up;
  }

  std::vector<ProcInfo> out;
  std::unordered_map<int, uint64_t> cur_ticks;

  for (int pid : list_pids()) {
    std::string base = "/proc/" + std::to_string(pid);
    std::string stat = read_file(base + "/stat");
    if (stat.empty()) continue;
    size_t lp = stat.find('(');
    size_t rp = stat.rfind(')');
    if (lp == std::string::npos || rp == std::string::npos || rp < lp) continue;

    ProcInfo p;
    p.pid = pid;
    p.name = stat.substr(lp + 1, rp - lp - 1);

    // fields after ')' start at field 3 (state)
    std::istringstream is(stat.substr(rp + 2));
    std::vector<std::string> f;
    std::string tok;
    while (is >> tok) f.push_back(tok);
    if (f.size() < 22) continue;

    p.state = f[0];
    p.ppid = atoi(f[1].c_str());
    uint64_t utime = strtoull(f[11].c_str(), nullptr, 10);
    uint64_t stime = strtoull(f[12].c_str(), nullptr, 10);
    p.threads = atoi(f[17].c_str());
    uint64_t start = strtoull(f[19].c_str(), nullptr, 10);
    uint64_t rss_pages = strtoull(f[21].c_str(), nullptr, 10);
    p.rss_bytes = rss_pages * static_cast<uint64_t>(page);

    double up = sys_up - static_cast<double>(start) / static_cast<double>(clk);
    p.uptime_s = up > 0 ? static_cast<uint64_t>(up) : 0;

    uint64_t ticks = utime + stime;
    cur_ticks[pid] = ticks;
    if (dt > 0.0) {
      auto it = prev_ticks_.find(pid);
      if (it != prev_ticks_.end() && ticks >= it->second)
        p.cpu = static_cast<double>(ticks - it->second) / (dt * static_cast<double>(clk)) * 100.0;
    }

    struct stat st;
    if (::stat(base.c_str(), &st) == 0) {
      auto uc = user_cache_.find(st.st_uid);
      if (uc == user_cache_.end()) {
        passwd* pw = getpwuid(st.st_uid);
        std::string name = pw ? pw->pw_name : std::to_string(st.st_uid);
        uc = user_cache_.emplace(st.st_uid, name).first;
      }
      p.user = uc->second;
    }
    out.push_back(std::move(p));
  }

  prev_ticks_ = std::move(cur_ticks);
  prev_time_ = now;
  have_prev_ = true;

  // Top to bottom by CPU, then memory.
  std::sort(out.begin(), out.end(), [](const ProcInfo& a, const ProcInfo& b) {
    if (a.cpu != b.cpu) return a.cpu > b.cpu;
    return a.rss_bytes > b.rss_bytes;
  });
  return out;
}

// -------------------------------------------------------------------- ports

static std::string hex_to_addr(const std::string& hex, bool v6) {
  char buf[INET6_ADDRSTRLEN] = {0};
  if (!v6) {
    uint32_t v = static_cast<uint32_t>(strtoul(hex.c_str(), nullptr, 16));
    in_addr a;
    a.s_addr = v;
    inet_ntop(AF_INET, &a, buf, sizeof buf);
  } else {
    if (hex.size() != 32) return "?";
    in6_addr a;
    for (int i = 0; i < 4; ++i) {
      uint32_t w = static_cast<uint32_t>(strtoul(hex.substr(i * 8, 8).c_str(), nullptr, 16));
      memcpy(a.s6_addr + i * 4, &w, 4);
    }
    inet_ntop(AF_INET6, &a, buf, sizeof buf);
  }
  return buf;
}

static std::string tcp_state(const std::string& h) {
  static const char* names[] = {"",           "ESTABLISHED", "SYN_SENT",  "SYN_RECV",
                                "FIN_WAIT1",  "FIN_WAIT2",   "TIME_WAIT", "CLOSE",
                                "CLOSE_WAIT", "LAST_ACK",    "LISTEN",    "CLOSING"};
  unsigned v = static_cast<unsigned>(strtoul(h.c_str(), nullptr, 16));
  return v < 12 ? names[v] : "?";
}

static std::unordered_map<uint64_t, std::pair<int, std::string>> inode_owners() {
  std::unordered_map<uint64_t, std::pair<int, std::string>> map;
  for (int pid : list_pids()) {
    std::string fd_dir = "/proc/" + std::to_string(pid) + "/fd";
    DIR* d = opendir(fd_dir.c_str());
    if (!d) continue;  // permission denied for other users' processes without root
    std::string comm;
    while (dirent* e = readdir(d)) {
      if (e->d_name[0] == '.') continue;
      std::string target = read_link(fd_dir + "/" + e->d_name);
      if (target.rfind("socket:[", 0) != 0) continue;
      uint64_t inode = strtoull(target.c_str() + 8, nullptr, 10);
      if (comm.empty()) {
        comm = read_file("/proc/" + std::to_string(pid) + "/comm");
        while (!comm.empty() && (comm.back() == '\n')) comm.pop_back();
      }
      map.emplace(inode, std::make_pair(pid, comm));
    }
    closedir(d);
  }
  return map;
}

static void parse_net(const std::string& proto, const std::string& path, bool v6, bool udp,
                      bool include_established, std::vector<PortInfo>& out) {
  std::istringstream in(read_file(path));
  std::string line;
  std::getline(in, line);  // header
  while (std::getline(in, line)) {
    std::istringstream ls(line);
    std::string sl, local, remote, st, queues, timer, retr, uid, timeout, inode;
    if (!(ls >> sl >> local >> remote >> st >> queues >> timer >> retr >> uid >> timeout >> inode))
      continue;
    PortInfo p;
    p.proto = proto;
    size_t c = local.rfind(':');
    p.local_addr = hex_to_addr(local.substr(0, c), v6);
    p.local_port = static_cast<int>(strtoul(local.substr(c + 1).c_str(), nullptr, 16));
    c = remote.rfind(':');
    p.remote_addr = hex_to_addr(remote.substr(0, c), v6);
    p.remote_port = static_cast<int>(strtoul(remote.substr(c + 1).c_str(), nullptr, 16));
    p.inode = strtoull(inode.c_str(), nullptr, 10);
    if (udp) {
      p.state = (st == "07") ? "UNCONN" : "ESTAB";
    } else {
      p.state = tcp_state(st);
      if (!include_established && p.state != "LISTEN") continue;
    }
    out.push_back(std::move(p));
  }
}

std::vector<PortInfo> read_ports(bool include_established) {
  std::vector<PortInfo> out;
  parse_net("tcp", "/proc/net/tcp", false, false, include_established, out);
  parse_net("tcp6", "/proc/net/tcp6", true, false, include_established, out);
  // UDP: show bound sockets only (remote port 0) unless established requested.
  std::vector<PortInfo> udp;
  parse_net("udp", "/proc/net/udp", false, true, true, udp);
  parse_net("udp6", "/proc/net/udp6", true, true, true, udp);
  for (auto& u : udp)
    if (include_established || u.remote_port == 0) out.push_back(std::move(u));

  auto owners = inode_owners();
  for (auto& p : out) {
    auto it = owners.find(p.inode);
    if (it != owners.end()) {
      p.pid = it->second.first;
      p.process = it->second.second;
    }
  }
  std::sort(out.begin(), out.end(), [](const PortInfo& a, const PortInfo& b) {
    bool al = a.state == "LISTEN", bl = b.state == "LISTEN";
    if (al != bl) return al;
    if (a.local_port != b.local_port) return a.local_port < b.local_port;
    return a.proto < b.proto;
  });
  return out;
}

// -------------------------------------------------------- AI context builders

std::string describe_process(int pid) {
  std::ostringstream o;
  std::string base = "/proc/" + std::to_string(pid);
  std::string stat = read_file(base + "/stat");
  size_t lp = stat.find('('), rp = stat.rfind(')');
  std::string name = (lp != std::string::npos && rp != std::string::npos)
                         ? stat.substr(lp + 1, rp - lp - 1)
                         : "?";
  std::istringstream is(rp != std::string::npos ? stat.substr(rp + 2) : "");
  std::vector<std::string> f;
  for (std::string t; is >> t;) f.push_back(t);

  o << "PID: " << pid << "\nName: " << name << "\n";
  o << "Command line: " << proc_cmdline(pid) << "\n";
  o << "Executable: " << proc_exe(pid) << "\n";
  o << "Working dir: " << proc_cwd(pid) << "\n";
  if (f.size() >= 22) {
    int ppid = atoi(f[1].c_str());
    o << "State: " << f[0] << "\nThreads: " << f[17] << "\n";
    std::string pn = read_file("/proc/" + std::to_string(ppid) + "/comm");
    while (!pn.empty() && pn.back() == '\n') pn.pop_back();
    o << "Parent: " << ppid << " (" << pn << ")\n";
    o << "RSS memory: " << fmt_bytes(strtoull(f[21].c_str(), nullptr, 10) *
                                      static_cast<uint64_t>(sysconf(_SC_PAGESIZE)))
      << "\n";
  }
  // status excerpts
  std::istringstream st(read_file(base + "/status"));
  for (std::string line; std::getline(st, line);)
    if (line.rfind("Uid:", 0) == 0 || line.rfind("VmPeak:", 0) == 0 ||
        line.rfind("VmSize:", 0) == 0 || line.rfind("VmRSS:", 0) == 0)
      o << line << "\n";

  // listening / connected sockets owned by this pid
  auto ports = read_ports(true);
  std::set<std::string> seen;
  for (auto& p : ports) {
    if (p.pid != pid) continue;
    std::string l = p.proto + " " + p.local_addr + ":" + std::to_string(p.local_port) + " " + p.state;
    if (p.state != "LISTEN" && p.state != "UNCONN")
      l += " -> " + p.remote_addr + ":" + std::to_string(p.remote_port);
    if (seen.insert(l).second) o << "Socket: " << l << "\n";
  }
  // a handful of open files (best-effort)
  DIR* d = opendir((base + "/fd").c_str());
  int shown = 0;
  if (d) {
    while (dirent* e = readdir(d)) {
      if (e->d_name[0] == '.') continue;
      std::string t = read_link(base + "/fd/" + e->d_name);
      if (t.empty() || t[0] != '/' || t.rfind("/dev/", 0) == 0 || t.rfind("/proc/", 0) == 0)
        continue;
      if (shown++ < 8) o << "Open file: " << t << "\n";
    }
    closedir(d);
  }
  return o.str();
}

PortFacts inspect_port(int port) {
  PortFacts f;
  std::ostringstream o;
  auto ports = read_ports(true);
  std::set<int> described_pids;
  int shown = 0, extra = 0;
  for (auto& p : ports) {
    if (p.local_port != port) continue;
    if (p.state == "TIME_WAIT" || p.state == "CLOSE") continue;  // nothing live there
    f.found = true;
    if (shown >= 6) {
      ++extra;
      continue;
    }
    ++shown;
    o << "Socket: " << p.proto << " " << p.local_addr << ":" << p.local_port << " state "
      << p.state;
    if (p.state != "LISTEN" && p.state != "UNCONN")
      o << " peer " << p.remote_addr << ":" << p.remote_port;
    o << "\n";
    if (p.pid > 0) {
      if (!f.owner_known) {
        f.owner_known = true;
        f.summary = p.proto + " " + p.local_addr + ":" + std::to_string(p.local_port) + " · " +
                    p.process + " (PID " + std::to_string(p.pid) + ")";
      }
      if (described_pids.insert(p.pid).second) {
        o << "  Owner PID " << p.pid << " (" << p.process << ")\n";
        o << "  Command: " << proc_cmdline(p.pid) << "\n";
        o << "  Executable: " << proc_exe(p.pid) << "\n";
        o << "  Working dir: " << proc_cwd(p.pid) << "\n";
      }
    } else {
      o << "  Owner not visible (other user's process; needs sudo)\n";
    }
  }
  if (extra > 0) o << "(+" << extra << " more connections on this port)\n";
  if (f.found && f.summary.empty())
    f.summary = "port " + std::to_string(port) + " · owner not visible";
  f.text = o.str();
  return f;
}

std::string describe_port(int port) {
  PortFacts f = inspect_port(port);
  return f.found ? f.text : "Nothing is using port " + std::to_string(port) + ".\n";
}
