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
    "saved_predicate": ('''int main(){int a=getint();int b=getint();int t=a<b;
      if(a>0){a=a+11;b=b-7;} else {a=a-13;b=b+17;}
      putf("%d %d %d\\n",a,b,t);if(t)putint(1);else putint(0);return 0;}''', "14 -2 1\n1\n0\n"),
    "repeat_integer": ('''int main(){int a[5]={1,2,3,4,5};int sum=getint();int r=2;int i=9;
      while(r<19){i=0;while(i<5){if(a[i]%2)sum=sum+a[i]*2;i=i+1;}r=r+1;}
      putf("%d %d %d\\n",sum,r,i);return 0;}''', "299 19 5\n0\n"),
    "repeat_memory_write": ('''int main(){int a[1]={1};int r=0;
      while(r<5){a[0]=a[0]+2;r=r+1;}putint(a[0]);return 0;}''', "11\n0\n"),
    "repeat_impure": ('''int calls;int bump(){calls=calls+1;return calls;}
      int main(){int sum=0;int r=0;while(r<5){sum=sum+bump();r=r+1;}
        putf("%d %d\\n",sum,calls);return 0;}''', "15 5\n0\n"),
    "repeat_carried": ('''int main(){int sum=1;int r=0;while(r<5){sum=sum+sum;r=r+1;}
      putf("%d %d\\n",sum,r);return 0;}''', "32 5\n0\n"),
    "uniform_rounding": ('''tensor float a[1][257];tensor float b[257][1];tensor float c[1][1];
      int main(){int i=0;while(i<1){int j=0;while(j<257){a[i][j]=0.1;j=j+1;}i=i+1;}
        i=0;while(i<257){int j=0;while(j<1){b[i][j]=0.3;j=j+1;}i=i+1;}c=a@b;
        float sum=0;int k=0;while(k<257){sum=sum+a[0][k]*b[k][0];k=k+1;}
        if(c[0][0]==sum)putint(1);else putint(0);return 0;}''', "1\n0\n"),
    "uniform_signed_zero": ('''tensor float a[1][3];tensor float b[3][1];tensor float c[1][1];
      int main(){int i=0;while(i<1){int j=0;while(j<3){a[i][j]=-0.0;j=j+1;}i=i+1;}
        i=0;while(i<3){int j=0;while(j<1){b[i][j]=1.0;j=j+1;}i=i+1;}c=a@b;
        putf("%f\\n",c[0][0]);return 0;}''', "0.000000\n0\n"),
    "canonical_nan": ('''float zero(){return 0.0;} int main(){float a=zero();float b=a/a;
      float c=0.0/0.0;putf("%f %f %f\\n",b,c,-c);return 0;}''', "nan nan nan\n0\n"),
    "uniform_unknown_call": ('''int i;tensor int a[2][3];tensor int b[3][2];tensor int c[2][2];
      void dirty(){i=1;a[0][0]=9;}
      int main(){i=0;dirty();while(i<2){int j=0;while(j<3){a[i][j]=i+1;j=j+1;}i=i+1;}
        i=0;while(i<3){int j=0;while(j<2){b[i][j]=j+1;j=j+1;}i=i+1;}
        c=a@b;putarray(4,c[0]);return 0;}''', "4: 9 18 6 12\n0\n"),
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
    import random
    rng = random.Random(137)
    numerators = [-2147483648, -2147483647, -65537, -1, 0, 1, 65537, 2147483647] + [rng.randrange(-2147483648, 2147483648) for _ in range(100)]
    divisors = [-2147483648, -2147483647, -1000000007, -65537, -97, -31, -10, -7, -3, -2, -1, 1, 2, 3, 7, 10, 31, 97, 65537, 1000000007, 2147483647]
    expressions = [f"x/({d}),x%({d})" for d in divisors]
    fmt = " ".join(["%d"] * (2 * len(divisors))) + "\\n"
    source = f'int main(){{int i=0;while(i<{len(numerators)}){{int x=getint();putf("{fmt}",{",".join(expressions)});i=i+1;}}return 0;}}'
    output = []
    for n in numerators:
        row = []
        for d in divisors:
            q = abs(n) // abs(d) * (-1 if (n < 0) != (d < 0) else 1)
            remainder = n - q*d
            if q == 2147483648: q = -2147483648
            row.extend([str(q), str(remainder)])
        output.append(" ".join(row))
    cases["constant_division"] = (source, "\n".join(output) + "\n0\n")
    matrix = '''tensor float a[2][7];tensor float b[7][5];tensor float c[2][5];
      int main(){int i=0;while(i<2){int j=0;while(j<7){a[i][j]=(i+1)*0.25;j=j+1;}i=i+1;}
        i=0;while(i<7){int j=0;while(j<5){b[i][j]=(j+1)*0.5;j=j+1;}i=i+1;}
        MODIFY c=a@b;int bad=0;i=0;while(i<2){int j=0;while(j<5){float sum=0;
          int k=0;while(k<7){sum=sum+a[i][k]*b[k][j];k=k+1;}
          if(sum!=c[i][j])bad=bad+1;j=j+1;}i=i+1;}putint(bad);return 0;}'''
    cases["uniform_rectangular"] = (matrix.replace("MODIFY", ""), "0\n0\n")
    cases["uniform_invalidation"] = (matrix.replace("MODIFY", "a[0][1]=9.0;"), "0\n0\n")
    for name, (source, expected) in cases.items():
        (directory / f"{name}.sy").write_text(source)
        (directory / f"{name}.out").write_text(expected)
    (directory / "float_conditions.in").write_text("0\n")
    (directory / "integer_edges.in").write_text("2147483647 5\n")
    (directory / "tensor_call_control_flow.in").write_text("0\n")
    (directory / "saved_predicate.in").write_text("3 5\n")
    (directory / "constant_division.in").write_text("\n".join(map(str,numerators)) + "\n")
    (directory / "repeat_integer.in").write_text("-7\n")
    for opt in (0, 2, 3):
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
    print(f"{len(cases) * 3} execution and {len(INVALID)} diagnostic regressions passed")


if __name__ == "__main__":
    main()
