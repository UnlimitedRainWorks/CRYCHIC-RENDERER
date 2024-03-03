#pragma once
#include "Common/d3dUtil.h"

class DeferredRenderTarget
{
public:
	DeferredRenderTarget(
		ID3D12Device* device,
		UINT width, UINT height,
		DXGI_FORMAT format
	);

	DeferredRenderTarget(const DeferredRenderTarget& rhs) = delete;
	DeferredRenderTarget& operator=(const DeferredRenderTarget& rhs) = delete;
	~DeferredRenderTarget() = default;

	ID3D12Resource* gBuffer0Resource();
	ID3D12Resource* gBuffer1Resource();
	ID3D12Resource* gBuffer2Resource();
	ID3D12Resource* gBuffer3Resource();

	CD3DX12_GPU_DESCRIPTOR_HANDLE gBuffer0Srv();
	CD3DX12_GPU_DESCRIPTOR_HANDLE gBuffer1Srv();
	CD3DX12_GPU_DESCRIPTOR_HANDLE gBuffer2Srv();
	CD3DX12_GPU_DESCRIPTOR_HANDLE gBuffer3Srv();
	CD3DX12_CPU_DESCRIPTOR_HANDLE gBuffer0Rtv();
	CD3DX12_CPU_DESCRIPTOR_HANDLE gBuffer1Rtv();
	CD3DX12_CPU_DESCRIPTOR_HANDLE gBuffer2Rtv();
	CD3DX12_CPU_DESCRIPTOR_HANDLE gBuffer3Rtv();
	DXGI_FORMAT Format();
	D3D12_VIEWPORT Viewport()const;
	D3D12_RECT ScissorRect()const;

	void BuildDescriptors(
		CD3DX12_CPU_DESCRIPTOR_HANDLE hCpuSrv,
		CD3DX12_GPU_DESCRIPTOR_HANDLE hGpuSrv,
		CD3DX12_CPU_DESCRIPTOR_HANDLE hCpuRtv);

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
	DXGI_FORMAT mFormat = DXGI_FORMAT_R32G32B32A32_FLOAT;

	CD3DX12_CPU_DESCRIPTOR_HANDLE gBuffer0CpuSrv;
	CD3DX12_CPU_DESCRIPTOR_HANDLE gBuffer1CpuSrv;
	CD3DX12_CPU_DESCRIPTOR_HANDLE gBuffer2CpuSrv;
	CD3DX12_CPU_DESCRIPTOR_HANDLE gBuffer3CpuSrv;

	CD3DX12_GPU_DESCRIPTOR_HANDLE gBuffer0GpuSrv;
	CD3DX12_GPU_DESCRIPTOR_HANDLE gBuffer1GpuSrv;
	CD3DX12_GPU_DESCRIPTOR_HANDLE gBuffer2GpuSrv;
	CD3DX12_GPU_DESCRIPTOR_HANDLE gBuffer3GpuSrv;

	CD3DX12_CPU_DESCRIPTOR_HANDLE gBuffer0CpuRtv;
	CD3DX12_CPU_DESCRIPTOR_HANDLE gBuffer1CpuRtv;
	CD3DX12_CPU_DESCRIPTOR_HANDLE gBuffer2CpuRtv;
	CD3DX12_CPU_DESCRIPTOR_HANDLE gBuffer3CpuRtv;

	Microsoft::WRL::ComPtr<ID3D12Resource> gBuffer0 = nullptr;
	Microsoft::WRL::ComPtr<ID3D12Resource> gBuffer1 = nullptr;
	Microsoft::WRL::ComPtr<ID3D12Resource> gBuffer2 = nullptr;
	Microsoft::WRL::ComPtr<ID3D12Resource> gBuffer3 = nullptr;

};