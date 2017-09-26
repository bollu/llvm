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
#include "llvm/ADT/BreadthFirstIterator.h"
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
#include "llvm/Support/GenericDomTreeConstruction.h"
#include "llvm/Support/GraphWriter.h"
#include "llvm/Transforms/GraphRewrite/PEGDominators.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Utils/Local.h"
#include <climits>

#define DEBUG_TYPE "graphrewrite"
using namespace llvm;

static cl::opt<bool>
    DotPEG("dot-peg", cl::init(false), cl::Hidden, cl::ZeroOrMore,
           cl::desc("write PEG from -graphrewrite to a dot file"));

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

bool PEGBasicBlock::isLoopHeader() const { return !IsVirtualForwardNode && LI.isLoopHeader(BB); }
void PEGBasicBlock::print(raw_ostream &os) const {
  os << "pegbb-" << this->getName() << "\n";
  if (Children.size())
    for (const PEGNode *Child : Children) {
      errs() << "\t-" << *Child << "\n";
    }
}

void PEGBasicBlock::printAsOperand(raw_ostream &OS, bool PrintType) const {
  OS << this->getName();
}

static std::string
makePEGBasicBlockName(const BasicBlock *BB,
                      const PEGBasicBlock *VirtualForwardNode,
                      const bool IsVirtualForwardNode) {
  std::string name = BB->getName();
  if (IsVirtualForwardNode)
    name += "-virtual";
  if (VirtualForwardNode)
      name += "-concrete";
  return name;
}
PEGBasicBlock::PEGBasicBlock(const LoopInfo &LI, PEGFunction *Parent,
                             const BasicBlock *BB, const Loop *SurroundingLoop,
                             bool isEntry,
                             const PEGBasicBlock *VirtualForwardNode,
                             bool IsVirtualForwardNode)
    : PEGNode(PEGNodeKind::PEGNK_BB, Parent,
              makePEGBasicBlockName(BB, VirtualForwardNode, IsVirtualForwardNode)),
      LI(LI), IsEntry(isEntry), APEG(true), Parent(Parent), BB(BB),
      SurroundingLoop(SurroundingLoop), VirtualForwardNode(VirtualForwardNode),
      IsVirtualForwardNode(IsVirtualForwardNode) {

 // IsVirtualForwardNode => !VirtualForwardNode
  assert(!IsVirtualForwardNode || !VirtualForwardNode);
  if (VirtualForwardNode) {
    assert(VirtualForwardNode->IsVirtualForwardNode &&
           "node that is supposed to be virtual forward node is not marked as "
           "such.");
  };
  Parent->getBasicBlocksList().push_back(this);
};

ConstLoopSet PEGBasicBlock::getLoopSet() const {
  return makeConstLoopSet(getSurroundingLoop());
}

//===----------------------------------------------------------------------===//
// PEGFunction
//===----------------------------------------------------------------------===//

void PEGFunction::print(raw_ostream &os) const {
  /*
for (const PEGNode &N : Nodes) {
  errs() << N << "\n\n";
}
*/
  errs() << "fn";
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

  DOTGraphTraits(bool isSimple = false) : DefaultDOTGraphTraits(true) {}

  static std::string getGraphName(const PEGFunction *F) {
    return "PEG for '" + F->getName().str() + "' function";
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

// using BBSet = SmallSet<const BasicBlock *, 4>;
class BBEdge {
private:
  const PEGBasicBlock *Source;
  const PEGBasicBlock *Dest;

  BBEdge(const PEGBasicBlock *Source, const PEGBasicBlock *Dest)
      : Source(Source), Dest(Dest){};

public:
  Optional<const PEGBasicBlock *> getSource() const {
    if (Source)
      return Optional<const PEGBasicBlock *>(Source);
    return Optional<const PEGBasicBlock *>(None);
  }

  const PEGBasicBlock *getDest() const { return Dest; }

  static BBEdge create(const PEGBasicBlock *Source, const PEGBasicBlock *Dest) {
    assert(Source);
    assert(Dest);

    return BBEdge(Source, Dest);
  }

  // Make an edge with no source but only dest into given edge.
  // Use with great caution.
  static BBEdge makeEntryEdge(const PEGBasicBlock *Dest) {
    assert(Dest);
    return BBEdge(nullptr, Dest);
  }

  bool operator==(const BBEdge &other) const {
    return Dest == other.Dest && Source == other.Source;
  }

  bool operator<(const BBEdge &other) const {
    return Dest < other.Dest || Source < other.Source;
  }

  friend raw_ostream &operator<<(raw_ostream &, const BBEdge &E);
};

raw_ostream &operator<<(raw_ostream &os, const BBEdge &E) {
  if (!E.getSource())
    os << "nullptr";
  else
    os << E.getSource().getValue()->getName();

  os << " --> ";

  assert(E.getDest());
  os << E.getDest()->getName();

  return os;
}
using BBEdgeSet = std::set<BBEdge>;

using ValueFn = std::function<const PEGNode *(const BBEdge &)>;

// -----
// Pass code
// Modeled after EarlyCSE.
class GraphRewrite {
public:
  GraphRewrite(DominatorTree &DT, LoopInfo &LI, ScalarEvolution &SE)
      : DT(DT), LI(LI), SE(SE), RootEdge(None), F(nullptr) {}

  bool run(Function &F);

private:
  DominatorTree &DT;
  PEGDominatorTree PEGDT;
  LoopInfo &LI;
  ScalarEvolution &SE;
  Optional<BBEdge> RootEdge;
  Function *F;

  // maps basic blocks to PEG blocks. does not contain virtual PEG blocks.
  // Please don't touch this unless necesary, it does not have a const
  // qualifier.
  std::map<const BasicBlock *, PEGBasicBlock *> BBMap;
  std::map<const PEGBasicBlock *, PEGConditionNode *> CondMap;
  std::map<PEGNode *, PEGNode *> LoopHeaderToVirtualPEGNode;

  PEGFunction *createAPEG(const Function &F);
  PEGNode *computeInputs(const PEGBasicBlock *BB) const;

  BBEdgeSet computeBreakEdges(const Loop *L) const;
  PEGNode *makeBreakCondition(const BasicBlock *Cur, const Loop *L,
                              BBEdgeSet BreakBBs, ConstLoopSet Outer) const;
  PEGNode *computeInputsFromInsideLoop(const BasicBlock *BB) const;
  PEGNode *makeDecideNode(BBEdge Source, BBEdgeSet &In, ValueFn VF,
                          ConstLoopSet Outer) const;

  BBEdgeSet getInEdges(const PEGBasicBlock *BB) const {
    BBEdgeSet Edges;
    if (BB->isEntry()) {
      Edges.insert(*RootEdge);
      return Edges;
    };

    for (const PEGBasicBlock *Pred : BB->predecessors()) {
      Edges.insert(BBEdge::create(Pred, BB));
    }
    return Edges;
  };

  static ValueFn createValueFnGetEdgeSource(const BBEdge &RootEdge) {
    return [&](const BBEdge &E) -> const PEGNode * {
      const PEGBasicBlock *BB = nullptr;
      if (E == RootEdge)
        BB = E.getDest();
      else
        BB = *E.getSource();

      assert(BB && "expected source BB to find corresponding PEGNode for");
      return BB;
    };
  }

  PEGConditionNode *getConditionNodeFor(const PEGBasicBlock *BB) const {
    auto It = CondMap.find(BB);
    if (It == CondMap.end())
      report_fatal_error("expected Cond for BB: " + BB->getName());
    return It->second;
  }
};

const BasicBlock *useToSourceBB(const Use &U) {
  return cast<BasicBlock>(U.get());
}

BasicBlock *useToSourceBB(Use &U) { return cast<BasicBlock>(U.get()); }

const PEGBasicBlock *findCommonDominator(const PEGDominatorTree &PEGDT,
                                         BBEdgeSet &In) {
  assert(In.size() > 0);
  const PEGBasicBlock *FinalDominator = nullptr;
  for (const BBEdge &E : In) {
    if (!FinalDominator) {
      FinalDominator = *E.getSource();
      continue;
    }
    FinalDominator =
        PEGDT.findNearestCommonDominator(FinalDominator, *E.getSource());
  }
  return FinalDominator;
}

template <typename T, typename F>
std::set<T> filterSet(const std::set<T> &In, F Predicate) {
  // std::function<bool(const T &)> &Predicate) {
  std::set<T> S;
  for (auto V : In) {
    if (Predicate(V))
      S.insert(V);
  }
  return S;
}

// Return the successor if the true, false branch are taken.
// I know, this is WTF, and will fail on switch. sue me :(
std::pair<const PEGBasicBlock *, const PEGBasicBlock *>
getTrueFalseSuccessors(const PEGBasicBlock *BB) {
  errs() << __PRETTY_FUNCTION__ << "\n\tBB: " << *BB << "\n";
  for (auto Succ : BB->successors())
    errs() << *Succ << "\n";

  assert(!BB->getUniqueSuccessor());
  // if (const BasicBlock *Succ = BB->getSingleSuccessor())
  //   return std::make_pair(Succ, Succ);

  const TerminatorInst *TI = BB->getTerminator();
  const BranchInst *BI = cast<BranchInst>(TI);
  assert(BI->isConditional() && "should not have reached here, should have "
                                "returned at getSingleSuccessor");
  return BB->getTrueFalseSuccessors();
  // return std::make_pair(BI->getSuccessor(0), BI->getSuccessor(1));
  // return std::make_pair(BB->getTrueSuccessor(), BB->getFalseSuccessor());
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

BBEdgeSet GraphRewrite::computeBreakEdges(const Loop *L) const {
  BBEdgeSet Edges;

  SmallVector<BasicBlock *, 4> ExitingBBVec;
  L->getExitBlocks(ExitingBBVec);

  for (auto BB : ExitingBBVec) {
    const PEGBasicBlock *PEGBB = BBMap.find(BB)->second;
    const PEGBasicBlock *Header = BBMap.find(L->getHeader())->second;
    Edges.insert(BBEdge::create(PEGBB, Header));
  }

  return Edges;
}

void printConstLoopSet(ConstLoopSet &LS) {
  errs() << "LS(" << LS.size() << ")\n";
  for (const Loop *L : LS) {
    L->dump();
  }
}

bool isReachable(const PEGBasicBlock *From, const PEGBasicBlock *To,
                 const PEGDominatorTree &DT) {
  if (DT.dominates(From, To))
    return true;

  std::set<const PEGBasicBlock *> Visited(bf_begin(From), bf_end(From));
  return Visited.count(To);
}

PEGNode *GraphRewrite::makeDecideNode(BBEdge Source, BBEdgeSet &In, ValueFn VF,
                                      ConstLoopSet Outer) const {

  errs() << "===\n";
  errs() << __PRETTY_FUNCTION__ << "\n";
  errs() << "Source: " << Source << "\n";
  errs() << "\n\n";
  errs() << "in:\n";
  for (const BBEdge &E : In)
    errs() << E << "\n";
  errs() << "\n\n";

  errs() << "Outer: ";
  printConstLoopSet(Outer);
  errs() << "\n\n";

  const PEGBasicBlock *CommonDom = findCommonDominator(PEGDT, In);
  errs() << "CommonDom: " << CommonDom->getName() << "\n";

  const Loop *CommonDomLoop = CommonDom->getSurroundingLoop();
  ConstLoopSet CommonDomLoopSet = makeConstLoopSet(CommonDomLoop);
  errs() << "CommonDomLoopSet: ";
  printConstLoopSet(CommonDomLoopSet);

  if (isSubset(CommonDomLoopSet, Outer)) {
    errs() << "isSubset(CommonDomLoopSet, Outer)) == T\n";
    auto getCommonMappedPEGNode = [&]() -> PEGNode * {
      const PEGNode *CommonNode = nullptr;
      for (const BBEdge &E : In) {
        errs() << "VF(" << E << ") = " << VF(E)->getName() << "\n";
        if (!CommonNode) {
          CommonNode = VF(E);
          continue;
        }
        if (CommonNode != VF(E))
          return nullptr;
      }
      // eugh.
      return const_cast<PEGNode *>(CommonNode);
    };
    // Perform optimization when all nodes are mapped to the same thing.
    if (PEGNode *Common = getCommonMappedPEGNode()) {
      errs() << "Common: " << Common->getName() << "\n";
      return Common;
    }

    assert(In.size() > 1);

    const PEGBasicBlock *TrueBB, *FalseBB;

    std::tie(TrueBB, FalseBB) = getTrueFalseSuccessors(CommonDom);
    errs() << "TrueBB: " << TrueBB->getName() << "\n";
    errs() << "FalseBB: " << FalseBB->getName() << "\n";
    assert(TrueBB && "TrueBB uninitialized");
    assert(FalseBB && "FalseBB uninitialized");

    BBEdgeSet TrueEdges = filterSet(In, [&](const BBEdge &E) -> bool {
      return isReachable(*E.getSource(), TrueBB, PEGDT);
    });

    errs() << "TrueEdges: " << TrueEdges.size() << "\n";
    ;
    for (auto E : TrueEdges) {
      errs() << "\t-" << E << "\n";
    }

    PEGNode *TrueNode = makeDecideNode(Source, TrueEdges, VF, Outer);
    errs() << "True: " << *TrueNode << "\n";

    BBEdgeSet FalseEdges = filterSet(In, [&](const BBEdge &E) -> bool {
      return isReachable(*E.getSource(), FalseBB, PEGDT);
    });

    PEGNode *FalseNode = makeDecideNode(Source, FalseEdges, VF, Outer);
    errs() << "False: " << *FalseNode << "\n";

    PEGConditionNode *Condition = getConditionNodeFor(CommonDom);
    return new PEGPhiNode(Condition, TrueNode, FalseNode);
  } else {
    const Loop *LNew = getOutermostLoopNotInLoop(CommonDomLoopSet, Outer);

    Outer.insert(LNew);
    PEGNode *Val = makeDecideNode(Source, In, VF, Outer);
    assert(false);
    /*
    auto BreakBBs = computeBreakEdges(LNew);
    PEGNode *Break = makeBreakCondition(useToSourceBB(Source), LNew, BreakBBs,
    Outer); return new PEGEvalNode(LNew, Val, new PEGPassNode(LNew, Break));
    */
  }
}

PEGNode *GraphRewrite::makeBreakCondition(const BasicBlock *Cur, const Loop *L,
                                          BBEdgeSet BreakBBs,
                                          ConstLoopSet Outer) const {

  assert(false);
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

PEGNode *GraphRewrite::computeInputs(const PEGBasicBlock *BB) const {
  assert(BB);
  assert(!BB->isEntry());
  errs() << "====\n";
  errs() << __PRETTY_FUNCTION__ << "\nBB: " << BB->getName() << "\n";

  // When we are looking for stuff inside the loop, we are in a "virtual" node
  // that is not a loop header
  if (BB->isLoopHeader()) {
    const Loop *L = BB->getSurroundingLoop();

    BBEdgeSet In = getInEdges(BB);

    {
      errs() << __PRETTY_FUNCTION__ << ":" << __LINE__ << "\n";
      errs() << "* BB:" << BB->getName() << "\n";
      errs() << "* In:\n";
      for (const BBEdge &I : In)
        errs() << "\t-" << I << "\n";
    };
    PEGNode *Decider = makeDecideNode(
        *RootEdge, In, createValueFnGetEdgeSource(*RootEdge), BB->getLoopSet());
    errs() << "* Decider: " << Decider->getName() << "\n";
    errs() << "* VirtualForwardNode:" << BB->getVirtualForwardNode() << "\n";
    return new PEGThetaNode(Decider,
                            computeInputs(BB->getVirtualForwardNode()));
  } else {

    BBEdgeSet In = getInEdges(BB);
    PEGNode *Decider = makeDecideNode(
        *RootEdge, In, createValueFnGetEdgeSource(*RootEdge), BB->getLoopSet());
    errs() << "* BB: " << BB->getName() << " | Decider: " << Decider->getName()
           << "\n";
    return Decider;
  }
}

PEGFunction *GraphRewrite::createAPEG(const Function &F) {
  std::map<const PEGBasicBlock *, PEGBasicBlock *> VirtualForwardMap;
  PEGFunction *PEGF = new PEGFunction(F);
  for (const BasicBlock &BB : F) {
    const bool IsEntry = &BB == &F.getEntryBlock();
    const Loop *L = LI.getLoopFor(&BB);

    PEGBasicBlock *VirtualForwardNode = nullptr;
    if (LI.isLoopHeader(&BB)) {
      VirtualForwardNode = new PEGBasicBlock(LI, PEGF, &BB,
                                             /* SurroundingLoop = */ nullptr,
                                             /*IsEntry = */ false,
                                             /* VirtualForwardNode = */ nullptr,
                                             /*IsVirtualForwardNode = */ true);
    };

    const bool IsVirtualForwardNode = false;
    PEGBasicBlock *PEGBB = new PEGBasicBlock(
        LI, PEGF, &BB, L, IsEntry, VirtualForwardNode, IsVirtualForwardNode);
    if (VirtualForwardNode)
      errs() << "Creating virtual forward node for: " << PEGBB << "| "
             << PEGBB->getName() << "| Node: " << VirtualForwardNode << " | "
             << VirtualForwardNode->getName();
    VirtualForwardMap[PEGBB] = VirtualForwardNode;
    BBMap[&BB] = PEGBB;
    CondMap[PEGBB] = new PEGConditionNode(PEGBB);

    if (IsEntry)
      RootEdge = BBEdge::makeEntryEdge(PEGBB);
  };

  for (auto It : BBMap) {
    // DOUBT: [Handling of entry block]
    // How should ComputeInputs handle the entry block?
    const BasicBlock *BB = It.first;
    PEGBasicBlock *PEGBB = It.second;
    for (auto PredBB : predecessors(BB)) {
      PEGBasicBlock *PredPEGBB = BBMap.find(PredBB)->second;
      // We need to create edges carefully if this is a loop header.
      if (LI.isLoopHeader(BB)) {
        // Loop latches are forwarded to the virtual node.
        if (isLoopLatch(LI, PEGBB->getSurroundingLoop(), PredBB)) {
          // We don't expose a mutable getVirtualForwardNode on purpose.
          // we want our data structures to be immutable as much as possible
          // after construction. #haskell.
          PEGBasicBlock *VirtualForwardPEGBB =
              VirtualForwardMap.find(PEGBB)->second;
          PEGBasicBlock::addEdge(PredPEGBB, VirtualForwardPEGBB);
        } else {
          // non loop latches are attached to the real node.
          PEGBasicBlock::addEdge(PredPEGBB, PEGBB);
        }
      }
      // not a loop header.
      else {
        PEGBasicBlock::addEdge(PredPEGBB, PEGBB);
      }
    }

    // Once we have added the edge, recalcuate the domtree.
    PEGDT.recalculate(*PEGF);

    if (!PEGBB->isEntry()) {
      PEGNode *Child = computeInputs(PEGBB);
      if (Child)
        It.second->setChild(Child);
      else
        errs() << *It.second << "can't have a child.\n";
    }
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
  this->F = &F;

  PEGFunction *PEGF = createAPEG(F);

  if (DotPEG) {
    writePEGToDotFile(*PEGF);
  }
  // outs() << *PEGF << "\n";
  RootEdge = None;
  this->F = nullptr;
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
