//***************************************************************************************
// Default.hlsl by Frank Luna (C) 2015 All Rights Reserved.
//***************************************************************************************

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
    float4 ambient = gAmbientLight * float4(albedo, 1.0f);
    //float3 fresnelR0 = lerp(0.04, albedo, metalness);
    Material mat;
    mat.DiffuseAlbedo = float4(albedo, 1.0f);
    mat.Roughness = roughness;
    mat.Metalness = metalness;
    //= { albedo, roughness, metalness};
    float3 shadowFactor = 1.0f;
    float4 directLight = PBRShading(gLights, mat, normalW, toEyeW, posW);
    float4 litColor = ambient + directLight;

    // Add in specular reflections.
    float3 r = reflect(-toEyeW, normalW);
    float4 reflectionColor = gCubeMap.Sample(gsamLinearWrap, r);
    float3 f0 = lerp(0.04, albedo, metalness);
    float nov = max(dot(normalW, toEyeW), 0.001f);
    float3 FresnelFactor = f0 + (1 - f0) * pow(1 - nov, 5);
    litColor.xyz += FresnelFactor * reflectionColor.xyz;

    litColor.a = 1.0;
    return litColor;
}