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
#include "llvm/ADT/GraphTraits.h"



namespace llvm {

class PEGBasicBlock;
class PEGNode;
class PEGFunction;

class PEGNode {
        public:
        enum PEGNodeKind {
            PEGNK_Cond,
            PEGNK_Phi,
            PEGNK_BB
        };
        using ChildrenType = SmallVector<PEGNode *, 2>;
        using iterator = ChildrenType::iterator;
        using const_iterator = ChildrenType::const_iterator;

        iterator begin() { return Children.begin(); }
        const_iterator begin() const { return Children.begin(); }

        iterator end() { return Children.begin(); }
        const_iterator end() const { return Children.end(); }

        int size() const {
            return Children.size();
        }

        void setChild(PEGNode *Child) {
            Children = { Child };
        }

        virtual void print(raw_ostream &os) const {
            report_fatal_error("expect children to implement this. Not marked as pure virtual because"
                    " it fucks with ilist of BB");
        }

        virtual StringRef getName() const {
            report_fatal_error("expect children to implement this. Not marked as pure virtual because"
                    " it fucks with ilist of BB");
        }

		 PEGNodeKind getKind() const { return Kind; }
        friend raw_ostream &operator <<(raw_ostream &os, const PEGNode &N);
        protected:
        PEGNode(PEGNodeKind Kind) : Kind(Kind) {};
        virtual ~PEGNode() {};
        ChildrenType Children;

        private:
          const PEGNodeKind Kind;

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
class PEGBasicBlock : public PEGNode, public ilist_node_with_parent<PEGBasicBlock, PEGFunction> {
  // Stores if this PEG is still an A-PEG.
  bool APEG;
  PEGFunction *Parent;
  const BasicBlock *BB;
  std::vector<PEGBasicBlock *> Predecessors;

  explicit PEGBasicBlock(PEGFunction *Parent, PEGBasicBlock *InsertBefore, const BasicBlock *BB);

    public:
  // Intrusive list support
  PEGBasicBlock() : PEGNode(PEGNode::PEGNK_BB) {};
  PEGBasicBlock(const PEGBasicBlock &other) = delete;

  static PEGBasicBlock *createAPEG(PEGFunction *Parent, const BasicBlock *BB) {
      return new PEGBasicBlock(Parent, nullptr, BB);
  }
  void print(raw_ostream &os) const override;
  /// getName - Return the name of the corresponding LLVM basic block.
  StringRef getName() const override { return BB->getName(); }

  static bool classof(const PEGNode *N) {
      return N->getKind() == PEGNode::PEGNK_BB;
  }
};


class PEGConditionNode : public PEGNode {
    private:
        PEGBasicBlock *PEGBB;
    public:
        void print(raw_ostream &os) const override;
        PEGConditionNode(PEGBasicBlock *PEGBB) : PEGNode(PEGNK_Cond), PEGBB(PEGBB) {
            assert(PEGBB);
            Children.clear();
            Children.push_back(PEGBB);
        };
        StringRef getName() const override {
            std::string Str;
            raw_string_ostream OS(Str);
            OS << "cond- " << PEGBB->getName();
            return OS.str();
        }
        static bool classof(const PEGNode *N) {
            return N->getKind() == PEGNode::PEGNK_Cond;
        }

};

class PEGPhiNode : public PEGNode {
    PEGNode *True, *False;
    PEGConditionNode *Cond;

    public:
    void print(raw_ostream &os) const override;
    PEGPhiNode(PEGConditionNode *Cond, PEGNode *True, PEGNode *False) :
    PEGNode(PEGNK_Phi), True(True), False(False), Cond(Cond) {
        assert(True);
        assert(False);
        assert(Cond);
        Children = {Cond, True, False};
    }
    const PEGNode *trueNode() const { return True; }
    const PEGNode *falseNode() const { return False; }
    const PEGNode *condNode() const { return Cond; }
    StringRef getName() const override {
        std::string Str;
        raw_string_ostream OS(Str);
        OS << "phi " << Cond->getName() << " ? " << True->getName() << " : " << False->getName();
        return OS.str();
    }

    static bool classof(const PEGNode *N) {
        return N->getKind() == PEGNode::PEGNK_Phi;
    }
};


class PEGFunction {
  const Function &Fn;

  // List of machine basic blocks in function
  using BasicBlockListType = ilist<PEGBasicBlock>;
  BasicBlockListType BasicBlocks;

  std::vector<PEGBasicBlock*> MBBNumbering;


public:
  PEGFunction(const Function &Fn) : Fn(Fn) {};
  PEGFunction(const PEGFunction &) = delete;
  PEGFunction &operator=(const PEGFunction &) = delete;
  ~PEGFunction();

  void print(raw_ostream &os) const;
  friend raw_ostream &operator <<(raw_ostream &os, const PEGFunction &F);
  /// getFunction - Return the LLVM function that this machine code represents
  const Function &getFunction() const { return Fn; }
  using iterator = BasicBlockListType::iterator;
  using const_iterator = BasicBlockListType::const_iterator;

  const BasicBlockListType &getBasicBlockList() const { return BasicBlocks; }
        BasicBlockListType &getBasicBlockList()       { return BasicBlocks; }

  iterator                begin()       { return BasicBlocks.begin(); }
  const_iterator          begin() const { return BasicBlocks.begin(); }
  iterator                end  ()       { return BasicBlocks.end();   }
  const_iterator          end  () const { return BasicBlocks.end();   }

  size_t                   size() const { return BasicBlocks.size();  }
  bool                    empty() const { return BasicBlocks.empty(); }
  const PEGBasicBlock       &front() const { return BasicBlocks.front(); }
        PEGBasicBlock       &front()       { return BasicBlocks.front(); }
  const PEGBasicBlock        &back() const { return BasicBlocks.back();  }
        PEGBasicBlock        &back()       { return BasicBlocks.back();  }



  /// getName - Return the name of the corresponding LLVM function.
  StringRef getName() const { return Fn.getName(); }

};

 template <> struct GraphTraits<const PEGFunction*> :
  public GraphTraits<const PEGNode*> {
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


 template <> struct GraphTraits<PEGFunction*> : public GraphTraits<PEGNode*> {
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
