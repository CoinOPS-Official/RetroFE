Texture2D imageTexture : register(t0, space2);
SamplerState imageSampler : register(s0, space2);
Texture2D bloomTexture : register(t1, space2);
SamplerState bloomSampler : register(s1, space2);
cbuffer Params : register(b0, space3) {
    float4 options; // x mode, y exposure, z filmic tone mapping
    float4 texel;
};
float3 tonemap(float3 x) { return x / (1 + x); }
float3 filmic(float3 x) {
    return saturate((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14));
}
float3 linearToSrgb(float3 c) {
    return lerp(c * 12.92, 1.055 * pow(c, 1.0 / 2.4) - 0.055, step(0.0031308, c));
}
float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target0 {
    float4 original = imageTexture.Sample(imageSampler, uv);
    float3 center = original.rgb;
    if (options.x > 2.5) {
        float3 bloom = bloomTexture.Sample(bloomSampler, uv).rgb;
        const float alpha = saturate(original.a + max(bloom.r,max(bloom.g,bloom.b)) * 0.12);
        // The transparent HDR clear and MSAA resolve leave RGB associated with
        // coverage alpha. Tone map the recovered color, then premultiply again
        // in display space for SDL's premultiplied texture blending and scaling.
        if (alpha <= 0.00001) return float4(0,0,0,0);
        float3 straightHDR = max(center + bloom * 0.42, 0) / alpha;
        float3 exposed = straightHDR * options.y;
        float3 straightSDR = linearToSrgb(options.z > 0.5 ? filmic(exposed) : saturate(tonemap(exposed)));
        return float4(straightSDR * alpha, alpha);
    }
    if (options.x < 0.5) {
        float brightness = dot(center, float3(0.2126, 0.7152, 0.0722));
        return float4(brightness > 1.0 ? center : 0, 1);
    }
    float2 direction = options.x < 1.5 ? float2(texel.x, 0) : float2(0, texel.y);
    float3 sum = center * 0.227027;
    sum += imageTexture.Sample(imageSampler, uv + direction * 1.384615).rgb * 0.316216;
    sum += imageTexture.Sample(imageSampler, uv - direction * 1.384615).rgb * 0.316216;
    sum += imageTexture.Sample(imageSampler, uv + direction * 3.230769).rgb * 0.070270;
    sum += imageTexture.Sample(imageSampler, uv - direction * 3.230769).rgb * 0.070270;
    return float4(sum, 1);
}
