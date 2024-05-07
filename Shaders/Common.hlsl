//***************************************************************************************
// Common.hlsl by Frank Luna (C) 2015 All Rights Reserved.
//***************************************************************************************

// Defaults for number of lights.
#ifndef NUM_DIR_LIGHTS
    #define NUM_DIR_LIGHTS 1
#endif

#ifndef NUM_POINT_LIGHTS
    #define NUM_POINT_LIGHTS 0
#endif

#ifndef NUM_SPOT_LIGHTS
    #define NUM_SPOT_LIGHTS 0
#endif

// Include structures and functions for lighting.
#include "PBR.hlsl"
#include "GBuffer.hlsl"
#define N_SAMPLE 16

struct InstanceData
{
    float4x4 World;
    float4x4 TexTransform;
    uint MaterialIndex;
    uint InstPad0;
    uint InstPad1;
    uint InstPad2;
};

struct MaterialData
{
	float4   DiffuseAlbedo;
	float3   FresnelR0;
	float    Roughness;
	float4x4 MatTransform;
	uint     DiffuseMapIndex;
	uint     NormalMapIndex;
	float    Metalness;
	uint     MatPad2;
};

TextureCube gCubeMap : register(t0);
Texture2D gShadowMap[12] : register(t1);
Texture2D gSsaoMap[5]   : register(t13);
Texture2D gBuffer[4] : register(t18);
// An array of textures, which is only supported in shader model 5.1+. 
// Unlike Texture2DArray, the textures in this array can be different sizes and formats, 
// making it more flexible than texture arrays.
Texture2D gTextureMaps[10] : register(t22);


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

// GPU instancing does not need this.
// Constant data that varies per frame.
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
    float4x4 gViewProjTex;
    float4x4 gShadowTransforms[12];
    float3 gEyePosW;
    float cbPerObjectPad1;
    float2 gRenderTargetSize;
    float2 gInvRenderTargetSize;
    float gNearZ;
    float gFarZ;
    float gTotalTime;
    float gDeltaTime;
    float4 gAmbientLight;

    // Indices [0, NUM_DIR_LIGHTS) are directional lights;
    // indices [NUM_DIR_LIGHTS, NUM_DIR_LIGHTS+NUM_POINT_LIGHTS) are point lights;
    // indices [NUM_DIR_LIGHTS+NUM_POINT_LIGHTS, NUM_DIR_LIGHTS+NUM_POINT_LIGHT+NUM_SPOT_LIGHTS)
    // are spot lights for a maximum of MaxLights per object.
    Light gLights[MaxLights];
};

//---------------------------------------------------------------------------------------
// Transforms a normal map sample to world space.
//---------------------------------------------------------------------------------------
float3 NormalSampleToWorldSpace(float3 normalMapSample, float3 unitNormalW, float3 tangentW)
{
	// Uncompress each component from [0,1] to [-1,1].
	float3 normalT = 2.0f*normalMapSample - 1.0f;

	// Build orthonormal basis.
	float3 N = unitNormalW;
	float3 T = normalize(tangentW - dot(tangentW, N)*N);
	float3 B = cross(N, T);

	float3x3 TBN = float3x3(T, B, N);

	// Transform from tangent space to world space.
	float3 bumpedNormalW = mul(normalT, TBN);

	return bumpedNormalW;
}

//---------------------------------------------------------------------------------------
// PCF for shadow mapping.
//---------------------------------------------------------------------------------------
//#define SMAP_SIZE = (2048.0f)
//#define SMAP_DX = (1.0f / SMAP_SIZE)
float CalcShadowFactor(float4 shadowPosH)
{
    // Complete projection by doing division by w.
    shadowPosH.xyz /= shadowPosH.w;

    // Depth in NDC space.
    float depth = shadowPosH.z;

    uint width, height, numMips;
    gShadowMap[0].GetDimensions(0, width, height, numMips);

    // Texel size.
    float dx = 1.0f / (float)width;

    float percentLit = 0.0f;
    const float2 offsets[9] =
    {
        float2(-dx,  -dx), float2(0.0f,  -dx), float2(dx,  -dx),
        float2(-dx, 0.0f), float2(0.0f, 0.0f), float2(dx, 0.0f),
        float2(-dx,  +dx), float2(0.0f,  +dx), float2(dx,  +dx)
    };

    [unroll]
    for(int i = 0; i < 9; ++i)
    {
        percentLit += gShadowMap[0].SampleCmpLevelZero(gsamShadow,
            shadowPosH.xy + offsets[i], depth).r;
    }
    
    return percentLit / 9.0f;
}

float nrand(in float2 uv)
{
    float2 noise = (frac(sin(dot(uv, float2(12.9898, 78.233) * 2.0)) * 43758.5453));
    return abs(noise.x + noise.y) * 0.5;
}

static float2 poissonDisk[16] =
{
    float2(-0.94201624, -0.39906216), float2(0.94558609, -0.76890725),
    float2(-0.094184101, -0.92938870), float2(0.34495938, 0.29387760),
    float2(-0.91588581, 0.45771432), float2(-0.81544232, -0.87912464),
    float2(-0.38277543, 0.27676845), float2(0.97484398, 0.75648379),
    float2(0.44323325, -0.97511554), float2(0.53742981, -0.47373420),
    float2(-0.26496911, -0.41893023), float2(0.79197514, 0.19090188),
    float2(-0.24188840, 0.99706507), float2(-0.81409955, 0.91437590),
    float2(0.19984126, 0.78641367), float2(0.14383161, -0.14100790)
};

float CalcCascadeShadowFactor3X3(uint index, float4 shadowPosH)
{
    // Complete projection by doing division by w.
    shadowPosH.xyz /= shadowPosH.w;

    // Depth in NDC space.
    float depth = shadowPosH.z;

    uint width, height, numMips;
    gShadowMap[index].GetDimensions(0, width, height, numMips);

    // Texel size.
    float dx = 1.0f / (float) width;

    float percentLit = 0.0f;
    const float2 offsets[9] =
    {
        float2(-dx, -dx), float2(0.0f, -dx), float2(dx, -dx),
        float2(-dx, 0.0f), float2(0.0f, 0.0f), float2(dx, 0.0f),
        float2(-dx, +dx), float2(0.0f, +dx), float2(dx, +dx)
    };
    [unroll]
    for (int i = 0; i < 9; ++i)
    {
        percentLit += gShadowMap[index].SampleCmpLevelZero(gsamShadow,
            shadowPosH.xy + offsets[i], depth).r;
    }
    return percentLit / 9.0f;
}

float CalcCascadeShadowFactor5X5(uint index, float4 shadowPosH)
{
    // Complete projection by doing division by w.
    shadowPosH.xyz /= shadowPosH.w;

    // Depth in NDC space.
    float depth = shadowPosH.z;

    uint width, height, numMips;
    gShadowMap[index].GetDimensions(0, width, height, numMips);

    // Texel size.
    float dx = 1.0f / (float)width;

    float percentLit = 0.0f;
    //const float2 offsets[9] =
    //{
    //    float2(-dx,  -dx), float2(0.0f,  -dx), float2(dx,  -dx),
    //    float2(-dx, 0.0f), float2(0.0f, 0.0f), float2(dx, 0.0f),
    //    float2(-dx,  +dx), float2(0.0f,  +dx), float2(dx,  +dx)
    //};
    //[unroll]
    //for(int i = 0; i < 9; ++i)
    //{
    //    percentLit += gShadowMap[index].SampleCmpLevelZero(gsamShadow,
    //        shadowPosH.xy + offsets[i], depth).r;
    //}
    //return percentLit / 9.0f;

    const float2 offsets_25[25] =
    {
        float2(-2*dx,  -2*dx), float2(-dx,  -2*dx), float2(0.0f,  -2*dx), float2(dx,  -2*dx), float2(2*dx,  -2*dx),
        float2(-2*dx, -dx),   float2(-dx, -dx),   float2(0.0f, -dx),      float2(dx, -dx),    float2(2*dx,  -dx),
        float2(-2*dx,  0.0f), float2(-dx,  0.0f), float2(0.0f,  0.0f),    float2(dx, 0.0f),   float2(2*dx,  0.0f),
        float2(-2*dx,  dx),   float2(-dx,  dx),   float2(0.0f,  +dx),     float2(dx, dx),     float2(2*dx,  dx),
        float2(-2*dx,  2*dx),  float2(-dx,  2*dx),  float2(0.0f,  2*dx),  float2(dx, 2*dx),   float2(2*dx,  2*dx),
    };
    
    [unroll]
    for (int i = 0; i < 25; ++i)
    {
        percentLit += gShadowMap[index].SampleCmpLevelZero(gsamShadow,
            shadowPosH.xy + offsets_25[i], depth).r;
    }
    return percentLit / 25.0f;

}

float CalcCascadeShadowFactorWithPoisson(uint index, float4 shadowPosH)
{
    // Complete projection by doing division by w.
    shadowPosH.xyz /= shadowPosH.w;

    // Depth in NDC space.
    float depth = shadowPosH.z;

    uint width, height, numMips;
    gShadowMap[index].GetDimensions(0, width, height, numMips);

    // Texel size.
    float dx = 1.0f / (float) width;

    float percentLit = 0.0f;
    //const float2 offsets[9] =
    //{
    //    float2(-dx,  -dx), float2(0.0f,  -dx), float2(dx,  -dx),
    //    float2(-dx, 0.0f), float2(0.0f, 0.0f), float2(dx, 0.0f),
    //    float2(-dx,  +dx), float2(0.0f,  +dx), float2(dx,  +dx)
    //};
    //[unroll]
    //for(int i = 0; i < 9; ++i)
    //{
    //    percentLit += gShadowMap[index].SampleCmpLevelZero(gsamShadow,
    //        shadowPosH.xy + offsets[i], depth).r;
    //}
    //return percentLit / 9.0f;

    const float2 offsets_25[25] =
    {
        float2(-2 * dx, -2 * dx), float2(-dx, -2 * dx), float2(0.0f, -2 * dx), float2(dx, -2 * dx), float2(2 * dx, -2 * dx),
        float2(-2 * dx, -dx), float2(-dx, -dx), float2(0.0f, -dx), float2(dx, -dx), float2(2 * dx, -dx),
        float2(-2 * dx, 0.0f), float2(-dx, 0.0f), float2(0.0f, 0.0f), float2(dx, 0.0f), float2(2 * dx, 0.0f),
        float2(-2 * dx, dx), float2(-dx, dx), float2(0.0f, +dx), float2(dx, dx), float2(2 * dx, dx),
        float2(-2 * dx, 2 * dx), float2(-dx, 2 * dx), float2(0.0f, 2 * dx), float2(dx, 2 * dx), float2(2 * dx, 2 * dx),
    };
    
    float rotation_theta = nrand(shadowPosH.xy);
    float cos_theta = cos(rotation_theta);
    float sin_theta = sin(rotation_theta);
    float2x2 rotation_matrix = float2x2(cos_theta, sin_theta, -sin_theta, cos_theta);
    float search_radius = 5 / width / 2.0f;
    
    [unroll]
    for (int i = 0; i < N_SAMPLE; ++i)
    {
        float2 p = mul(poissonDisk[i], rotation_matrix);
        float2 offset = float2(p * search_radius);
        percentLit += gShadowMap[index].SampleCmpLevelZero(gsamShadow,
            shadowPosH.xy + offset, depth).r;
    }
    return percentLit / N_SAMPLE;

}