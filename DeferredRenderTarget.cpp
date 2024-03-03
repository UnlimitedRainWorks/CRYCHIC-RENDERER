#include "DeferredRenderTarget.h"

DeferredRenderTarget::DeferredRenderTarget(
	ID3D12Device* device,
	UINT width, UINT height,
	DXGI_FORMAT format
) 
{
	md3dDevice = device;
	mWidth = width;
	mHeight = height;
	mViewport = { 0.0f, 0.0f, (float)width, (float)height, 0.0f, 1.0f };
	mScissorRect = { 0, 0, (int)width, (int)height };

	BuildResource();
}

ID3D12Resource* DeferredRenderTarget::gBuffer0Resource()
{
	return gBuffer0.Get();
}

ID3D12Resource* DeferredRenderTarget::gBuffer1Resource()
{
	return gBuffer1.Get();
}

ID3D12Resource* DeferredRenderTarget::gBuffer2Resource()
{
	return gBuffer2.Get();
}

ID3D12Resource* DeferredRenderTarget::gBuffer3Resource()
{
	return gBuffer3.Get();
}

CD3DX12_GPU_DESCRIPTOR_HANDLE DeferredRenderTarget::gBuffer0Srv()
{
	return gBuffer0GpuSrv;
}

CD3DX12_GPU_DESCRIPTOR_HANDLE DeferredRenderTarget::gBuffer1Srv()
{
	return gBuffer1GpuSrv;
}

CD3DX12_GPU_DESCRIPTOR_HANDLE DeferredRenderTarget::gBuffer2Srv()
{
	return gBuffer2GpuSrv;
}

CD3DX12_GPU_DESCRIPTOR_HANDLE DeferredRenderTarget::gBuffer3Srv()
{
	return gBuffer3GpuSrv;
}

CD3DX12_CPU_DESCRIPTOR_HANDLE DeferredRenderTarget::gBuffer0Rtv()
{
	return gBuffer0CpuRtv;
}

CD3DX12_CPU_DESCRIPTOR_HANDLE DeferredRenderTarget::gBuffer1Rtv()
{
	return gBuffer1CpuRtv;
}

CD3DX12_CPU_DESCRIPTOR_HANDLE DeferredRenderTarget::gBuffer2Rtv()
{
	return gBuffer2CpuRtv;
}

CD3DX12_CPU_DESCRIPTOR_HANDLE DeferredRenderTarget::gBuffer3Rtv()
{
	return gBuffer3CpuRtv;
}

DXGI_FORMAT DeferredRenderTarget::Format()
{
	return mFormat;
}

D3D12_VIEWPORT DeferredRenderTarget::Viewport()const
{
	return mViewport;
}

D3D12_RECT DeferredRenderTarget::ScissorRect()const
{
	return mScissorRect;
}

void DeferredRenderTarget::BuildDescriptors(CD3DX12_CPU_DESCRIPTOR_HANDLE hCpuSrv,
	CD3DX12_GPU_DESCRIPTOR_HANDLE hGpuSrv,
	CD3DX12_CPU_DESCRIPTOR_HANDLE hCpuRtv)
{
	// Save references to the descriptors. 
	UINT mRtvDescriptorSize = md3dDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
	UINT mCbvSrvDescriptorSize = md3dDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	gBuffer0CpuSrv = hCpuSrv;
	hCpuSrv.Offset(1, mCbvSrvDescriptorSize);
	gBuffer1CpuSrv = hCpuSrv;
	hCpuSrv.Offset(1, mCbvSrvDescriptorSize);
	gBuffer2CpuSrv = hCpuSrv;
	hCpuSrv.Offset(1, mCbvSrvDescriptorSize);
	gBuffer3CpuSrv = hCpuSrv;
	hCpuSrv.Offset(1, mCbvSrvDescriptorSize);

	gBuffer0GpuSrv = hGpuSrv;
	hGpuSrv.Offset(1, mCbvSrvDescriptorSize);
	gBuffer1GpuSrv = hGpuSrv;
	hGpuSrv.Offset(1, mCbvSrvDescriptorSize);
	gBuffer2GpuSrv = hGpuSrv;
	hGpuSrv.Offset(1, mCbvSrvDescriptorSize);
	gBuffer3GpuSrv = hGpuSrv;
	hGpuSrv.Offset(1, mCbvSrvDescriptorSize);

	gBuffer0CpuRtv = hCpuRtv;
	hCpuRtv.Offset(1, mRtvDescriptorSize);
	gBuffer1CpuRtv = hCpuRtv;
	hCpuRtv.Offset(1, mRtvDescriptorSize);
	gBuffer2CpuRtv = hCpuRtv;
	hCpuRtv.Offset(1, mRtvDescriptorSize);
	gBuffer3CpuRtv = hCpuRtv;

	//  Create the descriptors
	BuildDescriptors();
}

void DeferredRenderTarget::OnResize(UINT newWidth, UINT newHeight)
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

void DeferredRenderTarget::BuildDescriptors()
{
	D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
	srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srvDesc.Format = mFormat;
	srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	srvDesc.Texture2D.MostDetailedMip = 0;
	srvDesc.Texture2D.MipLevels = 1;

	// Create SRV for every gBuffer
	md3dDevice->CreateShaderResourceView(gBuffer0.Get(), &srvDesc, gBuffer0CpuSrv);
	md3dDevice->CreateShaderResourceView(gBuffer1.Get(), &srvDesc, gBuffer1CpuSrv);
	md3dDevice->CreateShaderResourceView(gBuffer2.Get(), &srvDesc, gBuffer2CpuSrv);
	md3dDevice->CreateShaderResourceView(gBuffer3.Get(), &srvDesc, gBuffer3CpuSrv);

	// Create RTV for every gBuffer
	D3D12_RENDER_TARGET_VIEW_DESC rtvDesc;
	rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
	rtvDesc.Format = mFormat;
	rtvDesc.Texture2DArray.MipSlice = 0;
	rtvDesc.Texture2DArray.FirstArraySlice = 0;
	rtvDesc.Texture2DArray.ArraySize = 1;
	rtvDesc.Texture2DArray.PlaneSlice = 0;
	md3dDevice->CreateRenderTargetView(gBuffer0.Get(), &rtvDesc, gBuffer0CpuRtv);
	md3dDevice->CreateRenderTargetView(gBuffer1.Get(), &rtvDesc, gBuffer1CpuRtv);
	md3dDevice->CreateRenderTargetView(gBuffer2.Get(), &rtvDesc, gBuffer2CpuRtv);
	md3dDevice->CreateRenderTargetView(gBuffer3.Get(), &rtvDesc, gBuffer3CpuRtv);

}

void DeferredRenderTarget::BuildResource()
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
	D3D12_CLEAR_VALUE clearValue = {};
	clearValue.Format = mFormat;
	clearValue.Color[0] = 0.0f;
	clearValue.Color[1] = 0.0f;
	clearValue.Color[2] = 0.0f;
	clearValue.Color[3] = 1.0f;

	// Create texture array with 4 elements
	ThrowIfFailed(md3dDevice->CreateCommittedResource(
		&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
		D3D12_HEAP_FLAG_NONE,
		&texDesc,
		D3D12_RESOURCE_STATE_GENERIC_READ,
		&clearValue,
		IID_PPV_ARGS(&gBuffer0)));

	ThrowIfFailed(md3dDevice->CreateCommittedResource(
		&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
		D3D12_HEAP_FLAG_NONE,
		&texDesc,
		D3D12_RESOURCE_STATE_GENERIC_READ,
		&clearValue,
		IID_PPV_ARGS(&gBuffer1)));

	ThrowIfFailed(md3dDevice->CreateCommittedResource(
		&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
		D3D12_HEAP_FLAG_NONE,
		&texDesc,
		D3D12_RESOURCE_STATE_GENERIC_READ,
		&clearValue,
		IID_PPV_ARGS(&gBuffer2)));

	ThrowIfFailed(md3dDevice->CreateCommittedResource(
		&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
		D3D12_HEAP_FLAG_NONE,
		&texDesc,
		D3D12_RESOURCE_STATE_GENERIC_READ,
		&clearValue,
		IID_PPV_ARGS(&gBuffer3)));
}