#include "FrameResource.h"

FrameResource::FrameResource(ID3D12Device* device, UINT passCount, 
	std::vector<int>& InstanceCounts, UINT itemCount, 
	UINT materialCount)
{
	ThrowIfFailed(device->CreateCommandAllocator(
		D3D12_COMMAND_LIST_TYPE_DIRECT,
		IID_PPV_ARGS(CmdListAlloc.GetAddressOf())));

	PassCB = std::make_unique<UploadBuffer<PassConstants>>(device, passCount, true);
	SsaoCB = std::make_unique<UploadBuffer<SsaoConstants>>(device, 1, true);
	MaterialBuffer = std::make_unique<UploadBuffer<MaterialData>>(device, materialCount, false);
	InstanceBuffers.resize(itemCount);
	for (size_t i = 0; i < itemCount; i++)
	{
		InstanceBuffers[i] = std::make_unique<UploadBuffer<InstanceData>>(device, InstanceCounts[i], false);
	}
	
}
