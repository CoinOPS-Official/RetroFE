#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

// Small, deterministic CPU reference preprocessing for the built-in environment.
// The runtime shader follows Filament's diffuse irradiance and split-sum GGX IBL.
namespace ibl {
constexpr float pi = 3.14159265358979323846f;
struct Vec3 {
    float x = 0, y = 0, z = 0;
    Vec3 operator+(Vec3 b) const { return {x+b.x,y+b.y,z+b.z}; }
    Vec3 operator-(Vec3 b) const { return {x-b.x,y-b.y,z-b.z}; }
    Vec3 operator*(float s) const { return {x*s,y*s,z*s}; }
    Vec3& operator+=(Vec3 b) { x += b.x; y += b.y; z += b.z; return *this; }
};
inline float dot(Vec3 a, Vec3 b) { return a.x*b.x+a.y*b.y+a.z*b.z; }
inline Vec3 cross(Vec3 a, Vec3 b) {
    return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};
}
inline Vec3 normalized(Vec3 v) { return v * (1.0f/std::sqrt(std::max(dot(v,v),1e-12f))); }
inline uint8_t unorm(float c) {
    return static_cast<uint8_t>(std::lround(std::clamp(c,0.0f,1.0f)*255.0f));
}
inline Vec3 direction(float u, float v) {
    const float phi = (u-0.5f)*2*pi, theta = v*pi;
    return {std::cos(phi)*std::sin(theta),std::cos(theta),std::sin(phi)*std::sin(theta)};
}
inline float radicalInverse(uint32_t bits) {
    bits = (bits << 16) | (bits >> 16);
    bits = ((bits & 0x55555555u) << 1) | ((bits & 0xaaaaaaaau) >> 1);
    bits = ((bits & 0x33333333u) << 2) | ((bits & 0xccccccccu) >> 2);
    bits = ((bits & 0x0f0f0f0fu) << 4) | ((bits & 0xf0f0f0f0u) >> 4);
    bits = ((bits & 0x00ff00ffu) << 8) | ((bits & 0xff00ff00u) >> 8);
    return float(bits) * 2.3283064365386963e-10f;
}
inline Vec3 ggxHalfVector(uint32_t sample, uint32_t count, float roughness) {
    const float a = roughness*roughness, a2 = a*a;
    const float phi = 2*pi*float(sample)/float(count);
    const float xi = radicalInverse(sample);
    const float cosTheta = std::sqrt((1-xi)/(1+(a2-1)*xi));
    const float sinTheta = std::sqrt(std::max(0.0f,1-cosTheta*cosTheta));
    return {std::cos(phi)*sinTheta,std::sin(phi)*sinTheta,cosTheta};
}
inline float smithVisibility(float noV, float noL, float roughness) {
    const float a = roughness*roughness, a2 = a*a;
    const float ggxL = noV*std::sqrt((noL-noL*a2)*noL+a2);
    const float ggxV = noL*std::sqrt((noV-noV*a2)*noV+a2);
    return 0.5f/std::max(ggxL+ggxV,1e-6f);
}
struct Maps {
    static constexpr int irradianceWidth = 32, irradianceHeight = 16;
    static constexpr int dfgSize = 64;
    // RGB radiance stays linear and unclamped, including values above 1.
    std::vector<float> irradiance;
    std::vector<std::vector<float>> specular;
    std::vector<uint8_t> dfg;
};
inline void writePixel(std::vector<float>& out, size_t p, Vec3 c) {
    out[p] = c.x; out[p+1] = c.y; out[p+2] = c.z; out[p+3] = 1.0f;
}
inline Maps generate(const std::vector<Vec3>& source, int width, int height) {
    Maps maps;
    std::vector<Vec3> radiance(size_t(width)*height), directions(radiance.size());
    std::vector<float> solidAngles(radiance.size());
    for (int y=0; y<height; ++y) for (int x=0; x<width; ++x) {
        const size_t p = size_t(y)*width+x;
        directions[p] = direction((x+0.5f)/width,(y+0.5f)/height);
        solidAngles[p] = (2*pi/width)*(pi/height)*std::sin(pi*(y+0.5f)/height);
        radiance[p] = source[p];
    }
    const auto sample = [&](Vec3 d) {
        const float u = std::atan2(d.z,d.x)/(2*pi)+0.5f;
        const float v = std::acos(std::clamp(d.y,-1.0f,1.0f))/pi;
        const float px = u*width-0.5f, py = v*height-0.5f;
        const int x0 = static_cast<int>(std::floor(px)), y0 = static_cast<int>(std::floor(py));
        const float fx = px-x0, fy = py-y0;
        const auto texel = [&](int x, int y) {
            x = (x%width+width)%width;
            y = std::clamp(y,0,height-1);
            return radiance[size_t(y)*width+x];
        };
        return (texel(x0,y0)*(1-fx)+texel(x0+1,y0)*fx)*(1-fy)+
               (texel(x0,y0+1)*(1-fx)+texel(x0+1,y0+1)*fx)*fy;
    };
    maps.irradiance.resize(size_t(Maps::irradianceWidth)*Maps::irradianceHeight*4);
    for (int y=0; y<Maps::irradianceHeight; ++y)
        for (int x=0; x<Maps::irradianceWidth; ++x) {
            const Vec3 n = direction((x+0.5f)/Maps::irradianceWidth,
                                     (y+0.5f)/Maps::irradianceHeight);
            Vec3 sum{};
            for (size_t p=0; p<radiance.size(); ++p)
                sum += radiance[p]*(std::max(0.0f,dot(n,directions[p]))*solidAngles[p]/pi);
            writePixel(maps.irradiance,(size_t(y)*Maps::irradianceWidth+x)*4,sum);
        }
    int levels = 1;
    for (int dim=std::max(width,height); dim>1; dim>>=1) ++levels;
    maps.specular.resize(levels);
    for (int level=0; level<levels; ++level) {
        const int w=std::max(1,width>>level), h=std::max(1,height>>level);
        auto& pixels = maps.specular[level];
        pixels.resize(size_t(w)*h*4);
        const float roughness = float(level)/float(levels-1);
        for (int y=0; y<h; ++y) for (int x=0; x<w; ++x) {
            const Vec3 n = direction((x+0.5f)/w,(y+0.5f)/h);
            if (level==0) {
                writePixel(pixels,(size_t(y)*w+x)*4,sample(n));
                continue;
            }
            const Vec3 up = std::abs(n.y)<0.999f ? Vec3{0,1,0} : Vec3{1,0,0};
            const Vec3 tangent = normalized(cross(up,n)), bitangent = cross(n,tangent);
            Vec3 sum{};
            float weight = 0;
            constexpr uint32_t samples = 128;
            for (uint32_t s=0; s<samples; ++s) {
                const Vec3 localH=ggxHalfVector(s,samples,roughness);
                const Vec3 halfVector = normalized(tangent*localH.x+bitangent*localH.y+n*localH.z);
                const float voH=dot(n,halfVector);
                const Vec3 l=halfVector*(2*voH)-n;
                const float noL=std::max(0.0f,dot(n,l));
                if (noL>0) { sum += sample(l)*noL; weight += noL; }
            }
            writePixel(pixels,(size_t(y)*w+x)*4,sum*(1.0f/std::max(weight,1e-6f)));
        }
    }
    maps.dfg.resize(size_t(Maps::dfgSize)*Maps::dfgSize*4);
    for (int y=0; y<Maps::dfgSize; ++y) for (int x=0; x<Maps::dfgSize; ++x) {
        const float noV = (x+0.5f)/Maps::dfgSize;
        const float roughness = (y+0.5f)/Maps::dfgSize;
        const Vec3 v{std::sqrt(1-noV*noV),0,noV};
        float scale=0, bias=0;
        constexpr uint32_t samples=256;
        for (uint32_t s=0; s<samples; ++s) {
            const Vec3 h=ggxHalfVector(s,samples,roughness);
            const float voH=std::max(0.0f,dot(v,h));
            const Vec3 l=h*(2*voH)-v;
            const float noL=std::max(0.0f,l.z);
            if (noL<=0) continue;
            const float g=4*smithVisibility(noV,noL,roughness)*voH/std::max(h.z,1e-5f)*noL;
            const float fc=std::pow(1.0f-voH,5.0f);
            scale += (1-fc)*g;
            bias += fc*g;
        }
        const size_t p=(size_t(y)*Maps::dfgSize+x)*4;
        maps.dfg[p]=unorm(scale/samples);
        maps.dfg[p+1]=unorm(bias/samples);
        maps.dfg[p+2]=0;
        maps.dfg[p+3]=255;
    }
    return maps;
}
} // namespace ibl
