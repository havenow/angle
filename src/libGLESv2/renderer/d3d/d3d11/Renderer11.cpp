//
// Copyright (c) 2012-2014 The ANGLE Project Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//

// Renderer11.cpp: Implements a back-end specific class for the D3D11 renderer.

#include "libGLESv2/main.h"
#include "libGLESv2/Buffer.h"
#include "libGLESv2/FramebufferAttachment.h"
#include "libGLESv2/ProgramBinary.h"
#include "libGLESv2/Framebuffer.h"
#include "libGLESv2/renderer/d3d/ProgramD3D.h"
#include "libGLESv2/renderer/d3d/ShaderD3D.h"
#include "libGLESv2/renderer/d3d/TextureD3D.h"
#include "libGLESv2/renderer/d3d/TransformFeedbackD3D.h"
#include "libGLESv2/renderer/d3d/d3d11/Renderer11.h"
#include "libGLESv2/renderer/d3d/d3d11/RenderTarget11.h"
#include "libGLESv2/renderer/d3d/d3d11/renderer11_utils.h"
#include "libGLESv2/renderer/d3d/d3d11/formatutils11.h"
#include "libGLESv2/renderer/d3d/d3d11/ShaderExecutable11.h"
#include "libGLESv2/renderer/d3d/d3d11/SwapChain11.h"
#include "libGLESv2/renderer/d3d/d3d11/Image11.h"
#include "libGLESv2/renderer/d3d/d3d11/VertexBuffer11.h"
#include "libGLESv2/renderer/d3d/d3d11/IndexBuffer11.h"
#include "libGLESv2/renderer/d3d/d3d11/Buffer11.h"
#include "libGLESv2/renderer/d3d/VertexDataManager.h"
#include "libGLESv2/renderer/d3d/IndexDataManager.h"
#include "libGLESv2/renderer/d3d/d3d11/TextureStorage11.h"
#include "libGLESv2/renderer/d3d/d3d11/Query11.h"
#include "libGLESv2/renderer/d3d/d3d11/Fence11.h"
#include "libGLESv2/renderer/d3d/d3d11/Blit11.h"
#include "libGLESv2/renderer/d3d/d3d11/Clear11.h"
#include "libGLESv2/renderer/d3d/d3d11/PixelTransfer11.h"
#include "libGLESv2/renderer/d3d/d3d11/VertexArray11.h"
#include "libGLESv2/renderer/d3d/d3d11/Buffer11.h"

#include "libEGL/Display.h"

#include "common/utilities.h"

#include <EGL/eglext.h>

#include <sstream>

// Enable ANGLE_SKIP_DXGI_1_2_CHECK if there is not a possibility of using cross-process
// HWNDs or the Windows 7 Platform Update (KB2670838) is expected to be installed.
#ifndef ANGLE_SKIP_DXGI_1_2_CHECK
#define ANGLE_SKIP_DXGI_1_2_CHECK 0
#endif

#ifdef _DEBUG
// this flag enables suppressing some spurious warnings that pop up in certain WebGL samples
// and conformance tests. to enable all warnings, remove this define.
#define ANGLE_SUPPRESS_D3D11_HAZARD_WARNINGS 1
#endif

namespace rx
{
static const DXGI_FORMAT RenderTargetFormats[] =
    {
        DXGI_FORMAT_B8G8R8A8_UNORM,
        DXGI_FORMAT_R8G8B8A8_UNORM,
#ifndef ANGLE_ENABLE_RENDER_TO_BACK_BUFFER
        // These formats are typically not valid swapchain buffer formats.
        // In particular, flip-model swapchains only support RGBA16, BGRA8 and RGBA8.
        DXGI_FORMAT_B5G6R5_UNORM,
        DXGI_FORMAT_B5G5R5A1_UNORM,
        DXGI_FORMAT_B4G4R4A4_UNORM
#endif
    };

static const DXGI_FORMAT DepthStencilFormats[] =
    {
        DXGI_FORMAT_UNKNOWN,
        DXGI_FORMAT_D24_UNORM_S8_UINT,
        DXGI_FORMAT_D16_UNORM
    };

enum
{
    MAX_TEXTURE_IMAGE_UNITS_VTF_SM4 = 16
};

Renderer11::Renderer11(egl::Display *display, EGLNativeDisplayType hDc, EGLint requestedDisplay)
    : Renderer(display),
      mDc(hDc),
      mRequestedDisplay(requestedDisplay)
{
    mVertexDataManager = NULL;
    mIndexDataManager = NULL;

    mLineLoopIB = NULL;
    mTriangleFanIB = NULL;

    mBlit = NULL;
    mPixelTransfer = NULL;

    mClear = NULL;

    mSyncQuery = NULL;

    mD3d11Module = NULL;
    mDxgiModule = NULL;

    mDeviceLost = false;

    mDevice = NULL;
    mDeviceContext = NULL;
    mDxgiAdapter = NULL;
    mDxgiFactory = NULL;

    mDriverConstantBufferVS = NULL;
    mDriverConstantBufferPS = NULL;

    mAppliedVertexShader = NULL;
    mAppliedGeometryShader = NULL;
    mCurPointGeometryShader = NULL;
    mAppliedPixelShader = NULL;

#ifdef ANGLE_ENABLE_RENDER_TO_BACK_BUFFER
    mRenderingToBackBuffer = false;
#endif

#if defined (ANGLE_ENABLE_WINDOWS_STORE)
    mSuspendedEventToken.value = 0;
#endif // defined (ANGLE_ENABLE_WINDOWS_STORE)
}

Renderer11::~Renderer11()
{
    unregisterForRendererTrimRequest();
    release();
}

Renderer11 *Renderer11::makeRenderer11(Renderer *renderer)
{
    ASSERT(HAS_DYNAMIC_TYPE(rx::Renderer11*, renderer));
    return static_cast<rx::Renderer11*>(renderer);
}

#ifndef __d3d11_1_h__
#define D3D11_MESSAGE_ID_DEVICE_DRAW_RENDERTARGETVIEW_NOT_SET ((D3D11_MESSAGE_ID)3146081)
#endif

EGLint Renderer11::initialize()
{
    if (!registerForRendererTrimRequest())
    {
        return EGL_NOT_INITIALIZED;
    }

    if (!mCompiler.initialize())
    {
        return EGL_NOT_INITIALIZED;
    }

#if !defined(ANGLE_ENABLE_WINDOWS_STORE)

    mDxgiModule = LoadLibrary(TEXT("dxgi.dll"));
    mD3d11Module = LoadLibrary(TEXT("d3d11.dll"));

    if (mD3d11Module == NULL || mDxgiModule == NULL)
    {
        ERR("Could not load D3D11 or DXGI library - aborting!\n");
        return EGL_NOT_INITIALIZED;
    }

    // create the D3D11 device
    ASSERT(mDevice == NULL);
    PFN_D3D11_CREATE_DEVICE D3D11CreateDevice = (PFN_D3D11_CREATE_DEVICE)GetProcAddress(mD3d11Module, "D3D11CreateDevice");

    if (D3D11CreateDevice == NULL)
    {
        ERR("Could not retrieve D3D11CreateDevice address - aborting!\n");
        return EGL_NOT_INITIALIZED;
    }

#endif // !defined(ANGLE_ENABLE_WINDOWS_STORE)

    D3D_FEATURE_LEVEL featureLevels[] = 
    {
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
        D3D_FEATURE_LEVEL_9_3,
    };

    D3D_FEATURE_LEVEL onlyfeatureLevel9_3[] =
    {
        D3D_FEATURE_LEVEL_9_3,
    };

    UINT featureLevelCount = 0; 
    D3D_FEATURE_LEVEL* featureLevelsRequested = featureLevels;
    if (mRequestedDisplay == EGL_PLATFORM_ANGLE_TYPE_D3D11_FL9_3_ANGLE || mRequestedDisplay == EGL_PLATFORM_ANGLE_TYPE_D3D11_FL9_3_WARP_ANGLE)
    {
        featureLevelsRequested = onlyfeatureLevel9_3;
        featureLevelCount = ArraySize(onlyfeatureLevel9_3);
    }
    else
    {
        featureLevelsRequested = featureLevels;
        featureLevelCount = ArraySize(featureLevels);
    }

    HRESULT result = S_OK;
    bool forceWarp = false;

    if (mRequestedDisplay == EGL_PLATFORM_ANGLE_TYPE_D3D11_WARP_ANGLE || mRequestedDisplay == EGL_PLATFORM_ANGLE_TYPE_D3D11_FL9_3_WARP_ANGLE)
    {
        forceWarp = true;
    }

#ifdef _DEBUG
    result = d3d11::createD3D11DeviceWithWARPFallback(D3D11_CREATE_DEVICE_DEBUG,
                                                      featureLevelsRequested,
                                                      featureLevelCount,
                                                      forceWarp,
                                                      &mDevice,
                                                      &mFeatureLevel,
                                                      &mDeviceContext);

    if (!mDevice || FAILED(result))
    {
        ERR("Failed creating Debug D3D11 device - falling back to release runtime.\n");
    }

    if (!mDevice || FAILED(result))
#endif
    {
        result = d3d11::createD3D11DeviceWithWARPFallback(0,
                                                          featureLevelsRequested,
                                                          featureLevelCount,
                                                          forceWarp,
                                                          &mDevice,
                                                          &mFeatureLevel,
                                                          &mDeviceContext);

        if (!mDevice || FAILED(result))
        {
            ERR("Could not create D3D11 device - aborting!\n");
            return EGL_NOT_INITIALIZED;   // Cleanup done by destructor through glDestroyRenderer
        }
    }

#if !defined(ANGLE_ENABLE_WINDOWS_STORE)
#if !ANGLE_SKIP_DXGI_1_2_CHECK
    // In order to create a swap chain for an HWND owned by another process, DXGI 1.2 is required.
    // The easiest way to check is to query for a IDXGIDevice2.
    bool requireDXGI1_2 = false;
    HWND hwnd = WindowFromDC(mDc);
    if (hwnd)
    {
        DWORD currentProcessId = GetCurrentProcessId();
        DWORD wndProcessId;
        GetWindowThreadProcessId(hwnd, &wndProcessId);
        requireDXGI1_2 = (currentProcessId != wndProcessId);
    }
    else
    {
        requireDXGI1_2 = true;
    }

    if (requireDXGI1_2)
    {
        IDXGIDevice2 *dxgiDevice2 = NULL;
        result = mDevice->QueryInterface(__uuidof(IDXGIDevice2), (void**)&dxgiDevice2);
        if (FAILED(result))
        {
            ERR("DXGI 1.2 required to present to HWNDs owned by another process.\n");
            return EGL_NOT_INITIALIZED;
        }
        SafeRelease(dxgiDevice2);
    }
#endif
#endif // !defined(ANGLE_ENABLE_WINDOWS_STORE)

    IDXGIDevice *dxgiDevice = NULL;
    result = mDevice->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDevice);

    if (FAILED(result))
    {
        ERR("Could not query DXGI device - aborting!\n");
        return EGL_NOT_INITIALIZED;
    }

    result = dxgiDevice->GetParent(__uuidof(IDXGIAdapter), (void**)&mDxgiAdapter);

    if (FAILED(result))
    {
        ERR("Could not retrieve DXGI adapter - aborting!\n");
        return EGL_NOT_INITIALIZED;
    }

    SafeRelease(dxgiDevice);

    IDXGIAdapter2* dxgiAdapter2 = NULL;
    result = mDxgiAdapter->QueryInterface(__uuidof(IDXGIAdapter2), (void**)&dxgiAdapter2);

    // On D3D_FEATURE_LEVEL_9_*, IDXGIAdapter::GetDesc returns "Software Adapter" for the description string.
    // If DXGI1.2 is available then IDXGIAdapter2::GetDesc2 can be used to get the actual hardware values.
    if (mFeatureLevel <= D3D_FEATURE_LEVEL_9_3 && SUCCEEDED(result))
    {
        DXGI_ADAPTER_DESC2 adapterDesc2 = {0};
        dxgiAdapter2->GetDesc2(&adapterDesc2);

        // Copy the contents of the DXGI_ADAPTER_DESC2 into mAdapterDescription (a DXGI_ADAPTER_DESC).
        memcpy(mAdapterDescription.Description, adapterDesc2.Description, sizeof(mAdapterDescription.Description));
        mAdapterDescription.VendorId = adapterDesc2.VendorId;
        mAdapterDescription.DeviceId = adapterDesc2.DeviceId;
        mAdapterDescription.SubSysId = adapterDesc2.SubSysId;
        mAdapterDescription.Revision = adapterDesc2.Revision;
        mAdapterDescription.DedicatedVideoMemory = adapterDesc2.DedicatedVideoMemory;
        mAdapterDescription.DedicatedSystemMemory = adapterDesc2.DedicatedSystemMemory;
        mAdapterDescription.SharedSystemMemory = adapterDesc2.SharedSystemMemory;
        mAdapterDescription.AdapterLuid = adapterDesc2.AdapterLuid;
    }
    else
    {
        mDxgiAdapter->GetDesc(&mAdapterDescription);
    }

    SafeRelease(dxgiAdapter2);

    memset(mDescription, 0, sizeof(mDescription));
    wcstombs(mDescription, mAdapterDescription.Description, sizeof(mDescription) - 1);

    result = mDxgiAdapter->GetParent(__uuidof(mDxgiFactory), (void**)&mDxgiFactory);

    if (!mDxgiFactory || FAILED(result))
    {
        ERR("Could not create DXGI factory - aborting!\n");
        return EGL_NOT_INITIALIZED;
    }

    // Disable some spurious D3D11 debug warnings to prevent them from flooding the output log
#if defined(ANGLE_SUPPRESS_D3D11_HAZARD_WARNINGS) && defined(_DEBUG)
    ID3D11InfoQueue *infoQueue;
    result = mDevice->QueryInterface(__uuidof(ID3D11InfoQueue),  (void **)&infoQueue);

    if (SUCCEEDED(result))
    {
        D3D11_MESSAGE_ID hideMessages[] =
        {
            D3D11_MESSAGE_ID_DEVICE_DRAW_RENDERTARGETVIEW_NOT_SET
        };

        D3D11_INFO_QUEUE_FILTER filter = {0};
        filter.DenyList.NumIDs = ArraySize(hideMessages);
        filter.DenyList.pIDList = hideMessages;

        infoQueue->AddStorageFilterEntries(&filter);
        SafeRelease(infoQueue);
    }
#endif

#ifdef ANGLE_ENABLE_RENDER_TO_BACK_BUFFER
    // We should initialize the variables for rendering to a backbuffer.
    // They are configured to render to an offscreen texture by default, but they should get overwritten as necessary.
    setRenderToBackBufferVariables(false);
#endif

    initializeDevice();

    return EGL_SUCCESS;
}

// do any one-time device initialization
// NOTE: this is also needed after a device lost/reset
// to reset the scene status and ensure the default states are reset.
void Renderer11::initializeDevice()
{
    mStateCache.initialize(mDevice);
    mInputLayoutCache.initialize(mDevice, mDeviceContext);

    ASSERT(!mVertexDataManager && !mIndexDataManager);
    mVertexDataManager = new VertexDataManager(this);
    mIndexDataManager = new IndexDataManager(this);

    ASSERT(!mBlit);
    mBlit = new Blit11(this);

    ASSERT(!mClear);
    mClear = new Clear11(this);

    ASSERT(!mPixelTransfer);
    // Fast copy buffer to texture isn't supported on D3D_FEATURE_LEVEL_9_X
    if (!(mFeatureLevel <= D3D_FEATURE_LEVEL_9_3))
    {
        mPixelTransfer = new PixelTransfer11(this);
    }

    const gl::Caps &rendererCaps = getRendererCaps();

    mForceSetVertexSamplerStates.resize(rendererCaps.maxVertexTextureImageUnits);
    mCurVertexSamplerStates.resize(rendererCaps.maxVertexTextureImageUnits);

    mForceSetPixelSamplerStates.resize(rendererCaps.maxTextureImageUnits);
    mCurPixelSamplerStates.resize(rendererCaps.maxTextureImageUnits);

    mCurVertexSRVs.resize(rendererCaps.maxVertexTextureImageUnits);
    mCurPixelSRVs.resize(rendererCaps.maxTextureImageUnits);

    markAllStateDirty();
}

int Renderer11::generateConfigs(ConfigDesc **configDescList)
{
    unsigned int numRenderFormats = ArraySize(RenderTargetFormats);
    unsigned int numDepthFormats = ArraySize(DepthStencilFormats);
    (*configDescList) = new ConfigDesc[numRenderFormats * numDepthFormats];
    int numConfigs = 0;

    for (unsigned int formatIndex = 0; formatIndex < numRenderFormats; formatIndex++)
    {
        const d3d11::DXGIFormat &renderTargetFormatInfo = d3d11::GetDXGIFormatInfo(RenderTargetFormats[formatIndex]);
        const gl::TextureCaps &renderTargetFormatCaps = getRendererTextureCaps().get(renderTargetFormatInfo.internalFormat);
        if (renderTargetFormatCaps.renderable)
        {
            for (unsigned int depthStencilIndex = 0; depthStencilIndex < numDepthFormats; depthStencilIndex++)
            {
                const d3d11::DXGIFormat &depthStencilFormatInfo = d3d11::GetDXGIFormatInfo(DepthStencilFormats[depthStencilIndex]);
                const gl::TextureCaps &depthStencilFormatCaps = getRendererTextureCaps().get(depthStencilFormatInfo.internalFormat);
                if (depthStencilFormatCaps.renderable || DepthStencilFormats[depthStencilIndex] == DXGI_FORMAT_UNKNOWN)
                {
                    ConfigDesc newConfig;
                    newConfig.renderTargetFormat = renderTargetFormatInfo.internalFormat;
                    newConfig.depthStencilFormat = depthStencilFormatInfo.internalFormat;
                    newConfig.multiSample = 0;     // FIXME: enumerate multi-sampling
                    newConfig.fastConfig = true;   // Assume all DX11 format conversions to be fast
                    newConfig.es3Capable = true;

                    (*configDescList)[numConfigs++] = newConfig;
                }
            }
        }
    }

    return numConfigs;
}

void Renderer11::deleteConfigs(ConfigDesc *configDescList)
{
    delete [] (configDescList);
}

void Renderer11::sync(bool block)
{
    if (block)
    {
        HRESULT result;

        if (!mSyncQuery)
        {
            D3D11_QUERY_DESC queryDesc;
            queryDesc.Query = D3D11_QUERY_EVENT;
            queryDesc.MiscFlags = 0;

            result = mDevice->CreateQuery(&queryDesc, &mSyncQuery);
            ASSERT(SUCCEEDED(result));
        }

        mDeviceContext->End(mSyncQuery);
        mDeviceContext->Flush();

        do
        {
            result = mDeviceContext->GetData(mSyncQuery, NULL, 0, D3D11_ASYNC_GETDATA_DONOTFLUSH);

            // Keep polling, but allow other threads to do something useful first
            Sleep(0);

            if (testDeviceLost(true))
            {
                return;
            }
        }
        while (result == S_FALSE);
    }
    else
    {
        mDeviceContext->Flush();
    }
}

SwapChain *Renderer11::createSwapChain(rx::NativeWindow nativeWindow, HANDLE shareHandle, GLenum backBufferFormat, GLenum depthBufferFormat)
{
    return new rx::SwapChain11(this, nativeWindow, shareHandle, backBufferFormat, depthBufferFormat);
}

gl::Error Renderer11::generateSwizzle(gl::Texture *texture)
{
    if (texture)
    {
        TextureStorage *texStorage = texture->getNativeTexture();
        if (texStorage)
        {
            TextureStorage11 *storage11 = TextureStorage11::makeTextureStorage11(texStorage);

            gl::Error error = storage11->generateSwizzles(texture->getSamplerState().swizzleRed,
                                                          texture->getSamplerState().swizzleGreen,
                                                          texture->getSamplerState().swizzleBlue,
                                                          texture->getSamplerState().swizzleAlpha);
            if (error.isError())
            {
                return error;
            }
        }
    }

    return gl::Error(GL_NO_ERROR);
}

gl::Error Renderer11::setSamplerState(gl::SamplerType type, int index, const gl::SamplerState &samplerState)
{
    if (type == gl::SAMPLER_PIXEL)
    {
        ASSERT(static_cast<unsigned int>(index) < getRendererCaps().maxTextureImageUnits);

        if (mForceSetPixelSamplerStates[index] || memcmp(&samplerState, &mCurPixelSamplerStates[index], sizeof(gl::SamplerState)) != 0)
        {
            ID3D11SamplerState *dxSamplerState = NULL;
            gl::Error error = mStateCache.getSamplerState(samplerState, &dxSamplerState);
            if (error.isError())
            {
                return error;
            }

            ASSERT(dxSamplerState != NULL);
            mDeviceContext->PSSetSamplers(index, 1, &dxSamplerState);

            mCurPixelSamplerStates[index] = samplerState;
        }

        mForceSetPixelSamplerStates[index] = false;
    }
    else if (type == gl::SAMPLER_VERTEX)
    {
        ASSERT(static_cast<unsigned int>(index) < getRendererCaps().maxVertexTextureImageUnits);

        if (mForceSetVertexSamplerStates[index] || memcmp(&samplerState, &mCurVertexSamplerStates[index], sizeof(gl::SamplerState)) != 0)
        {
            ID3D11SamplerState *dxSamplerState = NULL;
            gl::Error error = mStateCache.getSamplerState(samplerState, &dxSamplerState);
            if (error.isError())
            {
                return error;
            }

            ASSERT(dxSamplerState != NULL);
            mDeviceContext->VSSetSamplers(index, 1, &dxSamplerState);

            mCurVertexSamplerStates[index] = samplerState;
        }

        mForceSetVertexSamplerStates[index] = false;
    }
    else UNREACHABLE();

    return gl::Error(GL_NO_ERROR);
}

gl::Error Renderer11::setTexture(gl::SamplerType type, int index, gl::Texture *texture)
{
    ID3D11ShaderResourceView *textureSRV = NULL;
    bool forceSetTexture = false;

    if (texture)
    {
        TextureD3D* textureImpl = TextureD3D::makeTextureD3D(texture->getImplementation());
        TextureStorage *texStorage = textureImpl->getNativeTexture();
        ASSERT(texStorage != NULL);

        TextureStorage11 *storage11 = TextureStorage11::makeTextureStorage11(texStorage);
        gl::SamplerState samplerState;
        texture->getSamplerStateWithNativeOffset(&samplerState);
        textureSRV = storage11->getSRV(samplerState);

        // If we get NULL back from getSRV here, something went wrong in the texture class and we're unexpectedly
        // missing the shader resource view
        ASSERT(textureSRV != NULL);

        forceSetTexture = textureImpl->hasDirtyImages();
        textureImpl->resetDirty();
    }

    if (type == gl::SAMPLER_PIXEL)
    {
        ASSERT(static_cast<unsigned int>(index) < getRendererCaps().maxTextureImageUnits);

        if (forceSetTexture || mCurPixelSRVs[index] != textureSRV)
        {
            mDeviceContext->PSSetShaderResources(index, 1, &textureSRV);
        }

        mCurPixelSRVs[index] = textureSRV;
    }
    else if (type == gl::SAMPLER_VERTEX)
    {
        ASSERT(static_cast<unsigned int>(index) < getRendererCaps().maxVertexTextureImageUnits);

        if (forceSetTexture || mCurVertexSRVs[index] != textureSRV)
        {
            mDeviceContext->VSSetShaderResources(index, 1, &textureSRV);
        }

        mCurVertexSRVs[index] = textureSRV;
    }
    else UNREACHABLE();

    return gl::Error(GL_NO_ERROR);
}

gl::Error Renderer11::setUniformBuffers(const gl::Buffer *vertexUniformBuffers[], const gl::Buffer *fragmentUniformBuffers[])
{
    for (unsigned int uniformBufferIndex = 0; uniformBufferIndex < gl::IMPLEMENTATION_MAX_VERTEX_SHADER_UNIFORM_BUFFERS; uniformBufferIndex++)
    {
        const gl::Buffer *uniformBuffer = vertexUniformBuffers[uniformBufferIndex];
        if (uniformBuffer)
        {
            Buffer11 *bufferStorage = Buffer11::makeBuffer11(uniformBuffer->getImplementation());
            ID3D11Buffer *constantBuffer = bufferStorage->getBuffer(BUFFER_USAGE_UNIFORM);

            if (!constantBuffer)
            {
                return gl::Error(GL_OUT_OF_MEMORY);
            }

            if (mCurrentConstantBufferVS[uniformBufferIndex] != bufferStorage->getSerial())
            {
                mDeviceContext->VSSetConstantBuffers(getReservedVertexUniformBuffers() + uniformBufferIndex,
                                                     1, &constantBuffer);
                mCurrentConstantBufferVS[uniformBufferIndex] = bufferStorage->getSerial();
            }
        }
    }

    for (unsigned int uniformBufferIndex = 0; uniformBufferIndex < gl::IMPLEMENTATION_MAX_FRAGMENT_SHADER_UNIFORM_BUFFERS; uniformBufferIndex++)
    {
        const gl::Buffer *uniformBuffer = fragmentUniformBuffers[uniformBufferIndex];
        if (uniformBuffer)
        {
            Buffer11 *bufferStorage = Buffer11::makeBuffer11(uniformBuffer->getImplementation());
            ID3D11Buffer *constantBuffer = bufferStorage->getBuffer(BUFFER_USAGE_UNIFORM);

            if (!constantBuffer)
            {
                return gl::Error(GL_OUT_OF_MEMORY);
            }

            if (mCurrentConstantBufferPS[uniformBufferIndex] != bufferStorage->getSerial())
            {
                mDeviceContext->PSSetConstantBuffers(getReservedFragmentUniformBuffers() + uniformBufferIndex,
                                                     1, &constantBuffer);
                mCurrentConstantBufferPS[uniformBufferIndex] = bufferStorage->getSerial();
            }
        }
    }

    return gl::Error(GL_NO_ERROR);
}

gl::Error Renderer11::setRasterizerState(const gl::RasterizerState &rasterState)
{
    if (mForceSetRasterState || memcmp(&rasterState, &mCurRasterState, sizeof(gl::RasterizerState)) != 0)
    {
        ID3D11RasterizerState *dxRasterState = NULL;
        gl::Error error = mStateCache.getRasterizerState(rasterState, mScissorEnabled, &dxRasterState);
        if (error.isError())
        {
            return error;
        }

        mDeviceContext->RSSetState(dxRasterState);

        mCurRasterState = rasterState;
    }

    mForceSetRasterState = false;

    return gl::Error(GL_NO_ERROR);
}

gl::Error Renderer11::setBlendState(gl::Framebuffer *framebuffer, const gl::BlendState &blendState, const gl::ColorF &blendColor,
                                    unsigned int sampleMask)
{
    if (mForceSetBlendState ||
        memcmp(&blendState, &mCurBlendState, sizeof(gl::BlendState)) != 0 ||
        memcmp(&blendColor, &mCurBlendColor, sizeof(gl::ColorF)) != 0 ||
        sampleMask != mCurSampleMask)
    {
        ID3D11BlendState *dxBlendState = NULL;
        gl::Error error = mStateCache.getBlendState(framebuffer, blendState, &dxBlendState);
        if (error.isError())
        {
            return error;
        }

        ASSERT(dxBlendState != NULL);

        float blendColors[4] = {0.0f};
        if (blendState.sourceBlendRGB != GL_CONSTANT_ALPHA && blendState.sourceBlendRGB != GL_ONE_MINUS_CONSTANT_ALPHA &&
            blendState.destBlendRGB != GL_CONSTANT_ALPHA && blendState.destBlendRGB != GL_ONE_MINUS_CONSTANT_ALPHA)
        {
            blendColors[0] = blendColor.red;
            blendColors[1] = blendColor.green;
            blendColors[2] = blendColor.blue;
            blendColors[3] = blendColor.alpha;
        }
        else
        {
            blendColors[0] = blendColor.alpha;
            blendColors[1] = blendColor.alpha;
            blendColors[2] = blendColor.alpha;
            blendColors[3] = blendColor.alpha;
        }

        mDeviceContext->OMSetBlendState(dxBlendState, blendColors, sampleMask);

        mCurBlendState = blendState;
        mCurBlendColor = blendColor;
        mCurSampleMask = sampleMask;
    }

    mForceSetBlendState = false;

    return gl::Error(GL_NO_ERROR);
}

gl::Error Renderer11::setDepthStencilState(const gl::DepthStencilState &depthStencilState, int stencilRef,
                                           int stencilBackRef, bool frontFaceCCW)
{
    if (mForceSetDepthStencilState ||
        memcmp(&depthStencilState, &mCurDepthStencilState, sizeof(gl::DepthStencilState)) != 0 ||
        stencilRef != mCurStencilRef || stencilBackRef != mCurStencilBackRef)
    {
        ASSERT(depthStencilState.stencilWritemask == depthStencilState.stencilBackWritemask);
        ASSERT(stencilRef == stencilBackRef);
        ASSERT(depthStencilState.stencilMask == depthStencilState.stencilBackMask);

        ID3D11DepthStencilState *dxDepthStencilState = NULL;
        gl::Error error = mStateCache.getDepthStencilState(depthStencilState, &dxDepthStencilState);
        if (error.isError())
        {
            return error;
        }

        ASSERT(dxDepthStencilState);

        // Max D3D11 stencil reference value is 0xFF, corresponding to the max 8 bits in a stencil buffer
        // GL specifies we should clamp the ref value to the nearest bit depth when doing stencil ops
        META_ASSERT(D3D11_DEFAULT_STENCIL_READ_MASK == 0xFF);
        META_ASSERT(D3D11_DEFAULT_STENCIL_WRITE_MASK == 0xFF);
        UINT dxStencilRef = std::min<UINT>(stencilRef, 0xFFu);

        mDeviceContext->OMSetDepthStencilState(dxDepthStencilState, dxStencilRef);

        mCurDepthStencilState = depthStencilState;
        mCurStencilRef = stencilRef;
        mCurStencilBackRef = stencilBackRef;
    }

    mForceSetDepthStencilState = false;

    return gl::Error(GL_NO_ERROR);
}

void Renderer11::setScissorRectangle(const gl::Rectangle &scissor, bool enabled)
{
    if (mForceSetScissor || memcmp(&scissor, &mCurScissor, sizeof(gl::Rectangle)) != 0 ||
        enabled != mScissorEnabled)
    {
        if (enabled)
        {
            D3D11_RECT rect;
            rect.left = std::max(0, scissor.x);
            rect.top = std::max(0, scissor.y);
            rect.right = scissor.x + std::max(0, scissor.width);
            rect.bottom = scissor.y + std::max(0, scissor.height);

            mDeviceContext->RSSetScissorRects(1, &rect);
        }

        if (enabled != mScissorEnabled)
        {
            mForceSetRasterState = true;
        }

        mCurScissor = scissor;
        mScissorEnabled = enabled;
    }

    mForceSetScissor = false;
}

void Renderer11::setViewport(const gl::Rectangle &viewport, float zNear, float zFar, GLenum drawMode, GLenum frontFace,
                             bool ignoreViewport)
{
    gl::Rectangle actualViewport = viewport;
    float actualZNear = gl::clamp01(zNear);
    float actualZFar = gl::clamp01(zFar);

    if (ignoreViewport)
    {
        actualViewport.x = 0;
        actualViewport.y = 0;
        actualViewport.width = mRenderTargetDesc.width;
        actualViewport.height = mRenderTargetDesc.height;
        actualZNear = 0.0f;
        actualZFar = 1.0f;
    }
    else if (isRenderingToBackBuffer())
    {
        // When rendering directly to the swapchain backbuffer, we must invert the viewport in Y-axis.
        // This is due to the differences between the D3D and GL window origins.
        actualViewport.y = mRenderTargetDesc.height - viewport.y - viewport.height;
    }

    const gl::Caps& caps = getRendererCaps();

    // Clamp width and height first to the gl maximum, then clamp further if we extend past the D3D maximum bounds
    D3D11_VIEWPORT dxViewport;

    int maxViewportBoundsX = caps.maxViewportBoundsX;
    int maxViewportBoundsY = caps.maxViewportBoundsY;

    // On Feature Level 9_* we must also clamp the viewport to the size of the bound rendertarget.
    if (mFeatureLevel <= D3D_FEATURE_LEVEL_9_3)
    {
        maxViewportBoundsX = std::min<int>(maxViewportBoundsX, mRenderTargetDesc.width);
        maxViewportBoundsY = std::min<int>(maxViewportBoundsY, mRenderTargetDesc.height);
    }   

    int dxViewportTopLeftX = gl::clamp(actualViewport.x, caps.minViewportBoundsX, maxViewportBoundsX);
    int dxViewportTopLeftY = gl::clamp(actualViewport.y, caps.minViewportBoundsY, maxViewportBoundsY);

    dxViewport.TopLeftX = static_cast<float>(dxViewportTopLeftX);
    dxViewport.TopLeftY = static_cast<float>(dxViewportTopLeftY);
    dxViewport.Width    = static_cast<float>(gl::clamp(actualViewport.width, 0, maxViewportBoundsX - dxViewportTopLeftX));
    dxViewport.Height   = static_cast<float>(gl::clamp(actualViewport.height, 0, maxViewportBoundsY - dxViewportTopLeftY));
    dxViewport.MinDepth = actualZNear;
    dxViewport.MaxDepth = actualZFar;

    bool viewportChanged = mForceSetViewport || memcmp(&actualViewport, &mCurViewport, sizeof(gl::Rectangle)) != 0 ||
                           actualZNear != mCurNear || actualZFar != mCurFar;

    if (viewportChanged)
    {
        mDeviceContext->RSSetViewports(1, &dxViewport);

        mCurViewport = actualViewport;
        mCurNear = actualZNear;
        mCurFar = actualZFar;

        // On Feature Level 9_*, we must emulate negative (or large) viewports in the shaders using viewAdjust (like the D3D9 renderer).
        if (mFeatureLevel <= D3D_FEATURE_LEVEL_9_3)
        {
            mVertexConstants.viewAdjust[0] = (float)((actualViewport.width - (int)dxViewport.Width) + 2 * (actualViewport.x - dxViewportTopLeftX)) / dxViewport.Width;
            mVertexConstants.viewAdjust[1] = (float)((actualViewport.height - (int)dxViewport.Height) + 2 * (actualViewport.y - dxViewportTopLeftY)) / dxViewport.Height;
            mVertexConstants.viewAdjust[2] = (float)actualViewport.width / dxViewport.Width;
            mVertexConstants.viewAdjust[3] = (float)actualViewport.height / dxViewport.Height;
        }

        mPixelConstants.viewCoords[0] = actualViewport.width  * 0.5f;
        mPixelConstants.viewCoords[1] = actualViewport.height * 0.5f;
        mPixelConstants.viewCoords[2] = actualViewport.x + (actualViewport.width  * 0.5f);
        mPixelConstants.viewCoords[3] = actualViewport.y + (actualViewport.height * 0.5f);

        mPixelConstants.depthFront[0] = (actualZFar - actualZNear) * 0.5f;
        mPixelConstants.depthFront[1] = (actualZNear + actualZFar) * 0.5f;

        mVertexConstants.depthRange[0] = actualZNear;
        mVertexConstants.depthRange[1] = actualZFar;
        mVertexConstants.depthRange[2] = actualZFar - actualZNear;

        mPixelConstants.depthRange[0] = actualZNear;
        mPixelConstants.depthRange[1] = actualZFar;
        mPixelConstants.depthRange[2] = actualZFar - actualZNear;
    }

    mForceSetViewport = false;
}

bool Renderer11::applyPrimitiveType(GLenum mode, GLsizei count)
{
    D3D11_PRIMITIVE_TOPOLOGY primitiveTopology = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;

    GLsizei minCount = 0;

    switch (mode)
    {
      case GL_POINTS:         primitiveTopology = D3D11_PRIMITIVE_TOPOLOGY_POINTLIST;   minCount = 1; break;
      case GL_LINES:          primitiveTopology = D3D_PRIMITIVE_TOPOLOGY_LINELIST;      minCount = 2; break;
      case GL_LINE_LOOP:      primitiveTopology = D3D_PRIMITIVE_TOPOLOGY_LINESTRIP;     minCount = 2; break;
      case GL_LINE_STRIP:     primitiveTopology = D3D_PRIMITIVE_TOPOLOGY_LINESTRIP;     minCount = 2; break;
      case GL_TRIANGLES:      primitiveTopology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;  minCount = 3; break;
      case GL_TRIANGLE_STRIP: primitiveTopology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP; minCount = 3; break;
          // emulate fans via rewriting index buffer
      case GL_TRIANGLE_FAN:   primitiveTopology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;  minCount = 3; break;
      default:
        UNREACHABLE();
        return false;
    }

    if (primitiveTopology != mCurrentPrimitiveTopology)
    {
        mDeviceContext->IASetPrimitiveTopology(primitiveTopology);
        mCurrentPrimitiveTopology = primitiveTopology;
    }

    return count >= minCount;
}

gl::Error Renderer11::applyRenderTarget(gl::Framebuffer *framebuffer)
{
    // Get the color render buffer and serial
    // Also extract the render target dimensions and view
    unsigned int renderTargetWidth = 0;
    unsigned int renderTargetHeight = 0;
    GLenum renderTargetFormat = 0;
    unsigned int renderTargetSerials[gl::IMPLEMENTATION_MAX_DRAW_BUFFERS] = {0};
    ID3D11RenderTargetView* framebufferRTVs[gl::IMPLEMENTATION_MAX_DRAW_BUFFERS] = {NULL};
    bool missingColorRenderTarget = true;

    const gl::ColorbufferInfo &colorbuffers = framebuffer->getColorbuffersForRender();

    for (size_t colorAttachment = 0; colorAttachment < colorbuffers.size(); ++colorAttachment)
    {
        gl::FramebufferAttachment *colorbuffer = colorbuffers[colorAttachment];

        if (colorbuffer)
        {
            // the draw buffer must be either "none", "back" for the default buffer or the same index as this color (in order)

            // check for zero-sized default framebuffer, which is a special case.
            // in this case we do not wish to modify any state and just silently return false.
            // this will not report any gl error but will cause the calling method to return.
            if (colorbuffer->getWidth() == 0 || colorbuffer->getHeight() == 0)
            {
                return gl::Error(GL_NO_ERROR);
            }

            renderTargetSerials[colorAttachment] = GetAttachmentSerial(colorbuffer);

            // Extract the render target dimensions and view
            RenderTarget11 *renderTarget = d3d11::GetAttachmentRenderTarget(colorbuffer);
            if (!renderTarget)
            {
                return gl::Error(GL_OUT_OF_MEMORY, "Internal render target pointer unexpectedly null.");
            }

            framebufferRTVs[colorAttachment] = renderTarget->getRenderTargetView();
            if (!framebufferRTVs[colorAttachment])
            {
                return gl::Error(GL_OUT_OF_MEMORY, "Internal render target view pointer unexpectedly null.");
            }

            if (missingColorRenderTarget)
            {
                renderTargetWidth = colorbuffer->getWidth();
                renderTargetHeight = colorbuffer->getHeight();
                renderTargetFormat = colorbuffer->getActualFormat();
                missingColorRenderTarget = false;
            }

            // TODO: Detect if this color buffer is already bound as a texture and unbind it first to prevent
            //       D3D11 warnings.
        }
    }

    // Get the depth stencil render buffter and serials
    gl::FramebufferAttachment *depthStencil = framebuffer->getDepthbuffer();
    unsigned int depthbufferSerial = 0;
    unsigned int stencilbufferSerial = 0;
    if (depthStencil)
    {
        depthbufferSerial = GetAttachmentSerial(depthStencil);
    }
    else if (framebuffer->getStencilbuffer())
    {
        depthStencil = framebuffer->getStencilbuffer();
        stencilbufferSerial = GetAttachmentSerial(depthStencil);
    }

    ID3D11DepthStencilView* framebufferDSV = NULL;
    if (depthStencil)
    {
        RenderTarget11 *depthStencilRenderTarget = d3d11::GetAttachmentRenderTarget(depthStencil);
        if (!depthStencilRenderTarget)
        {
            SafeRelease(framebufferRTVs);
            return gl::Error(GL_OUT_OF_MEMORY, "Internal render target pointer unexpectedly null.");
        }

        framebufferDSV = depthStencilRenderTarget->getDepthStencilView();
        if (!framebufferDSV)
        {
            SafeRelease(framebufferRTVs);
            return gl::Error(GL_OUT_OF_MEMORY, "Internal depth stencil view pointer unexpectedly null.");
        }

        // If there is no render buffer, the width, height and format values come from
        // the depth stencil
        if (missingColorRenderTarget)
        {
            renderTargetWidth = depthStencil->getWidth();
            renderTargetHeight = depthStencil->getHeight();
            renderTargetFormat = depthStencil->getActualFormat();
        }
    }

    // Apply the render target and depth stencil
    if (!mRenderTargetDescInitialized || !mDepthStencilInitialized ||
        memcmp(renderTargetSerials, mAppliedRenderTargetSerials, sizeof(renderTargetSerials)) != 0 ||
        depthbufferSerial != mAppliedDepthbufferSerial ||
        stencilbufferSerial != mAppliedStencilbufferSerial)
    {
        mDeviceContext->OMSetRenderTargets(getRendererCaps().maxDrawBuffers, framebufferRTVs, framebufferDSV);

#ifdef ANGLE_ENABLE_RENDER_TO_BACK_BUFFER
        if (framebufferRTVs[0])
        {
            ID3D11Resource* res = NULL;
            framebufferRTVs[0]->GetResource(&res);

            setRenderToBackBufferVariables(d3d11::IsBackbuffer(res));
            SafeRelease(res);
        }
#endif

        mRenderTargetDesc.width = renderTargetWidth;
        mRenderTargetDesc.height = renderTargetHeight;
        mRenderTargetDesc.format = renderTargetFormat;
        mForceSetViewport = true;
        mForceSetScissor = true;
        mForceSetBlendState = true;

        if (!mDepthStencilInitialized)
        {
            mForceSetRasterState = true;
        }

        for (unsigned int rtIndex = 0; rtIndex < gl::IMPLEMENTATION_MAX_DRAW_BUFFERS; rtIndex++)
        {
            mAppliedRenderTargetSerials[rtIndex] = renderTargetSerials[rtIndex];
        }
        mAppliedDepthbufferSerial = depthbufferSerial;
        mAppliedStencilbufferSerial = stencilbufferSerial;
        mRenderTargetDescInitialized = true;
        mDepthStencilInitialized = true;
    }

    invalidateFramebufferSwizzles(framebuffer);

    return gl::Error(GL_NO_ERROR);
}

gl::Error Renderer11::applyVertexBuffer(gl::ProgramBinary *programBinary, const gl::VertexAttribute vertexAttributes[], const gl::VertexAttribCurrentValueData currentValues[],
                                        GLint first, GLsizei count, GLsizei instances)
{
    TranslatedAttribute attributes[gl::MAX_VERTEX_ATTRIBS];
    gl::Error error = mVertexDataManager->prepareVertexData(vertexAttributes, currentValues, programBinary, first, count, attributes, instances);
    if (error.isError())
    {
        return error;
    }

    return mInputLayoutCache.applyVertexBuffers(attributes, programBinary);
}

gl::Error Renderer11::applyIndexBuffer(const GLvoid *indices, gl::Buffer *elementArrayBuffer, GLsizei count, GLenum mode, GLenum type, TranslatedIndexData *indexInfo)
{
    gl::Error error = mIndexDataManager->prepareIndexData(type, count, elementArrayBuffer, indices, indexInfo);
    if (error.isError())
    {
        return error;
    }

    ID3D11Buffer *buffer = NULL;
    DXGI_FORMAT bufferFormat = (indexInfo->indexType == GL_UNSIGNED_INT) ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT;

    if (indexInfo->storage)
    {
        Buffer11 *storage = Buffer11::makeBuffer11(indexInfo->storage);
        buffer = storage->getBuffer(BUFFER_USAGE_INDEX);
    }
    else
    {
        IndexBuffer11* indexBuffer = IndexBuffer11::makeIndexBuffer11(indexInfo->indexBuffer);
        buffer = indexBuffer->getBuffer();
    }

    if (buffer != mAppliedIB || bufferFormat != mAppliedIBFormat || indexInfo->startOffset != mAppliedIBOffset)
    {
        mDeviceContext->IASetIndexBuffer(buffer, bufferFormat, indexInfo->startOffset);

        mAppliedIB = buffer;
        mAppliedIBFormat = bufferFormat;
        mAppliedIBOffset = indexInfo->startOffset;
    }

    return gl::Error(GL_NO_ERROR);
}

void Renderer11::applyTransformFeedbackBuffers(gl::Buffer *transformFeedbackBuffers[], GLintptr offsets[])
{
    ID3D11Buffer* d3dBuffers[gl::IMPLEMENTATION_MAX_TRANSFORM_FEEDBACK_BUFFERS];
    UINT d3dOffsets[gl::IMPLEMENTATION_MAX_TRANSFORM_FEEDBACK_BUFFERS];
    bool requiresUpdate = false;
    for (size_t i = 0; i < gl::IMPLEMENTATION_MAX_TRANSFORM_FEEDBACK_BUFFERS; i++)
    {
        if (transformFeedbackBuffers[i])
        {
            Buffer11 *storage = Buffer11::makeBuffer11(transformFeedbackBuffers[i]->getImplementation());
            ID3D11Buffer *buffer = storage->getBuffer(BUFFER_USAGE_VERTEX_OR_TRANSFORM_FEEDBACK);

            d3dBuffers[i] = buffer;
            d3dOffsets[i] = (mAppliedTFBuffers[i] != buffer) ? static_cast<UINT>(offsets[i]) : -1;
        }
        else
        {
            d3dBuffers[i] = NULL;
            d3dOffsets[i] = 0;
        }

        if (d3dBuffers[i] != mAppliedTFBuffers[i] || offsets[i] != mAppliedTFOffsets[i])
        {
            requiresUpdate = true;
        }
    }

    if (requiresUpdate)
    {
        mDeviceContext->SOSetTargets(ArraySize(d3dBuffers), d3dBuffers, d3dOffsets);
        for (size_t i = 0; i < gl::IMPLEMENTATION_MAX_TRANSFORM_FEEDBACK_BUFFERS; i++)
        {
            mAppliedTFBuffers[i] = d3dBuffers[i];
            mAppliedTFOffsets[i] = offsets[i];
        }
    }
}

gl::Error Renderer11::drawArrays(GLenum mode, GLsizei count, GLsizei instances, bool transformFeedbackActive)
{
    if (mode == GL_POINTS && transformFeedbackActive)
    {
        // Since point sprites are generated with a geometry shader, too many vertices will
        // be written if transform feedback is active.  To work around this, draw only the points
        // with the stream out shader and no pixel shader to feed the stream out buffers and then 
        // draw again with the point sprite geometry shader to rasterize the point sprites.

        mDeviceContext->PSSetShader(NULL, NULL, 0);

        if (instances > 0)
        {
            mDeviceContext->DrawInstanced(count, instances, 0, 0);
        }
        else
        {
            mDeviceContext->Draw(count, 0);
        }

        mDeviceContext->GSSetShader(mCurPointGeometryShader, NULL, 0);
        mDeviceContext->PSSetShader(mAppliedPixelShader, NULL, 0);

        if (instances > 0)
        {
            mDeviceContext->DrawInstanced(count, instances, 0, 0);
        }
        else
        {
            mDeviceContext->Draw(count, 0);
        }

        mDeviceContext->GSSetShader(mAppliedGeometryShader, NULL, 0);

        return gl::Error(GL_NO_ERROR);
    }
    else if (mode == GL_LINE_LOOP)
    {
        return drawLineLoop(count, GL_NONE, NULL, 0, NULL);
    }
    else if (mode == GL_TRIANGLE_FAN)
    {
        return drawTriangleFan(count, GL_NONE, NULL, 0, NULL, instances);
    }
    else if (instances > 0)
    {
        mDeviceContext->DrawInstanced(count, instances, 0, 0);
        return gl::Error(GL_NO_ERROR);
    }
    else
    {
        mDeviceContext->Draw(count, 0);
        return gl::Error(GL_NO_ERROR);
    }
}

gl::Error Renderer11::drawElements(GLenum mode, GLsizei count, GLenum type, const GLvoid *indices,
                                   gl::Buffer *elementArrayBuffer, const TranslatedIndexData &indexInfo, GLsizei instances)
{
    int minIndex = static_cast<int>(indexInfo.indexRange.start);

    if (mode == GL_LINE_LOOP)
    {
        return drawLineLoop(count, type, indices, minIndex, elementArrayBuffer);
    }
    else if (mode == GL_TRIANGLE_FAN)
    {
        return drawTriangleFan(count, type, indices, minIndex, elementArrayBuffer, instances);
    }
    else if (instances > 0)
    {
        mDeviceContext->DrawIndexedInstanced(count, instances, 0, -minIndex, 0);
        return gl::Error(GL_NO_ERROR);
    }
    else
    {
        mDeviceContext->DrawIndexed(count, 0, -minIndex);
        return gl::Error(GL_NO_ERROR);
    }
}

gl::Error Renderer11::drawLineLoop(GLsizei count, GLenum type, const GLvoid *indices, int minIndex, gl::Buffer *elementArrayBuffer)
{
    // Get the raw indices for an indexed draw
    if (type != GL_NONE && elementArrayBuffer)
    {
        gl::Buffer *indexBuffer = elementArrayBuffer;
        BufferImpl *storage = indexBuffer->getImplementation();
        intptr_t offset = reinterpret_cast<intptr_t>(indices);
        indices = static_cast<const GLubyte*>(storage->getData()) + offset;
    }

    if (!mLineLoopIB)
    {
        mLineLoopIB = new StreamingIndexBufferInterface(this);
        gl::Error error = mLineLoopIB->reserveBufferSpace(INITIAL_INDEX_BUFFER_SIZE, GL_UNSIGNED_INT);
        if (error.isError())
        {
            SafeDelete(mLineLoopIB);
            return error;
        }
    }

    // Checked by Renderer11::applyPrimitiveType
    ASSERT(count >= 0);

    if (static_cast<unsigned int>(count) + 1 > (std::numeric_limits<unsigned int>::max() / sizeof(unsigned int)))
    {
        return gl::Error(GL_OUT_OF_MEMORY, "Failed to create a 32-bit looping index buffer for GL_LINE_LOOP, too many indices required.");
    }

    const unsigned int spaceNeeded = (static_cast<unsigned int>(count) + 1) * sizeof(unsigned int);
    gl::Error error = mLineLoopIB->reserveBufferSpace(spaceNeeded, GL_UNSIGNED_INT);
    if (error.isError())
    {
        return error;
    }

    void* mappedMemory = NULL;
    unsigned int offset;
    error = mLineLoopIB->mapBuffer(spaceNeeded, &mappedMemory, &offset);
    if (error.isError())
    {
        return error;
    }

    unsigned int *data = reinterpret_cast<unsigned int*>(mappedMemory);
    unsigned int indexBufferOffset = offset;

    switch (type)
    {
      case GL_NONE:   // Non-indexed draw
        for (int i = 0; i < count; i++)
        {
            data[i] = i;
        }
        data[count] = 0;
        break;
      case GL_UNSIGNED_BYTE:
        for (int i = 0; i < count; i++)
        {
            data[i] = static_cast<const GLubyte*>(indices)[i];
        }
        data[count] = static_cast<const GLubyte*>(indices)[0];
        break;
      case GL_UNSIGNED_SHORT:
        for (int i = 0; i < count; i++)
        {
            data[i] = static_cast<const GLushort*>(indices)[i];
        }
        data[count] = static_cast<const GLushort*>(indices)[0];
        break;
      case GL_UNSIGNED_INT:
        for (int i = 0; i < count; i++)
        {
            data[i] = static_cast<const GLuint*>(indices)[i];
        }
        data[count] = static_cast<const GLuint*>(indices)[0];
        break;
      default: UNREACHABLE();
    }

    error = mLineLoopIB->unmapBuffer();
    if (error.isError())
    {
        return error;
    }

    IndexBuffer11 *indexBuffer = IndexBuffer11::makeIndexBuffer11(mLineLoopIB->getIndexBuffer());
    ID3D11Buffer *d3dIndexBuffer = indexBuffer->getBuffer();
    DXGI_FORMAT indexFormat = indexBuffer->getIndexFormat();

    if (mAppliedIB != d3dIndexBuffer || mAppliedIBFormat != indexFormat || mAppliedIBOffset != indexBufferOffset)
    {
        mDeviceContext->IASetIndexBuffer(d3dIndexBuffer, indexFormat, indexBufferOffset);
        mAppliedIB = d3dIndexBuffer;
        mAppliedIBFormat = indexFormat;
        mAppliedIBOffset = indexBufferOffset;
    }

    mDeviceContext->DrawIndexed(count + 1, 0, -minIndex);

    return gl::Error(GL_NO_ERROR);
}

gl::Error Renderer11::drawTriangleFan(GLsizei count, GLenum type, const GLvoid *indices, int minIndex, gl::Buffer *elementArrayBuffer, int instances)
{
    // Get the raw indices for an indexed draw
    if (type != GL_NONE && elementArrayBuffer)
    {
        gl::Buffer *indexBuffer = elementArrayBuffer;
        BufferImpl *storage = indexBuffer->getImplementation();
        intptr_t offset = reinterpret_cast<intptr_t>(indices);
        indices = static_cast<const GLubyte*>(storage->getData()) + offset;
    }

    if (!mTriangleFanIB)
    {
        mTriangleFanIB = new StreamingIndexBufferInterface(this);
        gl::Error error = mTriangleFanIB->reserveBufferSpace(INITIAL_INDEX_BUFFER_SIZE, GL_UNSIGNED_INT);
        if (error.isError())
        {
            SafeDelete(mTriangleFanIB);
            return error;
        }
    }

    // Checked by Renderer11::applyPrimitiveType
    ASSERT(count >= 3);

    const unsigned int numTris = count - 2;

    if (numTris > (std::numeric_limits<unsigned int>::max() / (sizeof(unsigned int) * 3)))
    {
        return gl::Error(GL_OUT_OF_MEMORY, "Failed to create a scratch index buffer for GL_TRIANGLE_FAN, too many indices required.");
    }

    const unsigned int spaceNeeded = (numTris * 3) * sizeof(unsigned int);
    gl::Error error = mTriangleFanIB->reserveBufferSpace(spaceNeeded, GL_UNSIGNED_INT);
    if (error.isError())
    {
        return error;
    }

    void* mappedMemory = NULL;
    unsigned int offset;
    error = mTriangleFanIB->mapBuffer(spaceNeeded, &mappedMemory, &offset);
    if (error.isError())
    {
        return error;
    }

    unsigned int *data = reinterpret_cast<unsigned int*>(mappedMemory);
    unsigned int indexBufferOffset = offset;

    switch (type)
    {
      case GL_NONE:   // Non-indexed draw
        for (unsigned int i = 0; i < numTris; i++)
        {
            data[i*3 + 0] = 0;
            data[i*3 + 1] = i + 1;
            data[i*3 + 2] = i + 2;
        }
        break;
      case GL_UNSIGNED_BYTE:
        for (unsigned int i = 0; i < numTris; i++)
        {
            data[i*3 + 0] = static_cast<const GLubyte*>(indices)[0];
            data[i*3 + 1] = static_cast<const GLubyte*>(indices)[i + 1];
            data[i*3 + 2] = static_cast<const GLubyte*>(indices)[i + 2];
        }
        break;
      case GL_UNSIGNED_SHORT:
        for (unsigned int i = 0; i < numTris; i++)
        {
            data[i*3 + 0] = static_cast<const GLushort*>(indices)[0];
            data[i*3 + 1] = static_cast<const GLushort*>(indices)[i + 1];
            data[i*3 + 2] = static_cast<const GLushort*>(indices)[i + 2];
        }
        break;
      case GL_UNSIGNED_INT:
        for (unsigned int i = 0; i < numTris; i++)
        {
            data[i*3 + 0] = static_cast<const GLuint*>(indices)[0];
            data[i*3 + 1] = static_cast<const GLuint*>(indices)[i + 1];
            data[i*3 + 2] = static_cast<const GLuint*>(indices)[i + 2];
        }
        break;
      default: UNREACHABLE();
    }

    error = mTriangleFanIB->unmapBuffer();
    if (error.isError())
    {
        return error;
    }

    IndexBuffer11 *indexBuffer = IndexBuffer11::makeIndexBuffer11(mTriangleFanIB->getIndexBuffer());
    ID3D11Buffer *d3dIndexBuffer = indexBuffer->getBuffer();
    DXGI_FORMAT indexFormat = indexBuffer->getIndexFormat();

    if (mAppliedIB != d3dIndexBuffer || mAppliedIBFormat != indexFormat || mAppliedIBOffset != indexBufferOffset)
    {
        mDeviceContext->IASetIndexBuffer(d3dIndexBuffer, indexFormat, indexBufferOffset);
        mAppliedIB = d3dIndexBuffer;
        mAppliedIBFormat = indexFormat;
        mAppliedIBOffset = indexBufferOffset;
    }

    if (instances > 0)
    {
        mDeviceContext->DrawIndexedInstanced(numTris * 3, instances, 0, -minIndex, 0);
    }
    else
    {
        mDeviceContext->DrawIndexed(numTris * 3, 0, -minIndex);
    }

    return gl::Error(GL_NO_ERROR);
}

gl::Error Renderer11::applyShaders(gl::ProgramBinary *programBinary, const gl::VertexFormat inputLayout[], const gl::Framebuffer *framebuffer,
                                   bool rasterizerDiscard, bool transformFeedbackActive)
{
    if (getRendererCaps().maxTransformFeedbackSeparateAttributes == 0)
    {
        ASSERT(!transformFeedbackActive);
    }

    ProgramD3D *programD3D = ProgramD3D::makeProgramD3D(programBinary->getImplementation());
    ShaderExecutable *vertexExe = programD3D->getVertexExecutableForInputLayout(inputLayout);
    ShaderExecutable *pixelExe = programD3D->getPixelExecutableForFramebuffer(framebuffer);
    ShaderExecutable *geometryExe = programD3D->getGeometryExecutable();

    ID3D11VertexShader *vertexShader = (vertexExe ? ShaderExecutable11::makeShaderExecutable11(vertexExe)->getVertexShader() : NULL);

    ID3D11PixelShader *pixelShader = NULL;
    // Skip pixel shader if we're doing rasterizer discard.
    if (!rasterizerDiscard)
    {
        pixelShader = (pixelExe ? ShaderExecutable11::makeShaderExecutable11(pixelExe)->getPixelShader() : NULL);
    }

    ID3D11GeometryShader *geometryShader = NULL;
    if (transformFeedbackActive)
    {
        geometryShader = (vertexExe ? ShaderExecutable11::makeShaderExecutable11(vertexExe)->getStreamOutShader() : NULL);
    }
    else if (mCurRasterState.pointDrawMode)
    {
        geometryShader = (geometryExe ? ShaderExecutable11::makeShaderExecutable11(geometryExe)->getGeometryShader() : NULL);
    }

    bool dirtyUniforms = false;

    if (vertexShader != mAppliedVertexShader)
    {
        mDeviceContext->VSSetShader(vertexShader, NULL, 0);
        mAppliedVertexShader = vertexShader;
        dirtyUniforms = true;
    }

    if (geometryShader != mAppliedGeometryShader)
    {
        mDeviceContext->GSSetShader(geometryShader, NULL, 0);
        mAppliedGeometryShader = geometryShader;
        dirtyUniforms = true;
    }

    if (geometryExe && mCurRasterState.pointDrawMode)
    {
        mCurPointGeometryShader = ShaderExecutable11::makeShaderExecutable11(geometryExe)->getGeometryShader();
    }
    else
    {
        mCurPointGeometryShader = NULL;
    }

    if (pixelShader != mAppliedPixelShader)
    {
        mDeviceContext->PSSetShader(pixelShader, NULL, 0);
        mAppliedPixelShader = pixelShader;
        dirtyUniforms = true;
    }

    if (dirtyUniforms)
    {
        programBinary->dirtyAllUniforms();
    }

    return gl::Error(GL_NO_ERROR);
}

gl::Error Renderer11::applyUniforms(const ProgramImpl &program, const std::vector<gl::LinkedUniform*> &uniformArray)
{
    unsigned int totalRegisterCountVS = 0;
    unsigned int totalRegisterCountPS = 0;

    bool vertexUniformsDirty = false;
    bool pixelUniformsDirty = false;

    for (size_t uniformIndex = 0; uniformIndex < uniformArray.size(); uniformIndex++)
    {
        const gl::LinkedUniform &uniform = *uniformArray[uniformIndex];

        if (uniform.isReferencedByVertexShader() && !uniform.isSampler())
        {
            totalRegisterCountVS += uniform.registerCount;
            vertexUniformsDirty = (vertexUniformsDirty || uniform.dirty);
        }

        if (uniform.isReferencedByFragmentShader() && !uniform.isSampler())
        {
            totalRegisterCountPS += uniform.registerCount;
            pixelUniformsDirty = (pixelUniformsDirty || uniform.dirty);
        }
    }

    const ProgramD3D *programD3D = ProgramD3D::makeProgramD3D(&program);
    const UniformStorage11 *vertexUniformStorage = UniformStorage11::makeUniformStorage11(&programD3D->getVertexUniformStorage());
    const UniformStorage11 *fragmentUniformStorage = UniformStorage11::makeUniformStorage11(&programD3D->getFragmentUniformStorage());
    ASSERT(vertexUniformStorage);
    ASSERT(fragmentUniformStorage);

    ID3D11Buffer *vertexConstantBuffer = vertexUniformStorage->getConstantBuffer();
    ID3D11Buffer *pixelConstantBuffer = fragmentUniformStorage->getConstantBuffer();

    float (*mapVS)[4] = NULL;
    float (*mapPS)[4] = NULL;

    if (totalRegisterCountVS > 0 && vertexUniformsDirty)
    {
        D3D11_MAPPED_SUBRESOURCE map = {0};
        HRESULT result = mDeviceContext->Map(vertexConstantBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &map);
        UNUSED_ASSERTION_VARIABLE(result);
        ASSERT(SUCCEEDED(result));
        mapVS = (float(*)[4])map.pData;
    }

    if (totalRegisterCountPS > 0 && pixelUniformsDirty)
    {
        D3D11_MAPPED_SUBRESOURCE map = {0};
        HRESULT result = mDeviceContext->Map(pixelConstantBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &map);
        UNUSED_ASSERTION_VARIABLE(result);
        ASSERT(SUCCEEDED(result));
        mapPS = (float(*)[4])map.pData;
    }

    for (size_t uniformIndex = 0; uniformIndex < uniformArray.size(); uniformIndex++)
    {
        gl::LinkedUniform *uniform = uniformArray[uniformIndex];

        if (!uniform->isSampler())
        {
            unsigned int componentCount = (4 - uniform->registerElement);

            // we assume that uniforms from structs are arranged in struct order in our uniforms list. otherwise we would
            // overwrite previously written regions of memory.

            if (uniform->isReferencedByVertexShader() && mapVS)
            {
                memcpy(&mapVS[uniform->vsRegisterIndex][uniform->registerElement], uniform->data, uniform->registerCount * sizeof(float) * componentCount);
            }

            if (uniform->isReferencedByFragmentShader() && mapPS)
            {
                memcpy(&mapPS[uniform->psRegisterIndex][uniform->registerElement], uniform->data, uniform->registerCount * sizeof(float) * componentCount);
            }
        }
    }

    if (mapVS)
    {
        mDeviceContext->Unmap(vertexConstantBuffer, 0);
    }

    if (mapPS)
    {
        mDeviceContext->Unmap(pixelConstantBuffer, 0);
    }

    if (mCurrentVertexConstantBuffer != vertexConstantBuffer)
    {
        mDeviceContext->VSSetConstantBuffers(0, 1, &vertexConstantBuffer);
        mCurrentVertexConstantBuffer = vertexConstantBuffer;
    }

    if (mCurrentPixelConstantBuffer != pixelConstantBuffer)
    {
        mDeviceContext->PSSetConstantBuffers(0, 1, &pixelConstantBuffer);
        mCurrentPixelConstantBuffer = pixelConstantBuffer;
    }

    // Driver uniforms
    if (!mDriverConstantBufferVS)
    {
        D3D11_BUFFER_DESC constantBufferDescription = {0};
        constantBufferDescription.ByteWidth = sizeof(dx_VertexConstants);
        constantBufferDescription.Usage = D3D11_USAGE_DEFAULT;
        constantBufferDescription.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        constantBufferDescription.CPUAccessFlags = 0;
        constantBufferDescription.MiscFlags = 0;
        constantBufferDescription.StructureByteStride = 0;

        HRESULT result = mDevice->CreateBuffer(&constantBufferDescription, NULL, &mDriverConstantBufferVS);
        UNUSED_ASSERTION_VARIABLE(result);
        ASSERT(SUCCEEDED(result));

        mDeviceContext->VSSetConstantBuffers(1, 1, &mDriverConstantBufferVS);
    }

    if (!mDriverConstantBufferPS)
    {
        D3D11_BUFFER_DESC constantBufferDescription = {0};
        constantBufferDescription.ByteWidth = sizeof(dx_PixelConstants);
        constantBufferDescription.Usage = D3D11_USAGE_DEFAULT;
        constantBufferDescription.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        constantBufferDescription.CPUAccessFlags = 0;
        constantBufferDescription.MiscFlags = 0;
        constantBufferDescription.StructureByteStride = 0;

        HRESULT result = mDevice->CreateBuffer(&constantBufferDescription, NULL, &mDriverConstantBufferPS);
        UNUSED_ASSERTION_VARIABLE(result);
        ASSERT(SUCCEEDED(result));

        mDeviceContext->PSSetConstantBuffers(1, 1, &mDriverConstantBufferPS);
    }

    if (memcmp(&mVertexConstants, &mAppliedVertexConstants, sizeof(dx_VertexConstants)) != 0)
    {
        mDeviceContext->UpdateSubresource(mDriverConstantBufferVS, 0, NULL, &mVertexConstants, 16, 0);
        memcpy(&mAppliedVertexConstants, &mVertexConstants, sizeof(dx_VertexConstants));
    }

    if (memcmp(&mPixelConstants, &mAppliedPixelConstants, sizeof(dx_PixelConstants)) != 0)
    {
        mDeviceContext->UpdateSubresource(mDriverConstantBufferPS, 0, NULL, &mPixelConstants, 16, 0);
        memcpy(&mAppliedPixelConstants, &mPixelConstants, sizeof(dx_PixelConstants));
    }

    // needed for the point sprite geometry shader
    if ((getRendererCaps().supportsGeometryShaders) && (mCurrentGeometryConstantBuffer != mDriverConstantBufferPS))
    {
        mDeviceContext->GSSetConstantBuffers(0, 1, &mDriverConstantBufferPS);
        mCurrentGeometryConstantBuffer = mDriverConstantBufferPS;
    }

    return gl::Error(GL_NO_ERROR);
}

gl::Error Renderer11::clear(const gl::ClearParameters &clearParams, gl::Framebuffer *frameBuffer)
{
    gl::Error error = mClear->clearFramebuffer(clearParams, frameBuffer);
    if (error.isError())
    {
        return error;
    }

    invalidateFramebufferSwizzles(frameBuffer);

    return gl::Error(GL_NO_ERROR);
}

void Renderer11::markAllStateDirty()
{
    for (unsigned int rtIndex = 0; rtIndex < gl::IMPLEMENTATION_MAX_DRAW_BUFFERS; rtIndex++)
    {
        mAppliedRenderTargetSerials[rtIndex] = 0;
    }
    mAppliedDepthbufferSerial = 0;
    mAppliedStencilbufferSerial = 0;
    mDepthStencilInitialized = false;
    mRenderTargetDescInitialized = false;

    ASSERT(mForceSetVertexSamplerStates.size() == mCurVertexSRVs.size());
    for (size_t vsamplerId = 0; vsamplerId < mForceSetVertexSamplerStates.size(); ++vsamplerId)
    {
        mForceSetVertexSamplerStates[vsamplerId] = true;
        mCurVertexSRVs[vsamplerId] = NULL;
    }

    ASSERT(mForceSetPixelSamplerStates.size() == mCurPixelSRVs.size());
    for (size_t fsamplerId = 0; fsamplerId < mForceSetPixelSamplerStates.size(); ++fsamplerId)
    {
        mForceSetPixelSamplerStates[fsamplerId] = true;
        mCurPixelSRVs[fsamplerId] = NULL;
    }

    mForceSetBlendState = true;
    mForceSetRasterState = true;
    mForceSetDepthStencilState = true;
    mForceSetScissor = true;
    mForceSetViewport = true;

#ifdef ANGLE_ENABLE_RENDER_TO_BACK_BUFFER
    mRenderingToBackBuffer = true;
#endif

    mAppliedIB = NULL;
    mAppliedIBFormat = DXGI_FORMAT_UNKNOWN;
    mAppliedIBOffset = 0;

    mAppliedVertexShader = NULL;
    mAppliedGeometryShader = NULL;
    mCurPointGeometryShader = NULL;
    mAppliedPixelShader = NULL;

    for (size_t i = 0; i < gl::IMPLEMENTATION_MAX_TRANSFORM_FEEDBACK_BUFFERS; i++)
    {
        mAppliedTFBuffers[i] = NULL;
        mAppliedTFOffsets[i] = 0;
    }

    memset(&mAppliedVertexConstants, 0, sizeof(dx_VertexConstants));
    memset(&mAppliedPixelConstants, 0, sizeof(dx_PixelConstants));

    mInputLayoutCache.markDirty();

    for (unsigned int i = 0; i < gl::IMPLEMENTATION_MAX_VERTEX_SHADER_UNIFORM_BUFFERS; i++)
    {
        mCurrentConstantBufferVS[i] = -1;
        mCurrentConstantBufferPS[i] = -1;
    }

    mCurrentVertexConstantBuffer = NULL;
    mCurrentPixelConstantBuffer = NULL;
    mCurrentGeometryConstantBuffer = NULL;

    mCurrentPrimitiveTopology = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
}

void Renderer11::releaseDeviceResources()
{
    mStateCache.clear();
    mInputLayoutCache.clear();

    SafeDelete(mVertexDataManager);
    SafeDelete(mIndexDataManager);
    SafeDelete(mLineLoopIB);
    SafeDelete(mTriangleFanIB);
    SafeDelete(mBlit);
    SafeDelete(mClear);
    SafeDelete(mPixelTransfer);

    SafeRelease(mDriverConstantBufferVS);
    SafeRelease(mDriverConstantBufferPS);
    SafeRelease(mSyncQuery);
}

void Renderer11::notifyDeviceLost()
{
    mDeviceLost = true;
    mDisplay->notifyDeviceLost();
}

bool Renderer11::isDeviceLost()
{
    return mDeviceLost;
}

// set notify to true to broadcast a message to all contexts of the device loss
bool Renderer11::testDeviceLost(bool notify)
{
    bool isLost = false;

    // GetRemovedReason is used to test if the device is removed
    HRESULT result = mDevice->GetDeviceRemovedReason();
    isLost = d3d11::isDeviceLostError(result);

    if (isLost)
    {
        // Log error if this is a new device lost event
        if (mDeviceLost == false)
        {
            ERR("The D3D11 device was removed: 0x%08X", result);
        }

        // ensure we note the device loss --
        // we'll probably get this done again by notifyDeviceLost
        // but best to remember it!
        // Note that we don't want to clear the device loss status here
        // -- this needs to be done by resetDevice
        mDeviceLost = true;
        if (notify)
        {
            notifyDeviceLost();
        }
    }

    return isLost;
}

bool Renderer11::testDeviceResettable()
{
    // determine if the device is resettable by creating a dummy device
    PFN_D3D11_CREATE_DEVICE D3D11CreateDevice = (PFN_D3D11_CREATE_DEVICE)GetProcAddress(mD3d11Module, "D3D11CreateDevice");

    if (D3D11CreateDevice == NULL)
    {
        return false;
    }

    D3D_FEATURE_LEVEL featureLevels[] =
    {
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
        D3D_FEATURE_LEVEL_9_3,
    };

    D3D_FEATURE_LEVEL onlyfeatureLevel9_3[] =
    {
        D3D_FEATURE_LEVEL_9_3,
    };

    UINT featureLevelCount = 0;
    D3D_FEATURE_LEVEL* featureLevelsRequested = featureLevels;
    if (mRequestedDisplay == EGL_PLATFORM_ANGLE_TYPE_D3D11_FL9_3_ANGLE || mRequestedDisplay == EGL_PLATFORM_ANGLE_TYPE_D3D11_FL9_3_WARP_ANGLE)
    {
        featureLevelsRequested = onlyfeatureLevel9_3;
        featureLevelCount = ArraySize(onlyfeatureLevel9_3);
    }
    else
    {
        featureLevelsRequested = featureLevels;
        featureLevelCount = ArraySize(featureLevels);
    }

    ID3D11Device* dummyDevice;
    D3D_FEATURE_LEVEL dummyFeatureLevel;
    ID3D11DeviceContext* dummyContext;
    unsigned int createFlags = 0;

    bool forceWarp = false;

    if (mRequestedDisplay == EGL_PLATFORM_ANGLE_TYPE_D3D11_WARP_ANGLE || mRequestedDisplay == EGL_PLATFORM_ANGLE_TYPE_D3D11_FL9_3_WARP_ANGLE)
    {
        forceWarp = true;
    }

#if defined(_DEBUG)
    createFlags = D3D11_CREATE_DEVICE_DEBUG;
#endif
    HRESULT result = d3d11::createD3D11DeviceWithWARPFallback(createFlags,
                                                              featureLevelsRequested,
                                                              featureLevelCount,
                                                              forceWarp,
                                                              &dummyDevice,
                                                              &dummyFeatureLevel,
                                                              &dummyContext);

    if (!mDevice || FAILED(result))
    {
        return false;
    }

    SafeRelease(dummyContext);
    SafeRelease(dummyDevice);

    return true;
}

void Renderer11::release()
{
    releaseShaderCompiler();
    releaseDeviceResources();

    SafeRelease(mDxgiFactory);
    SafeRelease(mDxgiAdapter);

    if (mDeviceContext)
    {
        mDeviceContext->ClearState();
        mDeviceContext->Flush();
        SafeRelease(mDeviceContext);
    }

    SafeRelease(mDevice);

    if (mD3d11Module)
    {
        FreeLibrary(mD3d11Module);
        mD3d11Module = NULL;
    }

    if (mDxgiModule)
    {
        FreeLibrary(mDxgiModule);
        mDxgiModule = NULL;
    }

    mCompiler.release();
}

bool Renderer11::resetDevice()
{
    // recreate everything
    release();
    EGLint result = initialize();

    if (result != EGL_SUCCESS)
    {
        ERR("Could not reinitialize D3D11 device: %08X", result);
        return false;
    }

    mDeviceLost = false;

    return true;
}

void Renderer11::trim()
{
    if (!mDevice)
    {
        return;
    }
#if defined (ANGLE_ENABLE_WINDOWS_STORE)
    // IDXGIDevice3 is only supported on Windows Phone 8.1 and above, and Windows 8.1 desktop/store applications.
    IDXGIDevice3 *dxgiDevice3 = NULL;
    HRESULT result = mDevice->QueryInterface(__uuidof(IDXGIDevice3), (void**)&dxgiDevice3);
    if (SUCCEEDED(result))
    {
        dxgiDevice3->Trim();
    }

    SafeRelease(dxgiDevice3);
#endif // defined (ANGLE_ENABLE_WINDOWS_STORE)
}

DWORD Renderer11::getAdapterVendor() const
{
    return mAdapterDescription.VendorId;
}

std::string Renderer11::getRendererDescription() const
{
    std::ostringstream rendererString;

    rendererString << mDescription;
    rendererString << " Direct3D11";

    rendererString << " vs_" << getMajorShaderModel() << "_" << getMinorShaderModel() << getShaderModelSuffix();
    rendererString << " ps_" << getMajorShaderModel() << "_" << getMinorShaderModel() << getShaderModelSuffix();

    return rendererString.str();
}

GUID Renderer11::getAdapterIdentifier() const
{
    // Use the adapter LUID as our adapter ID
    // This number is local to a machine is only guaranteed to be unique between restarts
    META_ASSERT(sizeof(LUID) <= sizeof(GUID));
    GUID adapterId = {0};
    memcpy(&adapterId, &mAdapterDescription.AdapterLuid, sizeof(LUID));
    return adapterId;
}

unsigned int Renderer11::getReservedVertexUniformVectors() const
{
    return 0;   // Driver uniforms are stored in a separate constant buffer
}

unsigned int Renderer11::getReservedFragmentUniformVectors() const
{
    return 0;   // Driver uniforms are stored in a separate constant buffer
}

unsigned int Renderer11::getReservedVertexUniformBuffers() const
{
    // we reserve one buffer for the application uniforms, and one for driver uniforms
    return 2;
}

unsigned int Renderer11::getReservedFragmentUniformBuffers() const
{
    // we reserve one buffer for the application uniforms, and one for driver uniforms
    return 2;
}

bool Renderer11::getShareHandleSupport() const
{
    // We only currently support share handles with BGRA surfaces, because
    // chrome needs BGRA. Once chrome fixes this, we should always support them.
    // PIX doesn't seem to support using share handles, so disable them.
    return getRendererExtensions().textureFormatBGRA8888 && !gl::perfActive();
}

bool Renderer11::getPostSubBufferSupport() const
{
    // D3D11 does not support present with dirty rectangles until D3D11.1 and DXGI 1.2.
    return false;
}

int Renderer11::getMajorShaderModel() const
{
    switch (mFeatureLevel)
    {
      case D3D_FEATURE_LEVEL_11_0: return D3D11_SHADER_MAJOR_VERSION;   // 5
      case D3D_FEATURE_LEVEL_10_1: return D3D10_1_SHADER_MAJOR_VERSION; // 4
      case D3D_FEATURE_LEVEL_10_0: return D3D10_SHADER_MAJOR_VERSION;   // 4
      case D3D_FEATURE_LEVEL_9_3:  return D3D10_SHADER_MAJOR_VERSION;   // 4
      default: UNREACHABLE();      return 0;
    }
}

int Renderer11::getMinorShaderModel() const
{
    switch (mFeatureLevel)
    {
      case D3D_FEATURE_LEVEL_11_0: return D3D11_SHADER_MINOR_VERSION;   // 0
      case D3D_FEATURE_LEVEL_10_1: return D3D10_1_SHADER_MINOR_VERSION; // 1
      case D3D_FEATURE_LEVEL_10_0: return D3D10_SHADER_MINOR_VERSION;   // 0
      case D3D_FEATURE_LEVEL_9_3:  return D3D10_SHADER_MINOR_VERSION;   // 0
      default: UNREACHABLE();      return 0;
    }
}

std::string Renderer11::getShaderModelSuffix() const
{
    switch (mFeatureLevel)
    {
      case D3D_FEATURE_LEVEL_11_0:
      case D3D_FEATURE_LEVEL_10_1:
      case D3D_FEATURE_LEVEL_10_0:
        return "";
      case D3D_FEATURE_LEVEL_9_3:
        return "_level_9_3";
      default: UNREACHABLE();
        return 0;
    }
}

int Renderer11::getMinSwapInterval() const
{
    return 0;
}

int Renderer11::getMaxSwapInterval() const
{
    return 4;
}

bool Renderer11::isRenderingToBackBuffer() const
{
#ifdef ANGLE_ENABLE_RENDER_TO_BACK_BUFFER
    return mRenderingToBackBuffer;
#else
    return false;
#endif
}

void Renderer11::setRenderToBackBufferVariables(bool renderingToBackBuffer)
{
#ifdef ANGLE_ENABLE_RENDER_TO_BACK_BUFFER
    mRenderingToBackBuffer = renderingToBackBuffer;

    // The rasterizer state must be updated, so that it will update its culling mode.
    mForceSetRasterState = true;

    mVertexConstants.viewScale[0] = 1.0f;
    mVertexConstants.viewScale[1] = 1.0f;
    mVertexConstants.viewScale[2] = 1.0f;
    mVertexConstants.viewScale[3] = 1.0f;

    mPixelConstants.viewScale[0] = 1.0f;
    mPixelConstants.viewScale[1] = 1.0f;
    mPixelConstants.viewScale[2] = 1.0f;
    mPixelConstants.viewScale[3] = 1.0f;

    // When rendering to a texture, invert the rendering by setting these constants to -1 instead of +1.
    if (!mRenderingToBackBuffer)
    {
        mVertexConstants.viewScale[1] = -1.0f;
        mPixelConstants.viewScale[1] = -1.0f;
    }
#else
    // This shouldn't be called if ANGLE_ENABLE_RENDER_TO_BACK_BUFFER isn't defined.
    UNIMPLEMENTED();
#endif
}

gl::Error Renderer11::copyImage2D(gl::Framebuffer *framebuffer, const gl::Rectangle &sourceRect, GLenum destFormat,
                                  GLint xoffset, GLint yoffset, TextureStorage *storage, GLint level)
{
    gl::FramebufferAttachment *colorbuffer = framebuffer->getReadColorbuffer();
    if (!colorbuffer)
    {
        return gl::Error(GL_OUT_OF_MEMORY, "Failed to retrieve the color buffer from the frame buffer.");
    }

    RenderTarget11 *sourceRenderTarget = d3d11::GetAttachmentRenderTarget(colorbuffer);
    if (!sourceRenderTarget)
    {
        return gl::Error(GL_OUT_OF_MEMORY, "Failed to retrieve the render target from the frame buffer.");
    }

    ID3D11ShaderResourceView *source = sourceRenderTarget->getShaderResourceView();
    if (!source)
    {
        return gl::Error(GL_OUT_OF_MEMORY, "Failed to retrieve the render target view from the render target.");
    }

    TextureStorage11_2D *storage11 = TextureStorage11_2D::makeTextureStorage11_2D(storage);
    if (!storage11)
    {
        return gl::Error(GL_OUT_OF_MEMORY, "Failed to retrieve the texture storage from the destination.");
    }

    gl::ImageIndex index = gl::ImageIndex::Make2D(level);
    RenderTarget11 *destRenderTarget = RenderTarget11::makeRenderTarget11(storage11->getRenderTarget(index));
    if (!destRenderTarget)
    {
        return gl::Error(GL_OUT_OF_MEMORY, "Failed to retrieve the render target from the destination storage.");
    }

    ID3D11RenderTargetView *dest = destRenderTarget->getRenderTargetView();
    if (!dest)
    {
        return gl::Error(GL_OUT_OF_MEMORY, "Failed to retrieve the render target view from the destination render target.");
    }

    gl::Box sourceArea(sourceRect.x, sourceRect.y, 0, sourceRect.width, sourceRect.height, 1);
    gl::Extents sourceSize(sourceRenderTarget->getWidth(), sourceRenderTarget->getHeight(), 1);

    gl::Box destArea(xoffset, yoffset, 0, sourceRect.width, sourceRect.height, 1);
    gl::Extents destSize(destRenderTarget->getWidth(), destRenderTarget->getHeight(), 1);

    // Use nearest filtering because source and destination are the same size for the direct
    // copy
    gl::Error error = mBlit->copyTexture(source, sourceArea, sourceSize, dest, destArea, destSize, NULL,
                                         destFormat, GL_NEAREST);
    if (error.isError())
    {
        return error;
    }

    storage11->invalidateSwizzleCacheLevel(level);

    return gl::Error(GL_NO_ERROR);
}

gl::Error Renderer11::copyImageCube(gl::Framebuffer *framebuffer, const gl::Rectangle &sourceRect, GLenum destFormat,
                                    GLint xoffset, GLint yoffset, TextureStorage *storage, GLenum target, GLint level)
{
    gl::FramebufferAttachment *colorbuffer = framebuffer->getReadColorbuffer();
    if (!colorbuffer)
    {
        return gl::Error(GL_OUT_OF_MEMORY, "Failed to retrieve the color buffer from the frame buffer.");
    }

    RenderTarget11 *sourceRenderTarget = d3d11::GetAttachmentRenderTarget(colorbuffer);
    if (!sourceRenderTarget)
    {
        return gl::Error(GL_OUT_OF_MEMORY, "Failed to retrieve the render target from the frame buffer.");
    }

    ID3D11ShaderResourceView *source = sourceRenderTarget->getShaderResourceView();
    if (!source)
    {
        return gl::Error(GL_OUT_OF_MEMORY, "Failed to retrieve the render target view from the render target.");
    }

    TextureStorage11_Cube *storage11 = TextureStorage11_Cube::makeTextureStorage11_Cube(storage);
    if (!storage11)
    {
        return gl::Error(GL_OUT_OF_MEMORY, "Failed to retrieve the texture storage from the destination.");;
    }

    gl::ImageIndex index = gl::ImageIndex::MakeCube(target, level);
    RenderTarget11 *destRenderTarget = RenderTarget11::makeRenderTarget11(storage11->getRenderTarget(index));
    if (!destRenderTarget)
    {
        return gl::Error(GL_OUT_OF_MEMORY, "Failed to retrieve the render target from the destination storage.");
    }

    ID3D11RenderTargetView *dest = destRenderTarget->getRenderTargetView();
    if (!dest)
    {
        return gl::Error(GL_OUT_OF_MEMORY, "Failed to retrieve the render target view from the destination render target.");
    }

    gl::Box sourceArea(sourceRect.x, sourceRect.y, 0, sourceRect.width, sourceRect.height, 1);
    gl::Extents sourceSize(sourceRenderTarget->getWidth(), sourceRenderTarget->getHeight(), 1);

    gl::Box destArea(xoffset, yoffset, 0, sourceRect.width, sourceRect.height, 1);
    gl::Extents destSize(destRenderTarget->getWidth(), destRenderTarget->getHeight(), 1);

    // Use nearest filtering because source and destination are the same size for the direct
    // copy
    gl::Error error = mBlit->copyTexture(source, sourceArea, sourceSize, dest, destArea, destSize, NULL,
                                         destFormat, GL_NEAREST);
    if (error.isError())
    {
        return error;
    }

    storage11->invalidateSwizzleCacheLevel(level);

    return gl::Error(GL_NO_ERROR);
}

gl::Error Renderer11::copyImage3D(gl::Framebuffer *framebuffer, const gl::Rectangle &sourceRect, GLenum destFormat,
                                  GLint xoffset, GLint yoffset, GLint zOffset, TextureStorage *storage, GLint level)
{
    gl::FramebufferAttachment *colorbuffer = framebuffer->getReadColorbuffer();
    if (!colorbuffer)
    {
        return gl::Error(GL_OUT_OF_MEMORY, "Failed to retrieve the color buffer from the frame buffer.");
    }

    RenderTarget11 *sourceRenderTarget = d3d11::GetAttachmentRenderTarget(colorbuffer);
    if (!sourceRenderTarget)
    {
        return gl::Error(GL_OUT_OF_MEMORY, "Failed to retrieve the render target from the frame buffer.");
    }

    ID3D11ShaderResourceView *source = sourceRenderTarget->getShaderResourceView();
    if (!source)
    {
        return gl::Error(GL_OUT_OF_MEMORY, "Failed to retrieve the render target view from the render target.");
    }

    TextureStorage11_3D *storage11 = TextureStorage11_3D::makeTextureStorage11_3D(storage);
    if (!storage11)
    {
        return gl::Error(GL_OUT_OF_MEMORY, "Failed to retrieve the texture storage from the destination.");
    }

    gl::ImageIndex index = gl::ImageIndex::Make3D(level, zOffset);
    RenderTarget11 *destRenderTarget = RenderTarget11::makeRenderTarget11(storage11->getRenderTarget(index));
    if (!destRenderTarget)
    {
        return gl::Error(GL_OUT_OF_MEMORY, "Failed to retrieve the render target from the destination storage.");
    }

    ID3D11RenderTargetView *dest = destRenderTarget->getRenderTargetView();
    if (!dest)
    {
        return gl::Error(GL_OUT_OF_MEMORY, "Failed to retrieve the render target view from the destination render target.");
    }

    gl::Box sourceArea(sourceRect.x, sourceRect.y, 0, sourceRect.width, sourceRect.height, 1);
    gl::Extents sourceSize(sourceRenderTarget->getWidth(), sourceRenderTarget->getHeight(), 1);

    gl::Box destArea(xoffset, yoffset, 0, sourceRect.width, sourceRect.height, 1);
    gl::Extents destSize(destRenderTarget->getWidth(), destRenderTarget->getHeight(), 1);

    // Use nearest filtering because source and destination are the same size for the direct
    // copy
    gl::Error error = mBlit->copyTexture(source, sourceArea, sourceSize, dest, destArea, destSize, NULL,
                                         destFormat, GL_NEAREST);
    if (error.isError())
    {
        return error;
    }

    storage11->invalidateSwizzleCacheLevel(level);

    return gl::Error(GL_NO_ERROR);
}

gl::Error Renderer11::copyImage2DArray(gl::Framebuffer *framebuffer, const gl::Rectangle &sourceRect, GLenum destFormat,
                                       GLint xoffset, GLint yoffset, GLint zOffset, TextureStorage *storage, GLint level)
{
    gl::FramebufferAttachment *colorbuffer = framebuffer->getReadColorbuffer();
    if (!colorbuffer)
    {
        return gl::Error(GL_OUT_OF_MEMORY, "Failed to retrieve the color buffer from the frame buffer.");
    }

    RenderTarget11 *sourceRenderTarget = d3d11::GetAttachmentRenderTarget(colorbuffer);
    if (!sourceRenderTarget)
    {
        return gl::Error(GL_OUT_OF_MEMORY, "Failed to retrieve the render target from the frame buffer.");
    }

    ID3D11ShaderResourceView *source = sourceRenderTarget->getShaderResourceView();
    if (!source)
    {
        return gl::Error(GL_OUT_OF_MEMORY, "Failed to retrieve the render target view from the render target.");
    }

    TextureStorage11_2DArray *storage11 = TextureStorage11_2DArray::makeTextureStorage11_2DArray(storage);
    if (!storage11)
    {
        SafeRelease(source);
        return gl::Error(GL_OUT_OF_MEMORY, "Failed to retrieve the texture storage from the destination.");
    }

    gl::ImageIndex index = gl::ImageIndex::Make2DArray(level, zOffset);
    RenderTarget11 *destRenderTarget = RenderTarget11::makeRenderTarget11(storage11->getRenderTarget(index));
    if (!destRenderTarget)
    {
        SafeRelease(source);
        return gl::Error(GL_OUT_OF_MEMORY, "Failed to retrieve the render target from the destination storage.");
    }

    ID3D11RenderTargetView *dest = destRenderTarget->getRenderTargetView();
    if (!dest)
    {
        return gl::Error(GL_OUT_OF_MEMORY, "Failed to retrieve the render target view from the destination render target.");
    }

    gl::Box sourceArea(sourceRect.x, sourceRect.y, 0, sourceRect.width, sourceRect.height, 1);
    gl::Extents sourceSize(sourceRenderTarget->getWidth(), sourceRenderTarget->getHeight(), 1);

    gl::Box destArea(xoffset, yoffset, 0, sourceRect.width, sourceRect.height, 1);
    gl::Extents destSize(destRenderTarget->getWidth(), destRenderTarget->getHeight(), 1);

    // Use nearest filtering because source and destination are the same size for the direct
    // copy
    gl::Error error = mBlit->copyTexture(source, sourceArea, sourceSize, dest, destArea, destSize, NULL,
                                         destFormat, GL_NEAREST);
    if (error.isError())
    {
        return error;
    }

    storage11->invalidateSwizzleCacheLevel(level);

    return gl::Error(GL_NO_ERROR);
}

void Renderer11::unapplyRenderTargets()
{
    setOneTimeRenderTarget(NULL);
}

void Renderer11::setOneTimeRenderTarget(ID3D11RenderTargetView *renderTargetView)
{
    ID3D11RenderTargetView *rtvArray[gl::IMPLEMENTATION_MAX_DRAW_BUFFERS] = {NULL};

    rtvArray[0] = renderTargetView;

    mDeviceContext->OMSetRenderTargets(getRendererCaps().maxDrawBuffers, rtvArray, NULL);

    // Do not preserve the serial for this one-time-use render target
    for (unsigned int rtIndex = 0; rtIndex < gl::IMPLEMENTATION_MAX_DRAW_BUFFERS; rtIndex++)
    {
        mAppliedRenderTargetSerials[rtIndex] = 0;
    }
}

RenderTarget *Renderer11::createRenderTarget(SwapChain *swapChain, bool depth)
{
    SwapChain11 *swapChain11 = SwapChain11::makeSwapChain11(swapChain);
    RenderTarget11 *renderTarget = NULL;

    if (depth)
    {
        // Note: depth stencil may be NULL for 0 sized surfaces
        renderTarget = new RenderTarget11(this, swapChain11->getDepthStencil(),
                                          swapChain11->getDepthStencilTexture(),
                                          swapChain11->getDepthStencilShaderResource(),
                                          swapChain11->getWidth(), swapChain11->getHeight(), 1);
    }
    else
    {
        // Note: render target may be NULL for 0 sized surfaces
        renderTarget = new RenderTarget11(this, swapChain11->getRenderTarget(),
                                          swapChain11->getTargetTexture(),
                                          swapChain11->getRenderTargetShaderResource(),
                                          swapChain11->getWidth(), swapChain11->getHeight(), 1);
    }
    return renderTarget;
}

RenderTarget *Renderer11::createRenderTarget(int width, int height, GLenum format, GLsizei samples)
{
    RenderTarget11 *renderTarget = new RenderTarget11(this, width, height, format, samples);
    return renderTarget;
}

ShaderImpl *Renderer11::createShader(GLenum type)
{
    return new ShaderD3D(type, this);
}

ProgramImpl *Renderer11::createProgram()
{
    return new ProgramD3D(this);
}

void Renderer11::releaseShaderCompiler()
{
    ShaderD3D::releaseCompiler();
}

ShaderExecutable *Renderer11::loadExecutable(const void *function, size_t length, rx::ShaderType type,
                                             const std::vector<gl::LinkedVarying> &transformFeedbackVaryings,
                                             bool separatedOutputBuffers)
{
    ShaderExecutable11 *executable = NULL;
    HRESULT result;

    switch (type)
    {
      case rx::SHADER_VERTEX:
        {
            ID3D11VertexShader *vertexShader = NULL;
            ID3D11GeometryShader *streamOutShader = NULL;

            result = mDevice->CreateVertexShader(function, length, NULL, &vertexShader);
            ASSERT(SUCCEEDED(result));

            if (transformFeedbackVaryings.size() > 0)
            {
                std::vector<D3D11_SO_DECLARATION_ENTRY> soDeclaration;
                for (size_t i = 0; i < transformFeedbackVaryings.size(); i++)
                {
                    const gl::LinkedVarying &varying = transformFeedbackVaryings[i];
                    GLenum transposedType = gl::TransposeMatrixType(varying.type);

                    for (size_t j = 0; j < varying.semanticIndexCount; j++)
                    {
                        D3D11_SO_DECLARATION_ENTRY entry = { 0 };
                        entry.Stream = 0;
                        entry.SemanticName = varying.semanticName.c_str();
                        entry.SemanticIndex = varying.semanticIndex + j;
                        entry.StartComponent = 0;
                        entry.ComponentCount = gl::VariableColumnCount(transposedType);
                        entry.OutputSlot = (separatedOutputBuffers ? i : 0);
                        soDeclaration.push_back(entry);
                    }
                }

                result = mDevice->CreateGeometryShaderWithStreamOutput(function, length, soDeclaration.data(), soDeclaration.size(),
                                                                       NULL, 0, 0, NULL, &streamOutShader);
                ASSERT(SUCCEEDED(result));
            }

            if (vertexShader)
            {
                executable = new ShaderExecutable11(function, length, vertexShader, streamOutShader);
            }
        }
        break;
      case rx::SHADER_PIXEL:
        {
            ID3D11PixelShader *pixelShader = NULL;

            result = mDevice->CreatePixelShader(function, length, NULL, &pixelShader);
            ASSERT(SUCCEEDED(result));

            if (pixelShader)
            {
                executable = new ShaderExecutable11(function, length, pixelShader);
            }
        }
        break;
      case rx::SHADER_GEOMETRY:
        {
            ID3D11GeometryShader *geometryShader = NULL;

            result = mDevice->CreateGeometryShader(function, length, NULL, &geometryShader);
            ASSERT(SUCCEEDED(result));

            if (geometryShader)
            {
                executable = new ShaderExecutable11(function, length, geometryShader);
            }
        }
        break;
      default:
        UNREACHABLE();
        break;
    }

    return executable;
}

ShaderExecutable *Renderer11::compileToExecutable(gl::InfoLog &infoLog, const std::string &shaderHLSL, rx::ShaderType type,
                                                  const std::vector<gl::LinkedVarying> &transformFeedbackVaryings,
                                                  bool separatedOutputBuffers, D3DWorkaroundType workaround)
{
    const char *profileType = NULL;
    switch (type)
    {
      case rx::SHADER_VERTEX:
        profileType = "vs";
        break;
      case rx::SHADER_PIXEL:
        profileType = "ps";
        break;
      case rx::SHADER_GEOMETRY:
        profileType = "gs";
        break;
      default:
        UNREACHABLE();
        return NULL;
    }

    std::string profile = FormatString("%s_%d_%d%s", profileType, getMajorShaderModel(), getMinorShaderModel(), getShaderModelSuffix().c_str());

    UINT flags = D3DCOMPILE_OPTIMIZATION_LEVEL0;

    if (gl::perfActive())
    {
#ifndef NDEBUG
        flags = D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

        flags |= D3DCOMPILE_DEBUG;
    }

    // Sometimes D3DCompile will fail with the default compilation flags for complicated shaders when it would otherwise pass with alternative options.
    // Try the default flags first and if compilation fails, try some alternatives.
    std::vector<CompileConfig> configs;
    configs.push_back(CompileConfig(flags,                                "default"          ));
    configs.push_back(CompileConfig(flags | D3DCOMPILE_SKIP_VALIDATION,   "skip validation"  ));
    configs.push_back(CompileConfig(flags | D3DCOMPILE_SKIP_OPTIMIZATION, "skip optimization"));

    ID3DBlob *binary = mCompiler.compileToBinary(infoLog, shaderHLSL, profile, configs);
    if (!binary)
    {
        return NULL;
    }

    ShaderExecutable *executable = loadExecutable(binary->GetBufferPointer(), binary->GetBufferSize(), type,
                                                  transformFeedbackVaryings, separatedOutputBuffers);
    SafeRelease(binary);

    return executable;
}

rx::UniformStorage *Renderer11::createUniformStorage(size_t storageSize)
{
    return new UniformStorage11(this, storageSize);
}

VertexBuffer *Renderer11::createVertexBuffer()
{
    return new VertexBuffer11(this);
}

IndexBuffer *Renderer11::createIndexBuffer()
{
    return new IndexBuffer11(this);
}

BufferImpl *Renderer11::createBuffer()
{
    return new Buffer11(this);
}

VertexArrayImpl *Renderer11::createVertexArray()
{
    return new VertexArray11(this);
}

QueryImpl *Renderer11::createQuery(GLenum type)
{
    return new Query11(this, type);
}

FenceImpl *Renderer11::createFence()
{
    return new Fence11(this);
}

TransformFeedbackImpl* Renderer11::createTransformFeedback()
{
    return new TransformFeedbackD3D();
}

bool Renderer11::supportsFastCopyBufferToTexture(GLenum internalFormat) const
{
    // D3D_FEATURE_LEVEL_9_X doesn't support FastCopyBufferToTexture since PixelTransfer uses
    // Geometry Shaders and int types in its PS/VS shaders.
    // TODO: Add limited support for FastCopyBufferToTexture on D3D_FEATURE_LEVEL_9_3.
    if (mFeatureLevel <= D3D_FEATURE_LEVEL_9_3)
    {
        return false;
    }

    ASSERT(getRendererExtensions().pixelBufferObject);

    const gl::InternalFormat &internalFormatInfo = gl::GetInternalFormatInfo(internalFormat);
    const d3d11::TextureFormat &d3d11FormatInfo = d3d11::GetTextureFormatInfo(internalFormat, mFeatureLevel);
    const d3d11::DXGIFormat &dxgiFormatInfo = d3d11::GetDXGIFormatInfo(d3d11FormatInfo.texFormat);

    // sRGB formats do not work with D3D11 buffer SRVs
    if (internalFormatInfo.colorEncoding == GL_SRGB)
    {
        return false;
    }

    // We cannot support direct copies to non-color-renderable formats
    if (d3d11FormatInfo.rtvFormat != DXGI_FORMAT_UNKNOWN)
    {
        return false;
    }

    // We skip all 3-channel formats since sometimes format support is missing
    if (internalFormatInfo.componentCount == 3)
    {
        return false;
    }

    // We don't support formats which we can't represent without conversion
    if (dxgiFormatInfo.internalFormat != internalFormat)
    {
        return false;
    }

    return true;
}

gl::Error Renderer11::fastCopyBufferToTexture(const gl::PixelUnpackState &unpack, unsigned int offset, RenderTarget *destRenderTarget,
                                              GLenum destinationFormat, GLenum sourcePixelsType, const gl::Box &destArea)
{
    ASSERT(supportsFastCopyBufferToTexture(destinationFormat));
    return mPixelTransfer->copyBufferToTexture(unpack, offset, destRenderTarget, destinationFormat, sourcePixelsType, destArea);
}

bool Renderer11::getRenderTargetResource(gl::FramebufferAttachment *colorbuffer, unsigned int *subresourceIndex, ID3D11Texture2D **resource)
{
    ASSERT(colorbuffer != NULL);

    RenderTarget11 *renderTarget = d3d11::GetAttachmentRenderTarget(colorbuffer);
    if (renderTarget)
    {
        *subresourceIndex = renderTarget->getSubresourceIndex();

        ID3D11RenderTargetView *colorBufferRTV = renderTarget->getRenderTargetView();
        if (colorBufferRTV)
        {
            ID3D11Resource *textureResource = NULL;
            colorBufferRTV->GetResource(&textureResource);

            if (textureResource)
            {
                HRESULT result = textureResource->QueryInterface(__uuidof(ID3D11Texture2D), (void**)resource);
                SafeRelease(textureResource);

                if (SUCCEEDED(result))
                {
                    return true;
                }
                else
                {
                    ERR("Failed to extract the ID3D11Texture2D from the render target resource, "
                        "HRESULT: 0x%X.", result);
                }
            }
        }
    }

    return false;
}

gl::Error Renderer11::blitRect(gl::Framebuffer *readTarget, const gl::Rectangle &readRect, gl::Framebuffer *drawTarget, const gl::Rectangle &drawRect,
                               const gl::Rectangle *scissor, bool blitRenderTarget, bool blitDepth, bool blitStencil, GLenum filter)
{
    if (blitRenderTarget)
    {
        gl::FramebufferAttachment *readBuffer = readTarget->getReadColorbuffer();
        if (!readBuffer)
        {
            return gl::Error(GL_OUT_OF_MEMORY, "Failed to retrieve the read buffer from the read framebuffer.");
        }

        RenderTarget *readRenderTarget = GetAttachmentRenderTarget(readBuffer);

        for (unsigned int colorAttachment = 0; colorAttachment < gl::IMPLEMENTATION_MAX_DRAW_BUFFERS; colorAttachment++)
        {
            if (drawTarget->isEnabledColorAttachment(colorAttachment))
            {
                gl::FramebufferAttachment *drawBuffer = drawTarget->getColorbuffer(colorAttachment);

                if (!drawBuffer)
                {
                    return gl::Error(GL_OUT_OF_MEMORY, "Failed to retrieve the draw buffer from the draw framebuffer.");
                }

                RenderTarget *drawRenderTarget = GetAttachmentRenderTarget(drawBuffer);

                gl::Error error = blitRenderbufferRect(readRect, drawRect, readRenderTarget, drawRenderTarget, filter,
                                                       scissor, blitRenderTarget, false, false);
                if (error.isError())
                {
                    return error;
                }
            }
        }
    }

    if (blitDepth || blitStencil)
    {
        gl::FramebufferAttachment *readBuffer = readTarget->getDepthOrStencilbuffer();
        if (!readBuffer)
        {
            return gl::Error(GL_OUT_OF_MEMORY, "Failed to retrieve the read depth-stencil buffer from the read framebuffer.");
        }

        gl::FramebufferAttachment *drawBuffer = drawTarget->getDepthOrStencilbuffer();
        if (!drawBuffer)
        {
            return gl::Error(GL_OUT_OF_MEMORY, "Failed to retrieve the draw depth-stencil buffer from the draw framebuffer.");
        }

        RenderTarget *readRenderTarget = GetAttachmentRenderTarget(readBuffer);
        RenderTarget *drawRenderTarget = GetAttachmentRenderTarget(drawBuffer);
        ASSERT(readRenderTarget && drawRenderTarget);

        gl::Error error = blitRenderbufferRect(readRect, drawRect, readRenderTarget, drawRenderTarget, filter, scissor,
                                               false, blitDepth, blitStencil);
        if (error.isError())
        {
            return error;
        }
    }

    invalidateFramebufferSwizzles(drawTarget);

    return gl::Error(GL_NO_ERROR);
}

gl::Error Renderer11::readPixels(gl::Framebuffer *framebuffer, GLint x, GLint y, GLsizei width, GLsizei height, GLenum format,
                                 GLenum type, GLuint outputPitch, const gl::PixelPackState &pack, uint8_t *pixels)
{
    ID3D11Texture2D *colorBufferTexture = NULL;
    unsigned int subresourceIndex = 0;

    gl::FramebufferAttachment *colorbuffer = framebuffer->getReadColorbuffer();

    if (colorbuffer && getRenderTargetResource(colorbuffer, &subresourceIndex, &colorBufferTexture))
    {
        gl::Rectangle area;
        area.x = x;
        area.y = y;
        area.width = width;
        area.height = height;

        gl::Buffer *packBuffer = pack.pixelBuffer.get();
        if (packBuffer != NULL)
        {
            rx::Buffer11 *packBufferStorage = Buffer11::makeBuffer11(packBuffer->getImplementation());
            PackPixelsParams packParams(area, format, type, outputPitch, pack, reinterpret_cast<ptrdiff_t>(pixels));

            gl::Error error = packBufferStorage->packPixels(colorBufferTexture, subresourceIndex, packParams);
            if (error.isError())
            {
                return error;
            }

            packBuffer->getIndexRangeCache()->clear();
        }
        else
        {
            gl::Error error = readTextureData(colorBufferTexture, subresourceIndex, area, format, type, outputPitch, pack, pixels);
            if (error.isError())
            {
                return error;
            }
        }

        SafeRelease(colorBufferTexture);
    }

    return gl::Error(GL_NO_ERROR);
}

Image *Renderer11::createImage()
{
    return new Image11(this);
}

void Renderer11::generateMipmap(Image *dest, Image *src)
{
    Image11 *dest11 = Image11::makeImage11(dest);
    Image11 *src11 = Image11::makeImage11(src);
    Image11::generateMipmap(dest11, src11);
}

TextureStorage *Renderer11::createTextureStorage2D(SwapChain *swapChain)
{
    SwapChain11 *swapChain11 = SwapChain11::makeSwapChain11(swapChain);
    return new TextureStorage11_2D(this, swapChain11);
}

TextureStorage *Renderer11::createTextureStorage2D(GLenum internalformat, bool renderTarget, GLsizei width, GLsizei height, int levels)
{
    return new TextureStorage11_2D(this, internalformat, renderTarget, width, height, levels);
}

TextureStorage *Renderer11::createTextureStorageCube(GLenum internalformat, bool renderTarget, int size, int levels)
{
    return new TextureStorage11_Cube(this, internalformat, renderTarget, size, levels);
}

TextureStorage *Renderer11::createTextureStorage3D(GLenum internalformat, bool renderTarget, GLsizei width, GLsizei height, GLsizei depth, int levels)
{
    return new TextureStorage11_3D(this, internalformat, renderTarget, width, height, depth, levels);
}

TextureStorage *Renderer11::createTextureStorage2DArray(GLenum internalformat, bool renderTarget, GLsizei width, GLsizei height, GLsizei depth, int levels)
{
    return new TextureStorage11_2DArray(this, internalformat, renderTarget, width, height, depth, levels);
}

TextureImpl *Renderer11::createTexture(GLenum target)
{
    switch(target)
    {
      case GL_TEXTURE_2D: return new TextureD3D_2D(this);
      case GL_TEXTURE_CUBE_MAP: return new TextureD3D_Cube(this);
      case GL_TEXTURE_3D: return new TextureD3D_3D(this);
      case GL_TEXTURE_2D_ARRAY: return new TextureD3D_2DArray(this);
      default:
        UNREACHABLE();
    }

    return NULL;
}

gl::Error Renderer11::readTextureData(ID3D11Texture2D *texture, unsigned int subResource, const gl::Rectangle &area, GLenum format,
                                      GLenum type, GLuint outputPitch, const gl::PixelPackState &pack, uint8_t *pixels)
{
    ASSERT(area.width >= 0);
    ASSERT(area.height >= 0);

    D3D11_TEXTURE2D_DESC textureDesc;
    texture->GetDesc(&textureDesc);

    // Clamp read region to the defined texture boundaries, preventing out of bounds reads
    // and reads of uninitialized data.
    gl::Rectangle safeArea;
    safeArea.x      = gl::clamp(area.x, 0, static_cast<int>(textureDesc.Width));
    safeArea.y      = gl::clamp(area.y, 0, static_cast<int>(textureDesc.Height));
    safeArea.width  = gl::clamp(area.width + std::min(area.x, 0), 0,
                                static_cast<int>(textureDesc.Width) - safeArea.x);
    safeArea.height = gl::clamp(area.height + std::min(area.y, 0), 0,
                                static_cast<int>(textureDesc.Height) - safeArea.y);

    ASSERT(safeArea.x >= 0 && safeArea.y >= 0);
    ASSERT(safeArea.x + safeArea.width  <= static_cast<int>(textureDesc.Width));
    ASSERT(safeArea.y + safeArea.height <= static_cast<int>(textureDesc.Height));

    if (safeArea.width == 0 || safeArea.height == 0)
    {
        // no work to do
        return gl::Error(GL_NO_ERROR);
    }

    D3D11_TEXTURE2D_DESC stagingDesc;
    stagingDesc.Width = safeArea.width;
    stagingDesc.Height = safeArea.height;
    stagingDesc.MipLevels = 1;
    stagingDesc.ArraySize = 1;
    stagingDesc.Format = textureDesc.Format;
    stagingDesc.SampleDesc.Count = 1;
    stagingDesc.SampleDesc.Quality = 0;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.BindFlags = 0;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    stagingDesc.MiscFlags = 0;

    ID3D11Texture2D* stagingTex = NULL;
    HRESULT result = mDevice->CreateTexture2D(&stagingDesc, NULL, &stagingTex);
    if (FAILED(result))
    {
        return gl::Error(GL_OUT_OF_MEMORY, "Failed to create internal staging texture for ReadPixels, HRESULT: 0x%X.", result);
    }

    ID3D11Texture2D* srcTex = NULL;
    if (textureDesc.SampleDesc.Count > 1)
    {
        D3D11_TEXTURE2D_DESC resolveDesc;
        resolveDesc.Width = textureDesc.Width;
        resolveDesc.Height = textureDesc.Height;
        resolveDesc.MipLevels = 1;
        resolveDesc.ArraySize = 1;
        resolveDesc.Format = textureDesc.Format;
        resolveDesc.SampleDesc.Count = 1;
        resolveDesc.SampleDesc.Quality = 0;
        resolveDesc.Usage = D3D11_USAGE_DEFAULT;
        resolveDesc.BindFlags = 0;
        resolveDesc.CPUAccessFlags = 0;
        resolveDesc.MiscFlags = 0;

        result = mDevice->CreateTexture2D(&resolveDesc, NULL, &srcTex);
        if (FAILED(result))
        {
            SafeRelease(stagingTex);
            return gl::Error(GL_OUT_OF_MEMORY, "Failed to create internal resolve texture for ReadPixels, HRESULT: 0x%X.", result);
        }

        mDeviceContext->ResolveSubresource(srcTex, 0, texture, subResource, textureDesc.Format);
        subResource = 0;
    }
    else
    {
        srcTex = texture;
        srcTex->AddRef();
    }

    D3D11_BOX srcBox;
    srcBox.left   = static_cast<UINT>(safeArea.x);
    srcBox.right  = static_cast<UINT>(safeArea.x + safeArea.width);
    srcBox.top    = static_cast<UINT>(safeArea.y);
    srcBox.bottom = static_cast<UINT>(safeArea.y + safeArea.height);
    srcBox.front  = 0;
    srcBox.back   = 1;

    mDeviceContext->CopySubresourceRegion(stagingTex, 0, 0, 0, 0, srcTex, subResource, &srcBox);

    SafeRelease(srcTex);

    PackPixelsParams packParams(safeArea, format, type, outputPitch, pack, 0);
    packPixels(stagingTex, packParams, pixels);

    SafeRelease(stagingTex);

    return gl::Error(GL_NO_ERROR);
}

void Renderer11::packPixels(ID3D11Texture2D *readTexture, const PackPixelsParams &params, uint8_t *pixelsOut)
{
    D3D11_TEXTURE2D_DESC textureDesc;
    readTexture->GetDesc(&textureDesc);

    D3D11_MAPPED_SUBRESOURCE mapping;
    HRESULT hr = mDeviceContext->Map(readTexture, 0, D3D11_MAP_READ, 0, &mapping);
    UNUSED_ASSERTION_VARIABLE(hr);
    ASSERT(SUCCEEDED(hr));

    uint8_t *source;
    int inputPitch;
    if (params.pack.reverseRowOrder)
    {
        source = static_cast<uint8_t*>(mapping.pData) + mapping.RowPitch * (params.area.height - 1);
        inputPitch = -static_cast<int>(mapping.RowPitch);
    }
    else
    {
        source = static_cast<uint8_t*>(mapping.pData);
        inputPitch = static_cast<int>(mapping.RowPitch);
    }

    const d3d11::DXGIFormat &dxgiFormatInfo = d3d11::GetDXGIFormatInfo(textureDesc.Format);
    const gl::InternalFormat &sourceFormatInfo = gl::GetInternalFormatInfo(dxgiFormatInfo.internalFormat);
    if (sourceFormatInfo.format == params.format && sourceFormatInfo.type == params.type)
    {
        uint8_t *dest = pixelsOut + params.offset;
        for (int y = 0; y < params.area.height; y++)
        {
            memcpy(dest + y * params.outputPitch, source + y * inputPitch, params.area.width * sourceFormatInfo.pixelBytes);
        }
    }
    else
    {
        const d3d11::DXGIFormat &sourceDXGIFormatInfo = d3d11::GetDXGIFormatInfo(textureDesc.Format);
        ColorCopyFunction fastCopyFunc = sourceDXGIFormatInfo.getFastCopyFunction(params.format, params.type);

        const gl::FormatType &destFormatTypeInfo = gl::GetFormatTypeInfo(params.format, params.type);
        const gl::InternalFormat &destFormatInfo = gl::GetInternalFormatInfo(destFormatTypeInfo.internalFormat);

        if (fastCopyFunc)
        {
            // Fast copy is possible through some special function
            for (int y = 0; y < params.area.height; y++)
            {
                for (int x = 0; x < params.area.width; x++)
                {
                    uint8_t *dest = pixelsOut + params.offset + y * params.outputPitch + x * destFormatInfo.pixelBytes;
                    const uint8_t *src = source + y * inputPitch + x * sourceFormatInfo.pixelBytes;

                    fastCopyFunc(src, dest);
                }
            }
        }
        else
        {
            uint8_t temp[16]; // Maximum size of any Color<T> type used.
            META_ASSERT(sizeof(temp) >= sizeof(gl::ColorF)  &&
                        sizeof(temp) >= sizeof(gl::ColorUI) &&
                        sizeof(temp) >= sizeof(gl::ColorI));

            for (int y = 0; y < params.area.height; y++)
            {
                for (int x = 0; x < params.area.width; x++)
                {
                    uint8_t *dest = pixelsOut + params.offset + y * params.outputPitch + x * destFormatInfo.pixelBytes;
                    const uint8_t *src = source + y * inputPitch + x * sourceFormatInfo.pixelBytes;

                    // readFunc and writeFunc will be using the same type of color, CopyTexImage
                    // will not allow the copy otherwise.
                    sourceDXGIFormatInfo.colorReadFunction(src, temp);
                    destFormatTypeInfo.colorWriteFunction(temp, dest);
                }
            }
        }
    }

    mDeviceContext->Unmap(readTexture, 0);
}

gl::Error Renderer11::blitRenderbufferRect(const gl::Rectangle &readRect, const gl::Rectangle &drawRect, RenderTarget *readRenderTarget,
                                           RenderTarget *drawRenderTarget, GLenum filter, const gl::Rectangle *scissor,
                                           bool colorBlit, bool depthBlit, bool stencilBlit)
{
    // Since blitRenderbufferRect is called for each render buffer that needs to be blitted,
    // it should never be the case that both color and depth/stencil need to be blitted at
    // at the same time.
    ASSERT(colorBlit != (depthBlit || stencilBlit));

    RenderTarget11 *drawRenderTarget11 = RenderTarget11::makeRenderTarget11(drawRenderTarget);
    if (!drawRenderTarget)
    {
        return gl::Error(GL_OUT_OF_MEMORY, "Failed to retrieve the internal draw render target from the draw framebuffer.");
    }

    ID3D11Resource *drawTexture = drawRenderTarget11->getTexture();
    unsigned int drawSubresource = drawRenderTarget11->getSubresourceIndex();
    ID3D11RenderTargetView *drawRTV = drawRenderTarget11->getRenderTargetView();
    ID3D11DepthStencilView *drawDSV = drawRenderTarget11->getDepthStencilView();

    RenderTarget11 *readRenderTarget11 = RenderTarget11::makeRenderTarget11(readRenderTarget);
    if (!readRenderTarget)
    {
        return gl::Error(GL_OUT_OF_MEMORY, "Failed to retrieve the internal read render target from the read framebuffer.");
    }

    ID3D11Resource *readTexture = NULL;
    ID3D11ShaderResourceView *readSRV = NULL;
    unsigned int readSubresource = 0;
    if (readRenderTarget->getSamples() > 0)
    {
        ID3D11Resource *unresolvedResource = readRenderTarget11->getTexture();
        ID3D11Texture2D *unresolvedTexture = d3d11::DynamicCastComObject<ID3D11Texture2D>(unresolvedResource);

        if (unresolvedTexture)
        {
            readTexture = resolveMultisampledTexture(unresolvedTexture, readRenderTarget11->getSubresourceIndex());
            readSubresource = 0;

            SafeRelease(unresolvedTexture);

            HRESULT hresult = mDevice->CreateShaderResourceView(readTexture, NULL, &readSRV);
            if (FAILED(hresult))
            {
                SafeRelease(readTexture);
                return gl::Error(GL_OUT_OF_MEMORY, "Failed to create shader resource view to resolve multisampled framebuffer.");
            }
        }
    }
    else
    {
        readTexture = readRenderTarget11->getTexture();
        readTexture->AddRef();
        readSubresource = readRenderTarget11->getSubresourceIndex();
        readSRV = readRenderTarget11->getShaderResourceView();
        readSRV->AddRef();
    }

    if (!readTexture || !readSRV)
    {
        SafeRelease(readTexture);
        SafeRelease(readSRV);
        return gl::Error(GL_OUT_OF_MEMORY, "Failed to retrieve the internal read render target view from the read render target.");
    }

    gl::Extents readSize(readRenderTarget->getWidth(), readRenderTarget->getHeight(), 1);
    gl::Extents drawSize(drawRenderTarget->getWidth(), drawRenderTarget->getHeight(), 1);

    bool scissorNeeded = scissor && gl::ClipRectangle(drawRect, *scissor, NULL);

    bool wholeBufferCopy = !scissorNeeded &&
                           readRect.x == 0 && readRect.width == readSize.width &&
                           readRect.y == 0 && readRect.height == readSize.height &&
                           drawRect.x == 0 && drawRect.width == drawSize.width &&
                           drawRect.y == 0 && drawRect.height == drawSize.height;

    bool stretchRequired = readRect.width != drawRect.width || readRect.height != drawRect.height;

    bool flipRequired = readRect.width < 0 || readRect.height < 0 || drawRect.width < 0 || drawRect.height < 0;

    bool outOfBounds = readRect.x < 0 || readRect.x + readRect.width > readSize.width ||
                       readRect.y < 0 || readRect.y + readRect.height > readSize.height ||
                       drawRect.x < 0 || drawRect.x + drawRect.width > drawSize.width ||
                       drawRect.y < 0 || drawRect.y + drawRect.height > drawSize.height;

    const gl::InternalFormat &actualFormatInfo = gl::GetInternalFormatInfo(drawRenderTarget->getActualFormat());
    bool partialDSBlit = (actualFormatInfo.depthBits > 0 && depthBlit) != (actualFormatInfo.stencilBits > 0 && stencilBlit);

    gl::Error result(GL_NO_ERROR);

    if (readRenderTarget11->getActualFormat() == drawRenderTarget->getActualFormat() &&
        !stretchRequired && !outOfBounds && !flipRequired && !partialDSBlit &&
        (!(depthBlit || stencilBlit) || wholeBufferCopy))
    {
        UINT dstX = drawRect.x;
        UINT dstY = drawRect.y;

        D3D11_BOX readBox;
        readBox.left = readRect.x;
        readBox.right = readRect.x + readRect.width;
        readBox.top = readRect.y;
        readBox.bottom = readRect.y + readRect.height;
        readBox.front = 0;
        readBox.back = 1;

        if (scissorNeeded)
        {
            // drawRect is guaranteed to have positive width and height because stretchRequired is false.
            ASSERT(drawRect.width >= 0 || drawRect.height >= 0);

            if (drawRect.x < scissor->x)
            {
                dstX = scissor->x;
                readBox.left += (scissor->x - drawRect.x);
            }
            if (drawRect.y < scissor->y)
            {
                dstY = scissor->y;
                readBox.top += (scissor->y - drawRect.y);
            }
            if (drawRect.x + drawRect.width > scissor->x + scissor->width)
            {
                readBox.right -= ((drawRect.x + drawRect.width) - (scissor->x + scissor->width));
            }
            if (drawRect.y + drawRect.height > scissor->y + scissor->height)
            {
                readBox.bottom -= ((drawRect.y + drawRect.height) - (scissor->y + scissor->height));
            }
        }

        // D3D11 needs depth-stencil CopySubresourceRegions to have a NULL pSrcBox
        // We also require complete framebuffer copies for depth-stencil blit.
        D3D11_BOX *pSrcBox = wholeBufferCopy ? NULL : &readBox;

        mDeviceContext->CopySubresourceRegion(drawTexture, drawSubresource, dstX, dstY, 0,
                                              readTexture, readSubresource, pSrcBox);
        result = gl::Error(GL_NO_ERROR);
    }
    else
    {
        gl::Box readArea(readRect.x, readRect.y, 0, readRect.width, readRect.height, 1);
        gl::Box drawArea(drawRect.x, drawRect.y, 0, drawRect.width, drawRect.height, 1);

        if (depthBlit && stencilBlit)
        {
            result = mBlit->copyDepthStencil(readTexture, readSubresource, readArea, readSize,
                                             drawTexture, drawSubresource, drawArea, drawSize,
                                             scissor);
        }
        else if (depthBlit)
        {
            result = mBlit->copyDepth(readSRV, readArea, readSize, drawDSV, drawArea, drawSize,
                                      scissor);
        }
        else if (stencilBlit)
        {
            result = mBlit->copyStencil(readTexture, readSubresource, readArea, readSize,
                                        drawTexture, drawSubresource, drawArea, drawSize,
                                        scissor);
        }
        else
        {
            GLenum format = gl::GetInternalFormatInfo(drawRenderTarget->getInternalFormat()).format;
            result = mBlit->copyTexture(readSRV, readArea, readSize, drawRTV, drawArea, drawSize,
                                        scissor, format, filter);
        }
    }

    SafeRelease(readTexture);
    SafeRelease(readSRV);

    return result;
}

ID3D11Texture2D *Renderer11::resolveMultisampledTexture(ID3D11Texture2D *source, unsigned int subresource)
{
    D3D11_TEXTURE2D_DESC textureDesc;
    source->GetDesc(&textureDesc);

    if (textureDesc.SampleDesc.Count > 1)
    {
        D3D11_TEXTURE2D_DESC resolveDesc;
        resolveDesc.Width = textureDesc.Width;
        resolveDesc.Height = textureDesc.Height;
        resolveDesc.MipLevels = 1;
        resolveDesc.ArraySize = 1;
        resolveDesc.Format = textureDesc.Format;
        resolveDesc.SampleDesc.Count = 1;
        resolveDesc.SampleDesc.Quality = 0;
        resolveDesc.Usage = textureDesc.Usage;
        resolveDesc.BindFlags = textureDesc.BindFlags;
        resolveDesc.CPUAccessFlags = 0;
        resolveDesc.MiscFlags = 0;

        ID3D11Texture2D *resolveTexture = NULL;
        HRESULT result = mDevice->CreateTexture2D(&resolveDesc, NULL, &resolveTexture);
        if (FAILED(result))
        {
            ERR("Failed to create a multisample resolve texture, HRESULT: 0x%X.", result);
            return NULL;
        }

        mDeviceContext->ResolveSubresource(resolveTexture, 0, source, subresource, textureDesc.Format);
        return resolveTexture;
    }
    else
    {
        source->AddRef();
        return source;
    }
}

void Renderer11::invalidateFBOAttachmentSwizzles(gl::FramebufferAttachment *attachment, int mipLevel)
{
    ASSERT(attachment->isTexture());
    gl::Texture *texture = attachment->getTexture();

    TextureStorage *texStorage = texture->getNativeTexture();
    if (texStorage)
    {
        TextureStorage11 *texStorage11 = TextureStorage11::makeTextureStorage11(texStorage);
        if (!texStorage11)
        {
            ERR("texture storage pointer unexpectedly null.");
            return;
        }

        texStorage11->invalidateSwizzleCacheLevel(mipLevel);
    }
}

void Renderer11::invalidateFramebufferSwizzles(gl::Framebuffer *framebuffer)
{
    for (unsigned int colorAttachment = 0; colorAttachment < gl::IMPLEMENTATION_MAX_DRAW_BUFFERS; colorAttachment++)
    {
        gl::FramebufferAttachment *attachment = framebuffer->getColorbuffer(colorAttachment);
        if (attachment && attachment->isTexture())
        {
            invalidateFBOAttachmentSwizzles(attachment, attachment->mipLevel());
        }
    }

    gl::FramebufferAttachment *depthAttachment = framebuffer->getDepthbuffer();
    if (depthAttachment && depthAttachment->isTexture())
    {
        invalidateFBOAttachmentSwizzles(depthAttachment, depthAttachment->mipLevel());
    }

    gl::FramebufferAttachment *stencilAttachment = framebuffer->getStencilbuffer();
    if (stencilAttachment && stencilAttachment->isTexture())
    {
        invalidateFBOAttachmentSwizzles(stencilAttachment, stencilAttachment->mipLevel());
    }
}

bool Renderer11::getLUID(LUID *adapterLuid) const
{
    adapterLuid->HighPart = 0;
    adapterLuid->LowPart = 0;

    if (!mDxgiAdapter)
    {
        return false;
    }

    DXGI_ADAPTER_DESC adapterDesc;
    if (FAILED(mDxgiAdapter->GetDesc(&adapterDesc)))
    {
        return false;
    }

    *adapterLuid = adapterDesc.AdapterLuid;
    return true;
}

rx::VertexConversionType Renderer11::getVertexConversionType(const gl::VertexFormat &vertexFormat) const
{
    return d3d11::GetVertexFormatInfo(vertexFormat, mFeatureLevel).conversionType;
}

GLenum Renderer11::getVertexComponentType(const gl::VertexFormat &vertexFormat) const
{
    return d3d11::GetDXGIFormatInfo(d3d11::GetVertexFormatInfo(vertexFormat, mFeatureLevel).nativeFormat).componentType;
}

void Renderer11::generateCaps(gl::Caps *outCaps, gl::TextureCapsMap *outTextureCaps, gl::Extensions *outExtensions) const
{
    d3d11_gl::GenerateCaps(mDevice, outCaps, outTextureCaps, outExtensions);
}

bool Renderer11::registerForRendererTrimRequest()
{
#if defined (ANGLE_ENABLE_WINDOWS_STORE)
    using namespace ABI::Windows::Foundation;
    using namespace ABI::Windows::ApplicationModel;
    using namespace ABI::Windows::ApplicationModel::Core;
    ComPtr<ICoreApplication> coreApplication;
    HRESULT result = GetActivationFactory(HStringReference(RuntimeClass_Windows_ApplicationModel_Core_CoreApplication).Get(), &coreApplication);
    if (SUCCEEDED(result))
    {
        auto suspendHandler = Callback<IEventHandler<SuspendingEventArgs*>>(
            [=](IInspectable*, ISuspendingEventArgs*) -> HRESULT
        {
            trim();
            return S_OK;
        });

        result = coreApplication->add_Suspending(suspendHandler.Get(), &mSuspendedEventToken);
    }

    if (FAILED(result))
    {
        return false;
    }
#endif // #if defined (ANGLE_ENABLE_WINDOWS_STORE)

    return true;
}

void Renderer11::unregisterForRendererTrimRequest()
{
#if defined (ANGLE_ENABLE_WINDOWS_STORE)
    using namespace ABI::Windows::Foundation;
    using namespace ABI::Windows::ApplicationModel;
    using namespace ABI::Windows::ApplicationModel::Core;
    // Unregister the application suspending event because the
    // renderer attached to this display is being destroyed.
    if (mSuspendedEventToken.value != 0)
    {
        ComPtr<ICoreApplication> coreApplication;
        HRESULT result = GetActivationFactory(HStringReference(RuntimeClass_Windows_ApplicationModel_Core_CoreApplication).Get(), &coreApplication);
        if (SUCCEEDED(result))
        {
            coreApplication->remove_Suspending(mSuspendedEventToken);
        }
        mSuspendedEventToken.value = 0;
    }
#endif // #if defined (ANGLE_ENABLE_WINDOWS_STORE)
}

Workarounds Renderer11::generateWorkarounds() const
{
    return d3d11::GenerateWorkarounds();
}

}
