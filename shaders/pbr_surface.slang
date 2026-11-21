// PLATZ Slang Shader: PBR Metallic-Roughness + Directional/Point Lighting + Selection Highlight + Object ID Output
// Compiled to Vulkan SPIR-V 1.5 via Slang 2026.22.1 (slangc.exe)

struct SceneUniforms
{
    float4x4 viewProj;
    float4   cameraPos;       // xyz = world camera pos, w = exposure
    float4   sunDirection;    // xyz = normalized direction to sun, w = intensity
    float4   sunColor;        // rgb = color, a = ambientIntensity
    float4   pointLightPos;   // xyz = pos, w = radius
    float4   pointLightColor; // rgb = color, a = intensity
    float4   skyTopColor;     // rgb
    float4   skyHorizonColor; // rgb
};

struct ObjectPushConstants
{
    float4x4 modelMatrix;
    float4   baseColor;       // rgba
    float4   pbrParams;       // x = metallic, y = roughness, z = emissiveStrength, w = normalScale
    float4   emissiveColor;   // rgb = emissive, w = isSelected (1.0 if selected)
    uint     objectIdLow;
    uint     objectIdHigh;
    uint     textureFlags;    // bit 0 = useProceduralChecker, bit 1 = useAlbeoTex, bit 2 = wireframeOverlay
    uint     pad0;
};

[[vk::binding(0, 0)]]
ConstantBuffer<SceneUniforms> gScene;

[[vk::binding(1, 0)]]
Sampler2D gAlbedoTexture;

[[vk::push_constant]]
ConstantBuffer<ObjectPushConstants> gObject;

struct VSInput
{
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(1)]] float3 normal   : NORMAL;
    [[vk::location(2)]] float2 uv       : TEXCOORD0;
    [[vk::location(3)]] float3 color    : COLOR0;
};

struct VSOutput
{
    float4 svPosition : SV_Position;
    [[vk::location(0)]] float3 worldPos    : TEXCOORD0;
    [[vk::location(1)]] float3 worldNormal : TEXCOORD1;
    [[vk::location(2)]] float2 uv          : TEXCOORD2;
    [[vk::location(3)]] float3 vertexColor : COLOR0;
};

struct FSOutput
{
    [[vk::location(0)]] float4 outColor    : SV_Target0;
    [[vk::location(1)]] uint   outObjectId : SV_Target1;
};

static const float PI = 3.14159265359;

float DistributionGGX(float3 N, float3 H, float roughness)
{
    float a = roughness * roughness;
    float a2 = a * a;
    float NdotH = max(dot(N, H), 0.0);
    float NdotH2 = NdotH * NdotH;
    float denom = (NdotH2 * (a2 - 1.0) + 1.0);
    denom = PI * denom * denom;
    return a2 / max(denom, 0.00001);
}

float GeometrySchlickGGX(float NdotV, float roughness)
{
    float r = (roughness + 1.0);
    float k = (r * r) / 8.0;
    return NdotV / max(NdotV * (1.0 - k) + k, 0.00001);
}

float GeometrySmith(float3 N, float3 V, float3 L, float roughness)
{
    float NdotV = max(dot(N, V), 0.0);
    float NdotL = max(dot(N, L), 0.0);
    return GeometrySchlickGGX(NdotV, roughness) * GeometrySchlickGGX(NdotL, roughness);
}

float3 FresnelSchlick(float cosTheta, float3 F0)
{
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

[shader("vertex")]
VSOutput vsMain(VSInput input)
{
    VSOutput output;
    float4 worldPos4 = mul(gObject.modelMatrix, float4(input.position, 1.0));
    output.worldPos = worldPos4.xyz;
    float3x3 normalMat = (float3x3)gObject.modelMatrix;
    output.worldNormal = normalize(mul(normalMat, input.normal));
    output.uv = input.uv;
    output.vertexColor = input.color;
    output.svPosition = mul(gScene.viewProj, worldPos4);
    return output;
}

[shader("fragment")]
FSOutput fsMain(VSOutput input)
{
    float3 N = normalize(input.worldNormal);
    float3 V = normalize(gScene.cameraPos.xyz - input.worldPos);

    float3 albedo = gObject.baseColor.rgb * input.vertexColor;
    if ((gObject.textureFlags & 1u) != 0u)
    {
        float2 grid = floor(input.uv * 8.0);
        float checker = fmod(grid.x + grid.y, 2.0);
        albedo *= lerp(0.72, 1.0, abs(checker));
    }
    if ((gObject.textureFlags & 2u) != 0u)
    {
        float4 texSample = gAlbedoTexture.Sample(input.uv);
        albedo *= texSample.rgb;
    }

    float metallic = clamp(gObject.pbrParams.x, 0.0, 1.0);
    float roughness = clamp(gObject.pbrParams.y, 0.04, 1.0);
    float emissiveStrength = max(gObject.pbrParams.z, 0.0);

    float3 F0 = lerp(float3(0.04, 0.04, 0.04), albedo, metallic);

    // Directional Sun Light
    float3 L_sun = normalize(gScene.sunDirection.xyz);
    float3 H_sun = normalize(V + L_sun);
    float3 radiance_sun = gScene.sunColor.rgb * max(gScene.sunDirection.w, 0.0);

    float NDF_sun = DistributionGGX(N, H_sun, roughness);
    float G_sun = GeometrySmith(N, V, L_sun, roughness);
    float3 F_sun = FresnelSchlick(max(dot(H_sun, V), 0.0), F0);

    float3 numerator_sun = NDF_sun * G_sun * F_sun;
    float denom_sun = 4.0 * max(dot(N, V), 0.0) * max(dot(N, L_sun), 0.0) + 0.0001;
    float3 specular_sun = numerator_sun / denom_sun;

    float3 kS_sun = F_sun;
    float3 kD_sun = (float3(1.0, 1.0, 1.0) - kS_sun) * (1.0 - metallic);
    float NdotL_sun = max(dot(N, L_sun), 0.0);
    float3 Lo = (kD_sun * albedo / PI + specular_sun) * radiance_sun * NdotL_sun;

    // Local Point Light
    float3 toPoint = gScene.pointLightPos.xyz - input.worldPos;
    float dist = length(toPoint);
    float radius = max(gScene.pointLightPos.w, 0.001);
    if (dist < radius && gScene.pointLightColor.a > 0.0)
    {
        float3 L_pt = toPoint / max(dist, 0.0001);
        float3 H_pt = normalize(V + L_pt);
        float atten = clamp(1.0 - (dist / radius), 0.0, 1.0);
        atten = atten * atten;
        float3 radiance_pt = gScene.pointLightColor.rgb * gScene.pointLightColor.a * atten;

        float NDF_pt = DistributionGGX(N, H_pt, roughness);
        float G_pt = GeometrySmith(N, V, L_pt, roughness);
        float3 F_pt = FresnelSchlick(max(dot(H_pt, V), 0.0), F0);
        float3 spec_pt = (NDF_pt * G_pt * F_pt) / (4.0 * max(dot(N, V), 0.0) * max(dot(N, L_pt), 0.0) + 0.0001);
        float3 kD_pt = (float3(1.0, 1.0, 1.0) - F_pt) * (1.0 - metallic);
        float NdotL_pt = max(dot(N, L_pt), 0.0);
        Lo += (kD_pt * albedo / PI + spec_pt) * radiance_pt * NdotL_pt;
    }

    // Hemisphere ambient diffuse + Studio Environment Specular Reflection (IBL approximation for conductors & dielectrics)
    float hemi = clamp(N.y * 0.5 + 0.5, 0.0, 1.0);
    float3 skyAmb = lerp(gScene.skyHorizonColor.rgb * 0.65, gScene.skyTopColor.rgb * 1.35, hemi);
    float3 kS_amb = FresnelSchlick(max(dot(N, V), 0.0), F0);
    float3 kD_amb = (1.0 - kS_amb) * (1.0 - metallic * 0.65);
    float3 diffuseAmb = skyAmb * albedo * kD_amb * max(gScene.sunColor.a, 0.25);

    float3 R = reflect(-V, N);
    float reflHemi = clamp(R.y * 0.5 + 0.5, 0.0, 1.0);
    float3 envSky = lerp(float3(0.22, 0.26, 0.34), float3(0.68, 0.78, 0.95), pow(reflHemi, 0.8));
    float sunReflLobe = pow(max(dot(R, L_sun), 0.0), lerp(32.0, 3.0, roughness));
    float3 envRadiance = envSky + gScene.sunColor.rgb * sunReflLobe * 0.85;
    float3 envSpec = envRadiance * kS_amb * (1.0 - roughness * 0.55) * lerp(0.35, 1.15, metallic);

    float3 ambient = diffuseAmb + envSpec;

    float3 emissive = gObject.emissiveColor.rgb * emissiveStrength;
    float3 color = ambient + Lo + emissive;

    // ACES filmic tone mapping
    float exposure = max(gScene.cameraPos.w, 0.1);
    color *= exposure;
    float3 x = color;
    color = clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0);

    // Selection rim highlight (PZ cyan/gold highlight)
    if (gObject.emissiveColor.w > 0.5)
    {
        float rim = 1.0 - max(dot(N, V), 0.0);
        rim = pow(clamp(rim, 0.0, 1.0), 2.2);
        float3 selAccent = float3(0.0, 0.85, 1.0);
        color = lerp(color, selAccent, clamp(rim * 0.75 + 0.15, 0.0, 0.85));
    }

    FSOutput output;
    output.outColor = float4(color, gObject.baseColor.a);
    output.outObjectId = gObject.objectIdLow;
    return output;
}
