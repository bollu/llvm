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
#include "llvm/Analysis/ConstantFolding.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Transforms/GraphRewrite.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Utils/Local.h"
#include "llvm/Analysis/OptimizationDiagnosticInfo.h"
#include <algorithm>
#include <climits>

#define DEBUG_TYPE "graphrewrite"
using namespace llvm;

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

