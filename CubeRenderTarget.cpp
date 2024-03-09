#include "CubeRenderTarget.h"

CubeRenderTarget::CubeRenderTarget(ID3D12Device* device, 
			UINT width, UINT height, DXGI_FORMAT format)
	: md3dDevice(device), mWidth(width), mHeight(height), mFormat(format),
	mViewport({ 0.0f, 0.0f, (float)width, (float)height, 0.0f, 1.0f }),
	mScissorRect({0, 0, (int)width, (int)height})
{
	BuildResource();
}

ID3D12Resource* CubeRenderTarget::Resource()
{
	return dynamicCubemap.Get();
}

CD3DX12_GPU_DESCRIPTOR_HANDLE CubeRenderTarget::Srv()
{
	return gpuSrv;
}

CD3DX12_CPU_DESCRIPTOR_HANDLE CubeRenderTarget::Rtv(int faceIndex)
{
	return cpuRtv[faceIndex];
}

DXGI_FORMAT CubeRenderTarget::Format()
{
	return mFormat;
}

D3D12_VIEWPORT CubeRenderTarget::Viewport() const
{
	return mViewport;
}

D3D12_RECT CubeRenderTarget::ScissorRect() const
{
	return mScissorRect;
}

void CubeRenderTarget::BuildDescriptors(CD3DX12_CPU_DESCRIPTOR_HANDLE hCpuSrv, 
	CD3DX12_GPU_DESCRIPTOR_HANDLE hGpuSrv, CD3DX12_CPU_DESCRIPTOR_HANDLE hCpuRtv[6])
{
	cpuSrv = hCpuSrv;
	gpuSrv = hGpuSrv;
	for (int i = 0; i < 6; i++)
	{
		cpuRtv[i] = hCpuRtv[i];
	}
	
	BuildDescriptors();
}

void CubeRenderTarget::OnResize(UINT newWidth, UINT newHeight)
{
	if (mWidth != newWidth || mHeight != newHeight)
	{
		mWidth = newWidth;
		mHeight = newHeight;
		BuildResource();
		BuildDescriptors();
	}
}

void CubeRenderTarget::BuildDescriptors()
{
	D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
	srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srvDesc.Format = mFormat;
	srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
	srvDesc.TextureCube.MostDetailedMip = 0;
	srvDesc.TextureCube.MipLevels = 1;
	srvDesc.TextureCube.ResourceMinLODClamp = 0.0f;
	// Create dynamicCubemap srv
	md3dDevice->CreateShaderResourceView(dynamicCubemap.Get(), &srvDesc, cpuSrv);

	// Create dynamicCubemap rtv
	for (int i = 0; i < 6; i++)
	{
		D3D12_RENDER_TARGET_VIEW_DESC rtvDesc;
		rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
		rtvDesc.Format = mFormat;
		rtvDesc.Texture2DArray.MipSlice = 0;
		rtvDesc.Texture2DArray.PlaneSlice = 0;
		rtvDesc.Texture2DArray.FirstArraySlice = i;
		rtvDesc.Texture2DArray.ArraySize = 1;
		md3dDevice->CreateRenderTargetView(dynamicCubemap.Get(), &rtvDesc, cpuRtv[i]);
	}
}

void CubeRenderTarget::BuildResource()
{
	D3D12_RESOURCE_DESC cubemapDesc;
	ZeroMemory(&cubemapDesc, sizeof(D3D12_RESOURCE_DESC));
	cubemapDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	cubemapDesc.Alignment = 0;
	cubemapDesc.Width = mWidth;
	cubemapDesc.Height = mHeight;
	cubemapDesc.DepthOrArraySize = 6;
	cubemapDesc.MipLevels = 1;
	cubemapDesc.Format = mFormat;
	cubemapDesc.SampleDesc.Count = 1;
	cubemapDesc.SampleDesc.Quality = 0;
	cubemapDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	cubemapDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

	// Clear the back buffer and depth buffer.
	D3D12_CLEAR_VALUE clearValue = {};
	clearValue.Format = mFormat;
	clearValue.Color[0] = 0;
	clearValue.Color[1] = 0;
	clearValue.Color[2] = 0;
	clearValue.Color[3] = 1;

	ThrowIfFailed(md3dDevice->CreateCommittedResource(
		&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
		D3D12_HEAP_FLAG_NONE,
		&cubemapDesc,
		D3D12_RESOURCE_STATE_GENERIC_READ,
		&clearValue,
		IID_PPV_ARGS(&dynamicCubemap)));

}
