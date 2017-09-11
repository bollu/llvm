add_llvm_library(LLVMGraphRewrite
  GraphRewrite.cpp

  ADDITIONAL_HEADER_DIRS
  ${LLVM_MAIN_INCLUDE_DIR}/llvm/Transforms
  ${LLVM_MAIN_INCLUDE_DIR}/llvm/Transforms/GraphRewrite

  DEPENDS
  intrinsics_gen
  
  )
