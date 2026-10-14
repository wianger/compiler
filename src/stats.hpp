#pragma once
#include <chrono>
#include <map>
#include <ostream>
#include <string>
#include <sys/resource.h>

namespace sysy {
class Stats {
  using Clock = std::chrono::steady_clock;
  bool enabled;
  Clock::time_point start = Clock::now();
  struct Measurement { double microseconds = 0; size_t calls = 0; };
  std::map<std::string, Measurement> times;
  std::map<std::string, size_t> counters;
public:
  explicit Stats(bool enabled = false) : enabled(enabled) {}
  template<class F> auto measure(const std::string &name, F body) -> decltype(body()) {
    if (!enabled) return body();
    struct Timer {
      Stats &stats; const std::string &name; Clock::time_point begin = Clock::now();
      ~Timer() {
        auto &m = stats.times[name]; ++m.calls;
        m.microseconds += std::chrono::duration<double, std::micro>(Clock::now()-begin).count();
      }
    } timer{*this, name};
    return body();
  }
  void count(const std::string &name, size_t value) { if (enabled) counters[name] += value; }
  void print(std::ostream &out) const {
    if (!enabled) return;
    struct rusage usage{}; getrusage(RUSAGE_SELF, &usage);
    out << "{\"total_us\":" << std::chrono::duration<double, std::micro>(Clock::now()-start).count()
        << ",\"peak_rss_kib\":" << usage.ru_maxrss << ",\"stages\":{";
    bool comma = false;
    for (const auto &[name,m] : times) {
      if (comma) out << ',';
      comma = true;
      out << '"' << name << "\":{\"us\":" << m.microseconds << ",\"calls\":" << m.calls << '}';
    }
    out << "},\"counters\":{"; comma = false;
    for (const auto &[name,value] : counters) { if (comma) out << ',';
      comma = true; out << '"' << name << "\":" << value; }
    out << "}}\n";
  }
};
} // namespace sysy
