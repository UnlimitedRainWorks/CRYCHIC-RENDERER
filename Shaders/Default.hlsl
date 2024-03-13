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
    float4 PosW    : POSITION;
    //float4 ShadowPosH : POSITION1;
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
    vout.PosW = posW;

    // Assumes nonuniform scaling; otherwise, need to use inverse-transpose of world matrix.
    vout.NormalW = mul(vin.NormalL, (float3x3)gWorld);

    // Transform to homogeneous clip space.
    vout.PosH = mul(posW, gViewProj);
	
	// Output vertex attributes for interpolation across triangle.
	float4 texC = mul(float4(vin.TexC, 0.0f, 1.0f), gTexTransform);
	vout.TexC = mul(texC, matData.MatTransform).xy;

    vout.TangentW = mul(float4(vin.TangentL, 1.0), gWorld).xyz;

    //vout.ShadowPosH = mul(posW, gShadowTransform[0]);

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
	
#ifdef ALPHA_TEST
    clip(diffuseAlbedo.a - 0.1f);
#endif

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
    float3 toEyeW = normalize(gEyePosW - pin.PosW.xyz);

    // vertex pos
    float3 pos = pin.PosW.xyz;

    // Light terms.
    float4 ambient = gAmbientLight * diffuseAlbedo;

    float shininess = normalSample.a * (1 - roughness);

    Material mat = { diffuseAlbedo, roughness, metalness, fresnelR0, shininess};
    
    float3 shadowFactor[3];
    for(int i = 0; i < 3; i++)
    {
        float4 shadowPosH = mul(pin.PosW, gShadowTransform[i]);
        shadowFactor[i] = CalcShadowFactor(i, shadowPosH);
    }
    
    //float3 shadowFactor[NUM_DIR_LIGHTS + NUM_POINT_LIGHTS + NUM_SPOT_LIGHTS]; 
    //for(int i = 0; i < NUM_DIR_LIGHTS + NUM_POINT_LIGHTS + NUM_SPOT_LIGHTS; i++ )
    //{
    //    float4 shadowPosH = mul(float4(pin.PosW, 1.0f), gShadowTransforms[i]);
    //    shadowFactor[i] = CalcShadowFactor(i, shadowPosH);
    //}

    // Blinning Phong
    //float4 directLight = ComputeLighting(gLights, mat, pin.PosW,
    //    normalT2W, toEyeW, shadowFactor);

    // 使用了normalmap解码到世界空间出来的法线
    float4 directLight = PBRShading(gLights, mat, normalT2W, toEyeW, pos, shadowFactor);
    float4 litColor = ambient + directLight;

    // Add in specular reflections.
    float3 r = reflect(-toEyeW, normalT2W);
    float4 reflectionColor = gCubeMap.Sample(gsamLinearWrap, r);
    float3 f0 = lerp(0.04, diffuseAlbedo.xyz, metalness);
    float nov = max(dot(normalT2W, toEyeW), 0.001f);
    float3 FresnelFactor = f0 + (1.0f - f0) * pow(1 - nov, 5);
    
    litColor.rgb += shininess * FresnelFactor * reflectionColor.xyz;
    litColor.a = diffuseAlbedo.a;
    //return reflectionColor;
    return litColor;
    //return gShadowTransform[0][3];
    //return float4(shadowFactor[0], 1.0f);
    //return directLight;
}


