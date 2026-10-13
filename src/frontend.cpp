#include "ir.hpp"

#include <cctype>
#include <unordered_set>

namespace sysy {
namespace {
bool digit(char c) { return c >= '0' && c <= '9'; }
bool alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
bool hex(char c) { return digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

class Lexer {
  const std::string &source;
  size_t position = 0;
  Location location;
  char peek(size_t offset = 0) const { return position + offset < source.size() ? source[position + offset] : '\0'; }
  char take() {
    char c = source[position++];
    if (c == '\n') { ++location.line; location.column = 1; }
    else ++location.column;
    return c;
  }
  std::string number() {
    size_t start = position;
    bool hexadecimal = peek() == '0' && (peek(1) == 'x' || peek(1) == 'X');
    if (hexadecimal) { take(); take(); }
    bool hasDigits = false, floating = false;
    while (hexadecimal ? hex(peek()) : digit(peek())) { take(); hasDigits = true; }
    if (peek() == '.') {
      floating = true; take();
      while (hexadecimal ? hex(peek()) : digit(peek())) { take(); hasDigits = true; }
    }
    if (!hasDigits) throw Error(location, "numeric literal requires digits");
    char exponent = hexadecimal ? 'p' : 'e';
    bool hasExponent = peek() == exponent || peek() == exponent - 'a' + 'A';
    if (hasExponent) {
      floating = true; take();
      if (peek() == '+' || peek() == '-') take();
      if (!digit(peek())) throw Error(location, "numeric exponent requires digits");
      while (digit(peek())) take();
    }
    if (hexadecimal && floating && !hasExponent)
      throw Error(location, "hexadecimal float requires a p exponent");
    if (floating) { if (peek() == 'f' || peek() == 'F' || peek() == 'l' || peek() == 'L') take(); }
    else { while (peek() == 'u' || peek() == 'U' || peek() == 'l' || peek() == 'L') take(); }
    if (alpha(peek()) || peek() == '.') throw Error(location, "invalid numeric literal");
    return source.substr(start, position - start);
  }
  std::string string() {
    Location start = location;
    take();
    std::string result;
    while (position < source.size() && peek() != '"') {
      if (peek() == '\n' || peek() == '\r') throw Error(start, "newline in string literal");
      char c = take();
      if (c != '\\') { result += c; continue; }
      if (position == source.size()) throw Error(start, "unterminated string literal");
      c = take();
      switch (c) {
      case 'a': result += '\a'; break;
      case 'b': result += '\b'; break;
      case 'f': result += '\f'; break;
      case 'n': result += '\n'; break;
      case 'r': result += '\r'; break;
      case 't': result += '\t'; break;
      case 'v': result += '\v'; break;
      case '\\': case '"': case '\'': case '?': result += c; break;
      case 'x': {
        if (!hex(peek())) throw Error(location, "hex escape requires digits");
        unsigned value = 0;
        while (hex(peek())) {
          char h = take();
          value = value * 16 + (digit(h) ? h - '0' : std::tolower(static_cast<unsigned char>(h)) - 'a' + 10);
        }
        result += static_cast<char>(value); break;
      }
      default:
        if (c < '0' || c > '7') throw Error(location, "unknown string escape");
        unsigned value = c - '0';
        for (int i = 1; i < 3 && peek() >= '0' && peek() <= '7'; ++i) value = value * 8 + take() - '0';
        result += static_cast<char>(value);
      }
    }
    if (position == source.size()) throw Error(start, "unterminated string literal");
    take();
    return result;
  }
public:
  explicit Lexer(const Source &source) : source(source.text) {}
  TokenIR run() {
    TokenIR result;
    while (position < source.size()) {
      if (std::isspace(static_cast<unsigned char>(peek()))) { take(); continue; }
      if (peek() == '/' && peek(1) == '/') {
        while (position < source.size() && peek() != '\n') take();
        continue;
      }
      if (peek() == '/' && peek(1) == '*') {
        Location start = location; take(); take();
        while (position < source.size() && !(peek() == '*' && peek(1) == '/')) take();
        if (position == source.size()) throw Error(start, "unterminated comment");
        take(); take(); continue;
      }
      Location start = location;
      if (alpha(peek())) {
        size_t begin = position;
        while (alpha(peek()) || digit(peek())) take();
        result.push_back({TokenKind::Identifier, source.substr(begin, position - begin), start});
      } else if (digit(peek()) || (peek() == '.' && digit(peek(1)))) {
        result.push_back({TokenKind::Number, number(), start});
      } else if (peek() == '"') {
        result.push_back({TokenKind::String, string(), start});
      } else {
        std::string op(1, take());
        if ((op == "&" && peek() == '&') || (op == "|" && peek() == '|') ||
            ((op == "=" || op == "!" || op == "<" || op == ">") && peek() == '=')) op += take();
        if (std::string("+-*/%@!=<>(){}[],;").find(op[0]) == std::string::npos && op != "&&" && op != "||")
          throw Error(start, "unexpected character '" + op + "'");
        result.push_back({TokenKind::Punctuation, op, start});
      }
    }
    result.push_back({TokenKind::End, "", location});
    return result;
  }
};

class Parser {
  const TokenIR &tokens;
  size_t position = 0;
  const Token &peek() const { return tokens.at(position); }
  bool at(const std::string &text) const { return peek().text == text; }
  Token take() { return tokens.at(position++); }
  bool accept(const std::string &text) { if (!at(text)) return false; take(); return true; }
  Token expect(const std::string &text) {
    if (!at(text)) throw Error(peek().location, "expected '" + text + "', found '" + peek().text + "'");
    return take();
  }
  Token identifier() {
    static const std::unordered_set<std::string> keywords = {
        "const", "tensor", "int", "float", "void", "if", "else", "while", "break", "continue", "return"};
    if (peek().kind != TokenKind::Identifier || keywords.count(peek().text))
      throw Error(peek().location, "expected identifier");
    return take();
  }
  Base base(bool allowVoid = false) {
    if (accept("int")) return Base::Int;
    if (accept("float")) return Base::Float;
    if (allowVoid && accept("void")) return Base::Void;
    throw Error(peek().location, "expected type");
  }
  static int precedence(const std::string &op) {
    if (op == "||") return 1;
    if (op == "&&") return 2;
    if (op == "==" || op == "!=") return 3;
    if (op == "<" || op == ">" || op == "<=" || op == ">=") return 4;
    if (op == "+" || op == "-") return 5;
    if (op == "*" || op == "/" || op == "%" || op == "@") return 6;
    return 0;
  }
  std::unique_ptr<Expr> primary() {
    if (accept("(")) { auto e = expression(); expect(")"); return e; }
    if (at("+") || at("-") || at("!")) {
      auto op = take();
      auto e = std::make_unique<Expr>(Expr::Kind::Unary, op.location, op.text);
      e->operands.push_back(primary()); return e;
    }
    if (peek().kind == TokenKind::Number || peek().kind == TokenKind::String) {
      auto t = take();
      return std::make_unique<Expr>(t.kind == TokenKind::Number ? Expr::Kind::Number : Expr::Kind::String, t.location, t.text);
    }
    auto name = identifier();
    auto e = std::make_unique<Expr>(Expr::Kind::Variable, name.location, name.text);
    if (accept("(")) {
      e->kind = Expr::Kind::Call;
      if (!at(")")) {
        do { e->operands.push_back(expression()); } while (accept(","));
      }
      expect(")");
    } else {
      while (accept("[")) { e->operands.push_back(expression()); expect("]"); }
    }
    return e;
  }
  std::unique_ptr<Expr> expression(int minimum = 1) {
    auto left = primary();
    while (precedence(peek().text) >= minimum) {
      auto op = take();
      auto e = std::make_unique<Expr>(Expr::Kind::Binary, op.location, op.text);
      e->operands.push_back(std::move(left));
      e->operands.push_back(expression(precedence(op.text) + 1));
      left = std::move(e);
    }
    return left;
  }
  Initializer initializer() {
    Initializer init; init.location = peek().location;
    if (!accept("{")) { init.value = expression(); return init; }
    init.list = true;
    if (!at("}")) {
      do {
        if (at("}")) break;
        init.children.push_back(initializer());
      } while (accept(","));
    }
    expect("}");
    return init;
  }
  Variable variable(Token name, Base type, bool isConst, bool parameter = false, bool tensor = false) {
    Variable v; v.location = name.location; v.name = name.text; v.base = type; v.isConst = isConst; v.tensor = tensor;
    if (parameter && accept("[")) { expect("]"); v.arrayParameter = true; }
    while (accept("[")) { v.dimensions.push_back(expression()); expect("]"); }
    if (!parameter) {
      if (accept("=")) v.initializer = initializer();
      else if (isConst) throw Error(name.location, "constant requires an initializer");
    }
    return v;
  }
  std::vector<Variable> declaration(Base type, bool isConst, Token name, bool tensor = false) {
    std::vector<Variable> variables;
    variables.push_back(variable(std::move(name), type, isConst, false, tensor));
    while (accept(",")) variables.push_back(variable(identifier(), type, isConst, false, tensor));
    expect(";");
    return variables;
  }
  std::unique_ptr<Stmt> block() {
    auto s = std::make_unique<Stmt>(Stmt::Kind::Block, expect("{").location);
    while (!at("}")) {
      if (peek().kind == TokenKind::End) throw Error(peek().location, "unterminated block");
      if (at("const") || at("tensor") || at("int") || at("float")) {
        auto d = std::make_unique<Stmt>(Stmt::Kind::Declaration, peek().location);
        bool isConst = accept("const");
        bool tensor = accept("tensor");
        Base type = base();
        d->variables = declaration(type, isConst, identifier(), tensor);
        s->children.push_back(std::move(d));
      } else s->children.push_back(statement());
    }
    expect("}"); return s;
  }
  std::unique_ptr<Stmt> statement() {
    if (at("{")) return block();
    Location loc = peek().location;
    auto s = std::make_unique<Stmt>(Stmt::Kind::Expression, loc);
    if (accept("if")) {
      s->kind = Stmt::Kind::If; expect("("); s->expression = expression(); expect(")");
      s->children.push_back(statement());
      if (accept("else")) s->children.push_back(statement());
    } else if (accept("while")) {
      s->kind = Stmt::Kind::While; expect("("); s->expression = expression(); expect(")");
      s->children.push_back(statement());
    } else if (accept("break")) { s->kind = Stmt::Kind::Break; expect(";"); }
    else if (accept("continue")) { s->kind = Stmt::Kind::Continue; expect(";"); }
    else if (accept("return")) {
      s->kind = Stmt::Kind::Return;
      if (!at(";")) s->expression = expression();
      expect(";");
    } else if (!accept(";")) {
      s->expression = expression();
      if (accept("=")) {
        s->kind = Stmt::Kind::Assignment;
        s->target = std::move(s->expression); s->expression = expression();
      }
      expect(";");
    }
    return s;
  }
public:
  explicit Parser(const TokenIR &tokens) : tokens(tokens) {}
  ProgramIR run() {
    ProgramIR program;
    while (peek().kind != TokenKind::End) {
      bool isConst = accept("const");
      bool tensor = accept("tensor");
      Base type = base(!isConst);
      auto name = identifier();
      Item item;
      if (accept("(")) {
        if (isConst) throw Error(name.location, "function cannot be const");
        if (tensor && type == Base::Void) throw Error(name.location, "tensor function requires int or float elements");
        item.function = std::make_unique<Function>();
        auto &f = *item.function;
        f.location = name.location; f.name = f.linkageName = name.text; f.result = type;
        f.tensorResult = tensor;
        if (!at(")")) {
          do {
            bool paramTensor = accept("tensor");
            Base paramType = base();
            f.parameters.push_back(variable(identifier(), paramType, false, true, paramTensor));
          } while (accept(","));
        }
        expect(")"); f.body = block();
      } else {
        if (type == Base::Void) throw Error(name.location, "variable cannot have void type");
        item.variables = declaration(type, isConst, std::move(name), tensor);
      }
      program.items.push_back(std::move(item));
    }
    return program;
  }
};
} // namespace

TokenIR tokenize(const Source &source) { return Lexer(source).run(); }
ProgramIR parse(const TokenIR &tokens) { return Parser(tokens).run(); }
} // namespace sysy
