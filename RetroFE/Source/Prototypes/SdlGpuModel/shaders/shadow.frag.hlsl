Texture2D baseTexture : register(t0, space2);
SamplerState baseSampler : register(s0, space2);
cbuffer ShadowMaterial : register(b0, space3) {
    float4 alphaOptions; // base alpha, cutoff, is MASK, unused
};
struct Input {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};
void main(Input i) {
    if (alphaOptions.z > 0.5 &&
        baseTexture.Sample(baseSampler,i.uv).a * alphaOptions.x < alphaOptions.y)
        discard;
}
