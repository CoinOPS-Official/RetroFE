cbuffer Scene : register(b0, space1) {
    float4x4 model;
    float4x4 viewProjection;
    float4x4 cabinetModel;
    float4x4 normalModel;
};

struct Input {
    float3 position : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float2 uv : TEXCOORD2;
    float4 tangent : TEXCOORD3;
};
struct Output {
    float4 position : SV_Position;
    float3 world : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float2 uv : TEXCOORD2;
    float3 tangent : TEXCOORD3;
    float tangentSign : TEXCOORD4;
    float cabinetY : TEXCOORD5;
};
Output main(Input v) {
    Output o;
    float4 world = mul(model, float4(v.position, 1));
    o.position = mul(viewProjection, world);
    o.world = world.xyz;
    o.normal = normalize(mul((float3x3)normalModel, v.normal));
    o.tangent = normalize(mul((float3x3)model, v.tangent.xyz));
    // A reflected glTF import changes tangent-space handedness. Node or
    // instance transforms can reflect it as well.
    o.tangentSign = v.tangent.w * (determinant((float3x3)model) < 0 ? -1.0 : 1.0);
    o.cabinetY = mul(cabinetModel,float4(v.position,1)).y;
    o.uv = v.uv;
    return o;
}
