cbuffer AoScene : register(b0, space1) {
    float4x4 model;
    float4x4 viewProjection;
    float4x4 normalModel;
};
struct Input {
    float3 position : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float2 uv : TEXCOORD2;
};
struct Output {
    float4 position : SV_Position;
    float3 world : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float2 uv : TEXCOORD2;
};
Output main(Input v) {
    Output o;
    float4 world = mul(model,float4(v.position,1));
    o.position = mul(viewProjection,world);
    o.world = world.xyz;
    o.normal = normalize(mul((float3x3)normalModel,v.normal));
    o.uv = v.uv;
    return o;
}
