Texture2D baseTexture : register(t0, space2);
Texture2D normalTexture : register(t1, space2);
Texture2D mrTexture : register(t2, space2);
Texture2D emissiveTexture : register(t3, space2);
Texture2D environmentTexture : register(t4, space2);
Texture2D matcapTexture : register(t5, space2);
Texture2D irradianceTexture : register(t6, space2);
Texture2D dfgTexture : register(t7, space2);
Texture2D occlusionTexture : register(t8, space2);
Texture2D keyShadowTexture : register(t9, space2);
Texture2D fillShadowTexture : register(t10, space2);
Texture2D screenShadowTexture : register(t11, space2);
Texture2D marqueeShadowTexture : register(t12, space2);
Texture2D screenAoTexture : register(t13, space2);
SamplerState linearSampler : register(s0, space2);
SamplerState normalSampler : register(s1, space2);
SamplerState mrSampler : register(s2, space2);
SamplerState emissiveSampler : register(s3, space2);
SamplerState envSampler : register(s4, space2);
SamplerState matcapSampler : register(s5, space2);
SamplerState irradianceSampler : register(s6, space2);
SamplerState dfgSampler : register(s7, space2);
SamplerState occlusionSampler : register(s8, space2);
SamplerState keyShadowSampler : register(s9, space2);
SamplerState fillShadowSampler : register(s10, space2);
SamplerState screenShadowSampler : register(s11, space2);
SamplerState marqueeShadowSampler : register(s12, space2);
SamplerState screenAoSampler : register(s13, space2);

cbuffer Material : register(b0, space3) {
    float4 baseFactor;
    float4 emissiveAndStrength;
    float4 surface; // metallic, roughness, normal scale, alpha cutoff
    float4 flags;   // screen, marquee, alpha mode, transmission
    float4 specular; // dielectric color RGB and strength
    float4 occlusion; // glTF occlusion strength
};
cbuffer Lighting : register(b1, space3) {
    float4 cameraAndTime;
    float4 keyPositionIntensity;
    float4 fillPositionIntensity;
    float4 screenPositionIntensity;
    float4 screenColor;
    float4 screenDirectionCone;
    float4 marqueePositionIntensity;
    float4 marqueeDirectionCone;
    float4 spillReceiverHeights;
    float4 ambientAndMatcap;
    float4 cameraRight;
    float4 cameraUp;
    float4 pbrOptions; // x multiple-scattering compensation
    float4x4 shadowViewProjection[4];
    float4 shadowOptions; // enabled, inverse map size, inverse target width/height
    float4 shadowActive;
};

struct Input {
    float4 position : SV_Position;
    float3 world : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float2 uv : TEXCOORD2;
    float3 tangent : TEXCOORD3;
    float tangentSign : TEXCOORD4;
    float cabinetY : TEXCOORD5;
};

static const float PI = 3.14159265359;
float3 fresnel(float cosTheta, float3 f0, float f90) {
    return f0 + (f90 - f0) * pow(1 - cosTheta, 5);
}
float3 materialF0(float3 albedo, float metallic) {
    return lerp(min(0.04 * specular.rgb, 1.0) * specular.a, albedo, metallic);
}
float materialF90(float metallic) { return lerp(specular.a, 1.0, metallic); }
float distributionGGX(float ndh, float roughness) {
    float a = roughness * roughness;
    float a2 = a * a;
    float d = ndh * ndh * (a2 - 1) + 1;
    return a2 / max(PI * d * d, 0.0001);
}
float visibilitySmithGGXCorrelated(float ndv, float ndl, float roughness) {
    float a = roughness * roughness;
    float a2 = a * a;
    float ggxL = ndv * sqrt((ndl - ndl * a2) * ndl + a2);
    float ggxV = ndl * sqrt((ndv - ndv * a2) * ndv + a2);
    return 0.5 / max(ggxV + ggxL, 1e-5);
}
float filteredRoughness(float perceptualRoughness, float3 geometricNormal) {
    // Filament's geometric specular AA, with its default variance and threshold.
    float3 dx = ddx(geometricNormal);
    float3 dy = ddy(geometricNormal);
    float variance = 0.15 * (dot(dx,dx) + dot(dy,dy));
    float alpha = perceptualRoughness * perceptualRoughness;
    return pow(saturate(alpha * alpha + min(2 * variance, 0.2)), 0.25);
}
float3 directLight(float3 n, float3 v, float3 l, float3 radiance,
                   float3 albedo, float metallic, float roughness,
                   float3 energyCompensation) {
    float3 h = normalize(v + l);
    float ndl = saturate(dot(n, l));
    float ndv = max(dot(n, v), 0.001);
    float3 f0 = materialF0(albedo,metallic);
    float3 f = fresnel(saturate(dot(h, v)), f0, materialF90(metallic));
    float spec = distributionGGX(saturate(dot(n, h)), roughness) *
        visibilitySmithGGXCorrelated(ndv, ndl, roughness);
    float diffuseWeight = 1 - max(f.r,max(f.g,f.b));
    return (diffuseWeight * (1 - metallic) * albedo / PI +
            f * spec * energyCompensation) * radiance * ndl;
}
float3 pointLight(float3 positionIntensity, float intensity, float3 tint,
                  float falloff, float3 world, float3 n, float3 v,
                  float3 albedo, float metallic, float roughness,
                  float3 energyCompensation) {
    float3 toLight = positionIntensity - world;
    float distance2 = dot(toLight, toLight);
    return directLight(n, v, toLight * rsqrt(max(distance2,1e-8)),
        tint * intensity / (1 + falloff * distance2), albedo, metallic, roughness,
        energyCompensation);
}
float3 spillLight(float4 positionIntensity, float4 directionCone, float3 tint,
                  float falloff, float minimumY, Input i, float3 n, float3 v,
                  float3 albedo, float metallic, float roughness,
                  float3 energyCompensation) {
    float3 fromLight = i.world - positionIntensity.xyz;
    float3 ray = fromLight * rsqrt(max(dot(fromLight,fromLight),1e-8));
    float angular = smoothstep(directionCone.w,min(1.0,directionCone.w + 0.22),
                               dot(ray,directionCone.xyz));
    float receiver = smoothstep(minimumY,minimumY + 0.08,i.cabinetY);
    return pointLight(positionIntensity.xyz,positionIntensity.w,tint,falloff,
                      i.world,n,v,albedo,metallic,roughness,energyCompensation) * angular * receiver;
}
float shadowVisibility(float active, Texture2D shadowTexture, SamplerState shadowSampler,
                       float4x4 lightViewProjection, float3 world, float3 n, float3 toLight) {
    if (shadowOptions.x < 0.5 || active < 0.5) return 1;
    float4 projected = mul(lightViewProjection,float4(world,1));
    if (projected.w <= 0) return 1;
    float3 clip = projected.xyz / projected.w;
    float2 uv = float2(clip.x * 0.5 + 0.5, 0.5 - clip.y * 0.5);
    if (uv.x < 0 || uv.x > 1 || uv.y < 0 || uv.y > 1 || clip.z < 0 || clip.z > 1)
        return 1;
    float ndl = saturate(dot(n,normalize(toLight)));
    // Grazing surfaces receive almost no direct energy and are the most
    // vulnerable to perspective-map self-shadowing.
    if (ndl < 0.1) return 1;
    float bias = 0.0015 + 0.003 * (1 - ndl);
    float visible = 0;
    [unroll] for (int y=-1; y<=1; ++y) {
        [unroll] for (int x=-1; x<=1; ++x) {
            float2 offset = float2(x,y) * shadowOptions.y;
            visible += clip.z - bias <= shadowTexture.SampleLevel(shadowSampler,uv+offset,0).r;
        }
    }
    return visible / 9;
}
float4 main(Input i) : SV_Target0 {
    float4 baseSample = baseTexture.Sample(linearSampler, i.uv);
    float4 base = float4(baseSample.rgb * baseFactor.rgb, baseSample.a * baseFactor.a);
    if (flags.z > 0.5 && flags.z < 1.5 && base.a < surface.w) discard;
    // A transmissive pane should pass the CRT image through. With no
    // scene-color refraction pass, keep only its surface reflection and a
    // small amount of coverage rather than blending a gray diffuse layer.
    base.rgb *= 1 - saturate(flags.w);
    float2 mr = mrTexture.Sample(mrSampler, i.uv).gb;
    float roughness = saturate(surface.y * mr.x);
    float metallic = saturate(surface.x * mr.y);
    float3 n = normalize(i.normal);
    if (ambientAndMatcap.w > 0.5) roughness = filteredRoughness(roughness,n);
    roughness = clamp(roughness, 0.089, 1);
    float3 t = normalize(i.tangent - n * dot(i.tangent, n));
    float3 b = normalize(cross(n, t)) * i.tangentSign;
    float3 mapN = normalTexture.Sample(normalSampler, i.uv).xyz * 2 - 1;
    mapN.xy *= surface.z;
    n = normalize(mapN.x * t + mapN.y * b + mapN.z * n);
    float3 v = normalize(cameraAndTime.xyz - i.world);
    float3 f0 = materialF0(base.rgb,metallic);
    float f90 = materialF90(metallic);
    float2 dfg = dfgTexture.SampleLevel(dfgSampler,
        float2(saturate(dot(n,v)),roughness),0).rg;
    // The current LUT stores the single-scattering scale and Fresnel bias.
    // Their sum is Filament's r: the directional albedo with F0 = F90 = 1.
    float directionalAlbedo = max(dfg.x + dfg.y, 0.05);
    float3 energyCompensation = pbrOptions.x > 0.5
        ? 1 + f0 * (1 / directionalAlbedo - 1) : 1.xxx;
    float keyVisibility = shadowVisibility(shadowActive.x,keyShadowTexture,keyShadowSampler,
        shadowViewProjection[0],i.world,n,keyPositionIntensity.xyz-i.world);
    float fillVisibility = shadowVisibility(shadowActive.y,fillShadowTexture,fillShadowSampler,
        shadowViewProjection[1],i.world,n,fillPositionIntensity.xyz-i.world);
    float screenVisibility = shadowVisibility(shadowActive.z,screenShadowTexture,screenShadowSampler,
        shadowViewProjection[2],i.world,n,screenPositionIntensity.xyz-i.world);
    float marqueeVisibility = shadowVisibility(shadowActive.w,marqueeShadowTexture,marqueeShadowSampler,
        shadowViewProjection[3],i.world,n,marqueePositionIntensity.xyz-i.world);
    float3 color = keyVisibility * pointLight(keyPositionIntensity.xyz, keyPositionIntensity.w,
        float3(1.0, 0.88, 0.72), 0.6, i.world, n, v, base.rgb, metallic, roughness,
        energyCompensation);
    color += fillVisibility * pointLight(fillPositionIntensity.xyz, fillPositionIntensity.w,
        float3(0.45, 0.65, 1.0), 0.9, i.world, n, v, base.rgb, metallic, roughness,
        energyCompensation);
    color += screenVisibility * spillLight(screenPositionIntensity,screenDirectionCone,screenColor.rgb,
        3.0,spillReceiverHeights.x,i,n,v,base.rgb,metallic,roughness,energyCompensation);
    color += marqueeVisibility * spillLight(marqueePositionIntensity,marqueeDirectionCone,float3(1.0, 0.25, 0.08),
        5.0,spillReceiverHeights.y,i,n,v,base.rgb,metallic,roughness,energyCompensation);
    float3 r = reflect(-v, n);
    float2 envUV = float2(atan2(r.z, r.x) / (2 * PI) + 0.5 + ambientAndMatcap.z / (2 * PI),
                          acos(clamp(r.y, -1, 1)) / PI);
    float2 normalEnvUV = float2(atan2(n.z, n.x) / (2 * PI) + 0.5 + ambientAndMatcap.z / (2 * PI),
                                acos(clamp(n.y, -1, 1)) / PI);
    float3 indirectDiffuse = irradianceTexture.Sample(irradianceSampler,normalEnvUV).rgb;
    float3 prefilteredSpecular = environmentTexture.SampleLevel(envSampler, envUV, roughness * 7).rgb;
    float authoredAo = 1 - saturate(occlusion.x) * (1 - occlusionTexture.Sample(occlusionSampler,i.uv).r);
    float screenAo = screenAoTexture.Sample(screenAoSampler,
        i.position.xy * shadowOptions.zw).r;
    float ao = authoredAo * screenAo;
    color += ao * ambientAndMatcap.x * (base.rgb * (1 - metallic) * indirectDiffuse +
        prefilteredSpecular * (f0 * dfg.x + f90 * dfg.y) * energyCompensation);
    // Camera-space material capture adds broad studio highlights to glossy
    // surfaces without requiring a real environment map or extra geometry.
    float2 matcapUV = float2(dot(r,cameraRight.xyz),-dot(r,cameraUp.xyz)) * 0.5 + 0.5;
    float3 matcap = matcapTexture.Sample(matcapSampler,matcapUV).rgb;
    color += matcap * fresnel(saturate(dot(n,v)),f0,f90) *
        (1 - 0.7 * roughness) * ambientAndMatcap.y;
    float3 emissive = emissiveTexture.Sample(emissiveSampler, i.uv).rgb *
        emissiveAndStrength.rgb * emissiveAndStrength.a;
    if (flags.x > 0.5) emissive += base.rgb * emissiveAndStrength.a;
    if (flags.y > 0.5) emissive += base.rgb * emissiveAndStrength.a;
    // OPAQUE and MASK materials cover their surviving samples completely.
    // Only BLEND materials contribute partial alpha within a covered sample.
    float alpha = flags.z > 1.5 ? lerp(base.a,0.08,flags.w) : 1.0;
    return float4(color + emissive, alpha);
}
