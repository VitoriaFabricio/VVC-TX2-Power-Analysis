/* The copyright in this software is being made available under the BSD
 * License, included below. This software may be subject to other third party
 * and contributor rights, including patent rights, and no such rights are
 * granted under this license.
 *
 * Copyright (c) 2010-2025, ITU/ISO/IEC
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *  * Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *  * Neither the name of the ITU/ISO/IEC nor the names of its contributors may
 *    be used to endorse or promote products derived from this software without
 *    specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

/** \file     AdaptiveLoopFilter.cpp
    \brief    adaptive loop filter class
*/

#include "AdaptiveLoopFilter.h"

#include "CodingStructure.h"
#include "Picture.h"
#include <array>
#include <cmath>
#include <sys/time.h>

const EnumArray<int, ChannelType> AdaptiveLoopFilter::ALF_NUM_CLIP_VALS = { 4, 4 };

AdaptiveLoopFilter::AdaptiveLoopFilter()
  : m_classifier( nullptr )
{
  for (size_t i = 0; i < NUM_DIRECTIONS; i++)
  {
    m_laplacian[i] = m_laplacianPtr[i];
    for (size_t j = 0; j < sizeof(m_laplacianPtr[i]) / sizeof(m_laplacianPtr[i][0]); j++)
    {
      m_laplacianPtr[i][j] = m_laplacianData[i][j];
    }
  }

  for( int compIdx = 0; compIdx < MAX_NUM_COMPONENT; compIdx++ )
  {
    m_modes[compIdx] = nullptr;
  }

  m_deriveClassificationBlk = deriveClassificationBlk;
  m_filterCcAlf = filterBlkCcAlf<CC_ALF>;
  m_filter5x5Blk = filterBlk<ALF_FILTER_5>;
  m_filter7x7Blk = filterBlk<ALF_FILTER_7>;

#if ENABLE_SIMD_OPT_ALF
#ifdef TARGET_SIMD_X86
  initAdaptiveLoopFilterX86();
#endif
#endif
}

bool AdaptiveLoopFilter::isCrossedByVirtualBoundaries( const CodingStructure& cs, const int xPos, const int yPos, const int width, const int height, bool& clipTop, bool& clipBottom, bool& clipLeft, bool& clipRight, int& numHorVirBndry, int& numVerVirBndry, int horVirBndryPos[], int verVirBndryPos[], int& rasterSliceAlfPad )
{
  clipTop = false; clipBottom = false; clipLeft = false; clipRight = false;
  numHorVirBndry = 0; numVerVirBndry = 0;
  const PPS*   pps = cs.pps;
  const PicHeader* picHeader = cs.picHeader;

  if( picHeader->getVirtualBoundariesPresentFlag() )
  {
    for( int i = 0; i < picHeader->getNumHorVirtualBoundaries(); i++ )
    {
      if( picHeader->getVirtualBoundariesPosY(i) == yPos )
      {
        clipTop = true;
      }
      else if( picHeader->getVirtualBoundariesPosY(i) == yPos + height )
      {
        clipBottom = true;
      }
      else if( yPos < picHeader->getVirtualBoundariesPosY(i) && picHeader->getVirtualBoundariesPosY(i) < yPos + height )
      {
        horVirBndryPos[numHorVirBndry++] = picHeader->getVirtualBoundariesPosY(i);
      }
    }
    for( int i = 0; i < picHeader->getNumVerVirtualBoundaries(); i++ )
    {
      if( picHeader->getVirtualBoundariesPosX(i) == xPos )
      {
        clipLeft = true;
      }
      else if( picHeader->getVirtualBoundariesPosX(i) == xPos + width )
      {
        clipRight = true;
      }
      else if( xPos < picHeader->getVirtualBoundariesPosX(i) && picHeader->getVirtualBoundariesPosX(i) < xPos + width )
      {
        verVirBndryPos[numVerVirBndry++] = picHeader->getVirtualBoundariesPosX(i);
      }
    }
  }

  const Slice& slice = *(cs.slice);
  int   ctuSize = slice.getSPS()->getCTUSize();
  const Position currCtuPos(xPos, yPos);
  const CodingUnit *currCtu   = cs.getCU(currCtuPos, ChannelType::LUMA);
  const SubPic& curSubPic = slice.getPPS()->getSubPicFromPos(currCtuPos);

  bool loopFilterAcrossSubPicEnabledFlag = curSubPic.getloopFilterAcrossEnabledFlag();
  //top
  if (yPos >= ctuSize && clipTop == false)
  {
    const Position prevCtuPos(xPos, yPos - ctuSize);
    const CodingUnit *prevCtu = cs.getCU(prevCtuPos, ChannelType::LUMA);
    if ((!pps->getLoopFilterAcrossSlicesEnabledFlag() && !CU::isSameSlice(*currCtu, *prevCtu)) ||
        (!pps->getLoopFilterAcrossTilesEnabledFlag()  && !CU::isSameTile(*currCtu,  *prevCtu))
      || (!loopFilterAcrossSubPicEnabledFlag && !CU::isSameSubPic(*currCtu, *prevCtu))
      )
    {
      clipTop = true;
    }
  }

  //bottom
  if (yPos + ctuSize < cs.pcv->lumaHeight && clipBottom == false)
  {
    const Position nextCtuPos(xPos, yPos + ctuSize);
    const CodingUnit *nextCtu = cs.getCU(nextCtuPos, ChannelType::LUMA);
    if ((!pps->getLoopFilterAcrossSlicesEnabledFlag() && !CU::isSameSlice(*currCtu, *nextCtu)) ||
        (!pps->getLoopFilterAcrossTilesEnabledFlag()  && !CU::isSameTile(*currCtu,  *nextCtu))
      || (!loopFilterAcrossSubPicEnabledFlag && !CU::isSameSubPic(*currCtu, *nextCtu))
      )
    {
      clipBottom = true;
    }
  }

  //left
  if (xPos >= ctuSize && clipLeft == false)
  {
    const Position prevCtuPos(xPos - ctuSize, yPos);
    const CodingUnit *prevCtu = cs.getCU(prevCtuPos, ChannelType::LUMA);
    if ((!pps->getLoopFilterAcrossSlicesEnabledFlag() && !CU::isSameSlice(*currCtu, *prevCtu)) ||
        (!pps->getLoopFilterAcrossTilesEnabledFlag()  && !CU::isSameTile(*currCtu,  *prevCtu))
      || (!loopFilterAcrossSubPicEnabledFlag && !CU::isSameSubPic(*currCtu, *prevCtu))
      )
    {
      clipLeft = true;
    }
  }

  //right
  if (xPos + ctuSize < cs.pcv->lumaWidth && clipRight == false)
  {
    const Position nextCtuPos(xPos + ctuSize, yPos);
    const CodingUnit *nextCtu = cs.getCU(nextCtuPos, ChannelType::LUMA);
    if ((!pps->getLoopFilterAcrossSlicesEnabledFlag() && !CU::isSameSlice(*currCtu, *nextCtu)) ||
        (!pps->getLoopFilterAcrossTilesEnabledFlag()  && !CU::isSameTile(*currCtu,  *nextCtu))
      || (!loopFilterAcrossSubPicEnabledFlag && !CU::isSameSubPic(*currCtu, *nextCtu))
      )
    {
      clipRight = true;
    }
  }

  rasterSliceAlfPad = 0;
  if ( !clipTop && !clipLeft )
  {
    //top-left CTU
    if ( xPos >= ctuSize && yPos >= ctuSize )
    {
      const Position prevCtuPos( xPos - ctuSize, yPos - ctuSize );
      const CodingUnit *prevCtu = cs.getCU(prevCtuPos, ChannelType::LUMA);
      if ( !pps->getLoopFilterAcrossSlicesEnabledFlag() && !CU::isSameSlice( *currCtu, *prevCtu ) )
      {
        rasterSliceAlfPad = 1;
      }
    }
  }

  if ( !clipBottom && !clipRight )
  {
    //bottom-right CTU
    if ( xPos + ctuSize < cs.pcv->lumaWidth && yPos + ctuSize < cs.pcv->lumaHeight )
    {
      const Position nextCtuPos( xPos + ctuSize, yPos + ctuSize );
      const CodingUnit *nextCtu = cs.getCU(nextCtuPos, ChannelType::LUMA);
      if ( !pps->getLoopFilterAcrossSlicesEnabledFlag() && !CU::isSameSlice( *currCtu, *nextCtu ) )
      {
        rasterSliceAlfPad += 2;
      }
    }
  }

  return numHorVirBndry > 0 || numVerVirBndry > 0 || clipTop || clipBottom || clipLeft || clipRight || rasterSliceAlfPad;
}

const AlfCoeff AdaptiveLoopFilter::m_fixedFilterSetCoeff[ALF_FIXED_FILTER_NUM][MAX_NUM_ALF_LUMA_COEFF] = {
  { 0, 0, 2, -3, 1, -4, 1, 7, -1, 1, -1, 5, 0 },       { 0, 0, 0, 0, 0, -1, 0, 1, 0, 0, -1, 2, 0 },
  { 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0 },           { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, -1, 1, 0 },
  { 2, 2, -7, -3, 0, -5, 13, 22, 12, -3, -3, 17, 0 },  { -1, 0, 6, -8, 1, -5, 1, 23, 0, 2, -5, 10, 0 },
  { 0, 0, -1, -1, 0, -1, 2, 1, 0, 0, -1, 4, 0 },       { 0, 0, 3, -11, 1, 0, -1, 35, 5, 2, -9, 9, 0 },
  { 0, 0, 8, -8, -2, -7, 4, 4, 2, 1, -1, 25, 0 },      { 0, 0, 1, -1, 0, -3, 1, 3, -1, 1, -1, 3, 0 },
  { 0, 0, 3, -3, 0, -6, 5, -1, 2, 1, -4, 21, 0 },      { -7, 1, 5, 4, -3, 5, 11, 13, 12, -8, 11, 12, 0 },
  { -5, -3, 6, -2, -3, 8, 14, 15, 2, -7, 11, 16, 0 },  { 2, -1, -6, -5, -2, -2, 20, 14, -4, 0, -3, 25, 0 },
  { 3, 1, -8, -4, 0, -8, 22, 5, -3, 2, -10, 29, 0 },   { 2, 1, -7, -1, 2, -11, 23, -5, 0, 2, -10, 29, 0 },
  { -6, -3, 8, 9, -4, 8, 9, 7, 14, -2, 8, 9, 0 },      { 2, 1, -4, -7, 0, -8, 17, 22, 1, -1, -4, 23, 0 },
  { 3, 0, -5, -7, 0, -7, 15, 18, -5, 0, -5, 27, 0 },   { 2, 0, 0, -7, 1, -10, 13, 13, -4, 2, -7, 24, 0 },
  { 3, 3, -13, 4, -2, -5, 9, 21, 25, -2, -3, 12, 0 },  { -5, -2, 7, -3, -7, 9, 8, 9, 16, -2, 15, 12, 0 },
  { 0, -1, 0, -7, -5, 4, 11, 11, 8, -6, 12, 21, 0 },   { 3, -2, -3, -8, -4, -1, 16, 15, -2, -3, 3, 26, 0 },
  { 2, 1, -5, -4, -1, -8, 16, 4, -2, 1, -7, 33, 0 },   { 2, 1, -4, -2, 1, -10, 17, -2, 0, 2, -11, 33, 0 },
  { 1, -2, 7, -15, -16, 10, 8, 8, 20, 11, 14, 11, 0 }, { 2, 2, 3, -13, -13, 4, 8, 12, 2, -3, 16, 24, 0 },
  { 1, 4, 0, -7, -8, -4, 9, 9, -2, -2, 8, 29, 0 },     { 1, 1, 2, -4, -1, -6, 6, 3, -1, -1, -3, 30, 0 },
  { -7, 3, 2, 10, -2, 3, 7, 11, 19, -7, 8, 10, 0 },    { 0, -2, -5, -3, -2, 4, 20, 15, -1, -3, -1, 22, 0 },
  { 3, -1, -8, -4, -1, -4, 22, 8, -4, 2, -8, 28, 0 },  { 0, 3, -14, 3, 0, 1, 19, 17, 8, -3, -7, 20, 0 },
  { 0, 2, -1, -8, 3, -6, 5, 21, 1, 1, -9, 13, 0 },     { -4, -2, 8, 20, -2, 2, 3, 5, 21, 4, 6, 1, 0 },
  { 2, -2, -3, -9, -4, 2, 14, 16, 3, -6, 8, 24, 0 },   { 2, 1, 5, -16, -7, 2, 3, 11, 15, -3, 11, 22, 0 },
  { 1, 2, 3, -11, -2, -5, 4, 8, 9, -3, -2, 26, 0 },    { 0, -1, 10, -9, -1, -8, 2, 3, 4, 0, 0, 29, 0 },
  { 1, 2, 0, -5, 1, -9, 9, 3, 0, 1, -7, 20, 0 },       { -2, 8, -6, -4, 3, -9, -8, 45, 14, 2, -13, 7, 0 },
  { 1, -1, 16, -19, -8, -4, -3, 2, 19, 0, 4, 30, 0 },  { 1, 1, -3, 0, 2, -11, 15, -5, 1, 2, -9, 24, 0 },
  { 0, 1, -2, 0, 1, -4, 4, 0, 0, 1, -4, 7, 0 },        { 0, 1, 2, -5, 1, -6, 4, 10, -2, 1, -4, 10, 0 },
  { 3, 0, -3, -6, -2, -6, 14, 8, -1, -1, -3, 31, 0 },  { 0, 1, 0, -2, 1, -6, 5, 1, 0, 1, -5, 13, 0 },
  { 3, 1, 9, -19, -21, 9, 7, 6, 13, 5, 15, 21, 0 },    { 2, 4, 3, -12, -13, 1, 7, 8, 3, 0, 12, 26, 0 },
  { 3, 1, -8, -2, 0, -6, 18, 2, -2, 3, -10, 23, 0 },   { 1, 1, -4, -1, 1, -5, 8, 1, -1, 2, -5, 10, 0 },
  { 0, 1, -1, 0, 0, -2, 2, 0, 0, 1, -2, 3, 0 },        { 1, 1, -2, -7, 1, -7, 14, 18, 0, 0, -7, 21, 0 },
  { 0, 1, 0, -2, 0, -7, 8, 1, -2, 0, -3, 24, 0 },      { 0, 1, 1, -2, 2, -10, 10, 0, -2, 1, -7, 23, 0 },
  { 0, 2, 2, -11, 2, -4, -3, 39, 7, 1, -10, 9, 0 },    { 1, 0, 13, -16, -5, -6, -1, 8, 6, 0, 6, 29, 0 },
  { 1, 3, 1, -6, -4, -7, 9, 6, -3, -2, 3, 33, 0 },     { 4, 0, -17, -1, -1, 5, 26, 8, -2, 3, -15, 30, 0 },
  { 0, 1, -2, 0, 2, -8, 12, -6, 1, 1, -6, 16, 0 },     { 0, 0, 0, -1, 1, -4, 4, 0, 0, 0, -3, 11, 0 },
  { 0, 1, 2, -8, 2, -6, 5, 15, 0, 2, -7, 9, 0 },       { 1, -1, 12, -15, -7, -2, 3, 6, 6, -1, 7, 30, 0 },
};

// clang-format off
const int AdaptiveLoopFilter::m_classToFilterMapping[ALF_NUM_FIXED_FILTER_SETS][MAX_NUM_ALF_CLASSES] =
{
  {  8,   2,   2,   2,   3,   4,  53,   9,   9,  52,   4,   4,   5,   9,   2,   8,  10,   9,   1,   3,  39,  39,  10,   9,  52 },
  { 11,  12,  13,  14,  15,  30,  11,  17,  18,  19,  16,  20,  20,   4,  53,  21,  22,  23,  14,  25,  26,  26,  27,  28,  10 },
  { 16,  12,  31,  32,  14,  16,  30,  33,  53,  34,  35,  16,  20,   4,   7,  16,  21,  36,  18,  19,  21,  26,  37,  38,  39 },
  { 35,  11,  13,  14,  43,  35,  16,   4,  34,  62,  35,  35,  30,  56,   7,  35,  21,  38,  24,  40,  16,  21,  48,  57,  39 },
  { 11,  31,  32,  43,  44,  16,   4,  17,  34,  45,  30,  20,  20,   7,   5,  21,  22,  46,  40,  47,  26,  48,  63,  58,  10 },
  { 12,  13,  50,  51,  52,  11,  17,  53,  45,   9,  30,   4,  53,  19,   0,  22,  23,  25,  43,  44,  37,  27,  28,  10,  55 },
  { 30,  33,  62,  51,  44,  20,  41,  56,  34,  45,  20,  41,  41,  56,   5,  30,  56,  38,  40,  47,  11,  37,  42,  57,   8 },
  { 35,  11,  23,  32,  14,  35,  20,   4,  17,  18,  21,  20,  20,  20,   4,  16,  21,  36,  46,  25,  41,  26,  48,  49,  58 },
  { 12,  31,  59,  59,   3,  33,  33,  59,  59,  52,   4,  33,  17,  59,  55,  22,  36,  59,  59,  60,  22,  36,  59,  25,  55 },
  { 31,  25,  15,  60,  60,  22,  17,  19,  55,  55,  20,  20,  53,  19,  55,  22,  46,  25,  43,  60,  37,  28,  10,  55,  52 },
  { 12,  31,  32,  50,  51,  11,  33,  53,  19,  45,  16,   4,   4,  53,   5,  22,  36,  18,  25,  43,  26,  27,  27,  28,  10 },
  {  5,   2,  44,  52,   3,   4,  53,  45,   9,   3,   4,  56,   5,   0,   2,   5,  10,  47,  52,   3,  63,  39,  10,   9,  52 },
  { 12,  34,  44,  44,   3,  56,  56,  62,  45,   9,  56,  56,   7,   5,   0,  22,  38,  40,  47,  52,  48,  57,  39,  10,   9 },
  { 35,  11,  23,  14,  51,  35,  20,  41,  56,  62,  16,  20,  41,  56,   7,  16,  21,  38,  24,  40,  26,  26,  42,  57,  39 },
  { 33,  34,  51,  51,  52,  41,  41,  34,  62,   0,  41,  41,  56,   7,   5,  56,  38,  38,  40,  44,  37,  42,  57,  39,  10 },
  { 16,  31,  32,  15,  60,  30,   4,  17,  19,  25,  22,  20,   4,  53,  19,  21,  22,  46,  25,  55,  26,  48,  63,  58,  55 },
};
// clang-format on

void AdaptiveLoopFilter::applyCcAlfFilter(CodingStructure& cs, ComponentID compID, const PelBuf& dstBuf,
                                          const PelUnitBuf& recYuvExt, uint8_t* filterControl,
                                          const AlfCoeff filterSet[MAX_NUM_CC_ALF_FILTERS][MAX_NUM_CC_ALF_CHROMA_COEFF],
                                          const int      selectedFilterIdx)
{
  bool clipTop = false, clipBottom = false, clipLeft = false, clipRight = false;
  int  numHorVirBndry = 0, numVerVirBndry = 0;
  int  horVirBndryPos[] = { 0, 0, 0 };
  int  verVirBndryPos[] = { 0, 0, 0 };

  for( int yPos = 0; yPos < m_picHeight; yPos += m_maxCUHeight )
  {
    for( int xPos = 0; xPos < m_picWidth; xPos += m_maxCUWidth )
    {
      int filterIdx =
        (filterControl == nullptr)
          ? selectedFilterIdx
          : filterControl[(yPos >> cs.pcv->maxCUHeightLog2) * cs.pcv->widthInCtus + (xPos >> cs.pcv->maxCUWidthLog2)];
      bool skipFiltering = (filterControl != nullptr && filterIdx == 0) ? true : false;
      if (!skipFiltering)
      {
        if (filterControl != nullptr)
        {
          filterIdx--;
        }

        const int16_t *filterCoeff = filterSet[filterIdx];

        const int width        = (xPos + m_maxCUWidth > m_picWidth) ? (m_picWidth - xPos) : m_maxCUWidth;
        const int height       = (yPos + m_maxCUHeight > m_picHeight) ? (m_picHeight - yPos) : m_maxCUHeight;
        const int chromaScaleX = getComponentScaleX(compID, m_chromaFormat);
        const int chromaScaleY = getComponentScaleY(compID, m_chromaFormat);

        int rasterSliceAlfPad = 0;
        if (isCrossedByVirtualBoundaries(cs, xPos, yPos, width, height, clipTop, clipBottom, clipLeft, clipRight,
                                         numHorVirBndry, numVerVirBndry, horVirBndryPos, verVirBndryPos,
                                         rasterSliceAlfPad))
        {
          int yStart = yPos;
          for (int i = 0; i <= numHorVirBndry; i++)
          {
            const int  yEnd   = i == numHorVirBndry ? yPos + height : horVirBndryPos[i];
            const int  h      = yEnd - yStart;
            const bool clipT  = (i == 0 && clipTop) || (i > 0) || (yStart == 0);
            const bool clipB  = (i == numHorVirBndry && clipBottom) || (i < numHorVirBndry) || (yEnd == m_picHeight);
            int        xStart = xPos;
            for (int j = 0; j <= numVerVirBndry; j++)
            {
              const int  xEnd  = j == numVerVirBndry ? xPos + width : verVirBndryPos[j];
              const int  w     = xEnd - xStart;
              const bool clipL = (j == 0 && clipLeft) || (j > 0) || (xStart == 0);
              const bool clipR = (j == numVerVirBndry && clipRight) || (j < numVerVirBndry) || (xEnd == m_picWidth);
              const int  wBuf  = w + (clipL ? 0 : MAX_ALF_PADDING_SIZE) + (clipR ? 0 : MAX_ALF_PADDING_SIZE);
              const int  hBuf  = h + (clipT ? 0 : MAX_ALF_PADDING_SIZE) + (clipB ? 0 : MAX_ALF_PADDING_SIZE);
              PelUnitBuf buf   = m_tempBuf2.subBuf(UnitArea(cs.area.chromaFormat, Area(0, 0, wBuf, hBuf)));
              buf.copyFrom(recYuvExt.subBuf(
                UnitArea(cs.area.chromaFormat, Area(xStart - (clipL ? 0 : MAX_ALF_PADDING_SIZE),
                                                    yStart - (clipT ? 0 : MAX_ALF_PADDING_SIZE), wBuf, hBuf))));
              // pad top-left unavailable samples for raster slice
              if (xStart == xPos && yStart == yPos && (rasterSliceAlfPad & 1))
              {
                buf.padBorderPel(MAX_ALF_PADDING_SIZE, 1);
              }

              // pad bottom-right unavailable samples for raster slice
              if (xEnd == xPos + width && yEnd == yPos + height && (rasterSliceAlfPad & 2))
              {
                buf.padBorderPel(MAX_ALF_PADDING_SIZE, 2);
              }
              buf.extendBorderPel(MAX_ALF_PADDING_SIZE);
              buf = buf.subBuf(UnitArea(
                cs.area.chromaFormat, Area(clipL ? 0 : MAX_ALF_PADDING_SIZE, clipT ? 0 : MAX_ALF_PADDING_SIZE, w, h)));

              const Area blkSrc(0, 0, w, h);

              const Area blkDst(xStart >> chromaScaleX, yStart >> chromaScaleY, w >> chromaScaleX, h >> chromaScaleY);
              m_filterCcAlf(dstBuf, buf, blkDst, blkSrc, compID, filterCoeff, m_clpRngs, cs, m_alfVBLumaCTUHeight,
                            m_alfVBLumaPos);
#if GREEN_METADATA_SEI_ENABLED
              cs.m_featureCounter.ccalf++;
#endif
              xStart = xEnd;
            }

            yStart = yEnd;
          }
        }
        else
        {
          const UnitArea area(m_chromaFormat, Area(xPos, yPos, width, height));

          Area blkDst(xPos >> chromaScaleX, yPos >> chromaScaleY, width >> chromaScaleX, height >> chromaScaleY);
          Area blkSrc(xPos, yPos, width, height);

          m_filterCcAlf(dstBuf, recYuvExt, blkDst, blkSrc, compID, filterCoeff, m_clpRngs, cs, m_alfVBLumaCTUHeight,
                        m_alfVBLumaPos);
#if GREEN_METADATA_SEI_ENABLED
          cs.m_featureCounter.ccalf++;
#endif
        }
      }
    }
  }
}

void AdaptiveLoopFilter::ALFProcess(CodingStructure& cs)
{

  ("Tempo na common lib\n");

  double time_classificaton, time_filter, time_alfprocess, time_filter5;
  timeval time_classificaton_start, time_classification_end, time_filter_start, time_filter_end, time_alfprocess_start, time_alfprocess_end;
  timeval time_filter5_start, time_filter5_end;

  // time alf process
  gettimeofday(&time_alfprocess_start, NULL);

  // set clipping range
  m_clpRngs = cs.slice->getClpRngs();

  // set CTU enable flags
  for( int compIdx = 0; compIdx < MAX_NUM_COMPONENT; compIdx++ )
  {
    m_modes[compIdx] = cs.picture->getAlfModes(compIdx);
  }
  AlfMode *lumaModes    = nullptr;
  uint32_t lastSliceIdx = 0xFFFFFFFF;

  PelUnitBuf recYuv = cs.getRecoBuf();
  m_tempBuf.copyFrom( recYuv );
  PelUnitBuf tmpYuv = m_tempBuf.getBuf( cs.area );
  tmpYuv.extendBorderPel( MAX_ALF_FILTER_LENGTH >> 1 );

  const PreCalcValues& pcv = *cs.pcv;

  int ctuIdx = 0;
  bool clipTop = false, clipBottom = false, clipLeft = false, clipRight = false;
  int numHorVirBndry = 0, numVerVirBndry = 0;
  int horVirBndryPos[] = { 0, 0, 0 };
  int verVirBndryPos[] = { 0, 0, 0 };

  for( int yPos = 0; yPos < pcv.lumaHeight; yPos += pcv.maxCUHeight )
  {
    for( int xPos = 0; xPos < pcv.lumaWidth; xPos += pcv.maxCUWidth )
    {
      // get first CU in CTU
      const CodingUnit *cu = cs.getCU(Position(xPos, yPos), ChannelType::LUMA);

      // skip this CTU if ALF is disabled
      if (!cu->slice->getAlfEnabledFlag(COMPONENT_Y) && !cu->slice->getAlfEnabledFlag(COMPONENT_Cb) && !cu->slice->getAlfEnabledFlag(COMPONENT_Cr))
      {
        ctuIdx++;
        continue;
      }

      // reload ALF APS each time the slice changes during raster scan filtering
      if (ctuIdx == 0 || lastSliceIdx != cu->slice->getSliceID() || lumaModes == nullptr)
      {
        cs.slice = cu->slice;
        reconstructCoeffAPSs(cs, true, cu->slice->getAlfEnabledFlag(COMPONENT_Cb) || cu->slice->getAlfEnabledFlag(COMPONENT_Cr), false);
        lumaModes          = cu->slice->getPic()->getAlfModes(COMPONENT_Y);
        m_ccAlfFilterParam = cu->slice->m_ccAlfFilterParam;
      }
      lastSliceIdx = cu->slice->getSliceID();

      const int width = ( xPos + pcv.maxCUWidth > pcv.lumaWidth ) ? ( pcv.lumaWidth - xPos ) : pcv.maxCUWidth;
      const int height = ( yPos + pcv.maxCUHeight > pcv.lumaHeight ) ? ( pcv.lumaHeight - yPos ) : pcv.maxCUHeight;
      bool      ctuEnableFlag = m_modes[COMPONENT_Y][ctuIdx] != AlfMode::OFF;
      for( int compIdx = 1; compIdx < MAX_NUM_COMPONENT; compIdx++ )
      {
        ctuEnableFlag |= m_modes[compIdx][ctuIdx] != AlfMode::OFF;
        if (cu->slice->m_ccAlfFilterParam.ccAlfFilterEnabled[compIdx - 1])
        {
          ctuEnableFlag |= m_ccAlfFilterControl[compIdx - 1][ctuIdx] > 0;
        }
      }
      int rasterSliceAlfPad = 0;
      if( ctuEnableFlag && isCrossedByVirtualBoundaries( cs, xPos, yPos, width, height, clipTop, clipBottom, clipLeft, clipRight, numHorVirBndry, numVerVirBndry, horVirBndryPos, verVirBndryPos, rasterSliceAlfPad ) )
      {
        int yStart = yPos;
        for( int i = 0; i <= numHorVirBndry; i++ )
        {
          const int yEnd = i == numHorVirBndry ? yPos + height : horVirBndryPos[i];
          const int h = yEnd - yStart;
          const bool clipT = ( i == 0 && clipTop ) || ( i > 0 ) || ( yStart == 0 );
          const bool clipB = ( i == numHorVirBndry && clipBottom ) || ( i < numHorVirBndry ) || ( yEnd == pcv.lumaHeight );
          int xStart = xPos;
          for( int j = 0; j <= numVerVirBndry; j++ )
          {
            const int xEnd = j == numVerVirBndry ? xPos + width : verVirBndryPos[j];
            const int w = xEnd - xStart;
            const bool clipL = ( j == 0 && clipLeft ) || ( j > 0 ) || ( xStart == 0 );
            const bool clipR = ( j == numVerVirBndry && clipRight ) || ( j < numVerVirBndry ) || ( xEnd == pcv.lumaWidth );
            const int wBuf = w + (clipL ? 0 : MAX_ALF_PADDING_SIZE) + (clipR ? 0 : MAX_ALF_PADDING_SIZE);
            const int hBuf = h + (clipT ? 0 : MAX_ALF_PADDING_SIZE) + (clipB ? 0 : MAX_ALF_PADDING_SIZE);
            PelUnitBuf buf = m_tempBuf2.subBuf( UnitArea( cs.area.chromaFormat, Area( 0, 0, wBuf, hBuf ) ) );
            buf.copyFrom( tmpYuv.subBuf( UnitArea( cs.area.chromaFormat, Area( xStart - (clipL ? 0 : MAX_ALF_PADDING_SIZE), yStart - (clipT ? 0 : MAX_ALF_PADDING_SIZE), wBuf, hBuf ) ) ) );
            // pad top-left unavailable samples for raster slice
            if ( xStart == xPos && yStart == yPos && ( rasterSliceAlfPad & 1 ) )
            {
              buf.padBorderPel( MAX_ALF_PADDING_SIZE, 1 );
            }

            // pad bottom-right unavailable samples for raster slice
            if ( xEnd == xPos + width && yEnd == yPos + height && ( rasterSliceAlfPad & 2 ) )
            {
              buf.padBorderPel( MAX_ALF_PADDING_SIZE, 2 );
            }
            buf.extendBorderPel( MAX_ALF_PADDING_SIZE );
            buf = buf.subBuf( UnitArea ( cs.area.chromaFormat, Area( clipL ? 0 : MAX_ALF_PADDING_SIZE, clipT ? 0 : MAX_ALF_PADDING_SIZE, w, h ) ) );

            if (m_modes[COMPONENT_Y][ctuIdx] != AlfMode::OFF)
            {
              const Area blkSrc( 0, 0, w, h );
              const Area blkDst( xStart, yStart, w, h );
              // time classification
              gettimeofday(&time_classificaton_start, NULL);
              deriveClassification( m_classifier, buf.get(COMPONENT_Y), blkDst, blkSrc );
              gettimeofday(&time_classification_end, NULL);
              time_classificaton = (double) (time_classification_end.tv_usec - time_classificaton_start.tv_usec)/1000000 + (double) (time_classification_end.tv_sec - time_classificaton_start.tv_sec);

              const AlfMode m     = lumaModes[ctuIdx];
              const AlfCoeff* coeff = getCoeffVals(m);
              const Pel*    clip  = getClipVals(m);
#if GREEN_METADATA_SEI_ENABLED
              cs.m_featureCounter.alfLumaType7+= (width * height / 16) ;
              cs.m_featureCounter.alfLumaPels += (width * height);
#endif
              //time filtering classification
              gettimeofday(&time_filter_start, NULL);

              m_filter7x7Blk(m_classifier, recYuv, buf, blkDst, blkSrc, COMPONENT_Y, coeff, clip, m_clpRngs.comp[COMPONENT_Y], cs
                , m_alfVBLumaCTUHeight
                , m_alfVBLumaPos
              );

              gettimeofday(&time_filter_end, NULL);
              time_filter = (double) (time_filter_end.tv_usec - time_filter_start.tv_usec)/1000000 + (double) (time_filter_end.tv_sec - time_filter_start.tv_sec);

            }

            for( int compIdx = 1; compIdx < MAX_NUM_COMPONENT; compIdx++ )
            {
              ComponentID compID = ComponentID( compIdx );
              const int chromaScaleX = getComponentScaleX( compID, tmpYuv.chromaFormat );
              const int chromaScaleY = getComponentScaleY( compID, tmpYuv.chromaFormat );
              
              gettimeofday(&time_filter5_start, NULL);

              if (m_modes[compIdx][ctuIdx] != AlfMode::OFF)
              {
                const Area blkSrc( 0, 0, w >> chromaScaleX, h >> chromaScaleY );
                const Area blkDst( xStart >> chromaScaleX, yStart >> chromaScaleY, w >> chromaScaleX, h >> chromaScaleY );
                const int  altNum = m_modes[compIdx][ctuIdx] - AlfMode::CHROMA0;
                m_filter5x5Blk(m_classifier, recYuv, buf, blkDst, blkSrc, compID, m_chromaCoeffFinal[altNum],
                               m_chromaClipValsFinal[altNum], m_clpRngs.comp[compIdx], cs, m_alfVBChmaCTUHeight,
                               m_alfVBChmaPos);
#if GREEN_METADATA_SEI_ENABLED
                cs.m_featureCounter.alfChromaType5+= ((width >> chromaScaleX) * (height >> chromaScaleY) / 16) ;
                cs.m_featureCounter.alfChromaPels += ((width >> chromaScaleX) * (height >> chromaScaleY)) ;
#endif
              }
              gettimeofday(&time_filter5_end, NULL);
              time_filter5 = (double) (time_filter5_end.tv_usec - time_filter5_start.tv_usec)/1000000 + (double) (time_filter5_end.tv_sec - time_filter5_start.tv_sec);

              if (cu->slice->m_ccAlfFilterParam.ccAlfFilterEnabled[compIdx - 1])
              {
                const int filterIdx = m_ccAlfFilterControl[compIdx - 1][ctuIdx];

                if (filterIdx != 0)
                {
                  const Area blkSrc(0, 0, w, h);
                  Area blkDst(xStart >> chromaScaleX, yStart >> chromaScaleY, w >> chromaScaleX, h >> chromaScaleY);

                  const AlfCoeff* filterCoeff = m_ccAlfFilterParam.ccAlfCoeff[compIdx - 1][filterIdx - 1];
#if GREEN_METADATA_SEI_ENABLED
                  cs.m_featureCounter.alfLumaType7+= (width * height / 16) ;
                  cs.m_featureCounter.alfLumaPels += (width * height);
#endif
                  m_filterCcAlf(recYuv.get(compID), buf, blkDst, blkSrc, compID, filterCoeff, m_clpRngs, cs,
                                m_alfVBLumaCTUHeight, m_alfVBLumaPos);
                }
              }
            }

            xStart = xEnd;
          }

          yStart = yEnd;
        }
      }
      else
      {
        const UnitArea area( cs.area.chromaFormat, Area( xPos, yPos, width, height ) );
        if (m_modes[COMPONENT_Y][ctuIdx] != AlfMode::OFF)
        {

          Area blk( xPos, yPos, width, height );
          gettimeofday(&time_classificaton_start, NULL);
          deriveClassification( m_classifier, tmpYuv.get( COMPONENT_Y ), blk, blk );
          gettimeofday(&time_classification_end, NULL);
          time_classificaton = (double) (time_classification_end.tv_usec - time_classificaton_start.tv_usec)/1000000 + (double) (time_classification_end.tv_sec - time_classificaton_start.tv_sec);

          const AlfMode m     = lumaModes[ctuIdx];
          const AlfCoeff* coeff = getCoeffVals(m);
          const Pel*    clip  = getClipVals(m);
#if GREEN_METADATA_SEI_ENABLED
          cs.m_featureCounter.alfLumaType7+= (width * height / 16) ;
          cs.m_featureCounter.alfLumaPels += (width * height);
#endif
          gettimeofday(&time_filter_start, NULL);
          m_filter7x7Blk(m_classifier, recYuv, tmpYuv, blk, blk, COMPONENT_Y, coeff, clip, m_clpRngs.comp[COMPONENT_Y],
                         cs, m_alfVBLumaCTUHeight, m_alfVBLumaPos);
          gettimeofday(&time_filter_end, NULL);
          time_filter = (double) (time_filter_end.tv_usec - time_filter_start.tv_usec)/1000000 + (double) (time_filter_end.tv_sec - time_filter_start.tv_sec);

        }

        for( int compIdx = 1; compIdx < MAX_NUM_COMPONENT; compIdx++ )
        {
          ComponentID compID = ComponentID( compIdx );
          const int chromaScaleX = getComponentScaleX( compID, tmpYuv.chromaFormat );
          const int chromaScaleY = getComponentScaleY( compID, tmpYuv.chromaFormat );

          if (m_modes[compIdx][ctuIdx] != AlfMode::OFF)
          {
            Area    blk(xPos >> chromaScaleX, yPos >> chromaScaleY, width >> chromaScaleX, height >> chromaScaleY);
            const int altNum = m_modes[compIdx][ctuIdx] - AlfMode::CHROMA0;
#if GREEN_METADATA_SEI_ENABLED
            cs.m_featureCounter.alfChromaType5+= ((width >> chromaScaleX) * (height >> chromaScaleY) / 16) ;
            cs.m_featureCounter.alfChromaPels += ((width >> chromaScaleX) * (height >> chromaScaleY)) ;
#endif
            gettimeofday(&time_filter5_start, NULL);

            m_filter5x5Blk(m_classifier, recYuv, tmpYuv, blk, blk, compID, m_chromaCoeffFinal[altNum],
                           m_chromaClipValsFinal[altNum], m_clpRngs.comp[compIdx], cs, m_alfVBChmaCTUHeight,
                           m_alfVBChmaPos);
            gettimeofday(&time_filter5_end, NULL);
            time_filter5 = (double) (time_filter5_end.tv_usec - time_filter5_start.tv_usec)/1000000 + (double) (time_filter5_end.tv_sec - time_filter5_start.tv_sec);

          }
          if (cu->slice->m_ccAlfFilterParam.ccAlfFilterEnabled[compIdx - 1])
          {
            const int filterIdx = m_ccAlfFilterControl[compIdx - 1][ctuIdx];

            if (filterIdx != 0)
            {
              Area blkDst(xPos >> chromaScaleX, yPos >> chromaScaleY, width >> chromaScaleX, height >> chromaScaleY);
              Area blkSrc(xPos, yPos, width, height);

              const int16_t *filterCoeff = m_ccAlfFilterParam.ccAlfCoeff[compIdx - 1][filterIdx - 1];
#if GREEN_METADATA_SEI_ENABLED
              cs.m_featureCounter.ccalf++;
#endif
              m_filterCcAlf(recYuv.get(compID), tmpYuv, blkDst, blkSrc, compID, filterCoeff, m_clpRngs, cs,
                            m_alfVBLumaCTUHeight, m_alfVBLumaPos);
            }
          }
        }
      }
      ctuIdx++;
    }
  }

  gettimeofday(&time_alfprocess_end, NULL);
  time_alfprocess = (double) (time_alfprocess_end.tv_usec - time_alfprocess_start.tv_usec)/1000000 + (double) (time_alfprocess_end.tv_sec - time_alfprocess_start.tv_sec);
  cs.picture->getPOC(); // numero do frame
  printf("[DECODER] POC %d, Tempo AlfProcess: %f, Tempo Classificacao: %f, Tempo Filtragem 7x7: %f, Tempo filtragem 5x5 (extra): %f,  Tempo Outros: %f", cs.picture->getPOC(), time_alfprocess,  time_classificaton, time_filter,  time_filter5,(time_alfprocess-time_classificaton-time_filter-time_filter5));


}

void AdaptiveLoopFilter::reconstructCoeffAPSs(CodingStructure& cs, bool luma, bool chroma, bool isRdo)
{
  //luma
  APS** aps = cs.slice->getAlfAPSs();
  AlfParam alfParamTmp;
  APS* curAPS;
  if (luma)
  {
    for (int i = 0; i < cs.slice->getNumAlfApsIdsLuma(); i++)
    {
      int apsIdx = cs.slice->getAlfApsIdsLuma()[i];
      curAPS = aps[apsIdx];
      CHECK(curAPS == nullptr, "invalid APS");
      alfParamTmp = curAPS->getAlfAPSParam();
      reconstructCoeff(alfParamTmp, ChannelType::LUMA, isRdo, true);
      memcpy(m_coeffApsLuma[i], m_coeffFinal, sizeof(m_coeffFinal));
      memcpy(m_clippApsLuma[i], m_clippFinal, sizeof(m_clippFinal));
      memcpy(m_clipValsApsLuma[i], m_clipValsFinal, sizeof(m_clipValsFinal));
    }
  }

  //chroma
  if (chroma)
  {
    int apsIdxChroma = cs.slice->getAlfApsIdChroma();
    curAPS = aps[apsIdxChroma];
    m_alfParamChroma = &curAPS->getAlfAPSParam();
    alfParamTmp = *m_alfParamChroma;
    reconstructCoeff(alfParamTmp, ChannelType::CHROMA, isRdo, true);
  }
}

void AdaptiveLoopFilter::reconstructCoeff(AlfParam &alfParam, const ChannelType channel, const bool isRdo,
                                          const bool isRedo)
{
  const int factor = isRdo ? 0 : 1 << COEFF_SCALE_BITS;

  AlfFilterType filterType = isLuma( channel ) ? ALF_FILTER_7 : ALF_FILTER_5;
  int numClasses = isLuma( channel ) ? MAX_NUM_ALF_CLASSES : 1;
  int numCoeff = filterType == ALF_FILTER_5 ? 7 : 13;
  int numCoeffMinus1 = numCoeff - 1;
  const int numAlts = isLuma( channel ) ? 1 : alfParam.numAlternativesChroma;

  for( int altIdx = 0; altIdx < numAlts; ++ altIdx )
  {
    int numFilters = isLuma( channel ) ? alfParam.numLumaFilters : 1;
    AlfCoeff* coeff      = isLuma(channel) ? alfParam.lumaCoeff : alfParam.chromaCoeff[altIdx];
    AlfClipIdx* clipp      = isLuma(channel) ? alfParam.lumaClipp : alfParam.chromaClipp[altIdx];

    for( int filterIdx = 0; filterIdx < numFilters; filterIdx++ )
    {
      coeff[filterIdx* MAX_NUM_ALF_LUMA_COEFF + numCoeffMinus1] = factor;
    }

    if( isChroma( channel ) )
    {
      for( int coeffIdx = 0; coeffIdx < numCoeffMinus1; ++coeffIdx )
      {
        m_chromaCoeffFinal[altIdx][coeffIdx] = coeff[coeffIdx];
        const int clipIdx                    = alfParam.nonLinearFlag[channel] ? clipp[coeffIdx] : 0;
        m_chromaClippFinal[altIdx][coeffIdx]    = clipIdx;
        m_chromaClipValsFinal[altIdx][coeffIdx] = m_alfClippingValues[channel][clipIdx];
      }
      m_chromaCoeffFinal[altIdx][numCoeffMinus1] = factor;
      m_chromaClippFinal[altIdx][numCoeffMinus1]    = 0;
      m_chromaClipValsFinal[altIdx][numCoeffMinus1] = m_alfClippingValues[channel][0];
      continue;
    }
    for( int classIdx = 0; classIdx < numClasses; classIdx++ )
    {
      AlfBankIdx filterIdx = alfParam.filterCoeffDeltaIdx[classIdx];

      CHECK(!(filterIdx >= 0 && filterIdx < alfParam.numLumaFilters), "Bad coeff delta idx in ALF");
      for (int coeffIdx = 0; coeffIdx < numCoeffMinus1; ++coeffIdx)
      {
        m_coeffFinal[classIdx * MAX_NUM_ALF_LUMA_COEFF + coeffIdx] = coeff[filterIdx * MAX_NUM_ALF_LUMA_COEFF + coeffIdx];
      }
      m_coeffFinal[classIdx* MAX_NUM_ALF_LUMA_COEFF + numCoeffMinus1] = factor;
      m_clippFinal[classIdx * MAX_NUM_ALF_LUMA_COEFF + numCoeffMinus1]    = 0;
      m_clipValsFinal[classIdx * MAX_NUM_ALF_LUMA_COEFF + numCoeffMinus1] = m_alfClippingValues[channel][0];
      for( int coeffIdx = 0; coeffIdx < numCoeffMinus1; ++coeffIdx )
      {
        int clipIdx = alfParam.nonLinearFlag[channel] ? clipp[filterIdx * MAX_NUM_ALF_LUMA_COEFF + coeffIdx] : 0;
        CHECK(!(clipIdx >= 0 && clipIdx < MAX_ALF_NUM_CLIP_VALS), "Bad clip idx in ALF");
        m_clippFinal[classIdx * MAX_NUM_ALF_LUMA_COEFF + coeffIdx]    = clipIdx;
        m_clipValsFinal[classIdx * MAX_NUM_ALF_LUMA_COEFF + coeffIdx] = m_alfClippingValues[channel][clipIdx];
      }
      m_clippFinal[classIdx * MAX_NUM_ALF_LUMA_COEFF + numCoeffMinus1]    = 0;
      m_clipValsFinal[classIdx * MAX_NUM_ALF_LUMA_COEFF + numCoeffMinus1] = m_alfClippingValues[channel][0];
    }
  }
}

void AdaptiveLoopFilter::create(const int picWidth, const int picHeight, const ChromaFormat format,
                                const int maxCUWidth, const int maxCUHeight, const int maxCUDepth,
                                const BitDepths &inputBitDepth)
{
  destroy();
  m_inputBitDepth = inputBitDepth;
  m_picWidth = picWidth;
  m_picHeight = picHeight;
  m_maxCUWidth = maxCUWidth;
  m_maxCUHeight = maxCUHeight;
  m_maxCUDepth = maxCUDepth;
  m_chromaFormat = format;

  m_numCTUsInWidth = ( m_picWidth / m_maxCUWidth ) + ( ( m_picWidth % m_maxCUWidth ) ? 1 : 0 );
  m_numCTUsInHeight = ( m_picHeight / m_maxCUHeight ) + ( ( m_picHeight % m_maxCUHeight ) ? 1 : 0 );
  m_numCTUsInPic = m_numCTUsInHeight * m_numCTUsInWidth;
  m_filterShapesCcAlf[0].push_back(AlfFilterShape(size_CC_ALF));
  m_filterShapesCcAlf[1].push_back(AlfFilterShape(size_CC_ALF));
  m_filterShapes[ChannelType::LUMA].push_back(AlfFilterShape(7));
  m_filterShapes[ChannelType::CHROMA].push_back(AlfFilterShape(5));
  m_alfVBLumaPos = m_maxCUHeight - ALF_VB_POS_ABOVE_CTUROW_LUMA;
  m_alfVBChmaPos = (m_maxCUHeight >> ((m_chromaFormat == ChromaFormat::_420) ? 1 : 0)) - ALF_VB_POS_ABOVE_CTUROW_CHMA;

  m_alfVBLumaCTUHeight = m_maxCUHeight;
  m_alfVBChmaCTUHeight = (m_maxCUHeight >> ((m_chromaFormat == ChromaFormat::_420) ? 1 : 0));

  CHECK(ALF_NUM_CLIP_VALS[ChannelType::LUMA] < 1, "ALF_NUM_CLIP_VALS[ChannelType::LUMA] must be at least one");
  m_alfClippingValues[ChannelType::LUMA][0] = 1 << m_inputBitDepth[ChannelType::LUMA];
  int shiftLuma                             = m_inputBitDepth[ChannelType::LUMA] - 8;
  for (int i = 1; i < ALF_NUM_CLIP_VALS[ChannelType::LUMA]; ++i)
  {
    m_alfClippingValues[ChannelType::LUMA][i] = 1 << (7 - 2 * i + shiftLuma);
  }
  CHECK(ALF_NUM_CLIP_VALS[ChannelType::CHROMA] < 1, "ALF_NUM_CLIP_VALS[ChannelType::CHROMA] must be at least one");
  m_alfClippingValues[ChannelType::CHROMA][0] = 1 << m_inputBitDepth[ChannelType::CHROMA];
  int shiftChroma                             = m_inputBitDepth[ChannelType::CHROMA] - 8;
  for (int i = 1; i < ALF_NUM_CLIP_VALS[ChannelType::CHROMA]; ++i)
  {
    m_alfClippingValues[ChannelType::CHROMA][i] = 1 << (7 - 2 * i + shiftChroma);
  }
  if (m_created)
  {
    return;
  }

  m_tempBuf.destroy();
  // NOTE: make border 1 sample wider to avoid out-of-bounds memory access in SIMD code (simdDeriveClassificationBlk
  // function)
  m_tempBuf.create(format, Area(0, 0, picWidth, picHeight), maxCUWidth, (MAX_ALF_FILTER_LENGTH + 1) >> 1, 0, false);
  m_tempBuf2.destroy();
  m_tempBuf2.create( format, Area( 0, 0, maxCUWidth + (MAX_ALF_PADDING_SIZE << 1), maxCUHeight + (MAX_ALF_PADDING_SIZE << 1) ), maxCUWidth, MAX_ALF_PADDING_SIZE, 0, false );

  // Classification
  if ( m_classifier == nullptr )
  {
    m_classifier = new AlfClassifier*[picHeight];
    m_classifier[0] = new AlfClassifier[picWidth * picHeight];

    for (int i = 1; i < picHeight; i++)
    {
      m_classifier[i] = m_classifier[0] + i * picWidth;
    }
  }

  for (int filterSetIndex = 0; filterSetIndex < ALF_NUM_FIXED_FILTER_SETS; filterSetIndex++)
  {
    for (int classIdx = 0; classIdx < MAX_NUM_ALF_CLASSES; classIdx++)
    {
      int fixedFilterIdx = m_classToFilterMapping[filterSetIndex][classIdx];
      for (int i = 0; i < MAX_NUM_ALF_LUMA_COEFF - 1; i++)
      {
       m_fixedFilterSetCoeffDec[filterSetIndex][classIdx * MAX_NUM_ALF_LUMA_COEFF + i] = m_fixedFilterSetCoeff[fixedFilterIdx][i];
      }
      m_fixedFilterSetCoeffDec[filterSetIndex][classIdx * MAX_NUM_ALF_LUMA_COEFF + MAX_NUM_ALF_LUMA_COEFF - 1] =
        1 << COEFF_SCALE_BITS;
    }
  }
  for (int i = 0; i < MAX_NUM_ALF_LUMA_COEFF * MAX_NUM_ALF_CLASSES; i++)
  {
    m_clipDefault[i] = m_alfClippingValues[ChannelType::LUMA][0];
  }
  m_created = true;

  m_ccAlfFilterControl[0] = new uint8_t[m_numCTUsInPic];
  m_ccAlfFilterControl[1] = new uint8_t[m_numCTUsInPic];
}

void AdaptiveLoopFilter::destroy()
{
  if (!m_created)
  {
    return;
  }

  if( m_classifier )
  {
    delete[] m_classifier[0];
    delete[] m_classifier;
    m_classifier = nullptr;
  }

  m_tempBuf.destroy();
  m_tempBuf2.destroy();
  m_filterShapes[ChannelType::LUMA].clear();
  m_filterShapes[ChannelType::CHROMA].clear();
  m_created = false;

  m_filterShapesCcAlf[0].clear();
  m_filterShapesCcAlf[1].clear();
  if ( m_ccAlfFilterControl[0] )
  {
    delete [] m_ccAlfFilterControl[0];
    m_ccAlfFilterControl[0] = nullptr;
  }

  if ( m_ccAlfFilterControl[1] )
  {
    delete [] m_ccAlfFilterControl[1];
    m_ccAlfFilterControl[1] = nullptr;
  }
}

void AdaptiveLoopFilter::deriveClassification( AlfClassifier** classifier, const CPelBuf& srcLuma, const Area& blkDst, const Area& blk )
{
  int height = blk.pos().y + blk.height;
  int width = blk.pos().x + blk.width;

  for( int i = blk.pos().y; i < height; i += m_CLASSIFICATION_BLK_SIZE )
  {
    int nHeight = std::min( i + m_CLASSIFICATION_BLK_SIZE, height ) - i;

    for( int j = blk.pos().x; j < width; j += m_CLASSIFICATION_BLK_SIZE )
    {
      int nWidth = std::min( j + m_CLASSIFICATION_BLK_SIZE, width ) - j;
      m_deriveClassificationBlk(
        classifier, m_laplacian, srcLuma,
        Area(j - blk.pos().x + blkDst.pos().x, i - blk.pos().y + blkDst.pos().y, nWidth, nHeight),
        Area(j, i, nWidth, nHeight), m_inputBitDepth[ChannelType::LUMA] + 4, m_alfVBLumaCTUHeight, m_alfVBLumaPos);
    }
  }
}

void AdaptiveLoopFilter::deriveClassificationBlk(AlfClassifier **classifier, int **laplacian[NUM_DIRECTIONS],
                                                 const CPelBuf &srcLuma, const Area &blkDst, const Area &blk,
                                                 const int shift, const int vbCTUHeight, int vbPos)
{
  CHECK((vbCTUHeight & (vbCTUHeight - 1)) != 0, "vbCTUHeight must be a power of 2");

  static const int th[16] = { 0, 1, 2, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3, 3, 3, 4 };

  const ptrdiff_t stride = srcLuma.stride;

  const Pel* src = srcLuma.buf;
  const int maxActivity = 15;

  int fl = 2;
  int flP1 = fl + 1;
  int fl2 = 2 * fl;

  int mainDirection, secondaryDirection, dirTempHV, dirTempD;

  int pixY;
  int height = blk.height + fl2;
  int width = blk.width + fl2;
  int posX = blk.pos().x;
  int posY = blk.pos().y;
  int startHeight = posY - flP1;

  for( int i = 0; i < height; i += 2 )
  {
    const ptrdiff_t yoffset = (i + 1 + startHeight) * stride - flP1;

    const Pel *src0 = &src[yoffset - stride];
    const Pel *src1 = &src[yoffset];
    const Pel *src2 = &src[yoffset + stride];
    const Pel *src3 = &src[yoffset + stride * 2];

    const int y = blkDst.pos().y - 2 + i;
    if (y > 0 && (y & (vbCTUHeight - 1)) == vbPos - 2)
    {
      src3 = &src[yoffset + stride];
    }
    else if (y > 0 && (y & (vbCTUHeight - 1)) == vbPos)
    {
      src0 = &src[yoffset];
    }
    int* pYver = laplacian[VER][i];
    int* pYhor = laplacian[HOR][i];
    int* pYdig0 = laplacian[DIAG0][i];
    int* pYdig1 = laplacian[DIAG1][i];

    for( int j = 0; j < width; j += 2 )
    {
      pixY = j + 1 + posX;
      const Pel *pY = src1 + pixY;
      const Pel* pYdown = src0 + pixY;
      const Pel* pYup = src2 + pixY;
      const Pel* pYup2 = src3 + pixY;

      const Pel y0 = pY[0] << 1;
      const Pel yup1 = pYup[1] << 1;

      pYver[j] = abs( y0 - pYdown[0] - pYup[0] ) + abs( yup1 - pY[1] - pYup2[1] );
      pYhor[j] = abs( y0 - pY[1] - pY[-1] ) + abs( yup1 - pYup[2] - pYup[0] );
      pYdig0[j] = abs( y0 - pYdown[-1] - pYup[1] ) + abs( yup1 - pY[0] - pYup2[2] );
      pYdig1[j] = abs( y0 - pYup[-1] - pYdown[1] ) + abs( yup1 - pYup2[0] - pY[2] );

      if( j > 4 && ( j - 6 ) % 4 == 0 )
      {
        int jM6 = j - 6;
        int jM4 = j - 4;
        int jM2 = j - 2;
        pYver[jM6] += pYver[jM4] + pYver[jM2] + pYver[j];
        pYhor[jM6] += pYhor[jM4] + pYhor[jM2] + pYhor[j];
        pYdig0[jM6] += pYdig0[jM4] + pYdig0[jM2] + pYdig0[j];
        pYdig1[jM6] += pYdig1[jM4] + pYdig1[jM2] + pYdig1[j];
      }
    }
  }

  // classification block size
  const int clsSizeY = 4;
  const int clsSizeX = 4;

  for( int i = 0; i < blk.height; i += clsSizeY )
  {
    int* pYver = laplacian[VER][i];
    int* pYver2 = laplacian[VER][i + 2];
    int* pYver4 = laplacian[VER][i + 4];
    int* pYver6 = laplacian[VER][i + 6];

    int* pYhor = laplacian[HOR][i];
    int* pYhor2 = laplacian[HOR][i + 2];
    int* pYhor4 = laplacian[HOR][i + 4];
    int* pYhor6 = laplacian[HOR][i + 6];

    int* pYdig0 = laplacian[DIAG0][i];
    int* pYdig02 = laplacian[DIAG0][i + 2];
    int* pYdig04 = laplacian[DIAG0][i + 4];
    int* pYdig06 = laplacian[DIAG0][i + 6];

    int* pYdig1 = laplacian[DIAG1][i];
    int* pYdig12 = laplacian[DIAG1][i + 2];
    int* pYdig14 = laplacian[DIAG1][i + 4];
    int* pYdig16 = laplacian[DIAG1][i + 6];

    for( int j = 0; j < blk.width; j += clsSizeX )
    {
      int sumV = 0; int sumH = 0; int sumD0 = 0; int sumD1 = 0;
      if (((i + blkDst.pos().y) % vbCTUHeight) == (vbPos - 4))
      {
        sumV = pYver[j] + pYver2[j] + pYver4[j];
        sumH = pYhor[j] + pYhor2[j] + pYhor4[j];
        sumD0 = pYdig0[j] + pYdig02[j] + pYdig04[j];
        sumD1 = pYdig1[j] + pYdig12[j] + pYdig14[j];
      }
      else if (((i + blkDst.pos().y) % vbCTUHeight) == vbPos)
      {
        sumV = pYver2[j] + pYver4[j] + pYver6[j];
        sumH = pYhor2[j] + pYhor4[j] + pYhor6[j];
        sumD0 = pYdig02[j] + pYdig04[j] + pYdig06[j];
        sumD1 = pYdig12[j] + pYdig14[j] + pYdig16[j];
      }
      else
      {
        sumV = pYver[j] + pYver2[j] + pYver4[j] + pYver6[j];
        sumH = pYhor[j] + pYhor2[j] + pYhor4[j] + pYhor6[j];
        sumD0 = pYdig0[j] + pYdig02[j] + pYdig04[j] + pYdig06[j];
        sumD1 = pYdig1[j] + pYdig12[j] + pYdig14[j] + pYdig16[j];
      }

      int tempAct = sumV + sumH;
      int activity = 0;

      const int y = (i + blkDst.pos().y) & (vbCTUHeight - 1);
      if (y == vbPos - 4 || y == vbPos)
      {
        activity = (Pel)Clip3<int>(0, maxActivity, (tempAct * 96) >> shift);
      }
      else
      {
        activity = (Pel)Clip3<int>(0, maxActivity, (tempAct * 64) >> shift);
      }
      int classIdx = th[activity];

      int hv1, hv0, d1, d0, hvd1, hvd0;

      if( sumV > sumH )
      {
        hv1 = sumV;
        hv0 = sumH;
        dirTempHV = 1;
      }
      else
      {
        hv1 = sumH;
        hv0 = sumV;
        dirTempHV = 3;
      }
      if( sumD0 > sumD1 )
      {
        d1 = sumD0;
        d0 = sumD1;
        dirTempD = 0;
      }
      else
      {
        d1 = sumD1;
        d0 = sumD0;
        dirTempD = 2;
      }
#if RExt__HIGH_BIT_DEPTH_SUPPORT
      if( (uint64_t)d1 * (uint64_t)hv0 > (uint64_t)hv1 * (uint64_t)d0 )
#else
      if( (uint32_t)d1 * (uint32_t)hv0 > (uint32_t)hv1 * (uint32_t)d0 )
#endif
      {
        hvd1 = d1;
        hvd0 = d0;
        mainDirection = dirTempD;
        secondaryDirection = dirTempHV;
      }
      else
      {
        hvd1 = hv1;
        hvd0 = hv0;
        mainDirection = dirTempHV;
        secondaryDirection = dirTempD;
      }

      int directionStrength = 0;
      if( hvd1 > 2 * hvd0 )
      {
        directionStrength = 1;
      }
      if( hvd1 * 2 > 9 * hvd0 )
      {
        directionStrength = 2;
      }

      if( directionStrength )
      {
        classIdx += ( ( ( mainDirection & 0x1 ) << 1 ) + directionStrength ) * 5;
      }

      static const int transposeTable[8] = { 0, 1, 0, 2, 2, 3, 1, 3 };
      int transposeIdx = transposeTable[mainDirection * 2 + ( secondaryDirection >> 1 )];

      int yOffset = i + blkDst.pos().y;
      int xOffset = j + blkDst.pos().x;

      AlfClassifier *cl0 = classifier[yOffset] + xOffset;
      AlfClassifier *cl1 = classifier[yOffset + 1] + xOffset;
      AlfClassifier *cl2 = classifier[yOffset + 2] + xOffset;
      AlfClassifier *cl3 = classifier[yOffset + 3] + xOffset;
      cl0[0] = cl0[1] = cl0[2] = cl0[3] = cl1[0] = cl1[1] = cl1[2] = cl1[3] = cl2[0] = cl2[1] = cl2[2] = cl2[3] = cl3[0] = cl3[1] = cl3[2] = cl3[3] = AlfClassifier( classIdx, transposeIdx );
    }
  }
}

template<AlfFilterType filtType>
void AdaptiveLoopFilter::filterBlk(AlfClassifier** classifier, const PelUnitBuf& recDst, const CPelUnitBuf& recSrc,
                                   const Area& blkDst, const Area& blk, const ComponentID compId,
                                   const AlfCoeff* filterSet, const Pel* fClipSet, const ClpRng& clpRng,
                                   CodingStructure& cs, const int vbCTUHeight, int vbPos)
{
  CHECK((vbCTUHeight & (vbCTUHeight - 1)) != 0, "vbCTUHeight must be a power of 2");

  const bool hasChroma = isChroma(compId);
  if (hasChroma)
  {
    CHECK( filtType != 0, "Chroma needs to have filtType == 0" );
  }

  const CPelBuf srcLuma = recSrc.get( compId );
  PelBuf dstLuma = recDst.get( compId );

  const ptrdiff_t srcStride = srcLuma.stride;
  const ptrdiff_t dstStride = dstLuma.stride;

  const int startHeight = blk.y;
  const int endHeight = blk.y + blk.height;
  const int startWidth = blk.x;
  const int endWidth = blk.x + blk.width;

  const Pel* src = srcLuma.buf;
  Pel* dst = dstLuma.buf + blkDst.y * dstStride;

  const Pel *pImgYPad0, *pImgYPad1, *pImgYPad2, *pImgYPad3, *pImgYPad4, *pImgYPad5, *pImgYPad6;
  const Pel *pImg0, *pImg1, *pImg2, *pImg3, *pImg4, *pImg5, *pImg6;

  const AlfCoeff* coef = filterSet;
  const Pel   *clip = fClipSet;

  const int shift  = COEFF_SCALE_BITS;
  const int offset = 1 << ( shift - 1 );

  int transposeIdx = 0;
  const int clsSizeY = 4;
  const int clsSizeX = 4;

  CHECK( startHeight % clsSizeY, "Wrong startHeight in filtering" );
  CHECK( startWidth % clsSizeX, "Wrong startWidth in filtering" );
  CHECK( ( endHeight - startHeight ) % clsSizeY, "Wrong endHeight in filtering" );
  CHECK( ( endWidth - startWidth ) % clsSizeX, "Wrong endWidth in filtering" );

  AlfClassifier *pClass = nullptr;

  const ptrdiff_t dstStride2 = dstStride * clsSizeY;
  const ptrdiff_t srcStride2 = srcStride * clsSizeY;

  std::array<int, MAX_NUM_ALF_LUMA_COEFF> filterCoeff;
  std::array<int, MAX_NUM_ALF_LUMA_COEFF> filterClipp;

  pImgYPad0 = src + startHeight * srcStride + startWidth;
  pImgYPad1 = pImgYPad0 + srcStride;
  pImgYPad2 = pImgYPad0 - srcStride;
  pImgYPad3 = pImgYPad1 + srcStride;
  pImgYPad4 = pImgYPad2 - srcStride;
  pImgYPad5 = pImgYPad3 + srcStride;
  pImgYPad6 = pImgYPad4 - srcStride;

  Pel* pRec0 = dst + blkDst.x;
  Pel* pRec1 = pRec0 + dstStride;

  for( int i = 0; i < endHeight - startHeight; i += clsSizeY )
  {
    if (!hasChroma)
    {
      pClass = classifier[blkDst.y + i] + blkDst.x;
    }

    for( int j = 0; j < endWidth - startWidth; j += clsSizeX )
    {
      if (!hasChroma)
      {
        AlfClassifier& cl = pClass[j];
        transposeIdx = cl.transposeIdx;
        coef = filterSet + cl.classIdx * MAX_NUM_ALF_LUMA_COEFF;
        clip = fClipSet + cl.classIdx * MAX_NUM_ALF_LUMA_COEFF;
      }

      if( filtType == ALF_FILTER_7 )
      {
        if( transposeIdx == 1 )
        {
          filterCoeff = { coef[9], coef[4], coef[10], coef[8], coef[1], coef[5], coef[11], coef[7], coef[3], coef[0], coef[2], coef[6], coef[12] };
          filterClipp = { clip[9], clip[4], clip[10], clip[8], clip[1], clip[5], clip[11], clip[7], clip[3], clip[0], clip[2], clip[6], clip[12] };
        }
        else if( transposeIdx == 2 )
        {
          filterCoeff = { coef[0], coef[3], coef[2], coef[1], coef[8], coef[7], coef[6], coef[5], coef[4], coef[9], coef[10], coef[11], coef[12] };
          filterClipp = { clip[0], clip[3], clip[2], clip[1], clip[8], clip[7], clip[6], clip[5], clip[4], clip[9], clip[10], clip[11], clip[12] };
        }
        else if( transposeIdx == 3 )
        {
          filterCoeff = { coef[9], coef[8], coef[10], coef[4], coef[3], coef[7], coef[11], coef[5], coef[1], coef[0], coef[2], coef[6], coef[12] };
          filterClipp = { clip[9], clip[8], clip[10], clip[4], clip[3], clip[7], clip[11], clip[5], clip[1], clip[0], clip[2], clip[6], clip[12] };
        }
        else
        {
          filterCoeff = { coef[0], coef[1], coef[2], coef[3], coef[4], coef[5], coef[6], coef[7], coef[8], coef[9], coef[10], coef[11], coef[12] };
          filterClipp = { clip[0], clip[1], clip[2], clip[3], clip[4], clip[5], clip[6], clip[7], clip[8], clip[9], clip[10], clip[11], clip[12] };
        }
      }
      else
      {
        if( transposeIdx == 1 )
        {
          filterCoeff = { coef[4], coef[1], coef[5], coef[3], coef[0], coef[2], coef[6] };
          filterClipp = { clip[4], clip[1], clip[5], clip[3], clip[0], clip[2], clip[6] };
        }
        else if( transposeIdx == 2 )
        {
          filterCoeff = { coef[0], coef[3], coef[2], coef[1], coef[4], coef[5], coef[6] };
          filterClipp = { clip[0], clip[3], clip[2], clip[1], clip[4], clip[5], clip[6] };
        }
        else if( transposeIdx == 3 )
        {
          filterCoeff = { coef[4], coef[3], coef[5], coef[1], coef[0], coef[2], coef[6] };
          filterClipp = { clip[4], clip[3], clip[5], clip[1], clip[0], clip[2], clip[6] };
        }
        else
        {
          filterCoeff = { coef[0], coef[1], coef[2], coef[3], coef[4], coef[5], coef[6] };
          filterClipp = { clip[0], clip[1], clip[2], clip[3], clip[4], clip[5], clip[6] };
        }
      }

      for( int ii = 0; ii < clsSizeY; ii++ )
      {
        pImg0 = pImgYPad0 + j + ii * srcStride;
        pImg1 = pImgYPad1 + j + ii * srcStride;
        pImg2 = pImgYPad2 + j + ii * srcStride;
        pImg3 = pImgYPad3 + j + ii * srcStride;
        pImg4 = pImgYPad4 + j + ii * srcStride;
        pImg5 = pImgYPad5 + j + ii * srcStride;
        pImg6 = pImgYPad6 + j + ii * srcStride;

        pRec1 = pRec0 + j + ii * dstStride;

        const int yVb = (blkDst.y + i + ii) & (vbCTUHeight - 1);
        if (yVb < vbPos && (yVb >= vbPos - (hasChroma ? 2 : 4)))   // above
        {
          pImg1 = (yVb == vbPos - 1) ? pImg0 : pImg1;
          pImg3 = (yVb >= vbPos - 2) ? pImg1 : pImg3;
          pImg5 = (yVb >= vbPos - 3) ? pImg3 : pImg5;

          pImg2 = (yVb == vbPos - 1) ? pImg0 : pImg2;
          pImg4 = (yVb >= vbPos - 2) ? pImg2 : pImg4;
          pImg6 = (yVb >= vbPos - 3) ? pImg4 : pImg6;
        }
        else if (yVb >= vbPos && (yVb <= vbPos + (hasChroma ? 1 : 3)))   // bottom
        {
          pImg2 = (yVb == vbPos) ? pImg0 : pImg2;
          pImg4 = (yVb <= vbPos + 1) ? pImg2 : pImg4;
          pImg6 = (yVb <= vbPos + 2) ? pImg4 : pImg6;

          pImg1 = (yVb == vbPos) ? pImg0 : pImg1;
          pImg3 = (yVb <= vbPos + 1) ? pImg1 : pImg3;
          pImg5 = (yVb <= vbPos + 2) ? pImg3 : pImg5;
        }
        bool isNearVBabove = yVb < vbPos && (yVb >= vbPos - 1);
        bool isNearVBbelow = yVb >= vbPos && (yVb <= vbPos);
        for( int jj = 0; jj < clsSizeX; jj++ )
        {
          int32_t   sum  = 0;
          const Pel curr = pImg0[+0];
          if( filtType == ALF_FILTER_7 )
          {
            sum += filterCoeff[0] * ( clipALF(filterClipp[0], curr, pImg5[+0], pImg6[+0]) );

            sum += filterCoeff[1] * ( clipALF(filterClipp[1], curr, pImg3[+1], pImg4[-1]) );
            sum += filterCoeff[2] * ( clipALF(filterClipp[2], curr, pImg3[+0], pImg4[+0]) );
            sum += filterCoeff[3] * ( clipALF(filterClipp[3], curr, pImg3[-1], pImg4[+1]) );

            sum += filterCoeff[4] * ( clipALF(filterClipp[4], curr, pImg1[+2], pImg2[-2]) );
            sum += filterCoeff[5] * ( clipALF(filterClipp[5], curr, pImg1[+1], pImg2[-1]) );
            sum += filterCoeff[6] * ( clipALF(filterClipp[6], curr, pImg1[+0], pImg2[+0]) );
            sum += filterCoeff[7] * ( clipALF(filterClipp[7], curr, pImg1[-1], pImg2[+1]) );
            sum += filterCoeff[8] * ( clipALF(filterClipp[8], curr, pImg1[-2], pImg2[+2]) );

            sum += filterCoeff[9] * ( clipALF(filterClipp[9], curr, pImg0[+3], pImg0[-3]) );
            sum += filterCoeff[10] * ( clipALF(filterClipp[10], curr, pImg0[+2], pImg0[-2]) );
            sum += filterCoeff[11] * ( clipALF(filterClipp[11], curr, pImg0[+1], pImg0[-1]) );
          }
          else
          {
            sum += filterCoeff[0] * ( clipALF(filterClipp[0], curr, pImg3[+0], pImg4[+0]) );

            sum += filterCoeff[1] * ( clipALF(filterClipp[1], curr, pImg1[+1], pImg2[-1]) );
            sum += filterCoeff[2] * ( clipALF(filterClipp[2], curr, pImg1[+0], pImg2[+0]) );
            sum += filterCoeff[3] * ( clipALF(filterClipp[3], curr, pImg1[-1], pImg2[+1]) );

            sum += filterCoeff[4] * ( clipALF(filterClipp[4], curr, pImg0[+2], pImg0[-2]) );
            sum += filterCoeff[5] * ( clipALF(filterClipp[5], curr, pImg0[+1], pImg0[-1]) );
          }
          if (!(isNearVBabove || isNearVBbelow))
          {
            sum = (sum + offset) >> shift;
          }
          else
          {
            sum = (sum + (1 << ((shift + 3) - 1))) >> (shift + 3);
          }
          sum += curr;
          pRec1[jj] = ClipPel( sum, clpRng );

          pImg0++;
          pImg1++;
          pImg2++;
          pImg3++;
          pImg4++;
          pImg5++;
          pImg6++;
        }
      }
    }

    pRec0 += dstStride2;
    pRec1 += dstStride2;

    pImgYPad0 += srcStride2;
    pImgYPad1 += srcStride2;
    pImgYPad2 += srcStride2;
    pImgYPad3 += srcStride2;
    pImgYPad4 += srcStride2;
    pImgYPad5 += srcStride2;
    pImgYPad6 += srcStride2;
  }
}

template<AlfFilterType filtTypeCcAlf>
void AdaptiveLoopFilter::filterBlkCcAlf(const PelBuf& dstBuf, const CPelUnitBuf& recSrc, const Area& blkDst,
                                        const Area& blkSrc, const ComponentID compId, const AlfCoeff* filterCoeff,
                                        const ClpRngs& clpRngs, CodingStructure& cs, int vbCTUHeight, int vbPos)
{
  CHECK(1 << floorLog2(vbCTUHeight) != vbCTUHeight, "Not a power of 2");

  CHECK(!isChroma(compId), "Must be chroma");

  const SPS*     sps           = cs.slice->getSPS();
  ChromaFormat nChromaFormat   = sps->getChromaFormatIdc();
  const int clsSizeY           = 4;
  const int clsSizeX           = 4;
  const int      startHeight        = blkDst.y;
  const int      endHeight          = blkDst.y + blkDst.height;
  const int      startWidth         = blkDst.x;
  const int      endWidth           = blkDst.x + blkDst.width;

  const int scaleX = getComponentScaleX(compId, nChromaFormat);
  const int scaleY = getComponentScaleY(compId, nChromaFormat);

  CHECK( startHeight % clsSizeY, "Wrong startHeight in filtering" );
  CHECK( startWidth % clsSizeX, "Wrong startWidth in filtering" );
  CHECK( ( endHeight - startHeight ) % clsSizeY, "Wrong endHeight in filtering" );
  CHECK( ( endWidth - startWidth ) % clsSizeX, "Wrong endWidth in filtering" );

  CPelBuf srcBuf = recSrc.get(COMPONENT_Y);

  const ptrdiff_t lumaStride   = srcBuf.stride;
  const ptrdiff_t chromaStride = dstBuf.stride;

  const Pel  *lumaPtr      = srcBuf.buf + blkSrc.y * lumaStride + blkSrc.x;
  Pel *       chromaPtr    = dstBuf.buf + blkDst.y * chromaStride + blkDst.x;

  for( int i = 0; i < endHeight - startHeight; i += clsSizeY )
  {
    for( int j = 0; j < endWidth - startWidth; j += clsSizeX )
    {
      for( int ii = 0; ii < clsSizeY; ii++ )
      {
        int row       = ii;
        int col       = j;
        Pel *srcSelf  = chromaPtr + col + row * chromaStride;

        ptrdiff_t offset1 = lumaStride;
        ptrdiff_t offset2 = -lumaStride;
        ptrdiff_t offset3 = 2 * lumaStride;
        row <<= scaleY;
        col <<= scaleX;
        const Pel *srcCross = lumaPtr + col + row * lumaStride;

        int pos = ((startHeight + i + ii) << scaleY) & (vbCTUHeight - 1);
        if (scaleY == 0 && (pos == vbPos || pos == vbPos + 1))
        {
          continue;
        }
        if (pos == (vbPos - 2) || pos == (vbPos + 1))
        {
          offset3 = offset1;
        }
        else if (pos == (vbPos - 1) || pos == vbPos)
        {
          offset1 = 0;
          offset2 = 0;
          offset3 = 0;
        }

        for (int jj = 0; jj < clsSizeX; jj++)
        {
          const int jj2     = (jj << scaleX);
          const int offset0 = 0;

          int sum = 0;
          const Pel currSrcCross = srcCross[offset0 + jj2];
          sum += filterCoeff[0] * (srcCross[offset2 + jj2    ] - currSrcCross);
          sum += filterCoeff[1] * (srcCross[offset0 + jj2 - 1] - currSrcCross);
          sum += filterCoeff[2] * (srcCross[offset0 + jj2 + 1] - currSrcCross);
          sum += filterCoeff[3] * (srcCross[offset1 + jj2 - 1] - currSrcCross);
          sum += filterCoeff[4] * (srcCross[offset1 + jj2    ] - currSrcCross);
          sum += filterCoeff[5] * (srcCross[offset1 + jj2 + 1] - currSrcCross);
          sum += filterCoeff[6] * (srcCross[offset3 + jj2    ] - currSrcCross);

          sum = (sum + (1 << COEFF_SCALE_BITS >> 1)) >> COEFF_SCALE_BITS;

          const int offset = 1 << clpRngs.comp[compId].bd >> 1;
          sum = ClipPel(sum + offset, clpRngs.comp[compId]) - offset;
          sum += srcSelf[jj];
          srcSelf[jj] = ClipPel(sum, clpRngs.comp[compId]);
        }
      }
    }

    chromaPtr += chromaStride * clsSizeY;

    lumaPtr += lumaStride * clsSizeY << getComponentScaleY(compId, nChromaFormat);
  }
}
