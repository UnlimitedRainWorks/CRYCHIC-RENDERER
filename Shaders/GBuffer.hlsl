
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
    float4 normal;
};

//---------------------------------------------------------------------------------------
// Transfer PBR information to GBuffer
//---------------------------------------------------------------------------------------
GBuffer EncodePBRToGBuffer(float3 pos,float metalness, 
						   float3 albedo, float4 normal, float roughness)
{
	GBuffer gout;
	gout.GBuffer0 = float4(pos, metalness);
	gout.GBuffer1 = float4(albedo, roughness);
	gout.GBuffer2 = float4(normal);
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
	pbrDesc.normal = float4(normalize(gBuffer2.xyz), gBuffer2.w);
	return pbrDesc;
}