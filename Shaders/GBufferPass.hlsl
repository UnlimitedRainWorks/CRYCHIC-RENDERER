//=============================================================================
// Sky.fx by Frank Luna (C) 2011 All Rights Reserved.
//=============================================================================

// Include common HLSL code.
#include "Common.hlsl"

struct VertexInGBuffer
{
	float3 PosL    : POSITION;
    float3 NormalL : NORMAL;
	float2 TexC    : TEXCOORD0;
};

struct VertexOutGBuffer
{
	float4 PosH    : SV_POSITION;
    float3 PosW    : POSITION;
    float3 NormalW : NORMAL;
	float2 TexC    : TEXCOORD0;
};

// GBuffer
VertexOutGBuffer GBufferVS(VertexInGBuffer vin)
{
    VertexOutGBuffer vout;
    // Fetch the material data.
	MaterialData matData = gMaterialData[gMaterialIndex];
	
    // Transform to world space.
    float4 posW = mul(float4(vin.PosL, 1.0f), gWorld);
    vout.PosW = posW.xyz;

    // Assumes nonuniform scaling; otherwise, need to use inverse-transpose of world matrix.
    vout.NormalW = mul(vin.NormalL, (float3x3)gWorld);

    // Transform to homogeneous clip space.
    vout.PosH = mul(posW, gViewProj);
	
	// Output vertex attributes for interpolation across triangle.
	float4 texC = mul(float4(vin.TexC, 0.0f, 1.0f), gTexTransform);
	vout.TexC = mul(texC, matData.MatTransform).xy;

    return vout;
}

GBuffer GBufferPS(VertexOutGBuffer pin)
{
    //GBuffer EncodePBRToGBuffer(float3 pos,float metalness, 
						   //float3 albedo, float3 normal, float roughness)
    float3 posW = pin.PosW;
    float3 normalW = normalize(pin.NormalW);
    float metalness = 0.3f;
    
    // Fetch the material data.
	MaterialData matData = gMaterialData[gMaterialIndex];
	float4 diffuseAlbedo = matData.DiffuseAlbedo;
	float3 fresnelR0 = matData.FresnelR0;
	float  roughness = matData.Roughness;
	uint diffuseTexIndex = matData.DiffuseMapIndex;

	// Dynamically look up the texture in the array.
	diffuseAlbedo *= gDiffuseMap[diffuseTexIndex].Sample(gsamLinearWrap, pin.TexC);

    return EncodePBRToGBuffer(posW, metalness, diffuseAlbedo.xyz, normalW, roughness);
}

//GBuffer GBufferPS(VertexOutGBuffer pin)
//{
//    //GBuffer EncodePBRToGBuffer(float3 pos,float metalness, 
//						   //float3 albedo, float3 normal, float roughness)
//    GBuffer gout;
//    gout.GBuffer0 = float4(1.0f, 1.0f, 1.0f, 1.0f);
//    gout.GBuffer1 = float4(1.0f, 1.0f, 1.0f, 1.0f);
//    gout.GBuffer2 = float4(1.0f, 1.0f, 1.0f, 1.0f);
//    gout.GBuffer3 = float4(1.0f, 1.0f, 1.0f, 1.0f);
//    return gout;
//}