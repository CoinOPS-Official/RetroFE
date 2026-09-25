Texture2D baseTexture : register(t0, space2);
SamplerState baseSampler : register(s0, space2);
cbuffer AoMaterial : register(b0, space3) {
    float4 alphaOptions; // base alpha, cutoff, is MASK, unused
};
struct Input {
    float4 position : SV_Position;
    float3 world : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float2 uv : TEXCOORD2;
};
struct Output {
    float4 world : SV_Target0;
    float4 normal : SV_Target1;
};
Output main(Input i) {
    if (alphaOptions.z > 0.5 &&
        baseTexture.Sample(baseSampler,i.uv).a * alphaOptions.x < alphaOptions.y)
        discard;
    Output o;
    o.world = float4(i.world,1);
    o.normal = float4(normalize(i.normal)*0.5+0.5,1);
    return o;
}
