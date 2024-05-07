#include "DeferredShading.h"

DeferredShading::DeferredShading(ID3D12Device* device, UINT width, 
					UINT height, DXGI_FORMAT format)
{
	md3dDevice = device;
	mWidth = width;
	mHeight = height;
	mViewport = { 0.0f, 0.0f, (float)width, (float)height, 0.0f, 1.0f };
	mScissorRect = { 0, 0, (int)width, (int)height };

	BuildResource();
}

UINT DeferredShading::Width() const
{
	return mWidth;
}

UINT DeferredShading::Height() const
{
	return mHeight;
}

DXGI_FORMAT DeferredShading::Format() const
{
	return mFormat;
}

ID3D12Resource* DeferredShading::Resource(int index)
{
	return mGBuffer[index].Get();
}

CD3DX12_GPU_DESCRIPTOR_HANDLE DeferredShading::Srv(int index) const
{
	return mhGpuSrv[index];
}

CD3DX12_CPU_DESCRIPTOR_HANDLE DeferredShading::Rtv(int index) const
{
	return mhCpuRtv[index];
}

D3D12_VIEWPORT DeferredShading::Viewport() const
{
	return mViewport;
}

D3D12_RECT DeferredShading::ScissorRect() const
{
	return mScissorRect;
}

void DeferredShading::BuildDescriptors(CD3DX12_CPU_DESCRIPTOR_HANDLE hCpuSrv, 
	CD3DX12_GPU_DESCRIPTOR_HANDLE hGpuSrv, 
	CD3DX12_CPU_DESCRIPTOR_HANDLE hCpuRtv)
{
	UINT mRtvDescriptorSize = md3dDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
	UINT mCbvSrvDescriptorSize = md3dDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	for (size_t i = 0; i < 4; i++)
	{
		mhCpuSrv[i] = hCpuSrv;
		mhGpuSrv[i] = hGpuSrv;
		mhCpuRtv[i] = hCpuRtv;
		hCpuSrv.Offset(1, mCbvSrvDescriptorSize);
		hGpuSrv.Offset(1, mCbvSrvDescriptorSize);
		hCpuRtv.Offset(1, mRtvDescriptorSize);
	}
	BuildDescriptors();
}

void DeferredShading::OnResize(UINT newWidth, UINT newHeight)
{
	if ((mWidth != newWidth) || (mHeight != newHeight))
	{
		mWidth = newWidth;
		mHeight = newHeight;

		BuildResource();

		// New resource, so we need new descriptors to that resource.
		BuildDescriptors();
	}
}

void DeferredShading::BuildDescriptors()
{
	D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
	srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srvDesc.Format = mFormat;
	srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	srvDesc.Texture2D.MostDetailedMip = 0;
	srvDesc.Texture2D.MipLevels = 1;

	// Create RTV for every gBuffer
	D3D12_RENDER_TARGET_VIEW_DESC rtvDesc;
	rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
	rtvDesc.Format = mFormat;
	rtvDesc.Texture2DArray.MipSlice = 0;
	rtvDesc.Texture2DArray.FirstArraySlice = 0;
	rtvDesc.Texture2DArray.ArraySize = 1;
	rtvDesc.Texture2DArray.PlaneSlice = 0;

	for (size_t i = 0; i < 4; i++)
	{
		md3dDevice->CreateShaderResourceView(mGBuffer[i].Get(), &srvDesc, mhCpuSrv[i]);
		md3dDevice->CreateRenderTargetView(mGBuffer[i].Get(), &rtvDesc, mhCpuRtv[i]);
	}
}

void DeferredShading::BuildResource()
{
	D3D12_RESOURCE_DESC texDesc;
	ZeroMemory(&texDesc, sizeof(D3D12_RESOURCE_DESC));
	texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	texDesc.Alignment = 0;
	texDesc.Width = mWidth;
	texDesc.Height = mHeight;
	texDesc.DepthOrArraySize = 4;
	texDesc.MipLevels = 1;
	texDesc.Format = mFormat;
	texDesc.SampleDesc.Count = 1;
	texDesc.SampleDesc.Quality = 0;
	texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	texDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

	// Clear the back buffer and depth buffer.
	float clearColor[] = { 0.0f, 0.0f, 0.0f, 1.0f };
	CD3DX12_CLEAR_VALUE clearValue(mFormat, clearColor);
	for (size_t i = 0; i < 4; i++)
	{
		// Create texture array with 4 elements
		ThrowIfFailed(md3dDevice->CreateCommittedResource(
			&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
			D3D12_HEAP_FLAG_NONE,
			&texDesc,
			D3D12_RESOURCE_STATE_GENERIC_READ,
			&clearValue,
			IID_PPV_ARGS(&mGBuffer[i])));
	}
}
