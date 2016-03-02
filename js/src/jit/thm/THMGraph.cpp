/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/thm/THMGraph.h"

#include "jit/MIRGraph.h"

namespace js {
namespace jit {
namespace thm {

THMGraph::THMGraph(MIRGraph& graph)
  : graph_(graph)
  , numBlocks_(0)
  , numInstructions_(0)
  , numOperands_(0)
  , numUses_(0)
  , osrBlockId_(0)
{
}

bool
THMGraph::initFromMIRGraph()
{
    numBlocks_ = 0;
    numInstructions_ = 0;
    numOperands_ = 0;
    numUses_ = 0;

    osrBlockId_ = 0xffffffff;

    for (ReversePostorderIterator block(graph_.rpoBegin()); block != graph_.rpoEnd(); block++) {
        if (!appendMIRBasicBlock(*block))
            return false;
    }

    if (graph_.osrBlock())
        osrBlockId_ = graph_.osrBlock()->id();

    if (!createControlFlowEdges() || !createDataFlowEdges())
        return false;

    return true;
}

bool
THMGraph::appendMIRBasicBlock(MBasicBlock* block)
{
    // renumber to make it easier to generate branches.
    block->setId(numBlocks_);

    InstructionRange range;
    ControlFlowEdges branches;
    branches.predecessorsIndex = 0;
    branches.successorsIndex = 0;

    range.phiStart = numInstructions_;
    for (MPhiIterator def(block->phisBegin()), end(block->phisEnd()); def != end; ++def) {
        if (!appendMIRNode(*def))
            return false;
    }

    range.insStart = numInstructions_;
    if (MResumePoint* rp = block->entryResumePoint()) {
        if (!appendMIRNode(rp))
            return false;
    }

    for (MInstructionIterator def(block->begin()), end(block->begin(block->lastIns()));
         def != end;
         ++def)
    {
        if (!appendMIRNode(*def))
            return false;
    }

    if (MResumePoint* rp = block->outerResumePoint()) {
        if (!appendMIRNode(rp))
            return false;
    }

    range.controlId = numInstructions_;
    {
        MControlInstruction* def = block->lastIns();
        if (!appendMIRNode(def))
            return false;
    }

    branches.numPredecessors = block->numPredecessors();
    branches.numSuccessors = block->numSuccessors();

    if (!instructionsRanges_.append(range) ||
        !controlFlow_.append(branches) ||
        !mirBasicBlocks_.append(block))
    {
        return false;
    }

    numBlocks_++;
    return true;
}

bool
THMGraph::appendMIRNode(MNode* node)
{
    uint32_t opcode;
    DataFlowEdges edges;
    edges.operandsIndex = 0;
    edges.usesIndex = 0;

    MResumePoint* laterRp = nullptr;

    edges.numOperands = node->numOperands();
    if (node->isDefinition()) {
        MDefinition* def = node->toDefinition();
        // renumber to make it easier to generate operands.
        def->setId(numInstructions_);

        opcode = uint32_t(def->op());
        edges.numUses = 0;
        for (MUseIterator i(def->usesBegin()), e(def->usesEnd()); i != e; ++i)
            edges.numUses++;
        if (def->isInstruction())
            laterRp = def->toInstruction()->resumePoint();
    } else {
        MOZ_ASSERT(node->isResumePoint());
        opcode = MDefinition::Op_Invalid; // This is a prototype ...
        edges.numUses = 0;
    }

    if (!instructionIndexes_.append(numInstructions_) ||
        !opcodes_.append(opcode) ||
        !dataFlow_.append(edges) ||
        !mirNodes_.append(node))
    {
        return false;
    }

    numInstructions_++;
    return laterRp ? appendMIRNode(laterRp) : true;
}

bool
THMGraph::createControlFlowEdges()
{
    BranchesId predecessorsIndex = 0;
    BranchesId successorsIndex = 0;
    for (size_t blockIndex = 0; blockIndex < numBlocks_; blockIndex++) {
        MBasicBlock* block = mirBasicBlocks_[blockIndex];
        ControlFlowEdges& edges = controlFlow_[blockIndex];
        edges.predecessorsIndex = predecessorsIndex;
        edges.successorsIndex = successorsIndex;

        // Note: The control flow edges have no duplicates, thus we do not need
        // to have a twice-half-stored double linked vectors, as we do for the
        // data flow.
        for (BranchesId i = 0; i < edges.numPredecessors; i++) {
            if (!successors_.append(block->getPredecessor(i)->id()))
                return false;
            predecessorsIndex++;
        }
        for (BranchesId i = 0; i < edges.numSuccessors; i++) {
            if (!successors_.append(block->getSuccessor(i)->id()))
                return false;
            successorsIndex++;
        }
    }

    return true;
}

bool
THMGraph::createDataFlowEdges()
{
    BranchesId operandsIndex = 0;
    for (size_t insIndex = 0; insIndex < numInstructions_; insIndex++) {
        MNode* node = mirNodes_[insIndex];
        DataFlowEdges& edges = dataFlow_[insIndex];
        edges.operandsIndex = operandsIndex;

        for (OperandId i = 0; i < edges.numOperands; i++) {
            // We temporarily store the instruction index instead of the uses
            // index.
            if (!operands_.append(node->getOperand(i)->id()))
                return false;
            operandsIndex++;
        }
    }

    numOperands_ = operandsIndex;
    if (!operands_.appendN(0, numOperands_))
        return false;
    numUses_ = operandsIndex;
    if (!uses_.appendN(0, numOperands_))
        return false;

    // Store the index, as being the sum all of the previous uses, plus the
    // number of uses of the current instruction, such that we can decrement it
    // as we add uses.
    BranchesId usesIndex = 0;
    for (size_t insIndex = 0; insIndex < numInstructions_; insIndex++) {
        DataFlowEdges& edges = dataFlow_[insIndex];
        usesIndex += edges.numUses;
        edges.usesIndex = usesIndex;
    }
    MOZ_ASSERT(operandsIndex == usesIndex);

    // Build the twice-half-stored double linked vector of operands and uses.
    for (size_t opIndex = 0; opIndex < numOperands_; opIndex++) {
        InstructionId insIndex = operands_[opIndex];
        DataFlowEdges& edges = dataFlow_[insIndex];

        // The uses_ vector has indexes of the operands_ vector.
        uses_[--edges.usesIndex] = opIndex;
        // The operands_ vector has indexes of the uses_ vector.
        operands_[opIndex] = edges.usesIndex;
    }

    return true;
}

bool
THMGraph::exportToMIRGraph()
{
    // For the prototype, assume that if a transformation adds a node, then it
    // would have a corresponding mir node allocated, but that none of the edges
    // are setup correctly.

    // For the prototype, assume that we do not change the number of basic
    // blocks at the moment, nor their order.

    // Replace all basic block instructions
    for (size_t blockIndex = 0; blockIndex < numBlocks_; blockIndex++) {
        MBasicBlock* block = mirBasicBlocks_[blockIndex];

        // Remove all existing relations. (use popFront instead of clear to clean-up
        // the next/prev fields)
        block->phis_.clear();
        block->instructions_.clear();

        // Add new instructions.
        InstructionRange ranges = instructionsRanges_[blockIndex];
        for (size_t insIndex = ranges.phiStart; insIndex < ranges.insStart; insIndex++) {
            MPhi* phi = mirNodes_[instructionIndexes_[insIndex]]->toDefinition()->toPhi();
            block->phis_.pushBackUnchecked(phi);
        }
        for (size_t insIndex = ranges.insStart; insIndex < ranges.controlId + 1; insIndex++) {
            MNode* node = mirNodes_[instructionIndexes_[insIndex]];
            if (node->isResumePoint())
                continue; // :prototype: Assumes that the original graph still
                          // has resume points attached to the instructions.
            MInstruction* ins = node->toDefinition()->toInstruction();
            block->instructions_.pushBackUnchecked(ins);
        }
        MOZ_ASSERT(block->hasLastIns());
    }

    // Replace all uses by their instruction index.
    for (size_t insIndex = 0; insIndex < numInstructions_; insIndex++) {
        MNode* node = mirNodes_[insIndex];
        DataFlowEdges edges = dataFlow_[insIndex];
        for (size_t useIndex = 0; useIndex < edges.numUses; useIndex++)
            uses_[edges.usesIndex + useIndex] = insIndex;
        if (!node->isDefinition())
            continue;
        node->toDefinition()->uses_.clear();
    }

    // Replace all operands.
    for (size_t insIndex = 0; insIndex < numInstructions_; insIndex++) {
        MNode* consumer = mirNodes_[insIndex];
        DataFlowEdges edges = dataFlow_[insIndex];
        for (size_t opIndex = 0; opIndex < edges.numOperands; opIndex++) {
            MUse* use = consumer->getUseFor(opIndex);
            MDefinition* producer = mirNodes_[uses_[operands_[edges.operandsIndex + opIndex]]]->toDefinition();
            use->initUnchecked(producer, consumer);
        }
    }

    return true;
}

} // namespace thm
} // namespace jit
} // namespace js
