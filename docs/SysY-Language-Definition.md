# SysY Language Definition

This document defines the language accepted by this repository: SysY2022 plus the `tensor` extension exercised by `test/`. The grammar below includes that extension; Section 4 specifies its shape, value and evaluation rules. Ordinary arrays retain their SysY semantics.

## 1. Overview of the SysY Language

SysY is the programming language to be implemented in the Compiler System Design Competition. It is extended from a subset of the C language. The source code of each SysY program is stored in a file whose extension is `.sy`. Such a file must contain exactly one definition of the `main` function, and may also contain several global variable declarations, constant declarations, and other function definitions. SysY supports the types `int` and `float`, as well as multidimensional array types whose element type is `int` or `float` and whose storage order is row-major. Here, `int` is a 32-bit signed integer, `float` is a 32-bit single-precision floating-point number, and the `const` qualifier is used to declare constants. SysY supports implicit conversions between `int` and `float`, but does not support explicit cast syntax.[^1]

SysY itself does not provide built-in language constructs for input/output (I/O). I/O is provided by way of a runtime library, and library functions may be called inside functions in SysY programs. Some SysY runtime-library functions accept parameter types beyond the data types supported by SysY itself, such as strings. A SysY compiler must be able to handle such cases and correctly pass such arguments from a SysY program to the SysY runtime library. For the library functions available in SysY programs, see the SysY runtime-library documentation.

**Functions:** A function may have parameters or may have none. Parameter types may be `int`, `float`, or array types. A function may return a value of type `int` or `float`, or may return no value (that is, it may be declared with type `void`). Parameters of type `int`/`float` are passed by value; parameters of array type are passed by passing the starting address of the array, and only the first dimension of the formal parameter may omit its length. A function body consists of several variable declarations and statements.

**Variable/constant declarations:** A single declaration statement may declare multiple variables or constants, and initialization expressions may be provided in the declaration. All variables/constants must be defined before use. Declarations outside functions are global variables/constants, while declarations inside functions are local variables/constants.

**Statements:** Statements include assignment statements, expression statements (where the expression may be empty), statement blocks, `if` statements, `while` statements, `break` statements, `continue` statements, and `return` statements. A statement block may contain several variable declarations and statements.

**Expressions:** SysY supports basic arithmetic operations (`+`, `-`, `*`, `/`, `%`), relational operations (`==`, `!=`, `<`, `>`, `<=`, `>=`), and logical operations (`!`, `&&`, `||`). A nonzero value means true, and 0 means false; the result of a relational or logical operation is represented as 1 for true and 0 for false. Operator precedence, associativity, and evaluation rules, including short-circuit evaluation for logical operations, are the same as in C.

The repository additionally supports `tensor int` and `tensor float` arrays, elementwise arithmetic, scalar broadcast, whole-tensor assignment, tensor function results and rank-two matrix multiplication with `@`.

## 2. Grammar of the SysY Language

The grammar of SysY is written in Extended Backus-Naur Form (EBNF), where:

- `[...]` denotes an optional item.
- `{...}` denotes an item that may be repeated zero or more times.
- A terminal symbol is either a quoted string or a token such as `Ident`, `IntConst`, `FloatConst`, or `StringLiteral`.

The grammar of SysY is as follows. `CompUnit` is the start symbol:

```ebnf
CompUnit     -> [ CompUnit ] ( Decl | FuncDef )
Decl         -> ConstDecl | VarDecl
ConstDecl    -> 'const' [ 'tensor' ] BType ConstDef { ',' ConstDef } ';'
BType        -> 'int' | 'float'
ConstDef     -> Ident { '[' ConstExp ']' } '=' ConstInitVal
ConstInitVal -> ConstExp
             | '{' [ ConstInitVal { ',' ConstInitVal } ] '}'

VarDecl        -> [ 'tensor' ] BType VarDef { ',' VarDef } ';'
VarDef         -> Ident { '[' ConstExp ']' }
               | Ident { '[' ConstExp ']' } '=' InitVal
InitVal        -> Exp | '{' [ InitVal { ',' InitVal } ] '}'

FuncDef        -> FuncType Ident '(' [FuncFParams] ')' Block
FuncType       -> 'void' | [ 'tensor' ] BType
FuncFParams    -> FuncFParam { ',' FuncFParam }
FuncFParam     -> [ 'tensor' ] BType Ident ['[' ']' { '[' ConstExp ']' }]

Block          -> '{' { BlockItem } '}'
BlockItem      -> Decl | Stmt
Stmt           -> LVal '=' Exp ';'
               | [Exp] ';'
               | Block
               | 'if' '(' Cond ')' Stmt [ 'else' Stmt ]
               | 'while' '(' Cond ')' Stmt
               | 'break' ';'
               | 'continue' ';'
               | 'return' [Exp] ';'

Exp            -> LOrExp
Cond           -> LOrExp
LVal           -> Ident {'[' Exp ']'}
PrimaryExp     -> '(' Exp ')' | LVal | Number | StringLiteral
Number         -> IntConst | FloatConst
UnaryExp       -> PrimaryExp | Ident '(' [FuncRParams] ')'
               | UnaryOp UnaryExp
UnaryOp        -> '+' | '-' | '!'
FuncRParams    -> Exp { ',' Exp }
MulExp         -> UnaryExp | MulExp ('*' | '/' | '%' | '@') UnaryExp
AddExp         -> MulExp | AddExp ('+' | '-') MulExp
RelExp         -> AddExp | RelExp ('<' | '>' | '<=' | '>=') AddExp
EqExp          -> RelExp | EqExp ('==' | '!=') RelExp
LAndExp        -> EqExp | LAndExp '&&' EqExp
LOrExp         -> LAndExp | LOrExp '||' LAndExp
ConstExp       -> LOrExp
```

**Notes:**

- Scalar `Exp` has type `int`/`float`. Tensor expressions and array arguments are subject to the constraints below.
- Relational and logical expressions, including `!`, may also appear as values, initializers, arguments and return expressions. Logical operations still use scalar operands and short-circuit evaluation.
- A `StringLiteral` may appear only in a runtime-library argument that requires a string, such as the format argument of `putf`; it is not a user-declarable type.
- Any `Ident` used in `ConstExp` must denote a constant.

### Terminal Symbols in SysY

#### 1. Identifier `Ident`

The syntax of identifiers `Ident` in SysY is as follows (`identifier`):

```ebnf
identifier -> identifier-nondigit
            | identifier identifier-nondigit
            | identifier digit
```

Here, `identifier-nondigit` is `_` or one of the following:

```text
a b c d e f g h i j k l m
n o p q r s t u v w x y z
A B C D E F G H I J K L M
N O P Q R S T U V W X Y Z
```

`digit` is one of the following:

```text
0 1 2 3 4 5 6 7 8 9
```

**Note:** See the definition of identifiers on page 51 of ISO/IEC 9899:
http://www.open-std.org/jtc1/sc22/wg14/www/docs/n1124.pdf

Conventions for identically named identifiers:

- `const`, `tensor`, `int`, `float`, `void`, `if`, `else`, `while`, `break`, `continue` and `return` are reserved keywords.
- The scopes of global variables and local variables may overlap; in the overlapping region, the local variable takes precedence.
- Two local declarations in the same scope may not use the same name. A nested block may shadow a declaration in an outer scope.
- In SysY, a variable name may be the same as a function name.

#### 2. Comments

The syntax of comments in SysY is the same as in C:

- Single-line comments start with the sequence `//` and end at the newline character; the newline itself is not part of the comment.
- Multi-line comments start with the sequence `/*` and end at the first occurrence of `*/`, including the closing `*/`.

**Note:** See the definition of comments on page 66 of ISO/IEC 9899:
http://www.open-std.org/jtc1/sc22/wg14/www/docs/n1124.pdf

#### 3. Numeric Constants

Numeric constants in SysY may be integer constants `IntConst` or floating-point constants `FloatConst`.

The syntax of integer constants `IntConst` is as follows (corresponding to `integer-const`):

```ebnf
integer-const        -> decimal-const | octal-const
                     | hexadecimal-const
decimal-const        -> nonzero-digit | decimal-const digit
octal-const          -> 0 | octal-const octal-digit
hexadecimal-const    -> hexadecimal-prefix hexadecimal-digit
                     | hexadecimal-const hexadecimal-digit

hexadecimal-prefix -> '0x' | '0X'
```

`nonzero-digit` is one of:

```text
1 2 3 4 5 6 7 8 9
```

`octal-digit` is one of:

```text
0 1 2 3 4 5 6 7
```

`hexadecimal-digit` is one of:

```text
0 1 2 3 4 5 6 7 8 9 a b c d e f A B C D E F
```

**Note:** See the definition of integer constants on page 54 of ISO/IEC 9899:
http://www.open-std.org/jtc1/sc22/wg14/www/docs/n1124.pdf
On that basis, all suffixes are ignored here.

The definition of floating-point constants `FloatConst` refers to the definition of floating constants on page 57 of ISO/IEC 9899: `http://www.open-std.org/jtc1/sc22/wg14/www/docs/n112` (the URL is visibly truncated in the source PDF). On that basis, all suffixes are ignored here as well.

#### 4. String Literals

String arguments use double-quoted `StringLiteral` tokens. Supported escapes are `\a`, `\b`, `\f`, `\n`, `\r`, `\t`, `\v`, `\\`, `\"`, `\'`, `\?`, one to three octal digits, and `\x` followed by hexadecimal digits. Character literals are not supported, and strings have no user-declarable type.

## 3. Semantic Constraints of the SysY Language

The set of programs matching the grammar above is a superset of the set of legal SysY programs. The semantic constraints of SysY are further specified below.

### CompUnit

```ebnf
CompUnit -> [ CompUnit ] ( Decl | FuncDef )
Decl     -> ConstDecl | VarDecl
```

1. A SysY program consists of a single file whose contents correspond to `CompUnit` in the EBNF above. In that `CompUnit`, there must exist exactly one `FuncDef` whose identifier is `main`, whose parameter list is empty, and whose return type is scalar `int`. The `main` function is the entry point and returns an exit status. The test harness appends that status to captured standard output; the compiler does not insert a print of the return value.
2. At the top level of `CompUnit`, variable/constant declaration statements (`Decl`) and function definitions (`FuncDef`) may not redefine the same identifier (`Ident`), even if the types differ.
3. The scope of a variable/constant/function declaration in `CompUnit` starts at the point of declaration and extends to the end of the file.

### ConstDef

```ebnf
ConstDef -> Ident { '[' ConstExp ']' } '=' ConstInitVal
```

1. `ConstDef` defines a symbolic constant. In `ConstDef`, `Ident` is the identifier of the constant. The part after `Ident` and before `=` optionally defines the array dimensions and the length of each dimension, and the part after `=` is the initializer.
2. If the array-dimension part of `ConstDef` is absent, a single variable is defined. In this case, the right-hand side of `=` must be a single initial numeric value.
3. If the array-dimension part of `ConstDef` is present, an array is defined. Its semantics are the same as in C. For example, `[2][8/2][1*3]` denotes a three-dimensional array whose first through third dimensions have lengths 2, 4, and 3, respectively, and whose indices start from 0 in each dimension. Every `ConstExp` used to specify a dimension length in `ConstDef` must be evaluable at compile time to a nonnegative integer.
4. SysY requires every array dimension length in a declaration to be given explicitly; unknown dimension sizes are not allowed.
5. When `ConstDef` defines an array, the `ConstInitVal` on the right-hand side of `=` is a constant initializer. The `ConstExp` expressions that appear in `ConstInitVal` are `int`/`float` expressions evaluable at compile time, and may refer to already-defined symbolic constants.
6. A `ConstInitVal` initializer must be one of the following:
   1. A pair of braces `{}`, meaning all elements are initialized to 0.
   2. An initializer that fully corresponds to the array rank and each dimension length of the multidimensional array. For example, `{{1,2},{3,4},{5,6}}`, `{1,2,3,4,5,6}`, and `{1,2,{3,4},5,6}` may all be used as initializers for `a[3][2]`.
   3. If the number of initializers in a brace-enclosed list is smaller than the number of elements in the corresponding dimension, the remaining part of that dimension is implicitly initialized. Every implicitly initialized integer element is initialized to 0. For example, `{{1, 2},{3}, {5}}`, `{1,2,{3},5}`, and `{{},{3,4},5,6}` may all be used as initializers for `a[3][2]`. The first two initialize `a` as `{{1,2},{3,0},{5,0}}`, while `{{},{3,4},5,6}` initializes `a` as `{{0,0},{3,4},{5,6}}`.
7. For example, the declarations and initializations of variables `a` through `e` below are all allowed:

   ```c
   int a[4][2] = {};
   int b[4][2] = {1, 2, 3, 4, 5, 6, 7, 8};
   int c[4][2] = {{1, 2}, {3, 4}, 5, 6, 7, 8};
   int d[4][2] = {1, 2, {3}, {5}, 7, 8};
   int e[4][2] = {{1, 2}, {3, 4}, {5, 6}, {7, 8}};
   ```

8. The type of each array-element initializer must be consistent with the declared element type of the array. For example, the initializer list of an integer array may not contain floating-point elements; however, the initializer list of a floating-point array may contain integer constants or integer constant expressions.
9. The value of each array-element initializer may not exceed the representable range of the corresponding element type.
10. The number of elements in the initializer list may not exceed the total number of elements specified in the array declaration.

### VarDef

```ebnf
VarDef -> Ident { '[' ConstExp ']' }
        | Ident { '[' ConstExp ']' } '=' InitVal
```

1. `VarDef` defines a variable. If `=` and an initializer are absent, the actual runtime initial value is undefined.
2. If the array-dimension part of `VarDef` is absent, a single variable is defined. If it is present, then, as in `ConstDef`, a multidimensional array is defined.
3. When `VarDef` contains `=` and an initializer, the structure of the `InitVal` on the right-hand side of `=` must be the same as that of `ConstInitVal`. The only difference is that the expressions in `ConstInitVal` are constant expressions `ConstExp`, while the expressions in `InitVal` may be any `Exp` valid in the current context.
4. Each `ConstExp` used to specify a dimension length in `VarDef` must be evaluable to a nonnegative integer, but the initializer values in `InitVal` are `Exp` and may therefore refer to variables. For example, in the code below, the initializer expression of variable `e` uses `d[2][1]`.

   ```c
   int a[4][2] = {};
   int b[4][2] = {1, 2, 3, 4, 5, 6, 7, 8};
   int c[4][2] = {{1, 2}, {3, 4}, 5, 6, 7, 8};
   int d[4][2] = {1, 2, {3}, {5}, 7, 8};
   int e[4][2] = {{d[2][1], c[2][1]}, {3, 4}, {5, 6}, {7, 8}};
   ```

### Initial Values

```ebnf
ConstInitVal -> ConstExp
             | '{' [ ConstInitVal { ',' ConstInitVal } ] '}'

InitVal          -> Exp
                 | '{' [ InitVal { ',' InitVal } ] '}'
```

1. In a global-variable declaration, the specified initializer expression must be a constant expression.
2. The initializer specified in a constant or variable declaration must be consistent with the type of that constant or variable. The following forms of `VarDef` / `ConstDef` do not satisfy the SysY semantic constraints:

   ```c
   a[4] = 4
   a[2] = {{1,2}, 3}
   a = {1,2,3}
   ```

3. A local variable that is not explicitly initialized has an indeterminate value; a global variable that is not explicitly initialized has its value (or its elements) initialized to 0 or 0.0.

### FuncFParam and Actual Arguments

```ebnf
FuncFParam -> [ 'tensor' ] BType Ident ['[' ']' { '[' ConstExp ']' }]
```

1. `FuncFParam` defines one formal parameter of a function. When the optional part after `Ident` is present, it denotes an array definition.
2. When `FuncFParam` is an array definition, the first-dimension length is omitted (written as `[]`), while each subsequent dimension must be given by an expression whose value is an integer constant.
3. The syntax of actual arguments is `Exp`. For parameters of type `int`/`float`, arguments are passed by value. For parameters of array type, the formal parameter receives the address of the actual argument array and accesses the actual array elements indirectly through that address.
4. For a multidimensional array, part of it may be passed to an array formal parameter. For example, if `int a[4][3]`, then `a[1]` is a one-dimensional array with three elements, and `a[1]` may be passed as an argument to a formal parameter of type `int[]`.

### FuncDef

```ebnf
FuncDef -> FuncType Ident '(' [FuncFParams] ')' Block
```

1. `FuncDef` denotes a function definition. `FuncType` specifies the return type.
   1. When the return type is `int`/`float`, every branch of the function should contain a `return` statement with an `Exp`. The return value of a branch that contains no return statement is undefined.
   2. When the return type is `void`, only `return` statements without a return value may appear in the function.
2. Each formal parameter declaration (`FuncFParam`) in the formal-parameter list (`FuncFParams`) of `FuncDef` declares either an `int`/`float` parameter or a multidimensional array parameter whose element type is `int`/`float`. The semantics of `FuncFParam` are given above.

### Block

1. `Block` denotes a statement block. A statement block creates a scope, and the lifetime of variables declared in the block is limited to that block.
2. Inside a statement block, variables or constants with the same name as declarations outside the block may be redefined (through `Decl`). Their scope begins at the point of definition and ends at the end of the block, and they hide the outer declarations with the same name.

### Stmt

1. `if` statements in `Stmt` follow the nearest-`else` matching rule.
2. A single `Exp` may appear as a `Stmt`. The expression is evaluated and the resulting value is discarded.

### LVal

1. `LVal` denotes an expression that has an lvalue; it may be a variable or an array element.
2. An array may have at most as many indices as dimensions. A fully indexed reference denotes a scalar element. Fewer indices select a subarray, which may be passed to an array parameter; a tensor subarray also retains the tensor qualifier and may participate in the operations in Section 4. An ordinary array cannot be assigned or used in scalar arithmetic as a whole.
3. When `LVal` denotes a single variable, no following brackets may appear.

### Exp and Cond

1. Scalar `Exp` and `Cond` both allow `LOrExp`. Relational and logical results have type `int` and value 0 or 1. A condition requires a scalar numeric value; arrays and tensors cannot be conditions. A dimension must evaluate at compile time to a nonnegative `int`.
2. `LVal` must denote a variable or constant that is defined in the current scope before the `Exp` statement in which it appears. For the `LVal` on the left-hand side of an assignment, it must denote a variable.
3. A function call has the form `Ident '(' [FuncRParams] ')'`. The argument count must match, except for variadic runtime functions. Scalar numeric arguments undergo implicit conversion to the parameter type. Array arguments must match the element type, rank and every dimension after the first; no elementwise conversion occurs when passing an array. Tensor parameters follow the same address-passing rule.
4. Operator precedence and associativity in SysY are the same as in C, and the grammar above already reflects those precedence and associativity rules.

### Implicit Type Conversions

The following implicit type conversions exist between values of type `int` and `float` in SysY:

1. When a value of type `float` is implicitly converted to an integer, for example by `int i = 4.0;`, the fractional part is discarded. If the integral part is outside the representable range of the integer type, the behavior is undefined.
2. Conversion from `int` to `float`, for example `float j = 3;`, rounds to the nearest representable binary32 value, with ties to even. Large integers need not remain exact.

**Note:** When implementing implicit type conversions, a compiler needs to combine the type-conversion instructions provided by the hardware architecture with the runtime ABI (Application Binary Interface). For example, on ARM, the runtime ABI function `float __aeabi_i2f(int)` may be called to convert an `int` to a `float`.

See: https://developer.arm.com/documentation/ihi0043/latest

## 4. Tensor Extension

### Declarations, indexing and shapes

`tensor` is a qualifier before `int` or `float`, optionally after `const`. A tensor variable must have at least one array dimension. Its shape is the ordered tuple of dimension lengths, all of which are nonnegative compile-time integers. Storage, brace initialization, zero filling of omitted initialized elements, scope and `const` rules are the same as for ordinary arrays. A declaration initializer uses scalar elements and braces; an expression producing a whole tensor is assigned after declaration.

```c
tensor int a[4] = {1, 2, 3, 4};
tensor float matrix[2][3] = {{1.0, 2.0}, {3.0}};
const tensor int weights[2] = {7, 11};
```

For `tensor int a[2][3]`, `a` has shape `(2,3)`, `a[1]` is a tensor view of shape `(3)`, and `a[1][2]` is a scalar `int`. A view refers to the original storage. Fully indexed tensor elements behave like ordinary scalar array elements.

### Elementwise arithmetic and scalar broadcast

| Operands | Operators | Result |
|---|---|---|
| Two tensors with identical shapes | `+`, `-`, `*`, `/` | The same shape, applying the scalar operator to corresponding elements |
| A tensor and a scalar, in either order | `+`, `-`, `*`, `/` | The tensor shape, broadcasting the scalar to every element |
| Integer operands in either of the forms above | `%` | Elementwise signed integer remainder |
| One tensor | Unary `+`, `-` | The same shape and element type |

Each array operand of tensor arithmetic must carry the `tensor` qualifier. Ordinary arrays do not acquire elementwise operators merely by appearing beside a tensor. Two tensor operands must match every extent: equal element counts alone are insufficient. Broadcasting applies to scalars only; dimensions of length one do not enable vector, row or general shape broadcasting. Tensor comparisons, `!`, `&&` and `||` are not defined.

An operation with a `float` operand has `float` elements; otherwise it has `int` elements. Mixed numeric operands undergo the scalar conversions before the operation. `%` requires both element types to be `int`. A scalar broadcast expression is evaluated once, including any function calls, rather than once per element.

```c
tensor int a[4] = {1, 2, 3, 4};
tensor int b[4] = {5, 6, 7, 8};
tensor int c[4];
c = a + b * 2 + 10; // {21, 24, 27, 30}
c = 3 - a;          // {2, 1, 0, -1}
c = a % 3;          // {1, 2, 0, 1}
c = -c;
```

### Assignment and evaluation

A mutable whole tensor or tensor view may receive a tensor expression of exactly the same shape, or a scalar that fills every element. Values are converted to the destination element type at assignment. An ordinary array does not support whole-array assignment. `const` tensors and their elements cannot be assignment destinations.

Tensor destination indices and scalar broadcast values are evaluated once. Matrix products and tensor function results are computed before assignment consumes them. In particular, `a = a @ a` reads the original matrix, and `a = a + a[0][0]` uses the original scalar element throughout the broadcast. Short-circuit conditions still suppress calls on the unevaluated side, including calls returning tensors.

All extents must be known for whole-tensor arithmetic and assignment. There is no run-time shape inference, dynamic allocation, resizing or automatic bounds checking.

### Matrix multiplication

`@` accepts two rank-two tensors. If the left shape is `(M,K)` and the right shape is `(K,N)`, the result shape is `(M,N)`; the two reduction extents must match. It has the same precedence as `*`, `/` and `%`, and associates to the left. `*` remains elementwise multiplication.

For each result element, the following specifies its value, with scalar conversions to the common element type:

```text
sum = 0                 // +0.0 for float
for k = 0, 1, ..., K-1:
    product = A[i,k] * B[k,j]
    sum = sum + product
C[i,j] = sum
```

Integer operations follow the int32 rules in Section 5. Floating-point multiplication and addition are separate binary32 operations in increasing `k` order. Reassociation, fused multiply-add and partial-sum reordering must not change the defined result. An empty reduction produces zero. A scalar, vector, higher-rank tensor or incompatible pair of matrices is not a valid `@` operand.

```c
tensor int A[2][3] = {{1, 2, 3}, {4, 5, 6}};
tensor int B[3][2] = {{7, 8}, {9, 10}, {11, 12}};
tensor int C[2][2];
C = A @ B; // {{58, 64}, {139, 154}}
```

### Parameters and return values

Tensor parameters use the ordinary array-parameter syntax, such as `tensor int a[]` or `tensor float a[][3]`. They receive the caller's storage address, and may modify it. Actual arrays must have the same element type and rank and match all extents after the first; either an ordinary array or a tensor can supply this storage. The omitted first extent is unknown inside the function, so these parameters can be indexed and passed onward, but cannot participate as whole operands in operations requiring that unknown extent.

`tensor int` or `tensor float` before a function name declares a tensor result. Every return must provide a tensor with a compile-time known shape; all returns must agree on that shape. The element type is converted to the declared result element type when necessary. The result is a value copied into caller-owned storage, so returning a local tensor does not leave a dangling reference. `tensor void`, scalar tensor variables, scalar returns from tensor functions and inconsistent return shapes are invalid. The compiler must have resolved the result shape before resolving a call; define tensor-returning helpers before their callers.

```c
tensor int add_tensors(tensor int a[], tensor int b[], int len) {
    tensor int result[10] = {};
    int i = 0;
    while (i < len) {
        result[i] = a[i] + b[i];
        i = i + 1;
    }
    return result; // Fixed result shape (10), independent of len.
}
```

Runtime array functions can consume fully compatible tensor storage or subarrays, for example `putarray(4,a)` and `putfarray(6,matrix[0])`. They retain the signatures in [SysY-Runtime-Library.md](SysY-Runtime-Library.md); there is no additional tensor runtime API.

### Test correspondence

The existing suite contains ten tensor functional programs and fifteen tensor performance programs. These are executable examples of the extension, rather than the complete set of legal programs.

| Rules | Functional examples | Performance examples |
|---|---|---|
| Declaration and indexing | [test01_tensor_decl.sy](../test/h_functional/test01_tensor_decl.sy) | Stencil inputs `tensor_g5_stencil_int-*` |
| Elementwise arithmetic and broadcast | [test02_tensor_add.sy](../test/h_functional/test02_tensor_add.sy), [test03_tensor_scalar.sy](../test/h_functional/test03_tensor_scalar.sy), [test04_tensor_mul.sy](../test/h_functional/test04_tensor_mul.sy) | `tensor_g1_elementwise_int-*`, `tensor_g2_elementwise_float-*` |
| Matrix multiplication | [test05_tensor_matmul.sy](../test/h_functional/test05_tensor_matmul.sy) | `tensor_g3_matmul_int-*`, `tensor_g4_matmul_float-*` |
| Remainder, division and negation | [test06_tensor_mod.sy](../test/h_functional/test06_tensor_mod.sy), [test07_tensor_div.sy](../test/h_functional/test07_tensor_div.sy), [test08_tensor_sub_neg.sy](../test/h_functional/test08_tensor_sub_neg.sy) | Integer elementwise inputs |
| Float elements | [test09_tensor_float.sy](../test/h_functional/test09_tensor_float.sy) | Float elementwise and matrix inputs |
| Parameters and value returns | [test10_tensor_func.sy](../test/h_functional/test10_tensor_func.sy) | — |

[tools/regression.py](../tools/regression.py) supplies additional examples and rejection checks for aliases, one-time scalar calls, short-circuit tensor calls, shapes, signed zero and strict reduction rounding.

## 5. Numeric Rules of the Repository

`int` arithmetic and constant folding use 32-bit values. Addition, subtraction, multiplication and unary negation wrap modulo 2³² and are interpreted as signed two's-complement results; optimizations must not assume that signed overflow is impossible. With a nonzero divisor, integer division truncates toward zero and the remainder satisfies the corresponding signed division equation in this representation. `INT_MIN / -1` produces `INT_MIN`, and `INT_MIN % -1` produces zero. Division by zero is outside the defined source behavior.

`float` values are IEEE 754 binary32. Each arithmetic operation rounds to binary32 with round-to-nearest, ties-to-even. Positive and negative zero compare equal and both are false in conditions. NaN is true in a scalar condition, unequal to every value including itself, and false in ordered comparisons. Optimization must preserve observable signed-zero and NaN behavior and the specified order of floating-point operations. Promotion for variadic runtime arguments occurs after the source binary32 value has been computed.

[^1]: Compared with the 2020 edition, SysY2022 adds the basic type `float` and supports multidimensional arrays whose element type is `float`. The tensor extension in Section 4 is specific to this repository's tests.
