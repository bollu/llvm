//===- GraphRewrite.h - InstCombine pass ------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
/// \file
///
/// This file provides the primary interface to the graph rewriting
/// infrastructure.
///
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_GRAPH_REWRITE_H
#define LLVM_TRANSFORMS_GRAPH_REWRITE_H

#include "llvm/IR/Function.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Transforms/InstCombine/InstCombineWorklist.h"

namespace llvm {

class GraphRewritePass : public PassInfoMixin<GraphRewritePass> {

public:
  static StringRef name() { return "GraphRewritePass"; }

  explicit GraphRewritePass()
  {}

  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM) { return llvm::PreservedAnalyses::all(); }
};

/// \brief The legacy pass manager's instcombine pass.
///
/// This is a basic whole-function wrapper around the instcombine utility. It
/// will try to combine all instructions in the function.
class GraphRewriteLegacyPass : public FunctionPass {

public:
  static char ID; // Pass identification, replacement for typeid

  GraphRewriteLegacyPass()
      : FunctionPass(ID) {
    initializeGraphRewriteLegacyPassPass(*PassRegistry::getPassRegistry());
  }

  void getAnalysisUsage(AnalysisUsage &AU) const override {};
  bool runOnFunction(Function &F) override { return false; }
};

Pass *createGraphRewriteLegacyPass();
}

#endif
