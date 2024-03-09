#pragma once
#include "Common/d3dUtil.h"

enum class CubeMapFace : int
{
	PositionX = 0,
	NegativeX = 1, 
	PositionY = 2,
	NegativeY = 3,
	PositionZ = 4,
	NegativeZ = 5
};

class CubeRenderTarget 
{
public:
	CubeRenderTarget(
		ID3D12Device* device,
		UINT width, UINT height,
		DXGI_FORMAT format
	);

	// 禁止通过拷贝构造函数来创建新对象
	CubeRenderTarget(const CubeRenderTarget& rhs) = delete;
	CubeRenderTarget& operator=(const CubeRenderTarget& rhs) = delete;
	~CubeRenderTarget() = default;

	ID3D12Resource* Resource();
	CD3DX12_GPU_DESCRIPTOR_HANDLE Srv();
	CD3DX12_CPU_DESCRIPTOR_HANDLE Rtv(int faceIndex);

	DXGI_FORMAT Format();
	D3D12_VIEWPORT Viewport()const;

	D3D12_RECT ScissorRect()const;

	void BuildDescriptors(
		CD3DX12_CPU_DESCRIPTOR_HANDLE hCpuSrv,
		CD3DX12_GPU_DESCRIPTOR_HANDLE hGpuSrv,
		CD3DX12_CPU_DESCRIPTOR_HANDLE hCpuRtv[6]);

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
	DXGI_FORMAT mFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

	CD3DX12_CPU_DESCRIPTOR_HANDLE cpuSrv;
	CD3DX12_GPU_DESCRIPTOR_HANDLE gpuSrv;
	CD3DX12_CPU_DESCRIPTOR_HANDLE cpuRtv[6];

	Microsoft::WRL::ComPtr<ID3D12Resource> dynamicCubemap = nullptr;

};