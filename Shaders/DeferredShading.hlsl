#include "Common.hlsl"

struct VertexIn
{
	float3 PosL : POSITION;
};

struct VertexOut
{
	float4 PosH : SV_POSITION;
};

VertexOut VS(VertexIn vin, uint instanceID : SV_InstanceID)
{
	VertexOut vout;
	InstanceData instanceData = gInstanceData[instanceID];
	float4x4 gWorld = instanceData.World;
	float4 posW = mul(float4(vin.PosL, 1.0f), gWorld);
	vout.PosH = mul(posW, gViewProj);
	return vout;
}

float4 PS(VertexOut pin) : SV_Target
{
	float2 screenUV = pin.PosH.xy * gInvRenderTargetSize;
	float4 gBuffer0 = gBuffer[0].Sample(gsamAnisotropicWrap, screenUV);
	float4 gBuffer1 = gBuffer[1].Sample(gsamAnisotropicWrap, screenUV);
	float4 gBuffer2 = gBuffer[2].Sample(gsamAnisotropicWrap, screenUV);
	float4 gBuffer3 = gBuffer[3].Sample(gsamAnisotropicWrap, screenUV);
	GBufferDesc gBufferDesc = DecodeGBuffer(gBuffer0, gBuffer1, gBuffer2, gBuffer3);
	float3 posW = gBufferDesc.pos;
	float3 view = normalize(gEyePosW - posW);
	float metalness = gBufferDesc.metalness;
	float4 diffuseAlbedo = float4(gBufferDesc.albedo, 1.0f);
	float3 fresnelR0 = lerp(0.04, diffuseAlbedo.xyz, metalness);

	float4 normalW = float4(gBufferDesc.normal, 1.0f);
	float roughness = gBufferDesc.roughness;

	float4 ssaoPosH = mul(float4(posW, 1.0f), gViewProjTex);
	ssaoPosH /= ssaoPosH.w;
	float ambientAccess = gSsaoMap[0].Sample(gsamLinearClamp, ssaoPosH.xy, 0.0f).r;

	float4 ambient = ambientAccess * gAmbientLight * diffuseAlbedo;

	float3 shadowFactors[MaxLights];
	[unroll]
	for (int i = 0; i < MaxLights; i++)
	{
		shadowFactors[i] = 1.0f;
	}

	float radius[4] = { 30.0f, 50.0f, 80.0f, 100.0f };
	int j = 0;
	for (j = 0; j < 4; j++)
	{
		float distance = length(gEyePosW - posW);
		float shadowFactor, shadowFactorNextLevel;
		float4 shadowPosH, shadowPosHNextLevel;
		if (j < 3 && distance < radius[j] && abs(distance - radius[j] < 5.0f))
		{
			shadowPosH = mul(float4(posW, 1.0f), gShadowTransforms[j]);
			shadowPosHNextLevel = mul(float4(posW, 1.0f), gShadowTransforms[j + 1]);
			shadowFactor = CalcCascadeShadowFactorWithPoisson(j, shadowPosH);
			shadowFactorNextLevel = CalcCascadeShadowFactorWithPoisson(j + 1, shadowPosHNextLevel);
			shadowFactors[0] = 0.5f * (shadowFactor + shadowFactorNextLevel);
			break;
		}
		else if(distance < radius[j])
		{
			shadowPosH = mul(float4(posW, 1.0f), gShadowTransforms[j]);
			shadowFactor = CalcCascadeShadowFactorWithPoisson(j, shadowPosH);
			shadowFactors[0] = shadowFactor;
			break;
		}
	}

	 // Area DEBUG
    //if(j == 0)return float4(1.0f, 0.0f, 0.0f, 1.0f);
    //if(j == 1)return float4(0.0f, 1.0f, 0.0f, 1.0f);
    //if(j == 2)return float4(0.0f, 0.0f, 1.0f, 1.0f);
    //if(j == 3)return float4(1.0f, 1.0f, 1.0f, 1.0f);

	const float shininess = (1.0f - roughness) * normalW.a;
	Material mat = { diffuseAlbedo, fresnelR0, roughness, metalness, shininess };

	// PBR
	float4 directLight = PBRShading(gLights, mat, normalW.xyz, view, posW, shadowFactors);
	directLight /= (directLight + 1.0f);
	directLight = pow(directLight, 1.0f / 2.2f);

	float4 litColor = directLight + ambient;
	//return litColor;
	float3 r = reflect(-view, normalW.xyz);
	float4 reflectionColor = gCubeMap.Sample(gsamLinearWrap, r);
	float3 fresnelFactor = SchlickFresnel(fresnelR0, normalW.xyz, r);
	litColor.rgb += shininess * fresnelFactor * reflectionColor.xyz;

	litColor.a = diffuseAlbedo.a;
	return litColor;
}