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

namespace llvm {

class PEGBasicBlock;
class PEGNode;
class PEGFunction;
class Loop;

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
// Provide graph traits for tranversing call graphs using standard graph
// traversals.
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

// Structured very similar to machineBB;
class PEGBasicBlock : public PEGNode {
  // Stores if this PEG is still an A-PEG.
  bool APEG;
  PEGFunction *Parent;
  const BasicBlock *BB;
  std::vector<PEGBasicBlock *> Predecessors;

public:
  explicit PEGBasicBlock(PEGFunction *Parent, const BasicBlock *BB);
  PEGBasicBlock(const PEGBasicBlock &other) = delete;

  static PEGBasicBlock *createAPEG(PEGFunction *Parent, const BasicBlock *BB) {
    return new PEGBasicBlock(Parent, BB);
  }
  void print(raw_ostream &os) const override;

  static bool classof(const PEGNode *N) {
    return N->getKind() == PEGNode::PEGNK_BB;
  }
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
  const Function &Fn;

  // List of machine basic blocks in function
  using NodesListType = ilist<PEGNode>;
  NodesListType Nodes;

public:
  PEGFunction(const Function &Fn) : Fn(Fn){};
  PEGFunction(const PEGFunction &) = delete;
  PEGFunction &operator=(const PEGFunction &) = delete;
  ~PEGFunction();

  void print(raw_ostream &os) const;
  friend raw_ostream &operator<<(raw_ostream &os, const PEGFunction &F);
  /// getFunction - Return the LLVM function that this machine code represents
  const Function &getFunction() const { return Fn; }
  using iterator = NodesListType::iterator;
  using const_iterator = NodesListType::const_iterator;

  const NodesListType &getNodesList() const { return Nodes; }
  NodesListType &getNodesList() { return Nodes; }

  iterator begin() { return Nodes.begin(); }
  const_iterator begin() const { return Nodes.begin(); }
  iterator end() { return Nodes.end(); }
  const_iterator end() const { return Nodes.end(); }

  size_t size() const { return Nodes.size(); }
  bool empty() const { return Nodes.empty(); }
  const PEGNode &front() const { return Nodes.front(); }
  PEGNode &front() { return Nodes.front(); }
  const PEGNode &back() const { return Nodes.back(); }
  PEGNode &back() { return Nodes.back(); }

  /// getName - Return the name of the corresponding LLVM function.
  StringRef getName() const { return Fn.getName(); }
};

template <>
struct GraphTraits<const PEGFunction *> : public GraphTraits<const PEGNode *> {
  using NodeRef = const PEGNode *;
  static NodeRef getEntryNode(const PEGFunction *F) { return &F->front(); }

  // nodes_iterator/begin/end - Allow iteration over all nodes in the graph
  using nodes_iterator = pointer_iterator<PEGFunction::const_iterator>;

  static nodes_iterator nodes_begin(const PEGFunction *F) {
    return nodes_iterator(F->begin());
  }

  static nodes_iterator nodes_end(const PEGFunction *F) {
    return nodes_iterator(F->end());
  }

  static size_t size(const PEGFunction *F) { return F->size(); }
};

template <> struct GraphTraits<PEGFunction *> : public GraphTraits<PEGNode *> {
  // nodes_iterator/begin/end - Allow iteration over all nodes in the graph
  using nodes_iterator = PEGFunction::iterator;

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
