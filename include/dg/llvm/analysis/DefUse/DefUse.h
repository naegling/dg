#ifndef _LLVM_DEF_USE_ANALYSIS_H_
#define _LLVM_DEF_USE_ANALYSIS_H_


// ignore unused parameters in LLVM libraries
#if (__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-parameter"
#else
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#endif

#include <llvm/IR/Instruction.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/DataLayout.h>

#if (__clang__)
#pragma clang diagnostic pop // ignore -Wunused-parameter
#else
#pragma GCC diagnostic pop
#endif

#include "dg/analysis/DataFlowAnalysis.h"
#include "dg/llvm/analysis/ReachingDefinitions/ReachingDefinitions.h"

using dg::analysis::rd::LLVMReachingDefinitions;

namespace llvm {
    class DataLayout;
    class ConstantExpr;
};

namespace dg {

class LLVMDependenceGraph;
class LLVMNode;

class LLVMDefUseAnalysis : public analysis::DataFlowAnalysis<LLVMNode>
{
    LLVMDependenceGraph *dg;
    LLVMReachingDefinitions *RD;
    LLVMPointerAnalysis *PTA;
    const llvm::DataLayout *DL;
    bool assume_pure_functions;
public:
    LLVMDefUseAnalysis(LLVMDependenceGraph *dg,
                       LLVMReachingDefinitions *rd,
                       LLVMPointerAnalysis *pta,
                       bool assume_pure_functions = false);
    ~LLVMDefUseAnalysis() { delete DL; }

    /* virtual */
    bool runOnNode(LLVMNode *node, LLVMNode *prev);
private:
    void addDataDependence(LLVMNode *node,
                           analysis::pta::PSNode *pts,
                           analysis::rd::RDNode *mem,
                           uint64_t size);

    void addDataDependence(LLVMNode *node,
                           const llvm::Value *where, /* in CFG */
                           const llvm::Value *ptrOp,
                           uint64_t size);

    void addDataDependence(LLVMNode *node,
                           const llvm::Value *where, /* in CFG */
                           PSNode *pts, /* what memory */
                           uint64_t size);

    void addDataDependence(LLVMNode *node, analysis::rd::RDNode *rd);
    void addDataDependence(LLVMNode *node, llvm::Value *val);

    void addUnknownDataDependence(LLVMNode *node, PSNode *pts);

    void handleLoadInst(llvm::LoadInst *, LLVMNode *);
    void handleCallInst(LLVMNode *);
    void handleInlineAsm(LLVMNode *callNode);
    void handleIntrinsicCall(LLVMNode *callNode, llvm::CallInst *CI);
    void handleUndefinedCall(LLVMNode *callNode, llvm::CallInst *CI);
};

} // namespace dg

#endif //  _LLVM_DEF_USE_ANALYSIS_H_
