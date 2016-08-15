/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/EffectiveAddressAnalysis.h"
#include "jit/MIR.h"
#include "jit/MIRGraph.h"

using namespace js;
using namespace jit;

#ifdef ENABLE_THM

// Matches patterns like:
//
//    base + ( ( index << {1,2,4,8} ) + disp:Int32 )
//
// And replaces them by:
//
//   EffectiveAddress(base, index, scale, disp)
//
static bool
AnalyzeLsh(MIRGraph& graph, TempAllocator& alloc)
{
    if (graph.opcodesInstructions[MDefinition::Op_Lsh].length() == 0)
        return true;
    if (graph.opcodesInstructions[MDefinition::Op_Constant].length() == 0)
        return true;
    if (graph.opcodesInstructions[MDefinition::Op_Lsh].length() == 0)
        return true;

    struct Matches {
        MLsh* lsh;
        Scale scale;
        int32_t displacement;
        MInstruction* last;
        MDefinition* base;
        bool done; // should be filtered out when copied into a new array.
    };

    Vector<Matches, 0, SystemAllocPolicy> matches;

    // Filter all potential left shifts which are shifted by a number which can
    // be converted into a scale of an effective address multiplier.
    if (!matches.reserve(graph.opcodesInstructions[MDefinition::Op_Lsh].length()))
        return false;
    for (MInstruction* ins : graph.opcodesInstructions[MDefinition::Op_Lsh]) {
        MLsh* lsh = ins->toLsh();
        if (lsh->isDiscarded())
            continue;
        if (lsh->specialization() != MIRType::Int32)
            continue;
        if (lsh->isRecoveredOnBailout())
            continue;
        MConstant* shiftValue = lsh->rhs()->maybeConstantValue();
        if (!shiftValue)
            continue;
        if (shiftValue->type() != MIRType::Int32 || !IsShiftInScaleRange(shiftValue->toInt32()))
            continue;
        Scale scale = ShiftToScale(shiftValue->toInt32());
        matches.infallibleAppend(Matches{ lsh, scale, 0, lsh, nullptr, false });
    }
    // TODO: shrinkTo length,

    if (matches.length() == 0)
        return true;

    for (Matches& m : matches) {
        while (true) {
            if (!m.last->hasOneUse())
                break;

            MUseIterator use = m.last->usesBegin();
            if (!use->consumer()->isDefinition() || !use->consumer()->toDefinition()->isAdd())
                break;

            MAdd* add = use->consumer()->toDefinition()->toAdd();
            if (add->specialization() != MIRType::Int32 || !add->isTruncated())
                break;

            MDefinition* other = add->getOperand(1 - add->indexOf(*use));

            if (MConstant* otherConst = other->maybeConstantValue()) {
                m.displacement += otherConst->toInt32();
            } else {
                if (m.base)
                    break;
                m.base = other;
            }

            m.last = add;
            if (m.last->isRecoveredOnBailout()) {
                m.done = true;
                break;
            }
        }
    }

    // Note, the next 2 loops can run concurrently.

    for (Matches& m : matches) {
        if (m.base)
            continue;
        if (m.done)
            continue;
        m.done = true;

        uint32_t elemSize = 1 << ScaleToShift(m.scale);
        if (m.displacement % elemSize != 0)
            continue;
        if (!m.last->hasOneUse())
            continue;
        MUseIterator use = m.last->usesBegin();
        if (!use->consumer()->isDefinition() || !use->consumer()->toDefinition()->isBitAnd())
            continue;
        MBitAnd* bitAnd = use->consumer()->toDefinition()->toBitAnd();
        if (bitAnd->isRecoveredOnBailout())
            continue;
        MDefinition* other = bitAnd->getOperand(1 - bitAnd->indexOf(*use));
        MConstant* otherConst = other->maybeConstantValue();
        if (!otherConst || otherConst->type() != MIRType::Int32)
            continue;
        uint32_t bitsClearedByShift = elemSize - 1;
        uint32_t bitsClearedByMask = ~uint32_t(otherConst->toInt32());
        if ((bitsClearedByShift & bitsClearedByMask) != bitsClearedByMask)
            continue;

        bitAnd->replaceAllUsesWith(m.last);
    }

    for (Matches& m : matches) {
        if (!m.base)
            continue;
        if (m.done)
            continue;
        m.done = true;

        if (m.base->isRecoveredOnBailout())
            continue;

        MEffectiveAddress* eaddr = MEffectiveAddress::New(alloc, m.base, m.lsh->lhs(), m.scale, m.displacement);
        m.last->replaceAllUsesWith(eaddr);
        m.last->block()->insertAfter(m.last, eaddr);
    }

    return true;
}

static bool
TryAddDisplacement(MIRGenerator* mir, MWasmMemoryAccess* ins, int32_t o)
{
    // Compute the new offset. Check for overflow.
    uint32_t oldOffset = ins->offset();
    uint32_t newOffset = oldOffset + o;
    if (o < 0 ? (newOffset >= oldOffset) : (newOffset < oldOffset))
        return false;

    // Compute the new offset to the end of the access. Check for overflow
    // here also.
    uint32_t newEnd = newOffset + ins->byteSize();
    if (newEnd < newOffset)
        return false;

    // Determine the range of valid offsets which can be folded into this
    // instruction and check whether our computed offset is within that range.
    size_t range = mir->foldableOffsetRange(ins);
    if (size_t(newEnd) > range)
        return false;

    // Everything checks out. This is the new offset.
    ins->setOffset(newOffset);
    return true;
}

static bool
AnalyzeAsmHeapAccess(MIRGraph& graph, MIRGenerator* mir)
{
    size_t len = graph.opcodesInstructions[MDefinition::Op_AsmJSLoadHeap].length();
    len += graph.opcodesInstructions[MDefinition::Op_AsmJSStoreHeap].length();
    if (len == 0)
        return true;

    struct CstMatches {
        MInstruction* ins;
        MWasmMemoryAccess* wma;
        MDefinition* base;
        int32_t imm;
    };
    Vector<CstMatches, 0, SystemAllocPolicy> insWithConstantBase;

    struct AddMatches {
        MInstruction* ins;
        MWasmMemoryAccess* wma;
        MDefinition* base;
    };
    Vector<AddMatches, 0, SystemAllocPolicy> insWithAddBase;

    if (!insWithConstantBase.reserve(len))
        return false;
    if (!insWithAddBase.reserve(len))
        return false;

    for (MInstruction* insIt : graph.opcodesInstructions[MDefinition::Op_AsmJSLoadHeap]) {
        MAsmJSLoadHeap* ins = insIt->toAsmJSLoadHeap();
        if (ins->isDiscarded())
            continue;
        MDefinition* base = ins->base();
        if (base->isConstant())
            insWithConstantBase.infallibleAppend(CstMatches{ ins, ins, base, 0 });
        else if (base->isAdd())
            insWithAddBase.infallibleAppend(AddMatches{ ins, ins, base });
    }

    for (MInstruction* insIt : graph.opcodesInstructions[MDefinition::Op_AsmJSStoreHeap]) {
        MAsmJSStoreHeap* ins = insIt->toAsmJSStoreHeap();
        if (ins->isDiscarded())
            continue;
        MDefinition* base = ins->base();
        if (base->isConstant())
            insWithConstantBase.infallibleAppend(CstMatches{ ins, ins, base, 0 });
        else if (base->isAdd())
            insWithAddBase.infallibleAppend(AddMatches{ ins, ins, base });
    }

    // Look for heap[i] where i is a constant offset, and fold the offset.
    // By doing the folding now, we simplify the task of codegen; the offset
    // is always the address mode immediate. This also allows it to avoid
    // a situation where the sum of a constant pointer value and a non-zero
    // offset doesn't actually fit into the address mode immediate.
    for (CstMatches& m : insWithConstantBase) {
        m.imm = m.base->toConstant()->toInt32();
    }

    for (CstMatches& m : insWithConstantBase) {
        if (m.imm == 0)
            continue;
        if (!TryAddDisplacement(mir, m.wma, m.imm))
            continue;

        MInstruction* zero = MConstant::New(graph.alloc(), Int32Value(0));
        m.ins->block()->insertBefore(m.ins, zero);
        if (m.ins->isAsmJSLoadHeap())
            m.ins->toAsmJSLoadHeap()->replaceBase(zero);
        else
            m.ins->toAsmJSStoreHeap()->replaceBase(zero);
    }

    for (CstMatches& m : insWithConstantBase) {
        // If the index is within the minimum heap length, we can optimize
        // away the bounds check.
        if (m.imm < 0)
            continue;
        int32_t end = uint32_t(m.imm) + m.wma->byteSize();
        if (end < m.imm)
            continue;
        if (uint32_t(end) > mir->minAsmJSHeapLength())
            continue;

        m.wma->removeBoundsCheck();
    }

    // Look for heap[a+i] where i is a constant offset, and fold the offset.
    // Alignment masks have already been moved out of the way by the
    // Alignment Mask Analysis pass.
    for (AddMatches& m : insWithAddBase) {
        MDefinition* op0 = m.base->toAdd()->getOperand(0);
        MDefinition* op1 = m.base->toAdd()->getOperand(1);
        if (op0->isConstant())
            mozilla::Swap(op0, op1);
        if (!op1->isConstant())
            continue;
        int32_t imm = op1->toConstant()->toInt32();
        if (!TryAddDisplacement(mir, m.wma, imm))
            continue;

        if (m.ins->isAsmJSLoadHeap())
            m.ins->toAsmJSLoadHeap()->replaceBase(op0);
        else
            m.ins->toAsmJSStoreHeap()->replaceBase(op0);
    }

    return true;
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
    // Note that we don't check for MAsmJSCompareExchangeHeap
    // or MAsmJSAtomicBinopHeap, because the backend and the OOB
    // mechanism don't support non-zero offsets for them yet
    // (TODO bug 1254935).
    if (!AnalyzeLsh(graph_, graph_.alloc()))
        return false;
    if (!AnalyzeAsmHeapAccess(graph_, mir_))
        return false;
    return true;
}

#else

static void
AnalyzeLsh(TempAllocator& alloc, MLsh* lsh)
{
    if (lsh->specialization() != MIRType::Int32)
        return;

    if (lsh->isRecoveredOnBailout())
        return;

    MDefinition* index = lsh->lhs();
    MOZ_ASSERT(index->type() == MIRType::Int32);

    MConstant* shiftValue = lsh->rhs()->maybeConstantValue();
    if (!shiftValue)
        return;

    if (shiftValue->type() != MIRType::Int32 || !IsShiftInScaleRange(shiftValue->toInt32()))
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
        if (add->specialization() != MIRType::Int32 || !add->isTruncated())
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
        if (!otherConst || otherConst->type() != MIRType::Int32)
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

template<typename MWasmMemoryAccessType>
bool
EffectiveAddressAnalysis::tryAddDisplacement(MWasmMemoryAccessType* ins, int32_t o)
{
    // Compute the new offset. Check for overflow.
    uint32_t oldOffset = ins->offset();
    uint32_t newOffset = oldOffset + o;
    if (o < 0 ? (newOffset >= oldOffset) : (newOffset < oldOffset))
        return false;

    // Compute the new offset to the end of the access. Check for overflow
    // here also.
    uint32_t newEnd = newOffset + ins->byteSize();
    if (newEnd < newOffset)
        return false;

    // Determine the range of valid offsets which can be folded into this
    // instruction and check whether our computed offset is within that range.
    size_t range = mir_->foldableOffsetRange(ins);
    if (size_t(newEnd) > range)
        return false;

    // Everything checks out. This is the new offset.
    ins->setOffset(newOffset);
    return true;
}

template<typename MWasmMemoryAccessType>
void
EffectiveAddressAnalysis::analyzeAsmHeapAccess(MWasmMemoryAccessType* ins)
{
    MDefinition* base = ins->base();

    if (base->isConstant()) {
        // Look for heap[i] where i is a constant offset, and fold the offset.
        // By doing the folding now, we simplify the task of codegen; the offset
        // is always the address mode immediate. This also allows it to avoid
        // a situation where the sum of a constant pointer value and a non-zero
        // offset doesn't actually fit into the address mode immediate.
        int32_t imm = base->toConstant()->toInt32();
        if (imm != 0 && tryAddDisplacement(ins, imm)) {
            MInstruction* zero = MConstant::New(graph_.alloc(), Int32Value(0));
            ins->block()->insertBefore(ins, zero);
            ins->replaceBase(zero);
        }

        // If the index is within the minimum heap length, we can optimize
        // away the bounds check.
        if (imm >= 0) {
            int32_t end = (uint32_t)imm + ins->byteSize();
            if (end >= imm && (uint32_t)end <= mir_->minAsmJSHeapLength())
                 ins->removeBoundsCheck();
        }
    } else if (base->isAdd()) {
        // Look for heap[a+i] where i is a constant offset, and fold the offset.
        // Alignment masks have already been moved out of the way by the
        // Alignment Mask Analysis pass.
        MDefinition* op0 = base->toAdd()->getOperand(0);
        MDefinition* op1 = base->toAdd()->getOperand(1);
        if (op0->isConstant())
            mozilla::Swap(op0, op1);
        if (op1->isConstant()) {
            int32_t imm = op1->toConstant()->toInt32();
            if (tryAddDisplacement(ins, imm))
                ins->replaceBase(op0);
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
            if (!graph_.alloc().ensureBallast())
                return false;

            // Note that we don't check for MAsmJSCompareExchangeHeap
            // or MAsmJSAtomicBinopHeap, because the backend and the OOB
            // mechanism don't support non-zero offsets for them yet
            // (TODO bug 1254935).
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

#endif
