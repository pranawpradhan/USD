//
// Copyright 2016 Pixar
//
// Licensed under the Apache License, Version 2.0 (the "Apache License")
// with the following modification; you may not use this file except in
// compliance with the Apache License and the following modification to it:
// Section 6. Trademarks. is deleted and replaced with:
//
// 6. Trademarks. This License does not grant permission to use the trade
//    names, trademarks, service marks, or product names of the Licensor
//    and its affiliates, except as required to comply with Section 4(c) of
//    the License and to reproduce the content of the NOTICE file.
//
// You may obtain a copy of the Apache License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the Apache License with the above modification is
// distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied. See the Apache License for the specific
// language governing permissions and limitations under the Apache License.
//
#ifndef HDST_QUADRANGULATE_H
#define HDST_QUADRANGULATE_H

#include "pxr/pxr.h"
#include "pxr/imaging/hd/version.h"
#include "pxr/imaging/hd/bufferSource.h"
#include "pxr/imaging/hd/computation.h"
#include "pxr/imaging/hd/perfLog.h"
#include "pxr/imaging/hd/tokens.h"

#include "pxr/base/vt/array.h"
#include "pxr/base/tf/token.h"
#include "pxr/usd/sdf/path.h"

#include <boost/shared_ptr.hpp>

PXR_NAMESPACE_OPEN_SCOPE


typedef boost::shared_ptr<class HdSt_QuadInfoBuilderComputation>
                                       HdSt_QuadInfoBuilderComputationSharedPtr;

class HdSt_MeshTopology;

/*
  computation classes for quadrangulation.

  Dependencies

   *CPU quadrangulation

    (buffersource)
     QuadIndexBuilderComputation  (quad indices)
      |
      +--QuadrangulateComputation (primvar quadrangulation)

     note: QuadrangulateComputation also copies the original primvars.
           no need to transfer the original primvars to GPU separately.

       +--------------------+
   CPU |  original primvars |
       +--------------------+
                |
                v
       +--------------------+-------------------------+
   CPU |  original primvars | quadrangulated primvars |
       +--------------------+-------------------------+
       <---------------------------------------------->
                    filled by computation
                          |
                          v
                         GPU

   *GPU quadrangulation

    (buffersource)
     QuadIndexBuilderComputation  (quad indices)
      |
      +--QuadrangulateTableComputation  (quadrangulate table on GPU)

    (computation)
     QuadrangulateComputationGPU  (primvar quadrangulation)

     note: QuadrangulateComputationGPU just fills quadrangulated primvars.
           the original primvars has to be transfered before the computation.

       +--------------------+
   CPU |  original primvars |
       +--------------------+
                |
                v
               GPU
                |
                v
       +--------------------+-------------------------+
   GPU |  original primvars | quadrangulated primvars |
       +--------------------+-------------------------+
                            <------------------------->
                               filled by computation

 */
    // quadrangulation info
    //
    // v0           v2
    // +-----e2----+
    //  \    |    /
    //   \ __c__ /
    //   e0     e1
    //     \   /
    //      \ /
    //       + v1
    //
    //
    //  original points       additional center and edge points
    // +------------ ... ----+--------------------------------+
    // | p0 p1 p2         pn | e0 e1 e2 c0, e3 e4 e5 c1 ...   |
    // +------------ ... ----+--------------------------------+
    //                       ^
    //                   pointsOffset
    //                       <----- numAdditionalPoints  ---->

struct HdSt_QuadInfo {
    HdSt_QuadInfo() : pointsOffset(0), numAdditionalPoints(0), maxNumVert(0) { }

    /// Returns true if the mesh is all-quads.
    bool IsAllQuads() const { return numAdditionalPoints == 0; }

    int pointsOffset;
    int numAdditionalPoints;
    int maxNumVert;
    std::vector<int> numVerts;  // num vertices of non-quads
    std::vector<int> verts;     // vertex indices of non-quads
};

/*
    computation dependencies

    Topology ---> QuadInfo --->  QuadIndices
                           --->  QuadrangulateComputation(CPU)
                           --->  QuadrangulateTable --->
                           ----------------------------> QuadrangulateComputationGPU
 */

/// \class HdSt_QuadInfoBuilderComputation
///
/// Quad info computation.
///
class HdSt_QuadInfoBuilderComputation : public HdNullBufferSource {
public:
    HdSt_QuadInfoBuilderComputation(HdSt_MeshTopology *topology, SdfPath const &id);
    virtual bool Resolve();

protected:
    virtual bool _CheckValid() const;

private:
    SdfPath const _id;
    HdSt_MeshTopology *_topology;
};

/// \class HdSt_QuadIndexBuilderComputation
///
/// Quad indices computation CPU.
///
class HdSt_QuadIndexBuilderComputation : public HdComputedBufferSource {
public:
    HdSt_QuadIndexBuilderComputation(
        HdSt_MeshTopology *topology,
        HdSt_QuadInfoBuilderComputationSharedPtr const &quadInfoBuilder,
        SdfPath const &id);
    virtual void AddBufferSpecs(HdBufferSpecVector *specs) const;
    virtual bool Resolve();

    virtual bool HasChainedBuffer() const;
    virtual HdBufferSourceSharedPtr GetChainedBuffer() const;

protected:
    virtual bool _CheckValid() const;

private:
    SdfPath const _id;
    HdSt_MeshTopology *_topology;
    HdSt_QuadInfoBuilderComputationSharedPtr _quadInfoBuilder;
    HdBufferSourceSharedPtr _primitiveParam;
};

/// \class HdSt_QuadrangulateTableComputation
///
/// Quadrangulate table computation (for GPU quadrangulation).
///
class HdSt_QuadrangulateTableComputation : public HdComputedBufferSource {
public:
    HdSt_QuadrangulateTableComputation(
        HdSt_MeshTopology *topology,
        HdBufferSourceSharedPtr const &quadInfoBuilder);
    virtual void AddBufferSpecs(HdBufferSpecVector *specs) const;
    virtual bool Resolve();

protected:
    virtual bool _CheckValid() const;

private:
    SdfPath const _id;
    HdSt_MeshTopology *_topology;
    HdBufferSourceSharedPtr _quadInfoBuilder;
};

/// \class HdSt_QuadrangulateComputation
///
/// CPU quadrangulation.
///
class HdSt_QuadrangulateComputation : public HdComputedBufferSource {
public:
    HdSt_QuadrangulateComputation(HdSt_MeshTopology *topology,
                                HdBufferSourceSharedPtr const &source,
                                HdBufferSourceSharedPtr const &quadInfoBuilder,
                                SdfPath const &id);
    virtual void AddBufferSpecs(HdBufferSpecVector *specs) const;
    virtual bool Resolve();

    // overrides GetGLComponentDataType to return source spec
    // since Hd_SmoothNormals::AddBufferSpecs() uses source datatype,
    // which happens before calling _SetResult().
    virtual int GetGLComponentDataType() const;

protected:
    virtual bool _CheckValid() const;

private:
    SdfPath const _id;
    HdSt_MeshTopology *_topology;
    HdBufferSourceSharedPtr _source;
    HdBufferSourceSharedPtr _quadInfoBuilder;
};

/// \class HdSt_QuadrangulateFaceVaryingComputation
///
/// CPU face-varying quadrangulation.
///
class HdSt_QuadrangulateFaceVaryingComputation : public HdComputedBufferSource {
public:
    HdSt_QuadrangulateFaceVaryingComputation(HdSt_MeshTopology *topolgoy,
                                           HdBufferSourceSharedPtr const &source,
                                           SdfPath const &id);

    virtual void AddBufferSpecs(HdBufferSpecVector *specs) const;
    virtual bool Resolve();

protected:
    virtual bool _CheckValid() const;

private:
    SdfPath const _id;
    HdSt_MeshTopology *_topology;
    HdBufferSourceSharedPtr _source;
};

/// \class HdSt_QuadrangulateComputationGPU
///
/// GPU quadrangulation.
///
class HdSt_QuadrangulateComputationGPU : public HdComputation {
public:
    /// This computaion doesn't generate buffer source (i.e. 2nd phase)
    HdSt_QuadrangulateComputationGPU(HdSt_MeshTopology *topology,
                                   TfToken const &sourceName,
                                   GLenum dataType,
                                   SdfPath const &id);
    virtual void Execute(HdBufferArrayRangeSharedPtr const &range);
    virtual void AddBufferSpecs(HdBufferSpecVector *specs) const;
    virtual int GetNumOutputElements() const;
private:
    SdfPath const _id;
    HdSt_MeshTopology *_topology;
    TfToken _name;
    GLenum _dataType;
};

// primitiveParam : quads to faces mapping buffer
//
// In order to access per-face signals (face color, face selection etc)
// in glsl shader, we need a mapping from primitiveID (triangulated
// or quadrangulated, or can be an adaptively refined patch) to authored
// face index domain.
//
/*
               +--------+-------+
              /|        |    |   \
             / |        |  2 | 2 /\
            /  |        |     \ /  \
           / 0 |    1   |------+  2 +
          /\  /|        |     / \  /
         /  \/ |        |  2 | 2 \/
        / 0 | 0|        |    |   /
       +-------+--------+-------+
*/
// We store this mapping buffer alongside topology index buffers, so
// that same aggregation locators can be used for such an additional
// buffer as well. This change transforms index buffer from int array
// to int[3] array or int[4] array at first. Thanks to the heterogenius
// non-interleaved buffer aggregation ability in hd, we'll get this kind
// of buffer layout:
//
// ----+-----------+-----------+------
// ... |i0 i1 i2 i3|i4 i5 i6 i7| ...    index buffer (for quads)
// ----+-----------+-----------+------
// ... |     m0    |     m1    | ...    primitive param buffer
// ----+-----------+-----------+------
//


PXR_NAMESPACE_CLOSE_SCOPE

#endif  // HDST_QUADRANGULATE_H
