/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_thm_THMVector_h
#define jit_thm_THMVector_h
#if ENABLE_THM

#include "mozilla/Assertions.h"
#include "mozilla/Vector.h"

#include "jsalloc.h" // SystemAllocPolicy

namespace js {
namespace jit {

class MNode;
class MBasicBlock;
class MIRGraph;

} // namespace jit
} // namespace js

namespace js {
namespace jit {
namespace thm {

using BlockId = uint32_t;
using BranchesId = uint16_t;
using InstructionId = uint32_t;
using OperandId = uint32_t;

template <typename Elem>
using DenseVector = mozilla::Vector<Elem, 0, SystemAllocPolicy>;

// template <typename Elem>
// using SparseVector = mozilla::Vector<std::pair<uint32_t, Elem>, 0, SystemAllocPolicy>;

class THMGraph
{
  public:
    THMGraph(MIRGraph& graph);

    bool initFromMIRGraph();
    bool exportToMIRGraph();

  private:
    bool appendMIRBasicBlock(MBasicBlock* block);
    bool appendMIRNode(MNode* node);
    bool createControlFlowEdges();
    bool createDataFlowEdges();

  private:
    MIRGraph& graph_;

    BlockId numBlocks_;
    InstructionId numInstructions_;
    OperandId numOperands_;
    OperandId numUses_;

    BlockId osrBlockId_;

    /*\
    |*| Basic Blocks
    \*/
    struct InstructionRange {
        // Offsets inside instructionIndexes_;
        InstructionId phiStart;
        InstructionId insStart;

        // Not needed, as it can be computed from the successor phiStart.
        // But let's keep it for now.
        InstructionId controlId;
    };
    // for each block, gives the ranges of instruction indexes
    DenseVector<InstructionRange> instructionsRanges_;
    // for each instruction inside a basic block, gives the index of each
    // instruction. This vector is sorted based on the index of each instruction
    // inside a basic block.
    DenseVector<InstructionId> instructionIndexes_;

    struct ControlFlowEdges {
        // Note: ideally this should be part of the predecessors vector.
        BranchesId predecessorsIndex;
        BranchesId numPredecessors;
        // Note: ideally this should be part of the successors vector.
        BranchesId successorsIndex;
        BranchesId numSuccessors;
    };
    // for each block, gives the indexes inside the predecessors_ and
    // successors_ vectors of the control flow.
    DenseVector<ControlFlowEdges> controlFlow_;
    // for each block, gives the MIR representation of the data.
    DenseVector<MBasicBlock*> mirBasicBlocks_;

    /*\
    |*| Control Flow.
    \*/
    DenseVector<BlockId> predecessors_;
    DenseVector<BlockId> successors_;

    /*\
    |*| Instructions.
    \*/
    // for each instruction, gives the opcode of the instruction.
    DenseVector<uint32_t> opcodes_;

    struct DataFlowEdges {
        // Note: ideally this should be part of the operands vector.
        OperandId operandsIndex;
        OperandId numOperands; // uint16_t (no more than 4000)
        // Note: ideally this should be part of the uses vector.
        OperandId usesIndex;
        OperandId numUses;
    };
    // for each instruction, gives the indexes inside the operands_ and uses_
    // vectors of the data flow.
    DenseVector<DataFlowEdges> dataFlow_;
    // for each instruction, gives the MIR representation of the data.
    DenseVector<MNode*> mirNodes_;

    /*\
    |*| Data Flow.
    \*/
    DenseVector<OperandId> operands_;
    DenseVector<OperandId> uses_;
};

} // namespace thm
} // namespace jit
} // namespace js

#endif  // ENABLE_THM
#endif  /* jit_thm_THMVector_h */
