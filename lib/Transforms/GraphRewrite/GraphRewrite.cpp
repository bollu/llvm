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

#include "llvm/Transforms/GraphRewrite/GraphRewrite.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/Analysis/CFG.h"
#include "llvm/Analysis/ConstantFolding.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/OptimizationDiagnosticInfo.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Dominators.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/GraphWriter.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Utils/Local.h"
#include <climits>

#define DEBUG_TYPE "graphrewrite"
using namespace llvm;

static cl::opt<bool>
    DotPEG("dot-peg", cl::init(false), cl::Hidden, cl::ZeroOrMore,
           cl::desc("write PEG from -graphrewrite to a dot file"));

//===----------------------------------------------------------------------===//
// PEGConditionNode
//===----------------------------------------------------------------------===//

void PEGConditionNode::print(raw_ostream &os) const { os << getName(); }

//===----------------------------------------------------------------------===//
// PEGPhiNode
//===----------------------------------------------------------------------===//
void PEGPhiNode::print(raw_ostream &os) const { os << getName(); }

//===----------------------------------------------------------------------===//
// PEGThetaNode
//===----------------------------------------------------------------------===//
void PEGThetaNode::print(raw_ostream &os) const { os << getName(); }

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

PEGBasicBlock::PEGBasicBlock(PEGFunction *Parent, const BasicBlock *BB)
    : PEGNode(PEGNodeKind::PEGNK_BB, Parent, BB->getName()), APEG(true),
      Parent(Parent), BB(BB){};

//===----------------------------------------------------------------------===//
// PEGFunction
//===----------------------------------------------------------------------===//

void PEGFunction::print(raw_ostream &os) const {
  for (const PEGNode &N : Nodes) {
    errs() << N << "\n\n";
  }
}
raw_ostream &llvm::operator<<(raw_ostream &os, const PEGFunction &F) {
  F.print(os);
  return os;
}

//===----------------------------------------------------------------------===//
// PEGNode
//===----------------------------------------------------------------------===//
PEGNode::PEGNode(PEGNodeKind Kind, PEGFunction *Parent, const StringRef Name)
    : Parent(Parent), Kind(Kind), Name(Name) {
  Parent->getNodesList().push_back(this);
}
raw_ostream &llvm::operator<<(raw_ostream &os, const PEGNode &N) {
  N.print(os);
  return os;
}
template <>
struct DOTGraphTraits<const PEGFunction *> : public DefaultDOTGraphTraits {

  // HACK: DOTGraphTraits (bool isSimple=false) :
  // DefaultDOTGraphTraits(isSimple) {}
  DOTGraphTraits(bool isSimple = false) : DefaultDOTGraphTraits(true) {}

  static std::string getGraphName(const PEGFunction *F) {
    return "CFG for '" + F->getName().str() + "' function";
  }

  static std::string getNodeLabel(const PEGNode *Node, const PEGFunction *) {

    assert(Node);

    std::string Str;
    raw_string_ostream OS(Str);
    OS << Node->getName();

    return OS.str();
  }
};

//===----------------------------------------------------------------------===//
// GraphRewrite
//===----------------------------------------------------------------------===//

using LoopSet = std::set<Loop *>;
using ConstLoopSet = std::set<const Loop *>;

LoopSet makeLoopSet(Loop *L) {
  LoopSet LS;
  if (!L)
    return LS;

  for (Loop *Cur = L; Cur->getParentLoop() != nullptr;
       Cur = Cur->getParentLoop())
    LS.insert(Cur);
  return LS;
};

ConstLoopSet makeConstLoopSet(const Loop *L) {
  ConstLoopSet LS;
  if (!L)
    return LS;

  for (const Loop *Cur = L; Cur->getParentLoop() != nullptr;
       Cur = Cur->getParentLoop())
    LS.insert(Cur);
  return LS;
};


// using BBSet = SmallSet<const BasicBlock *, 4>;
using BBEdge = Use;
using BBEdgeSet = SmallSet<const BBEdge *, 4>;

using ValueFn = std::function<PEGNode*(const BBEdge&)>;

// Pass code
// Modeled after EarlyCSE.
class GraphRewrite {
public:
  GraphRewrite(DominatorTree &DT, LoopInfo &LI, ScalarEvolution &SE)
      : DT(DT), LI(LI), SE(SE) {}

  bool run(Function &F);

private:
  DominatorTree &DT;
  LoopInfo &LI;
  ScalarEvolution &SE;

  std::map<const BasicBlock *, PEGBasicBlock *> BBMap;
  std::map<const BasicBlock *, PEGConditionNode *> CondMap;

  PEGFunction *createAPEG(const Function &F);
  PEGNode *computeInputs(const BasicBlock *BB,
                         bool fromInsideLoop = false) const;
  BBEdgeSet computeBreakEdges(const Loop *L) const;
  PEGNode *makeBreakCondition(const BasicBlock *Cur, const Loop *L, BBEdgeSet BreakBBs, ConstLoopSet Outer) const;
  PEGNode *computeInputsFromInsideLoop(const BasicBlock *BB) const;
  PEGNode *makeDecideNode(BBEdge &Source,
                          BBEdgeSet &In,
                          ValueFn VF,
                          ConstLoopSet Outer) const;

  PEGBasicBlock *getPEGNodeFor(const BasicBlock *BB) const {
    auto It = BBMap.find(BB);
    if (It == BBMap.end())
      report_fatal_error("expected PEG for BB: " + BB->getName());
    return It->second;
  }

  PEGConditionNode *getConditionNodeFor(const BasicBlock *BB) const {
    auto It = CondMap.find(BB);
    if (It == CondMap.end())
      report_fatal_error("expected Cond for BB: " + BB->getName());
    return It->second;
  }
};

const BasicBlock *useToSourceBB(const Use &U) {
    return cast<BasicBlock>(U.get());
}

BasicBlock *useToSourceBB(Use &U) {
    return cast<BasicBlock>(U.get());
}

const BasicBlock *
findCommonDominator(DominatorTree &DT,
                    BBEdgeSet &In) {
  assert(In.size() > 0);
  const BasicBlock *FinalDominator = nullptr;
  for (const Use *U : In) {
    if (!FinalDominator) {
      FinalDominator = useToSourceBB(*U);
      continue;
    }
    FinalDominator = DT.findNearestCommonDominator(FinalDominator, useToSourceBB(*U));
  }
  return FinalDominator;
}

SmallSet<const BasicBlock *, 4>
filterSet(const SmallSet<const BasicBlock *, 4> &In,
          std::function<bool(const BasicBlock *)> Predicate) {
  SmallSet<const BasicBlock *, 4> BBSet;
  for (auto BB : In) {
    if (Predicate(BB))
      BBSet.insert(BB);
  }
  return BBSet;
}

// Return the successor if the true, false branch are taken.
// I know, this is WTF, and will fail on switch. sue me :(
std::pair<const BasicBlock *, const BasicBlock *>
getTrueFalseSuccessors(const BasicBlock *BB) {
  errs() << "BB: " << BB->getName() << "\n";
  // assert(!BB->getSingleSuccessor());

  if (const BasicBlock *Succ = BB->getSingleSuccessor())
    return std::make_pair(Succ, Succ);

  const TerminatorInst *TI = BB->getTerminator();
  const BranchInst *BI = cast<BranchInst>(TI);
  assert(BI->isConditional() && "should not have reached here, should have "
                                "returned at getSingleSuccessor");
  return std::make_pair(BI->getSuccessor(0), BI->getSuccessor(1));
}

template <typename T>
bool isSubset(const std::set<T> &MayInner, const std::set<T> &Outer) {
  return std::includes(Outer.begin(), Outer.end(), MayInner.begin(),
                       MayInner.end());
}

const Loop *getOutermostLoopNotInLoop(ConstLoopSet &Inner,
                                      ConstLoopSet &Outer) {
  assert(isSubset(Inner, Outer));
  const Loop *CurOutermost = nullptr;
  for (const Loop *CurOuter : Outer) {
    bool ContainsAllInner = true;
    for (const Loop *CurInner : Inner)
      if (!CurOuter->contains(CurInner))
        ContainsAllInner = false;

    if (!ContainsAllInner)
      continue;

    if (!CurOutermost) {
      CurOuter = CurOutermost;
    } else {
      if (CurOuter->contains(CurOutermost))
        CurOuter = CurOutermost;
    }
  }

  assert(CurOutermost);
  return CurOutermost;
}

BBSet GraphRewrite::computeBreakEdges(const Loop *L) const {
    BBSet BBs;

    SmallVector<BasicBlock *, 4> ExitingBBVec;
    L->getExitBlocks(ExitingBBVec);

    for(auto BB: ExitingBBVec) BBs.insert(BB);

    return BBs;
}

PEGNode *GraphRewrite::makeDecideNode(BBEdge &Source,
                        BBEdgeSet &In,
                        ValueFn VF,
                        ConstLoopSet Outer) const {


  if (In.size() == 0)
    return nullptr; // not sure if this is correct.
  const BasicBlock *CommonDom = findCommonDominator(DT, In);

  const Loop *CommonDomLoop = LI.getLoopFor(CommonDom);
  ConstLoopSet CommonDomLoopSet = makeConstLoopSet(CommonDomLoop);

  if (isSubset(CommonDomLoopSet, Outer)) {
    if (In.size() == 1) {
      for (const BasicBlock *BB : In)
        return getPEGNodeFor(BB);
    }

    assert(In.size() > 1);

    const BasicBlock *TrueBB, *FalseBB;

    std::tie(TrueBB, FalseBB) = getTrueFalseSuccessors(CommonDom);
    assert(TrueBB && "TrueBB uninitialized");
    assert(FalseBB && "FalseBB uninitialized");

    SmallSet<const BasicBlock *, 4> TrueNodes =
        filterSet(In, [&](const BasicBlock *BB) {
          return isPotentiallyReachable(TrueBB, BB, &DT, &LI);
        });
    PEGNode *TrueNode = makeDecideNode(Cur, TrueNodes, Outer);

    SmallSet<const BasicBlock *, 4> FalseNodes =
        filterSet(In, [&](const BasicBlock *BB) {
          return isPotentiallyReachable(FalseBB, BB, &DT, &LI);
        });
    PEGNode *FalseNode = makeDecideNode(Cur, FalseNodes, Outer);

    PEGConditionNode *Condition = getConditionNodeFor(CommonDom);
    return new PEGPhiNode(Condition, TrueNode, FalseNode);
  } else {
    const Loop *LNew = getOutermostLoopNotInLoop(CommonDomLoopSet, Outer);


    Outer.insert(LNew);
    PEGNode *Val = makeDecideNode(Cur, In, Outer);
    auto BreakBBs = computeBreakEdges(LNew);
    PEGNode *Break = makeBreakCondition(Cur, LNew, BreakBBs, Outer);
    return new PEGEvalNode(LNew, Val, new PEGPassNode(LNew, Break));
  }
}

PEGNode *GraphRewrite::makeBreakCondition(const BasicBlock *Cur, const Loop *L, 
        BBEdgeSet BreakBBs,
        ConstLoopSet Outer) const {
    return new makeDecideNode(Cur, 

};
static bool isLoopLatch(const LoopInfo &LI, const Loop *L,
                        const BasicBlock *Check) {
  assert(L);
  Loop *LCheck = LI.getLoopFor(Check);
  if (!LCheck)
    return false;
  if (LCheck != L)
    return false;
  return L->isLoopLatch(Check);
};

PEGNode *GraphRewrite::computeInputs(const BasicBlock *BB,
                                     bool fromInsideLoop) const {
  // When we are looking for stuff inside the loop, we are in a "virtual" node
  // that is not a loop header
  if (LI.isLoopHeader(BB) && !fromInsideLoop) {
    Loop *L = LI.getLoopFor(BB);

    BBEdgeSet In;
    // We want basic blocks that are _from_ the loop.
    if (fromInsideLoop) {
      for (auto PredBB : predecessors(BB)) {
        if (isLoopLatch(LI, L, PredBB))
          In.insert(PredBB->get);
      }
    } else {
      // We want basic blocks that are _outside_ the loop.
      for (auto PredBB : predecessors(BB)) {
        // The predecessor is not within us
        if (!isLoopLatch(LI, L, PredBB))
          In.insert(PredBB);
        // Not sure if the conditions are equivalent
        // if (!L->isLoopLatch(PredBB)) In.insert(PredBB);
      }
    };
    PEGNode *Decider =
        makeDecideNode(BB, In, makeConstLoopSet(LI.getLoopFor(BB)));
    {
      errs() << __PRETTY_FUNCTION__ << "\n";
      errs() << "* fromInsideLoop:" << fromInsideLoop << "\n";
      errs() << "* BB:" << BB->getName() << "\n";
      errs() << "* In:\n";
      for (auto I : In)
        errs() << "\t-" << I->getName() << "\n";
      errs() << "* Decider: " << Decider->getName() << "\n";
    };
    return new PEGThetaNode(Decider, computeInputs(BB, true));
  } else {
    SmallSet<const BasicBlock *, 4> In;
    for (auto PredBB : predecessors(BB)) {
      In.insert(PredBB);
    }
    PEGNode *Decider =
        makeDecideNode(BB, In, makeConstLoopSet(LI.getLoopFor(BB)));
    return Decider;
  }
}

PEGFunction *GraphRewrite::createAPEG(const Function &F) {
  PEGFunction *PEGF = new PEGFunction(F);
  for (const BasicBlock &BB : F) {
    DEBUG(dbgs() << "running on: " << BB.getName() << "\n");
    PEGBasicBlock *PEGBB = PEGBasicBlock::createAPEG(PEGF, &BB);
    BBMap[&BB] = PEGBB;
    CondMap[&BB] = new PEGConditionNode(PEGBB);
  };

  for (auto It : BBMap) {
    PEGNode *Child = computeInputs(It.first);
    if (Child)
      It.second->setChild(Child);
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
    WriteGraph(File, (const PEGFunction *)&F);
  else
    errs() << "  error opening file for writing!";
  errs() << "\n";
}

bool GraphRewrite::run(Function &F) {
  PEGFunction *PEGF = createAPEG(F);

  if (DotPEG) {
    writePEGToDotFile(*PEGF);
  }
  outs() << *PEGF << "\n";
  return false;
}
//===----------------------------------------------------------------------===//
// GraphRewritePass
//===----------------------------------------------------------------------===//

PreservedAnalyses llvm::GraphRewritePass::run(Function &F,
                                              FunctionAnalysisManager &AM) {
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
