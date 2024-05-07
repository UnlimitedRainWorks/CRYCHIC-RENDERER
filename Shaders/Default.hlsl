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

struct VertexIn
{
	float3 PosL    : POSITION;
    float3 NormalL : NORMAL;
	float2 TexC    : TEXCOORD;
	float3 TangentU : TANGENT;
};

struct VertexOut
{
	float4 PosH    : SV_POSITION;
    float4 SsaoPosH   : POSITION1;
    float3 PosW    : POSITION2;
    float3 NormalW : NORMAL;
	float3 TangentW : TANGENT;
	float2 TexC    : TEXCOORD;
    nointerpolation uint MatIndex : MATINDEX;
};

VertexOut VS(VertexIn vin, uint instanceID : SV_InstanceID)
{
	VertexOut vout = (VertexOut)0.0f;

    InstanceData instanceData = gInstanceData[instanceID];
    float4x4 gWorld = instanceData.World;
    float4x4 gTexTransform = instanceData.TexTransform;
    uint gMaterialIndex = instanceData.MaterialIndex;

    vout.MatIndex = gMaterialIndex;

	// Fetch the material data.
	MaterialData matData = gMaterialData[gMaterialIndex];
	
    // Transform to world space.
    float4 posW = mul(float4(vin.PosL, 1.0f), gWorld);
    vout.PosW = posW.xyz;

    // Assumes nonuniform scaling; otherwise, need to use inverse-transpose of world matrix.
    vout.NormalW = mul(vin.NormalL, (float3x3)gWorld);
	
	vout.TangentW = mul(vin.TangentU, (float3x3)gWorld);

    // Transform to homogeneous clip space.
    vout.PosH = mul(posW, gViewProj);

    // Generate projective tex-coords to project SSAO map onto scene.
    vout.SsaoPosH = mul(posW, gViewProjTex);
	
	// Output vertex attributes for interpolation across triangle.
	float4 texC = mul(float4(vin.TexC, 0.0f, 1.0f), gTexTransform);
	vout.TexC = mul(texC, matData.MatTransform).xy;
	
    return vout;
}

float4 PS(VertexOut pin) : SV_Target
{
	// Fetch the material data.
	MaterialData matData = gMaterialData[pin.MatIndex];
	float4 diffuseAlbedo = matData.DiffuseAlbedo;
	//float3 fresnelR0 = matData.FresnelR0;
	float  roughness = matData.Roughness;
    float  metalness = matData.Metalness;
    float3 fresnelR0 = lerp(0.04, diffuseAlbedo.xyz, metalness);
	uint diffuseMapIndex = matData.DiffuseMapIndex;
	uint normalMapIndex = matData.NormalMapIndex;
	
    // Dynamically look up the texture in the array.
    diffuseAlbedo *= gTextureMaps[diffuseMapIndex].Sample(gsamAnisotropicWrap, pin.TexC);

#ifdef ALPHA_TEST
    // Discard pixel if texture alpha < 0.1.  We do this test as soon 
    // as possible in the shader so that we can potentially exit the
    // shader early, thereby skipping the rest of the shader code.
    clip(diffuseAlbedo.a - 0.1f);
#endif

	// Interpolating normal can unnormalize it, so renormalize it.
    pin.NormalW = normalize(pin.NormalW);
	
    float4 normalMapSample = gTextureMaps[normalMapIndex].Sample(gsamAnisotropicWrap, pin.TexC);
	float3 bumpedNormalW = NormalSampleToWorldSpace(normalMapSample.rgb, pin.NormalW, pin.TangentW);

	// Uncomment to turn off normal mapping.
    //bumpedNormalW = pin.NormalW;

    // Vector from point being lit to eye. 
    float3 toEyeW = normalize(gEyePosW - pin.PosW);

    // Finish texture projection and sample SSAO map.
    pin.SsaoPosH /= pin.SsaoPosH.w;
    float ambientAccess = gSsaoMap[0].Sample(gsamLinearClamp, pin.SsaoPosH.xy, 0.0f).r;

    // Light terms.
    float4 ambient = ambientAccess*gAmbientLight*diffuseAlbedo;

    // Only the first light casts a shadow.
    float3 shadowFactors[MaxLights];// = float3(1.0f, 1.0f, 1.0f);
    for (int i = 0; i < MaxLights; i++)
    {
        shadowFactors[i] = 1.0f;
    }

    //int i = 0;
    float radius[4]={30.0f, 50.0f, 80.0f,100.0f};
    int j = 0;
    for (j = 0; j < 4; j++)
    {
        float distance = length(gEyePosW - pin.PosW);
        float shadowFactor, shadowFactorNextLevel;
       float4 shadowPosH, shadowPosHNextLevel;
       if(j < 3 && distance < radius[j] && abs(distance - radius[j]) < 10.0f)
       {
            
            shadowPosH = mul(float4(pin.PosW, 1.0f), gShadowTransforms[j]);
            shadowPosHNextLevel = mul(float4(pin.PosW, 1.0f), gShadowTransforms[j + 1]);

            shadowFactor = CalcCascadeShadowFactorWithPoisson(j, shadowPosH);
            shadowFactorNextLevel = CalcCascadeShadowFactorWithPoisson(j + 1, shadowPosHNextLevel);
            shadowFactors[0] = 0.5 * (shadowFactor + shadowFactorNextLevel);
            break;
       }    
       else if(distance < radius[j])
       {
            shadowPosH = mul(float4(pin.PosW, 1.0f), gShadowTransforms[j]);
            shadowFactor = CalcCascadeShadowFactorWithPoisson(j, shadowPosH);
            shadowFactors[0] = shadowFactor;
            break;   
       }
    }
    //shadowFactors[0] = CalcShadowFactor(pin.ShadowPosH);
    
    // Area DEBUG
    //if(j == 0)return float4(1.0f, 0.0f, 0.0f, 1.0f);
    //if(j == 1)return float4(0.0f, 1.0f, 0.0f, 1.0f);
    //if(j == 2)return float4(0.0f, 0.0f, 1.0f, 1.0f);
    //if(j == 3)return float4(1.0f, 1.0f, 1.0f, 1.0f);


    const float shininess = (1.0f - roughness) * normalMapSample.a;
    //const float shininess = (1.0f - roughness) * 1.0;
    Material mat = { diffuseAlbedo, fresnelR0, roughness, metalness, shininess };
    // BlinnPhon
    //float4 directLight = ComputeLighting(gLights, mat, pin.PosW, bumpedNormalW, toEyeW, shadowFactors);
    // PBRShading
    float4 directLight = PBRShading(gLights, mat, bumpedNormalW, toEyeW, pin.PosW, shadowFactors);

    directLight /= (directLight + 1.0f);
    directLight = pow(directLight, 1.0f / 2.2f);

    float4 litColor = ambient + directLight;

    //litColor /= (litColor + 1.0f);
    //litColor = pow(litColor, 1.0f / 2.2f);
    //return litColor;
	// Add in specular reflections.
    float3 r = reflect(-toEyeW, bumpedNormalW);
    float4 reflectionColor = gCubeMap.Sample(gsamLinearWrap, r);
    float3 fresnelFactor = SchlickFresnel(fresnelR0, bumpedNormalW, r);
    litColor.rgb += shininess * fresnelFactor * reflectionColor.rgb;
	
    // Common convention to take alpha from diffuse albedo.
    litColor.a = diffuseAlbedo.a;

    return litColor;
}


