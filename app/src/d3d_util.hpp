#pragma once

#include "win_prelude.hpp"

#include <d3d11.h>
#include <wrl/client.h>

namespace touchfc {

struct Gpu {
  Microsoft::WRL::ComPtr<ID3D11Device> device;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
  bool warp = false;
};

struct FullscreenPass {
  Microsoft::WRL::ComPtr<ID3D11VertexShader> vs;
  Microsoft::WRL::ComPtr<ID3D11PixelShader> ps;
  Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler;
  Microsoft::WRL::ComPtr<ID3D11BlendState> blend;
};

bool create_gpu(Gpu& gpu);
bool compile_pass(ID3D11Device* device, bool premultiply, FullscreenPass& pass);
void draw_fullscreen(ID3D11DeviceContext* context, FullscreenPass& pass, ID3D11ShaderResourceView* srv,
                     const D3D11_VIEWPORT& viewport);

}  // namespace touchfc
