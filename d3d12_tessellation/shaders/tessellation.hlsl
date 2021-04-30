
#include "light_utils.hlsl"

Texture2D global_diffuse_map : register(t0);

SamplerState global_sam_point_wrap : register(s0);
SamplerState global_sam_point_clamp : register(s1);
SamplerState global_sam_linear_wrap : register(s2);
SamplerState global_sam_linear_clamp : register(s3);
SamplerState global_sam_anisotropic_wrap : register(s4);
SamplerState global_sam_anisotropic_clamp : register(s5);

cbuffer PerObjectConstantBuffer : register(b0){
    float4x4 global_world;
    float4x4 global_tex_transform;
}
cbuffer PerPassConstantBuffer : register(b1){
    float4x4 global_view;
    float4x4 global_inv_view;
    float4x4 global_proj;
    float4x4 global_inv_proj;
    float4x4 global_view_proj;
    float4x4 global_inv_view_proj;
    float3 global_eye_pos_w;
    float cb_per_obj_padding1;
    float2 global_render_target_size;
    float2 global_inv_render_target_size;
    float global_near_z;
    float global_far_z;
    float global_total_time;
    float global_delta_time;
    float4 global_ambient_light;
    
    // Allow application to change fog parameters once per frame.
    // For example, we may only use fog for certain times of day.
    float4 global_fog_color;
    float global_fog_start;
    float global_fog_range;
    float2 cb_per_obj_padding2;

    // Indices [0, NUM_DIR_LIGHTS) are directional lights;
    // indices [NUM_DIR_LIGHTS, NUM_DIR_LIGHTS+NUM_POINT_LIGHTS) are point lights;
    // indices [NUM_DIR_LIGHTS+NUM_POINT_LIGHTS, NUM_DIR_LIGHTS+NUM_POINT_LIGHT+NUM_SPOT_LIGHTS)
    // are spot lights for a maximum of MAX_LIGHTS per object.
    Light global_lights[MAX_LIGHTS];
}
cbuffer MaterialConstantBuffer : register(b2){
    float4 global_diffuse_albedo;
    float3 global_fresnel_r0;
    float global_roughness;
    float4x4 global_mat_transform;
};

struct VertexIn {
    float3 pos_local : POSITION;
};
struct VertexOut {
    float3 pos_local : Position;
};
struct PatchTess {
    float edge_tess[4] : SV_TessFactor;
    float inside_tess[2] : SV_InsideTessFactor;
};
struct HullOut {
    float3 pos_local : POSITION;
};
struct DomainOut {
    float4 pos_homogenous_clip_space : SV_Position;
};
VertexOut
pass_through_vs (VertexIn vin) {
    VertexOut vout;
    vout.pos_local = vin.pos_local;
    return vout;
}
PatchTess
constant_hs (
    InputPatch<VertexOut, 4> patch,
    uint patch_id : SV_PrimitiveID
) {
    PatchTess ret;
    float3 center_local = 0.25f * (patch[0].pos_local + patch[1].pos_local + patch[2].pos_local + patch[3].pos_local);
    float3 center_world = mul(float4(center_local, 1.0f), global_world).xyz;

    // -- tessellation factors based on center of patch distance from camera
    // -- for d > d1 tessellation is 0 (far away) and for d < d0 tessellation is 64 (close to eye)
    // -- a distnace-based linear function for [d1, d0]
    // -- similar to falloff function used in light attenuation
    float d = distance(center_world, global_eye_pos_w);
    float d0 = 20.0f;
    float d1 = 100.0f;
    float tess = 64.0f * saturate((d1 - d) / (d1 - d0));
 
    // -- uniformly tessellate patch
    ret.edge_tess[0] = tess;
    ret.edge_tess[1] = tess;
    ret.edge_tess[2] = tess;
    ret.edge_tess[3] = tess;
 
    ret.inside_tess[0] = tess;
    ret.inside_tess[1] = tess;
 
    return ret;
}
[domain("quad")]
[partitioning("integer")]
[outputtopology("triangle_cw")]
[outputcontrolpoints(4)]
[patchconstantfunc("constant_hs")]
[maxtessfactor(64.0f)]
HullOut
pass_through_hs (
    InputPatch<VertexOut, 4> p,
    uint i : SV_OutputControlPointID,
    uint patch_id : SV_PrimitiveID
) {
    HullOut hout;
    hout.pos_local = p[i].pos_local;
    return hout;
}

[domain("quad")]
DomainOut
domain_shader (
    PatchTess tess,
    float2 uv : SV_DomainLocation,
    OutputPatch<HullOut, 4> quad
) {
    DomainOut ret;

    // -- bilinear interpolation
    float3 v1 = lerp(quad[0].pos_local, quad[1].pos_local, uv.x);
    float3 v2 = lerp(quad[2].pos_local, quad[3].pos_local, uv.x);
    float3 p = lerp(v1, v2, uv.y);

    // -- displacement mapping (similar to hills calculation in wave samples)
    p.y = 0.3f * (p.z * sin(p.x) + p.x * cos(p.z));

    float4 pos_world = mul(float4(p, 1.0f), global_world);
    ret.pos_homogenous_clip_space = mul(pos_world, global_view_proj);

    return ret;
}

float4
pixel_shader (DomainOut pin) : SV_Target {
    return float4(1.0f, 1.0f, 1.0f, 1.0f);
}
