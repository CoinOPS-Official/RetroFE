cbuffer ShadowScene : register(b0, space1) {
    float4x4 model;
    float4x4 lightViewProjection;
};

struct Input {
    float3 position : TEXCOORD0;
    float2 uv : TEXCOORD1;
};
struct Output {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};
Output main(Input v) {
    Output o;
    o.position = mul(lightViewProjection,mul(model,float4(v.position,1)));
    o.uv = v.uv;
    return o;
}
