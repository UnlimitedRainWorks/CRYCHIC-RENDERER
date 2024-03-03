//***************************************************************************************
// Default.hlsl by Frank Luna (C) 2015 All Rights Reserved.
//***************************************************************************************

// Defaults for number of lights.
#ifndef NUM_DIR_LIGHTS
    #define NUM_DIR_LIGHTS 3
#endif

#ifndef NUM_POINT_LIGHTS
    #define NUM_POINT_LIGHTS 0
#endif

#ifndef NUM_SPOT_LIGHTS
    #define NUM_SPOT_LIGHTS 0
#endif

#include "Common.hlsl"

struct VertexInDeferred
{
    float3 PosL    : POSITION;
};

struct VertexOutDeferred
{
	float4 PosH : SV_POSITION;
};

// 根据GBuffer进行延迟渲染
VertexOutDeferred DeferredVS(VertexInDeferred vin, uint instanceID : SV_InstanceID)
{
    VertexOutDeferred vout;
    InstanceData instanceData = gInstanceData[instanceID];
    float4x4 gWorld = instanceData.World;
    float4 posW = mul(float4(vin.PosL, 1.0), gWorld);
    vout.PosH = mul(posW, gViewProj);
    return vout;
}

float4 DeferredPS(VertexOutDeferred pin) : SV_Target
{
    // 像素着色器中的PosH此时不在裁剪空间，而是在硬件处理后的屏幕空间
    float2 screenUV = pin.PosH.xy * gInvRenderTargetSize;
    float4 gBuffer0 = gBuffer[0].Sample(gsamAnisotropicWrap, screenUV);
    float4 gBuffer1 = gBuffer[1].Sample(gsamAnisotropicWrap, screenUV);
    float4 gBuffer2 = gBuffer[2].Sample(gsamAnisotropicWrap, screenUV);
    float4 gBuffer3 = gBuffer[3].Sample(gsamAnisotropicWrap, screenUV);
    PBRDesc pbrDesc = DecodeGBuffer(gBuffer0, gBuffer1, gBuffer2, gBuffer3);
    float3 posW = pbrDesc.pos;
    float metalness = pbrDesc.metalness;
    float3 albedo = pbrDesc.albedo;
    float roughness = pbrDesc.roughness;
    float3 normalW = pbrDesc.normal;
    float3 toEyeW = normalize(gEyePosW - posW);
    float4 ambient = gAmbientLight * float4(albedo, 1.0);
    const float shininess = 1.0f - roughness;
    float3 fresnelR0 = lerp(0.04, albedo, metalness);
    Material mat = { float4(albedo, 1.0), fresnelR0, shininess};
    float3 shadowFactor = 1.0f;
    float4 directLight = 0.0f;
    directLight = PBRShading(gLights, mat, normalW, toEyeW);
    float4  litColor = ambient + directLight;
    litColor.a = 1.0;
    return litColor;
}