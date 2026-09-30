#include "monitor/rtss_engine.h"

#include <intrin.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <format>

#include "common/log.h"
#include "common/util.h"

namespace po {

// ------------------------------------------------------------------ espressioni
struct RtssEngine::Node {
  enum class Kind { Num, Var, Call, Unary, Binary, Ternary } kind = Kind::Num;
  double num = 0;
  std::string name;  // Var: nome sorgente/variabile; Call: funzione; Unary/Binary: operatore
  std::vector<std::shared_ptr<Node>> args;
};

namespace {

using NodeP = std::shared_ptr<RtssEngine::Node>;
using Kind = RtssEngine::Node::Kind;

struct Tok {
  enum class T { Num, Ident, Str, Op, End } t = T::End;
  std::string s;
  double num = 0;
};

std::vector<Tok> Tokenize(const std::string& src) {
  std::vector<Tok> out;
  size_t i = 0;
  while (i < src.size()) {
    const char c = src[i];
    if (isspace(uint8_t(c))) {
      ++i;
    } else if (isdigit(uint8_t(c)) || (c == '.' && i + 1 < src.size() && isdigit(uint8_t(src[i + 1])))) {
      Tok t{Tok::T::Num};
      if (c == '0' && i + 1 < src.size() && (src[i + 1] == 'x' || src[i + 1] == 'X')) {
        size_t j = i + 2;
        while (j < src.size() && isxdigit(uint8_t(src[j]))) ++j;
        t.num = double(std::stoull(src.substr(i + 2, j - i - 2), nullptr, 16));
        i = j;
      } else {
        size_t j = i;
        while (j < src.size() && (isdigit(uint8_t(src[j])) || src[j] == '.')) ++j;
        t.num = std::stod(src.substr(i, j - i));
        i = j;
      }
      out.push_back(t);
    } else if (isalpha(uint8_t(c)) || c == '_') {
      size_t j = i;
      while (j < src.size() && (isalnum(uint8_t(src[j])) || src[j] == '_')) ++j;
      out.push_back({Tok::T::Ident, src.substr(i, j - i)});
      i = j;
    } else if (c == '"') {
      // Nome di sorgente tra virgolette; l'OverlayEditor a volte le raddoppia: ""Framerate"".
      size_t q = i;
      while (q < src.size() && src[q] == '"') ++q;
      const size_t j = src.find('"', q);
      out.push_back({Tok::T::Str, src.substr(q, (j == std::string::npos ? src.size() : j) - q)});
      i = j == std::string::npos ? src.size() : j;
      while (i < src.size() && src[i] == '"') ++i;
    } else {
      static const char* kTwo[] = {"<=", ">=", "==", "!=", "&&", "||"};
      std::string op(1, c);
      for (const char* t : kTwo)
        if (src.compare(i, 2, t) == 0) op = t;
      out.push_back({Tok::T::Op, op});
      i += op.size();
    }
  }
  out.push_back({Tok::T::End});
  return out;
}

class Parser {
 public:
  explicit Parser(std::vector<Tok> t) : t_(std::move(t)) {}
  NodeP Parse() {
    NodeP n = Ternary();
    return t_[p_].t == Tok::T::End ? n : nullptr;
  }

 private:
  bool IsOp(const char* op) const { return t_[p_].t == Tok::T::Op && t_[p_].s == op; }
  NodeP Make(Kind k, std::string name, std::vector<NodeP> args) {
    auto n = std::make_shared<RtssEngine::Node>();
    n->kind = k;
    n->name = std::move(name);
    n->args = std::move(args);
    return n;
  }
  NodeP Ternary() {
    NodeP c = Binary(0);
    if (!c || !IsOp("?")) return c;
    ++p_;
    NodeP a = Ternary();
    if (!IsOp(":")) return nullptr;
    ++p_;
    NodeP b = Ternary();
    return a && b ? Make(Kind::Ternary, "?", {c, a, b}) : nullptr;
  }
  static int Prec(const std::string& op) {
    if (op == "||") return 1;
    if (op == "&&") return 2;
    if (op == "|") return 3;
    if (op == "&") return 4;
    if (op == "==" || op == "!=") return 5;
    if (op == "<" || op == "<=" || op == ">" || op == ">=") return 6;
    if (op == "+" || op == "-") return 7;
    if (op == "*" || op == "/" || op == "%") return 8;
    return -1;
  }
  NodeP Binary(int minPrec) {
    NodeP left = Unary();
    while (left && t_[p_].t == Tok::T::Op) {
      const std::string op = t_[p_].s;
      const int prec = Prec(op);
      if (prec < 0 || prec <= minPrec - 1 || prec < minPrec) break;
      ++p_;
      NodeP right = Binary(prec + 1);
      if (!right) return nullptr;
      left = Make(Kind::Binary, op, {left, right});
    }
    return left;
  }
  NodeP Unary() {
    if (IsOp("!") || IsOp("-") || IsOp("+")) {
      const std::string op = t_[p_++].s;
      NodeP a = Unary();
      return a ? Make(Kind::Unary, op, {a}) : nullptr;
    }
    return Primary();
  }
  NodeP Primary() {
    const Tok& t = t_[p_];
    if (t.t == Tok::T::Num) {
      ++p_;
      auto n = Make(Kind::Num, "", {});
      n->num = t.num;
      return n;
    }
    if (t.t == Tok::T::Str) {
      ++p_;
      return Make(Kind::Var, t.s, {});
    }
    if (t.t == Tok::T::Ident) {
      ++p_;
      if (IsOp("(")) {
        ++p_;
        std::vector<NodeP> args;
        if (!IsOp(")")) {
          while (true) {
            NodeP a = Ternary();
            if (!a) return nullptr;
            args.push_back(a);
            if (IsOp(",")) {
              ++p_;
              continue;
            }
            break;
          }
        }
        if (!IsOp(")")) return nullptr;
        ++p_;
        return Make(Kind::Call, ToLowerAscii(t.s), std::move(args));
      }
      return Make(Kind::Var, t.s, {});
    }
    if (IsOp("(")) {
      ++p_;
      NodeP n = Ternary();
      if (!IsOp(")")) return nullptr;
      ++p_;
      return n;
    }
    return nullptr;
  }
  std::vector<Tok> t_;
  size_t p_ = 0;
};

std::string Key(std::string s) {
  s = Trim(s);
  if (s.size() >= 2 && s.front() == '"' && s.back() == '"') s = s.substr(1, s.size() - 2);
  return ToLowerAscii(Trim(s));
}

int PhysicalCores() {
  static const int n = [] {
    DWORD len = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &len);
    std::vector<BYTE> buf(len);
    int cores = 0;
    if (GetLogicalProcessorInformationEx(RelationProcessorCore,
                                         reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buf.data()), &len)) {
      for (DWORD off = 0; off < len;) {
        auto* info = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buf.data() + off);
        ++cores;
        off += info->Size;
      }
    }
    return std::max(1, cores);
  }();
  return n;
}

int CpuVendor() {
  static const int v = [] {
    int r[4] = {};
    __cpuid(r, 0);
    char name[13] = {};
    memcpy(name, &r[1], 4);
    memcpy(name + 4, &r[3], 4);
    memcpy(name + 8, &r[2], 4);
    if (strcmp(name, "AuthenticAMD") == 0) return 0x1022;
    if (strcmp(name, "GenuineIntel") == 0) return 0x8086;
    return 0;
  }();
  return v;
}

bool IsFrameTimeName(const std::string& k) {
  return k == "frametime" || k == "msbetweenpresents" || k == "msbetweendisplaychange" || k == "msgpuactive" ||
         k == "msuntildisplayed";
}

double PercentileOf(std::vector<float> v, double p) {
  if (v.empty()) return 0;
  std::sort(v.begin(), v.end());
  const double idx = std::clamp(p / 100.0, 0.0, 1.0) * double(v.size() - 1);
  return v[size_t(std::lround(idx))];
}

}  // namespace

// ------------------------------------------------------------------ caricamento
void RtssEngine::Load(const RtssLayout& preset) {
  preset_ = &preset;
  sources_.clear();
  index_.clear();
  for (const auto& d : preset.defs) {
    Source s;
    s.def = d;
    if (!Trim(d.formula).empty()) {
      s.formula = Parser(Tokenize(d.formula)).Parse();
      if (!s.formula) LogWarn("RTSS: formula non valida in \"{}\": {}", d.name, d.formula);
    }
    index_.emplace(Key(d.name), sources_.size());
    sources_.push_back(std::move(s));
  }
  startTick_ = GetTickCount64();
  loaded_ = true;
}

RtssEngine::Source* RtssEngine::Find(const std::string& name) {
  const auto it = index_.find(Key(name));
  return it == index_.end() ? nullptr : &sources_[it->second];
}

void RtssEngine::Update(const SystemSnapshot& sys, const FrameStats* frames) {
  sys_ = sys;
  framesValid_ = frames && frames->valid;
  if (framesValid_) frames_ = *frames;
  nowMs_ = double(GetTickCount64() - startTick_);
  ++stamp_;
  for (auto& s : sources_) {
    const Val before = s.value;
    const Val v = Evaluate(s);
    if (v.ok) {
      s.history.push_back(float(v.v));
      if (s.history.size() > 600) s.history.pop_front();
    }
    s.previous = before;
  }
  for (auto& [name, h] : builtinHistory_) {
    const Val v = Builtin(name);
    if (v.ok) {
      h.push_back(float(v.v));
      if (h.size() > 600) h.pop_front();
    }
  }
}

// ------------------------------------------------------------------ dati
RtssEngine::Val RtssEngine::Builtin(const std::string& rawName) {
  const std::string k = Key(rawName);
  const SystemSnapshot& s = sys_;
  const FrameStats* f = Frames();
  auto opt = [](const std::optional<double>& v) { return v ? Val{*v, true} : Val{}; };
  auto num = [](double v) { return Val{v, true}; };
  auto sensor = [&](const std::string& id) -> Val {
    if (const SensorEntry* e = FindSensor(s, id)) return {e->value, true};
    return {};
  };

  if (k == "physicalcorecount") return num(PhysicalCores());
  if (k == "logicalcorecount") return num(double(GetActiveProcessorCount(ALL_PROCESSOR_GROUPS)));
  if (k == "cpuvendor") return num(CpuVendor());
  if (k == "gpuvendor") return num(s.gpu.vendorId);

  // Frame (HAL e PresentMon)
  if (k == "framerate" || k == "fps" || k == "frameratepresented" || k == "frameratedisplayed" ||
      k == "framerateapp")
    return f ? num(f->fps) : Val{};
  if (IsFrameTimeName(k)) return f ? num(f->frameTimeMs) : Val{};
  if (k.find("1dot0plow") != std::string::npos || k == "framerate 1% low")
    return f && !f->frameTimes.empty() ? num(1000.0 / PercentileOf(f->frameTimes, 99)) : Val{};
  if (k.find("0dot1plow") != std::string::npos || k == "framerate 0.1% low")
    return f && !f->frameTimes.empty() ? num(1000.0 / PercentileOf(f->frameTimes, 99.9)) : Val{};
  if (k == "framerate avg" || k == "frameratepresentedavg") return f ? num(f->fpsAvg) : Val{};
  if (k == "status") return f ? num(1) : Val{};

  // GPU
  const bool gpu = k.rfind("gpu1 ", 0) == 0 || k.rfind("gpu ", 0) == 0;
  const std::string g = gpu ? k.substr(k.find(' ') + 1) : "";
  if (gpu) {
    if (g == "usage") return opt(s.gpu.usage);
    if (g == "temperature") return opt(s.gpu.tempC);
    if (g == "power") return opt(s.gpu.powerW);
    if (g == "clock" || g == "core clock") return opt(s.gpu.clockMHz);
    if (g == "memory clock") return sensor("nv:0:clock2");
    if (g == "fan speed" || g == "fan speed 1") {
      const Val v = sensor("nv:0:fan0");
      return v.ok ? v : sensor("nv:0:fan");
    }
    if (g == "fan speed 2") return sensor("nv:0:fan1");
    if (g == "memory usage")
      return s.gpu.vramUsedGB ? num(*s.gpu.vramUsedGB * 1024.0) : Val{};
    if (g == "total memory")
      return s.gpu.vramTotalGB ? num(*s.gpu.vramTotalGB * 1024.0) : Val{};
    if (g == "memory usage percent")
      return s.gpu.vramUsedGB && s.gpu.vramTotalGB && *s.gpu.vramTotalGB > 0
                 ? num(100.0 * *s.gpu.vramUsedGB / *s.gpu.vramTotalGB)
                 : Val{};
    if (g == "power limit") return sensor("nv:0:powerlimit");
    if (g == "voltage") return {};
  }
  // CPU
  if (k == "cpu usage") return opt(s.cpu.usage);
  if (k == "cpu clock" || k == "averageeffectiveclock" || k == "average effective clock")
    return s.cpu.freqGHz ? num(*s.cpu.freqGHz * 1000.0) : Val{};
  if (k == "cpu temperature") return opt(s.cpuTempC);
  if (k == "cpu power") return opt(s.cpuPowerW);
  if (k.rfind("cpu", 0) == 0 && k.size() > 9 && k.ends_with(" usage")) {  // "cpu7 usage" (da 1)
    const int n = atoi(k.c_str() + 3);
    if (n >= 1) return sensor(std::format("cpu:core{}:usage", n - 1));
  }
  if (k.size() >= 2 && k[0] == 'c' && std::all_of(k.begin() + 1, k.end(), ::isdigit))  // C0..Cn: clock per core
    return sensor(std::format("cpu:core{}:clock", atoi(k.c_str() + 1)));
  // RAM
  constexpr double kMB = 1024.0;
  if (k == "ram usage") return s.ram.totalGB > 0 ? num(s.ram.usedGB * kMB) : Val{};
  if (k == "total ram") return s.ram.totalGB > 0 ? num(s.ram.totalGB * kMB) : Val{};
  if (k == "ram usage percent")
    return s.ram.totalGB > 0 ? num(100.0 * s.ram.usedGB / s.ram.totalGB) : Val{};
  // Batteria
  if (k == "battery charge level" || k == "battery level")
    return s.battery.present && s.battery.percent >= 0 ? num(s.battery.percent) : Val{};
  return {};
}

RtssEngine::Val RtssEngine::Raw(const Source& s) {
  const std::string provider = ToLowerAscii(s.def.provider);
  const std::string id = s.def.id;
  if (ToLowerAscii(id) == "timer") return {nowMs_, true};
  if (ToLowerAscii(id) == "stub") return {};
  if (provider == "hwinfo") {
    // HWiNFO (shared memory): stessa etichetta della lettura; altrimenti il dato equivalente di PerfOverlay.
    if (!s.def.reading.empty() && sys_.sensors)
      for (const auto& e : *sys_.sensors)
        if (e.id.rfind("hwi:", 0) == 0 && IEquals(e.name, s.def.reading)) return {e.value, true};
    const Val v = Builtin(s.def.name);
    return v.ok ? v : Builtin(s.def.reading);
  }
  Val v = Builtin(id.empty() ? s.def.name : id);
  if (!v.ok && !id.empty()) v = Builtin(s.def.name);
  return v;
}

RtssEngine::Val RtssEngine::Evaluate(Source& s) {
  if (s.evalStamp == stamp_) return s.value;
  if (s.evaluating) return {};  // riferimento circolare
  s.evaluating = true;
  const Val raw = Raw(s);
  s.value = s.formula ? Eval(*s.formula, &s, raw) : raw;
  if (s.value.ok && !std::isfinite(s.value.v)) s.value = {};
  s.evalStamp = stamp_;
  s.evaluating = false;
  return s.value;
}

RtssEngine::Val RtssEngine::Value(const std::string& name) {
  if (Source* s = Find(name)) return Evaluate(*s);
  return Builtin(name);
}

const std::deque<float>* RtssEngine::History(const std::string& name) {
  const std::string k = Key(name);
  if (IsFrameTimeName(k) || k == "framerate" || k == "fps") {
    // Grafici di frame: un punto per frame.
    static thread_local std::deque<float> tmp;
    tmp.clear();
    if (const FrameStats* f = Frames()) {
      const bool rate = k == "framerate" || k == "fps";
      const size_t n = std::min<size_t>(f->frameTimes.size(), 240);
      for (size_t i = f->frameTimes.size() - n; i < f->frameTimes.size(); ++i) {
        const float ft = f->frameTimes[i];
        tmp.push_back(rate ? (ft > 0 ? 1000.0f / ft : 0.0f) : ft);
      }
    }
    if (Source* s = Find(name); s && !s->formula && !tmp.empty()) return &tmp;
    if (!Find(name)) return &tmp;
  }
  if (Source* s = Find(name)) return &s->history;
  return &builtinHistory_[k];  // da ora in poi viene registrato a ogni Update
}

RtssEngine::Val RtssEngine::Percentile(const std::string& name, double p) {
  const std::string k = Key(name);
  if (IsFrameTimeName(k)) {
    const FrameStats* f = Frames();
    if (!f || f->frameTimes.empty()) return {};
    return {PercentileOf(f->frameTimes, p), true};
  }
  const auto* h = History(name);
  if (!h || h->empty()) return {};
  return {PercentileOf(std::vector<float>(h->begin(), h->end()), p), true};
}

RtssEngine::Val RtssEngine::SlidingWindow(const std::string& name, int n, int mode) {
  const std::string k = Key(name);
  std::vector<float> v;
  if (IsFrameTimeName(k)) {
    if (const FrameStats* f = Frames()) v.assign(f->frameTimes.begin(), f->frameTimes.end());
  } else if (const auto* h = History(name)) {
    v.assign(h->begin(), h->end());
  }
  if (v.empty()) return Value(name);
  n = std::clamp(n, 1, int(v.size()));
  // mode: 0 = media, 1 = massimo, 2 = minimo
  double acc = mode == 1 ? -1e300 : mode == 2 ? 1e300 : 0;
  for (size_t i = v.size() - size_t(n); i < v.size(); ++i)
    acc = mode == 1 ? std::max(acc, double(v[i])) : mode == 2 ? std::min(acc, double(v[i])) : acc + v[i];
  return {mode == 0 ? acc / n : acc, true};
}

RtssEngine::Val RtssEngine::Eval(const Node& n, Source* self, const Val& x) {
  switch (n.kind) {
    case Kind::Num:
      return {n.num, true};
    case Kind::Var: {
      if (n.name == "x" || n.name == "X") return x;
      if (Source* s = Find(n.name); s && s != self) return Evaluate(*s);
      return Builtin(n.name);
    }
    case Kind::Unary: {
      const Val a = Eval(*n.args[0], self, x);
      if (!a.ok) return {};
      if (n.name == "!") return {a.v == 0 ? 1.0 : 0.0, true};
      if (n.name == "-") return {-a.v, true};
      return a;
    }
    case Kind::Ternary: {
      const Val c = Eval(*n.args[0], self, x);
      if (!c.ok) return {};
      return Eval(*n.args[c.v != 0 ? 1 : 2], self, x);
    }
    case Kind::Binary: {
      const std::string& op = n.name;
      const Val a = Eval(*n.args[0], self, x);
      if (op == "&&" && a.ok && a.v == 0) return {0, true};
      if (op == "||" && a.ok && a.v != 0) return {1, true};
      const Val b = Eval(*n.args[1], self, x);
      if (!a.ok || !b.ok) return {};
      if (op == "+") return {a.v + b.v, true};
      if (op == "-") return {a.v - b.v, true};
      if (op == "*") return {a.v * b.v, true};
      if (op == "/") return b.v == 0 ? Val{} : Val{a.v / b.v, true};
      if (op == "%") return b.v == 0 ? Val{} : Val{std::fmod(a.v, b.v), true};
      if (op == "<") return {a.v < b.v ? 1.0 : 0.0, true};
      if (op == "<=") return {a.v <= b.v ? 1.0 : 0.0, true};
      if (op == ">") return {a.v > b.v ? 1.0 : 0.0, true};
      if (op == ">=") return {a.v >= b.v ? 1.0 : 0.0, true};
      if (op == "==") return {std::abs(a.v - b.v) < 1e-9 ? 1.0 : 0.0, true};
      if (op == "!=") return {std::abs(a.v - b.v) >= 1e-9 ? 1.0 : 0.0, true};
      if (op == "&&") return {a.v != 0 && b.v != 0 ? 1.0 : 0.0, true};
      if (op == "||") return {a.v != 0 || b.v != 0 ? 1.0 : 0.0, true};
      if (op == "&") return {double(int64_t(a.v) & int64_t(b.v)), true};
      if (op == "|") return {double(int64_t(a.v) | int64_t(b.v)), true};
      return {};
    }
    case Kind::Call: {
      const std::string& f = n.name;
      auto arg = [&](size_t i) { return i < n.args.size() ? Eval(*n.args[i], self, x) : Val{}; };
      auto argName = [&](size_t i) -> std::string {
        if (i >= n.args.size()) return {};
        const Node& a = *n.args[i];
        if (a.kind == Kind::Var) return (a.name == "x" && self) ? self->def.name : a.name;
        return {};
      };
      if (f == "validate") return {arg(0).ok ? 1.0 : 0.0, true};
      if (f == "if") {
        const Val c = arg(0);
        if (!c.ok) return {};
        return arg(c.v != 0 ? 1 : 2);
      }
      if (f == "min" || f == "max") {
        Val r;
        for (size_t i = 0; i < n.args.size(); ++i) {
          const Val v = arg(i);
          if (!v.ok) return {};
          r = !r.ok ? v : Val{f == "min" ? std::min(r.v, v.v) : std::max(r.v, v.v), true};
        }
        return r;
      }
      if (f == "abs" || f == "round" || f == "floor" || f == "ceil" || f == "sqrt" || f == "int") {
        const Val v = arg(0);
        if (!v.ok) return {};
        if (f == "abs") return {std::abs(v.v), true};
        if (f == "round") return {std::round(v.v), true};
        if (f == "floor" || f == "int") return {std::floor(v.v), true};
        if (f == "ceil") return {std::ceil(v.v), true};
        return v.v < 0 ? Val{} : Val{std::sqrt(v.v), true};
      }
      if (f == "pow") {
        const Val a = arg(0), b = arg(1);
        return a.ok && b.ok ? Val{std::pow(a.v, b.v), true} : Val{};
      }
      if (f == "percentile") {
        const Val p = arg(1);
        return p.ok ? Percentile(argName(0), p.v) : Val{};
      }
      if (f == "swavg" || f == "swmax" || f == "swmin") {
        const Val w = arg(1);
        if (!w.ok) return {};
        return SlidingWindow(argName(0), int(w.v), f == "swmax" ? 1 : f == "swmin" ? 2 : 0);
      }
      if (f == "buf") return self ? self->previous : Val{};  // valore precedente della sorgente
      // reflexlatency / presentmonlatency e altre funzioni dei driver: dato non disponibile
      return {};
    }
  }
  return {};
}

// ------------------------------------------------------------------ testo e condizioni
std::wstring RtssEngine::Formatted(const std::string& name) {
  const Val v = Value(name);
  if (!v.ok) return L"N/A";
  int decimals = 0;
  if (const Source* s = Find(name)) {
    const std::string& fmt = s->def.format;
    if (const auto dot = fmt.find('.'); dot != std::string::npos && dot + 1 < fmt.size() && isdigit(uint8_t(fmt[dot + 1])))
      decimals = fmt[dot + 1] - '0';
  }
  return std::format(L"{:.{}f}", v.v, std::clamp(decimals, 0, 4));
}

bool RtssEngine::Knows(const std::string& name) { return Find(name) != nullptr || Builtin(name).ok; }

bool RtssEngine::Truthy(const std::string& condition) {
  std::string c = Trim(condition);
  bool negate = false;
  while (!c.empty() && c[0] == '!') {
    negate = !negate;
    c = Trim(c.substr(1));
  }
  if (c.empty()) return !negate;
  const Val v = Value(c);
  const bool t = v.ok && v.v != 0;
  return negate ? !t : t;
}

// ------------------------------------------------------------------ colori
namespace {
bool ParseHexColor(std::string s, ArgbColor& out) {
  s = Trim(s);
  if (s.empty() || s.size() > 8 || !std::all_of(s.begin(), s.end(), ::isxdigit)) return false;
  const unsigned long v = std::stoul(s, nullptr, 16);
  out.r = ((v >> 16) & 0xFF) / 255.0f;
  out.g = ((v >> 8) & 0xFF) / 255.0f;
  out.b = (v & 0xFF) / 255.0f;
  out.a = s.size() > 6 ? ((v >> 24) & 0xFF) / 255.0f : 1.0f;  // senza canale alfa = opaco
  return true;
}
}  // namespace

ArgbColor ParseRtssColor(const std::string& spec, RtssEngine& engine, ArgbColor fallback) {
  ArgbColor c = fallback;
  if (spec.empty()) return c;
  const auto paren = spec.find('(');
  if (paren == std::string::npos) {
    ParseHexColor(Split(spec, ',')[0], c);
    return c;
  }
  // Soglie: colore, minimo, massimo, ... (sorgente, flag)
  const std::vector<std::string> t = Split(spec.substr(0, paren), ',');
  struct Band {
    ArgbColor c;
    std::optional<double> lo, hi;
  };
  std::vector<Band> bands;
  auto numOpt = [](const std::string& s) -> std::optional<double> {
    const std::string v = Trim(s);
    if (v.empty()) return std::nullopt;
    try {
      return std::stod(v);
    } catch (...) {
      return std::nullopt;
    }
  };
  for (size_t i = 0; i < t.size(); i += 3) {
    Band b{fallback};
    if (!ParseHexColor(t[i], b.c)) continue;
    if (i + 1 < t.size()) b.lo = numOpt(t[i + 1]);
    if (i + 2 < t.size()) b.hi = numOpt(t[i + 2]);
    bands.push_back(b);
  }
  if (bands.empty()) return c;
  std::string inner = spec.substr(paren + 1);
  if (const auto close = inner.find(')'); close != std::string::npos) inner = inner.substr(0, close);
  const auto comma = inner.rfind(',');
  const std::string src = comma == std::string::npos ? inner : inner.substr(0, comma);
  const int flags = comma == std::string::npos ? 0 : atoi(inner.c_str() + comma + 1);
  const auto v = engine.Value(src);
  if (!v.ok) return bands.front().c;
  for (size_t i = 0; i < bands.size(); ++i) {
    const Band& b = bands[i];
    if ((b.lo && v.v < *b.lo) || (b.hi && v.v >= *b.hi)) continue;
    if ((flags & 1) && b.lo && b.hi && *b.hi > *b.lo && i + 1 < bands.size()) {  // sfumatura verso il colore dopo
      const float k = float((v.v - *b.lo) / (*b.hi - *b.lo));
      const ArgbColor& n = bands[i + 1].c;
      return {b.c.r + (n.r - b.c.r) * k, b.c.g + (n.g - b.c.g) * k, b.c.b + (n.b - b.c.b) * k,
              b.c.a + (n.a - b.c.a) * k};
    }
    return b.c;
  }
  return v.v < bands.front().lo.value_or(-1e300) ? bands.front().c : bands.back().c;
}

}  // namespace po
