#include "machine_ir.hpp"

#include <algorithm>
#include <cstring>
#include <iomanip>
#include <queue>
#include <unordered_set>

namespace sysy {
namespace {
ValueType valueType(Base base) {
  if (base == Base::Float) return ValueType::Float;
  if (base == Base::String) return ValueType::Pointer;
  return ValueType::Int;
}

class Lowering {
  ModuleIR module;
  FunctionIR *function = nullptr;
  int current = 0;
  std::unordered_map<const Symbol *, int> parameters;
  std::vector<std::pair<int, int>> loops;

  int block(std::string name) {
    int id = function->blocks.size(); function->blocks.push_back({std::move(name), {}}); return id;
  }
  bool live() const {
    const auto &code = function->blocks[current].instructions;
    if (code.empty()) return true;
    return code.back().op != Op::Jump && code.back().op != Op::Branch && code.back().op != Op::Return;
  }
  void emit(Instruction instruction) { function->blocks[current].instructions.push_back(std::move(instruction)); }
  int value(Op op, ValueType type, std::vector<int> args = {}, int64_t immediate = 0,
            const Symbol *symbol = nullptr, std::string text = {}) {
    int id = function->values.size(); function->values.push_back(type);
    Instruction i{op}; i.result = id; i.args = std::move(args); i.immediate = immediate;
    i.symbol = symbol; i.text = std::move(text); emit(std::move(i)); return id;
  }
  int constant(Number n) {
    int64_t bits = n.integer;
    if (n.base == Base::Float) { uint32_t raw; std::memcpy(&raw, &n.floating, 4); bits = raw; }
    return value(Op::Constant, valueType(n.base), {}, bits);
  }
  void jump(int target) { if (live()) { Instruction i{Op::Jump}; i.target = target; emit(std::move(i)); } }
  void store(int address, int data) { Instruction i{Op::Store}; i.args = {address, data}; emit(std::move(i)); }
  int address(const Symbol *symbol, int64_t offset = 0) {
    auto it = parameters.find(symbol);
    if (it != parameters.end()) {
      if (offset == 0) return it->second;
      return value(Op::Offset, ValueType::Pointer, {it->second, constant(Number::fromInt(offset / 4))}, 4);
    }
    return value(Op::Address, ValueType::Pointer, {}, offset, symbol);
  }
  int address(const Expr &e) {
    int64_t fixed = 0;
    bool allConstant = true;
    for (const auto &index : e.operands) allConstant &= index->constant.has_value();
    if (allConstant) {
      for (size_t i = 0; i < e.operands.size(); ++i) {
        int64_t stride = 4;
        for (size_t j = i + 1; j < e.symbol->type.shape.size(); ++j) stride *= e.symbol->type.shape[j];
        fixed += e.operands[i]->constant->integer * stride;
      }
      return address(e.symbol, fixed);
    }
    int ptr = address(e.symbol);
    for (size_t i = 0; i < e.operands.size(); ++i) {
      int64_t stride = 4;
      for (size_t j = i + 1; j < e.symbol->type.shape.size(); ++j) stride *= e.symbol->type.shape[j];
      int index = expression(*e.operands[i]);
      ptr = value(Op::Offset, ValueType::Pointer, {ptr, index}, stride);
    }
    return ptr;
  }
  int convertValue(int id, Base from, Base to) {
    if (from == to) return id;
    return value(Op::Convert, valueType(to), {id});
  }
  int truth(int id, Base base) {
    int zero = constant(base == Base::Float ? Number::fromFloat(0) : Number::fromInt(0));
    return value(Op::Ne, ValueType::Int, {id, zero});
  }
  void condition(const Expr &e, int yes, int no) {
    if (e.constant) { jump(e.constant->truth() ? yes : no); return; }
    if (e.kind == Expr::Kind::Unary && e.text == "!") { condition(*e.operands[0], no, yes); return; }
    if (e.kind == Expr::Kind::Binary && (e.text == "&&" || e.text == "||")) {
      int rhs = block("logic.rhs");
      if (e.text == "&&") condition(*e.operands[0], rhs, no);
      else condition(*e.operands[0], yes, rhs);
      current = rhs; condition(*e.operands[1], yes, no); return;
    }
    int test = expression(e);
    if (e.type.base == Base::Float) test = truth(test, e.type.base);
    Instruction i{Op::Branch}; i.args = {test}; i.target = yes; i.otherwise = no; emit(std::move(i));
  }
  int expression(const Expr &e) {
    if (e.constant) return constant(*e.constant);
    switch (e.kind) {
    case Expr::Kind::Number: throw std::logic_error("unresolved number");
    case Expr::Kind::String: return value(Op::String, ValueType::Pointer, {}, 0, nullptr, e.text);
    case Expr::Kind::Variable: {
      int ptr = address(e);
      return e.type.array() ? ptr : value(Op::Load, valueType(e.type.base), {ptr});
    }
    case Expr::Kind::Call: {
      const auto &f = *e.callee;
      std::vector<int> args;
      for (size_t i = 0; i < e.operands.size(); ++i) {
        const auto &arg = *e.operands[i]; int id = expression(arg);
        if (i < f.parameters.size()) {
          const auto &expected = f.parameters[i].symbol->type;
          if (expected.numeric()) id = convertValue(id, arg.type.base, expected.base);
        } else if (arg.type.base == Base::Float) id = value(Op::Convert, ValueType::Double, {id});
        args.push_back(id);
      }
      if (f.timer) args.push_back(constant(Number::fromInt(e.location.line)));
      Instruction call{Op::Call}; call.args = std::move(args); call.callee = &f;
      int result = -1;
      if (f.result != Base::Void) {
        result = function->values.size(); function->values.push_back(valueType(f.result)); call.result = result;
      }
      emit(std::move(call)); return result;
    }
    case Expr::Kind::Unary: {
      int a = expression(*e.operands[0]);
      if (e.text == "+") return a;
      if (e.text == "-") return value(Op::Neg, valueType(e.type.base), {a});
      int zero = constant(e.operands[0]->type.base == Base::Float ? Number::fromFloat(0) : Number::fromInt(0));
      return value(Op::Eq, ValueType::Int, {a, zero});
    }
    case Expr::Kind::Binary: {
      if (e.text == "&&" || e.text == "||") {
        auto symbol = std::make_unique<Symbol>(); symbol->name = ".logic";
        auto temp = symbol.get(); module.temporaries.push_back(std::move(symbol)); function->objects.push_back(temp);
        int yes = block("logic.true"), no = block("logic.false"), end = block("logic.end");
        condition(e, yes, no);
        current = yes; store(address(temp), constant(Number::fromInt(1))); jump(end);
        current = no; store(address(temp), constant(Number::fromInt(0))); jump(end);
        current = end; return value(Op::Load, ValueType::Int, {address(temp)});
      }
      const auto &a = *e.operands[0], &b = *e.operands[1];
      Base common = a.type.base == Base::Float || b.type.base == Base::Float ? Base::Float : Base::Int;
      int x = convertValue(expression(a), a.type.base, common);
      int y = convertValue(expression(b), b.type.base, common);
      static const std::unordered_map<std::string, Op> ops = {
          {"+", Op::Add}, {"-", Op::Sub}, {"*", Op::Mul}, {"/", Op::Div}, {"%", Op::Rem},
          {"==", Op::Eq}, {"!=", Op::Ne}, {"<", Op::Lt}, {"<=", Op::Le}, {">", Op::Gt}, {">=", Op::Ge}};
      return value(ops.at(e.text), valueType(e.type.base), {x, y});
    }
    }
    throw std::logic_error("unknown expression");
  }
  void variable(const Variable &v) {
    function->objects.push_back(v.symbol);
    if (!v.initializer) return;
    if (v.symbol->type.array()) {
      Instruction i{Op::Zero}; i.args = {address(v.symbol)}; i.immediate = v.symbol->type.elements() * 4; emit(std::move(i));
    } else if (v.initializers.empty()) store(address(v.symbol), constant(v.base == Base::Float ? Number::fromFloat(0) : Number::fromInt(0)));
    for (auto [index, e] : v.initializers) {
      int data = convertValue(expression(*e), e->type.base, v.base);
      store(address(v.symbol, index * 4), data);
    }
  }
  void statement(const Stmt &s) {
    if (!live()) return;
    switch (s.kind) {
    case Stmt::Kind::Block: for (const auto &child : s.children) statement(*child); return;
    case Stmt::Kind::Declaration: for (const auto &v : s.variables) variable(v); return;
    case Stmt::Kind::Expression: if (s.expression) expression(*s.expression); return;
    case Stmt::Kind::Assignment: {
      int ptr = address(*s.target);
      int data = convertValue(expression(*s.expression), s.expression->type.base, s.target->type.base);
      store(ptr, data); return;
    }
    case Stmt::Kind::Return: {
      Instruction i{Op::Return};
      if (s.expression) i.args.push_back(convertValue(expression(*s.expression), s.expression->type.base, function->source->result));
      emit(std::move(i)); return;
    }
    case Stmt::Kind::Break: jump(loops.back().second); return;
    case Stmt::Kind::Continue: jump(loops.back().first); return;
    case Stmt::Kind::If: {
      int yes = block("if.then"), end = block("if.end"), no = s.children.size() == 2 ? block("if.else") : end;
      condition(*s.expression, yes, no);
      current = yes; statement(*s.children[0]); jump(end);
      if (no != end) { current = no; statement(*s.children[1]); jump(end); }
      current = end; return;
    }
    case Stmt::Kind::While: {
      int test = block("while.test"), body = block("while.body"), end = block("while.end");
      jump(test); current = test; condition(*s.expression, body, end);
      loops.emplace_back(test, end); current = body; statement(*s.children[0]); jump(test);
      loops.pop_back(); current = end; return;
    }
    }
  }
public:
  ModuleIR run(const ProgramIR &program) {
    module.source = &program;
    for (const auto &item : program.items) {
      if (!item.function) continue;
      module.functions.push_back({item.function.get(), {}, {}, {}}); function = &module.functions.back();
      parameters.clear(); current = block("entry");
      int index = 0;
      for (const auto &v : item.function->parameters) {
        auto &s = *v.symbol;
        int arg = value(Op::Parameter, s.type.array() ? ValueType::Pointer : valueType(s.type.base), {}, index++);
        if (s.type.array()) parameters[&s] = arg;
        else { function->objects.push_back(&s); store(address(&s), arg); }
      }
      statement(*item.function->body);
      if (live()) {
        Instruction i{Op::Return};
        if (item.function->result != Base::Void) i.args.push_back(constant(item.function->result == Base::Float ? Number::fromFloat(0) : Number::fromInt(0)));
        emit(std::move(i));
      }
    }
    return std::move(module);
  }
};

bool pure(Op op) { return op != Op::Store && op != Op::Zero && op != Op::Call && op != Op::Jump && op != Op::Branch && op != Op::Return; }

void prune(FunctionIR &f) {
  std::vector<bool> seen(f.blocks.size(), false); std::queue<int> pending; pending.push(0); seen[0] = true;
  while (!pending.empty()) {
    int b = pending.front(); pending.pop();
    const auto &last = f.blocks[b].instructions.back();
    auto visit = [&](int next) { if (next >= 0 && !seen[next]) { seen[next] = true; pending.push(next); } };
    visit(last.target); visit(last.otherwise);
  }
  std::vector<int> ids(f.blocks.size(), -1); std::vector<BasicBlock> blocks;
  for (size_t i = 0; i < f.blocks.size(); ++i) if (seen[i]) { ids[i] = blocks.size(); blocks.push_back(std::move(f.blocks[i])); }
  for (auto &b : blocks) for (auto &i : b.instructions) {
    if (i.target >= 0) i.target = ids[i.target];
    if (i.otherwise >= 0) i.otherwise = ids[i.otherwise];
  }
  f.blocks = std::move(blocks);
}

void promote(FunctionIR &f) {
  std::vector<const Instruction *> definitions(f.values.size(), nullptr);
  for (auto &b : f.blocks) for (auto &i : b.instructions) if (i.result >= 0) definitions[i.result] = &i;
  std::unordered_set<const Symbol *> escaped;
  for (auto &b : f.blocks) for (auto &i : b.instructions) for (size_t n = 0; n < i.args.size(); ++n) {
    auto def = definitions[i.args[n]];
    if (def && def->op == Op::Address && !(n == 0 && (i.op == Op::Load || i.op == Op::Store))) escaped.insert(def->symbol);
  }
  std::unordered_map<const Symbol *, int> homes;
  for (const auto *s : f.objects) if (!s->type.array() && !escaped.count(s)) {
    int id = f.values.size(); f.values.push_back(valueType(s->type.base)); homes[s] = id;
  }
  for (auto &b : f.blocks) for (auto &i : b.instructions) if (i.op == Op::Load || i.op == Op::Store) {
    auto def = definitions[i.args[0]];
    if (!def || def->op != Op::Address || !homes.count(def->symbol)) continue;
    int home = homes.at(def->symbol);
    if (i.op == Op::Load) { i.op = Op::Move; i.args = {home}; }
    else { i.op = Op::Move; i.result = home; i.args = {i.args[1]}; }
  }
  f.objects.erase(std::remove_if(f.objects.begin(), f.objects.end(), [&](const Symbol *s) { return homes.count(s); }), f.objects.end());
}

void eliminateDead(FunctionIR &f) {
  bool changed;
  do {
    changed = false;
    std::vector<int> uses(f.values.size(), 0);
    for (const auto &b : f.blocks) for (const auto &i : b.instructions) for (int arg : i.args) ++uses[arg];
    for (auto &b : f.blocks) {
      auto end = std::remove_if(b.instructions.begin(), b.instructions.end(), [&](const Instruction &i) {
        return i.result >= 0 && uses[i.result] == 0 && pure(i.op);
      });
      changed |= end != b.instructions.end(); b.instructions.erase(end, b.instructions.end());
    }
  } while (changed);
}
} // namespace

ModuleIR lower(const ProgramIR &program) { return Lowering().run(program); }
void optimize(ModuleIR &module, int level) {
  for (auto &f : module.functions) {
    prune(f);
    if (level > 0) { promote(f); eliminateDead(f); }
  }
}
void verify(const ModuleIR &module) {
  for (const auto &f : module.functions) {
    if (f.blocks.empty()) throw std::logic_error("IR function has no entry block");
    for (const auto &b : f.blocks) {
      if (b.instructions.empty()) throw std::logic_error("IR block has no terminator");
      for (size_t n = 0; n < b.instructions.size(); ++n) {
        const auto &i = b.instructions[n];
        if (i.result >= static_cast<int>(f.values.size())) throw std::logic_error("IR result is out of range");
        for (int arg : i.args) if (arg < 0 || arg >= static_cast<int>(f.values.size())) throw std::logic_error("IR operand is out of range");
        bool terminal = i.op == Op::Jump || i.op == Op::Branch || i.op == Op::Return;
        if (terminal != (n + 1 == b.instructions.size())) throw std::logic_error("IR terminator is misplaced");
        if ((i.op == Op::Jump || i.op == Op::Branch) && (i.target < 0 || i.target >= static_cast<int>(f.blocks.size())))
          throw std::logic_error("IR branch target is out of range");
        if (i.op == Op::Branch && (i.otherwise < 0 || i.otherwise >= static_cast<int>(f.blocks.size())))
          throw std::logic_error("IR false target is out of range");
      }
    }
  }
}
void printIR(const ModuleIR &module, std::ostream &out) {
  static const char *ops[] = {"constant", "address", "string", "parameter", "move", "load", "store", "offset", "zero",
      "add", "sub", "mul", "div", "rem", "neg", "convert", "eq", "ne", "lt", "le", "gt", "ge", "call", "jump", "branch", "return"};
  static const char *types[] = {"i32", "f32", "ptr", "f64"};
  for (const auto &f : module.functions) {
    out << "function " << f.source->name << " {\n";
    for (size_t b = 0; b < f.blocks.size(); ++b) {
      out << "  b" << b << " " << f.blocks[b].name << ":\n";
      for (const auto &i : f.blocks[b].instructions) {
        out << "    ";
        if (i.result >= 0) out << '%' << i.result << ':' << types[static_cast<int>(f.values[i.result])] << " = ";
        out << ops[static_cast<int>(i.op)];
        if (i.symbol) out << ' ' << i.symbol->name;
        if (i.callee) out << ' ' << i.callee->linkageName;
        for (int arg : i.args) out << " %" << arg;
        if (i.op == Op::Constant || i.op == Op::Parameter || i.op == Op::Offset || i.op == Op::Zero || i.op == Op::Address) out << ' ' << i.immediate;
        if (i.op == Op::String) out << ' ' << std::quoted(i.text);
        if (i.target >= 0) out << " b" << i.target;
        if (i.otherwise >= 0) out << " b" << i.otherwise;
        out << '\n';
      }
    }
    out << "}\n";
  }
}
} // namespace sysy
