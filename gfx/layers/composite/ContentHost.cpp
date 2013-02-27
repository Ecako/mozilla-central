/* -*- Mode: C++; tab-width: 20; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "ipc/AutoOpenSurface.h"
#include "mozilla/layers/ShadowLayers.h"
#include "mozilla/layers/ContentHost.h"
#include "mozilla/layers/TextureFactoryIdentifier.h" // for TextureInfo
#include "mozilla/layers/Effects.h"
#include "ReusableTileStoreComposite.h"
#include "gfxPlatform.h"
#include "LayersLogging.h"
#include "nsPrintfCString.h"

namespace mozilla {

using namespace gfx;

namespace layers {

void
CompositingThebesLayerBuffer::Composite(EffectChain& aEffectChain,
                                        float aOpacity,
                                        const gfx::Matrix4x4& aTransform,
                                        const gfx::Point& aOffset,
                                        const gfx::Filter& aFilter,
                                        const gfx::Rect& aClipRect,
                                        const nsIntRegion* aVisibleRegion /* = nullptr */,
                                        TiledLayerProperties* aLayerProperties /* = nullptr */)
{
  NS_ASSERTION(aVisibleRegion, "Requires a visible region");

  if (!mTextureHost || !mInitialised) {
    return;
  }

  bool isLocked = mTextureHost->Lock();
  if (!isLocked) {
    return;
  }

  RefPtr<TexturedEffect> effect = CreateTexturedEffect(mTextureHost, aFilter);
  if (mTextureHostOnWhite) {
    RefPtr<TexturedEffect> effectOnWhite = CreateTexturedEffect(mTextureHostOnWhite, aFilter);
    TextureSource* sourceOnBlack = mTextureHost->AsTextureSource();
    TextureSource* sourceOnWhite = mTextureHostOnWhite->AsTextureSource();
    aEffectChain.mPrimaryEffect =
      new EffectComponentAlpha(sourceOnBlack, sourceOnWhite);
  } else {
      aEffectChain.mPrimaryEffect = effect;
  }

  nsIntRegion tmpRegion;
  const nsIntRegion* renderRegion;
  if (PaintWillResample()) {
    // If we're resampling, then the texture image will contain exactly the
    // entire visible region's bounds, and we should draw it all in one quad
    // to avoid unexpected aliasing.
    tmpRegion = aVisibleRegion->GetBounds();
    renderRegion = &tmpRegion;
  } else {
    renderRegion = aVisibleRegion;
  }

  nsIntRegion region(*renderRegion);
  nsIntPoint origin = GetOriginOffset();
  region.MoveBy(-origin);           // translate into TexImage space, buffer origin might not be at texture (0,0)

  // Figure out the intersecting draw region
  TextureSource* source = mTextureHost->AsTextureSource();
  MOZ_ASSERT(source);
  gfx::IntSize texSize = source->GetSize();
  nsIntRect textureRect = nsIntRect(0, 0, texSize.width, texSize.height);
  textureRect.MoveBy(region.GetBounds().TopLeft());
  nsIntRegion subregion;
  subregion.And(region, textureRect);
  if (subregion.IsEmpty()) {
    // Region is empty, nothing to draw
    mTextureHost->Unlock();
    return;
  }

  nsIntRegion screenRects;
  nsIntRegion regionRects;

  // Collect texture/screen coordinates for drawing
  nsIntRegionRectIterator iter(subregion);
  while (const nsIntRect* iterRect = iter.Next()) {
    nsIntRect regionRect = *iterRect;
    nsIntRect screenRect = regionRect;
    screenRect.MoveBy(origin);

    screenRects.Or(screenRects, screenRect);
    regionRects.Or(regionRects, regionRect);
  }

  TileIterator* tileIter = source->AsTileIterator();
  TileIterator* iterOnWhite = nullptr;
  if (tileIter) {
    tileIter->BeginTileIteration();
  }

  if (mTextureHostOnWhite) {
    iterOnWhite = mTextureHostOnWhite->AsTextureSource()->AsTileIterator();
    NS_ASSERTION((!tileIter) || tileIter->GetTileCount() == iterOnWhite->GetTileCount(),
                 "Tile count mismatch on component alpha texture");
    if (iterOnWhite) {
      iterOnWhite->BeginTileIteration();
    }
  }

  bool usingTiles = (tileIter && tileIter->GetTileCount() > 1);
  do {
    if (iterOnWhite) {
      NS_ASSERTION(iterOnWhite->GetTileRect() == tileIter->GetTileRect(), "component alpha textures should be the same size.");
    }

    nsIntRect texRect = tileIter ? tileIter->GetTileRect()
                                 : nsIntRect(0, 0,
                                             texSize.width,
                                             texSize.height);

    // Draw texture. If we're using tiles, we do repeating manually, as texture
    // repeat would cause each individual tile to repeat instead of the
    // compound texture as a whole. This involves drawing at most 4 sections,
    // 2 for each axis that has texture repeat.
    for (int y = 0; y < (usingTiles ? 2 : 1); y++) {
      for (int x = 0; x < (usingTiles ? 2 : 1); x++) {
        nsIntRect currentTileRect(texRect);
        currentTileRect.MoveBy(x * texSize.width, y * texSize.height);

        nsIntRegionRectIterator screenIter(screenRects);
        nsIntRegionRectIterator regionIter(regionRects);

        const nsIntRect* screenRect;
        const nsIntRect* regionRect;
        while ((screenRect = screenIter.Next()) &&
               (regionRect = regionIter.Next())) {
            nsIntRect tileScreenRect(*screenRect);
            nsIntRect tileRegionRect(*regionRect);

            // When we're using tiles, find the intersection between the tile
            // rect and this region rect. Tiling is then handled by the
            // outer for-loops and modifying the tile rect.
            if (usingTiles) {
                tileScreenRect.MoveBy(-origin);
                tileScreenRect = tileScreenRect.Intersect(currentTileRect);
                tileScreenRect.MoveBy(origin);

                if (tileScreenRect.IsEmpty())
                  continue;

                tileRegionRect = regionRect->Intersect(currentTileRect);
                tileRegionRect.MoveBy(-currentTileRect.TopLeft());
            }
            gfx::Rect rect(tileScreenRect.x, tileScreenRect.y,
                           tileScreenRect.width, tileScreenRect.height);
            gfx::Rect sourceRect(tileRegionRect.x, tileRegionRect.y,
                                 tileRegionRect.width, tileRegionRect.height);
            gfx::Rect textureRect(0, 0,
                                  texRect.width, texRect.height);

            // XXX - Bas - Needs to be fixed for new lock API
            mCompositor->DrawQuad(rect, &aClipRect, aEffectChain,
                                  aOpacity, aTransform, aOffset);
        }
      }
    }

    if (iterOnWhite)
        iterOnWhite->NextTile();
  } while (usingTiles && tileIter->NextTile());

  mTextureHost->Unlock();
}

ContentHost::ContentHost(Compositor* aCompositor)
  : AContentHost(aCompositor)
  , mPaintWillResample(false)
  , mInitialised(false)
{
}

ContentHost::~ContentHost()
{
}

void 
ContentHost::AddTextureHost(TextureHost* aTextureHost)
{
  mTextureHost = aTextureHost;
}

TextureHost*
ContentHost::GetTextureHost()
{
  return mTextureHost.get();
}

void
ContentHost::Composite(EffectChain& aEffectChain,
                       float aOpacity,
                       const gfx::Matrix4x4& aTransform,
                       const Point& aOffset,
                       const Filter& aFilter,
                       const Rect& aClipRect,
                       const nsIntRegion* aVisibleRegion,
                       TiledLayerProperties* aLayerProperties)
{
  NS_ASSERTION(aVisibleRegion, "Requires a visible region");

  if (!mTextureHost->Lock()) {
    return;
  }

  aEffectChain.mPrimaryEffect = mTextureEffect;

  nsIntRegion tmpRegion;
  const nsIntRegion* renderRegion;
  if (PaintWillResample()) {
    // If we're resampling, then the texture image will contain exactly the
    // entire visible region's bounds, and we should draw it all in one quad
    // to avoid unexpected aliasing.
    tmpRegion = aVisibleRegion->GetBounds();
    renderRegion = &tmpRegion;
  } else {
    renderRegion = aVisibleRegion;
  }

  nsIntRegion region(*renderRegion);
  nsIntPoint origin = GetOriginOffset();
  region.MoveBy(-origin);           // translate into TexImage space, buffer origin might not be at texture (0,0)

  // Figure out the intersecting draw region
  TextureSource* source = mTextureHost->AsTextureSource();
  MOZ_ASSERT(source);
  gfx::IntSize texSize = source->GetSize();
  nsIntRect textureRect = nsIntRect(0, 0, texSize.width, texSize.height);
  textureRect.MoveBy(region.GetBounds().TopLeft());
  nsIntRegion subregion;
  subregion.And(region, textureRect);
  if (subregion.IsEmpty()) {
    // Region is empty, nothing to draw
    mTextureHost->Unlock();
    return;
  }

  nsIntRegion screenRects;
  nsIntRegion regionRects;

  // Collect texture/screen coordinates for drawing
  nsIntRegionRectIterator iter(subregion);
  while (const nsIntRect* iterRect = iter.Next()) {
    nsIntRect regionRect = *iterRect;
    nsIntRect screenRect = regionRect;
    screenRect.MoveBy(origin);

    screenRects.Or(screenRects, screenRect);
    regionRects.Or(regionRects, regionRect);
  }

  TileIterator* tileIter = source->AsTileIterator();
  TileIterator* iterOnWhite = nullptr;
  if (tileIter) {
    tileIter->BeginTileIteration();
  }

  if (mTextureHostOnWhite) {
    iterOnWhite = mTextureHostOnWhite->AsTextureSource()->AsTileIterator();
    NS_ASSERTION((!tileIter) || tileIter->GetTileCount() == iterOnWhite->GetTileCount(),
                 "Tile count mismatch on component alpha texture");
    if (iterOnWhite) {
      iterOnWhite->BeginTileIteration();
    }
  }

  bool usingTiles = (tileIter && tileIter->GetTileCount() > 1);
  do {
    if (iterOnWhite) {
      NS_ASSERTION(iterOnWhite->GetTileRect() == tileIter->GetTileRect(), "component alpha textures should be the same size.");
    }

    nsIntRect texRect = tileIter ? tileIter->GetTileRect()
                                 : nsIntRect(0, 0,
                                             texSize.width,
                                             texSize.height);

    // Draw texture. If we're using tiles, we do repeating manually, as texture
    // repeat would cause each individual tile to repeat instead of the
    // compound texture as a whole. This involves drawing at most 4 sections,
    // 2 for each axis that has texture repeat.
    for (int y = 0; y < (usingTiles ? 2 : 1); y++) {
      for (int x = 0; x < (usingTiles ? 2 : 1); x++) {
        nsIntRect currentTileRect(texRect);
        currentTileRect.MoveBy(x * texSize.width, y * texSize.height);

        nsIntRegionRectIterator screenIter(screenRects);
        nsIntRegionRectIterator regionIter(regionRects);

        const nsIntRect* screenRect;
        const nsIntRect* regionRect;
        while ((screenRect = screenIter.Next()) &&
               (regionRect = regionIter.Next())) {
            nsIntRect tileScreenRect(*screenRect);
            nsIntRect tileRegionRect(*regionRect);

            // When we're using tiles, find the intersection between the tile
            // rect and this region rect. Tiling is then handled by the
            // outer for-loops and modifying the tile rect.
            if (usingTiles) {
                tileScreenRect.MoveBy(-origin);
                tileScreenRect = tileScreenRect.Intersect(currentTileRect);
                tileScreenRect.MoveBy(origin);

                if (tileScreenRect.IsEmpty())
                  continue;

                tileRegionRect = regionRect->Intersect(currentTileRect);
                tileRegionRect.MoveBy(-currentTileRect.TopLeft());
            }
            gfx::Rect rect(tileScreenRect.x, tileScreenRect.y,
                           tileScreenRect.width, tileScreenRect.height);

            mTextureEffect->mTextureCoords = Rect(Float(tileRegionRect.x) / texRect.width,
                                                  Float(tileRegionRect.y) / texRect.height,
                                                  Float(tileRegionRect.width) / texRect.width,
                                                  Float(tileRegionRect.height) / texRect.height);
            GetCompositor()->DrawQuad(rect, &aClipRect, aEffectChain, aOpacity, aTransform, aOffset);
        }
      }
    }

    if (iterOnWhite)
        iterOnWhite->NextTile();
  } while (usingTiles && tileIter->NextTile());

  mTextureHost->Unlock();
}

void
ContentHostTexture::UpdateThebes(const ThebesBuffer& aNewFront,
                                 const nsIntRegion& aUpdated,
                                 OptionalThebesBuffer* aNewBack,
                                 const nsIntRegion& aOldValidRegionBack,
                                 OptionalThebesBuffer* aNewBackResult,
                                 nsIntRegion* aNewValidRegionFront,
                                 nsIntRegion* aUpdatedRegionBack)
{
  *aNewBackResult = null_t();
  *aNewBack = aNewFront;
  *aNewValidRegionFront = aOldValidRegionBack;
  aUpdatedRegionBack->SetEmpty();

  if (!mTextureHost) {
    mInitialised = false;
    return;
  }

  // updated is in screen coordinates. Convert it to buffer coordinates.
  nsIntRegion destRegion(aUpdated);
  destRegion.MoveBy(-aNewFront.rect().TopLeft());

  // Correct for rotation
  destRegion.MoveBy(aNewFront.rotation());

  gfxIntSize size = aNewFront.rect().Size();
  nsIntRect destBounds = destRegion.GetBounds();
  destRegion.MoveBy((destBounds.x >= size.width) ? -size.width : 0,
                    (destBounds.y >= size.height) ? -size.height : 0);

  // There's code to make sure that updated regions don't cross rotation
  // boundaries, so assert here that this is the case
  NS_ASSERTION(((destBounds.x % size.width) + destBounds.width <= size.width) &&
               ((destBounds.y % size.height) + destBounds.height <= size.height),
               "updated region lies across rotation boundaries!");

  mTextureHost->Update(aNewFront.buffer(), nullptr, nullptr, &destRegion);
  mInitialised = true;

  mBufferRect = aNewFront.rect();
  mBufferRotation = aNewFront.rotation();

  mTextureEffect =
    CreateTexturedEffect(mTextureHost, FILTER_LINEAR);
}

void
ContentHostDirect::UpdateThebes(const ThebesBuffer& aNewBack,
                                const nsIntRegion& aUpdated,
                                OptionalThebesBuffer* aNewFront,
                                const nsIntRegion& aOldValidRegionBack,
                                OptionalThebesBuffer* aNewBackResult,
                                nsIntRegion* aNewValidRegionFront,
                                nsIntRegion* aUpdatedRegionBack)
{
  mBufferRect = aNewBack.rect();
  mBufferRotation = aNewBack.rotation();

  if (!mTextureHost) {
    *aNewFront = null_t();
    mInitialised = false;

    aNewValidRegionFront->SetEmpty();
    *aNewBackResult = null_t();
    *aUpdatedRegionBack = aUpdated;
    return;
  }

  bool needsReset;
  SurfaceDescriptor newFrontBuffer;
  Update(aNewBack.buffer(), &newFrontBuffer, &mInitialised, &needsReset);
  
  if (!mInitialised) {
    // XXX if this happens often we could fallback to a different kind of
    // texture host. But that involves the TextureParent too, so it is not
    // trivial.
    NS_WARNING("Could not initialise texture host");
    *aNewFront = null_t();
    mInitialised = false;

    aNewValidRegionFront->SetEmpty();
    *aNewBackResult = null_t();
    *aUpdatedRegionBack = aUpdated;
    return;
  }

  *aNewFront = ThebesBuffer(newFrontBuffer, mBufferRect, mBufferRotation);

  // We have to invalidate the pixels painted into the new buffer.
  // They might overlap with our old pixels.
  aNewValidRegionFront->Sub(needsReset ? nsIntRegion()
                                       : mValidRegionForNextBackBuffer, 
                            aUpdated);
  *aNewBackResult = *aNewFront;
  *aUpdatedRegionBack = aUpdated;

  mTextureEffect =
    CreateTexturedEffect(mTextureHost, FILTER_LINEAR);
    
  // Save the current valid region of our front buffer, because if
  // we're double buffering, it's going to be the valid region for the
  // next back buffer sent back to the renderer.
  //
  // NB: we rely here on the fact that mValidRegion is initialized to
  // empty, and that the first time Swap() is called we don't have a
  // valid front buffer that we're going to return to content.
  mValidRegionForNextBackBuffer = aOldValidRegionBack;
}

void
TiledLayerBufferComposite::Upload(const BasicTiledLayerBuffer* aMainMemoryTiledBuffer,
                                  const nsIntRegion& aNewValidRegion,
                                  const nsIntRegion& aInvalidateRegion,
                                  const gfxSize& aResolution)
{
#ifdef GFX_TILEDLAYER_PREF_WARNINGS
  printf_stderr("Upload %i, %i, %i, %i\n", aInvalidateRegion.GetBounds().x, aInvalidateRegion.GetBounds().y, aInvalidateRegion.GetBounds().width, aInvalidateRegion.GetBounds().height);
  long start = PR_IntervalNow();
#endif

  mFrameResolution = aResolution;
  mMainMemoryTiledBuffer = aMainMemoryTiledBuffer;
  Update(aNewValidRegion, aInvalidateRegion);
  mMainMemoryTiledBuffer = nullptr;
#ifdef GFX_TILEDLAYER_PREF_WARNINGS
  if (PR_IntervalNow() - start > 10) {
    printf_stderr("Time to upload %i\n", PR_IntervalNow() - start);
  }
#endif
}

TiledTexture
TiledLayerBufferComposite::ValidateTile(TiledTexture aTile,
                                        const nsIntPoint& aTileOrigin,
                                        const nsIntRegion& aDirtyRect)
{
#ifdef GFX_TILEDLAYER_PREF_WARNINGS
  printf_stderr("Upload tile %i, %i\n", aTileOrigin.x, aTileOrigin.y);
  long start = PR_IntervalNow();
#endif

  aTile.Validate(mMainMemoryTiledBuffer->GetTile(aTileOrigin).GetSurface(), mCompositor, GetTileLength());

#ifdef GFX_TILEDLAYER_PREF_WARNINGS
  if (PR_IntervalNow() - start > 1) {
    printf_stderr("Tile Time to upload %i\n", PR_IntervalNow() - start);
  }
#endif
  return aTile;
}

TiledContentHost::~TiledContentHost()
{
  mMainMemoryTiledBuffer.ReadUnlock();
  mLowPrecisionMainMemoryTiledBuffer.ReadUnlock();
  if (mReusableTileStore)
    delete mReusableTileStore;
}

void
TiledContentHost::MemoryPressure()
{
  if (mReusableTileStore) {
    delete mReusableTileStore;
    mReusableTileStore = new ReusableTileStoreComposite(1);
  }
}

void
TiledContentHost::PaintedTiledLayerBuffer(const BasicTiledLayerBuffer* mTiledBuffer)
{
  if (mTiledBuffer->IsLowPrecision()) {
    mLowPrecisionMainMemoryTiledBuffer.ReadUnlock();
    mLowPrecisionMainMemoryTiledBuffer = *mTiledBuffer;
    mLowPrecisionRegionToUpload.Or(mLowPrecisionRegionToUpload,
                                   mLowPrecisionMainMemoryTiledBuffer.GetPaintedRegion());
    mLowPrecisionMainMemoryTiledBuffer.ClearPaintedRegion();
    mPendingLowPrecisionUpload = true;
  } else {
    mMainMemoryTiledBuffer.ReadUnlock();
    mMainMemoryTiledBuffer = *mTiledBuffer;
    mRegionToUpload.Or(mRegionToUpload, mMainMemoryTiledBuffer.GetPaintedRegion());
    mMainMemoryTiledBuffer.ClearPaintedRegion();
    mPendingUpload = true;
  }

  // TODO: Remove me once Bug 747811 lands.
  delete mTiledBuffer;
}

void
TiledContentHost::ProcessLowPrecisionUploadQueue()
{
  if (!mPendingLowPrecisionUpload)
    return;

  mLowPrecisionRegionToUpload.And(mLowPrecisionRegionToUpload,
                                  mLowPrecisionMainMemoryTiledBuffer.GetValidRegion());
  mLowPrecisionVideoMemoryTiledBuffer.SetResolution(
    mLowPrecisionMainMemoryTiledBuffer.GetResolution());
  // XXX It's assumed that the video memory tiled buffer has an up-to-date
  //     frame resolution. As it's always updated first when zooming, this
  //     should always be true.
  mLowPrecisionVideoMemoryTiledBuffer.Upload(&mLowPrecisionMainMemoryTiledBuffer,
                                 mLowPrecisionMainMemoryTiledBuffer.GetValidRegion(),
                                 mLowPrecisionRegionToUpload,
                                 mVideoMemoryTiledBuffer.GetFrameResolution());
  nsIntRegion validRegion = mLowPrecisionVideoMemoryTiledBuffer.GetValidRegion();

  mLowPrecisionMainMemoryTiledBuffer.ReadUnlock();

  mLowPrecisionMainMemoryTiledBuffer = BasicTiledLayerBuffer();
  mLowPrecisionRegionToUpload = nsIntRegion();
  mPendingLowPrecisionUpload = false;
}

void
TiledContentHost::ProcessUploadQueue(nsIntRegion* aNewValidRegion)
{
  if (!mPendingUpload)
    return;

  if (mReusableTileStore) {
    mReusableTileStore->HarvestTiles(mLayerProperties.mVisibleRegion,
                                     mLayerProperties.mDisplayPort,
                                     &mVideoMemoryTiledBuffer,
                                     mVideoMemoryTiledBuffer.GetValidRegion(),
                                     mMainMemoryTiledBuffer.GetValidRegion(),
                                     mVideoMemoryTiledBuffer.GetFrameResolution(),
                                     mLayerProperties.mEffectiveResolution);
  }

  // If we coalesce uploads while the layers' valid region is changing we will
  // end up trying to upload area outside of the valid region. (bug 756555)
  mRegionToUpload.And(mRegionToUpload, mMainMemoryTiledBuffer.GetValidRegion());

  mVideoMemoryTiledBuffer.Upload(&mMainMemoryTiledBuffer,
                                 mMainMemoryTiledBuffer.GetValidRegion(),
                                 mRegionToUpload, mLayerProperties.mEffectiveResolution);

  *aNewValidRegion = mVideoMemoryTiledBuffer.GetValidRegion();

  mMainMemoryTiledBuffer.ReadUnlock();
  // Release all the tiles by replacing the tile buffer with an empty
  // tiled buffer. This will prevent us from doing a double unlock when
  // calling  ~TiledThebesLayerComposite.
  // FIXME: This wont be needed when we do progressive upload and lock
  // tile by tile.
  mMainMemoryTiledBuffer = BasicTiledLayerBuffer();
  mRegionToUpload = nsIntRegion();
  mPendingUpload = false;
}

void
TiledContentHost::EnsureTileStore()
{
  // We should only be retaining old tiles if we're not fixed position.
  // Fixed position layers don't/shouldn't move on the screen, so retaining
  // tiles is not useful and often results in rendering artifacts.
  if (mReusableTileStore && !mLayerProperties.mRetainTiles) {
    delete mReusableTileStore;
    mReusableTileStore = nullptr;
  } else if (gfxPlatform::UseReusableTileStore() &&
             !mReusableTileStore && mLayerProperties.mRetainTiles) {
    // XXX Add a pref for reusable tile store size
    mReusableTileStore = new ReusableTileStoreComposite(1);
  }
}

void
TiledContentHost::UpdateThebes(const ThebesBuffer& aNewBack,
                               const nsIntRegion& aUpdated,
                               OptionalThebesBuffer* aNewFront,
                               const nsIntRegion& aOldValidRegionBack,
                               OptionalThebesBuffer* aNewBackResult,
                               nsIntRegion* aNewValidRegionFront,
                               nsIntRegion* aUpdatedRegionBack)
{
  MOZ_ASSERT(false, "N/A for tiled layers");
}

void
TiledContentHost::Composite(EffectChain& aEffectChain,
                            float aOpacity,
                            const gfx::Matrix4x4& aTransform,
                            const gfx::Point& aOffset,
                            const gfx::Filter& aFilter,
                            const gfx::Rect& aClipRect,
                            const nsIntRegion* aVisibleRegion /* = nullptr */,
                            TiledLayerProperties* aLayerProperties /* = nullptr */)
{
  MOZ_ASSERT(aLayerProperties, "aLayerProperties required for TiledContentHost");
  mLayerProperties = *aLayerProperties;

  EnsureTileStore();

  // note that ProcessUploadQueue updates the valid region which is then used by
  // the RenderLayerBuffer calls below and then sent back to the layer.
  ProcessUploadQueue(&mLayerProperties.mValidRegion);
  ProcessLowPrecisionUploadQueue();

  // Render old tiles to fill in gaps we haven't had the time to render yet.
  if (mReusableTileStore) {
    mReusableTileStore->DrawTiles(this,
                                  mVideoMemoryTiledBuffer.GetValidRegion(),
                                  mVideoMemoryTiledBuffer.GetFrameResolution(),
                                  aTransform, aOffset, aEffectChain, aOpacity, aFilter,
                                  aClipRect, mLayerProperties.mCompositionBounds);
  }

  // Render valid tiles.
  nsIntRect visibleRect = aVisibleRegion->GetBounds();

  RenderLayerBuffer(mLowPrecisionVideoMemoryTiledBuffer,
                    mLowPrecisionVideoMemoryTiledBuffer.GetValidRegion(), aEffectChain, aOpacity,
                    aOffset, aFilter, aClipRect, mLayerProperties.mValidRegion, visibleRect, aTransform);
  RenderLayerBuffer(mVideoMemoryTiledBuffer, mLayerProperties.mValidRegion, aEffectChain, aOpacity, aOffset,
                    aFilter, aClipRect, nsIntRegion(), visibleRect, aTransform);
}


void
TiledContentHost::RenderTile(const TiledTexture& aTile,
                             EffectChain& aEffectChain,
                             float aOpacity,
                             const gfx::Matrix4x4& aTransform,
                             const gfx::Point& aOffset,
                             const gfx::Filter& aFilter,
                             const gfx::Rect& aClipRect,
                             const nsIntRegion& aScreenRegion,
                             const nsIntPoint& aTextureOffset,
                             const nsIntSize& aTextureBounds)
{
  MOZ_ASSERT(aTile.mTextureHost, "Trying to render a placeholder tile?");

  //TODO y flip
  RefPtr<TexturedEffect> effect =
    CreateTexturedEffect(aTile.mTextureHost, aFilter);
  if (aTile.mTextureHost->Lock()) {
    aEffectChain.mPrimaryEffect = effect;
  } else {
    return;
  }

  nsIntRegionRectIterator it(aScreenRegion);
  for (const nsIntRect* rect = it.Next(); rect != nullptr; rect = it.Next()) {
    Rect graphicsRect(rect->x, rect->y, rect->width, rect->height);
    Rect textureRect(rect->x - aTextureOffset.x, rect->y - aTextureOffset.y,
                     rect->width, rect->height);

    effect->mTextureCoords = Rect(textureRect.x / aTextureBounds.width,
                                  textureRect.y / aTextureBounds.height,
                                  textureRect.width / aTextureBounds.width,
                                  textureRect.height / aTextureBounds.height);
    mCompositor->DrawQuad(graphicsRect, &aClipRect, aEffectChain, aOpacity, aTransform, aOffset);
  }

  aTile.mTextureHost->Unlock();
}

void
TiledContentHost::RenderLayerBuffer(TiledLayerBufferComposite& aLayerBuffer,
                                    const nsIntRegion& aValidRegion,
                                    EffectChain& aEffectChain,
                                    float aOpacity,
                                    const gfx::Point& aOffset,
                                    const gfx::Filter& aFilter,
                                    const gfx::Rect& aClipRect,
                                    const nsIntRegion& aMaskRegion,
                                    nsIntRect aVisibleRect,
                                    gfx::Matrix4x4 aTransform)
{
  float resolution = aLayerBuffer.GetResolution();
  gfxSize layerScale(1, 1);
  // We assume that the current frame resolution is the one used in our primary
  // layer buffer. Compensate for a changing frame resolution.
  if (aLayerBuffer.GetFrameResolution() != mVideoMemoryTiledBuffer.GetFrameResolution()) {
    const gfxSize& layerResolution = aLayerBuffer.GetFrameResolution();
    const gfxSize& localResolution = mVideoMemoryTiledBuffer.GetFrameResolution();
    layerScale.width = layerResolution.width / localResolution.width;
    layerScale.height = layerResolution.height / localResolution.height;
    aVisibleRect.ScaleRoundOut(layerScale.width, layerScale.height);
  }
  aTransform.Scale(1/(resolution * layerScale.width),
                   1/(resolution * layerScale.height), 1);

  uint32_t rowCount = 0;
  uint32_t tileX = 0;
  for (int32_t x = aVisibleRect.x; x < aVisibleRect.x + aVisibleRect.width;) {
    rowCount++;
    int32_t tileStartX = aLayerBuffer.GetTileStart(x);
    int32_t w = aLayerBuffer.GetScaledTileLength() - tileStartX;
    if (x + w > aVisibleRect.x + aVisibleRect.width)
      w = aVisibleRect.x + aVisibleRect.width - x;
    int tileY = 0;
    for (int32_t y = aVisibleRect.y; y < aVisibleRect.y + aVisibleRect.height;) {
      int32_t tileStartY = aLayerBuffer.GetTileStart(y);
      int32_t h = aLayerBuffer.GetScaledTileLength() - tileStartY;
      if (y + h > aVisibleRect.y + aVisibleRect.height)
        h = aVisibleRect.y + aVisibleRect.height - y;

      TiledTexture tileTexture = aLayerBuffer.
        GetTile(nsIntPoint(aLayerBuffer.RoundDownToTileEdge(x),
                           aLayerBuffer.RoundDownToTileEdge(y)));
      if (tileTexture != aLayerBuffer.GetPlaceholderTile()) {
        nsIntRegion tileDrawRegion;
        tileDrawRegion.And(aValidRegion,
                           nsIntRect(x * layerScale.width,
                                     y * layerScale.height,
                                     w * layerScale.width,
                                     h * layerScale.height));
        tileDrawRegion.Sub(tileDrawRegion, aMaskRegion);

        if (!tileDrawRegion.IsEmpty()) {
          tileDrawRegion.ScaleRoundOut(resolution / layerScale.width,
                                       resolution / layerScale.height);

          nsIntPoint tileOffset((x - tileStartX) * resolution,
                                (y - tileStartY) * resolution);
          uint32_t tileSize = aLayerBuffer.GetTileLength();
          RenderTile(tileTexture, aEffectChain, aOpacity, aTransform, aOffset, aFilter, aClipRect, tileDrawRegion,
                     tileOffset, nsIntSize(tileSize, tileSize));
        }
      }
      tileY++;
      y += h;
    }
    tileX++;
    x += w;
  }
}

void
TiledTexture::Validate(gfxReusableSurfaceWrapper* aReusableSurface, Compositor* aCompositor, uint16_t aSize) {
  TextureFlags flags = 0;
  if (!mTextureHost) {
    // convert placeholder tile to a real tile
    mTextureHost = aCompositor->CreateTextureHost(SURFACEDESCRIPTOR_UNKNOWN,
                                                  TEXTURE_HOST_TILED,
                                                  false,
                                                  NoFlags, nullptr);
    flags |= NewTile;
  }

  mTextureHost->Update(aReusableSurface, flags, gfx::IntSize(aSize, aSize));
}

#ifdef MOZ_LAYERS_HAVE_LOG
void
ContentHost::PrintInfo(nsACString& aTo, const char* aPrefix)
{
  aTo += aPrefix;
  aTo += nsPrintfCString("ContentHost (0x%p)", this);

  AppendToString(aTo, mBufferRect, " [buffer-rect=", "]");
  AppendToString(aTo, mBufferRotation, " [buffer-rotation=", "]");
  if (PaintWillResample()) {
    aTo += " [paint-will-resample]";
  }

  nsAutoCString pfx(aPrefix);
  pfx += "  ";

  if (mTextureEffect) {
    aTo += "\n";
    mTextureEffect->PrintInfo(aTo, pfx.get());
  }

  if (mTextureHost) {
    aTo += "\n";
    mTextureHost->PrintInfo(aTo, pfx.get());
  }

  if (mTextureHostOnWhite) {
    aTo += "\n";
    mTextureHostOnWhite->PrintInfo(aTo, pfx.get());
  }
}

void
TiledContentHost::PrintInfo(nsACString& aTo, const char* aPrefix)
{
  aTo += aPrefix;
  aTo += nsPrintfCString("TiledContentHost (0x%p)", this);

}
#endif


} // namespace
} // namespace
