#include "ShadowMap.h"

ShadowMap::ShadowMap(ID3D12Device* device, UINT width, UINT height)
{
	md3dDevice = device;
	mWidth = width;
	mHeight = height;
	mViewport = { 0.0f, 0.0f, (float)width, (float)height, 0.0f, 1.0f };
	mScissorRect = { 0, 0, (int)width, (int)height };
	BuildResource();
}

UINT ShadowMap::Width() const
{
	return this->mWidth;
}

UINT ShadowMap::Height() const
{
	return mHeight;
}

ID3D12Resource* ShadowMap::Resource(int index)
{
	return mShadowMap[index].Get();
}

CD3DX12_GPU_DESCRIPTOR_HANDLE ShadowMap::Srv(int index) const
{
	return mhGpuSrv[index];
}

CD3DX12_CPU_DESCRIPTOR_HANDLE ShadowMap::Dsv(int index) const
{
	return mhCpuDsv[index];
}

D3D12_VIEWPORT ShadowMap::Viewport() const
{
	return mViewport;
}

D3D12_RECT ShadowMap::ScissorRect() const
{
	return mScissorRect;
}

void ShadowMap::BuildDescriptors(CD3DX12_CPU_DESCRIPTOR_HANDLE hCpuSrv, 
	CD3DX12_GPU_DESCRIPTOR_HANDLE hGpuSrv, 
	CD3DX12_CPU_DESCRIPTOR_HANDLE hCpuDsv)
{
	UINT mDsvDescriptorSize = md3dDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
	UINT mCbvSrvUavDescriptorSize = md3dDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

	for (size_t i = 0; i < 4 + 6 + 2; i++)
	{
		mhCpuSrv[i] = CD3DX12_CPU_DESCRIPTOR_HANDLE(hCpuSrv, i, mCbvSrvUavDescriptorSize);
		mhGpuSrv[i] = CD3DX12_GPU_DESCRIPTOR_HANDLE(hGpuSrv, i, mCbvSrvUavDescriptorSize);
		mhCpuDsv[i] = CD3DX12_CPU_DESCRIPTOR_HANDLE(hCpuDsv, i, mDsvDescriptorSize);
	}
	//mhCpuSrv = hCpuSrv;
	//mhGpuSrv = hGpuSrv;
	//mhCpuDsv = hCpuDsv;
	BuildDescriptors();
}

void ShadowMap::OnResize(UINT newWidth, UINT newHeight)
{
	if ((mWidth != newWidth) || (mHeight != newHeight))
	{
		mWidth = newWidth;
		mHeight = newHeight;
		BuildResource();
		BuildDescriptors();
	}
}

void ShadowMap::BuildDescriptors()
{
	// Create SRV, so we can sample the shadowmap in shader program.
	D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
	srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srvDesc.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
	srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	srvDesc.Texture2D.MostDetailedMip = 0;
	srvDesc.Texture2D.MipLevels = 1;
	srvDesc.Texture2D.ResourceMinLODClamp = 0.0f;
	srvDesc.Texture2D.PlaneSlice = 0;
	//md3dDevice->CreateShaderResourceView(mShadowMap.Get(), &srvDesc, mhCpuSrv);

	D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
	dsvDesc.Flags = D3D12_DSV_FLAG_NONE;
	dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
	dsvDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
	dsvDesc.Texture2D.MipSlice = 0;
	//md3dDevice->CreateDepthStencilView(mShadowMap.Get(), &dsvDesc, mhCpuDsv);
	for (size_t i = 0; i < 4 + 6 + 2; i++)
	{
		md3dDevice->CreateShaderResourceView(mShadowMap[i].Get(), &srvDesc, mhCpuSrv[i]);
		md3dDevice->CreateDepthStencilView(mShadowMap[i].Get(), &dsvDesc, mhCpuDsv[i]);
	}
}

void ShadowMap::BuildResource()
{
	D3D12_RESOURCE_DESC texDesc;
	ZeroMemory(&texDesc, sizeof(D3D12_RESOURCE_DESC));
	texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	texDesc.Alignment = 0;
	texDesc.Width = mWidth;
	texDesc.Height = mHeight;
	texDesc.DepthOrArraySize = 4 + 6 + 2;
	texDesc.MipLevels = 1;
	texDesc.Format = mFormat;
	texDesc.SampleDesc.Count = 1;
	texDesc.SampleDesc.Quality = 0;
	texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	texDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

	D3D12_CLEAR_VALUE optClear;
	optClear.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
	optClear.DepthStencil.Depth = 1.0f;
	optClear.DepthStencil.Stencil = 0;

	for (size_t i = 0; i < 12; i++)
	{
		ThrowIfFailed(md3dDevice->CreateCommittedResource(
				&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
				D3D12_HEAP_FLAG_NONE,
				&texDesc,
				D3D12_RESOURCE_STATE_GENERIC_READ,
				&optClear,
				IID_PPV_ARGS(&mShadowMap[i])));
	}
	
}
