//===- JumpThreading.cpp - Thread control through conditional blocks ------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements the always inline isolator, which splits calls which
// are tagged with alwaysinline into a separate BB
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/IPO/AlwaysInlineIsolator.h"
#include "llvm/Transforms/IPO.h"

using namespace llvm;

#define DEBUG_TYPE "always-inline-isolator"

namespace {

/// This pass performs 'jump threading', which looks at blocks that have
/// multiple predecessors and multiple successors.  If one or more of the
/// predecessors of the block can be proven to always jump to one of the
/// successors, we forward the edge from the predecessor to the successor by
/// duplicating the contents of this block.
///
/// An example of when this can occur CIs code like this:
///
///   if () { ...
///     X = 4;
///   }
///   if (X < 3) {
///
/// In this case, the unconditional branch at the end of the first if can be
/// revectored to the false side of the second if.
class AlwaysInlineIsolator : public FunctionPass {
  AlwaysInlineIsolatorPass Impl;

public:
  static char ID; // Pass identification

  AlwaysInlineIsolator() : FunctionPass(ID), Impl() {
    initializeAlwaysInlineIsolatorPass(*PassRegistry::getPassRegistry());
  }

  bool runOnFunction(Function &F) override {
    Impl.runImpl(F);
    return false;
  }
};
} // namespace

char AlwaysInlineIsolator::ID = 0;

// ORIGINAL:
// bb:
//   ..
//   X
//   I
//   Y
//   ..
//
// NEW:
// ---
//
// bb:
// ...
//   X
//   br bbi
//
// bbi:
//   I
//   br bb'
//
// bb:
//   Y
//   ..
std::tuple<BasicBlock *, BasicBlock *, BasicBlock *>
isolateInstInNewBB(Instruction *I, std::string MiddleBBName) {
  BasicBlock *A = I->getParent();
  BasicBlock *B = SplitBlock(A, I);
  B->setName(MiddleBBName);
  BasicBlock *C = SplitBlock(B, B->getFirstNonPHI()->getNextNode());
  C->setName(A->getName() + ".split");
  return std::make_tuple(A, B, C);
}

void AlwaysInlineIsolatorPass::runImpl(Function &F) {
  std::vector<CallInst *> CIs;
  for (BasicBlock &BB : F) {
    for (Instruction &I : BB) {
      CallInst *CI = dyn_cast<CallInst>(&I);
      if (!CI)
        continue;

      Function *Called = CI->getCalledFunction();
      if (!Called)
        continue;
      if (!Called->hasFnAttribute(Attribute::AlwaysInline))
        continue;

      CIs.push_back(CI);
    }
  }

  for (CallInst *CI : CIs) {
    isolateInstInNewBB(CI, std::string(CI->getCalledFunction()->getName()) +
                               ":" + std::string(CI->getParent()->getName()) +
                               ".isolated");
    errs() << __FUNCTION__ << ":" << __LINE__ << "\n";
  }
}

INITIALIZE_PASS_BEGIN(AlwaysInlineIsolator, "alwaysinlineisolator",
                      "Always inline isolator", false, false)

INITIALIZE_PASS_END(AlwaysInlineIsolator, "alwaysinlineisolator",
                    "Always inline isolator", false, false)

// Public interface to the AlwaysInlineIsolator pass
FunctionPass *llvm::createAlwaysInlineIsolatorPass() {
  return new AlwaysInlineIsolator();
}
