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
* @file rasterizer.cpp
*
* @brief Implementation for the rasterizer.
*
******************************************************************************/

#include <vector>
#include <algorithm>

#include "rasterizer.h"
#include "multisample.h"
#include "rdtsc_core.h"
#include "backend.h"
#include "utils.h"
#include "frontend.h"
#include "tilemgr.h"
#include "memory/tilingtraits.h"

void GetRenderHotTiles(DRAW_CONTEXT *pDC, uint32_t macroID, uint32_t x, uint32_t y, RenderOutputBuffers &renderBuffers, 
    uint32_t numSamples, uint32_t renderTargetArrayIndex);
void StepRasterTileX(uint32_t MaxRT, RenderOutputBuffers &buffers, uint32_t colorTileStep, uint32_t depthTileStep, uint32_t stencilTileStep);
void StepRasterTileY(uint32_t MaxRT, RenderOutputBuffers &buffers, RenderOutputBuffers &startBufferRow, 
                     uint32_t colorRowStep, uint32_t depthRowStep, uint32_t stencilRowStep);

#define MASKTOVEC(i3,i2,i1,i0) {-i0,-i1,-i2,-i3}
const __m128 gMaskToVec[] = {
    MASKTOVEC(0,0,0,0),
    MASKTOVEC(0,0,0,1),
    MASKTOVEC(0,0,1,0),
    MASKTOVEC(0,0,1,1),
    MASKTOVEC(0,1,0,0),
    MASKTOVEC(0,1,0,1),
    MASKTOVEC(0,1,1,0),
    MASKTOVEC(0,1,1,1),
    MASKTOVEC(1,0,0,0),
    MASKTOVEC(1,0,0,1),
    MASKTOVEC(1,0,1,0),
    MASKTOVEC(1,0,1,1),
    MASKTOVEC(1,1,0,0),
    MASKTOVEC(1,1,0,1),
    MASKTOVEC(1,1,1,0),
    MASKTOVEC(1,1,1,1),
};

const __m256d gMaskToVecpd[] =
{
    MASKTOVEC(0, 0, 0, 0),
    MASKTOVEC(0, 0, 0, 1),
    MASKTOVEC(0, 0, 1, 0),
    MASKTOVEC(0, 0, 1, 1),
    MASKTOVEC(0, 1, 0, 0),
    MASKTOVEC(0, 1, 0, 1),
    MASKTOVEC(0, 1, 1, 0),
    MASKTOVEC(0, 1, 1, 1),
    MASKTOVEC(1, 0, 0, 0),
    MASKTOVEC(1, 0, 0, 1),
    MASKTOVEC(1, 0, 1, 0),
    MASKTOVEC(1, 0, 1, 1),
    MASKTOVEC(1, 1, 0, 0),
    MASKTOVEC(1, 1, 0, 1),
    MASKTOVEC(1, 1, 1, 0),
    MASKTOVEC(1, 1, 1, 1),
};

//////////////////////////////////////////////////////////////////////////
/// @brief rasterize a raster tile partially covered by the triangle
/// @param vEdge0-2 - edge equations evaluated at sample pos at each of the 4 corners of a raster tile
/// @param vA, vB - A & B coefs for each edge of the triangle (Ax + Bx + C)
/// @param vStepQuad0-2 - edge equations evaluated at the UL corners of the 2x2 pixel quad.
///        Used to step between quads when sweeping over the raster tile.
INLINE uint64_t rasterizePartialTile(DRAW_CONTEXT *pDC, __m256d vEdge0, __m256d vEdge1, __m256d vEdge2,
    __m128i &vA, __m128i &vB, __m256d &vStepQuad0, __m256d &vStepQuad1, __m256d &vStepQuad2)
{
    uint64_t coverageMask = 0;

    // Step to the pixel sample locations of the 1st quad
    double edge0;
    double edge1;
    double edge2;
    _mm_store_sd(&edge0, _mm256_castpd256_pd128(vEdge0));
    _mm_store_sd(&edge1, _mm256_castpd256_pd128(vEdge1));
    _mm_store_sd(&edge2, _mm256_castpd256_pd128(vEdge2));

    vEdge0 = _mm256_broadcast_sd(&edge0);
    vEdge1 = _mm256_broadcast_sd(&edge1);
    vEdge2 = _mm256_broadcast_sd(&edge2);

    vEdge0 = _mm256_add_pd(vEdge0, vStepQuad0);
    vEdge1 = _mm256_add_pd(vEdge1, vStepQuad1);
    vEdge2 = _mm256_add_pd(vEdge2, vStepQuad2);

    // compute step to next quad (mul by 2 in x and y direction)
    __m256d vAEdge0 = _mm256_cvtepi32_pd(_mm_shuffle_epi32(vA, _MM_SHUFFLE(0, 0, 0, 0)));
    __m256d vAEdge1 = _mm256_cvtepi32_pd(_mm_shuffle_epi32(vA, _MM_SHUFFLE(1, 1, 1, 1)));
    __m256d vAEdge2 = _mm256_cvtepi32_pd(_mm_shuffle_epi32(vA, _MM_SHUFFLE(2, 2, 2, 2)));
    __m256d vBEdge0 = _mm256_cvtepi32_pd(_mm_shuffle_epi32(vB, _MM_SHUFFLE(0, 0, 0, 0)));
    __m256d vBEdge1 = _mm256_cvtepi32_pd(_mm_shuffle_epi32(vB, _MM_SHUFFLE(1, 1, 1, 1)));
    __m256d vBEdge2 = _mm256_cvtepi32_pd(_mm_shuffle_epi32(vB, _MM_SHUFFLE(2, 2, 2, 2)));

    __m256d vStep0X = _mm256_mul_pd(vAEdge0, _mm256_set1_pd(2 * FIXED_POINT_SCALE));
    __m256d vStep0Y = _mm256_mul_pd(vBEdge0, _mm256_set1_pd(2 * FIXED_POINT_SCALE));

    __m256d vStep1X = _mm256_mul_pd(vAEdge1, _mm256_set1_pd(2 * FIXED_POINT_SCALE));
    __m256d vStep1Y = _mm256_mul_pd(vBEdge1, _mm256_set1_pd(2 * FIXED_POINT_SCALE));

    __m256d vStep2X = _mm256_mul_pd(vAEdge2, _mm256_set1_pd(2 * FIXED_POINT_SCALE));
    __m256d vStep2Y = _mm256_mul_pd(vBEdge2, _mm256_set1_pd(2 * FIXED_POINT_SCALE));

    // fast unrolled version for 8x8 tile
#if KNOB_TILE_X_DIM == 8 && KNOB_TILE_Y_DIM == 8
    int mask0, mask1, mask2;
    uint64_t mask;

        // evaluate which pixels in the quad are covered
#define EVAL \
            mask0 = _mm256_movemask_pd(vEdge0);\
            mask1 = _mm256_movemask_pd(vEdge1);\
            mask2 = _mm256_movemask_pd(vEdge2);

        // update coverage mask
#define UPDATE_MASK(bit) \
            mask = mask0 & mask1 & mask2;\
            coverageMask |= (mask << bit);

        // step in the +x direction to the next quad 
#define INCX \
            vEdge0 = _mm256_add_pd(vEdge0, vStep0X);\
            vEdge1 = _mm256_add_pd(vEdge1, vStep1X);\
            vEdge2 = _mm256_add_pd(vEdge2, vStep2X);
        // step in the +y direction to the next quad 
#define INCY \
        vEdge0 = _mm256_add_pd(vEdge0, vStep0Y);\
        vEdge1 = _mm256_add_pd(vEdge1, vStep1Y);\
        vEdge2 = _mm256_add_pd(vEdge2, vStep2Y);
        // step in the -x direction to the next quad 
#define DECX \
            vEdge0 = _mm256_sub_pd(vEdge0, vStep0X);\
            vEdge1 = _mm256_sub_pd(vEdge1, vStep1X);\
            vEdge2 = _mm256_sub_pd(vEdge2, vStep2X);

    // sweep 2x2 quad back and forth through the raster tile, 
    // computing coverage masks for the entire tile
    
    // raster tile
    // 0  1  2  3  4  5  6  7 
    // x  x
    // x  x ------------------>  
    //                   x  x  |
    // <-----------------x  x  V
    // ..

    // row 0
    EVAL;
    UPDATE_MASK(0);
    INCX;
    EVAL;
    UPDATE_MASK(4);
    INCX;
    EVAL;
    UPDATE_MASK(8);
    INCX;
    EVAL;
    UPDATE_MASK(12);
    INCY;

    //row 1
    EVAL;
    UPDATE_MASK(28);
    DECX;
    EVAL;
    UPDATE_MASK(24);
    DECX;
    EVAL;
    UPDATE_MASK(20);
    DECX;
    EVAL;
    UPDATE_MASK(16);
    INCY;

    // row 2
    EVAL;
    UPDATE_MASK(32);
    INCX;
    EVAL;
    UPDATE_MASK(36);
    INCX;
    EVAL;
    UPDATE_MASK(40);
    INCX;
    EVAL;
    UPDATE_MASK(44);
    INCY;

    // row 3
    EVAL;
    UPDATE_MASK(60);
    DECX;
    EVAL;
    UPDATE_MASK(56);
    DECX;
    EVAL;
    UPDATE_MASK(52);
    DECX;
    EVAL;
    UPDATE_MASK(48);
#else
    uint32_t bit = 0;
    for (uint32_t y = 0; y < KNOB_TILE_Y_DIM/2; ++y)
    {
        __m256d vStartOfRowEdge0 = vEdge0;
        __m256d vStartOfRowEdge1 = vEdge1;
        __m256d vStartOfRowEdge2 = vEdge2;

        for (uint32_t x = 0; x < KNOB_TILE_X_DIM/2; ++x)
        {
            int mask0 = _mm256_movemask_pd(vEdge0);
            int mask1 = _mm256_movemask_pd(vEdge1);
            int mask2 = _mm256_movemask_pd(vEdge2);

            uint64_t mask = mask0 & mask1 & mask2;
            coverageMask |= (mask << bit);

            // step to the next pixel in the x
            vEdge0 = _mm256_add_pd(vEdge0, vStep0X);
            vEdge1 = _mm256_add_pd(vEdge1, vStep1X);
            vEdge2 = _mm256_add_pd(vEdge2, vStep2X);
            bit+=4;
        }

        // step to the next row
        vEdge0 = _mm256_add_pd(vStartOfRowEdge0, vStep0Y);
        vEdge1 = _mm256_add_pd(vStartOfRowEdge1, vStep1Y);
        vEdge2 = _mm256_add_pd(vStartOfRowEdge2, vStep2Y);
    }
#endif
    return coverageMask;

}
// Top left rule:
// Top: if an edge is horizontal, and it is above other edges in tri pixel space, it is a 'top' edge
// Left: if an edge is not horizontal, and it is on the left side of the triangle in pixel space, it is a 'left' edge
// Top left: a sample is in if it is a top or left edge.
// Out: !(horizontal && above) = !horizontal && below
// Out: !horizontal && left = !(!horizontal && left) = horizontal and right 
INLINE __m256d adjustTopLeftRuleIntFix16(const __m128i vA, const __m128i vB, const __m256d vEdge)
{
    // if vA < 0, vC--
    // if vA == 0 && vB < 0, vC--

    __m256d vEdgeOut = vEdge;
    __m256d vEdgeAdjust = _mm256_sub_pd(vEdge, _mm256_set1_pd(1.0));

    // if vA < 0 (line is not horizontal and below)
    int msk = _mm_movemask_ps(_mm_castsi128_ps(vA));

    // if vA == 0 && vB < 0 (line is horizontal and we're on the left edge of a tri)
    __m128i vCmp = _mm_cmpeq_epi32(vA, _mm_setzero_si128());
    int msk2 = _mm_movemask_ps(_mm_castsi128_ps(vCmp));
    msk2 &= _mm_movemask_ps(_mm_castsi128_ps(vB));

    // if either of these are true and we're on the line (edge == 0), bump it outside the line
    vEdgeOut = _mm256_blendv_pd(vEdgeOut, vEdgeAdjust, gMaskToVecpd[msk | msk2]);
    return vEdgeOut;
}

// max(abs(dz/dx), abs(dz,dy)
INLINE float ComputeMaxDepthSlope(const SWR_TRIANGLE_DESC* pDesc)
{
    /*
    // evaluate i,j at (0,0)
    float i00 = pDesc->I[0] * 0.0f + pDesc->I[1] * 0.0f + pDesc->I[2];
    float j00 = pDesc->J[0] * 0.0f + pDesc->J[1] * 0.0f + pDesc->J[2];

    // evaluate i,j at (1,0)
    float i10 = pDesc->I[0] * 1.0f + pDesc->I[1] * 0.0f + pDesc->I[2];
    float j10 = pDesc->J[0] * 1.0f + pDesc->J[1] * 0.0f + pDesc->J[2];

    // compute dz/dx
    float d00 = pDesc->Z[0] * i00 + pDesc->Z[1] * j00 + pDesc->Z[2];
    float d10 = pDesc->Z[0] * i10 + pDesc->Z[1] * j10 + pDesc->Z[2];
    float dzdx = abs(d10 - d00);

    // evaluate i,j at (0,1)
    float i01 = pDesc->I[0] * 0.0f + pDesc->I[1] * 1.0f + pDesc->I[2];
    float j01 = pDesc->J[0] * 0.0f + pDesc->J[1] * 1.0f + pDesc->J[2];

    float d01 = pDesc->Z[0] * i01 + pDesc->Z[1] * j01 + pDesc->Z[2];
    float dzdy = abs(d01 - d00);
    */

    // optimized version of above
    float dzdx = fabsf(pDesc->recipDet * (pDesc->Z[0] * pDesc->I[0] + pDesc->Z[1] * pDesc->J[0]));
    float dzdy = fabsf(pDesc->recipDet * (pDesc->Z[0] * pDesc->I[1] + pDesc->Z[1] * pDesc->J[1]));

    return std::max(dzdx, dzdy);
}

INLINE float ComputeBiasFactor(const SWR_RASTSTATE* pState, const SWR_TRIANGLE_DESC* pDesc, const float* z)
{
    if (pState->depthFormat == R24_UNORM_X8_TYPELESS)
    {
        return (1.0f / (1 << 24));
    }
    else if (pState->depthFormat == R16_UNORM)
    {
        return (1.0f / (1 << 16));
    }
    else
    {
        SWR_ASSERT(pState->depthFormat == R32_FLOAT);

        // for f32 depth, factor = 2^(exponent(max(abs(z) - 23)
        float zMax = std::max(fabsf(z[0]), std::max(fabsf(z[1]), fabsf(z[2])));
        uint32_t zMaxInt = *(uint32_t*)&zMax;
        zMaxInt &= 0x7f800000;
        zMax = *(float*)&zMaxInt;

        return zMax * (1.0f / (1 << 23));
    }
}

INLINE float ComputeDepthBias(const SWR_RASTSTATE* pState, const SWR_TRIANGLE_DESC* pTri, const float* z)
{
    if (pState->depthBias == 0 && pState->slopeScaledDepthBias == 0)
    {
        return 0.0f;
    }

    float scale = pState->slopeScaledDepthBias;
    if (scale != 0.0f)
    {
        scale *= ComputeMaxDepthSlope(pTri);
    }

    float bias = pState->depthBias * ComputeBiasFactor(pState, pTri, z) + scale;
    if (pState->depthBiasClamp > 0.0f)
    {
        bias = std::min(bias, pState->depthBiasClamp);
    }
    else if (pState->depthBiasClamp < 0.0f)
    {
        bias = std::max(bias, pState->depthBiasClamp);
    }

    return bias;
}

// Prevent DCE by writing coverage mask from rasterizer to volatile
#if KNOB_ENABLE_TOSS_POINTS
__declspec(thread) volatile uint64_t gToss;
#endif

static const uint32_t vertsPerTri = 3, componentsPerAttrib = 4;
// try to avoid _chkstk insertions; make this thread local
static THREAD OSALIGN(float, 16) perspAttribsTLS[vertsPerTri * KNOB_NUM_ATTRIBUTES * componentsPerAttrib];

template<SWR_MULTISAMPLE_COUNT sampleCount>
void RasterizeTriangle(DRAW_CONTEXT* pDC, uint32_t workerId, uint32_t macroTile, void* pDesc)
{

    const TRIANGLE_WORK_DESC &workDesc = *((TRIANGLE_WORK_DESC*)pDesc);
#if KNOB_ENABLE_TOSS_POINTS
    if (KNOB_TOSS_BIN_TRIS)
    {
        return;
    }
#endif
    RDTSC_START(BERasterizeTriangle);

    RDTSC_START(BETriangleSetup);
    const API_STATE &state = GetApiState(pDC);
    const SWR_RASTSTATE &rastState = state.rastState;

    OSALIGN(SWR_TRIANGLE_DESC, 16) triDesc;
    triDesc.pUserClipBuffer = workDesc.pUserClipBuffer;

    __m128 vX, vY, vZ, vRecipW;
    
    // pTriBuffer data layout: grouped components of the 3 triangle points and 1 don't care
    // eg: vX = [x0 x1 x2 dc]
    vX = _mm_load_ps(workDesc.pTriBuffer);
    vY = _mm_load_ps(workDesc.pTriBuffer + 4);
    vZ = _mm_load_ps(workDesc.pTriBuffer + 8);
    vRecipW = _mm_load_ps(workDesc.pTriBuffer + 12);

    // convert to fixed point
    __m128i vXi = fpToFixedPoint(vX);
    __m128i vYi = fpToFixedPoint(vY);

    // quantize floating point position to fixed point precision
    // to prevent attribute creep around the triangle vertices
    vX = _mm_mul_ps(_mm_cvtepi32_ps(vXi), _mm_set1_ps(1.0f / FIXED_POINT_SCALE));
    vY = _mm_mul_ps(_mm_cvtepi32_ps(vYi), _mm_set1_ps(1.0f / FIXED_POINT_SCALE));

    // triangle setup - A and B edge equation coefs
    __m128 vA, vB;
    triangleSetupAB(vX, vY, vA, vB);

    __m128i vAi, vBi;
    triangleSetupABInt(vXi, vYi, vAi, vBi);
    
    // determinant
    float det = calcDeterminantInt(vAi, vBi);

    /// @todo: This test is flipped...we have a stray '-' sign somewhere
    // Convert CW triangles to CCW
    if (det > 0.0)
    {
        vA  = _mm_mul_ps(vA, _mm_set1_ps(-1));
        vB  = _mm_mul_ps(vB, _mm_set1_ps(-1));
        vAi = _mm_mullo_epi32(vAi, _mm_set1_epi32(-1));
        vBi = _mm_mullo_epi32(vBi, _mm_set1_epi32(-1));
        det = -det;
    }

    __m128 vC;
    // Finish triangle setup - C edge coef
    triangleSetupC(vX, vY, vA, vB, vC);

    // compute barycentric i and j
    // i = (A1x + B1y + C1)/det
    // j = (A2x + B2y + C2)/det
    __m128 vDet = _mm_set1_ps(det);
    __m128 vRecipDet = _mm_div_ps(_mm_set1_ps(1.0f), vDet);//_mm_rcp_ps(vDet);
    _mm_store_ss(&triDesc.recipDet, vRecipDet);

    // only extract coefs for 2 of the barycentrics; the 3rd can be 
    // determined from the barycentric equation:
    // i + j + k = 1 <=> k = 1 - j - i
    _MM_EXTRACT_FLOAT(triDesc.I[0], vA, 1);
    _MM_EXTRACT_FLOAT(triDesc.I[1], vB, 1);
    _MM_EXTRACT_FLOAT(triDesc.I[2], vC, 1);
    _MM_EXTRACT_FLOAT(triDesc.J[0], vA, 2);
    _MM_EXTRACT_FLOAT(triDesc.J[1], vB, 2);
    _MM_EXTRACT_FLOAT(triDesc.J[2], vC, 2);

    OSALIGN(float, 16) oneOverW[4];
    _mm_store_ps(oneOverW, vRecipW);
    triDesc.OneOverW[0] = oneOverW[0] - oneOverW[2];
    triDesc.OneOverW[1] = oneOverW[1] - oneOverW[2];
    triDesc.OneOverW[2] = oneOverW[2];

    // calculate perspective correct coefs per vertex attrib 
    float* pPerspAttribs = perspAttribsTLS;
    float* pAttribs = workDesc.pAttribs;
    triDesc.pPerspAttribs = pPerspAttribs;
    triDesc.pAttribs = pAttribs;
    float *pRecipW = workDesc.pTriBuffer + 12;
    __m128 vOneOverWV0 = _mm_broadcast_ss(pRecipW);
    __m128 vOneOverWV1 = _mm_broadcast_ss(pRecipW+=1);
    __m128 vOneOverWV2 = _mm_broadcast_ss(pRecipW+=1);
    for(uint32_t i = 0; i < workDesc.numAttribs; i++)
    {
        __m128 attribA = _mm_load_ps(pAttribs);
        __m128 attribB = _mm_load_ps(pAttribs+=4);
        __m128 attribC = _mm_load_ps(pAttribs+=4);
        pAttribs+=4;

        attribA = _mm_mul_ps(attribA, vOneOverWV0);
        attribB = _mm_mul_ps(attribB, vOneOverWV1);
        attribC = _mm_mul_ps(attribC, vOneOverWV2);

        _mm_store_ps(pPerspAttribs, attribA);
        _mm_store_ps(pPerspAttribs+=4, attribB);
        _mm_store_ps(pPerspAttribs+=4, attribC);
        pPerspAttribs+=4;
    }

    // compute bary Z
    // zInterp = zVert0 + i(zVert1-zVert0) + j (zVert2 - zVert0)
    OSALIGN(float, 16) a[4];
    _mm_store_ps(a, vZ);
    triDesc.Z[0] = a[0] - a[2];
    triDesc.Z[1] = a[1] - a[2];
    triDesc.Z[2] = a[2];
        
    // add depth bias
    triDesc.Z[2] += ComputeDepthBias(&rastState, &triDesc, workDesc.pTriBuffer + 8);

    // broadcast A and B coefs for each edge to all slots
    __m128i vAEdge0h = _mm_shuffle_epi32(vAi, _MM_SHUFFLE(0,0,0,0));
    __m128i vAEdge1h = _mm_shuffle_epi32(vAi, _MM_SHUFFLE(1,1,1,1));
    __m128i vAEdge2h = _mm_shuffle_epi32(vAi, _MM_SHUFFLE(2,2,2,2));
    __m128i vBEdge0h = _mm_shuffle_epi32(vBi, _MM_SHUFFLE(0,0,0,0));
    __m128i vBEdge1h = _mm_shuffle_epi32(vBi, _MM_SHUFFLE(1,1,1,1));
    __m128i vBEdge2h = _mm_shuffle_epi32(vBi, _MM_SHUFFLE(2,2,2,2));

    __m256d vAEdge0Fix8 = _mm256_cvtepi32_pd(vAEdge0h);
    __m256d vAEdge1Fix8 = _mm256_cvtepi32_pd(vAEdge1h);
    __m256d vAEdge2Fix8 = _mm256_cvtepi32_pd(vAEdge2h);
    __m256d vBEdge0Fix8 = _mm256_cvtepi32_pd(vBEdge0h);
    __m256d vBEdge1Fix8 = _mm256_cvtepi32_pd(vBEdge1h);
    __m256d vBEdge2Fix8 = _mm256_cvtepi32_pd(vBEdge2h);

    // Precompute pixel quad step offsets
    // 0,0  ------  1,0
    //     |      |
    //     |      |
    // 1,0  ------  1,1
    const __m256d vQuadOffsetsXIntFix8 = _mm256_set_pd(FIXED_POINT_SCALE, 0, FIXED_POINT_SCALE, 0);
    const __m256d vQuadOffsetsYIntFix8 = _mm256_set_pd(FIXED_POINT_SCALE, FIXED_POINT_SCALE, 0, 0);

    // Evaluate edge equations at 4 upper left corners of a 2x2 pixel quad
    // used to step between quads while sweeping over a raster tile
    __m256d vQuadStepX0Fix16 = _mm256_mul_pd(vAEdge0Fix8, vQuadOffsetsXIntFix8);
    __m256d vQuadStepX1Fix16 = _mm256_mul_pd(vAEdge1Fix8, vQuadOffsetsXIntFix8);
    __m256d vQuadStepX2Fix16 = _mm256_mul_pd(vAEdge2Fix8, vQuadOffsetsXIntFix8);

    __m256d vQuadStepY0Fix16 = _mm256_mul_pd(vBEdge0Fix8, vQuadOffsetsYIntFix8);
    __m256d vQuadStepY1Fix16 = _mm256_mul_pd(vBEdge1Fix8, vQuadOffsetsYIntFix8);
    __m256d vQuadStepY2Fix16 = _mm256_mul_pd(vBEdge2Fix8, vQuadOffsetsYIntFix8);

    // vStepQuad = A*vQuadOffsetsXInt + B*vQuadOffsetsYInt
    __m256d vStepQuad0Fix16 = _mm256_add_pd(vQuadStepX0Fix16, vQuadStepY0Fix16);
    __m256d vStepQuad1Fix16 = _mm256_add_pd(vQuadStepX1Fix16, vQuadStepY1Fix16);
    __m256d vStepQuad2Fix16 = _mm256_add_pd(vQuadStepX2Fix16, vQuadStepY2Fix16);

    // Precompute tile step offsets
    //                 0,0  ------  KNOB_TILE_X_DIM-1,0
    //                     |      |
    //                     |      |
    // KNOB_TILE_Y_DIM-1,0  ------  KNOB_TILE_X_DIM-1,KNOB_TILE_Y_DIM-1
    const __m256d vTileOffsetsXIntFix8 = _mm256_set_pd((KNOB_TILE_X_DIM-1)*FIXED_POINT_SCALE, 0, (KNOB_TILE_X_DIM-1)*FIXED_POINT_SCALE, 0);
    const __m256d vTileOffsetsYIntFix8 = _mm256_set_pd((KNOB_TILE_Y_DIM-1)*FIXED_POINT_SCALE, (KNOB_TILE_Y_DIM-1)*FIXED_POINT_SCALE, 0, 0);

    // Calc bounding box of triangle
    OSALIGN(BBOX, 16) bbox;
    calcBoundingBoxInt(vXi, vYi, bbox);

    // Intersect with scissor/viewport
    bbox.left = std::max(bbox.left, state.scissorInFixedPoint.left);
    bbox.right = std::min(bbox.right - 1, state.scissorInFixedPoint.right);
    bbox.top = std::max(bbox.top, state.scissorInFixedPoint.top);
    bbox.bottom = std::min(bbox.bottom - 1, state.scissorInFixedPoint.bottom);

    triDesc.triFlags = workDesc.triFlags;

    // further constrain backend to intersecting bounding box of macro tile and scissored triangle bbox
    uint32_t macroX, macroY;
    MacroTileMgr::getTileIndices(macroTile, macroX, macroY);
    int32_t macroBoxLeft = macroX * KNOB_MACROTILE_X_DIM_FIXED;
    int32_t macroBoxRight = macroBoxLeft + KNOB_MACROTILE_X_DIM_FIXED - 1;
    int32_t macroBoxTop = macroY * KNOB_MACROTILE_Y_DIM_FIXED;
    int32_t macroBoxBottom = macroBoxTop + KNOB_MACROTILE_Y_DIM_FIXED - 1;

    OSALIGN(BBOX, 16) intersect;
    intersect.left   = std::max(bbox.left, macroBoxLeft);
    intersect.top    = std::max(bbox.top, macroBoxTop);
    intersect.right  = std::min(bbox.right, macroBoxRight);
    intersect.bottom = std::min(bbox.bottom, macroBoxBottom);

    SWR_ASSERT(intersect.left <= intersect.right && intersect.top <= intersect.bottom && intersect.left >= 0 && intersect.right >= 0 && intersect.top >= 0 && intersect.bottom >= 0);

    RDTSC_STOP(BETriangleSetup, 0, pDC->drawId);

    // update triangle desc
    uint32_t tileX = intersect.left >> (KNOB_TILE_X_DIM_SHIFT + FIXED_POINT_SHIFT);
    uint32_t tileY = intersect.top >> (KNOB_TILE_Y_DIM_SHIFT + FIXED_POINT_SHIFT);
    uint32_t maxTileX = intersect.right >> (KNOB_TILE_X_DIM_SHIFT + FIXED_POINT_SHIFT);
    uint32_t maxTileY = intersect.bottom >> (KNOB_TILE_Y_DIM_SHIFT + FIXED_POINT_SHIFT);
    uint32_t numTilesX = maxTileX - tileX + 1;
    uint32_t numTilesY = maxTileY - tileY + 1;

    if (numTilesX == 0 || numTilesY == 0) 
    {
        RDTSC_EVENT(BEEmptyTriangle, 1, 0);
        RDTSC_STOP(BERasterizeTriangle, 1, 0);
        return;
    }

    RDTSC_START(BEStepSetup);

    // Step to pixel center of top-left pixel of the triangle bbox
    // Align intersect bbox (top/left) to raster tile's (top/left).
    int32_t x = AlignDown(intersect.left, (FIXED_POINT_SCALE * KNOB_TILE_X_DIM));
    int32_t y = AlignDown(intersect.top, (FIXED_POINT_SCALE * KNOB_TILE_Y_DIM));

    if(sampleCount == SWR_MULTISAMPLE_1X)
    {
        // Add 0.5, in fixed point, to offset to pixel center
        x += (FIXED_POINT_SCALE / 2);
        y += (FIXED_POINT_SCALE / 2);
    }

    __m128i vTopLeftX = _mm_set1_epi32(x);
    __m128i vTopLeftY = _mm_set1_epi32(y);

    // evaluate edge equations at top-left pixel using 64bit math
    // all other evaluations will be 32bit steps from it
    // small triangles could skip this and do all 32bit math
    // edge 0
    // 
    // line = Ax + By + C
    // solving for C:
    // C = -Ax - By
    // we know x0 and y0 are on the line; plug them in:
    // C = -Ax0 - By0
    // plug C back into line equation:
    // line = Ax - Bx - Ax0 - Bx1
    // line = A(x - x0) + B(y - y0)
    // line = A(x0+dX) + B(y0+dY) + C = Ax0 + AdX + By0 + BdY + c = AdX + BdY

    // edge 0 and 1
    // edge0 = A0(x - x0) + B0(y - y0)
    // edge1 = A1(x - x1) + B1(y - y1)
    __m128i vDeltaX = _mm_sub_epi32(vTopLeftX, vXi);
    __m128i vDeltaY = _mm_sub_epi32(vTopLeftY, vYi);

    __m256d vEdgeFix16[3];

    // evaluate A(dx) and B(dY) for all points
    __m256d vAipd = _mm256_cvtepi32_pd(vAi);
    __m256d vBipd = _mm256_cvtepi32_pd(vBi);
    __m256d vDeltaXpd = _mm256_cvtepi32_pd(vDeltaX);
    __m256d vDeltaYpd = _mm256_cvtepi32_pd(vDeltaY);

    __m256d vAiDeltaXFix16 = _mm256_mul_pd(vAipd, vDeltaXpd);
    __m256d vBiDeltaYFix16 = _mm256_mul_pd(vBipd, vDeltaYpd);
    __m256d vEdge = _mm256_add_pd(vAiDeltaXFix16, vBiDeltaYFix16);

    // adjust for top-left rule
    vEdge = adjustTopLeftRuleIntFix16(vAi, vBi, vEdge);

    // broadcast respective edge results to all lanes
    double* pEdge = (double*)&vEdge;
    vEdgeFix16[0] = _mm256_set1_pd(pEdge[0]);
    vEdgeFix16[1] = _mm256_set1_pd(pEdge[1]);
    vEdgeFix16[2] = _mm256_set1_pd(pEdge[2]);

    // compute step to the next tile
    __m256d vNextXTileFix8 = _mm256_set1_pd(KNOB_TILE_X_DIM * FIXED_POINT_SCALE);
    __m256d vNextYTileFix8 = _mm256_set1_pd(KNOB_TILE_Y_DIM * FIXED_POINT_SCALE);
    __m256d vTileStepX0Fix16 = _mm256_mul_pd(vAEdge0Fix8, vNextXTileFix8);
    __m256d vTileStepY0Fix16 = _mm256_mul_pd(vBEdge0Fix8, vNextYTileFix8);
    __m256d vTileStepX1Fix16 = _mm256_mul_pd(vAEdge1Fix8, vNextXTileFix8);
    __m256d vTileStepY1Fix16 = _mm256_mul_pd(vBEdge1Fix8, vNextYTileFix8);
    __m256d vTileStepX2Fix16 = _mm256_mul_pd(vAEdge2Fix8, vNextXTileFix8);
    __m256d vTileStepY2Fix16 = _mm256_mul_pd(vBEdge2Fix8, vNextYTileFix8);

    // Evaluate edge equations at sample positions of each of the 4 corners of a raster tile
    // used to for testing if entire raster tile is inside a triangle
    __m256d vResultAxFix16 = _mm256_mul_pd(vAEdge0Fix8, vTileOffsetsXIntFix8);
    __m256d vResultByFix16 = _mm256_mul_pd(vBEdge0Fix8, vTileOffsetsYIntFix8);
    vEdgeFix16[0] = _mm256_add_pd(vEdgeFix16[0], _mm256_add_pd(vResultAxFix16, vResultByFix16));

    vResultAxFix16 = _mm256_mul_pd(vAEdge1Fix8, vTileOffsetsXIntFix8);
    vResultByFix16 = _mm256_mul_pd(vBEdge1Fix8, vTileOffsetsYIntFix8);
    vEdgeFix16[1] = _mm256_add_pd(vEdgeFix16[1], _mm256_add_pd(vResultAxFix16, vResultByFix16));

    vResultAxFix16 = _mm256_mul_pd(vAEdge2Fix8, vTileOffsetsXIntFix8);
    vResultByFix16 = _mm256_mul_pd(vBEdge2Fix8, vTileOffsetsYIntFix8);
    vEdgeFix16[2] = _mm256_add_pd(vEdgeFix16[2], _mm256_add_pd(vResultAxFix16, vResultByFix16));

    // at this point vEdge has been evaluated at the UL pixel corners of raster tile bbox
    // step sample positions to the raster tile bbox of multisample points
    // min(xSamples),min(ySamples)  ------  max(xSamples),min(ySamples)
    //                             |      |
    //                             |      |
    // min(xSamples),max(ySamples)  ------  max(xSamples),max(ySamples)
    __m256d vEdge0TileBbox, vEdge1TileBbox, vEdge2TileBbox;
    if(sampleCount > SWR_MULTISAMPLE_1X)
    {
        __m128i vTileSampleBBoxXh = MultisampleTraits<sampleCount>::TileSampleOffsetsX();
        __m128i vTileSampleBBoxYh = MultisampleTraits<sampleCount>::TileSampleOffsetsY();

        __m256d vTileSampleBBoxXFix8 = _mm256_cvtepi32_pd(vTileSampleBBoxXh);
        __m256d vTileSampleBBoxYFix8 = _mm256_cvtepi32_pd(vTileSampleBBoxYh);

        // step edge equation tests from Tile
        // used to for testing if entire raster tile is inside a triangle
        vResultAxFix16 = _mm256_mul_pd(vAEdge0Fix8, vTileSampleBBoxXFix8);
        vResultByFix16 = _mm256_mul_pd(vBEdge0Fix8, vTileSampleBBoxYFix8);
        vEdge0TileBbox = _mm256_add_pd(vResultAxFix16, vResultByFix16);

        vResultAxFix16 = _mm256_mul_pd(vAEdge1Fix8, vTileSampleBBoxXFix8);
        vResultByFix16 = _mm256_mul_pd(vBEdge1Fix8, vTileSampleBBoxYFix8);
        vEdge1TileBbox = _mm256_add_pd(vResultAxFix16, vResultByFix16);

        vResultAxFix16 = _mm256_mul_pd(vAEdge2Fix8, vTileSampleBBoxXFix8);
        vResultByFix16 = _mm256_mul_pd(vBEdge2Fix8, vTileSampleBBoxYFix8);
        vEdge2TileBbox = _mm256_add_pd(vResultAxFix16, vResultByFix16);
    }
    
    RDTSC_STOP(BEStepSetup, 0, pDC->drawId);

    uint32_t tY = tileY;
    uint32_t tX = tileX;
    uint32_t maxY = maxTileY;
    uint32_t maxX = maxTileX;

    triDesc.pSamplePos = pDC->pState->state.samplePos;

    // compute steps between raster tiles for render output buffers
    static const uint32_t colorRasterTileStep{(KNOB_TILE_X_DIM * KNOB_TILE_Y_DIM * (FormatTraits<KNOB_COLOR_HOT_TILE_FORMAT>::bpp / 8)) * MultisampleTraits<sampleCount>::numSamples};
    static const uint32_t colorRasterTileRowStep{(KNOB_MACROTILE_X_DIM / KNOB_TILE_X_DIM) * colorRasterTileStep};
    static const uint32_t depthRasterTileStep{(KNOB_TILE_X_DIM * KNOB_TILE_Y_DIM * (FormatTraits<KNOB_DEPTH_HOT_TILE_FORMAT>::bpp / 8)) * MultisampleTraits<sampleCount>::numSamples};
    static const uint32_t depthRasterTileRowStep{(KNOB_MACROTILE_X_DIM / KNOB_TILE_X_DIM)* depthRasterTileStep};
    static const uint32_t stencilRasterTileStep{(KNOB_TILE_X_DIM * KNOB_TILE_Y_DIM * (FormatTraits<KNOB_STENCIL_HOT_TILE_FORMAT>::bpp / 8)) * MultisampleTraits<sampleCount>::numSamples};
    static const uint32_t stencilRasterTileRowStep{(KNOB_MACROTILE_X_DIM / KNOB_TILE_X_DIM) * stencilRasterTileStep};
    RenderOutputBuffers renderBuffers, currentRenderBufferRow;

    GetRenderHotTiles(pDC, macroTile, tileX, tileY, renderBuffers, MultisampleTraits<sampleCount>::numSamples,
        triDesc.triFlags.renderTargetArrayIndex);
    currentRenderBufferRow = renderBuffers;

    // rasterize and generate coverage masks per sample
    uint32_t maxSamples = MultisampleTraits<sampleCount>::numSamples;
    for (uint32_t tileY = tY; tileY <= maxY; ++tileY)
    {
        __m256d vStartOfRowEdge0 = vEdgeFix16[0];
        __m256d vStartOfRowEdge1 = vEdgeFix16[1];
        __m256d vStartOfRowEdge2 = vEdgeFix16[2];

        for (uint32_t tileX = tX; tileX <= maxX; ++tileX)
        {
            uint64_t anyCoveredSamples = 0;

            // is the corner of the edge outside of the raster tile? (vEdge < 0)
            int mask0, mask1, mask2;
            if(sampleCount == SWR_MULTISAMPLE_1X)
            {
                // is the corner of the edge outside of the raster tile? (vEdge < 0)
                mask0 = _mm256_movemask_pd(vEdgeFix16[0]);
                mask1 = _mm256_movemask_pd(vEdgeFix16[1]);
                mask2 = _mm256_movemask_pd(vEdgeFix16[2]);
            }
            else
            {
                __m256d vSampleBboxTest0, vSampleBboxTest1, vSampleBboxTest2;
                // evaluate edge equations at the tile multisample bounding box
                vSampleBboxTest0 = _mm256_add_pd(vEdge0TileBbox, vEdgeFix16[0]);
                vSampleBboxTest1 = _mm256_add_pd(vEdge1TileBbox, vEdgeFix16[1]);
                vSampleBboxTest2 = _mm256_add_pd(vEdge2TileBbox, vEdgeFix16[2]);
                mask0 = _mm256_movemask_pd(vSampleBboxTest0);
                mask1 = _mm256_movemask_pd(vSampleBboxTest1);
                mask2 = _mm256_movemask_pd(vSampleBboxTest2);
            }

            for (uint32_t sampleNum = 0; sampleNum < maxSamples; sampleNum++)
            {
                // trivial reject, at least one edge has all 4 corners of raster tile outside
                bool trivialReject = (!(mask0 && mask1 && mask2)) ? true : false;

                if (!trivialReject)
                {
                    // trivial accept mask
                    triDesc.coverageMask[sampleNum] = 0xffffffffffffffffULL;
                    if ((mask0 & mask1 & mask2) == 0xf)
                    {
                        anyCoveredSamples = triDesc.coverageMask[sampleNum];
                        // trivial accept, all 4 corners of all 3 edges are negative 
                        // i.e. raster tile completely inside triangle
                        RDTSC_EVENT(BETrivialAccept, 1, 0);
                    }
                    else
                    {
                        __m256d vEdge0AtSample, vEdge1AtSample, vEdge2AtSample; 
                        if(sampleCount == SWR_MULTISAMPLE_1X)
                        {
                            // should get optimized out for single sample case (global value numbering or copy propagation)
                            vEdge0AtSample = vEdgeFix16[0];
                            vEdge1AtSample = vEdgeFix16[1];
                            vEdge2AtSample = vEdgeFix16[2];
                        }
                        else
                        {
                            __m128i vSampleOffsetXh = MultisampleTraits<sampleCount>::vXi(sampleNum);
                            __m128i vSampleOffsetYh = MultisampleTraits<sampleCount>::vYi(sampleNum);
                            __m256d vSampleOffsetX = _mm256_cvtepi32_pd(vSampleOffsetXh);
                            __m256d vSampleOffsetY = _mm256_cvtepi32_pd(vSampleOffsetYh);

                            // *note*: none of this needs to be vectorized as rasterizePartialTile just takes vEdge[0]
                            // for each edge and broadcasts it before offsetting to individual pixel quads

                            // step edge equation tests from UL tile corner to pixel sample position
                            vResultAxFix16 = _mm256_mul_pd(vAEdge0Fix8, vSampleOffsetX);
                            vResultByFix16 = _mm256_mul_pd(vBEdge0Fix8, vSampleOffsetY);
                            vEdge0AtSample = _mm256_add_pd(vResultAxFix16, vResultByFix16);
                            vEdge0AtSample = _mm256_add_pd(vEdgeFix16[0], vEdge0AtSample);

                            vResultAxFix16 = _mm256_mul_pd(vAEdge1Fix8, vSampleOffsetX);
                            vResultByFix16 = _mm256_mul_pd(vBEdge1Fix8, vSampleOffsetY);
                            vEdge1AtSample = _mm256_add_pd(vResultAxFix16, vResultByFix16);
                            vEdge1AtSample = _mm256_add_pd(vEdgeFix16[1], vEdge1AtSample);

                            vResultAxFix16 = _mm256_mul_pd(vAEdge2Fix8, vSampleOffsetX);
                            vResultByFix16 = _mm256_mul_pd(vBEdge2Fix8, vSampleOffsetY);
                            vEdge2AtSample = _mm256_add_pd(vResultAxFix16, vResultByFix16);
                            vEdge2AtSample = _mm256_add_pd(vEdgeFix16[2], vEdge2AtSample);
                        }

                        // not trivial accept or reject, must rasterize full tile
                        RDTSC_START(BERasterizePartial);
                        triDesc.coverageMask[sampleNum] = rasterizePartialTile(pDC, vEdge0AtSample, vEdge1AtSample, vEdge2AtSample,
                            vAi, vBi, vStepQuad0Fix16, vStepQuad1Fix16, vStepQuad2Fix16);
                        RDTSC_STOP(BERasterizePartial, 0, 0);

                        anyCoveredSamples |= triDesc.coverageMask[sampleNum]; 
                    }
                }
                else
                {
                    if(sampleCount > SWR_MULTISAMPLE_1X)
                    {
                        triDesc.coverageMask[sampleNum] = 0;
                    }
                    RDTSC_EVENT(BETrivialReject, 1, 0);
                }
            }

#if KNOB_ENABLE_TOSS_POINTS
            if(KNOB_TOSS_RS)
            {
                gToss = triDesc.coverageMask[0];
            }
            else
#endif
            if(anyCoveredSamples)
            {
                RDTSC_START(BEPixelBackend);
                pDC->pState->pfnBackend(pDC, workerId, tileX << KNOB_TILE_X_DIM_SHIFT, tileY << KNOB_TILE_Y_DIM_SHIFT, triDesc, renderBuffers);
                RDTSC_STOP(BEPixelBackend, 0, 0);
            }

            // step to the next tile in X
            vEdgeFix16[0] = _mm256_add_pd(vEdgeFix16[0], vTileStepX0Fix16);
            vEdgeFix16[1] = _mm256_add_pd(vEdgeFix16[1], vTileStepX1Fix16);
            vEdgeFix16[2] = _mm256_add_pd(vEdgeFix16[2], vTileStepX2Fix16);

            StepRasterTileX(state.psState.maxRTSlotUsed, renderBuffers, colorRasterTileStep, depthRasterTileStep, stencilRasterTileStep);
        }

        // step to the next tile in Y
        vEdgeFix16[0] = _mm256_add_pd(vStartOfRowEdge0, vTileStepY0Fix16);
        vEdgeFix16[1] = _mm256_add_pd(vStartOfRowEdge1, vTileStepY1Fix16);
        vEdgeFix16[2] = _mm256_add_pd(vStartOfRowEdge2, vTileStepY2Fix16);

        StepRasterTileY(state.psState.maxRTSlotUsed, renderBuffers, currentRenderBufferRow, colorRasterTileRowStep, depthRasterTileRowStep, stencilRasterTileRowStep);
    }

    RDTSC_STOP(BERasterizeTriangle, 1, 0);
}

void RasterizePoint(DRAW_CONTEXT *pDC, uint32_t workerId, const TRIANGLE_WORK_DESC &workDesc, uint32_t macroTile)
{
#if KNOB_ENABLE_TOSS_POINTS
    if (KNOB_TOSS_BIN_TRIS)
    {
        return;
    }
#endif

    // map x,y relative offsets from start of raster tile to bit position in 
    // coverage mask for the point
    static const uint32_t coverageMap[8][8] = {
        { 0, 1, 4, 5, 8, 9, 12, 13 },
        { 2, 3, 6, 7, 10, 11, 14, 15 },
        { 16, 17, 20, 21, 24, 25, 28, 29 },
        { 18, 19, 22, 23, 26, 27, 30, 31 },
        { 32, 33, 36, 37, 40, 41, 44, 45 },
        { 34, 35, 38, 39, 42, 43, 46, 47 },
        { 48, 49, 52, 53, 56, 57, 60, 61 },
        { 50, 51, 54, 55, 58, 59, 62, 63 }
    };

    OSALIGN(SWR_TRIANGLE_DESC, 16) triDesc;

    // pull point information from triangle buffer
    // @todo use structs for readability
    uint32_t tileAlignedX = *(uint32_t*)workDesc.pTriBuffer;
    uint32_t tileAlignedY = *(uint32_t*)(workDesc.pTriBuffer + 1);
    float z = *(workDesc.pTriBuffer + 2);

    // construct triangle descriptor for point
    // no interpolation, set up i,j for constant interpolation of z and attribs
    // @todo implement an optimized backend that doesn't require triangle information

    // compute coverage mask from x,y packed into the coverageMask flag
    // mask indices by the maximum valid index for x/y of coveragemap.
    uint32_t tX = workDesc.triFlags.coverageMask & 0x7;
    uint32_t tY = (workDesc.triFlags.coverageMask >> 4) & 0x7;
    // todo: multisample points?
    triDesc.coverageMask[0] = 1ULL << coverageMap[tY][tX];

    // no persp divide needed for points
    triDesc.pAttribs = triDesc.pPerspAttribs = workDesc.pAttribs;
    triDesc.triFlags = workDesc.triFlags;
    triDesc.recipDet = 1.0f;
    triDesc.OneOverW[0] = triDesc.OneOverW[1] = triDesc.OneOverW[2] = 1.0f;
    triDesc.I[0] = triDesc.I[1] = triDesc.I[2] = 0.0f;
    triDesc.J[0] = triDesc.J[1] = triDesc.J[2] = 0.0f;
    triDesc.Z[0] = triDesc.Z[1] = triDesc.Z[2] = z;

    RenderOutputBuffers renderBuffers;
    GetRenderHotTiles(pDC, macroTile, tileAlignedX >> KNOB_TILE_X_DIM_SHIFT , tileAlignedY >> KNOB_TILE_Y_DIM_SHIFT, 
        renderBuffers, 1, triDesc.triFlags.renderTargetArrayIndex);

    RDTSC_START(BEPixelBackend);
    pDC->pState->pfnBackend(pDC, workerId, tileAlignedX, tileAlignedY, triDesc, renderBuffers);
    RDTSC_STOP(BEPixelBackend, 0, 0);
}

void rastPoint(DRAW_CONTEXT *pDC, uint32_t workerId, uint32_t macroTile, void *pData)
{
    TRIANGLE_WORK_DESC *pDesc = (TRIANGLE_WORK_DESC*)pData;
    RasterizePoint(pDC, workerId, *pDesc, macroTile);

}
// Get pointers to hot tile memory for color RT, depth, stencil
void GetRenderHotTiles(DRAW_CONTEXT *pDC, uint32_t macroID, uint32_t tileX, uint32_t tileY, RenderOutputBuffers &renderBuffers, 
    uint32_t numSamples, uint32_t renderTargetArrayIndex)
{
    const API_STATE& state = GetApiState(pDC);
    SWR_CONTEXT *pContext = pDC->pContext;
    const SWR_DEPTH_STENCIL_STATE *pDSState = &state.depthStencilState;
    const uint32_t MaxRT = state.psState.maxRTSlotUsed;

    uint32_t mx, my;
    MacroTileMgr::getTileIndices(macroID, mx, my);
    tileX -= KNOB_MACROTILE_X_DIM_IN_TILES * mx;
    tileY -= KNOB_MACROTILE_Y_DIM_IN_TILES * my;

    if(state.psState.pfnPixelShader != NULL)
    {
        // compute tile offset for active hottile buffers
        const uint32_t pitch = KNOB_MACROTILE_X_DIM * FormatTraits<KNOB_COLOR_HOT_TILE_FORMAT>::bpp / 8;
        uint32_t offset = ComputeTileOffset2D<TilingTraits<SWR_TILE_SWRZ, FormatTraits<KNOB_COLOR_HOT_TILE_FORMAT>::bpp> >(pitch, tileX, tileY);
        offset*=numSamples;
        for(uint32_t rt = 0; rt <= MaxRT; ++rt)
        {
            HOTTILE *pColor = pContext->pHotTileMgr->GetHotTile(pContext, pDC, macroID, (SWR_RENDERTARGET_ATTACHMENT)(SWR_ATTACHMENT_COLOR0 + rt), true, 
                numSamples, renderTargetArrayIndex);
            pColor->state = HOTTILE_DIRTY;
            renderBuffers.pColor[rt] = pColor->pBuffer + offset;
        }
    }
    if(pDSState->depthTestEnable || pDSState->depthWriteEnable)
    {
        const uint32_t pitch = KNOB_MACROTILE_X_DIM * FormatTraits<KNOB_DEPTH_HOT_TILE_FORMAT>::bpp / 8;
        uint32_t offset = ComputeTileOffset2D<TilingTraits<SWR_TILE_SWRZ, FormatTraits<KNOB_DEPTH_HOT_TILE_FORMAT>::bpp> >(pitch, tileX, tileY);
        offset*=numSamples;
        HOTTILE *pDepth = pContext->pHotTileMgr->GetHotTile(pContext, pDC, macroID, SWR_ATTACHMENT_DEPTH, true, 
            numSamples, renderTargetArrayIndex);
        pDepth->state = HOTTILE_DIRTY;
        SWR_ASSERT(pDepth->pBuffer != nullptr);
        renderBuffers.pDepth = pDepth->pBuffer + offset;
    }
    if(pDSState->stencilTestEnable)
    {
        const uint32_t pitch = KNOB_MACROTILE_X_DIM * FormatTraits<KNOB_STENCIL_HOT_TILE_FORMAT>::bpp / 8;
        uint32_t offset = ComputeTileOffset2D<TilingTraits<SWR_TILE_SWRZ, FormatTraits<KNOB_STENCIL_HOT_TILE_FORMAT>::bpp> >(pitch, tileX, tileY);
        offset*=numSamples;
        HOTTILE* pStencil = pContext->pHotTileMgr->GetHotTile(pContext, pDC, macroID, SWR_ATTACHMENT_STENCIL, true, 
            numSamples, renderTargetArrayIndex);
        pStencil->state = HOTTILE_DIRTY;
        SWR_ASSERT(pStencil->pBuffer != nullptr);
        renderBuffers.pStencil = pStencil->pBuffer + offset;
    }
}

INLINE
void StepRasterTileX(uint32_t MaxRT, RenderOutputBuffers &buffers, uint32_t colorTileStep, uint32_t depthTileStep, uint32_t stencilTileStep)
{
    for(uint32_t rt = 0; rt <= MaxRT; ++rt)
    {
        buffers.pColor[rt] += colorTileStep;
    }
    
    buffers.pDepth += depthTileStep;
    buffers.pStencil += stencilTileStep;
}

INLINE
void StepRasterTileY(uint32_t MaxRT, RenderOutputBuffers &buffers, RenderOutputBuffers &startBufferRow, uint32_t colorRowStep, uint32_t depthRowStep, uint32_t stencilRowStep)
{
    for(uint32_t rt = 0; rt <= MaxRT; ++rt)
    {
        startBufferRow.pColor[rt] += colorRowStep;
        buffers.pColor[rt] = startBufferRow.pColor[rt];
    }
    startBufferRow.pDepth += depthRowStep;
    buffers.pDepth = startBufferRow.pDepth;

    startBufferRow.pStencil += stencilRowStep;
    buffers.pStencil = startBufferRow.pStencil;
}

// initialize rasterizer function table
PFN_WORK_FUNC gRasterizerTable[SWR_MULTISAMPLE_TYPE_MAX] =
{
    RasterizeTriangle<SWR_MULTISAMPLE_1X>,
    RasterizeTriangle<SWR_MULTISAMPLE_2X>,
    RasterizeTriangle<SWR_MULTISAMPLE_4X>,
    RasterizeTriangle<SWR_MULTISAMPLE_8X>,
    RasterizeTriangle<SWR_MULTISAMPLE_16X>
};

void RasterizeLine(DRAW_CONTEXT *pDC, uint32_t workerId, uint32_t macroTile, void *pData)
{
    const TRIANGLE_WORK_DESC &workDesc = *((TRIANGLE_WORK_DESC*)pData);
#if KNOB_ENABLE_TOSS_POINTS
    if (KNOB_TOSS_BIN_TRIS)
    {
        return;
    }
#endif

    // bloat line to two tris and call the triangle rasterizer twice
    RDTSC_START(BERasterizeLine);

    const API_STATE &state = GetApiState(pDC);

    // macrotile dimensioning
    uint32_t macroX, macroY;
    MacroTileMgr::getTileIndices(macroTile, macroX, macroY);
    int32_t macroBoxLeft = macroX * KNOB_MACROTILE_X_DIM_FIXED;
    int32_t macroBoxRight = macroBoxLeft + KNOB_MACROTILE_X_DIM_FIXED - 1;
    int32_t macroBoxTop = macroY * KNOB_MACROTILE_Y_DIM_FIXED;
    int32_t macroBoxBottom = macroBoxTop + KNOB_MACROTILE_Y_DIM_FIXED - 1;

    // create a copy of the triangle buffer to write our adjusted vertices to
    OSALIGNSIMD(float) newTriBuffer[4 * 4];
    TRIANGLE_WORK_DESC newWorkDesc = workDesc;
    newWorkDesc.pTriBuffer = &newTriBuffer[0];

    // create a copy of the attrib buffer to write our adjusted attribs to
    OSALIGNSIMD(float) newAttribBuffer[4 * 3 * KNOB_NUM_ATTRIBUTES];
    newWorkDesc.pAttribs = &newAttribBuffer[0];

    const __m128 vBloat0 = _mm_set_ps(0.5f, -0.5f, -0.5f, 0.5f);
    const __m128 vBloat1 = _mm_set_ps(0.5f, 0.5f, 0.5f, -0.5f);

    __m128 vX, vY, vZ, vRecipW;

    vX = _mm_load_ps(workDesc.pTriBuffer);
    vY = _mm_load_ps(workDesc.pTriBuffer + 4);
    vZ = _mm_load_ps(workDesc.pTriBuffer + 8);
    vRecipW = _mm_load_ps(workDesc.pTriBuffer + 12);

    // triangle 0
    // v0,v1 -> v0,v0,v1
    __m128 vXa = _mm_shuffle_ps(vX, vX, _MM_SHUFFLE(1, 1, 0, 0));
    __m128 vYa = _mm_shuffle_ps(vY, vY, _MM_SHUFFLE(1, 1, 0, 0));
    __m128 vZa = _mm_shuffle_ps(vZ, vZ, _MM_SHUFFLE(1, 1, 0, 0));
    __m128 vRecipWa = _mm_shuffle_ps(vRecipW, vRecipW, _MM_SHUFFLE(1, 1, 0, 0));

    __m128 vLineWidth = _mm_set1_ps(pDC->pState->state.rastState.lineWidth);
    __m128 vAdjust = _mm_mul_ps(vLineWidth, vBloat0);
    if (workDesc.triFlags.yMajor)
    {
        vXa = _mm_add_ps(vAdjust, vXa);
    }
    else
    {
        vYa = _mm_add_ps(vAdjust, vYa);
    }

    // Store triangle description for rasterizer
    _mm_store_ps((float*)&newTriBuffer[0], vXa);
    _mm_store_ps((float*)&newTriBuffer[4], vYa);
    _mm_store_ps((float*)&newTriBuffer[8], vZa);
    _mm_store_ps((float*)&newTriBuffer[12], vRecipWa);

    // binner bins 3 edges for lines as v0, v1, v1
    // tri0 needs v0, v0, v1
    for (uint32_t a = 0; a < workDesc.numAttribs; ++a)
    {
        __m128 vAttrib0 = _mm_load_ps(&workDesc.pAttribs[a*12 + 0]);
        __m128 vAttrib1 = _mm_load_ps(&workDesc.pAttribs[a*12 + 4]);

        _mm_store_ps((float*)&newAttribBuffer[a*12 + 0], vAttrib0);
        _mm_store_ps((float*)&newAttribBuffer[a*12 + 4], vAttrib0);
        _mm_store_ps((float*)&newAttribBuffer[a*12 + 8], vAttrib1);
    }

    // Store user clip distances for triangle 0
    float newClipBuffer[3 * 8];
    uint32_t numClipDist = _mm_popcnt_u32(state.rastState.clipDistanceMask);
    if (numClipDist)
    {
        newWorkDesc.pUserClipBuffer = newClipBuffer;

        float* pOldBuffer = workDesc.pUserClipBuffer;
        float* pNewBuffer = newClipBuffer;
        for (uint32_t i = 0; i < numClipDist; ++i)
        {
            // read barycentric coeffs from binner
            float a = *(pOldBuffer++);
            float b = *(pOldBuffer++);

            // reconstruct original clip distance at vertices
            float c0 = a + b;
            float c1 = b;

            // construct triangle barycentrics
            *(pNewBuffer++) = c0 - c1;
            *(pNewBuffer++) = c0 - c1;
            *(pNewBuffer++) = c1;
        }
    }

    // make sure this macrotile intersects the triangle
    __m128i vXai = fpToFixedPoint(vXa);
    __m128i vYai = fpToFixedPoint(vYa);
    OSALIGN(BBOX, 16) bboxA;
    calcBoundingBoxInt(vXai, vYai, bboxA);

    if (!(bboxA.left > macroBoxRight ||
          bboxA.left > state.scissorInFixedPoint.right ||
          bboxA.right - 1 < macroBoxLeft ||
          bboxA.right - 1 < state.scissorInFixedPoint.left ||
          bboxA.top > macroBoxBottom ||
          bboxA.top > state.scissorInFixedPoint.bottom ||
          bboxA.bottom - 1 < macroBoxTop ||
          bboxA.bottom - 1 < state.scissorInFixedPoint.top)) {
        // rasterize triangle
        RasterizeTriangle<SWR_MULTISAMPLE_1X>(pDC, workerId, macroTile, (void*)&newWorkDesc);
    }

    // triangle 1
    // v0,v1 -> v1,v1,v0
    vXa = _mm_shuffle_ps(vX, vX, _MM_SHUFFLE(1, 0, 1, 1));
    vYa = _mm_shuffle_ps(vY, vY, _MM_SHUFFLE(1, 0, 1, 1));
    vZa = _mm_shuffle_ps(vZ, vZ, _MM_SHUFFLE(1, 0, 1, 1));
    vRecipWa = _mm_shuffle_ps(vRecipW, vRecipW, _MM_SHUFFLE(1, 0, 1, 1));

    vAdjust = _mm_mul_ps(vLineWidth, vBloat1);
    if (workDesc.triFlags.yMajor)
    {
        vXa = _mm_add_ps(vAdjust, vXa);
    }
    else
    {
        vYa = _mm_add_ps(vAdjust, vYa);
    }

    // Store triangle description for rasterizer
    _mm_store_ps((float*)&newTriBuffer[0], vXa);
    _mm_store_ps((float*)&newTriBuffer[4], vYa);
    _mm_store_ps((float*)&newTriBuffer[8], vZa);
    _mm_store_ps((float*)&newTriBuffer[12], vRecipWa);

    // binner bins 3 edges for lines as v0, v1, v1
    // tri1 needs v1, v1, v0
    for (uint32_t a = 0; a < workDesc.numAttribs; ++a)
    {
        __m128 vAttrib0 = _mm_load_ps(&workDesc.pAttribs[a * 12 + 0]);
        __m128 vAttrib1 = _mm_load_ps(&workDesc.pAttribs[a * 12 + 4]);

        _mm_store_ps((float*)&newAttribBuffer[a * 12 + 0], vAttrib1);
        _mm_store_ps((float*)&newAttribBuffer[a * 12 + 4], vAttrib1);
        _mm_store_ps((float*)&newAttribBuffer[a * 12 + 8], vAttrib0);
    }

    // store user clip distance for triangle 1
    if (numClipDist)
    {
        float* pOldBuffer = workDesc.pUserClipBuffer;
        float* pNewBuffer = newClipBuffer;
        for (uint32_t i = 0; i < numClipDist; ++i)
        {
            // read barycentric coeffs from binner
            float a = *(pOldBuffer++);
            float b = *(pOldBuffer++);

            // reconstruct original clip distance at vertices
            float c0 = a + b;
            float c1 = b;

            // construct triangle barycentrics
            *(pNewBuffer++) = c1 - c0;
            *(pNewBuffer++) = c1 - c0;
            *(pNewBuffer++) = c0;
        }
    }

    vXai = fpToFixedPoint(vXa);
    vYai = fpToFixedPoint(vYa);
    calcBoundingBoxInt(vXai, vYai, bboxA);

    if (!(bboxA.left > macroBoxRight ||
          bboxA.left > state.scissorInFixedPoint.right ||
          bboxA.right - 1 < macroBoxLeft ||
          bboxA.right - 1 < state.scissorInFixedPoint.left ||
          bboxA.top > macroBoxBottom ||
          bboxA.top > state.scissorInFixedPoint.bottom ||
          bboxA.bottom - 1 < macroBoxTop ||
          bboxA.bottom - 1 < state.scissorInFixedPoint.top)) {
        // rasterize triangle
        RasterizeTriangle<SWR_MULTISAMPLE_1X>(pDC, workerId, macroTile, (void*)&newWorkDesc);
    }

    RDTSC_STOP(BERasterizeLine, 1, 0);
}

