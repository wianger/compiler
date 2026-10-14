#include "target_ir.hpp"

#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>

namespace {
enum class Output { Assembly, Tokens, HighIR, ExpandedIR, ControlIR, MachineIR, AllocatedIR, TensorPlan, MemoryIR, Effects };
struct Options {
  std::string input, output;
  Output kind = Output::Assembly;
  int optimization = 0;
  int tensorTile = 0;
  std::optional<std::string> passes;
  bool stats = false, checkAllocation = false;
};
void help() {
  std::cout << "Usage: compiler [options] input.sy\n"
      "  -S                 emit RV64GC / LP64D assembly (default)\n"
      "  -o PATH            output file; - means stdout\n"
      "  -O0 / -O1 / -O2 / -O3  none / scalar / loop+tensor / unroll+repetition\n"
      "  --emit-tokens      print Token IR\n"
      "  --emit-hir         print typed, resolved source IR\n"
      "  --emit-expanded-hir  print source IR after tensor expansion\n"
      "  --emit-ir          print control-flow IR after optimization\n"
      "  --emit-mir         print target IR after phi lowering and instruction selection\n"
      "  --emit-allocated-mir  print physical locations and spill decisions\n"
      "  --check-alloc      run symbolic allocation dataflow checker\n"
      "  --emit-tensor-plan print shaped iteration domains and kernel schedules\n"
      "  --tensor-tile=N    override automatic column tile (1, 2, 4, 8)\n"
      "  --emit-memory-ir   print memory definitions, phis, uses and clobbers\n"
      "  --emit-effects     print function read/write and observable-effect summaries\n"
      "  --stats            print stage timings and peak RSS as JSON to stderr\n"
      "  --passes=PIPELINE  override ordered IR passes (comma separated)\n"
      "    prune,inline,globaldce,promote,ssa,simplify,sccp,gvn,memory,egraph,tail,arith,licm,strength,rotate,unroll,dce,compact,lower-phi,isel,layout\n"
      "  --help             print this help\n";
}
Options options(int argc, char **argv) {
  Options o;
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "-S") o.kind = Output::Assembly;
    else if (arg == "-o") {
      if (++i == argc) throw std::runtime_error("-o requires a path");
      o.output = argv[i];
    } else if (arg == "--emit-tokens") o.kind = Output::Tokens;
    else if (arg == "--emit-hir") o.kind = Output::HighIR;
    else if (arg == "--emit-expanded-hir") o.kind = Output::ExpandedIR;
    else if (arg == "--emit-ir") o.kind = Output::ControlIR;
    else if (arg == "--emit-mir") o.kind = Output::MachineIR;
    else if (arg == "--emit-allocated-mir") o.kind = Output::AllocatedIR;
    else if (arg == "--check-alloc") o.checkAllocation = true;
    else if (arg == "--emit-tensor-plan") o.kind = Output::TensorPlan;
    else if (arg.rfind("--tensor-tile=",0) == 0) {
      auto tile = arg.substr(14);
      if (tile != "1" && tile != "2" && tile != "4" && tile != "8") throw std::runtime_error("tensor tile must be 1, 2, 4 or 8");
      o.tensorTile = std::stoi(tile);
    }
    else if (arg == "--emit-memory-ir") o.kind = Output::MemoryIR;
    else if (arg == "--emit-effects") o.kind = Output::Effects;
    else if (arg == "--stats") o.stats = true;
    else if (arg.rfind("--passes=", 0) == 0) o.passes = arg.substr(9);
    else if (arg.size() == 3 && arg.substr(0, 2) == "-O" && arg[2] >= '0' && arg[2] <= '3') o.optimization = arg[2] - '0';
    else if (arg != "-" && !arg.empty() && arg[0] == '-') throw std::runtime_error("unknown option: " + arg);
    else {
      if (!o.input.empty()) throw std::runtime_error("expected exactly one input file");
      o.input = arg;
    }
  }
  if (o.input.empty()) throw std::runtime_error("no input file; use --help for usage");
  if (o.output.empty()) o.output = o.kind == Output::Assembly ? "a.s" : "-";
  return o;
}
void diagnostic(const sysy::Source &source, const sysy::Error &error) {
  std::cerr << source.path << ':' << error.location.line << ':' << error.location.column << ": error: " << error.what() << '\n';
  std::istringstream lines(source.text); std::string line;
  for (int n = 1; n <= error.location.line && std::getline(lines, line); ++n)
    if (n == error.location.line) std::cerr << line << '\n' << std::string(error.location.column - 1, ' ') << "^\n";
}
} // namespace

int main(int argc, char **argv) {
  sysy::Source source;
  try {
    for (int i = 1; i < argc; ++i) if (std::string(argv[i]) == "--help" || std::string(argv[i]) == "-h") { help(); return 0; }
    auto o = options(argc, argv); source.path = o.input;
    sysy::Stats stats(o.stats);
    std::ostringstream input;
    if (o.input == "-") input << std::cin.rdbuf();
    else {
      std::ifstream file(o.input, std::ios::binary);
      if (!file) throw std::runtime_error("cannot read input: " + o.input);
      input << file.rdbuf();
    }
    source.text = input.str();
    auto tokens = stats.measure("lex", [&] { return sysy::tokenize(source); });
    std::ostringstream result;
    if (o.kind == Output::Tokens) {
      for (const auto &t : tokens) result << t.location.line << ':' << t.location.column << ' ' << std::quoted(std::string(t.text)) << '\n';
    } else {
      auto program = stats.measure("parse", [&] { return sysy::parse(tokens); });
      tokens = sysy::TokenIR{};
      stats.measure("resolve", [&] { sysy::resolve(program); });
      if (o.kind == Output::HighIR) sysy::printIR(program, result);
      else {
        if (o.optimization >= 2) stats.measure("tensor.analysis", [&] { sysy::analyzeTensors(program); });
        stats.measure("tensor.plan", [&] { sysy::planTensors(program,o.tensorTile); });
        if (o.kind == Output::TensorPlan) sysy::printTensorPlan(program,result);
        else {
          stats.measure("tensor.expand", [&] { sysy::expandTensors(program); });
          stats.measure("hir.optimize", [&] { sysy::optimizeHighIR(program, o.optimization); });
          if (o.kind == Output::ExpandedIR) sysy::printIR(program, result);
          else {
            auto module = stats.measure("lower", [&] { return sysy::lower(program); });
            sysy::verify(module);
            if (o.passes) sysy::runPasses(module, *o.passes, &stats);
            else sysy::optimize(module, o.optimization, &stats);
            for (const auto &f : module.functions) {
              stats.count("cfg.hits", f.cfgHits); stats.count("cfg.builds", f.cfgBuilds);
            }
            if (o.kind == Output::MachineIR) {
              stats.measure("selection", [&] { sysy::printTarget(module, result); });
            } else if (o.kind == Output::AllocatedIR) sysy::printAllocated(module,result,&stats,o.checkAllocation);
            else if (o.kind == Output::MemoryIR) sysy::printMemoryIR(module, result);
            else if (o.kind == Output::Effects) sysy::printEffects(module, result);
            else if (o.kind == Output::ControlIR) sysy::printIR(module, result);
            else stats.measure("backend", [&] { sysy::emitAssembly(module, result,&stats,o.checkAllocation); });
          }
        }
      }
      stats.count("expr.nodes", program.expressions->size());
      stats.count("stmt.nodes", program.statements->size());
    }
    if (o.output == "-") std::cout << result.str();
    else {
      std::ofstream file(o.output, std::ios::binary);
      if (!file || !(file << result.str())) throw std::runtime_error("cannot write output: " + o.output);
    }
    stats.print(std::cerr);
    return 0;
  } catch (const sysy::Error &e) { diagnostic(source, e); }
  catch (const std::exception &e) { std::cerr << "compiler: error: " << e.what() << '\n'; }
  return 1;
}
