#include "machine_ir.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <numeric>
#include <set>
#include <unordered_set>

namespace sysy {
namespace {
bool fp(ValueType type) { return type == ValueType::Float || type == ValueType::Double; }
int64_t align(int64_t size, int64_t alignment) { return (size + alignment - 1) / alignment * alignment; }
bool small(int64_t value) { return value >= -2048 && value <= 2047; }

struct Argument { int reg = -1; bool floating = false; int64_t offset = 0; };
std::vector<Argument> arguments(const std::vector<ValueType> &types, size_t named) {
  int integer = 0, floating = 0; int64_t stack = 0;
  std::vector<Argument> result;
  for (size_t i = 0; i < types.size(); ++i) {
    Argument a;
    if (i < named && fp(types[i]) && floating < 8) { a.reg = floating++; a.floating = true; }
    else if (integer < 8) a.reg = integer++;
    else { a.offset = stack; stack += 8; }
    result.push_back(a);
  }
  return result;
}

struct Allocation {
  std::vector<int> registers;
  std::vector<int64_t> spills;
  std::vector<const Instruction *> definitions;
  std::vector<const Instruction *> original;
  std::vector<int> uses;
  std::unordered_map<const Symbol *, int64_t> objects;
  std::set<int> integers, floats;
  int64_t frame = 0, saves = 0;
  bool leaf = true;
};

Allocation allocate(const FunctionIR &f) {
  const size_t count = f.values.size(), blocks = f.blocks.size();
  Allocation result;
  result.registers.assign(count, -1); result.spills.assign(count, -1); result.definitions.assign(count, nullptr);
  std::vector<int> definitionCount(count, 0);
  std::vector<int> sites(count, 0);
  auto depth = loopDepth(f);
  std::vector<double> weight(count, 0);
  std::vector<std::unordered_set<int>> use(blocks), def(blocks), in(blocks), out(blocks);
  for (size_t b = 0; b < blocks; ++b) {
    for (const auto &i : f.blocks[b].instructions) {
      result.leaf &= i.op != Op::Call;
      for (int arg : i.args) {
        if (!def[b].count(arg)) use[b].insert(arg);
        weight[arg] += std::pow(10., std::min(depth[b], 6));
      }
      if (i.result >= 0) {
        def[b].insert(i.result); result.definitions[i.result] = &i; ++definitionCount[i.result];
        sites[i.result] = b; weight[i.result] += std::pow(10., std::min(depth[b], 6));
      }
    }
  }
  result.original = result.definitions;
  result.uses.assign(count, 0);
  for (const auto &b : f.blocks) for (const auto &i : b.instructions) for (int a : i.args) ++result.uses[a];
  for (size_t v = 0; v < count; ++v) if (definitionCount[v] != 1) result.original[v] = nullptr;
  std::vector<bool> cached(count, false);
  for (size_t b = 0; b < blocks; ++b) for (const auto &i : f.blocks[b].instructions) for (int a : i.args) {
    const auto *source = result.original[a];
    if (!source || depth[b] <= depth[sites[a]]) continue;
    if (source->op == Op::Address && i.op == Op::Offset) cached[a] = true;
    if (source->op == Op::Constant && source->immediate != 0 &&
        (fp(f.values[a]) || (!small(source->immediate) && i.op >= Op::Eq && i.op <= Op::Ge))) cached[a] = true;
  }
  for (size_t v = 0; v < count; ++v) {
    const auto *i = result.definitions[v];
    if (definitionCount[v] != 1 || cached[v] || !i || (i->op != Op::Constant && i->op != Op::Address && i->op != Op::String)) result.definitions[v] = nullptr;
  }
  auto tracked = [&](int v) { return result.definitions[v] == nullptr; };
  bool changed;
  do {
    changed = false;
    for (size_t reverse = blocks; reverse > 0; --reverse) {
      size_t b = reverse - 1;
      auto oldIn = in[b], oldOut = out[b];
      const auto &last = f.blocks[b].instructions.back();
      for (int successor : {last.target, last.otherwise}) if (successor >= 0)
        out[b].insert(in[successor].begin(), in[successor].end());
      in[b] = use[b];
      for (int v : out[b]) if (!def[b].count(v)) in[b].insert(v);
      changed |= oldIn != in[b] || oldOut != out[b];
    }
  } while (changed);
  std::vector<std::unordered_set<int>> interference(count);
  std::vector<std::vector<int>> hints(count);
  std::vector<bool> crossesCall(count, false);
  for (size_t b = 0; b < blocks; ++b) {
    auto live = out[b];
    for (auto it = f.blocks[b].instructions.rbegin(); it != f.blocks[b].instructions.rend(); ++it) {
      const auto &i = *it;
      if (i.op == Op::Call) for (int v : live) if (v != i.result) crossesCall[v] = true;
      if (i.result >= 0 && tracked(i.result)) {
        int source = i.op == Op::Move ? i.args[0] : -1;
        if (source >= 0 && tracked(source)) { hints[i.result].push_back(source); hints[source].push_back(i.result); }
        for (int v : live) if (v != i.result && v != source && tracked(v) && fp(f.values[v]) == fp(f.values[i.result])) {
          interference[v].insert(i.result); interference[i.result].insert(v);
        }
        live.erase(i.result);
      }
      for (int v : i.args) if (tracked(v)) live.insert(v);
    }
  }
  std::vector<ValueType> parameterTypes;
  for (const auto &p : f.source->parameters) parameterTypes.push_back(p.symbol->type.array() ? ValueType::Pointer : p.base == Base::Float ? ValueType::Float : ValueType::Int);
  auto incoming = arguments(parameterTypes, parameterTypes.size());
  auto preferred = [&](int v) {
    const auto *i = result.original[v];
    if (!i || i->op != Op::Parameter) return -1;
    auto a = incoming.at(i->immediate);
    if (a.reg < 0 || (fp(f.values[v]) && !a.floating)) return -1;
    return a.reg + (fp(f.values[v]) ? 21 : 13);
  };
  auto allowed = [&](int v, int r) {
    if (r >= 12 && crossesCall[v]) return false;
    const auto *i = result.original[v];
    if (i && i->op == Op::Parameter && r >= (fp(f.values[v]) ? 21 : 13) && r != preferred(v)) return false;
    return true;
  };
  for (bool floating : {false, true}) {
    int colors = floating ? 29 : 21;
    std::vector<int> parent(count); std::iota(parent.begin(), parent.end(), 0);
    auto leader = [&](int v) { while (v != parent[v]) v = parent[v]; return v; };
    std::vector<uint32_t> masks(count, 0);
    std::vector<std::set<int>> conflicts(count);
    std::set<int> pending;
    for (size_t v = 0; v < count; ++v) if (fp(f.values[v]) == floating && tracked(v) && weight[v] > 0) {
      pending.insert(v);
      for (int r = 0; r < colors; ++r) if (allowed(v, r)) masks[v] |= uint32_t{1} << r;
      conflicts[v].insert(interference[v].begin(), interference[v].end());
    }
    auto population = [](uint32_t mask) { int n = 0; for (; mask; mask &= mask-1) ++n; return n; };
    std::vector<std::pair<int, int>> moves;
    for (int v : pending) for (int other : hints[v]) if (v < other) moves.push_back({v, other});
    std::stable_sort(moves.begin(), moves.end(), [&](auto a, auto b) { return weight[a.first]+weight[a.second] > weight[b.first]+weight[b.second]; });
    for (auto pair : moves) {
      int a = leader(pair.first), b = leader(pair.second);
      uint32_t mask = masks[a] & masks[b];
      if (a == b || !mask || conflicts[a].count(b)) continue;
      auto neighbors = conflicts[a]; neighbors.insert(conflicts[b].begin(), conflicts[b].end());
      int high = 0, available = population(mask);
      for (int v : neighbors) high += conflicts[v].size() >= static_cast<size_t>(available);
      if (high >= available) continue;
      // Conservative Briggs coalescing with the intersection of ABI masks.
      parent[b] = a; pending.erase(b); masks[a] = mask; weight[a] += weight[b];
      conflicts[a] = std::move(neighbors); conflicts[b].clear();
      for (int v : conflicts[a]) { conflicts[v].erase(b); conflicts[v].insert(a); }
    }
    std::vector<int> degree(count), order;
    for (int v : pending) degree[v] = conflicts[v].size();
    while (!pending.empty()) {
      int selected = -1; double cheapest = std::numeric_limits<double>::infinity();
      for (int v : pending) {
        int available = population(masks[v]);
        if (degree[v] < available) { selected = v; break; }
        double cost = weight[v] / std::max(1, degree[v]);
        if (cost < cheapest) { cheapest = cost; selected = v; }
      }
      pending.erase(selected); order.push_back(selected);
      for (int v : conflicts[selected]) if (pending.count(v)) --degree[v];
    }
    for (auto it = order.rbegin(); it != order.rend(); ++it) {
      int v = *it; std::set<int> occupied;
      for (int neighbor : conflicts[v]) if (result.registers[neighbor] >= 0) occupied.insert(result.registers[neighbor]);
      auto choose = [&](int r) { if (r >= 0 && r < colors && (masks[v] & (uint32_t{1} << r)) && !occupied.count(r)) { result.registers[v] = r; return true; } return false; };
      if (choose(preferred(v))) continue;
      bool chosen = false;
      for (int hint : hints[v]) if (choose(result.registers[leader(hint)])) { chosen = true; break; }
      if (chosen) continue;
      for (int r = 12; r < colors && !chosen; ++r) chosen = choose(r);
      for (int r = 0; r < 12 && !chosen; ++r) chosen = choose(r);
    }
    for (size_t v = 0; v < count; ++v) if (fp(f.values[v]) == floating && tracked(v)) result.registers[v] = result.registers[leader(v)];
  }
  int64_t offset = 0;
  for (const auto &b : f.blocks) for (const auto &i : b.instructions) if (i.op == Op::Call) {
    std::vector<ValueType> types;
    for (int arg : i.args) types.push_back(f.values[arg]);
    auto abi = arguments(types, i.callee->timer ? 1 : i.callee->parameters.size());
    for (auto a : abi) if (a.reg < 0) offset = std::max(offset, a.offset + 8);
  }
  for (auto s : f.objects) { offset = align(offset, 4); result.objects[s] = offset; offset += s->type.elements() * 4; }
  offset = align(offset, 8);
  for (size_t v = 0; v < count; ++v) {
    if (result.definitions[v] || weight[v] == 0) continue;
    if (result.registers[v] < 0) { result.spills[v] = offset; offset += 8; }
    else if (result.registers[v] < 12) (fp(f.values[v]) ? result.floats : result.integers).insert(result.registers[v]);
  }
  result.saves = offset;
  result.frame = align(offset + (result.integers.size() + result.floats.size() + (result.leaf ? 0 : 1)) * 8, 16);
  return result;
}

class Assembly {
  const ModuleIR &module;
  std::ostream &out;
  const FunctionIR *function = nullptr;
  Allocation allocation;
  size_t functionId = 0;
  int privateLabel = 0;
  std::vector<Argument> incoming;
  int nextEmittedBlock = 0;
  std::vector<int> emissionTargets;
  std::optional<int64_t> scratchConstant;

  void line(const std::string &text) { out << "\t" << text << '\n'; }
  void literalT2(int64_t value) {
    if (!scratchConstant || *scratchConstant != value) line("li t2, " + std::to_string(value));
    scratchConstant = value;
  }
  std::string label(int block) const { return ".L" + std::to_string(functionId) + "_b" + std::to_string(block); }
  std::string reg(int id) const {
    int r = allocation.registers[id];
    if (fp(function->values[id])) return r < 12 ? "fs" + std::to_string(r) : r < 21 ? "ft" + std::to_string(r - 9) : "fa" + std::to_string(r - 21);
    return r < 12 ? "s" + std::to_string(r) : r == 12 ? "t5" : "a" + std::to_string(r - 13);
  }
  std::string stringLabel(int id) const { return ".Lstr" + std::to_string(functionId) + '_' + std::to_string(id); }
  void memory(const std::string &op, const std::string &r, int64_t offset, const std::string &base = "sp") {
    if (small(offset)) line(op + " " + r + ", " + std::to_string(offset) + "(" + base + ")");
    else {
      line("li t6, " + std::to_string(offset)); line("add t6, " + base + ", t6"); line(op + " " + r + ", 0(t6)");
    }
  }
  void move(const std::string &destination, const std::string &source, ValueType type) {
    if (destination != source) line(std::string(type == ValueType::Float ? "fmv.s " : type == ValueType::Double ? "fmv.d " : "mv ") + destination + ", " + source);
  }
  void objectAddress(const Symbol *s, int64_t offset, const std::string &destination) {
    if (s->global) {
      line("la " + destination + ", " + s->name);
      if (offset == 0) return;
      if (small(offset)) line("addi " + destination + ", " + destination + ", " + std::to_string(offset));
      else { line("li t6, " + std::to_string(offset)); line("add " + destination + ", " + destination + ", t6"); }
    } else {
      offset += allocation.objects.at(s);
      if (small(offset)) line("addi " + destination + ", sp, " + std::to_string(offset));
      else { line("li " + destination + ", " + std::to_string(offset)); line("add " + destination + ", sp, " + destination); }
    }
  }
  std::string read(int id, const std::string &scratch) {
    auto def = allocation.definitions[id];
    if (def) {
      if (def->op == Op::Constant) {
        if (def->immediate == 0) {
          if (!fp(function->values[id])) return "zero";
          line(std::string(function->values[id] == ValueType::Double ? "fmv.d.x " : "fmv.w.x ") + scratch + ", zero"); return scratch;
        }
        if (fp(function->values[id])) {
          bool wide = function->values[id] == ValueType::Double;
          line("li t4, " + std::to_string(wide ? def->immediate : static_cast<int32_t>(def->immediate)));
          line(std::string(wide ? "fmv.d.x " : "fmv.w.x ") + scratch + ", t4");
        } else line("li " + scratch + ", " + std::to_string(def->immediate));
      } else if (def->op == Op::String) line("la " + scratch + ", " + stringLabel(id));
      else objectAddress(def->symbol, def->immediate, scratch);
      return scratch;
    }
    if (allocation.registers[id] >= 0) return reg(id);
    auto type = function->values[id];
    memory(type == ValueType::Float ? "flw" : type == ValueType::Double ? "fld" : type == ValueType::Int ? "lw" : "ld", scratch, allocation.spills[id]);
    return scratch;
  }
  std::string destination(int id) const {
    if (allocation.registers[id] >= 0) return reg(id);
    return fp(function->values[id]) ? "ft2" : "t3";
  }
  void finish(int id, const std::string &r) {
    if (allocation.registers[id] >= 0) return;
    auto type = function->values[id];
    memory(type == ValueType::Float ? "fsw" : type == ValueType::Double ? "fsd" : type == ValueType::Int ? "sw" : "sd", r, allocation.spills[id]);
  }
  void loadStore(const Instruction &i) {
    int ptr = i.args[0]; bool load = i.op == Op::Load;
    int data = load ? i.result : i.args[1]; auto type = function->values[data];
    std::string r = load ? destination(data) : read(data, fp(type) ? "ft1" : "t1");
    std::string op = load ? (type == ValueType::Float ? "flw" : "lw") : (type == ValueType::Float ? "fsw" : "sw");
    auto def = allocation.definitions[ptr];
    if (def && def->op == Op::Address && !def->symbol->global)
      memory(op, r, allocation.objects.at(def->symbol) + def->immediate);
    else memory(op, r, 0, read(ptr, "t0"));
    if (load) finish(data, r);
  }
  void parameter(const Instruction &i) {
    auto location = incoming.at(i.immediate); auto type = function->values[i.result];
    auto dst = destination(i.result);
    if (location.reg < 0) memory(type == ValueType::Float ? "flw" : type == ValueType::Pointer ? "ld" : "lw", dst, allocation.frame + location.offset);
    else if (fp(type) && !location.floating) line("fmv.w.x " + dst + ", a" + std::to_string(location.reg));
    else move(dst, (location.floating ? "fa" : "a") + std::to_string(location.reg), type);
    finish(i.result, dst);
  }
  void call(const Instruction &i) {
    std::vector<ValueType> types;
    for (int arg : i.args) types.push_back(function->values[arg]);
    auto abi = arguments(types, i.callee->timer ? 1 : i.callee->parameters.size());
    // Stack arguments must consume their sources before register arguments
    // overwrite any caller-saved register. Register arguments form a parallel
    // copy, including mixed-bank transfers for floating variadic arguments.
    struct Copy { int value; ValueType type; std::string dst, src; };
    std::vector<Copy> copies;
    for (size_t n = 0; n < i.args.size(); ++n) {
      auto type = types[n]; auto location = abi[n];
      if (location.reg < 0) {
        auto src = read(i.args[n], fp(type) ? "ft0" : "t0");
        memory(type == ValueType::Float ? "fsw" : type == ValueType::Double ? "fsd" : "sd", src, location.offset);
      } else {
        auto dst = (location.floating ? "fa" : "a") + std::to_string(location.reg);
        std::string src;
        if (!allocation.definitions[i.args[n]] && allocation.registers[i.args[n]] >= 0) src = reg(i.args[n]);
        if (src != dst) copies.push_back({i.args[n], type, dst, src});
      }
    }
    while (!copies.empty()) {
      auto ready = std::find_if(copies.begin(), copies.end(), [&](const Copy &c) {
        return std::none_of(copies.begin(), copies.end(), [&](const Copy &other) { return other.src == c.dst; });
      });
      if (ready == copies.end()) {
        // Break a cycle with a reserved register in the destination bank.
        auto dst = copies.front().dst;
        bool floating = dst[0] == 'f'; auto scratch = floating ? "ft0" : "t0";
        move(scratch, dst, floating ? ValueType::Double : ValueType::Pointer);
        for (auto &c : copies) if (c.src == dst) c.src = scratch;
        continue;
      }
      auto c = *ready; copies.erase(ready);
      auto src = c.src;
      if (src.empty()) {
        // Rematerialization needs a distinct scratch while a saved cycle is
        // pending; ft1/t1 are also reserved by the allocator.
        src = read(c.value, fp(c.type) ? "ft1" : "t1");
      }
      if (fp(c.type) && c.dst[0] != 'f')
        line(std::string(c.type == ValueType::Double ? "fmv.x.d " : "fmv.x.w ") + c.dst + ", " + src);
      else move(c.dst, src, c.type);
    }
    line("call " + i.callee->linkageName);
    if (i.result >= 0 && allocation.uses[i.result]) {
      auto dst = destination(i.result); auto type = function->values[i.result];
      move(dst, fp(type) ? "fa0" : "a0", type); finish(i.result, dst);
    }
  }
  bool integerImmediate(const Instruction &i, const std::string &dst, const std::string &a) {
    if (i.args.size() != 2 || function->values[i.result] != ValueType::Int) return false;
    auto constant = allocation.original[i.args[1]];
    if (!constant || constant->op != Op::Constant) return false;
    int32_t d = constant->immediate;
    if (i.op == Op::And && small(d)) { line("andi " + dst + ", " + a + ", " + std::to_string(d)); return true; }
    if (i.op == Op::Shr) { line("srliw " + dst + ", " + a + ", " + std::to_string(d & 31)); return true; }
    if ((i.op == Op::Add && small(d)) || (i.op == Op::Sub && small(-static_cast<int64_t>(d)))) {
      line("addiw " + dst + ", " + a + ", " + std::to_string(i.op == Op::Add ? d : -static_cast<int64_t>(d))); return true;
    }
    uint64_t magnitude = d < 0 ? -static_cast<int64_t>(d) : d;
    bool power = magnitude && (magnitude & (magnitude - 1)) == 0;
    int shift = 0; if (power) for (auto n = magnitude; n > 1; n >>= 1) ++shift;
    if (i.op == Op::Mul) {
      if (d == 0) { move(dst, "zero", ValueType::Int); return true; }
      if (power) {
        if (shift) line("slliw " + dst + ", " + a + ", " + std::to_string(shift));
        else move(dst, a, ValueType::Int);
        if (d < 0 && shift != 31) line("negw " + dst + ", " + dst);
        return true;
      }
      return false;
    }
    if ((i.op != Op::Div && i.op != Op::Rem) || d == 0) return false;
    scratchConstant.reset();
    if (power) {
      if (shift == 0) {
        if (i.op == Op::Rem) move(dst, "zero", ValueType::Int);
        else if (d < 0) line("negw " + dst + ", " + a);
        else move(dst, a, ValueType::Int);
        return true;
      }
      line("sraiw t2, " + a + ", 31");
      line("srliw t2, t2, " + std::to_string(32-shift));
      line("addw t2, " + a + ", t2"); line("sraiw t2, t2, " + std::to_string(shift));
      if (i.op == Op::Rem) { line("slliw t2, t2, " + std::to_string(shift)); line("subw " + dst + ", " + a + ", t2"); }
      else if (d < 0) line("negw " + dst + ", t2");
      else move(dst, "t2", ValueType::Int);
      return true;
    }
    // Signed reciprocal multiplication. Generate the multiplier with integer
    // remainders, then correct the high product to truncation toward zero.
    uint64_t half = uint64_t{1} << 31, t = half + (d < 0);
    uint64_t anchor = t - 1 - t % magnitude, q1 = half / anchor, r1 = half % anchor;
    uint64_t q2 = half / magnitude, r2 = half % magnitude, delta;
    int p = 31;
    do {
      ++p; q1 *= 2; r1 *= 2; if (r1 >= anchor) { ++q1; r1 -= anchor; }
      q2 *= 2; r2 *= 2; if (r2 >= magnitude) { ++q2; r2 -= magnitude; }
      delta = magnitude - r2;
    } while (q1 < delta || (q1 == delta && r1 == 0));
    uint32_t raw = q2 + 1; if (d < 0) raw = 0u - raw;
    int32_t multiplier = raw;
    line("li t2, " + std::to_string(multiplier)); line("mul t2, " + a + ", t2"); line("srai t2, t2, 32");
    if (d > 0 && multiplier < 0) line("addw t2, t2, " + a);
    if (d < 0 && multiplier > 0) line("subw t2, t2, " + a);
    if (p > 32) line("sraiw t2, t2, " + std::to_string(p-32));
    line("srliw t4, t2, 31"); line("addw t2, t2, t4");
    if (i.op == Op::Rem) { line("li t4, " + std::to_string(d)); line("mulw t2, t2, t4"); line("subw " + dst + ", " + a + ", t2"); }
    else move(dst, "t2", ValueType::Int);
    return true;
  }
  void instruction(const Instruction &i) {
    if (i.op == Op::Constant || i.op == Op::Address || i.op == Op::String) {
      if (allocation.definitions[i.result]) return;
      auto dst = destination(i.result);
      if (i.op == Op::Address) objectAddress(i.symbol, i.immediate, dst);
      else if (i.op == Op::String) line("la " + dst + ", " + stringLabel(i.result));
      else if (fp(function->values[i.result])) {
        bool wide = function->values[i.result] == ValueType::Double;
        line("li t4, " + std::to_string(wide ? i.immediate : static_cast<int32_t>(i.immediate)));
        line(std::string(wide ? "fmv.d.x " : "fmv.w.x ") + dst + ", t4");
      } else line("li " + dst + ", " + std::to_string(i.immediate));
      finish(i.result, dst); return;
    }
    if (i.op == Op::Parameter) { parameter(i); return; }
    if (i.op == Op::Load || i.op == Op::Store) { loadStore(i); return; }
    if (i.op == Op::Call) { scratchConstant.reset(); call(i); return; }
    if (i.op == Op::Jump) { int target = emissionTargets[i.target]; if (target != nextEmittedBlock) line("j " + label(target)); return; }
    if (i.op == Op::Branch || i.op == Op::BranchCompare) {
      bool fused = i.op == Op::BranchCompare;
      int yes = emissionTargets[i.target], no = emissionTargets[i.otherwise];
      bool inverse = yes == nextEmittedBlock;
      int taken = inverse ? no : yes, fall = inverse ? yes : no;
      if (fused) {
        auto a = read(i.args[0], "t0"), b = read(i.args[1], "t1");
        std::string op;
        switch (static_cast<Op>(i.immediate)) {
        case Op::Eq: op = inverse ? "bne" : "beq"; break;
        case Op::Ne: op = inverse ? "beq" : "bne"; break;
        case Op::Lt: op = inverse ? "bge" : "blt"; break;
        case Op::Ge: op = inverse ? "blt" : "bge"; break;
        case Op::Gt: op = inverse ? "bge" : "blt"; std::swap(a,b); break;
        default: op = inverse ? "blt" : "bge"; std::swap(a,b); break;
        }
        line(op + " " + a + ", " + b + ", " + label(taken));
      } else {
        auto test = read(i.args[0], "t0"); line(std::string(inverse ? "beqz " : "bnez ") + test + ", " + label(taken));
      }
      if (fall != nextEmittedBlock) line("j " + label(fall));
      return;
    }
    if (i.op == Op::Return) {
      if (!i.args.empty()) { auto type = function->values[i.args[0]]; std::string dst = fp(type) ? "fa0" : "a0"; move(dst, read(i.args[0], dst), type); }
      if (allocation.frame == 0) line("ret");
      else line("j .L" + std::to_string(functionId) + "_return");
      return;
    }
    if (i.op == Op::Zero) {
      if (i.immediate == 0) return;
      move("t0", read(i.args[0], "t0"), ValueType::Pointer);
      if (i.immediate <= 32) { for (int64_t offset = 0; offset < i.immediate; offset += 4) memory("sw", "zero", offset, "t0"); return; }
      line("li t1, " + std::to_string(i.immediate / 4));
      std::string loop = ".Lzero" + std::to_string(functionId) + '_' + std::to_string(privateLabel++);
      out << loop << ":\n"; line("sw zero, 0(t0)"); line("addi t0, t0, 4"); line("addi t1, t1, -1"); line("bnez t1, " + loop); return;
    }
    auto type = function->values[i.result]; auto dst = destination(i.result);
    auto aType = function->values[i.args[0]];
    auto a = read(i.args[0], fp(aType) ? "ft0" : "t0");
    if (i.op == Op::Move) move(dst, a, type);
    else if (i.op == Op::Convert) {
      if (type == ValueType::Int) line("fcvt.w.s " + dst + ", " + a + ", rtz");
      else if (type == ValueType::Double) line("fcvt.d.s " + dst + ", " + a);
      else line("fcvt.s.w " + dst + ", " + a);
    } else if (i.op == Op::Neg) line(std::string(fp(type) ? "fneg.s " : "negw ") + dst + ", " + a);
    else {
      auto bType = function->values[i.args[1]];
      if (integerImmediate(i, dst, a)) { finish(i.result, dst); return; }
      auto constant = allocation.original[i.args[1]];
      bool immediate = i.op == Op::Offset && constant && constant->op == Op::Constant;
      auto b = immediate ? std::string() : read(i.args[1], fp(bType) ? "ft1" : "t1");
      if (i.op == Op::Offset) {
        auto def = allocation.original[i.args[1]];
        if (def && def->op == Op::Constant) {
          int64_t delta = def->immediate * i.immediate;
          if (small(delta)) line("addi " + dst + ", " + a + ", " + std::to_string(delta));
          else { literalT2(delta); line("add " + dst + ", " + a + ", t2"); }
        } else if (i.immediate == 0) move(dst, a, type);
        else {
          if ((i.immediate & (i.immediate - 1)) == 0) {
            int shift = 0; for (int64_t n = i.immediate; n > 1; n >>= 1) ++shift;
            line("slli t2, " + b + ", " + std::to_string(shift));
          } else { line("li t2, " + std::to_string(i.immediate)); line("mul t2, " + b + ", t2"); }
          line("add " + dst + ", " + a + ", t2");
          scratchConstant.reset();
        }
      } else if (i.op >= Op::Eq && i.op <= Op::Ge) {
        if (fp(aType)) {
          if (i.op == Op::Eq || i.op == Op::Ne) line("feq.s " + dst + ", " + a + ", " + b);
          else if (i.op == Op::Lt) line("flt.s " + dst + ", " + a + ", " + b);
          else if (i.op == Op::Le) line("fle.s " + dst + ", " + a + ", " + b);
          else if (i.op == Op::Gt) line("flt.s " + dst + ", " + b + ", " + a);
          else line("fle.s " + dst + ", " + b + ", " + a);
          if (i.op == Op::Ne) line("xori " + dst + ", " + dst + ", 1");
        } else {
          if (i.op == Op::Eq || i.op == Op::Ne) {
            line("xor " + dst + ", " + a + ", " + b); line(std::string(i.op == Op::Eq ? "seqz " : "snez ") + dst + ", " + dst);
          } else {
            bool swap = i.op == Op::Gt || i.op == Op::Le;
            line("slt " + dst + ", " + (swap ? b : a) + ", " + (swap ? a : b));
            if (i.op == Op::Le || i.op == Op::Ge) line("xori " + dst + ", " + dst + ", 1");
          }
        }
      } else if (i.op == Op::And || i.op == Op::Shr) {
        line(std::string(i.op == Op::And ? "and " : "srlw ") + dst + ", " + a + ", " + b);
      } else {
        std::string op = i.op == Op::Add ? "add" : i.op == Op::Sub ? "sub" : i.op == Op::Mul ? "mul" : i.op == Op::Div ? "div" : "rem";
        op = fp(type) ? "f" + op + ".s" : op + "w";
        line(op + " " + dst + ", " + a + ", " + b);
      }
    }
    finish(i.result, dst);
  }
  void emitFunction(const FunctionIR &f) {
    function = &f; allocation = allocate(f);
    emissionTargets.resize(f.blocks.size()); std::iota(emissionTargets.begin(), emissionTargets.end(), 0);
    std::vector<bool> neutral(f.blocks.size(), false);
    for (size_t b = 0; b < f.blocks.size(); ++b) {
      const auto &code = f.blocks[b].instructions;
      neutral[b] = code.back().op == Op::Jump;
      for (size_t n = 0; n+1 < code.size(); ++n) {
        const auto &i = code[n];
        neutral[b] = neutral[b] && i.op == Op::Move && allocation.registers[i.result] >= 0 &&
            allocation.registers[i.result] == allocation.registers[i.args[0]] && !allocation.definitions[i.args[0]];
      }
    }
    for (size_t b = 0; b < f.blocks.size(); ++b) {
      int target = b; std::set<int> seen;
      while (neutral[target] && seen.insert(target).second) target = f.blocks[target].instructions.back().target;
      if (!neutral[target]) emissionTargets[b] = target;
    }
    std::vector<ValueType> types;
    for (const auto &p : f.source->parameters) types.push_back(p.symbol->type.array() ? ValueType::Pointer : p.base == Base::Float ? ValueType::Float : ValueType::Int);
    incoming = arguments(types, types.size());
    out << "\n.text\n.p2align 2\n.globl " << f.source->name << "\n.type " << f.source->name << ", @function\n" << f.source->name << ":\n";
    if (allocation.frame == 0) {}
    else if (small(-allocation.frame)) line("addi sp, sp, " + std::to_string(-allocation.frame));
    else { line("li t6, " + std::to_string(allocation.frame)); line("sub sp, sp, t6"); }
    int64_t offset = allocation.saves;
    if (!allocation.leaf) { memory("sd", "ra", offset); offset += 8; }
    for (int r : allocation.integers) { memory("sd", "s" + std::to_string(r), offset); offset += 8; }
    for (int r : allocation.floats) { memory("fsd", "fs" + std::to_string(r), offset); offset += 8; }
    for (size_t b = 0; b < f.blocks.size(); ++b) {
      if (emissionTargets[b] != static_cast<int>(b)) continue;
      nextEmittedBlock = b+1;
      while (nextEmittedBlock < static_cast<int>(f.blocks.size()) && emissionTargets[nextEmittedBlock] != nextEmittedBlock) ++nextEmittedBlock;
      scratchConstant.reset();
      out << label(b) << ":\n";
      for (const auto &i : f.blocks[b].instructions) instruction(i);
    }
    out << ".L" << functionId << "_return:\n";
    offset = allocation.saves;
    if (!allocation.leaf) { memory("ld", "ra", offset); offset += 8; }
    for (int r : allocation.integers) { memory("ld", "s" + std::to_string(r), offset); offset += 8; }
    for (int r : allocation.floats) { memory("fld", "fs" + std::to_string(r), offset); offset += 8; }
    if (allocation.frame == 0) {}
    else if (small(allocation.frame)) line("addi sp, sp, " + std::to_string(allocation.frame));
    else { line("li t6, " + std::to_string(allocation.frame)); line("add sp, sp, t6"); }
    line("ret"); out << ".size " << f.source->name << ", .-" << f.source->name << '\n';
    for (const auto &b : f.blocks) for (const auto &i : b.instructions) if (i.op == Op::String) {
      out << ".section .rodata\n" << stringLabel(i.result) << ":\n\t.byte ";
      for (unsigned char c : i.text) out << static_cast<unsigned>(c) << ',';
      out << "0\n";
    }
  }
  void global(const Variable &v) {
    const auto &s = *v.symbol; auto size = s.type.elements() * 4;
    out << (v.isConst ? ".section .rodata\n" : s.values.empty() ? ".bss\n" : ".data\n");
    out << ".p2align 2\n.globl " << v.name << "\n.type " << v.name << ", @object\n.size " << v.name << ", " << size << '\n' << v.name << ":\n";
    std::vector<int64_t> indices;
    for (auto [index, value] : s.values) { (void)value; indices.push_back(index); }
    std::sort(indices.begin(), indices.end());
    int64_t cursor = 0;
    for (auto index : indices) {
      if (index * 4 > cursor) line(".zero " + std::to_string(index * 4 - cursor));
      auto value = s.values.at(index); uint32_t raw = static_cast<uint32_t>(value.integer);
      if (value.base == Base::Float) std::memcpy(&raw, &value.floating, 4);
      line(".word " + std::to_string(raw)); cursor = index * 4 + 4;
    }
    if (cursor < size) line(".zero " + std::to_string(size - cursor));
  }
public:
  Assembly(const ModuleIR &module, std::ostream &out) : module(module), out(out) {}
  void run() {
    out << ".option nopic\n.attribute arch, \"rv64gc\"\n";
    for (const auto &item : module.source->items) for (const auto &v : item.variables) global(v);
    for (auto f : module.functions) { destroySSA(f); selectInstructions(f); layoutBlocks(f); emitFunction(f); ++functionId; }
    out << ".section .note.GNU-stack,\"\",@progbits\n";
  }
};
} // namespace

void emitAssembly(const ModuleIR &module, std::ostream &out) { Assembly(module, out).run(); }
void selectInstructions(FunctionIR &f) {
  std::vector<int> uses(f.values.size(), 0);
  for (const auto &b : f.blocks) for (const auto &i : b.instructions) for (int a : i.args) ++uses[a];
  for (auto &b : f.blocks) {
    auto &code = b.instructions;
    if (code.size() < 2 || code.back().op != Op::Branch) continue;
    const auto compare = code[code.size()-2];
    if (compare.op >= Op::Eq && compare.op <= Op::Ge && compare.result == code.back().args[0] &&
        uses[compare.result] == 1 && !fp(f.values[compare.args[0]])) {
      code.back().op = Op::BranchCompare; code.back().args = compare.args; code.back().immediate = static_cast<int>(compare.op);
      code.erase(code.end()-2);
    }
  }
}
} // namespace sysy
