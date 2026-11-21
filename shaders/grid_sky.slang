// PLATZ Slang Shader: 3D World Reference Grid & Axis Lines
// Compiled to Vulkan SPIR-V 1.5 via Slang 2026.22.1 (slangc.exe)

struct SceneUniforms
{
    float4x4 viewProj;
    float4   cameraPos;
    float4   sunDirection;
    float4   sunColor;
    float4   pointLightPos;
    float4   pointLightColor;
    float4   skyTopColor;
    float4   skyHorizonColor;
};

[[vk::binding(0, 0)]]
ConstantBuffer<SceneUniforms> gScene;

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
    [[vk::location(0)]] float3 worldPos : TEXCOORD0;
    [[vk::location(1)]] float3 color    : COLOR0;
};

struct FSOutput
{
    [[vk::location(0)]] float4 outColor    : SV_Target0;
    [[vk::location(1)]] uint   outObjectId : SV_Target1;
};

[shader("vertex")]
VSOutput vsMain(VSInput input)
{
    VSOutput output;
    output.worldPos = input.position;
    output.color = input.color;
    output.svPosition = mul(gScene.viewProj, float4(input.position, 1.0));
    return output;
}

[shader("fragment")]
FSOutput fsMain(VSOutput input)
{
    float dist = length(input.worldPos.xz - gScene.cameraPos.xz);
    float fade = clamp(1.0 - (dist / 80.0), 0.0, 1.0);
    FSOutput output;
    output.outColor = float4(input.color * fade, 1.0);
    output.outObjectId = 0u;
    return output;
}
