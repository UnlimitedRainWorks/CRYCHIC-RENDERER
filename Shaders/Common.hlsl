// Defaults for number of lights.
#ifndef NUM_DIR_LIGHTS
    #define NUM_DIR_LIGHTS 1
#endif

#ifndef NUM_POINT_LIGHTS
    #define NUM_POINT_LIGHTS 0
#endif

#ifndef NUM_SPOT_LIGHTS
    #define NUM_SPOT_LIGHTS 2
#endif

// Include structures and functions for lighting.
#include "PBR.hlsl"
#include "GBuffer.hlsl"

struct MaterialData
{
	float4   DiffuseAlbedo;
	float3   FresnelR0;
	float    Roughness;
    float    Metalness;
	float4x4 MatTransform;
	uint     DiffuseMapIndex;
    uint     NormalMapIndex;
	uint     MatPad0;
	uint     MatPad1;
	uint     MatPad2;
};

struct InstanceData{
    float4x4 World;
    float4x4 TexTransform;
    uint MaterialIndex;
    uint InstPad0;
    uint InstPad1;
    uint InstPad2;
};

// An array of textures, which is only supported in shader model 5.1+.  Unlike Texture2DArray, the textures
// in this array can be different sizes and formats, making it more flexible than texture arrays.
Texture2D gDiffuseMap[12] : register(t0);
Texture2D gShadowMap[3] : register(t12);
TextureCube gCubeMap : register(t15);
Texture2D gBuffer[4] : register(t16);

// Put in space1, so the texture array does not overlap with these resources.  
// The texture array will occupy registers t0, t1, ..., t3 in space0. 
StructuredBuffer<InstanceData> gInstanceData : register(t0, space1);
StructuredBuffer<MaterialData> gMaterialData : register(t1, space1);


SamplerState gsamPointWrap        : register(s0);
SamplerState gsamPointClamp       : register(s1);
SamplerState gsamLinearWrap       : register(s2);
SamplerState gsamLinearClamp      : register(s3);
SamplerState gsamAnisotropicWrap  : register(s4);
SamplerState gsamAnisotropicClamp : register(s5);
SamplerComparisonState gsamShadow : register(s6);

// Constant data that varies per frame.
// 实例化就不需要给每个对象单独的常量缓冲区了
//cbuffer cbPerObject : register(b0)
//{
//    float4x4 gWorld;
//	float4x4 gTexTransform;
//	uint gMaterialIndex;
//	uint gObjPad0;
//	uint gObjPad1;
//	uint gObjPad2;
//};

// Constant data that varies per material.
cbuffer cbPass : register(b0)
{
    float4x4 gView;
    float4x4 gInvView;
    float4x4 gProj;
    float4x4 gInvProj;
    float4x4 gViewProj;
    float4x4 gInvViewProj;
    float3 gEyePosW;
    float cbPerObjectPad1;
    float2 gRenderTargetSize;
    float2 gInvRenderTargetSize;
    float gNearZ;
    float gFarZ;
    float gTotalTime;
    float gDeltaTime;
    float4 gAmbientLight;
    Light gLights[MaxLights]; 
    float4x4 gShadowTransform[MaxLights];//s[1 + NUM_POINT_LIGHTS + NUM_SPOT_LIGHTS];
};

float3 EncodeNormalTangentSpace2World(float3 sampleNormal, float3 unitNormalW, float3 tangentW)
{
    float3 normalT = 2.0f * sampleNormal - 1.0f;
    float3 N = unitNormalW;
    float3 T = normalize(tangentW - dot(N, tangentW) * N);
    float3 B = cross(T, N);
    float3x3 TBN = float3x3(T, B, N);
    return mul(normalT, TBN);
}

// PCF
float CalcShadowFactor(uint index, float4 shadowPosH)
{
    shadowPosH.xyz /= shadowPosH.w;
    float depth = shadowPosH.z;
    uint width, height, numMips;
    gShadowMap[index].GetDimensions(0, width, height, numMips);

    float dx = 1.0f / (float)width;
    float percentLit = 0.0f;
    const float2 offsets[9] =
    {
        float2(-dx, -dx), float2(0.0f, -dx), float2(dx, -dx),
        float2(-dx, 0.0f), float2(0.0f, 0.0f), float2(dx, 0.0f),
        float2(-dx, dx), float2(0.0f, dx), float2(dx, dx),
    };

    [unroll]
    for(int i = 0; i < 9; i++)
    {
        percentLit += gShadowMap[index].SampleCmpLevelZero(gsamShadow,
            shadowPosH.xy + offsets[i], depth).r;
    }
    return percentLit / 9.0f;
}