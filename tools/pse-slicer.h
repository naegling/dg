#ifndef _DG_TOOL_PSE_SLICER_H_
#define _DG_TOOL_PSE_SLICER_H_

// ignore unused parameters in LLVM libraries
#if (__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-parameter"
#else
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#endif

#include <llvm/IR/Module.h>
#include <llvm/Support/raw_os_ostream.h>

#if (__clang__)
#pragma clang diagnostic pop // ignore -Wunused-parameter
#else
#pragma GCC diagnostic pop
#endif

#include "dg/llvm/LLVMDependenceGraph.h"
#include "dg/llvm/LLVMDependenceGraphBuilder.h"
#include "dg/llvm/LLVMSlicer.h"

#include "llvm/LLVMDGAssemblyAnnotationWriter.h"
#include "pse-slicer-opts.h"
#include "TimeMeasure.h"



/// --------------------------------------------------------------------
//   - Slicer class -
//
//  The main class that takes the bitcode, constructs the dependence graph
//  and then slices it w.r.t given slicing criteria.
/// --------------------------------------------------------------------
class Slicer {
  llvm::Module *M{};
  const SlicerOptions &_options;

  dg::llvmdg::LLVMDependenceGraphBuilder _builder;
  std::unique_ptr<dg::LLVMDependenceGraph> _dg{};

  dg::LLVMSlicer slicer;
  uint32_t slice_id = 0;

public:
  Slicer(llvm::Module *mod, const SlicerOptions &opts)
      : M(mod), _options(opts), _builder(mod, _options.dgOptions) {
    assert(mod && "Need module");
  }

  const dg::LLVMDependenceGraph &getDG() const { return *_dg.get(); }
  dg::LLVMDependenceGraph &getDG() { return *_dg.get(); }

  // mark the nodes from the slice
  bool mark(dg::LLVMNode * criteria_node) {
    assert(_dg && "mark() called without the dependence graph built");

    dg::debug::TimeMeasure tm;

    slice_id = 0xdead;

    tm.start();
    slice_id = slicer.mark(criteria_node, slice_id, false);

    assert(slice_id != 0 && "Something went wrong when marking nodes");

    tm.stop();
    tm.report("INFO: Finding dependent nodes took");
    return true;
  }

  bool resetSlices() {
    return setAllSlice(0);
  }

  bool setAllSlice(uint32_t sl_id) {

    // for all nodes, setSlice(sl_id)
    for (auto pair1: dg::getConstructedFunctions()) {
      dg::LLVMDependenceGraph *subdg = pair1.second;
      for (auto pair2: subdg->getBlocks()) {
         auto bb = pair2.second;
         bb->setSlice(sl_id);
         for (auto pair3: *subdg) {
           dg::LLVMNode *n = pair3.second;
           n->setSlice(sl_id);
         }
      }
    }
    return true;
  }


  // now slice away instructions from BBlocks that left
//  for (auto I = graph->begin(), E = graph->end(); I != E;) {


    bool slice() {
    assert(slice_id != 0 && "Must run mark() method before slice()");

    dg::debug::TimeMeasure tm;

    tm.start();
//    slicer.slice(_dg.get(), nullptr, slice_id);

    tm.stop();
    tm.report("INFO: Slicing dependence graph took");

    dg::analysis::SlicerStatistics &st = slicer.getStatistics();
    llvm::errs() << "INFO: Sliced away " << st.nodesRemoved
                 << " from " << st.nodesTotal << " nodes in DG\n";

    return true;
  }

  bool buildDG() {
    _dg = std::move(_builder.constructCFGOnly());

    if (!_dg) {
      llvm::errs() << "Building the dependence graph failed!\n";
      return false;
    }

    _dg = _builder.computeDependencies(std::move(_dg));
    return true;
  }

};

#endif // _DG_TOOL_PSE_SLICER_H_
