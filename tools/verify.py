#!/usr/bin/env python3
"""Mutation-test the MIR verifier and optionally prove int32 rewrite rules."""
import argparse
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]

MUTATIONS = r'''
#include "target_ir.hpp"
#include <functional>
#include <iostream>
#include <stdexcept>
using namespace sysy;
int main() {
  Function source; int rejected=0, valid=0;
  auto specimen=[&](RV opcode,ValueType result,std::initializer_list<ValueType> inputs) {
    MachineFunction f; f.source=&source; f.values={result};
    MachineInstruction instruction{Op::Target}; instruction.result=0; instruction.opcode=opcode;
    std::vector<int> args; for(auto type:inputs) {args.push_back(f.values.size());f.values.push_back(type);}
    instruction.args=args; f.blocks.push_back({"entry",{instruction,MachineInstruction{Op::Return}}}); return f;
  };
  auto reject=[&](MachineFunction f) {
    try { verifyTarget(f); } catch(const std::logic_error &) { ++rejected; return; }
    throw std::runtime_error("verifier accepted a malformed MIR mutation");
  };
  std::vector<MachineFunction> examples={
    specimen(RV::Addw,ValueType::Int,{ValueType::Int,ValueType::Int}),
    specimen(RV::FmulS,ValueType::Float,{ValueType::Float,ValueType::Float}),
    specimen(RV::FleS,ValueType::Int,{ValueType::Float,ValueType::Float}),
    specimen(RV::FcvtDS,ValueType::Double,{ValueType::Float}),
    specimen(RV::FmvDX,ValueType::Double,{ValueType::Pointer}),
    specimen(RV::Addi,ValueType::Pointer,{ValueType::Pointer}),
    specimen(RV::Mul,ValueType::Pointer,{ValueType::Int,ValueType::Int})};
  for(const auto &f:examples) {
    verifyTarget(f);++valid;
    for(size_t a=1;a<f.values.size();++a) {
      auto bad=f; bad.values[a]=f.values[a]==ValueType::Float ? ValueType::Int : ValueType::Float;reject(bad);
    }
    auto bad=f;bad.values[0]=f.values[0]==ValueType::Int||f.values[0]==ValueType::Pointer ? ValueType::Float : ValueType::Int;reject(bad);
    bad=f;bad.blocks[0].instructions[0].result=-2;reject(bad);
    bad=f;bad.blocks[0].instructions[0].args={99};reject(bad);
    bad=f;bad.blocks[0].instructions[0].opcode=static_cast<RV>(999);reject(bad);
    bad=f;bad.blocks[0].instructions.insert(bad.blocks[0].instructions.begin(),MachineInstruction{Op::Return});reject(bad);
  }
  auto bad=examples[1];bad.values[1]=ValueType::Double;reject(bad);
  bad=examples[0];bad.values[0]=ValueType::Pointer;reject(bad);
  bad=examples[5];bad.blocks[0].instructions[0].immediate=2048;reject(bad);
  bad=examples[0];bad.blocks[0].instructions.back()=MachineInstruction{Op::Jump};bad.blocks[0].instructions.back().target=1;reject(bad);
  bad=examples[0];bad.blocks[0].instructions[0]=MachineInstruction{Op::Load};bad.blocks[0].instructions[0].result=0;bad.blocks[0].instructions[0].args={1};reject(bad);
  bad=examples[0];bad.blocks[0].instructions[0]=MachineInstruction{Op::SpillLoad};bad.blocks[0].instructions[0].result=0;bad.slots={ValueType::Float};reject(bad);
  bad=examples[0];bad.values[0]=static_cast<ValueType>(99);reject(bad);
  bad=examples[0];bad.blocks[0].instructions[0].integerClobbers=uint32_t{1}<<31;reject(bad);
  for(int displacement:{-2048,2047}) {
    auto memory=examples[0];memory.values[1]=ValueType::Pointer;
    auto &load=memory.blocks[0].instructions[0];load=MachineInstruction{Op::Load};
    load.result=0;load.args={1};load.immediate=displacement;verifyTarget(memory);++valid;
    load.immediate=displacement<0 ? -2049 : 2048;reject(memory);
  }
  std::cout<<valid<<" valid MIR examples; "<<rejected<<" malformed mutations rejected\n";
}
'''


def check_mir(build):
    objects = sorted((build / "CMakeFiles/compiler.dir/src").glob("*.cpp.o"))
    objects = [p for p in objects if p.name != "main.cpp.o"]
    if not objects:
        raise RuntimeError("Build the compiler with CMake before running MIR checks")
    with tempfile.TemporaryDirectory(prefix="sysy-verify-") as directory:
        source, binary = Path(directory)/"mutations.cpp", Path(directory)/"mutations"
        source.write_text(MUTATIONS)
        subprocess.run([*shlex.split(os.environ.get("CXX","c++")), "-std=c++17", "-I", str(ROOT/"src"),
                        str(source), *map(str,objects), "-o", str(binary)],check=True)
        subprocess.run([str(binary)],check=True)


def check_smt():
    import z3
    x, accumulator, term = z3.BitVecs("x accumulator term",32)
    condition = z3.Bool("condition")
    count = 0

    def prove(name, before, after, *assumptions):
        nonlocal count
        solver = z3.Solver(); solver.set(timeout=10000)
        solver.add(*assumptions, before != after)
        result = solver.check()
        if result != z3.unsat:
            detail = solver.model() if result == z3.sat else solver.reason_unknown()
            raise AssertionError(f"{name}: {result}: {detail}")
        count += 1

    # These express IR/RISC-V int32 semantics independently of the optimizer.
    # Signed overflow is modular; signed remainder uses truncation, not modulo.
    for shift in range(32):
        for sign in (1,-1):
            divisor = sign*(1<<shift)
            if divisor > 2**31-1:
                continue
            d = z3.BitVecVal(divisor,32)
            mask = z3.BitVecVal((1<<shift)-1,32)
            q = z3.LShR(x,shift)
            prove(f"nonnegative division/{divisor}", x/d, q if sign > 0 else -q, x >= 0)
            prove(f"nonnegative remainder/{divisor}", z3.SRem(x,d), x & mask, x >= 0)
            prove(f"zero remainder/{divisor}", z3.SRem(x,d)==0, (x & mask)==0)
    prove("conditional integer accumulation", z3.If(condition,accumulator+term,accumulator),
          accumulator+z3.If(condition,z3.BitVecVal(1,32),z3.BitVecVal(0,32))*term)
    # Repeated positive power-of-two division truncates towards zero, even
    # for INT_MIN. Logical shifts act on its unsigned magnitude; large total
    # shifts saturate to zero instead of using the ISA's masked shift count.
    for shift in (1,4,30):
        for trips in (1,2,7,8,31,32,34):
            before=x
            for _ in range(trips):before=before/z3.BitVecVal(1<<shift,32)
            bits=shift*trips
            after=z3.BitVecVal(0,32) if bits>=32 else z3.If(x<0,-z3.LShR(-x,bits),z3.LShR(x,bits))
            prove(f"quotient recurrence/{shift}/{trips}",before,after)
    # Mathematical residue induction. Runtime guards separately ensure that
    # all source additions and the closed expression are nonnegative int32.
    integer=z3.Int('residue')
    for delta,modulus in ((128,65535),(1,17),(7,2147483647)):
        prove(f"residue induction/{delta}/{modulus}",((integer%modulus)+delta)%modulus,
              (integer+delta)%modulus,integer>=0)
    y=z3.BitVec('summary_arg',32)
    prove('composed affine return summary',x-(x+y)+y,z3.BitVecVal(0,32))
    pointer,displacement,stride=z3.BitVecs('pointer displacement stride',64)
    alpha,beta=z3.BitVecs('seed_alpha seed_beta',64)
    prove('congruent pointer recurrence seeds',pointer+alpha,(pointer+beta)+(alpha-beta))
    prove('congruent pointer recurrence step',(pointer+displacement)+stride,(pointer+stride)+displacement)
    for trips in (1,2,7,35,2147483647):
        before=accumulator
        if trips<36:
            for _ in range(trips):before=before+term
            prove(f'replay integer reduction/{trips}',before,accumulator+term*trips)
        else:
            prove('replay reduction induction',(accumulator+term*(trips-1))+term,accumulator+term*trips)
    for offset in (0,1,17,1024,2**31-1):
        prove(f"guarded nonnegative addition/{offset}", x+offset >= 0, z3.BoolVal(True),
              x >= 0,x <= 2**31-1-offset)
    # Equality discovery operates in the int32 ring. Validate its factoring,
    # constant reassociation and identities without signed-overflow premises.
    y,z = accumulator,term
    initial,limit,iv = z3.BitVecs("initial limit iv",32)
    prove("signed +1 induction preserves lower bound",iv+1>=initial,z3.BoolVal(True),
          initial>=0,iv>=initial,iv<limit)
    prove("signed +1 induction preserves upper bound",iv+1<=limit,z3.BoolVal(True),
          initial>=0,iv>=initial,iv<limit)
    prove("zero-trip header included",initial<=z3.If(initial>limit,initial,limit),z3.BoolVal(True))
    prove("canonical false-edge exact exit",iv,limit,limit>=0,iv>=0,iv<=limit,z3.Not(iv<limit))
    for j in range(4):
        prove(f"runtime chunk/{j}",iv+j<limit,z3.BoolVal(True),
              initial>=0,initial<limit,iv>=initial,iv<=limit-4)
    table=z3.Array("table",z3.BitVecSort(32),z3.BitVecSort(32))
    cells=[z3.Select(table,z3.BitVecVal(i,32))==term for i in range(16,20)]
    prove("uniform table slice",z3.Select(table,iv),term,iv>=16,iv<20,*cells)
    # These hypotheses matter: an unconstrained cell outside the slice differs.
    solver=z3.Solver();solver.add(*cells,iv==20,z3.Select(table,iv)!=term)
    if solver.check()!=z3.sat:raise AssertionError("table bounds counterexample missing")
    prove("integer factoring",x*y+x*z,x*(y+z))
    prove("add reassociation",(x+y)+z,x+(y+z))
    prove("multiply reassociation",(x*y)*z,x*(y*z))
    prove("and reassociation",(x & y) & z,x & (y & z))
    prove("subtraction to addition",x-y,x+(-y))
    prove("negation",-(-x),x)
    for name,before,after in (("add zero",x+0,x),("multiply one",x*1,x),
                               ("multiply zero",x*0,z3.BitVecVal(0,32)),
                               ("and ones",x & -1,x),("and zero",x & 0,z3.BitVecVal(0,32)),
                               ("subtract self",x-x,z3.BitVecVal(0,32))):
        prove(name,before,after)
    # A mutation must produce a counterexample: dropping the sign guard is unsound.
    solver=z3.Solver();solver.add(x/8 != z3.LShR(x,3))
    if solver.check() != z3.sat:
        raise AssertionError("SMT check failed to detect a missing sign guard")
    print(f"{count} integer rule obligations proved; missing-guard mutation rejected; Z3 {z3.get_version_string()}")


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build",type=Path,default=ROOT/"build")
    parser.add_argument("--smt",action="store_true",help="also prove rules with optional z3-solver")
    opts=parser.parse_args()
    check_mir(opts.build.resolve())
    if opts.smt: check_smt()


if __name__ == "__main__":
    main()
