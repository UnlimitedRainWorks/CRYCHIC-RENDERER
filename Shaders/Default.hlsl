//***************************************************************************************
// Default.hlsl by Frank Luna (C) 2015 All Rights Reserved.
//***************************************************************************************

#include "Common.hlsl"
struct VertexIn
{
	float3 PosL    : POSITION;
    float3 NormalL : NORMAL;
	float2 TexC    : TEXCOORD;
    float3 TangentL : TANGENT;
};

struct VertexOut
{
	float4 PosH    : SV_POSITION;
    float3 PosW    : POSITION;
    float3 NormalW : NORMAL;
	float2 TexC    : TEXCOORD;
    float3 TangentW : TANGENT;
    // 该索引指向的都是未经插值的三角形
    nointerpolation uint MatIndex : MATINDEX;
};

VertexOut VS(VertexIn vin, uint instanceID : SV_InstanceID)
{
	VertexOut vout = (VertexOut)0.0f;

    // Get instanceData.
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

    vout.TangentW = mul(float4(vin.TangentL, 1.0), gWorld).xyz;
    return vout;
}

float4 PS(VertexOut pin) : SV_Target
{
	// Fetch the material data.
	MaterialData matData = gMaterialData[pin.MatIndex];
	float4 diffuseAlbedo = matData.DiffuseAlbedo;
	float3 fresnelR0 = matData.FresnelR0;
	float  roughness = matData.Roughness;
    float  metalness = matData.Metalness;
	uint diffuseTexIndex = matData.DiffuseMapIndex;
    int normalTexIndex = matData.NormalMapIndex;

	// Dynamically look up the texture in the array.
	diffuseAlbedo *= gDiffuseMap[diffuseTexIndex].Sample(gsamLinearWrap, pin.TexC);
	
    // Interpolating normal can unnormalize it, so renormalize it.
    pin.NormalW = normalize(pin.NormalW);
    
    float3 normalT2W = pin.NormalW; 
    float4 normalSample = float4(pin.NormalW, 1.0f);
    if(normalTexIndex > 0)
    {
        normalSample = gDiffuseMap[normalTexIndex].Sample(gsamAnisotropicWrap, pin.TexC);
        float3 normalT = normalSample.xyz;
        normalT2W = EncodeNormalTangentSpace2World(normalT, pin.NormalW, pin.TangentW); 
    }

    // Vector from point being lit to eye. 
    float3 toEyeW = normalize(gEyePosW - pin.PosW);

    // vertex pos
    float3 pos = pin.PosW;

    // Light terms.
    float4 ambient = gAmbientLight * diffuseAlbedo;

    //const float shininess = 1.0f - roughness;
    Material mat = { diffuseAlbedo, roughness, metalness, fresnelR0, 1.0f - roughness};
    float3 shadowFactor = 1.0f;
    //float4 directLight = ComputeLighting(gLights, mat, pin.PosW,
    //    normalT2W, toEyeW, shadowFactor);

    // 使用了normalmap解码到世界空间出来的法线
    float4 directLight = PBRShading(gLights, mat, normalT2W, toEyeW, pos);
    float4 litColor = ambient + directLight;

    // Add in specular reflections.
    float3 r = reflect(-toEyeW, normalT2W);
    float4 reflectionColor = gCubeMap.Sample(gsamLinearWrap, r);
    float3 f0 = lerp(0.04, diffuseAlbedo.xyz, metalness);
    float nov = max(dot(normalT2W, toEyeW), 0.001f);
    float shininess = normalSample.a * (1 - roughness);
    //float3 FresnelFactor = fresnelR0 + (1.0f - fresnelR0) * pow(1 - nov, 5);
    float3 FresnelFactor = f0 + (1.0f - f0) * pow(1 - nov, 5);
    //litColor.rgb = 0.0f;
    litColor.rgb += shininess * FresnelFactor * reflectionColor;

    // Common convention to take alpha from diffuse albedo.
    litColor.a = diffuseAlbedo.a;
    //return reflectionColor;
    return litColor;
    //return litColor;
    //return float4(normalT2W, 1.0);
}


