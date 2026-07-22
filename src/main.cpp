#include "compiler.h"
#include <fstream>
#include <iostream>

int main() {
  std::ifstream file("../test/testfile15.txt");
  if (!file.is_open()) {
    std::cerr << "Error opening file" << std::endl;
    return 1;
  }
  std::string source((std::istreambuf_iterator<char>(file)),
                     std::istreambuf_iterator<char>());
  Compiler compiler(source);
  compiler.compile();
  return 0;
}