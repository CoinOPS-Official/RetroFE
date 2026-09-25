Texture2D worldTexture : register(t0, space2);
Texture2D normalTexture : register(t1, space2);
SamplerState worldSampler : register(s0, space2);
SamplerState normalSampler : register(s1, space2);
cbuffer AoOptions : register(b0, space3) {
    float4 texelAndRadius; // inverse width, inverse height, world radius, strength
};
float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target0 {
    float4 center = worldTexture.SampleLevel(worldSampler,uv,0);
    if (center.w < 0.5) return 1.xxxx;
    float3 n = normalize(normalTexture.SampleLevel(normalSampler,uv,0).xyz*2-1);
    float occluded = 0;
    [unroll] for (int i=0; i<16; ++i) {
        float angle = (i+0.5) * 2.39996323;
        float radius = 2 + 16 * sqrt((i+0.5)/16.0);
        float2 offset = float2(cos(angle),sin(angle)) * radius * texelAndRadius.xy;
        float4 nearby = worldTexture.SampleLevel(worldSampler,uv+offset,0);
        if (nearby.w < 0.5) continue;
        float3 delta = nearby.xyz-center.xyz;
        float distanceToSample = length(delta);
        float height = dot(n,delta);
        float range = saturate(1-distanceToSample/texelAndRadius.z);
        occluded += saturate((height-0.001)/max(distanceToSample,0.001)) * range;
    }
    float ao = 1-saturate(occluded*texelAndRadius.w/16.0);
    return float4(ao,ao,ao,1);
}
