#ifndef _DG_TOOLS_PSE_SLICER_OPTS_H_
#define  _DG_TOOLS_PSE_SLICER_OPTS_H_

#include <vector>

#include "dg/llvm/LLVMDependenceGraphBuilder.h"

// CommandLine Category for slicer options
extern llvm::cl::OptionCategory SlicingOpts;

// Object representing options for slicer
struct SlicerOptions {
  dg::llvmdg::LLVMDependenceGraphOptions dgOptions{};
  std::string inputFile{};
  std::string outputFile{};
  std::string testDirectory{};
  bool includeSlice{};
};

///
// Return filled SlicerOptions structure.
void parseSlicerOptions(int argc, char *argv[], SlicerOptions &opts);

#endif  // _DG_TOOLS_PSE_SLICER_OPTS_H_

