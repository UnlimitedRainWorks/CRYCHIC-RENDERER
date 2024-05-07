struct GBuffer
{
	float4 GBuffer0 : SV_Target0;
	float4 GBuffer1 : SV_Target1;
	float4 GBuffer2 : SV_Target2;
	float4 GBuffer3 : SV_Target3;
};

struct GBufferDesc
{
	float3 pos;
    float metalness;
    float3 albedo;
	float ao;
    float3 normal;
	float roughness;
};

//---------------------------------------------------------------------------------------
// Transfer PBR information to GBuffer
//---------------------------------------------------------------------------------------
GBuffer EncodePBRToGBuffer(float3 pos, float metalness, float3 albedo, float roughness,
						   float3 normal)
{
	GBuffer gout;
	gout.GBuffer0 = float4(pos, metalness);
	gout.GBuffer1 = float4(albedo, roughness);
	gout.GBuffer2 = float4(normal, 1.0f);
	gout.GBuffer3 = 0.0f;
	return gout;
}

GBufferDesc DecodeGBuffer(float4 gBuffer0, float4 gBuffer1, float4 gBuffer2,
						  float4 gBuffer3)
{
	GBufferDesc desc;
	desc.pos = gBuffer0.xyz;
	desc.metalness = gBuffer0.w;
	desc.albedo = gBuffer1.xyz;
	desc.roughness = gBuffer1.w;
	desc.normal = normalize(gBuffer2.xyz);
	return desc;
}