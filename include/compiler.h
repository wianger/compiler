#include "lexer.h"
#include "mips.h"
#include "parser.h"

class Compiler {
public:
  Compiler(const std::string &source) : source(source) {}
  void compile();

private:
  std::string source;
};
