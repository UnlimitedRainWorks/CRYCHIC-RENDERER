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

VertexOut shadowVS(VertexIn vin, uint instanceID : SV_InstanceID)
{
	VertexOut vout = (VertexOut)0.0f;
	InstanceData instanceData = gInstanceData[instanceID];
	float4x4 gWorld = instanceData.World;
	float4x4 gTexTransform = instanceData.TexTransform;
	uint gMaterialIndex = instanceData.MaterialIndex;
	vout.MatIndex = gMaterialIndex;

	MaterialData matData = gMaterialData[gMaterialIndex];

	float4 posW = mul(float4(vin.PosL, 1.0f), gWorld);
	vout.PosH = mul(posW, gViewProj);
	float4 texC = mul(float4(vin.TexC, 0.0f, 1.0f), gTexTransform);
	vout.TexC = mul(texC, matData.MatTransform).xy;
	return vout;
}

void shadowPS(VertexOut pin)
{
	MaterialData matData = gMaterialData[pin.MatIndex];
	float4 diffuseAlbedo = matData.DiffuseAlbedo;
	uint diffuseMapIndex = matData.DiffuseMapIndex;
	diffuseAlbedo *= gDiffuseMap[diffuseMapIndex].Sample(gsamAnisotropicWrap, pin.TexC);

#ifdef ALPHA_TEST
	clip(diffuseAlbedo.a - 0.1f);
#endif
}