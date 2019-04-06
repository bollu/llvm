#ifndef LLVM_TRANSFORMS_IPO_ALWAYSINLINEISOLATOR_H
#define LLVM_TRANSFORMS_IPO_ALWAYSINLINEISOLATOR_H

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include <unordered_map>

namespace llvm {
/// Pass to remove unused function declarations.
struct AlwaysInlineIsolatorPass
    : public PassInfoMixin<AlwaysInlineIsolatorPass> {
public:
  void runImpl(Function &F);
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &) {
    runImpl(F);
    return PreservedAnalyses();
  };
};
} // namespace llvm

#endif // LLVM_TRANSFORMS_IPO_ALWAYSINLINEISOLATOR_H
