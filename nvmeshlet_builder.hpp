/*
 * Copyright (c) 2017-2022, NVIDIA CORPORATION.  All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * SPDX-FileCopyrightText: Copyright (c) 2017-2022 NVIDIA CORPORATION
 * SPDX-License-Identifier: Apache-2.0
 */


#ifndef _NV_MESHLET_BUILDER_H__
#define _NV_MESHLET_BUILDER_H__

#include <NvFoundation.h>
#include <algorithm>
#include <cstdint>
#include <vector>
#include <stdio.h>

#ifdef _MSC_VER
#include <intrin.h>
#endif

namespace NVMeshlet {
// Each Meshlet can have a varying count of its maximum number
// of vertices and primitives. We hardcode a few absolute maxima
// to accelerate some functions and allow usage of
// smaller datastructures.

// The builder, however, is configurable to use smaller maxima,
// which is recommended.

// The limits below are hard limits due to the encoding chosen for the
// meshlet descriptor. Actual hw-limits can be higher, but typically
// make things slower due to large on-chip allocation.

#define NVMESHLET_ASSERT_ON_DEGENERATES 1

static const int MAX_VERTEX_COUNT_LIMIT    = 256;
static const int MAX_PRIMITIVE_COUNT_LIMIT = 256;

// must not change
typedef uint8_t PrimitiveIndexType;  // must store [0,MAX_VERTEX_COUNT_LIMIT-1]

inline uint32_t alignedSize(uint32_t v, uint32_t align)
{
  return (v + align - 1) & (~(align - 1));
}

// opaque type, all builders will specialize this, but fit within
struct MeshletDesc
{
  uint32_t fieldX;
  uint32_t fieldY;
  uint32_t fieldZ;
  uint32_t fieldW;
};

struct MeshletBbox
{
  float bboxMin[3]{};
  float bboxMax[3]{};

  MeshletBbox()
  {
    bboxMin[0] = FLT_MAX;
    bboxMin[1] = FLT_MAX;
    bboxMin[2] = FLT_MAX;
    bboxMax[0] = -FLT_MAX;
    bboxMax[1] = -FLT_MAX;
    bboxMax[2] = -FLT_MAX;
  }
};

enum StatusCode
{
  STATUS_NO_ERROR,
  STATUS_PRIM_OUT_OF_BOUNDS,
  STATUS_VERTEX_OUT_OF_BOUNDS,
  STATUS_MISMATCH_INDICES,
};

//////////////////////////////////////////////////////////////////////////
//

struct Stats
{
  size_t meshletsTotal = 0;
  // slightly more due to task-shader alignment
  size_t meshletsStored = 0;

  // number of meshlets that can be backface cluster culled at all
  // due to similar normals
  size_t backfaceTotal = 0;

  size_t primIndices = 0;
  size_t primTotal   = 0;

  size_t vertexIndices = 0;
  size_t vertexTotal   = 0;

  size_t posBitTotal = 0;

  // used when we sum multiple stats into a single to
  // compute averages of the averages/variances below.

  size_t appended = 0;

  double primloadAvg   = 0.f;
  double primloadVar   = 0.f;
  double vertexloadAvg = 0.f;
  double vertexloadVar = 0.f;

  void append(const Stats& other)
  {
    meshletsTotal += other.meshletsTotal;
    meshletsStored += other.meshletsStored;
    backfaceTotal += other.backfaceTotal;

    primIndices += other.primIndices;
    vertexIndices += other.vertexIndices;
    vertexTotal += other.vertexTotal;
    primTotal += other.primTotal;

    appended += other.appended;
    primloadAvg += other.primloadAvg;
    primloadVar += other.primloadVar;
    vertexloadAvg += other.vertexloadAvg;
    vertexloadVar += other.vertexloadVar;
  }

  void fprint(FILE* log) const
  {
    if(!appended || !meshletsTotal)
      return;

    double fprimloadAvg   = primloadAvg / double(appended);
    double fvertexloadAvg = vertexloadAvg / double(appended);

    double statsNum    = double(meshletsTotal);
    double backfaceAvg = double(backfaceTotal) / statsNum;

    double primWaste    = double(primIndices) / double(primTotal * 3) - 1.0;
    double vertexWaste  = double(vertexIndices) / double(vertexTotal) - 1.0;
    double meshletWaste = double(meshletsStored) / double(meshletsTotal) - 1.0;

    fprintf(log, "meshlets; %7zd; prim; %9zd; %.2f; vertex; %9zd; %.2f; backface; %.2f; waste; v; %.2f; p; %.2f; m; %.2f;\n",
            meshletsTotal, primTotal, fprimloadAvg, vertexTotal, fvertexloadAvg, backfaceAvg, vertexWaste, primWaste, meshletWaste);
  }
};

//////////////////////////////////////////////////////////////////////////
// simple vector class to reduce dependencies

struct vec
{
  float x{};
  float y{};
  float z{};

  vec() = default;
  vec(float v)
      : x(v)
      , y(v)
      , z(v)
  {
  }
  vec(float _x, float _y, float _z)
      : x(_x)
      , y(_y)
      , z(_z)
  {
  }
  explicit vec(const float* v)
      : x(v[0])
      , y(v[1])
      , z(v[2])
  {
  }
};

inline vec vec_min(const vec& a, const vec& b)
{
  return {std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z)};
}
inline vec vec_max(const vec& a, const vec& b)
{
  return {std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z)};
}
inline vec operator+(const vec& a, const vec& b)
{
  return {a.x + b.x, a.y + b.y, a.z + b.z};
}
inline vec operator-(const vec& a, const vec& b)
{
  return {a.x - b.x, a.y - b.y, a.z - b.z};
}
inline vec operator/(const vec& a, const vec& b)
{
  return {a.x / b.x, a.y / b.y, a.z / b.z};
}
inline vec operator*(const vec& a, const vec& b)
{
  return {a.x * b.x, a.y * b.y, a.z * b.z};
}
inline vec operator*(const vec& a, const float b)
{
  return {a.x * b, a.y * b, a.z * b};
}
inline vec vec_floor(const vec& a)
{
  return {floorf(a.x), floorf(a.y), floorf(a.z)};
}
inline vec vec_clamp(const vec& a, const float lowerV, const float upperV)
{
  return {std::max(std::min(upperV, a.x), lowerV), std::max(std::min(upperV, a.y), lowerV), std::max(std::min(upperV, a.z), lowerV)};
}
inline vec vec_cross(const vec& a, const vec& b)
{
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline float vec_dot(const vec& a, const vec& b)
{
  return a.x * b.x + a.y * b.y + a.z * b.z;
}
inline float vec_length(const vec& a)
{
  return sqrtf(vec_dot(a, a));
}
inline vec vec_normalize(const vec& a)
{
  float len = vec_length(a);
  return a * 1.0f / len;
}

// all oct functions derived from "A Survey of Efficient Representations for Independent Unit Vectors"
// http://jcgt.org/published/0003/02/01/paper.pdf
// Returns +/- 1
inline vec oct_signNotZero(vec v)
{
  // leaves z as is
  return {(v.x >= 0.0f) ? +1.0f : -1.0f, (v.y >= 0.0f) ? +1.0f : -1.0f, 1.0f};
}

// Assume normalized input. Output is on [-1, 1] for each component.
inline vec float32x3_to_oct(vec v)
{
  // Project the sphere onto the octahedron, and then onto the xy plane
  vec p = vec(v.x, v.y, 0) * (1.0f / (fabsf(v.x) + fabsf(v.y) + fabsf(v.z)));
  // Reflect the folds of the lower hemisphere over the diagonals
  return (v.z <= 0.0f) ? vec(1.0f - fabsf(p.y), 1.0f - fabsf(p.x), 0.0f) * oct_signNotZero(p) : p;
}

inline vec oct_to_float32x3(vec e)
{
  vec v = vec(e.x, e.y, 1.0f - fabsf(e.x) - fabsf(e.y));
  if(v.z < 0.0f)
  {
    v = vec(1.0f - fabs(v.y), 1.0f - fabs(v.x), v.z) * oct_signNotZero(v);
  }
  return vec_normalize(v);
}

inline vec float32x3_to_octn_precise(vec v, const int n)
{
  vec s = float32x3_to_oct(v);  // Remap to the square
                                // Each snorm's max value interpreted as an integer,
                                // e.g., 127.0 for snorm8
  float M = float(1 << ((n / 2) - 1)) - 1.0f;
  // Remap components to snorm(n/2) precision...with floor instead
  // of round (see equation 1)
  s                        = vec_floor(vec_clamp(s, -1.0f, +1.0f) * M) * (1.0f / M);
  vec   bestRepresentation = s;
  float highestCosine      = vec_dot(oct_to_float32x3(s), v);
  // Test all combinations of floor and ceil and keep the best.
  // Note that at +/- 1, this will exit the square... but that
  // will be a worse encoding and never win.
  for(int i = 0; i <= 1; ++i)
    for(int j = 0; j <= 1; ++j)
      // This branch will be evaluated at compile time
      if((i != 0) || (j != 0))
      {
        // Offset the bit pattern (which is stored in floating
        // point!) to effectively change the rounding mode
        // (when i or j is 0: floor, when it is one: ceiling)
        vec   candidate = vec(static_cast<float>(i), static_cast<float>(j), 0) * (1 / M) + s;
        float cosine    = vec_dot(oct_to_float32x3(candidate), v);
        if(cosine > highestCosine)
        {
          bestRepresentation = candidate;
          highestCosine      = cosine;
        }
      }
  return bestRepresentation;
}

//////////////////////////////////////////////////////////////////////////

// quantized vector
struct qvec
{
  uint32_t bits[3]{};

  qvec()
  {
    bits[0] = 0;
    bits[1] = 0;
    bits[2] = 0;
  }
  explicit qvec(uint32_t raw)
  {
    bits[0] = raw;
    bits[1] = raw;
    bits[2] = raw;
  }
  qvec(uint32_t x, uint32_t y, uint32_t z)
  {
    bits[0] = x;
    bits[1] = y;
    bits[2] = z;
  }
  qvec(const vec& v, const vec& bboxMin, const vec& bboxExtent, float quantizedMul)
  {
    vec nrm = (v - bboxMin) / bboxExtent;
    bits[0] = uint32_t(round(nrm.x * quantizedMul));
    bits[1] = uint32_t(round(nrm.y * quantizedMul));
    bits[2] = uint32_t(round(nrm.z * quantizedMul));
  }
};

inline qvec operator-(const qvec& a, const qvec& b)
{
  return {a.bits[0] - b.bits[0], a.bits[1] - b.bits[1], a.bits[2] - b.bits[2]};
}

inline qvec qvec_min(const qvec& a, const qvec& b)
{
  return {std::min(a.bits[0], b.bits[0]), std::min(a.bits[1], b.bits[1]), std::min(a.bits[2], b.bits[2])};
}

inline qvec qvec_max(const qvec& a, const qvec& b)
{
  return {std::max(a.bits[0], b.bits[0]), std::max(a.bits[1], b.bits[1]), std::max(a.bits[2], b.bits[2])};
}

//////////////////////////////////////////////////////////////////////////

inline uint32_t pack(uint32_t value, int width, int offset)
{
  return (uint32_t)((value & ((1 << width) - 1)) << offset);
}
inline uint32_t unpack(uint32_t value, int width, int offset)
{
  return (uint32_t)((value >> offset) & ((1 << width) - 1));
}

inline void setBitField(uint32_t arraySize, uint32_t* bits, uint32_t width, uint32_t offset, uint32_t value)
{
  uint32_t idx     = offset / 32u;
  uint32_t shiftLo = offset % 32;

  assert(idx < arraySize);

  bool onlyLo = (shiftLo + width) <= 32;

  uint32_t sizeLo = onlyLo ? width : 32 - shiftLo;
  uint32_t sizeHi = onlyLo ? 0 : (shiftLo + width - 32);

  uint32_t shiftHi = sizeLo;

  uint32_t retLo = (value << shiftLo);
  uint32_t retHi = (value >> shiftHi);

  bits[idx] |= retLo;
  if(idx + 1 < arraySize)
  {
    bits[idx + 1] |= retHi;
  }
}

inline uint32_t getBitField(uint32_t arraySize, const uint32_t* bits, uint32_t width, uint32_t offset)
{
  uint32_t idx = offset / 32;

  // assumes out-of-bounds access is not fatal
  uint32_t rawLo = bits[idx];
  uint32_t rawHi = idx + 1 < arraySize ? bits[idx + 1] : 0;

  uint32_t shiftLo = offset % 32;

  bool onlyLo = (shiftLo + width) <= 32;

  uint32_t sizeLo = onlyLo ? width : 32 - shiftLo;
  uint32_t sizeHi = onlyLo ? 0 : (shiftLo + width - 32);

  uint32_t shiftHi = sizeLo;

  uint32_t maskLo = (width == 32) ? ~0 : ((1 << sizeLo) - 1);
  uint32_t maskU  = (1 << sizeHi) - 1;

  uint32_t retLo = ((rawLo >> shiftLo) & maskLo);
  uint32_t retHi = ((rawHi & maskU) << shiftHi);

  return retLo | retHi;
}

#if defined(_MSC_VER)

#pragma intrinsic(_BitScanReverse)

inline uint32_t findMSB(uint32_t value)
{
  unsigned long idx = 0;
  _BitScanReverse(&idx, value);
  return idx;
}
#else
inline uint32_t findMSB(uint32_t value)
{
  uint32_t idx = __builtin_clz(value);
  return idx;
}
#endif

//////////////////////////////////////////////////////////////////////////

struct PrimitiveCache
{
  //  Utility class to generate the meshlets from triangle indices.
  //  It finds the unique vertex set used by a series of primitives.
  //  The cache is exhausted if either of the maximums is hit.
  //  The effective limits used with the cache must be < MAX.

  PrimitiveIndexType primitives[MAX_PRIMITIVE_COUNT_LIMIT][3]{};
  uint32_t           vertices[MAX_VERTEX_COUNT_LIMIT]{};
  uint32_t           numPrims{};
  uint32_t           numVertices{};
  uint32_t           numVertexDeltaBits{};
  uint32_t           numVertexAllBits{};

  uint32_t maxVertexSize{};
  uint32_t maxPrimitiveSize{};
  uint32_t primitiveBits = 1;
  uint32_t maxBlockBits  = ~0;

  [[nodiscard]] bool empty() const { return numVertices == 0; }

  void reset()
  {
    numPrims           = 0;
    numVertices        = 0;
    numVertexDeltaBits = 0;
    numVertexAllBits   = 0;
    // reset
    memset(vertices, static_cast<int>(0xFFFFFFFF), sizeof(vertices));
  }

  [[nodiscard]] bool fitsBlock() const
  {
    uint32_t primBits = (numPrims - 1) * 3 * primitiveBits;
    uint32_t vertBits = (numVertices - 1) * numVertexDeltaBits;
    bool     state    = (primBits + vertBits) <= maxBlockBits;
    return state;
  }

  [[nodiscard]] bool cannotInsert(uint32_t idxA, uint32_t idxB, uint32_t idxC) const
  {
    const uint32_t indices[3] = {idxA, idxB, idxC};
    // skip degenerate
    if(indices[0] == indices[1] || indices[0] == indices[2] || indices[1] == indices[2])
    {
#if NVMESHLET_ASSERT_ON_DEGENERATES
      //assert(0 && "degenerate triangle");
#endif
      return false;
    }

    uint32_t found = 0;
    for(uint32_t v = 0; v < numVertices; v++)
    {
      for(unsigned int idx : indices)
      {
        if(vertices[v] == idx)
        {
          found++;
        }
      }
    }
    // out of bounds
    return (numVertices + 3 - found) > maxVertexSize || (numPrims + 1) > maxPrimitiveSize;
  }

  [[nodiscard]] bool cannotInsertBlock(uint32_t idxA, uint32_t idxB, uint32_t idxC) const
  {
    const uint32_t indices[3] = {idxA, idxB, idxC};
    // skip degenerate
    if(indices[0] == indices[1] || indices[0] == indices[2] || indices[1] == indices[2])
    {
      return false;
    }

    uint32_t found = 0;
    for(uint32_t v = 0; v < numVertices; v++)
    {
      for(unsigned int idx : indices)
      {
        if(vertices[v] == idx)
        {
          found++;
        }
      }
    }
    // ensure one bit is set in deltas for findMSB returning 0
    uint32_t firstVertex = numVertices ? vertices[0] : indices[0];
    uint32_t cmpBits     = std::max(findMSB((firstVertex ^ indices[0]) | 1),
                                std::max(findMSB((firstVertex ^ indices[1]) | 1), findMSB((firstVertex ^ indices[2]) | 1)))
                       + 1;

    uint32_t deltaBits = std::max(cmpBits, numVertexDeltaBits);

    uint32_t newVertices = numVertices + 3 - found;
    uint32_t newPrims    = numPrims + 1;

    uint32_t newBits;

    {
      uint32_t newVertBits = (newVertices - 1) * deltaBits;
      uint32_t newPrimBits = (newPrims - 1) * 3 * primitiveBits;
      newBits              = newVertBits + newPrimBits;
    }

    // out of bounds
    return (newPrims > maxPrimitiveSize) || (newVertices > maxVertexSize) || (newBits > maxBlockBits);
  }

  void insert(uint32_t idxA, uint32_t idxB, uint32_t idxC)
  {
    const uint32_t indices[3] = {idxA, idxB, idxC};
    uint32_t       tri[3];

    // skip degenerate
    if(indices[0] == indices[1] || indices[0] == indices[2] || indices[1] == indices[2])
    {
      return;
    }

    for(int i = 0; i < 3; i++)
    {
      uint32_t idx   = indices[i];
      bool     found = false;
      for(uint32_t v = 0; v < numVertices; v++)
      {
        if(idx == vertices[v])
        {
          tri[i] = v;
          found  = true;
          break;
        }
      }
      if(!found)
      {
        vertices[numVertices] = idx;
        tri[i]                = numVertices;

        if(numVertices)
        {
          numVertexDeltaBits = std::max(findMSB((idx ^ vertices[0]) | 1) + 1, numVertexDeltaBits);
        }
        numVertexAllBits = std::max(numVertexAllBits, findMSB(idx) + 1);

        numVertices++;
      }
    }

    primitives[numPrims][0] = tri[0];
    primitives[numPrims][1] = tri[1];
    primitives[numPrims][2] = tri[2];
    numPrims++;

    assert(fitsBlock());
  }
};

}  // namespace NVMeshlet

#endif
