//==- llvm/CodeGen/MachineDominators.h - Machine Dom Calculation -*- C++ -*-==//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file defines classes mirroring those in llvm/Analysis/Dominators.h,
// but for target-specific code rather than target-independent IR.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_PEGDOMINATORS
#define LLVM_TRANSFORMS_PEGDOMINATORS

#include "llvm/ADT/SmallSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Transforms/GraphRewrite/GraphRewrite.h"
#include "llvm/Support/GenericDomTree.h"
#include "llvm/Support/GenericDomTreeConstruction.h"
#include <cassert>
#include <memory>
#include <vector>

using namespace llvm;

namespace llvm {

template <>
inline void DominatorTreeBase<PEGBasicBlock, false>::addRoot(
    PEGBasicBlock *MBB) {
  this->Roots.push_back(MBB);
}

extern template class DomTreeNodeBase<PEGBasicBlock>;
extern template class DominatorTreeBase<PEGBasicBlock, false>; // DomTree
extern template class DominatorTreeBase<PEGBasicBlock, true>; // PostDomTree

using PEGDomTreeNode = DomTreeNodeBase<PEGBasicBlock>;

class PEGDominatorTree : public DominatorTreeBase<PEGBasicBlock, false> {
 public:
  using Base = DominatorTreeBase<PEGBasicBlock, false>;

  PEGDominatorTree() = default;
  explicit PEGDominatorTree(PEGFunction &F) { recalculate(F); }

  /// Handle invalidation explicitly.
  // bool invalidate(Function &F, const PreservedAnalyses &PA,
  //                 FunctionAnalysisManager::Invalidator &);
// 
  /// \brief Returns *false* if the other dominator tree matches this dominator
  /// tree.
  inline bool compare(const PEGDominatorTree &Other) const {
    const PEGDomTreeNode *R = getRootNode();
    const PEGDomTreeNode *OtherR = Other.getRootNode();
    return !R || !OtherR || R->getBlock() != OtherR->getBlock() ||
           Base::compare(Other);
  }

};


template <class Node, class ChildIterator>
struct PEGDOMTreeGraphTraitsBase {
  using NodeRef = Node *;
  using ChildIteratorType = ChildIterator;
  using nodes_iterator = df_iterator<Node *, df_iterator_default_set<Node*>>;

  static NodeRef getEntryNode(NodeRef N) { return N; }
  static ChildIteratorType child_begin(NodeRef N) { return N->begin(); }
  static ChildIteratorType child_end(NodeRef N) { return N->end(); }
};

template <class T> struct GraphTraits;

template <>
struct GraphTraits<PEGDomTreeNode *>
    : public PEGDOMTreeGraphTraitsBase<PEGDomTreeNode,
                                           PEGDomTreeNode::iterator> {};

template <>
struct GraphTraits<const PEGDomTreeNode *>
    : public PEGDOMTreeGraphTraitsBase<const PEGDomTreeNode,
                                           PEGDomTreeNode::const_iterator> {
};


template <> struct GraphTraits<PEGDominatorTree*>
  : public GraphTraits<PEGDomTreeNode*> {
  static NodeRef getEntryNode(PEGDominatorTree *DT) { return DT->getRootNode(); }

  static nodes_iterator nodes_begin(PEGDominatorTree *N) {
    return df_begin(getEntryNode(N));
  }

  static nodes_iterator nodes_end(PEGDominatorTree *N) {
    return df_end(getEntryNode(N));
  }
};

} // end namespace llvm

#endif // LLVM_TRANSFORMS_PEGDOMINATORS
