//===- GraphRewrite.cpp -- Algebraically rewrite instruction graphs -------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
//===----------------------------------------------------------------------===//

#include "llvm/Analysis/CFG.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/Dominators.h"
#include "llvm/Analysis/ScalarEvolution.h"

#include "llvm/Analysis/ConstantFolding.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Transforms/GraphRewrite/GraphRewrite.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Utils/Local.h"
#include "llvm/Analysis/OptimizationDiagnosticInfo.h"
#include <algorithm>
#include <climits>

#define DEBUG_TYPE "graphrewrite"
using namespace llvm;

//===----------------------------------------------------------------------===//
// GraphRewrite
//===----------------------------------------------------------------------===//


// Pass code
// Modeled after EarlyCSE.
class GraphRewrite {
public:
      GraphRewrite(DominatorTree &DT,LoopInfo &LI, ScalarEvolution &SE)
      : DT(DT), LI(LI), SE(SE) {
  }

  bool run(Function &F);
private:
    DominatorTree &DT;
    LoopInfo &LI;
    ScalarEvolution &SE;
};

bool GraphRewrite::run(Function &F) {
  return false;   
}
//===----------------------------------------------------------------------===//
// GraphRewritePass
//===----------------------------------------------------------------------===//

PreservedAnalyses llvm::GraphRewritePass::run(Function &F, FunctionAnalysisManager &AM) {
  auto &LI = AM.getResult<LoopAnalysis>(F);
  auto &DT = AM.getResult<DominatorTreeAnalysis>(F);
  auto &SE = AM.getResult<ScalarEvolutionAnalysis>(F);
  GraphRewrite GR(DT, LI, SE);
  GR.run(F);


  PreservedAnalyses PA = PreservedAnalyses::none();
  return PA;

} // { return llvm::PreservedAnalyses::all(); }


//===----------------------------------------------------------------------===//
// GraphRewriteLegacyPass
//===----------------------------------------------------------------------===//
char GraphRewriteLegacyPass::ID = 0;

INITIALIZE_PASS_BEGIN(GraphRewriteLegacyPass, "graphrewrite",
                      "rewrite instructions as graph grammars", false, false)
INITIALIZE_PASS_DEPENDENCY(OptimizationRemarkEmitterWrapperPass)
INITIALIZE_PASS_END(GraphRewriteLegacyPass, "graphrewrite",
                      "rewrite instructions as graph grammars", false, false)

Pass *llvm::createGraphRewriteLegacyPass() {
    return new GraphRewriteLegacyPass();
}

void llvm::initializeGraphRewrite(PassRegistry &Registry) {
    initializeGraphRewriteLegacyPassPass(Registry);
}

void GraphRewriteLegacyPass::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.setPreservesCFG();
  AU.addRequired<DominatorTreeWrapperPass>();

};


bool GraphRewriteLegacyPass::runOnFunction(Function &F) { 
    auto &LI = getAnalysis<LoopInfoWrapperPass>().getLoopInfo();
    auto &SE = getAnalysis<ScalarEvolutionWrapperPass>().getSE();
    auto &DT = getAnalysis<DominatorTreeWrapperPass>().getDomTree();

    GraphRewrite GR(DT, LI, SE);
    return GR.run(F);

}

