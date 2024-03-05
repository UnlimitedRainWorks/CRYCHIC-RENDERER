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

// ����GBuffer�����ӳ���Ⱦ
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
    // ������ɫ���е�PosH��ʱ���ڲü��ռ䣬������Ӳ����������Ļ�ռ�
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
    //float3 fresnelR0 = lerp(0.04, albedo, metalness);
    Material1 mat;
    mat.DiffuseAlbedo = albedo;
    mat.Roughness = roughness;
    mat.Metalness = metalness;
    //= { albedo, roughness, metalness};
    float3 shadowFactor = 1.0f;
    float4 directLight = PBRShading(gLights, mat, normalW, toEyeW, posW);
    float4 litColor = ambient + directLight;
    litColor.a = 1.0;
    return litColor;
}