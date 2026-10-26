/**
 * @file    gpu/d3d/hooks/state.cpp
 * @brief   N&B's D3D state calls, hooked with the native ports (d3d_native.h).
 *
 * The render and sampler state setters are the ones N&B's state tables name
 * (D3D::InitializeApiState puts them into every device's dispatch table,
 * which the game's inline SetRenderState / SetSamplerState call through).
 * The rest are D3DDevice_Set* and the CDevice internals that write the ring.
 */
#include <cstring>

#include <rex/hook.h>

#include "renut_engine/gpu/banjo_renut_select.h"
#include <rex/types.h>

#include "renut_engine/gpu/d3d/d3d_native.h"
#include "renut_engine/gpu/guest/guest_memory.h"

using namespace renut::gpu;

namespace {

template <u32 State>
void SetRenderStateHook(u32 device, u32 value) {
  native::SetRenderState(native::DeviceAt(device), State, value);
}

template <u32 State>
void SetSamplerStateHook(u32 device, u32 sampler, u32 value) {
  native::SetSamplerState(native::DeviceAt(device), sampler, State, value);
}

template <u32 State>
u32 GetRenderStateHook(u32 device) {
  return native::GetRenderState(native::DeviceAt(device), State);
}

// zuma-nx (2026-10-26): XDK 2135's render state 57 (between ColorWriteEnable3 and TessellationMode), which N&B's
// order does not have: its setter (sub_8212C260) stores the value in four device words (+12124 to +12136), its getter
// (sub_8212C278) reads the first. Renut's device has its own fields there, so the value is kept here instead; nothing
// in Renut's renderer depends on it.
// zuma-nx (2026-10-26): the game's textures are XDK 2135 objects (hooks/resource.cpp, "Zuma's textures"): Renut's
// SetTexture gets the texture's shadow header, which carries the same fetch constant at N&B's place.
void ZumaSetTexture(u32 device, u32 sampler, u32 texture) {
  native::SetTexture(native::DeviceAt(device), sampler, native::ZumaTextureShadow(texture));
}

u32 g_zuma_render_state57 = 0;
void ZumaSetRenderState57(u32 /*device*/, u32 value) {
  g_zuma_render_state57 = value;
}
u32 ZumaGetRenderState57(u32 /*device*/) {
  return g_zuma_render_state57;
}

template <u32 State>
u32 GetSamplerStateHook(u32 device, u32 sampler) {
  return native::GetSamplerState(native::DeviceAt(device), sampler, State);
}

// The device's AddRef of a resource it hands out (D3DResource_AddRef,
// resource.cpp).
u32 AddRefResource(u32 resource) {
  return native::AddRefResource(resource);
}

// D3DSurface *GetRenderTarget(D3DDevice*, DWORD Index) [0x8264D8D0]
u32 D3DDevice_GetRenderTarget_hook(u32 device, u32 index) {
  const native::Device d = native::DeviceAt(device);
  const u32 surface = d && index < 4 ? d.W(native::off::RenderTargets + index * 4) : 0;
  if (surface)
    AddRefResource(surface);
  return surface;
}

// D3DSurface *GetDepthStencilSurface(D3DDevice*) [0x8264D910]
u32 D3DDevice_GetDepthStencilSurface_hook(u32 device) {
  const native::Device d = native::DeviceAt(device);
  const u32 surface = d ? d.W(native::off::DepthStencil) : 0;
  if (surface)
    AddRefResource(surface);
  return surface;
}

// D3DVertexBuffer *GetStreamSource(D3DDevice*, UINT StreamNumber,
//     UINT *pOffsetInBytes, UINT *pStride) [0x8264D840]: the offset is the
// stream's fetch constant address less the buffer's.
u32 D3DDevice_GetStreamSource_hook(u32 device, u32 stream, mapped_u32 offset, mapped_u32 stride) {
  const native::Device d = native::DeviceAt(device);
  if (!d || stream >= 16)
    return 0;
  const u32 buffer = d.W(native::off::StreamSources + stream * 4);
  u32 bytes = 0;
  if (buffer) {
    const u32 fetch = d.W(native::off::TextureFetch + (95 - stream) * 8);
    bytes = fetch - native::GpuAddress(GuestPtr<const be_u32>(buffer)[6]);
    AddRefResource(buffer);
  }
  if (offset)
    *offset = bytes;
  if (stride)
    *stride = u32(d.B(native::off::StreamStrides + stream)) << 2;
  return buffer;
}

// void D3DDevice_GetViewport(D3DDevice*, D3DVIEWPORT9*) [0x82276AC0]
void D3DDevice_GetViewport_hook(u32 device, mapped_u32 viewport) {
  const native::Device d = native::DeviceAt(device);
  if (!d || !viewport)
    return;
  for (u32 i = 0; i < 4; ++i)  // fctidz, the low word
    viewport[i] = u32(native::TruncateToI64(d.F(0x32C8 + i * 4)));
  viewport[4] = d.W(0x32D8);
  viewport[5] = d.W(0x32DC);
}

// void D3DDevice_SetClipPlane(D3DDevice*, DWORD Index, const float *pPlane) [0x8264DAE8]
void D3DDevice_SetClipPlane_hook(u32 device, u32 index, mapped_u32 plane) {
  native::Device d = native::DeviceAt(device);
  if (!d || !plane || index >= 6)
    return;
  for (u32 i = 0; i < 4; ++i)
    d.SetW(0x2820 + index * 16 + i * 4, plane[i]);
  d.Dirty(4, u64(1) << (54 - index));
}

// void D3DDevice_SetVertexFetchConstant(D3DDevice*, UINT VertexFetchRegister,
//     D3DVertexBuffer*, UINT Offset) [0x8264DB38]: a vertex fetch slot of the
// 3 in a fetch constant, made a vertex fetch constant first.
void D3DDevice_SetVertexFetchConstant_hook(u32 device, u32 reg, u32 vertex_buffer, u32 offset) {
  native::Device d = native::DeviceAt(device);
  const auto *vb = vertex_buffer ? GuestPtr<const be_u32>(vertex_buffer) : nullptr;
  if (!d || !vb || reg > 0x5F)
    return;
  const u32 slot = (0x5F - reg) / 3;
  const u32 part = (0x5F - reg) - slot * 3;
  const u32 constant = native::off::TextureFetch + slot * 24;
  const u32 type = d.W(constant) & 3;
  if (type == 0 || type == 2)
    std::memset(d.bytes + constant, 0, 24);
  d.SetW(constant, native::Insert(d.W(constant), 1, 0, 2));
  d.Dirty(3, u64(1) << (31 - slot));
  d.SetW(constant + part * 8, native::GpuAddress(u32(vb[6]) + offset));
  d.SetW(constant + part * 8 + 4, native::Insert(u32(vb[7]) - offset, 1, 28, 2));
}

// The boolean and integer shader constants (D3DDevice_Set{Vertex,Pixel}
// ShaderConstant{B,I}): bits from 0x2780 (vertex) / 0x2790 (pixel), a word
// of x, y, z bytes each from 0x27A0 / 0x27E0.
void SetShaderConstantsB(u32 device, u32 base, u32 start, mapped_u32 data, u32 count) {
  native::Device d = native::DeviceAt(device);
  if (!d || !data)
    return;
  for (u32 i = 0; i < count; ++i, ++start) {
    const u32 word = base + (start >> 5) * 4;
    const u32 bit = 1u << (start & 31);
    d.SetW(word, (d.W(word) & ~bit) | ((u32(data[i]) & 1) << (start & 31)));
  }
  d.Dirty(4, u64(1) << 56);
}

void SetShaderConstantsI(u32 device, u32 base, u32 start, mapped_u32 data, u32 count) {
  native::Device d = native::DeviceAt(device);
  if (!d || !data)
    return;
  for (u32 i = 0; i < count; ++i) {
    const u32 x = u32(data[i * 4]) & 0xFF, y = u32(data[i * 4 + 1]) & 0xFF;
    const u32 z = u32(data[i * 4 + 2]) & 0xFF;
    d.SetW(base + (start + i) * 4, (z << 16) | (y << 8) | x);
  }
  d.Dirty(4, u64(1) << 56);
}

// void D3DDevice_SetVertexShaderConstantB(D3DDevice*, UINT StartRegister,
//     const BOOL*, UINT BoolCount) [0x82221930], and the others alike.
void D3DDevice_SetVertexShaderConstantB_hook(u32 device, u32 start, mapped_u32 data, u32 count) {
  SetShaderConstantsB(device, 0x2780, start, data, count);
}
void D3DDevice_SetPixelShaderConstantB_hook(u32 device, u32 start, mapped_u32 data, u32 count) {
  SetShaderConstantsB(device, 0x2790, start, data, count);
}
void D3DDevice_SetVertexShaderConstantI_hook(u32 device, u32 start, mapped_u32 data, u32 count) {
  SetShaderConstantsI(device, 0x27A0, start, data, count);
}
void D3DDevice_SetPixelShaderConstantI_hook(u32 device, u32 start, mapped_u32 data, u32 count) {
  SetShaderConstantsI(device, 0x27E0, start, data, count);
}

// void D3DDevice_EndIndexedVertices(D3DDevice*) [0x826561F8]: the inline end
// of BeginIndexedVertices, back to the ring pointer it saved.
void D3DDevice_EndIndexedVertices_hook(u32 device) {
  native::Device d = native::DeviceAt(device);
  if (d)
    d.SetW(native::off::Ring, d.W(0x3604));
}

// void D3DDevice_SetTexture(D3DDevice*, DWORD Sampler, D3DBaseTexture*,
//     UINT64 PendingMask3) [0x8222F410]; the mask is the sampler's fetch
// constant bit, which the port derives from Sampler.
void D3DDevice_SetTexture_hook(u32 device, u32 sampler, u32 texture) {
  native::SetTexture(native::DeviceAt(device), sampler, texture);
}

// void D3DDevice_SetStreamSource(D3DDevice*, UINT StreamNumber, D3DVertexBuffer*,
//     UINT OffsetInBytes, UINT Stride, UINT64 PendingMask3) [0x8222A408]
void D3DDevice_SetStreamSource_hook(u32 device, u32 stream, u32 vertex_buffer, u32 offset,
                                    u32 stride) {
  native::SetStreamSource(native::DeviceAt(device), stream, vertex_buffer, offset, stride);
}

// void D3DDevice_SetIndices(D3DDevice*, D3DIndexBuffer*) [0x8220EE38]
void D3DDevice_SetIndices_hook(u32 device, u32 index_buffer) {
  native::SetIndices(native::DeviceAt(device), index_buffer);
}

// void D3DDevice_SetVertexDeclaration(D3DDevice*, D3DVertexDeclaration*) [0x82205700]
void D3DDevice_SetVertexDeclaration_hook(u32 device, u32 declaration) {
  native::SetVertexDeclaration(native::DeviceAt(device), declaration);
}

// void D3DDevice_SetViewport(D3DDevice*, const D3DVIEWPORT9*) [0x82222EA0]
void D3DDevice_SetViewport_hook(u32 device, u32 viewport) {
  native::SetViewport(native::DeviceAt(device), viewport);
}

// void D3D::SetViewport(D3DDevice*, float X, float Y, float Width, float Height,
//     float MinZ, float MaxZ) [0x82222F20]
void D3D_SetViewport_hook(u32 device, float x, float y, float width, float height, float min_z,
                          float max_z) {
  native::SetViewport(native::DeviceAt(device), x, y, width, height, min_z, max_z);
}

// void D3DDevice_SetScissorRect(D3DDevice*, const RECT*) [0x82223178]
void D3DDevice_SetScissorRect_hook(u32 device, mapped_u32 rect) {
  if (!rect)
    return;
  native::SetScissorRect(native::DeviceAt(device), i32(u32(rect[0])), i32(u32(rect[1])),
                         i32(u32(rect[2])), i32(u32(rect[3])));
}

// void D3D::SetSurfaceClip(CDevice*, int, int, int, int) [0x82223270]: the
// window scissor and per-tile clip packets, nothing else.
void D3D_SetSurfaceClip_hook(u32 /*device*/, u32, u32, u32, u32) {}

// void D3DDevice_SetRenderTarget(D3DDevice*, DWORD Index, D3DSurface*) [0x8227AB68]
void D3DDevice_SetRenderTarget_hook(u32 device, u32 index, u32 surface) {
  native::SetRenderTarget(native::DeviceAt(device), index, surface);
}

// void D3DDevice_SetDepthStencilSurface(D3DDevice*, D3DSurface*) [0x8227AF40]
void D3DDevice_SetDepthStencilSurface_hook(u32 device, u32 surface) {
  native::SetDepthStencilSurface(native::DeviceAt(device), surface);
}

// void D3D::SetSurfaceInfo(CDevice*, D3DSurface*) [0x8227AEB0]
void D3D_SetSurfaceInfo_hook(u32 device, u32 surface) {
  native::SetSurfaceInfo(native::DeviceAt(device), surface);
}

// void D3DDevice_SetPredication(D3DDevice*, DWORD PredicationMask) [0x8222AA18]
void D3DDevice_SetPredication_hook(u32 device, u32 mask) {
  native::SetPredication(native::DeviceAt(device), mask);
}

// void D3DDevice_SetCommandBufferPredication(D3DDevice*, DWORD TilePredication,
//     DWORD RunPredication) [0x82658678]
void D3DDevice_SetCommandBufferPredication_hook(u32 device, u32 tile_predication,
                                                u32 run_predication) {
  native::SetCommandBufferPredication(native::DeviceAt(device), tile_predication,
                                      run_predication);
}

// void D3DDevice_SetShaderGPRAllocation(D3DDevice*, DWORD Flags,
//     DWORD VertexShaderCount, DWORD PixelShaderCount) [0x82220B20]
void D3DDevice_SetShaderGPRAllocation_hook(u32 device, u32 flags, u32 vertex_count,
                                           u32 pixel_count) {
  native::SetShaderGPRAllocation(native::DeviceAt(device), flags, vertex_count, pixel_count);
}

// void D3D::CleanupGprAllocationSetup(CDevice*) [0x8264E2C0]
void D3D_CleanupGprAllocationSetup_hook(u32 device) {
  native::CleanupGprAllocationSetup(native::DeviceAt(device));
}

// void D3DDevice_SetScreenExtentQueryMode(D3DDevice*, D3DSCREENEXTENTQUERYMODE)
// [0x82658988]
void D3DDevice_SetScreenExtentQueryMode_hook(u32 device, u32 mode) {
  native::SetScreenExtentQueryMode(native::DeviceAt(device), mode);
}

// void D3DDevice_UnsetAll(D3DDevice*) [0x8264E1A8]
void D3DDevice_UnsetAll_hook(u32 device) {
  native::UnsetAll(native::DeviceAt(device));
}

// void D3D::ResetAllState(CDevice*, BOOL) [0x82287D38]
void D3D_ResetAllState_hook(u32 device, u32 /*skip_packets*/) {
  native::ResetAllState(native::DeviceAt(device));
}

// void D3D::InvalidateConstants(D3DDevice*) [0x8264E128]
void D3D_InvalidateConstants_hook(u32 device) {
  native::InvalidateConstants(native::DeviceAt(device));
}

// void D3DDevice_BlockUntilIdle(D3DDevice*) [0x82657488] (IDA: the second
// D3D::InvalidateConstants): waits for the device's current fence (N&B calls
// it before releasing a device).
void D3DDevice_BlockUntilIdle_hook(u32 device) {
  native::InvalidateConstantsAndWait(native::DeviceAt(device));
}

} // namespace

// Render states, by their index in N&B's table (off_82E506F0).
BANJO_RENUT_HOOK(sub_8212B638, SetRenderStateHook<10>);  // D3DDevice_SetRenderState_ZEnable  // zuma-nx: the game's state table (Feeding Frenzy 8212B638)
BANJO_RENUT_HOOK(sub_8212B6C0, SetRenderStateHook<11>);  // D3DDevice_SetRenderState_ZFunc  // zuma-nx: the game's state table (Feeding Frenzy 8212B6C0)
BANJO_RENUT_HOOK(sub_8212B688, SetRenderStateHook<12>);  // D3DDevice_SetRenderState_ZWriteEnable  // zuma-nx: the game's state table (Feeding Frenzy 8212B688)
BANJO_RENUT_HOOK(sub_8212AF80, SetRenderStateHook<13>);  // D3DDevice_SetRenderState_FillMode  // zuma-nx: the game's state table (Feeding Frenzy 8212AF80)
BANJO_RENUT_HOOK(sub_8212AF48, SetRenderStateHook<14>);  // D3DDevice_SetRenderState_CullMode  // zuma-nx: the game's state table (Feeding Frenzy 8212AF48)
BANJO_RENUT_HOOK(sub_8212B000, SetRenderStateHook<15>);  // D3DDevice_SetRenderState_AlphaBlendEnable  // zuma-nx: the game's state table (Feeding Frenzy 8212B000)
BANJO_RENUT_HOOK(sub_8212B3D0, SetRenderStateHook<16>);  // D3DDevice_SetRenderState_SeparateAlphaBlendEnable  // zuma-nx: the game's state table (Feeding Frenzy 8212B3D0)
BANJO_RENUT_HOOK(sub_8212B518, SetRenderStateHook<17>);  // D3DDevice_SetRenderState_BlendFactor  // zuma-nx: the game's state table (Feeding Frenzy 8212B518)
BANJO_RENUT_HOOK(sub_8212B138, SetRenderStateHook<18>);  // D3DDevice_SetRenderState_SrcBlend  // zuma-nx: the game's state table (Feeding Frenzy 8212B138)
BANJO_RENUT_HOOK(sub_8212B1D0, SetRenderStateHook<19>);  // D3DDevice_SetRenderState_DestBlend  // zuma-nx: the game's state table (Feeding Frenzy 8212B1D0)
BANJO_RENUT_HOOK(sub_8212B0A0, SetRenderStateHook<20>);  // D3DDevice_SetRenderState_BlendOp  // zuma-nx: the game's state table (Feeding Frenzy 8212B0A0)
BANJO_RENUT_HOOK(sub_8212B2E0, SetRenderStateHook<21>);  // D3DDevice_SetRenderState_SrcBlendAlpha  // zuma-nx: the game's state table (Feeding Frenzy 8212B2E0)
BANJO_RENUT_HOOK(sub_8212B358, SetRenderStateHook<22>);  // D3DDevice_SetRenderState_DestBlendAlpha  // zuma-nx: the game's state table (Feeding Frenzy 8212B358)
BANJO_RENUT_HOOK(sub_8212B268, SetRenderStateHook<23>);  // D3DDevice_SetRenderState_BlendOpAlpha  // zuma-nx: the game's state table (Feeding Frenzy 8212B268)
BANJO_RENUT_HOOK(sub_8212AFB8, SetRenderStateHook<24>);  // D3DDevice_SetRenderState_AlphaTestEnable  // zuma-nx: the game's state table (Feeding Frenzy 8212AFB8)
BANJO_RENUT_HOOK(sub_8212B470, SetRenderStateHook<25>);  // D3DDevice_SetRenderState_AlphaRef  // zuma-nx: the game's state table (Feeding Frenzy 8212B470)
BANJO_RENUT_HOOK(sub_8212B4E0, SetRenderStateHook<26>);  // D3DDevice_SetRenderState_AlphaFunc  // zuma-nx: the game's state table (Feeding Frenzy 8212B4E0)
BANJO_RENUT_HOOK(sub_8212B708, SetRenderStateHook<27>);  // D3DDevice_SetRenderState_StencilEnable  // zuma-nx: the game's state table (Feeding Frenzy 8212B708)
BANJO_RENUT_HOOK(sub_8212B758, SetRenderStateHook<28>);  // D3DDevice_SetRenderState_TwoSidedStencilMode  // zuma-nx: the game's state table (Feeding Frenzy 8212B758)
BANJO_RENUT_HOOK(sub_8212B7D8, SetRenderStateHook<29>);  // D3DDevice_SetRenderState_StencilFail  // zuma-nx: the game's state table (Feeding Frenzy 8212B7D8)
BANJO_RENUT_HOOK(sub_8212B820, SetRenderStateHook<30>);  // D3DDevice_SetRenderState_StencilZFail  // zuma-nx: the game's state table (Feeding Frenzy 8212B820)
BANJO_RENUT_HOOK(sub_8212B868, SetRenderStateHook<31>);  // D3DDevice_SetRenderState_StencilPass  // zuma-nx: the game's state table (Feeding Frenzy 8212B868)
BANJO_RENUT_HOOK(sub_8212B7A0, SetRenderStateHook<32>);  // D3DDevice_SetRenderState_StencilFunc  // zuma-nx: the game's state table (Feeding Frenzy 8212B7A0)
BANJO_RENUT_HOOK(sub_8212B9A0, SetRenderStateHook<33>);  // D3DDevice_SetRenderState_StencilRef  // zuma-nx: the game's state table (Feeding Frenzy 8212B9A0)
BANJO_RENUT_HOOK(sub_8212B9C8, SetRenderStateHook<34>);  // D3DDevice_SetRenderState_StencilMask  // zuma-nx: the game's state table (Feeding Frenzy 8212B9C8)
BANJO_RENUT_HOOK(sub_8212B9F0, SetRenderStateHook<35>);  // D3DDevice_SetRenderState_StencilWriteMask  // zuma-nx: the game's state table (Feeding Frenzy 8212B9F0)
BANJO_RENUT_HOOK(sub_8212B8D8, SetRenderStateHook<36>);  // D3DDevice_SetRenderState_CCWStencilFail  // zuma-nx: the game's state table (Feeding Frenzy 8212B8D8)
BANJO_RENUT_HOOK(sub_8212B920, SetRenderStateHook<37>);  // D3DDevice_SetRenderState_CCWStencilZFail  // zuma-nx: the game's state table (Feeding Frenzy 8212B920)
BANJO_RENUT_HOOK(sub_8212B968, SetRenderStateHook<38>);  // D3DDevice_SetRenderState_CCWStencilPass  // zuma-nx: the game's state table (Feeding Frenzy 8212B968)
BANJO_RENUT_HOOK(sub_8212B8A0, SetRenderStateHook<39>);  // D3DDevice_SetRenderState_CCWStencilFunc  // zuma-nx: the game's state table (Feeding Frenzy 8212B8A0)
BANJO_RENUT_HOOK(sub_8212BA18, SetRenderStateHook<40>);  // D3DDevice_SetRenderState_CCWStencilRef  // zuma-nx: the game's state table (Feeding Frenzy 8212BA18)
BANJO_RENUT_HOOK(sub_8212BA40, SetRenderStateHook<41>);  // D3DDevice_SetRenderState_CCWStencilMask  // zuma-nx: the game's state table (Feeding Frenzy 8212BA40)
BANJO_RENUT_HOOK(sub_8212BA68, SetRenderStateHook<42>);  // D3DDevice_SetRenderState_CCWStencilWriteMask  // zuma-nx: the game's state table (Feeding Frenzy 8212BA68)
BANJO_RENUT_HOOK(sub_8212BA90, SetRenderStateHook<43>);  // D3DDevice_SetRenderState_ClipPlaneEnable  // zuma-nx: the game's state table (Feeding Frenzy 8212BA90)
BANJO_RENUT_HOOK(sub_8212BDD8, SetRenderStateHook<44>);  // D3DDevice_SetRenderState_PointSize  // zuma-nx: the game's state table (Feeding Frenzy 8212BDD8)
BANJO_RENUT_HOOK(sub_8212BE30, SetRenderStateHook<45>);  // D3DDevice_SetRenderState_PointSizeMin  // zuma-nx: the game's state table (Feeding Frenzy 8212BE30)
BANJO_RENUT_HOOK(sub_8212BDC8, SetRenderStateHook<46>);  // D3DDevice_SetRenderState_PointSpriteEnable  // zuma-nx: the game's state table (Feeding Frenzy 8212BDC8)
BANJO_RENUT_HOOK(sub_8212BE80, SetRenderStateHook<47>);  // D3DDevice_SetRenderState_PointSizeMax  // zuma-nx: the game's state table (Feeding Frenzy 8212BE80)
BANJO_RENUT_HOOK(sub_8212BC40, SetRenderStateHook<48>);  // D3DDevice_SetRenderState_MultiSampleAntiAlias  // zuma-nx: the game's state table (Feeding Frenzy 8212BC40)
BANJO_RENUT_HOOK(sub_8212BC78, SetRenderStateHook<49>);  // D3DDevice_SetRenderState_MultiSampleMask  // zuma-nx: the game's state table (Feeding Frenzy 8212BC78)
BANJO_RENUT_HOOK(sub_8212D938, SetRenderStateHook<50>);  // D3DDevice_SetRenderState_ScissorTestEnable  // zuma-nx: the game's state table (Feeding Frenzy 8212D938)
BANJO_RENUT_HOOK(sub_8212BAD8, SetRenderStateHook<51>);  // D3DDevice_SetRenderState_SlopeScaleDepthBias  // zuma-nx: the game's state table (Feeding Frenzy 8212BAD8)
BANJO_RENUT_HOOK(sub_8212BB98, SetRenderStateHook<52>);  // D3DDevice_SetRenderState_DepthBias  // zuma-nx: the game's state table (Feeding Frenzy 8212BB98)
BANJO_RENUT_HOOK(sub_8212BCA8, SetRenderStateHook<53>);  // D3DDevice_SetRenderState_ColorWriteEnable  // zuma-nx: the game's state table (Feeding Frenzy 8212BCA8)
BANJO_RENUT_HOOK(sub_8212BCF0, SetRenderStateHook<54>);  // D3DDevice_SetRenderState_ColorWriteEnable1  // zuma-nx: the game's state table (Feeding Frenzy 8212BCF0)
BANJO_RENUT_HOOK(sub_8212BD38, SetRenderStateHook<55>);  // D3DDevice_SetRenderState_ColorWriteEnable2  // zuma-nx: the game's state table (Feeding Frenzy 8212BD38)
BANJO_RENUT_HOOK(sub_8212BD80, SetRenderStateHook<56>);  // D3DDevice_SetRenderState_ColorWriteEnable3  // zuma-nx: the game's state table (Feeding Frenzy 8212BD80)
BANJO_RENUT_HOOK(sub_8212C260, ZumaSetRenderState57);  // XDK 2135's render state 57  // zuma-nx: the game's state table, entry 57
BANJO_RENUT_HOOK(sub_8212C278, ZumaGetRenderState57);  // XDK 2135's render state 57 (getter)  // zuma-nx: the game's state table, entry 57
BANJO_RENUT_HOOK(sub_8212C620, SetRenderStateHook<57>);  // D3DDevice_SetRenderState_TessellationMode  // zuma-nx: the game's state table (Feeding Frenzy 8212C620)
BANJO_RENUT_HOOK(sub_8212C5C0, SetRenderStateHook<58>);  // D3DDevice_SetRenderState_MinTessellationLevel  // zuma-nx: the game's state table (Feeding Frenzy 8212C5C0)
BANJO_RENUT_HOOK(sub_8212C5F0, SetRenderStateHook<59>);  // D3DDevice_SetRenderState_MaxTessellationLevel  // zuma-nx: the game's state table (Feeding Frenzy 8212C5F0)
BANJO_RENUT_HOOK(sub_8212BED0, SetRenderStateHook<60>);  // D3DDevice_SetRenderState_Wrap0  // zuma-nx: the game's state table (Feeding Frenzy 8212BED0)
BANJO_RENUT_HOOK(sub_8212BF00, SetRenderStateHook<61>);  // D3DDevice_SetRenderState_Wrap1  // zuma-nx: the game's state table (Feeding Frenzy 8212BF00)
BANJO_RENUT_HOOK(sub_8212BF30, SetRenderStateHook<62>);  // D3DDevice_SetRenderState_Wrap2  // zuma-nx: the game's state table (Feeding Frenzy 8212BF30)
BANJO_RENUT_HOOK(sub_8212BF60, SetRenderStateHook<63>);  // D3DDevice_SetRenderState_Wrap3  // zuma-nx: the game's state table (Feeding Frenzy 8212BF60)
BANJO_RENUT_HOOK(sub_8212BF90, SetRenderStateHook<64>);  // D3DDevice_SetRenderState_Wrap4  // zuma-nx: the game's state table (Feeding Frenzy 8212BF90)
BANJO_RENUT_HOOK(sub_8212BFC0, SetRenderStateHook<65>);  // D3DDevice_SetRenderState_Wrap5  // zuma-nx: the game's state table (Feeding Frenzy 8212BFC0)
BANJO_RENUT_HOOK(sub_8212BFF0, SetRenderStateHook<66>);  // D3DDevice_SetRenderState_Wrap6  // zuma-nx: the game's state table (Feeding Frenzy 8212BFF0)
BANJO_RENUT_HOOK(sub_8212C020, SetRenderStateHook<67>);  // D3DDevice_SetRenderState_Wrap7  // zuma-nx: the game's state table (Feeding Frenzy 8212C020)
BANJO_RENUT_HOOK(sub_8212C048, SetRenderStateHook<68>);  // D3DDevice_SetRenderState_Wrap8  // zuma-nx: the game's state table (Feeding Frenzy 8212C048)
BANJO_RENUT_HOOK(sub_8212C078, SetRenderStateHook<69>);  // D3DDevice_SetRenderState_Wrap9  // zuma-nx: the game's state table (Feeding Frenzy 8212C078)
BANJO_RENUT_HOOK(sub_8212C0A8, SetRenderStateHook<70>);  // D3DDevice_SetRenderState_Wrap10  // zuma-nx: the game's state table (Feeding Frenzy 8212C0A8)
BANJO_RENUT_HOOK(sub_8212C0D8, SetRenderStateHook<71>);  // D3DDevice_SetRenderState_Wrap11  // zuma-nx: the game's state table (Feeding Frenzy 8212C0D8)
BANJO_RENUT_HOOK(sub_8212C108, SetRenderStateHook<72>);  // D3DDevice_SetRenderState_Wrap12  // zuma-nx: the game's state table (Feeding Frenzy 8212C108)
BANJO_RENUT_HOOK(sub_8212C138, SetRenderStateHook<73>);  // D3DDevice_SetRenderState_Wrap13  // zuma-nx: the game's state table (Feeding Frenzy 8212C138)
BANJO_RENUT_HOOK(sub_8212C168, SetRenderStateHook<74>);  // D3DDevice_SetRenderState_Wrap14  // zuma-nx: the game's state table (Feeding Frenzy 8212C168)
BANJO_RENUT_HOOK(sub_8212C198, SetRenderStateHook<75>);  // D3DDevice_SetRenderState_Wrap15  // zuma-nx: the game's state table (Feeding Frenzy 8212C198)
BANJO_RENUT_HOOK(sub_8212C280, SetRenderStateHook<76>);  // D3DDevice_SetRenderState_ViewportEnable  // zuma-nx: the game's state table (Feeding Frenzy 8212C280)
BANJO_RENUT_HOOK(sub_8212C2E0, SetRenderStateHook<77>);  // D3DDevice_SetRenderState_HighPrecisionBlendEnable  // zuma-nx: the game's state table (Feeding Frenzy 8212C2E0)
BANJO_RENUT_HOOK(sub_8212C398, SetRenderStateHook<78>);  // D3DDevice_SetRenderState_HighPrecisionBlendEnable1  // zuma-nx: the game's state table (Feeding Frenzy 8212C398)
BANJO_RENUT_HOOK(sub_8212C450, SetRenderStateHook<79>);  // D3DDevice_SetRenderState_HighPrecisionBlendEnable2  // zuma-nx: the game's state table (Feeding Frenzy 8212C450)
BANJO_RENUT_HOOK(sub_8212C508, SetRenderStateHook<80>);  // D3DDevice_SetRenderState_HighPrecisionBlendEnable3  // zuma-nx: the game's state table (Feeding Frenzy 8212C508)
BANJO_RENUT_HOOK(sub_8212C650, SetRenderStateHook<81>);  // D3DDevice_SetRenderState_HalfPixelOffset  // zuma-nx: the game's state table (Feeding Frenzy 8212C650)
BANJO_RENUT_HOOK(sub_8212C688, SetRenderStateHook<82>);  // D3DDevice_SetRenderState_PrimitiveResetEnable  // zuma-nx: the game's state table (Feeding Frenzy 8212C688)
BANJO_RENUT_HOOK(sub_8212C6C0, SetRenderStateHook<83>);  // D3DDevice_SetRenderState_PrimitiveResetIndex  // zuma-nx: the game's state table (Feeding Frenzy 8212C6C0)
BANJO_RENUT_HOOK(sub_8212C6E8, SetRenderStateHook<84>);  // D3DDevice_SetRenderState_AlphaToMaskEnable  // zuma-nx: the game's state table (Feeding Frenzy 8212C6E8)
BANJO_RENUT_HOOK(sub_8212C720, SetRenderStateHook<85>);  // D3DDevice_SetRenderState_AlphaToMaskOffsets  // zuma-nx: the game's state table (Feeding Frenzy 8212C720)
BANJO_RENUT_HOOK(sub_8212C760, SetRenderStateHook<86>);  // D3DDevice_SetRenderState_GuardBand_X  // zuma-nx: the game's state table (Feeding Frenzy 8212C760)
BANJO_RENUT_HOOK(sub_8212C790, SetRenderStateHook<87>);  // D3DDevice_SetRenderState_GuardBand_Y  // zuma-nx: the game's state table (Feeding Frenzy 8212C790)
BANJO_RENUT_HOOK(sub_8212C7C0, SetRenderStateHook<88>);  // D3DDevice_SetRenderState_DiscardBand_X  // zuma-nx: the game's state table (Feeding Frenzy 8212C7C0)
BANJO_RENUT_HOOK(sub_8212C7F0, SetRenderStateHook<89>);  // D3DDevice_SetRenderState_DiscardBand_Y  // zuma-nx: the game's state table (Feeding Frenzy 8212C7F0)
BANJO_RENUT_HOOK(sub_8212C820, SetRenderStateHook<90>);  // D3DDevice_SetRenderState_HiStencilEnable  // zuma-nx: the game's state table (Feeding Frenzy 8212C820)
BANJO_RENUT_HOOK(sub_8212C858, SetRenderStateHook<91>);  // D3DDevice_SetRenderState_HiStencilWriteEnable  // zuma-nx: the game's state table (Feeding Frenzy 8212C858)
BANJO_RENUT_HOOK(sub_8212C890, SetRenderStateHook<92>);  // D3DDevice_SetRenderState_HiStencilFunc  // zuma-nx: the game's state table (Feeding Frenzy 8212C890)
BANJO_RENUT_HOOK(sub_8212C8C8, SetRenderStateHook<93>);  // D3DDevice_SetRenderState_HiStencilRef  // zuma-nx: the game's state table (Feeding Frenzy 8212C8C8)
BANJO_RENUT_HOOK(sub_8212C8F0, SetRenderStateHook<94>);  // D3DDevice_SetRenderState_PresentInterval  // zuma-nx: the game's state table (Feeding Frenzy 8212C8F0)
BANJO_RENUT_HOOK(sub_8212C900, SetRenderStateHook<95>);  // D3DDevice_SetRenderState_PresentImmediateThreshold  // zuma-nx: the game's state table (Feeding Frenzy 8212C900)
// zuma-nx: not linked or not called in XDK 2135: BANJO_RENUT_HOOK(sub_XXXXXXXX, SetRenderStateHook<96>);  // D3DDevice_SetRenderState_HiZEnable (not in Feeding Frenzy's table)
// zuma-nx: not linked or not called in XDK 2135: BANJO_RENUT_HOOK(sub_XXXXXXXX, SetRenderStateHook<97>);  // D3DDevice_SetRenderState_HiZWriteEnable (not in Feeding Frenzy's table)
// zuma-nx: not linked or not called in XDK 2135: BANJO_RENUT_HOOK(sub_XXXXXXXX, SetRenderStateHook<98>);  // D3DDevice_SetRenderState_LastPixel (not in Feeding Frenzy's table)
// zuma-nx: not linked or not called in XDK 2135: BANJO_RENUT_HOOK(sub_XXXXXXXX, SetRenderStateHook<99>);  // D3DDevice_SetRenderState_LineWidth (not in Feeding Frenzy's table)
// zuma-nx: not linked or not called in XDK 2135: BANJO_RENUT_HOOK(sub_XXXXXXXX, SetRenderStateHook<100>);  // D3DDevice_SetRenderState_Buffer2Frames (not in Feeding Frenzy's table)

// Sampler states, by their index in N&B's table (off_82E50BD0).
BANJO_RENUT_HOOK(sub_8212CF10, SetSamplerStateHook<0>);  // D3DDevice_SetSamplerState_AddressU  // zuma-nx: the game's state table (Feeding Frenzy 8212CF10)
BANJO_RENUT_HOOK(sub_8212CF60, SetSamplerStateHook<1>);  // D3DDevice_SetSamplerState_AddressV  // zuma-nx: the game's state table (Feeding Frenzy 8212CF60)
BANJO_RENUT_HOOK(sub_8212CFB0, SetSamplerStateHook<2>);  // D3DDevice_SetSamplerState_AddressW  // zuma-nx: the game's state table (Feeding Frenzy 8212CFB0)
BANJO_RENUT_HOOK(sub_8212CEA0, SetSamplerStateHook<3>);  // D3DDevice_SetSamplerState_BorderColor  // zuma-nx: the game's state table (Feeding Frenzy 8212CEA0)
BANJO_RENUT_HOOK(sub_8212CA30, SetSamplerStateHook<4>);  // D3DDevice_SetSamplerState_MagFilter  // zuma-nx: the game's state table (Feeding Frenzy 8212CA30)
BANJO_RENUT_HOOK(sub_8212C920, SetSamplerStateHook<5>);  // D3DDevice_SetSamplerState_MinFilter  // zuma-nx: the game's state table (Feeding Frenzy 8212C920)
BANJO_RENUT_HOOK(sub_8212CB38, SetSamplerStateHook<6>);  // D3DDevice_SetSamplerState_MipFilter  // zuma-nx: the game's state table (Feeding Frenzy 8212CB38)
BANJO_RENUT_HOOK(sub_8212CD00, SetSamplerStateHook<7>);  // D3DDevice_SetSamplerState_MipMapLodBias  // zuma-nx: the game's state table (Feeding Frenzy 8212CD00)
BANJO_RENUT_HOOK(sub_8212CDA0, SetSamplerStateHook<8>);  // D3DDevice_SetSamplerState_MaxMipLevel  // zuma-nx: the game's state table (Feeding Frenzy 8212CDA0)
BANJO_RENUT_HOOK(sub_8212CBE0, SetSamplerStateHook<9>);  // D3DDevice_SetSamplerState_MaxAnisotropy  // zuma-nx: the game's state table (Feeding Frenzy 8212CBE0)
BANJO_RENUT_HOOK(sub_8212CAF0, SetSamplerStateHook<10>);  // D3DDevice_SetSamplerState_MagFilterZ  // zuma-nx: the game's state table (Feeding Frenzy 8212CAF0)
BANJO_RENUT_HOOK(sub_8212C9E0, SetSamplerStateHook<11>);  // D3DDevice_SetSamplerState_MinFilterZ  // zuma-nx: the game's state table (Feeding Frenzy 8212C9E0)
BANJO_RENUT_HOOK(sub_8212CB90, SetSamplerStateHook<12>);  // D3DDevice_SetSamplerState_SeparateZFilterEnable  // zuma-nx: the game's state table (Feeding Frenzy 8212CB90)
BANJO_RENUT_HOOK(sub_8212CE20, SetSamplerStateHook<13>);  // D3DDevice_SetSamplerState_MinMipLevel  // zuma-nx: the game's state table (Feeding Frenzy 8212CE20)
BANJO_RENUT_HOOK(sub_8212D000, SetSamplerStateHook<14>);  // D3DDevice_SetSamplerState_TrilinearThreshold  // zuma-nx: the game's state table (Feeding Frenzy 8212D000)
BANJO_RENUT_HOOK(sub_8212CC58, SetSamplerStateHook<15>);  // D3DDevice_SetSamplerState_AnisotropyBias  // zuma-nx: the game's state table (Feeding Frenzy 8212CC58)
BANJO_RENUT_HOOK(sub_8212D058, SetSamplerStateHook<16>);  // D3DDevice_SetSamplerState_HGradientExpBias  // zuma-nx: the game's state table (Feeding Frenzy 8212D058)
BANJO_RENUT_HOOK(sub_8212D0B0, SetSamplerStateHook<17>);  // D3DDevice_SetSamplerState_VGradientExpBias  // zuma-nx: the game's state table (Feeding Frenzy 8212D0B0)
BANJO_RENUT_HOOK(sub_8212D108, SetSamplerStateHook<18>);  // D3DDevice_SetSamplerState_WhiteBorderColorW  // zuma-nx: the game's state table (Feeding Frenzy 8212D108)
BANJO_RENUT_HOOK(sub_8212D160, SetSamplerStateHook<19>);  // D3DDevice_SetSamplerState_PointBorderEnable  // zuma-nx: the game's state table (Feeding Frenzy 8212D160)

BANJO_RENUT_HOOK(sub_8212A900, ZumaSetTexture);  // D3DDevice_SetTexture  // zuma-nx: (dev, sampler, texture): Renut's SetTexture on the texture's shadow (adapter) (Feeding Frenzy 8212A900)
#if 0  // peggle2-nx / peggle1-nx / bejeweled3-nx: N&B sub_82B96538 (D3DDevice_SetTexture_Inline) is linked in none of Peggle 2, Peggle 1 and Bejeweled 3
BANJO_RENUT_HOOK(sub_82B96538, D3DDevice_SetTexture_hook);  // D3DDevice_SetTexture_Inline
#endif
BANJO_RENUT_HOOK(sub_8212D4D0, D3DDevice_SetStreamSource_hook);  // D3DDevice_SetStreamSource  // zuma-nx: (dev, stream, vb, offset, stride): 40 instructions, the old buffer released through sub_82122418 (Feeding Frenzy 8212D4D0)
// feedingfrenzy-nx: not linked or not called in XDK 2571: BANJO_RENUT_HOOK(sub_XXXXXXXX, D3DDevice_SetIndices_hook);  // D3DDevice_SetIndices  // N&B sub_8220EE38
BANJO_RENUT_HOOK(sub_82125C90, D3DDevice_SetVertexDeclaration_hook);  // D3DDevice_SetVertexDeclaration  // zuma-nx: (dev, decl): AddRef, stored at +11408, the old one released, dirty bits 0x80 / 0x400 of the qword at +48 (Feeding Frenzy 82125C90)
// feedingfrenzy-nx: not linked or not called in XDK 2571: BANJO_RENUT_HOOK(sub_XXXXXXXX, D3DDevice_SetViewport_hook);  // D3DDevice_SetViewport  // N&B sub_82222EA0
// feedingfrenzy-nx: not linked or not called in XDK 2571: BANJO_RENUT_HOOK(sub_XXXXXXXX, D3D_SetViewport_hook);  // D3D_SetViewport  // N&B sub_82222F20
// feedingfrenzy-nx: not linked or not called in XDK 2571: BANJO_RENUT_HOOK(sub_XXXXXXXX, D3DDevice_SetScissorRect_hook);  // D3DDevice_SetScissorRect  // N&B sub_82223178
// feedingfrenzy-nx: not linked or not called in XDK 2571: BANJO_RENUT_HOOK(sub_XXXXXXXX, D3D_SetSurfaceClip_hook);  // D3D_SetSurfaceClip  // N&B sub_82223270
// feedingfrenzy-nx: not linked or not called in XDK 2571: BANJO_RENUT_HOOK(sub_XXXXXXXX, D3DDevice_SetRenderTarget_hook);  // D3DDevice_SetRenderTarget  // N&B sub_8227AB68
// feedingfrenzy-nx: not linked or not called in XDK 2571: BANJO_RENUT_HOOK(sub_XXXXXXXX, D3DDevice_SetDepthStencilSurface_hook);  // D3DDevice_SetDepthStencilSurface  // N&B sub_8227AF40
// feedingfrenzy-nx: not linked or not called in XDK 2571: BANJO_RENUT_HOOK(sub_XXXXXXXX, D3D_SetSurfaceInfo_hook);  // D3D_SetSurfaceInfo  // N&B sub_8227AEB0
// feedingfrenzy-nx: not linked or not called in XDK 2571: BANJO_RENUT_HOOK(sub_XXXXXXXX, D3DDevice_SetPredication_hook);  // D3DDevice_SetPredication  // N&B sub_8222AA18
#if 0  // bejeweled3-nx: N&B sub_82658678 (D3DDevice_SetCommandBufferPredication) is not linked in Bejeweled 3
BANJO_RENUT_HOOK(sub_82446398, D3DDevice_SetCommandBufferPredication_hook);  // D3DDevice_SetCommandBufferPredication  // N&B sub_82658678
#endif
// feedingfrenzy-nx: not linked or not called in XDK 2571: BANJO_RENUT_HOOK(sub_XXXXXXXX, D3DDevice_SetShaderGPRAllocation_hook);  // D3DDevice_SetShaderGPRAllocation  // N&B sub_82220B20
// feedingfrenzy-nx: not linked or not called in XDK 2571: BANJO_RENUT_HOOK(sub_XXXXXXXX, D3D_CleanupGprAllocationSetup_hook);  // D3D_CleanupGprAllocationSetup  // N&B sub_8264E2C0
// feedingfrenzy-nx: not linked or not called in XDK 2571: BANJO_RENUT_HOOK(sub_XXXXXXXX, D3DDevice_SetScreenExtentQueryMode_hook);  // D3DDevice_SetScreenExtentQueryMode  // N&B sub_82658988
// feedingfrenzy-nx: not linked or not called in XDK 2571: BANJO_RENUT_HOOK(sub_XXXXXXXX, D3DDevice_UnsetAll_hook);  // D3DDevice_UnsetAll  // N&B sub_8264E1A8
// feedingfrenzy-nx: not linked or not called in XDK 2571: BANJO_RENUT_HOOK(sub_XXXXXXXX, D3D_ResetAllState_hook);  // D3D_ResetAllState  // N&B sub_82287D38
#if 0  // bejeweled3-nx: N&B sub_8264E128 (D3D_InvalidateConstants) is not linked in Bejeweled 3
BANJO_RENUT_HOOK(sub_82443FC8, D3D_InvalidateConstants_hook);  // D3D_InvalidateConstants  // N&B sub_8264E128
#endif
// feedingfrenzy-nx: not linked or not called in XDK 2571: BANJO_RENUT_HOOK(sub_XXXXXXXX, D3DDevice_BlockUntilIdle_hook);  // D3DDevice_BlockUntilIdle  // N&B sub_82657488
// Through a thunk (j_D3DDevice_SetRenderTarget 0x8264D8C8).
// feedingfrenzy-nx: not linked or not called in XDK 2571: BANJO_RENUT_HOOK(sub_XXXXXXXX, D3DDevice_SetRenderTarget_hook);  // D3DDevice_SetRenderTarget_Thunk  // N&B sub_8264D8C8
// zuma-nx: not linked or not called in XDK 2135: BANJO_RENUT_HOOK(sub_XXXXXXXX, D3DDevice_GetRenderTarget_hook);  // D3DDevice_GetRenderTarget (Feeding Frenzy 820BBE98)
// feedingfrenzy-nx: not linked or not called in XDK 2571: BANJO_RENUT_HOOK(sub_XXXXXXXX, D3DDevice_GetDepthStencilSurface_hook);  // D3DDevice_GetDepthStencilSurface  // N&B sub_8264D910
// feedingfrenzy-nx: not linked or not called in XDK 2571: BANJO_RENUT_HOOK(sub_XXXXXXXX, D3DDevice_GetStreamSource_hook);  // D3DDevice_GetStreamSource  // N&B sub_8264D840
#if 0  // bejeweled3-nx: N&B sub_82276AC0 (D3DDevice_GetViewport) is not linked in Bejeweled 3
BANJO_RENUT_HOOK(sub_82436468, D3DDevice_GetViewport_hook);  // D3DDevice_GetViewport  // N&B sub_82276AC0
#endif
// feedingfrenzy-nx: not linked or not called in XDK 2571: BANJO_RENUT_HOOK(sub_XXXXXXXX, D3DDevice_SetClipPlane_hook);  // D3DDevice_SetClipPlane  // N&B sub_8264DAE8
#if 0  // bejeweled3-nx: N&B sub_8264DB38 (D3DDevice_SetVertexFetchConstant) is not linked in Bejeweled 3
BANJO_RENUT_HOOK(sub_82436B20, D3DDevice_SetVertexFetchConstant_hook);  // D3DDevice_SetVertexFetchConstant  // N&B sub_8264DB38
#endif
BANJO_RENUT_HOOK(sub_821243D0, D3DDevice_SetVertexShaderConstantB_hook);  // D3DDevice_SetVertexShaderConstantB  // zuma-nx: aligned with Feeding Frenzy's 820B5C78 (Feeding Frenzy 821243D0)
BANJO_RENUT_HOOK(sub_82124428, D3DDevice_SetPixelShaderConstantB_hook);  // D3DDevice_SetPixelShaderConstantB  // zuma-nx: aligned with Feeding Frenzy's 820B5CD0 (Feeding Frenzy 82124428)
BANJO_RENUT_HOOK(sub_82124480, D3DDevice_SetVertexShaderConstantI_hook);  // D3DDevice_SetVertexShaderConstantI  // zuma-nx: aligned with Feeding Frenzy's 820B5D28 (Feeding Frenzy 82124480)
BANJO_RENUT_HOOK(sub_821244D0, D3DDevice_SetPixelShaderConstantI_hook);  // D3DDevice_SetPixelShaderConstantI  // zuma-nx: aligned with Feeding Frenzy's 820B5D78 (Feeding Frenzy 821244D0)
// feedingfrenzy-nx: not linked or not called in XDK 2571: BANJO_RENUT_HOOK(sub_XXXXXXXX, D3DDevice_EndIndexedVertices_hook);  // D3DDevice_EndIndexedVertices  // N&B sub_826561F8

// Render state getters, by their index in N&B's table (off_82E506F0, field 0).
BANJO_RENUT_HOOK(sub_8212B680, GetRenderStateHook<10>);  // D3DDevice_GetRenderState_ZEnable  // zuma-nx: the game's state table (Feeding Frenzy 8212B680)
BANJO_RENUT_HOOK(sub_8212B6F8, GetRenderStateHook<11>);  // D3DDevice_GetRenderState_ZFunc  // zuma-nx: the game's state table (Feeding Frenzy 8212B6F8)
BANJO_RENUT_HOOK(sub_8212B6B0, GetRenderStateHook<12>);  // D3DDevice_GetRenderState_ZWriteEnable  // zuma-nx: the game's state table (Feeding Frenzy 8212B6B0)
BANJO_RENUT_HOOK(sub_8212AFA8, GetRenderStateHook<13>);  // D3DDevice_GetRenderState_FillMode  // zuma-nx: the game's state table (Feeding Frenzy 8212AFA8)
BANJO_RENUT_HOOK(sub_8212AF70, GetRenderStateHook<14>);  // D3DDevice_GetRenderState_CullMode  // zuma-nx: the game's state table (Feeding Frenzy 8212AF70)
BANJO_RENUT_HOOK(sub_8212B090, GetRenderStateHook<15>);  // D3DDevice_GetRenderState_AlphaBlendEnable  // zuma-nx: the game's state table (Feeding Frenzy 8212B090)
BANJO_RENUT_HOOK(sub_8212B460, GetRenderStateHook<16>);  // D3DDevice_GetRenderState_SeparateAlphaBlendEnable  // zuma-nx: the game's state table (Feeding Frenzy 8212B460)
BANJO_RENUT_HOOK(sub_8212B5B8, GetRenderStateHook<17>);  // D3DDevice_GetRenderState_BlendFactor  // zuma-nx: the game's state table (Feeding Frenzy 8212B5B8)
BANJO_RENUT_HOOK(sub_8212B1C0, GetRenderStateHook<18>);  // D3DDevice_GetRenderState_SrcBlend  // zuma-nx: the game's state table (Feeding Frenzy 8212B1C0)
BANJO_RENUT_HOOK(sub_8212B258, GetRenderStateHook<19>);  // D3DDevice_GetRenderState_DestBlend  // zuma-nx: the game's state table (Feeding Frenzy 8212B258)
BANJO_RENUT_HOOK(sub_8212B128, GetRenderStateHook<20>);  // D3DDevice_GetRenderState_BlendOp  // zuma-nx: the game's state table (Feeding Frenzy 8212B128)
BANJO_RENUT_HOOK(sub_8212B348, GetRenderStateHook<21>);  // D3DDevice_GetRenderState_SrcBlendAlpha  // zuma-nx: the game's state table (Feeding Frenzy 8212B348)
BANJO_RENUT_HOOK(sub_8212B3C0, GetRenderStateHook<22>);  // D3DDevice_GetRenderState_DestBlendAlpha  // zuma-nx: the game's state table (Feeding Frenzy 8212B3C0)
BANJO_RENUT_HOOK(sub_8212B2D0, GetRenderStateHook<23>);  // D3DDevice_GetRenderState_BlendOpAlpha  // zuma-nx: the game's state table (Feeding Frenzy 8212B2D0)
BANJO_RENUT_HOOK(sub_8212AFF0, GetRenderStateHook<24>);  // D3DDevice_GetRenderState_AlphaTestEnable  // zuma-nx: the game's state table (Feeding Frenzy 8212AFF0)
BANJO_RENUT_HOOK(sub_8212B4B0, GetRenderStateHook<25>);  // D3DDevice_GetRenderState_AlphaRef  // zuma-nx: the game's state table (Feeding Frenzy 8212B4B0)
BANJO_RENUT_HOOK(sub_8212B508, GetRenderStateHook<26>);  // D3DDevice_GetRenderState_AlphaFunc  // zuma-nx: the game's state table (Feeding Frenzy 8212B508)
BANJO_RENUT_HOOK(sub_8212B750, GetRenderStateHook<27>);  // D3DDevice_GetRenderState_StencilEnable  // zuma-nx: the game's state table (Feeding Frenzy 8212B750)
BANJO_RENUT_HOOK(sub_8212B790, GetRenderStateHook<28>);  // D3DDevice_GetRenderState_TwoSidedStencilMode  // zuma-nx: the game's state table (Feeding Frenzy 8212B790)
BANJO_RENUT_HOOK(sub_8212B810, GetRenderStateHook<29>);  // D3DDevice_GetRenderState_StencilFail  // zuma-nx: the game's state table (Feeding Frenzy 8212B810)
BANJO_RENUT_HOOK(sub_8212B858, GetRenderStateHook<30>);  // D3DDevice_GetRenderState_StencilZFail  // zuma-nx: the game's state table (Feeding Frenzy 8212B858)
BANJO_RENUT_HOOK(sub_8212B890, GetRenderStateHook<31>);  // D3DDevice_GetRenderState_StencilPass  // zuma-nx: the game's state table (Feeding Frenzy 8212B890)
BANJO_RENUT_HOOK(sub_8212B7C8, GetRenderStateHook<32>);  // D3DDevice_GetRenderState_StencilFunc  // zuma-nx: the game's state table (Feeding Frenzy 8212B7C8)
BANJO_RENUT_HOOK(sub_8212B9C0, GetRenderStateHook<33>);  // D3DDevice_GetRenderState_StencilRef  // zuma-nx: the game's state table (Feeding Frenzy 8212B9C0)
BANJO_RENUT_HOOK(sub_8212B9E8, GetRenderStateHook<34>);  // D3DDevice_GetRenderState_StencilMask  // zuma-nx: the game's state table (Feeding Frenzy 8212B9E8)
BANJO_RENUT_HOOK(sub_8212BA10, GetRenderStateHook<35>);  // D3DDevice_GetRenderState_StencilWriteMask  // zuma-nx: the game's state table (Feeding Frenzy 8212BA10)
BANJO_RENUT_HOOK(sub_8212B910, GetRenderStateHook<36>);  // D3DDevice_GetRenderState_CCWStencilFail  // zuma-nx: the game's state table (Feeding Frenzy 8212B910)
BANJO_RENUT_HOOK(sub_8212B958, GetRenderStateHook<37>);  // D3DDevice_GetRenderState_CCWStencilZFail  // zuma-nx: the game's state table (Feeding Frenzy 8212B958)
BANJO_RENUT_HOOK(sub_8212B990, GetRenderStateHook<38>);  // D3DDevice_GetRenderState_CCWStencilPass  // zuma-nx: the game's state table (Feeding Frenzy 8212B990)
BANJO_RENUT_HOOK(sub_8212B8C8, GetRenderStateHook<39>);  // D3DDevice_GetRenderState_CCWStencilFunc  // zuma-nx: the game's state table (Feeding Frenzy 8212B8C8)
BANJO_RENUT_HOOK(sub_8212BA38, GetRenderStateHook<40>);  // D3DDevice_GetRenderState_CCWStencilRef  // zuma-nx: the game's state table (Feeding Frenzy 8212BA38)
BANJO_RENUT_HOOK(sub_8212BA60, GetRenderStateHook<41>);  // D3DDevice_GetRenderState_CCWStencilMask  // zuma-nx: the game's state table (Feeding Frenzy 8212BA60)
BANJO_RENUT_HOOK(sub_8212BA88, GetRenderStateHook<42>);  // D3DDevice_GetRenderState_CCWStencilWriteMask  // zuma-nx: the game's state table (Feeding Frenzy 8212BA88)
BANJO_RENUT_HOOK(sub_8212BAC0, GetRenderStateHook<43>);  // D3DDevice_GetRenderState_ClipPlaneEnable  // zuma-nx: the game's state table (Feeding Frenzy 8212BAC0)
BANJO_RENUT_HOOK(sub_8212BE20, GetRenderStateHook<44>);  // D3DDevice_GetRenderState_PointSize  // zuma-nx: the game's state table (Feeding Frenzy 8212BE20)
BANJO_RENUT_HOOK(sub_8212BE70, GetRenderStateHook<45>);  // D3DDevice_GetRenderState_PointSizeMin  // zuma-nx: the game's state table (Feeding Frenzy 8212BE70)
BANJO_RENUT_HOOK(sub_8212BDD0, GetRenderStateHook<46>);  // D3DDevice_GetRenderState_PointSpriteEnable  // zuma-nx: the game's state table (Feeding Frenzy 8212BDD0)
BANJO_RENUT_HOOK(sub_8212BEC0, GetRenderStateHook<47>);  // D3DDevice_GetRenderState_PointSizeMax  // zuma-nx: the game's state table (Feeding Frenzy 8212BEC0)
BANJO_RENUT_HOOK(sub_8212BC68, GetRenderStateHook<48>);  // D3DDevice_GetRenderState_MultiSampleAntiAlias  // zuma-nx: the game's state table (Feeding Frenzy 8212BC68)
BANJO_RENUT_HOOK(sub_8212BCA0, GetRenderStateHook<49>);  // D3DDevice_GetRenderState_MultiSampleMask  // zuma-nx: the game's state table (Feeding Frenzy 8212BCA0)
BANJO_RENUT_HOOK(sub_8212BAD0, GetRenderStateHook<50>);  // D3DDevice_GetRenderState_ScissorTestEnable  // zuma-nx: the game's state table (Feeding Frenzy 8212BAD0)
BANJO_RENUT_HOOK(sub_8212BB78, GetRenderStateHook<51>);  // D3DDevice_GetRenderState_SlopeScaleDepthBias  // zuma-nx: the game's state table (Feeding Frenzy 8212BB78)
BANJO_RENUT_HOOK(sub_8212BC30, GetRenderStateHook<52>);  // D3DDevice_GetRenderState_DepthBias  // zuma-nx: the game's state table (Feeding Frenzy 8212BC30)
BANJO_RENUT_HOOK(sub_8212BCE8, GetRenderStateHook<53>);  // D3DDevice_GetRenderState_ColorWriteEnable  // zuma-nx: the game's state table (Feeding Frenzy 8212BCE8)
BANJO_RENUT_HOOK(sub_8212BD30, GetRenderStateHook<54>);  // D3DDevice_GetRenderState_ColorWriteEnable1  // zuma-nx: the game's state table (Feeding Frenzy 8212BD30)
BANJO_RENUT_HOOK(sub_8212BD78, GetRenderStateHook<55>);  // D3DDevice_GetRenderState_ColorWriteEnable2  // zuma-nx: the game's state table (Feeding Frenzy 8212BD78)
BANJO_RENUT_HOOK(sub_8212BDC0, GetRenderStateHook<56>);  // D3DDevice_GetRenderState_ColorWriteEnable3  // zuma-nx: the game's state table (Feeding Frenzy 8212BDC0)
BANJO_RENUT_HOOK(sub_8212C640, GetRenderStateHook<57>);  // D3DDevice_GetRenderState_TessellationMode  // zuma-nx: the game's state table (Feeding Frenzy 8212C640)
BANJO_RENUT_HOOK(sub_8212C5E0, GetRenderStateHook<58>);  // D3DDevice_GetRenderState_MinTessellationLevel  // zuma-nx: the game's state table (Feeding Frenzy 8212C5E0)
BANJO_RENUT_HOOK(sub_8212C610, GetRenderStateHook<59>);  // D3DDevice_GetRenderState_MaxTessellationLevel  // zuma-nx: the game's state table (Feeding Frenzy 8212C610)
BANJO_RENUT_HOOK(sub_8212C1C0, GetRenderStateHook<60>);  // D3DDevice_GetRenderState_Wrap0  // zuma-nx: the game's state table (Feeding Frenzy 8212C1C0)
BANJO_RENUT_HOOK(sub_8212C1D0, GetRenderStateHook<61>);  // D3DDevice_GetRenderState_Wrap1  // zuma-nx: the game's state table (Feeding Frenzy 8212C1D0)
BANJO_RENUT_HOOK(sub_8212C1E0, GetRenderStateHook<62>);  // D3DDevice_GetRenderState_Wrap2  // zuma-nx: the game's state table (Feeding Frenzy 8212C1E0)
BANJO_RENUT_HOOK(sub_8212C1F0, GetRenderStateHook<63>);  // D3DDevice_GetRenderState_Wrap3  // zuma-nx: the game's state table (Feeding Frenzy 8212C1F0)
BANJO_RENUT_HOOK(sub_8212C200, GetRenderStateHook<64>);  // D3DDevice_GetRenderState_Wrap4  // zuma-nx: the game's state table (Feeding Frenzy 8212C200)
BANJO_RENUT_HOOK(sub_8212C210, GetRenderStateHook<65>);  // D3DDevice_GetRenderState_Wrap5  // zuma-nx: the game's state table (Feeding Frenzy 8212C210)
BANJO_RENUT_HOOK(sub_8212C220, GetRenderStateHook<66>);  // D3DDevice_GetRenderState_Wrap6  // zuma-nx: the game's state table (Feeding Frenzy 8212C220)
BANJO_RENUT_HOOK(sub_8212C230, GetRenderStateHook<67>);  // D3DDevice_GetRenderState_Wrap7  // zuma-nx: the game's state table (Feeding Frenzy 8212C230)
BANJO_RENUT_HOOK(sub_8212C240, GetRenderStateHook<68>);  // D3DDevice_GetRenderState_Wrap8  // zuma-nx: the game's state table (Feeding Frenzy 8212C240)
BANJO_RENUT_HOOK(sub_8212C250, GetRenderStateHook<69>);  // D3DDevice_GetRenderState_Wrap9  // zuma-nx: the game's state table (Feeding Frenzy 8212C250)
// zuma-nx: not linked or not called in XDK 2135: BANJO_RENUT_HOOK(sub_XXXXXXXX, GetRenderStateHook<70>);  // D3DDevice_GetRenderState_Wrap10 (Feeding Frenzy 821939F0)
// zuma-nx: not linked or not called in XDK 2135: BANJO_RENUT_HOOK(sub_XXXXXXXX, GetRenderStateHook<71>);  // D3DDevice_GetRenderState_Wrap11 (Feeding Frenzy 821939F0)
// zuma-nx: not linked or not called in XDK 2135: BANJO_RENUT_HOOK(sub_XXXXXXXX, GetRenderStateHook<72>);  // D3DDevice_GetRenderState_Wrap12 (Feeding Frenzy 821939F0)
// zuma-nx: not linked or not called in XDK 2135: BANJO_RENUT_HOOK(sub_XXXXXXXX, GetRenderStateHook<73>);  // D3DDevice_GetRenderState_Wrap13 (Feeding Frenzy 821939F0)
// zuma-nx: not linked or not called in XDK 2135: BANJO_RENUT_HOOK(sub_XXXXXXXX, GetRenderStateHook<74>);  // D3DDevice_GetRenderState_Wrap14 (Feeding Frenzy 821939F0)
// zuma-nx: not linked or not called in XDK 2135: BANJO_RENUT_HOOK(sub_XXXXXXXX, GetRenderStateHook<75>);  // D3DDevice_GetRenderState_Wrap15 (Feeding Frenzy 821939F0)
BANJO_RENUT_HOOK(sub_8212C2D0, GetRenderStateHook<76>);  // D3DDevice_GetRenderState_ViewportEnable  // zuma-nx: the game's state table (Feeding Frenzy 8212C2D0)
BANJO_RENUT_HOOK(sub_8212C390, GetRenderStateHook<77>);  // D3DDevice_GetRenderState_HighPrecisionBlendEnable  // zuma-nx: the game's state table (Feeding Frenzy 8212C390)
BANJO_RENUT_HOOK(sub_8212C448, GetRenderStateHook<78>);  // D3DDevice_GetRenderState_HighPrecisionBlendEnable1  // zuma-nx: the game's state table (Feeding Frenzy 8212C448)
BANJO_RENUT_HOOK(sub_8212C500, GetRenderStateHook<79>);  // D3DDevice_GetRenderState_HighPrecisionBlendEnable2  // zuma-nx: the game's state table (Feeding Frenzy 8212C500)
BANJO_RENUT_HOOK(sub_8212C5B8, GetRenderStateHook<80>);  // D3DDevice_GetRenderState_HighPrecisionBlendEnable3  // zuma-nx: the game's state table (Feeding Frenzy 8212C5B8)
BANJO_RENUT_HOOK(sub_8212C678, GetRenderStateHook<81>);  // D3DDevice_GetRenderState_HalfPixelOffset  // zuma-nx: the game's state table (Feeding Frenzy 8212C678)
BANJO_RENUT_HOOK(sub_8212C6B0, GetRenderStateHook<82>);  // D3DDevice_GetRenderState_PrimitiveResetEnable  // zuma-nx: the game's state table (Feeding Frenzy 8212C6B0)
BANJO_RENUT_HOOK(sub_8212C6E0, GetRenderStateHook<83>);  // D3DDevice_GetRenderState_PrimitiveResetIndex  // zuma-nx: the game's state table (Feeding Frenzy 8212C6E0)
BANJO_RENUT_HOOK(sub_8212C710, GetRenderStateHook<84>);  // D3DDevice_GetRenderState_AlphaToMaskEnable  // zuma-nx: the game's state table (Feeding Frenzy 8212C710)
BANJO_RENUT_HOOK(sub_8212C748, GetRenderStateHook<85>);  // D3DDevice_GetRenderState_AlphaToMaskOffsets  // zuma-nx: the game's state table (Feeding Frenzy 8212C748)
BANJO_RENUT_HOOK(sub_8212C788, GetRenderStateHook<86>);  // D3DDevice_GetRenderState_GuardBand_X  // zuma-nx: the game's state table (Feeding Frenzy 8212C788)
BANJO_RENUT_HOOK(sub_8212C7B8, GetRenderStateHook<87>);  // D3DDevice_GetRenderState_GuardBand_Y  // zuma-nx: the game's state table (Feeding Frenzy 8212C7B8)
BANJO_RENUT_HOOK(sub_8212C7E8, GetRenderStateHook<88>);  // D3DDevice_GetRenderState_DiscardBand_X  // zuma-nx: the game's state table (Feeding Frenzy 8212C7E8)
BANJO_RENUT_HOOK(sub_8212C818, GetRenderStateHook<89>);  // D3DDevice_GetRenderState_DiscardBand_Y  // zuma-nx: the game's state table (Feeding Frenzy 8212C818)
BANJO_RENUT_HOOK(sub_8212C848, GetRenderStateHook<90>);  // D3DDevice_GetRenderState_HiStencilEnable  // zuma-nx: the game's state table (Feeding Frenzy 8212C848)
BANJO_RENUT_HOOK(sub_8212C880, GetRenderStateHook<91>);  // D3DDevice_GetRenderState_HiStencilWriteEnable  // zuma-nx: the game's state table (Feeding Frenzy 8212C880)
BANJO_RENUT_HOOK(sub_8212C8B8, GetRenderStateHook<92>);  // D3DDevice_GetRenderState_HiStencilFunc  // zuma-nx: the game's state table (Feeding Frenzy 8212C8B8)
BANJO_RENUT_HOOK(sub_8212C8E8, GetRenderStateHook<93>);  // D3DDevice_GetRenderState_HiStencilRef  // zuma-nx: the game's state table (Feeding Frenzy 8212C8E8)
BANJO_RENUT_HOOK(sub_8212C8F8, GetRenderStateHook<94>);  // D3DDevice_GetRenderState_PresentInterval  // zuma-nx: the game's state table (Feeding Frenzy 8212C8F8)
BANJO_RENUT_HOOK(sub_8212C910, GetRenderStateHook<95>);  // D3DDevice_GetRenderState_PresentImmediateThreshold  // zuma-nx: the game's state table (Feeding Frenzy 8212C910)
// zuma-nx: not linked or not called in XDK 2135: BANJO_RENUT_HOOK(sub_XXXXXXXX, GetRenderStateHook<96>);  // D3DDevice_GetRenderState_HiZEnable (not in Feeding Frenzy's table)
// zuma-nx: not linked or not called in XDK 2135: BANJO_RENUT_HOOK(sub_XXXXXXXX, GetRenderStateHook<97>);  // D3DDevice_GetRenderState_HiZWriteEnable (Feeding Frenzy 820BAFD8)
// zuma-nx: not linked or not called in XDK 2135: BANJO_RENUT_HOOK(sub_XXXXXXXX, GetRenderStateHook<98>);  // D3DDevice_GetRenderState_LastPixel (not in Feeding Frenzy's table)
// zuma-nx: not linked or not called in XDK 2135: BANJO_RENUT_HOOK(sub_XXXXXXXX, GetRenderStateHook<99>);  // D3DDevice_GetRenderState_LineWidth (not in Feeding Frenzy's table)
// zuma-nx: not linked or not called in XDK 2135: BANJO_RENUT_HOOK(sub_XXXXXXXX, GetRenderStateHook<100>);  // D3DDevice_GetRenderState_Buffer2Frames (not in Feeding Frenzy's table)

// Sampler state getters (off_82E50BD0, field 0).
BANJO_RENUT_HOOK(sub_8212CF48, GetSamplerStateHook<0>);  // D3DDevice_GetSamplerState_AddressU  // zuma-nx: the game's state table (Feeding Frenzy 8212CF48)
BANJO_RENUT_HOOK(sub_8212CF98, GetSamplerStateHook<1>);  // D3DDevice_GetSamplerState_AddressV  // zuma-nx: the game's state table (Feeding Frenzy 8212CF98)
BANJO_RENUT_HOOK(sub_8212CFE8, GetSamplerStateHook<2>);  // D3DDevice_GetSamplerState_AddressW  // zuma-nx: the game's state table (Feeding Frenzy 8212CFE8)
BANJO_RENUT_HOOK(sub_8212CEF0, GetSamplerStateHook<3>);  // D3DDevice_GetSamplerState_BorderColor  // zuma-nx: the game's state table (Feeding Frenzy 8212CEF0)
BANJO_RENUT_HOOK(sub_8212CAB8, GetSamplerStateHook<4>);  // D3DDevice_GetSamplerState_MagFilter  // zuma-nx: the game's state table (Feeding Frenzy 8212CAB8)
BANJO_RENUT_HOOK(sub_8212C9A8, GetSamplerStateHook<5>);  // D3DDevice_GetSamplerState_MinFilter  // zuma-nx: the game's state table (Feeding Frenzy 8212C9A8)
BANJO_RENUT_HOOK(sub_8212CB78, GetSamplerStateHook<6>);  // D3DDevice_GetSamplerState_MipFilter  // zuma-nx: the game's state table (Feeding Frenzy 8212CB78)
BANJO_RENUT_HOOK(sub_8212CD60, GetSamplerStateHook<7>);  // D3DDevice_GetSamplerState_MipMapLodBias  // zuma-nx: the game's state table (Feeding Frenzy 8212CD60)
BANJO_RENUT_HOOK(sub_8212CE10, GetSamplerStateHook<8>);  // D3DDevice_GetSamplerState_MaxMipLevel  // zuma-nx: the game's state table (Feeding Frenzy 8212CE10)
BANJO_RENUT_HOOK(sub_8212CC48, GetSamplerStateHook<9>);  // D3DDevice_GetSamplerState_MaxAnisotropy  // zuma-nx: the game's state table (Feeding Frenzy 8212CC48)
BANJO_RENUT_HOOK(sub_8212CB28, GetSamplerStateHook<10>);  // D3DDevice_GetSamplerState_MagFilterZ  // zuma-nx: the game's state table (Feeding Frenzy 8212CB28)
BANJO_RENUT_HOOK(sub_8212CA20, GetSamplerStateHook<11>);  // D3DDevice_GetSamplerState_MinFilterZ  // zuma-nx: the game's state table (Feeding Frenzy 8212CA20)
BANJO_RENUT_HOOK(sub_8212CBD0, GetSamplerStateHook<12>);  // D3DDevice_GetSamplerState_SeparateZFilterEnable  // zuma-nx: the game's state table (Feeding Frenzy 8212CBD0)
BANJO_RENUT_HOOK(sub_8212CE90, GetSamplerStateHook<13>);  // D3DDevice_GetSamplerState_MinMipLevel  // zuma-nx: the game's state table (Feeding Frenzy 8212CE90)
BANJO_RENUT_HOOK(sub_8212D040, GetSamplerStateHook<14>);  // D3DDevice_GetSamplerState_TrilinearThreshold  // zuma-nx: the game's state table (Feeding Frenzy 8212D040)
BANJO_RENUT_HOOK(sub_8212CCB8, GetSamplerStateHook<15>);  // D3DDevice_GetSamplerState_AnisotropyBias  // zuma-nx: the game's state table (Feeding Frenzy 8212CCB8)
BANJO_RENUT_HOOK(sub_8212D098, GetSamplerStateHook<16>);  // D3DDevice_GetSamplerState_HGradientExpBias  // zuma-nx: the game's state table (Feeding Frenzy 8212D098)
BANJO_RENUT_HOOK(sub_8212D0F0, GetSamplerStateHook<17>);  // D3DDevice_GetSamplerState_VGradientExpBias  // zuma-nx: the game's state table (Feeding Frenzy 8212D0F0)
BANJO_RENUT_HOOK(sub_8212D148, GetSamplerStateHook<18>);  // D3DDevice_GetSamplerState_WhiteBorderColorW  // zuma-nx: the game's state table (Feeding Frenzy 8212D148)
BANJO_RENUT_HOOK(sub_8212D1A8, GetSamplerStateHook<19>);  // D3DDevice_GetSamplerState_PointBorderEnable  // zuma-nx: the game's state table (Feeding Frenzy 8212D1A8)
