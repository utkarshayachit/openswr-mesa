/****************************************************************************
* Copyright (C) 2014-2015 Intel Corporation.   All Rights Reserved.
*
* Permission is hereby granted, free of charge, to any person obtaining a
* copy of this software and associated documentation files (the "Software"),
* to deal in the Software without restriction, including without limitation
* the rights to use, copy, modify, merge, publish, distribute, sublicense,
* and/or sell copies of the Software, and to permit persons to whom the
* Software is furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice (including the next
* paragraph) shall be included in all copies or substantial portions of the
* Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
* THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
* FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
* IN THE SOFTWARE.
* 
* @file builder_x86.cpp
* 
* @brief auto-generated file
* 
* DO NOT EDIT
* 
******************************************************************************/

#include "builder.h"

//////////////////////////////////////////////////////////////////////////
Value *Builder::VGATHERPS(Value* src, Value* pBase, Value* indices, Value* mask, Value* scale)
{
    Function *func = Intrinsic::getDeclaration(JM()->mpCurrentModule, Intrinsic::x86_avx2_gather_d_ps_256);
    return IRB()->CreateCall5(func, src, pBase, indices, mask, scale);
}

//////////////////////////////////////////////////////////////////////////
Value *Builder::VGATHERDD(Value* src, Value* pBase, Value* indices, Value* mask, Value* scale)
{
    Function *func = Intrinsic::getDeclaration(JM()->mpCurrentModule, Intrinsic::x86_avx2_gather_d_d_256);
    return IRB()->CreateCall5(func, src, pBase, indices, mask, scale);
}

//////////////////////////////////////////////////////////////////////////
Value *Builder::VSQRTPS(Value* a)
{
    Function *func = Intrinsic::getDeclaration(JM()->mpCurrentModule, Intrinsic::x86_avx_sqrt_ps_256);
    return IRB()->CreateCall(func, a);
}

//////////////////////////////////////////////////////////////////////////
Value *Builder::VRSQRTPS(Value* a)
{
    Function *func = Intrinsic::getDeclaration(JM()->mpCurrentModule, Intrinsic::x86_avx_rsqrt_ps_256);
    return IRB()->CreateCall(func, a);
}

//////////////////////////////////////////////////////////////////////////
Value *Builder::VRCPPS(Value* a)
{
    Function *func = Intrinsic::getDeclaration(JM()->mpCurrentModule, Intrinsic::x86_avx_rcp_ps_256);
    return IRB()->CreateCall(func, a);
}

//////////////////////////////////////////////////////////////////////////
Value *Builder::VMINPS(Value* a, Value* b)
{
    Function *func = Intrinsic::getDeclaration(JM()->mpCurrentModule, Intrinsic::x86_avx_min_ps_256);
    return IRB()->CreateCall2(func, a, b);
}

//////////////////////////////////////////////////////////////////////////
Value *Builder::VMAXPS(Value* a, Value* b)
{
    Function *func = Intrinsic::getDeclaration(JM()->mpCurrentModule, Intrinsic::x86_avx_max_ps_256);
    return IRB()->CreateCall2(func, a, b);
}

//////////////////////////////////////////////////////////////////////////
Value *Builder::VPMINSD(Value* a, Value* b)
{
    Function *func = Intrinsic::getDeclaration(JM()->mpCurrentModule, Intrinsic::x86_avx2_pmins_d);
    return IRB()->CreateCall2(func, a, b);
}

//////////////////////////////////////////////////////////////////////////
Value *Builder::VPMAXSD(Value* a, Value* b)
{
    Function *func = Intrinsic::getDeclaration(JM()->mpCurrentModule, Intrinsic::x86_avx2_pmaxs_d);
    return IRB()->CreateCall2(func, a, b);
}

//////////////////////////////////////////////////////////////////////////
Value *Builder::VROUND(Value* a, Value* rounding)
{
    Function *func = Intrinsic::getDeclaration(JM()->mpCurrentModule, Intrinsic::x86_avx_round_ps_256);
    return IRB()->CreateCall2(func, a, rounding);
}

//////////////////////////////////////////////////////////////////////////
Value *Builder::VCMPPS(Value* a, Value* b, Value* cmpop)
{
    Function *func = Intrinsic::getDeclaration(JM()->mpCurrentModule, Intrinsic::x86_avx_cmp_ps_256);
    return IRB()->CreateCall3(func, a, b, cmpop);
}

//////////////////////////////////////////////////////////////////////////
Value *Builder::VBLENDVPS(Value* a, Value* b, Value* mask)
{
    Function *func = Intrinsic::getDeclaration(JM()->mpCurrentModule, Intrinsic::x86_avx_blendv_ps_256);
    return IRB()->CreateCall3(func, a, b, mask);
}

//////////////////////////////////////////////////////////////////////////
Value *Builder::BEXTR_32(Value* src, Value* control)
{
    Function *func = Intrinsic::getDeclaration(JM()->mpCurrentModule, Intrinsic::x86_bmi_bextr_32);
    return IRB()->CreateCall2(func, src, control);
}

//////////////////////////////////////////////////////////////////////////
Value *Builder::VMASKLOADD(Value* src, Value* mask)
{
    Function *func = Intrinsic::getDeclaration(JM()->mpCurrentModule, Intrinsic::x86_avx2_maskload_d_256);
    return IRB()->CreateCall2(func, src, mask);
}

//////////////////////////////////////////////////////////////////////////
Value *Builder::VMASKMOVPS(Value* src, Value* mask)
{
    Function *func = Intrinsic::getDeclaration(JM()->mpCurrentModule, Intrinsic::x86_avx_maskload_ps_256);
    return IRB()->CreateCall2(func, src, mask);
}

//////////////////////////////////////////////////////////////////////////
Value *Builder::VPSHUFB(Value* a, Value* b)
{
    Function *func = Intrinsic::getDeclaration(JM()->mpCurrentModule, Intrinsic::x86_avx2_pshuf_b);
    return IRB()->CreateCall2(func, a, b);
}

//////////////////////////////////////////////////////////////////////////
Value *Builder::VPMOVSXBD(Value* a)
{
    Function *func = Intrinsic::getDeclaration(JM()->mpCurrentModule, Intrinsic::x86_avx2_pmovsxbd);
    return IRB()->CreateCall(func, a);
}

//////////////////////////////////////////////////////////////////////////
Value *Builder::VPMOVSXWD(Value* a)
{
    Function *func = Intrinsic::getDeclaration(JM()->mpCurrentModule, Intrinsic::x86_avx2_pmovsxwd);
    return IRB()->CreateCall(func, a);
}

//////////////////////////////////////////////////////////////////////////
Value *Builder::VPERMD(Value* idx, Value* a)
{
    Function *func = Intrinsic::getDeclaration(JM()->mpCurrentModule, Intrinsic::x86_avx2_permd);
    return IRB()->CreateCall2(func, idx, a);
}

//////////////////////////////////////////////////////////////////////////
Value *Builder::VCVTPH2PS(Value* a)
{
    Function *func = Intrinsic::getDeclaration(JM()->mpCurrentModule, Intrinsic::x86_vcvtph2ps_256);
    return IRB()->CreateCall(func, a);
}

//////////////////////////////////////////////////////////////////////////
Value *Builder::VCVTPS2PH(Value* a, Value* round)
{
    Function *func = Intrinsic::getDeclaration(JM()->mpCurrentModule, Intrinsic::x86_vcvtps2ph_256);
    return IRB()->CreateCall2(func, a, round);
}

//////////////////////////////////////////////////////////////////////////
Value *Builder::VEXTRACTF128(Value* a, Value* imm8)
{
    Function *func = Intrinsic::getDeclaration(JM()->mpCurrentModule, Intrinsic::x86_avx_vextractf128_ps_256);
    return IRB()->CreateCall2(func, a, imm8);
}

//////////////////////////////////////////////////////////////////////////
Value *Builder::VEXTRACTI128(Value* a, Value* imm8)
{
    Function *func = Intrinsic::getDeclaration(JM()->mpCurrentModule, Intrinsic::x86_avx_vextractf128_si_256);
    return IRB()->CreateCall2(func, a, imm8);
}

//////////////////////////////////////////////////////////////////////////
Value *Builder::VINSERTF128(Value* a, Value* b, Value* imm8)
{
    Function *func = Intrinsic::getDeclaration(JM()->mpCurrentModule, Intrinsic::x86_avx_vinsertf128_ps_256);
    return IRB()->CreateCall3(func, a, b, imm8);
}

//////////////////////////////////////////////////////////////////////////
Value *Builder::VINSERTI128(Value* a, Value* b, Value* imm8)
{
    Function *func = Intrinsic::getDeclaration(JM()->mpCurrentModule, Intrinsic::x86_avx_vinsertf128_si_256);
    return IRB()->CreateCall3(func, a, b, imm8);
}

//////////////////////////////////////////////////////////////////////////
Value *Builder::VHSUBPS(Value* a, Value* b)
{
    Function *func = Intrinsic::getDeclaration(JM()->mpCurrentModule, Intrinsic::x86_avx_hsub_ps_256);
    return IRB()->CreateCall2(func, a, b);
}

//////////////////////////////////////////////////////////////////////////
Value *Builder::VPTESTC(Value* a, Value* b)
{
    Function *func = Intrinsic::getDeclaration(JM()->mpCurrentModule, Intrinsic::x86_avx_ptestc_256);
    return IRB()->CreateCall2(func, a, b);
}

//////////////////////////////////////////////////////////////////////////
Value *Builder::VFMADDPS(Value* a, Value* b, Value* c)
{
    Function *func = Intrinsic::getDeclaration(JM()->mpCurrentModule, Intrinsic::x86_fma_vfmadd_ps_256);
    return IRB()->CreateCall3(func, a, b, c);
}

//////////////////////////////////////////////////////////////////////////
Value *Builder::VCVTTPS2DQ(Value* a)
{
    Function *func = Intrinsic::getDeclaration(JM()->mpCurrentModule, Intrinsic::x86_avx_cvtt_ps2dq_256);
    return IRB()->CreateCall(func, a);
}

//////////////////////////////////////////////////////////////////////////
Value *Builder::VMOVMSKPS(Value* a)
{
    Function *func = Intrinsic::getDeclaration(JM()->mpCurrentModule, Intrinsic::x86_avx_movmsk_ps_256);
    return IRB()->CreateCall(func, a);
}

