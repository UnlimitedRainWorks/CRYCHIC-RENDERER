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

CRYCHIC::CRYCHIC(HINSTANCE hInstance) : D3DApp(hInstance)
{
	mSceneBounds.Center = XMFLOAT3(0.0f, 0.0f, 0.0f);
	mSceneBounds.Radius = sqrtf(30.0f * 30.0f + 45.0f * 45.0f);
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

    mCamera.SetPosition(0.0f, 2.0f, -15.0f);

    mShadowMap = std::make_unique<ShadowMap>(md3dDevice.Get(),
        4096, 4096);

    mSsao = std::make_unique<Ssao>(
        md3dDevice.Get(),
        mCommandList.Get(),
        mClientWidth, mClientHeight);

    mDeferred = std::make_unique<DeferredShading>(
        md3dDevice.Get(),
        mClientWidth, mClientHeight, DXGI_FORMAT_R32G32B32A32_FLOAT);

    LoadTextures();
    BuildRootSignature();
    BuildSsaoRootSignature();
    BuildDescriptorHeaps();
    BuildShadersAndInputLayout();
    BuildShapeGeometry();
    BuildSkullGeometry();
    BuildMaterials();
    /*BuildRenderItems();
    BuildRenderItemsWithShadow();*/
    BuildCascadeShadowRenderItems();
    BuildCascadeShadowRenderItemsWithShadow();
    BuildFrameResources();
    BuildPSOs();

    mSsao->SetPSOs(mPSOs["ssao"].Get(), mPSOs["ssaoBlur"].Get());

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
    // Add +1 for screen normal map, +2 for ambient maps. ssao
    // Add +4 for GBuffer. deferred shading
    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc;
    rtvHeapDesc.NumDescriptors = SwapChainBufferCount + 3 + 4;
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    rtvHeapDesc.NodeMask = 0;
    ThrowIfFailed(md3dDevice->CreateDescriptorHeap(
        &rtvHeapDesc, IID_PPV_ARGS(mRtvHeap.GetAddressOf())));

    // Add +1 DSV for shadow map.
    D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc;
    dsvHeapDesc.NumDescriptors = 1 + 12;
    dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    dsvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    dsvHeapDesc.NodeMask = 0;
    ThrowIfFailed(md3dDevice->CreateDescriptorHeap(
        &dsvHeapDesc, IID_PPV_ARGS(mDsvHeap.GetAddressOf())));
}

void CRYCHIC::OnResize()
{
    D3DApp::OnResize();

    mCamera.SetLens(0.25f * MathHelper::Pi, AspectRatio(), 1.0f, 100.0f);
    BoundingFrustum::CreateFromMatrix(mCamFrustum, mCamera.GetProj());
    if (mSsao != nullptr)
    {
        mSsao->OnResize(mClientWidth, mClientHeight);

        // Resources changed, so need to rebuild descriptors.
        mSsao->RebuildDescriptors(mDepthStencilBuffer.Get());
    }
    if (mDeferred != nullptr)
    {
        mDeferred->OnResize(mClientWidth, mClientHeight);
        mDeferred->BuildDescriptors();
    }
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

    //
    // Animate the lights (and hence shadows).
    //

    mLightRotationAngle += 0.0f * gt.DeltaTime();

    XMMATRIX R = XMMatrixRotationY(mLightRotationAngle);
    for (int i = 0; i < 3; ++i)
    {
        XMVECTOR lightDir = XMLoadFloat3(&mBaseLightDirections[i]);
        lightDir = XMVector3TransformNormal(lightDir, R);
        XMStoreFloat3(&mRotatedLightDirections[i], lightDir);
    }

    AnimateMaterials(gt);
    //UpdateObjectCBs(gt);
    UpdateInstanceData(gt);
    UpdateMaterialBuffer(gt);
    UpdateCascadeShadowTransform(gt);
    UpdateMainPassCB(gt);
    UpdateShadowPassCB(gt);
    UpdateSsaoCB(gt);
}

void CRYCHIC::Draw(const GameTimer& gt)
{
    if (isDeferred)
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

        //
        // Shadow map pass.
        //

        // Bind all the materials used in this scene.  For structured buffers, we can bypass the heap and 
        // set as a root descriptor.
        auto matBuffer = mCurrFrameResource->MaterialBuffer->Resource();
        mCommandList->SetGraphicsRootShaderResourceView(1, matBuffer->GetGPUVirtualAddress());

        // Bind null SRV for shadow map pass.
        mCommandList->SetGraphicsRootDescriptorTable(3, mNullSrv);

        // Bind all the textures used in this scene.  Observe
        // that we only have to specify the first descriptor in the table.  
        // The root signature knows how many descriptors are expected in the table.
        mCommandList->SetGraphicsRootDescriptorTable(4, mSrvDescriptorHeap->GetGPUDescriptorHandleForHeapStart());

        DrawSceneToShadowMap();

        //
        // Normal/depth pass.
        //

        DrawNormalsAndDepth();

        //
        // Compute SSAO.
        // 

        mCommandList->SetGraphicsRootSignature(mSsaoRootSignature.Get());
        mSsao->ComputeSsao(mCommandList.Get(), mCurrFrameResource, 3);

        //
        // Main rendering pass.
        //

        mCommandList->SetGraphicsRootSignature(mRootSignature.Get());

        // Rebind state whenever graphics root signature changes.

        // Bind all the materials used in this scene.  For structured buffers, we can bypass the heap and 
        // set as a root descriptor.
        matBuffer = mCurrFrameResource->MaterialBuffer->Resource();
        mCommandList->SetGraphicsRootShaderResourceView(1, matBuffer->GetGPUVirtualAddress());
        mCommandList->SetGraphicsRootDescriptorTable(4, mSrvDescriptorHeap->GetGPUDescriptorHandleForHeapStart());
        DrawGBuffer();

        mCommandList->SetPipelineState(mPSOs["deferredShading"].Get());
        mCommandList->RSSetViewports(1, &mScreenViewport);
        mCommandList->RSSetScissorRects(1, &mScissorRect);
        
        // Indicate a state transition on the resource usage.
        mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(CurrentBackBuffer(),
            D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET));

        // Clear the back buffer.
        mCommandList->ClearRenderTargetView(CurrentBackBufferView(), Colors::LightSteelBlue, 0, nullptr);
        mCommandList->ClearDepthStencilView(DepthStencilView(), D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);
        // WE ALREADY WROTE THE DEPTH INFO TO THE DEPTH BUFFER IN DrawNormalsAndDepth,
        // SO DO NOT CLEAR DEPTH.

        // Specify the buffers we are going to render to.
        mCommandList->OMSetRenderTargets(1, &CurrentBackBufferView(), true, &DepthStencilView());

        // Bind all the textures used in this scene.  Observe
        // that we only have to specify the first descriptor in the table.  
        // The root signature knows how many descriptors are expected in the table.
        mCommandList->SetGraphicsRootDescriptorTable(4, mSrvDescriptorHeap->GetGPUDescriptorHandleForHeapStart());

        auto passCB = mCurrFrameResource->PassCB->Resource();
        mCommandList->SetGraphicsRootConstantBufferView(2, passCB->GetGPUVirtualAddress());

        // Bind the sky cube map.  For our demos, we just use one "world" cube map representing the environment
        // from far away, so all objects will use the same cube map and we only need to set it once per-frame.  
        // If we wanted to use "local" cube maps, we would have to change them per-object, or dynamically
        // index into an array of cube maps.

        CD3DX12_GPU_DESCRIPTOR_HANDLE skyTexDescriptor(mSrvDescriptorHeap->GetGPUDescriptorHandleForHeapStart());
        skyTexDescriptor.Offset(mSkyTexHeapIndex, mCbvSrvUavDescriptorSize);
        mCommandList->SetGraphicsRootDescriptorTable(3, skyTexDescriptor);

        //mCommandList->SetPipelineState(mPSOs["opaque"].Get());
        DrawRenderItems(mCommandList.Get(), mRitemLayer[(int)RenderLayer::Opaque]);

        /*mCommandList->SetPipelineState(mPSOs["debug"].Get());
        DrawRenderItems(mCommandList.Get(), mRitemLayer[(int)RenderLayer::Debug]);*/

        mCommandList->SetPipelineState(mPSOs["sky"].Get());
        DrawRenderItems(mCommandList.Get(), mRitemLayer[(int)RenderLayer::Sky]);

        // Indicate a state transition on the resource usage.
        mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(CurrentBackBuffer(),
            D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT));

        // Done recording commands.
        ThrowIfFailed(mCommandList->Close());

        // Add the command list to the queue for execution.
        ID3D12CommandList* cmdsLists[] = { mCommandList.Get() };
        mCommandQueue->ExecuteCommandLists(_countof(cmdsLists), cmdsLists);

        // Swap the back and front buffers

        UINT presentFlags = m_tearingSupport ? DXGI_PRESENT_ALLOW_TEARING : 0;

        ThrowIfFailed(mSwapChain->Present(0, presentFlags));
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

        //
        // Shadow map pass.
        //

        // Bind all the materials used in this scene.  For structured buffers, we can bypass the heap and 
        // set as a root descriptor.
        auto matBuffer = mCurrFrameResource->MaterialBuffer->Resource();
        mCommandList->SetGraphicsRootShaderResourceView(1, matBuffer->GetGPUVirtualAddress());

        // Bind null SRV for shadow map pass.
        mCommandList->SetGraphicsRootDescriptorTable(3, mNullSrv);

        // Bind all the textures used in this scene.  Observe
        // that we only have to specify the first descriptor in the table.  
        // The root signature knows how many descriptors are expected in the table.
        mCommandList->SetGraphicsRootDescriptorTable(4, mSrvDescriptorHeap->GetGPUDescriptorHandleForHeapStart());

        DrawSceneToShadowMap();

        //
        // Normal/depth pass.
        //

        DrawNormalsAndDepth();

        //
        // Compute SSAO.
        // 

        mCommandList->SetGraphicsRootSignature(mSsaoRootSignature.Get());
        mSsao->ComputeSsao(mCommandList.Get(), mCurrFrameResource, 3);

        //
        // Main rendering pass.
        //

        mCommandList->SetGraphicsRootSignature(mRootSignature.Get());

        // Rebind state whenever graphics root signature changes.

        // Bind all the materials used in this scene.  For structured buffers, we can bypass the heap and 
        // set as a root descriptor.
        matBuffer = mCurrFrameResource->MaterialBuffer->Resource();
        mCommandList->SetGraphicsRootShaderResourceView(1, matBuffer->GetGPUVirtualAddress());


        mCommandList->RSSetViewports(1, &mScreenViewport);
        mCommandList->RSSetScissorRects(1, &mScissorRect);

        // Indicate a state transition on the resource usage.
        mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(CurrentBackBuffer(),
            D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET));

        // Clear the back buffer.
        mCommandList->ClearRenderTargetView(CurrentBackBufferView(), Colors::LightSteelBlue, 0, nullptr);

        // WE ALREADY WROTE THE DEPTH INFO TO THE DEPTH BUFFER IN DrawNormalsAndDepth,
        // SO DO NOT CLEAR DEPTH.

        // Specify the buffers we are going to render to.
        mCommandList->OMSetRenderTargets(1, &CurrentBackBufferView(), true, &DepthStencilView());

        // Bind all the textures used in this scene.  Observe
        // that we only have to specify the first descriptor in the table.  
        // The root signature knows how many descriptors are expected in the table.
        mCommandList->SetGraphicsRootDescriptorTable(4, mSrvDescriptorHeap->GetGPUDescriptorHandleForHeapStart());

        auto passCB = mCurrFrameResource->PassCB->Resource();
        mCommandList->SetGraphicsRootConstantBufferView(2, passCB->GetGPUVirtualAddress());

        // Bind the sky cube map.  For our demos, we just use one "world" cube map representing the environment
        // from far away, so all objects will use the same cube map and we only need to set it once per-frame.  
        // If we wanted to use "local" cube maps, we would have to change them per-object, or dynamically
        // index into an array of cube maps.

        CD3DX12_GPU_DESCRIPTOR_HANDLE skyTexDescriptor(mSrvDescriptorHeap->GetGPUDescriptorHandleForHeapStart());
        skyTexDescriptor.Offset(mSkyTexHeapIndex, mCbvSrvUavDescriptorSize);
        mCommandList->SetGraphicsRootDescriptorTable(3, skyTexDescriptor);

        mCommandList->SetPipelineState(mPSOs["opaque"].Get());
        DrawRenderItems(mCommandList.Get(), mRitemLayer[(int)RenderLayer::Opaque]);

        mCommandList->SetPipelineState(mPSOs["debug"].Get());
        DrawRenderItems(mCommandList.Get(), mRitemLayer[(int)RenderLayer::Debug]);

        mCommandList->SetPipelineState(mPSOs["sky"].Get());
        DrawRenderItems(mCommandList.Get(), mRitemLayer[(int)RenderLayer::Sky]);

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
//    auto currObjectCB = mCurrFrameResource->ObjectCB.get();
//    for (auto& e : mAllRitems)
//    {
//        // Only update the cbuffer data if the constants have changed.  
//        // This needs to be tracked per frame resource.
//        if (e->NumFramesDirty > 0)
//        {
//            XMMATRIX world = XMLoadFloat4x4(&e->World);
//            XMMATRIX texTransform = XMLoadFloat4x4(&e->TexTransform);
//
//            ObjectConstants objConstants;
//            XMStoreFloat4x4(&objConstants.World, XMMatrixTranspose(world));
//            XMStoreFloat4x4(&objConstants.TexTransform, XMMatrixTranspose(texTransform));
//            objConstants.MaterialIndex = e->Mat->MatCBIndex;
//
//            currObjectCB->CopyData(e->ObjCBIndex, objConstants);
//
//            // Next FrameResource need to be updated too.
//            e->NumFramesDirty--;
//        }
//    }
//}

void CRYCHIC::UpdateInstanceData(const GameTimer& gt)
{
    XMMATRIX view = mCamera.GetView();
    XMMATRIX invView = XMMatrixInverse(&XMMatrixDeterminant(view), view);

    UINT totalVisibleInstanceCount = 0;
    UINT totalSceneInstanceCount = 0;

    for (size_t i = 0; i < mAllRitems.size(); i++)
    {
        auto currInstanceBuffer = mCurrFrameResource->InstanceBuffers[mAllRitems[i]->itemIndex].get();
        const auto& instanceData = mAllRitems[i]->Instances;
        int visibleInstanceCount = 0;
        for (size_t j = 0; j < instanceData.size(); j++)
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
            // mSceneItemCount 目的是避免生成动态cubemap时，场景中的物体被裁剪导致生成的cubemap
            // 中的物体也减少
            if ((i >= mSceneItemCount) || (localSpaceFrustum.Contains(mAllRitems[i]->Bounds) !=
                DISJOINT) || (mFrustumCullingEnabled == false))
            {
                InstanceData data;
                XMStoreFloat4x4(&data.World, XMMatrixTranspose(world));
                XMStoreFloat4x4(&data.TexTransform, XMMatrixTranspose(texTransform));
                data.MaterialIndex = instanceData[j].MaterialIndex;
                // visibleInstanceCount 记录了每个渲染项对应的实例数量
                currInstanceBuffer->CopyData(visibleInstanceCount++, data);
            }
        }
        mAllRitems[i]->InstanceCount = visibleInstanceCount;
        if (i < mSceneItemCount)
            totalVisibleInstanceCount += visibleInstanceCount;
    }
    std::wostringstream outs;
    outs.precision(6);
    outs << L"Instancing and Culling Demo" <<
        L"    " << totalVisibleInstanceCount <<
        L" objects visible out of " << mSceneInstancesCount;
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
            XMStoreFloat4x4(&matData.MatTransform, XMMatrixTranspose(matTransform));
            matData.DiffuseMapIndex = mat->DiffuseSrvHeapIndex;
            matData.NormalMapIndex = mat->NormalSrvHeapIndex;

            currMaterialBuffer->CopyData(mat->MatCBIndex, matData);

            // Next FrameResource need to be updated too.
            mat->NumFramesDirty--;
        }
    }
}

void CRYCHIC::UpdateShadowTransform(const GameTimer& gt)
{
    // Only the first "main" light casts a shadow.
    //XMVECTOR lightDir = XMLoadFloat3(&mRotatedLightDirections[0]);
    //XMVECTOR lightPos = -2.0f * mSceneBounds.Radius * lightDir;
    //XMVECTOR targetPos = XMLoadFloat3(&mSceneBounds.Center);
    //XMVECTOR lightUp = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
    //XMMATRIX lightView = XMMatrixLookAtLH(lightPos, targetPos, lightUp);

    //XMStoreFloat3(&mLightPosW, lightPos);

    //// Transform bounding sphere to light space.
    //XMFLOAT3 sphereCenterLS;
    //XMStoreFloat3(&sphereCenterLS, XMVector3TransformCoord(targetPos, lightView));

    //// Ortho frustum in light space encloses scene.
    //float l = sphereCenterLS.x - mSceneBounds.Radius;
    //float b = sphereCenterLS.y - mSceneBounds.Radius;
    //float n = sphereCenterLS.z - mSceneBounds.Radius;
    //float r = sphereCenterLS.x + mSceneBounds.Radius;
    //float t = sphereCenterLS.y + mSceneBounds.Radius;
    //float f = sphereCenterLS.z + mSceneBounds.Radius;

    //mLightNearZ = n;
    //mLightFarZ = f;
    //XMMATRIX lightProj = XMMatrixOrthographicOffCenterLH(l, r, b, t, n, f);

    //// Transform NDC space [-1,+1]^2 to texture space [0,1]^2
    //XMMATRIX T(
    //    0.5f, 0.0f, 0.0f, 0.0f,
    //    0.0f, -0.5f, 0.0f, 0.0f,
    //    0.0f, 0.0f, 1.0f, 0.0f,
    //    0.5f, 0.5f, 0.0f, 1.0f);

    //XMMATRIX S = lightView * lightProj * T;
    //XMStoreFloat4x4(&mLightView, lightView);
    //XMStoreFloat4x4(&mLightProj, lightProj);
    //XMStoreFloat4x4(&mShadowTransform, S);
}

void CRYCHIC::UpdateCascadeShadowTransform(const GameTimer& gt)
{
    XMMATRIX mCameraView = mCamera.GetView();
    XMMATRIX mInvCameraView = XMMatrixInverse(&XMMatrixDeterminant(mCameraView), mCameraView);


    float zNear[] = { mCamera.GetNearZ(), 30.0f, 50.0f, 80.0f };
    float zFar[] = { 30.0f, 50.0f, 80.0f, mCamera.GetFarZ() };


    for (size_t i = 0; i < 4; i++)
    {
        XMMATRIX mCameraProj = XMMatrixPerspectiveFovLH(mCamera.GetFovY(), mCamera.GetAspect(),
            zNear[i], zFar[i]);

        //XMMATRIX mInvCameraProj =  XMMatrixInverse(&XMMatrixDeterminant(mCameraProj), mCameraProj);
        XMMATRIX mInvViewProj = XMMatrixInverse(&XMMatrixDeterminant(mCameraView * mCameraProj), mCameraView * mCameraProj);
        //XMMATRIX mInvViewProj = XMMatrixMultiply(mInvCameraProj, mInvCameraView);
            //mInvCameraProj * mInvCameraView;
        //mInvViewProj = XMMatrixTranspose(mInvViewProj);
        /*XMMATRIX m = XMMatrixInverse(&XMMatrixDeterminant(mCameraView * mCameraProj), mCameraView * mCameraProj);*/
        // corners in NDC space
        XMFLOAT4 corners[] =
        {
            // near plane
            XMFLOAT4(-1.0f, +1.0f, 0.0f, +1.0f),
            XMFLOAT4(+1.0f, +1.0f, 0.0f, +1.0f),
            XMFLOAT4(+1.0f, -1.0f, 0.0f, +1.0f),
            XMFLOAT4(-1.0f, -1.0f, 0.0f, +1.0f),

            // far plane
            XMFLOAT4(-1.0f, +1.0f, 1.0f, +1.0f),
            XMFLOAT4(+1.0f, +1.0f, 1.0f, +1.0f),
            XMFLOAT4(+1.0f, -1.0f, 1.0f, +1.0f),
            XMFLOAT4(-1.0f, -1.0f, 1.0f, +1.0f)
        };

        //XMFLOAT4 corners[] =
        //{
        //    // near plane
        //    XMFLOAT4(-1.0f * zNear[i], +1.0f * zNear[i], zNear[i], zNear[i]),
        //    XMFLOAT4(+1.0f * zNear[i], +1.0f * zNear[i], zNear[i], zNear[i]),
        //    XMFLOAT4(+1.0f * zNear[i], -1.0f * zNear[i], zNear[i], zNear[i]),
        //    XMFLOAT4(-1.0f * zNear[i], -1.0f * zNear[i], zNear[i], zNear[i]),

        //    // far plane
        //    XMFLOAT4(-1.0f * zFar[i], +1.0f * zFar[i], zFar[i], zFar[i]),
        //    XMFLOAT4(+1.0f * zFar[i], +1.0f * zFar[i], zFar[i], zFar[i]),
        //    XMFLOAT4(+1.0f * zFar[i], -1.0f * zFar[i], zFar[i], zFar[i]),
        //    XMFLOAT4(-1.0f * zFar[i], -1.0f * zFar[i], zFar[i], zFar[i])
        //};

        // transform from ndc to world
        for (int j = 0; j < 8; j++)
        {
            XMVECTOR corner = XMLoadFloat4(&corners[j]);
            XMVECTOR cornerW = XMVector3Transform(corner, mInvViewProj);
            XMStoreFloat4(&corners[j], cornerW);
            //if (zNear[i] != mCamera.GetNearZ())
            {
                corners[j].x /= corners[j].w;
                corners[j].y /= corners[j].w;
                corners[j].z /= corners[j].w;
            }
            //XMFLOAT4 temp;
            //XMStoreFloat4(&temp, cornerV);
            ////temp.x /= temp.w;
            ////temp.y /= temp.w;
            ////temp.z /= temp.w;
            //XMVECTOR cornerW = XMVector3Transform(XMLoadFloat4(&temp), mInvCameraView);
            //XMStoreFloat4(&corners[i], cornerW);
        }

        // compute maxDistance
        float crossFar = sqrt((corners[7].x - corners[5].x) * (corners[7].x - corners[5].x) +
            (corners[7].y - corners[5].y) * (corners[7].y - corners[5].y) +
            (corners[7].z - corners[5].z) * (corners[7].z - corners[5].z));
        float crossNear2Far = sqrt((corners[3].x - corners[5].x) * (corners[3].x - corners[5].x) +
            (corners[3].y - corners[5].y) * (corners[3].y - corners[5].y) +
            (corners[3].z - corners[5].z) * (corners[3].z - corners[5].z));
        float boundingBoxLength = crossFar > crossNear2Far ? crossFar : crossNear2Far;
        
        XMFLOAT4 targetPos;
        targetPos.x = 0.5f * (corners[3].x + corners[5].x);
        targetPos.y = 0.5f * (corners[3].y + corners[5].y);
        targetPos.z = 0.5f * (corners[3].z + corners[5].z);
        targetPos.w = 1.0f;

        // construct a light view matrix
        // light position , target position, up
        //float distance = 0.5f * (zFar[i] - zNear[i]);
        float distance = boundingBoxLength;
        XMFLOAT3 lightDir = mBaseLightDirections[0];
        XMFLOAT4 lightPos;
        lightPos.x = -distance * lightDir.x + targetPos.x;
        lightPos.y = -distance * lightDir.y + targetPos.y;
        lightPos.z = -distance * lightDir.z + targetPos.z;
        lightPos.w = 1.0f;
        //XMFLOAT4 targetPos = corners[0];
        XMFLOAT4 up = XMFLOAT4(0.0f, 1.0f, 0.0f, 0.0f);
        XMMATRIX lightView = XMMatrixLookAtLH(XMLoadFloat4(&lightPos), XMLoadFloat4(&targetPos),
            XMLoadFloat4(&up));

        // transform world to light view space
        for (int i = 0; i < 8; i++)
        {
            XMVECTOR cornerW = XMLoadFloat4(&corners[i]);
            XMVECTOR cornerLight = XMVector3Transform(cornerW, lightView);
            XMStoreFloat4(&corners[i], cornerLight);
        }

        XMFLOAT3 vMinf3(+MathHelper::Infinity, +MathHelper::Infinity, +MathHelper::Infinity);
        XMFLOAT3 vMaxf3(-MathHelper::Infinity, -MathHelper::Infinity, -MathHelper::Infinity);
        XMVECTOR vMin = XMLoadFloat3(&vMinf3);
        XMVECTOR vMax = XMLoadFloat3(&vMaxf3);
        for (size_t i = 0; i < 8; i++)
        {
            vMin = XMVectorMin(XMLoadFloat4(&corners[i]), vMin);
            vMax = XMVectorMax(XMLoadFloat4(&corners[i]), vMax);
        }
        XMFLOAT3 vertexMax, vertexMin;
        XMStoreFloat3(&vertexMax, vMax);
        XMStoreFloat3(&vertexMin, vMin);

        float fWorldUnitsPerTexel = boundingBoxLength / mShadowMap->Width();
        XMVECTOR center = 0.5 * (vMin + vMax);
        XMFLOAT3 fCenter;
        XMStoreFloat3(&fCenter, center);
        fCenter.x /= fWorldUnitsPerTexel;
        fCenter.x = floor(fCenter.x);
        fCenter.x *= fWorldUnitsPerTexel;

        fCenter.y /= fWorldUnitsPerTexel;
        fCenter.y = floor(fCenter.y);
        fCenter.y *= fWorldUnitsPerTexel;

        fCenter.z /= fWorldUnitsPerTexel;
        fCenter.z = floor(fCenter.z);
        fCenter.z *= fWorldUnitsPerTexel;

        //XMFLOAT4 targetPosNew;
        //targetPosNew.x = fCenter.x;
        //targetPosNew.y = fCenter.y;
        //targetPosNew.z = fCenter.z;
        //targetPosNew.w = 1.0f;
        ////float distanceNew = boundingBoxLength;    
        //XMFLOAT4 lightPosNew;
        //lightPosNew.x = -distance * lightDir.x + targetPosNew.x;
        //lightPosNew.y = -distance * lightDir.y + targetPosNew.y;
        //lightPosNew.z = -distance * lightDir.z + targetPosNew.z;
        //lightPosNew.w = 1.0f;     
        //lightView = XMMatrixLookAtLH(XMLoadFloat4(&lightPosNew), XMLoadFloat4(&targetPosNew),
        //    XMLoadFloat4(&up));


        float l = fCenter.x - 0.5 * boundingBoxLength;
        float b = fCenter.y - 0.5 * boundingBoxLength;
        float n = fCenter.z - 0.5 * boundingBoxLength;
        float r = fCenter.x + 0.5 * boundingBoxLength;
        float t = fCenter.y + 0.5 * boundingBoxLength;
        float f = fCenter.z + 0.5 * boundingBoxLength;

        /*float l = vertexMin.x;
        float b = vertexMin.y;
        float n = vertexMin.z;
        float r = vertexMax.x;
        float t = vertexMax.y;
        float f = vertexMax.z;*/

        // construct light proj
        XMMATRIX lightProj = XMMatrixOrthographicOffCenterLH(l, r, b, t, n, f);
        XMMATRIX T(
            0.5f, 0.0f, 0.0f, 0.0f,
            0.0f, -0.5f, 0.0f, 0.0f,
            0.0f, 0.0f, 1.0f, 0.0f,
            0.5f, 0.5f, 0.0f, 1.0f);
        XMMATRIX shadowTransform = lightView * lightProj * T;
        XMStoreFloat4x4(&mLightViews[i], lightView);
        XMStoreFloat4x4(&mLightProjs[i], lightProj);
        XMStoreFloat4x4(&mShadowTransforms[i], shadowTransform);
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

    // Transform NDC space [-1,+1]^2 to texture space [0,1]^2
    XMMATRIX T(
        0.5f, 0.0f, 0.0f, 0.0f,
        0.0f, -0.5f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.5f, 0.5f, 0.0f, 1.0f);

    XMMATRIX viewProjTex = XMMatrixMultiply(viewProj, T);
    

    for (size_t i = 0; i < 12; i++)
    {
        XMMATRIX shadowTransform = XMLoadFloat4x4(&mShadowTransforms[i]);
        XMStoreFloat4x4(&mMainPassCB.ShadowTransforms[i], XMMatrixTranspose(shadowTransform));
    }

    XMStoreFloat4x4(&mMainPassCB.View, XMMatrixTranspose(view));
    XMStoreFloat4x4(&mMainPassCB.InvView, XMMatrixTranspose(invView));
    XMStoreFloat4x4(&mMainPassCB.Proj, XMMatrixTranspose(proj));
    XMStoreFloat4x4(&mMainPassCB.InvProj, XMMatrixTranspose(invProj));
    XMStoreFloat4x4(&mMainPassCB.ViewProj, XMMatrixTranspose(viewProj));
    XMStoreFloat4x4(&mMainPassCB.InvViewProj, XMMatrixTranspose(invViewProj));
    XMStoreFloat4x4(&mMainPassCB.ViewProjTex, XMMatrixTranspose(viewProjTex));
    
    mMainPassCB.EyePosW = mCamera.GetPosition3f();
    mMainPassCB.RenderTargetSize = XMFLOAT2((float)mClientWidth, (float)mClientHeight);
    mMainPassCB.InvRenderTargetSize = XMFLOAT2(1.0f / mClientWidth, 1.0f / mClientHeight);
    mMainPassCB.NearZ = 1.0f;
    mMainPassCB.FarZ = 1000.0f;
    mMainPassCB.TotalTime = gt.TotalTime();
    mMainPassCB.DeltaTime = gt.DeltaTime();
    mMainPassCB.AmbientLight = { 0.4f, 0.4f, 0.6f, 1.0f };
    mMainPassCB.Lights[0].Direction = mRotatedLightDirections[0];
    mMainPassCB.Lights[0].Strength = { 2.4f, 2.4f, 2.5f };
    mMainPassCB.Lights[1].Direction = mRotatedLightDirections[1];
    mMainPassCB.Lights[1].Strength = { 0.1f, 0.1f, 0.1f };
    mMainPassCB.Lights[2].Direction = mRotatedLightDirections[2];
    mMainPassCB.Lights[2].Strength = { 0.0f, 0.0f, 0.0f };

    auto currPassCB = mCurrFrameResource->PassCB.get();
    currPassCB->CopyData(0, mMainPassCB);
}

void CRYCHIC::UpdateShadowPassCB(const GameTimer& gt)
{
    for (size_t i = 0; i < 12; i++)
    {
        XMMATRIX view = XMLoadFloat4x4(&mLightViews[i]);
        XMMATRIX proj = XMLoadFloat4x4(&mLightProjs[i]);

        XMMATRIX viewProj = XMMatrixMultiply(view, proj);
        XMMATRIX invView = XMMatrixInverse(&XMMatrixDeterminant(view), view);
        XMMATRIX invProj = XMMatrixInverse(&XMMatrixDeterminant(proj), proj);
        XMMATRIX invViewProj = XMMatrixInverse(&XMMatrixDeterminant(viewProj), viewProj);

        UINT w = mShadowMap->Width();
        UINT h = mShadowMap->Height();

        XMStoreFloat4x4(&mShadowPassCB.View, XMMatrixTranspose(view));
        XMStoreFloat4x4(&mShadowPassCB.InvView, XMMatrixTranspose(invView));
        XMStoreFloat4x4(&mShadowPassCB.Proj, XMMatrixTranspose(proj));
        XMStoreFloat4x4(&mShadowPassCB.InvProj, XMMatrixTranspose(invProj));
        XMStoreFloat4x4(&mShadowPassCB.ViewProj, XMMatrixTranspose(viewProj));
        XMStoreFloat4x4(&mShadowPassCB.InvViewProj, XMMatrixTranspose(invViewProj));
        mShadowPassCB.EyePosW = mLightPosW;
        mShadowPassCB.RenderTargetSize = XMFLOAT2((float)w, (float)h);
        mShadowPassCB.InvRenderTargetSize = XMFLOAT2(1.0f / w, 1.0f / h);
        mShadowPassCB.NearZ = mLightNearZ;
        mShadowPassCB.FarZ = mLightFarZ;

        auto currPassCB = mCurrFrameResource->PassCB.get();
        currPassCB->CopyData(1 + i, mShadowPassCB);
    }
    
}

void CRYCHIC::UpdateSsaoCB(const GameTimer& gt)
{
    SsaoConstants ssaoCB;

    XMMATRIX P = mCamera.GetProj();

    // Transform NDC space [-1,+1]^2 to texture space [0,1]^2
    XMMATRIX T(
        0.5f, 0.0f, 0.0f, 0.0f,
        0.0f, -0.5f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.5f, 0.5f, 0.0f, 1.0f);

    ssaoCB.Proj = mMainPassCB.Proj;
    ssaoCB.InvProj = mMainPassCB.InvProj;
    XMStoreFloat4x4(&ssaoCB.ProjTex, XMMatrixTranspose(P * T));

    mSsao->GetOffsetVectors(ssaoCB.OffsetVectors);

    auto blurWeights = mSsao->CalcGaussWeights(2.5f);
    ssaoCB.BlurWeights[0] = XMFLOAT4(&blurWeights[0]);
    ssaoCB.BlurWeights[1] = XMFLOAT4(&blurWeights[4]);
    ssaoCB.BlurWeights[2] = XMFLOAT4(&blurWeights[8]);

    ssaoCB.InvRenderTargetSize = XMFLOAT2(1.0f / mSsao->SsaoMapWidth(), 1.0f / mSsao->SsaoMapHeight());

    // Coordinates given in view space.
    ssaoCB.OcclusionRadius = 0.5f;
    ssaoCB.OcclusionFadeStart = 0.2f;
    ssaoCB.OcclusionFadeEnd = 1.0f;
    ssaoCB.SurfaceEpsilon = 0.05f;

    auto currSsaoCB = mCurrFrameResource->SsaoCB.get();
    currSsaoCB->CopyData(0, ssaoCB);
}

void CRYCHIC::LoadTextures()
{
    std::vector<std::string> texNames =
    {
        "bricksDiffuseMap",
        "bricksNormalMap",
        "tileDiffuseMap",
        "tileNormalMap",
        "defaultDiffuseMap",
        "defaultNormalMap",
        "skyCubeMap"
    };

    std::vector<std::wstring> texFilenames =
    {
        L"Textures/bricks2.dds",
        L"Textures/bricks2_nmap.dds",
        L"Textures/tile.dds",
        L"Textures/tile_nmap.dds",
        L"Textures/white1x1.dds",
        L"Textures/default_nmap.dds",
        L"Textures/snowcube1024.dds"
    };

    for (int i = 0; i < (int)texNames.size(); ++i)
    {
        auto texMap = std::make_unique<Texture>();
        texMap->Name = texNames[i];
        texMap->Filename = texFilenames[i];
        ThrowIfFailed(DirectX::CreateDDSTextureFromFile12(md3dDevice.Get(),
            mCommandList.Get(), texMap->Filename.c_str(),
            texMap->Resource, texMap->UploadHeap));

        mTextures[texMap->Name] = std::move(texMap);
    }
}

void CRYCHIC::BuildRootSignature()
{
    // cubemap, shadowmap, ssao
    CD3DX12_DESCRIPTOR_RANGE texTable0;
    texTable0.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1 + 12 + 5 + 4, 0, 0);

    // textures
    CD3DX12_DESCRIPTOR_RANGE texTable1;
    texTable1.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 10, 22, 0);

    // Root parameter can be a table, root descriptor or root constants.
    CD3DX12_ROOT_PARAMETER slotRootParameter[5];

    // Perfomance TIP: Order from most frequent to least frequent.
    // structuredbuffer instanceData
    slotRootParameter[0].InitAsShaderResourceView(0, 1);
    // structuredbuffer materialData
    slotRootParameter[1].InitAsShaderResourceView(1, 1);
    // passCB
    slotRootParameter[2].InitAsConstantBufferView(0);
    slotRootParameter[3].InitAsDescriptorTable(1, &texTable0, D3D12_SHADER_VISIBILITY_PIXEL);
    slotRootParameter[4].InitAsDescriptorTable(1, &texTable1, D3D12_SHADER_VISIBILITY_PIXEL);


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

void CRYCHIC::BuildSsaoRootSignature()
{
    CD3DX12_DESCRIPTOR_RANGE texTable0;
    texTable0.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0, 0);

    CD3DX12_DESCRIPTOR_RANGE texTable1;
    texTable1.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 2, 0);

    // Root parameter can be a table, root descriptor or root constants.
    CD3DX12_ROOT_PARAMETER slotRootParameter[4];

    // Perfomance TIP: Order from most frequent to least frequent.
    slotRootParameter[0].InitAsConstantBufferView(0);
    slotRootParameter[1].InitAsConstants(1, 1);
    slotRootParameter[2].InitAsDescriptorTable(1, &texTable0, D3D12_SHADER_VISIBILITY_PIXEL);
    slotRootParameter[3].InitAsDescriptorTable(1, &texTable1, D3D12_SHADER_VISIBILITY_PIXEL);

    const CD3DX12_STATIC_SAMPLER_DESC pointClamp(
        0, // shaderRegister
        D3D12_FILTER_MIN_MAG_MIP_POINT, // filter
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressU
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressV
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP); // addressW

    const CD3DX12_STATIC_SAMPLER_DESC linearClamp(
        1, // shaderRegister
        D3D12_FILTER_MIN_MAG_MIP_LINEAR, // filter
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressU
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressV
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP); // addressW

    const CD3DX12_STATIC_SAMPLER_DESC depthMapSam(
        2, // shaderRegister
        D3D12_FILTER_MIN_MAG_MIP_LINEAR, // filter
        D3D12_TEXTURE_ADDRESS_MODE_BORDER,  // addressU
        D3D12_TEXTURE_ADDRESS_MODE_BORDER,  // addressV
        D3D12_TEXTURE_ADDRESS_MODE_BORDER,  // addressW
        0.0f,
        0,
        D3D12_COMPARISON_FUNC_LESS_EQUAL,
        D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE);

    const CD3DX12_STATIC_SAMPLER_DESC linearWrap(
        3, // shaderRegister
        D3D12_FILTER_MIN_MAG_MIP_LINEAR, // filter
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,  // addressU
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,  // addressV
        D3D12_TEXTURE_ADDRESS_MODE_WRAP); // addressW

    std::array<CD3DX12_STATIC_SAMPLER_DESC, 4> staticSamplers =
    {
        pointClamp, linearClamp, depthMapSam, linearWrap
    };

    // A root signature is an array of root parameters.
    CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(4, slotRootParameter,
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
        IID_PPV_ARGS(mSsaoRootSignature.GetAddressOf())));
}

void CRYCHIC::BuildDescriptorHeaps()
{
    //
    // Create the SRV heap.
    //
    D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {};
    srvHeapDesc.NumDescriptors = 18 + 10 + 4 + 2;
    srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ThrowIfFailed(md3dDevice->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&mSrvDescriptorHeap)));

    //
    // Fill out the heap with actual descriptors.
    //
    CD3DX12_CPU_DESCRIPTOR_HANDLE hDescriptor(mSrvDescriptorHeap->GetCPUDescriptorHandleForHeapStart());

    std::vector<ComPtr<ID3D12Resource>> tex2DList =
    {
        mTextures["bricksDiffuseMap"]->Resource,
        mTextures["bricksNormalMap"]->Resource,
        mTextures["tileDiffuseMap"]->Resource,
        mTextures["tileNormalMap"]->Resource,
        mTextures["defaultDiffuseMap"]->Resource,
        mTextures["defaultNormalMap"]->Resource
    };

    auto skyCubeMap = mTextures["skyCubeMap"]->Resource;

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MostDetailedMip = 0;
    srvDesc.Texture2D.ResourceMinLODClamp = 0.0f;

    for (UINT i = 0; i < (UINT)tex2DList.size(); ++i)
    {
        srvDesc.Format = tex2DList[i]->GetDesc().Format;
        srvDesc.Texture2D.MipLevels = tex2DList[i]->GetDesc().MipLevels;
        md3dDevice->CreateShaderResourceView(tex2DList[i].Get(), &srvDesc, hDescriptor);

        // next descriptor
        hDescriptor.Offset(1, mCbvSrvUavDescriptorSize);
    }

    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
    srvDesc.TextureCube.MostDetailedMip = 0;
    srvDesc.TextureCube.MipLevels = skyCubeMap->GetDesc().MipLevels;
    srvDesc.TextureCube.ResourceMinLODClamp = 0.0f;
    srvDesc.Format = skyCubeMap->GetDesc().Format;
    md3dDevice->CreateShaderResourceView(skyCubeMap.Get(), &srvDesc, hDescriptor);


    // 预留的十个描述符位置给texture
    //mDeferredIndex = 10;
    mSkyTexHeapIndex = (UINT)tex2DList.size();
    //mSkyTexHeapIndex = mDeferredIndex + 4;
    mShadowMapHeapIndex = mSkyTexHeapIndex + 1;
    mSsaoHeapIndexStart = mShadowMapHeapIndex + 12;
    mSsaoAmbientMapIndex = mSsaoHeapIndexStart + 3;
    mDeferredIndex = mSsaoHeapIndexStart + 5;
    mNullCubeSrvIndex = mDeferredIndex + 4;
    mNullTexSrvIndex1 = mNullCubeSrvIndex + 1;
    mNullTexSrvIndex2 = mNullTexSrvIndex1 + 1;

    auto nullSrv = GetCpuSrv(mNullCubeSrvIndex);
    mNullSrv = GetGpuSrv(mNullCubeSrvIndex);

    md3dDevice->CreateShaderResourceView(nullptr, &srvDesc, nullSrv);
    nullSrv.Offset(1, mCbvSrvUavDescriptorSize);

    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srvDesc.Texture2D.MostDetailedMip = 0;
    srvDesc.Texture2D.MipLevels = 1;
    srvDesc.Texture2D.ResourceMinLODClamp = 0.0f;
    md3dDevice->CreateShaderResourceView(nullptr, &srvDesc, nullSrv);

    /*nullSrv.Offset(1, mCbvSrvUavDescriptorSize);
    md3dDevice->CreateShaderResourceView(nullptr, &srvDesc, nullSrv);*/

    // swapchainBufferCount + normalMapCount(1) + aoMapsCount(2)
    mDeferred->BuildDescriptors(
        GetCpuSrv(mDeferredIndex),
        GetGpuSrv(mDeferredIndex),
        GetRtv(SwapChainBufferCount + 3)
    );

    mShadowMap->BuildDescriptors(
        GetCpuSrv(mShadowMapHeapIndex),
        GetGpuSrv(mShadowMapHeapIndex),
        GetDsv(1));

    mSsao->BuildDescriptors(
        mDepthStencilBuffer.Get(),
        GetCpuSrv(mSsaoHeapIndexStart),
        GetGpuSrv(mSsaoHeapIndexStart),
        GetRtv(SwapChainBufferCount),
        mCbvSrvUavDescriptorSize,
        mRtvDescriptorSize);
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

    mShaders["shadowVS"] = d3dUtil::CompileShader(L"Shaders\\Shadows.hlsl", nullptr, "VS", "vs_5_1");
    mShaders["shadowOpaquePS"] = d3dUtil::CompileShader(L"Shaders\\Shadows.hlsl", nullptr, "PS", "ps_5_1");
    mShaders["shadowAlphaTestedPS"] = d3dUtil::CompileShader(L"Shaders\\Shadows.hlsl", alphaTestDefines, "PS", "ps_5_1");

    mShaders["debugVS"] = d3dUtil::CompileShader(L"Shaders\\ShadowDebug.hlsl", nullptr, "VS", "vs_5_1");
    mShaders["debugPS"] = d3dUtil::CompileShader(L"Shaders\\ShadowDebug.hlsl", nullptr, "PS", "ps_5_1");

    mShaders["drawNormalsVS"] = d3dUtil::CompileShader(L"Shaders\\DrawNormals.hlsl", nullptr, "VS", "vs_5_1");
    mShaders["drawNormalsPS"] = d3dUtil::CompileShader(L"Shaders\\DrawNormals.hlsl", nullptr, "PS", "ps_5_1");

    mShaders["ssaoVS"] = d3dUtil::CompileShader(L"Shaders\\Ssao.hlsl", nullptr, "VS", "vs_5_1");
    mShaders["ssaoPS"] = d3dUtil::CompileShader(L"Shaders\\Ssao.hlsl", nullptr, "PS", "ps_5_1");

    mShaders["ssaoBlurVS"] = d3dUtil::CompileShader(L"Shaders\\SsaoBlur.hlsl", nullptr, "VS", "vs_5_1");
    mShaders["ssaoBlurPS"] = d3dUtil::CompileShader(L"Shaders\\SsaoBlur.hlsl", nullptr, "PS", "ps_5_1");

    mShaders["skyVS"] = d3dUtil::CompileShader(L"Shaders\\Sky.hlsl", nullptr, "VS", "vs_5_1");
    mShaders["skyPS"] = d3dUtil::CompileShader(L"Shaders\\Sky.hlsl", nullptr, "PS", "ps_5_1");

    mShaders["deferredVS"] = d3dUtil::CompileShader(L"Shaders\\DeferredShading.hlsl", nullptr, "VS", "vs_5_1");
    mShaders["deferredPS"] = d3dUtil::CompileShader(L"Shaders\\DeferredShading.hlsl", nullptr, "PS", "ps_5_1");

    mShaders["geometryVS"] = d3dUtil::CompileShader(L"Shaders\\GeometryPass.hlsl", nullptr, "VS", "vs_5_1");
    mShaders["geometryPS"] = d3dUtil::CompileShader(L"Shaders\\GeometryPass.hlsl", nullptr, "PS", "ps_5_1");
    
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
    UINT quadVertexOffset = cylinderVertexOffset + (UINT)cylinder.Vertices.size();

    // Cache the starting index for each object in the concatenated index buffer.
    UINT boxIndexOffset = 0;
    UINT gridIndexOffset = (UINT)box.Indices32.size();
    UINT sphereIndexOffset = gridIndexOffset + (UINT)grid.Indices32.size();
    UINT cylinderIndexOffset = sphereIndexOffset + (UINT)sphere.Indices32.size();
    UINT quadIndexOffset = cylinderIndexOffset + (UINT)cylinder.Indices32.size();

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

    XMFLOAT3 vMinf3(+MathHelper::Infinity, +MathHelper::Infinity, +MathHelper::Infinity);
    XMFLOAT3 vMaxf3(-MathHelper::Infinity, -MathHelper::Infinity, -MathHelper::Infinity);
    XMVECTOR vMin = XMLoadFloat3(&vMinf3);
    XMVECTOR vMax = XMLoadFloat3(&vMaxf3);

    UINT k = 0;
    for (size_t i = 0; i < box.Vertices.size(); ++i, ++k)
    {
        vertices[k].Pos = box.Vertices[i].Position;
        vertices[k].Normal = box.Vertices[i].Normal;
        vertices[k].TexC = box.Vertices[i].TexC;
        vertices[k].TangentU = box.Vertices[i].TangentU;
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

    for (size_t i = 0; i < grid.Vertices.size(); ++i, ++k)
    {
        vertices[k].Pos = grid.Vertices[i].Position;
        vertices[k].Normal = grid.Vertices[i].Normal;
        vertices[k].TexC = grid.Vertices[i].TexC;
        vertices[k].TangentU = grid.Vertices[i].TangentU;
        XMVECTOR P = XMLoadFloat3(&vertices[k].Pos);
        vMin = XMVectorMin(vMin, P);
        vMax = XMVectorMax(vMax, P);
    }
    XMStoreFloat3(&bounds.Center, 0.5f * (vMin + vMax));
    XMStoreFloat3(&bounds.Extents, 0.5f * (vMax - vMin));
    gridSubmesh.Bounds = bounds;


    vMin = XMLoadFloat3(&vMinf3);
    vMax = XMLoadFloat3(&vMaxf3);
    for (size_t i = 0; i < sphere.Vertices.size(); ++i, ++k)
    {
        vertices[k].Pos = sphere.Vertices[i].Position;
        vertices[k].Normal = sphere.Vertices[i].Normal;
        vertices[k].TexC = sphere.Vertices[i].TexC;
        vertices[k].TangentU = sphere.Vertices[i].TangentU;
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
        vertices[k].TangentU = cylinder.Vertices[i].TangentU;
        XMVECTOR P = XMLoadFloat3(&vertices[k].Pos);
        vMin = XMVectorMin(vMin, P);
        vMax = XMVectorMax(vMax, P);
    }

    XMStoreFloat3(&bounds.Center, 0.5f * (vMin + vMax));
    XMStoreFloat3(&bounds.Extents, 0.5f * (vMax - vMin));
    cylinderSubmesh.Bounds = bounds;

    vMin = XMLoadFloat3(&vMinf3);
    vMax = XMLoadFloat3(&vMaxf3);
    for (int i = 0; i < quad.Vertices.size(); ++i, ++k)
    {
        vertices[k].Pos = quad.Vertices[i].Position;
        vertices[k].Normal = quad.Vertices[i].Normal;
        vertices[k].TexC = quad.Vertices[i].TexC;
        vertices[k].TangentU = quad.Vertices[i].TangentU;
        XMVECTOR P = XMLoadFloat3(&vertices[k].Pos);
        vMin = XMVectorMin(vMin, P);
        vMax = XMVectorMax(vMax, P);
    }

    XMStoreFloat3(&bounds.Center, 0.5f * (vMin + vMax));
    XMStoreFloat3(&bounds.Extents, 0.5f * (vMax - vMin));
    quadSubmesh.Bounds = bounds;

    std::vector<std::uint16_t> indices;
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

void CRYCHIC::BuildSkullGeometry()
{
    std::ifstream fin("Models/skull.txt");

    if (!fin)
    {
        MessageBox(0, L"Models/skull.txt not found.", 0, 0);
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

        vertices[i].TexC = { 0.0f, 0.0f };

        XMVECTOR P = XMLoadFloat3(&vertices[i].Pos);

        XMVECTOR N = XMLoadFloat3(&vertices[i].Normal);

        // Generate a tangent vector so normal mapping works.  We aren't applying
        // a texture map to the skull, so we just need any tangent vector so that
        // the math works out to give us the original interpolated vertex normal.
        XMVECTOR up = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
        if (fabsf(XMVectorGetX(XMVector3Dot(N, up))) < 1.0f - 0.001f)
        {
            XMVECTOR T = XMVector3Normalize(XMVector3Cross(up, N));
            XMStoreFloat3(&vertices[i].TangentU, T);
        }
        else
        {
            up = XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f);
            XMVECTOR T = XMVector3Normalize(XMVector3Cross(N, up));
            XMStoreFloat3(&vertices[i].TangentU, T);
        }


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

    auto geo = std::make_unique<MeshGeometry>();
    geo->Name = "skullGeo";

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

    geo->DrawArgs["skull"] = submesh;

    mGeometries[geo->Name] = std::move(geo);
}

void CRYCHIC::BuildPSOs()
{
    D3D12_GRAPHICS_PIPELINE_STATE_DESC basePsoDesc;


    ZeroMemory(&basePsoDesc, sizeof(D3D12_GRAPHICS_PIPELINE_STATE_DESC));
    basePsoDesc.InputLayout = { mInputLayout.data(), (UINT)mInputLayout.size() };
    basePsoDesc.pRootSignature = mRootSignature.Get();
    basePsoDesc.VS =
    {
        reinterpret_cast<BYTE*>(mShaders["standardVS"]->GetBufferPointer()),
        mShaders["standardVS"]->GetBufferSize()
    };
    basePsoDesc.PS =
    {
        reinterpret_cast<BYTE*>(mShaders["opaquePS"]->GetBufferPointer()),
        mShaders["opaquePS"]->GetBufferSize()
    };
    basePsoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    basePsoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    basePsoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    basePsoDesc.SampleMask = UINT_MAX;
    basePsoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    basePsoDesc.NumRenderTargets = 1;
    basePsoDesc.RTVFormats[0] = mBackBufferFormat;
    basePsoDesc.SampleDesc.Count = m4xMsaaState ? 4 : 1;
    basePsoDesc.SampleDesc.Quality = m4xMsaaState ? (m4xMsaaQuality - 1) : 0;
    basePsoDesc.DSVFormat = mDepthStencilFormat;

    //
    // PSO for opaque objects.
    //

    D3D12_GRAPHICS_PIPELINE_STATE_DESC opaquePsoDesc = basePsoDesc;
    opaquePsoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_EQUAL;
    opaquePsoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&opaquePsoDesc, IID_PPV_ARGS(&mPSOs["opaque"])));

    //
    // PSO for shadow map pass.
    //
    D3D12_GRAPHICS_PIPELINE_STATE_DESC smapPsoDesc = basePsoDesc;
    smapPsoDesc.RasterizerState.DepthBias = 10000;
    smapPsoDesc.RasterizerState.DepthBiasClamp = 0.0f;
    smapPsoDesc.RasterizerState.SlopeScaledDepthBias = 2.0f;
    smapPsoDesc.pRootSignature = mRootSignature.Get();
    smapPsoDesc.VS =
    {
        reinterpret_cast<BYTE*>(mShaders["shadowVS"]->GetBufferPointer()),
        mShaders["shadowVS"]->GetBufferSize()
    };
    smapPsoDesc.PS =
    {
        reinterpret_cast<BYTE*>(mShaders["shadowOpaquePS"]->GetBufferPointer()),
        mShaders["shadowOpaquePS"]->GetBufferSize()
    };

    // Shadow map pass does not have a render target.
    smapPsoDesc.RTVFormats[0] = DXGI_FORMAT_UNKNOWN;
    smapPsoDesc.NumRenderTargets = 0;
    ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&smapPsoDesc, IID_PPV_ARGS(&mPSOs["shadow_opaque"])));

    //
    // PSO for debug layer.
    //
    D3D12_GRAPHICS_PIPELINE_STATE_DESC debugPsoDesc = basePsoDesc;
    debugPsoDesc.pRootSignature = mRootSignature.Get();
    debugPsoDesc.VS =
    {
        reinterpret_cast<BYTE*>(mShaders["debugVS"]->GetBufferPointer()),
        mShaders["debugVS"]->GetBufferSize()
    };
    debugPsoDesc.PS =
    {
        reinterpret_cast<BYTE*>(mShaders["debugPS"]->GetBufferPointer()),
        mShaders["debugPS"]->GetBufferSize()
    };
    ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&debugPsoDesc, IID_PPV_ARGS(&mPSOs["debug"])));

    //
    // PSO for drawing normals.
    //
    D3D12_GRAPHICS_PIPELINE_STATE_DESC drawNormalsPsoDesc = basePsoDesc;
    drawNormalsPsoDesc.VS =
    {
        reinterpret_cast<BYTE*>(mShaders["drawNormalsVS"]->GetBufferPointer()),
        mShaders["drawNormalsVS"]->GetBufferSize()
    };
    drawNormalsPsoDesc.PS =
    {
        reinterpret_cast<BYTE*>(mShaders["drawNormalsPS"]->GetBufferPointer()),
        mShaders["drawNormalsPS"]->GetBufferSize()
    };
    drawNormalsPsoDesc.RTVFormats[0] = Ssao::NormalMapFormat;
    drawNormalsPsoDesc.SampleDesc.Count = 1;
    drawNormalsPsoDesc.SampleDesc.Quality = 0;
    drawNormalsPsoDesc.DSVFormat = mDepthStencilFormat;
    ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&drawNormalsPsoDesc, IID_PPV_ARGS(&mPSOs["drawNormals"])));

    //
    // PSO for SSAO.
    //
    D3D12_GRAPHICS_PIPELINE_STATE_DESC ssaoPsoDesc = basePsoDesc;
    ssaoPsoDesc.InputLayout = { nullptr, 0 };
    ssaoPsoDesc.pRootSignature = mSsaoRootSignature.Get();
    ssaoPsoDesc.VS =
    {
        reinterpret_cast<BYTE*>(mShaders["ssaoVS"]->GetBufferPointer()),
        mShaders["ssaoVS"]->GetBufferSize()
    };
    ssaoPsoDesc.PS =
    {
        reinterpret_cast<BYTE*>(mShaders["ssaoPS"]->GetBufferPointer()),
        mShaders["ssaoPS"]->GetBufferSize()
    };

    // SSAO effect does not need the depth buffer.
    ssaoPsoDesc.DepthStencilState.DepthEnable = false;
    ssaoPsoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    ssaoPsoDesc.RTVFormats[0] = Ssao::AmbientMapFormat;
    ssaoPsoDesc.SampleDesc.Count = 1;
    ssaoPsoDesc.SampleDesc.Quality = 0;
    ssaoPsoDesc.DSVFormat = DXGI_FORMAT_UNKNOWN;
    ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&ssaoPsoDesc, IID_PPV_ARGS(&mPSOs["ssao"])));

    //
    // PSO for SSAO blur.
    //
    D3D12_GRAPHICS_PIPELINE_STATE_DESC ssaoBlurPsoDesc = ssaoPsoDesc;
    ssaoBlurPsoDesc.VS =
    {
        reinterpret_cast<BYTE*>(mShaders["ssaoBlurVS"]->GetBufferPointer()),
        mShaders["ssaoBlurVS"]->GetBufferSize()
    };
    ssaoBlurPsoDesc.PS =
    {
        reinterpret_cast<BYTE*>(mShaders["ssaoBlurPS"]->GetBufferPointer()),
        mShaders["ssaoBlurPS"]->GetBufferSize()
    };
    ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&ssaoBlurPsoDesc, IID_PPV_ARGS(&mPSOs["ssaoBlur"])));

    //
    // PSO for sky.
    //
    D3D12_GRAPHICS_PIPELINE_STATE_DESC skyPsoDesc = basePsoDesc;

    // The camera is inside the sky sphere, so just turn off culling.
    skyPsoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;

    // Make sure the depth function is LESS_EQUAL and not just LESS.  
    // Otherwise, the normalized depth values at z = 1 (NDC) will 
    // fail the depth test if the depth buffer was cleared to 1.
    skyPsoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    skyPsoDesc.pRootSignature = mRootSignature.Get();
    skyPsoDesc.VS =
    {
        reinterpret_cast<BYTE*>(mShaders["skyVS"]->GetBufferPointer()),
        mShaders["skyVS"]->GetBufferSize()
    };
    skyPsoDesc.PS =
    {
        reinterpret_cast<BYTE*>(mShaders["skyPS"]->GetBufferPointer()),
        mShaders["skyPS"]->GetBufferSize()
    };
    ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&skyPsoDesc, IID_PPV_ARGS(&mPSOs["sky"])));

    //
    // PSO for GBuffer.
    //
    D3D12_GRAPHICS_PIPELINE_STATE_DESC gBufferPsoDesc = basePsoDesc;
    gBufferPsoDesc.VS = {
        reinterpret_cast<BYTE*>(mShaders["geometryVS"]->GetBufferPointer()),
        mShaders["geometryVS"]->GetBufferSize()
    };
    gBufferPsoDesc.PS = {
        reinterpret_cast<BYTE*>(mShaders["geometryPS"]->GetBufferPointer()),
        mShaders["geometryPS"]->GetBufferSize()
    };
    gBufferPsoDesc.NumRenderTargets = 4;
    for (size_t i = 0; i < 4; i++)
    {
        gBufferPsoDesc.RTVFormats[i] = mDeferred->Format();
    }
    ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&gBufferPsoDesc, IID_PPV_ARGS(&mPSOs["geometryPass"])));

    //
    // PSO for DeferredShading.
    //
    D3D12_GRAPHICS_PIPELINE_STATE_DESC deferredPsoDesc = basePsoDesc;
    deferredPsoDesc.VS = {
        reinterpret_cast<BYTE*>(mShaders["deferredVS"]->GetBufferPointer()),
        mShaders["deferredVS"]->GetBufferSize()
    };
    deferredPsoDesc.PS = {
    reinterpret_cast<BYTE*>(mShaders["deferredPS"]->GetBufferPointer()),
    mShaders["deferredPS"]->GetBufferSize()
    };
    ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&deferredPsoDesc, IID_PPV_ARGS(&mPSOs["deferredShading"])));
}

void CRYCHIC::BuildFrameResources()
{
    for (int i = 0; i < gNumFrameResources; ++i)
    {
        mFrameResources.push_back(std::make_unique<FrameResource>(md3dDevice.Get(),
            1 + 12, mInstanceCounts, (UINT)mAllRitems.size(), (UINT)mMaterials.size()));
    }
}

void CRYCHIC::BuildMaterials()
{
    auto bricks0 = std::make_unique<Material>();
    bricks0->Name = "bricks0";
    bricks0->MatCBIndex = 0;
    bricks0->DiffuseSrvHeapIndex = 0;
    bricks0->NormalSrvHeapIndex = 1;
    bricks0->DiffuseAlbedo = XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);
    bricks0->FresnelR0 = XMFLOAT3(0.1f, 0.1f, 0.1f);
    bricks0->Roughness = 0.3f;
    //bricks0->Metalness = 0.3f;

    auto tile0 = std::make_unique<Material>();
    tile0->Name = "tile0";
    tile0->MatCBIndex = 1;
    tile0->DiffuseSrvHeapIndex = 2;
    tile0->NormalSrvHeapIndex = 3;
    tile0->DiffuseAlbedo = XMFLOAT4(0.9f, 0.9f, 0.9f, 1.0f);
    tile0->FresnelR0 = XMFLOAT3(0.2f, 0.2f, 0.2f);
    tile0->Roughness = 0.7f;

    auto mirror0 = std::make_unique<Material>();
    mirror0->Name = "mirror0";
    mirror0->MatCBIndex = 2;
    mirror0->DiffuseSrvHeapIndex = 4;
    mirror0->NormalSrvHeapIndex = 5;
    mirror0->DiffuseAlbedo = XMFLOAT4(0.0f, 0.0f, 0.0f, 1.0f);
    mirror0->FresnelR0 = XMFLOAT3(0.98f, 0.97f, 0.95f);
    mirror0->Roughness = 0.1f;

    auto skullMat = std::make_unique<Material>();
    skullMat->Name = "skullMat";
    skullMat->MatCBIndex = 3;
    skullMat->DiffuseSrvHeapIndex = 4;
    skullMat->NormalSrvHeapIndex = 5;
    skullMat->DiffuseAlbedo = XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);
    skullMat->FresnelR0 = XMFLOAT3(0.6f, 0.6f, 0.6f);
    skullMat->Roughness = 0.8f;

    auto sky = std::make_unique<Material>();
    sky->Name = "sky";
    sky->MatCBIndex = 4;
    sky->DiffuseSrvHeapIndex = 6;
    sky->NormalSrvHeapIndex = 7;
    sky->DiffuseAlbedo = XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);
    sky->FresnelR0 = XMFLOAT3(0.1f, 0.1f, 0.1f);
    sky->Roughness = 1.0f;

    mMaterials["bricks0"] = std::move(bricks0);
    mMaterials["tile0"] = std::move(tile0);
    mMaterials["mirror0"] = std::move(mirror0);
    mMaterials["skullMat"] = std::move(skullMat);
    mMaterials["sky"] = std::move(sky);
}

void CRYCHIC::BuildRenderItems()
{
    auto skyRitem = std::make_unique<RenderItem>();
    //XMStoreFloat4x4(&skyRitem->World, XMMatrixScaling(5000.0f, 5000.0f, 5000.0f));
    //skyRitem->TexTransform = MathHelper::Identity4x4();
    //skyRitem->ObjCBIndex = 0;
    skyRitem->itemIndex = mItemIndex++;
    skyRitem->Mat = mMaterials["sky"].get();
    skyRitem->Geo = mGeometries["shapeGeo"].get();
    skyRitem->PrimitiveType = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    skyRitem->IndexCount = skyRitem->Geo->DrawArgs["sphere"].IndexCount;
    skyRitem->StartIndexLocation = skyRitem->Geo->DrawArgs["sphere"].StartIndexLocation;
    skyRitem->BaseVertexLocation = skyRitem->Geo->DrawArgs["sphere"].BaseVertexLocation;
    skyRitem->Bounds = skyRitem->Geo->DrawArgs["sphere"].Bounds;
    UINT skyInstanceCount = 1;
    mInstanceCounts.push_back(skyInstanceCount);
    mSceneInstancesCount += skyInstanceCount;
    skyRitem->Instances.resize(skyInstanceCount);
    skyRitem->InstanceCount = skyInstanceCount;
    XMStoreFloat4x4(&skyRitem->Instances[0].World, XMMatrixScaling(5000.0f, 5000.0f, 5000.0f));
    skyRitem->Instances[0].TexTransform = MathHelper::Identity4x4();
    skyRitem->Instances[0].MaterialIndex = 4;
    mRitemLayer[(int)RenderLayer::Sky].push_back(skyRitem.get());
    mAllRitems.push_back(std::move(skyRitem));

    auto quadRitem = std::make_unique<RenderItem>();
    //quadRitem->World = MathHelper::Identity4x4();
    //quadRitem->TexTransform = MathHelper::Identity4x4();
    //quadRitem->ObjCBIndex = 1;
    quadRitem->itemIndex = mItemIndex++;
    quadRitem->Mat = mMaterials["bricks0"].get();
    quadRitem->Geo = mGeometries["shapeGeo"].get();
    quadRitem->PrimitiveType = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    quadRitem->IndexCount = quadRitem->Geo->DrawArgs["quad"].IndexCount;
    quadRitem->StartIndexLocation = quadRitem->Geo->DrawArgs["quad"].StartIndexLocation;
    quadRitem->BaseVertexLocation = quadRitem->Geo->DrawArgs["quad"].BaseVertexLocation;
    quadRitem->Bounds = quadRitem->Geo->DrawArgs["quad"].Bounds;
    UINT quadInstanceCount = 1;
    mInstanceCounts.push_back(quadInstanceCount);
    mSceneInstancesCount += quadInstanceCount;
    quadRitem->Instances.resize(quadInstanceCount);
    quadRitem->InstanceCount = quadInstanceCount;
    quadRitem->Instances[0].World = MathHelper::Identity4x4();
    quadRitem->Instances[0].TexTransform = MathHelper::Identity4x4();
    quadRitem->Instances[0].MaterialIndex = 0;
    mRitemLayer[(int)RenderLayer::Debug].push_back(quadRitem.get());
    mAllRitems.push_back(std::move(quadRitem));

    auto boxRitem = std::make_unique<RenderItem>();
    //XMStoreFloat4x4(&boxRitem->World, XMMatrixScaling(2.0f, 1.0f, 2.0f) * XMMatrixTranslation(0.0f, 0.5f, 0.0f));
    //XMStoreFloat4x4(&boxRitem->TexTransform, XMMatrixScaling(1.0f, 0.5f, 1.0f));
    //boxRitem->ObjCBIndex = 2;
    boxRitem->itemIndex = mItemIndex++;
    boxRitem->Mat = mMaterials["bricks0"].get();
    boxRitem->Geo = mGeometries["shapeGeo"].get();
    boxRitem->PrimitiveType = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    boxRitem->IndexCount = boxRitem->Geo->DrawArgs["box"].IndexCount;
    boxRitem->StartIndexLocation = boxRitem->Geo->DrawArgs["box"].StartIndexLocation;
    boxRitem->BaseVertexLocation = boxRitem->Geo->DrawArgs["box"].BaseVertexLocation;
    boxRitem->Bounds = boxRitem->Geo->DrawArgs["box"].Bounds;

    UINT boxInstanceCount = 1;
    mInstanceCounts.push_back(boxInstanceCount);
    mSceneInstancesCount += boxInstanceCount;
    boxRitem->Instances.resize(boxInstanceCount);
    boxRitem->InstanceCount = boxInstanceCount;
    XMStoreFloat4x4(&boxRitem->Instances[0].World, XMMatrixScaling(2.0f, 1.0f, 2.0f) * XMMatrixTranslation(0.0f, 0.5f, 0.0f));
    XMStoreFloat4x4(&boxRitem->Instances[0].TexTransform, XMMatrixScaling(1.0f, 0.5f, 1.0f));
    boxRitem->Instances[0].MaterialIndex = 0;
    mRitemLayer[(int)RenderLayer::Opaque].push_back(boxRitem.get());
    mAllRitems.push_back(std::move(boxRitem));

    auto skullRitem = std::make_unique<RenderItem>();
    /*XMStoreFloat4x4(&skullRitem->World, XMMatrixScaling(0.4f, 0.4f, 0.4f) * XMMatrixTranslation(0.0f, 1.0f, 0.0f));
    skullRitem->TexTransform = MathHelper::Identity4x4();
    skullRitem->ObjCBIndex = 3;*/
    skullRitem->itemIndex = mItemIndex++;
    skullRitem->Mat = mMaterials["skullMat"].get();
    skullRitem->Geo = mGeometries["skullGeo"].get();
    skullRitem->PrimitiveType = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    skullRitem->IndexCount = skullRitem->Geo->DrawArgs["skull"].IndexCount;
    skullRitem->StartIndexLocation = skullRitem->Geo->DrawArgs["skull"].StartIndexLocation;
    skullRitem->BaseVertexLocation = skullRitem->Geo->DrawArgs["skull"].BaseVertexLocation;
    skullRitem->Bounds = skullRitem->Geo->DrawArgs["skull"].Bounds;

    UINT skullInstanceCount = 1;
    mInstanceCounts.push_back(skullInstanceCount);
    mSceneInstancesCount += skullInstanceCount;
    skullRitem->Instances.resize(skullInstanceCount);
    skullRitem->InstanceCount = skullInstanceCount;
    XMStoreFloat4x4(&skullRitem->Instances[0].World, XMMatrixScaling(0.4f, 0.4f, 0.4f) * XMMatrixTranslation(0.0f, 1.0f, 0.0f));
    skullRitem->Instances[0].TexTransform = MathHelper::Identity4x4();
    skullRitem->Instances[0].MaterialIndex = 3;
    mRitemLayer[(int)RenderLayer::Opaque].push_back(skullRitem.get());
    mAllRitems.push_back(std::move(skullRitem));

    auto gridRitem = std::make_unique<RenderItem>();
    //gridRitem->World = MathHelper::Identity4x4();
    //XMStoreFloat4x4(&gridRitem->TexTransform, XMMatrixScaling(8.0f, 8.0f, 1.0f));
    //gridRitem->ObjCBIndex = 4;
    gridRitem->itemIndex = mItemIndex++;
    gridRitem->Mat = mMaterials["tile0"].get();
    gridRitem->Geo = mGeometries["shapeGeo"].get();
    gridRitem->PrimitiveType = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    gridRitem->IndexCount = gridRitem->Geo->DrawArgs["grid"].IndexCount;
    gridRitem->StartIndexLocation = gridRitem->Geo->DrawArgs["grid"].StartIndexLocation;
    gridRitem->BaseVertexLocation = gridRitem->Geo->DrawArgs["grid"].BaseVertexLocation;
    gridRitem->Bounds = gridRitem->Geo->DrawArgs["grid"].Bounds;

    UINT gridInstanceCount = 1;
    mInstanceCounts.push_back(gridInstanceCount);
    mSceneInstancesCount += gridInstanceCount;
    gridRitem->Instances.resize(gridInstanceCount);
    gridRitem->InstanceCount = gridInstanceCount;
    gridRitem->Instances[0].World = MathHelper::Identity4x4();
    XMStoreFloat4x4(&gridRitem->Instances[0].TexTransform, XMMatrixScaling(8.0f, 8.0f, 1.0f));
    gridRitem->Instances[0].MaterialIndex = 1; // tile
    mRitemLayer[(int)RenderLayer::Opaque].push_back(gridRitem.get());
    mAllRitems.push_back(std::move(gridRitem));

    auto leftCylRitem = std::make_unique<RenderItem>();
    leftCylRitem->itemIndex = mItemIndex++;
    leftCylRitem->Mat = mMaterials["bricks0"].get();
    leftCylRitem->Geo = mGeometries["shapeGeo"].get();
    leftCylRitem->PrimitiveType = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    leftCylRitem->IndexCount = leftCylRitem->Geo->DrawArgs["cylinder"].IndexCount;
    leftCylRitem->StartIndexLocation = leftCylRitem->Geo->DrawArgs["cylinder"].StartIndexLocation;
    leftCylRitem->BaseVertexLocation = leftCylRitem->Geo->DrawArgs["cylinder"].BaseVertexLocation;
    leftCylRitem->Bounds = leftCylRitem->Geo->DrawArgs["cylinder"].Bounds;
    UINT leftCylInstanceCount = 5;
    leftCylRitem->InstanceCount = leftCylInstanceCount;
    mInstanceCounts.push_back(leftCylInstanceCount);
    mSceneInstancesCount += leftCylInstanceCount;
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
    mInstanceCounts.push_back(rightCylInstanceCount);
    mSceneInstancesCount += rightCylInstanceCount;
    rightCylRitem->Instances.resize(rightCylInstanceCount);

    auto leftSphereRitem = std::make_unique<RenderItem>();
    leftSphereRitem->itemIndex = mItemIndex++;
    //leftSphereRitem->TexTransform = MathHelper::Identity4x4();
    //leftSphereRitem->ObjCBIndex = objCBIndex++;
    leftSphereRitem->Mat = mMaterials["mirror0"].get();
    leftSphereRitem->Geo = mGeometries["shapeGeo"].get();
    leftSphereRitem->PrimitiveType = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    leftSphereRitem->IndexCount = leftSphereRitem->Geo->DrawArgs["sphere"].IndexCount;
    leftSphereRitem->StartIndexLocation = leftSphereRitem->Geo->DrawArgs["sphere"].StartIndexLocation;
    leftSphereRitem->BaseVertexLocation = leftSphereRitem->Geo->DrawArgs["sphere"].BaseVertexLocation;
    leftSphereRitem->Bounds = leftSphereRitem->Geo->DrawArgs["sphere"].Bounds;
    UINT leftSphereInstanceCount = 5;
    leftSphereRitem->InstanceCount = leftSphereInstanceCount;
    mInstanceCounts.push_back(leftSphereInstanceCount);
    mSceneInstancesCount += leftSphereInstanceCount;
    leftSphereRitem->Instances.resize(leftSphereInstanceCount);

    auto rightSphereRitem = std::make_unique<RenderItem>();
    //rightSphereRitem->TexTransform = MathHelper::Identity4x4();
    rightSphereRitem->itemIndex = mItemIndex++;
    rightSphereRitem->Mat = mMaterials["mirror0"].get();
    rightSphereRitem->Geo = mGeometries["shapeGeo"].get();
    rightSphereRitem->PrimitiveType = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    rightSphereRitem->IndexCount = rightSphereRitem->Geo->DrawArgs["sphere"].IndexCount;
    rightSphereRitem->StartIndexLocation = rightSphereRitem->Geo->DrawArgs["sphere"].StartIndexLocation;
    rightSphereRitem->BaseVertexLocation = rightSphereRitem->Geo->DrawArgs["sphere"].BaseVertexLocation;
    rightSphereRitem->Bounds = rightSphereRitem->Geo->DrawArgs["sphere"].Bounds;
    UINT rightSphereInstanceCount = 5;
    rightSphereRitem->InstanceCount = rightSphereInstanceCount;
    mInstanceCounts.push_back(rightSphereInstanceCount);
    mSceneInstancesCount += rightSphereInstanceCount;
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
        leftSphereRitem->Instances[i].MaterialIndex = 2; // mirror
        XMStoreFloat4x4(&rightSphereRitem->Instances[i].World, rightSphereWorld);
        XMStoreFloat4x4(&rightSphereRitem->Instances[i].TexTransform, brickTexTransform);
        rightSphereRitem->Instances[i].MaterialIndex = 2; // mirror
    }
    mRitemLayer[(int)RenderLayer::Opaque].push_back(leftCylRitem.get());
    mRitemLayer[(int)RenderLayer::Opaque].push_back(rightCylRitem.get());
    mRitemLayer[(int)RenderLayer::Opaque].push_back(leftSphereRitem.get());
    mRitemLayer[(int)RenderLayer::Opaque].push_back(rightSphereRitem.get());

    mAllRitems.push_back(std::move(leftCylRitem));
    mAllRitems.push_back(std::move(rightCylRitem));
    mAllRitems.push_back(std::move(leftSphereRitem));
    mAllRitems.push_back(std::move(rightSphereRitem));

    mSceneItemCount = mItemIndex;
    /*XMMATRIX brickTexTransform = XMMatrixScaling(1.5f, 2.0f, 1.0f);
    UINT objCBIndex = 5;
    for (int i = 0; i < 5; ++i)
    {
        auto leftCylRitem = std::make_unique<RenderItem>();
        auto rightCylRitem = std::make_unique<RenderItem>();
        auto leftSphereRitem = std::make_unique<RenderItem>();
        auto rightSphereRitem = std::make_unique<RenderItem>();

        XMMATRIX leftCylWorld = XMMatrixTranslation(-5.0f, 1.5f, -10.0f + i * 5.0f);
        XMMATRIX rightCylWorld = XMMatrixTranslation(+5.0f, 1.5f, -10.0f + i * 5.0f);

        XMMATRIX leftSphereWorld = XMMatrixTranslation(-5.0f, 3.5f, -10.0f + i * 5.0f);
        XMMATRIX rightSphereWorld = XMMatrixTranslation(+5.0f, 3.5f, -10.0f + i * 5.0f);

        XMStoreFloat4x4(&leftCylRitem->World, rightCylWorld);
        XMStoreFloat4x4(&leftCylRitem->TexTransform, brickTexTransform);
        leftCylRitem->ObjCBIndex = objCBIndex++;
        leftCylRitem->Mat = mMaterials["bricks0"].get();
        leftCylRitem->Geo = mGeometries["shapeGeo"].get();
        leftCylRitem->PrimitiveType = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        leftCylRitem->IndexCount = leftCylRitem->Geo->DrawArgs["cylinder"].IndexCount;
        leftCylRitem->StartIndexLocation = leftCylRitem->Geo->DrawArgs["cylinder"].StartIndexLocation;
        leftCylRitem->BaseVertexLocation = leftCylRitem->Geo->DrawArgs["cylinder"].BaseVertexLocation;

        XMStoreFloat4x4(&rightCylRitem->World, leftCylWorld);
        XMStoreFloat4x4(&rightCylRitem->TexTransform, brickTexTransform);
        rightCylRitem->ObjCBIndex = objCBIndex++;
        rightCylRitem->Mat = mMaterials["bricks0"].get();
        rightCylRitem->Geo = mGeometries["shapeGeo"].get();
        rightCylRitem->PrimitiveType = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        rightCylRitem->IndexCount = rightCylRitem->Geo->DrawArgs["cylinder"].IndexCount;
        rightCylRitem->StartIndexLocation = rightCylRitem->Geo->DrawArgs["cylinder"].StartIndexLocation;
        rightCylRitem->BaseVertexLocation = rightCylRitem->Geo->DrawArgs["cylinder"].BaseVertexLocation;

        XMStoreFloat4x4(&leftSphereRitem->World, leftSphereWorld);
        leftSphereRitem->TexTransform = MathHelper::Identity4x4();
        leftSphereRitem->ObjCBIndex = objCBIndex++;
        leftSphereRitem->Mat = mMaterials["mirror0"].get();
        leftSphereRitem->Geo = mGeometries["shapeGeo"].get();
        leftSphereRitem->PrimitiveType = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        leftSphereRitem->IndexCount = leftSphereRitem->Geo->DrawArgs["sphere"].IndexCount;
        leftSphereRitem->StartIndexLocation = leftSphereRitem->Geo->DrawArgs["sphere"].StartIndexLocation;
        leftSphereRitem->BaseVertexLocation = leftSphereRitem->Geo->DrawArgs["sphere"].BaseVertexLocation;

        XMStoreFloat4x4(&rightSphereRitem->World, rightSphereWorld);
        rightSphereRitem->TexTransform = MathHelper::Identity4x4();
        rightSphereRitem->ObjCBIndex = objCBIndex++;
        rightSphereRitem->Mat = mMaterials["mirror0"].get();
        rightSphereRitem->Geo = mGeometries["shapeGeo"].get();
        rightSphereRitem->PrimitiveType = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        rightSphereRitem->IndexCount = rightSphereRitem->Geo->DrawArgs["sphere"].IndexCount;
        rightSphereRitem->StartIndexLocation = rightSphereRitem->Geo->DrawArgs["sphere"].StartIndexLocation;
        rightSphereRitem->BaseVertexLocation = rightSphereRitem->Geo->DrawArgs["sphere"].BaseVertexLocation;

        mRitemLayer[(int)RenderLayer::Opaque].push_back(leftCylRitem.get());
        mRitemLayer[(int)RenderLayer::Opaque].push_back(rightCylRitem.get());
        mRitemLayer[(int)RenderLayer::Opaque].push_back(leftSphereRitem.get());
        mRitemLayer[(int)RenderLayer::Opaque].push_back(rightSphereRitem.get());

        mAllRitems.push_back(std::move(leftCylRitem));
        mAllRitems.push_back(std::move(rightCylRitem));
        mAllRitems.push_back(std::move(leftSphereRitem));
        mAllRitems.push_back(std::move(rightSphereRitem));
    }*/
}

void CRYCHIC::BuildRenderItemsWithShadow()
{
    auto boxRitem = std::make_unique<RenderItem>();
    //XMStoreFloat4x4(&boxRitem->World, XMMatrixScaling(2.0f, 1.0f, 2.0f) * XMMatrixTranslation(0.0f, 0.5f, 0.0f));
    //XMStoreFloat4x4(&boxRitem->TexTransform, XMMatrixScaling(1.0f, 0.5f, 1.0f));
    //boxRitem->ObjCBIndex = 2;
    boxRitem->itemIndex = mItemIndex++;
    boxRitem->Mat = mMaterials["bricks0"].get();
    boxRitem->Geo = mGeometries["shapeGeo"].get();
    boxRitem->PrimitiveType = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    boxRitem->IndexCount = boxRitem->Geo->DrawArgs["box"].IndexCount;
    boxRitem->StartIndexLocation = boxRitem->Geo->DrawArgs["box"].StartIndexLocation;
    boxRitem->BaseVertexLocation = boxRitem->Geo->DrawArgs["box"].BaseVertexLocation;
    boxRitem->Bounds = boxRitem->Geo->DrawArgs["box"].Bounds;

    UINT boxInstanceCount = 1;
    mInstanceCounts.push_back(boxInstanceCount);
    mSceneInstancesCount += boxInstanceCount;
    boxRitem->Instances.resize(boxInstanceCount);
    boxRitem->InstanceCount = boxInstanceCount;
    XMStoreFloat4x4(&boxRitem->Instances[0].World, XMMatrixScaling(2.0f, 1.0f, 2.0f) * XMMatrixTranslation(0.0f, 0.5f, 0.0f));
    XMStoreFloat4x4(&boxRitem->Instances[0].TexTransform, XMMatrixScaling(1.0f, 0.5f, 1.0f));
    boxRitem->Instances[0].MaterialIndex = 0;
    mRitemLayer[(int)RenderLayer::OpaqueShadow].push_back(boxRitem.get());
    mAllRitems.push_back(std::move(boxRitem));

    auto skullRitem = std::make_unique<RenderItem>();
    /*XMStoreFloat4x4(&skullRitem->World, XMMatrixScaling(0.4f, 0.4f, 0.4f) * XMMatrixTranslation(0.0f, 1.0f, 0.0f));
    skullRitem->TexTransform = MathHelper::Identity4x4();
    skullRitem->ObjCBIndex = 3;*/
    skullRitem->itemIndex = mItemIndex++;
    skullRitem->Mat = mMaterials["skullMat"].get();
    skullRitem->Geo = mGeometries["skullGeo"].get();
    skullRitem->PrimitiveType = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    skullRitem->IndexCount = skullRitem->Geo->DrawArgs["skull"].IndexCount;
    skullRitem->StartIndexLocation = skullRitem->Geo->DrawArgs["skull"].StartIndexLocation;
    skullRitem->BaseVertexLocation = skullRitem->Geo->DrawArgs["skull"].BaseVertexLocation;
    skullRitem->Bounds = skullRitem->Geo->DrawArgs["skull"].Bounds;

    UINT skullInstanceCount = 1;
    mInstanceCounts.push_back(skullInstanceCount);
    mSceneInstancesCount += skullInstanceCount;
    skullRitem->Instances.resize(skullInstanceCount);
    skullRitem->InstanceCount = skullInstanceCount;
    XMStoreFloat4x4(&skullRitem->Instances[0].World, XMMatrixScaling(0.4f, 0.4f, 0.4f) * XMMatrixTranslation(0.0f, 1.0f, 0.0f));
    skullRitem->Instances[0].TexTransform = MathHelper::Identity4x4();
    skullRitem->Instances[0].MaterialIndex = 3;
    mRitemLayer[(int)RenderLayer::OpaqueShadow].push_back(skullRitem.get());
    mAllRitems.push_back(std::move(skullRitem));

    auto gridRitem = std::make_unique<RenderItem>();
    //gridRitem->World = MathHelper::Identity4x4();
    //XMStoreFloat4x4(&gridRitem->TexTransform, XMMatrixScaling(8.0f, 8.0f, 1.0f));
    //gridRitem->ObjCBIndex = 4;
    gridRitem->itemIndex = mItemIndex++;
    gridRitem->Mat = mMaterials["tile0"].get();
    gridRitem->Geo = mGeometries["shapeGeo"].get();
    gridRitem->PrimitiveType = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    gridRitem->IndexCount = gridRitem->Geo->DrawArgs["grid"].IndexCount;
    gridRitem->StartIndexLocation = gridRitem->Geo->DrawArgs["grid"].StartIndexLocation;
    gridRitem->BaseVertexLocation = gridRitem->Geo->DrawArgs["grid"].BaseVertexLocation;
    gridRitem->Bounds = gridRitem->Geo->DrawArgs["grid"].Bounds;

    UINT gridInstanceCount = 1;
    mInstanceCounts.push_back(gridInstanceCount);
    mSceneInstancesCount += gridInstanceCount;
    gridRitem->Instances.resize(gridInstanceCount);
    gridRitem->InstanceCount = gridInstanceCount;
    gridRitem->Instances[0].World = MathHelper::Identity4x4();
    XMStoreFloat4x4(&gridRitem->Instances[0].TexTransform, XMMatrixScaling(8.0f, 8.0f, 1.0f));
    gridRitem->Instances[0].MaterialIndex = 1; // tile
    mRitemLayer[(int)RenderLayer::OpaqueShadow].push_back(gridRitem.get());
    mAllRitems.push_back(std::move(gridRitem));

    auto leftCylRitem = std::make_unique<RenderItem>();
    leftCylRitem->itemIndex = mItemIndex++;
    leftCylRitem->Mat = mMaterials["bricks0"].get();
    leftCylRitem->Geo = mGeometries["shapeGeo"].get();
    leftCylRitem->PrimitiveType = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    leftCylRitem->IndexCount = leftCylRitem->Geo->DrawArgs["cylinder"].IndexCount;
    leftCylRitem->StartIndexLocation = leftCylRitem->Geo->DrawArgs["cylinder"].StartIndexLocation;
    leftCylRitem->BaseVertexLocation = leftCylRitem->Geo->DrawArgs["cylinder"].BaseVertexLocation;
    leftCylRitem->Bounds = leftCylRitem->Geo->DrawArgs["cylinder"].Bounds;
    UINT leftCylInstanceCount = 5;
    leftCylRitem->InstanceCount = leftCylInstanceCount;
    mInstanceCounts.push_back(leftCylInstanceCount);
    mSceneInstancesCount += leftCylInstanceCount;
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
    mInstanceCounts.push_back(rightCylInstanceCount);
    mSceneInstancesCount += rightCylInstanceCount;
    rightCylRitem->Instances.resize(rightCylInstanceCount);

    auto leftSphereRitem = std::make_unique<RenderItem>();
    leftSphereRitem->itemIndex = mItemIndex++;
    //leftSphereRitem->TexTransform = MathHelper::Identity4x4();
    //leftSphereRitem->ObjCBIndex = objCBIndex++;
    leftSphereRitem->Mat = mMaterials["mirror0"].get();
    leftSphereRitem->Geo = mGeometries["shapeGeo"].get();
    leftSphereRitem->PrimitiveType = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    leftSphereRitem->IndexCount = leftSphereRitem->Geo->DrawArgs["sphere"].IndexCount;
    leftSphereRitem->StartIndexLocation = leftSphereRitem->Geo->DrawArgs["sphere"].StartIndexLocation;
    leftSphereRitem->BaseVertexLocation = leftSphereRitem->Geo->DrawArgs["sphere"].BaseVertexLocation;
    leftSphereRitem->Bounds = leftSphereRitem->Geo->DrawArgs["sphere"].Bounds;
    UINT leftSphereInstanceCount = 5;
    leftSphereRitem->InstanceCount = leftSphereInstanceCount;
    mInstanceCounts.push_back(leftSphereInstanceCount);
    mSceneInstancesCount += leftSphereInstanceCount;
    leftSphereRitem->Instances.resize(leftSphereInstanceCount);

    auto rightSphereRitem = std::make_unique<RenderItem>();
    //rightSphereRitem->TexTransform = MathHelper::Identity4x4();
    rightSphereRitem->itemIndex = mItemIndex++;
    rightSphereRitem->Mat = mMaterials["mirror0"].get();
    rightSphereRitem->Geo = mGeometries["shapeGeo"].get();
    rightSphereRitem->PrimitiveType = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    rightSphereRitem->IndexCount = rightSphereRitem->Geo->DrawArgs["sphere"].IndexCount;
    rightSphereRitem->StartIndexLocation = rightSphereRitem->Geo->DrawArgs["sphere"].StartIndexLocation;
    rightSphereRitem->BaseVertexLocation = rightSphereRitem->Geo->DrawArgs["sphere"].BaseVertexLocation;
    rightSphereRitem->Bounds = rightSphereRitem->Geo->DrawArgs["sphere"].Bounds;
    UINT rightSphereInstanceCount = 5;
    rightSphereRitem->InstanceCount = rightSphereInstanceCount;
    mInstanceCounts.push_back(rightSphereInstanceCount);
    mSceneInstancesCount += rightSphereInstanceCount;
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
        leftSphereRitem->Instances[i].MaterialIndex = 2; // mirror
        XMStoreFloat4x4(&rightSphereRitem->Instances[i].World, rightSphereWorld);
        XMStoreFloat4x4(&rightSphereRitem->Instances[i].TexTransform, brickTexTransform);
        rightSphereRitem->Instances[i].MaterialIndex = 2; // mirror
    }
    mRitemLayer[(int)RenderLayer::OpaqueShadow].push_back(leftCylRitem.get());
    mRitemLayer[(int)RenderLayer::OpaqueShadow].push_back(rightCylRitem.get());
    mRitemLayer[(int)RenderLayer::OpaqueShadow].push_back(leftSphereRitem.get());
    mRitemLayer[(int)RenderLayer::OpaqueShadow].push_back(rightSphereRitem.get());

    mAllRitems.push_back(std::move(leftCylRitem));
    mAllRitems.push_back(std::move(rightCylRitem));
    mAllRitems.push_back(std::move(leftSphereRitem));
    mAllRitems.push_back(std::move(rightSphereRitem));
}

void CRYCHIC::BuildCascadeShadowRenderItems()
{
    auto skyRitem = std::make_unique<RenderItem>();
    //XMStoreFloat4x4(&skyRitem->World, XMMatrixScaling(5000.0f, 5000.0f, 5000.0f));
    //skyRitem->TexTransform = MathHelper::Identity4x4();
    //skyRitem->ObjCBIndex = 0;
    skyRitem->itemIndex = mItemIndex++;
    skyRitem->Mat = mMaterials["sky"].get();
    skyRitem->Geo = mGeometries["shapeGeo"].get();
    skyRitem->PrimitiveType = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    skyRitem->IndexCount = skyRitem->Geo->DrawArgs["sphere"].IndexCount;
    skyRitem->StartIndexLocation = skyRitem->Geo->DrawArgs["sphere"].StartIndexLocation;
    skyRitem->BaseVertexLocation = skyRitem->Geo->DrawArgs["sphere"].BaseVertexLocation;
    skyRitem->Bounds = skyRitem->Geo->DrawArgs["sphere"].Bounds;
    UINT skyInstanceCount = 1;
    mInstanceCounts.push_back(skyInstanceCount);
    mSceneInstancesCount += skyInstanceCount;
    skyRitem->Instances.resize(skyInstanceCount);
    skyRitem->InstanceCount = skyInstanceCount;
    XMStoreFloat4x4(&skyRitem->Instances[0].World, XMMatrixScaling(5000.0f, 5000.0f, 5000.0f));
    skyRitem->Instances[0].TexTransform = MathHelper::Identity4x4();
    skyRitem->Instances[0].MaterialIndex = 4;
    mRitemLayer[(int)RenderLayer::Sky].push_back(skyRitem.get());
    mAllRitems.push_back(std::move(skyRitem));

    auto quadRitem = std::make_unique<RenderItem>();
    //quadRitem->World = MathHelper::Identity4x4();
    //quadRitem->TexTransform = MathHelper::Identity4x4();
    //quadRitem->ObjCBIndex = 1;
    quadRitem->itemIndex = mItemIndex++;
    quadRitem->Mat = mMaterials["bricks0"].get();
    quadRitem->Geo = mGeometries["shapeGeo"].get();
    quadRitem->PrimitiveType = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    quadRitem->IndexCount = quadRitem->Geo->DrawArgs["quad"].IndexCount;
    quadRitem->StartIndexLocation = quadRitem->Geo->DrawArgs["quad"].StartIndexLocation;
    quadRitem->BaseVertexLocation = quadRitem->Geo->DrawArgs["quad"].BaseVertexLocation;
    quadRitem->Bounds = quadRitem->Geo->DrawArgs["quad"].Bounds;
    UINT quadInstanceCount = 1;
    mInstanceCounts.push_back(quadInstanceCount);
    mSceneInstancesCount += quadInstanceCount;
    quadRitem->Instances.resize(quadInstanceCount);
    quadRitem->InstanceCount = quadInstanceCount;
    quadRitem->Instances[0].World = MathHelper::Identity4x4();
    quadRitem->Instances[0].TexTransform = MathHelper::Identity4x4();
    quadRitem->Instances[0].MaterialIndex = 0;
    mRitemLayer[(int)RenderLayer::Debug].push_back(quadRitem.get());
    mAllRitems.push_back(std::move(quadRitem));

    auto boxRitem = std::make_unique<RenderItem>();
    boxRitem->itemIndex = mItemIndex++;
    //boxRitem->Mat = mMaterials["bricks0"].get();
    boxRitem->Geo = mGeometries["shapeGeo"].get();
    boxRitem->PrimitiveType = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    boxRitem->IndexCount = boxRitem->Geo->DrawArgs["box"].IndexCount;
    boxRitem->StartIndexLocation = boxRitem->Geo->DrawArgs["box"].StartIndexLocation;
    boxRitem->BaseVertexLocation = boxRitem->Geo->DrawArgs["box"].BaseVertexLocation;
    boxRitem->Bounds = boxRitem->Geo->DrawArgs["box"].Bounds;

    UINT boxInstanceCount = 100;
    mInstanceCounts.push_back(boxInstanceCount);
    mSceneInstancesCount += boxInstanceCount;
    boxRitem->Instances.resize(boxInstanceCount);
    boxRitem->InstanceCount = boxInstanceCount;
    
    for (int i = 0; i < 10; i++)
    {
        for (int j = 0; j < 10; j++)
        {
            XMMATRIX world = XMMatrixScaling(1.6f, 1.6f, 1.6f) *
                XMMatrixTranslation((-5 + i) * 5.0f, 0.8f, (-5 + j) * 5.0f);
            XMStoreFloat4x4(&boxRitem->Instances[i * 10 + j].World, world);
            boxRitem->Instances[i * 10 + j].MaterialIndex = i % 2;
        }
    }

    //boxRitem->Instances[0].MaterialIndex = 0;
    mRitemLayer[(int)RenderLayer::Opaque].push_back(boxRitem.get());
    mAllRitems.push_back(std::move(boxRitem));

    auto gridRitem = std::make_unique<RenderItem>();
    //gridRitem->World = MathHelper::Identity4x4();
    //XMStoreFloat4x4(&gridRitem->TexTransform, XMMatrixScaling(8.0f, 8.0f, 1.0f));
    //gridRitem->ObjCBIndex = 4;
    gridRitem->itemIndex = mItemIndex++;
    gridRitem->Mat = mMaterials["skullMat"].get();
    gridRitem->Geo = mGeometries["shapeGeo"].get();
    gridRitem->PrimitiveType = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    gridRitem->IndexCount = gridRitem->Geo->DrawArgs["grid"].IndexCount;
    gridRitem->StartIndexLocation = gridRitem->Geo->DrawArgs["grid"].StartIndexLocation;
    gridRitem->BaseVertexLocation = gridRitem->Geo->DrawArgs["grid"].BaseVertexLocation;
    gridRitem->Bounds = gridRitem->Geo->DrawArgs["grid"].Bounds;

    UINT gridInstanceCount = 1;
    mInstanceCounts.push_back(gridInstanceCount);
    mSceneInstancesCount += gridInstanceCount;
    gridRitem->Instances.resize(gridInstanceCount);
    gridRitem->InstanceCount = gridInstanceCount;
    XMStoreFloat4x4(&gridRitem->Instances[0].World, XMMatrixScaling(3.0f, 3.0f, 3.0f));
    XMStoreFloat4x4(&gridRitem->Instances[0].TexTransform, XMMatrixScaling(1.0f, 1.0f, 1.0f));
    gridRitem->Instances[0].MaterialIndex = 3; // skullMat
    mRitemLayer[(int)RenderLayer::Opaque].push_back(gridRitem.get());
    mAllRitems.push_back(std::move(gridRitem));

    mSceneItemCount = mItemIndex;
}

void CRYCHIC::BuildCascadeShadowRenderItemsWithShadow()
{
    auto boxRitem = std::make_unique<RenderItem>();
    boxRitem->itemIndex = mItemIndex++;
    //boxRitem->Mat = mMaterials["bricks0"].get();
    boxRitem->Geo = mGeometries["shapeGeo"].get();
    boxRitem->PrimitiveType = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    boxRitem->IndexCount = boxRitem->Geo->DrawArgs["box"].IndexCount;
    boxRitem->StartIndexLocation = boxRitem->Geo->DrawArgs["box"].StartIndexLocation;
    boxRitem->BaseVertexLocation = boxRitem->Geo->DrawArgs["box"].BaseVertexLocation;
    boxRitem->Bounds = boxRitem->Geo->DrawArgs["box"].Bounds;

    UINT boxInstanceCount = 100;
    mInstanceCounts.push_back(boxInstanceCount);
    //mSceneInstancesCount += boxInstanceCount;
    boxRitem->Instances.resize(boxInstanceCount);
    boxRitem->InstanceCount = boxInstanceCount;

    for (int i = 0; i < 10; i++)
    {
        for (int j = 0; j < 10; j++)
        {
            XMMATRIX world = XMMatrixScaling(1.6f, 1.6f, 1.6f) *
                XMMatrixTranslation((-5 + i) * 5.0f, 0.8f, (-5 + j) * 5.0f);
            XMStoreFloat4x4(&boxRitem->Instances[i * 10 + j].World, world);
            boxRitem->Instances[i * 10 + j].MaterialIndex = i % 3;
        }
    }

    //boxRitem->Instances[0].MaterialIndex = 0;
    mRitemLayer[(int)RenderLayer::OpaqueShadow].push_back(boxRitem.get());
    mAllRitems.push_back(std::move(boxRitem));

    auto gridRitem = std::make_unique<RenderItem>();
    //gridRitem->World = MathHelper::Identity4x4();
    //XMStoreFloat4x4(&gridRitem->TexTransform, XMMatrixScaling(8.0f, 8.0f, 1.0f));
    //gridRitem->ObjCBIndex = 4;
    gridRitem->itemIndex = mItemIndex++;
    gridRitem->Mat = mMaterials["tile0"].get();
    gridRitem->Geo = mGeometries["shapeGeo"].get();
    gridRitem->PrimitiveType = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    gridRitem->IndexCount = gridRitem->Geo->DrawArgs["grid"].IndexCount;
    gridRitem->StartIndexLocation = gridRitem->Geo->DrawArgs["grid"].StartIndexLocation;
    gridRitem->BaseVertexLocation = gridRitem->Geo->DrawArgs["grid"].BaseVertexLocation;
    gridRitem->Bounds = gridRitem->Geo->DrawArgs["grid"].Bounds;

    UINT gridInstanceCount = 1;
    mInstanceCounts.push_back(gridInstanceCount);
    //mSceneInstancesCount += gridInstanceCount;
    gridRitem->Instances.resize(gridInstanceCount);
    gridRitem->InstanceCount = gridInstanceCount;
    XMStoreFloat4x4(&gridRitem->Instances[0].World, XMMatrixScaling(3.0f, 3.0f, 3.0f));
    XMStoreFloat4x4(&gridRitem->Instances[0].TexTransform, XMMatrixScaling(1.0f, 1.0f, 1.0f));
    gridRitem->Instances[0].MaterialIndex = 1; // tile
    mRitemLayer[(int)RenderLayer::OpaqueShadow].push_back(gridRitem.get());
    mAllRitems.push_back(std::move(gridRitem));
}

void CRYCHIC::DrawRenderItems(ID3D12GraphicsCommandList* cmdList, const std::vector<RenderItem*>& ritems)
{
    /*UINT objCBByteSize = d3dUtil::CalcConstantBufferByteSize(sizeof(ObjectConstants));

    auto objectCB = mCurrFrameResource->ObjectCB->Resource();

    // For each render item...
    for (size_t i = 0; i < ritems.size(); ++i)
    {
        auto ri = ritems[i];

        cmdList->IASetVertexBuffers(0, 1, &ri->Geo->VertexBufferView());
        cmdList->IASetIndexBuffer(&ri->Geo->IndexBufferView());
        cmdList->IASetPrimitiveTopology(ri->PrimitiveType);

        D3D12_GPU_VIRTUAL_ADDRESS objCBAddress = objectCB->GetGPUVirtualAddress() + ri->ObjCBIndex * objCBByteSize;

        cmdList->SetGraphicsRootConstantBufferView(0, objCBAddress);

        cmdList->DrawIndexedInstanced(ri->IndexCount, 1, ri->StartIndexLocation, ri->BaseVertexLocation, 0);
    }*/

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

void CRYCHIC::DrawSceneToShadowMap()
{
    for (size_t i = 0; i < 6; i++)
    {
        mCommandList->RSSetViewports(1, &mShadowMap->Viewport());
        mCommandList->RSSetScissorRects(1, &mShadowMap->ScissorRect());

        // Change to DEPTH_WRITE.
        mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mShadowMap->Resource(i),
            D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_DEPTH_WRITE));

        // Clear the back buffer and depth buffer.
        mCommandList->ClearDepthStencilView(mShadowMap->Dsv(i),
            D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);

        // Specify the buffers we are going to render to.
        mCommandList->OMSetRenderTargets(0, nullptr, false, &mShadowMap->Dsv(i));

        // Bind the pass constant buffer for the shadow map pass.
        UINT passCBByteSize = d3dUtil::CalcConstantBufferByteSize(sizeof(PassConstants));
        auto passCB = mCurrFrameResource->PassCB->Resource();
        D3D12_GPU_VIRTUAL_ADDRESS passCBAddress = passCB->GetGPUVirtualAddress() + (1 + i) * passCBByteSize;
        mCommandList->SetGraphicsRootConstantBufferView(2, passCBAddress);

        mCommandList->SetPipelineState(mPSOs["shadow_opaque"].Get());

        DrawRenderItems(mCommandList.Get(), mRitemLayer[(int)RenderLayer::OpaqueShadow]);

        // Change back to GENERIC_READ so we can read the texture in a shader.
        mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mShadowMap->Resource(i),
            D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_GENERIC_READ));
    }
    
}

void CRYCHIC::DrawNormalsAndDepth()
{
    mCommandList->RSSetViewports(1, &mScreenViewport);
    mCommandList->RSSetScissorRects(1, &mScissorRect);

    auto normalMap = mSsao->NormalMap();
    auto normalMapRtv = mSsao->NormalMapRtv();

    // Change to RENDER_TARGET.
    mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(normalMap,
        D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_RENDER_TARGET));

    // Clear the screen normal map and depth buffer.
    float clearValue[] = { 0.0f, 0.0f, 1.0f, 0.0f };
    mCommandList->ClearRenderTargetView(normalMapRtv, clearValue, 0, nullptr);
    mCommandList->ClearDepthStencilView(DepthStencilView(), D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);

    // Specify the buffers we are going to render to.
    mCommandList->OMSetRenderTargets(1, &normalMapRtv, true, &DepthStencilView());

    // Bind the constant buffer for this pass.
    auto passCB = mCurrFrameResource->PassCB->Resource();
    mCommandList->SetGraphicsRootConstantBufferView(2, passCB->GetGPUVirtualAddress());

    mCommandList->SetPipelineState(mPSOs["drawNormals"].Get());

    DrawRenderItems(mCommandList.Get(), mRitemLayer[(int)RenderLayer::Opaque]);

    // Change back to GENERIC_READ so we can read the texture in a shader.
    mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(normalMap,
        D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_GENERIC_READ));
}

void CRYCHIC::DrawGBuffer()
{
    mCommandList->RSSetViewports(1, &mScreenViewport);
    mCommandList->RSSetScissorRects(1, &mScissorRect);
    mCommandList->SetPipelineState(mPSOs["geometryPass"].Get());
    for (size_t i = 0; i < 4; i++)
    {
        mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mDeferred->Resource(i),
            D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_RENDER_TARGET));
        mCommandList->ClearRenderTargetView(mDeferred->Rtv(i), Colors::Black, 0, nullptr);
    }
    mCommandList->ClearDepthStencilView(DepthStencilView(), D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);
    // Specify the buffers we are going to render to.
    D3D12_CPU_DESCRIPTOR_HANDLE deferredRtvs[4];
    for (size_t i = 0; i < 4; i++)
    {
        deferredRtvs[i] = mDeferred->Rtv(i);
    }
    mCommandList->OMSetRenderTargets(4, deferredRtvs, false, &DepthStencilView());
    DrawRenderItems(mCommandList.Get(), mRitemLayer[(int)RenderLayer::Opaque]);
    //DrawRenderItems(mCommandList.Get(), mRitemLayer[(int)RenderLayer::Sky]);
    for (size_t i = 0; i < 4; i++)
    {
        mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mDeferred->Resource(i),
            D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_GENERIC_READ));
    }
}

CD3DX12_CPU_DESCRIPTOR_HANDLE CRYCHIC::GetCpuSrv(int index) const
{
    auto srv = CD3DX12_CPU_DESCRIPTOR_HANDLE(mSrvDescriptorHeap->GetCPUDescriptorHandleForHeapStart());
    srv.Offset(index, mCbvSrvUavDescriptorSize);
    return srv;
}

CD3DX12_GPU_DESCRIPTOR_HANDLE CRYCHIC::GetGpuSrv(int index) const
{
    auto srv = CD3DX12_GPU_DESCRIPTOR_HANDLE(mSrvDescriptorHeap->GetGPUDescriptorHandleForHeapStart());
    srv.Offset(index, mCbvSrvUavDescriptorSize);
    return srv;
}

CD3DX12_CPU_DESCRIPTOR_HANDLE CRYCHIC::GetDsv(int index) const
{
    auto dsv = CD3DX12_CPU_DESCRIPTOR_HANDLE(mDsvHeap->GetCPUDescriptorHandleForHeapStart());
    dsv.Offset(index, mDsvDescriptorSize);
    return dsv;
}

CD3DX12_CPU_DESCRIPTOR_HANDLE CRYCHIC::GetRtv(int index) const
{
    auto rtv = CD3DX12_CPU_DESCRIPTOR_HANDLE(mRtvHeap->GetCPUDescriptorHandleForHeapStart());
    rtv.Offset(index, mRtvDescriptorSize);
    return rtv;
}

std::array<const CD3DX12_STATIC_SAMPLER_DESC, 7> CRYCHIC::GetStaticSamplers()
{
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
        shadow
    };
}
