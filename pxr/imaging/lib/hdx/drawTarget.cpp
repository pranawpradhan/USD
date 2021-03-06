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
#include "pxr/imaging/hdx/drawTarget.h"
#include "pxr/imaging/hdx/drawTargetAttachmentDescArray.h"
#include "pxr/imaging/hdx/drawTargetTextureResource.h"
#include "pxr/imaging/hdx/camera.h"

#include "pxr/imaging/hd/conversions.h"
#include "pxr/imaging/hd/perfLog.h"
#include "pxr/imaging/hd/resourceRegistry.h"
#include "pxr/imaging/hd/sceneDelegate.h"
#include "pxr/imaging/hd/sprim.h"

#include "pxr/imaging/glf/glContext.h"

#include "pxr/base/tf/stl.h"

PXR_NAMESPACE_OPEN_SCOPE


static const std::string DEPTH_ATTACHMENT_NAME = "depth";

TF_DEFINE_PUBLIC_TOKENS(HdxDrawTargetTokens, HDX_DRAW_TARGET_TOKENS);

HdxDrawTarget::HdxDrawTarget(SdfPath const &id)
    : HdSprim(id)
    , _version(1) // Clients tacking start at 0.
    , _enabled(true)
    , _cameraId()
    , _resolution(512, 512)
    , _collections()
    , _renderPassState()
    , _drawTargetContext()
    , _drawTarget()

{
}

HdxDrawTarget::~HdxDrawTarget()
{
}

/*virtual*/
void
HdxDrawTarget::Sync(HdSceneDelegate *sceneDelegate,
                    HdRenderParam   *renderParam,
                    HdDirtyBits     *dirtyBits)
{
    HD_TRACE_FUNCTION();
    HF_MALLOC_TAG_FUNCTION();

    TF_UNUSED(renderParam);

    SdfPath const &id = GetID();
    if (!TF_VERIFY(sceneDelegate != nullptr)) {
        return;
    }

    HdDirtyBits bits = *dirtyBits;

    if (bits & DirtyDTEnable) {
        VtValue vtValue =  sceneDelegate->Get(id, HdxDrawTargetTokens->enable);

        // Optional attribute.
        _enabled = vtValue.GetWithDefault<bool>(true);
    }

    if (bits & DirtyDTCamera) {
        VtValue vtValue =  sceneDelegate->Get(id, HdxDrawTargetTokens->camera);
        _cameraId = vtValue.Get<SdfPath>();
        _renderPassState.SetCamera(_cameraId);
    }

    if (bits & DirtyDTResolution) {
        VtValue vtValue =
                        sceneDelegate->Get(id, HdxDrawTargetTokens->resolution);

        _resolution = vtValue.Get<GfVec2i>();

        // No point in Resizing the textures if new ones are going to
        // be created (see _SetAttachments())
        if (_drawTarget && ((bits & DirtyDTAttachment) == Clean)) {
            _ResizeDrawTarget();
        }
    }

    if (bits & DirtyDTAttachment) {
        // Depends on resolution being set correctly.
        VtValue vtValue =
                       sceneDelegate->Get(id, HdxDrawTargetTokens->attachments);


        const HdxDrawTargetAttachmentDescArray &attachments =
            vtValue.GetWithDefault<HdxDrawTargetAttachmentDescArray>(
                    HdxDrawTargetAttachmentDescArray());

        _SetAttachments(sceneDelegate, attachments);
    }


    if (bits & DirtyDTDepthClearValue) {
        VtValue vtValue =
                   sceneDelegate->Get(id, HdxDrawTargetTokens->depthClearValue);

        float depthClearValue = vtValue.GetWithDefault<float>(1.0f);

        _renderPassState.SetDepthClearValue(depthClearValue);
    }

    if (bits & DirtyDTCollection) {
        VtValue vtValue =
                        sceneDelegate->Get(id, HdxDrawTargetTokens->collection);

        const HdRprimCollectionVector &collections =
                vtValue.GetWithDefault<HdRprimCollectionVector>(
                                                     HdRprimCollectionVector());

        _collections = collections;
        size_t newColSize     = collections.size();
        for (size_t colNum = 0; colNum < newColSize; ++colNum) {
            TfToken const &currentName = _collections[colNum].GetName();

            HdChangeTracker& changeTracker =
                             sceneDelegate->GetRenderIndex().GetChangeTracker();

            changeTracker.MarkCollectionDirty(currentName);
        }

        if (newColSize > 0)
        {
            // XXX:  Draw Targets currently only support a single collection right
            // now as each collect requires it's own render pass and then
            // it becomes a complex matrix of values as we have race needing to
            // know the number of attachments and number of render passes to
            // handle clear color and keeping that all in sync
            if (_collections.size() != 1) {
                TF_CODING_ERROR("Draw targets currently supports only a "
                                "single collection");
            }

            _renderPassState.SetRprimCollection(_collections[0]);
        }
    }

    *dirtyBits = Clean;
}

// virtual
VtValue
HdxDrawTarget::Get(TfToken const &token) const
{
    // nothing here, since right now all draw target tasks accessing
    // HdxDrawTarget perform downcast from Sprim To HdxDrawTarget
    // and use the C++ interface (e.g. IsEnabled(), GetRenderPassState()).
    return VtValue();
}

// virtual
HdDirtyBits
HdxDrawTarget::GetInitialDirtyBitsMask() const
{
    return AllDirty;
}

bool
HdxDrawTarget::WriteToFile(const HdRenderIndex &renderIndex,
                           const std::string &attachment,
                           const std::string &path) const
{
    // Check the draw targets been allocated
    if (!_drawTarget || !_drawTargetContext) {
        TF_WARN("Missing draw target");
        return false;
    }

    // XXX: The GlfDrawTarget will throw an error if attachment is invalid,
    // so need to check that it is valid first.
    //
    // This ends in a double-search of the map, but this path is for
    // debug and testing and not meant to be a performance path.
    if (!_drawTarget->GetAttachment(attachment)) {
        TF_WARN("Missing attachment\n");
        return false;
    }

    const HdxCamera *camera = _GetCamera(renderIndex);
    if (camera == nullptr) {
        TF_WARN("Missing camera\n");
        return false;
    }


    // embed camera matrices into metadata
    VtValue viewMatrixVt  = camera->Get(HdShaderTokens->worldToViewMatrix);
    VtValue projMatrixVt  = camera->Get(HdShaderTokens->projectionMatrix);
    const GfMatrix4d &viewMatrix = viewMatrixVt.Get<GfMatrix4d>();
    const GfMatrix4d &projMatrix = projMatrixVt.Get<GfMatrix4d>();

    // Make sure all draw target operations happen on the same
    // context.
    GlfGLContextSharedPtr oldContext = GlfGLContext::GetCurrentGLContext();
    GlfGLContext::MakeCurrent(_drawTargetContext);

    bool result = _drawTarget->WriteToFile(attachment, path,
                                           viewMatrix, projMatrix);

    GlfGLContext::MakeCurrent(oldContext);

    return result;
}

void
HdxDrawTarget::_SetAttachments(HdSceneDelegate *sceneDelegate,
                            const HdxDrawTargetAttachmentDescArray &attachments)
{
    if (!_drawTargetContext) {
        // Use one of the shared contexts as the master.
        _drawTargetContext = GlfGLContext::GetSharedGLContext();
    }

    // Clear out old texture resources for the attachments.
    _colorTextureResources.clear();
    _depthTextureResource.reset();


    // Make sure all draw target operations happen on the same
    // context.
    GlfGLContextSharedPtr oldContext = GlfGLContext::GetCurrentGLContext();

    GlfGLContext::MakeCurrent(_drawTargetContext);

    // XXX: Discard old draw target and create a new one
    // This is necessary because a we have to clone the draw target into each
    // gl context.
    // XXX : All draw targets in Hydra are currently trying to create MSAA
    // buffers (as long as they are allowed by the environment variables) 
    // because we need alpha to coverage for transparent object.
    _drawTarget = GlfDrawTarget::New(_resolution, /* MSAA */ true);

    size_t numAttachments = attachments.GetNumAttachments();
    _renderPassState.SetNumColorAttachments(numAttachments);

    _drawTarget->Bind();

    _colorTextureResources.resize(numAttachments);

    for (size_t attachmentNum = 0; attachmentNum < numAttachments;
                                                              ++attachmentNum) {
      const HdxDrawTargetAttachmentDesc &desc =
                                       attachments.GetAttachment(attachmentNum);

        GLenum format = GL_RGBA;
        GLenum type   = GL_BYTE;
        GLenum internalFormat = GL_RGBA8;
        HdConversions::GetGlFormat(desc.GetFormat(),
                                   &format, &type, &internalFormat);

        const std::string &name = desc.GetName();
        _drawTarget->AddAttachment(name,
                                   format,
                                   type,
                                   internalFormat);

        _renderPassState.SetColorClearValue(attachmentNum, desc.GetClearColor());

        _RegisterTextureResource(sceneDelegate,
                                 name,
                                 &_colorTextureResources[attachmentNum]);

        Hdx_DrawTargetTextureResource *resource =
                static_cast<Hdx_DrawTargetTextureResource *>(
                                 _colorTextureResources[attachmentNum].get());

        resource->SetAttachment(_drawTarget->GetAttachment(name));
        resource->SetSampler(desc.GetWrapS(),
                             desc.GetWrapT(),
                             desc.GetMinFilter(),
                             desc.GetMagFilter());

    }

    // Always add depth texture
    // XXX: GlfDrawTarget requires the depth texture be added last,
    // otherwise the draw target indexes are off-by-1.
    _drawTarget->AddAttachment(DEPTH_ATTACHMENT_NAME,
                               GL_DEPTH_COMPONENT,
                               GL_FLOAT,
                               GL_DEPTH_COMPONENT32F);

    _RegisterTextureResource(sceneDelegate,
                             DEPTH_ATTACHMENT_NAME,
                             &_depthTextureResource);


    Hdx_DrawTargetTextureResource *depthResource =
                    static_cast<Hdx_DrawTargetTextureResource *>(
                                                  _depthTextureResource.get());

    depthResource->SetAttachment(_drawTarget->GetAttachment(DEPTH_ATTACHMENT_NAME));
    depthResource->SetSampler(attachments.GetDepthWrapS(),
                              attachments.GetDepthWrapT(),
                              attachments.GetDepthMinFilter(),
                              attachments.GetDepthMagFilter());
   _drawTarget->Unbind();

   GlfGLContext::MakeCurrent(oldContext);

   // The texture bindings have changed so increment the version
   ++_version;
}


const HdxCamera *
HdxDrawTarget::_GetCamera(const HdRenderIndex &renderIndex) const
{
    return static_cast<const HdxCamera *>(
            renderIndex.GetSprim(HdPrimTypeTokens->camera, _cameraId));
}

void
HdxDrawTarget::_ResizeDrawTarget()
{
    // Make sure all draw target operations happen on the same
    // context.
    GlfGLContextSharedPtr oldContext = GlfGLContext::GetCurrentGLContext();

    GlfGLContext::MakeCurrent(_drawTargetContext);

    _drawTarget->Bind();
    _drawTarget->SetSize(_resolution);
    _drawTarget->Unbind();

    // The texture bindings might have changed so increment the version
    ++_version;

    GlfGLContext::MakeCurrent(oldContext);
}

void
HdxDrawTarget::_RegisterTextureResource(HdSceneDelegate *sceneDelegate,
                                        const std::string &name,
                                        HdTextureResourceSharedPtr *resourcePtr)
{
    HdResourceRegistry &resourceRegistry = HdResourceRegistry::GetInstance();

    // Create Path for the texture resource
    SdfPath resourcePath = GetID().AppendProperty(TfToken(name));

    // Ask delegate for an ID for this tex
    HdTextureResource::ID texID =
                              sceneDelegate->GetTextureResourceID(resourcePath);

    // Add to resource registry
    HdInstance<HdTextureResource::ID, HdTextureResourceSharedPtr> texInstance;
    std::unique_lock<std::mutex> regLock =
                  resourceRegistry.RegisterTextureResource(texID, &texInstance);

    if (texInstance.IsFirstInstance()) {
        texInstance.SetValue(HdTextureResourceSharedPtr(
                                          new Hdx_DrawTargetTextureResource()));
    }

    *resourcePtr =  texInstance.GetValue();
}


/*static*/
void
HdxDrawTarget::GetDrawTargets(HdSceneDelegate *sceneDelegate,
                              HdxDrawTargetPtrConstVector *drawTargets)
{
    const HdRenderIndex &renderIndex = sceneDelegate->GetRenderIndex();

    SdfPathVector sprimPaths =
            renderIndex.GetSprimSubtree(HdPrimTypeTokens->drawTarget,
                                        SdfPath::AbsoluteRootPath());

    TF_FOR_ALL (it, sprimPaths) {
        HdSprim const *drawTarget =
                        renderIndex.GetSprim(HdPrimTypeTokens->drawTarget, *it);

        if (drawTarget != nullptr)
        {
            drawTargets->push_back(static_cast<HdxDrawTarget const *>(drawTarget));
        }
    }
}

PXR_NAMESPACE_CLOSE_SCOPE

