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

#include "llvm/ADT/GraphTraits.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Transforms/InstCombine/InstCombineWorklist.h"
#include <set>

namespace llvm {

class PEGBasicBlock;
class PEGNode;
class PEGFunction;
class Loop;
class LoopInfo;

class PEGNode : public ilist_node_with_parent<PEGNode, PEGFunction> {
public:
  enum PEGNodeKind {
    PEGNK_Cond,
    PEGNK_Phi,
    PEGNK_Theta,
    PEGNK_BB,
    PEGNK_Eval,
    PEGNK_Pass
  };
  using ChildrenType = SmallVector<PEGNode *, 2>;
  using iterator = ChildrenType::iterator;
  using const_iterator = ChildrenType::const_iterator;

  iterator begin() { return Children.begin(); }
  const_iterator begin() const { return Children.begin(); }

  iterator end() { return Children.begin(); }
  const_iterator end() const { return Children.end(); }

  int size() const { return Children.size(); }

  void setChild(PEGNode *Child) { Children = {Child}; }

  virtual void print(raw_ostream &os) const {
    report_fatal_error(
        "expect children to implement this. Not marked as pure virtual because"
        " it fucks with ilist of BB");
  }

  StringRef getName() const { return Name; }

  PEGNodeKind getKind() const { return Kind; }
  friend raw_ostream &operator<<(raw_ostream &os, const PEGNode &N);
  virtual ~PEGNode(){};

  PEGFunction *getParent() { return Parent; }

protected:
  PEGNode(PEGNodeKind Kind, PEGFunction *Parent, const StringRef Name);
  ChildrenType Children;

private:
  PEGFunction *Parent;
  const PEGNodeKind Kind;
  std::string Name;
};

using LoopSet = std::set<Loop *>;
using ConstLoopSet = std::set<const Loop *>;

// Structured very similar to machineBB;
class PEGBasicBlock
    : public PEGNode,
      public ilist_node_with_parent<PEGBasicBlock, PEGFunction> {
  // Stores if this PEG is still an A-PEG.
  bool IsEntry;
  bool APEG;
  const LoopInfo &LI;

  PEGFunction *Parent;
  const BasicBlock *BB;
  const Loop *SurroundingLoop;
  std::vector<PEGBasicBlock *> Predecessors;
  std::vector<PEGBasicBlock *> Successors;
  const PEGBasicBlock *VirtualForwardNode;

  void addPredecessor(PEGBasicBlock *Pred) {
    this->Predecessors.push_back(Pred);
  }

  void addSuccessor(PEGBasicBlock *Succ) { this->Successors.push_back(Succ); }

public:
  explicit PEGBasicBlock(const LoopInfo &LI, PEGFunction *Parent,
                         const BasicBlock *BB, const Loop *SurroundingLoop,
                         bool isEntry, const PEGBasicBlock *VirtualForwardNode);
  PEGBasicBlock(const PEGBasicBlock &other) = delete;

  const TerminatorInst *getTerminator() const { return BB->getTerminator(); };

  const PEGBasicBlock *getUniqueSuccessor() const {
    if (Successors.size() == 1)
      return Successors[0];
    return nullptr;
  }
  std::pair<const PEGBasicBlock *, const PEGBasicBlock *>
  getTrueFalseSuccessors() const {
    assert(Successors.size() == 2);
    return std::make_pair(Successors[0], Successors[1]);
  }

  bool isEntry() const { return IsEntry; }
  void print(raw_ostream &os) const override;
  // Printing method used by genericdomtree.
  void printAsOperand(raw_ostream &OS, bool PrintType = true) const;

  static bool classof(const PEGNode *N) {
    return N->getKind() == PEGNode::PEGNK_BB;
  }


  const PEGBasicBlock *getVirtualForwardNode() const {
    return VirtualForwardNode;
  }

  const Loop *getSurroundingLoop() const { return SurroundingLoop; }

  bool isLoopHeader() const;

  ConstLoopSet getLoopSet() const;

  using iterator = std::vector<PEGBasicBlock *>::iterator;
  using const_iterator = std::vector<const PEGBasicBlock *>::const_iterator;

  iterator begin_succ() { return this->Successors.begin(); }
  iterator end_succ() { return this->Successors.end(); }
  const_iterator begin_succ() const { return this->Successors.begin(); }
  const_iterator end_succ() const { return this->Successors.end(); }
  size_t size_succ() const { return Successors.size(); }
  iterator_range<iterator> successors() {
    return make_range(begin_succ(), end_succ());
  }
  iterator_range<const_iterator> successors() const {
    return make_range(begin_succ(), end_succ());
  }

  iterator begin_pred() { return this->Predecessors.begin(); }
  iterator end_pred() { return this->Predecessors.end(); }
  const_iterator begin_pred() const { return this->Predecessors.begin(); }
  const_iterator end_pred() const { return this->Predecessors.end(); }
  size_t size_pred() const { return Predecessors.size(); }

  iterator_range<iterator> predecessors() {
    return iterator_range<iterator>(begin_pred(), end_pred());
  }

  iterator_range<const_iterator> predecessors() const {
    return iterator_range<const_iterator>(begin_pred(), end_pred());
  }

  static void addEdge(PEGBasicBlock *From, PEGBasicBlock *To) {
    To->addPredecessor(From);
    From->addSuccessor(To);
  }
};

template <> struct GraphTraits<PEGBasicBlock *> {
  using NodeRef = PEGBasicBlock *;
  using ChildIteratorType = PEGBasicBlock::iterator;

  static NodeRef getEntryNode(NodeRef N) { return N; }

  static ChildIteratorType child_begin(NodeRef N) { return N->begin_succ(); }
  static ChildIteratorType child_end(NodeRef N) { return N->end_succ(); }
  static unsigned size(NodeRef N) { return N->size_succ(); }
};

template <> struct GraphTraits<const PEGBasicBlock *> {
  using NodeRef = const PEGBasicBlock *;
  using ChildIteratorType = PEGBasicBlock::const_iterator;

  static NodeRef getEntryNode(NodeRef N) { return N; }

  static ChildIteratorType child_begin(NodeRef N) { return N->begin_succ(); }
  static ChildIteratorType child_end(NodeRef N) { return N->end_succ(); }
  static unsigned size(NodeRef N) { return N->size_succ(); }
};

template <> struct GraphTraits<Inverse<PEGBasicBlock *>> {
  using NodeRef = PEGBasicBlock *;
  using ChildIteratorType = PEGBasicBlock::iterator;

  static NodeRef getEntryNode(NodeRef G) { return G; }

  static ChildIteratorType child_begin(NodeRef N) { return N->begin_pred(); }
  static ChildIteratorType child_end(NodeRef N) { return N->end_pred(); }
  static unsigned size(NodeRef N) { return N->size_pred(); }
};

template <> struct GraphTraits<Inverse<const PEGBasicBlock *>> {
  using NodeRef = const PEGBasicBlock *;
  using ChildIteratorType = PEGBasicBlock::const_iterator;

  static NodeRef getEntryNode(NodeRef G) { return G; }

  static ChildIteratorType child_begin(NodeRef N) { return N->begin_pred(); }
  static ChildIteratorType child_end(NodeRef N) { return N->end_pred(); }
  static unsigned size(NodeRef N) { return N->size_pred(); }
};

class PEGConditionNode : public PEGNode {
private:
  PEGBasicBlock *PEGBB;

  static std::string makeName(const PEGBasicBlock *BB) {
    return ("cond(" + BB->getName() + ")").str();
  }

public:
  void print(raw_ostream &os) const override;
  PEGConditionNode(PEGBasicBlock *PEGBB)
      : PEGNode(PEGNK_Cond, PEGBB->getParent(), makeName(PEGBB)), PEGBB(PEGBB) {
    assert(PEGBB);
    Children.clear();
    Children.push_back(PEGBB);
  };
  static bool classof(const PEGNode *N) {
    return N->getKind() == PEGNode::PEGNK_Cond;
  }
};

class PEGPhiNode : public PEGNode {
  PEGNode *True, *False;
  PEGConditionNode *Cond;

  static std::string makeName(const PEGNode *Cond, const PEGNode *True,
                              const PEGNode *False) {
    return ("phi(" + Cond->getName() + ", " + True->getName() + ", " +
            False->getName() + ")")
        .str();
  }

public:
  void print(raw_ostream &os) const override;
  PEGPhiNode(PEGConditionNode *Cond, PEGNode *True, PEGNode *False)
      : PEGNode(PEGNK_Phi, Cond->getParent(), makeName(Cond, True, False)),
        True(True), False(False), Cond(Cond) {
    assert(True);
    assert(False);
    assert(Cond);
    Children = {Cond, True, False};
  }

  static bool classof(const PEGNode *N) {
    return N->getKind() == PEGNode::PEGNK_Phi;
  }
};

class PEGPassNode : public PEGNode {
  const Loop *L;
  PEGNode *Cond;
  static std::string makeName(PEGNode *Cond) {
    return ("pass(" + Cond->getName() + ")").str();
  }

public:
  PEGPassNode(const Loop *L, PEGNode *Cond)
      : PEGNode(PEGNK_Pass, Cond->getParent(), makeName(Cond)), L(L) {
    Children = {Cond};
  }
  void print(raw_ostream &os) const override;

  static bool classof(const PEGNode *N) {
    return N->getKind() == PEGNode::PEGNK_Pass;
  }
};

class PEGEvalNode : public PEGNode {
  const Loop *L;
  PEGNode *Value;
  PEGPassNode *Pass;

  static std::string makeName(PEGNode *Value, PEGPassNode *Pass) {
    return ("eval(" + Value->getName() + ", " + Pass->getName() + ")").str();
  }

public:
  PEGEvalNode(const Loop *L, PEGNode *Value, PEGPassNode *Pass)
      : PEGNode(PEGNK_Eval, Value->getParent(), makeName(Value, Pass)),
        Value(Value), Pass(Pass), L(L) {
    Children = {Value, Pass};
  };
  void print(raw_ostream &os) const override;

  static bool classof(const PEGNode *N) {
    return N->getKind() == PEGNode::PEGNK_Eval;
  }
};

class PEGThetaNode : public PEGNode {
private:
  PEGNode *Base;
  PEGNode *Recur;

  static std::string makeName(PEGNode *Base, PEGNode *Recur) {
    return ("theta(" + Base->getName() + ", " + Recur->getName() + ")").str();
  }

public:
  PEGThetaNode(PEGNode *Base, PEGNode *Recur)
      : PEGNode(PEGNK_Theta, Base->getParent(), makeName(Base, Recur)),
        Base(Base), Recur(Recur) {
    Children = {Base, Recur};
  };

  void print(raw_ostream &os) const override;

  static bool classof(const PEGNode *N) {
    return N->getKind() == PEGNode::PEGNK_Theta;
  }
};

class PEGFunction {
public:
  using NodesListType = ilist<PEGNode>;
  using BasicBlockListType = ilist<PEGBasicBlock>;

private:
  const Function &Fn;
  // List of machine basic blocks in function
  NodesListType Nodes;
  BasicBlockListType BasicBlocks;

public:
  PEGFunction(const Function &Fn) : Fn(Fn){};

  using ChildIteratorType = BasicBlockListType::iterator;
  using iterator = BasicBlockListType::iterator;
  using const_iterator = BasicBlockListType::const_iterator;

  const BasicBlockListType &getBasicBlocksList() const { return BasicBlocks; }
  BasicBlockListType &getBasicBlocksList() { return BasicBlocks; }

  iterator begin() { return BasicBlocks.begin(); }
  const_iterator begin() const { return BasicBlocks.begin(); }

  iterator end() { return BasicBlocks.end(); }
  const_iterator end() const { return BasicBlocks.end(); }

  size_t size() const { return BasicBlocks.size(); }
  bool empty() const { return BasicBlocks.empty(); }

  const PEGBasicBlock &front() const { return BasicBlocks.front(); }
  PEGBasicBlock &front() { return BasicBlocks.front(); }

  const PEGBasicBlock &back() const { return BasicBlocks.back(); }
  PEGBasicBlock &back() { return BasicBlocks.back(); }

  using node_iterator = NodesListType::iterator;
  using const_node_iterator = NodesListType::const_iterator;

  const NodesListType &getNodesList() const { return Nodes; }
  NodesListType &getNodesList() { return Nodes; }

  node_iterator begin_nodes() { return Nodes.begin(); }
  const_node_iterator begin_nodes() const { return Nodes.begin(); }

  node_iterator end_nodes() { return Nodes.end(); }
  const_node_iterator end_nodes() const { return Nodes.end(); }

  size_t size_nodes() const { return Nodes.size(); }
  bool empty_nodes() const { return Nodes.empty(); }

  const PEGNode &front_nodes() const { return Nodes.front(); }
  PEGNode &front_nodes() { return Nodes.front(); }

  const PEGNode &back_nodes() const { return Nodes.back(); }
  PEGNode &back_nodes() { return Nodes.back(); }

  /// getName - Return the name of the corresponding LLVM function.
  StringRef getName() const { return Fn.getName(); }

  void print(raw_ostream &os) const;
  friend raw_ostream &operator<<(raw_ostream &os, const PEGFunction &F);
};

// Provide graph traits for tranversing call graphs using standard graph
// traversals.
/*
template <> struct GraphTraits<const PEGNode *> {
  using NodeRef = const PEGNode *;
  using ChildIteratorType = PEGNode::const_iterator;

  static NodeRef getEntryNode(NodeRef N) { return N; }

  static ChildIteratorType child_begin(NodeRef N) { return N->begin(); }

  static ChildIteratorType child_end(NodeRef N) { return N->end(); }
  static unsigned size(NodeRef N) { return N->size(); }
};

template <> struct GraphTraits<PEGNode *> {
  using NodeRef = PEGNode *;
  using ChildIteratorType = PEGNode::iterator;

  static NodeRef getEntryNode(NodeRef N) { return N; }

  static ChildIteratorType child_begin(NodeRef N) { return N->begin(); }

  static ChildIteratorType child_end(NodeRef N) { return N->end(); }
  static unsigned size(NodeRef N) { return N->size(); }
};

*/

/*

template <>
struct GraphTraits<const PEGFunction *> : public GraphTraits<const PEGNode *> {
  using NodeRef = const PEGNode *;
  static NodeRef getEntryNode(const PEGFunction *F) { return &F->front_nodes();
}

  // nodes_iterator/begin/end - Allow iteration over all nodes in the graph
  using nodes_iterator = pointer_iterator<PEGFunction::const_node_iterator>;

  static nodes_iterator nodes_begin(const PEGFunction *F) {
    return nodes_iterator(F->begin_nodes());
  }

  static nodes_iterator nodes_end(const PEGFunction *F) {
    return nodes_iterator(F->end_nodes());
  }

  static size_t size(const PEGFunction *F) { return F->size_nodes(); }
};

template <> struct GraphTraits<PEGFunction *> : public GraphTraits<PEGNode *> {
  // nodes_iterator/begin/end - Allow iteration over all nodes in the graph
  using nodes_iterator = PEGFunction::node_iterator;

  static NodeRef getEntryNode(PEGFunction *F) { return &F->front_nodes(); }

  static nodes_iterator nodes_begin(PEGFunction *F) {
    return nodes_iterator(F->begin_nodes());
  }

  static nodes_iterator nodes_end(PEGFunction *F) {
    return nodes_iterator(F->end_nodes());
  }

  static size_t size(PEGFunction *F) { return F->size_nodes(); }
};
*/

template <>
struct GraphTraits<const PEGFunction *>
    : public GraphTraits<const PEGBasicBlock *> {

  // nodes_iterator/begin/end - Allow iteration over all nodes in the graph
  using nodes_iterator = pointer_iterator<PEGFunction::const_iterator>;

  static NodeRef getEntryNode(const PEGFunction *F) { return &F->front(); }

  static nodes_iterator nodes_begin(const PEGFunction *F) {
    return nodes_iterator(F->begin());
  }

  static nodes_iterator nodes_end(const PEGFunction *F) {
    return nodes_iterator(F->end());
  }

  static size_t size(const PEGFunction *F) { return F->size(); }
};

template <>
struct GraphTraits<PEGFunction *> : public GraphTraits<PEGBasicBlock *> {
  // nodes_iterator/begin/end - Allow iteration over all nodes in the graph
  using nodes_iterator = pointer_iterator<PEGFunction::iterator>;

  static NodeRef getEntryNode(PEGFunction *F) { return &F->front(); }

  static nodes_iterator nodes_begin(PEGFunction *F) {
    return nodes_iterator(F->begin());
  }

  static nodes_iterator nodes_end(PEGFunction *F) {
    return nodes_iterator(F->end());
  }

  static size_t size(PEGFunction *F) { return F->size(); }
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

  GraphRewriteLegacyPass() : FunctionPass(ID) {
    initializeGraphRewriteLegacyPassPass(*PassRegistry::getPassRegistry());
  }

  void getAnalysisUsage(AnalysisUsage &AU) const override;
  bool runOnFunction(Function &F) override;
};

Pass *createGraphRewriteLegacyPass();
} // namespace llvm

#endif
