/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: sw=2 ts=8 et :
 */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include <set>
#include <vector>

#include "gfxSharedImageSurface.h"
#include "gfxPlatform.h"

#include "AutoOpenSurface.h"
#include "mozilla/ipc/SharedMemorySysV.h"
#include "mozilla/layers/PLayerChild.h"
#include "mozilla/layers/PLayersChild.h"
#include "mozilla/layers/PLayersParent.h"
#include "mozilla/layers/TextureChild.h"
#include "mozilla/layers/CompositableClient.h"
#include "mozilla/layers/LayerTransaction.h"
#include "ShadowLayers.h"
#include "ShadowLayerChild.h"
#include "gfxipc/ShadowLayerUtils.h"
#include "SharedImageUtils.h"
#include "RenderTrace.h"
#include "sampler.h"
#include "nsXULAppAPI.h"
#include "TextureClient.h"
#include "ImageClient.h"
#include "CanvasClient.h"
#include "ContentClient.h"
#include "ISurfaceAllocator.h"

using namespace mozilla::ipc;
using namespace mozilla::gl;
using namespace mozilla::dom;

namespace mozilla {
namespace layers {

typedef nsTArray<SurfaceDescriptor> BufferArray; 
typedef std::vector<Edit> EditVector;
typedef std::set<ShadowableLayer*> ShadowableLayerSet;

class Transaction
{
public:
  Transaction()
    : mSwapRequired(false)
    , mOpen(false)
    , mRotationChanged(false)
  {}

  void Begin(const nsIntRect& aTargetBounds, ScreenRotation aRotation,
             const nsIntRect& aClientBounds, ScreenOrientation aOrientation)
  {
    mOpen = true;
    mTargetBounds = aTargetBounds;
    if (aRotation != mTargetRotation) {
        mRotationChanged = true;
    }
    mTargetRotation = aRotation;
    mClientBounds = aClientBounds;
    mTargetOrientation = aOrientation;
  }

  void AddEdit(const Edit& aEdit)
  {
    NS_ABORT_IF_FALSE(!Finished(), "forgot BeginTransaction?");
    mCset.push_back(aEdit);
  }
  void AddPaint(const Edit& aPaint)
  {
    AddNoSwapPaint(aPaint);
    mSwapRequired = true;
  }
  void AddPaint(const CompositableOperation& aPaint)
  {
    AddNoSwapPaint(Edit(aPaint));
    mSwapRequired = true;
  }

  void AddNoSwapPaint(const Edit& aPaint)
  {
    NS_ABORT_IF_FALSE(!Finished(), "forgot BeginTransaction?");
    mPaints.push_back(aPaint);
  }
  void AddNoSwapPaint(const CompositableOperation& aPaint)
  {
    NS_ABORT_IF_FALSE(!Finished(), "forgot BeginTransaction?");
    mPaints.push_back(Edit(aPaint));
  }
  void AddMutant(ShadowableLayer* aLayer)
  {
    NS_ABORT_IF_FALSE(!Finished(), "forgot BeginTransaction?");
    mMutants.insert(aLayer);
  }
  void AddBufferToDestroy(gfxSharedImageSurface* aBuffer)
  {
    return AddBufferToDestroy(aBuffer->GetShmem());
  }
  void AddBufferToDestroy(const SurfaceDescriptor& aBuffer)
  {
    NS_ABORT_IF_FALSE(!Finished(), "forgot BeginTransaction?");
    mDyingBuffers.AppendElement(aBuffer);
  }

  void End()
  {
    mCset.clear();
    mPaints.clear();
    mDyingBuffers.Clear();
    mMutants.clear();
    mOpen = false;
    mSwapRequired = false;
    mRotationChanged = false;
  }

  bool Empty() const {
    return mCset.empty() && mPaints.empty() && mMutants.empty();
  }
  bool RotationChanged() const {
    return mRotationChanged;
  }
  bool Finished() const { return !mOpen && Empty(); }

  EditVector mCset;
  EditVector mPaints;
  BufferArray mDyingBuffers;
  ShadowableLayerSet mMutants;
  nsIntRect mTargetBounds;
  ScreenRotation mTargetRotation;
  nsIntRect mClientBounds;
  ScreenOrientation mTargetOrientation;
  bool mSwapRequired;

private:
  bool mOpen;
  bool mRotationChanged;

  // disabled
  Transaction(const Transaction&);
  Transaction& operator=(const Transaction&);
};
struct AutoTxnEnd {
  AutoTxnEnd(Transaction* aTxn) : mTxn(aTxn) {}
  ~AutoTxnEnd() { mTxn->End(); }
  Transaction* mTxn;
};

void 
CompositableForwarder::IdentifyTextureHost(const TextureFactoryIdentifier& aIdentifier)
{
  mMaxTextureSize = aIdentifier.mMaxTextureSize;
  mCompositorBackend = aIdentifier.mParentBackend;
}

ShadowLayerForwarder::ShadowLayerForwarder()
 : mShadowManager(NULL)
 , mIsFirstPaint(false)
{
  mTxn = new Transaction();
}

ShadowLayerForwarder::~ShadowLayerForwarder()
{
  NS_ABORT_IF_FALSE(mTxn->Finished(), "unfinished transaction?");
  delete mTxn;
}

void
ShadowLayerForwarder::BeginTransaction(const nsIntRect& aTargetBounds,
                                       ScreenRotation aRotation,
                                       const nsIntRect& aClientBounds,
                                       ScreenOrientation aOrientation)
{
  NS_ABORT_IF_FALSE(HasShadowManager(), "no manager to forward to");
  NS_ABORT_IF_FALSE(mTxn->Finished(), "uncommitted txn?");
  mTxn->Begin(aTargetBounds, aRotation, aClientBounds, aOrientation);
}

static PLayerChild*
Shadow(ShadowableLayer* aLayer)
{
  return aLayer->GetShadow();
}

template<typename OpCreateT>
static void
CreatedLayer(Transaction* aTxn, ShadowableLayer* aLayer)
{
  aTxn->AddEdit(OpCreateT(NULL, Shadow(aLayer)));
}

void
ShadowLayerForwarder::CreatedThebesLayer(ShadowableLayer* aThebes)
{
  CreatedLayer<OpCreateThebesLayer>(mTxn, aThebes);
}
void
ShadowLayerForwarder::CreatedContainerLayer(ShadowableLayer* aContainer)
{
  CreatedLayer<OpCreateContainerLayer>(mTxn, aContainer);
}
void
ShadowLayerForwarder::CreatedImageLayer(ShadowableLayer* aImage)
{
  CreatedLayer<OpCreateImageLayer>(mTxn, aImage);
}
void
ShadowLayerForwarder::CreatedColorLayer(ShadowableLayer* aColor)
{
  CreatedLayer<OpCreateColorLayer>(mTxn, aColor);
}
void
ShadowLayerForwarder::CreatedCanvasLayer(ShadowableLayer* aCanvas)
{
  CreatedLayer<OpCreateCanvasLayer>(mTxn, aCanvas);
}
void
ShadowLayerForwarder::CreatedRefLayer(ShadowableLayer* aRef)
{
  CreatedLayer<OpCreateRefLayer>(mTxn, aRef);
}

void
ShadowLayerForwarder::DestroyedThebesBuffer(const SurfaceDescriptor& aBackBufferToDestroy)
{
  mTxn->AddBufferToDestroy(aBackBufferToDestroy);
}

void
ShadowLayerForwarder::Mutated(ShadowableLayer* aMutant)
{
mTxn->AddMutant(aMutant);
}

void
ShadowLayerForwarder::SetRoot(ShadowableLayer* aRoot)
{
  mTxn->AddEdit(OpSetRoot(NULL, Shadow(aRoot)));
}
void
ShadowLayerForwarder::InsertAfter(ShadowableLayer* aContainer,
                                  ShadowableLayer* aChild,
                                  ShadowableLayer* aAfter)
{
  if (aAfter)
    mTxn->AddEdit(OpInsertAfter(NULL, Shadow(aContainer),
                                NULL, Shadow(aChild),
                                NULL, Shadow(aAfter)));
  else
    mTxn->AddEdit(OpAppendChild(NULL, Shadow(aContainer),
                                NULL, Shadow(aChild)));
}
void
ShadowLayerForwarder::RemoveChild(ShadowableLayer* aContainer,
                                  ShadowableLayer* aChild)
{
  mTxn->AddEdit(OpRemoveChild(NULL, Shadow(aContainer),
                              NULL, Shadow(aChild)));
}
void
ShadowLayerForwarder::RepositionChild(ShadowableLayer* aContainer,
                                      ShadowableLayer* aChild,
                                      ShadowableLayer* aAfter)
{
  if (aAfter)
    mTxn->AddEdit(OpRepositionChild(NULL, Shadow(aContainer),
                                    NULL, Shadow(aChild),
                                    NULL, Shadow(aAfter)));
  else
    mTxn->AddEdit(OpRaiseToTopChild(NULL, Shadow(aContainer),
                                    NULL, Shadow(aChild)));
}

void
ShadowLayerForwarder::PaintedTiledLayerBuffer(ShadowableLayer* aLayer,
                                              BasicTiledLayerBuffer* aTiledLayerBuffer)
{
  if (XRE_GetProcessType() != GeckoProcessType_Default)
    NS_RUNTIMEABORT("PaintedTiledLayerBuffer must be made IPC safe (not share pointers)");
  mTxn->AddNoSwapPaint(OpPaintTiledLayerBuffer(NULL, Shadow(aLayer),
                                               uintptr_t(aTiledLayerBuffer)));
}

void
ShadowLayerForwarder::UpdateTexture(TextureClient* aTexture,
                                    const SurfaceDescriptor& aImage)
{
  MOZ_ASSERT(aImage.type() != SurfaceDescriptor::T__None, "[debug] STOP");
  MOZ_ASSERT(aImage.type() != SurfaceDescriptor::Tnull_t, "[debug] STOP");
  MOZ_ASSERT(aTexture);
  MOZ_ASSERT(aTexture->GetIPDLActor());
  mTxn->AddPaint(OpPaintTexture(nullptr, aTexture->GetIPDLActor(), aImage));
}

void
ShadowLayerForwarder::UpdateTextureRegion(TextureClient* aTexture,
                                          const ThebesBuffer& aThebesBuffer,
                                          const nsIntRegion& aUpdatedRegion)
{
  MOZ_ASSERT(aTexture);
  MOZ_ASSERT(aTexture->GetIPDLActor());
  mTxn->AddPaint(OpPaintTextureRegion(nullptr, aTexture->GetIPDLActor(),
                                      aThebesBuffer,
                                      aUpdatedRegion));
}

void
ShadowLayerForwarder::UpdatePictureRect(CompositableClient* aCompositable,
                                        const nsIntRect& aRect)
{
  mTxn->AddNoSwapPaint(OpUpdatePictureRect(nullptr, aCompositable->GetIPDLActor(), aRect));
}

bool
ShadowLayerForwarder::EndTransaction(InfallibleTArray<EditReply>* aReplies)
{
  SAMPLE_LABEL("ShadowLayerForwarder", "EndTranscation");
  RenderTraceScope rendertrace("Foward Transaction", "000091");
  NS_ABORT_IF_FALSE(HasShadowManager(), "no manager to forward to");
  NS_ABORT_IF_FALSE(!mTxn->Finished(), "forgot BeginTransaction?");

  AutoTxnEnd _(mTxn);

  if (mTxn->Empty() && !mTxn->RotationChanged()) {
    MOZ_LAYERS_LOG(("[LayersForwarder] 0-length cset (?) and no rotation event, skipping Update()"));
    return true;
  }

  MOZ_LAYERS_LOG(("[LayersForwarder] destroying buffers..."));

  for (uint32_t i = 0; i < mTxn->mDyingBuffers.Length(); ++i) {
    DestroySharedSurface(&mTxn->mDyingBuffers[i]);
  }

  MOZ_LAYERS_LOG(("[LayersForwarder] building transaction..."));

  // We purposely add attribute-change ops to the final changeset
  // before we add paint ops.  This allows layers to record the
  // attribute changes before new pixels arrive, which can be useful
  // for setting up back/front buffers.
  RenderTraceScope rendertrace2("Foward Transaction", "000092");
  for (ShadowableLayerSet::const_iterator it = mTxn->mMutants.begin();
       it != mTxn->mMutants.end(); ++it) {
    ShadowableLayer* shadow = *it;
    Layer* mutant = shadow->AsLayer();
    NS_ABORT_IF_FALSE(!!mutant, "unshadowable layer?");

    LayerAttributes attrs;
    CommonLayerAttributes& common = attrs.common();
    common.visibleRegion() = mutant->GetVisibleRegion();
    common.postXScale() = mutant->GetPostXScale();
    common.postYScale() = mutant->GetPostYScale();
    common.transform() = mutant->GetBaseTransform();
    common.contentFlags() = mutant->GetContentFlags();
    common.opacity() = mutant->GetOpacity();
    common.useClipRect() = !!mutant->GetClipRect();
    common.clipRect() = (common.useClipRect() ?
                         *mutant->GetClipRect() : nsIntRect());
    common.isFixedPosition() = mutant->GetIsFixedPosition();
    common.fixedPositionAnchor() = mutant->GetFixedPositionAnchor();
    if (Layer* maskLayer = mutant->GetMaskLayer()) {
      common.maskLayerChild() = Shadow(maskLayer->AsShadowableLayer());
    } else {
      common.maskLayerChild() = NULL;
    }
    common.maskLayerParent() = NULL;
    common.animations() = mutant->GetAnimations();
    attrs.specific() = null_t();
    mutant->FillSpecificAttributes(attrs.specific());

    mTxn->AddEdit(OpSetLayerAttributes(NULL, Shadow(shadow), attrs));
  }

  AutoInfallibleTArray<Edit, 10> cset;
  size_t nCsets = mTxn->mCset.size() + mTxn->mPaints.size();
  NS_ABORT_IF_FALSE(nCsets > 0, "should have bailed by now");

  cset.SetCapacity(nCsets);
  if (!mTxn->mCset.empty()) {
    cset.AppendElements(&mTxn->mCset.front(), mTxn->mCset.size());
  }
  // Paints after non-paint ops, including attribute changes.  See
  // above.
  if (!mTxn->mPaints.empty()) {
    cset.AppendElements(&mTxn->mPaints.front(), mTxn->mPaints.size());
  }

  TargetConfig targetConfig(mTxn->mTargetBounds, mTxn->mTargetRotation, mTxn->mClientBounds, mTxn->mTargetOrientation);

  MOZ_LAYERS_LOG(("[LayersForwarder] syncing before send..."));
  PlatformSyncBeforeUpdate();

  if (mTxn->mSwapRequired) {
    MOZ_LAYERS_LOG(("[LayersForwarder] sending transaction..."));
    RenderTraceScope rendertrace3("Forward Transaction", "000093");
    if (!mShadowManager->SendUpdate(cset, targetConfig, mIsFirstPaint,
                                    aReplies)) {
      MOZ_LAYERS_LOG(("[LayersForwarder] WARNING: sending transaction failed!"));
      return false;
    }
  } else {
    // If we don't require a swap we can call SendUpdateNoSwap which
    // assumes that aReplies is empty (DEBUG assertion)
    MOZ_LAYERS_LOG(("[LayersForwarder] sending no swap transaction..."));
    RenderTraceScope rendertrace3("Forward NoSwap Transaction", "000093");
    if (!mShadowManager->SendUpdateNoSwap(cset, targetConfig, mIsFirstPaint)) {
      MOZ_LAYERS_LOG(("[LayersForwarder] WARNING: sending transaction failed!"));
      return false;
    }
  }

  mIsFirstPaint = false;
  MOZ_LAYERS_LOG(("[LayersForwarder] ... done"));
  return true;
}

SharedMemory::SharedMemoryType
OptimalShmemType()
{
#if defined(MOZ_PLATFORM_MAEMO) && defined(MOZ_HAVE_SHAREDMEMORYSYSV)
  // Use SysV memory because maemo5 on the N900 only allots 64MB to
  // /dev/shm, even though it has 1GB(!!) of system memory.  Sys V shm
  // is allocated from a different pool.  We don't want an arbitrary
  // cap that's much much lower than available memory on the memory we
  // use for layers.
  return SharedMemory::TYPE_SYSV;
#else
  return SharedMemory::TYPE_BASIC;
#endif
}

bool
ISurfaceAllocator::AllocSharedImageSurface(const gfxIntSize& aSize,
                               gfxASurface::gfxContentType aContent,
                               gfxSharedImageSurface** aBuffer)
{
  SharedMemory::SharedMemoryType shmemType = OptimalShmemType();
  gfxASurface::gfxImageFormat format = gfxPlatform::GetPlatform()->OptimalFormatForContent(aContent);

  nsRefPtr<gfxSharedImageSurface> back =
    gfxSharedImageSurface::CreateUnsafe(this, aSize, format, shmemType);
  if (!back)
    return false;

  *aBuffer = nullptr;
  back.swap(*aBuffer);
  return true;
}

bool
ISurfaceAllocator::AllocSurfaceDescriptor(const gfxIntSize& aSize,
                                          gfxASurface::gfxContentType aContent,
                                          SurfaceDescriptor* aBuffer)
{
  return AllocSurfaceDescriptorWithCaps(aSize, aContent, DEFAULT_BUFFER_CAPS, aBuffer);
}

bool
ISurfaceAllocator::AllocSurfaceDescriptorWithCaps(const gfxIntSize& aSize,
                                                  gfxASurface::gfxContentType aContent,
                                                  uint32_t aCaps,
                                                  SurfaceDescriptor* aBuffer)
{
  bool tryPlatformSurface = true;
#ifdef DEBUG
  tryPlatformSurface = !PR_GetEnv("MOZ_LAYERS_FORCE_SHMEM_SURFACES");
#endif
  if (tryPlatformSurface &&
      PlatformAllocSurfaceDescriptor(aSize, aContent, aCaps, aBuffer)) {
    return true;
  }

  nsRefPtr<gfxSharedImageSurface> buffer;
  if (!AllocSharedImageSurface(aSize, aContent,
                               getter_AddRefs(buffer))) {
    return false;
  }

  *aBuffer = buffer->GetShmem();
  return true;
}

bool
ShadowLayerForwarder::AllocShmem(size_t aSize,
                                 ipc::SharedMemory::SharedMemoryType aType,
                                 ipc::Shmem* aShmem)
{
  return mShadowManager->AllocShmem(aSize, aType, aShmem);
}
bool
ShadowLayerForwarder::AllocUnsafeShmem(size_t aSize,
                                          ipc::SharedMemory::SharedMemoryType aType,
                                          ipc::Shmem* aShmem)
{
  return mShadowManager->AllocUnsafeShmem(aSize, aType, aShmem);
}
void
ShadowLayerForwarder::DeallocShmem(ipc::Shmem& aShmem)
{
  mShadowManager->DeallocShmem(aShmem);
}

/*static*/ already_AddRefed<gfxASurface>
ShadowLayerForwarder::OpenDescriptor(OpenMode aMode,
                                     const SurfaceDescriptor& aSurface)
{
  nsRefPtr<gfxASurface> surf = PlatformOpenDescriptor(aMode, aSurface);
  if (surf) {
    return surf.forget();
  }

  switch (aSurface.type()) {
  case SurfaceDescriptor::TShmem: {
    surf = gfxSharedImageSurface::Open(aSurface.get_Shmem());
    return surf.forget();
  }
  default:
    NS_RUNTIMEABORT("unexpected SurfaceDescriptor type!");
    return nullptr;
  }
}

/*static*/ gfxContentType
ShadowLayerForwarder::GetDescriptorSurfaceContentType(
  const SurfaceDescriptor& aDescriptor, OpenMode aMode,
  gfxASurface** aSurface)
{
  gfxContentType content;
  if (PlatformGetDescriptorSurfaceContentType(aDescriptor, aMode,
                                              &content, aSurface)) {
    return content;
  }

  nsRefPtr<gfxASurface> surface = OpenDescriptor(aMode, aDescriptor);
  content = surface->GetContentType();
  *aSurface = surface.forget().get();
  return content;
}

/*static*/ gfxIntSize
ShadowLayerForwarder::GetDescriptorSurfaceSize(
  const SurfaceDescriptor& aDescriptor, OpenMode aMode,
  gfxASurface** aSurface)
{
  gfxIntSize size;
  if (PlatformGetDescriptorSurfaceSize(aDescriptor, aMode, &size, aSurface)) {
    return size;
  }

  nsRefPtr<gfxASurface> surface = OpenDescriptor(aMode, aDescriptor);
  size = surface->GetSize();
  *aSurface = surface.forget().get();
  return size;
}

/*static*/ void
ShadowLayerForwarder::CloseDescriptor(const SurfaceDescriptor& aDescriptor)
{
  PlatformCloseDescriptor(aDescriptor);
  // There's no "close" needed for Shmem surfaces.
}

void
ISurfaceAllocator::DestroySharedSurface(SurfaceDescriptor* aSurface)
{
#ifdef GFX_COMPOSITOR_LOGGING
  printf(" -- ISurfaceAllocator::DestroySharedSurface\n");
#endif
  MOZ_ASSERT(aSurface);
  if (!aSurface) {
    return;
  }
  if (PlatformDestroySharedSurface(aSurface)) {
    return;
  }
  switch (aSurface->type()) {
    case SurfaceDescriptor::TShmem:
      DeallocShmem(aSurface->get_Shmem());
      break;
    case SurfaceDescriptor::TYCbCrImage:
      DeallocShmem(aSurface->get_YCbCrImage().data());
      break;
    case SurfaceDescriptor::TRGBImage:
      DeallocShmem(aSurface->get_RGBImage().data());
      break;
    case SurfaceDescriptor::TSurfaceDescriptorD3D10:
      break;
    case SurfaceDescriptor::Tnull_t:
    case SurfaceDescriptor::T__None:
#ifdef GFX_COMPOSITOR_LOGGING
      printf("    DestroySharedSurface: empty surface\n");
#endif
      break;
    default:
      NS_RUNTIMEABORT("surface type not implemented!");
  }
  *aSurface = SurfaceDescriptor();
}

void
ISurfaceAllocator::DestroySharedSurface(gfxSharedImageSurface* aSurface)
{
  NS_RUNTIMEABORT("TODO");
}


TemporaryRef<ImageClient>
ShadowLayerForwarder::CreateImageClientFor(const CompositableType& aCompositableType,
                                           ShadowableLayer* aLayer,
                                           TextureFlags aFlags)
{
  RefPtr<ImageClient> client = CompositingFactory::CreateImageClient(GetCompositorBackendType(),
                                                                     aCompositableType,
                                                                     this, aFlags);
  if (aCompositableType == BUFFER_BRIDGE) {
    static_cast<ImageClientBridge*>(client.get())->SetLayer(aLayer);
  }
  return client.forget();
}

TemporaryRef<CanvasClient>
ShadowLayerForwarder::CreateCanvasClientFor(const CompositableType& aCompositableType,
                                            ShadowableLayer* aLayer,
                                            TextureFlags aFlags)
{
  RefPtr<CanvasClient> client = CompositingFactory::CreateCanvasClient(GetCompositorBackendType(),
                                                                       aCompositableType,
                                                                       this, aFlags);
  return client.forget();
}

TemporaryRef<ContentClient>
ShadowLayerForwarder::CreateContentClientFor(const CompositableType& aCompositableType,
                                             ShadowableLayer* aLayer)
{
  RefPtr<ContentClient> client = CompositingFactory::CreateContentClient(GetCompositorBackendType(),
                                                                         aCompositableType,
                                                                         this);
  return client.forget();
}

PLayerChild*
ShadowLayerForwarder::ConstructShadowFor(ShadowableLayer* aLayer)
{
  NS_ABORT_IF_FALSE(HasShadowManager(), "no manager to forward to");
  return mShadowManager->SendPLayerConstructor(new ShadowLayerChild(aLayer));
}

/*
void
ShadowLayerManager::DestroySharedSurface(gfxSharedImageSurface* aSurface,
                                         PLayersParent* aDeallocator)
{
  aDeallocator->DeallocShmem(aSurface->GetShmem());
}

void
ShadowLayerManager::DestroySharedSurface(SurfaceDescriptor* aSurface,
                                         PLayersParent* aDeallocator)
{
  if (PlatformDestroySharedSurface(aSurface)) {
    return;
  }
  if (aSurface->type() == SurfaceDescriptor::TShmem) {
    DestroySharedShmemSurface(aSurface, aDeallocator);
  }
}

#if !defined(MOZ_HAVE_PLATFORM_SPECIFIC_LAYER_BUFFERS)

*/
#if !defined(MOZ_HAVE_PLATFORM_SPECIFIC_LAYER_BUFFERS)
bool
ISurfaceAllocator::PlatformAllocSurfaceDescriptor(const gfxIntSize&,
                                                  gfxASurface::gfxContentType,
                                                  uint32_t,
                                                  SurfaceDescriptor*)
{
  return false;
}

/*static*/ already_AddRefed<gfxASurface>
ShadowLayerForwarder::PlatformOpenDescriptor(OpenMode,
                                             const SurfaceDescriptor&)
{
  return nullptr;
}

/*static*/ bool
ShadowLayerForwarder::PlatformCloseDescriptor(const SurfaceDescriptor&)
{
  return false;
}

/*static*/ bool
ShadowLayerForwarder::PlatformGetDescriptorSurfaceContentType(
  const SurfaceDescriptor&,
  OpenMode,
  gfxContentType*,
  gfxASurface**)
{
  return false;
}

/*static*/ bool
ShadowLayerForwarder::PlatformGetDescriptorSurfaceSize(
  const SurfaceDescriptor&,
  OpenMode,
  gfxIntSize*,
  gfxASurface**)
{
  return false;
}

bool
ShadowLayerForwarder::PlatformDestroySharedSurface(SurfaceDescriptor*)
{
  return false;
}

/*static*/ void
ShadowLayerForwarder::PlatformSyncBeforeUpdate()
{
}

bool
ISurfaceAllocator::PlatformDestroySharedSurface(SurfaceDescriptor*)
{
  return false;
}

/*static*/ already_AddRefed<TextureImage>
ShadowLayerManager::OpenDescriptorForDirectTexturing(GLContext*,
                                                     const SurfaceDescriptor&,
                                                     GLenum)
{
  return nullptr;
}

/*static*/ bool
ShadowLayerManager::SupportsDirectTexturing()
{
  return false;
}

/*static*/ void
ShadowLayerManager::PlatformSyncBeforeReplyUpdate()
{
}

#endif  // !defined(MOZ_HAVE_PLATFORM_SPECIFIC_LAYER_BUFFERS)

bool
IsSurfaceDescriptorValid(const SurfaceDescriptor& aSurface)
{
  return aSurface.type() != SurfaceDescriptor::T__None &&
         aSurface.type() != SurfaceDescriptor::Tnull_t;
}

AutoOpenSurface::AutoOpenSurface(OpenMode aMode,
                                 const SurfaceDescriptor& aDescriptor)
  : mDescriptor(aDescriptor)
  , mMode(aMode)
{
  MOZ_ASSERT(IsSurfaceDescriptorValid(mDescriptor));
}

AutoOpenSurface::~AutoOpenSurface()
{
  if (mSurface) {
    mSurface = nullptr;
    ShadowLayerForwarder::CloseDescriptor(mDescriptor);
  }
}

gfxContentType
AutoOpenSurface::ContentType()
{
  if (mSurface) {
    return mSurface->GetContentType();
  }
  return ShadowLayerForwarder::GetDescriptorSurfaceContentType(
    mDescriptor, mMode, getter_AddRefs(mSurface));
}

gfxIntSize
AutoOpenSurface::Size()
{
  if (mSurface) {
    return mSurface->GetSize();
  }
  return ShadowLayerForwarder::GetDescriptorSurfaceSize(
    mDescriptor, mMode, getter_AddRefs(mSurface));
}

gfxASurface*
AutoOpenSurface::Get()
{
  if (!mSurface) {
    mSurface = ShadowLayerForwarder::OpenDescriptor(mMode, mDescriptor);
  }
  return mSurface.get();
}

gfxImageSurface*
AutoOpenSurface::GetAsImage()
{
  if (!mSurfaceAsImage) {
    mSurfaceAsImage = Get()->GetAsImageSurface();
  }
  return mSurfaceAsImage.get();
}

void
ShadowLayerForwarder::Connect(CompositableClient* aCompositable)
{
#ifdef GFX_COMPOSITOR_LOGGING
  printf("ShadowLayerForwarder::Connect(Compositable)\n");
#endif
  MOZ_ASSERT(aCompositable);
  CompositableChild* child = static_cast<CompositableChild*>(
    mShadowManager->SendPCompositableConstructor(aCompositable->GetType()));
  MOZ_ASSERT(child);
  aCompositable->SetIPDLActor(child);
  child->SetClient(aCompositable);
}

void ShadowLayerForwarder::Attach(CompositableClient* aCompositable,
                                  ShadowableLayer* aLayer)
{
  MOZ_ASSERT(aLayer);
  MOZ_ASSERT(aCompositable);
  MOZ_ASSERT(aCompositable->GetIPDLActor());
  mTxn->AddEdit(OpAttachCompositable(nullptr, Shadow(aLayer),
                                     nullptr, aCompositable->GetIPDLActor()));
}

void ShadowLayerForwarder::AttachAsyncCompositable(uint64_t aCompositableID,
                                                   ShadowableLayer* aLayer)
{
  MOZ_ASSERT(aLayer);
  MOZ_ASSERT(aCompositableID != 0); // zero is always an invalid compositable id.
  mTxn->AddEdit(OpAttachAsyncCompositable(nullptr, Shadow(aLayer),
                                          aCompositableID));
}

} // namespace layers
} // namespace mozilla
