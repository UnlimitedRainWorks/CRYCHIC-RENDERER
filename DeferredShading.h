#pragma once
#include "Common/d3dUtil.h"

class DeferredShading
{
public:
	DeferredShading(ID3D12Device* device, UINT width, UINT height,
					DXGI_FORMAT format);
	DeferredShading(const DeferredShading& rhs) = delete;
	DeferredShading& operator=(const DeferredShading& rhs) = delete;
	virtual ~DeferredShading() = default;

	UINT Width() const;
	UINT Height() const;
	DXGI_FORMAT Format() const;
	ID3D12Resource* Resource(int index);
	CD3DX12_GPU_DESCRIPTOR_HANDLE Srv(int index)const;
	CD3DX12_CPU_DESCRIPTOR_HANDLE Rtv(int index)const;

	D3D12_VIEWPORT Viewport()const;
	D3D12_RECT ScissorRect()const;

	void BuildDescriptors(
		CD3DX12_CPU_DESCRIPTOR_HANDLE hCpuSrv,
		CD3DX12_GPU_DESCRIPTOR_HANDLE hGpuSrv,
		CD3DX12_CPU_DESCRIPTOR_HANDLE hCpuRtv);

	void OnResize(UINT newWidth, UINT newHeight);
	
	void BuildDescriptors();

private:	
	void BuildResource();

private:
	ID3D12Device* md3dDevice = nullptr;
	D3D12_VIEWPORT mViewport;
	D3D12_RECT mScissorRect;
	UINT mWidth = 0;
	UINT mHeight = 0;
	DXGI_FORMAT mFormat = DXGI_FORMAT_R32G32B32A32_FLOAT;
	CD3DX12_CPU_DESCRIPTOR_HANDLE mhCpuSrv[4];
	CD3DX12_GPU_DESCRIPTOR_HANDLE mhGpuSrv[4];
	CD3DX12_CPU_DESCRIPTOR_HANDLE mhCpuRtv[4];
	Microsoft::WRL::ComPtr<ID3D12Resource> mGBuffer[4];
};