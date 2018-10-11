#include <set>
#include <string>

#include <cassert>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <cctype>

#ifndef HAVE_LLVM
#error "This code needs LLVM enabled"
#endif

#include <llvm/Config/llvm-config.h>

#if (LLVM_VERSION_MAJOR < 6)
#error "Unsupported version of LLVM"
#endif

#include "pse-slicer.h"
#include "pse-slicer-opts.h"

// ignore unused parameters in LLVM libraries
#if (__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-parameter"
#else
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#endif

#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/Bitcode/BitcodeWriter.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/CallSite.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_os_ostream.h>
#include <llvm/Support/FormattedStream.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/Signals.h>
#include <llvm/Support/PrettyStackTrace.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/IntrinsicInst.h>

#include <boost/filesystem.hpp>
#include <boost/algorithm/string.hpp>

#if (__clang__)
#pragma clang diagnostic pop // ignore -Wunused-parameter
#else
#pragma GCC diagnostic pop
#endif

#include <iostream>
#include <fstream>

#include "dg/llvm/LLVMDG2Dot.h"
#include "json/json.h"

using namespace dg;
using namespace boost::filesystem;

using llvm::errs;
using llvm::outs;
using dg::analysis::LLVMPointerAnalysisOptions;
using dg::analysis::LLVMReachingDefinitionsAnalysisOptions;

void Slicer::constructMaps() {

  unsigned int mdkline = M->getMDKindID("klee.assemblyLine");

  for (auto pair1: dg::getConstructedFunctions()) {

    dg::LLVMDependenceGraph *subdg = pair1.second;
    for (auto pair2: subdg->getBlocks()) {
      auto bb = pair2.second;
      for (auto pair3: *subdg) {
        dg::LLVMNode *n = pair3.second;
        if (llvm::Instruction *i = llvm::dyn_cast<llvm::Instruction>(pair3.first)) {

          // look for klee line number metadata
          if (llvm::MDNode *md = i->getMetadata(mdkline)) {
            std::string line = llvm::cast<llvm::MDString>(md->getOperand(0))->getString().str();
            mapKleeIDs[std::stoi(line)] = pair3.second;
          }

          // check for a marker call
          if (llvm::CallInst *ci = llvm::dyn_cast<llvm::CallInst>(i)) {
            llvm::CallSite cs(ci);
            const llvm::Function *targetFn = cs.getCalledFunction();

            // two arguments returning void
            if (targetFn->arg_size() == 2 && targetFn->getReturnType()->isVoidTy()) {

              // check the name
              std::string targetName = targetFn->getName().str();
              boost::algorithm::to_lower(targetName);
              if (targetName == "mark") {

                const llvm::Constant *arg0 = llvm::dyn_cast<llvm::Constant>(cs.getArgument(0));
                const llvm::Constant *arg1 = llvm::dyn_cast<llvm::Constant>(cs.getArgument(1));
                if ((arg0 != nullptr) && (arg1 != nullptr)) {
                  unsigned fnID = (unsigned) arg0->getUniqueInteger().getZExtValue();
                  unsigned bbID = (unsigned) arg1->getUniqueInteger().getZExtValue();
                  unsigned marker = (fnID * 1000) + bbID;
                  mapMarkers[marker] = bb;
                }
              }
            }
          }
        }
      }
    }
  }
}

std::unique_ptr<llvm::Module> parseModule(llvm::LLVMContext &context,
                                          const SlicerOptions &options) {
  llvm::SMDiagnostic SMD;
  auto M = llvm::parseIRFile(options.inputFile, SMD, context);
  // _M is unique pointer, we need to get Module *

  if (!M) {
    SMD.print("llvm-slicer", errs());
  }

  return M;
}

void setupStackTraceOnError(int argc, char *argv[]) {
#ifndef USING_SANITIZERS

  llvm::sys::PrintStackTraceOnErrorSignal(llvm::StringRef());
  llvm::PrettyStackTraceProgram X(argc, argv);

#endif // USING_SANITIZERS
}


bool retrieve_testcase(std::string filename, unsigned &criteria, std::vector<unsigned> &trace) {

  bool result = false;
  criteria = 0;
  trace.clear();

  std::ifstream testcase;
  testcase.open(filename);
  if (testcase.is_open()) {
    Json::Value root;
    testcase >> root;

    criteria = root["instFaulting"].asUInt();
    Json::Value path = root["markerPath"];
    if (path.isArray()) {

      trace.reserve(path.size());
      for (Json::Value &element : path) {
        trace.push_back(element.asUInt());
      }
      result = true;
    }
  }
  return result;
}

int main(int argc, char *argv[]) {

  setupStackTraceOnError(argc, argv);
  SlicerOptions options = parseSlicerOptions(argc, argv);

  llvm::LLVMContext context;
  std::unique_ptr<llvm::Module> M = parseModule(context, options);
  if (!M) {
    errs() << "Failed parsing '" << options.inputFile << "' file:\n";
    return 1;
  }

  if (!M->getFunction(options.dgOptions.entryFunction)) {
    errs() << "The entry function not found: " << options.dgOptions.entryFunction << "\n";
    return 1;
  }



  /// ---------------
  // slice the code
  /// ---------------

  Slicer slicer(M.get(), options);
  if (!slicer.buildDG()) {
    errs() << "ERROR: Failed building DG\n";
    return 1;
  }

  path test_dir(options.testDirectory);
  if (is_directory(test_dir)) {

    const std::string test_ext = ".json";
    const std::string test_start = "test";

    // iterate through each test case in the test directory
    for (directory_entry &test_file: directory_iterator(test_dir)) {
      std::string ext = test_file.path().extension().string();
      boost::algorithm::to_lower(ext);
      if (ext == test_ext) {
        std::string fname = test_file.path().filename().string();
        if (fname.compare(0, test_start.length(), test_start) == 0) {

          outs() << fname << '\n';

          unsigned criteria;
          std::vector<unsigned> trace;
          if (retrieve_testcase(test_file.path().string(), criteria, trace)) {

//            slicer.resetSlices();
//            slicer.mark(criteria);
//            slicer.slice();
          }
        }
      }
    }

  } else {
    errs() << "ERROR: test case directory not found: " << options.testDirectory << "\n";
  }

  return 0;
}

