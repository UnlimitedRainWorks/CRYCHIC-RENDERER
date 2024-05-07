#include "LightingUtil.hlsl"
#define pi 3.1415926

float NDF_GGX(float3 normal, float3 halfVec, float a)
{
	float a2 = a * a;
	float nDoth = max(dot(normal, halfVec), 0.001f);
	float nDoth2 = nDoth * nDoth;

	float top = a2;
	float tmp = pow(nDoth2 * (a2 - 1) + 1, 2);
	float bottom = pi * tmp;
	return top * rcp(bottom);
}

float GeometrySchlickGGX(float nDotvec, float k)
{
	float top = nDotvec;
	float bottom = nDotvec * (1 - k) + k;
	return top / bottom;
}

float GeometrySchlickGGX1(float nDotvec, float k)
{
	float bottom = nDotvec * (1 - k) + k;
	return rcp(bottom);
}

float GeometrySmith(PBRDesc pbrDesc)
{
	float nDotv = pbrDesc.nDotv;
	float nDotl = pbrDesc.nDotl;
	float roughness = pbrDesc.roughness;
	float k = 0.125 * (roughness + 1) * (roughness + 1); 
	float ggx1 = GeometrySchlickGGX(nDotv, k);
	float ggx2 = GeometrySchlickGGX(nDotl, k);
	return ggx1 * ggx2;
}

float3 FresnelSchlick(float hDotv, float3 f0)
{
	return f0 + (1.0f - f0) * pow(clamp(1.0f - hDotv, 0.0f, 1.0f), 5.0f);
}

float3 GetBRDF(PBRDesc pbrDesc)
{
	float3 normal = pbrDesc.normal;
	float3 pos = pbrDesc.pos;
	float3 halfVec = pbrDesc.halfVec;
	float3 lightDir = pbrDesc.lightDir;
	float3 view = pbrDesc.view;
	float3 diffuseAlbedo = pbrDesc.diffuseAlbedo;
	float roughness = pbrDesc.roughness;
	float metalness = pbrDesc.metalness;
	float3 f0 = lerp(0.04, diffuseAlbedo, metalness);
	float hDotv = pbrDesc.hDotv;
	float nDotl = pbrDesc.nDotl;
	float nDotv = pbrDesc.hDotv;
	
	float D = NDF_GGX(normal, halfVec, roughness);
	float3 F = FresnelSchlick(nDotv, f0);
	float G = GeometrySmith(pbrDesc);
	float3 fs = 0.25 * D * G * F;
	fs /= (nDotl * nDotv);
	float3 fd = diffuseAlbedo * rcp(pi);
	float3 ks = F;
	float3 kd = (1.0f - F) * (1 - metalness);
	float3 brdf = kd * fd + ks * fs;
	return brdf;
}

PBRDesc GetPBRDesc(Material mat, float3 normal, float3 view, float3 lightDir, float3 pos)
{
	PBRDesc pbrDesc;
	pbrDesc.normal = normal;
	pbrDesc.pos = pos;
	pbrDesc.view = view;
	float3 halfVec = normalize(view + lightDir);
	pbrDesc.halfVec = halfVec;
	pbrDesc.lightDir = lightDir;
	pbrDesc.roughness = mat.Roughness;
	pbrDesc.metalness = mat.Metalness;
	pbrDesc.diffuseAlbedo = mat.DiffuseAlbedo.xyz;
	pbrDesc.hDotv = max(dot(halfVec, view), 0.001f);
	pbrDesc.nDotl = max(dot(normal, lightDir), 0.001f);
	pbrDesc.nDotv = max(dot(normal, view), 0.001f);
	return pbrDesc;
}


float4 PBRShading(Light gLights[MaxLights], Material mat, float3 normal, float3 v, float3 pos,
					float3 shadowFactor[MaxLights])
{
	float3 result = 0.0f;
	int i = 0;

#if (NUM_DIR_LIGHTS > 0)
	
	for(i = 0; i < NUM_DIR_LIGHTS; ++i)
    {
		PBRDesc pbrDesc = GetPBRDesc(mat, normal, v, -gLights[i].Direction, pos);
		float3 brdf = GetBRDF(pbrDesc);
		float nDotl = pbrDesc.nDotl;
		float3 irradiance = gLights[i].Strength * nDotl;
		result += pow(shadowFactor[i], 5.0f) * brdf * irradiance;
	}
#endif
	
#if (NUM_POINT_LIGHTS > 0)
	for(i = NUM_DIR_LIGHTS; i < NUM_DIR_LIGHTS + NUM_POINT_LIGHTS; ++i)
	{
		float3 l = gLights[i].Position - pos;
		float d = length(l);
		l /= d;
		PBRDesc pbrDesc = GetPBRDesc(mat, normal, v, l, pos);
		float3 brdf = GetBRDF(pbrDesc);
		float nDotl = pbr.nDotl;
		float3 lightStrength = gLights[i].Strength * nDotl;
		float att = CalcAttenuation(d, gLights[i].FalloffStart, gLights[i].FalloffEnd);
		lightStrength *= att;

		//result += shadowFactor[i] * brdf * lightStrength;
	}
#endif

#if (NUM_SPOT_LIGHTS > 0)
    for(i = NUM_DIR_LIGHTS + NUM_POINT_LIGHTS; i < NUM_DIR_LIGHTS + NUM_POINT_LIGHTS + NUM_SPOT_LIGHTS; ++i)
	{
		float3 spotDirection = -gLights[i].Direction;
		float3 l = gLights[i].Position - pos;
		float d = length(l);
		l /= d;
		if (d > gLights[i].FalloffEnd)
		{
			result += 0.0f;
			continue;
		}
		PBRDesc pbrDesc = GetPBRDesc(mat, normal, v, l, pos);
		float3 brdf = GetBRDF(pbrDesc);
		float nDotl = pbrDesc.nDotl;
		float3 lightStrength = gLights[i].Strength * nDotl;
		float att = CalcAttenuation(d, gLights[i].FalloffStart, gLights[i].FalloffEnd);
		att *= pow(max(dot(spotDirection, l), 0.001f), gLights[i].SpotPower);
		lightStrength *= att;
		//result += shadowFactor[i] * brdf * lightStrength;
	}
#endif
	return float4(result, 0.0f);
}