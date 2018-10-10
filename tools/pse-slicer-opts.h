#ifndef _DG_TOOLS_PSE_SLICER_OPTS_H_
#define  _DG_TOOLS_PSE_SLICER_OPTS_H_

#include <vector>

#include "dg/llvm/LLVMDependenceGraphBuilder.h"

// CommandLine Category for slicer options
extern llvm::cl::OptionCategory SlicingOpts;

// Object representing options for slicer
struct SlicerOptions {
  dg::llvmdg::LLVMDependenceGraphOptions dgOptions{};

  std::vector<std::string> untouchedFunctions{};
  // FIXME: get rid of this once we got the secondary SC
  std::vector<std::string> additionalSlicingCriteria{};
  std::string slicingCriteria{};
  std::string inputFile{};
  std::string outputFile{};
};

///
// Return filled SlicerOptions structure.
SlicerOptions parseSlicerOptions(int argc, char *argv[]);

#endif  // _DG_TOOLS_PSE_SLICER_OPTS_H_

