#include "d3d_util.hpp"
#include "log.hpp"

#include <d3dcompiler.h>

namespace touchfc {
namespace {

const char kHlsl[] = R"(
Texture2D tex0 : register(t0);
SamplerState samp0 : register(s0);
struct VOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VOut vs_main(uint id : SV_VertexID) {
  float2 uv = float2((id << 1) & 2, id & 2);
  VOut o;
  o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0, 1);
  o.uv = uv;
  return o;
}
float4 ps_copy(VOut i) : SV_Target { return tex0.Sample(samp0, i.uv); }
float4 ps_premul(VOut i) : SV_Target {
  float4 c = tex0.Sample(samp0, i.uv);
  return float4(c.rgb * c.a, c.a);
}
)";

bool compile_shader(const char* entry, const char* target, Microsoft::WRL::ComPtr<ID3DBlob>& blob) {
  Microsoft::WRL::ComPtr<ID3DBlob> errors;
  const HRESULT hr = D3DCompile(kHlsl, sizeof(kHlsl) - 1, "touchfc.hlsl", nullptr, nullptr, entry, target,
                                D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, blob.GetAddressOf(),
                                errors.GetAddressOf());
  if (FAILED(hr)) {
    if (errors) log_line(static_cast<const char*>(errors->GetBufferPointer()));
    return false;
  }
  return true;
}

}  // namespace

bool create_gpu(Gpu& gpu) {
  UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
  HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, nullptr, 0,
                                 D3D11_SDK_VERSION, gpu.device.GetAddressOf(), nullptr,
                                 gpu.context.GetAddressOf());
  if (FAILED(hr)) {
    hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, nullptr, 0, D3D11_SDK_VERSION,
                           gpu.device.GetAddressOf(), nullptr, gpu.context.GetAddressOf());
    gpu.warp = SUCCEEDED(hr);
  }
  if (FAILED(hr)) {
    log_line("D3D11CreateDevice failed");
    return false;
  }
  if (gpu.warp) log_line("using the WARP software device");
  return true;
}

bool compile_pass(ID3D11Device* device, bool premultiply, FullscreenPass& pass) {
  Microsoft::WRL::ComPtr<ID3DBlob> vs;
  Microsoft::WRL::ComPtr<ID3DBlob> ps;
  if (!compile_shader("vs_main", "vs_5_0", vs)) return false;
  if (!compile_shader(premultiply ? "ps_premul" : "ps_copy", "ps_5_0", ps)) return false;
  if (FAILED(device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr,
                                        pass.vs.GetAddressOf()))) {
    return false;
  }
  if (FAILED(device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr,
                                       pass.ps.GetAddressOf()))) {
    return false;
  }
  D3D11_SAMPLER_DESC samp{};
  samp.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
  samp.AddressU = samp.AddressV = samp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
  if (FAILED(device->CreateSamplerState(&samp, pass.sampler.GetAddressOf()))) return false;

  D3D11_BLEND_DESC blend{};
  auto& rt = blend.RenderTarget[0];
  rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
  if (premultiply) {
    rt.BlendEnable = TRUE;
    rt.SrcBlend = D3D11_BLEND_ONE;
    rt.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    rt.BlendOp = D3D11_BLEND_OP_ADD;
    rt.SrcBlendAlpha = D3D11_BLEND_ONE;
    rt.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
  }
  return SUCCEEDED(device->CreateBlendState(&blend, pass.blend.GetAddressOf()));
}

void draw_fullscreen(ID3D11DeviceContext* context, FullscreenPass& pass, ID3D11ShaderResourceView* srv,
                     const D3D11_VIEWPORT& viewport) {
  context->RSSetViewports(1, &viewport);
  context->IASetInputLayout(nullptr);
  context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  context->VSSetShader(pass.vs.Get(), nullptr, 0);
  context->PSSetShader(pass.ps.Get(), nullptr, 0);
  context->PSSetShaderResources(0, 1, &srv);
  context->PSSetSamplers(0, 1, pass.sampler.GetAddressOf());
  context->OMSetBlendState(pass.blend.Get(), nullptr, 0xffffffffu);
  context->Draw(3, 0);
  ID3D11ShaderResourceView* none = nullptr;
  context->PSSetShaderResources(0, 1, &none);
}

}  // namespace touchfc
