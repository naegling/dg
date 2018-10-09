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

#if (LLVM_VERSION_MAJOR < 3)
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

#if LLVM_VERSION_MAJOR >= 4
#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/Bitcode/BitcodeWriter.h>
#else
#include <llvm/Bitcode/ReaderWriter.h>
#endif

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Instructions.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_os_ostream.h>
#include <llvm/Support/FormattedStream.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/Signals.h>
#include <llvm/Support/PrettyStackTrace.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/IntrinsicInst.h>

#if (__clang__)
#pragma clang diagnostic pop // ignore -Wunused-parameter
#else
#pragma GCC diagnostic pop
#endif

#include <iostream>
#include <fstream>

#include "dg/llvm/LLVMDG2Dot.h"

using namespace dg;

using llvm::errs;
using dg::analysis::LLVMPointerAnalysisOptions;
using dg::analysis::LLVMReachingDefinitionsAnalysisOptions;

// mapping of AllocaInst to the names of C variables
std::map<const llvm::Value *, std::string> valuesToVariables;

static std::vector<std::string> splitList(const std::string& opt, char sep = ',')
{
    std::vector<std::string> ret;
    if (opt.empty())
        return ret;

    size_t old_pos = 0;
    size_t pos = 0;
    while (true) {
        old_pos = pos;

        pos = opt.find(sep, pos);
        ret.push_back(opt.substr(old_pos, pos - old_pos));

        if (pos == std::string::npos)
            break;
        else
            ++pos;
    }

    return ret;
}

std::pair<std::vector<std::string>, std::vector<std::string>>
splitStringVector(std::vector<std::string>& vec,
                  std::function<bool(std::string&)> cmpFunc)
{
    std::vector<std::string> part1;
    std::vector<std::string> part2;

    for (auto& str : vec) {
        if (cmpFunc(str)) {
            part1.push_back(std::move(str));
        } else {
            part2.push_back(std::move(str));
        }
    }

    return {part1, part2};
}

static bool usesTheVariable(LLVMDependenceGraph& dg,
                            const llvm::Value *v,
                            const std::string& var)
{
    auto ptrNode = dg.getPTA()->getPointsTo(v);
    if (!ptrNode)
        return true; // it may be a definition of the variable, we do not know

    for (const auto& ptr : ptrNode->pointsTo) {
        if (ptr.isUnknown())
            return true; // it may be a definition of the variable, we do not know

        auto alloca = ptr.target->getUserData<llvm::Value>();
        if (!alloca)
            continue;

        if (const llvm::AllocaInst *AI = llvm::dyn_cast<llvm::AllocaInst>(alloca)) {
            auto name = valuesToVariables.find(AI);
            if (name != valuesToVariables.end()) {
                if (name->second == var)
                    return true;
            }
        }
    }

    return false;
}

template <typename InstT>
static bool useOfTheVar(LLVMDependenceGraph& dg,
                        const llvm::Instruction& I,
                        const std::string& var)
{
    // check that we store to that variable
    const InstT *tmp = llvm::dyn_cast<InstT>(&I);
    if (!tmp)
        return false;

    return usesTheVariable(dg, tmp->getPointerOperand(), var);
}

static bool isStoreToTheVar(LLVMDependenceGraph& dg,
                            const llvm::Instruction& I,
                            const std::string& var)
{
    return useOfTheVar<llvm::StoreInst>(dg, I, var);
}

static bool isLoadOfTheVar(LLVMDependenceGraph& dg,
                           const llvm::Instruction& I,
                           const std::string& var)
{
    return useOfTheVar<llvm::LoadInst>(dg, I, var);
}


static bool instMatchesCrit(LLVMDependenceGraph& dg,
                            const llvm::Instruction& I,
                            const std::vector<std::pair<int, std::string>>& parsedCrit)
{
    for (const auto& c : parsedCrit) {
        auto& Loc = I.getDebugLoc();
        if (!Loc)
            continue;
        if (static_cast<int>(Loc.getLine()) != c.first)
            continue;

        if (isStoreToTheVar(dg, I, c.second) ||
            isLoadOfTheVar(dg, I, c.second)) {
            llvm::errs() << "Matched line " << c.first << " with variable "
                         << c.second << " to:\n" << I << "\n";
            return true;
        }
    }

    return false;
}

static bool globalMatchesCrit(const llvm::GlobalVariable& G,
                              const std::vector<std::pair<int, std::string>>& parsedCrit)
{
    for (const auto& c : parsedCrit) {
        if (c.first != -1)
            continue;
        if (c.second == G.getName().str()) {
            llvm::errs() << "Matched global variable "
                         << c.second << " to:\n" << G << "\n";
            return true;
        }
    }

    return false;
}

static inline bool isNumber(const std::string& s) {
    assert(!s.empty());

    for (const auto c : s)
        if (!isdigit(c))
            return false;

    return true;
}

static void getLineCriteriaNodes(LLVMDependenceGraph& dg,
                                 std::vector<std::string>& criteria,
                                 std::set<LLVMNode *>& nodes)
{
    assert(!criteria.empty() && "No criteria given");

    std::vector<std::pair<int, std::string>> parsedCrit;
    for (auto& crit : criteria) {
        auto parts = splitList(crit, ':');
        assert(parts.size() == 2);

        // parse the line number
        if (parts[0].empty()) {
            // global variable
            parsedCrit.emplace_back(-1, parts[1]);
        } else if (isNumber(parts[0])) {
            int line = atoi(parts[0].c_str());
            if (line > 0)
                parsedCrit.emplace_back(line, parts[1]);
        } else {
            llvm::errs() << "Invalid line: '" << parts[0] << "'. "
                         << "Needs to be a number or empty for global variables.\n";
        }
    }

    assert(!parsedCrit.empty() && "Failed parsing criteria");

    // create the mapping from LLVM values to C variable names
    for (auto& it : getConstructedFunctions()) {
        for (auto& I : llvm::instructions(*llvm::cast<llvm::Function>(it.first))) {
            if (const llvm::DbgDeclareInst *DD = llvm::dyn_cast<llvm::DbgDeclareInst>(&I)) {
                auto val = DD->getAddress();
                valuesToVariables[val] = DD->getVariable()->getName().str();
            } else if (const llvm::DbgValueInst *DV
                        = llvm::dyn_cast<llvm::DbgValueInst>(&I)) {
                auto val = DV->getValue();
                valuesToVariables[val] = DV->getVariable()->getName().str();
            }
        }
    }

    if (valuesToVariables.empty()) {
        llvm::errs() << "No debugging information found in program,\n"
                     << "slicing criteria with lines and variables will not work.\n"
                     << "You can still use the criteria based on call sites ;)\n";
        return;
    }

    // map line criteria to nodes
    for (auto& it : getConstructedFunctions()) {
        for (auto& I : llvm::instructions(*llvm::cast<llvm::Function>(it.first))) {
            if (instMatchesCrit(dg, I, parsedCrit)) {
                LLVMNode *nd = dg.getNode(&I);
                assert(nd);
                nodes.insert(nd);
            }
        }
    }

    for (auto& G : dg.getModule()->globals()) {
        if (globalMatchesCrit(G, parsedCrit)) {
            LLVMNode *nd = dg.getGlobalNode(&G);
            assert(nd);
            nodes.insert(nd);
        }
    }
}

static std::set<LLVMNode *> getSlicingCriteriaNodes(LLVMDependenceGraph& dg,
                                                    const std::string& slicingCriteria)
{
    std::set<LLVMNode *> nodes;
    std::vector<std::string> criteria = splitList(slicingCriteria);
    assert(!criteria.empty() && "Did not get slicing criteria");

    std::vector<std::string> line_criteria;
    std::vector<std::string> node_criteria;
    std::tie(line_criteria, node_criteria)
        = splitStringVector(criteria, [](std::string& s) -> bool
            { return s.find(':') != std::string::npos; }
          );

    // if user wants to slice with respect to the return of main,
    // insert the ret instructions to the nodes.
    for (const auto& c : node_criteria) {
        if (c == "ret") {
            LLVMNode *exit = dg.getExit();
            // We could insert just the exit node, but this way we will
            // get annotations to the functions.
            for (auto it = exit->rev_control_begin(), et = exit->rev_control_end();
                 it != et; ++it) {
                nodes.insert(*it);
            }
        }
    }

    // map the criteria to nodes
    if (!node_criteria.empty())
        dg.getCallSites(node_criteria, &nodes);
    if (!line_criteria.empty())
        getLineCriteriaNodes(dg, line_criteria, nodes);

    return nodes;
}

std::unique_ptr<llvm::Module> parseModule(llvm::LLVMContext& context,
                                          const SlicerOptions& options)
{
    llvm::SMDiagnostic SMD;

#if ((LLVM_VERSION_MAJOR == 3) && (LLVM_VERSION_MINOR <= 5))
    auto _M = llvm::ParseIRFile(options.inputFile, SMD, context);
    auto M = std::unique_ptr<llvm::Module>(_M);
#else
    auto M = llvm::parseIRFile(options.inputFile, SMD, context);
    // _M is unique pointer, we need to get Module *
#endif

    if (!M) {
        SMD.print("llvm-slicer", llvm::errs());
    }

    return M;
}

void setupStackTraceOnError(int argc, char *argv[])
{
#ifndef USING_SANITIZERS

#if LLVM_VERSION_MAJOR == 3 && LLVM_VERSION_MINOR < 9
    llvm::sys::PrintStackTraceOnErrorSignal();
#else
    llvm::sys::PrintStackTraceOnErrorSignal(llvm::StringRef());
#endif
    llvm::PrettyStackTraceProgram X(argc, argv);

#endif // USING_SANITIZERS
}


int main(int argc, char *argv[])
{
    setupStackTraceOnError(argc, argv);

    SlicerOptions options = parseSlicerOptions(argc, argv);

    llvm::LLVMContext context;
    std::unique_ptr<llvm::Module> M = parseModule(context, options);
    if (!M) {
        llvm::errs() << "Failed parsing '" << options.inputFile << "' file:\n";
        return 1;
    }

    if (!M->getFunction(options.dgOptions.entryFunction)) {
        llvm::errs() << "The entry function not found: "
                     << options.dgOptions.entryFunction << "\n";
        return 1;
    }

    /// ---------------
    // slice the code
    /// ---------------

    // we do not want to slice away any assumptions
    // about the code
    // FIXME: do this optional only for SV-COMP
    options.additionalSlicingCriteria = {
        "__VERIFIER_assume",
        "__VERIFIER_exit",
        "klee_assume",
    };

    options.untouchedFunctions = {
        "__VERIFIER_assume",
        "__VERIFIER_exit"
    };

    Slicer slicer(M.get(), options);
    if (!slicer.buildDG()) {
        errs() << "ERROR: Failed building DG\n";
        return 1;
    }

    auto criteria_nodes = getSlicingCriteriaNodes(slicer.getDG(), options.slicingCriteria);
    if (criteria_nodes.empty()) {
        llvm::errs() << "Did not find slicing criterioa: '"
                     << options.slicingCriteria << "'\n";
    }

    // mark nodes that are going to be in the slice
    slicer.mark(criteria_nodes, true);

    // slice the graph
    if (!slicer.slice()) {
        errs() << "ERROR: Slicing failed\n";
        return 1;
    }

    return 0;
}
