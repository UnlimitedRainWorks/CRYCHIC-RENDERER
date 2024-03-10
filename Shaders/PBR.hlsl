//***************************************************************************************
// LightingUtil.hlsl by Frank Luna (C) 2015 All Rights Reserved.
//
// Contains API for shader lighting.
//***************************************************************************************

#include "LightingUtil.hlsl"
#define pi 3.1415926

//---------------------------------------------------------------------------------------
// NDF
//---------------------------------------------------------------------------------------
float D_GGX_TR(float3 normal, float3 h, float roughness)
{
    float r2 = roughness * roughness;
    float noh = max(dot(normal, h), 0.01);
    float noh2 = noh * noh;
    float temp = noh2 * (r2 - 1) + 1;
    float bottom = pi * temp * temp;
    return r2 * rcp(bottom);
}

//---------------------------------------------------------------------------------------
// Geometry  Kdirect = (r + 1) * (r + 1) / 8;
//---------------------------------------------------------------------------------------
float GeometrySchlickGGX(float vecDotN, float k)
{
    float bottom = vecDotN * (1 - k) + k;
    return vecDotN * rcp(bottom);
}

float GeometrySmith(float3 normal, float3 v, float3 l, float k)
{
    float nov = max(dot(normal, v), 0.01);
    float nol = max(dot(normal, l), 0.01);
    return GeometrySchlickGGX(nov, k) * GeometrySchlickGGX(nol, k);
}

//---------------------------------------------------------------------------------------
// Fresnel
//---------------------------------------------------------------------------------------
float3 Fresnel(float3 normal, float3 v, float3 f0)
{
    float nov = max(dot(normal, v), 0.01f);
    return f0 + (1 - f0) * pow(1 - nov, 5);
}

//---------------------------------------------------------------------------------------
// Diffuse and Specular BRDF
//---------------------------------------------------------------------------------------

float3 GetBRDF(float3 normal, float3 h, float3 v, float3 l, float3 diffuseAlbedo, float roughness, float metalness)
{
    float3 f0 = lerp(0.04, diffuseAlbedo, metalness);
    float D = D_GGX_TR(normal, h, roughness);
    float3 F = Fresnel(normal, v, f0);

    // Kdirect = (roughness + 1) * (roughness + 1) / 8
    // KIBL = roughness * roughness / 2;
    float Kdirect = (roughness + 1) * (roughness + 1) / 8;
    float G = GeometrySmith(normal, v, l, Kdirect);
    float nov = max(dot(normal, v), 0.01);
    float nol = max(dot(normal, l), 0.01);
    float3 top = D * F * G;
    float bottom = 4 * nov * nol;
    float3 fcook_torrance = top * rcp(bottom);
    // 直接光照中反射部分所占比率
    float3 ks = F;
    float3 flambert = diffuseAlbedo * rcp(pi);
    // 金属部分没有漫反射
    float3 kd = (1 - F) * (1 - metalness);
    float3 BRDF = kd * flambert + ks * fcook_torrance;
    return BRDF;
}

//---------------------------------------------------------------------------------------
// PBRShading
//---------------------------------------------------------------------------------------

float4 PBRShading(Light gLights[MaxLights], Material mat,
                  float3 normal, float3 v, float3 pos)
{
    float3 result = 0.0f;

    int i = 0;

#if (NUM_DIR_LIGHTS > 0)
    for(i = 0; i < NUM_DIR_LIGHTS; ++i)
    {
        float3 diffuseAlbedo = mat.DiffuseAlbedo.xyz;
        float roughness = pow(mat.Roughness, 2.0);
        //float roughness = mat.Roughness;
        float metalness = mat.Metalness;

        // Directional Lights' directions should be reversed.
        float3 l = -gLights[i].Direction;
        float3 h = normalize(l + v);
        float3 BRDF = GetBRDF(normal, h, v, l, diffuseAlbedo, roughness, metalness);
        float shadowFactor = 1.0f;
        float nol = max(dot(normal, l), 0.001);
        // 如果是选取光强最强的DirectLight作为该点在半球领域内接受的所有光，那么这里应该是irradiance
        float3 irradiance = gLights[i].Strength * nol;
        result += shadowFactor * BRDF * irradiance;
    }
#endif

#if (NUM_POINT_LIGHTS > 0)
    for(i = NUM_DIR_LIGHTS; i < NUM_DIR_LIGHTS + NUM_POINT_LIGHTS; ++i)
    {
        float3 diffuseAlbedo = mat.DiffuseAlbedo.xyz;
        float roughness = pow(mat.Roughness, 2.0);
        //float roughness = mat.Roughness;
        float metalness = mat.Metalness;

        // Point Lights' directions should be ToEyeW(v).
        float3 l = gLights[i].Position - pos;
        float d = length(l);
        
        // lightVec normalize
        l /= d;
        float3 h = normalize(l + v);
        float3 BRDF = GetBRDF(normal, h, v, l, diffuseAlbedo, roughness, metalness);
        float shadowFactor = 1.0f;
        float nol = max(dot(normal, l), 0.001);
        
        float3 lightStrength = gLights[i].Strength * nol;

        // Attenuate light by distance.
        float att = CalcAttenuation(d, gLights[i].FalloffStart, gLights[i].FalloffEnd);
        lightStrength *= att;

        result += shadowFactor * BRDF * lightStrength;
    }
#endif

#if (NUM_SPOT_LIGHTS > 0)
    for(i = NUM_DIR_LIGHTS + NUM_POINT_LIGHTS; i < NUM_DIR_LIGHTS + NUM_POINT_LIGHTS + NUM_SPOT_LIGHTS; ++i)
    {
        float3 diffuseAlbedo = mat.DiffuseAlbedo.xyz;
        float roughness = pow(mat.Roughness, 2.0);
        //float roughness = mat.Roughness;
        float metalness = mat.Metalness;

        // Spot Lights' directions should be reversed.
        float3 spotDirection = -gLights[i].Direction;
        float3 l = gLights[i].Position - pos;
        float d = length(l);

        // lightVec normalize
        l /= d;
        if(d > gLights[i].FalloffEnd) 
        {
            result += 0.0f;
            continue;
        }
        float3 h = normalize(l + v);
        float3 BRDF = GetBRDF(normal, h, v, l, diffuseAlbedo, roughness, metalness);
        float shadowFactor = 1.0f;
        float nol = max(dot(normal, l), 0.001);
        float3 lightStrength = gLights[i].Strength * nol;
        // Attenuate light by distance.
        float att = CalcAttenuation(d, gLights[i].FalloffStart, gLights[i].FalloffEnd);
        att *= pow(max(dot(-l, gLights[i].Direction), 0.001f), gLights[i].SpotPower);
        lightStrength *= att;

        result += shadowFactor * BRDF * lightStrength;
    }
#endif

    return float4(result, 0.0f);
}