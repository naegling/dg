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

  for (const llvm::Function &fn: *M) {
    for (const llvm::BasicBlock &bb: fn) {

      bool marked = false;
      for (const llvm::Instruction &i: bb) {

        // look for klee line number metadata
        if (llvm::MDNode *md = i.getMetadata(mdkline)) {
          std::string line = llvm::cast<llvm::MDString>(md->getOperand(0))->getString().str();
          mapKleeIDs[std::stoi(line)] = &i;
        }

        if (!marked) {
          unsigned marker = is_marker(&i);
          if (marker != 0) {
            mapMarkers[marker] = &bb;
            marked = true;
          }
        }
      }
    }
  }
}

unsigned Slicer::is_marker(const llvm::Instruction *i) {

  unsigned result = 0;
  if (const llvm::CallInst *ci = llvm::dyn_cast<llvm::CallInst>(i)) {
    llvm::CallSite cs(const_cast<llvm::CallInst*>(ci));
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
          result = (fnID * 1000) + bbID;
        }
      }
    }
  }
  return result;
}

unsigned Slicer::calc_marker_length(unsigned marker) {

  unsigned result = 0;

  auto mkitr = mapMarkers.find(marker);
  assert(mkitr != mapMarkers.end());

  // find basicblock for marker value
  const llvm::BasicBlock *bb = mkitr->second;
  const llvm::Function *fn = bb->getParent();

  // first find function for this basicblock
  auto fnitr = dg::getConstructedFunctions().find(const_cast<llvm::Function*>(fn));
  assert(fnitr != dg::getConstructedFunctions().end());

  auto &dg = fnitr->second;
  assert(dg != nullptr && dg->getSlice() == slice_id);

  auto bbitr = dg->getBlocks().find(const_cast<llvm::BasicBlock *>(bb));
  assert(bbitr != dg->getBlocks().end());

  dg::LLVMBBlock *block = bbitr->second;
  assert(block != nullptr && block->getSlice() == slice_id);

  for (const auto &node : block->getNodes()) {

    // count nodes in the slice that are not marker calls
    if (node->getSlice() == slice_id) {
      llvm::Value *val = node->getKey();
      if (llvm::Instruction *i = llvm::dyn_cast<llvm::Instruction>(val)) {
        if (is_marker(i) == 0) {
          result += 1;
        }
      }
    }
  }

  return result;
}

unsigned Slicer::sliceInstrLength(const std::vector<unsigned> &slice) {

  unsigned result = 0;
  std::map<unsigned,unsigned> marker_lengths;

  for (unsigned marker: slice) {
    auto itr = marker_lengths.find(marker);
    if (itr == marker_lengths.end()) {
      unsigned length = calc_marker_length(marker);
      result += length;
      marker_lengths.insert(std::make_pair(marker, length));
    } else {
      result += itr->second;
    }
  }
  return result;
}

void Slicer::diag_dump() {

  unsigned int mdkline = M->getMDKindID("klee.assemblyLine");
  for (auto fnitr: getConstructedFunctions()) {
    const llvm::Function *fn = llvm::dyn_cast<llvm::Function>(fnitr.first);
    if (fn != nullptr) {
      auto dg = fnitr.second;
      if (dg->getSlice() == slice_id) {
        outs() << "Fn: " << fn->getName().str() << "\n";

        for (auto &bb : *fn) {

          auto bbitr = dg->getBlocks().find(const_cast<llvm::BasicBlock *>(&bb));
          if (bbitr != dg->getBlocks().end()) {
            dg::LLVMBBlock *block = bbitr->second;
            if (block->getSlice() == slice_id) {

              unsigned marker = 0;
              for (auto pair: mapMarkers) {
                if (pair.second == &bb) {
                  marker = pair.first;
                  break;
                }
              }
              outs() << marker << ":";
              unsigned counter = 0;
              for (auto &i : bb) {
                dg::LLVMNode *node = dg->getNode(const_cast<llvm::Instruction*>(&i));
                if (node->getSlice() == slice_id) {
                  if (llvm::MDNode *md = i.getMetadata(mdkline)) {
                    std::string line = llvm::cast<llvm::MDString>(md->getOperand(0))->getString().str();
                    if (counter++ > 0) {
                      outs() << ",";
                    }
                    outs() << line;
                  }
                }
              }
              outs() << "\n";
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

class Emitter {

  llvm::raw_ostream &output;
  bool include_slice;
  unsigned counter;

public:

  Emitter(llvm::raw_ostream &o, bool b) : output(o), include_slice(b), counter(0)
              { output << "[\n"; }
  ~Emitter()  { output << "\n]\n"; }
  void write_case(std::string name, const std::vector<unsigned> &slice, unsigned instr_length) {
    if (counter++ > 0) {
      output << ",\n";
    }
    output << "  {\n";
    output << "    \"testcase\": \"" << name << "\",\n";
    if (include_slice) {
      output << "    \"slice\": " << "[";
      for (auto itr = slice.begin(), start = slice.begin(), end = slice.end(); itr != end; ++itr) {
        if (itr != start) {
          output << ",";
        }
        output << *itr;
      }
      output << "],\n";
    }
    output << "    \"sliceLength\": " << slice.size() << ",\n";
    output << "    \"instrLength\": " << instr_length << "\n";
    output << "  }";
  }

};

int main(int argc, char *argv[]) {

  setupStackTraceOnError(argc, argv);
  SlicerOptions options;
  parseSlicerOptions(argc, argv, options);
  llvm::raw_ostream *output = &outs();

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

    // create an output stream if outfile is specified
    // else, just use outs
    if (!options.outputFile.empty()) {
      std::error_code ec;
      output = new llvm::raw_fd_ostream(options.outputFile, ec, llvm::sys::fs::F_None);
    }

    Emitter emitter(*output, options.includeSlice);

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

          unsigned counter = 0;
          unsigned criteria;
          std::vector<unsigned> trace;
          if (retrieve_testcase(test_file.path().string(), criteria, trace)) {

            std::vector<unsigned> slice;
            slicer.setSliceID(++counter);
            slicer.mark(criteria);
            slicer.slice(trace, slice);
            emitter.write_case(fname, slice, slicer.sliceInstrLength(slice));
//          slicer.diag_dump();
          }
        }
      }
    }

  } else {
    errs() << "ERROR: test case directory not found: " << options.testDirectory << "\n";
  }

  if (output != &outs()) {
    delete output;
  }

  return 0;
}

