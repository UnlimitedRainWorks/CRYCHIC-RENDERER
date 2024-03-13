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
    float3 TangentL : TANGENT;
};

struct VertexOutGBuffer
{
	float4 PosH    : SV_POSITION;
    float3 PosW    : POSITION;
    float3 NormalW : NORMAL;
	float2 TexC    : TEXCOORD0;
    float3 TangentW : TANGENT;
    // 该索引指向的都是未经插值的三角形
    nointerpolation uint MatIndex : MATINDEX;
};

// GBuffer
VertexOutGBuffer GBufferVS(VertexInGBuffer vin, uint instanceID : SV_InstanceID)
{
    VertexOutGBuffer vout = (VertexOutGBuffer)0.0f;

    // Fetch the instance data.
    InstanceData instanceData = gInstanceData[instanceID];
    float4x4 gWorld = instanceData.World;
    float4x4 gTexTransform = instanceData.TexTransform;
    uint gMaterialIndex = instanceData.MaterialIndex;

    vout.MatIndex = gMaterialIndex;

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

    vout.TangentW = mul(float4(vin.TangentL, 1.0f), gWorld).xyz;
    return vout;
}

GBuffer GBufferPS(VertexOutGBuffer pin)
{
    //GBuffer EncodePBRToGBuffer(float3 pos,float metalness, 
						   //float3 albedo, float3 normal, float roughness)
    float3 posW = pin.PosW;
    float3 normalW = normalize(pin.NormalW);
   
    // Fetch the material data.
	MaterialData matData = gMaterialData[pin.MatIndex];
	float4 diffuseAlbedo = matData.DiffuseAlbedo;
	float3 fresnelR0 = matData.FresnelR0;
	float  roughness = matData.Roughness;
    float  metalness = matData.Metalness;
	uint diffuseTexIndex = matData.DiffuseMapIndex;
    int normalMapIndex = matData.NormalMapIndex;

    float3 normalT2W = normalW;
    float4 normalSample = float4(normalW, 1.0f);
    if (normalMapIndex > 0)
    {
        normalSample = gDiffuseMap[normalMapIndex].Sample(gsamAnisotropicWrap, pin.TexC);
        float3 normalT = normalSample.xyz;
        normalT2W = EncodeNormalTangentSpace2World(normalT, normalW, pin.TangentW);
    }
	// Dynamically look up the texture in the array.
	diffuseAlbedo *= gDiffuseMap[diffuseTexIndex].Sample(gsamLinearWrap, pin.TexC);

    return EncodePBRToGBuffer(posW, metalness, diffuseAlbedo.xyz, float4(normalT2W, normalSample.w), roughness);
}
