#include "machine_ir.hpp"

#include <algorithm>
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

struct Interval { int start = std::numeric_limits<int>::max(), end = -1; };
struct Allocation {
  std::vector<int> registers;
  std::vector<int64_t> spills;
  std::vector<const Instruction *> definitions;
  std::unordered_map<const Symbol *, int64_t> objects;
  std::set<int> integers, floats;
  int64_t frame = 0, saves = 0;
};

Allocation allocate(const FunctionIR &f) {
  const size_t count = f.values.size(), blocks = f.blocks.size();
  Allocation result;
  result.registers.assign(count, -1); result.spills.assign(count, -1); result.definitions.assign(count, nullptr);
  std::vector<int> definitionCount(count, 0);
  std::vector<Interval> intervals(count);
  std::vector<std::unordered_set<int>> use(blocks), def(blocks), in(blocks), out(blocks);
  std::vector<int> begin(blocks), end(blocks);
  int position = 0;
  for (size_t b = 0; b < blocks; ++b) {
    begin[b] = position;
    for (const auto &i : f.blocks[b].instructions) {
      for (int arg : i.args) {
        if (!def[b].count(arg)) use[b].insert(arg);
        intervals[arg].end = std::max(intervals[arg].end, position);
        intervals[arg].start = std::min(intervals[arg].start, position);
      }
      if (i.result >= 0) {
        def[b].insert(i.result); result.definitions[i.result] = &i; ++definitionCount[i.result];
        intervals[i.result].start = std::min(intervals[i.result].start, position + 1);
        intervals[i.result].end = std::max(intervals[i.result].end, position + 1);
      }
      position += 2;
    }
    end[b] = position;
  }
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
  for (size_t b = 0; b < blocks; ++b) {
    for (int v : in[b]) { intervals[v].start = std::min(intervals[v].start, begin[b]); intervals[v].end = std::max(intervals[v].end, begin[b]); }
    for (int v : out[b]) { intervals[v].start = std::min(intervals[v].start, end[b]); intervals[v].end = std::max(intervals[v].end, end[b]); }
  }
  for (size_t v = 0; v < count; ++v) {
    const auto *i = result.definitions[v];
    if (definitionCount[v] != 1 || !i || (i->op != Op::Constant && i->op != Op::Address && i->op != Op::String)) result.definitions[v] = nullptr;
  }
  for (bool floating : {false, true}) {
    std::vector<int> order;
    for (size_t v = 0; v < count; ++v)
      if (fp(f.values[v]) == floating && !result.definitions[v] && intervals[v].end >= 0) order.push_back(v);
    std::sort(order.begin(), order.end(), [&](int a, int b) { return intervals[a].start < intervals[b].start; });
    std::vector<int> active;
    std::set<int> free;
    for (int r = 0; r < 12; ++r) free.insert(r);
    for (int v : order) {
      active.erase(std::remove_if(active.begin(), active.end(), [&](int old) {
        if (intervals[old].end < intervals[v].start) { free.insert(result.registers[old]); return true; }
        return false;
      }), active.end());
      if (free.empty()) {
        auto victim = std::max_element(active.begin(), active.end(), [&](int a, int b) { return intervals[a].end < intervals[b].end; });
        if (victim != active.end() && intervals[*victim].end > intervals[v].end) {
          int old = *victim; result.registers[v] = result.registers[old]; result.registers[old] = -1; *victim = v;
        }
      } else {
        result.registers[v] = *free.begin(); free.erase(free.begin()); active.push_back(v);
      }
    }
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
    if (result.definitions[v] || intervals[v].end < 0) continue;
    if (result.registers[v] < 0) { result.spills[v] = offset; offset += 8; }
    else (fp(f.values[v]) ? result.floats : result.integers).insert(result.registers[v]);
  }
  result.saves = offset;
  result.frame = align(offset + (result.integers.size() + result.floats.size() + 1) * 8, 16);
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

  void line(const std::string &text) { out << "\t" << text << '\n'; }
  std::string label(int block) const { return ".L" + std::to_string(functionId) + "_b" + std::to_string(block); }
  std::string reg(int id) const { return (fp(function->values[id]) ? "fs" : "s") + std::to_string(allocation.registers[id]); }
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
        if (fp(function->values[id])) {
          line("li t4, " + std::to_string(static_cast<int32_t>(def->immediate)));
          line("fmv.w.x " + scratch + ", t4");
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
    for (size_t n = 0; n < i.args.size(); ++n) {
      auto type = types[n]; auto location = abi[n];
      auto src = read(i.args[n], fp(type) ? "ft0" : "t0");
      if (location.reg < 0) memory(type == ValueType::Float ? "fsw" : type == ValueType::Double ? "fsd" : "sd", src, location.offset);
      else {
        auto dst = (location.floating ? "fa" : "a") + std::to_string(location.reg);
        if (fp(type) && !location.floating) line(std::string(type == ValueType::Double ? "fmv.x.d " : "fmv.x.w ") + dst + ", " + src);
        else move(dst, src, type);
      }
    }
    line("call " + i.callee->linkageName);
    if (i.result >= 0) {
      auto dst = destination(i.result); auto type = function->values[i.result];
      move(dst, fp(type) ? "fa0" : "a0", type); finish(i.result, dst);
    }
  }
  void instruction(const Instruction &i) {
    if (i.op == Op::Constant || i.op == Op::Address || i.op == Op::String) return;
    if (i.op == Op::Parameter) { parameter(i); return; }
    if (i.op == Op::Load || i.op == Op::Store) { loadStore(i); return; }
    if (i.op == Op::Call) { call(i); return; }
    if (i.op == Op::Jump) { line("j " + label(i.target)); return; }
    if (i.op == Op::Branch) {
      auto test = read(i.args[0], "t0"); line("bnez " + test + ", " + label(i.target)); line("j " + label(i.otherwise)); return;
    }
    if (i.op == Op::Return) {
      if (!i.args.empty()) { auto type = function->values[i.args[0]]; move(fp(type) ? "fa0" : "a0", read(i.args[0], fp(type) ? "ft0" : "t0"), type); }
      line("j .L" + std::to_string(functionId) + "_return"); return;
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
      auto b = read(i.args[1], fp(bType) ? "ft1" : "t1");
      if (i.op == Op::Offset) {
        auto def = allocation.definitions[i.args[1]];
        if (def && def->op == Op::Constant) {
          int64_t delta = def->immediate * i.immediate;
          if (small(delta)) line("addi " + dst + ", " + a + ", " + std::to_string(delta));
          else { line("li t2, " + std::to_string(delta)); line("add " + dst + ", " + a + ", t2"); }
        } else if (i.immediate == 0) move(dst, a, type);
        else {
          if ((i.immediate & (i.immediate - 1)) == 0) {
            int shift = 0; for (int64_t n = i.immediate; n > 1; n >>= 1) ++shift;
            line("slli t2, " + b + ", " + std::to_string(shift));
          } else { line("li t2, " + std::to_string(i.immediate)); line("mul t2, " + b + ", t2"); }
          line("add " + dst + ", " + a + ", t2");
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
    std::vector<ValueType> types;
    for (const auto &p : f.source->parameters) types.push_back(p.symbol->type.array() ? ValueType::Pointer : p.base == Base::Float ? ValueType::Float : ValueType::Int);
    incoming = arguments(types, types.size());
    out << "\n.text\n.p2align 2\n.globl " << f.source->name << "\n.type " << f.source->name << ", @function\n" << f.source->name << ":\n";
    if (small(-allocation.frame)) line("addi sp, sp, " + std::to_string(-allocation.frame));
    else { line("li t6, " + std::to_string(allocation.frame)); line("sub sp, sp, t6"); }
    int64_t offset = allocation.saves;
    memory("sd", "ra", offset); offset += 8;
    for (int r : allocation.integers) { memory("sd", "s" + std::to_string(r), offset); offset += 8; }
    for (int r : allocation.floats) { memory("fsd", "fs" + std::to_string(r), offset); offset += 8; }
    for (size_t b = 0; b < f.blocks.size(); ++b) {
      out << label(b) << ":\n";
      for (const auto &i : f.blocks[b].instructions) instruction(i);
    }
    out << ".L" << functionId << "_return:\n";
    offset = allocation.saves;
    memory("ld", "ra", offset); offset += 8;
    for (int r : allocation.integers) { memory("ld", "s" + std::to_string(r), offset); offset += 8; }
    for (int r : allocation.floats) { memory("fld", "fs" + std::to_string(r), offset); offset += 8; }
    if (small(allocation.frame)) line("addi sp, sp, " + std::to_string(allocation.frame));
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
    for (const auto &f : module.functions) { emitFunction(f); ++functionId; }
    out << ".section .note.GNU-stack,\"\",@progbits\n";
  }
};
} // namespace

void emitAssembly(const ModuleIR &module, std::ostream &out) { Assembly(module, out).run(); }
} // namespace sysy
