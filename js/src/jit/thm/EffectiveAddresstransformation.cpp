/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/thm/EffectiveAddressTransformation.h"

#include <algorithm>

#include "jit/MIR.h"

#include "jit/thm/THMGraph.h"

using namespace js;
using namespace jit;

// Assert way too much things which are supposed to be true by construction.
#define THM_ASSERT(...) MOZ_ASSERT(__VA_ARGS__)

struct FoldToEffectiveAddress
{
    OperandId lhs, rhs;
    OperandId usesIndex, numUses;
    InstructionId last;
    InstructionId constant;

    InstructionId tmpIns;
    OperandId tmpOp;

    InstructionId base;
    InstructionId index;
    int32_t displacement;
    union {
        Scale scale;
        uint32_t bitsClearedByShift;
    };
    InstructionId lastRelative;
    BlockId lastBlock;

    enum class State {
        Exit, // Stop this transaction.
        Continue, // Go to the next step.

        // while (true) {
        CheckLastHasOneUse0,

        AddDisplacement,
        AddBase,

        // }

        CheckBase,
        ComputeBitsClearedByShift,
        CheckBaseIsRecovered
    }

    // Read opcode_, fork This structure for each Lsh
    //             , set |last|.
    State step_MatchLsh(InstructionId i, const uint32_t* opcode) {
        if (*opcode != MDefinition::Op_Lsh)
            return State::Exit;
        last = i;
        displacement = 0;
        base = UINT32_MAX;
        return State::Continue;
    }

    // Read mirNodes_, filter out if lsh->type() != MIRType_Int32.
    State step_CheckLshType_last(InstructionId i, const MNode* mir) {
        THM_ASSERT(i == last);
        return mir->toDefinition()->toLsh()->type() == MIRType_Int32 ?
            State::Continue : State::Exit;
    }

    // Read mirNodes_, filter out if lsh->isRecoveredOnBailout().
    State step_CheckLshRecoveredOnBailout_last(InstructionId i, const MNode* mir) {
        THM_ASSERT(i == last);
        return !mir->toDefinition()->toLsh()->isRecoveredOnBailout() ?
            State::Continue : State::Exit;
    }

    // Read dataFlow_, set |lhs|, |rhs|, |usesIndex| and |numUses|.
    void step_GetLshOperands_0_last(InstructionId i, const DataFlowEdges* edges) {
        THM_ASSERT(i == last);
        lhs = edges->operandsIndex;
        rhs = edges->operandsIndex + 1;
        usesIndex = edges->usesIndex;
        numUses = edges->numUses;
    }

    // Read operands_, set |lhs| and |rhs| to the uses indexes.
    void step_GetLshOperands_1_lhs(OperandId i, const OperandId* operand) {
        THM_ASSERT(i == lhs);
        lhs = *operand;
    }
    void step_GetLshOperands_1_rhs(OperandId i, const OperandId* operand) {
        THM_ASSERT(i == rhs);
        rhs = *operand;
    }

    // Read dataFlow_, set |index| and |constant| to the instructions indexes.
    void step_GetLshOperands_2_lhs(InstructionId i, const DataFlowEdges* edges) {
        THM_ASSERT(edges->usesIndex <= lhs);
        THM_ASSERT(lhs < edges->usesIndex + edges->numUses);
        index = i;
    }
    void step_GetLshOperands_2_rhs(InstructionId i, const DataFlowEdges* edges) {
        THM_ASSERT(edges->usesIndex <= rhs);
        THM_ASSERT(rhs < edges->usesIndex + edges->numUses);
        constant = i;
    }

    // Read mirNodes_, assert if type() != MIRType_Int32.
    void step_AssertLshLHSType_index(InstructionId i, const MNode* mir) {
        THM_ASSERT(i == index);
        MOZ_ASSERT(mir->toDefinition()->type() == MIRType_Int32);
    }

    // Read mirNodes_, filter out if constant->type() != MIRType_Int32.
    State step_CheckLshConstant_constant(InstructionId i, const MNode* mirRaw) {
        THM_ASSERT(i == constant);
        MConstant* mir = mirRaw->toDefinition()->toConstant();
        return mir->type() == MIRType_Int32 && IsShiftInScaleRange(mir->toInt32()) ?
            State::Continue : State::Exit;
    }

    // Read mirNodes_, set |scale|.
    void step_SetScale_constant(InstructionId i, const MNode* mirRaw) {
        THM_ASSERT(i == constant);
        MConstant* mir = mirRaw->toDefinition()->toConstant();
        scale = ShiftToScale(mir->toInt32());
    }

    // Switch state, based on the fact that the |last| instruction has one or
    // more uses.
    State step_CheckLastHasOneUse0() {
        return numUses == 1 ? State::Continue : State::CheckBase;
    }

    // Read uses_, set |tmpOp| to the operand index.
    void step_getUsesInstruction_0_usesIndex(OperandId i, const OperandId* use) {
        THM_ASSERT(i == usesIndex);
        tmpOp = *use;
    }
    // Read dataFlow_, set |tmpOp| to the operand index of the other operand
    //               , set |tmpIns| to the potential Add instruction.
    void step_GetUsesInstruction_1_tmpOp(InstructionId i, const DataFlowEdges* edges) {
        THM_ASSERT(edges->operandsIndex <= tmpOp);
        THM_ASSERT(tmpOp < edges->operandsIndex + edges->numOperands);
        tmpOp = (1 - (tmpOp - edges->usesIndex)) + edges->usesIndex;
        tmpIns = i;
    }
    // Read operands_, set |tmpOp| to the index of the other operand.
    void step_GetUsesInstruction_2_tmpOp(OperandId i, const OperandId* use) {
        THM_ASSERT(tmpOp == i);
        tmpOp = *use;
    }
    // Read dataFlow_, set |constant| to the index of the other operand instruction.
    void step_GetUsesInstruction_3_tmpOp(InstructionId i, const DataFlowEdges* edges) {
        THM_ASSERT(edges->usesIndex <= tmpOp);
        THM_ASSERT(tmpOp < edges->usesIndex + edges->numUses);
        constant = i;
    }

    // Read opcode_, switch state if tmpIns->toDefinition()->isAdd() == false
    State step_CheckIsAdd_tmpIns(InstructionId i, const uint32_t* opcode) {
        THM_ASSERT(tmpIns == i);
        return opcode == MDefinition::Op_Add ? State::Continue : State::CheckBase;
    }
    // Read mirNodes_, switch state if tmpIns->specialization() != MIRType_Int32
    State step_CheckAddIsSpecializeInt32_tmpIns(InstructionId i, const MNode* mirRaw) {
        THM_ASSERT(tmpIns == i);
        MAdd* mir = mirRaw->toDefinition()->toAdd();
        return mir->specialization() == MIRType_Int32 ? State::Continue : State::CheckBase;
    }
    // Read mirNodes_, switch state if tmpIns->isTruncated() == false
    State step_CheckAddIsTruncated_tmpIns(InstructionId i, const MNode* mirRaw) {
        THM_ASSERT(tmpIns == i);
        MAdd* mir = mirRaw->toDefinition()->toAdd();
        return mir->isTruncated() ? State::Continue : State::CheckBase;
    }
    // Read opcodes_, switch state if constant->opcode() == MDefinition::Op_Constant
    State step_CheckIsConstant_constant(InstructionId i, const uint32_t* opcode) {
        THM_ASSERT(constant == i);
        return *opcode == MDefinition::Op_Constant ? State::AddDisplacement : State::AddBase;
    }

    // Label State::AddDisplacement

    // Read mirNodes_, collect displacement.
    void step_AddDisplacement_constant(InstructionId i, const MNode* mirRaw) {
        THM_ASSERT(constant == i);
        MConstant* mir = mirRaw->toDefinition()->toConstant();
        displacement += mir->toInt32();
        // Implicit return State::SetLastAndCheckRecovered;
    }

    // Label State::AddBase

    // Copy constant into base.
    State step_AddBase_constant() {
        if (base != UINT32_MAX)
            return State::CheckBase;
        base = constant;
        return State::Continue;
    }

    // Read mirNodes_, filter out if the |last| instruction is recovered on bailout.
    State step_SetLastAndCheckRecovered_tmpIns(InstructionId i, const MNode* mirRaw) {
        THM_ASSERT(tmpIns == i);
        last = tmpIns;
        return !mirRaw->toDefinition()->isRecoveredOnBailout() ?
            State::Continue : State::Exit
    }
    // Read dataFlow_, set |numUses| and |usesIndex|.
    void step_GetLastUsesNumber_last(InstructionId i, const DataFlowEdges* edges) {
        THM_ASSERT(i == last);
        usesIndex = edges->usesIndex;
        numUses = edges->numUses;
        // Implicit return State::CheckLastHasOneUse0;
    }


    // Label State::CheckBase
    State step_CheckBase() {
        return base != UINT32_MAX ? State::ComputeBitsClearedByShift : State::CheckBaseIsRecovered;
    }


    // Label State::ComputeBitsClearedByShift

    // Compute bitsClearedByShift
    State step_ComputeBitsClearedByShift() {
        uint32_t elemSize = 1 << ScaleToShift(scale);
        bitsClearedByShift = elemSize - 1;
        return displacement & bitsClearedByShift == 0 ? State::Continue : State::Exit;
    }
    // Check last->hasOneUse()
    State step_CheckLastHasOneUse1() {
        return numUses == 1 ? State::Continue : State::Exit;
    }

    // Read uses_, set |tmpOp| to the operand index.
    void step_getUsesInstruction_4_usesIndex(OperandId i, const OperandId* use) {
        THM_ASSERT(i == usesIndex);
        tmpOp = *use;
    }
    // Read dataFlow_, set |tmpOp| to the operand index of the other operand
    //               , set |tmpIns| to the potential BitAnd instruction.
    //               , set |numUses| and |usesIndex| as we are going to copy
    //                 them to the newly added instruction.
    void step_GetUsesInstruction_5_tmpOp(InstructionId i, const DataFlowEdges* edges) {
        THM_ASSERT(edges->operandsIndex <= tmpOp);
        THM_ASSERT(tmpOp < edges->operandsIndex + edges->numOperands);
        tmpOp = (1 - (tmpOp - edges->usesIndex)) + edges->usesIndex;
        tmpIns = i;
        // :TODO: If this code were to be used in concurrent threads, we would
        // have to record this transaction index, such that other transactions
        // we can terminate this one in case of conflicts, and before we start
        // to commit it to the graph.
        usesIndex = edges->usesIndex;
        numUses = edges->numUses;
    }
    // Read operands_, set |tmpOp| to the index of the other operand.
    void step_GetUsesInstruction_6_tmpOp(OperandId i, const OperandId* use) {
        THM_ASSERT(tmpOp == i);
        tmpOp = *use;
    }
    // Read dataFlow_, set |constant| to the index of the other operand instruction.
    void step_GetUsesInstruction_7_tmpOp(InstructionId i, const DataFlowEdges* edges) {
        THM_ASSERT(edges->usesIndex <= tmpOp);
        THM_ASSERT(tmpOp < edges->usesIndex + edges->numUses);
        constant = i;
    }
    // Read opcodes_, filter out if tmpIns->opcode() != MDefinition::Op_BitAnd.
    State step_CheckBitAnd_tmpIns(InstructionId i, const uint32_t* opcode) {
        THM_ASSERT(tmpIns == i);
        return *opcode == MDefinition::Op_BitAnd ? State::Continue : State::Exit;
    }
    // Read opcodes_, filter out if cosntant->opcode() != MDefinition::Op_Constant.
    State step_CheckBitAndOtherOperandIsConstant_constant(InstructionId i, const uint32_t* opcode) {
        THM_ASSERT(constant == i);
        return *opcode == MDefinition::Op_Constant ? State::Continue : State::Exit;
    }
    // Read mirNodes_, filter out if tmpIns->isRecoveredOnBailout() == true
    State step_CheckBitAndIsRecovered_tmpIns(InstructionId i, const MNode* mirRaw) {
        THM_ASSERT(tmpIns == i);
        MDefinition* mir = mirRaw->toDefinition();
        return !mir->isRecoveredOnBailout() ? State::Continue : State::Exit;
    }
    // Read mirNodes_, filter out if constant->type() != MIRType_Int32
    State step_CheckOtherOperandType_constant(InstructionId i, const MNode* mirRaw) {
        THM_ASSERT(constant == i);
        MConstant* mir = mirRaw->toDefinition()->toConstant();
        return mir->type() == MIRType_Int32 ? State::Continue : State::Exit;
    }
    // Read mirNodes_, filter out if the mask does not clear the same bits as the shift.
    State step_CheckBitsClearedByMask_constant(InstructionId i, const MNode* mirRaw) {
        THM_ASSERT(constant == i);
        MConstant* mir = mirRaw->toDefinition()->toConstant();
        uint32_t bitsClearedByMask = ~uint32_t(mir->toInt32());
        return (bitsClearedByShift & bitsClearedByMask) == bitsClearedByMask
            ? State::Continue : State::Exit;
    }
    // Remove all uses of BitAnd.
    void step_ReplaceBitAndByItsOperand_0_tmpIns(InstructionId i, DataFlowEdges* edges) {
        THM_ASSERT(i == tmpIns);
        edges->numUses = 0;
    }
    // Replace all uses of |last| and replace them by the old uses of the
    // |BitAnd| instruction.
    void step_ReplaceBitAndByItsOperand_1_last(InstructionId i, DataFlowEdges* edges) {
        THM_ASSERT(i == last);
        // :TODO: We should probably add the current list of uses of |last| into
        // a free-list, such that we can reuse for new instructions or compact
        // the structure later uses.
        edges->usesIndex = usesIndex;
        edges->numUses = numUses;
    }

    // Label State::CheckBaseIsRecovered

    // Read mirNodes_, filter out if the base is recovered on bailout.
    State step_CheckBaseIsRecovered_base(InstructionId i, const MNode* mirRaw) {
        THM_ASSERT(i == base);
        MDefinition* mir = mirRaw->toDefinition();
        return !mir->isRecoveredOnBailout() ? State::Continue : State::Exit;
    }

    // Record the index of the operands of the newly added instruction.
    void step_InsertEffectiveAddress_setOperandIndex(OperandId i, OperandId** insertOperands) {
        tmpOp = i;
    }

    // Read dataFlow_, return the number of uses.
    uint32_t step_CollectBaseNumUses_base(InstructionId i, const DataFlowEdges* edges) {
        THM_ASSERT(i == base);
        return edges->numUses;
    }

    // Read dataFlow_, return the number of uses.
    uint32_t step_CollectIndexNumUses_index(InstructionId i, const DataFlowEdges* edges) {
        THM_ASSERT(i == index);
        return edges->numUses;
    }

    // Remove all uses of the |last| instruction.
    void step_RemoveLastUses_last(InstructionId i, DataFlowEdges* edges) {
        THM_ASSERT(i == last);
        edges->numUses = 0;
    }
    // Read instructionIndexes_, Find the location of the |last| instruction.
    void step_GetInstructionIndex_last(InstructionId i, InstructionId* indexes) {
        THM_ASSERT(*indexes == last);
        lastRelative = i;
    }
    // Read instructionRanges_, Find the block of the |last| instruction.
    void step_GetBlockIndex_last(BlockId i, InstructionRange* range) {
        THM_ASSERT(range->insStart <= lastRelative);
        THM_ASSERT(lastRelative < range->constrolId);
        lastBlock = i;
        lastRelative -= range->insStart;
    }



    bool executeForAll(THMGraph& graph) {
        DenseVector<FoldToEffectiveAddress> transactions;
        DenseVector<FoldToEffectiveAddress*> tPtr;
        size_t numTransactions = 0;
        size_t ti = 0;
        size_t removed = 0;

        const size_t sliceSize = 128;
        size_t sliceBase, sliceEnd, max;

        sliceBase = 0;
        max = graph.numInstructions_;
        for (; sliceBase < max; sliceBase = sliceEnd) {
            sliceEnd = sliceBase + sliceSize;
            if (max < sliceEnd)
                sliceEnd = max;

            // As we do not know how many instruction would match, we reserve
            // extra entries to ensure that the wrose case is covered.
            if (!transactions.growBy(numTransactions + sliceSize - transactions.length())) // growTo
                return false;
            FoldToEffectiveAddress* t = &transactions[numTransactions];

            for (InstructionId ins = sliceBase; ins < sliceEnd; ins++) {
                if (t->step_MatchLsh(ins, &opcode_[ins]) == State::Continue)
                    t++;
            }

            numTransactions += t - &transactions[numTransactions];
        }

        // No transactions.
        if (!numTransactions)
            return true;
        transactions.shrinkTo(numTransactions);

        // Make an indirection table, such that we can sort them without sorting the transactions.
        if (!tPtr.reserve(numTransactions))
            return false;
        for (ti = 0; ti < numTransactions; ti++)
            tPtr.infallibleAppend(&transactions[ti]);

        auto sortByLast = [](FoldToEffectiveAddress* a, FoldToEffectiveAddress* b) {
            return a->last < b->last;
        };
        auto sortByBase = [](FoldToEffectiveAddress* a, FoldToEffectiveAddress* b) {
            return a->base < b->base;
        };
        auto sortByLhs = [](FoldToEffectiveAddress* a, FoldToEffectiveAddress* b) {
            return a->lhs < b->lhs;
        };
        auto sortByRhs = [](FoldToEffectiveAddress* a, FoldToEffectiveAddress* b) {
            return a->rhs < b->rhs;
        };
        auto sortByIndex = [](FoldToEffectiveAddress* a, FoldToEffectiveAddress* b) {
            return a->index < b->index;
        };
        auto sortByConstant = [](FoldToEffectiveAddress* a, FoldToEffectiveAddress* b) {
            return a->constant < b->constant;
        };
        auto sortByUsesIndex = [](FoldToEffectiveAddress* a, FoldToEffectiveAddress* b) {
            return a->usesIndex < b->usesIndex;
        };
        auto sortByTmpOp = [](FoldToEffectiveAddress* a, FoldToEffectiveAddress* b) {
            return a->tmpOp < b->tmpOp;
        };
        auto sortByTmpIns = [](FoldToEffectiveAddress* a, FoldToEffectiveAddress* b) {
            return a->tmpIns < b->tmpIns;
        };


        // Next state to go to.
        DenseVector<State> tStates;
        if (!tStates.reserve(numTransactions))
            return false;


        // step_CheckLshType_last
        for (ti = 0; ti < numTransactions; ti++) {
            InstructionId ins = tPtr[ti]->last;
            tStates[ti] = tPtr[ti]->step_CheckLshType_last(ins, &mirNodes_[ins]);
        }

        removed = 0;
        for (ti = 0; ti < numTransactions; ti++) {
            tPtr[ti - removed] = tPtr[ti];
            if (tStates[ti] != State::Continue)
                removed++;
        }

        numTransactions -= removed;
        if (!numTransactions)
            return true;
        tPtr.shrinkTo(numTransactions);


        // step_CheckLshType_last
        for (ti = 0; ti < numTransactions; ti++) {
            InstructionId ins = tPtr[ti]->last;
            tStates[ti] = tPtr[ti]->step_CheckLshRecoveredOnBailout_last(ins, &mirNodes_[ins]);
        }

        removed = 0;
        for (ti = 0; ti < numTransactions; ti++) {
            tPtr[ti - removed] = tPtr[ti];
            if (tStates[ti] != State::Continue)
                removed++;
        }

        numTransactions -= removed;
        if (!numTransactions)
            return true;
        tPtr.shrinkTo(numTransactions);


        for (ti = 0; ti < numTransactions; ti++) {
            InstructionId ins = tPtr[ti]->last;
            tPtr[ti]->step_GetLshOperands_0_last(ins, &dataFlow_[ins]);
        }

        std::sort(tPtr.begin(), tPtr.end(), sortByLhs);
        for (ti = 0; ti < numTransactions; ti++) {
            OperandId i = tPtr[ti]->lhs;
            tPtr[ti]->step_GetLshOperands_1_lhs(i, &operands_[i]);
        }

        // step_GetLshOperands_1_rhs
        std::sort(tPtr.begin(), tPtr.end(), sortByRhs);
        for (ti = 0; ti < numTransactions; ti++) {
            OperandId i = tPtr[ti]->rhs;
            tPtr[ti]->step_GetLshOperands_1_rhs(i, &operands_[i]);
        }

        std::sort(tPtr.begin(), tPtr.end(), sortByLhs);
        for (ti = 0; ti < numTransactions; ti++) {
            InstructionId i = tPtr[ti]->lhs;
            tPtr[ti]->step_GetLshOperands_1_lhs(i, &dataFlow_[i]);
        }

        std::sort(tPtr.begin(), tPtr.end(), sortByRhs);
        for (ti = 0; ti < numTransactions; ti++) {
            InstructionId i = tPtr[ti]->rhs;
            tPtr[ti]->step_GetLshOperands_1_rhs(i, &dataFlow_[i]);
        }

#ifdef DEBUG
        std::sort(tPtr.begin(), tPtr.end(), sortByIndex);
        for (ti = 0; ti < numTransactions; ti++) {
            InstructionId i = tPtr[ti]->index;
            tPtr[ti]->step_AssertLshLHSType_index(i, &mirNodes_[i]);
        }
#endif

        std::sort(tPtr.begin(), tPtr.end(), sortByConstant);
        for (ti = 0; ti < numTransactions; ti++) {
            InstructionId i = tPtr[ti]->constant;
            tPtr[ti]->step_CheckLshConstant_constant(i, &mirNodes_[i]);
        }

        for (ti = 0; ti < numTransactions; ti++) {
            InstructionId i = tPtr[ti]->constant;
            tPtr[ti]->step_SetScale_constant(i, &mirNodes_[i]);
        }

        size_t numWaitTransactions = 0;
        DenseVector<FoldToEffectiveAddress*> tPtrWait;
        if (!tPtrWait.reserve(numTransactions))
            return false;

        size_t numBaseOperands = 0;
        DenseVector<FoldToEffectiveAddress*> tPtrIsBase;
        if (!tPtrIsBase.reserve(numTransactions))
            return false;

        while (true) {
            for (ti = 0; ti < numTransactions; ti++)
                tStates[ti] = tPtr[ti]->step_CheckLastHasOneUse0();

            removed = 0;
            for (ti = 0; ti < numTransactions; ti++) {
                if (tStates[ti] == State::CheckBase) {
                    tPtrWait[numWaitTransactions++] = tPtr[ti];
                    removed++;
                } else {
                    tPtr[ti - removed] = tPtr[ti];
                }
            }

            numWaitTransactions += removed;
            numTransactions -= removed;
            if (!numTransactions)
                break;

            std::sort(tPtr.begin(), tPtr.end(), sortByUsesIndex);
            for (ti = 0; ti < numTransactions; ti++) {
                OperandId i = tPtr[ti]->usesIndex;
                tPtr[ti]->step_SetScale_constant(i, &uses_[i]);
            }

            std::sort(tPtr.begin(), tPtr.end(), sortByTmpOp);
            for (ti = 0; ti < numTransactions; ti++) {
                InstructionId i = tPtr[ti]->tmpOp;
                tPtr[ti]->step_GetUsesInstruction_1_tmpOp(i, &dataFlow_[i]);
            }

            std::sort(tPtr.begin(), tPtr.end(), sortByTmpOp);
            for (ti = 0; ti < numTransactions; ti++) {
                OperandId i = tPtr[ti]->tmpOp;
                tPtr[ti]->step_GetUsesInstruction_2_tmpOp(i, &operands_[i]);
            }

            std::sort(tPtr.begin(), tPtr.end(), sortByTmpOp);
            for (ti = 0; ti < numTransactions; ti++) {
                InstructionId i = tPtr[ti]->tmpOp;
                tPtr[ti]->step_GetUsesInstruction_1_tmpOp(i, &dataFlow_[i]);
            }

            std::sort(tPtr.begin(), tPtr.end(), sortByTmpIns);
            for (ti = 0; ti < numTransactions; ti++) {
                InstructionId i = tPtr[ti]->tmpIns;
                tStates[ti] = tPtr[ti]->step_CheckIsAdd_tmpIns(i, &opcode_[i]);
            }

            removed = 0;
            for (ti = 0; ti < numTransactions; ti++) {
                if (tStates[ti] == State::CheckBase) {
                    tPtrWait[numWaitTransactions++] = tPtr[ti];
                    removed++;
                } else {
                    tPtr[ti - removed] = tPtr[ti];
                }
            }

            numWaitTransactions += removed;
            numTransactions -= removed;
            if (!numTransactions)
                break;

            for (ti = 0; ti < numTransactions; ti++) {
                InstructionId i = tPtr[ti]->tmpIns;
                tStates[ti] = tPtr[ti]->step_CheckAddIsSpecializeInt32_tmpIns(i, &opcode_[i]);
            }

            removed = 0;
            for (ti = 0; ti < numTransactions; ti++) {
                if (tStates[ti] == State::CheckBase) {
                    tPtrWait[numWaitTransactions++] = tPtr[ti];
                    removed++;
                } else {
                    tPtr[ti - removed] = tPtr[ti];
                }
            }

            numWaitTransactions += removed;
            numTransactions -= removed;
            if (!numTransactions)
                break;

            for (ti = 0; ti < numTransactions; ti++) {
                InstructionId i = tPtr[ti]->tmpIns;
                tStates[ti] = tPtr[ti]->step_CheckAddIsTruncated_tmpIns(i, &opcode_[i]);
            }

            removed = 0;
            for (ti = 0; ti < numTransactions; ti++) {
                if (tStates[ti] == State::CheckBase) {
                    tPtrWait[numWaitTransactions++] = tPtr[ti];
                    removed++;
                } else {
                    tPtr[ti - removed] = tPtr[ti];
                }
            }

            numWaitTransactions += removed;
            numTransactions -= removed;
            if (!numTransactions)
                break;

            std::sort(tPtr.begin(), tPtr.end(), sortByConstant);
            for (ti = 0; ti < numTransactions; ti++) {
                InstructionId i = tPtr[ti]->constant;
                tStates[ti] = tPtr[ti]->step_CheckIsConstant_constant(i, &opcode_[i]);
            }

            numBaseOperands = 0;
            for (ti = 0; ti < numTransactions; ti++) {
                if (tStates[ti] == State::AddBase)
                    tPtrIsBase[numBaseOperands++] = tPtr[ti];
                else
                    tPtr[ti - numBaseOperands] = tPtr[ti];
            }
            numTransactions -= numBaseOperands;

            do { // used to skip forward with break.
                if (!numTransactions)
                    break;
                
                for (ti = 0; ti < numTransactions; ti++) {
                    InstructionId i = tPtr[ti]->constant;
                    step_AddDisplacement_constant(i, mirNodes_[i]);
                }
            } while (false);

            do {
                if (!numBaseOperands)
                    break;

                for (ti = 0; ti < numBaseOperands; ti++)
                    tStates[ti] = tPtrIsBase[ti]->step_AddBase_constant();

                removed = 0;
                for (ti = 0; ti < numBaseOperands; ti++) {
                    if (tStates[ti] == State::CheckBase) {
                        tPtrWait[numWaitTransactions++] = tPtrIsBase[ti];
                        removed++;
                    } else {
                        tPtrIsBase[ti - removed] = tPtrIsBase[ti];
                    }
                }

                numWaitTransactions += removed;
                numBaseOperands -= removed;

                for (ti = 0; ti < numBaseOperands; ti++)
                    tPtr[numTransactions++] = tPtrIsBase[ti];

            } while (false);

            if (!numTransactions)
                break;

            std::sort(tPtr.begin(), tPtr.end(), sortByTmpIns);
            for (ti = 0; ti < numTransactions; ti++) {
                InstructionId i = tPtr[ti]->tmpIns;
                tStates[ti] = tPtr[ti]->step_SetLastAndCheckRecovered_tmpIns(i, mirNodes_[i]);
            }

            removed = 0;
            for (ti = 0; ti < numTransactions; ti++) {
                tPtr[ti - removed] = tPtr[ti];
                if (tStates[ti] != State::Continue)
                    removed++;
            }

            numTransactions -= removed;
            if (!numTransactions)
                return true;

            // Already sorted by last (last == tmpIns)
            // std::sort(tPtr.begin(), tPtr.end(), sortByLast);
            for (ti = 0; ti < numTransactions; ti++) {
                InstructionId i = tPtr[ti]->last;
                step_GetLastUsesNumber_last(i, dataFlow_[i]);
            }
        }

        tPtr = Move(tPtrWait);
        numTransactions = numWaitTransactions;
        if (!numTransactions)
            return true;

        for (ti = 0; ti < numTransactions; ti++)
            tStates[ti] = tPtr[ti]->step_CheckBase();

        numBaseOperands = 0;
        for (ti = 0; ti < numTransactions; ti++) {
            if (tStates[ti] == State::CheckBaseIsRecovered)
                tPtrIsBase[numBaseOperands++] = tPtr[ti];
            else
                tPtr[ti - numBaseOperands] = tPtr[ti];
        }
        numTransactions -= numBaseOperands;

        do { // used to skip forward with break.
            if (!numTransactions)
                break;

            for (ti = 0; ti < numTransactions; ti++)
                tStates[ti] = tPtr[ti]->step_ComputeBitsClearedByShift();

            removed = 0;
            for (ti = 0; ti < numTransactions; ti++) {
                tPtr[ti - removed] = tPtr[ti];
                if (tStates[ti] != State::Continue)
                    removed++;
            }

            numTransactions -= removed;
            if (!numTransactions)
                return true;
            tPtr.shrinkTo(numTransactions);

            for (ti = 0; ti < numTransactions; ti++)
                tStates[ti] = tPtr[ti]->step_CheckLastHasOneUse1();

            removed = 0;
            for (ti = 0; ti < numTransactions; ti++) {
                tPtr[ti - removed] = tPtr[ti];
                if (tStates[ti] != State::Continue)
                    removed++;
            }

            numTransactions -= removed;
            if (!numTransactions)
                return true;
            tPtr.shrinkTo(numTransactions);

            std::sort(tPtr.begin(), tPtr.end(), sortByUsesIndex);
            for (ti = 0; ti < numTransactions; ti++) {
                OperandId i = tPtr[ti]->usesIndex;
                tPtr[ti]->step_getUsesInstruction_4_usesIndex(i, uses[i]);
            }

            std::sort(tPtr.begin(), tPtr.end(), sortByTmpOp);
            for (ti = 0; ti < numTransactions; ti++) {
                InstructionId i = tPtr[ti]->tmpOp;
                tPtr[ti]->step_GetUsesInstruction_5_tmpOp(i, dataFlow_[i]);
            }

            std::sort(tPtr.begin(), tPtr.end(), sortByTmpOp);
            for (ti = 0; ti < numTransactions; ti++) {
                OperandId i = tPtr[ti]->tmpOp;
                tPtr[ti]->step_GetUsesInstruction_6_tmpOp(i, operands_[i]);
            }

            std::sort(tPtr.begin(), tPtr.end(), sortByTmpOp);
            for (ti = 0; ti < numTransactions; ti++) {
                InstructionId i = tPtr[ti]->tmpOp;
                tPtr[ti]->step_GetUsesInstruction_7_tmpOp(i, dataFlow_[i]);
            }

            std::sort(tPtr.begin(), tPtr.end(), sortByTmpIns);
            for (ti = 0; ti < numTransactions; ti++) {
                InstructionId i = tPtr[ti]->tmpIns;
                tPtr[ti]->step_CheckBitAnd_tmpIns(i, opcodes_[i]);
            }

            std::sort(tPtr.begin(), tPtr.end(), sortByConstant);
            for (ti = 0; ti < numTransactions; ti++) {
                InstructionId i = tPtr[ti]->constant;
                tPtr[ti]->step_CheckBitAndOtherOperandIsConstant_constant(i, opcodes_[i]);
            }

            std::sort(tPtr.begin(), tPtr.end(), sortByTmpIns);
            for (ti = 0; ti < numTransactions; ti++) {
                InstructionId i = tPtr[ti]->tmpIns;
                tPtr[ti]->step_CheckBitAndIsRecovered_tmpIns(i, mirNodes_[i]);
            }

            std::sort(tPtr.begin(), tPtr.end(), sortByTmpIns);
            for (ti = 0; ti < numTransactions; ti++) {
                InstructionId i = tPtr[ti]->tmpIns;
                tStates[ti] = tPtr[ti]->step_CheckBitAndIsRecovered_tmpIns(i, mirNodes_[i]);
            }

            removed = 0;
            for (ti = 0; ti < numTransactions; ti++) {
                tPtr[ti - removed] = tPtr[ti];
                if (tStates[ti] != State::Continue)
                    removed++;
            }

            numTransactions -= removed;
            if (!numTransactions)
                return true;
            tPtr.shrinkTo(numTransactions);

            std::sort(tPtr.begin(), tPtr.end(), sortByConstant);
            for (ti = 0; ti < numTransactions; ti++) {
                InstructionId i = tPtr[ti]->constant;
                tStates[ti] = tPtr[ti]->step_CheckBitsClearedByMask_constant(i, &mirNodes_[i]);
            }

            removed = 0;
            for (ti = 0; ti < numTransactions; ti++) {
                tPtr[ti - removed] = tPtr[ti];
                if (tStates[ti] != State::Continue)
                    removed++;
            }

            numTransactions -= removed;
            if (!numTransactions)
                return true;
            tPtr.shrinkTo(numTransactions);

            std::sort(tPtr.begin(), tPtr.end(), sortByConstant);
            for (ti = 0; ti < numTransactions; ti++) {
                InstructionId i = tPtr[ti]->constant;
                tStates[ti] = tPtr[ti]->step_CheckBitsClearedByMask_constant(i, &mirNodes_[i]);
            }

            // Start making transformations!  Note, doing this is fine, even if
            // we have other transactions in the pipeline, because the set of
            // instructions covered by each transaction does not overlap/conflict.

            std::sort(tPtr.begin(), tPtr.end(), sortByTmpIns);
            for (ti = 0; ti < numTransactions; ti++) {
                InstructionId i = tPtr[ti]->tmpIns;
                tPtr[ti]->step_ReplaceBitAndByItsOperand_0_tmpIns(i, &dataFlow_[i]);
            }

            std::sort(tPtr.begin(), tPtr.end(), sortByLast);
            for (ti = 0; ti < numTransactions; ti++) {
                InstructionId i = tPtr[ti]->last;
                step_ReplaceBitAndByItsOperand_1_last(i, &dataFlow_[i]);
            }

            tPtr.clearAndFree();
        } while(false);

        tPtr = Move(tPtrIsBase);
        do { // used to skip forward with break.
            if (!numBaseOperands)
                break;

            std::sort(tPtr.begin(), tPtr.end(), sortByBase);
            for (ti = 0; ti < numBaseOperands; ti++) {
                InstructionId i = tPtr[ti]->constant;
                tStates[ti] = tPtr[ti]->step_CheckBaseIsRecovered_base(i, &mirNodes_[i]);
            }

            removed = 0;
            for (ti = 0; ti < numBaseOperands; ti++) {
                tPtr[ti - removed] = tPtr[ti];
                if (tStates[ti] != State::Continue)
                    removed++;
            }

            numBaseOperands -= removed;
            if (!numBaseOperands)
                return true;
            tPtr.shrinkTo(numBaseOperands);

            // Apply each transaction one after the other as I am too lazy to go
            // through the complexity of dealing with having |inxed| and |base|
            // instruction which have more than one added effective address uses
            // at a time.
            for (ti = 0; ti < numBaseOperands; ti++) {
                FoldToEffectiveAddress* t = tPtr[ti];

                // Create the new instruction, create its operands and steal the
                // uses of the last instruction.
                numOperands_ += 2;
                if (!operands_.appendN(UINT32_MAX, 2))
                    return false;

                numInstructions_ += 1;
                if (!opcodes_.append(MDefinition::Op_EffectiveAddress))
                    return false;
                DataFlowEdges dataFlowDummy = { UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX };
                if (!dataFlow_.append(dataFlowDummy))
                    return false;
                if (!mirNodes_.append(nullptr))
                    return false;

                InstructionId* ins = opcodes_.last();
                MNode** insNode = mirNodes_.last();
                DataFlowEdges* insEdge = dataFlow_.last();
                MInstruction* baseMir = mirRaw[base]->toDefinition()->toInstruction();
                MInstruction* indexMir = mirRaw[index]->toDefinition()->toInstruction();
                MEffectiveAddress* eaddr = MEffectiveAddress::New(alloc, baseMir, indexMir, t->scale, t->displacement);
                *insNode = eaddr;
                insEdge->operandsIndex = numOperands_ - 2;
                insEdge->numOperands = 2;
                insEdge->usesIndex = t->usesIndex;
                insEdge->numUses = t->numUses;

                // Remove all |last| uses.
                dataFlow_[t->last]->numUses = 0;

                // Copy the vector of uses of |base|, and add one use.
                DataFlowEdges* baseEdges = &dataFlow_[t->base];
                size_t newBaseUsesIndex =  numUses_;
                if (!uses_.appendN(UINT32_MAX, baseEdges->numUses + 1))
                    return false;
                numUses_ += baseEdges->numUses + 1;
                memcpy(&uses_[newBaseUsesIndex], &uses_[baseEdges->usesIndex], sizeof(OperandId) * baseEdges->numUses);
                memset(&uses_[baseEdges->usesIndex], 0xff, sizeof(OperandId) * baseEdges->numUses);
                baseEdges->usesIndex = newBaseUsesIndex;
                baseEdges->numUses += 1;
                uses_[newBaseUsesIndex + baseEdges->numUses - 1] = numOperands_ - 2;
                operands_[numOperands_ - 2] = newBaseUsesIndex + baseEdges->numUses - 1;

                // Copy the vector of uses of |index|, and add one use.
                DataFlowEdges* indexEdges = &dataFlow_[t->index];
                size_t newIndexUsesIndex =  numUses_;
                if (!uses_.appendN(UINT32_MAX, indexEdges->numUses + 1))
                    return false;
                numUses_ += indexEdges->numUses + 1;
                memcpy(&uses_[newIndexUsesIndex], &uses_[indexEdges->usesIndex], sizeof(OperandId) * indexEdges->numUses);
                memset(&uses_[indexEdges->usesIndex], 0xff, sizeof(OperandId) * indexEdges->numUses);
                indexEdges->usesIndex = newIndexUsesIndex;
                indexEdges->numUses += 1;
                uses_[newIndexUsesIndex + indexEdges->numUses - 1] = numOperands_ - 1;
                operands_[numOperands_ - 1] = newIndexUsesIndex + indexEdges->numUses - 1;

                // Lookup for the block which contains the current instruction index.
                // Copy the basic block instruction range to add the new instruction in it.
                
            }


            std::sort(tPtr.begin(), tPtr.end(), sortByLast);
            for (ti = 0; ti < numBaseOperands; ti++) {
                InstructionId ins = tPtr[ti]->last;
                numMovedUses += tPtr[ti]->step_RemoveLastUses_last(ins, &dataFlow_[ins]);
            }

            // Note, we do not add a new instruction, we replace the old one.
            for (ti = 0; ti < numBaseOperands; ti++) {
                InstructionId ins = tPtr[ti]->last;
                numMovedUses += tPtr[ti]->step_RemoveLastUses_last(ins, &instructionIndexes_[ins]);
            }

            // :TODO: Insert a new instruction after |last| inside the MIR graph.
        } while(false);

        return true;
    }
};



static void
AnalyzeLsh(TempAllocator& alloc, MLsh* lsh)
{
    if (lsh->specialization() != MIRType_Int32)
        return;

    if (lsh->isRecoveredOnBailout())
        return;

    MDefinition* index = lsh->lhs();
    MOZ_ASSERT(index->type() == MIRType_Int32);

    MConstant* shiftValue = lsh->rhs()->maybeConstantValue();
    if (!shiftValue)
        return;

    if (shiftValue->type() != MIRType_Int32 || !IsShiftInScaleRange(shiftValue->toInt32()))
        return;

    Scale scale = ShiftToScale(shiftValue->toInt32());

    int32_t displacement = 0;
    MInstruction* last = lsh;
    MDefinition* base = nullptr;
    while (true) {
        if (!last->hasOneUse())
            break;

        MUseIterator use = last->usesBegin();
        if (!use->consumer()->isDefinition() || !use->consumer()->toDefinition()->isAdd())
            break;

        MAdd* add = use->consumer()->toDefinition()->toAdd();
        if (add->specialization() != MIRType_Int32 || !add->isTruncated())
            break;

        MDefinition* other = add->getOperand(1 - add->indexOf(*use));

        if (MConstant* otherConst = other->maybeConstantValue()) {
            displacement += otherConst->toInt32();
        } else {
            if (base)
                break;
            base = other;
        }

        last = add;
        if (last->isRecoveredOnBailout())
            return;
    }

    if (!base) {
        uint32_t elemSize = 1 << ScaleToShift(scale);
        if (displacement % elemSize != 0)
            return;

        if (!last->hasOneUse())
            return;

        MUseIterator use = last->usesBegin();
        if (!use->consumer()->isDefinition() || !use->consumer()->toDefinition()->isBitAnd())
            return;

        MBitAnd* bitAnd = use->consumer()->toDefinition()->toBitAnd();
        if (bitAnd->isRecoveredOnBailout())
            return;

        MDefinition* other = bitAnd->getOperand(1 - bitAnd->indexOf(*use));
        MConstant* otherConst = other->maybeConstantValue();
        if (!otherConst || otherConst->type() != MIRType_Int32)
            return;

        uint32_t bitsClearedByShift = elemSize - 1;
        uint32_t bitsClearedByMask = ~uint32_t(otherConst->toInt32());
        if ((bitsClearedByShift & bitsClearedByMask) != bitsClearedByMask)
            return;

        bitAnd->replaceAllUsesWith(last);
        return;
    }

    if (base->isRecoveredOnBailout())
        return;

    MEffectiveAddress* eaddr = MEffectiveAddress::New(alloc, base, index, scale, displacement);
    last->replaceAllUsesWith(eaddr);
    last->block()->insertAfter(last, eaddr);
}

template<typename MAsmJSHeapAccessType>
bool
EffectiveAddressAnalysis::tryAddDisplacement(MAsmJSHeapAccessType* ins, int32_t o)
{
    // Compute the new offset. Check for overflow and negative. In theory it
    // ought to be possible to support negative offsets, but it'd require
    // more elaborate bounds checking mechanisms than we currently have.
    MOZ_ASSERT(ins->offset() >= 0);
    int32_t newOffset = uint32_t(ins->offset()) + o;
    if (newOffset < 0)
        return false;

    // Compute the new offset to the end of the access. Check for overflow
    // and negative here also.
    int32_t newEnd = uint32_t(newOffset) + ins->byteSize();
    if (newEnd < 0)
        return false;
    MOZ_ASSERT(uint32_t(newEnd) >= uint32_t(newOffset));

    // Determine the range of valid offsets which can be folded into this
    // instruction and check whether our computed offset is within that range.
    size_t range = mir_->foldableOffsetRange(ins);
    if (size_t(newEnd) > range)
        return false;

    // Everything checks out. This is the new offset.
    ins->setOffset(newOffset);
    return true;
}

template<typename MAsmJSHeapAccessType>
void
EffectiveAddressAnalysis::analyzeAsmHeapAccess(MAsmJSHeapAccessType* ins)
{
    MDefinition* ptr = ins->ptr();

    if (ptr->isConstant()) {
        // Look for heap[i] where i is a constant offset, and fold the offset.
        // By doing the folding now, we simplify the task of codegen; the offset
        // is always the address mode immediate. This also allows it to avoid
        // a situation where the sum of a constant pointer value and a non-zero
        // offset doesn't actually fit into the address mode immediate.
        int32_t imm = ptr->toConstant()->toInt32();
        if (imm != 0 && tryAddDisplacement(ins, imm)) {
            MInstruction* zero = MConstant::New(graph_.alloc(), Int32Value(0));
            ins->block()->insertBefore(ins, zero);
            ins->replacePtr(zero);
        }
    } else if (ptr->isAdd()) {
        // Look for heap[a+i] where i is a constant offset, and fold the offset.
        // Alignment masks have already been moved out of the way by the
        // Alignment Mask Analysis pass.
        MDefinition* op0 = ptr->toAdd()->getOperand(0);
        MDefinition* op1 = ptr->toAdd()->getOperand(1);
        if (op0->isConstant())
            mozilla::Swap(op0, op1);
        if (op1->isConstant()) {
            int32_t imm = op1->toConstant()->toInt32();
            if (tryAddDisplacement(ins, imm))
                ins->replacePtr(op0);
        }
    }
}

// This analysis converts patterns of the form:
//   truncate(x + (y << {0,1,2,3}))
//   truncate(x + (y << {0,1,2,3}) + imm32)
// into a single lea instruction, and patterns of the form:
//   asmload(x + imm32)
//   asmload(x << {0,1,2,3})
//   asmload((x << {0,1,2,3}) + imm32)
//   asmload((x << {0,1,2,3}) & mask)            (where mask is redundant with shift)
//   asmload(((x << {0,1,2,3}) + imm32) & mask)  (where mask is redundant with shift + imm32)
// into a single asmload instruction (and for asmstore too).
//
// Additionally, we should consider the general forms:
//   truncate(x + y + imm32)
//   truncate((y << {0,1,2,3}) + imm32)
bool
EffectiveAddressAnalysis::analyze()
{
    for (ReversePostorderIterator block(graph_.rpoBegin()); block != graph_.rpoEnd(); block++) {
        for (MInstructionIterator i = block->begin(); i != block->end(); i++) {
            // Note that we don't check for MAsmJSCompareExchangeHeap
            // or MAsmJSAtomicBinopHeap, because the backend and the OOB
            // mechanism don't support non-zero offsets for them yet.
            if (i->isLsh())
                AnalyzeLsh(graph_.alloc(), i->toLsh());
            else if (i->isAsmJSLoadHeap())
                analyzeAsmHeapAccess(i->toAsmJSLoadHeap());
            else if (i->isAsmJSStoreHeap())
                analyzeAsmHeapAccess(i->toAsmJSStoreHeap());
        }
    }
    return true;
}
