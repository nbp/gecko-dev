/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_thm_EffectiveAddressTransformation_h
#define jit_thm_EffectiveAddressTransformation_h
#if ENABLE_THM

namespace js {
namespace jit {

class MIRGenerator;

namespace thm {

class THMGraph;

class EffectiveAddressTransformation
{
    MIRGenerator* mir_;
    THMGraph& graph_;

    template<typename MAsmJSHeapAccessType>
    bool tryAddDisplacement(MAsmJSHeapAccessType* ins, int32_t o);

    template<typename MAsmJSHeapAccessType>
    void analyzeAsmHeapAccess(MAsmJSHeapAccessType* ins);

  public:
    EffectiveAddressTransformation(MIRGenerator* mir, MIRGraph& graph)
      : mir_(mir), graph_(graph)
    {}

    bool analyze();
};

} /* namespace thm */
} /* namespace jit */
} /* namespace js */

#endif  // ENABLE_THM
#endif /* jit_thm_EffectiveAddressTransformation_h */
