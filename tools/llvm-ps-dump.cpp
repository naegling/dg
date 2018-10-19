#ifndef HAVE_LLVM
#error "This code needs LLVM enabled"
#endif

#include <set>
#include <string>
#include <iostream>
#include <sstream>
#include <fstream>
#include <cassert>
#include <cstdio>
#include <cstdlib>

// ignore unused parameters in LLVM libraries
#if (__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-parameter"
#else
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#endif

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Instructions.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_os_ostream.h>
#include <llvm/IRReader/IRReader.h>

#if LLVM_VERSION_MAJOR >= 4
#include <llvm/Bitcode/BitcodeReader.h>
#else
#include <llvm/Bitcode/ReaderWriter.h>
#endif

#if (__clang__)
#pragma clang diagnostic pop // ignore -Wunused-parameter
#else
#pragma GCC diagnostic pop
#endif

#include "dg/llvm/analysis/PointsTo/PointerAnalysis.h"
#include "dg/analysis/PointsTo/PointerAnalysisFI.h"
#include "dg/analysis/PointsTo/PointerAnalysisFS.h"
#include "dg/analysis/PointsTo/PointerAnalysisFSInv.h"
#include "dg/analysis/PointsTo/Pointer.h"

#include "TimeMeasure.h"

using namespace dg;
using namespace dg::analysis::pta;
using dg::debug::TimeMeasure;
using llvm::errs;

static bool verbose;
static bool verbose_more;
static bool ids_only = false;
static bool dump_graph_only = false;
static uint64_t dump_iteration = 0;
static const char *entry_func = "main";

std::unique_ptr<PointerAnalysis> PA;

enum PTType {
    FLOW_SENSITIVE = 1,
    FLOW_INSENSITIVE,
    WITH_INVALIDATE,
};

static std::string
getInstName(const llvm::Value *val)
{
    std::ostringstream ostr;
    llvm::raw_os_ostream ro(ostr);

    assert(val);
    if (llvm::isa<llvm::Function>(val))
        ro << val->getName().data();
    else
        ro << *val;

    ro.flush();

    // break the string if it is too long
    return ostr.str();
}

void printPSNodeType(enum PSNodeType type) {
    printf("%s", PSNodeTypeToCString(type));
}


static void dumpPointer(const Pointer& ptr, bool dot);

static void
printName(PSNode *node, bool dot = false)
{
    std::string nm;
    const char *name = nullptr;
    if (node->isNull()) {
        name = "null";
    } else if (node->isUnknownMemory()) {
        name = "unknown";
    } else if (node->isInvalidated() &&
        !node->getUserData<llvm::Value>()) {
            name = "invalidated";
    }

    if (!name) {
        if (ids_only) {
            printf(" <%u>", node->getID());
            return;
        }

        if (!node->getUserData<llvm::Value>()) {
            if (dot) {
                printf("<%u> (no name)\\n", node->getID());

                if (node->getType() == PSNodeType::CONSTANT) {
                    dumpPointer(*(node->pointsTo.begin()), dot);
                } else if (node->getType() == PSNodeType::CALL_RETURN) {
                    if (PSNode *paired = node->getPairedNode())
                        printName(paired, dot);
                } else if (PSNodeEntry *entry = PSNodeEntry::get(node)) {
                    printf("%s\\n", entry->getFunctionName().c_str());
                }
            } else {
                printf("<%u> ", node->getID());
                printPSNodeType(node->getType());
            }

            return;
        }

        nm = getInstName(node->getUserData<llvm::Value>());
        name = nm.c_str();
    }

    if (ids_only) {
        printf(" <%u>", node->getID());
        return;
    }

    // escape the " character
    for (int i = 0; name[i] != '\0'; ++i) {
        // crop long names
        if (i >= 70) {
            printf(" ...");
            break;
        }

        if (name[i] == '"')
            putchar('\\');

        putchar(name[i]);
    }
}

static void dumpPointer(const Pointer& ptr, bool dot)
{
    printName(ptr.target, dot);

    if (ptr.offset.isUnknown())
        printf(" + UNKNOWN");
    else
        printf(" + %lu", *ptr.offset);
}

static void
dumpMemoryObject(MemoryObject *mo, int ind, bool dot)
{
    bool printed_multi = false;
    for (auto& it : mo->pointsTo) {
        int width = 0;
        for (const Pointer& ptr : it.second) {
            // print indentation
            printf("%*s", ind, "");

            if (width > 0) {
                    printf("%*s -> ", width, "");
            } else {
                if (it.first.isUnknown())
                    width = printf("[??]");
                else
                    width = printf("[%lu]", *it.first);

                // print a new line if there are multiple items
                if (dot &&
                    (it.second.size() > 1 ||
                     (printed_multi && mo->pointsTo.size() > 1))) {
                    printed_multi = true;
                    printf("\\l%*s",ind + width, "");
                }

                printf(" -> ");

                assert(width > 0);
            }

            dumpPointer(ptr, dot);

            if (dot)
                printf("\\l");
            else
                putchar('\n');
        }
    }
}

static void
dumpMemoryMap(PointerAnalysisFS::MemoryMapT *mm, int ind, bool dot)
{
    for (const auto& it : *mm) {
        // print the key
        if (!dot)
            printf("%*s", ind, "");

        putchar('<');
        printName(it.first, dot);
        putchar('>');

        if (dot)
            printf("\\l");
        else
            putchar('\n');

        dumpMemoryObject(it.second.get(), ind + 4, dot);
    }
}

static bool mmChanged(PSNode *n)
{
    if (n->predecessorsNum() == 0)
        return true;

    PointerAnalysisFS::MemoryMapT *mm
        = n->getData<PointerAnalysisFS::MemoryMapT>();

    for (PSNode *pred : n->getPredecessors()) {
        if (pred->getData<PointerAnalysisFS::MemoryMapT>() != mm)
            return true;
    }

    return false;
}

static void
dumpPointerSubgraphData(PSNode *n, PTType type, bool dot = false)
{
    assert(n && "No node given");
    if (type == FLOW_INSENSITIVE) {
        MemoryObject *mo = n->getData<MemoryObject>();
        if (!mo)
            return;

        if (dot)
            printf("\\n    Memory: ---\\n");
        else
            printf("    Memory: ---\n");

        dumpMemoryObject(mo, 6, dot);

        if (!dot)
            printf("    -----------\n");
    } else {
        PointerAnalysisFS::MemoryMapT *mm
            = n->getData<PointerAnalysisFS::MemoryMapT>();
        if (!mm)
            return;

        if (dot)
            printf("\\n------\\n    --- Memory map [%p] ---\\n", static_cast<void*>(mm));
        else
            printf("    Memory map: [%p]\n", static_cast<void*>(mm));

        if (verbose_more || mmChanged(n))
            dumpMemoryMap(mm, 6, dot);

        if (!dot)
            printf("    ----------------\n");
    }
}

static void
dumpPSNode(PSNode *n, PTType type)
{
    printf("NODE %3u: ", n->getID());
    printName(n);

    PSNodeAlloc *alloc = PSNodeAlloc::get(n);
    if (alloc &&
        (alloc->getSize() || alloc->isHeap() || alloc->isZeroInitialized()))
        printf(" [size: %lu, heap: %u, zeroed: %u]",
               alloc->getSize(), alloc->isHeap(), alloc->isZeroInitialized());

    printf(" (points-to size: %lu)\n", n->pointsTo.size());

    for (const Pointer& ptr : n->pointsTo) {
        printf("    -> ");
        printName(ptr.target, false);
        if (ptr.offset.isUnknown())
            puts(" + Offset::UNKNOWN");
        else
            printf(" + %lu\n", *ptr.offset);
    }
    if (verbose) {
        dumpPointerSubgraphData(n, type);
    }
}

static void
dumpPointerSubgraphdot(LLVMPointerAnalysis *pta, PTType type)
{

    printf("digraph \"Pointer State Subgraph\" {\n");

    /* dump nodes */
    const auto& nodes = pta->getNodes();
    for (const auto& node : nodes) {
        if (!node)
            continue;
        printf("\tNODE%u [label=\"<%u> ", node->getID(), node->getID());
        printPSNodeType(node->getType());
        printf("\\n");
        printName(node.get(), true);
        printf("\\nparent: %u\\n", node->getParent() ? node->getParent()->getID() : 0);

        PSNodeAlloc *alloc = PSNodeAlloc::get(node.get());
        if (alloc && (alloc->getSize() || alloc->isHeap() || alloc->isZeroInitialized()))
            printf("\\n[size: %lu, heap: %u, zeroed: %u]",
               alloc->getSize(), alloc->isHeap(), alloc->isZeroInitialized());

        if (verbose && node->getOperandsNum() > 0) {
            printf("\\n--- operands ---\\n");
            for (PSNode *op : node->getOperands()) {
                printName(op, true);
                printf("\\n");
            }
            printf("------\\n");
        }

        if (verbose) {
            printf("--- points-to set ---\\n");
        }

        for (const Pointer& ptr : node->pointsTo) {
            printf("\\n    -> ");
            printName(ptr.target, true);
            printf(" + ");
            if (ptr.offset.isUnknown())
                printf("Offset::UNKNOWN");
            else
                printf("%lu", *ptr.offset);
        }

        if (verbose) {
            dumpPointerSubgraphData(node.get(), type, true /* dot */);
        }

        printf("\", shape=box");
        if (node->getType() != PSNodeType::STORE) {
            if (node->pointsTo.size() == 0
                && (node->getType() == PSNodeType::LOAD ||
                    node->getType() == PSNodeType::GEP  ||
                    node->getType() == PSNodeType::CAST ||
                    node->getType() == PSNodeType::PHI))
                printf(", style=filled, fillcolor=red");
        } else {
            printf(", style=filled, fillcolor=orange");
        }

        printf("]\n");
    }

    /* dump edges */
    for (const auto& node : nodes) {
        if (!node) // node id 0 is nullptr
            continue;

        for (PSNode *succ : node->getSuccessors()) {
            printf("\tNODE%u -> NODE%u [penwidth=2]\n",
                   node->getID(), succ->getID());
        }

        for (PSNode *op : node->getOperands()) {
            printf("\tNODE%u -> NODE%u [color=blue,style=dotted,constraint=false]\n",
                   op->getID(), node->getID());
        }
    }

    printf("}\n");
}

static void
dumpPointerSubgraph(LLVMPointerAnalysis *pta, PTType type, bool todot)
{
    assert(pta);

    if (todot)
        dumpPointerSubgraphdot(pta, type);
    else {
        const auto& nodes = pta->getNodes();
        for (const auto& node : nodes) {
            if (node) // node id 0 is nullptr
                dumpPSNode(node.get(), type);
        }
    }
}

int main(int argc, char *argv[])
{
    llvm::Module *M;
    llvm::LLVMContext context;
    llvm::SMDiagnostic SMD;
    bool todot = false;
    const char *module = nullptr;
    PTType type = FLOW_INSENSITIVE;
    uint64_t field_senitivity = Offset::UNKNOWN;

    // parse options
    for (int i = 1; i < argc; ++i) {
        // run given points-to analysis
        if (strcmp(argv[i], "-pta") == 0) {
            if (strcmp(argv[i+1], "fs") == 0)
                type = FLOW_SENSITIVE;
            else if (strcmp(argv[i+1], "inv") == 0)
                type = WITH_INVALIDATE;
        } else if (strcmp(argv[i], "-pta-field-sensitive") == 0) {
            field_senitivity = static_cast<uint64_t>(atoll(argv[i + 1]));
        } else if (strcmp(argv[i], "-dot") == 0) {
            todot = true;
        } else if (strcmp(argv[i], "-ids-only") == 0) {
            ids_only = true;
        } else if (strcmp(argv[i], "-iteration") == 0) {
            dump_iteration = static_cast<uint64_t>(atoll(argv[i + 1]));
        } else if (strcmp(argv[i], "-graph-only") == 0) {
            dump_graph_only = true;
        } else if (strcmp(argv[i], "-v") == 0) {
            verbose = true;
        } else if (strcmp(argv[i], "-vv") == 0) {
            verbose = true;
            verbose_more = true;
        } else if (strcmp(argv[i], "-entry") == 0) {
            entry_func = argv[i + 1];
        } else {
            module = argv[i];
        }
    }

    if (!module) {
        errs() << "Usage: % IR_module [output_file]\n";
        return 1;
    }

#if ((LLVM_VERSION_MAJOR == 3) && (LLVM_VERSION_MINOR <= 5))
    M = llvm::ParseIRFile(module, SMD, context);
#else
    auto _M = llvm::parseIRFile(module, SMD, context);
    // _M is unique pointer, we need to get Module *
    M = _M.get();
#endif

    if (!M) {
        llvm::errs() << "Failed parsing '" << module << "' file:\n";
        SMD.print(argv[0], errs());
        return 1;
    }

    TimeMeasure tm;

    LLVMPointerAnalysis PTA(M, entry_func, field_senitivity);

    tm.start();

    // use createAnalysis instead of the run() method so that we won't delete
    // the analysis data (like memory objects) which may be needed
    if (type == FLOW_INSENSITIVE) {
        PA = std::unique_ptr<PointerAnalysis>(
            PTA.createPTA<analysis::pta::PointerAnalysisFI>()
            );
    } else if (type == WITH_INVALIDATE) {
        PA = std::unique_ptr<PointerAnalysis>(
            PTA.createPTA<analysis::pta::PointerAnalysisFSInv>()
            );
    } else {
        PA = std::unique_ptr<PointerAnalysis>(
            PTA.createPTA<analysis::pta::PointerAnalysisFS>()
            );
    }

    if (dump_graph_only) {
        dumpPointerSubgraph(&PTA, type, true);
        return 0;
    }

    // run the analysis
    if (dump_iteration > 0) {
        // do preprocessing and queue the nodes
        PA->preprocess();
        PA->initialize_queue();

        // do fixpoint
        for (unsigned i = 0; i < dump_iteration; ++i) {
            if (PA->iteration() == false)
                break;
            PA->queue_changed();
        }
    } else {
        PA->run();
    }

    tm.stop();
    tm.report("INFO: Points-to analysis [new] took");
    dumpPointerSubgraph(&PTA, type, todot);

    return 0;
}
