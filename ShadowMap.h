#pragma once
#include "Common/d3dUtil.h"
#define MaxLights 16

class ShadowMap {
public:
	ShadowMap(ID3D12Device* device,
		UINT width, UINT height, int shadowMapSize);

	ShadowMap(const ShadowMap& rhs) = delete;
	ShadowMap& operator=(const ShadowMap& rhs) = delete;
	~ShadowMap() = default;

	UINT Width() const;
	UINT Height() const;
	ID3D12Resource* Resource(int index);
	ID3D12Resource* Resource();
	CD3DX12_GPU_DESCRIPTOR_HANDLE Srv(int index)const;
	CD3DX12_GPU_DESCRIPTOR_HANDLE Srv()const;
	CD3DX12_CPU_DESCRIPTOR_HANDLE Dsv(int index)const;
	CD3DX12_CPU_DESCRIPTOR_HANDLE Dsv()const;

	D3D12_VIEWPORT Viewport()const;
	D3D12_RECT ScissorRect()const;

	void BuildDescriptors(
		CD3DX12_CPU_DESCRIPTOR_HANDLE hCpuSrv,
		CD3DX12_GPU_DESCRIPTOR_HANDLE hGpuSrv,
		CD3DX12_CPU_DESCRIPTOR_HANDLE hCpuDsv);

	void OnResize(UINT newWidth, UINT newHeight);

private:
	void BuildDescriptors();
	void BuildResource();

private:
	ID3D12Device* md3dDevice = nullptr;

	D3D12_VIEWPORT mViewport;
	D3D12_RECT mScissorRect;

	UINT mWidth = 0;
	UINT mHeight = 0;
	DXGI_FORMAT mFormat = DXGI_FORMAT_R24G8_TYPELESS;
	int mShadowMapSize = 0;
	CD3DX12_CPU_DESCRIPTOR_HANDLE mhCpuSrv[MaxLights];
	CD3DX12_GPU_DESCRIPTOR_HANDLE mhGpuSrv[MaxLights];
	CD3DX12_CPU_DESCRIPTOR_HANDLE mhCpuDsv[MaxLights];
	UINT mCbvSrvDescriptorSize = 0;
	UINT mDsvDescriptorSize = 0;

	Microsoft::WRL::ComPtr<ID3D12Resource> mShadowMap = nullptr;
	Microsoft::WRL::ComPtr<ID3D12Resource> mShadowMaps[MaxLights];
};