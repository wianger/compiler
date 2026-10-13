#include "machine_ir.hpp"

#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>

namespace {
enum class Output { Assembly, Tokens, HighIR, ExpandedIR, ControlIR };
struct Options {
  std::string input, output;
  Output kind = Output::Assembly;
  int optimization = 0;
  std::optional<std::string> passes;
};
void help() {
  std::cout << "Usage: compiler [options] input.sy\n"
      "  -S                 emit RV64GC / LP64D assembly (default)\n"
      "  -o PATH            output file; - means stdout\n"
      "  -O0 / -O1 / -O2 / -O3  disable / enable IR optimization\n"
      "  --emit-tokens      print Token IR\n"
      "  --emit-hir         print typed, resolved source IR\n"
      "  --emit-expanded-hir  print source IR after tensor expansion\n"
      "  --emit-ir          print control-flow IR after optimization\n"
      "  --passes=PIPELINE  override IR passes: prune,promote,dce\n"
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
    std::ostringstream input;
    if (o.input == "-") input << std::cin.rdbuf();
    else {
      std::ifstream file(o.input, std::ios::binary);
      if (!file) throw std::runtime_error("cannot read input: " + o.input);
      input << file.rdbuf();
    }
    source.text = input.str();
    auto tokens = sysy::tokenize(source);
    std::ostringstream result;
    if (o.kind == Output::Tokens) {
      for (const auto &t : tokens) result << t.location.line << ':' << t.location.column << ' ' << std::quoted(t.text) << '\n';
    } else {
      auto program = sysy::parse(tokens);
      sysy::resolve(program);
      if (o.kind == Output::HighIR) sysy::printIR(program, result);
      else {
        sysy::expandTensors(program);
        if (o.kind == Output::ExpandedIR) sysy::printIR(program, result);
        else {
          auto module = sysy::lower(program);
          sysy::verify(module);
          if (o.passes) sysy::runPasses(module, *o.passes);
          else sysy::optimize(module, o.optimization);
          if (o.kind == Output::ControlIR) sysy::printIR(module, result);
          else sysy::emitAssembly(module, result);
        }
      }
    }
    if (o.output == "-") std::cout << result.str();
    else {
      std::ofstream file(o.output, std::ios::binary);
      if (!file || !(file << result.str())) throw std::runtime_error("cannot write output: " + o.output);
    }
    return 0;
  } catch (const sysy::Error &e) { diagnostic(source, e); }
  catch (const std::exception &e) { std::cerr << "compiler: error: " << e.what() << '\n'; }
  return 1;
}
