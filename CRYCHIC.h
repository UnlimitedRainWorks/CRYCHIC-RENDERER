#pragma once
#include "Common/d3dApp.h"
#include "Common/MathHelper.h"
#include "Common/UploadBuffer.h"
#include "Common/GeometryGenerator.h"
#include "Common/Camera.h"
#include "FrameResource.h"
#include "DeferredRenderTarget.h"
#include "CubeRenderTarget.h"

using Microsoft::WRL::ComPtr;
using namespace DirectX;
using namespace DirectX::PackedVector;

#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "D3D12.lib")

const int gNumFrameResources = 3;

const UINT CubeMapSize = 512;
// Lightweight structure stores parameters to draw a shape.  This will
// vary from app-to-app.
struct RenderItem
{
	RenderItem() = default;
	RenderItem(const RenderItem& rhs) = delete;
	/*{
		World = rhs.World;
		TexTransform = rhs.TexTransform;
		Mat = rhs.Mat;
		Geo = rhs.Geo;
		IndexCount = rhs.IndexCount;
		StartIndexLocation = rhs.StartIndexLocation;
		BaseVertexLocation = rhs.BaseVertexLocation;
		InstanceCount = rhs.InstanceCount;
		Bounds = rhs.Bounds;
		itemIndex = rhs.itemIndex;
		Instances.resize(rhs.Instances.size());
		for (int i = 0; i < rhs.Instances.size(); i++)
		{
			Instances[i] = rhs.Instances[i];
		}
	}*/

	// World matrix of the shape that describes the object's local space
	// relative to the world space, which defines the position, orientation,
	// and scale of the object in the world.
	XMFLOAT4X4 World = MathHelper::Identity4x4();

	XMFLOAT4X4 TexTransform = MathHelper::Identity4x4();

	// Dirty flag indicating the object data has changed and we need to update the constant buffer.
	// Because we have an object cbuffer for each FrameResource, we have to apply the
	// update to each FrameResource.  Thus, when we modify obect data we should set 
	// NumFramesDirty = gNumFrameResources so that each frame resource gets the update.
	int NumFramesDirty = gNumFrameResources;

	// Index into GPU constant buffer corresponding to the ObjectCB for this render item.
	//UINT ObjCBIndex = -1;

	Material* Mat = nullptr;
	MeshGeometry* Geo = nullptr;

	// Primitive topology.
	D3D12_PRIMITIVE_TOPOLOGY PrimitiveType = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;

	// DrawIndexedInstanced parameters.
	UINT IndexCount = 0;
	UINT StartIndexLocation = 0;
	int BaseVertexLocation = 0;
	UINT InstanceCount = 0;
	std::vector<InstanceData> Instances;
	BoundingBox Bounds;
	UINT itemIndex = 0;
};

enum class RenderLayer : int
{
	Opaque = 0,
	OpaqueDynamicReflectors,
	Sky,
	OpaqueDynamicCamera,
	SkyDynamicCamera,
	Count
};

class CRYCHIC : public D3DApp 
{
public:
	CRYCHIC(HINSTANCE hInstance);
	CRYCHIC(const CRYCHIC& rhs) = delete;
	CRYCHIC& operator=(const CRYCHIC& rhs) = delete;
	~CRYCHIC();

	virtual bool Initialize()override;

private:
	virtual void CreateRtvAndDsvDescriptorHeaps()override;
	virtual void OnResize()override;
	virtual void Update(const GameTimer& gt)override;
	virtual void Draw(const GameTimer& gt)override;

	virtual void OnMouseDown(WPARAM btnState, int x, int y)override;
	virtual void OnMouseUp(WPARAM btnState, int x, int y)override;
	virtual void OnMouseMove(WPARAM btnState, int x, int y)override;

	void OnKeyboardInput(const GameTimer& gt);
	void AnimateMaterials(const GameTimer& gt);
	//void UpdateObjectCBs(const GameTimer& gt);
	void UpdateInstanceData(const GameTimer& gt);
	//void UpdateInstanceData(const Camera& camera, std::vector<std::unique_ptr<RenderItem>>& ri);
	void UpdateMaterialBuffer(const GameTimer& gt);
	void UpdateMainPassCB(const GameTimer& gt);
	void UpdateCubeMapFacePassCBs();
	void BuildCubeFaceCamera(float x, float y, float z);
	void LoadTextures();
	void BuildRootSignature();
	void BuildDescriptorHeaps();
	void BuildShadersAndInputLayout();
	void BuildShapeGeometry();
	void BuildGeometry(std::string fileName);
	void BuildPSOs();
	void BuildFrameResources();
	void BuildMaterials();
	void BuildRenderItems();
	void BuildCubeMapRenderItems();
	//void BuildInstancingSceneRenderItems();
	void DrawRenderItems(ID3D12GraphicsCommandList* cmdList, const std::vector<RenderItem*>& ritems);
	void DrawGBuffer();
	void DrawSceneToCubeMap();
	void BuildCubeDepthStencil();
	std::array<const CD3DX12_STATIC_SAMPLER_DESC, 6> GetStaticSamplers();

private:

	// �Ƿ����ӳ���Ⱦ
	bool isDeferred = false;

	UINT mTexSize = 0;
	UINT mGBufferSize = 4;
	UINT mCubemapRTSize = 6;
	// cubemap num
	UINT mCubemapSize = 0;
	UINT mDynamicCubemapSize = 1;
	CD3DX12_CPU_DESCRIPTOR_HANDLE mCubeDSV;
	std::unique_ptr<CubeRenderTarget> mDynamicCubeMap = nullptr;

	std::vector<std::unique_ptr<FrameResource>> mFrameResources;
	FrameResource* mCurrFrameResource = nullptr;
	int mCurrFrameResourceIndex = 0;

	UINT mCbvSrvDescriptorSize = 0;

	ComPtr<ID3D12RootSignature> mRootSignature = nullptr;

	ComPtr<ID3D12DescriptorHeap> mSrvDescriptorHeap = nullptr;
	ComPtr<ID3D12DescriptorHeap> gBufferSrvDescriptorHeap = nullptr;

	ComPtr<ID3D12Resource> mCubeDepthStencilBuffer;

	std::unordered_map<std::string, std::unique_ptr<MeshGeometry>> mGeometries;
	std::unordered_map<std::string, std::unique_ptr<Material>> mMaterials;
	std::unordered_map<std::string, std::unique_ptr<Texture>> mTextures;
	std::unordered_map<std::string, ComPtr<ID3DBlob>> mShaders;
	std::unordered_map<std::string, ComPtr<ID3D12PipelineState>> mPSOs;

	std::vector<D3D12_INPUT_ELEMENT_DESC> mInputLayout;

	// List of all the render items.
	std::vector<std::unique_ptr<RenderItem>> mAllRitems;
	std::vector<std::unique_ptr<RenderItem>> mCubeMapAllRitems;

	// Render items divided by PSO.
	std::vector<RenderItem*> mRitemLayer[(int)RenderLayer::Count];

	//UINT mInstanceCount = 0;
	UINT mCubeMapTexHeapIndex = 0;
	UINT mDynamicTexHeapIndex = 0;
	UINT gBufferHeapIndex = 0;
	std::unique_ptr<DeferredRenderTarget> mDeferred = nullptr;

	BoundingFrustum mCamFrustum;

	bool mFrustumCullingEnabled = true;

	PassConstants mMainPassCB;

	Camera mCamera;
	Camera mCubeMapCamera[6];

	POINT mLastMousePos;


	XMFLOAT3 mEyePos = { 0.0f, 0.0f, 0.0f };
	float mTheta = 1.5f * XM_PI;
	float mPhi = 0.2f * XM_PI;
	float mRadius = 15.0f;

	std::vector<std::string> texNames;
	std::vector<std::string> cubeMapNames;
	std::vector<int> mInstancesCount;
	UINT mItemIndex = 0;
	UINT mSceneItemCount = 0;
};