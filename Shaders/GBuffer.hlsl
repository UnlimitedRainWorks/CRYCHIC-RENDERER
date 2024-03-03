//=============================================================================
// Sky.fx by Frank Luna (C) 2011 All Rights Reserved.
//=============================================================================

// Include common HLSL code.

struct GBuffer
{
	float4 GBuffer0 : SV_TARGET0;
	float4 GBuffer1 : SV_TARGET1;
	float4 GBuffer2 : SV_TARGET2;
	float4 GBuffer3 : SV_TARGET3;
};

struct PBRDesc
{
    float3 pos;
    float metalness;
    float3 albedo;
    float roughness;
    float3 normal;
};

//---------------------------------------------------------------------------------------
// Transfer PBR information to GBuffer
//---------------------------------------------------------------------------------------
GBuffer EncodePBRToGBuffer(float3 pos,float metalness, 
						   float3 albedo, float3 normal, float roughness)
{
	GBuffer gout;
	gout.GBuffer0 = float4(pos, metalness);
	gout.GBuffer1 = float4(albedo, roughness);
	gout.GBuffer2 = float4(normal, 1.0);
	gout.GBuffer3 = 0.0f;
	return gout;
}

PBRDesc DecodeGBuffer(float4 gBuffer0, float4 gBuffer1, float4 gBuffer2, float4 gBuffer3)
{
	PBRDesc pbrDesc;
	pbrDesc.pos = gBuffer0.xyz;
	pbrDesc.metalness = gBuffer0.w;
	pbrDesc.albedo = gBuffer1.xyz;
	pbrDesc.roughness = gBuffer1.w;
	pbrDesc.normal = normalize(gBuffer2.xyz);
	return pbrDesc;
}