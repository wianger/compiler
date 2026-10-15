#include "target_ir.hpp"
#include <algorithm>
#include <functional>
#include <numeric>
#include <unordered_map>

namespace sysy {
const char *rvName(RV op) {
  static const char *names[] = {"pseudo","add","addi","addw","addiw","subw","mul","mulw","divw","remw","rem","and","andi",
    "slli","slliw","srlw","srliw","srai","sraiw","negw","xor","xori","slt","seqz","snez",
    "fadd.s","fsub.s","fmul.s","fdiv.s","fneg.s","feq.s","flt.s","fle.s",
    "fcvt.w.s","fcvt.s.w","fcvt.d.s","fmv.w.x","fmv.d.x"};
  return names[static_cast<size_t>(op)];
}
namespace {
bool small(int64_t n) { return n >= -2048 && n <= 2047; }
bool floating(ValueType t) { return t == ValueType::Float || t == ValueType::Double; }
class Selector {
  MachineFunction result;
  const FunctionIR &input;
  std::vector<const Instruction *> defs;
  std::vector<MachineInstruction> *output = nullptr;
  int value(ValueType type) { int id = result.values.size(); result.values.push_back(type); return id; }
  int constant(int64_t n, ValueType type = ValueType::Int) {
    int id = value(type); MachineInstruction i{Op::Constant}; i.result = id; i.immediate = n; output->push_back(std::move(i)); return id;
  }
  int emit(RV op, MachineArgs args, int64_t imm = 0, int dst = -1, ValueType type = ValueType::Int) {
    if (dst < 0) dst = value(type);
    MachineInstruction i{Op::Target}; i.opcode = op; i.args = std::move(args); i.result = dst; i.immediate = imm;
    output->push_back(std::move(i)); return dst;
  }
  void copy(int dst, int src) { MachineInstruction i{Op::Move}; i.result = dst; i.args = {src}; output->push_back(std::move(i)); }
  std::optional<int64_t> literal(int id) const {
    const auto *d = defs.at(id);
    if (d && d->op == Op::Constant && !floating(input.values[id])) return d->immediate;
    return {};
  }
  std::pair<int,int64_t> address(int pointer,int64_t displacement = 0) const {
    for (int depth = 0; depth < 16 && defs[pointer]; ++depth) {
      const auto &d = *defs[pointer];
      if (d.op == Op::Move) { pointer = d.args[0]; continue; }
      if (d.op != Op::Offset || !literal(d.args[1])) break;
      uint64_t bits = uint64_t(displacement)+uint64_t(*literal(d.args[1]))*uint64_t(d.immediate);
      int64_t next = static_cast<int64_t>(bits);
      if (!small(next)) break;
      displacement = next; pointer = d.args[0];
    }
    return {pointer,displacement};
  }
  bool immediate(const Instruction &i) {
    if (i.args.size() != 2 || input.values[i.result] != ValueType::Int) return false;
    auto c = literal(i.args[1]); if (!c) return false;
    int a = i.args[0], dst = i.result; int32_t d = *c;
    if (i.op == Op::And && small(d)) { emit(RV::Andi,{a},d,dst); return true; }
    if (i.op == Op::Shr) { emit(RV::Srliw,{a},d&31,dst); return true; }
    if (i.op == Op::Add || i.op == Op::Sub) {
      // Word additions compose modulo 2^32, including overflow and subtraction.
      // defs excludes multiply-defined Phi/edge-copy values after SSA lowering.
      int base = a; uint32_t delta = i.op == Op::Add ? uint32_t(d) : 0u-uint32_t(d);
      for (int depth = 0; depth < 16 && defs[base]; ++depth) {
        const auto &prior = *defs[base];
        if (prior.op != Op::Add && prior.op != Op::Sub) break;
        auto step = literal(prior.args[1]); if (!step) break;
        delta += prior.op == Op::Add ? uint32_t(*step) : 0u-uint32_t(*step);
        base = prior.args[0];
      }
      if (small(static_cast<int32_t>(delta))) { emit(RV::Addiw,{base},static_cast<int32_t>(delta),dst); return true; }
      if ((i.op == Op::Add && small(d)) || (i.op == Op::Sub && small(-int64_t(d)))) {
        emit(RV::Addiw,{a},i.op == Op::Add ? d : -int64_t(d),dst); return true;
      }
    }
    uint64_t magnitude = d < 0 ? -int64_t(d) : d;
    bool power = magnitude && !(magnitude & (magnitude-1)); int shift = 0;
    if (power) for (auto n = magnitude; n > 1; n >>= 1) ++shift;
    if (i.op == Op::Mul) {
      if (!d) { copy(dst,constant(0)); return true; }
      if (!power) return false;
      int product = a;
      if (shift) product = emit(RV::Slliw,{a},shift,d < 0 && shift != 31 ? -1 : dst);
      if (d < 0 && shift != 31) emit(RV::Negw,{product},0,dst);
      else if (!shift) copy(dst,a);
      return true;
    }
    if ((i.op != Op::Div && i.op != Op::Rem) || !d) return false;
    int quotient = a;
    if (power) {
      if (!shift) {
        if (i.op == Op::Rem) copy(dst,constant(0));
        else if (d < 0) emit(RV::Negw,{a},0,dst);
        else copy(dst,a);
        return true;
      }
      int bias = shift == 1 ? emit(RV::Srliw,{a},31) : emit(RV::Sraiw,{a},31);
      if (shift != 1) bias = emit(RV::Srliw,{bias},32-shift);
      int adjusted = emit(RV::Addw,{a,bias});
      if (i.op == Op::Rem && small(int64_t(magnitude-1))) {
        // Bias corrects truncation towards zero. Mask before removing it:
        // ((x+bias)&(2^k-1))-bias also preserves negative remainders.
        int residue = emit(RV::Andi,{adjusted},magnitude-1);
        emit(RV::Subw,{residue,bias},0,dst);
      } else {
        quotient = emit(RV::Sraiw,{adjusted},shift);
        // Large masks require materialization. Keep the cheaper shifted
        // quotient reconstruction, also sharing it with an existing division.
        if (i.op == Op::Rem) {
          int product = emit(RV::Slliw,{quotient},shift); emit(RV::Subw,{a,product},0,dst);
        } else if (d < 0) emit(RV::Negw,{quotient},0,dst); else copy(dst,quotient);
      }
      return true;
    }
    uint64_t half = uint64_t{1} << 31, t = half+(d<0), anchor = t-1-t%magnitude;
    uint64_t q1 = half/anchor, r1 = half%anchor, q2 = half/magnitude, r2 = half%magnitude, delta;
    int p = 31;
    do {
      ++p; q1 *= 2; r1 *= 2; if (r1 >= anchor) { ++q1; r1 -= anchor; }
      q2 *= 2; r2 *= 2; if (r2 >= magnitude) { ++q2; r2 -= magnitude; }
      delta = magnitude-r2;
    } while (q1 < delta || (q1 == delta && !r1));
    uint32_t raw = q2+1; if (d < 0) raw = 0u-raw; int32_t multiplier = raw;
    int product = emit(RV::Mul,{a,constant(multiplier)},0,-1,ValueType::Pointer);
    quotient = emit(RV::Srai,{product},32);
    if (d > 0 && multiplier < 0) quotient = emit(RV::Addw,{quotient,a});
    if (d < 0 && multiplier > 0) quotient = emit(RV::Subw,{quotient,a});
    if (p > 32) quotient = emit(RV::Sraiw,{quotient},p-32);
    int sign = emit(RV::Srliw,{quotient},31); quotient = emit(RV::Addw,{quotient,sign});
    if (i.op == Op::Rem) {
      product = emit(RV::Mulw,{quotient,constant(d)}); emit(RV::Subw,{a,product},0,dst);
    } else copy(dst,quotient);
    return true;
  }
  void select(const Instruction &i) {
    // Pseudos expose their virtual operands too; their reserved scratch and ABI
    // clobbers are checked by allocation. No arithmetic expansion follows RA.
    if (i.op == Op::Constant && floating(input.values[i.result])) {
      int bits = constant(input.values[i.result] == ValueType::Float ? static_cast<int32_t>(i.immediate) : i.immediate,ValueType::Pointer);
      emit(input.values[i.result] == ValueType::Float ? RV::FmvWX : RV::FmvDX,{bits},0,i.result,input.values[i.result]); return;
    }
    if (i.op <= Op::Store || i.op == Op::Zero || i.op == Op::Call || i.op == Op::Jump || i.op == Op::Branch || i.op == Op::Return || i.op == Op::BranchCompare) {
      MachineInstruction selected{i.op}; selected.result = i.result; selected.args = i.args; selected.immediate = i.immediate;
      selected.symbol = i.symbol; selected.callee = i.callee; selected.text = i.text;
      selected.target = i.target; selected.otherwise = i.otherwise;
      if (i.op == Op::Load || i.op == Op::Store) {
        auto [pointer,displacement] = address(i.args[0]);
        selected.args[0] = pointer; selected.immediate = displacement;
      }
      if (i.op == Op::Call) { selected.integerClobbers = ((1u<<integerRegisterCount)-1) & ~((1u<<12)-1); selected.floatClobbers = ((1u<<floatRegisterCount)-1) & ~((1u<<12)-1); }
      output->push_back(std::move(selected)); return;
    }
    int dst = i.result; auto type = input.values[dst]; bool fp = floating(type);
    int a = i.args[0];
    if (i.op == Op::ModMul) {
      int product = emit(RV::Mul,{a,i.args[1]},0,-1,ValueType::WideInt);
      int remainder = emit(RV::Rem,{product,constant(i.immediate,ValueType::WideInt)},0,-1,ValueType::WideInt);
      emit(RV::Addiw,{remainder},0,dst); return;
    }
    if (i.op == Op::Convert) { emit(type == ValueType::Int ? RV::FcvtWS : type == ValueType::Double ? RV::FcvtDS : RV::FcvtSW,{a},0,dst,type); return; }
    if (i.op == Op::Neg) { emit(fp ? RV::FnegS : RV::Negw,{a},0,dst,type); return; }
    if (immediate(i)) return;
    int b = i.args[1];
    if (i.op == Op::Offset) {
      auto c = literal(b);
      if (c) {
        int64_t delta = static_cast<int64_t>(uint64_t(*c)*uint64_t(i.immediate));
        if (small(delta)) {
          auto [base,offset] = address(a,delta); emit(RV::Addi,{base},offset,dst,type);
        }
        else emit(RV::Add,{a,constant(delta,ValueType::Pointer)},0,dst,type);
      } else if (!i.immediate) copy(dst,a);
      else {
        int scaled;
        if (!(i.immediate & (i.immediate-1))) {
          int shift = 0; for (int64_t n = i.immediate; n > 1; n >>= 1) ++shift;
          scaled = emit(RV::Slli,{b},shift,-1,ValueType::Pointer);
        } else scaled = emit(RV::Mul,{b,constant(i.immediate,ValueType::Pointer)},0,-1,ValueType::Pointer);
        emit(RV::Add,{a,scaled},0,dst,type);
      }
      return;
    }
    if (i.op >= Op::Eq && i.op <= Op::Ge) {
      bool swap = i.op == Op::Gt || i.op == Op::Le;
      if (floating(input.values[a])) {
        RV op = i.op == Op::Eq || i.op == Op::Ne ? RV::FeqS : i.op == Op::Lt || i.op == Op::Gt ? RV::FltS : RV::FleS;
        bool reverse = i.op == Op::Gt || i.op == Op::Ge;
        if (i.op == Op::Ne) { int eq = emit(op,{a,b}); emit(RV::Xori,{eq},1,dst); }
        else emit(op,reverse ? MachineArgs{b,a} : MachineArgs{a,b},0,dst);
      } else if (i.op == Op::Eq || i.op == Op::Ne) {
        int diff = emit(RV::Xor,{a,b},0,-1,ValueType::Pointer);
        emit(i.op == Op::Eq ? RV::Seqz : RV::Snez,{diff},0,dst);
      } else {
        int cmp = emit(RV::Slt,swap ? MachineArgs{b,a} : MachineArgs{a,b},0,
                       i.op == Op::Le || i.op == Op::Ge ? -1 : dst);
        if (i.op == Op::Le || i.op == Op::Ge) emit(RV::Xori,{cmp},1,dst);
      }
      return;
    }
    RV op = i.op == Op::And ? RV::And : i.op == Op::Shr ? RV::Srlw :
      fp ? (i.op == Op::Add ? RV::FaddS : i.op == Op::Sub ? RV::FsubS : i.op == Op::Mul ? RV::FmulS : RV::FdivS) :
      i.op == Op::Add ? RV::Addw : i.op == Op::Sub ? RV::Subw : i.op == Op::Mul ? RV::Mulw : i.op == Op::Div ? RV::Divw : RV::Remw;
    emit(op,{a,b},0,dst,type);
  }
public:
  explicit Selector(const FunctionIR &f) : input(f), defs(f.values.size(),nullptr) {
    result.source = f.source; result.values = f.values; result.objects = f.objects; result.depth = loopDepth(f);
    std::vector<int> counts(f.values.size(),0);
    for (const auto &b : f.blocks) for (const auto &i : b.instructions) if (i.result >= 0) { defs[i.result] = &i; ++counts[i.result]; }
    for (size_t v = 0; v < defs.size(); ++v) if (counts[v] != 1) defs[v] = nullptr;
  }
  MachineFunction run() {
    for (const auto &b : input.blocks) {
      result.blocks.push_back({b.name,{}}); output = &result.blocks.back().instructions;
      for (const auto &i : b.instructions) select(i);
    }
    return std::move(result);
  }
};

// Legalization exposes computations absent from the source IR (reciprocal
// products, sign correction and scaled addresses). Number those expressions
// locally, including versions of mutable values after phi edge lowering.
void combineTarget(MachineFunction &f) {
  struct Key {
    Op op; RV opcode; ValueType type; int64_t immediate; int a, b;
    bool operator==(const Key &k) const {
      return op == k.op && opcode == k.opcode && type == k.type &&
        immediate == k.immediate && a == k.a && b == k.b;
    }
  };
  struct Hash {
    size_t operator()(const Key &k) const {
      size_t h = static_cast<size_t>(k.op);
      for (auto x : {static_cast<int64_t>(k.opcode),static_cast<int64_t>(k.type),k.immediate,int64_t(k.a),int64_t(k.b)})
        h ^= std::hash<int64_t>{}(x)+0x9e3779b9+(h<<6)+(h>>2);
      return h;
    }
  };
  const size_t count = f.values.size();
  auto pure = [](Op op) { return op == Op::Target || op == Op::Constant || op == Op::Move; };
  for (auto &b : f.blocks) {
    std::vector<int> numbers(count); std::iota(numbers.begin(),numbers.end(),0);
    std::vector<std::pair<int,int>> aliases(count,{-1,-1});
    int next = count;
    std::unordered_map<Key,std::pair<int,int>,Hash> available;
    for (auto &i : b.instructions) {
      for (int &a : i.args) {
        auto alias = aliases[a];
        if (alias.first >= 0 && numbers[alias.first] == alias.second) a = alias.first;
      }
      if (i.result < 0) continue;
      aliases[i.result] = {-1,-1};
      int number = next++;
      if (i.op == Op::Move) { number = numbers[i.args[0]]; aliases[i.result] = {i.args[0],number}; }
      else if (pure(i.op)) {
        Key key{i.op,i.opcode,f.values[i.result],i.immediate,
          i.args.empty() ? -1 : numbers[i.args[0]],i.args.size() < 2 ? -1 : numbers[i.args[1]]};
        auto found = available.find(key);
        if (found != available.end() && numbers[found->second.first] == found->second.second) {
          number = found->second.second;
          // Constants remain independent recipes: a copy would turn a free
          // rematerialization into a live register and an extra instruction.
          if (i.op != Op::Constant) {
            i.op = Op::Move; i.opcode = RV::Pseudo; i.immediate = 0; i.args = {found->second.first};
            aliases[i.result] = found->second;
          }
        } else available[key] = {i.result,number};
      }
      numbers[i.result] = number;
    }
  }
  // Delete unused single-definition expansion temporaries with a def-use
  // worklist. Mutable edge-copy values retain their original definitions.
  std::vector<int> uses(count,0), definitions(count,0);
  std::vector<MachineInstruction *> defs(count,nullptr);
  for (auto &b : f.blocks) for (auto &i : b.instructions) {
    for (int a : i.args) ++uses[a];
    if (i.result >= 0) { defs[i.result] = &i; ++definitions[i.result]; }
  }
  std::vector<int> work;
  auto enqueue = [&](int v) {
    if (!uses[v] && definitions[v] == 1 && pure(defs[v]->op)) work.push_back(v);
  };
  for (size_t v = 0; v < count; ++v) enqueue(v);
  while (!work.empty()) {
    int v = work.back(); work.pop_back(); auto &i = *defs[v];
    for (int a : i.args) { --uses[a]; enqueue(a); }
    i.result = -1;
  }
  for (auto &b : f.blocks) b.instructions.erase(std::remove_if(b.instructions.begin(),b.instructions.end(),
    [&](const MachineInstruction &i) { return i.result < 0 && pure(i.op); }),b.instructions.end());
  std::vector<int> ids(count,-1);
  for (const auto &b : f.blocks) for (const auto &i : b.instructions) {
    if (i.result >= 0) ids[i.result] = 0;
    for (int a : i.args) ids[a] = 0;
  }
  std::vector<ValueType> types;
  for (size_t v = 0; v < count; ++v) if (ids[v] == 0) { ids[v] = types.size(); types.push_back(f.values[v]); }
  for (auto &b : f.blocks) for (auto &i : b.instructions) {
    if (i.result >= 0) i.result = ids[i.result];
    for (int &a : i.args) a = ids[a];
  }
  f.values = std::move(types);
}
} // namespace
MachineFunction selectTarget(FunctionIR f, Stats *stats) {
  destroySSA(f); selectInstructions(f); layoutBlocks(f);
  auto result = Selector(f).run();
  auto combine = [&] { combineTarget(result); };
  if (stats) stats->measure("target.combine",combine); else combine();
  verifyTarget(result); return result;
}
void verifyTarget(const MachineFunction &f) {
  for (auto type : f.values) if (type < ValueType::Int || type > ValueType::WideInt) throw std::logic_error("invalid machine value type");
  for (const auto &b : f.blocks) {
    if (b.instructions.empty()) throw std::logic_error("empty machine block");
    auto terminal = b.instructions.back().op;
    if (terminal != Op::Jump && terminal != Op::Branch && terminal != Op::BranchCompare && terminal != Op::Return) throw std::logic_error("unterminated machine block");
    for (size_t position = 0; position < b.instructions.size(); ++position) {
      const auto &i = b.instructions[position];
      if ((i.integerClobbers >> integerRegisterCount) || (i.floatClobbers >> floatRegisterCount))
        throw std::logic_error("invalid machine clobber mask");
      if ((i.op == Op::Jump || i.op == Op::Branch || i.op == Op::BranchCompare || i.op == Op::Return) && position+1 != b.instructions.size())
        throw std::logic_error("machine terminator before end of block");
      for (int target : {i.target,i.otherwise}) if (target < -1 || target >= static_cast<int>(f.blocks.size()))
        throw std::logic_error("invalid machine edge");
      for (int a : i.args) if (a < 0 || a >= static_cast<int>(f.values.size())) throw std::logic_error("invalid machine use");
      if (i.result < -1 || i.result >= static_cast<int>(f.values.size())) throw std::logic_error("invalid machine definition");
      if (i.opcode < RV::Pseudo || i.opcode > RV::FmvDX) throw std::logic_error("invalid target opcode");
      if (i.op == Op::Target && (i.result < 0 || i.opcode == RV::Pseudo)) throw std::logic_error("unselected instruction");
      if (i.op == Op::Target) {
        bool unary = i.opcode == RV::Addi || i.opcode == RV::Addiw || i.opcode == RV::Andi ||
          (i.opcode >= RV::Slli && i.opcode <= RV::Negw && i.opcode != RV::Srlw) || i.opcode == RV::Xori ||
          i.opcode == RV::Seqz || i.opcode == RV::Snez || i.opcode == RV::FnegS || i.opcode >= RV::FcvtWS;
        if (i.args.size() != (unary ? 1u : 2u)) throw std::logic_error("invalid target arity");
        if ((i.opcode == RV::Addi || i.opcode == RV::Addiw || i.opcode == RV::Andi || i.opcode == RV::Xori) && !small(i.immediate)) throw std::logic_error("target immediate out of range");
        if ((i.opcode == RV::Slli || i.opcode == RV::Srai) && (i.immediate < 0 || i.immediate > 63)) throw std::logic_error("invalid 64-bit shift");
        if ((i.opcode == RV::Slliw || i.opcode == RV::Srliw || i.opcode == RV::Sraiw) && (i.immediate < 0 || i.immediate > 31)) throw std::logic_error("invalid 32-bit shift");
        ValueType result = f.values[i.result];
        auto require = [&](ValueType output, ValueType input) {
          if (result != output || std::any_of(i.args.begin(),i.args.end(),[&](int a) { return f.values[a] != input; }))
            throw std::logic_error("invalid target operand type");
        };
        if (i.opcode == RV::Rem) require(ValueType::WideInt,ValueType::WideInt);
        else if (i.opcode >= RV::FaddS && i.opcode <= RV::FnegS) require(ValueType::Float,ValueType::Float);
        else if (i.opcode >= RV::FeqS && i.opcode <= RV::FcvtWS) require(ValueType::Int,ValueType::Float);
        else if (i.opcode == RV::FcvtSW) require(ValueType::Float,ValueType::Int);
        else if (i.opcode == RV::FcvtDS) require(ValueType::Double,ValueType::Float);
        else {
          bool bits = i.opcode == RV::FmvWX || i.opcode == RV::FmvDX;
          if (bits ? result != (i.opcode == RV::FmvWX ? ValueType::Float : ValueType::Double) : floating(result))
            throw std::logic_error("invalid target register bank");
          for (int a : i.args) if (floating(f.values[a])) throw std::logic_error("invalid target operand bank");
          bool wide = result == ValueType::WideInt || std::any_of(i.args.begin(),i.args.end(),[&](int a) { return f.values[a] == ValueType::WideInt; });
          if (wide) {
            bool product = i.opcode == RV::Mul && result == ValueType::WideInt &&
                std::all_of(i.args.begin(),i.args.end(),[&](int a) { return f.values[a] == ValueType::Int; });
            bool truncate = i.opcode == RV::Addiw && result == ValueType::Int && f.values[i.args[0]] == ValueType::WideInt;
            if (!product && !truncate) throw std::logic_error("invalid wide integer operation");
          }
          bool word = (i.opcode >= RV::Addw && i.opcode <= RV::Remw && i.opcode != RV::Mul) ||
              (i.opcode >= RV::Slliw && i.opcode <= RV::Negw && i.opcode != RV::Srai) ||
              i.opcode == RV::Slt || i.opcode == RV::Seqz || i.opcode == RV::Snez;
          if (word && result != ValueType::Int) throw std::logic_error("invalid word result type");
        }
      }
      auto arity = [&](size_t n, bool definition) {
        if (i.args.size() != n || (i.result >= 0) != definition) throw std::logic_error("invalid machine pseudo arity");
      };
      if (i.op == Op::Move) { arity(1,true); if (f.values[i.result] != f.values[i.args[0]]) throw std::logic_error("invalid machine copy type"); }
      if (i.op == Op::Load || i.op == Op::Store || i.op == Op::Zero) {
        arity(i.op == Op::Store ? 2 : 1,i.op == Op::Load);
        if (f.values[i.args[0]] != ValueType::Pointer) throw std::logic_error("invalid machine memory address");
        if (i.op == Op::Zero && (i.immediate < 0 || i.immediate%4)) throw std::logic_error("invalid machine zero size");
        if (i.op != Op::Zero && !small(i.immediate)) throw std::logic_error("machine memory displacement out of range");
      }
      if (i.op == Op::Jump) { arity(0,false); if (i.target < 0 || i.otherwise != -1) throw std::logic_error("invalid machine jump"); }
      if (i.op == Op::Branch || i.op == Op::BranchCompare) {
        arity(i.op == Op::Branch ? 1 : 2,false);
        if (i.target < 0 || i.otherwise < 0) throw std::logic_error("invalid machine branch");
        if (i.op == Op::Branch && f.values[i.args[0]] != ValueType::Int) throw std::logic_error("invalid machine predicate");
        if (i.op == Op::BranchCompare && f.values[i.args[0]] != f.values[i.args[1]]) throw std::logic_error("invalid machine comparison type");
      }
      if (i.op == Op::SpillLoad || i.op == Op::SpillStore) {
        if (i.immediate < 0 || i.immediate >= static_cast<int64_t>(f.slots.size())) throw std::logic_error("invalid split spill slot");
        arity(i.op == Op::SpillLoad ? 0 : 1,i.op == Op::SpillLoad);
        if (f.values[i.op == Op::SpillLoad ? i.result : i.args[0]] != f.slots[i.immediate]) throw std::logic_error("invalid split spill type");
      }

      if (i.op == Op::Phi || i.op == Op::ModMul || (i.op > Op::Zero && i.op < Op::Call)) throw std::logic_error("generic operation in target IR");
    }
  }
}
void printTarget(const ModuleIR &module, std::ostream &out) {
  for (const auto &source : module.functions) {
    auto f = selectTarget(source); out << "machine " << f.source->name << " rv64gc lp64d\n";
    for (size_t b = 0; b < f.blocks.size(); ++b) {
      out << "b" << b << ":\n";
      for (const auto &i : f.blocks[b].instructions) {
        out << "  "; if (i.result >= 0) out << '%' << i.result << " = ";
        if (i.op == Op::Target) out << rvName(i.opcode);
        else {
          static const char *pseudo[] = {"constant","frame.address","string.address","abi.parameter","copy","load","store","offset","zero"};
          if (i.op == Op::Address && i.symbol->global) out << "global.address";
          else if (i.op <= Op::Zero) out << pseudo[static_cast<size_t>(i.op)];
          else out << (i.op == Op::Call ? "abi.call" : i.op == Op::Return ? "abi.return" : i.op == Op::Jump ? "jump" : "branch");
        }
        for (int a : i.args) out << " %" << a << (floating(f.values[a]) ? ":fp:early" : ":gp:early");
        out << " imm=" << i.immediate;
        if (i.target >= 0) out << " b" << i.target;
        if (i.otherwise >= 0) out << " b" << i.otherwise;
        if (i.symbol) out << " object=" << i.symbol->name;
        if (i.op == Op::Call && i.callee) out << " @" << i.callee->linkageName << " clobber=gp:" << i.integerClobbers << ",fp:" << i.floatClobbers;
        out << '\n';
      }
    }
  }
}
} // namespace sysy
