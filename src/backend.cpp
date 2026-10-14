#include "target_ir.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <numeric>
#include <queue>
#include <set>
#include <unordered_set>

namespace sysy {
namespace {
bool fp(ValueType type) { return type == ValueType::Float || type == ValueType::Double; }
int64_t align(int64_t size, int64_t alignment) { return (size + alignment - 1) / alignment * alignment; }
bool small(int64_t value) { return value >= -2048 && value <= 2047; }

// Sparse 64-value chunks keep block liveness compact without allocating a
// full values-by-blocks matrix. Union/difference operate on machine words.
class ValueSet {
  using Chunk = std::pair<int,uint64_t>;
  std::vector<Chunk> chunks;
  auto find(int word) const {
    return std::lower_bound(chunks.begin(),chunks.end(),word,[](auto c, int w) { return c.first < w; });
  }
public:
  bool count(int v) const {
    auto it = find(v/64); return it != chunks.end() && it->first == v/64 && (it->second & (uint64_t{1}<<(v%64)));
  }
  void insert(int v) {
    auto it = std::lower_bound(chunks.begin(),chunks.end(),v/64,[](auto c, int w) { return c.first < w; });
    if (it == chunks.end() || it->first != v/64) it = chunks.insert(it,{v/64,0});
    it->second |= uint64_t{1}<<(v%64);
  }
  void erase(int v) {
    auto it = std::lower_bound(chunks.begin(),chunks.end(),v/64,[](auto c, int w) { return c.first < w; });
    if (it != chunks.end() && it->first == v/64) {
      it->second &= ~(uint64_t{1}<<(v%64)); if (!it->second) chunks.erase(it);
    }
  }
  bool merge(const ValueSet &source, const ValueSet *exclude = nullptr) {
    std::vector<Chunk> joined; joined.reserve(chunks.size()+source.chunks.size());
    size_t a = 0, b = 0, e = 0;
    while (a < chunks.size() || b < source.chunks.size()) {
      int word = std::min(a < chunks.size() ? chunks[a].first : INT32_MAX,
                          b < source.chunks.size() ? source.chunks[b].first : INT32_MAX);
      uint64_t bits = 0;
      if (b < source.chunks.size() && source.chunks[b].first == word) bits = source.chunks[b++].second;
      if (exclude) {
        while (e < exclude->chunks.size() && exclude->chunks[e].first < word) ++e;
        if (e < exclude->chunks.size() && exclude->chunks[e].first == word) bits &= ~exclude->chunks[e].second;
      }
      if (a < chunks.size() && chunks[a].first == word) bits |= chunks[a++].second;
      if (bits) joined.push_back({word,bits});
    }
    if (chunks == joined) return false;
    chunks.swap(joined); return true;
  }
  bool operator!=(const ValueSet &other) const { return chunks != other.chunks; }
  template<class F> void each(F visit) const {
    for (auto [word,bits] : chunks) while (bits) {
      visit(word*64+__builtin_ctzll(bits)); bits &= bits-1;
    }
  }
};

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
  std::vector<int64_t> spills, slots;
  std::vector<const MachineInstruction *> definitions;
  std::vector<const MachineInstruction *> original;
  std::vector<int> uses;
  std::unordered_map<const Symbol *, int64_t> objects;
  std::set<int> integers, floats;
  int64_t frame = 0, saves = 0;
  bool leaf = true;
};

Allocation allocate(const MachineFunction &f) {
  const size_t count = f.values.size(), blocks = f.blocks.size();
  Allocation result;
  result.registers.assign(count, -1); result.spills.assign(count, -1); result.definitions.assign(count, nullptr);
  std::vector<int> definitionCount(count, 0);
  std::vector<int> sites(count, 0);
  const auto &depth = f.depth;
  std::vector<double> weight(count, 0);
  std::vector<ValueSet> use(blocks), def(blocks), in(blocks), out(blocks);
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
    if (source->op == Op::Address && i.op == Op::Target) cached[a] = true;
    if (source->op == Op::Constant && source->immediate != 0 &&
        (fp(f.values[a]) || (!small(source->immediate) && i.op == Op::BranchCompare))) cached[a] = true;
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
      const auto &last = f.blocks[b].instructions.back();
      for (int successor : {last.target, last.otherwise}) if (successor >= 0)
        changed |= out[b].merge(in[successor]);
      auto next = use[b]; next.merge(out[b],&def[b]);
      if (next != in[b]) { in[b] = std::move(next); changed = true; }
    }
  } while (changed);
  std::vector<std::unordered_set<int>> interference(count);
  std::vector<std::vector<int>> hints(count);
  std::vector<uint32_t> forbidden(count,0);
  using Range = std::pair<int,int>;
  std::vector<std::vector<Range>> ranges(count);
  int blockStart = 0;
  for (size_t b = 0; b < blocks; ++b) {
    auto live = out[b];
    int blockEnd = blockStart + 2*f.blocks[b].instructions.size();
    std::map<int,int> ends;
    live.each([&](int v) { if (tracked(v)) ends[v] = blockEnd; });
    int point = blockEnd;
    for (auto it = f.blocks[b].instructions.rbegin(); it != f.blocks[b].instructions.rend(); ++it) {
      const auto &i = *it; point -= 2;
      if (i.result >= 0 && tracked(i.result)) {
        auto end = ends.find(i.result);
        ranges[i.result].push_back({point+1,end == ends.end() ? point+2 : end->second});
        if (end != ends.end()) ends.erase(end);
      }
      for (int a : i.args) if (tracked(a) && !ends.count(a)) ends[a] = point+1;
      if (i.integerClobbers || i.floatClobbers)
        live.each([&](int v) { if (v != i.result) forbidden[v] |= fp(f.values[v]) ? i.floatClobbers : i.integerClobbers; });
      if (i.result >= 0 && tracked(i.result)) {
        int source = i.op == Op::Move ? i.args[0] : -1;
        if (source >= 0 && tracked(source)) { hints[i.result].push_back(source); hints[source].push_back(i.result); }
        live.each([&](int v) { if (v != i.result && v != source && tracked(v) && fp(f.values[v]) == fp(f.values[i.result])) {
          interference[v].insert(i.result); interference[i.result].insert(v);
        } });
        live.erase(i.result);
      }
      for (int v : i.args) if (tracked(v)) live.insert(v);
    }
    for (auto [v,end] : ends) ranges[v].push_back({blockStart,end});
    blockStart = blockEnd;
  }
  auto normalize = [](std::vector<Range> &list) {
    std::sort(list.begin(),list.end()); std::vector<Range> merged;
    for (auto r : list) {
      if (!merged.empty() && r.first <= merged.back().second) merged.back().second = std::max(r.second,merged.back().second);
      else merged.push_back(r);
    }
    list = std::move(merged);
  };
  for (auto &list : ranges) normalize(list);
  std::vector<ValueType> parameterTypes;
  for (const auto &p : f.source->parameters) parameterTypes.push_back(p.symbol->type.array() ? ValueType::Pointer : p.base == Base::Float ? ValueType::Float : ValueType::Int);
  auto incoming = arguments(parameterTypes, parameterTypes.size());
  auto preferred = [&](int v) {
    const auto *i = result.original[v];
    if (!i || i->op != Op::Parameter) return -1;
    auto a = incoming.at(i->immediate);
    if (a.reg < 0 || (fp(f.values[v]) && !a.floating)) return -1;
    return a.reg + (fp(f.values[v]) ? floatArgumentStart : integerArgumentStart);
  };
  auto allowed = [&](int v, int r) {
    if (forbidden[v] & (uint32_t{1} << r)) return false;
    const auto *i = result.original[v];
    int first = fp(f.values[v]) ? floatArgumentStart : integerArgumentStart;
    if (i && i->op == Op::Parameter && r >= first && r < first+8 && r != preferred(v)) return false;
    return true;
  };
  for (bool floating : {false, true}) {
    int colors = floating ? floatRegisterCount : integerRegisterCount;
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
      // Keep affinity as a hint when a cold, constrained copy would restrict
      // a substantially hotter bundle. Fewer moves need not mean fewer spills.
      if ((mask != masks[a] && weight[a] >= 10 && weight[a] > 2*weight[b]) ||
          (mask != masks[b] && weight[b] >= 10 && weight[b] > 2*weight[a])) continue;
      auto neighbors = conflicts[a]; neighbors.insert(conflicts[b].begin(), conflicts[b].end());
      int high = 0, available = population(mask);
      for (int v : neighbors) high += conflicts[v].size() >= static_cast<size_t>(available);
      if (high >= available) continue;
      // Conservative Briggs coalescing with the intersection of ABI masks.
      parent[b] = a; pending.erase(b); masks[a] = mask; weight[a] += weight[b];
      conflicts[a] = std::move(neighbors); conflicts[b].clear();
      for (int v : conflicts[a]) { conflicts[v].erase(b); conflicts[v].insert(a); }
    }
    // Allocate precise ranges in priority order. Coalesced values form bundles;
    // evict cheaper bundles and retry with a bounded number of attempts.
    std::vector<std::vector<Range>> bundles(count);
    for (size_t v = 0; v < count; ++v) if (fp(f.values[v]) == floating && tracked(v)) {
      auto &bundle = bundles[leader(v)]; bundle.insert(bundle.end(),ranges[v].begin(),ranges[v].end());
    }
    std::vector<double> priority(count,0);
    std::vector<int> attempts(count,0);
    // Per-register occupied ranges are disjoint. Queries visit only intervals
    // intersecting the candidate, rather than scanning every allocated bundle.
    std::vector<std::map<int,std::pair<int,int>>> occupancy(colors);
    auto overlaps = [&](int r, const std::vector<Range> &candidate) {
      std::set<int> found;
      const auto &index = occupancy[r];
      for (auto range : candidate) {
        auto it = index.lower_bound(range.first);
        if (it != index.begin()) {
          auto previous = std::prev(it);
          if (previous->second.first > range.first) found.insert(previous->second.second);
        }
        for (; it != index.end() && it->first < range.second; ++it) found.insert(it->second.second);
      }
      return found;
    };
    std::priority_queue<std::pair<double,int>> work;
    for (int v : pending) {
      normalize(bundles[v]); size_t length = 0;
      for (auto r : bundles[v]) length += r.second-r.first;
      priority[v] = weight[v]/std::sqrt(double(std::max<size_t>(1,length)));
      work.push({priority[v],v});
    }
    while (!work.empty()) {
      int v = work.top().second; work.pop(); ++attempts[v];
      std::vector<int> order;
      auto append = [&](int r) { if (r >= 0 && r < colors && std::find(order.begin(),order.end(),r) == order.end()) order.push_back(r); };
      append(preferred(v));
      for (int hint : hints[v]) append(result.registers[leader(hint)]);
      for (int r = 12; r < colors; ++r) append(r);
      for (int r = 0; r < 12; ++r) append(r);
      int chosen = -1; double cheapest = std::numeric_limits<double>::infinity(); std::vector<int> victims;
      for (int r : order) {
        if (!(masks[v] & (uint32_t{1} << r))) continue;
        double cost = 0; std::vector<int> conflictsHere;
        for (int other : overlaps(r,bundles[v])) {
          conflictsHere.push_back(other);
          cost += attempts[other] >= 3 ? std::numeric_limits<double>::infinity() : priority[other];
        }
        if (cost == 0) { chosen = r; victims.clear(); break; }
        if (cost < cheapest && cost*1.25 < priority[v]) { cheapest = cost; chosen = r; victims = std::move(conflictsHere); }
      }
      if (chosen < 0) continue;
      for (int other : victims) {
        for (auto range : bundles[other]) occupancy[chosen].erase(range.first);
        result.registers[other] = -1; work.push({priority[other],other});
      }
      for (auto range : bundles[v]) {
        if (!occupancy[chosen].emplace(range.first,std::make_pair(range.second,v)).second) throw std::logic_error("overlapping allocation index");
      }
      result.registers[v] = chosen;
    }
    for (size_t v = 0; v < count; ++v) if (fp(f.values[v]) == floating && tracked(v)) result.registers[v] = result.registers[leader(v)];
  }
  // Check allocation against independently constructed interference and ABI
  // masks, rather than trusting the interval allocator's own overlap queries.
  for (size_t v = 0; v < count; ++v) if (result.registers[v] >= 0) {
    if (!allowed(v,result.registers[v])) throw std::logic_error("allocation violates ABI mask");
    for (int other : interference[v]) if (result.registers[other] == result.registers[v])
      throw std::logic_error("allocation gives interfering values the same register");
  }
  int64_t offset = 0;
  for (const auto &b : f.blocks) for (const auto &i : b.instructions) if (i.op == Op::Call) {
    std::vector<ValueType> types;
    for (int arg : i.args) types.push_back(f.values[arg]);
    auto abi = arguments(types, i.callee->timer ? 1 : i.callee->parameters.size());
    for (auto a : abi) if (a.reg < 0) offset = std::max(offset, a.offset + 8);
  }
  offset = align(offset,8);
  for (auto type : f.slots) { (void)type; result.slots.push_back(offset); offset += 8; }
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

// Split spilled live ranges into short use clusters with a write-through home.
// Block edges and calls end a cluster; long gaps do not keep a register live.
// ABI call operands remain intact because they are simultaneous uses.
bool splitSpills(MachineFunction &f, const Allocation &allocation) {
  std::set<int> excluded, selected;
  for (const auto &b : f.blocks) for (const auto &i : b.instructions) if (i.op == Op::Call)
    excluded.insert(i.args.begin(),i.args.end());
  for (size_t b = 0; b < f.blocks.size(); ++b) if (f.depth[b] > 0)
    for (const auto &i : f.blocks[b].instructions) for (int v : i.args)
      if (allocation.spills[v] >= 0 && !excluded.count(v) && selected.size() < 16) selected.insert(v);
  if (selected.empty()) return false;
  std::map<int,int> slots;
  for (int v : selected) { slots[v] = f.slots.size(); f.slots.push_back(f.values[v]); }
  // With few homes, short mandatory fragments already avoid most spills;
  // sharing can instead increase pressure. Use clusters for heavier spilling.
  bool clusterUses = selected.size() == 16;
  auto fresh = [&](int v) { int id = f.values.size(); f.values.push_back(f.values[v]); return id; };
  for (auto &b : f.blocks) {
    std::vector<MachineInstruction> code;
    struct Cluster { int value; size_t start, last; };
    std::map<int,Cluster> cached;
    size_t point = 0;
    for (auto i : b.instructions) {
      if (!clusterUses) cached.clear();
      for (int &a : i.args) if (slots.count(a)) {
        int original = a;
        auto found = cached.find(a);
        if (found == cached.end() || point-found->second.last > 8 || point-found->second.start > 16) {
          int id = fresh(a); MachineInstruction reload{Op::SpillLoad}; reload.result = id; reload.immediate = slots.at(a);
          code.push_back(std::move(reload)); cached[original] = {id,point,point};
        }
        auto &cluster = cached.at(original); a = cluster.value; cluster.last = point;
      }
      if (i.op == Op::Call) cached.clear();
      int original = i.result;
      if (slots.count(original)) i.result = fresh(original);
      int produced = i.result; code.push_back(std::move(i));
      if (slots.count(original)) {
        MachineInstruction save{Op::SpillStore}; save.args = {produced}; save.immediate = slots.at(original); code.push_back(std::move(save));
        cached[original] = {produced,point,point};
      }
      ++point;
    }
    b.instructions = std::move(code);
  }
  return true;
}

// Independent must-dataflow checker: a location records which current virtual
// values it contains. At joins only facts true on every predecessor survive.
// This verifies copy/coalescing, spill locations and call clobbers symbolically.
void checkAllocation(const MachineFunction &f, const Allocation &a) {
  using State = std::map<int,std::set<int>>;
  auto location = [&](int v) {
    if (a.definitions[v]) return -1;
    if (a.registers[v] >= 0) return a.registers[v]+(fp(f.values[v]) ? integerRegisterCount : 0);
    return a.spills[v] >= 0 ? integerRegisterCount+floatRegisterCount+v : -1;
  };
  auto slot = [&](int n) { return integerRegisterCount+floatRegisterCount+static_cast<int>(f.values.size())+n; };
  std::vector<std::vector<int>> pred(f.blocks.size());
  for (size_t b = 0; b < f.blocks.size(); ++b) {
    const auto &i = f.blocks[b].instructions.back();
    for (int t : {i.target,i.otherwise}) if (t >= 0) pred[t].push_back(b);
  }
  std::vector<std::optional<State>> in(f.blocks.size()), out(f.blocks.size());
  auto transfer = [&](size_t b, State state, bool check) {
    for (size_t n = 0; n < f.blocks[b].instructions.size(); ++n) {
      const auto &i = f.blocks[b].instructions[n]; std::set<int> source;
      for (size_t p = 0; p < i.args.size(); ++p) {
        int v = i.args[p], loc = location(v); auto aliases = loc < 0 ? std::set<int>{v} : state[loc];
        if (check && !aliases.count(v)) throw std::logic_error("allocation lost %"+std::to_string(v)+" in "+f.source->name+" b"+std::to_string(b)+"."+std::to_string(n));
        if (!p) source = std::move(aliases);
      }
      for (int r = 0; r < std::max(integerRegisterCount,floatRegisterCount); ++r) {
        if (r < integerRegisterCount && (i.integerClobbers & (uint32_t{1} << r))) state.erase(r);
        if (i.floatClobbers & (uint32_t{1} << r)) state.erase(integerRegisterCount+r);
      }
      if (i.op == Op::SpillStore) state[slot(i.immediate)] = source;
      if (i.result >= 0 && location(i.result) >= 0) {
        if (i.op == Op::SpillLoad) source = state[slot(i.immediate)];
        for (auto &entry : state) entry.second.erase(i.result);
        source.erase(i.result);
        std::set<int> contents;
        if (i.op == Op::Move || i.op == Op::SpillLoad) contents = std::move(source);
        contents.insert(i.result); state[location(i.result)] = std::move(contents);
      }
      for (auto it = state.begin(); it != state.end();) if (it->second.empty()) it = state.erase(it); else ++it;
    }
    return state;
  };
  bool changed;
  do {
    changed = false;
    for (size_t b = 0; b < f.blocks.size(); ++b) {
      std::optional<State> state;
      if (!b) state = State{};
      else for (int p : pred[b]) if (out[p]) {
        if (!state) { state = *out[p]; continue; }
        for (auto it = state->begin(); it != state->end();) {
          auto other = out[p]->find(it->first);
          if (other == out[p]->end()) { it = state->erase(it); continue; }
          std::set<int> common;
          std::set_intersection(it->second.begin(),it->second.end(),other->second.begin(),other->second.end(),std::inserter(common,common.end()));
          if (common.empty()) it = state->erase(it); else { it->second = std::move(common); ++it; }
        }
      }
      if (!state) continue;
      auto next = transfer(b,*state,false); in[b] = std::move(state);
      if (!out[b] || *out[b] != next) { out[b] = std::move(next); changed = true; }
    }
  } while (changed);
  for (size_t b = 0; b < f.blocks.size(); ++b) if (in[b]) transfer(b,*in[b],true);
}

class Assembly {
  const ModuleIR &module;
  std::ostream &out;
  Stats *stats;
  bool check, dump;
  const MachineFunction *function = nullptr;
  Allocation allocation;
  size_t functionId = 0;
  int privateLabel = 0;
  std::vector<Argument> incoming;
  int nextEmittedBlock = 0;
  std::vector<int> emissionTargets;
  void line(const std::string &text) { out << "\t" << text << '\n'; }
  std::string label(int block) const { return ".L" + std::to_string(functionId) + "_b" + std::to_string(block); }
  std::string reg(int id) const {
    int r = allocation.registers[id];
    if (fp(function->values[id])) return r < 12 ? "fs" + std::to_string(r) : r < 21 ? "ft" + std::to_string(r - 9) : "fa" + std::to_string(r - 21);
    return r < 12 ? "s" + std::to_string(r) : r == 12 ? "t5" : r == 21 ? "t2" : "a" + std::to_string(r - integerArgumentStart);
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
  void loadStore(const MachineInstruction &i) {
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
  void parameter(const MachineInstruction &i) {
    auto location = incoming.at(i.immediate); auto type = function->values[i.result];
    auto dst = destination(i.result);
    if (location.reg < 0) memory(type == ValueType::Float ? "flw" : type == ValueType::Pointer ? "ld" : "lw", dst, allocation.frame + location.offset);
    else if (fp(type) && !location.floating) line("fmv.w.x " + dst + ", a" + std::to_string(location.reg));
    else move(dst, (location.floating ? "fa" : "a") + std::to_string(location.reg), type);
    finish(i.result, dst);
  }
  void call(const MachineInstruction &i) {
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
  void instruction(const MachineInstruction &i) {
    if (i.op == Op::SpillLoad || i.op == Op::SpillStore) {
      int v = i.op == Op::SpillLoad ? i.result : i.args[0]; auto type = function->values[v];
      bool load = i.op == Op::SpillLoad;
      auto r = load ? destination(v) : read(v,fp(type) ? "ft0" : "t0");
      memory(load ? (type == ValueType::Float ? "flw" : type == ValueType::Double ? "fld" : type == ValueType::Int ? "lw" : "ld") :
                    (type == ValueType::Float ? "fsw" : type == ValueType::Double ? "fsd" : type == ValueType::Int ? "sw" : "sd"),r,allocation.slots.at(i.immediate));
      if (load) finish(v,r);
      return;
    }
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
    if (i.op == Op::Call) { call(i); return; }
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
      // Pair words only when the exact frame address proves eight-byte
      // alignment. Small clears avoid a counted loop; never widen its tail.
      auto def = allocation.original[i.args[0]];
      bool wide = def && def->op == Op::Address && !def->symbol->global &&
          (allocation.objects.at(def->symbol)+def->immediate)%8 == 0;
      int64_t stride = wide ? 8 : 4, bulk = i.immediate / stride;
      if (i.immediate <= 128) {
        for (int64_t offset = 0; offset < i.immediate; offset += stride)
          memory(wide && i.immediate-offset >= 8 ? "sd" : "sw", "zero", offset, "t0");
        return;
      }
      line("li t1, " + std::to_string(bulk));
      std::string loop = ".Lzero" + std::to_string(functionId) + '_' + std::to_string(privateLabel++);
      out << loop << ":\n"; line(std::string(wide ? "sd" : "sw")+" zero, 0(t0)");
      line("addi t0, t0, " + std::to_string(stride)); line("addi t1, t1, -1"); line("bnez t1, " + loop);
      if (i.immediate%stride) line("sw zero, 0(t0)");
      return;
    }
    auto type = function->values[i.result]; auto dst = destination(i.result);
    auto aType = function->values[i.args[0]];
    auto a = read(i.args[0], fp(aType) ? "ft0" : "t0");
    if (i.op == Op::Move) move(dst,a,type);
    else {
      std::string text = std::string(rvName(i.opcode)) + " " + dst + ", " + a;
      if (i.args.size() == 2) text += ", " + read(i.args[1], fp(function->values[i.args[1]]) ? "ft1" : "t1");
      switch (i.opcode) {
      case RV::Addi: case RV::Addiw: case RV::Andi: case RV::Slli: case RV::Slliw:
      case RV::Srliw: case RV::Srai: case RV::Sraiw: case RV::Xori: text += ", " + std::to_string(i.immediate); break;
      case RV::FcvtWS: text += ", rtz"; break;
      default: break;
      }
      line(text);
    }
    finish(i.result,dst);
  }

  void emitFunction(MachineFunction &f) {
    function = &f;
    auto allocateNow = [&] { return allocate(f); };
    allocation = stats ? stats->measure("allocation",allocateNow) : allocateNow();
    if (splitSpills(f,allocation)) allocation = stats ? stats->measure("allocation.retry",allocateNow) : allocateNow();
    verifyTarget(f);
    if (check) { auto verify = [&] { checkAllocation(f,allocation); }; if (stats) stats->measure("allocation.check",verify); else verify(); }
    if (stats) {
      stats->count("machine.instructions",[&] { size_t n = 0; for (const auto &b : f.blocks) n += b.instructions.size(); return n; }());
      stats->count("machine.values",f.values.size()); stats->count("allocation.split_slots",f.slots.size());
      for (size_t v = 0; v < f.values.size(); ++v) if (allocation.spills[v] >= 0) stats->count("allocation.spills",1);
    }
    if (dump) {
      out << "allocated " << f.source->name << " frame=" << allocation.frame << "\n";
      for (size_t v = 0; v < f.values.size(); ++v) if (allocation.uses[v]) {
        out << "  %" << v << " = ";
        if (allocation.definitions[v]) out << "rematerialize";
        else if (allocation.registers[v] >= 0) out << reg(v);
        else out << "stack[" << allocation.spills[v] << ']';
        out << '\n';
      }
      for (size_t n = 0; n < allocation.slots.size(); ++n) out << "  split.slot" << n << " = stack[" << allocation.slots[n] << "]\n";
      return;
    }
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
  Assembly(const ModuleIR &module, std::ostream &out, Stats *stats, bool check, bool dump = false) : module(module), out(out), stats(stats), check(check), dump(dump) {}
  void run() {
    if (!dump) out << ".option nopic\n.attribute arch, \"rv64gc\"\n";
    if (!dump) for (const auto &item : module.source->items) for (const auto &v : item.variables) global(v);
    for (const auto &source : module.functions) { auto select = [&] { return selectTarget(source,stats); }; auto f = stats ? stats->measure("selection",select) : select(); emitFunction(f); ++functionId; }
    if (!dump) out << ".section .note.GNU-stack,\"\",@progbits\n";
  }
};
} // namespace

void emitAssembly(const ModuleIR &module, std::ostream &out, Stats *stats, bool check) { Assembly(module,out,stats,check).run(); }
void printAllocated(const ModuleIR &module, std::ostream &out, Stats *stats, bool check) { Assembly(module,out,stats,check,true).run(); }
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
