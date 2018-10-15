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

  std::map<unsigned,const llvm::BasicBlock*> mapMarkers;
  std::map<unsigned,const llvm::Instruction*> mapKleeIDs;

  void constructMaps();

public:
  Slicer(llvm::Module *mod, const SlicerOptions &opts)
      : M(mod), _options(opts), _builder(mod, _options.dgOptions) {
    assert(mod && "Need module");
  }

  void setSliceID(uint32_t s) { slice_id = s; }

  const dg::LLVMDependenceGraph &getDG() const { return *_dg.get(); }
  dg::LLVMDependenceGraph &getDG() { return *_dg.get(); }

  // mark the nodes from the slice
  bool mark(unsigned klee_id) {

    assert(_dg && "mark() called without the dependence graph built");

    // find the referenced instruction and dg node
    auto itr = mapKleeIDs.find(klee_id);
    if (itr != mapKleeIDs.end()) {


      dg::LLVMNode *criteria = nullptr;

      const llvm::Instruction *i = itr->second;
      for (const auto &fn: dg::getConstructedFunctions()) {
        auto dg = fn.second;
        criteria = dg->getNode(const_cast<llvm::Instruction*>(i));
        if (criteria != nullptr) {
          slice_id = slicer.mark(criteria, slice_id, false);
          assert(slice_id != 0 && "Something went wrong when marking nodes");
          return true;
        }
      }
    }
    return false;
  }

  bool slice(const std::vector<unsigned> &path, std::vector<unsigned> &slice) {

    assert(slice_id != 0 && "Must run mark() method before slice()");

    dg::debug::TimeMeasure tm;

    slice.reserve(path.size());
    for (unsigned marker: path) {
      auto itr = mapMarkers.find(marker);
      if (itr != mapMarkers.end()) {
        const llvm::BasicBlock *bb = itr->second;
//        dg::LLVMBBlock *n = _dg->getNode(const_cast<llvm::BasicBlock*>(bb));

      }
    }

//    tm.start();
//    slicer.slice(_dg.get(), nullptr, slice_id);

//    tm.stop();
//    tm.report("INFO: Slicing dependence graph took");

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
    constructMaps();
    return true;
  }

};

#endif // _DG_TOOL_PSE_SLICER_H_
