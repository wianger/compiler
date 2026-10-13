#!/usr/bin/env python3
"""ABI, initialization, diagnostics and register-pressure regression cases."""
import argparse
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]

CASES = {
    "varargs": ('''int main() {
      putf("%d %f %c %d %f %d %f %d %f %d %f %d %f %d %f %d %f %d %f\\n",
        -7, 1.25, 65, 2, 2.5, 3, 3.75, 4, 4.0, 5, 5.25, 6, 6.5, 7, 7.75, 8, 8.0, 9, 9.25);
      return 0;
    }''', "-7 1.250000 A 2 2.500000 3 3.750000 4 4.000000 5 5.250000 6 6.500000 7 7.750000 8 8.000000 9 9.250000\n0\n"),
    "large_frame": ('''int main() {
      int a[4096] = {}; a[4095] = 17; a[2048] = 9;
      putint(a[4095] + a[2048] + a[0]); return 0;
    }''', "26\n0\n"),
    "initializer_order": ('''int n; int next() { n=n+1; return n; }
    int main() {
      int a[3][2] = {next(),next(),{next()},next()};
      putarray(6,a[0]); return 0;
    }''', "6: 1 2 3 0 4 0\n0\n"),
    "float_conditions": ('''int touch() { putint(99); return 1; }
    int main() {
      float zero = getfloat(); float nan = zero/zero;
      if (zero && touch()) putint(88);
      if (1 || touch()) putint(1);
      if (nan) putint(2);
      if (!nan) putint(88);
      if (nan != nan) putint(3);
      if (nan == nan) putint(88);
      putf(" %f\\n", -zero); return 0;
    }''', "123 -0.000000\n0\n"),
    "const_shapes": ('''const int a[2][3] = {{1}, {2,3}};
    const int n = a[1][1]; const float x = 0x1.8p+1;
    int main() { int b[n][2] = {{7},{8},9}; putarray(6,b[0]); putfloat(x); return 0; }''', "6: 7 0 8 0 9 0\n0x1.8p+1\n0\n"),
    "scopes_and_recursion": ('''int f(int f) { if(f == 0) return 1; return f*f(f-1); }
    int main() { int f=9; { int f=6; putint(f(f)); } putch(32); putint(f); return 0; }''', "720 9\n0\n"),
    "conversions": ('''float f(int n) { return n; } int g(float x) { return x; }
    int main() { int x = -3.75; float y = 7; putint(x); putch(32); putint(g(f(11)+0.75));
      putch(32); putfloat(y); return 0; }''', "-3 11 0x1.cp+2\n0\n"),
    "short_circuit_values": ('''int count; int tick() { count=count+1; return count; }
    int main() { int a = 0 && tick(); int b = 1 || tick(); int c = tick() && tick();
      putf("%d %d %d %d\\n",a,b,c,count); return 0; }''', "0 1 1 2\n0\n"),
    "integer_edges": ('''int main() { int x=getint(); int y=getint();
      putf("%d %d %d %d %d\\n",x+y,x-y,x*y,x/y,x%y); return 0; }''', "-2147483644 2147483642 2147483643 429496729 2\n0\n"),
    "tensor_alias_and_broadcast": ('''int calls; int tick(){calls=calls+1;return 2;}
      int main(){ tensor int a[2][2]={{1,2},{3,4}};
        a=a@a; putarray(4,a[0]); a=a+tick(); putarray(4,a[0]);
        a=a+a[0][0]; putarray(4,a[0]); putint(calls); return 0; }''',
      "4: 7 10 15 22\n4: 9 12 17 24\n4: 18 21 26 33\n1\n0\n"),
    "tensor_call_control_flow": ('''int calls;
      tensor int make(){tensor int a[2]={1,2};calls=calls+1;return a;}
      int first(tensor int a[]){return a[0];}
      int main(){int x=getint(); if(x && first(make()))putint(99);
        if(x || first(make()))putint(calls);
        while(calls<3 && first(make())){continue;}
        putch(32); putint(calls); make(); putch(32); putint(calls); return 0;}''', "1 3 4\n0\n"),
}

INVALID = {
    "undefined": ("int main(){return x;}", "undefined variable"),
    "const_write": ("int main(){const int x=1; x=2; return x;}", "mutable lvalue"),
    "array_shape": ("int f(int x[][3]){return 0;} int main(){int x[2][4]; return f(x);}", "incompatible shape"),
    "dimension": ("int main(){int n=2; int a[n]; return 0;}", "constant int"),
    "excess_initializer": ("int main(){int a[2]={1,2,3};return 0;}", "too many"),
    "break": ("int main(){break;return 0;}", "outside a loop"),
    "return_type": ("void f(){return 1;} int main(){return 0;}", "void function"),
    "param_redefinition": ("int f(int x){int x=1;return x;} int main(){return 0;}", "redefinition"),
    "forward_call": ("int main(){return f();} int f(){return 0;}", "undefined function"),
    "main": ("void main(){}", "main must have signature"),
    "missing_main": ("int f(){return 0;}", "program must define"),
    "global_initializer": ("int x; int y=x; int main(){return 0;}", "constant expression"),
    "comment": ("int main(){ /*", "unterminated comment"),
    "octal": ("int main(){return 09;}", "invalid or overflowing"),
    "exponent": ("int main(){return 1e+;}", "exponent requires digits"),
    "array_element_type": ("int main(){int a[2]={1.5,2};return 0;}", "float elements"),
    "tensor_shape": ("int main(){tensor int a[2];tensor int b[3];a=b;return 0;}", "matching tensor shape"),
    "matrix_shape": ("int main(){tensor int a[2][3];tensor int b[2][3];a=a@b;return 0;}", "compatible rank-2"),
    "tensor_return_shape": ("tensor int f(int x){tensor int a[2];tensor int b[3];if(x)return a;return b;} int main(){return 0;}", "consistent shape"),
    "tensor_void": ("tensor void f(){tensor int a[2];return a;} int main(){return 0;}", "int or float elements"),
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", type=Path, default=ROOT / "build/compiler")
    opts = parser.parse_args()
    directory = ROOT / "build/regressions"
    directory.mkdir(parents=True, exist_ok=True)
    cases = dict(CASES)
    # All parameters survive another call: exercise both argument banks,
    # incoming stack slots, spilling, and preservation across calls.
    params = ",".join(f"int i{n}" for n in range(12)) + "," + ",".join(f"float f{n}" for n in range(12))
    total = "+".join([f"i{n}" for n in range(12)] + [f"f{n}" for n in range(12)])
    args = ",".join([str(n) for n in range(12)] + [f"{n}.5" for n in range(12)])
    cases["mixed_arguments"] = (f"float sum({params}){{putch(65);return {total};}} int main(){{putfloat(sum({args}));return 0;}}", "A0x1.14p+7\n0\n")
    for name, (source, expected) in cases.items():
        (directory / f"{name}.sy").write_text(source)
        (directory / f"{name}.out").write_text(expected)
    (directory / "float_conditions.in").write_text("0\n")
    (directory / "integer_edges.in").write_text("2147483647 5\n")
    (directory / "tensor_call_control_flow.in").write_text("0\n")
    for opt in (0, 2):
        subprocess.run(["python3", str(ROOT / "tools/test.py"), str(directory), "--compiler", str(opts.compiler),
                        "--opt", str(opt), "--output", str(ROOT / f"build/regression-o{opt}")], check=True)
    invalid_dir = ROOT / "build/invalid"
    invalid_dir.mkdir(parents=True, exist_ok=True)
    for name, (source, message) in INVALID.items():
        path = invalid_dir / f"{name}.sy"
        path.write_text(source)
        output = invalid_dir / "output.s"
        output.write_text("preserve existing output\n")
        result = subprocess.run([str(opts.compiler), str(path), "-o", str(output)], capture_output=True, text=True)
        if result.returncode != 1 or message not in result.stderr or output.read_text() != "preserve existing output\n":
            raise AssertionError(f"{name}: {result.stderr}")
        print(f"PASS diagnostic/{name}")
    print(f"{len(cases) * 2} execution and {len(INVALID)} diagnostic regressions passed")


if __name__ == "__main__":
    main()
