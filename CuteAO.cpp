#include "CuteAO.h"
#include "AEFX_SuiteHandlerTemplate.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <thread>
#include <vector>

namespace {

constexpr const char* kEffectName = "cute-AO";
constexpr const char* kMatchName = "multipleWives cute-AO";
constexpr const char* kCategory = "Stylize";
constexpr float kPi = 3.14159265358979323846f;
constexpr float kHalfPi = 1.57079632679489661923f;
constexpr float kFarPlane = 1000.0f;
constexpr int kSectorCount = 32;

struct Vec2 { float x, y; };
struct Vec3 { float x, y, z; };

Vec2 operator+(Vec2 a, Vec2 b) { return {a.x + b.x, a.y + b.y}; }
Vec2 operator-(Vec2 a, Vec2 b) { return {a.x - b.x, a.y - b.y}; }
Vec2 operator*(Vec2 a, float s) { return {a.x * s, a.y * s}; }
Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 operator-(Vec3 a) { return {-a.x, -a.y, -a.z}; }
Vec3 operator*(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
Vec3& operator+=(Vec3& a, Vec3 b) { a = a + b; return a; }

float Clamp01(float v) { return std::max(0.0f, std::min(1.0f, v)); }
float Dot(Vec3 a, Vec3 b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
float Length(Vec3 v) { return std::sqrt(std::max(0.0f, Dot(v, v))); }
Vec3 Normalize(Vec3 v) {
    const float len = Length(v);
    return len > 1.0e-12f ? v * (1.0f / len) : Vec3{0.0f, 0.0f, -1.0f};
}
Vec3 Cross(Vec3 a, Vec3 b) {
    return {a.y*b.z - a.z*b.y, a.z*b.x - a.x*b.z, a.x*b.y - a.y*b.x};
}
float SmoothStep(float edge0, float edge1, float x) {
    const float denom = edge1 - edge0;
    if (std::fabs(denom) < 1.0e-8f) return x < edge0 ? 0.0f : 1.0f;
    const float t = Clamp01((x - edge0) / denom);
    return t * t * (3.0f - 2.0f * t);
}
float FastAcos(float x) {
    x = std::max(-1.0f, std::min(1.0f, x));
    float result = -0.156583f * std::fabs(x) + kHalfPi;
    result *= std::sqrt(std::max(0.0f, 1.0f - std::fabs(x)));
    return x >= 0.0f ? result : kPi - result;
}

struct Settings {
    float intensity;
    float thickness;
    float radius;
    float render_scale;
    A_long ray_steps;
    A_long sample_count;
    float fade_start;
    float fade_end;
    float blur_sharpness;
    PF_Boolean enable_taa;
    float temporal_stability;
    PF_Boolean enable_depth_multiplier;
    float depth_multiplier;
    float sky_threshold;
    float fov;
    PF_Pixel occlusion_color;
    A_long view_mode;
    A_long depth_encoding;
    float depth_smoothing;
    A_long frame_index;
};

void UnionRect(const PF_LRect& source, PF_LRect* destination) {
    if (source.left < destination->left) destination->left = source.left;
    if (source.top < destination->top) destination->top = source.top;
    if (source.right > destination->right) destination->right = source.right;
    if (source.bottom > destination->bottom) destination->bottom = source.bottom;
}

template <typename Pixel>
const Pixel* PixelAt(const PF_EffectWorld& world, A_long x, A_long y) {
    if (!world.data || world.width <= 0 || world.height <= 0) return nullptr;
    x = std::max<A_long>(0, std::min<A_long>(x, world.width - 1));
    y = std::max<A_long>(0, std::min<A_long>(y, world.height - 1));
    const auto* row = reinterpret_cast<const A_u_char*>(world.data) + y * world.rowbytes;
    return reinterpret_cast<const Pixel*>(row) + x;
}

template <typename Pixel>
float RawDepthNearest(const PF_EffectWorld& world, A_long x, A_long y, float channel_scale) {
    const Pixel* p = PixelAt<Pixel>(world, x, y);
    if (!p) return 1.0f;
    return (0.2126f * static_cast<float>(p->red) +
            0.7152f * static_cast<float>(p->green) +
            0.0722f * static_cast<float>(p->blue)) / channel_scale;
}

template <typename Pixel>
float RawDepth(const PF_EffectWorld& world, float u, float v, float channel_scale) {
    const float x = Clamp01(u) * static_cast<float>(world.width) - 0.5f;
    const float y = Clamp01(v) * static_cast<float>(world.height) - 0.5f;
    const A_long x0 = static_cast<A_long>(std::floor(x));
    const A_long y0 = static_cast<A_long>(std::floor(y));
    const float tx = x - static_cast<float>(x0);
    const float ty = y - static_cast<float>(y0);
    const float d00 = RawDepthNearest<Pixel>(world, x0, y0, channel_scale);
    const float d10 = RawDepthNearest<Pixel>(world, x0 + 1, y0, channel_scale);
    const float d01 = RawDepthNearest<Pixel>(world, x0, y0 + 1, channel_scale);
    const float d11 = RawDepthNearest<Pixel>(world, x0 + 1, y0 + 1, channel_scale);
    return (d00 + (d10 - d00) * tx) +
           ((d01 + (d11 - d01) * tx) - (d00 + (d10 - d00) * tx)) * ty;
}

float DecodeDepth(float raw, A_long encoding) {
    if (encoding == 2) {
        return (1.0f - raw) / std::max(1.0e-6f, 1.0f + raw * (kFarPlane - 1.0f));
    }
    if (encoding == 3) {
        return raw / std::max(1.0e-6f, kFarPlane - raw * (kFarPlane - 1.0f));
    }
    return raw;
}

template <typename Pixel>
float SampleDepth(const PF_EffectWorld& world, float u, float v, float channel_scale, const Settings& s) {
    float raw = RawDepth<Pixel>(world, u, v, channel_scale);
    if (s.depth_smoothing > 0.001f && world.width > 1 && world.height > 1) {
        const float ox = s.depth_smoothing / static_cast<float>(world.width);
        const float oy = s.depth_smoothing / static_cast<float>(world.height);
        raw = (raw * 4.0f +
               RawDepth<Pixel>(world, u - ox, v, channel_scale) +
               RawDepth<Pixel>(world, u + ox, v, channel_scale) +
               RawDepth<Pixel>(world, u, v - oy, channel_scale) +
               RawDepth<Pixel>(world, u, v + oy, channel_scale)) * 0.125f;
    }
    return DecodeDepth(raw, s.depth_encoding);
}

Vec3 ViewPosition(float u, float v, float view_z, Vec2 pscale) {
    const float ndc_x = u * 2.0f - 1.0f;
    const float ndc_y = v * 2.0f - 1.0f;
    return {ndc_x * pscale.x * view_z, -ndc_y * pscale.y * view_z, view_z};
}

template <typename Pixel>
Vec3 ReconstructNormal(const PF_EffectWorld& depth, float u, float v, float channel_scale,
                       const Settings& s, Vec2 pscale, float depth_mult) {
    const float px = 1.0f / static_cast<float>(std::max<A_long>(1, depth.width));
    const float py = 1.0f / static_cast<float>(std::max<A_long>(1, depth.height));
    const float dc = SampleDepth<Pixel>(depth, u, v, channel_scale, s);
    const float dl = SampleDepth<Pixel>(depth, u - px, v, channel_scale, s);
    const float dr = SampleDepth<Pixel>(depth, u + px, v, channel_scale, s);
    const float dt = SampleDepth<Pixel>(depth, u, v - py, channel_scale, s);
    const float db = SampleDepth<Pixel>(depth, u, v + py, channel_scale, s);
    const Vec3 pc = ViewPosition(u, v, dc * kFarPlane * depth_mult, pscale);
    Vec3 pl = ViewPosition(u - px, v, dl * kFarPlane * depth_mult, pscale);
    Vec3 pr = ViewPosition(u + px, v, dr * kFarPlane * depth_mult, pscale);
    Vec3 pt = ViewPosition(u, v - py, dt * kFarPlane * depth_mult, pscale);
    Vec3 pb = ViewPosition(u, v + py, db * kFarPlane * depth_mult, pscale);

    const float z[4] = {pl.z, pr.z, pt.z, pb.z};
    float e[4] = {z[0]-pc.z, z[1]-pc.z, z[2]-pc.z, z[3]-pc.z};
    const float paired[4] = {e[0]+e[1], e[1]+e[0], e[2]+e[3], e[3]+e[2]};
    const float denom = std::max(1.0e-6f, std::fabs(pc.z) * 0.040f);
    for (int i=0; i<4; ++i) e[i] = Clamp01(1.3f - std::min(std::fabs(e[i]), std::fabs(paired[i])) / denom);

    pl = Normalize(pl - pc); pr = Normalize(pr - pc);
    pt = Normalize(pt - pc); pb = Normalize(pb - pc);
    Vec3 n{0.0f, 0.0f, -0.0005f};
    n += Cross(pl, pt) * (e[0]*e[2]);
    n += Cross(pt, pr) * (e[2]*e[1]);
    n += Cross(pr, pb) * (e[1]*e[3]);
    n += Cross(pb, pl) * (e[3]*e[0]);
    return Normalize(n);
}

float Bayer8x8(int x, int y, int phase) {
    static const int bayer[64] = {
        0,32,8,40,2,34,10,42,48,16,56,24,50,18,58,26,
        12,44,4,36,14,46,6,38,60,28,52,20,62,30,54,22,
        3,35,11,43,1,33,9,41,51,19,59,27,49,17,57,25,
        15,47,7,39,13,45,5,37,63,31,55,23,61,29,53,21
    };
    return static_cast<float>(bayer[((x + phase) & 7) + (((y + phase * 3) & 7) * 8)]) / 64.0f;
}

uint32_t UpdateSectors(float min_h, float max_h, uint32_t bits) {
    int start = std::max(0, std::min(31, static_cast<int>(min_h * kSectorCount)));
    int count = std::max(0, std::min(kSectorCount - start,
        static_cast<int>(std::ceil((max_h - min_h) * kSectorCount))));
    if (count <= 0) return bits;
    const uint32_t mask = count >= 32 ? 0xFFFFFFFFu : ((1u << count) - 1u) << start;
    return bits | mask;
}

int CountBits(uint32_t v) {
#if defined(_MSC_VER)
    return static_cast<int>(__popcnt(v));
#else
    int n=0; while(v){v&=v-1; ++n;} return n;
#endif
}

template <typename Pixel>
float ComputeAO(const PF_EffectWorld& depth, float u, float v, int ax, int ay, int phase,
                float channel_scale, const Settings& s, Vec2 pscale) {
    const float center_depth = SampleDepth<Pixel>(depth, u, v, channel_scale, s);
    if (center_depth >= s.sky_threshold) return 1.0f;
    const float depth_mult = s.enable_depth_multiplier
        ? 0.1f + (5.0f - 0.1f) * Clamp01(s.depth_multiplier) : 1.0f;
    Vec3 position = ViewPosition(u, v, center_depth * kFarPlane * depth_mult, pscale);
    const Vec3 normal = ReconstructNormal<Pixel>(depth, u, v, channel_scale, s, pscale, depth_mult);
    position += normal * (0.005f * depth_mult);
    const Vec3 view = Normalize(-position);
    const float random_value = Bayer8x8(ax, ay, phase);
    const float step_distance = std::max(s.radius, 0.01f) / static_cast<float>(std::max<A_long>(3, s.ray_steps));
    const float center_z = std::max(1.0e-6f, position.z);
    float total_visibility = 0.0f;

    for (int i=0; i<std::max<A_long>(1, s.sample_count); ++i) {
        const float angle = (static_cast<float>(i) + random_value) * (kPi / static_cast<float>(s.sample_count));
        const Vec2 dir{std::cos(angle), std::sin(angle)};
        const Vec3 slice_n = Normalize(Cross({dir.x, dir.y, 0.0f}, view));
        const Vec3 projected = normal - slice_n * Dot(normal, slice_n);
        const float projected_len = Length(projected);
        const float cos_n = Dot(projected * (1.0f / (projected_len + 1.0e-6f)), view);
        const Vec3 tangent = Cross(view, slice_n);
        const float sign = Dot(projected, tangent) < 0.0f ? -1.0f : 1.0f;
        const float normal_angle = -sign * FastAcos(cos_n);
        const Vec2 uv_dir{dir.x * (1.0f / pscale.x) * 0.5f,
                          dir.y * (1.0f / pscale.y) * -0.5f};
        const Vec2 uv_step = uv_dir * (step_distance / center_z);
        uint32_t sectors = 0;

        for (int side=-1; side<=1; side+=2) {
            float progress = random_value + 0.1f;
            for (int j=0; j<std::max<A_long>(3, s.ray_steps); ++j) {
                const float su = u + uv_step.x * static_cast<float>(side) * progress;
                const float sv = v + uv_step.y * static_cast<float>(side) * progress;
                if (su >= 0.0f && su <= 1.0f && sv >= 0.0f && sv <= 1.0f) {
                    const float sample_depth = SampleDepth<Pixel>(depth, su, sv, channel_scale, s);
                    const float distance = step_distance * progress;
                    const Vec3 ray_dir{dir.x * static_cast<float>(side), dir.y * static_cast<float>(side), 0.0f};
                    const Vec3 sample_ray = position + ray_dir * distance;
                    const Vec3 sample_pos = sample_ray * ((sample_depth * kFarPlane) / center_z);
                    const Vec3 delta = sample_pos - position;
                    const Vec3 delta_back = delta - view * s.thickness;
                    float h0 = FastAcos(Dot(Normalize(delta), view));
                    float h1 = FastAcos(Dot(Normalize(delta_back), view));
                    h0 = Clamp01((static_cast<float>(side) * -h0 - normal_angle + kHalfPi) / kPi);
                    h1 = Clamp01((static_cast<float>(side) * -h1 - normal_angle + kHalfPi) / kPi);
                    sectors = UpdateSectors(std::min(h0,h1), std::max(h0,h1), sectors);
                }
                progress += 1.0f;
            }
        }
        total_visibility += 1.0f - static_cast<float>(CountBits(sectors)) / static_cast<float>(kSectorCount);
    }
    const float visibility = total_visibility / static_cast<float>(std::max<A_long>(1, s.sample_count));
    float occlusion = 1.0f - visibility;
    occlusion = std::pow(Clamp01(occlusion * s.intensity), 2.0f);
    occlusion *= SmoothStep(s.fade_end * 2.0f, s.fade_start, center_depth);
    return 1.0f - Clamp01(occlusion);
}

float SampleBuffer(const std::vector<float>& image, int width, int height, float x, float y) {
    x = std::max(0.0f, std::min(static_cast<float>(width-1), x));
    y = std::max(0.0f, std::min(static_cast<float>(height-1), y));
    const int x0=static_cast<int>(std::floor(x)), y0=static_cast<int>(std::floor(y));
    const int x1=std::min(width-1,x0+1), y1=std::min(height-1,y0+1);
    const float tx=x-x0, ty=y-y0;
    const float a=image[y0*width+x0] + (image[y0*width+x1]-image[y0*width+x0])*tx;
    const float b=image[y1*width+x0] + (image[y1*width+x1]-image[y1*width+x0])*tx;
    return a+(b-a)*ty;
}

template <typename Pixel, typename Channel>
PF_Err RenderTyped(PF_InData* in_data, const PF_EffectWorld& source, const PF_EffectWorld& depth,
                   const Settings& s, float channel_scale, Channel opaque, PF_EffectWorld* output) {
    if (!depth.data || depth.width<=0 || depth.height<=0) return PF_COPY(const_cast<PF_EffectWorld*>(&source), output, nullptr, nullptr);
    PF_Err err = PF_ABORT(in_data); if (err) return err;
    const int out_w=output->width, out_h=output->height;
    const int ao_w=std::max(1, static_cast<int>(std::lround(out_w*std::max(0.1f,std::min(1.0f,s.render_scale)))));
    const int ao_h=std::max(1, static_cast<int>(std::lround(out_h*std::max(0.1f,std::min(1.0f,s.render_scale)))));
    std::vector<float> ao(static_cast<size_t>(ao_w)*ao_h,1.0f), blurred(ao.size(),1.0f);
    const float fov_rad=s.fov*(kPi/180.0f);
    const float py=std::tan(fov_rad*0.5f);
    const Vec2 pscale{py*(static_cast<float>(out_w)/std::max(1,out_h)),py};

    const unsigned thread_count=std::max(1u,std::min<unsigned>(std::thread::hardware_concurrency(),static_cast<unsigned>(ao_h)));
    std::vector<std::thread> workers;
    for(unsigned t=0;t<thread_count;++t){
        workers.emplace_back([&,t]{
            const int y0=ao_h*static_cast<int>(t)/static_cast<int>(thread_count);
            const int y1=ao_h*static_cast<int>(t+1)/static_cast<int>(thread_count);
            for(int y=y0;y<y1;++y) for(int x=0;x<ao_w;++x){
                const float u=(x+0.5f)/ao_w, v=(y+0.5f)/ao_h;
                float value=ComputeAO<Pixel>(depth,u,v,x,y,s.frame_index,channel_scale,s,pscale);
                if(s.enable_taa){
                    const float alternate=ComputeAO<Pixel>(depth,u,v,x,y,s.frame_index-1,channel_scale,s,pscale);
                    value=value+(alternate-value)*std::max(0.0f,std::min(0.99f,s.temporal_stability));
                }
                ao[y*ao_w+x]=value;
            }
        });
    }
    for(auto& w:workers) w.join();
    err=PF_ABORT(in_data); if(err) return err;

    static const int offsets[14][2]={{0,1},{0,-1},{0,2},{0,-2},{1,0},{-1,0},{1,1},{1,-1},{-1,1},{-1,-1},{1,2},{1,-2},{-1,2},{-1,-2}};
    static const float weights[14]={.8825f,.8825f,.6065f,.6065f,.8825f,.8825f,.7788f,.7788f,.7788f,.7788f,.5353f,.5353f,.5353f,.5353f};
    workers.clear();
    for(unsigned t=0;t<thread_count;++t){
        workers.emplace_back([&,t]{
            const int y0=ao_h*static_cast<int>(t)/static_cast<int>(thread_count);
            const int y1=ao_h*static_cast<int>(t+1)/static_cast<int>(thread_count);
            for(int y=y0;y<y1;++y) for(int x=0;x<ao_w;++x){
                const float u=(x+0.5f)/ao_w,v=(y+0.5f)/ao_h;
                const float center_d=SampleDepth<Pixel>(depth,u,v,channel_scale,s);
                float total=ao[y*ao_w+x], total_w=1.0f;
                for(int k=0;k<14;++k){
                    const float sx=x+offsets[k][0]*1.5f, sy=y+offsets[k][1]*1.5f;
                    const float su=(sx+0.5f)/ao_w, sv=(sy+0.5f)/ao_h;
                    const float sd=SampleDepth<Pixel>(depth,su,sv,channel_scale,s);
                    const float w=weights[k]*std::exp(-std::fabs(center_d-sd)*(1000.0f*s.blur_sharpness));
                    total+=SampleBuffer(ao,ao_w,ao_h,sx,sy)*w; total_w+=w;
                }
                blurred[y*ao_w+x]=total/total_w;
            }
        });
    }
    for(auto& w:workers) w.join();

    const float occ_r=s.occlusion_color.red/255.0f, occ_g=s.occlusion_color.green/255.0f, occ_b=s.occlusion_color.blue/255.0f;
    for(int y=0;y<out_h;++y){
        auto* row=reinterpret_cast<A_u_char*>(output->data)+y*output->rowbytes;
        auto* outp=reinterpret_cast<Pixel*>(row);
        for(int x=0;x<out_w;++x,++outp){
            const float u=(x+0.5f)/out_w,v=(y+0.5f)/out_h;
            const float visibility=SampleBuffer(blurred,ao_w,ao_h,u*ao_w-0.5f,v*ao_h-0.5f);
            const Pixel* src=PixelAt<Pixel>(source,x,y);
            float r=0,g=0,b=0; Channel alpha=opaque;
            if(s.view_mode==2){r=g=b=visibility;}
            else if(s.view_mode==3){r=g=b=Clamp01(SampleDepth<Pixel>(depth,u,v,channel_scale,s));}
            else if(src){
                r=static_cast<float>(src->red)/channel_scale; g=static_cast<float>(src->green)/channel_scale; b=static_cast<float>(src->blue)/channel_scale; alpha=src->alpha;
                r=(r*visibility)*visibility+occ_r*(1.0f-visibility);
                g=(g*visibility)*visibility+occ_g*(1.0f-visibility);
                b=(b*visibility)*visibility+occ_b*(1.0f-visibility);
            }
            auto cv=[&](float q)->Channel{return static_cast<Channel>(Clamp01(q)*channel_scale+(channel_scale>1?0.5f:0.0f));};
            outp->alpha=alpha; outp->red=cv(r); outp->green=cv(g); outp->blue=cv(b);
        }
    }
    return PF_Err_NONE;
}

PF_Err RenderForFormat(PF_InData* in_data, PF_OutData* out_data, const PF_EffectWorld& source,
                       const PF_EffectWorld& depth, const Settings& s, PF_EffectWorld* output){
    AEFX_SuiteScoper<PF_WorldSuite2> ws(in_data,kPFWorldSuite,kPFWorldSuiteVersion2,out_data);
    PF_PixelFormat format=PF_PixelFormat_INVALID; PF_Err err=ws->PF_GetPixelFormat(output,&format); if(err)return err;
    if(format==PF_PixelFormat_ARGB128) return RenderTyped<PF_PixelFloat,PF_FpShort>(in_data,source,depth,s,1.0f,1.0f,output);
    if(format==PF_PixelFormat_ARGB64) return RenderTyped<PF_Pixel16,A_u_short>(in_data,source,depth,s,static_cast<float>(PF_MAX_CHAN16),static_cast<A_u_short>(PF_MAX_CHAN16),output);
    if(format==PF_PixelFormat_ARGB32) return RenderTyped<PF_Pixel8,A_u_char>(in_data,source,depth,s,static_cast<float>(PF_MAX_CHAN8),static_cast<A_u_char>(PF_MAX_CHAN8),output);
    return PF_Err_BAD_CALLBACK_PARAM;
}

void FillSettings(PF_ParamDef* p[], PF_InData* in, Settings* s){
    s->intensity=static_cast<float>(p[NEO_INTENSITY]->u.fs_d.value); s->thickness=static_cast<float>(p[NEO_THICKNESS]->u.fs_d.value);
    s->radius=static_cast<float>(p[NEO_RADIUS]->u.fs_d.value); s->render_scale=static_cast<float>(p[NEO_RENDER_SCALE]->u.fs_d.value);
    s->ray_steps=p[NEO_RAY_STEPS]->u.sd.value; s->sample_count=p[NEO_SAMPLE_COUNT]->u.sd.value;
    s->fade_start=static_cast<float>(p[NEO_FADE_START]->u.fs_d.value); s->fade_end=static_cast<float>(p[NEO_FADE_END]->u.fs_d.value);
    s->blur_sharpness=static_cast<float>(p[NEO_BLUR_SHARPNESS]->u.fs_d.value); s->enable_taa=static_cast<PF_Boolean>(p[NEO_ENABLE_TAA]->u.bd.value != 0);
    s->temporal_stability=static_cast<float>(p[NEO_TEMPORAL_STABILITY]->u.fs_d.value); s->enable_depth_multiplier=static_cast<PF_Boolean>(p[NEO_ENABLE_DEPTH_MULTIPLIER]->u.bd.value != 0);
    s->depth_multiplier=static_cast<float>(p[NEO_DEPTH_MULTIPLIER]->u.fs_d.value); s->sky_threshold=static_cast<float>(p[NEO_DEPTH_THRESHOLD]->u.fs_d.value);
    s->fov=static_cast<float>(p[NEO_FOV]->u.fs_d.value); s->occlusion_color=p[NEO_OCCLUSION_COLOR]->u.cd.value;
    s->view_mode=p[NEO_VIEW_MODE]->u.pd.value; s->depth_encoding=p[NEO_DEPTH_ENCODING]->u.pd.value;
    s->depth_smoothing=static_cast<float>(p[NEO_DEPTH_SMOOTHING]->u.fs_d.value);
    s->frame_index=in->time_step?static_cast<A_long>(in->current_time/in->time_step):0;
}

PF_Err About(PF_OutData* out){wsprintfA(out->return_msg,"%s v1.0\rAmbient occlusion from a depth map for After Effects.",kEffectName);return PF_Err_NONE;}
PF_Err GlobalSetup(PF_OutData* out){out->my_version=PF_VERSION(CUTE_AO_MAJOR_VERSION,CUTE_AO_MINOR_VERSION,CUTE_AO_BUG_VERSION,CUTE_AO_STAGE_VERSION,CUTE_AO_BUILD_VERSION);out->out_flags=PF_OutFlag_DEEP_COLOR_AWARE;out->out_flags2=PF_OutFlag2_SUPPORTS_SMART_RENDER|PF_OutFlag2_FLOAT_COLOR_AWARE;return PF_Err_NONE;}

PF_Err ParamsSetup(PF_InData* in_data,PF_OutData* out_data){
    PF_ParamDef def; AEFX_CLR_STRUCT(def);
    PF_ADD_LAYER("Depth Layer",PF_LayerDefault_NONE,NEO_DEPTH_DISK_ID); AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Intensity",0,5,0,5,1,PF_Precision_HUNDREDTHS,0,0,NEO_INTENSITY_DISK_ID); AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Thickness",.1,5,.1,5,.5,PF_Precision_HUNDREDTHS,0,0,NEO_THICKNESS_DISK_ID); AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Radius",0,4,0,4,1,PF_Precision_HUNDREDTHS,0,0,NEO_RADIUS_DISK_ID); AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Render Resolution",.1,1,.1,1,1,PF_Precision_HUNDREDTHS,0,0,NEO_RENDER_SCALE_DISK_ID); AEFX_CLR_STRUCT(def);
    PF_ADD_SLIDER("Ray Steps",3,32,3,32,8,NEO_RAY_STEPS_DISK_ID); AEFX_CLR_STRUCT(def);
    PF_ADD_SLIDER("Slice Count",1,16,1,16,4,NEO_SAMPLE_COUNT_DISK_ID); AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Fade Start",0,1,0,1,0,PF_Precision_HUNDREDTHS,0,0,NEO_FADE_START_DISK_ID); AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Fade End",0,1,0,1,.5,PF_Precision_HUNDREDTHS,0,0,NEO_FADE_END_DISK_ID); AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Blur Sharpness",0,5,0,5,1,PF_Precision_TENTHS,0,0,NEO_BLUR_SHARPNESS_DISK_ID); AEFX_CLR_STRUCT(def);
    PF_ADD_CHECKBOXX("Enable TAA",FALSE,0,NEO_ENABLE_TAA_DISK_ID); AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Temporal Stability",0,.99,0,.99,.85,PF_Precision_HUNDREDTHS,0,0,NEO_TEMPORAL_STABILITY_DISK_ID); AEFX_CLR_STRUCT(def);
    PF_ADD_CHECKBOXX("Enable Depth Multiplier",FALSE,0,NEO_ENABLE_DEPTH_MULTIPLIER_DISK_ID); AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Depth Multiplier",0,1,0,1,1,PF_Precision_HUNDREDTHS,0,0,NEO_DEPTH_MULTIPLIER_DISK_ID); AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Sky Threshold",0,1,0,1,.999,PF_Precision_THOUSANDTHS,0,0,NEO_DEPTH_THRESHOLD_DISK_ID); AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Field of View (Vertical)",1,120,1,120,75,PF_Precision_TENTHS,0,0,NEO_FOV_DISK_ID); AEFX_CLR_STRUCT(def);
    PF_ADD_COLOR("Occlusion Color",0,0,0,NEO_OCCLUSION_COLOR_DISK_ID); AEFX_CLR_STRUCT(def);
    PF_ADD_POPUP("View Mode",3,1,"None|AO Only|Depth",NEO_VIEW_MODE_DISK_ID); AEFX_CLR_STRUCT(def);
    PF_ADD_POPUP("Depth Encoding",3,1,"Linear|Reversed-Z|Forward-Z",NEO_DEPTH_ENCODING_DISK_ID); AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Depth Smoothing",0,3,0,3,0,PF_Precision_HUNDREDTHS,0,0,NEO_DEPTH_SMOOTHING_DISK_ID);
    out_data->num_params=NEO_NUM_PARAMS; return PF_Err_NONE;
}

PF_Err LegacyRender(PF_InData* in,PF_OutData* out,PF_ParamDef* p[],PF_LayerDef* output){Settings s{};FillSettings(p,in,&s);return RenderForFormat(in,out,p[NEO_INPUT]->u.ld,p[NEO_DEPTH]->u.ld,s,output);}

PF_Err SmartPreRender(PF_InData* in,PF_OutData* out,PF_PreRenderExtra* extra){
    PF_Err err=PF_Err_NONE; PF_RenderRequest req=extra->input->output_request; PF_CheckoutResult src{},dep{};
    AEFX_SuiteScoper<PF_HandleSuite1> hs(in,kPFHandleSuite,kPFHandleSuiteVersion1,out); PF_Handle h=hs->host_new_handle(sizeof(Settings)); if(!h)return PF_Err_OUT_OF_MEMORY;
    auto* s=reinterpret_cast<Settings*>(hs->host_lock_handle(h)); if(!s){hs->host_dispose_handle(h);return PF_Err_OUT_OF_MEMORY;} extra->output->pre_render_data=h;
    PF_ParamDef pdefs[NEO_NUM_PARAMS]{}; PF_ParamDef* pp[NEO_NUM_PARAMS]{};
    for(int i=1;i<NEO_NUM_PARAMS;++i){pp[i]=&pdefs[i]; if(!err)err=PF_CHECKOUT_PARAM(in,i,in->current_time,in->time_step,in->time_scale,pp[i]);}
    if(!err)FillSettings(pp,in,s);
    if(!err)err=extra->cb->checkout_layer(in->effect_ref,NEO_INPUT,NEO_INPUT,&req,in->current_time,in->time_step,in->time_scale,&src);
    if(!err){UnionRect(src.result_rect,&extra->output->result_rect);UnionRect(src.max_result_rect,&extra->output->max_result_rect);err=extra->cb->checkout_layer(in->effect_ref,NEO_DEPTH,NEO_DEPTH,&req,in->current_time,in->time_step,in->time_scale,&dep);}
    hs->host_unlock_handle(h); return err;
}

PF_Err SmartRender(PF_InData* in,PF_OutData* out,PF_SmartRenderExtra* extra){
    PF_Err err=PF_Err_NONE,err2=PF_Err_NONE; PF_EffectWorld *src=nullptr,*dep=nullptr,*dst=nullptr;
    AEFX_SuiteScoper<PF_HandleSuite1> hs(in,kPFHandleSuite,kPFHandleSuiteVersion1,out); auto h=reinterpret_cast<PF_Handle>(extra->input->pre_render_data); auto* s=reinterpret_cast<Settings*>(hs->host_lock_handle(h)); if(!s)return PF_Err_OUT_OF_MEMORY;
    err=extra->cb->checkout_layer_pixels(in->effect_ref,NEO_INPUT,&src); if(!err)err=extra->cb->checkout_layer_pixels(in->effect_ref,NEO_DEPTH,&dep); if(!err)err=extra->cb->checkout_output(in->effect_ref,&dst);
    if(!err&&src&&dst){const PF_EffectWorld empty{};err=RenderForFormat(in,out,*src,dep?*dep:empty,*s,dst);} if(dep){err2=extra->cb->checkin_layer_pixels(in->effect_ref,NEO_DEPTH);if(!err)err=err2;} if(src){err2=extra->cb->checkin_layer_pixels(in->effect_ref,NEO_INPUT);if(!err)err=err2;} hs->host_unlock_handle(h);return err;
}

} // namespace

extern "C" DllExport PF_Err PluginDataEntryFunction2(PF_PluginDataPtr p,PF_PluginDataCB2 cb,SPBasicSuite*,const char*,const char*){PF_Err result=PF_Err_INVALID_CALLBACK;result=PF_REGISTER_EFFECT_EXT2(p,cb,kEffectName,kMatchName,kCategory,AE_RESERVED_INFO,"EffectMain","https://github.com/multipleWives/cute-AO");return result;}
PF_Err EffectMain(PF_Cmd cmd,PF_InData* in,PF_OutData* out,PF_ParamDef* p[],PF_LayerDef* output,void* extra){switch(cmd){case PF_Cmd_ABOUT:return About(out);case PF_Cmd_GLOBAL_SETUP:return GlobalSetup(out);case PF_Cmd_PARAMS_SETUP:return ParamsSetup(in,out);case PF_Cmd_RENDER:return LegacyRender(in,out,p,output);case PF_Cmd_SMART_PRE_RENDER:return SmartPreRender(in,out,reinterpret_cast<PF_PreRenderExtra*>(extra));case PF_Cmd_SMART_RENDER:return SmartRender(in,out,reinterpret_cast<PF_SmartRenderExtra*>(extra));default:return PF_Err_NONE;}}
