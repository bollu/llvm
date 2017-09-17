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

class PEGBasicBlock;
class PEGInst;
class PEGFunction;
class PEGOperand;
class PEGOperand {

};

class PEGInst : public User, public ilist_node_with_parent<PEGInst, PEGBasicBlock,
                                    ilist_sentinel_tracking<true>> {
  PEGBasicBlock *Parent = nullptr;  // Pointer to the owning basic block.
  std::vector<PEGOperand *> Operands;
};


// Structured very similar to machineBB;
class PEGBasicBlock : public ilist_node_with_parent<PEGBasicBlock, PEGFunction> {
  using PEGInsts = ilist<PEGInst, ilist_sentinel_tracking<true>>;
  PEGInsts Insts;
  const BasicBlock *BB;
  std::vector<PEGBasicBlock *> Predecessors;
  std::vector<PEGBasicBlock *> Successors;

  // Intrusive list support
  PEGBasicBlock() = default;

  explicit PEGBasicBlock(PEGFunction &PEGF, const BasicBlock *BB);

  ~PEGBasicBlock();

  // MachineBasicBlocks are allocated and owned by MachineFunction.
  friend class PEGFunction;
};

class PEGFunction {
  const Function *Fn;

  // List of machine basic blocks in function
  using BasicBlockListType = ilist<PEGBasicBlock>;
  BasicBlockListType BasicBlocks;

  std::vector<PEGBasicBlock*> MBBNumbering;


public:
  PEGFunction(const Function *Fn);
  PEGFunction(const PEGFunction &) = delete;
  PEGFunction &operator=(const PEGFunction &) = delete;
  ~PEGFunction();

  /// getFunction - Return the LLVM function that this machine code represents
  const Function *getFunction() const { return Fn; }

  /// getName - Return the name of the corresponding LLVM function.
  StringRef getName() const;


};

class GraphRewritePass : public PassInfoMixin<GraphRewritePass> {

public:
  static StringRef name() { return "GraphRewritePass"; }
  explicit GraphRewritePass() {}
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM);
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

  void getAnalysisUsage(AnalysisUsage &AU) const override;
  bool runOnFunction(Function &F) override;
};

Pass *createGraphRewriteLegacyPass();
}

#endif
