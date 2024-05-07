#include "Common.hlsl"

struct VertexIn
{
	float3 PosL     : POSITION;
	float3 NormalL  : NORMAL;
	float2 TexC     : TEXCOORD0;
	float3 TangentU : TANGENT;
};

struct VertexOut
{
	float4 PosH     : SV_POSITION;
	//float4 SsaoPosH : POSITION1;
	float3 PosW     : POSITION1;
	float3 NormalW  : NORMAL;
	float3 TangentW : TANGENT;
	float2 TexC     : TEXCOORD;
	nointerpolation uint MatIndex : MATINDEX;
};

VertexOut VS(VertexIn vin, uint instanceID : SV_InstanceID)
{
	VertexOut vout = (VertexOut)0.0f;
	InstanceData instanceData = gInstanceData[instanceID];
	float4x4 gWorld = instanceData.World;
	float4x4 gTexTransform = instanceData.TexTransform;
	uint gMaterialIndex = instanceData.MaterialIndex;

	vout.MatIndex = gMaterialIndex;
	MaterialData matData = gMaterialData[gMaterialIndex];
	float4 posW = mul(float4(vin.PosL, 1.0f), gWorld);
	vout.PosW = posW.xyz;
	vout.PosH = mul(posW, gViewProj);
	// 若gWorld是非等比变换，使用gWorld的逆转置矩阵
	vout.NormalW = mul(vin.NormalL, (float3x3)gWorld);
	vout.TangentW = mul(vin.TangentU, (float3x3)gWorld);
	//vout.SsaoPosH = mul(posW, gViewProjTex);
	float4 texC = mul(float4(vin.TexC, 0.0f, 1.0f), gTexTransform);
	vout.TexC = mul(texC, matData.MatTransform).xy;
	return vout;
}

GBuffer PS(VertexOut pin)
{
	MaterialData matData = gMaterialData[pin.MatIndex];
	float4 diffuseAlbedo = matData.DiffuseAlbedo;
	//float3 fresnelR0 = matData.FresnelR0;
	float roughness = matData.Roughness;
	float  metalness = matData.Metalness;
	uint diffuseMapIndex = matData.DiffuseMapIndex;
	uint normalMapIndex = matData.NormalMapIndex;
	diffuseAlbedo *= gTextureMaps[diffuseMapIndex].Sample(gsamAnisotropicWrap, pin.TexC);

#ifdef ALPHA_TEST
	clip(diffuseAlbedo.a - 0.1f);
#endif
	pin.NormalW = normalize(pin.NormalW);

	float4 normalMapSample = gTextureMaps[normalMapIndex].Sample(gsamAnisotropicWrap, pin.TexC);
	float3 bumpedNormalW = NormalSampleToWorldSpace(normalMapSample.rgb, pin.NormalW, pin.TangentW);

	//pin.SsaoPosH /= pin.SsaoPosH.w;
	//float ambientAccess = gSsaoMap[0].Sample(gsamAnisotropicWrap, pin.SsaoPosH.xy, 0.0f).r;
	return EncodePBRToGBuffer(pin.PosW, metalness, diffuseAlbedo.rgb, roughness, bumpedNormalW);
}
