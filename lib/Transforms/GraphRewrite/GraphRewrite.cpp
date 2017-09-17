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
#include "llvm/ADT/SmallSet.h"
#include "llvm/IR/CFG.h"
#include "llvm/Analysis/ConstantFolding.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Transforms/GraphRewrite/GraphRewrite.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Utils/Local.h"
#include "llvm/Analysis/OptimizationDiagnosticInfo.h"
#include "llvm/Support/FileSystem.h"
#include <climits>
#include "llvm/Support/GraphWriter.h"

#define DEBUG_TYPE "graphrewrite"
using namespace llvm;

static cl::opt<bool>
DotPEG("dot-peg", cl::init(false), cl::Hidden, cl::ZeroOrMore,
            cl::desc("write PEG from -graphrewrite to a dot file"));


//===----------------------------------------------------------------------===//
// PEGNode
//===----------------------------------------------------------------------===//
raw_ostream &llvm::operator <<(raw_ostream &os, const PEGNode &N) {
    N.print(os);
    return os;
}
//===----------------------------------------------------------------------===//
// PEGConditionNode
//===----------------------------------------------------------------------===//

void PEGConditionNode::print(raw_ostream &os) const  {
    os << "condition: " << PEGBB->getName();
}

//===----------------------------------------------------------------------===//
// PEGPhiNode
//===----------------------------------------------------------------------===//
void PEGPhiNode::print(raw_ostream &os)  const {
    os << "phi(" << *Cond << ", " << *True << ", " << *False << ")\n";
}

//===----------------------------------------------------------------------===//
// PEGBasicBlock
//===----------------------------------------------------------------------===//
void PEGBasicBlock::print(raw_ostream &os) const {
    os << "pegbb-" << std::string(BB->getName()) << "\n";
    if (Children.size())
        for (const PEGNode *Child : Children) {
            errs() << "\t-" << *Child << "\n";
        }
}

PEGBasicBlock::PEGBasicBlock(PEGFunction *Parent, PEGBasicBlock *InsertBefore, const BasicBlock *BB) :
    APEG(true), Parent(Parent), BB(BB) {
        if (InsertBefore)
            Parent->getBasicBlockList().insert(InsertBefore->getIterator(), this);
        else
            Parent->getBasicBlockList().push_back(this);
    };

//===----------------------------------------------------------------------===//
// PEGFunction
//===----------------------------------------------------------------------===//

void PEGFunction::print(raw_ostream &os) const {
    for (const PEGBasicBlock &BB : BasicBlocks) {
        errs() << "- " << BB;
    }
}
 raw_ostream &llvm::operator <<(raw_ostream &os, const PEGFunction &F) {
     F.print(os);
     return os;
 }

 template<>
 struct DOTGraphTraits<const PEGFunction*> : public DefaultDOTGraphTraits {

   // HACK: DOTGraphTraits (bool isSimple=false) : DefaultDOTGraphTraits(isSimple) {}
   DOTGraphTraits (bool isSimple=false) : DefaultDOTGraphTraits(true) {}

   static std::string getGraphName(const PEGFunction *F) {
     return "CFG for '" + F->getName().str() + "' function";
   }

   static std::string getNodeLabel(const PEGNode *Node,
           const PEGFunction *) {

       assert (Node);

       std::string Str;
       raw_string_ostream OS(Str);
       Node->print(OS);

       return OS.str();

   }

 };



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

    std::map<const BasicBlock*, PEGBasicBlock*> BBMap;
    std::map<const BasicBlock *, PEGConditionNode*> CondMap;

    PEGFunction *createAPEG(const Function &F);
    PEGNode *computeInputs(const BasicBlock *BB) const;
    PEGNode *makeDecideNode(const BasicBlock *Cur, const SmallSet<const BasicBlock*, 4> &In, const Loop *Outer) const;

    PEGBasicBlock *getPEGNodeFor(const BasicBlock *BB) const {
        auto It = BBMap.find(BB);
        if (It == BBMap.end())
            report_fatal_error("expected PEG for BB: " + BB->getName());
        return It->second;
    }
};

const BasicBlock *findCommonDominator(DominatorTree &DT, const SmallSet<const BasicBlock *, 4> &In) {
    assert(In.size() > 0);
    const BasicBlock *FinalDominator = nullptr;
    for (const BasicBlock *BB : In) {
        if (!FinalDominator) {
            FinalDominator = BB;
            continue;
        }
        FinalDominator = DT.findNearestCommonDominator(FinalDominator, BB);
    }
    return FinalDominator;
}

bool LoopContainedIn(const Loop *Inner, const Loop *Outer) {
     if (Inner == nullptr && Outer == nullptr) return true;
     if (!Inner || !Outer) return false;
     return Outer->contains(Inner);
}

SmallSet<const BasicBlock *, 4> filterSet(const SmallSet<const BasicBlock*, 4> &In,
        std::function<bool(const BasicBlock *)> Predicate) {
    SmallSet<const BasicBlock *, 4> BBSet;
    for(auto BB : In) {
        if (Predicate(BB))
            BBSet.insert(BB);
    }
    return BBSet;
}

// Return the successor if the true, false branch are taken.
// I know, this is WTF, and will fail on switch. sue me :(
std::pair<const BasicBlock *, const BasicBlock*> getTrueFalseSuccessors(const BasicBlock *BB) {
    if (auto *Succ = BB->getSingleSuccessor())
        return std::make_pair(Succ, Succ);

    const TerminatorInst *TI = BB->getTerminator();
    const BranchInst *BI = cast<BranchInst>(TI);
    assert(BI->isConditional() && "should not have reached here, should have returned at getSingleSuccessor");
    return std::make_pair(BI->getSuccessor(0), BI->getSuccessor(1));
}


PEGNode *GraphRewrite::makeDecideNode(const BasicBlock *Cur, const SmallSet<const BasicBlock*, 4> &In, const Loop *Outer) const {
    if (In.size() == 0) return nullptr; // not sure if this is correct.
    const BasicBlock *CommonDom = findCommonDominator(DT, In);
    const BasicBlock *TrueBB, *FalseBB;

    std::tie(TrueBB, FalseBB) = getTrueFalseSuccessors(CommonDom);
    assert(TrueBB && "TrueBB uninitialized");
    assert(FalseBB && "FalseBB uninitialized");

    const Loop *CommonDomLoop = LI.getLoopFor(CommonDom);

    if (LoopContainedIn(CommonDomLoop, Outer)) {
        if (In.size() == 1) {
            for(const BasicBlock *BB : In)
                return getPEGNodeFor(BB);
        }

        SmallSet<const BasicBlock *, 4> TrueNodes = filterSet(In,
                [&](const BasicBlock *BB) {
                return isPotentiallyReachable(TrueBB, BB, &DT, &LI);
                });
        PEGNode *TrueNode = makeDecideNode(Cur, TrueNodes, Outer);

        SmallSet<const BasicBlock *, 4> FalseNodes = filterSet(In,
                [&](const BasicBlock *BB) {
                return isPotentiallyReachable(FalseBB, BB, &DT, &LI);
                });
        PEGNode *FalseNode = makeDecideNode(Cur, TrueNodes, Outer);

        PEGConditionNode *Condition;
        auto It = CondMap.find(CommonDom);
        if (It == CondMap.end()) report_fatal_error("expected condition node for basic block");
        Condition = It->second;

        return new PEGPhiNode(Condition, TrueNode, FalseNode);
    }
    else {
        assert (false && "unimplemented");
    }


}

PEGNode *GraphRewrite::computeInputs(const BasicBlock *BB) const {
    if(LI.isLoopHeader(BB)) {
        assert(false && "unhandled");
    // return new PEGThetaNode(
    }

    SmallSet<const BasicBlock *, 4> In;
    for(auto PredBB : predecessors(BB)) In.insert(PredBB);
    return makeDecideNode(BB, In, LI.getLoopFor(BB));
}


PEGFunction *GraphRewrite::createAPEG(const Function &F) {
    PEGFunction *PEGF = new PEGFunction(F);
    for (const BasicBlock &BB : F) {
        DEBUG(dbgs() << "running on: " << BB.getName() << "\n");
        PEGBasicBlock *PEGBB = PEGBasicBlock::createAPEG(PEGF, &BB);
        BBMap[&BB] = PEGBB;
        CondMap[&BB] = new PEGConditionNode(PEGBB);
    };

    for(auto It : BBMap) {
        PEGNode *Child = computeInputs(It.first);
        if (Child) It.second->setChild(Child);
        else
            errs() << *It.second << "can't have a child.\n";
    }

    return PEGF;
};


static void writePEGToDotFile(PEGFunction &F) {
  std::string Filename = ("peg." + F.getName() + ".dot").str();
  errs() << "Writing '" << Filename << "'...";

  std::error_code EC;
  raw_fd_ostream File(Filename, EC, sys::fs::F_Text);

  if (!EC)
    WriteGraph(File, (const PEGFunction*)&F);
  else
    errs() << "  error opening file for writing!";
  errs() << "\n";
}


bool GraphRewrite::run(Function &F) {
  PEGFunction *PEGF = createAPEG(F);

  if(DotPEG) {
      writePEGToDotFile(*PEGF);
  }
  outs() << *PEGF << "\n";
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
  AU.addRequired<DominatorTreeWrapperPass>();
  AU.addRequired<LoopInfoWrapperPass>();
  AU.addRequired<ScalarEvolutionWrapperPass>();

};


bool GraphRewriteLegacyPass::runOnFunction(Function &F) {
    auto &LI = getAnalysis<LoopInfoWrapperPass>().getLoopInfo();
    auto &SE = getAnalysis<ScalarEvolutionWrapperPass>().getSE();
    auto &DT = getAnalysis<DominatorTreeWrapperPass>().getDomTree();

    GraphRewrite GR(DT, LI, SE);
    return GR.run(F);

}

