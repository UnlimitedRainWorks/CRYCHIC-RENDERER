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
	mSceneSphere.Center = XMFLOAT3(0.0f, 0.0f, 0.0f);
	mSceneSphere.Radius = sqrtf(20.0f * 20.0f + 30.0f * 30.0f);
	UpdateLights();
	//mMainPassCB.Lights[0].Direction = { 0.57735f, -0.57735f, 0.57735f };
	//mMainPassCB.Lights[0].Strength = { 2.8f, 2.8f, 2.8f };
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

	mDynamicCubeMap = std::make_unique<CubeRenderTarget>(md3dDevice.Get(),
		CubeMapSize, CubeMapSize, DXGI_FORMAT_R8G8B8A8_UNORM);

	
	mShadowMap = std::make_unique<ShadowMap>(md3dDevice.Get(),
		2048, 2048, mShadowMapSize);

	mCamera.SetPosition(0.0f, 2.0f, -15.0f);

	BuildCubeFaceCamera(0.0f, 2.0f, -3.0f);

	LoadTextures();
	BuildRootSignature();
	BuildDescriptorHeaps();
	BuildCubeDepthStencil();
	BuildShadersAndInputLayout();
	BuildShapeGeometry();
	BuildGeometry("Models/skull.txt");
	BuildGeometry("Models/car.txt");
	BuildMaterials();
	BuildRenderItems();
	BuildCubeMapRenderItems();
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
	D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc;
	rtvHeapDesc.NumDescriptors = SwapChainBufferCount + mGBufferSize + mCubemapRTSize;
	rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
	rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
	rtvHeapDesc.NodeMask = 0;
	ThrowIfFailed(md3dDevice->CreateDescriptorHeap(
		&rtvHeapDesc, IID_PPV_ARGS(mRtvHeap.GetAddressOf())));

	// 为cubemap rt新增1个dsv
	// for shadowpass add shadowmapsize dsv
	D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc;
	dsvHeapDesc.NumDescriptors = 1 + 1 + mShadowMapSize;
	dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
	dsvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
	dsvHeapDesc.NodeMask = 0;
	ThrowIfFailed(md3dDevice->CreateDescriptorHeap(
		&dsvHeapDesc, IID_PPV_ARGS(mDsvHeap.GetAddressOf())));
	mCubeDSV = CD3DX12_CPU_DESCRIPTOR_HANDLE(
		mDsvHeap->GetCPUDescriptorHandleForHeapStart(),
		1,
		mDsvDescriptorSize
	);
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

	/*mLightRotationAngle += 0.01f * gt.DeltaTime();
	mLightRotationAngle += 0.1f * gt.DeltaTime();
	XMMATRIX R = XMMatrixRotationY(mLightRotationAngle);
	// 这里直接旋转一个主光源
	XMVECTOR lightDir = XMLoadFloat3(&mMainPassCB.Lights[0].Direction);
	XMStoreFloat3(&mMainPassCB.Lights[0].Direction, XMVector3TransformNormal(lightDir, R));
	for (int i = 0; i < 3; ++i)
	{
		XMVECTOR lightDir = XMLoadFloat3(&mBaseLightDirections[i]);
		lightDir = XMVector3TransformNormal(lightDir, R);
		XMStoreFloat3(&mRotatedLightDirections[i], lightDir);
	}*/

	AnimateMaterials(gt);
	UpdateInstanceData(gt);
	UpdateMaterialBuffer(gt);
	//UpdateLights(gt);
	UpdateDirShadowTransform(gt);
	UpdateSpotShadowTransform(gt);
	UpdateMainPassCB(gt);
	UpdateCubeMapFacePassCBs();
	UpdateShadowPassCB(gt);
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
		ThrowIfFailed(mCommandList->Reset(cmdListAlloc.Get(), mPSOs["opaque"].Get()));

		ID3D12DescriptorHeap* descriptorHeaps[] = { mSrvDescriptorHeap.Get() };
		mCommandList->SetDescriptorHeaps(_countof(descriptorHeaps), descriptorHeaps);

		mCommandList->SetGraphicsRootSignature(mRootSignature.Get());

		auto matBuffer = mCurrFrameResource->MaterialBuffer->Resource();
		mCommandList->SetGraphicsRootShaderResourceView(2, matBuffer->GetGPUVirtualAddress());

		mCommandList->SetGraphicsRootDescriptorTable(3, mSrvDescriptorHeap->GetGPUDescriptorHandleForHeapStart());
		
		DrawSceneToShadowMap();

		auto hDesciptor = CD3DX12_GPU_DESCRIPTOR_HANDLE(mSrvDescriptorHeap->GetGPUDescriptorHandleForHeapStart(), 
			mCubeMapTexHeapIndex, mCbvSrvDescriptorSize);
		mCommandList->SetGraphicsRootDescriptorTable(5, hDesciptor);
		
		DrawSceneToCubeMap();

		mCommandList->SetPipelineState(mPSOs["gBuffer"].Get());

		auto passCB = mCurrFrameResource->PassCB->Resource();
		mCommandList->SetGraphicsRootConstantBufferView(1, passCB->GetGPUVirtualAddress());

		DrawGBuffer();

		mCommandList->SetPipelineState(mPSOs["deferred"].Get());

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

		mCommandList->SetPipelineState(mPSOs["opaque"].Get());

		CD3DX12_GPU_DESCRIPTOR_HANDLE dynamicTexDescriptor(
			mSrvDescriptorHeap->GetGPUDescriptorHandleForHeapStart());
		dynamicTexDescriptor.Offset(mDynamicTexHeapIndex, mCbvSrvDescriptorSize);
		mCommandList->SetGraphicsRootDescriptorTable(5, dynamicTexDescriptor);
		DrawRenderItems(mCommandList.Get(), mRitemLayer[int(RenderLayer::OpaqueDynamicReflectors)]);

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

		ID3D12DescriptorHeap* descriptorHeaps[] = { mSrvDescriptorHeap.Get() };
		mCommandList->SetDescriptorHeaps(_countof(descriptorHeaps), descriptorHeaps);

		mCommandList->SetGraphicsRootSignature(mRootSignature.Get());

		// Bind all the materials used in this scene.  For structured buffers, we can bypass the heap and 
		// set as a root descriptor.
		auto matBuffer = mCurrFrameResource->MaterialBuffer->Resource();
		mCommandList->SetGraphicsRootShaderResourceView(2, matBuffer->GetGPUVirtualAddress());

		mCommandList->SetGraphicsRootDescriptorTable(3, mSrvDescriptorHeap->GetGPUDescriptorHandleForHeapStart());

		DrawSceneToShadowMap();

		CD3DX12_GPU_DESCRIPTOR_HANDLE cubeMapTexDescriptor(
			mSrvDescriptorHeap->GetGPUDescriptorHandleForHeapStart());
		cubeMapTexDescriptor.Offset(mCubeMapTexHeapIndex, mCbvSrvDescriptorSize);
		mCommandList->SetGraphicsRootDescriptorTable(5, cubeMapTexDescriptor);

		//mCommandList->SetPipelineState(mPSOs["opaque"].Get());
		DrawSceneToCubeMap();
		
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

		auto passCB = mCurrFrameResource->PassCB->Resource();
		mCommandList->SetGraphicsRootConstantBufferView(1, passCB->GetGPUVirtualAddress());

		// bind dynamiccubemapTex for opaqueDynamicReflectors
		CD3DX12_GPU_DESCRIPTOR_HANDLE dynamicTexDescriptor(
			mSrvDescriptorHeap->GetGPUDescriptorHandleForHeapStart());
		dynamicTexDescriptor.Offset(mDynamicTexHeapIndex, mCbvSrvDescriptorSize);
		mCommandList->SetGraphicsRootDescriptorTable(5, dynamicTexDescriptor);

		mCommandList->SetPipelineState(mPSOs["opaque"].Get());
		DrawRenderItems(mCommandList.Get(), mRitemLayer[int(RenderLayer::OpaqueDynamicReflectors)]);

		mCommandList->SetGraphicsRootDescriptorTable(5, cubeMapTexDescriptor);
		
		DrawRenderItems(mCommandList.Get(), mRitemLayer[int(RenderLayer::Opaque)]);

		//mCommandList->SetPipelineState(mPSOs["shadowDebug"].Get());
		//DrawRenderItems(mCommandList.Get(), mRitemLayer[(int)RenderLayer::Debug]);
		//mCommandList->SetGraphicsRootDescriptorTable(5, cubeMapTexDescriptor);
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

void CRYCHIC::UpdateInstanceData(const GameTimer& gt)
{
	XMMATRIX view = mCamera.GetView();
	XMMATRIX invView = XMMatrixInverse(&XMMatrixDeterminant(view), view);

	UINT totalVisibleInstanceCount = 0;
	UINT totalSceneInstanceCount = 0;
	for (int i = 0; i < mInstancesCount.size(); ++i)
	{
		// 每个渲染项取得自己的InstanceData
		auto currInstanceBuffer = mCurrFrameResource->InstanceBuffers[mAllRitems[i]->itemIndex].get();
		const auto& instanceData = mAllRitems[i]->Instances;
		//totalInstanceCount += e->InstanceCount;
		int visibleInstanceCount = 0;
		for (UINT j = 0; j < (UINT)instanceData.size(); ++j)
		{
			XMMATRIX world = XMLoadFloat4x4(&instanceData[j].World);
			XMMATRIX texTransform = XMLoadFloat4x4(&instanceData[j].TexTransform);
			XMMATRIX invWorld = XMMatrixInverse(&XMMatrixDeterminant(world), world);
			XMMATRIX viewToLocal = XMMatrixMultiply(invView, invWorld);

			BoundingFrustum localSpaceFrustum;
			mCamFrustum.Transform(localSpaceFrustum, viewToLocal);

			// 如果检测结果为相交或者不进行视锥裁剪
			// 通过视锥裁剪检测的对象的数据才会被加入instance缓冲区
			// 关闭视锥裁剪就直接加入缓冲区
			if ((i >= mSceneItemCount) || (localSpaceFrustum.Contains(mAllRitems[i]->Bounds) != DISJOINT) || (mFrustumCullingEnabled == false))
			{
				InstanceData data;
				XMStoreFloat4x4(&data.World, XMMatrixTranspose(world));
				XMStoreFloat4x4(&data.TexTransform, XMMatrixTranspose(texTransform));
				data.MaterialIndex = instanceData[j].MaterialIndex;

				currInstanceBuffer->CopyData(visibleInstanceCount++, data);
			}

		}
		mAllRitems[i]->InstanceCount = visibleInstanceCount;
		if (i < mSceneItemCount)
		{
			totalSceneInstanceCount += mAllRitems[i]->InstanceCount;
			totalVisibleInstanceCount += visibleInstanceCount;
		}
	}
		std::wostringstream outs;
		outs.precision(6);
		outs << L"Instancing and Culling Demo" <<
			L"    " << totalVisibleInstanceCount <<
			L" objects visible out of " << totalSceneInstanceCount;
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
			matData.NormalMapIndex = mat->NormalSrvHeapIndex;
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
	
	//XMMATRIX shadowTransform1 = XMLoadFloat4x4(&mShadowTransforms[0]);
	//mMainPassCB.ShadowTransforms.resize(mShadowTransforms.size());
	//for (int i = 0; i < mShadowTransforms.size(); i++)
	//{
	//	auto st = XMMatrixTranspose(XMLoadFloat4x4(&mShadowTransforms[i]));
	//	XMStoreFloat4x4(&mMainPassCB.ShadowTransforms[i], XMMatrixTranspose(st));
	//}
	//mMainPassCB.ShadowTransforms.resize(1);
	XMStoreFloat4x4(&mMainPassCB.View, XMMatrixTranspose(view));
	XMStoreFloat4x4(&mMainPassCB.InvView, XMMatrixTranspose(invView));
	XMStoreFloat4x4(&mMainPassCB.Proj, XMMatrixTranspose(proj));
	XMStoreFloat4x4(&mMainPassCB.InvProj, XMMatrixTranspose(invProj));
	XMStoreFloat4x4(&mMainPassCB.ViewProj, XMMatrixTranspose(viewProj));
	XMStoreFloat4x4(&mMainPassCB.InvViewProj, XMMatrixTranspose(invViewProj));

	for (size_t i = 0; i < mShadowMapSize; i++)
	{
		XMMATRIX shadowTransform = XMLoadFloat4x4(&mShadowTransforms[i]);
		XMStoreFloat4x4(&mMainPassCB.ShadowTransforms[i], XMMatrixTranspose(shadowTransform));
	}
	
	mMainPassCB.EyePosW = mCamera.GetPosition3f();
	mMainPassCB.RenderTargetSize = XMFLOAT2((float)mClientWidth, (float)mClientHeight);
	mMainPassCB.InvRenderTargetSize = XMFLOAT2(1.0f / mClientWidth, 1.0f / mClientHeight);
	mMainPassCB.NearZ = 1.0f;
	mMainPassCB.FarZ = 1000.0f;
	mMainPassCB.TotalTime = gt.TotalTime();
	mMainPassCB.DeltaTime = gt.DeltaTime();
	mMainPassCB.AmbientLight = { 0.25f, 0.25f, 0.35f, 1.0f };

	//mMainPassCB.Lights[0].Direction = mBaseLightDirections[0];
	//mMainPassCB.Lights[0].Strength = { 2.9f, 2.8f, 2.7f };
	
	//mMainPassCB.Lights[1].Direction = mBaseLightDirections[1];
	//mMainPassCB.Lights[1].Strength = { 2.4f, 2.4f, 2.4f };
	//mMainPassCB.Lights[2].Direction = mBaseLightDirections[2];
	//mMainPassCB.Lights[2].Strength = { 2.2f, 2.2f, 2.2f };

	//int dirLightsNum = 3;
	//int pointLightsNum = 0;
	//int spotLightsNum = 0;
	////// Directional Lights
	//////mMainPassCB.Lights[0].Direction = { 0.57735f, -0.57735f, 0.57735f };
	////mMainPassCB.Lights[0].Strength = { 4.8f, 4.8f, 4.8f };
	////mMainPassCB.Lights[1].Direction = { -0.57735f, -0.57735f, 0.57735f };
	////mMainPassCB.Lights[1].Strength = { 0.4f, 0.4f, 0.4f };
	////mMainPassCB.Lights[2].Direction = { 0.0f, -0.707f, -0.707f };
	////mMainPassCB.Lights[2].Strength = { 0.2f, 0.2f, 0.2f };

	//// Point Lights
	//for (int i = dirLightsNum, j = 0; i < dirLightsNum + pointLightsNum; i++,j++)
	//{
	//	mMainPassCB.Lights[i].Position = { -5.0f, 4.5f, -10.0f + j * 5.0f};
	//	mMainPassCB.Lights[i].Strength = { 1.2f, 1.0f, 1.1f };
	//	mMainPassCB.Lights[i].FalloffStart = 1.0f;
	//	mMainPassCB.Lights[i].FalloffEnd = 8.0f;
	//}

	// Spot Lights
	/*for (int i = dirLightsNum + pointLightsNum, j = 0; i < dirLightsNum + pointLightsNum + spotLightsNum; i++, j++)
	{
		mMainPassCB.Lights[i].Position = { 4.0f, 4.5f, -10.0f + j * 5.0f };
		mMainPassCB.Lights[i].Direction = { 0.0f, -1.0f, 0.0f };
		mMainPassCB.Lights[i].Strength = { 100.2f, 2.1f, 2.0f };
		mMainPassCB.Lights[i].FalloffStart = 1.0f;
		mMainPassCB.Lights[i].FalloffEnd = 10.1f;
		mMainPassCB.Lights[i].SpotPower = 8.0f;
	}*/

	auto currPassCB = mCurrFrameResource->PassCB.get();
	currPassCB->CopyData(0, mMainPassCB);
}

void CRYCHIC::UpdateCubeMapFacePassCBs()
{
	for (int i = 0; i < mCubemapRTSize; i++)
	{
		PassConstants cubeFacePassCB = mMainPassCB;

		XMMATRIX view = mCubeMapCamera[i].GetView();
		XMMATRIX proj = mCubeMapCamera[i].GetProj();

		XMMATRIX viewProj = view * proj;
		XMMATRIX invView = XMMatrixInverse(&XMMatrixDeterminant(view), view);
		XMMATRIX invProj = XMMatrixInverse(&XMMatrixDeterminant(proj), proj);
		XMMATRIX invViewProj = XMMatrixInverse(&XMMatrixDeterminant(viewProj), viewProj);

		XMStoreFloat4x4(&cubeFacePassCB.View, XMMatrixTranspose(view));
		XMStoreFloat4x4(&cubeFacePassCB.Proj, XMMatrixTranspose(proj));
		XMStoreFloat4x4(&cubeFacePassCB.ViewProj, XMMatrixTranspose(viewProj));
		XMStoreFloat4x4(&cubeFacePassCB.InvView, XMMatrixTranspose(invView));
		XMStoreFloat4x4(&cubeFacePassCB.InvProj, XMMatrixTranspose(invProj));
		XMStoreFloat4x4(&cubeFacePassCB.InvViewProj, XMMatrixTranspose(invViewProj));

		cubeFacePassCB.EyePosW = mCubeMapCamera[i].GetPosition3f();
		cubeFacePassCB.RenderTargetSize = XMFLOAT2((float)CubeMapSize, (float)CubeMapSize);
		cubeFacePassCB.InvRenderTargetSize = XMFLOAT2(1.0f / (float)CubeMapSize, 1.0f / (float)CubeMapSize);

		auto currPassCB = mCurrFrameResource->PassCB.get();

		// 0 is for mainpass , 1 - 6 for cubemapcamera
		currPassCB->CopyData(1 + i, cubeFacePassCB);
	}
}

void CRYCHIC::UpdateDirShadowTransform(const GameTimer& gt)
{
	XMVECTOR lightDir = XMLoadFloat3(&mBaseLightDirections[0]);
	XMVECTOR lightPos = -2.0f * mSceneSphere.Radius * lightDir;
	XMVECTOR targetPos = XMLoadFloat3(&mSceneSphere.Center);
	XMVECTOR lightUp = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
	XMMATRIX lightView = XMMatrixLookAtLH(lightPos, targetPos, lightUp);

	XMStoreFloat3(&mLightPosW, lightPos);

	// Transform bouding sphere to light space.
	XMFLOAT3 sphereCenterLS;
	XMStoreFloat3(&sphereCenterLS, XMVector3TransformCoord(targetPos, lightView));

	float l = sphereCenterLS.x - mSceneSphere.Radius;
	float b = sphereCenterLS.y - mSceneSphere.Radius;
	float n = sphereCenterLS.z - mSceneSphere.Radius;
	float r = sphereCenterLS.x + mSceneSphere.Radius;
	float t = sphereCenterLS.y + mSceneSphere.Radius;
	float f = sphereCenterLS.z + mSceneSphere.Radius;

	mLightNearZ = n;
	mLightFarZ = f;
	XMMATRIX lightProj = XMMatrixOrthographicOffCenterLH(l, r, b, t, n, f);

	XMMATRIX T(
		0.5f, 0.0f, 0.0f, 0.0f,
		0.0f, -0.5f, 0.0f, 0.0f,
		0.0f, 0.0f, 1.0f, 0.0f,
		0.5f, 0.5f, 0.0f, 1.0f);
	XMMATRIX S = lightView * lightProj * T;
	XMStoreFloat4x4(&mLightView, lightView);
	XMStoreFloat4x4(&mLightProj, lightProj);
	XMStoreFloat4x4(&mShadowTransform, S);
	XMStoreFloat4x4(&mLightViews[0], lightView);
	XMStoreFloat4x4(&mLightProjs[0], lightProj);
	XMStoreFloat4x4(&mShadowTransforms[0], S);
}

void CRYCHIC::UpdateSpotShadowTransform(const GameTimer& gt)
{
	for (size_t i = 0; i < spotLightsNum; i++)
	{
		XMVECTOR lightDir = XMVectorSet(0.0f, -1.0f, 0.0f, 0.0f);
		XMVECTOR lightPos = XMLoadFloat3(&mMainPassCB.Lights[dirLightsNum + pointLightsNum + i].Position);
		XMVECTOR targetPos = { 5.0f, 0.0f, -10.0f + i * 5.0f };
		XMVECTOR lightUp = XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f);
		XMMATRIX lightView = XMMatrixLookAtLH(lightPos, targetPos, lightUp);

		XMStoreFloat3(&mLightPosW, lightPos);

		// Transform bouding sphere to light space.
		//XMFLOAT3 sphereCenterLS;
		//XMStoreFloat3(&sphereCenterLS, XMVector3TransformCoord(targetPos, lightView));

		//float l = sphereCenterLS.x - mSceneSphere.Radius;
		//float b = sphereCenterLS.y - mSceneSphere.Radius;
		//float n = sphereCenterLS.z - mSceneSphere.Radius;
		//float r = sphereCenterLS.x + mSceneSphere.Radius;
		//float t = sphereCenterLS.y + mSceneSphere.Radius;
		//float f = sphereCenterLS.z + mSceneSphere.Radius;

		//mLightNearZ = n;
		//mLightFarZ = f;
		//XMMATRIX lightProj = XMMatrixOrthographicOffCenterLH(l, r, b, t, n, f);
		XMMATRIX lightProj = XMMatrixPerspectiveFovLH(0.5f * XM_PI, 1.0f, 0.1f, 3.5f);

		XMMATRIX T(
			0.5f, 0.0f, 0.0f, 0.0f,
			0.0f, -0.5f, 0.0f, 0.0f,
			0.0f, 0.0f, 1.0f, 0.0f,
			0.5f, 0.5f, 0.0f, 1.0f);
		XMMATRIX S = lightView * lightProj * T;
		
		// 主平行光光源投射阴影，这里dirnum = 1
		XMStoreFloat4x4(&mLightViews[1 + pointLightsNum + i], lightView);
		XMStoreFloat4x4(&mLightProjs[1 + pointLightsNum + i], lightProj);
		XMStoreFloat4x4(&mShadowTransforms[1 + pointLightsNum + i], S);
	}
}

void CRYCHIC::UpdateShadowPassCB(const GameTimer& gt)
{
	for (size_t i = 0; i < dirLightsNum + pointLightsNum + spotLightsNum; i++)
	{
		XMMATRIX view = XMLoadFloat4x4(&mLightViews[i]);
		XMMATRIX proj = XMLoadFloat4x4(&mLightProjs[i]);

		XMMATRIX viewProj = XMMatrixMultiply(view, proj);
		XMMATRIX invView = XMMatrixInverse(&XMMatrixDeterminant(view), view);
		XMMATRIX invProj = XMMatrixInverse(&XMMatrixDeterminant(proj), proj);
		XMMATRIX invViewProj = XMMatrixMultiply(invProj, invView);

		UINT w = mShadowMap->Width();
		UINT h = mShadowMap->Height();

		XMStoreFloat4x4(&mShadowPassCB.View, XMMatrixTranspose(view));
		XMStoreFloat4x4(&mShadowPassCB.Proj, XMMatrixTranspose(proj));
		XMStoreFloat4x4(&mShadowPassCB.InvView, XMMatrixTranspose(invView));
		XMStoreFloat4x4(&mShadowPassCB.InvProj, XMMatrixTranspose(invProj));
		XMStoreFloat4x4(&mShadowPassCB.ViewProj, XMMatrixTranspose(viewProj));
		XMStoreFloat4x4(&mShadowPassCB.InvViewProj, XMMatrixTranspose(invViewProj));

		mShadowPassCB.EyePosW = mLightPosW;
		mShadowPassCB.RenderTargetSize = XMFLOAT2((float)w, (float)h);
		mShadowPassCB.InvRenderTargetSize = XMFLOAT2(1.0f / w, 1.0f / h);
		mShadowPassCB.NearZ = mLightNearZ;
		mShadowPassCB.FarZ = mLightFarZ;

		auto currPassCB = mCurrFrameResource->PassCB.get();
		currPassCB->CopyData(1 + mDynamicCubemapSize * 6 + i, mShadowPassCB);
	}
}

// 设置灯光信息
void CRYCHIC::UpdateLights()
{
	// Directional Lights
	mMainPassCB.Lights[0].Direction = mBaseLightDirections[0];
	mMainPassCB.Lights[0].Strength = { 2.9f, 2.8f, 2.7f };
	//mMainPassCB.Lights[1].Direction = mBaseLightDirections[1];
	//mMainPassCB.Lights[1].Strength = { 2.4f, 2.4f, 2.4f };
	//mMainPassCB.Lights[2].Direction = mBaseLightDirections[2];
	//mMainPassCB.Lights[2].Strength = { 2.2f, 2.2f, 2.2f };

	// 平行光设置为只有主光源产生阴影
	mShadowMapSize = 1 + pointLightsNum + spotLightsNum;
	// Point Lights
	// 透视投影矩阵、cubemap式的shadowmap
	for (int i = dirLightsNum, j = 0; i < dirLightsNum + pointLightsNum; i++, j++)
	{
		mMainPassCB.Lights[i].Position = { -5.0f, 4.5f, -10.0f + j * 5.0f };
		mMainPassCB.Lights[i].Strength = { 1.2f, 1.0f, 1.1f };
		mMainPassCB.Lights[i].FalloffStart = 1.0f;
		mMainPassCB.Lights[i].FalloffEnd = 8.0f;
	}

	// Spot Lights
	mMainPassCB.Lights[1].Position = { 4.0f, 3.5f, -10.0f };
	mMainPassCB.Lights[1].Direction = { 0.0f, -1.0f, 0.0f };
	mMainPassCB.Lights[1].Strength = { 10.2f, 10.1f, 10.0f };
	mMainPassCB.Lights[1].FalloffStart = 1.0f;
	mMainPassCB.Lights[1].FalloffEnd = 10.1f;
	mMainPassCB.Lights[1].SpotPower = 8.0f;
	
	mMainPassCB.Lights[2].Position = { 2.0f, 7.5f, 0.0f };
	mMainPassCB.Lights[2].Direction = { 0.0f, -1.0f, 0.0f };
	mMainPassCB.Lights[2].Strength = { 10.2f, 10.1f, 10.0f };
	mMainPassCB.Lights[2].FalloffStart = 1.0f;
	mMainPassCB.Lights[2].FalloffEnd = 30.1f;
	mMainPassCB.Lights[2].SpotPower = 8.0f;

}

// 设置灯光信息
void CRYCHIC::UpdateLights(const GameTimer& gt)
{
	// Directional Lights
	mMainPassCB.Lights[0].Direction = mBaseLightDirections[0];
	mMainPassCB.Lights[0].Strength = { 2.9f, 2.8f, 2.7f };
	mMainPassCB.Lights[1].Direction = mBaseLightDirections[1];
	mMainPassCB.Lights[1].Strength = { 2.4f, 2.4f, 2.4f };
	mMainPassCB.Lights[2].Direction = mBaseLightDirections[2];
	mMainPassCB.Lights[2].Strength = { 2.2f, 2.2f, 2.2f };

	// 平行光设置为只有主光源产生阴影
	mShadowMapSize = 1 + pointLightsNum + spotLightsNum;

	// Point Lights
	// 透视投影矩阵、cubemap式的shadowmap
	for (int i = dirLightsNum, j = 0; i < dirLightsNum + pointLightsNum; i++, j++)
	{
		mMainPassCB.Lights[i].Position = { -5.0f, 4.5f, -10.0f + j * 5.0f };
		mMainPassCB.Lights[i].Strength = { 1.2f, 1.0f, 1.1f };
		mMainPassCB.Lights[i].FalloffStart = 1.0f;
		mMainPassCB.Lights[i].FalloffEnd = 8.0f;
	}

	// Spot Lights
	for (int i = dirLightsNum + pointLightsNum, j = 0; i < dirLightsNum + pointLightsNum + spotLightsNum; i++, j++)
	{
		mMainPassCB.Lights[i].Position = { 5.0f, 4.5f, -10.0f + j * 5.0f };
		mMainPassCB.Lights[i].Direction = { 0.0f, -1.0f, 0.0f };
		mMainPassCB.Lights[i].Strength = { 1.2f, 1.1f, 1.0f };
		mMainPassCB.Lights[i].FalloffStart = 1.0f;
		mMainPassCB.Lights[i].FalloffEnd = 10.1f;
		mMainPassCB.Lights[i].SpotPower = 8.0f;
	}
}

void CRYCHIC::BuildCubeFaceCamera(float x, float y, float z)
{
	XMFLOAT3 center(x, y, z);
	XMFLOAT3 worldUp(0.0f, 1.0f, 0.0f);

	XMFLOAT3 targets[6] =
	{
		XMFLOAT3(x + 1.0f, y, z),
		XMFLOAT3(x - 1.0f, y, z),
		XMFLOAT3(x, y + 1.0f, z),
		XMFLOAT3(x, y - 1.0f, z),
		XMFLOAT3(x, y, z + 1.0f),
		XMFLOAT3(x, y, z - 1.0f),
	};

	XMFLOAT3 ups[6] =
	{
		XMFLOAT3(0.0f, 1.0f, 0.0f),
		XMFLOAT3(0.0f, 1.0f, 0.0f),
		XMFLOAT3(0.0f, 0.0f, -1.0f), // +y
		XMFLOAT3(0.0f, 0.0f, 1.0f),	 //  -y
		XMFLOAT3(0.0f, 1.0f, 0.0f),
		XMFLOAT3(0.0f, 1.0f, 0.0f),
	};

	for (int i = 0; i < 6; i++)
	{
		mCubeMapCamera[i].LookAt(center, targets[i], ups[i]);
		mCubeMapCamera[i].SetLens(0.5f * XM_PI, 1.0f, 0.1f, 1000.0f);
		mCubeMapCamera[i].UpdateViewMatrix();
	}
}

void CRYCHIC::LoadTextures()
{
	std::vector<std::string> texs =
	{
		"bricksTex",
		"bricksNormalTex",
		"stoneTex",
		"tileTex",
		"tileNormalTex",
		"crateTex",
		"crate02Tex",
		"flareTex",
		"flareNormalTex",
		"iceTex",
		"defaultTex",
		"defaultNormalTex"
	};

	std::vector<std::string> cubeMapTexs =
	{
		"skyCubeMap"
	};

	std::vector<std::wstring> fileNames = {
		L"Textures/bricks2.dds",
		L"Textures/bricks2_nmap.dds",
		L"Textures/stone.dds",
		L"Textures/tile.dds",
		L"Textures/tile_nmap.dds",
		L"Textures/WoodCrate01.dds",
		L"Textures/WoodCrate02.dds",
		L"Textures/flare.dds",
		L"Textures/flare_NRM.dds",
		L"Textures/ice.dds",
		L"Textures/white1x1.dds",
		L"Textures/default_nmap.dds",
	};

	std::vector<std::wstring> cubeMapFiles = {
		L"Textures/snowcube1024.dds"
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

	for (int i = 0; i < cubeMapTexs.size(); i++)
	{
		auto tex = std::make_unique<Texture>();
		tex->Name = cubeMapTexs[i];
		tex->Filename = cubeMapFiles[i];
		ThrowIfFailed(DirectX::CreateDDSTextureFromFile12(md3dDevice.Get(),
			mCommandList.Get(), tex->Filename.c_str(),
			tex->Resource, tex->UploadHeap));
		mTextures[tex->Name] = std::move(tex);
	}

	// 记录纹理数量后续用来创建srv
	mTexSize = texs.size();
	mCubemapSize = cubeMapTexs.size();
	texNames = std::move(texs);
	cubeMapNames = std::move(cubeMapTexs);
}

void CRYCHIC::BuildRootSignature()
{
	CD3DX12_DESCRIPTOR_RANGE texTable;
	texTable.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, mTexSize + mShadowMapSize, 0, 0);

	CD3DX12_DESCRIPTOR_RANGE cubeMapTable;
	cubeMapTable.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, mCubemapSize, mTexSize + mShadowMapSize, 0);

	CD3DX12_DESCRIPTOR_RANGE gBufferTable;
	gBufferTable.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 4, mTexSize + mCubemapSize + mShadowMapSize, 0);

	// Root parameter can be a table, root descriptor or root constants.
	CD3DX12_ROOT_PARAMETER slotRootParameter[6];

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
	// cubemap/shadowmap descriptor table
	slotRootParameter[5].InitAsDescriptorTable(1, &cubeMapTable, D3D12_SHADER_VISIBILITY_PIXEL);

	auto staticSamplers = GetStaticSamplers();

	// A root signature is an array of root parameters.
	CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(6, slotRootParameter,
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
	srvHeapDesc.NumDescriptors = mTexSize + mShadowMapSize + mCubemapSize + mDynamicCubemapSize+ mGBufferSize;
	srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	ThrowIfFailed(md3dDevice->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&mSrvDescriptorHeap)));

	auto srvCpuStart = mSrvDescriptorHeap->GetCPUDescriptorHandleForHeapStart();
	auto srvGpuStart = mSrvDescriptorHeap->GetGPUDescriptorHandleForHeapStart();
	auto rtvCpuStart = mRtvHeap->GetCPUDescriptorHandleForHeapStart();
	auto dsvCpuStart = mDsvHeap->GetCPUDescriptorHandleForHeapStart();

	mShadowMapHeapIndex = mTexSize;
	mCubeMapTexHeapIndex = mShadowMapHeapIndex + mShadowMapSize;
	mDynamicTexHeapIndex = mCubeMapTexHeapIndex + mCubemapSize;
	gBufferHeapIndex = mDynamicTexHeapIndex + mDynamicCubemapSize;

	D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
	srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	srvDesc.Texture2D.MostDetailedMip = 0;
	srvDesc.Texture2D.ResourceMinLODClamp = 0.0f;

	// 自动根据纹理数量创建srv
	for (int i = 0; i < texNames.size(); i++)
	{
		auto texName = texNames[i];
		auto tex = mTextures[texName]->Resource;
		srvDesc.Format = tex->GetDesc().Format;
		srvDesc.Texture2D.MipLevels = tex->GetDesc().MipLevels;
		md3dDevice->CreateShaderResourceView(tex.Get(), &srvDesc,
			CD3DX12_CPU_DESCRIPTOR_HANDLE(srvCpuStart, i, mCbvSrvDescriptorSize));
	}

	mShadowMap->BuildDescriptors(
		CD3DX12_CPU_DESCRIPTOR_HANDLE(srvCpuStart, mShadowMapHeapIndex, mCbvSrvDescriptorSize),
		CD3DX12_GPU_DESCRIPTOR_HANDLE(srvGpuStart, mShadowMapHeapIndex, mCbvSrvDescriptorSize),
		CD3DX12_CPU_DESCRIPTOR_HANDLE(dsvCpuStart, 2, mDsvDescriptorSize));

	for (int i = 0; i < cubeMapNames.size(); i++)
	{
		auto cubeMapName = cubeMapNames[i];
		auto cubeMap = mTextures[cubeMapName]->Resource;
		srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
		srvDesc.Format = cubeMap->GetDesc().Format;
		srvDesc.TextureCube.MipLevels = cubeMap->GetDesc().MipLevels;
		srvDesc.TextureCube.MostDetailedMip = 0;
		srvDesc.TextureCube.ResourceMinLODClamp = 0.0f;
		md3dDevice->CreateShaderResourceView(cubeMap.Get(), &srvDesc, 
			CD3DX12_CPU_DESCRIPTOR_HANDLE(srvCpuStart, i + mCubeMapTexHeapIndex, mCbvSrvDescriptorSize));
	}

	// cubemap RTV goes after the swap chain descriptors.
	int rtvOffset = SwapChainBufferCount;

	CD3DX12_CPU_DESCRIPTOR_HANDLE cubeRtvHandles[6];
	for (int i = 0; i < 6; i++)
	{
		cubeRtvHandles[i] = CD3DX12_CPU_DESCRIPTOR_HANDLE(rtvCpuStart, rtvOffset + i, mRtvDescriptorSize);
	}

	mDynamicCubeMap->BuildDescriptors(
		CD3DX12_CPU_DESCRIPTOR_HANDLE(srvCpuStart, mDynamicTexHeapIndex, mCbvSrvDescriptorSize),
		CD3DX12_GPU_DESCRIPTOR_HANDLE(srvGpuStart, mDynamicTexHeapIndex, mCbvSrvDescriptorSize),
		cubeRtvHandles);
	
	CD3DX12_CPU_DESCRIPTOR_HANDLE gBufferRtvHandles;
	gBufferRtvHandles = CD3DX12_CPU_DESCRIPTOR_HANDLE(rtvCpuStart, rtvOffset + mCubemapSize * mCubemapRTSize, mRtvDescriptorSize);

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
	mShaders["shadowMappingVS"] = d3dUtil::CompileShader(L"Shaders\\Shadows.hlsl", nullptr, "shadowVS", "vs_5_1");
	mShaders["shadowMappingPS"] = d3dUtil::CompileShader(L"Shaders\\Shadows.hlsl", nullptr, "shadowPS", "ps_5_1");
	mShaders["shadowDebugVS"] = d3dUtil::CompileShader(L"Shaders\\ShadowsDebug.hlsl", nullptr, "shadowDebugVS", "vs_5_1");
	mShaders["shadowDebugPS"] = d3dUtil::CompileShader(L"Shaders\\ShadowsDebug.hlsl", nullptr, "shadowDebugPS", "ps_5_1");
	
	mInputLayout =
	{
		{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TANGENT", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
	};
}

void CRYCHIC::BuildShapeGeometry()
{
	GeometryGenerator geoGen;
	GeometryGenerator::MeshData box = geoGen.CreateBox(1.0f, 1.0f, 1.0f, 3);
	GeometryGenerator::MeshData grid = geoGen.CreateGrid(20.0f, 30.0f, 60, 40);
	GeometryGenerator::MeshData sphere = geoGen.CreateSphere(0.5f, 20, 20);
	GeometryGenerator::MeshData cylinder = geoGen.CreateCylinder(0.5f, 0.3f, 3.0f, 20, 20);
	GeometryGenerator::MeshData quad = geoGen.CreateQuad(0.0f, 0.0f, 1.0f, 1.0f, 0.0f);

	//
	// We are concatenating all the geometry into one big vertex/index buffer.  So
	// define the regions in the buffer each submesh covers.
	//

	// Cache the vertex offsets to each object in the concatenated vertex buffer.
	UINT boxVertexOffset = 0;
	UINT gridVertexOffset = (UINT)box.Vertices.size();
	UINT sphereVertexOffset = gridVertexOffset + (UINT)grid.Vertices.size();
	UINT cylinderVertexOffset = sphereVertexOffset + (UINT)sphere.Vertices.size();
	UINT quadVertexOffset = cylinderVertexOffset + (UINT)quad.Vertices.size();

	// Cache the starting index for each object in the concatenated index buffer.
	UINT boxIndexOffset = 0;
	UINT gridIndexOffset = (UINT)box.Indices32.size();
	UINT sphereIndexOffset = gridIndexOffset + (UINT)grid.Indices32.size();
	UINT cylinderIndexOffset = sphereIndexOffset + (UINT)sphere.Indices32.size();
	UINT quadIndexOffset = cylinderIndexOffset + (UINT)quad.Indices32.size();

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

	SubmeshGeometry quadSubmesh;
	quadSubmesh.IndexCount = (UINT)quad.Indices32.size();
	quadSubmesh.StartIndexLocation = quadIndexOffset;
	quadSubmesh.BaseVertexLocation = quadVertexOffset;

	//
	// Extract the vertex elements we are interested in and pack the
	// vertices of all the meshes into one vertex buffer.
	//

	auto totalVertexCount =
		box.Vertices.size() +
		grid.Vertices.size() +
		sphere.Vertices.size() +
		cylinder.Vertices.size() +
		quad.Vertices.size();

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
	
	vMin = XMLoadFloat3(&vMinf3);
	vMax = XMLoadFloat3(&vMaxf3);
	for (size_t i = 0; i < quad.Vertices.size(); i++, k++)
	{
		vertices[k].Pos = quad.Vertices[i].Position;
		vertices[k].Normal = quad.Vertices[i].Normal;
		vertices[k].TexC = quad.Vertices[i].TexC;
		XMVECTOR P = XMLoadFloat3(&vertices[k].Pos);
		vMin = XMVectorMin(vMin, P);
		vMax = XMVectorMax(vMax, P);
	}
	XMStoreFloat3(&bounds.Center, 0.5f * (vMin + vMax));
	XMStoreFloat3(&bounds.Extents, 0.5f * (vMax - vMin));
	quadSubmesh.Bounds = bounds;

	indices.insert(indices.end(), std::begin(box.GetIndices16()), std::end(box.GetIndices16()));
	indices.insert(indices.end(), std::begin(grid.GetIndices16()), std::end(grid.GetIndices16()));
	indices.insert(indices.end(), std::begin(sphere.GetIndices16()), std::end(sphere.GetIndices16()));
	indices.insert(indices.end(), std::begin(cylinder.GetIndices16()), std::end(cylinder.GetIndices16()));
	indices.insert(indices.end(), std::begin(quad.GetIndices16()), std::end(quad.GetIndices16()));

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
	geo->DrawArgs["quad"] = quadSubmesh;

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

	//
	// PSO for shadow mapping pass.
	//
	D3D12_GRAPHICS_PIPELINE_STATE_DESC shadowMapPsoDesc = opaquePsoDesc;
	shadowMapPsoDesc.RasterizerState.DepthBias = 100000;
	shadowMapPsoDesc.RasterizerState.DepthBiasClamp = 0.0f;
	shadowMapPsoDesc.RasterizerState.SlopeScaledDepthBias = 1.0f;
	shadowMapPsoDesc.pRootSignature = mRootSignature.Get();
	shadowMapPsoDesc.VS =
	{
	reinterpret_cast<BYTE*>(mShaders["shadowMappingVS"]->GetBufferPointer()),
	mShaders["shadowMappingVS"]->GetBufferSize()
	};
	shadowMapPsoDesc.PS =
	{
	reinterpret_cast<BYTE*>(mShaders["shadowMappingPS"]->GetBufferPointer()),
	mShaders["shadowMappingPS"]->GetBufferSize()
	};
	shadowMapPsoDesc.RTVFormats[0] = DXGI_FORMAT_UNKNOWN;
	shadowMapPsoDesc.NumRenderTargets = 0;
	ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&shadowMapPsoDesc, IID_PPV_ARGS(&mPSOs["shadowMapping"])));

	//
	// PSO for debug layer.
	//
	auto shadowDebugPsoDesc = opaquePsoDesc;
	shadowDebugPsoDesc.pRootSignature = mRootSignature.Get();
	shadowDebugPsoDesc.VS = 
	{
	reinterpret_cast<BYTE*>(mShaders["shadowDebugVS"]->GetBufferPointer()),
	mShaders["shadowDebugVS"]->GetBufferSize()
	};
	shadowDebugPsoDesc.PS = 
	{
	reinterpret_cast<BYTE*>(mShaders["shadowDebugPS"]->GetBufferPointer()),
	mShaders["shadowDebugPS"]->GetBufferSize()
	};
	ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&shadowDebugPsoDesc, IID_PPV_ARGS(&mPSOs["shadowDebug"])));

}

void CRYCHIC::BuildFrameResources()
{
	for (int i = 0; i < gNumFrameResources; ++i)
	{
		mFrameResources.push_back(std::make_unique<FrameResource>(md3dDevice.Get(),
			1 + mCubemapRTSize + mShadowMapSize, mInstancesCount, mAllRitems.size(), (UINT)mMaterials.size()));
	}
}

void CRYCHIC::BuildMaterials()
{
	// mat0
	auto bricks0 = std::make_unique<Material>();
	bricks0->Name = "bricks0";
	bricks0->MatCBIndex = 0;
	bricks0->DiffuseSrvHeapIndex = 0;
	bricks0->NormalSrvHeapIndex = 1;
	bricks0->DiffuseAlbedo = XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);
	bricks0->FresnelR0 = XMFLOAT3(0.1f, 0.1f, 0.1f);
	bricks0->Roughness = 0.5f;
	bricks0->Metalness = 0.3f;

	// mat1
	auto stone0 = std::make_unique<Material>();
	stone0->Name = "stone0";
	stone0->MatCBIndex = 1;
	stone0->DiffuseSrvHeapIndex = 2;
	stone0->DiffuseAlbedo = XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);
	stone0->FresnelR0 = XMFLOAT3(0.05f, 0.05f, 0.05f);
	stone0->Roughness = 0.3f;

	// mat2
	auto tile0 = std::make_unique<Material>();
	tile0->Name = "tile0";
	tile0->MatCBIndex = 2;
	tile0->DiffuseSrvHeapIndex = 3;
	tile0->NormalSrvHeapIndex = 4;
	tile0->DiffuseAlbedo = XMFLOAT4(0.9f, 0.9f, 0.9f, 1.0f);
	tile0->FresnelR0 = XMFLOAT3(0.2f, 0.2f, 0.2f);
	tile0->Roughness = 0.5f;
	tile0->Metalness = 0.2f;

	// mat3
	auto crate0 = std::make_unique<Material>();
	crate0->Name = "crate0";
	crate0->MatCBIndex = 3;
	crate0->DiffuseSrvHeapIndex = 5;
	crate0->DiffuseAlbedo = XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);
	crate0->FresnelR0 = XMFLOAT3(0.05f, 0.05f, 0.05f);
	crate0->Roughness = 0.2f;

	// mat4
	auto crate1 = std::make_unique<Material>();
	crate1->Name = "crate1";
	crate1->MatCBIndex = 4;
	crate1->DiffuseSrvHeapIndex = 6;
	crate1->DiffuseAlbedo = XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);
	crate1->FresnelR0 = XMFLOAT3(0.05f, 0.05f, 0.05f);
	crate1->Roughness = 0.2f;

	// mat5
	auto flare = std::make_unique<Material>();
	flare->Name = "flare";
	flare->MatCBIndex = 5;
	flare->DiffuseSrvHeapIndex = 7;
	flare->NormalSrvHeapIndex = 8;
	flare->DiffuseAlbedo = XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);
	flare->FresnelR0 = XMFLOAT3(0.05f, 0.05f, 0.05f);
	flare->Roughness = 0.2f;
	flare->Metalness = 0.12f;

	// mat6
	auto ice = std::make_unique<Material>();
	ice->Name = "ice";
	ice->MatCBIndex = 6;
	ice->DiffuseSrvHeapIndex = 9;
	ice->DiffuseAlbedo = XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);
	ice->FresnelR0 = XMFLOAT3(0.05f, 0.05f, 0.05f);
	ice->Roughness = 0.2f;

	// mat7
	auto default = std::make_unique<Material>();
	default->Name = "default";
	default->MatCBIndex = 7;
	default->DiffuseSrvHeapIndex = 10;
	default->NormalSrvHeapIndex = 11;
	default->DiffuseAlbedo = XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);
	default->FresnelR0 = XMFLOAT3(0.05f, 0.05f, 0.05f);
	default->Roughness = 0.1f;

	// mat8
	auto mirror = std::make_unique<Material>();
	mirror->Name = "mirror";
	mirror->MatCBIndex = 8;
	mirror->DiffuseSrvHeapIndex = 10;
	mirror->NormalSrvHeapIndex = 11;
	mirror->DiffuseAlbedo = XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);
	mirror->FresnelR0 = XMFLOAT3(0.95f, 0.95f, 0.95f);
	mirror->Roughness = 0.1f;
	mirror->Metalness = 0.95f;

	mMaterials["bricks0"] = std::move(bricks0);
	mMaterials["stone0"] = std::move(stone0);
	mMaterials["tile0"] = std::move(tile0);
	mMaterials["crate0"] = std::move(crate0);
	mMaterials["crate1"] = std::move(crate1);
	mMaterials["flare"] = std::move(flare);
	mMaterials["ice"] = std::move(ice);
	mMaterials["default"] = std::move(default);
	mMaterials["mirror"] = std::move(mirror);

}

void CRYCHIC::BuildRenderItems()
{
	auto boxRitem = std::make_unique<RenderItem>();
	// 每一个渲染项都有一个单独的InstanceData缓冲区
	// 给个下标去找对应的指向缓冲区的指针
	boxRitem->itemIndex = mItemIndex++;
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
	//mInstanceCount += boxInstanceCount;
	mInstancesCount.push_back(boxInstanceCount);
	boxRitem->Instances.resize(boxInstanceCount);
	boxRitem->InstanceCount = boxInstanceCount;
	for (size_t i = 0; i < boxInstanceCount; i++)
	{
		XMMATRIX world = XMMatrixScaling(2.0f, 2.0f, 2.0f) * XMMatrixTranslation(2.0f, 1.0f, 0.0f);
		XMStoreFloat4x4(&boxRitem->Instances[i].World, world);
		XMStoreFloat4x4(&boxRitem->Instances[i].TexTransform, XMMatrixScaling(1.0f, 1.0f, 1.0f));
		boxRitem->Instances[i].MaterialIndex = 5; // flareMat
	}
	mRitemLayer[(int)RenderLayer::Opaque].push_back(boxRitem.get());
	mAllRitems.push_back(std::move(boxRitem));

	auto gridRitem = std::make_unique<RenderItem>();
	gridRitem->itemIndex = mItemIndex++;
	gridRitem->World = MathHelper::Identity4x4();
	XMStoreFloat4x4(&gridRitem->TexTransform, XMMatrixScaling(8.0f, 8.0f, 1.0f));
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
	//mInstanceCount += gridInstanceCount;
	mInstancesCount.push_back(gridInstanceCount);
	gridRitem->Instances.resize(gridInstanceCount);
	for (size_t i = 0; i < gridInstanceCount; i++)
	{
		//gridRitem->Instances[i].World = MathHelper::Identity4x4();
		XMStoreFloat4x4(&gridRitem->Instances[i].World, XMMatrixScaling(2.0f, 2.0f, 2.0f) * 
		XMMatrixTranslation(0.0f, 0.0f, 0.0f));
		XMStoreFloat4x4(&gridRitem->Instances[i].TexTransform, XMMatrixScaling(8.0f, 8.0f, 1.0f));
		gridRitem->Instances[i].MaterialIndex = 2; // tileMat
	}
	mRitemLayer[(int)RenderLayer::Opaque].push_back(gridRitem.get());
	mAllRitems.push_back(std::move(gridRitem));

	auto skullRitem = std::make_unique<RenderItem>();
	skullRitem->itemIndex = mItemIndex++;
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
	//mInstanceCount += skullInstanceCount;
	mInstancesCount.push_back(skullInstanceCount);
	skullRitem->Instances.resize(skullInstanceCount);
	for (size_t i = 0; i < skullInstanceCount; i++)
	{
		XMMATRIX world = XMMatrixMultiply(XMMatrixScaling(0.5f, 0.5f, 0.5f), XMMatrixTranslation(2.0f, 2.0f, 0.0f));
		XMStoreFloat4x4(&skullRitem->Instances[i].World, world);
		XMStoreFloat4x4(&skullRitem->Instances[i].TexTransform, XMMatrixScaling(1.0f, 1.0f, 1.0f));
		skullRitem->Instances[i].MaterialIndex = 7; // defaultMat
	}
	mRitemLayer[(int)RenderLayer::Opaque].push_back(skullRitem.get());
	mAllRitems.push_back(std::move(skullRitem));

	auto carRitem = std::make_unique<RenderItem>();
	carRitem->itemIndex = mItemIndex++;
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
	mInstancesCount.push_back(carInstanceCount);
	carRitem->Instances.resize(carInstanceCount);
	for (size_t i = 0; i < carInstanceCount; i++)
	{
		XMMATRIX world = XMMatrixMultiply(XMMatrixScaling(0.5f, 0.5f, 0.5f), XMMatrixTranslation(-3.0f, 1.5f, 0.0f));
		XMStoreFloat4x4(&carRitem->Instances[i].World, world);
		XMStoreFloat4x4(&carRitem->Instances[i].TexTransform, XMMatrixScaling(1.0f, 1.0f, 1.0f));
		carRitem->Instances[i].MaterialIndex = 6; // iceMat
	}
	mRitemLayer[(int)RenderLayer::Opaque].push_back(carRitem.get());
	mAllRitems.push_back(std::move(carRitem));

	auto leftCylRitem = std::make_unique<RenderItem>();
	leftCylRitem->itemIndex = mItemIndex++;
	//leftCylRitem->Mat = mMaterials["bricks0"].get();
	leftCylRitem->Geo = mGeometries["shapeGeo"].get();
	leftCylRitem->PrimitiveType = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
	leftCylRitem->IndexCount = leftCylRitem->Geo->DrawArgs["cylinder"].IndexCount;
	leftCylRitem->StartIndexLocation = leftCylRitem->Geo->DrawArgs["cylinder"].StartIndexLocation;
	leftCylRitem->BaseVertexLocation = leftCylRitem->Geo->DrawArgs["cylinder"].BaseVertexLocation;
	leftCylRitem->Bounds = leftCylRitem->Geo->DrawArgs["cylinder"].Bounds;
	UINT leftCylInstanceCount = 5;
	leftCylRitem->InstanceCount = leftCylInstanceCount;
	mInstancesCount.push_back(leftCylInstanceCount);
	leftCylRitem->Instances.resize(leftCylInstanceCount);

	auto rightCylRitem = std::make_unique<RenderItem>();
	rightCylRitem->itemIndex = mItemIndex++;
	rightCylRitem->Mat = mMaterials["bricks0"].get();
	rightCylRitem->Geo = mGeometries["shapeGeo"].get();
	rightCylRitem->PrimitiveType = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
	rightCylRitem->IndexCount = rightCylRitem->Geo->DrawArgs["cylinder"].IndexCount;
	rightCylRitem->StartIndexLocation = rightCylRitem->Geo->DrawArgs["cylinder"].StartIndexLocation;
	rightCylRitem->BaseVertexLocation = rightCylRitem->Geo->DrawArgs["cylinder"].BaseVertexLocation;
	rightCylRitem->Bounds = rightCylRitem->Geo->DrawArgs["cylinder"].Bounds;
	UINT rightCylInstanceCount = 5;
	rightCylRitem->InstanceCount = rightCylInstanceCount;
	mInstancesCount.push_back(rightCylInstanceCount);
	rightCylRitem->Instances.resize(rightCylInstanceCount);

	auto leftSphereRitem = std::make_unique<RenderItem>();
	leftSphereRitem->itemIndex = mItemIndex++;
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
	mInstancesCount.push_back(leftSphereInstanceCount);
	leftSphereRitem->Instances.resize(leftCylInstanceCount);

	auto rightSphereRitem = std::make_unique<RenderItem>();
	rightSphereRitem->TexTransform = MathHelper::Identity4x4();
	rightSphereRitem->itemIndex = mItemIndex++;
	rightSphereRitem->Mat = mMaterials["stone0"].get();
	rightSphereRitem->Geo = mGeometries["shapeGeo"].get();
	rightSphereRitem->PrimitiveType = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
	rightSphereRitem->IndexCount = rightSphereRitem->Geo->DrawArgs["sphere"].IndexCount;
	rightSphereRitem->StartIndexLocation = rightSphereRitem->Geo->DrawArgs["sphere"].StartIndexLocation;
	rightSphereRitem->BaseVertexLocation = rightSphereRitem->Geo->DrawArgs["sphere"].BaseVertexLocation;
	rightSphereRitem->Bounds = rightSphereRitem->Geo->DrawArgs["sphere"].Bounds;
	UINT rightSphereInstanceCount = 5;
	rightSphereRitem->InstanceCount = rightSphereInstanceCount;
	mInstancesCount.push_back(rightSphereInstanceCount);
	rightSphereRitem->Instances.resize(rightSphereInstanceCount);

	XMMATRIX brickTexTransform = XMMatrixScaling(1.5f, 2.0f, 1.0f);
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
		leftSphereRitem->Instances[i].MaterialIndex = 8; // mirror
		XMStoreFloat4x4(&rightSphereRitem->Instances[i].World, rightSphereWorld);
		XMStoreFloat4x4(&rightSphereRitem->Instances[i].TexTransform, brickTexTransform);
		rightSphereRitem->Instances[i].MaterialIndex = 8; // mirror
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
	skyRitem->itemIndex = mItemIndex++;
	skyRitem->Geo = mGeometries["shapeGeo"].get();
	skyRitem->PrimitiveType = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
	skyRitem->IndexCount = skyRitem->Geo->DrawArgs["sphere"].IndexCount;
	skyRitem->StartIndexLocation = skyRitem->Geo->DrawArgs["sphere"].StartIndexLocation;
	skyRitem->BaseVertexLocation = skyRitem->Geo->DrawArgs["sphere"].BaseVertexLocation;
	skyRitem->Bounds = skyRitem->Geo->DrawArgs["sphere"].Bounds;
	UINT skyInstanceCount = 1;
	skyRitem->InstanceCount = skyInstanceCount;
	mInstancesCount.push_back(skyInstanceCount);
	skyRitem->Instances.resize(skyInstanceCount);
	XMStoreFloat4x4(&skyRitem->Instances[0].World, XMMatrixScaling(5000.0f, 5000.0f, 5000.0f));
	skyRitem->TexTransform = MathHelper::Identity4x4();

	mRitemLayer[(int)RenderLayer::Sky].push_back(skyRitem.get());
	mAllRitems.push_back(std::move(skyRitem));

	auto globeRitem = std::make_unique<RenderItem>();
	globeRitem->itemIndex = mItemIndex++;
	globeRitem->Geo = mGeometries["shapeGeo"].get();
	globeRitem->PrimitiveType = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
	globeRitem->IndexCount = globeRitem->Geo->DrawArgs["sphere"].IndexCount;
	globeRitem->StartIndexLocation = globeRitem->Geo->DrawArgs["sphere"].StartIndexLocation;
	globeRitem->BaseVertexLocation = globeRitem->Geo->DrawArgs["sphere"].BaseVertexLocation;
	globeRitem->Bounds = globeRitem->Geo->DrawArgs["sphere"].Bounds;
	UINT globeInstaceCount = 1;
	globeRitem->InstanceCount = globeInstaceCount;
	mInstancesCount.push_back(globeInstaceCount);
	globeRitem->Instances.resize(globeInstaceCount);
	XMStoreFloat4x4(&globeRitem->Instances[0].World, XMMatrixScaling(2.0f, 2.0f, 2.0f) * XMMatrixTranslation(0.0f, 2.0f, -3.0f));
	XMStoreFloat4x4(&globeRitem->Instances[0].TexTransform, XMMatrixScaling(1.0f, 1.0f, 1.0f));
	globeRitem->Instances[0].MaterialIndex = 8; // mirror

	mRitemLayer[(int)RenderLayer::OpaqueDynamicReflectors].push_back(globeRitem.get());
	mAllRitems.push_back(std::move(globeRitem));

	// shadow debug quad
	auto quadRitem = std::make_unique<RenderItem>();
	quadRitem->itemIndex = mItemIndex++;
	quadRitem->Geo = mGeometries["shapeGeo"].get();
	quadRitem->PrimitiveType = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
	quadRitem->IndexCount = quadRitem->Geo->DrawArgs["quad"].IndexCount;
	quadRitem->StartIndexLocation = quadRitem->Geo->DrawArgs["quad"].StartIndexLocation;
	quadRitem->BaseVertexLocation = quadRitem->Geo->DrawArgs["quad"].BaseVertexLocation;
	quadRitem->Bounds = quadRitem->Geo->DrawArgs["quad"].Bounds;

	UINT quadInstaceCount = 1;
	quadRitem->InstanceCount = quadInstaceCount;
	mInstancesCount.push_back(quadInstaceCount);
	quadRitem->Instances.resize(quadInstaceCount);
	XMStoreFloat4x4(&quadRitem->Instances[0].World, XMMatrixScaling(1.0f, 1.0f, 1.0f));
	XMStoreFloat4x4(&quadRitem->Instances[0].TexTransform, XMMatrixScaling(1.0f, 1.0f, 1.0f));
	quadRitem->Instances[0].MaterialIndex = 8; // shadowdebug
	mRitemLayer[(int)RenderLayer::Debug].push_back(quadRitem.get());
	mAllRitems.push_back(std::move(quadRitem));
}

void CRYCHIC::BuildCubeMapRenderItems()
{
	mSceneItemCount = mItemIndex;
	auto boxRitem = std::make_unique<RenderItem>();
	// 每一个渲染项都有一个单独的InstanceData缓冲区
	// 给个下标去找对应的指向缓冲区的指针
	boxRitem->itemIndex = mItemIndex++;
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
	mInstancesCount.push_back(boxInstanceCount);
	boxRitem->Instances.resize(boxInstanceCount);
	boxRitem->InstanceCount = boxInstanceCount;
	for (size_t i = 0; i < boxInstanceCount; i++)
	{
		XMMATRIX world = XMMatrixScaling(2.0f, 2.0f, 2.0f) * XMMatrixTranslation(2.0f, 1.0f, 0.0f);
		XMStoreFloat4x4(&boxRitem->Instances[i].World, world);
		XMStoreFloat4x4(&boxRitem->Instances[i].TexTransform, XMMatrixScaling(1.0f, 1.0f, 1.0f));
		boxRitem->Instances[i].MaterialIndex = 5; // flareMat
	}
	mRitemLayer[(int)RenderLayer::OpaqueDynamicCamera].push_back(boxRitem.get());
	mAllRitems.push_back(std::move(boxRitem));

	auto gridRitem = std::make_unique<RenderItem>();
	gridRitem->itemIndex = mItemIndex++;
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
	mInstancesCount.push_back(gridInstanceCount);
	gridRitem->Instances.resize(gridInstanceCount);
	for (size_t i = 0; i < gridInstanceCount; i++)
	{
		gridRitem->Instances[i].World = MathHelper::Identity4x4();
		//XMMATRIX world = XMMatrixScaling(2.0f, 2.0f, 2.0f);
		//XMStoreFloat4x4(&gridRitem->Instances[i].World, world);
		XMStoreFloat4x4(&gridRitem->Instances[i].TexTransform, XMMatrixScaling(8.0f, 8.0f, 1.0f));
		gridRitem->Instances[i].MaterialIndex = 2; // tileMat
	}
	mRitemLayer[(int)RenderLayer::OpaqueDynamicCamera].push_back(gridRitem.get());
	mAllRitems.push_back(std::move(gridRitem));

	auto skullRitem = std::make_unique<RenderItem>();
	skullRitem->itemIndex = mItemIndex++;
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
	mInstancesCount.push_back(skullInstanceCount);
	skullRitem->Instances.resize(skullInstanceCount);
	for (size_t i = 0; i < skullInstanceCount; i++)
	{
		XMMATRIX world = XMMatrixMultiply(XMMatrixScaling(0.5f, 0.5f, 0.5f), XMMatrixTranslation(2.0f, 2.0f, 0.0f));
		XMStoreFloat4x4(&skullRitem->Instances[i].World, world);
		XMStoreFloat4x4(&skullRitem->Instances[i].TexTransform, XMMatrixScaling(1.0f, 1.0f, 1.0f));
		skullRitem->Instances[i].MaterialIndex = 7; // defaultMat
	}
	mRitemLayer[(int)RenderLayer::OpaqueDynamicCamera].push_back(skullRitem.get());
	mAllRitems.push_back(std::move(skullRitem));

	auto carRitem = std::make_unique<RenderItem>();
	carRitem->itemIndex = mItemIndex++;
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
	mInstancesCount.push_back(carInstanceCount);
	carRitem->Instances.resize(carInstanceCount);
	for (size_t i = 0; i < carInstanceCount; i++)
	{
		XMMATRIX world = XMMatrixMultiply(XMMatrixScaling(0.5f, 0.5f, 0.5f), XMMatrixTranslation(-3.0f, 1.5f, 0.0f));
		XMStoreFloat4x4(&carRitem->Instances[i].World, world);
		XMStoreFloat4x4(&carRitem->Instances[i].TexTransform, XMMatrixScaling(1.0f, 1.0f, 1.0f));
		carRitem->Instances[i].MaterialIndex = 6; // iceMat
	}
	mRitemLayer[(int)RenderLayer::OpaqueDynamicCamera].push_back(carRitem.get());
	mAllRitems.push_back(std::move(carRitem));

	auto leftCylRitem = std::make_unique<RenderItem>();
	leftCylRitem->itemIndex = mItemIndex++;
	//leftCylRitem->Mat = mMaterials["bricks0"].get();
	leftCylRitem->Geo = mGeometries["shapeGeo"].get();
	leftCylRitem->PrimitiveType = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
	leftCylRitem->IndexCount = leftCylRitem->Geo->DrawArgs["cylinder"].IndexCount;
	leftCylRitem->StartIndexLocation = leftCylRitem->Geo->DrawArgs["cylinder"].StartIndexLocation;
	leftCylRitem->BaseVertexLocation = leftCylRitem->Geo->DrawArgs["cylinder"].BaseVertexLocation;
	leftCylRitem->Bounds = leftCylRitem->Geo->DrawArgs["cylinder"].Bounds;
	UINT leftCylInstanceCount = 5;
	leftCylRitem->InstanceCount = leftCylInstanceCount;
	mInstancesCount.push_back(leftCylInstanceCount);
	leftCylRitem->Instances.resize(leftCylInstanceCount);

	auto rightCylRitem = std::make_unique<RenderItem>();
	rightCylRitem->itemIndex = mItemIndex++;
	rightCylRitem->Mat = mMaterials["bricks0"].get();
	rightCylRitem->Geo = mGeometries["shapeGeo"].get();
	rightCylRitem->PrimitiveType = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
	rightCylRitem->IndexCount = rightCylRitem->Geo->DrawArgs["cylinder"].IndexCount;
	rightCylRitem->StartIndexLocation = rightCylRitem->Geo->DrawArgs["cylinder"].StartIndexLocation;
	rightCylRitem->BaseVertexLocation = rightCylRitem->Geo->DrawArgs["cylinder"].BaseVertexLocation;
	rightCylRitem->Bounds = rightCylRitem->Geo->DrawArgs["cylinder"].Bounds;
	UINT rightCylInstanceCount = 5;
	rightCylRitem->InstanceCount = rightCylInstanceCount;
	mInstancesCount.push_back(rightCylInstanceCount);
	rightCylRitem->Instances.resize(rightCylInstanceCount);

	auto leftSphereRitem = std::make_unique<RenderItem>();
	leftSphereRitem->itemIndex = mItemIndex++;
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
	mInstancesCount.push_back(leftSphereInstanceCount);
	leftSphereRitem->Instances.resize(leftCylInstanceCount);

	auto rightSphereRitem = std::make_unique<RenderItem>();
	rightSphereRitem->TexTransform = MathHelper::Identity4x4();
	rightSphereRitem->itemIndex = mItemIndex++;
	rightSphereRitem->Mat = mMaterials["stone0"].get();
	rightSphereRitem->Geo = mGeometries["shapeGeo"].get();
	rightSphereRitem->PrimitiveType = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
	rightSphereRitem->IndexCount = rightSphereRitem->Geo->DrawArgs["sphere"].IndexCount;
	rightSphereRitem->StartIndexLocation = rightSphereRitem->Geo->DrawArgs["sphere"].StartIndexLocation;
	rightSphereRitem->BaseVertexLocation = rightSphereRitem->Geo->DrawArgs["sphere"].BaseVertexLocation;
	rightSphereRitem->Bounds = rightSphereRitem->Geo->DrawArgs["sphere"].Bounds;
	UINT rightSphereInstanceCount = 5;
	rightSphereRitem->InstanceCount = rightSphereInstanceCount;
	mInstancesCount.push_back(rightSphereInstanceCount);
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
		leftSphereRitem->Instances[i].MaterialIndex = 8; // mirror
		XMStoreFloat4x4(&rightSphereRitem->Instances[i].World, rightSphereWorld);
		XMStoreFloat4x4(&rightSphereRitem->Instances[i].TexTransform, brickTexTransform);
		rightSphereRitem->Instances[i].MaterialIndex = 8; // mirror
	}
	mRitemLayer[(int)RenderLayer::OpaqueDynamicCamera].push_back(leftCylRitem.get());
	mRitemLayer[(int)RenderLayer::OpaqueDynamicCamera].push_back(rightCylRitem.get());
	mRitemLayer[(int)RenderLayer::OpaqueDynamicCamera].push_back(leftSphereRitem.get());
	mRitemLayer[(int)RenderLayer::OpaqueDynamicCamera].push_back(rightSphereRitem.get());

	mAllRitems.push_back(std::move(leftCylRitem));
	mAllRitems.push_back(std::move(rightCylRitem));
	mAllRitems.push_back(std::move(leftSphereRitem));
	mAllRitems.push_back(std::move(rightSphereRitem));
	auto skyRitem = std::make_unique<RenderItem>();
	skyRitem->itemIndex = mItemIndex++;
	skyRitem->Geo = mGeometries["shapeGeo"].get();
	skyRitem->PrimitiveType = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
	skyRitem->IndexCount = skyRitem->Geo->DrawArgs["sphere"].IndexCount;
	skyRitem->StartIndexLocation = skyRitem->Geo->DrawArgs["sphere"].StartIndexLocation;
	skyRitem->BaseVertexLocation = skyRitem->Geo->DrawArgs["sphere"].BaseVertexLocation;
	skyRitem->Bounds = skyRitem->Geo->DrawArgs["sphere"].Bounds;
	UINT skyInstanceCount = 1;
	skyRitem->InstanceCount = skyInstanceCount;
	mInstancesCount.push_back(skyInstanceCount);
	skyRitem->Instances.resize(skyInstanceCount);
	XMStoreFloat4x4(&skyRitem->Instances[0].World, XMMatrixScaling(5000.0f, 5000.0f, 5000.0f));
	skyRitem->TexTransform = MathHelper::Identity4x4();

	mRitemLayer[(int)RenderLayer::SkyDynamicCamera].push_back(skyRitem.get());
	mAllRitems.push_back(std::move(skyRitem));
	
}


//void CRYCHIC::BuildInstancingSceneRenderItems()
//{
//	auto boxRitem = std::make_unique<RenderItem>();
//	XMStoreFloat4x4(&boxRitem->World, XMMatrixScaling(1.0f, 1.0f, 1.0f));
//	XMStoreFloat4x4(&boxRitem->TexTransform, XMMatrixScaling(1.0f, 1.0f, 1.0f));
//	//boxRitem->ObjCBIndex = 0;
//	boxRitem->Mat = mMaterials["crate0"].get();
//	boxRitem->Geo = mGeometries["shapeGeo"].get();
//	boxRitem->PrimitiveType = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
//	boxRitem->InstanceCount = 0;
//	boxRitem->IndexCount = boxRitem->Geo->DrawArgs["box"].IndexCount;
//	boxRitem->StartIndexLocation = boxRitem->Geo->DrawArgs["box"].StartIndexLocation;
//	boxRitem->BaseVertexLocation = boxRitem->Geo->DrawArgs["box"].BaseVertexLocation;
//	boxRitem->Bounds = boxRitem->Geo->DrawArgs["box"].Bounds;
//
//	const int n = 5;
//	mInstancesCount[] = n * n * n;
//	boxRitem->Instances.resize(mInstanceCount);
//	float width = 200.0f;
//	float height = 200.0f;
//	float depth = 200.0f;
//
//	float x = -0.5f * width;
//	float y = -0.5f * height;
//	float z = -0.5f * depth;
//	float dx = width / (n - 1);
//	float dy = height / (n - 1);
//	float dz = depth / (n - 1);
//	for (size_t i = 0; i < n; i++)
//	{
//		for (size_t j = 0; j < n; j++) 
//		{
//			for (size_t k = 0; k < n; k++) 
//			{
//				int index = i * n * n + j * n + k;
//				XMStoreFloat4x4(&boxRitem->Instances[index].World, XMMatrixTranslation(x + k * dx,
//					y + j * dy, z + i * dz));
//				XMStoreFloat4x4(&boxRitem->Instances[index].TexTransform, XMMatrixScaling(2.0f, 1.0f, 2.0f));
//				boxRitem->Instances[index].MaterialIndex = index % mMaterials.size();
//			}
//		}
//	}
//	mRitemLayer[(int)RenderLayer::Opaque].push_back(boxRitem.get());
//	mAllRitems.push_back(std::move(boxRitem));
//}

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

	//UINT passCBByteSize = d3dUtil::CalcConstantBufferByteSize(sizeof(PassConstants));

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

void CRYCHIC::DrawSceneToCubeMap()
{
	mCommandList->SetPipelineState(mPSOs["opaque"].Get());
	mCommandList->RSSetViewports(1, &mDynamicCubeMap->Viewport());
	mCommandList->RSSetScissorRects(1, &mDynamicCubeMap->ScissorRect());

	mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mDynamicCubeMap->Resource(),
		D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_RENDER_TARGET));
	
	UINT passCBByteSize = d3dUtil::CalcConstantBufferByteSize(sizeof(PassConstants));
	// loop all cubemap faces
	for (int i = 0; i < mCubemapRTSize; i++)
	{
		// clear the back buffer and depth buffer
		mCommandList->ClearRenderTargetView(mDynamicCubeMap->Rtv(i), Colors::Black, 0, nullptr);
		mCommandList->ClearDepthStencilView(mCubeDSV, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);
		
		// specify the render target
		mCommandList->OMSetRenderTargets(1, &mDynamicCubeMap->Rtv(i), false, &mCubeDSV);
		// bind the pass constant buffer
		auto passCB = mCurrFrameResource->PassCB->Resource();
		D3D12_GPU_VIRTUAL_ADDRESS passCBAddress = passCB->GetGPUVirtualAddress() + (1 + i) * passCBByteSize;
		mCommandList->SetGraphicsRootConstantBufferView(1, passCBAddress);
		DrawRenderItems(mCommandList.Get(), mRitemLayer[int(RenderLayer::OpaqueDynamicCamera)]);
		mCommandList->SetPipelineState(mPSOs["sky"].Get());
		DrawRenderItems(mCommandList.Get(), mRitemLayer[int(RenderLayer::SkyDynamicCamera)]);
		mCommandList->SetPipelineState(mPSOs["opaque"].Get());
	}
	mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mDynamicCubeMap->Resource(),
		D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_GENERIC_READ));
}

void CRYCHIC::DrawSceneToShadowMap()
{
	mCommandList->SetPipelineState(mPSOs["shadowMapping"].Get());
	for (size_t i = 0; i < mShadowMapSize; i++)
	{
		mCommandList->RSSetViewports(1, &mShadowMap->Viewport());
		mCommandList->RSSetScissorRects(1, &mShadowMap->ScissorRect());

		mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mShadowMap->Resource(i),
			D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_DEPTH_WRITE));

		UINT passCBByteSize = d3dUtil::CalcConstantBufferByteSize(sizeof(PassConstants));

		mCommandList->ClearDepthStencilView(mShadowMap->Dsv(i),
			D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);

		mCommandList->OMSetRenderTargets(0, nullptr, false, &mShadowMap->Dsv(i));

		auto passCB = mCurrFrameResource->PassCB->Resource();
		D3D12_GPU_VIRTUAL_ADDRESS passCBAddress = passCB->GetGPUVirtualAddress() + (1 + mDynamicCubemapSize * 6 + i) * passCBByteSize;
		mCommandList->SetGraphicsRootConstantBufferView(1, passCBAddress);
		// shadowmapping
		DrawRenderItems(mCommandList.Get(), mRitemLayer[(int)RenderLayer::OpaqueDynamicCamera]);
		mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mShadowMap->Resource(i),
			D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_GENERIC_READ));
	}
}

void CRYCHIC::BuildCubeDepthStencil()
{
	// Create the depth/stencil buffer and view.
	D3D12_RESOURCE_DESC depthStencilDesc;
	depthStencilDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	depthStencilDesc.Alignment = 0;
	depthStencilDesc.Width = CubeMapSize;
	depthStencilDesc.Height = CubeMapSize;
	depthStencilDesc.DepthOrArraySize = 1;
	depthStencilDesc.MipLevels = 1;
	depthStencilDesc.Format = mDepthStencilFormat;
	depthStencilDesc.SampleDesc.Count = 1;
	depthStencilDesc.SampleDesc.Quality = 0;
	depthStencilDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	depthStencilDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

	D3D12_CLEAR_VALUE optClear;
	optClear.Format = mDepthStencilFormat;
	optClear.DepthStencil.Depth = 1.0f;
	optClear.DepthStencil.Stencil = 0;
	ThrowIfFailed(md3dDevice->CreateCommittedResource(
		&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
		D3D12_HEAP_FLAG_NONE,
		&depthStencilDesc,
		D3D12_RESOURCE_STATE_COMMON,
		&optClear,
		IID_PPV_ARGS(mCubeDepthStencilBuffer.GetAddressOf())));

	// Create descriptor to mip level 0 of entire resource using the format of the resource.
	md3dDevice->CreateDepthStencilView(mCubeDepthStencilBuffer.Get(), nullptr, mCubeDSV);

	// Transition the resource from its initial state to be used as a depth buffer.
	mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mCubeDepthStencilBuffer.Get(),
		D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_DEPTH_WRITE));
}

std::array<const CD3DX12_STATIC_SAMPLER_DESC, 7> CRYCHIC::GetStaticSamplers()
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

	const CD3DX12_STATIC_SAMPLER_DESC shadow(
		6, // shaderRegister
		D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT, // filter
		D3D12_TEXTURE_ADDRESS_MODE_BORDER,  // addressU
		D3D12_TEXTURE_ADDRESS_MODE_BORDER,  // addressV
		D3D12_TEXTURE_ADDRESS_MODE_BORDER,  // addressW
		0.0f,                               // mipLODBias
		16,                                 // maxAnisotropy
		D3D12_COMPARISON_FUNC_LESS_EQUAL,
		D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK);

	return {
		pointWrap, pointClamp,
		linearWrap, linearClamp,
		anisotropicWrap, anisotropicClamp,
		shadow };
}

