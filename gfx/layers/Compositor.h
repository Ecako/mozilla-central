/* -*- Mode: C++; tab-width: 20; indent-tabs-mode: nil; c-basic-offset: 2 -*-
* This Source Code Form is subject to the terms of the Mozilla Public
* License, v. 2.0. If a copy of the MPL was not distributed with this
* file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef MOZILLA_GFX_COMPOSITOR_H
#define MOZILLA_GFX_COMPOSITOR_H

#include "mozilla/RefPtr.h"
#include "mozilla/gfx/Rect.h"
#include "mozilla/gfx/Matrix.h"
#include "gfxMatrix.h"
#include "nsAutoPtr.h"
#include "nsRegion.h"
#include "mozilla/layers/LayersTypes.h"
#include "mozilla/layers/CompositorTypes.h"
#include "Layers.h"

class gfxContext;
class gfxASurface;
class gfxImageSurface;
class nsIWidget;
class gfxReusableSurfaceWrapper;

typedef int32_t SurfaceDescriptorType;

namespace mozilla {
namespace gfx {
class DrawTarget;
}

namespace layers {

class Compositor;
struct Effect;
struct EffectChain;
class SurfaceDescriptor;
class Image;
class ISurfaceAllocator;
class CompositableHost;
class TextureHost;
class TextureInfo;
class TextureClient;
class ImageClient;
class CanvasClient;
class ContentClient;
class CompositableForwarder;
class ShadowableLayer;
class PTextureChild;
class TextureSourceOGL;
class TextureSourceD3D11;
class TextureParent;
struct TexturedEffect;

/**
 * A view on a TextureHost where the texture is internally represented as tiles
 * (contrast with a tiled buffer, where each texture is a tile). For iteration by
 * the texture's buffer host.
 * This is only useful when the underlying surface is too big to fit in one
 * device texture, which forces us to split it in smaller parts.
 * Tiled Compositable is a different thing.
 */
class TileIterator
{
public:
  virtual void BeginTileIteration() = 0;
  virtual nsIntRect GetTileRect() = 0;
  virtual size_t GetTileCount() = 0;
  virtual bool NextTile() = 0;
};

/**
 * TextureSource is the interface for texture objects that can be composited
 * by a given compositor backend. Since the drawing APIs are different
 * between backends, the TextureSource interface is split into different
 * interfaces (TextureSourceOGL, etc.), and TextureSource mostly provide
 * access to these interfaces.
 *
 * This class is used on the compositor side.
 */
class TextureSource : public RefCounted<TextureSource>
{
public:
  TextureSource() {
    MOZ_COUNT_CTOR(TextureSource);
  };
  virtual ~TextureSource() {
    MOZ_COUNT_DTOR(TextureSource);
  };

  virtual gfx::IntSize GetSize() const = 0;
  /**
   * Cast to an TextureSource for the OpenGL backend.
   */
  virtual TextureSourceOGL* AsSourceOGL() { return nullptr; }
  /**
   * Cast to an TextureSource for the D3D11 backend.
   */
  virtual TextureSourceD3D11* AsSourceD3D11() { return nullptr; }
  /**
   * In some rare cases we currently need to consider a group of textures as one
   * TextureSource, that can be split in sub-TextureSources. 
   */
  virtual TextureSource* GetSubSource(int index) { return nullptr; }
  /**
   * Overload this if the TextureSource supports big textures that don't fit in
   * one device texture and must be tiled internally.
   */
  virtual TileIterator* AsTileIterator() { return nullptr; }

#ifdef MOZ_LAYERS_HAVE_LOG
  virtual void PrintInfo(nsACString& aTo, const char* aPrefix);
#endif
};

/**
 * Interface
 *
 * TextureHost is a thin abstraction over texture data that need to be shared
 * or transfered from the content process to the compositor process.
 * TextureHost only knows how to deserialize or synchronize generic image data
 * (SurfaceDescriptor) and provide access to one or more TextureSource objects
 * (these provide the necessary APIs for compositor backends to composite the
 * image).
 *
 * A TextureHost should mostly correspond to one or several SurfaceDescriptor
 * types. This means that for YCbCr planes, even though they are represented as
 * 3 textures internally, use 1 TextureHost and not 3, because the 3 planes
 * arrive in the same IPC message.
 *
 * The Lock/Unlock mecanism here mirrors Lock/Unlock in TextureClient. These two
 * methods don't always have to use blocking locks, unless a resource is shared
 * between the two sides (like shared texture handles). For instance, in some cases
 * the data received in Update(...) is a copy in shared memory of the data owned 
 * by the content process, in which case no blocking lock is required.
 *
 * The TextureHost class handles buffering and the necessary code for async
 * texture updates, and the internals of this should not be exposed to the
 * the different implementations of TextureHost (other than selecting the
 * right strategy at construction time).
 *
 * TextureHosts can be changed at any time, for example if we receive a
 * SurfaceDescriptor type that was not expected. This should be an incentive
 * to keep the ownership model simple (especially on the OpenGL case, where
 * we have additionnal constraints).
 *
 * The class TextureImageTextureHostOGL is a good example of a TextureHost
 * implementation.
 *
 * This class is used only on the compositor side.
 */
class TextureHost : public TextureSource
{
public:
  TextureHost();
  virtual ~TextureHost();

  virtual gfx::SurfaceFormat GetFormat() const { return mFormat; }

  virtual bool IsValid() const { return true; }

  /**
   * Update the texture host using the data from aSurfaceDescriptor.
   */
  void Update(const SurfaceDescriptor& aImage,
              nsIntRegion *aRegion = nullptr);
  
  /**
   * Change the current surface of the texture host to aImage. aResult will return 
   * the previous surface.
   */
  void SwapTextures(const SurfaceDescriptor& aImage,
                    SurfaceDescriptor* aResult = nullptr,
                    nsIntRegion *aRegion = nullptr);

  /**
   * Update for tiled texture hosts could probably have a better signature, but we
   * will replace it with PTexture stuff anyway, so nm.
   */
  virtual void Update(gfxReusableSurfaceWrapper* aReusableSurface, TextureFlags aFlags, const gfx::IntSize& aSize) {}

  /**
   * Lock the texture host for compositing, returns an effect that should
   * be used to composite this texture.
   */
  virtual bool Lock() { return true; }

  /**
   * Unlock the texture host after compositing
   */
  virtual void Unlock() {}

  /**
   * Leave the texture host in a sane state if we abandon an update part-way
   * through.
   */
  virtual void Abort() {}

  void SetFlags(TextureFlags aFlags) { mFlags = aFlags; }
  void AddFlag(TextureFlags aFlag) { mFlags |= aFlag; }
  TextureFlags GetFlags() { return mFlags; }

  virtual void CleanupResources() {}
  virtual void SetCompositor(Compositor* aCompositor) {}

  ISurfaceAllocator* GetDeAllocator()
  {
    return mDeAllocator;
  }

#ifdef MOZ_DUMP_PAINTING
  virtual already_AddRefed<gfxImageSurface> Dump() { return nullptr; }
#endif

  bool operator== (const TextureHost& o) const
  {
    return GetIdentifier() == o.GetIdentifier();
  }
  bool operator!= (const TextureHost& o) const
  {
    return GetIdentifier() != o.GetIdentifier();
  }

  LayerRenderState GetRenderState()
  {
    return LayerRenderState(mBuffer,
                            mFlags & NeedsYFlip ? LAYER_RENDER_STATE_Y_FLIPPED : 0);
  }

  // IPC

  void SetTextureParent(TextureParent* aParent) {
    MOZ_ASSERT(!mTextureParent || mTextureParent == aParent);
    mTextureParent = aParent;
  }

  TextureParent* GetIPDLActor() const {
    return mTextureParent;
  }

#ifdef MOZ_LAYERS_HAVE_LOG
  virtual const char *Name() =0;
  virtual void PrintInfo(nsACString& aTo, const char* aPrefix);
#endif


  SurfaceDescriptor* GetBuffer() const { return mBuffer; }
  /**
   * Set a SurfaceDescriptor for this texture host. By setting a buffer and
   * allocator/de-allocator for the TextureHost, you cause the TextureHost to
   * retain a SurfaceDescriptor.
   * Ownership of the SurfaceDescriptor passes to this.
   */
  void SetBuffer(SurfaceDescriptor* aBuffer, ISurfaceAllocator* aAllocator)
  {
    MOZ_ASSERT(!mBuffer, "Will leak the old mBuffer");
    mBuffer = aBuffer;
    mDeAllocator = aAllocator;
  }

protected:
  /**
   * Should be implemented by the backend-specific TextureHost classes 
   * 
   * It should not take a reference to aImage, unless it knows the data 
   * to be thread-safe.
   */
  virtual void UpdateImpl(const SurfaceDescriptor& aImage,
                          nsIntRegion *aRegion)
  {
    NS_RUNTIMEABORT("Should not be reached");
  }
 
  /**
   * Should be implemented by the backend-specific TextureHost classes.
   *
   * Doesn't need to do the actual surface descriptor swap, just
   * any preparation work required to use the new descriptor.
   *
   * If the implementation doesn't define anything in particular
   * for handling swaps, then we can just do an update instead.
   */
  virtual void SwapTexturesImpl(const SurfaceDescriptor& aImage,
                                nsIntRegion *aRegion)
  {
    UpdateImpl(aImage, aRegion);
  }

  // An internal identifier for this texture host. Two texture hosts
  // should be considered equal iff their identifiers match. Should
  // not be exposed publicly.
  virtual uint64_t GetIdentifier() const {
    return reinterpret_cast<uint64_t>(this);
  }

  // Texture info
  TextureFlags mFlags;
  SurfaceDescriptor* mBuffer;
  gfx::SurfaceFormat mFormat;

  TextureParent* mTextureParent;
  ISurfaceAllocator* mDeAllocator;
};

/**
 * This can be used as an offscreen rendering target by the compositor, and
 * subsequently can be used as a source by the compositor.
 */
class CompositingRenderTarget : public TextureSource
{
public:
  virtual ~CompositingRenderTarget() {}

#ifdef MOZ_DUMP_PAINTING
  virtual already_AddRefed<gfxImageSurface> Dump(Compositor* aCompositor) { return nullptr; }
#endif
};

enum SurfaceInitMode
{
  INIT_MODE_NONE,
  INIT_MODE_CLEAR,
  INIT_MODE_COPY
};

/**
 * Common interface for compositor backends.
 */
class Compositor : public RefCounted<Compositor>
{
public:
  Compositor()
    : mCompositorID(0)
  {
    MOZ_COUNT_CTOR(Compositor);
  }
  virtual ~Compositor() {
    MOZ_COUNT_DTOR(Compositor);
  }

  virtual bool Initialize() = 0;
  virtual void Destroy() = 0;

  /* Request a texture host identifier that may be used for creating textures
   * accross process or thread boundaries that are compatible with this
   * compositor.
   */
  virtual TextureFactoryIdentifier
    GetTextureFactoryIdentifier() = 0;

  /**
   * Properties of the compositor
   */
  virtual bool CanUseCanvasLayerForSize(const gfxIntSize &aSize) = 0;
  virtual int32_t GetMaxTextureSize() const = 0;

  /**
   * Set the target for rendering, intended to be used for the duration of a transaction
   */
  virtual void SetTargetContext(gfxContext *aTarget) = 0;

  /**
   * Make sure that the underlying rendering API selects the right current
   * rendering context.
   */
  virtual void MakeCurrent(bool aForce = false) = 0;

  /**
   * Modifies the TextureIdentifier if needed in a fallback situation for aId
   */
  virtual void FallbackTextureInfo(TextureInfo& aInfo) {}

  /**
   * This creates a Surface that can be used as a rendering target by this
   * compositor.
   */
  virtual TemporaryRef<CompositingRenderTarget> CreateRenderTarget(const gfx::IntRect &aRect,
                                                                              SurfaceInitMode aInit) = 0;

  /**
   * This creates a Surface that can be used as a rendering target by this compositor,
   * and initializes this surface by copying from the given surface. If the given surface
   * is nullptr, the screen frame in progress is used as the source.
   */
  virtual TemporaryRef<CompositingRenderTarget> CreateRenderTargetFromSource(const gfx::IntRect &aRect,
                                                                             const CompositingRenderTarget* aSource) = 0;

  /**
   * Sets the given surface as the target for subsequent calls to DrawQuad.
   * Passing nullptr as aSurface sets the screen as the target.
   */
  virtual void SetRenderTarget(CompositingRenderTarget *aSurface) = 0;

  /**
   * Mostly the compositor will pull the size from a widget and this will
   * be ignored, but compositor implementations are free to use it if they
   * like.
   */
  virtual void SetRenderTargetSize(int aWidth, int aHeight) = 0;

  /**
   * This tells the compositor to actually draw a quad, where the area is
   * specified in userspace, and the source rectangle is the area of the
   * currently set textures to sample from. This area may not refer directly
   * to pixels depending on the effect.
   */
  virtual void DrawQuad(const gfx::Rect &aRect, const gfx::Rect *aClipRect,
                        const EffectChain &aEffectChain,
                        gfx::Float aOpacity, const gfx::Matrix4x4 &aTransform,
                        const gfx::Point &aOffset) = 0;

  /**
   * Start a new frame. If aClipRectIn is null, sets *aClipRectOut to the screen dimensions. 
   */
  virtual void BeginFrame(const gfx::Rect *aClipRectIn, const gfxMatrix& aTransform,
                          const gfx::Rect& aRenderBounds, gfx::Rect *aClipRectOut = nullptr) = 0;

  /**
   * Flush the current frame to the screen.
   */
  virtual void EndFrame(const gfxMatrix& aTransform) = 0;

  /**
   * Post rendering stuff if the rendering is outside of this Compositor
   * e.g., by Composer2D
   */
  virtual void EndFrameForExternalComposition(const gfxMatrix& aTransform) = 0;

  /**
   * Tidy up if BeginFrame has been called, but EndFrame won't be
   */
  virtual void AbortFrame() = 0;

  /**
   * Setup the viewport and projection matrix for rendering
   * to a window of the given dimensions.
   */
  virtual void PrepareViewport(int aWidth, int aHeight, const gfxMatrix& aWorldTransform) = 0;

  // save the current viewport
  virtual void SaveViewport() = 0;
  // resotre the previous viewport and return its bounds
  virtual gfx::IntRect RestoreViewport() = 0;

  /**
   * Whether textures created by this compositor can receive partial updates.
   */
  virtual bool SupportsPartialTextureUpdate() = 0;

#ifdef MOZ_DUMP_PAINTING
  virtual const char* Name() const = 0;
#endif // MOZ_DUMP_PAINTING


  /**
   * Each Compositor has a unique ID.
   * This ID is used to keep references to each Compositor in a map accessed
   * from the compositor thread only, so that async compositables can find
   * the right compositor parent and schedule compositing even if the compositor
   * changed.
   */
  uint32_t GetCompositorID() const
  {
    return mCompositorID;
  }
  void SetCompositorID(uint32_t aID)
  {
    NS_ASSERTION(mCompositorID==0, "The compositor ID must be set only once.");
    mCompositorID = aID;
  }

  virtual void NotifyShadowTreeTransaction() = 0;

  /**
   * Notify the compositor that composition is being paused/resumed.
   */
  virtual void Pause() {}
  /**
   * Returns true if succeeded
   */
  virtual bool Resume() { return true; }

  // I expect we will want to move mWidget into this class and implement this
  // method properly.
  virtual nsIWidget* GetWidget() const { return nullptr; }
  virtual nsIntSize* GetWidgetSize() {
    return nullptr;
  }

  /**
   * We enforce that there can only be one Compositor backend type off the main
   * thread at the same time. The backend type in use can be checked with this
   * static method.
   */
  static LayersBackend GetBackend();
protected:
  uint32_t mCompositorID;
  static LayersBackend sBackend;
};

class CompositingFactory
{
public:
  /**
   * The Create*Client methods each create, configure, and return a new compositable
   * client. If necessary, a message will be sent to the compositor
   * to create a corresponding compositable host.
   */
  static TemporaryRef<ImageClient> CreateImageClient(LayersBackend aBackendType,
                                                     CompositableType aImageHostType,
                                                     CompositableForwarder* aFwd,
                                                     TextureFlags aFlags);
  static TemporaryRef<CanvasClient> CreateCanvasClient(LayersBackend aBackendType,
                                                       CompositableType aImageHostType,
                                                       CompositableForwarder* aFwd,
                                                       TextureFlags aFlags);
  static TemporaryRef<ContentClient> CreateContentClient(LayersBackend aBackendType,
                                                         CompositableType aImageHostType,
                                                         CompositableForwarder* aFwd);

  static CompositableType TypeForImage(Image* aImage);
};

/**
 * Create a new texture host to handle surfaces of aDescriptorType
 *
 * @param aDescriptorType The SurfaceDescriptor type being passed
 * @param aTextureHostFlags Modifier flags that specify changes in the usage of a aDescriptorType, see TextureHostFlags
 * @param aTextureFlags Flags to pass to the new TextureHost
 * @param aBuffered True if the texture will be buffered (and updated via SwapTextures), or false if it will be used
 * unbuffered (and updated using Update).
 * #@param aDeAllocator A surface deallocator..
 */
TemporaryRef<TextureHost> CreateTextureHost(SurfaceDescriptorType aDescriptorType,
                                            uint32_t aTextureHostFlags,
                                            uint32_t aTextureFlags);

}
}

#endif /* MOZILLA_GFX_COMPOSITOR_H */
