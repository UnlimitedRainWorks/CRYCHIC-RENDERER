#include "Common.hlsl"

struct VertexIn
{
	float3 PosL : POSITION;
	float2 TexC : TEXCOORD;
};

struct VertexOut
{
	float4 PosH : SV_POSITION;
	float2 TexC : TEXCOORD;
	nointerpolation uint MatIndex : MATINDEX;
};

VertexOut shadowDebugVS(VertexIn vin, uint instanceID : SV_INSTANCEID)
{
	VertexOut vout;
	//InstanceData instanceData = gInstanceData[instanceID];
	//uint materialIndex = instanceData.MaterialIndex;
	//float4x4 gWorld = instanceData.World;
	//float4 posW = mul(float4(vin.PosL, 1.0f), gWorld);
	//vout.PosH = mul(posW, gViewProj);
	vout.PosH = float4(vin.PosL, 1.0f);
	vout.TexC = vin.TexC;
	return vout;
}

float4 shadowDebugPS(VertexOut pin) : SV_Target
{
	//return float4(gShadowMap[0].Sample(gsamLinearWrap, pin.TexC).rrr, 1.0f);
	return 1.0f;
}