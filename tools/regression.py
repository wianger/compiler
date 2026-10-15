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
    "recursive_purity": ('''int fib(int n){if(n<2)return n+1;return fib(n-1)+fib(n-2);}
      int data[6]={1,2,3,4,5,6};
      int main(){int n=getint();int i=0;int s=0;while(i<n){s=s+fib(8)+data[i%6];i=i+1;}
        putint(s);return 0;}''', "1053\n0\n"),
    "memory_call_effects": ('''int g;int a[2]={3,4};int b[2]={5,6};
      int read(int p[],int n){if(n==0)return p[0]+g;return read(p,n-1);}
      void write(int p[],int n){if(n==0){p[0]=p[0]+7;return;}write(p,n-1);}
      int main(){g=1;int x=read(a,3);write(b,2);int y=read(a,3);write(a,2);
        int z=read(a,3);g=2;int w=read(a,3);putf("%d %d %d %d %d\\n",x,y,z,w,b[0]);return 0;}''', "4 4 11 12 12\n0\n"),
    "parameter_alias": ('''int calc(int a[],int b[]){int i=0;int s=0;
      while(i<8){a[0]=a[0]+i;s=s+b[0];i=i+1;}return s;}
      int main(){int a[2]={1,2};int b[2]={3,4};int x=calc(a,a);int y=calc(a,b);
        putf("%d %d %d\\n",x,y,a[0]);return 0;}''', "92 24 57\n0\n"),
    "tail_parallel_state": ('''int go(int n,int a,int b,int c){if(n==0)return a*100+b*10+c;
      return go(n-1,b,c,a+1);}
      int main(){putint(go(getint(),1,2,3));return 0;}''', "656\n0\n"),
    "loop_multiple_exits": ('''int f(int n){int i=0;int s=0;
      while(i<n){if(i==7)return s;if(i==5)break;s=s+i;i=i+1;}return s+i;}
      int main(){putf("%d %d %d\\n",f(0),f(4),f(9));return 0;}''', "0 10 15\n0\n"),
    "guarded_divergence": ('''int spin(int n){if(n==0)return spin(n);return 7;}
      int main(){int n=getint();int i=0;int s=0;while(i<n){s=s+spin(0);i=i+1;}
        putint(s);return 0;}''', "0\n0\n"),
    "argument_copy_cycles": ('''int sink(int a,int b,int c,int d,int e,int f,int g,int h,float x,float y,float z){
      putf("%d %d %d %d %d %d %d %d %.2f %.2f %.2f\\n",a,b,c,d,e,f,g,h,x,y,z);return a;}
      int relay(int a,int b,int c,int d,int e,int f,int g,int h,float x,float y,float z){
        return sink(h,g,f,e,d,c,b,a,z,y,x);}
      int main(){putint(relay(1,2,3,4,5,6,7,8,1.25,2.5,3.75));return 0;}''',
      "8 7 6 5 4 3 2 1 3.75 2.50 1.25\n8\n0\n"),
    "sparse_array_effects": ('''int count;int tick(){count=count+1;return count;}
      int main(){int a[2000000]={tick()};a[1999999]=tick();a[731]=tick();a[731]=tick();
        int i=0;while(i<7){a[0]=a[0]+a[1999999];if(i%2)a[1999999]=a[1999999]+1;i=i+1;}
        putf("%d %d %d %d\\n",a[0],a[1999999],a[1],count);return 0;}''', "24 5 0 4\n0\n"),
    "sroa_reinitialization": ('''int main(){int n=getint();int sum=0;int i=0;
      while(i<n){int a[3][2]={{i}, {i+1}};a[2][1]=sum;
        if(i%2==0){i=i+1;continue;}sum=sum+a[0][0]+a[1][0]+a[0][1]+a[2][0]+a[2][1];
        if(i>6)break;i=i+1;}putf("%d %d\\n",sum,i);return 0;}''', "89 7\n0\n"),
    "memory_read_barriers": ('''int a[2];int seen(int p[],int n){if(n)return seen(p,n-1);return p[0];}
      int main(){a[0]=7;int x=seen(a,3);a[1]=9;a[0]=11;int y=seen(a,2);
        a[0]=13;putf("%d %d %d %d\\n",x,y,a[0],a[1]);return 0;}''', "7 11 13 9\n0\n"),
    "memory_float_bits": ('''int main(){float a[4]={};float x=getfloat();a[0]=-x;a[1]=x/x;
      float y=a[0];a[2]=17.0;float z=a[0];a[3]=3.0;
      putf("%f %f %f %f\\n",y,z,a[1],a[2]+a[3]);return 0;}''', "-0.000000 -0.000000 nan 20.000000\n0\n"),
    "memory_negative_subarray": ('''int calc(int a[],int b[],int n){if(n)return calc(a,b,n-1);
      int x=a[-1];b[3]=17;int y=a[-1];a[-2]=9;int z=a[-1];return x*100+y*10+z;}
      int main(){int a[2][4]={{1,2,3,4},{5,6,7,8}};putint(calc(a[1],a[0],2));
        putch(32);putarray(8,a[0]);return 0;}''', "587 8: 1 2 9 17 5 6 7 8\n0\n"),
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
    parser.add_argument("--benchmark", action="store_true", help="write timed pressure, division, memory and loop fixtures")
    parser.add_argument("--compiler", type=Path, default=ROOT / "build/compiler")
    opts = parser.parse_args()
    directory = ROOT / "build/regressions"
    directory.mkdir(parents=True, exist_ok=True)
    cases = dict(CASES)
    stores = "".join(f"data[{i}]=x+{i};" for i in range(1,71))
    cases["memory_search_budget"] = (f'''int data[80];int main(){{int x=getint();data[0]=x;
      {stores}data[0]=x+3;putarray(80,data);return 0;}}''',
      "80: "+" ".join(map(str,[-14]+[-17+i for i in range(1,71)]+[0]*9))+"\n0\n")
    (directory / "memory_search_budget.in").write_text("-17\n")
    chain = "data["*8 + "0" + "]"*8
    chain_source = f'''int data[32];int other[1];int main(){{int n=getint();int i=0;int sum=0;
      while(i<32){{data[i]=(i+1)%32;i=i+1;}}i=0;START
      while(i<n){{data[i%32]=(i*7+5)%32;int x={chain};other[0]=i;
        int y={chain};sum=sum+x+y;i=i+1;}}STOP
      putf("%d %d\\n",sum,other[0]);return 0;}}'''
    def chain_model(n):
        data,total = [(i+1)%32 for i in range(32)],0
        def read():
            x = 0
            for _ in range(8): x = data[x]
            return x
        for i in range(min(n,32)):
            data[i%32] = (i*7+5)%32
            total += 2*read()
        if n > 32: total += (n-32)*2*read()
        return (total+2**31)%2**32-2**31,max(0,n-1)
    total,last = chain_model(47)
    cases["memory_numbering_chain"] = (chain_source.replace("START","").replace("STOP",""),f"{total} {last}\n0\n")
    (directory / "memory_numbering_chain.in").write_text("47\n")
    cases["deep_expression"] = ("int main(){int x=getint();putint(x"+"+1"*20000+");return 0;}", "20007\n0\n")
    (directory / "deep_expression.in").write_text("7\n")
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
    lengths = [-1,0,1,2,3,4,5,7,8,9,15,16,17,31]
    cases["runtime_unroll_tails"] = ('''int main(){int r=0;while(r<14){int n=getint();
      int a[32]={};int i=0;int sum=0;float f=0.0;
      while(i<n){a[i]=i*3+1;sum=sum+a[i];f=f+0.1;i=i+1;}
      float ref=0.0;int j=0;while(j<n){ref=ref+0.1;j=j+1;}
      putf("%d %d %d %d\\n",sum,i,a[0],f==ref);r=r+1;}return 0;}''',
      "\n".join(f"{sum(i*3+1 for i in range(max(n,0)))} {max(n,0)} {int(n>0)} 1" for n in lengths)+"\n0\n")
    cases["signed_mask_tests"] = ('''int main(){int n=getint();int x=getint();int i=0;int sum=0;
      while(i<n){sum=sum+i/8+i%8+i/(-4)+i%(-4);if((x-i)%8==0)sum=sum+100;
        if((x+i)%(-16)!=0)sum=sum+1;i=i+1;}putint(sum);return 0;}''',
      f"{sum(i//8+i%8-(i//4)+i%4+100*int((-99-i)%8==0)+int((-99+i)%16!=0) for i in range(137))}\n0\n")
    # Seeded combinations of parameter aliasing, recursive read/write effects,
    # zero trips and unroll tails. Compute expected values independently.
    import struct
    alias_rng = random.Random(8143)
    for k in range(40):
        n = alias_rng.choice([0,1,2,3,4,5,7,8,9,16,23,31])
        sign, offset, same = alias_rng.choice([-1,1]), alias_rng.randrange(-19,20), k % 2
        source = f'''int g;int a[32];int b[32];
          int read(int p[],int n){{if(n==0)return p[0]+g;return read(p,n-1);}}
          void write(int p[],int n){{if(n==0){{p[0]=p[0]+7;return;}}write(p,n-1);}}
          int calc(int x[],int y[],int n){{int i=0;int s=0;float f=0.0;
            while(i<n){{x[i]=x[i]+i+({offset});s=s+y[i]+read(y,2);f=f+0.1;i=i+1;}}
            write(x,2);s=s+read(y,2);putfloat(f);putch(32);return s;}}
          int main(){{int n=getint();int i=0;while(i<32){{a[i]=i*({sign});b[i]=31-i;i=i+1;}}
            int s=calc(a,{'a' if same else 'b'},n);putf("%d %d %d\\n",s,a[0],b[0]);return 0;}}'''
        a0 = (offset if n else 0)+7
        total = (sign+1)*n*(n-1)//2 + 2*offset*n + a0 if same else 62*n-n*(n-1)//2+31
        f32 = lambda x: struct.unpack('f', struct.pack('f', x))[0]
        value = 0.0
        for _ in range(n): value = f32(value+f32(0.1))
        cases[f"alias_generated_{k:02d}"] = (source, f"{value.hex()} {total} {a0} 31\n0\n")
        (directory / f"alias_generated_{k:02d}.in").write_text(f"{n}\n")
    # Create pressure deliberately through overlapping loop-carried values,
    # then compare with a separate arithmetic model rather than another compiler.
    pressure_rng = random.Random(260626)
    wrap = lambda x: (x + 2**31) % 2**32 - 2**31
    for k in range(24):
        width = pressure_rng.randrange(25,49)
        iterations = pressure_rng.randrange(1,9)
        data = [pressure_rng.randrange(-1000000000,1000000000) for _ in range(width)]
        c1,c2 = pressure_rng.randrange(-31,32),pressure_rng.randrange(-31,32)
        declarations = "".join(f"int v{n}=getint();" for n in range(width))
        updates = "".join(f"v{n}=(v{n}*({c1})+v{n}*({c2}))+v{(n+1)%width};" for n in range(width))
        source = f"int main(){{{declarations}int n=getint();int i=0;while(i<n){{{updates}if(i%2==0){{int t=v0;v0=v1;v1=t;}}i=i+1;}}putint({'+'.join(f'v{n}' for n in range(width))});return 0;}}"
        model = data[:]
        for i in range(iterations):
            for n in range(width): model[n] = wrap(model[n]*c1+model[n]*c2+model[(n+1)%width])
            if i%2 == 0: model[0],model[1] = model[1],model[0]
        if k == 1 and opts.benchmark:
            # Model the two-iteration recurrence over Z/(2^32), then exponentiate
            # its matrix. This computes a million iterations independently.
            count = 1000000
            def step(v, swap):
                v = v[:]
                for n in range(width): v[n] = wrap(v[n]*c1+v[n]*c2+v[(n+1)%width])
                if swap: v[0],v[1] = v[1],v[0]
                return v
            columns = [step(step([int(i==j) for i in range(width)],True),False) for j in range(width)]
            matrix = [[columns[j][i] & 0xffffffff for j in range(width)] for i in range(width)]
            vector, power = data[:], count//2
            while power:
                if power & 1: vector = [sum(a*b for a,b in zip(row,vector)) & 0xffffffff for row in matrix]
                power >>= 1
                if power: matrix = [[sum(matrix[i][k]*matrix[k][j] for k in range(width)) & 0xffffffff for j in range(width)] for i in range(width)]
            fixture = ROOT / "build/pressure-fixture/performance"
            fixture.mkdir(parents=True,exist_ok=True)
            timed = source.replace("int i=0;while", "int i=0;starttime();while").replace("putint(","stoptime();putint(")
            (fixture / "pressure.sy").write_text(timed)
            (fixture / "pressure.in").write_text(" ".join(map(str,data+[count]))+"\n")
            (fixture / "pressure.out").write_text(f"{wrap(sum(vector))}\n0\n")
            print(f"Wrote {width} live integers, coefficients {c1} and {c2}, {count} iterations to {fixture}")
            # Quotient/remainder pairs expose shared reciprocal arithmetic
            # only after instruction selection. Check against Python integers.
            fixture = ROOT / "build/division-fixture/performance"
            fixture.mkdir(parents=True,exist_ok=True)
            source = '''int main(){int x=getint();int n=getint();int i=0;int sum=0;
              starttime();while(i<n){sum=sum+x/7+x%7+x/31+x%31+x/97+x%97;
                x=x*1664525+1013904223;i=i+1;}stoptime();
              putf("%d %d\\n",sum,x);return 0;}'''
            division_count = 10*count
            x, total = -2147483648, 0
            for _ in range(division_count):
                for d in (7,31,97):
                    q = abs(x)//d * (-1 if x < 0 else 1)
                    total = wrap(total+q+x-q*d)
                x = wrap(x*1664525+1013904223)
            (fixture / "division.sy").write_text(source)
            (fixture / "division.in").write_text(f"-2147483648 {division_count}\n")
            (fixture / "division.out").write_text(f"{total} {x}\n0\n")
            print(f"Wrote {division_count} quotient/remainder iterations to {fixture}")
            fixture = ROOT / "build/memory-fixture/performance"
            fixture.mkdir(parents=True,exist_ok=True)
            # Exponentiate an independent recurrence over Z/(2^32).
            def recurrence(matrix, vector, count):
                while count:
                    if count & 1: vector = [sum(a*b for a,b in zip(row,vector)) & 0xffffffff for row in matrix]
                    count >>= 1
                    if count: matrix = [[sum(matrix[i][k]*matrix[k][j] for k in range(3)) & 0xffffffff for j in range(3)] for i in range(3)]
                return [wrap(x) for x in vector]
            a,b,count = 1664525,1013904223,10000000
            x,y,_ = recurrence([[a,0,b],[a,1,b],[0,0,1]],[3,7,1],count)
            source = '''int main(){int a[2000000]={3};a[1999999]=7;int n=getint();int i=0;
              starttime();while(i<n){a[0]=a[0]*1664525+1013904223;
                a[1999999]=a[1999999]+a[0];i=i+1;}stoptime();
              putf("%d %d\\n",a[0],a[1999999]);return 0;}'''
            (fixture / "sparse.sy").write_text(source)
            (fixture / "sparse.in").write_text(f"{count}\n")
            (fixture / "sparse.out").write_text(f"{x} {y}\n0\n")
            x,total,_ = recurrence([[a,0,b],[3,1,3],[0,0,1]],[-31,0,1],count)
            source = '''int data[2]={3,7};int main(){int x=getint();int n=getint();int i=0;int sum=0;
              starttime();while(i<n){sum=sum+data[0];data[1]=x;sum=sum+data[0]*data[1];
                x=x*1664525+1013904223;i=i+1;}stoptime();
              putf("%d %d\\n",sum,x);return 0;}'''
            (fixture / "fields.sy").write_text(source)
            (fixture / "fields.in").write_text(f"-31 {count}\n")
            (fixture / "fields.out").write_text(f"{total} {x}\n0\n")
            x,previous,_ = recurrence([[a,0,b],[1,0,0],[0,0,1]],[-31,0,1],count)
            source = '''int data[2];int main(){int x=getint();int n=getint();int i=0;
              starttime();while(i<n){data[0]=x+3;data[1]=x+7;
                data[0]=x*31+11;data[1]=x*37+13;data[0]=x*5+17;data[1]=x*7+19;
                x=x*1664525+1013904223;i=i+1;}stoptime();
              putf("%d %d\\n",data[0]+data[1],x);return 0;}'''
            (fixture / "stores.sy").write_text(source)
            (fixture / "stores.in").write_text(f"-31 {count}\n")
            (fixture / "stores.out").write_text(f"{wrap(12*previous+36)} {x}\n0\n")
            print(f"Wrote sparse-cell, disjoint-field and overwritten-store fixtures to {fixture}")
            total,last = chain_model(count)
            (fixture / "chain.sy").write_text(chain_source.replace("START","starttime();").replace("STOP","stoptime();"))
            (fixture / "chain.in").write_text(f"{count}\n")
            (fixture / "chain.out").write_text(f"{total} {last}\n0\n")
        name = f"pressure_generated_{k:02d}"
        cases[name] = (source,f"{wrap(sum(model))}\n0\n")
        (directory / f"{name}.in").write_text(" ".join(map(str,data+[iterations]))+"\n")
    cases["tensor_wide_ordered"] = ('''tensor float a[2][9];tensor float b[9][11];tensor float c[2][11];
      int main(){int i=0;while(i<2){int k=0;while(k<9){a[i][k]=(i+k+1)*0.1;k=k+1;}i=i+1;}
        int k=0;while(k<9){int j=0;while(j<11){b[k][j]=(k-j)*0.3;j=j+1;}k=k+1;}c=a@b;
        int bad=0;i=0;while(i<2){int j=0;while(j<11){float sum=0;k=0;
          while(k<9){sum=sum+a[i][k]*b[k][j];k=k+1;}if(sum!=c[i][j])bad=bad+1;j=j+1;}i=i+1;}
        putint(bad);return 0;}''', "0\n0\n")
    # Affine chains with wrapping coefficients, shared intermediate values,
    # branch joins and loop backedges. Model each operation independently.
    affine_rng = random.Random(20261014)
    for k in range(16):
        x,y = [affine_rng.randrange(-2**31,2**31) for _ in range(2)]
        c = [affine_rng.choice([-2**31,2**31-1,-65537,-31,-1,0,2,17,65536]) for _ in range(8)]
        mode,n = k%2,k%8
        shared = wrap(wrap(x+c[0])*c[1])
        p = wrap(wrap(shared+c[2])*c[3]+y)
        q = wrap(wrap(shared-c[4])*c[5]-y)
        p = wrap((p+c[6])*c[7]) if mode else wrap((p-c[7])*c[6])
        for _ in range(n): p,q = wrap((p+17)*31),wrap(q*31+7)
        source = f'''int main(){{int x=getint();int y=getint();int mode=getint();int n=getint();
          int shared=(x+({c[0]}))*({c[1]});int p=(shared+({c[2]}))*({c[3]})+y;
          int q=(shared-({c[4]}))*({c[5]})-y;
          if(mode)p=(p+({c[6]}))*({c[7]});else p=(p-({c[7]}))*({c[6]});
          int i=0;while(i<n){{p=(p+17)*31;q=q*31+7;i=i+1;}}
          putf("%d %d %d %d\\n",p,q,shared,x);return 0;}}'''
        name = f"affine_generated_{k:02d}"
        cases[name] = (source,f"{p} {q} {shared} {x}\n0\n")
        (directory / f"{name}.in").write_text(f"{x} {y} {mode} {n}\n")
    # Fixed fields relative to dynamic row pointers, distinct and identical
    # actual parameters, and unknown-index writes. No noalias assumptions.
    memory_rng = random.Random(20261015)
    for k in range(24):
        n,row,same = [0,1,2,3,4,7,8,13][k%8],k%4,k%2
        values = [memory_rng.randrange(-100000,100000) for _ in range(16)]
        model = values[:]; total = 0
        for i in range(n):
            a,b = row*4, row*4 if same else ((row+1)%4)*4
            total = wrap(total+model[a])
            model[b+1] = wrap(model[b+1]+i)
            total = wrap(total+model[a]+model[b+1])
            model[b+i%4] = wrap(total+i)
            total = wrap(total+model[a])
        source = '''int calc(int a[],int b[],int n){int i=0;int sum=0;
          while(i<n){sum=sum+a[0];b[1]=b[1]+i;sum=sum+a[0]+b[1];
            b[i%4]=sum+i;sum=sum+a[0];i=i+1;}return sum;}
          int data[4][4];int main(){int i=0;while(i<4){int j=0;
            while(j<4){data[i][j]=getint();j=j+1;}i=i+1;}
          int n=getint();int row=getint();int same=getint();int sum=0;
          if(same)sum=calc(data[row],data[row],n);else sum=calc(data[row],data[(row+1)%4],n);
          putint(sum);putch(32);putarray(16,data[0]);return 0;}'''
        name = f"memory_generated_{k:02d}"
        cases[name] = (source,f"{total} 16: {' '.join(map(str,model))}\n0\n")
        (directory / f"{name}.in").write_text(" ".join(map(str,values+[n,row,same]))+"\n")
    # Independently execute the original loop order. These cover legal and
    # forbidden permutations, modular indices, parameter aliases and jam tails.
    poly_rng = random.Random(20261016)
    for k in range(18):
        height,width = poly_rng.randrange(4,12),poly_rng.randrange(5,14)
        seed = poly_rng.randrange(-1000000000,1000000000)
        mode = k%6
        data = [[wrap(seed+i*31+j) for j in range(width+2)] for i in range(height+2)]
        if mode == 0:
            statement = "a[i][j]=a[i][j]*(i+1)+j;if(j%3==0)a[i][j]=a[i][j]+11;"
            for j in range(1,width+1):
                for i in range(1,height+1):
                    data[i][j] = wrap(wrap(data[i][j]*(i+1))+j)
                    if j%3 == 0: data[i][j] = wrap(data[i][j]+11)
        elif mode == 1:
            statement = "a[i][j]=a[i-1][j]+a[i][j-1];"
            for j in range(1,width+1):
                for i in range(1,height+1): data[i][j] = wrap(data[i-1][j]+data[i][j-1])
        elif mode == 2:
            statement = "a[i][j]=a[i-1][j+1]+1;"
            for j in range(1,width+1):
                for i in range(1,height+1): data[i][j] = wrap(data[i-1][j+1]+1)
        elif mode == 3:
            statement = "a[i][j]=a[j%H][i%W]+5;"
            for j in range(1,width+1):
                for i in range(1,height+1): data[i][j] = wrap(data[j%(height+2)][i%(width+2)]+5)
        elif mode == 4:
            statement = "a[i][j]=a[i][j-1]+3;a[i][j-1]=a[i][j];"
            for j in range(1,width+1):
                for i in range(1,height+1):
                    data[i][j] = wrap(data[i][j-1]+3); data[i][j-1] = data[i][j]
        else:
            statement = "int t=i*2147483647+i+2;int x=(t%H+H)%H;a[x][j]=a[i][j]+1;"
            for j in range(1,width+1):
                for i in range(1,height+1):
                    t = wrap(wrap(i*2147483647)+i+2)
                    data[t%(height+2)][j] = wrap(data[i][j]+1)
        source = f'''const int H={height+2};const int W={width+2};int a[H][W];
          int main(){{int seed=getint();int i=0;while(i<H){{int j=0;while(j<W){{
            a[i][j]=seed+i*31+j;j=j+1;}}i=i+1;}}
          int j=1;while(j<{width+1}){{i=1;while(i<{height+1}){{{statement}i=i+1;}}j=j+1;}}
          putf("%d %d\\n",i,j);putarray(H*W,a[0]);return 0;}}'''
        name = f"poly_order_{k:02d}"
        cases[name] = (source,f"{height+1} {width+1}\n{(height+2)*(width+2)}: "+
                       " ".join(str(x) for row in data for x in row)+"\n0\n")
        (directory / f"{name}.in").write_text(f"{seed}\n")
    # The bases can alias even though the formal parameter Symbols differ.
    data = [[i*17+j-100 for j in range(17)] for i in range(17)]
    for j in range(17):
        for i in range(13): data[i][j] = data[j][i]+5
    cases["poly_parameter_alias"] = ('''void calc(int a[][17],int b[][17]){
      int j=0;while(j<17){int i=0;while(i<13){b[i][j]=a[j][i]+5;i=i+1;}j=j+1;}}
      int data[17][17];int main(){int i=0;while(i<17){int j=0;
        while(j<17){data[i][j]=i*17+j-100;j=j+1;}i=i+1;}
      calc(data,data);putarray(289,data[0]);return 0;}''',
      "289: "+" ".join(str(x) for row in data for x in row)+"\n0\n")
    cases["poly_call_clobber"] = ('''int n;int a[8][8];int tick(){n=7;return 1;}
      int main(){n=4;int x=tick();int j=0;
        while(j<n){int i=0;while(i<4){a[i][j]=i+j;i=i+1;}j=j+1;}
        putint(j);putch(32);n=4;if(tick()){j=0;
          while(j<n){int i=0;while(i<4){a[i][j]=i-j;i=i+1;}j=j+1;}}
        putf("%d %d\\n",j,a[3][6]);return 0;}''', "7 7 -3\n0\n")
    for k in range(8):
        rows,columns,reduction = 3+k%2,5+k,7+k
        seed = poly_rng.randrange(-100000000,100000000)
        a = [[wrap(seed+i*31-q*7) for q in range(reduction)] for i in range(rows)]
        b = [[wrap(q*11+j*17-9) for j in range(columns)] for q in range(reduction)]
        c = []
        for i in range(rows):
            for j in range(columns):
                total = 0
                for q in range(reduction):
                    if wrap(a[i][q]+b[q][j])%3: total = wrap(total+wrap(a[i][q]*b[q][j]))
                    else: total = wrap(total-a[i][q])
                c.append(total)
        source = f'''int a[{rows}][{reduction}];int b[{reduction}][{columns}];int c[{rows}][{columns}];
          int main(){{int seed=getint();int i=0;while(i<{rows}){{int k=0;
            while(k<{reduction}){{a[i][k]=seed+i*31-k*7;k=k+1;}}i=i+1;}}
          i=0;while(i<{reduction}){{int j=0;while(j<{columns}){{b[i][j]=i*11+j*17-9;j=j+1;}}i=i+1;}}
          i=0;while(i<{rows}){{int j=0;while(j<{columns}){{int k=0;int sum=0;
            while(k<{reduction}){{if((a[i][k]+b[k][j])%3)sum=sum+a[i][k]*b[k][j];
              else sum=sum-a[i][k];k=k+1;}}c[i][j]=sum;j=j+1;}}i=i+1;}}
          putarray({rows*columns},c[0]);return 0;}}'''
        name = f"poly_jam_{k:02d}"
        cases[name] = (source,f"{rows*columns}: {' '.join(map(str,c))}\n0\n")
        (directory / f"{name}.in").write_text(f"{seed}\n")
    f32 = lambda x: struct.unpack('f',struct.pack('f',x))[0]
    expected = []
    for i in range(3):
        for j in range(9):
            total = 0.0
            for q in range(17):
                x,y = f32((i-q)*f32(.1)),f32((q+j-7)*f32(.3))
                total = f32(total+f32(x*y))
            expected.append(total.hex())
    cases["poly_float_order"] = (f'''float a[3][17];float b[17][9];float c[3][9];
      float expected[3][9]={{{','.join(expected)}}};
      int main(){{int i=0;while(i<3){{int k=0;while(k<17){{a[i][k]=(i-k)*0.1;k=k+1;}}i=i+1;}}
        i=0;while(i<17){{int j=0;while(j<9){{b[i][j]=(i+j-7)*0.3;j=j+1;}}i=i+1;}}
        i=0;while(i<3){{int j=0;while(j<9){{float sum=0;int k=0;while(k<17){{
          sum=sum+a[i][k]*b[k][j];k=k+1;}}c[i][j]=sum;j=j+1;}}i=i+1;}}
        int bad=0;i=0;while(i<3){{int j=0;while(j<9){{if(c[i][j]!=expected[i][j])bad=bad+1;
          j=j+1;}}i=i+1;}}putint(bad);return 0;}}''',"0\n0\n")
    cases["poly_signed_zero"] = ('''float a[2][3];float b[3][5];float c[2][5];
      int main(){float z=getfloat();int i=0;while(i<2){int k=0;while(k<3){
        if(i==0)a[i][k]=-z;else a[i][k]=z;k=k+1;}i=i+1;}
      i=0;while(i<3){int j=0;while(j<5){b[i][j]=1.0;j=j+1;}i=i+1;}
      i=0;while(i<2){int j=0;while(j<5){float sum=-0.0;int k=0;
        while(k<3){sum=sum+a[i][k]*b[k][j];k=k+1;}c[i][j]=sum;j=j+1;}i=i+1;}
      putf("a=%f b=%f\\n",c[0][0],c[1][0]);return 0;}''',"a=-0.000000 b=0.000000\n0\n")
    (directory / "poly_signed_zero.in").write_text("0\n")
    data = [[i*17+j for j in range(17)] for i in range(17)]
    for j in range(7):
        for i in range(5): data[2*i][2*j] = data[2*i+1][2*j+1]+1
    cases["poly_congruence"] = ('''int a[17][17];int main(){int i=0;
      while(i<17){int j=0;while(j<17){a[i][j]=i*17+j;j=j+1;}i=i+1;}
      int j=0;while(j<7){i=0;while(i<5){a[2*i][2*j]=a[2*i+1][2*j+1]+1;i=i+1;}j=j+1;}
      putarray(289,a[0]);return 0;}''',"289: "+" ".join(str(x) for row in data for x in row)+"\n0\n")
    # Masking must normalize arbitrary integer conditions and retain guards
    # for out-of-domain accesses and variable divisors.
    for mode in ("previous", "division", "truth"):
        a = [[i*10+k+1 for k in range(7)] for i in range(3)]
        b = [[k*9+j-10 for j in range(9)] for k in range(7)]
        output = []
        for i in range(3):
            for j in range(9):
                total = 0
                for k in range(7):
                    if mode == "previous" and k > 0: total += a[i][k-1]*b[k][j]
                    if mode == "division" and k != 2:
                        total += abs(a[i][k])//abs(k-2)*(-1 if k < 2 else 1)+b[k][j]
                    if mode == "truth" and (i-k)*47: total += a[i][k]*b[k][j]
                output.append(wrap(total))
        update = {"previous":"if(k>0)sum=sum+a[i][k-1]*b[k][j];",
                  "division":"if(k!=2)sum=sum+a[i][k]/(k-2)+b[k][j];",
                  "truth":"if((i-k)*47)sum=sum+a[i][k]*b[k][j];"}[mode]
        cases[f"poly_predicate_{mode}"] = (f'''int a[3][7];int b[7][9];int c[3][9];
          int main(){{int i=0;while(i<3){{int k=0;while(k<7){{a[i][k]=i*10+k+1;k=k+1;}}i=i+1;}}
            i=0;while(i<7){{int j=0;while(j<9){{b[i][j]=i*9+j-10;j=j+1;}}i=i+1;}}
            i=0;while(i<3){{int j=0;while(j<9){{int sum=0;int k=0;
              while(k<7){{{update}k=k+1;}}c[i][j]=sum;j=j+1;}}i=i+1;}}
            putarray(27,c[0]);return 0;}}''',"27: "+" ".join(map(str,output))+"\n0\n")
    cases["poly_private_initializer"] = ('''int a[7];int b[7][9];int c[9];
      int main(){int k=0;while(k<7){a[k]=k+1;int j=0;
        while(j<9){b[k][j]=k+j;j=j+1;}k=k+1;}
      int j=0;while(j<9){int first=1;int q=first;int sum=0;
        while(q<7){sum=sum+a[q]*b[q][j];q=q+1;}c[j]=sum;j=j+1;}
      putarray(9,c);return 0;}''',"9: "+" ".join(str(sum((k+1)*(k+j) for k in range(1,7))) for j in range(9))+"\n0\n")
    # Unequal, non-unit affine maps exercise full source/destination relations,
    # integer projection and congruence, including non-injective writes.
    relation_rng = random.Random(20261017)
    for n in range(16):
        height,width = relation_rng.randrange(4,10),relation_rng.randrange(5,11)
        wi,wj,ri,rj = [relation_rng.randrange(1,6) for _ in range(4)]
        wo,ro = relation_rng.randrange(10,71),relation_rng.randrange(10,71)
        seed = relation_rng.randrange(-1000000,1000000)
        data = [wrap(seed+k*31) for k in range(192)]
        for j in range(width):
            for i in range(height):
                data[wi*i+wj*j+wo] = wrap(data[ri*i+rj*j+ro]*3+seed+i*7+j)
        name = f"poly_relation_{n:02d}"
        cases[name] = (f'''int a[192];int main(){{int seed=getint();int k=0;
          while(k<192){{a[k]=seed+k*31;k=k+1;}}int j=0;
          while(j<{width}){{int i=0;while(i<{height}){{
            a[{wi}*i+{wj}*j+{wo}]=a[{ri}*i+{rj}*j+{ro}]*3+seed+i*7+j;i=i+1;}}j=j+1;}}
          putarray(192,a);return 0;}}''',"192: "+" ".join(map(str,data))+"\n0\n")
        (directory / f"{name}.in").write_text(f"{seed}\n")
    # Loop memory promotion must commit the reaching state on every exit and
    # leave observers and unknown-index aliases in the original memory order.
    state_rng = random.Random(20261018)
    for n in range(24):
        mode,trip = n%6,[-2,0,4,9][n//6]
        seed = state_rng.randrange(-1000000000,1000000000)
        data = [wrap(seed+k*7) for k in range(8)]
        i,total,result = 0,0,None
        while i < trip:
            data[2] = wrap(data[2]*31+i)
            if mode == 1 and i%3 == 0: i += 1; continue
            if mode == 2 and i == 5: break
            if mode == 3 and i == 5: result = -i; break
            data[5] = wrap(data[5]+data[2])
            if mode == 4: total = wrap(total+wrap(data[2]+data[5]))
            if mode == 5: data[i%8] = i+17
            i += 1
        if result is None: result = wrap(i+total)
        control = {0:"",1:"if(i%3==0){i=i+1;continue;}",2:"if(i==5)break;",3:"if(i==5)return -i;",4:"",5:""}[mode]
        observe = "total=total+observe(2);" if mode == 4 else "a[i%8]=i+17;" if mode == 5 else ""
        name = f"loop_memory_{n:02d}"
        cases[name] = (f'''int a[8];int observe(int n){{if(n==0)return a[2]+a[5];return observe(n-1);}}
          int calc(int n){{int i=0;int total=0;while(i<n){{a[2]=a[2]*31+i;{control}
            a[5]=a[5]+a[2];{observe}i=i+1;}}return i+total;}}
          int main(){{int seed=getint();int n=getint();int k=0;while(k<8){{a[k]=seed+k*7;k=k+1;}}
            putint(calc(n));putch(32);putarray(8,a);return 0;}}''',f"{result} 8: "+" ".join(map(str,data))+"\n0\n")
        (directory / f"{name}.in").write_text(f"{seed} {trip}\n")
    cases["loop_memory_one_past"] = ('''int calc(int a[],int n){int i=0;
      while(i<n){a[0]=a[0]+i;i=i+1;}return i;}
      int main(){int a[1][4]={1,2,3,4};putint(calc(a[1],getint()));return 0;}''',"0\n0\n")
    (directory / "loop_memory_one_past.in").write_text("0\n")
    cases["loop_memory_const_zero_trip"] = ('''const int a[4]={1,2,3,4};
      int calc(int p[],int n){int i=0;while(i<n){p[0]=p[0]+i;i=i+1;}return i;}
      int main(){putint(calc(a,getint()));return 0;}''',"0\n0\n")
    (directory / "loop_memory_const_zero_trip.in").write_text("0\n")
    for n in (-2,0,1,2,6,9,31):
        last = max((i for i in range(max(n,0)) if i%3 == 0),default=None)
        expected = 17 if last is None else last*31
        name = f"loop_memory_write_only_{max(n,0)}" + ("_negative" if n < 0 else "")
        cases[name] = ('''int state=17;void calc(int n){int i=0;
          while(i<n){if(i%3==0)state=i*31;i=i+1;}}
          int main(){calc(getint());putint(state);return 0;}''',f"{expected}\n0\n")
        (directory / f"{name}.in").write_text(f"{n}\n")
    for trip in (0,1,7):
        total = f32(-.0)
        for _ in range(trip): total = f32(total+f32(.1))
        cases[f"loop_memory_float_{trip}"] = ('''float state;int main(){int n=getint();state=-0.0;
          int i=0;while(i<n){state=state+0.1;i=i+1;}
          putf("state=%f\\n",state);return 0;}''',f"state={total:.6f}\n0\n")
        (directory / f"loop_memory_float_{trip}.in").write_text(f"{trip}\n")
    def quotient(x,d): return wrap(abs(x)//abs(d)*(-1 if (x<0)!=(d<0) else 1))
    def remainder(x,d): return wrap(x-quotient(x,d)*d)
    for scale in (1,2):
        for alias in (False,True):
            for case,(initial,limit) in enumerate(((0,0),(0,1),(3,11),(11,3),(2147483647,2147483646),(-2147483647,-2147483648))):
                name=f"dynamic_induction_{scale}_{int(alias)}_{case}"
                data=[[j*3+17 for j in range(32)],[j*-5+31 for j in range(32)]]
                a,b=data[0],data[0 if alias else 1];total=0
                for i in range(initial,limit):
                    index=i*scale;a[index]=wrap(a[index]+b[index]-7);b[index]=wrap(a[index]+13)
                    total=wrap(total+b[index])
                final=max(initial,limit)
                cases[name]=(f'''int run(int a[],int b[],int first,int end){{int i=first;int sum=0;
                  while(i<end){{a[i*{scale}]=a[i*{scale}]+b[i*{scale}]-7;
                    b[i*{scale}]=a[i*{scale}]+13;sum=sum+b[i*{scale}];i=i+1;}}return i+sum;}}
                  int main(){{int data[2][32];int j=0;while(j<32){{data[0][j]=j*3+17;data[1][j]=j*(-5)+31;j=j+1;}}
                    int first=getint();int end=getint();putint(run(data[0],data[{0 if alias else 1}],first,end));
                    putch(10);putarray(32,data[0]);putarray(32,data[1]);return 0;}}''',
                  f"{wrap(final+total)}\n"+"".join("32: "+" ".join(map(str,row))+"\n" for row in data)+"0\n")
                (directory/f"{name}.in").write_text(f"{initial} {limit}\n")
    for alias in (False,True):
        for first in (-7,-1):
            name=f"negative_induction_{int(alias)}_{-first}"
            data=[[i+1 for i in range(16)],[i+17 for i in range(16)]]
            a,b=data[0],data[0 if alias else 1]
            for i in range(12):a[i]=wrap(a[i]+b[i]);b[i]=wrap(a[i]-7)
            cases[name]=(f'''int main(){{int data[2][16];int i=0;while(i<16){{data[0][i]=i+1;data[1][i]=i+17;i=i+1;}}
              i=getint();while(i<12){{if(i>=0){{data[0][i]=data[0][i]+data[{0 if alias else 1}][i];
                data[{0 if alias else 1}][i]=data[0][i]-7;}}i=i+1;}}putint(i);putch(10);
              putarray(16,data[0]);putarray(16,data[1]);return 0;}}''',
              "12\n"+"".join("16: "+" ".join(map(str,row))+"\n" for row in data)+"0\n")
            (directory/f"{name}.in").write_text(f"{first}\n")
    for first,limit in ((0,4),(2147483647,2147483647)):
        name=f"guarded_induction_load_{first}"
        cases[name]=('''int main(){int a[8]={1,2,3,4,5,6,7,8};int i=getint();int n=getint();
          int sum=0;while(i<n && a[i]){sum=sum+a[i];i=i+1;}putint(sum);putch(32);putint(i);return 0;}''',
          f"{10 if first==0 else 0} {limit}\n0\n")
        (directory/f"{name}.in").write_text(f"{first} {limit}\n")
    power_rng=random.Random(20261016)
    for group,shifts in enumerate((range(1,12),range(12,22),range(22,32))):
        divisors=[sign*(1<<k) for k in shifts for sign in (1,-1) if sign*(1<<k)<=2147483647]
        inputs=[-2147483648,-2147483647,-1,0,1,2147483646,2147483647]
        inputs += [wrap(sign*(1<<k)+delta) for k in shifts for sign in (1,-1) for delta in (-1,0,1)]
        inputs += [power_rng.randrange(-2147483648,2147483648) for _ in range(16)]
        for paired in (False,True):
            name=f"power_divrem_{group}_{int(paired)}"
            expressions=[e for d in divisors for e in ([f"x/({d})",f"x%({d})"] if paired else [f"x%({d})"])]
            fmt=" ".join(["%d"]*len(expressions))+"\\n"
            expected=[]
            for x in inputs:
                row=[str(v) for d in divisors for v in ([quotient(x,d),remainder(x,d)] if paired else [remainder(x,d)])]
                expected.append(" ".join(row))
            cases[name]=(f'''int main(){{int i=0;while(i<{len(inputs)}){{int x=getint();
              putf("{fmt}",{','.join(expressions)});i=i+1;}}return 0;}}''',"\n".join(expected)+"\n0\n")
            (directory/f"{name}.in").write_text("\n".join(map(str,inputs))+"\n")
    congruence_rng=random.Random(20261020)
    for shift in (1,2,3,7,11,16,30,31):
        for sign in (1,-1):
            d=sign*(1<<shift)
            if d>2147483647:continue
            inputs=[-2147483648,-2147483647,-1,0,1,2147483646,2147483647]
            inputs += [wrap(m*(1<<shift)+delta) for m in (-3,-2,-1,1,2,3) for delta in (-1,0,1)]
            inputs += [congruence_rng.randrange(-2147483648,2147483648) for _ in range(12)]
            lines=[]
            for x in inputs:
                row=[]
                if remainder(x,d)==0:
                    row=[quotient(x,d),remainder(x,d),quotient(wrap(x*3),d),
                         quotient(wrap(x+d),d),quotient(wrap(-x),d),quotient(wrap(x+1),d)]
                else:row=[quotient(x,d),remainder(x,d)]
                row += [quotient(x,d),remainder(x,d)]
                lines.append(" ".join(map(str,row)))
            name=f"congruence_divrem_{shift}_{int(sign<0)}"
            cases[name]=(f'''int main(){{int i=0;while(i<{len(inputs)}){{int x=getint();
              if(x%({d})==0)putf("%d %d %d %d %d %d ",x/({d}),x%({d}),
                (x*3)/({d}),(x+({d}))/({d}),(-x)/({d}),(x+1)/({d}));
              else putf("%d %d ",x/({d}),x%({d}));
              putf("%d %d\\n",x/({d}),x%({d}));i=i+1;}}return 0;}}''',"\n".join(lines)+"\n0\n")
            (directory/f"{name}.in").write_text("\n".join(map(str,inputs))+"\n")
    for tag in (0,1):
        inputs=[-2147483648,-2147483647,-129,-128,-17,-16,-8,-1,0,1,8,16,17,128,2147483647]
        lines=[]
        for x in inputs:
            row=[]
            if remainder(x,8)==0:
                y=wrap(x+1) if tag else x
                row += [quotient(y,8)]
                if remainder(x,16)==0:row += [quotient(x,16)]
                else:row += [quotient(x,-8)]
                row += [quotient(x,16)]
            if remainder(x,8)!=0:row += [quotient(x,8)]
            else:row += [quotient(x,-8)]
            row += [quotient(x,8),quotient(x,16)]
            lines.append(" ".join(map(str,row)))
        name=f"congruence_scope_{tag}"
        cases[name]=(f'''int main(){{int tag=getint();int i=0;while(i<{len(inputs)}){{int x=getint();
          if(x%8==0){{int y=x;if(tag)y=x+1;putf("%d ",y/8);
            if(x%16==0)putf("%d ",x/16);else putf("%d ",x/(-8));putf("%d ",x/16);}}
          if(x%8!=0)putf("%d ",x/8);else putf("%d ",x/(-8));
          putf("%d %d\\n",x/8,x/16);i=i+1;}}return 0;}}''',"\n".join(lines)+"\n0\n")
        (directory/f"{name}.in").write_text("\n".join(map(str,[tag,*inputs]))+"\n")
    inputs=[-2147483648,-2147483647,-1,0,1,2147483647]+[congruence_rng.randrange(-2147483648,2147483648) for _ in range(16)]
    lines=[]
    for x in inputs:
        a,b=wrap(x*16),wrap(wrap(x+3)*8)
        lines.append(" ".join(map(str,[quotient(wrap(a+b),8),quotient(wrap(a-b),8),
          quotient(wrap(a*b),128),remainder(wrap(a*b),128),quotient(wrap(-a),-16)])))
    cases['congruence_wrapping']=('''int main(){int n=getint();int i=0;while(i<n){int x=getint();
      int a=x*16;int b=(x+3)*8;putf("%d %d %d %d %d\\n",(a+b)/8,(a-b)/8,(a*b)/128,(a*b)%128,(-a)/(-16));i=i+1;}return 0;}''',
      "\n".join(lines)+"\n0\n")
    (directory/'congruence_wrapping.in').write_text("\n".join(map(str,[len(inputs),*inputs]))+"\n")
    lines=[]
    for original in inputs:
        x,total=original,0
        for _ in range(17):
            total=wrap(total+x)
            x=quotient(x,2) if remainder(x,2)==0 else wrap(x*3+1)
        lines.append(str(wrap(total+x)))
    cases['congruence_tail']=('''int walk(int x,int sum,int n){if(n==0)return sum+x;
      if(x%2==0)return walk(x/2,sum+x,n-1);else return walk(x*3+1,sum+x,n-1);}
      int main(){int n=getint();int i=0;while(i<n){putint(walk(getint(),0,17));putch(10);i=i+1;}return 0;}''',
      "\n".join(lines)+"\n0\n")
    (directory/'congruence_tail.in').write_text("\n".join(map(str,[len(inputs),*inputs]))+"\n")
    guard_rng = random.Random(20261019)
    for n in range(24):
        x = ([-2**31,-2**31+1,-65537,-1,0,1,65537,2**31-18,2**31-1]+[guard_rng.randrange(-2**31,2**31) for _ in range(15)])[n]
        original = x
        op = ["<","<=",">",">=","==","!="][n%6]; c = [-1,0,17,2**31-1][n//6]
        condition = {"<":x<c,"<=":x<=c,">":x>c,">=":x>=c,"==":x==c,"!=":x!=c}[op]
        predicate = f"x{op}({c})" if n%2 else f"({c})"+{"<":">","<=":">=",">":"<",">=":"<=","==":"==","!=":"!="}[op]+"x"
        if condition:
            total = wrap(quotient(x,8)+remainder(x,8))
            if 0 <= x < 1024:
                total = wrap(total+quotient(x+17,16)+remainder(x+17,16))
        else: total = wrap(quotient(x,-16)+remainder(x,-16))
        total = wrap(total+quotient(x,8)+remainder(x,8))
        if x >= 0:
            x = wrap(x*1664525+1013904223)
            total = wrap(total+quotient(x,8)+remainder(x,8))
        name = f"predicate_range_{n:02d}"
        cases[name] = (f'''int main(){{int x=getint();int sum=0;if({predicate}){{sum=x/8+x%8;
          if(x>=0&&x<1024){{sum=sum+(x+17)/16+(x+17)%16;if(x<0)sum=9999;}}}}
          else sum=x/(-16)+x%(-16);
          sum=sum+x/8+x%8;if(x>=0){{x=x*1664525+1013904223;sum=sum+x/8+x%8;}}
          putf("%d %d\\n",sum,x);return 0;}}''',f"{total} {x}\n0\n")
        (directory / f"{name}.in").write_text(f"{original}\n")
    for trips in (0,1,3,7):
        name=f"repeat_revisited_{trips}"
        cases[name]=('''int main(){int n=getint();int r=0;int i=0;int sum=0;
          while(r<n){while(i<5){sum=sum+7;i=i+1;}r=r+1;}
          putf("%d %d %d\\n",sum,i,r);return 0;}''',
          f"{35 if trips else 0} {5 if trips else 0} {trips}\n0\n")
        (directory/f"{name}.in").write_text(f"{trips}\n")
    for n,x in enumerate((-2147483648,-17,0,17,2147483647)):
        name=f"affine_functions_{n}"
        # A constant result may still have observable argument evaluation. A
        # caller that evaluates getint cannot itself receive a pure summary.
        cases[name]=('''int count;
          int combine(int a,int b){return a+b;}
          int cancel(int a,int b){return a-combine(a,b)+b;}
          int shifted(int a,int b){return combine(a*31+7,b*(-17))+19;}
          int tick(){count=count+1;return count;}
          int read(int x){return cancel(getint(),x);}
          int main(){int x=getint();int value=cancel(tick(),tick());
            int other=read(x);int tail=getint();putf("%d %d %d %d %d\\n",value,count,other,tail,shifted(x,x));return 0;}''',
          f"0 2 0 99 {wrap(x*14+26)}\n0\n")
        (directory/f"{name}.in").write_text(f"{x} 123 99\n")
    for case in range(30):
        delta,mod=[(128,65535),(1,17),(2147483647,1),(7,2147483647),(-7,31),(0,17)][case%6]
        trip=[-3,0,1,3,31][case//6]
        seed=[-2147483648,-17,0,31,2147483647-1][case//6]
        total=seed
        for _ in range(max(trip,0)):total=remainder(wrap(total+delta),mod)
        name=f"modular_recurrence_{case:02d}"
        cases[name]=(f'''int compute(int x,int n,int depth){{if(depth)return compute(x,n,depth-1);int i=0;
          while(i<n){{x=x+({delta});x=x%{mod};i=i+1;}}putint(i);putch(32);return x;}}
          int main(){{int x=getint();int n=getint();putint(compute(x,n,2));return 0;}}''',
          f"{max(trip,0)} {total}\n0\n")
        (directory/f"{name}.in").write_text(f"{seed} {trip}\n")
    cases['modular_resumed']=('''int main(){int x=getint();int i=0;int n=2;int r=0;
      while(r<3){while(i<n){x=x+7;x=x%31;i=i+1;}n=n+3;r=r+1;}
      putf("%d %d %d\\n",x,i,n);return 0;}''','5 8 11\n0\n')
    (directory/'modular_resumed.in').write_text('11\n')
    for case,seed in enumerate((65534,2147483647-128*31,2147483647-128*31+1,2147483647-127)):
        total=seed
        for _ in range(31):total=remainder(wrap(total+128),65535)
        name=f'modular_guard_boundary_{case}'
        cases[name]=('''int main(){int x=getint();int n=getint();int i=0;
          while(i<n){x=x+128;x=x%65535;i=i+1;}putf("%d %d\\n",x,i);return 0;}''',f'{total} 31\n0\n')
        (directory/f'{name}.in').write_text(f'{seed} 31\n')
    cases['runtime_memory_contract']=('''int g=17;int a[2][3]={{1,2,3},{4,5,6}};
      int main(){int before=a[1][0];int x=getint();putarray(3,a[0]);
        int n=getarray(a[1]);putf("%d %d %d %d\\n",g,x,before,n);putarray(6,a[0]);return 0;}''',
      '3: 1 2 3\n17 99 4 2\n6: 1 2 3 8 9 6\n0\n')
    (directory/'runtime_memory_contract.in').write_text('99\n2 8 9\n')
    for case,(divisor,trip,seed) in enumerate((d,n,x) for d in (2,16,1073741824)
                                           for n in (-1,0,1,7,8,31,32,34)
                                           for x in (-2147483648,-17,0,17,2147483647)):
        total=seed
        for _ in range(max(trip,0)):total=abs(total)//divisor*(-1 if total<0 else 1)
        name=f'quotient_recurrence_{case:03d}'
        cases[name]=(f'''int main(){{int x=getint();int n=getint();int i=0;
          while(i<n){{x=x/{divisor};i=i+1;}}putf("%d %d\\n",x,i);return 0;}}''',f'{total} {max(trip,0)}\n0\n')
        (directory/f'{name}.in').write_text(f'{seed} {trip}\n')
    cases['quotient_resumed']=('''int main(){int x=getint();int i=0;int n=2;int r=0;
      while(r<3){while(i<n){x=x/2;i=i+1;}n=n+3;r=r+1;}putf("%d %d\\n",x,i);return 0;}''','-8 8\n0\n')
    (directory/'quotient_resumed.in').write_text('-2048\n')
    for case,seed in enumerate((-2147483648,-17,0,17,2147483647)):
        def choice(x,n):
            if x<0:return wrap(x+5)
            for _ in range(max(n,0)):x=abs(x)//2*(-1 if x<0 else 1)
            return x
        x=seed;total=0
        for _ in range(4):total=wrap(total+wrap(3*choice(x,3)));x=choice(x,2)
        name=f'ssa_inline_returns_{case}'
        cases[name]=('''int choose(int x,int n){if(x<0)return x+5;int i=0;
          while(i<n){x=x/2;i=i+1;}return x;}
          int main(){int x=getint();int i=0;int total=0;
            while(i<4){total=total+choose(x,3)+choose(x,3)+choose(x,3);x=choose(x,2);i=i+1;}
            putf("%d %d\\n",total,x);return 0;}''',f'{total} {x}\n0\n')
        (directory/f'{name}.in').write_text(f'{seed}\n')
    for count in (16,31,32,33,34):
        name=f'zero_alignment_tail_{count}'
        cases[name]=(f'''int main(){{int prefix[1]={{7}};int a[{count}]={{}};int b[{count}]={{}};int suffix[1]={{9}};
          putarray(1,prefix);putarray({count},a);putarray({count},b);putarray(1,suffix);return 0;}}''',
          '1: 7\n'+(f'{count}: '+' '.join(['0']*count)+'\n')*2+'1: 9\n0\n')
    for flag in (0,1):
        name=f'init_sink_escape_{flag}'
        cases[name]=('''void observe(int a[],int depth){if(depth)observe(a,depth-1);else putarray(16,a);}
          int main(){int a[16]={};int flag=getint();if(flag)observe(a,2);return 0;}''',
          ('16: '+' '.join(['0']*16)+'\n' if flag else '')+'0\n')
        (directory/f'{name}.in').write_text(f'{flag}\n')
        name=f'init_sink_loop_{flag}'
        code='''int main(){int flag=getint();int r=0;while(r<3){int a[16]={};
          if(flag){a[0]=r;putarray(16,a);}r=r+1;}return 0;}'''
        output=''.join('16: '+' '.join([str(r)]+['0']*15)+'\n' for r in range(3)) if flag else ''
        cases[name]=(code,output+'0\n');(directory/f'{name}.in').write_text(f'{flag}\n')
    cases['init_sink_input']=('''int main(){int a[16]={};int n=getarray(a);
      if(n)putarray(16,a);return 0;}''','16: 3 7 '+' '.join(['0']*14)+'\n0\n')
    (directory/'init_sink_input.in').write_text('2 3 7\n')
    replay='''int a[8]={1,2,3,4,5,6,7,8};int b[8]={1,2,3,4,5,6,7,8};
      int main(){int n=getint();int r=getint();while(r<n){PRINT int i=0;
        while(i<8){a[i]=VALUE;i=i+1;}r=r+1;}putint(r);putch(10);putarray(8,a);return 0;}'''
    for mode in range(3):
        for n in (-3,0,1,3,5):
            values=list(range(1,9));prefix='';r=-2
            while r<n:
                if mode==2:prefix+=f'{r} '
                for i in range(8):values[i]=values[i]+r+i if mode==1 else (i+1)*3+i
                r+=1
            code=replay.replace('PRINT','putint(r);putch(32);' if mode==2 else '')
            code=code.replace('VALUE','a[i]+r+i' if mode==1 else 'b[i]*3+i')
            name=f'overwrite_replay_{mode}_{n+3}'
            cases[name]=(code,prefix+f'{r}\n8: '+' '.join(map(str,values))+'\n0\n')
            (directory/f'{name}.in').write_text(f'{n} -2\n')
    reset_kernel='''int hits;void kernel(int n,int a[][4],int b[][4]){EFFECT
      int i=0;while(i<n){int k=0;while(k<n){b[i][k]=b[i][k]+a[i][i]+1;k=k+1;}
        int j=i+1;while(j<n){k=0;while(k<n){b[j][k]=b[j][k]-a[j][i]*b[i][k];k=k+1;}j=j+1;}i=i+1;}}
      int a[4][4]={{2,1,0,3},{1,2,1,0},{3,1,2,1},{1,0,1,2}};
      int c[4][4]={{1,2,3,4},{5,6,7,8},{9,10,11,12},{13,14,15,16}};
      int b[4][4]={{-1,-2,-3,-4},{-5,-6,-7,-8},{-9,-10,-11,-12},{-13,-14,-15,-16}};
      int main(){int n=getint();int i=-9;int j=-8;int r=0;
        while(r<4){i=0;while(i<n){j=0;while(j<n){b[i][j]=SOURCE[i][j];j=j+1;}i=i+1;}
          kernel(n,a,DEST);r=r+1;}
        putf("%d %d %d %d\\n",i,j,r,hits);putarray(16,b[0]);putarray(16,c[0]);return 0;}'''
    for mode in range(4):
        for n in (-1,0,1,3,4):
            a=[[2,1,0,3],[1,2,1,0],[3,1,2,1],[1,0,1,2]]
            c=[[4*i+j+1 for j in range(4)] for i in range(4)]
            b=[[-v for v in row] for row in c]
            for _ in range(4):
                source=b if mode==3 else c
                for i in range(max(n,0)):
                    for j in range(n):b[i][j]=source[i][j]
                dst=c if mode==2 else b
                for i in range(max(n,0)):
                    for k in range(n):dst[i][k]=wrap(dst[i][k]+a[i][i]+1)
                    for j in range(i+1,n):
                        for k in range(n):dst[j][k]=wrap(dst[j][k]-wrap(a[j][i]*dst[i][k]))
            code=reset_kernel.replace('EFFECT','hits=hits+1;' if mode==1 else '')
            code=code.replace('SOURCE','b' if mode==3 else 'c').replace('DEST','c' if mode==2 else 'b')
            output=f'{max(n,0)} {n if n>0 else -8} 4 {4 if mode==1 else 0}\n'
            output+='16: '+' '.join(str(v) for row in b for v in row)+'\n'
            output+='16: '+' '.join(str(v) for row in c for v in row)+'\n0\n'
            name=f'reset_region_{mode}_{n+1}'
            cases[name]=(code,output)
            (directory/f'{name}.in').write_text(f'{n}\n')
    # Independent full-memory model for symbolic region kills. Rejected variants
    # expose incoming output, partial coverage, I/O, changing arguments and a
    # scalar reduction read by the callee. Observe untouched cells as well.
    region_kernel='''int data[8][8];int input[8];int hits;int total;
      int choose(int x,int y){if(x>y)return x;return y;}
      int work(int n,int m){PREFIX int i=0;
        while(i<=n){int j=RESET_START;while(j<=m){data[i][j]=0;j=j+1;}i=i+1;}
        EFFECT i=1;while(i<=n){int j=1;while(j<=m){
          data[i][j]=choose(data[i-1][j],data[i][j-1])+input[i-1]+input[j-1];j=j+1;}i=i+1;}
        OUTSIDE return data[0][0]+data[1][1]RETURN_EXTRA;}
      int argument(int n){hits=hits+1;return n;}
      int main(){int n=getint();int m=getint();int r=getint();total=getint();
        int i=0;while(i<8){input[i]=getint();int j=0;while(j<8){data[i][j]=getint();j=j+1;}i=i+1;}
        while(r<7){total=total+work(ARG,m);r=r+1;}
        putf("%d %d %d\\n",total,r,hits);putarray(64,data[0]);return 0;}'''
    for mode in range(9):
        for case,(n,m,r) in enumerate(((0,0,0),(1,2,3),(5,6,0),(6,5,6),(0,3,7),(-1,2,1),(2,3,-1))):
            data=[[17+8*i+j for j in range(8)] for i in range(8)]
            original=[row[:] for row in data]
            values=[2147483647,-2147483648,7,-19,31,0,-1,5]
            total,hits,start=-2147483617,0,r
            while r<7:
                size=n+(r%2) if mode==5 else n
                old=data[0][0] if mode==4 else 0
                if mode==6:hits+=1
                for i in range(max(size+1,0)):
                    for j in range(1 if mode==1 else 0,max(m+1,0)):data[i][j]=0
                if mode==2:hits+=1
                for i in range(1,size+1):
                    for j in range(1,m+1):data[i][j]=wrap(max(data[i-1][j],data[i][j-1])+values[i-1]+values[j-1])
                if mode==3:data[7][7]=wrap(data[7][7]+3)
                result=wrap(data[0][0]+data[1][1]+(old if mode==4 else total if mode==7 else 0))
                total=wrap(total+result);r+=1
            code=region_kernel.replace('PREFIX','int old=data[0][0];' if mode==4 else '')
            code=code.replace('RESET_START','1' if mode==1 else '0')
            code=code.replace('EFFECT','hits=hits+1;' if mode==2 else 'putch(65);' if mode==8 else '')
            code=code.replace('OUTSIDE','data[7][7]=data[7][7]+3;' if mode==3 else '')
            code=code.replace('RETURN_EXTRA','+old' if mode==4 else '+total' if mode==7 else '')
            code=code.replace('ARG','n+r%2' if mode==5 else 'argument(n)' if mode==6 else 'n')
            output=('A'*max(7-start,0) if mode==8 else '')+f'{total} {r} {hits}\n64: '
            output+=' '.join(str(v) for row in data for v in row)+'\n0\n'
            name=f'region_summary_{mode}_{case}'
            cases[name]=(code,output)
            (directory/f'{name}.in').write_text(f'{n} {m} {start} -2147483617\n'+
                '\n'.join(str(v)+' '+' '.join(map(str,row)) for v,row in zip(values,original))+'\n')
    for n in (0,1,2,4,6):
        for divisor in (-1,3,7):
            data=[[[0]*8 for _ in range(7)] for _ in range(6)];data[0][0][0]=9
            for i in range(n):
                for j in range(n):
                    for k in range(n):data[i][j][k]=1
            for i in range(1,n-1):
                for j in range(1,n-1):
                    for k in range(1,n-1):
                        value=wrap(data[i-1][j][k]+data[i+1][j][k]+data[i][j-1][k]+data[i][j+1][k]+
                                   data[i][j][k-1]+data[i][j][k+1]+data[i-1][j-1][k-1])
                        data[i][j][k]=abs(value)//abs(divisor)*(-1 if (value<0)!=(divisor<0) else 1)
            name=f'congruent_stencil_{n}_{divisor+1}'
            cases[name]=('''int x[6][7][8]={9};int unused[6][7][8];
              int main(){int n=getint();int f=getint();int i=0;while(i<n){int j=0;
                while(j<n){int k=0;while(k<n){x[i][j][k]=1;unused[i][j][k]=0;k=k+1;}j=j+1;}i=i+1;}
                i=1;while(i<n-1){int j=1;while(j<n-1){int k=1;while(k<n-1){
                  x[i][j][k]=(x[i-1][j][k]+x[i+1][j][k]+x[i][j-1][k]+x[i][j+1][k]+
                    x[i][j][k-1]+x[i][j][k+1]+x[i-1][j-1][k-1])/f;k=k+1;}j=j+1;}i=i+1;}
                putarray(336,x[0][0]);return 0;}''',
                '336: '+' '.join(str(v) for plane in data for row in plane for v in row)+'\n0\n')
            (directory/f'{name}.in').write_text(f'{n} {divisor}\n')
    cases['dead_global_evaluation']=('''int ignored[4];int calls;
      int tick(){calls=calls+1;return getint();}
      int main(){ignored[tick()%4]=tick();int count=getarray(ignored);
        putf("%d %d %d\\n",calls,count,getint());return 0;}''','2 2 99\n0\n')
    (directory/'dead_global_evaluation.in').write_text('1 17 2 31 32 99\n')
    for select in (0,1):
        name=f'global_parameter_observation_{select}'
        cases[name]=('''int a[2]={3,4};int b[2]={5,6};
          void observe(int p[],int depth){if(depth)observe(p,depth-1);else putarray(2,p);}
          int main(){int c=getint();a[0]=getint();b[0]=getint();
            if(c)observe(a,3);else observe(b,3);return 0;}''',f'2: {17 if select else 19} {4 if select else 6}\n0\n')
        (directory/f'{name}.in').write_text(f'{select} 17 19\n')
    for mode in range(4):
        for start in (-2,0,15,16,31,63,64,2147483647):
            data=[0]*80;counter=start;result=-2147483641;hits=0
            while counter<64:
                if mode==1 and counter==17:break
                if mode==2 and counter==13:counter+=1;continue
                if mode==3:hits+=1
                factor=3 if counter<16 else 7 if counter<32 else -5 if counter<48 else 11
                result=wrap(result*factor+counter);data[counter+2]=result;counter+=1
            name=f'loop_partition_{mode}_{start+2}'
            cases[name]=('''int data[80];int hits;
              int effect(){hits=hits+1;return 0;}
              int main(){int i=getint();int result=-2147483641;while(i<64){CONTROL EFFECT
                int factor;if(i<16)factor=3;else if(i<32)factor=7;else if(i<48)factor=-5;else factor=11;
                result=result*factor+i;data[i+2]=result;i=i+1;}
                putf("%d %d %d\\n",result,i,hits);putarray(80,data);return 0;}'''
                .replace('CONTROL','if(i==17)break;' if mode==1 else 'if(i==13){i=i+1;continue;}' if mode==2 else '')
                .replace('EFFECT','effect();' if mode==3 else ''),
                f'{result} {counter} {hits}\n80: '+' '.join(map(str,data))+'\n0\n')
            (directory/f'{name}.in').write_text(f'{start}\n')
    for rank in (1,3):
        target='out'+''.join(f'[i{d}]' for d in range(rank))
        reset=f'{target}=0;';kernel=f'{target}={target}+input[i0]'+''.join(f'+i{d}' for d in range(1,rank))+';'
        for d in reversed(range(rank)):
            reset=f'int i{d}=0;while(i{d}<n){{{reset}i{d}=i{d}+1;}}'
            kernel=f'int i{d}=0;while(i{d}<n){{{kernel}i{d}=i{d}+1;}}'
        kernel=kernel.replace('int i0=0;','i0=0;',1)
        for n in (-1,0,1,3,4):
            for start in (-1,0,6,7):
                data=[9]+[0]*(4**rank-1);total=2147483627;r=start
                while r<7:
                    import itertools
                    for index in itertools.product(range(max(n,0)),repeat=rank):
                        offset=0
                        for i in index:offset=offset*4+i
                        data[offset]=[-17,3,2147483647,-2147483648][index[0]]+sum(index[1:])
                        data[offset]=wrap(data[offset])
                    total=wrap(total+data[0]);r+=1
                name=f'region_rank_{rank}_{n+1}_{start+1}'
                access='out'+('[0]'*rank)
                source=f'''int out{('[4]'*rank)}={{9}};int input[4]={{-17,3,2147483647,-2147483648}};
                  int work(int n){{{reset}{kernel}return {access};}}
                  int main(){{int n=getint();int r=getint();int sum=2147483627;
                    while(r<7){{sum=sum+work(n);r=r+1;}}
                    putf("%d %d\\n",sum,r);putarray({4**rank},{'out'+('[0]'*(rank-1))});return 0;}}'''
                cases[name]=(source,f'{total} {r}\n{4**rank}: '+' '.join(map(str,data))+'\n0\n')
                (directory/f'{name}.in').write_text(f'{n} {start}\n')
    network='''int data[64];int hits;
      void exchange(int x,int y,int d){HIT if(d==1){if(data[x]>data[y]){
        int t=data[x];data[x]=data[y]EXTRA;data[y]=t;}}
        else {if(data[x]<data[y]){int t=data[x];data[x]=data[y]EXTRA;data[y]=t;}}}
      void merge_network(int low,int count,int d){if(count>1){int half=count/2;int q=low;
        while(q<low+half){exchange(q,q+half,d);q=q+1;}
        merge_network(low,half,d);merge_network(low+half,half,d);}}
      void sort_network(int low,int count,int d){if(count>1){int half=count/2;
        sort_network(low,half,1);sort_network(low+half,half,0);merge_network(low,count,d);}}'''
    def network_model(data,low,count,d,extra=0):
        hits=0
        def merge(low,count,d):
            nonlocal hits
            if count<=1:return
            half=count//2
            for q in range(low,low+half):
                hits+=1
                if (data[q]>data[q+half]) if d==1 else (data[q]<data[q+half]):
                    data[q],data[q+half]=wrap(data[q+half]+extra),data[q]
            merge(low,half,d);merge(low+half,half,d)
        def sort(low,count,d):
            if count<=1:return
            half=count//2;sort(low,half,1);sort(low+half,half,0);merge(low,count,d)
        sort(low,count,d);return hits
    sorting_rng=random.Random(20261016)
    for case in range(24):
        length=[-2147483648,-1,0,1,2,3,4,5,6,7,8,16][case%12]
        direction=[0,1,7][case%3];low=3 if length>1 else 2147483647
        data=[sorting_rng.choice(numerators) for _ in range(64)]
        initial=data[:];mode=case//8;hits=0
        for _ in range(4):hits+=network_model(data,low,length,direction,int(mode==2))
        prefix=network.replace('HIT','hits=hits+1;' if mode==1 else '').replace('EXTRA','+1' if mode==2 else '')
        name=f"sorting_contract_{case:02d}"
        cases[name]=(prefix+'''int main(){int count=getint();int low=getint();int d=getint();
          int i=0;while(i<64){data[i]=getint();i=i+1;}int r=0;
          while(r<4){sort_network(low,count,d);r=r+1;}
          putf("%d %d\\n",r,hits);putarray(64,data);return 0;}''',
          f"4 {hits if mode==1 else 0}\n64: "+' '.join(map(str,data))+'\n0\n')
        (directory/f"{name}.in").write_text(f"{length} {low} {direction}\n"+' '.join(map(str,initial))+'\n')
    prefix=network.replace('HIT','').replace('EXTRA','')
    output=[]
    for mask in range(256):
        data=[-7]*64
        for j in range(8):data[j+3]=(mask>>j)&1
        for _ in range(4):network_model(data,3,8,1)
        output.append('64: '+' '.join(map(str,data)))
    cases['sorting_zero_one']=(prefix+'''int main(){int mask=0;while(mask<256){
      int i=0;while(i<64){data[i]=-7;i=i+1;}int x=mask;i=0;
      while(i<8){data[i+3]=x%2;x=x/2;i=i+1;}int r=0;
      while(r<4){sort_network(3,8,1);r=r+1;}putarray(64,data);mask=mask+1;}return 0;}''',
      '\n'.join(output)+'\n0\n')
    # Memory joins: missing writes, nested joins, recursive Mod/Ref, aliases and
    # dynamic offsets must all preserve the source's reaching memory contents.
    for case in range(48):
        x = [-2147483648,-17,-1,0,1,2147483647][case%6]
        same,index,mode = case%2,[0,2,7][case%3],case//6
        a = [wrap(x+j) for j in range(8)]
        b = a if same else [wrap(31-j) for j in range(8)]
        yes = x > 0
        left = "a[2]=x+17;"; right = "a[2]=x*31;"
        if mode == 1: right = "a[3]=x*31;"
        if mode == 2: right = "if(x%3==0)a[2]=x+3;else a[2]=x-7;"
        if yes: a[2] = wrap(x+17)
        elif mode == 1: a[3] = wrap(x*31)
        elif mode == 2: a[2] = wrap(x+3 if remainder(x,3)==0 else x-7)
        else: a[2] = wrap(x*31)
        suffix = ""
        if mode in (3,4):
            suffix = "touch(b,2);";b[2] = wrap(b[2]+7)
        if mode == 5:
            suffix = "b[index]=x+99;";b[index] = wrap(x+99)
        if mode == 6:
            suffix = "int seen=observe(a,2);touch(b,2);a[4]=seen;"
            seen=a[2];b[2]=wrap(b[2]+7);a[4]=seen
        if mode == 7:
            suffix = "int j=0;while(j<index){if(j%2==0)a[2]=a[2]+j;else b[2]=x-j;j=j+1;}"
            for j in range(index):
                if j%2 == 0: a[2]=wrap(a[2]+j)
                else: b[2]=wrap(x-j)
        name = f"memory_phi_{case:02d}"
        source = f'''void touch(int p[],int n){{if(n)touch(p,n-1);else p[2]=p[2]+7;}}
          int observe(int p[],int n){{if(n)return observe(p,n-1);return p[2];}}
          int calc(int a[],int b[],int x,int index,int depth){{if(depth)return calc(a,b,x,index,depth-1);
            if(x>0){{{left}}}else{{{right}}}{suffix}return a[2]+a[3]+a[4]+b[2];}}
          int main(){{int x=getint();int index=getint();int a[8];int b[8];int i=0;
            while(i<8){{a[i]=x+i;b[i]=31-i;i=i+1;}}
            putint(calc(a,{ 'a' if same else 'b' },x,index,2));putch(32);putarray(8,a);return 0;}}'''
        cases[name] = (source,f"{wrap(a[2]+a[3]+a[4]+b[2])} 8: "+" ".join(map(str,a))+"\n0\n")
        (directory / f"{name}.in").write_text(f"{x} {index}\n")
    for width in (1,8,40):
        for positive in (False,True):
            x = 2147483647 if positive else -2147483648
            left = "".join(f"a[{j}]=x*{j*3+1};" for j in range(width))
            right = "".join(f"a[{j}]=x*{j*7+3};" for j in range(width))
            selected = [wrap(x*(j*3+1 if positive else j*7+3)) for j in range(width)]
            name = f"memory_phi_budget_{width}_{int(positive)}"
            cases[name] = (f'''int observe(int p[],int n){{if(n)return observe(p,n-1);return p[0];}}
              int main(){{int x=getint();int a[{width}]={{}};if(x>0){{{left}}}else{{{right}}}
              int sum={'+'.join(f'a[{j}]' for j in range(width))};
              putf("%d %d\\n",sum,observe(a,2));return 0;}}''',f"{wrap(sum(selected))} {selected[0]}\n0\n")
            (directory / f"{name}.in").write_text(f"{x}\n")
    for negative in (False,True):
        name = f"memory_phi_float_{int(negative)}"
        cases[name] = ('''float a[2];int main(){int x=getint();float z=getfloat();
          if(x<0)a[0]=-z;else a[0]=z;if(x%2)a[1]=z/z;else a[1]=3.5;
          putf("%f %f\\n",a[0],a[1]);return 0;}''',
          ("-0.000000 nan" if negative else "0.000000 3.500000")+"\n0\n")
        (directory / f"{name}.in").write_text(f"{'-1' if negative else '0'} 0\n")
    # Pure integer DAGs with several observable roots, effects, Phi edges and
    # regions exceeding the extraction budget. Model each source operation in
    # Python's unbounded integers, reducing after each operation.
    equality_rng = random.Random(20261016)
    for case in range(24):
        inputs = [equality_rng.choice(numerators) for _ in range(4)]
        values = inputs[:]
        declarations = [f"int v{i}=getint();" for i in range(4)]
        for group in range(8 if case < 20 else 90):
            a,b,c = equality_rng.sample(range(len(values)),3)
            first = len(values)
            expressions = [f"v{a}*v{b}",f"v{a}*v{c}",f"v{first}+v{first+1}",
                           f"v{a}*(v{b}+v{c})",f"(v{first+2}-v{first+3})+v{b}"]
            model = [wrap(values[a]*values[b]),wrap(values[a]*values[c]),
                     wrap(values[a]*values[b]+values[a]*values[c]),
                     wrap(values[a]*wrap(values[b]+values[c])),values[b]]
            for expression,value in zip(expressions,model):
                declarations.append(f"int v{len(values)}={expression};"); values.append(value)
        selected = sorted(equality_rng.sample(range(4,len(values)),12))
        fmt = " ".join(["%d"]*len(selected))+"\\n"
        arguments = ",".join(f"v{i}" for i in selected)
        # Keep several products live while their sums have factored alternatives.
        selected += [len(values)-5,len(values)-4,len(values)-3]
        tail = f"putint(v{selected[-3]});putch(32);putint(v{selected[-2]});putch(32);int z;"
        tail += f"if(v0<0)z=v{selected[-1]};else z=v{selected[-3]}+v{selected[-2]};putint(z);"
        name = f"equality_dag_{case:02d}"
        cases[name] = ("int main(){"+"".join(declarations)+f'putf("{fmt}",{arguments});'+tail+"return 0;}",
                       " ".join(str(values[i]) for i in selected[:-3])+"\n"+
                       " ".join(str(values[i]) for i in selected[-3:])+"\n0\n")
        (directory / f"{name}.in").write_text(" ".join(map(str,inputs))+"\n")
    # Model the original recursive int32 operations, including overflow at each
    # addition. This deliberately differs from widened multiplication outside
    # the proven guard or when the doubling modulus exceeds 2^30.
    def modular_model(a,b,m,mode="",trace=None):
        if b==0:return 1 if mode=="zero" else 0
        if b==1:return remainder(a,m+1 if mode=="unit" else m)
        half=abs(b)//(3 if mode=="half" else 2)*(1 if b>=0 else -1)
        cur=modular_model(wrap(a+1) if mode=="argument" else a,half,m,mode,trace)
        if mode=="effect":trace.append("X")
        cur=wrap(cur*2)
        if mode!="double":cur=remainder(cur,m)
        if remainder(b,2)==(0 if mode=="parity" else 1):cur=remainder(wrap(cur+a),m)
        return cur
    def modular_source(m,mode="",swapped=False):
        params="int b,int a" if swapped else "int a,int b"
        recur=f"{'b/3' if mode=='half' else 'b/2'},a" if swapped else f"{'a+1' if mode=='argument' else 'a'},b/{3 if mode=='half' else 2}"
        zero="1" if mode=="zero" else "0";unit="(M+1)" if mode=="unit" else "M"
        double="(c+c)" if mode=="double" else "(c+c)%M"
        prefix='putch(88);' if mode=="effect" else ''
        final="if(1==b%2)return (a+c)%M;return c;" if swapped else f"if(b%2=={0 if mode=='parity' else 1})return (c+a)%M;else return c;"
        return f'''const int M={m};int ring({params}){{if({'0==b' if swapped else 'b==0'})return {zero};
          if(b==1)return a%{unit};int c=ring({recur});{prefix}c={double};{final}}}'''
    mod_rng=random.Random(20261015)
    for m in (1,2,3,97,998244353,1073741824,1073741825,2147483647,-97):
        for swapped in (False,True):
            pairs=[(0,0),(0,2147483647),(1,1),(1,2),(m-1,2),(m-1,31),
                   (m-1,2147483647),(m,3),(wrap(m+1),2147483647),(-1,2),(-1,3),
                   (-2147483648,-2147483648),(-2147483648,2147483647),(2147483647,3),(2147483647,2147483647)]
            pairs += [(mod_rng.randrange(-2147483648,2147483648),mod_rng.randrange(-2147483648,2147483648)) for _ in range(16)]
            name=f"modular_product_{m}_{int(swapped)}"
            args="b,a" if swapped else "a,b"
            cases[name]=(modular_source(m,swapped=swapped)+f'''int main(){{int i=0;
              while(i<{len(pairs)}){{int a=getint();int b=getint();putint(ring({args}));putch(10);i=i+1;}}return 0;}}''',
              "\n".join(str(modular_model(a,b,m)) for a,b in pairs)+"\n0\n")
            (directory / f"{name}.in").write_text("".join(f"{a} {b}\n" for a,b in pairs))
    for mode in ("zero","unit","half","argument","double","parity","effect"):
        pairs=[(0,0),(13,1),(-17,3),(96,2147483647),(2147483647,31),(-2147483648,-1)]
        output=[]
        for a,b in pairs:
            trace=[];result=modular_model(a,b,97,mode,trace);output.append("".join(trace)+str(result))
        name="modular_product_reject_"+mode
        cases[name]=(modular_source(97,mode)+f'''int main(){{int i=0;while(i<{len(pairs)}){{
          int a=getint();int b=getint();putint(ring(a,b));putch(10);i=i+1;}}return 0;}}''',"\n".join(output)+"\n0\n")
        (directory / f"{name}.in").write_text("".join(f"{a} {b}\n" for a,b in pairs))
    for a,b in ((96,2147483647),(-17,31)):
        name=f"modular_product_arguments_{a}"
        cases[name]=(modular_source(97)+'''int main(){putint(ring(getint(),getint()));
          putch(32);putint(getint());return 0;}''',f"{modular_model(a,b,97)} 731\n0\n")
        (directory / f"{name}.in").write_text(f"{a} {b} 731\n")
    source=modular_source(97).replace('const int M=97;','int M=97;')
    cases["modular_product_mutable_modulus"]=(source+'''int main(){int a=getint();int b=getint();
      putint(ring(a,b));putch(32);M=1073741825;putint(ring(a,b));return 0;}''',
      f"{modular_model(1073741824,3,97)} {modular_model(1073741824,3,1073741825)}\n0\n")
    (directory / "modular_product_mutable_modulus.in").write_text("1073741824 3\n")
    for changed in (False,True):
        for unknown_read in (False,True):
            name=f"table_sequential_{int(changed)}_{int(unknown_read)}"
            data=[3]*16+[7]*16
            if changed:data[20]=-17
            cases[name]=(f'''int main(){{int x=getint();int a[32]={{}};int i=0;
              while(i<32){{if(i<16)a[i]=3;else a[i]=7;i=i+1;}}
              {'a[20]=x;' if changed else ''}int sum=x;i=0;
              while(i<16){{sum=sum+a[i];i=i+1;}}while(i<32){{sum=sum+a[i];i=i+1;}}
              putf("%d %d\\n",sum,i);{'int j=getint();putint(a[j]);' if unknown_read else ''}return 0;}}''',
              f"{sum(data)-17} 32\n"+(f"{data[20]}\n" if unknown_read else "")+"0\n")
            (directory / f"{name}.in").write_text("-17 20\n")
    for inclusive in (False,True):
        for begin,end in ((-2147483648,-2147483648),(-2147483648,-2147483647),
                          (-2147483648,-2147483645),(-2147483648,-2147483644),
                          (-2147483647,-2147483644),(-2147483645,-2147483646),
                          (-9,-1),(-3,5),(0,0),(3,17),(2147483638,2147483646),
                          (2147483646,2147483647),(2147483647,2147483646)):
            if inclusive and end==2147483647:continue  # Signed wrap makes this loop nonterminating.
            name=f"signed_unroll_{int(inclusive)}_{begin}_{end}"
            data=[0]*8;total=0;x=-731;i=begin;k=0
            while i<=end if inclusive else i<end:
                data[k%8]=x;total=wrap(total+x);x=wrap(x*1664525+1013904223);k+=1;i=wrap(i+1)
            cases[name]=(f'''int main(){{int i=getint();int end=getint();int x=getint();
              int a[8]={{}};int k=0;int sum=0;while(i{'<=' if inclusive else '<'}end){{
                a[k%8]=x;sum=sum+x;x=x*1664525+1013904223;k=k+1;i=i+1;}}
              putf("%d %d %d %d\\n",i,k,sum,x);putarray(8,a);return 0;}}''',
              f"{i} {k} {total} {x}\n8: "+" ".join(map(str,data))+"\n0\n")
            (directory / f"{name}.in").write_text(f"{begin} {end} -731\n")
    for begin,end in ((0,-1),(3,3),(-5,-5),(-7,2),(4,13)):
        name=f"unroll_mandatory_first_{begin}_{end}"
        i=begin;x=-731;total=0
        while True:
            total=wrap(total+x);x=wrap(x*1664525+1013904223);i=wrap(i+1)
            if i>=end:break
        cases[name]=('''int run(int i,int end,int x,int sum){sum=sum+x;
          x=x*1664525+1013904223;i=i+1;if(i<end)return run(i,end,x,sum);
          putf("%d %d\\n",i,sum);return x;}
          int main(){int i=getint();int n=getint();putint(run(i,n,-731,0));return 0;}''',
          f"{i} {total}\n{x}\n0\n")
        (directory / f"{name}.in").write_text(f"{begin} {end}\n")
    for live in (False,True):
        for escaped in (False,True):
            name=f"private_dead_writes_{int(live)}_{int(escaped)}"
            cases[name]=(f'''int observe(int p[],int n){{if(n)return observe(p,n-1);return p[2];}}
              int main(){{int a[8]={{}};int i=0;while(i<5){{a[i]=getint();i=i+1;}}
                putint(getint());putch(32);{'putint(a[2]);putch(32);' if live else ''}
                {'putint(observe(a,2));putch(32);' if escaped else ''}return 0;}}''',
              "99 "+("-17 " if live else "")+("-17 " if escaped else "")+"\n0\n")
            (directory / f"{name}.in").write_text("1 3 -17 7 11 99\n")
    # Observe complete arrays and loop exits, with independent scalar models.
    # Dynamic starts exercise the guarded specialization and its fallback.
    for start in (-2,0,2,5,2147483647):
        for changed in (0,1):
            name = f"small_loop_state_{start}_{changed}"
            data = [0]*10; exits = []
            for r in range(3):
                i = start if not changed else r-1
                while i < 5:
                    data[i+2] = wrap(data[i+2]+i*31+r)
                    i += 1
                exits.append(i)
            cases[name] = ('''int main(){int start=getint();int change=getint();int a[10]={};
              int r=0;while(r<3){int i=start;if(change)i=r-1;
                while(i<5){a[i+2]=a[i+2]+i*31+r;i=i+1;}putint(i);putch(32);r=r+1;}
              putarray(10,a);return 0;}''',
              " ".join(map(str,exits))+" 10: "+" ".join(map(str,data))+"\n0\n")
            (directory / f"{name}.in").write_text(f"{start} {changed}\n")
    for start in (0,2,5):
        total = f32(-0.0)
        for i in range(start,5): total = f32(total+f32(f32(i-3)*f32(.1)))
        name = f"small_loop_effects_{start}"
        cases[name] = ('''int main(){int i=getint();float x=-0.0;
          while(i<5){putch(65+i);x=x+(i-3)*0.1;i=i+1;}putch(32);putfloat(x);return 0;}''',
          "".join(chr(65+i) for i in range(start,5))+" "+total.hex()+"\n0\n")
        (directory / f"{name}.in").write_text(f"{start}\n")
    for same in (False,True):
        for start in (0,3,26):
            a,b = [17]*26,[31]*26
            if same: b = a
            x = -9;i = start
            while i < 26:
                x = 2147483647 if i == 0 else wrap(x*1664525+1013904223)
                a[i] = x;b[25-i] = wrap(x+7);i += 1
            name = f"constant_loop_alias_{int(same)}_{start}"
            cases[name] = (f'''int a[26];int b[26];
              int fill(int p[],int q[],int i){{int x=-9;
                while(i<26){{if(i==0)x=2147483647;else x=x*1664525+1013904223;
                  p[i]=x;q[25-i]=x+7;i=i+1;}}return x+i;}}
              int main(){{int i=getint();int j=0;while(j<26){{a[j]=17;b[j]=31;j=j+1;}}
                putint(fill(a,{'a' if same else 'b'},i));putch(32);putarray(26,a);putarray(26,b);return 0;}}''',
              f"{wrap(x+i)} 26: "+" ".join(map(str,a))+"\n26: "+
              " ".join(map(str,[31]*26 if same else b))+"\n0\n")
            (directory / f"{name}.in").write_text(f"{start}\n")
    cases["constant_loop_nested_wrap"] = ('''int a[5][5];int main(){int i=0;int x;
      while(i<5){int j=0;while(j<5){if(i==0&&j==0)x=-2147483648/(-1);
        else x=x*31+17;a[i][j]=x;j=j+1;}i=i+1;}
      putint(x);putch(32);putint(i);putch(32);putarray(25,a[0]);return 0;}''', "")
    data=[];x=-2147483648
    for k in range(25):
        if k:x=wrap(x*31+17)
        data.append(x)
    cases["constant_loop_nested_wrap"] = (cases["constant_loop_nested_wrap"][0],
      f"{x} 5 25: "+" ".join(map(str,data))+"\n0\n")
    # A nonconstant read/call, loop-local storage, and excessive evaluator work
    # require retaining execution. Short circuiting must still suppress I/O.
    for count in (5,128,129):
        name=f"constant_loop_fallback_{count}"
        values=[wrap(-731+i*17) for i in range(count)]
        cases[name]=(f'''int a[{count}];int main(){{int i=0;
          while(i<{count}){{int scratch[2]={{i,i+1}};a[i]=getint()+scratch[0]*17;
            if(0&&getint())a[i]=99;i=i+1;}}putarray({count},a);return 0;}}''',
          f"{count}: "+" ".join(map(str,values))+"\n0\n")
        (directory / f"{name}.in").write_text("-731\n"*count)
    for start in (-3,0,15,16,31,32,2147483647):
        for stop in (0,16,32):
            name=f"table_phase_{start}_{stop}";total=0;i=start
            table=[3]*16+[7]*16
            while i < stop:
                total=wrap(total+(table[i] if i>=0 else -11));i+=1
            cases[name]=('''int main(){int begin=getint();int n=getint();int a[32]={};int i=0;
              while(i<16){a[i]=3;i=i+1;}while(i<32){a[i]=7;i=i+1;}
              i=begin;int sum=0;while(i<n){if(i<0)sum=sum-11;else sum=sum+a[i];i=i+1;}
              putf("%d %d\\n",sum,i);return 0;}''',f"{total} {i}\n0\n")
            (directory / f"{name}.in").write_text(f"{start} {stop}\n")
    for mode in range(5):
        name=f"table_observation_{mode}";table=[0]*32;table[:8]=[3]*8;table[8:]=[7]*24
        action="";prefix="";extra=""
        if mode==1:action="a[20]=x;";table[20]=-17
        if mode==2:action="mutate(a,2);";table[20]=-17
        if mode==3:action="getarray(a);";table[:3]=[11,-7,19];extra="3 11 -7 19\n"
        if mode==4:prefix="putint(a[20]);putch(32);";action="a[20]=x;";table[20]=-17
        cases[name]=(f'''void mutate(int p[],int depth){{if(depth)mutate(p,depth-1);else p[20]=-17;}}
          int main(){{int x=getint();int a[32]={{}};{prefix}int i=0;
            while(i<8){{a[i]=3;i=i+1;}}while(i<32){{a[i]=7;i=i+1;}}{action}
            int sum=0;i=0;while(i<32){{sum=sum+a[i];i=i+1;}}
            putint(sum);putch(32);putarray(32,a);return 0;}}''',
          ("0 " if mode==4 else "")+f"{sum(table)} 32: "+" ".join(map(str,table))+"\n0\n")
        (directory / f"{name}.in").write_text("-17\n"+extra)
    # Recursive callees remain visible to cross-call analyses. All call sites,
    # intervening writes, object offsets and argument effects are observable.
    for mode in range(8):
        name=f"global_context_{mode}";data=[5,7,11,13];out=[]
        actions=[]
        if mode==0:actions.append("putint(read(2));putch(32);");out.append(data[2])
        if mode==1:actions.append("if(x>0)data[2]=19;")
        else:actions.append("data[2]=19;");data[2]=19
        actions.append("putint(read(2));putch(32);");out.append(data[2])
        if mode==2:actions.append("data[2]=23;");data[2]=23
        if mode==3:actions.append("getarray(data);");data[:3]=[29,31,37]
        if mode==4:actions.append("data[1]=23;");data[1]=23
        if mode==5:actions.append("data[2]=x;");data[2]=-17
        if mode==6:actions.append("touch(data,2);");data[2]=41
        if mode==7:actions.append("data[2]=19;")
        actions.append("putint(read(2));putch(32);");out.append(data[2])
        cases[name]=('''int data[4]={5,7,11,13};int read(int n){if(n)return read(n-1);return data[2];}
          void touch(int p[],int n){if(n)touch(p,n-1);else p[2]=41;}
          int main(){int x=getint();'''+"".join(actions)+"putarray(4,data);return 0;}",
          " ".join(map(str,out))+" 4: "+" ".join(map(str,data))+"\n0\n")
        (directory / f"{name}.in").write_text("-17\n"+("3 29 31 37\n" if mode==3 else ""))
    for mode in range(5):
        name=f"pointer_context_{mode}"
        args=["a[0],a[0]","a[0],b[0]","a[0],a[1]","local,a[0]","a[0],a[0]"][mode]
        a=list(range(8));b=list(range(31,39));local=[71,73,79,83]
        p=local if mode==3 else a
        q=a if mode in (0,3,4) else b if mode==1 else a
        offset=4 if mode==2 else 0
        p[1]=wrap(p[1]-17);q[offset+1]=wrap(q[offset+1]+7)
        value=wrap(p[1]+q[offset+1])
        # A second actual-object/offset conflicts with otherwise uniform facts.
        second=""
        if mode==4:second="putint(run(b[0],b[1],3,2));putch(32);";b[1]+=3;b[5]+=7
        cases[name]=(f'''int a[2][4];int b[2][4];
          int run(int p[],int q[],int x,int n){{if(n)return run(p,q,x,n-1);
            p[1]=p[1]+x;q[1]=q[1]+7;return p[1]+q[1];}}
          int main(){{int x=getint();int local[4]={{71,73,79,83}};int i=0;
            while(i<8){{a[i/4][i%4]=i;b[i/4][i%4]=31+i;i=i+1;}}
            putint(run({args},x,2));putch(32);{second}putarray(8,a[0]);putarray(8,b[0]);putarray(4,local);return 0;}}''',
          f"{value} "+(f"{b[1]+b[5]} " if mode==4 else "")+"8: "+" ".join(map(str,a))+
          "\n8: "+" ".join(map(str,b))+"\n4: "+" ".join(map(str,local))+"\n0\n")
        (directory / f"{name}.in").write_text("-17\n")
    if opts.benchmark:
        fixture = ROOT / "build/polyhedral-fixture/performance"
        fixture.mkdir(parents=True,exist_ok=True)
        size,count,seed = 257,160,-731
        source = f'''int a[{size}][{size}];int main(){{int seed=getint();int r=0;starttime();
          while(r<{count}){{int j=0;while(j<{size}){{int i=0;while(i<{size}){{
            a[i][j]=a[i][j]*3+seed+i*31+j;i=i+1;}}j=j+1;}}r=r+1;}}stoptime();
          int sum=0;int i=0;while(i<{size}){{int j=0;while(j<{size}){{sum=sum+a[i][j];j=j+1;}}i=i+1;}}
          putint(sum);return 0;}}'''
        factor = 0
        for _ in range(count): factor = wrap(factor*3+1)
        expected = wrap(factor*sum(seed+i*31+j for i in range(size) for j in range(size)))
        (fixture / "column_recurrence.sy").write_text(source)
        (fixture / "column_recurrence.in").write_text(f"{seed}\n")
        (fixture / "column_recurrence.out").write_text(f"{expected}\n0\n")
        # Ordinary array GEMM: runtime seed prevents value specialization,
        # independent modular / binary32 models preserve each reduction step.
        for floating in (False,True):
            rows,columns,reduction,repeats = 43,67,71,20 if floating else 80
            kind = "float" if floating else "int"
            seed = -13 if floating else -731
            name = f"{kind}_dot"
            ax = "(seed+i-k)*0.1" if floating else "seed+i*31-k*7"
            bx = "(k+j-7)*0.3" if floating else "k*11+j*17-9"
            source = f'''{kind} a[{rows}][{reduction}];{kind} b[{reduction}][{columns}];{kind} c[{rows}][{columns}];
              int main(){{int seed=getint();int i=0;while(i<{rows}){{int k=0;while(k<{reduction}){{
                a[i][k]={ax};k=k+1;}}i=i+1;}}int k=0;while(k<{reduction}){{int j=0;
                while(j<{columns}){{b[k][j]={bx};j=j+1;}}k=k+1;}}
              int r=0;starttime();while(r<{repeats}){{i=0;while(i<{rows}){{int j=0;
                while(j<{columns}){{{kind} sum=0;k=0;while(k<{reduction}){{sum=sum+a[i][k]*b[k][j];k=k+1;}}
                  c[i][j]=sum;j=j+1;}}i=i+1;}}r=r+1;}}stoptime();
              put{ 'farray' if floating else 'array' }({rows*columns},c[0]);return 0;}}'''
            expected = []
            for i in range(rows):
                for j in range(columns):
                    total = 0.0 if floating else 0
                    for k in range(reduction):
                        x = f32((seed+i-k)*f32(.1)) if floating else seed+i*31-k*7
                        y = f32((k+j-7)*f32(.3)) if floating else k*11+j*17-9
                        total = f32(total+f32(x*y)) if floating else wrap(total+wrap(x*y))
                    expected.append(total.hex() if floating else str(total))
            (fixture / f"{name}.sy").write_text(source)
            (fixture / f"{name}.in").write_text(f"{seed}\n")
            (fixture / f"{name}.out").write_text(f"{rows*columns}: "+" ".join(expected)+"\n0\n")
        print(f"Wrote column recurrence and ordered integer/float dot fixtures to {fixture}")
        fixture = ROOT / "build/scalar-fixture/performance"
        fixture.mkdir(parents=True,exist_ok=True)
        count,seed = 10000000,-31
        x,y,_ = recurrence([[1664525,0,1013904223],[1664525,1,1013904223],[0,0,1]],[seed,7,1],count)
        source = '''int data[8];int main(){int x=getint();int n=getint();data[2]=x;data[5]=7;
          int i=0;starttime();while(i<n){data[2]=data[2]*1664525+1013904223;
            data[5]=data[5]+data[2];i=i+1;}stoptime();putf("%d %d\\n",data[2],data[5]);return 0;}'''
        (fixture / "loop_state.sy").write_text(source)
        (fixture / "loop_state.in").write_text(f"{seed} {count}\n")
        (fixture / "loop_state.out").write_text(f"{x} {y}\n0\n")
        source = '''int state=17;int main(){int n=getint();int i=0;starttime();
          while(i<n){state=i*31;i=i+1;}stoptime();putint(state);return 0;}'''
        (fixture / "write_only.sy").write_text(source)
        (fixture / "write_only.in").write_text(f"{count*10}\n")
        (fixture / "write_only.out").write_text(f"{wrap((count*10-1)*31)}\n0\n")
        source = '''int main(){int x=getint();int n=getint();int i=0;int sum=0;starttime();
          while(i<n){if(x>=0)sum=sum+x/8+x%8+x/16+x%16+x/64+x%64;
            else sum=sum-x/8-x%8-x/16-x%16-x/64-x%64;
            x=x*1664525+1013904223;i=i+1;}
          stoptime();putf("%d %d\\n",sum,x);return 0;}'''
        x,total = seed,0
        for _ in range(count):
            magnitude,sign = abs(x),1 if x >= 0 else -1
            contribution = sum(sign*(magnitude>>shift)+x-sign*((magnitude>>shift)<<shift) for shift in (3,4,6))
            total = wrap(total+sign*contribution)
            x = wrap(x*1664525+1013904223)
        (fixture / "guarded_division.sy").write_text(source)
        (fixture / "guarded_division.in").write_text(f"{seed} {count}\n")
        (fixture / "guarded_division.out").write_text(f"{total} {x}\n0\n")
        print(f"Wrote loop-state, write-only and predicate-range fixtures to {fixture}")
        fixture = ROOT / "build/equality-fixture/performance"
        fixture.mkdir(parents=True,exist_ok=True)
        count = 10000000
        # Loop-carried products are observable separately and as a sum. A tree
        # cost favors factoring the sum even though the products already exist.
        source = '''int main(){int a=getint();int b=getint();int c=getint();int n=getint();
          int sum=7;int i=0;starttime();while(i<n){int p=a*b;int q=a*c;int r=p+q;
            sum=sum+p*q+r*sum;a=p+1;b=q+3;c=r+5;i=i+1;}stoptime();
          putf("%d %d %d %d\\n",a,b,c,sum);return 0;}'''
        a,b,c,total = -731,17,23,7
        for _ in range(count):
            p,q = wrap(a*b),wrap(a*c);r = wrap(p+q)
            total = wrap(total+p*q+r*total);a,b,c = wrap(p+1),wrap(q+3),wrap(r+5)
        (fixture / "shared_outputs.sy").write_text(source)
        (fixture / "shared_outputs.in").write_text(f"-731 17 23 {count}\n")
        (fixture / "shared_outputs.out").write_text(f"{a} {b} {c} {total}\n0\n")
        print(f"Wrote shared-output equality fixture to {fixture}")
        fixture = ROOT / "build/memory-phi-fixture/performance"
        fixture.mkdir(parents=True,exist_ok=True)
        source = '''int run(int a[],int b[],int seed,int n,int depth){
          if(depth)return run(a,b,seed,n,depth-1);int i=0;int sum=0;
          while(i<n){if(seed>=0)a[2]=seed*31;else a[2]=seed+17;
            sum=sum+a[2];b[2]=sum;seed=seed*1664525+1013904223;i=i+1;}
          return sum+a[2]+b[2];}
          int main(){int seed=getint();int n=getint();int a[4]={};int b[4]={};
            starttime();int result=run(a,b,seed,n,2);stoptime();putint(result);return 0;}'''
        seed,total,value = -31,0,0
        for _ in range(count):
            value = wrap(seed*31 if seed>=0 else seed+17)
            total = wrap(total+value);seed = wrap(seed*1664525+1013904223)
        (fixture / "branch_state.sy").write_text(source)
        (fixture / "branch_state.in").write_text(f"-31 {count}\n")
        (fixture / "branch_state.out").write_text(f"{wrap(total*2+value)}\n0\n")
        left = "".join(f"a[{j}]=seed*{j*2+3}+{j};" for j in range(8))
        right = "".join(f"a[{j}]=seed+{j*7+17};" for j in range(8))
        source = f'''int run(int a[],int b[],int seed,int n,int depth){{
          if(depth)return run(a,b,seed,n,depth-1);int i=0;int sum=0;
          while(i<n){{if(seed>=0){{{left}}}else{{{right}}}
            sum=sum+{'+'.join(f'a[{j}]' for j in range(8))};b[0]=sum;
            seed=seed*1664525+1013904223;i=i+1;}}
          return sum+a[0]+b[0];}}
          int main(){{int seed=getint();int n=getint();int a[8]={{}};int b[8]={{}};
            starttime();int result=run(a,b,seed,n,2);stoptime();putint(result);return 0;}}'''
        seed,total,value = -31,0,0
        for _ in range(count):
            value = wrap(seed*3 if seed>=0 else seed+17)
            contribution = seed*80+28 if seed>=0 else seed*8+332
            total = wrap(total+contribution);seed = wrap(seed*1664525+1013904223)
        (fixture / "branch_fields.sy").write_text(source)
        (fixture / "branch_fields.in").write_text(f"-31 {count}\n")
        (fixture / "branch_fields.out").write_text(f"{wrap(total*2+value)}\n0\n")
        print(f"Wrote path-specific memory fixture to {fixture}")
        return
    for name, (source, expected) in cases.items():
        (directory / f"{name}.sy").write_text(source)
        (directory / f"{name}.out").write_text(expected)
    (directory / "float_conditions.in").write_text("0\n")
    (directory / "integer_edges.in").write_text("2147483647 5\n")
    (directory / "tensor_call_control_flow.in").write_text("0\n")
    (directory / "saved_predicate.in").write_text("3 5\n")
    (directory / "constant_division.in").write_text("\n".join(map(str,numerators)) + "\n")
    (directory / "repeat_integer.in").write_text("-7\n")
    (directory / "recursive_purity.in").write_text("18\n")
    (directory / "tail_parallel_state.in").write_text("11\n")
    (directory / "guarded_divergence.in").write_text("0\n")
    (directory / "runtime_unroll_tails.in").write_text("\n".join(map(str,lengths))+"\n")
    (directory / "signed_mask_tests.in").write_text("137 -99\n")
    (directory / "sroa_reinitialization.in").write_text("12\n")
    (directory / "memory_float_bits.in").write_text("0\n")
    for opt in (0, 2, 3):
        subprocess.run(["python3", str(ROOT / "tools/test.py"), str(directory), "--compiler", str(opts.compiler),
                        "--opt", str(opt), "--check-alloc", "--output", str(ROOT / f"build/regression-o{opt}")], check=True)
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
