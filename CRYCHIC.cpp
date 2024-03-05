#include "CRYCHIC.h"

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE prevInstance,
	PSTR cmdLine, int showCmd)
{
	// Enable run-time memory check for debug builds.
#if defined(DEBUG) | defined(_DEBUG)
	_CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
#endif

	try
	{
		CRYCHIC theApp(hInstance);
		if (!theApp.Initialize())
			return 0;

		return theApp.Run();
	}
	catch (DxException& e)
	{
		MessageBox(nullptr, e.ToString().c_str(), L"HR Failed", MB_OK);
		return 0;
	}
}

CRYCHIC::CRYCHIC(HINSTANCE hInstance)
	: D3DApp(hInstance)
{
}

CRYCHIC::~CRYCHIC()
{
	if (md3dDevice != nullptr)
		FlushCommandQueue();
}

bool CRYCHIC::Initialize()
{
	if (!D3DApp::Initialize())
		return false;

	// Reset the command list to prep for initialization commands.
	ThrowIfFailed(mCommandList->Reset(mDirectCmdListAlloc.Get(), nullptr));

	// Get the increment size of a descriptor in this heap type.  This is hardware specific, 
	// so we have to query this information.
	mCbvSrvDescriptorSize = md3dDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

	mDeferred = std::make_unique<DeferredRenderTarget>(md3dDevice.Get(),
		mClientWidth, mClientHeight, DXGI_FORMAT_R32G32B32A32_FLOAT);

	mCamera.SetPosition(0.0f, 2.0f, -15.0f);

	LoadTextures();
	BuildRootSignature();
	BuildDescriptorHeaps();
	BuildShadersAndInputLayout();
	BuildShapeGeometry();
	BuildGeometry("Models/skull.txt");
	BuildGeometry("Models/car.txt");
	BuildMaterials();
	BuildRenderItems();
	//BuildInstancingSceneRenderItems();
	BuildFrameResources();
	BuildPSOs();

	// Execute the initialization commands.
	ThrowIfFailed(mCommandList->Close());
	ID3D12CommandList* cmdsLists[] = { mCommandList.Get() };
	mCommandQueue->ExecuteCommandLists(_countof(cmdsLists), cmdsLists);

	// Wait until initialization is complete.
	FlushCommandQueue();

	return true;
}

void CRYCHIC::CreateRtvAndDsvDescriptorHeaps()
{
	// Add +4 RTV for gbuffer render target.
	const int gBufferSize = 4;
	D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc;
	rtvHeapDesc.NumDescriptors = SwapChainBufferCount + gBufferSize;
	rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
	rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
	rtvHeapDesc.NodeMask = 0;
	ThrowIfFailed(md3dDevice->CreateDescriptorHeap(
		&rtvHeapDesc, IID_PPV_ARGS(mRtvHeap.GetAddressOf())));

	// 保持不变即可，相机位置未发生改变
	D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc;
	dsvHeapDesc.NumDescriptors = 1;
	dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
	dsvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
	dsvHeapDesc.NodeMask = 0;
	ThrowIfFailed(md3dDevice->CreateDescriptorHeap(
		&dsvHeapDesc, IID_PPV_ARGS(mDsvHeap.GetAddressOf())));
}

void CRYCHIC::OnResize()
{
	D3DApp::OnResize();
	//mDeferred->OnResize(mClientWidth, mClientHeight);
	mCamera.SetLens(0.25f * MathHelper::Pi, AspectRatio(), 1.0f, 1000.0f);

	BoundingFrustum::CreateFromMatrix(mCamFrustum, mCamera.GetProj());
}

void CRYCHIC::Update(const GameTimer& gt)
{
	OnKeyboardInput(gt);

	// Cycle through the circular frame resource array.
	mCurrFrameResourceIndex = (mCurrFrameResourceIndex + 1) % gNumFrameResources;
	mCurrFrameResource = mFrameResources[mCurrFrameResourceIndex].get();

	// Has the GPU finished processing the commands of the current frame resource?
	// If not, wait until the GPU has completed commands up to this fence point.
	if (mCurrFrameResource->Fence != 0 && mFence->GetCompletedValue() < mCurrFrameResource->Fence)
	{
		HANDLE eventHandle = CreateEventEx(nullptr, false, false, EVENT_ALL_ACCESS);
		ThrowIfFailed(mFence->SetEventOnCompletion(mCurrFrameResource->Fence, eventHandle));
		WaitForSingleObject(eventHandle, INFINITE);
		CloseHandle(eventHandle);
	}

	AnimateMaterials(gt);
	UpdateInstanceData(gt);
	UpdateMaterialBuffer(gt);
	UpdateMainPassCB(gt);
}

void CRYCHIC::Draw(const GameTimer& gt)
{
	if (isDeferred)
	{
		auto cmdListAlloc = mCurrFrameResource->CmdListAlloc;

		// Reuse the memory associated with command recording.
		// We can only reset when the associated command lists have finished execution on the GPU.
		ThrowIfFailed(cmdListAlloc->Reset());

		// Geometry pass
		ThrowIfFailed(mCommandList->Reset(cmdListAlloc.Get(), mPSOs["gBuffer"].Get()));

		ID3D12DescriptorHeap* descriptorHeaps[] = { mSrvDescriptorHeap.Get() };
		mCommandList->SetDescriptorHeaps(_countof(descriptorHeaps), descriptorHeaps);

		mCommandList->SetGraphicsRootSignature(mRootSignature.Get());

		auto passCB = mCurrFrameResource->PassCB->Resource();
		mCommandList->SetGraphicsRootConstantBufferView(1, passCB->GetGPUVirtualAddress());

		// Bind all the materials used in this scene.  For structured buffers, we can bypass the heap and 
		// set as a root descriptor.
		auto matBuffer = mCurrFrameResource->MaterialBuffer->Resource();
		mCommandList->SetGraphicsRootShaderResourceView(2, matBuffer->GetGPUVirtualAddress());

		// Bind all the textures used in this scene.  Observe
		// that we only have to specify the first descriptor in the table.  
		// The root signature knows how many descriptors are expected in the table.
		mCommandList->SetGraphicsRootDescriptorTable(3, mSrvDescriptorHeap->GetGPUDescriptorHandleForHeapStart());

		DrawGBuffer();

		// A command list can be reset after it has been added to the command queue via ExecuteCommandList.
		// Reusing the command list reuses memory.
		//ThrowIfFailed(mCommandList->Reset(cmdListAlloc.Get(), mPSOs["deferred"].Get()));
		mCommandList->SetPipelineState(mPSOs["deferred"].Get());

		mCommandList->RSSetViewports(1, &mScreenViewport);
		mCommandList->RSSetScissorRects(1, &mScissorRect);

		// Indicate a state transition on the resource usage.
		mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(CurrentBackBuffer(),
			D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET));

		// Clear the back buffer and depth buffer.
		mCommandList->ClearRenderTargetView(CurrentBackBufferView(), Colors::LightSteelBlue, 0, nullptr);
		mCommandList->ClearDepthStencilView(DepthStencilView(), D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);

		// Specify the buffers we are going to render to.
		mCommandList->OMSetRenderTargets(1, &CurrentBackBufferView(), true, &DepthStencilView());

		mCommandList->SetGraphicsRootDescriptorTable(4, mDeferred->gBuffer0Srv());

		DrawRenderItems(mCommandList.Get(), mRitemLayer[int(RenderLayer::Opaque)]);

		mCommandList->SetPipelineState(mPSOs["sky"].Get());
		DrawRenderItems(mCommandList.Get(), mRitemLayer[int(RenderLayer::Sky)]);

		// Indicate a state transition on the resource usage.
		mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(CurrentBackBuffer(),
			D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT));

		// Done recording commands.
		ThrowIfFailed(mCommandList->Close());

		// Add the command list to the queue for execution.
		ID3D12CommandList* cmdsLists[] = { mCommandList.Get() };
		mCommandQueue->ExecuteCommandLists(_countof(cmdsLists), cmdsLists);

		// Swap the back and front buffers
		ThrowIfFailed(mSwapChain->Present(0, 0));
		mCurrBackBuffer = (mCurrBackBuffer + 1) % SwapChainBufferCount;

		// Advance the fence value to mark commands up to this fence point.
		mCurrFrameResource->Fence = ++mCurrentFence;

		// Add an instruction to the command queue to set a new fence point. 
		// Because we are on the GPU timeline, the new fence point won't be 
		// set until the GPU finishes processing all the commands prior to this Signal().
		mCommandQueue->Signal(mFence.Get(), mCurrentFence);
	}
	else
	{
		auto cmdListAlloc = mCurrFrameResource->CmdListAlloc;

		// Reuse the memory associated with command recording.
		// We can only reset when the associated command lists have finished execution on the GPU.
		ThrowIfFailed(cmdListAlloc->Reset());

		// A command list can be reset after it has been added to the command queue via ExecuteCommandList.
		// Reusing the command list reuses memory.
		ThrowIfFailed(mCommandList->Reset(cmdListAlloc.Get(), mPSOs["opaque"].Get()));

		mCommandList->RSSetViewports(1, &mScreenViewport);
		mCommandList->RSSetScissorRects(1, &mScissorRect);

		// Indicate a state transition on the resource usage.
		mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(CurrentBackBuffer(),
			D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET));

		// Clear the back buffer and depth buffer.
		mCommandList->ClearRenderTargetView(CurrentBackBufferView(), Colors::LightSteelBlue, 0, nullptr);
		mCommandList->ClearDepthStencilView(DepthStencilView(), D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);

		// Specify the buffers we are going to render to.
		mCommandList->OMSetRenderTargets(1, &CurrentBackBufferView(), true, &DepthStencilView());

		ID3D12DescriptorHeap* descriptorHeaps[] = { mSrvDescriptorHeap.Get() };
		mCommandList->SetDescriptorHeaps(_countof(descriptorHeaps), descriptorHeaps);

		mCommandList->SetGraphicsRootSignature(mRootSignature.Get());

		auto passCB = mCurrFrameResource->PassCB->Resource();
		mCommandList->SetGraphicsRootConstantBufferView(1, passCB->GetGPUVirtualAddress());

		// Bind all the materials used in this scene.  For structured buffers, we can bypass the heap and 
		// set as a root descriptor.
		auto matBuffer = mCurrFrameResource->MaterialBuffer->Resource();
		mCommandList->SetGraphicsRootShaderResourceView(2, matBuffer->GetGPUVirtualAddress());

		// Bind all the textures used in this scene.  Observe
		// that we only have to specify the first descriptor in the table.  
		// The root signature knows how many descriptors are expected in the table.
		mCommandList->SetGraphicsRootDescriptorTable(3, mSrvDescriptorHeap->GetGPUDescriptorHandleForHeapStart());

		DrawRenderItems(mCommandList.Get(), mRitemLayer[int(RenderLayer::Opaque)]);

		mCommandList->SetPipelineState(mPSOs["sky"].Get());
		DrawRenderItems(mCommandList.Get(), mRitemLayer[int(RenderLayer::Sky)]);

		// Indicate a state transition on the resource usage.
		mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(CurrentBackBuffer(),
			D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT));

		// Done recording commands.
		ThrowIfFailed(mCommandList->Close());

		// Add the command list to the queue for execution.
		ID3D12CommandList* cmdsLists[] = { mCommandList.Get() };
		mCommandQueue->ExecuteCommandLists(_countof(cmdsLists), cmdsLists);

		// Swap the back and front buffers
		ThrowIfFailed(mSwapChain->Present(0, 0));
		mCurrBackBuffer = (mCurrBackBuffer + 1) % SwapChainBufferCount;

		// Advance the fence value to mark commands up to this fence point.
		mCurrFrameResource->Fence = ++mCurrentFence;

		// Add an instruction to the command queue to set a new fence point. 
		// Because we are on the GPU timeline, the new fence point won't be 
		// set until the GPU finishes processing all the commands prior to this Signal().
		mCommandQueue->Signal(mFence.Get(), mCurrentFence);
	}
}


void CRYCHIC::OnMouseDown(WPARAM btnState, int x, int y)
{
	mLastMousePos.x = x;
	mLastMousePos.y = y;

	SetCapture(mhMainWnd);
}

void CRYCHIC::OnMouseUp(WPARAM btnState, int x, int y)
{
	ReleaseCapture();
}

void CRYCHIC::OnMouseMove(WPARAM btnState, int x, int y)
{
	if ((btnState & MK_LBUTTON) != 0)
	{
		// Make each pixel correspond to a quarter of a degree.
		float dx = XMConvertToRadians(0.25f * static_cast<float>(x - mLastMousePos.x));
		float dy = XMConvertToRadians(0.25f * static_cast<float>(y - mLastMousePos.y));

		mCamera.Pitch(dy);
		mCamera.RotateY(dx);
	}

	mLastMousePos.x = x;
	mLastMousePos.y = y;
}

void CRYCHIC::OnKeyboardInput(const GameTimer& gt)
{
	const float dt = gt.DeltaTime();

	if (GetAsyncKeyState('W') & 0x8000)
		mCamera.Walk(10.0f * dt);

	if (GetAsyncKeyState('S') & 0x8000)
		mCamera.Walk(-10.0f * dt);

	if (GetAsyncKeyState('A') & 0x8000)
		mCamera.Strafe(-10.0f * dt);

	if (GetAsyncKeyState('D') & 0x8000)
		mCamera.Strafe(10.0f * dt);

	mCamera.UpdateViewMatrix();
}

void CRYCHIC::AnimateMaterials(const GameTimer& gt)
{

}

//void CRYCHIC::UpdateObjectCBs(const GameTimer& gt)
//{
//	auto currObjectCB = mCurrFrameResource->ObjectCB.get();
//	for (auto& e : mAllRitems)
//	{
//		// Only update the cbuffer data if the constants have changed.  
//		// This needs to be tracked per frame resource.
//		if (e->NumFramesDirty > 0)
//		{
//			XMMATRIX world = XMLoadFloat4x4(&e->World);
//			XMMATRIX texTransform = XMLoadFloat4x4(&e->TexTransform);
//
//			ObjectConstants objConstants;
//			XMStoreFloat4x4(&objConstants.World, XMMatrixTranspose(world));
//			XMStoreFloat4x4(&objConstants.TexTransform, XMMatrixTranspose(texTransform));
//			objConstants.MaterialIndex = e->Mat->MatCBIndex;
//
//			currObjectCB->CopyData(e->ObjCBIndex, objConstants);
//
//			// Next FrameResource need to be updated too.
//			e->NumFramesDirty--;
//		}
//	}
//}

void CRYCHIC::UpdateInstanceData(const GameTimer& gt)
{
	XMMATRIX view = mCamera.GetView();
	XMMATRIX invView = XMMatrixInverse(&XMMatrixDeterminant(view), view);

	UINT totalVisibleInstanceCount = 0, totalInstanceCount = 0;
	for (auto& e : mAllRitems)
	{
		// 每个渲染项取得自己的InstanceData
		auto currInstanceBuffer = mCurrFrameResource->InstanceBuffers[e->itemIndex].get();
		const auto& instanceData = e->Instances;
		totalInstanceCount += e->InstanceCount;
		int visibleInstanceCount = 0;
		for (UINT i = 0; i < (UINT)instanceData.size(); ++i)
		{
			XMMATRIX world = XMLoadFloat4x4(&instanceData[i].World);
			XMMATRIX texTransform = XMLoadFloat4x4(&instanceData[i].TexTransform);
			XMMATRIX invWorld = XMMatrixInverse(&XMMatrixDeterminant(world), world);
			XMMATRIX viewToLocal = XMMatrixMultiply(invView, invWorld);

			BoundingFrustum localSpaceFrustum;
			mCamFrustum.Transform(localSpaceFrustum, viewToLocal);

			// 如果检测结果为相交或者不进行视锥裁剪
			// 通过视锥裁剪检测的对象的数据才会被加入instance缓冲区
			// 关闭视锥裁剪就直接加入缓冲区
			if ((localSpaceFrustum.Contains(e->Bounds) != DISJOINT) || (mFrustumCullingEnabled == false))
			{
				InstanceData data;
				XMStoreFloat4x4(&data.World, XMMatrixTranspose(world));
				XMStoreFloat4x4(&data.TexTransform, XMMatrixTranspose(texTransform));
				data.MaterialIndex = instanceData[i].MaterialIndex;

				currInstanceBuffer->CopyData(visibleInstanceCount++, data);
			}

		}
		e->InstanceCount = visibleInstanceCount;
		totalVisibleInstanceCount += visibleInstanceCount;
	}
		std::wostringstream outs;
		outs.precision(6);
		outs << L"Instancing and Culling Demo" <<
			L"    " << totalVisibleInstanceCount <<
			L" objects visible out of " << totalInstanceCount;
		mMainWndCaption = outs.str();
}

void CRYCHIC::UpdateMaterialBuffer(const GameTimer& gt)
{
	auto currMaterialBuffer = mCurrFrameResource->MaterialBuffer.get();
	for (auto& e : mMaterials)
	{
		// Only update the cbuffer data if the constants have changed.  If the cbuffer
		// data changes, it needs to be updated for each FrameResource.
		Material* mat = e.second.get();
		if (mat->NumFramesDirty > 0)
		{
			XMMATRIX matTransform = XMLoadFloat4x4(&mat->MatTransform);

			MaterialData matData;
			matData.DiffuseAlbedo = mat->DiffuseAlbedo;
			matData.FresnelR0 = mat->FresnelR0;
			matData.Roughness = mat->Roughness;
			matData.Metalness = mat->Metalness;
			XMStoreFloat4x4(&matData.MatTransform, XMMatrixTranspose(matTransform));
			matData.DiffuseMapIndex = mat->DiffuseSrvHeapIndex;

			currMaterialBuffer->CopyData(mat->MatCBIndex, matData);

			// Next FrameResource need to be updated too.
			mat->NumFramesDirty--;
		}
	}
}

void CRYCHIC::UpdateMainPassCB(const GameTimer& gt)
{
	XMMATRIX view = mCamera.GetView();
	XMMATRIX proj = mCamera.GetProj();

	XMMATRIX viewProj = XMMatrixMultiply(view, proj);
	XMMATRIX invView = XMMatrixInverse(&XMMatrixDeterminant(view), view);
	XMMATRIX invProj = XMMatrixInverse(&XMMatrixDeterminant(proj), proj);
	XMMATRIX invViewProj = XMMatrixInverse(&XMMatrixDeterminant(viewProj), viewProj);

	XMStoreFloat4x4(&mMainPassCB.View, XMMatrixTranspose(view));
	XMStoreFloat4x4(&mMainPassCB.InvView, XMMatrixTranspose(invView));
	XMStoreFloat4x4(&mMainPassCB.Proj, XMMatrixTranspose(proj));
	XMStoreFloat4x4(&mMainPassCB.InvProj, XMMatrixTranspose(invProj));
	XMStoreFloat4x4(&mMainPassCB.ViewProj, XMMatrixTranspose(viewProj));
	XMStoreFloat4x4(&mMainPassCB.InvViewProj, XMMatrixTranspose(invViewProj));
	mMainPassCB.EyePosW = mCamera.GetPosition3f();
	mMainPassCB.RenderTargetSize = XMFLOAT2((float)mClientWidth, (float)mClientHeight);
	mMainPassCB.InvRenderTargetSize = XMFLOAT2(1.0f / mClientWidth, 1.0f / mClientHeight);
	mMainPassCB.NearZ = 1.0f;
	mMainPassCB.FarZ = 1000.0f;
	mMainPassCB.TotalTime = gt.TotalTime();
	mMainPassCB.DeltaTime = gt.DeltaTime();
	mMainPassCB.AmbientLight = { 0.25f, 0.25f, 0.35f, 1.0f };

	int dirLightsNum = 0;
	int pointLightsNum = 5;
	int spotLightsNum = 5;
	// Directional Lights
	mMainPassCB.Lights[0].Direction = { 0.57735f, -0.57735f, 0.57735f };
	mMainPassCB.Lights[0].Strength = { 2.8f, 2.8f, 2.8f };
	mMainPassCB.Lights[1].Direction = { -0.57735f, -0.57735f, 0.57735f };
	mMainPassCB.Lights[1].Strength = { 0.4f, 0.4f, 0.4f };
	mMainPassCB.Lights[2].Direction = { 0.0f, -0.707f, -0.707f };
	mMainPassCB.Lights[2].Strength = { 0.2f, 0.2f, 0.2f };

	// Point Lights
	for (int i = dirLightsNum, j = 0; i < dirLightsNum + pointLightsNum; i++,j++)
	{
		mMainPassCB.Lights[i].Position = { -5.0f, 4.5f, -10.0f + j * 5.0f};
		mMainPassCB.Lights[i].Strength = { 5.2f, 3.0f, 5.1f };
		mMainPassCB.Lights[i].FalloffStart = 1.0f;
		mMainPassCB.Lights[i].FalloffEnd = 8.0f;
	}

	// Spot Lights
	for (int i = dirLightsNum + pointLightsNum, j = 0; i < dirLightsNum + pointLightsNum + spotLightsNum; i++, j++)
	{
		mMainPassCB.Lights[i].Position = { 5.0f, 4.5f, -10.0f + j * 5.0f };
		mMainPassCB.Lights[i].Direction = { 0.0f, -1.0f, 0.0f };
		mMainPassCB.Lights[i].Strength = { 5.2f, 5.1f, 5.0f };
		mMainPassCB.Lights[i].FalloffStart = 1.0f;
		mMainPassCB.Lights[i].FalloffEnd = 10.1f;
		mMainPassCB.Lights[i].SpotPower = 8.0f;
	}

	//mMainPassCB.Lights[4].Position = { -5.0f, 3.5f, -5.0f };
	//mMainPassCB.Lights[4].Strength = { 2.2f, 2.2f, 2.2f };
	//mMainPassCB.Lights[4].FalloffStart = 1.0f;
	//mMainPassCB.Lights[4].FalloffEnd = 8.0f;

	//mMainPassCB.Lights[5].Position = { -5.0f, 3.5f, 0.0f };
	//mMainPassCB.Lights[5].Strength = { 2.2f, 2.2f, 2.2f };
	//mMainPassCB.Lights[5].FalloffStart = 1.0f;
	//mMainPassCB.Lights[5].FalloffEnd = 8.0f;

	//mMainPassCB.Lights[6].Position = { -5.0f, 3.5f, 5.0f };
	//mMainPassCB.Lights[6].Strength = { 2.2f, 2.2f, 2.2f };
	//mMainPassCB.Lights[6].FalloffStart = 1.0f;
	//mMainPassCB.Lights[6].FalloffEnd = 8.0f;

	//mMainPassCB.Lights[7].Position = { -5.0f, 3.5f, 10.0f };
	//mMainPassCB.Lights[7].Strength = { 2.2f, 2.2f, 2.2f };
	//mMainPassCB.Lights[7].FalloffStart = 1.0f;
	//mMainPassCB.Lights[7].FalloffEnd = 8.0f;

	//// Spot Lights
	//mMainPassCB.Lights[8].Position = { 5.0f, 3.5f, -10.0f };
	//mMainPassCB.Lights[8].Strength = { 2.2f, 2.2f, 2.2f };
	//mMainPassCB.Lights[8].FalloffStart = 1.0f;
	//mMainPassCB.Lights[8].FalloffEnd = 8.0f;
	//mMainPassCB.Lights[8].SpotPower = 64.0f;

	//mMainPassCB.Lights[9].Position = { 5.0f, 3.5f, -5.0f };
	//mMainPassCB.Lights[9].Strength = { 2.2f, 2.2f, 2.2f };
	//mMainPassCB.Lights[9].FalloffStart = 1.0f;
	//mMainPassCB.Lights[9].FalloffEnd = 8.0f;
	//mMainPassCB.Lights[9].SpotPower = 64.0f;

	//mMainPassCB.Lights[10].Position = { 5.0f, 3.5f, 0.0f };
	//mMainPassCB.Lights[10].Strength = { 2.2f, 2.2f, 2.2f };
	//mMainPassCB.Lights[10].FalloffStart = 1.0f;
	//mMainPassCB.Lights[10].FalloffEnd = 8.0f;
	//mMainPassCB.Lights[10].SpotPower = 64.0f;

	//mMainPassCB.Lights[11].Position = { 5.0f, 3.5f, 5.0f };
	//mMainPassCB.Lights[11].Strength = { 2.2f, 2.2f, 2.2f };
	//mMainPassCB.Lights[11].FalloffStart = 1.0f;
	//mMainPassCB.Lights[11].FalloffEnd = 8.0f;
	//mMainPassCB.Lights[11].SpotPower = 64.0f;

	//mMainPassCB.Lights[12].Position = { 5.0f, 3.5f, 10.0f };
	//mMainPassCB.Lights[12].Strength = { 2.2f, 2.2f, 2.2f };
	//mMainPassCB.Lights[12].FalloffStart = 1.0f;
	//mMainPassCB.Lights[12].FalloffEnd = 8.0f;
	//mMainPassCB.Lights[12].SpotPower = 64.0f;

	auto currPassCB = mCurrFrameResource->PassCB.get();
	currPassCB->CopyData(0, mMainPassCB);
}

void CRYCHIC::LoadTextures()
{
	std::vector<std::string> texs =
	{
		"bricksTex",
		"stoneTex",
		"tileTex",
		"crateTex",
		"crate02Tex",
		"flareTex",
		"iceTex",
		"defaultTex",
		"skyCubeMap"
	};

	std::vector<std::wstring> fileNames = {
		L"Textures/bricks.dds",
		L"Textures/stone.dds",
		L"Textures/tile.dds",
		L"Textures/WoodCrate01.dds",
		L"Textures/WoodCrate02.dds",
		L"Textures/flare.dds",
		L"Textures/ice.dds",
		L"Textures/white1x1.dds",
		L"Textures/grasscube1024.dds"
	};

	for (int i = 0; i < texs.size(); i++)
	{
		auto tex = std::make_unique<Texture>();
		tex->Name = texs[i];
		tex->Filename = fileNames[i];
		ThrowIfFailed(DirectX::CreateDDSTextureFromFile12(md3dDevice.Get(),
			mCommandList.Get(), tex->Filename.c_str(),
			tex->Resource, tex->UploadHeap));
		mTextures[tex->Name] = std::move(tex);
	}
	// 记录纹理数量后续用来创建srv
	mTexSize = texs.size();
	texNames = std::move(texs);
}

void CRYCHIC::BuildRootSignature()
{
	CD3DX12_DESCRIPTOR_RANGE texTable;
	texTable.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, mTexSize, 0, 0);

	CD3DX12_DESCRIPTOR_RANGE gBufferTable;
	gBufferTable.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 4, mTexSize, 0);

	// Root parameter can be a table, root descriptor or root constants.
	CD3DX12_ROOT_PARAMETER slotRootParameter[5];

	// Perfomance TIP: Order from most frequent to least frequent.
	
	// instanceData srv root descriptor
	slotRootParameter[0].InitAsShaderResourceView(0, 1);
	// passCB cbv root descriptor
	slotRootParameter[1].InitAsConstantBufferView(0);
	// material srv root descriptor
	slotRootParameter[2].InitAsShaderResourceView(1, 1);
	// bind all the textures descriptor table
	slotRootParameter[3].InitAsDescriptorTable(1, &texTable, D3D12_SHADER_VISIBILITY_PIXEL);
	// bind gbuffers descriptor table
	slotRootParameter[4].InitAsDescriptorTable(1, &gBufferTable, D3D12_SHADER_VISIBILITY_PIXEL);

	auto staticSamplers = GetStaticSamplers();

	// A root signature is an array of root parameters.
	CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(5, slotRootParameter,
		(UINT)staticSamplers.size(), staticSamplers.data(),
		D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

	// create a root signature with a single slot which points to a descriptor range consisting of a single constant buffer
	ComPtr<ID3DBlob> serializedRootSig = nullptr;
	ComPtr<ID3DBlob> errorBlob = nullptr;
	HRESULT hr = D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1,
		serializedRootSig.GetAddressOf(), errorBlob.GetAddressOf());

	if (errorBlob != nullptr)
	{
		::OutputDebugStringA((char*)errorBlob->GetBufferPointer());
	}
	ThrowIfFailed(hr);

	ThrowIfFailed(md3dDevice->CreateRootSignature(
		0,
		serializedRootSig->GetBufferPointer(),
		serializedRootSig->GetBufferSize(),
		IID_PPV_ARGS(mRootSignature.GetAddressOf())));
}

void CRYCHIC::BuildDescriptorHeaps()
{
	//
	// Create the SRV heap.
	//
	D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {};
	srvHeapDesc.NumDescriptors = mTexSize + mGBufferSize;
	srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	ThrowIfFailed(md3dDevice->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&mSrvDescriptorHeap)));

	//
	// Fill out the heap with actual descriptors.
	//
	CD3DX12_CPU_DESCRIPTOR_HANDLE hDescriptor(mSrvDescriptorHeap->GetCPUDescriptorHandleForHeapStart());

	D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
	srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	srvDesc.Texture2D.MostDetailedMip = 0;
	srvDesc.Texture2D.ResourceMinLODClamp = 0.0f;

	// 自动根据纹理数量创建srv
	for (auto& texName : texNames)
	{
		auto tex = mTextures[texName]->Resource;
		srvDesc.Format = tex->GetDesc().Format;
		srvDesc.Texture2D.MipLevels = tex->GetDesc().MipLevels;
		md3dDevice->CreateShaderResourceView(tex.Get(), &srvDesc, hDescriptor);
		hDescriptor.Offset(1, mCbvSrvDescriptorSize);
	}

	//auto bricksTex = mTextures["bricksTex"]->Resource;
	//auto stoneTex = mTextures["stoneTex"]->Resource;
	//auto tileTex = mTextures["tileTex"]->Resource;
	//auto crateTex = mTextures["crateTex"]->Resource;

	//D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
	//srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	//srvDesc.Format = bricksTex->GetDesc().Format;
	//srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	//srvDesc.Texture2D.MostDetailedMip = 0;
	//srvDesc.Texture2D.MipLevels = bricksTex->GetDesc().MipLevels;
	//srvDesc.Texture2D.ResourceMinLODClamp = 0.0f;
	//md3dDevice->CreateShaderResourceView(bricksTex.Get(), &srvDesc, hDescriptor);

	//// next descriptor
	//hDescriptor.Offset(1, mCbvSrvDescriptorSize);

	//srvDesc.Format = stoneTex->GetDesc().Format;
	//srvDesc.Texture2D.MipLevels = stoneTex->GetDesc().MipLevels;
	//md3dDevice->CreateShaderResourceView(stoneTex.Get(), &srvDesc, hDescriptor);

	//// next descriptor
	//hDescriptor.Offset(1, mCbvSrvDescriptorSize);

	//srvDesc.Format = tileTex->GetDesc().Format;
	//srvDesc.Texture2D.MipLevels = tileTex->GetDesc().MipLevels;
	//md3dDevice->CreateShaderResourceView(tileTex.Get(), &srvDesc, hDescriptor);

	//// next descriptor
	//hDescriptor.Offset(1, mCbvSrvDescriptorSize);

	//srvDesc.Format = crateTex->GetDesc().Format;
	//srvDesc.Texture2D.MipLevels = crateTex->GetDesc().MipLevels;
	//md3dDevice->CreateShaderResourceView(crateTex.Get(), &srvDesc, hDescriptor);

	mTexsHeapIndex = mTexSize - 1;
	gBufferHeapIndex = mTexsHeapIndex + 1;

	auto srvCpuStart = mSrvDescriptorHeap->GetCPUDescriptorHandleForHeapStart();
	auto srvGpuStart = mSrvDescriptorHeap->GetGPUDescriptorHandleForHeapStart();
	auto rtvCpuStart = mRtvHeap->GetCPUDescriptorHandleForHeapStart();

	// gBuffer RTV goes after the swap chain descriptors.
	int rtvOffset = SwapChainBufferCount;

	CD3DX12_CPU_DESCRIPTOR_HANDLE gBufferRtvHandles;
	gBufferRtvHandles = CD3DX12_CPU_DESCRIPTOR_HANDLE(rtvCpuStart, rtvOffset, mRtvDescriptorSize);

	mDeferred->BuildDescriptors(
		CD3DX12_CPU_DESCRIPTOR_HANDLE(srvCpuStart, gBufferHeapIndex, mCbvSrvDescriptorSize),
		CD3DX12_GPU_DESCRIPTOR_HANDLE(srvGpuStart, gBufferHeapIndex, mCbvSrvDescriptorSize),
		gBufferRtvHandles);

}

void CRYCHIC::BuildShadersAndInputLayout()
{
	const D3D_SHADER_MACRO alphaTestDefines[] =
	{
		"ALPHA_TEST", "1",
		NULL, NULL
	};

	mShaders["standardVS"] = d3dUtil::CompileShader(L"Shaders\\Default.hlsl", nullptr, "VS", "vs_5_1");
	mShaders["opaquePS"] = d3dUtil::CompileShader(L"Shaders\\Default.hlsl", nullptr, "PS", "ps_5_1");
	mShaders["gBufferVS"] = d3dUtil::CompileShader(L"Shaders\\GBufferPass.hlsl", nullptr, "GBufferVS", "vs_5_1");
	mShaders["gBufferPS"] = d3dUtil::CompileShader(L"Shaders\\GBufferPass.hlsl", nullptr, "GBufferPS", "ps_5_1");
	mShaders["deferredShadingVS"] = d3dUtil::CompileShader(L"Shaders\\DeferredShading.hlsl", nullptr, "DeferredVS", "vs_5_1");
	mShaders["deferredShadingPS"] = d3dUtil::CompileShader(L"Shaders\\DeferredShading.hlsl", nullptr, "DeferredPS", "ps_5_1");
	mShaders["skyVS"] = d3dUtil::CompileShader(L"Shaders\\sky.hlsl", nullptr, "VS", "vs_5_1");
	mShaders["skyPS"] = d3dUtil::CompileShader(L"Shaders\\sky.hlsl", nullptr, "PS", "ps_5_1");

	mInputLayout =
	{
		{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
	};
}

void CRYCHIC::BuildShapeGeometry()
{
	GeometryGenerator geoGen;
	GeometryGenerator::MeshData box = geoGen.CreateBox(1.0f, 1.0f, 1.0f, 3);
	GeometryGenerator::MeshData grid = geoGen.CreateGrid(20.0f, 30.0f, 60, 40);
	GeometryGenerator::MeshData sphere = geoGen.CreateSphere(0.5f, 20, 20);
	GeometryGenerator::MeshData cylinder = geoGen.CreateCylinder(0.5f, 0.3f, 3.0f, 20, 20);

	//
	// We are concatenating all the geometry into one big vertex/index buffer.  So
	// define the regions in the buffer each submesh covers.
	//

	// Cache the vertex offsets to each object in the concatenated vertex buffer.
	UINT boxVertexOffset = 0;
	UINT gridVertexOffset = (UINT)box.Vertices.size();
	UINT sphereVertexOffset = gridVertexOffset + (UINT)grid.Vertices.size();
	UINT cylinderVertexOffset = sphereVertexOffset + (UINT)sphere.Vertices.size();

	// Cache the starting index for each object in the concatenated index buffer.
	UINT boxIndexOffset = 0;
	UINT gridIndexOffset = (UINT)box.Indices32.size();
	UINT sphereIndexOffset = gridIndexOffset + (UINT)grid.Indices32.size();
	UINT cylinderIndexOffset = sphereIndexOffset + (UINT)sphere.Indices32.size();

	SubmeshGeometry boxSubmesh;
	boxSubmesh.IndexCount = (UINT)box.Indices32.size();
	boxSubmesh.StartIndexLocation = boxIndexOffset;
	boxSubmesh.BaseVertexLocation = boxVertexOffset;

	SubmeshGeometry gridSubmesh;
	gridSubmesh.IndexCount = (UINT)grid.Indices32.size();
	gridSubmesh.StartIndexLocation = gridIndexOffset;
	gridSubmesh.BaseVertexLocation = gridVertexOffset;

	SubmeshGeometry sphereSubmesh;
	sphereSubmesh.IndexCount = (UINT)sphere.Indices32.size();
	sphereSubmesh.StartIndexLocation = sphereIndexOffset;
	sphereSubmesh.BaseVertexLocation = sphereVertexOffset;

	SubmeshGeometry cylinderSubmesh;
	cylinderSubmesh.IndexCount = (UINT)cylinder.Indices32.size();
	cylinderSubmesh.StartIndexLocation = cylinderIndexOffset;
	cylinderSubmesh.BaseVertexLocation = cylinderVertexOffset;

	//
	// Extract the vertex elements we are interested in and pack the
	// vertices of all the meshes into one vertex buffer.
	//

	auto totalVertexCount =
		box.Vertices.size() +
		grid.Vertices.size() +
		sphere.Vertices.size() +
		cylinder.Vertices.size();

	std::vector<Vertex> vertices(totalVertexCount);
	std::vector<std::uint16_t> indices;

	XMFLOAT3 vMinf3(+MathHelper::Infinity, +MathHelper::Infinity, +MathHelper::Infinity);
	XMFLOAT3 vMaxf3(-MathHelper::Infinity, -MathHelper::Infinity, -MathHelper::Infinity);

	XMVECTOR vMin = XMLoadFloat3(&vMinf3);
	XMVECTOR vMax = XMLoadFloat3(&vMaxf3);

	UINT k = 0;
	for (int i = 0; i < box.Vertices.size(); ++i, ++k)
	{
		vertices[k].Pos = box.Vertices[i].Position;
		vertices[k].Normal = box.Vertices[i].Normal;
		vertices[k].TexC = box.Vertices[i].TexC;
		XMVECTOR P = XMLoadFloat3(&vertices[k].Pos);
		vMin = XMVectorMin(vMin, P);
		vMax = XMVectorMax(vMax, P);
	}

	BoundingBox bounds;
	XMStoreFloat3(&bounds.Center, 0.5f * (vMin + vMax));
	XMStoreFloat3(&bounds.Extents, 0.5f * (vMax - vMin));
	boxSubmesh.Bounds = bounds;


	vMin = XMLoadFloat3(&vMinf3);
	vMax = XMLoadFloat3(&vMaxf3);

	for (int i = 0; i < grid.Vertices.size(); ++i, ++k)
	{
		vertices[k].Pos = grid.Vertices[i].Position;
		vertices[k].Normal = grid.Vertices[i].Normal;
		vertices[k].TexC = grid.Vertices[i].TexC;
		XMVECTOR P = XMLoadFloat3(&vertices[k].Pos);
		vMin = XMVectorMin(vMin, P);
		vMax = XMVectorMax(vMax, P);
	}

	XMStoreFloat3(&bounds.Center, 0.5f * (vMin + vMax));
	XMStoreFloat3(&bounds.Extents, 0.5f * (vMax - vMin));
	gridSubmesh.Bounds = bounds;


	vMin = XMLoadFloat3(&vMinf3);
	vMax = XMLoadFloat3(&vMaxf3);
	for (int i = 0; i < sphere.Vertices.size(); ++i, ++k)
	{
		vertices[k].Pos = sphere.Vertices[i].Position;
		vertices[k].Normal = sphere.Vertices[i].Normal;
		vertices[k].TexC = sphere.Vertices[i].TexC;
		XMVECTOR P = XMLoadFloat3(&vertices[k].Pos);
		vMin = XMVectorMin(vMin, P);
		vMax = XMVectorMax(vMax, P);
	}

	XMStoreFloat3(&bounds.Center, 0.5f * (vMin + vMax));
	XMStoreFloat3(&bounds.Extents, 0.5f * (vMax - vMin));
	sphereSubmesh.Bounds = bounds;


	vMin = XMLoadFloat3(&vMinf3);
	vMax = XMLoadFloat3(&vMaxf3);
	for (size_t i = 0; i < cylinder.Vertices.size(); ++i, ++k)
	{
		vertices[k].Pos = cylinder.Vertices[i].Position;
		vertices[k].Normal = cylinder.Vertices[i].Normal;
		vertices[k].TexC = cylinder.Vertices[i].TexC;
		XMVECTOR P = XMLoadFloat3(&vertices[k].Pos);
		vMin = XMVectorMin(vMin, P);
		vMax = XMVectorMax(vMax, P);
	}

	XMStoreFloat3(&bounds.Center, 0.5f * (vMin + vMax));
	XMStoreFloat3(&bounds.Extents, 0.5f * (vMax - vMin));
	cylinderSubmesh.Bounds = bounds;
	
	indices.insert(indices.end(), std::begin(box.GetIndices16()), std::end(box.GetIndices16()));
	indices.insert(indices.end(), std::begin(grid.GetIndices16()), std::end(grid.GetIndices16()));
	indices.insert(indices.end(), std::begin(sphere.GetIndices16()), std::end(sphere.GetIndices16()));
	indices.insert(indices.end(), std::begin(cylinder.GetIndices16()), std::end(cylinder.GetIndices16()));

	const UINT vbByteSize = (UINT)vertices.size() * sizeof(Vertex);
	const UINT ibByteSize = (UINT)indices.size() * sizeof(std::uint16_t);

	auto geo = std::make_unique<MeshGeometry>();
	geo->Name = "shapeGeo";

	ThrowIfFailed(D3DCreateBlob(vbByteSize, &geo->VertexBufferCPU));
	CopyMemory(geo->VertexBufferCPU->GetBufferPointer(), vertices.data(), vbByteSize);

	ThrowIfFailed(D3DCreateBlob(ibByteSize, &geo->IndexBufferCPU));
	CopyMemory(geo->IndexBufferCPU->GetBufferPointer(), indices.data(), ibByteSize);

	geo->VertexBufferGPU = d3dUtil::CreateDefaultBuffer(md3dDevice.Get(),
		mCommandList.Get(), vertices.data(), vbByteSize, geo->VertexBufferUploader);

	geo->IndexBufferGPU = d3dUtil::CreateDefaultBuffer(md3dDevice.Get(),
		mCommandList.Get(), indices.data(), ibByteSize, geo->IndexBufferUploader);

	geo->VertexByteStride = sizeof(Vertex);
	geo->VertexBufferByteSize = vbByteSize;
	geo->IndexFormat = DXGI_FORMAT_R16_UINT;
	geo->IndexBufferByteSize = ibByteSize;

	geo->DrawArgs["box"] = boxSubmesh;
	geo->DrawArgs["grid"] = gridSubmesh;
	geo->DrawArgs["sphere"] = sphereSubmesh;
	geo->DrawArgs["cylinder"] = cylinderSubmesh;

	mGeometries[geo->Name] = std::move(geo);
}

LPCWSTR stringToLPCWSTR(std::string orig)
{
	size_t origsize = orig.length() + 1;
	const size_t newsize = 100;
	size_t convertedChars = 0;
	wchar_t* wcstring = (wchar_t*)malloc(sizeof(wchar_t) * (orig.length() - 1));
	mbstowcs_s(&convertedChars, wcstring, origsize, orig.c_str(), _TRUNCATE);

	return wcstring;
}

void CRYCHIC::BuildGeometry(std::string fileName)
{
	std::ifstream fin(fileName);

	if (!fin)
	{
		MessageBox(0, stringToLPCWSTR(fileName + " not found!"), 0, 0);
		return;
	}

	UINT vcount = 0;
	UINT tcount = 0;
	std::string ignore;

	fin >> ignore >> vcount;
	fin >> ignore >> tcount;
	fin >> ignore >> ignore >> ignore >> ignore;

	XMFLOAT3 vMinf3(+MathHelper::Infinity, +MathHelper::Infinity, +MathHelper::Infinity);
	XMFLOAT3 vMaxf3(-MathHelper::Infinity, -MathHelper::Infinity, -MathHelper::Infinity);

	XMVECTOR vMin = XMLoadFloat3(&vMinf3);
	XMVECTOR vMax = XMLoadFloat3(&vMaxf3);

	std::vector<Vertex> vertices(vcount);
	for (UINT i = 0; i < vcount; ++i)
	{
		fin >> vertices[i].Pos.x >> vertices[i].Pos.y >> vertices[i].Pos.z;
		fin >> vertices[i].Normal.x >> vertices[i].Normal.y >> vertices[i].Normal.z;

		XMVECTOR P = XMLoadFloat3(&vertices[i].Pos);

		// Project point onto unit sphere and generate spherical texture coordinates.
		XMFLOAT3 spherePos;
		XMStoreFloat3(&spherePos, XMVector3Normalize(P));

		float theta = atan2f(spherePos.z, spherePos.x);

		// Put in [0, 2pi].
		if (theta < 0.0f)
			theta += XM_2PI;

		float phi = acosf(spherePos.y);

		float u = theta / (2.0f * XM_PI);
		float v = phi / XM_PI;

		vertices[i].TexC = { u, v };

		vMin = XMVectorMin(vMin, P);
		vMax = XMVectorMax(vMax, P);
	}

	BoundingBox bounds;
	XMStoreFloat3(&bounds.Center, 0.5f * (vMin + vMax));
	XMStoreFloat3(&bounds.Extents, 0.5f * (vMax - vMin));

	fin >> ignore;
	fin >> ignore;
	fin >> ignore;

	std::vector<std::int32_t> indices(3 * tcount);
	for (UINT i = 0; i < tcount; ++i)
	{
		fin >> indices[i * 3 + 0] >> indices[i * 3 + 1] >> indices[i * 3 + 2];
	}

	fin.close();

	//
	// Pack the indices of all the meshes into one index buffer.
	//

	const UINT vbByteSize = (UINT)vertices.size() * sizeof(Vertex);

	const UINT ibByteSize = (UINT)indices.size() * sizeof(std::int32_t);

	std::string name;
	int i = 0;
	while (fileName[i] != '\0' && fileName[i] != '/')
	{
		i++;
	}
	i++;
	while (fileName[i] != '\0' && fileName[i] != '.')
	{
		name += fileName[i++];
	}

	auto geo = std::make_unique<MeshGeometry>();
	geo->Name = name + "Geo";

	ThrowIfFailed(D3DCreateBlob(vbByteSize, &geo->VertexBufferCPU));
	CopyMemory(geo->VertexBufferCPU->GetBufferPointer(), vertices.data(), vbByteSize);

	ThrowIfFailed(D3DCreateBlob(ibByteSize, &geo->IndexBufferCPU));
	CopyMemory(geo->IndexBufferCPU->GetBufferPointer(), indices.data(), ibByteSize);

	geo->VertexBufferGPU = d3dUtil::CreateDefaultBuffer(md3dDevice.Get(),
		mCommandList.Get(), vertices.data(), vbByteSize, geo->VertexBufferUploader);

	geo->IndexBufferGPU = d3dUtil::CreateDefaultBuffer(md3dDevice.Get(),
		mCommandList.Get(), indices.data(), ibByteSize, geo->IndexBufferUploader);

	geo->VertexByteStride = sizeof(Vertex);
	geo->VertexBufferByteSize = vbByteSize;
	geo->IndexFormat = DXGI_FORMAT_R32_UINT;
	geo->IndexBufferByteSize = ibByteSize;

	SubmeshGeometry submesh;
	submesh.IndexCount = (UINT)indices.size();
	submesh.StartIndexLocation = 0;
	submesh.BaseVertexLocation = 0;
	submesh.Bounds = bounds;

	geo->DrawArgs[name] = submesh;

	mGeometries[geo->Name] = std::move(geo);
}

void CRYCHIC::BuildPSOs()
{
	D3D12_GRAPHICS_PIPELINE_STATE_DESC opaquePsoDesc;

	//
	// PSO for opaque objects.
	//
	ZeroMemory(&opaquePsoDesc, sizeof(D3D12_GRAPHICS_PIPELINE_STATE_DESC));
	opaquePsoDesc.InputLayout = { mInputLayout.data(), (UINT)mInputLayout.size() };
	opaquePsoDesc.pRootSignature = mRootSignature.Get();
	opaquePsoDesc.VS =
	{
		reinterpret_cast<BYTE*>(mShaders["standardVS"]->GetBufferPointer()),
		mShaders["standardVS"]->GetBufferSize()
	};
	opaquePsoDesc.PS =
	{
		reinterpret_cast<BYTE*>(mShaders["opaquePS"]->GetBufferPointer()),
		mShaders["opaquePS"]->GetBufferSize()
	};
	opaquePsoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
	opaquePsoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
	opaquePsoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
	opaquePsoDesc.SampleMask = UINT_MAX;
	opaquePsoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	opaquePsoDesc.NumRenderTargets = 1;
	opaquePsoDesc.RTVFormats[0] = mBackBufferFormat;
	opaquePsoDesc.SampleDesc.Count = m4xMsaaState ? 4 : 1;
	opaquePsoDesc.SampleDesc.Quality = m4xMsaaState ? (m4xMsaaQuality - 1) : 0;
	opaquePsoDesc.DSVFormat = mDepthStencilFormat;
	ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&opaquePsoDesc, IID_PPV_ARGS(&mPSOs["opaque"])));

	//
	// PSO for generate GBuffers.
	//
	D3D12_GRAPHICS_PIPELINE_STATE_DESC gBufferPsoDesc = opaquePsoDesc;
	gBufferPsoDesc.VS = {
		reinterpret_cast<BYTE*>(mShaders["gBufferVS"]->GetBufferPointer()),
		mShaders["gBufferVS"]->GetBufferSize()
	};
	gBufferPsoDesc.PS = {
	reinterpret_cast<BYTE*>(mShaders["gBufferPS"]->GetBufferPointer()),
	mShaders["gBufferPS"]->GetBufferSize()
	};
	gBufferPsoDesc.NumRenderTargets = 4;
	gBufferPsoDesc.RTVFormats[0] = mDeferred->Format();
	gBufferPsoDesc.RTVFormats[1] = mDeferred->Format();
	gBufferPsoDesc.RTVFormats[2] = mDeferred->Format();
	gBufferPsoDesc.RTVFormats[3] = mDeferred->Format();
	ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&gBufferPsoDesc, IID_PPV_ARGS(&mPSOs["gBuffer"])));

	//
	// PSO for deferred shading.
	//
	D3D12_GRAPHICS_PIPELINE_STATE_DESC deferredPsoDesc = opaquePsoDesc;
	deferredPsoDesc.VS = {
		reinterpret_cast<BYTE*>(mShaders["deferredShadingVS"]->GetBufferPointer()),
		mShaders["deferredShadingVS"]->GetBufferSize()
	};
	deferredPsoDesc.PS = {
	reinterpret_cast<BYTE*>(mShaders["deferredShadingPS"]->GetBufferPointer()),
	mShaders["deferredShadingPS"]->GetBufferSize()
	};
	ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&deferredPsoDesc, IID_PPV_ARGS(&mPSOs["deferred"])));

	//
	// PSO for skybox.
	//
	D3D12_GRAPHICS_PIPELINE_STATE_DESC skyPsoDesc = opaquePsoDesc;
	skyPsoDesc.VS = {
		reinterpret_cast<BYTE*>(mShaders["skyVS"]->GetBufferPointer()),
		mShaders["skyVS"]->GetBufferSize()
	};
	skyPsoDesc.PS = {
	reinterpret_cast<BYTE*>(mShaders["skyPS"]->GetBufferPointer()),
	mShaders["skyPS"]->GetBufferSize()
	};
	// 从cubemap中心向cubemap采样
	skyPsoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
	skyPsoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
	ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&skyPsoDesc, IID_PPV_ARGS(&mPSOs["sky"])));
}

void CRYCHIC::BuildFrameResources()
{
	for (int i = 0; i < gNumFrameResources; ++i)
	{
		mFrameResources.push_back(std::make_unique<FrameResource>(md3dDevice.Get(),
			1, mInstanceCount, mAllRitems.size(), (UINT)mMaterials.size()));
	}
}

void CRYCHIC::BuildMaterials()
{
	// mat0
	auto bricks0 = std::make_unique<Material>();
	bricks0->Name = "bricks0";
	bricks0->MatCBIndex = 0;
	bricks0->DiffuseSrvHeapIndex = 0;
	bricks0->DiffuseAlbedo = XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);
	bricks0->FresnelR0 = XMFLOAT3(0.02f, 0.02f, 0.02f);
	bricks0->Roughness = 0.1f;
	bricks0->Metalness = 0.3f;

	// mat1
	auto stone0 = std::make_unique<Material>();
	stone0->Name = "stone0";
	stone0->MatCBIndex = 1;
	stone0->DiffuseSrvHeapIndex = 1;
	stone0->DiffuseAlbedo = XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);
	stone0->FresnelR0 = XMFLOAT3(0.05f, 0.05f, 0.05f);
	stone0->Roughness = 0.3f;

	// mat2
	auto tile0 = std::make_unique<Material>();
	tile0->Name = "tile0";
	tile0->MatCBIndex = 2;
	tile0->DiffuseSrvHeapIndex = 2;
	tile0->DiffuseAlbedo = XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);
	tile0->FresnelR0 = XMFLOAT3(0.02f, 0.02f, 0.02f);
	tile0->Roughness = 0.3f;
	tile0->Metalness = 0.1f;

	// mat3
	auto crate0 = std::make_unique<Material>();
	crate0->Name = "crate0";
	crate0->MatCBIndex = 3;
	crate0->DiffuseSrvHeapIndex = 3;
	crate0->DiffuseAlbedo = XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);
	crate0->FresnelR0 = XMFLOAT3(0.05f, 0.05f, 0.05f);
	crate0->Roughness = 0.2f;

	// mat4
	auto crate1 = std::make_unique<Material>();
	crate1->Name = "crate1";
	crate1->MatCBIndex = 4;
	crate1->DiffuseSrvHeapIndex = 4;
	crate1->DiffuseAlbedo = XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);
	crate1->FresnelR0 = XMFLOAT3(0.05f, 0.05f, 0.05f);
	crate1->Roughness = 0.2f;

	// mat5
	auto flare = std::make_unique<Material>();
	flare->Name = "flare";
	flare->MatCBIndex = 5;
	flare->DiffuseSrvHeapIndex = 5;
	flare->DiffuseAlbedo = XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);
	flare->FresnelR0 = XMFLOAT3(0.05f, 0.05f, 0.05f);
	flare->Roughness = 0.2f;

	// mat6
	auto ice = std::make_unique<Material>();
	ice->Name = "ice";
	ice->MatCBIndex = 6;
	ice->DiffuseSrvHeapIndex = 6;
	ice->DiffuseAlbedo = XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);
	ice->FresnelR0 = XMFLOAT3(0.05f, 0.05f, 0.05f);
	ice->Roughness = 0.2f;

	// mat7
	auto default = std::make_unique<Material>();
	default->Name = "default";
	default->MatCBIndex = 7;
	default->DiffuseSrvHeapIndex = 7;
	default->DiffuseAlbedo = XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);
	default->FresnelR0 = XMFLOAT3(0.05f, 0.05f, 0.05f);
	default->Roughness = 0.5f;

	mMaterials["bricks0"] = std::move(bricks0);
	mMaterials["stone0"] = std::move(stone0);
	mMaterials["tile0"] = std::move(tile0);
	mMaterials["crate0"] = std::move(crate0);
	mMaterials["crate1"] = std::move(crate1);
	mMaterials["flare"] = std::move(flare);
	mMaterials["ice"] = std::move(ice);
	mMaterials["default"] = std::move(default);

}

void CRYCHIC::BuildRenderItems()
{
	auto boxRitem = std::make_unique<RenderItem>();
	// 每一个渲染项都有一个单独的InstanceData缓冲区
	// 给个下标去找对应的指向缓冲区的指针
	boxRitem->itemIndex = 0;
	boxRitem->World = MathHelper::Identity4x4();
	boxRitem->TexTransform = MathHelper::Identity4x4();
	boxRitem->Mat = mMaterials["stone0"].get();
	boxRitem->Geo = mGeometries["shapeGeo"].get();
	boxRitem->PrimitiveType = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
	boxRitem->IndexCount = boxRitem->Geo->DrawArgs["box"].IndexCount;
	boxRitem->StartIndexLocation = boxRitem->Geo->DrawArgs["box"].StartIndexLocation;
	boxRitem->BaseVertexLocation = boxRitem->Geo->DrawArgs["box"].BaseVertexLocation;
	boxRitem->Bounds = boxRitem->Geo->DrawArgs["box"].Bounds;
	
	// generate box instance data
	UINT boxInstanceCount = 1;
	mInstanceCount += boxInstanceCount;
	boxRitem->Instances.resize(boxInstanceCount);
	boxRitem->InstanceCount = boxInstanceCount;
	for (size_t i = 0; i < boxInstanceCount; i++)
	{
		XMMATRIX world = XMMatrixMultiply(XMMatrixScaling(2.0f, 2.0f, 2.0f), XMMatrixTranslation(0.0f, 1.0f, 0.0f));
		XMStoreFloat4x4(&boxRitem->Instances[i].World, world);
		XMStoreFloat4x4(&boxRitem->Instances[i].TexTransform, XMMatrixScaling(1.0f, 1.0f, 1.0f));
		boxRitem->Instances[i].MaterialIndex = 5; // flareMat
	}
	mRitemLayer[(int)RenderLayer::Opaque].push_back(boxRitem.get());
	mAllRitems.push_back(std::move(boxRitem));

	auto gridRitem = std::make_unique<RenderItem>();
	gridRitem->itemIndex = 1;
	gridRitem->World = MathHelper::Identity4x4();
	XMStoreFloat4x4(&gridRitem->TexTransform, XMMatrixScaling(8.0f, 8.0f, 1.0f));
	//gridRitem->ObjCBIndex = 1;
	gridRitem->Mat = mMaterials["tile0"].get();
	gridRitem->Geo = mGeometries["shapeGeo"].get();
	gridRitem->PrimitiveType = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
	gridRitem->IndexCount = gridRitem->Geo->DrawArgs["grid"].IndexCount;
	gridRitem->StartIndexLocation = gridRitem->Geo->DrawArgs["grid"].StartIndexLocation;
	gridRitem->BaseVertexLocation = gridRitem->Geo->DrawArgs["grid"].BaseVertexLocation;
	gridRitem->Bounds = gridRitem->Geo->DrawArgs["grid"].Bounds;
	
	// generate grid instance data
	UINT gridInstanceCount = 1;
	gridRitem->InstanceCount = gridInstanceCount;
	mInstanceCount += gridInstanceCount;
	gridRitem->Instances.resize(gridInstanceCount);
	for (size_t i = 0; i < gridInstanceCount; i++)
	{
		gridRitem->Instances[i].World = MathHelper::Identity4x4();
		XMStoreFloat4x4(&gridRitem->Instances[i].TexTransform, XMMatrixScaling(8.0f, 8.0f, 1.0f));
		gridRitem->Instances[i].MaterialIndex = 2; // tileMat
	}
	mRitemLayer[(int)RenderLayer::Opaque].push_back(gridRitem.get());
	mAllRitems.push_back(std::move(gridRitem));

	auto skullRitem = std::make_unique<RenderItem>();
	skullRitem->itemIndex = 2;
	skullRitem->TexTransform = MathHelper::Identity4x4();
	skullRitem->Mat = mMaterials["default"].get();
	skullRitem->Geo = mGeometries["skullGeo"].get();
	skullRitem->PrimitiveType = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
	skullRitem->IndexCount = skullRitem->Geo->DrawArgs["skull"].IndexCount;
	skullRitem->StartIndexLocation = skullRitem->Geo->DrawArgs["skull"].StartIndexLocation;
	skullRitem->BaseVertexLocation = skullRitem->Geo->DrawArgs["skull"].BaseVertexLocation;
	skullRitem->Bounds = skullRitem->Geo->DrawArgs["skull"].Bounds;

	// generate skull instance data
	UINT skullInstanceCount = 1;
	skullRitem->InstanceCount = skullInstanceCount;
	mInstanceCount += skullInstanceCount;
	skullRitem->Instances.resize(skullInstanceCount);
	for (size_t i = 0; i < skullInstanceCount; i++)
	{
		XMMATRIX world = XMMatrixMultiply(XMMatrixScaling(0.5f, 0.5f, 0.5f), XMMatrixTranslation(0.0f, 2.0f, 0.0f));
		XMStoreFloat4x4(&skullRitem->Instances[i].World, world);
		XMStoreFloat4x4(&skullRitem->Instances[i].TexTransform, XMMatrixScaling(1.0f, 1.0f, 1.0f));
		skullRitem->Instances[i].MaterialIndex = 7; // defaultMat
	}
	mRitemLayer[(int)RenderLayer::Opaque].push_back(skullRitem.get());
	mAllRitems.push_back(std::move(skullRitem));

	auto carRitem = std::make_unique<RenderItem>();
	carRitem->itemIndex = 3;
	carRitem->TexTransform = MathHelper::Identity4x4();
	//carRitem->Mat = mMaterials["default"].get();
	carRitem->Geo = mGeometries["carGeo"].get();
	carRitem->PrimitiveType = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
	carRitem->IndexCount = carRitem->Geo->DrawArgs["car"].IndexCount;
	carRitem->StartIndexLocation = carRitem->Geo->DrawArgs["car"].StartIndexLocation;
	carRitem->BaseVertexLocation = carRitem->Geo->DrawArgs["car"].BaseVertexLocation;
	carRitem->Bounds = carRitem->Geo->DrawArgs["car"].Bounds;

	// generate car instance data
	UINT carInstanceCount = 1;
	carRitem->InstanceCount = carInstanceCount;
	mInstanceCount += carInstanceCount;
	carRitem->Instances.resize(carInstanceCount);
	for (size_t i = 0; i < carInstanceCount; i++)
	{
		XMMATRIX world = XMMatrixMultiply(XMMatrixScaling(0.5f, 0.5f, 0.5f), XMMatrixTranslation(-2.0f, 1.5f, 0.0f));
		XMStoreFloat4x4(&carRitem->Instances[i].World, world);
		XMStoreFloat4x4(&carRitem->Instances[i].TexTransform, XMMatrixScaling(1.0f, 1.0f, 1.0f));
		carRitem->Instances[i].MaterialIndex = 6; // iceMat
	}
	mRitemLayer[(int)RenderLayer::Opaque].push_back(carRitem.get());
	mAllRitems.push_back(std::move(carRitem));


	auto leftCylRitem = std::make_unique<RenderItem>();
	leftCylRitem->itemIndex = 4;
	//leftCylRitem->Mat = mMaterials["bricks0"].get();
	leftCylRitem->Geo = mGeometries["shapeGeo"].get();
	leftCylRitem->PrimitiveType = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
	leftCylRitem->IndexCount = leftCylRitem->Geo->DrawArgs["cylinder"].IndexCount;
	leftCylRitem->StartIndexLocation = leftCylRitem->Geo->DrawArgs["cylinder"].StartIndexLocation;
	leftCylRitem->BaseVertexLocation = leftCylRitem->Geo->DrawArgs["cylinder"].BaseVertexLocation;
	leftCylRitem->Bounds = leftCylRitem->Geo->DrawArgs["cylinder"].Bounds;
	UINT leftCylInstanceCount = 5;
	leftCylRitem->InstanceCount = leftCylInstanceCount;
	mInstanceCount += leftCylInstanceCount;
	leftCylRitem->Instances.resize(leftCylInstanceCount);

	auto rightCylRitem = std::make_unique<RenderItem>();
	rightCylRitem->itemIndex = 5;
	rightCylRitem->Mat = mMaterials["bricks0"].get();
	rightCylRitem->Geo = mGeometries["shapeGeo"].get();
	rightCylRitem->PrimitiveType = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
	rightCylRitem->IndexCount = rightCylRitem->Geo->DrawArgs["cylinder"].IndexCount;
	rightCylRitem->StartIndexLocation = rightCylRitem->Geo->DrawArgs["cylinder"].StartIndexLocation;
	rightCylRitem->BaseVertexLocation = rightCylRitem->Geo->DrawArgs["cylinder"].BaseVertexLocation;
	rightCylRitem->Bounds = rightCylRitem->Geo->DrawArgs["cylinder"].Bounds;
	UINT rightCylInstanceCount = 5;
	rightCylRitem->InstanceCount = rightCylInstanceCount;
	mInstanceCount += rightCylInstanceCount;
	rightCylRitem->Instances.resize(rightCylInstanceCount);

	auto leftSphereRitem = std::make_unique<RenderItem>();
	leftSphereRitem->itemIndex = 6;
	leftSphereRitem->TexTransform = MathHelper::Identity4x4();
	//leftSphereRitem->ObjCBIndex = objCBIndex++;
	leftSphereRitem->Mat = mMaterials["stone0"].get();
	leftSphereRitem->Geo = mGeometries["shapeGeo"].get();
	leftSphereRitem->PrimitiveType = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
	leftSphereRitem->IndexCount = leftSphereRitem->Geo->DrawArgs["sphere"].IndexCount;
	leftSphereRitem->StartIndexLocation = leftSphereRitem->Geo->DrawArgs["sphere"].StartIndexLocation;
	leftSphereRitem->BaseVertexLocation = leftSphereRitem->Geo->DrawArgs["sphere"].BaseVertexLocation;
	leftSphereRitem->Bounds = leftSphereRitem->Geo->DrawArgs["sphere"].Bounds;
	UINT leftSphereInstanceCount = 5;
	leftSphereRitem->InstanceCount = leftSphereInstanceCount;
	mInstanceCount += leftSphereInstanceCount;
	leftSphereRitem->Instances.resize(leftCylInstanceCount);

	auto rightSphereRitem = std::make_unique<RenderItem>();
	rightSphereRitem->TexTransform = MathHelper::Identity4x4();
	rightSphereRitem->itemIndex = 7;
	rightSphereRitem->Mat = mMaterials["stone0"].get();
	rightSphereRitem->Geo = mGeometries["shapeGeo"].get();
	rightSphereRitem->PrimitiveType = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
	rightSphereRitem->IndexCount = rightSphereRitem->Geo->DrawArgs["sphere"].IndexCount;
	rightSphereRitem->StartIndexLocation = rightSphereRitem->Geo->DrawArgs["sphere"].StartIndexLocation;
	rightSphereRitem->BaseVertexLocation = rightSphereRitem->Geo->DrawArgs["sphere"].BaseVertexLocation;
	rightSphereRitem->Bounds = rightSphereRitem->Geo->DrawArgs["sphere"].Bounds;
	UINT rightSphereInstanceCount = 5;
	rightSphereRitem->InstanceCount = rightSphereInstanceCount;
	mInstanceCount += rightSphereInstanceCount;
	rightSphereRitem->Instances.resize(rightSphereInstanceCount);

	XMMATRIX brickTexTransform = XMMatrixScaling(1.0f, 1.0f, 1.0f);
	for (size_t i = 0; i < 5; i++)
	{
		XMMATRIX leftCylWorld = XMMatrixTranslation(-5.0f, 1.5f, -10.0f + i * 5.0f);
		XMMATRIX rightCylWorld = XMMatrixTranslation(+5.0f, 1.5f, -10.0f + i * 5.0f);

		XMMATRIX leftSphereWorld = XMMatrixTranslation(-5.0f, 3.5f, -10.0f + i * 5.0f);
		XMMATRIX rightSphereWorld = XMMatrixTranslation(+5.0f, 3.5f, -10.0f + i * 5.0f);	
		XMStoreFloat4x4(&leftCylRitem->Instances[i].World, leftCylWorld);
		XMStoreFloat4x4(&leftCylRitem->Instances[i].TexTransform, brickTexTransform);
		leftCylRitem->Instances[i].MaterialIndex = 0; // brickMat
		XMStoreFloat4x4(&rightCylRitem->Instances[i].World, rightCylWorld);
		XMStoreFloat4x4(&rightCylRitem->Instances[i].TexTransform, brickTexTransform);
		rightCylRitem->Instances[i].MaterialIndex = 0; // brickMat
		XMStoreFloat4x4(&leftSphereRitem->Instances[i].World, leftSphereWorld);
		XMStoreFloat4x4(&leftSphereRitem->Instances[i].TexTransform, brickTexTransform);
		leftSphereRitem->Instances[i].MaterialIndex = 1; // stoneMat
		XMStoreFloat4x4(&rightSphereRitem->Instances[i].World, rightSphereWorld);
		XMStoreFloat4x4(&rightSphereRitem->Instances[i].TexTransform, brickTexTransform);
		rightSphereRitem->Instances[i].MaterialIndex = 1; // stonekMat
	}
	mRitemLayer[(int)RenderLayer::Opaque].push_back(leftCylRitem.get());
	mRitemLayer[(int)RenderLayer::Opaque].push_back(rightCylRitem.get());
	mRitemLayer[(int)RenderLayer::Opaque].push_back(leftSphereRitem.get());
	mRitemLayer[(int)RenderLayer::Opaque].push_back(rightSphereRitem.get());
	mAllRitems.push_back(std::move(leftCylRitem));
	mAllRitems.push_back(std::move(rightCylRitem));
	mAllRitems.push_back(std::move(leftSphereRitem));
	mAllRitems.push_back(std::move(rightSphereRitem));

	auto skyRitem = std::make_unique<RenderItem>();
	skyRitem->Geo = mGeometries["shapeGeo"].get();
	skyRitem->PrimitiveType = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
	skyRitem->IndexCount = skyRitem->Geo->DrawArgs["sphere"].IndexCount;
	skyRitem->StartIndexLocation = skyRitem->Geo->DrawArgs["sphere"].StartIndexLocation;
	skyRitem->BaseVertexLocation = skyRitem->Geo->DrawArgs["sphere"].BaseVertexLocation;
	skyRitem->Bounds = skyRitem->Geo->DrawArgs["sphere"].Bounds;
	UINT skyInstanceCount = 1;
	skyRitem->InstanceCount = skyInstanceCount;
	mInstanceCount += skyInstanceCount;
	skyRitem->Instances.resize(skyInstanceCount);
	XMStoreFloat4x4(&skyRitem->Instances[0].World, XMMatrixScaling(5000.0f, 5000.0f, 5000.0f));
	skyRitem->TexTransform = MathHelper::Identity4x4();

	mRitemLayer[(int)RenderLayer::Sky].push_back(skyRitem.get());
	mAllRitems.push_back(std::move(skyRitem));
}

void CRYCHIC::BuildInstancingSceneRenderItems()
{
	auto boxRitem = std::make_unique<RenderItem>();
	XMStoreFloat4x4(&boxRitem->World, XMMatrixScaling(1.0f, 1.0f, 1.0f));
	XMStoreFloat4x4(&boxRitem->TexTransform, XMMatrixScaling(1.0f, 1.0f, 1.0f));
	//boxRitem->ObjCBIndex = 0;
	boxRitem->Mat = mMaterials["crate0"].get();
	boxRitem->Geo = mGeometries["shapeGeo"].get();
	boxRitem->PrimitiveType = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
	boxRitem->InstanceCount = 0;
	boxRitem->IndexCount = boxRitem->Geo->DrawArgs["box"].IndexCount;
	boxRitem->StartIndexLocation = boxRitem->Geo->DrawArgs["box"].StartIndexLocation;
	boxRitem->BaseVertexLocation = boxRitem->Geo->DrawArgs["box"].BaseVertexLocation;
	boxRitem->Bounds = boxRitem->Geo->DrawArgs["box"].Bounds;

	const int n = 5;
	mInstanceCount = n * n * n;
	boxRitem->Instances.resize(mInstanceCount);
	float width = 200.0f;
	float height = 200.0f;
	float depth = 200.0f;

	float x = -0.5f * width;
	float y = -0.5f * height;
	float z = -0.5f * depth;
	float dx = width / (n - 1);
	float dy = height / (n - 1);
	float dz = depth / (n - 1);
	for (size_t i = 0; i < n; i++)
	{
		for (size_t j = 0; j < n; j++) 
		{
			for (size_t k = 0; k < n; k++) 
			{
				int index = i * n * n + j * n + k;
				XMStoreFloat4x4(&boxRitem->Instances[index].World, XMMatrixTranslation(x + k * dx,
					y + j * dy, z + i * dz));
				XMStoreFloat4x4(&boxRitem->Instances[index].TexTransform, XMMatrixScaling(2.0f, 1.0f, 2.0f));
				boxRitem->Instances[index].MaterialIndex = index % mMaterials.size();
			}
		}
	}
	mRitemLayer[(int)RenderLayer::Opaque].push_back(boxRitem.get());
	mAllRitems.push_back(std::move(boxRitem));
}

void CRYCHIC::DrawRenderItems(ID3D12GraphicsCommandList* cmdList, const std::vector<RenderItem*>& ritems)
{
	// For each render item...
	for (size_t i = 0; i < ritems.size(); ++i)
	{
		auto ri = ritems[i];

		cmdList->IASetVertexBuffers(0, 1, &ri->Geo->VertexBufferView());
		cmdList->IASetIndexBuffer(&ri->Geo->IndexBufferView());
		cmdList->IASetPrimitiveTopology(ri->PrimitiveType);

		// Set instance buffer used by the render item. 
		auto instanceBuffer = mCurrFrameResource->InstanceBuffers[ri->itemIndex]->Resource();
		mCommandList->SetGraphicsRootShaderResourceView(0, instanceBuffer->GetGPUVirtualAddress());
		// debug时发现ri->InstanceCount = 0，因为初始位置看不到这些物体，被裁剪了
		cmdList->DrawIndexedInstanced(ri->IndexCount, ri->InstanceCount, ri->StartIndexLocation, ri->BaseVertexLocation, 0);
	}
}

void CRYCHIC::DrawGBuffer()
{
	mCommandList->RSSetViewports(1, &mScreenViewport);
	mCommandList->RSSetScissorRects(1, &mScissorRect);

	// Change to Render Target.
	mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mDeferred->gBuffer0Resource(),
		D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_RENDER_TARGET));
	mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mDeferred->gBuffer1Resource(),
		D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_RENDER_TARGET));
	mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mDeferred->gBuffer2Resource(),
		D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_RENDER_TARGET));
	mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mDeferred->gBuffer3Resource(),
		D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_RENDER_TARGET));

	UINT passCBByteSize = d3dUtil::CalcConstantBufferByteSize(sizeof(PassConstants));

	mCommandList->ClearRenderTargetView(mDeferred->gBuffer0Rtv(), Colors::Black, 0, nullptr);
	mCommandList->ClearRenderTargetView(mDeferred->gBuffer1Rtv(), Colors::Black, 0, nullptr);
	mCommandList->ClearRenderTargetView(mDeferred->gBuffer2Rtv(), Colors::Black, 0, nullptr);
	mCommandList->ClearRenderTargetView(mDeferred->gBuffer3Rtv(), Colors::Black, 0, nullptr);
	mCommandList->ClearDepthStencilView(DepthStencilView(), D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);

	// Specify the buffers we are going to render to.
	D3D12_CPU_DESCRIPTOR_HANDLE deferredRtvs[4] = { mDeferred->gBuffer0Rtv(), mDeferred->gBuffer1Rtv(),
		mDeferred->gBuffer2Rtv(), mDeferred->gBuffer3Rtv() };
	mCommandList->OMSetRenderTargets(4, deferredRtvs, false, &DepthStencilView());

	DrawRenderItems(mCommandList.Get(), mRitemLayer[int(RenderLayer::Opaque)]);

	// Change back to GENERIC_READ so we can read the texture in a shader.
	mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mDeferred->gBuffer0Resource(),
		D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_GENERIC_READ));
	mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mDeferred->gBuffer1Resource(),
		D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_GENERIC_READ));
	mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mDeferred->gBuffer2Resource(),
		D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_GENERIC_READ));
	mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mDeferred->gBuffer3Resource(),
		D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_GENERIC_READ));
}

std::array<const CD3DX12_STATIC_SAMPLER_DESC, 6> CRYCHIC::GetStaticSamplers()
{
	// Applications usually only need a handful of samplers.  So just define them all up front
	// and keep them available as part of the root signature.  

	const CD3DX12_STATIC_SAMPLER_DESC pointWrap(
		0, // shaderRegister
		D3D12_FILTER_MIN_MAG_MIP_POINT, // filter
		D3D12_TEXTURE_ADDRESS_MODE_WRAP,  // addressU
		D3D12_TEXTURE_ADDRESS_MODE_WRAP,  // addressV
		D3D12_TEXTURE_ADDRESS_MODE_WRAP); // addressW

	const CD3DX12_STATIC_SAMPLER_DESC pointClamp(
		1, // shaderRegister
		D3D12_FILTER_MIN_MAG_MIP_POINT, // filter
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressU
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressV
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP); // addressW

	const CD3DX12_STATIC_SAMPLER_DESC linearWrap(
		2, // shaderRegister
		D3D12_FILTER_MIN_MAG_MIP_LINEAR, // filter
		D3D12_TEXTURE_ADDRESS_MODE_WRAP,  // addressU
		D3D12_TEXTURE_ADDRESS_MODE_WRAP,  // addressV
		D3D12_TEXTURE_ADDRESS_MODE_WRAP); // addressW

	const CD3DX12_STATIC_SAMPLER_DESC linearClamp(
		3, // shaderRegister
		D3D12_FILTER_MIN_MAG_MIP_LINEAR, // filter
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressU
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressV
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP); // addressW

	const CD3DX12_STATIC_SAMPLER_DESC anisotropicWrap(
		4, // shaderRegister
		D3D12_FILTER_ANISOTROPIC, // filter
		D3D12_TEXTURE_ADDRESS_MODE_WRAP,  // addressU
		D3D12_TEXTURE_ADDRESS_MODE_WRAP,  // addressV
		D3D12_TEXTURE_ADDRESS_MODE_WRAP,  // addressW
		0.0f,                             // mipLODBias
		8);                               // maxAnisotropy

	const CD3DX12_STATIC_SAMPLER_DESC anisotropicClamp(
		5, // shaderRegister
		D3D12_FILTER_ANISOTROPIC, // filter
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressU
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressV
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressW
		0.0f,                              // mipLODBias
		8);                                // maxAnisotropy

	return {
		pointWrap, pointClamp,
		linearWrap, linearClamp,
		anisotropicWrap, anisotropicClamp };
}

