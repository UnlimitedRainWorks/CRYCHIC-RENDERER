#include "Common.hlsl"

struct VertexIn
{
	float3 PosL : POSITION;
	float3 NormalL : NORMAL;
	float2 TexC : TEXCOORD;
};

struct VertexOut
{
	float4 PosH : SV_POSITION;
	float3 PosL : POSITION;
};

VertexOut VS(VertexIn vin, uint instanceID : SV_InstanceID)
{
	VertexOut vout;
	vout.PosL = vin.PosL;
	InstanceData instanceData = gInstanceData[instanceID];
	float4x4 gWorld = instanceData.World;
	float4 posW = mul(float4(vin.PosL, 1.0f), gWorld);
	// Always center sky about camera.
	posW.xyz += gEyePosW;
	// Set z = w so that z/w = 1 (i.e., skydome always on far plane).
	vout.PosH = mul(posW, gViewProj).xyww;
	return vout;
}

float4 PS(VertexOut pin) : SV_Target
{
	return gCubeMap.Sample(gsamLinearWrap, pin.PosL);
}