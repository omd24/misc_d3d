
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

PatchTess
constant_hs_uniform (
    InputPatch<VertexOut, 16> patch,
    uint patch_id : SV_PrimitiveID
) {
    PatchTess ret;

    ret.edge_tess[0] = 25;
    ret.edge_tess[1] = 25;
    ret.edge_tess[2] = 25;
    ret.edge_tess[3] = 25;

    ret.inside_tess[0] = 25;
    ret.inside_tess[1] = 25;

    return ret;
}
[domain("quad")]
[partitioning("integer")]
[outputtopology("triangle_cw")]
[outputcontrolpoints(16)]
[patchconstantfunc("constant_hs_uniform")]
[maxtessfactor(64.0f)]
HullOut
pass_through_hs_16cp (
    InputPatch<VertexOut, 16> p,
    uint i : SV_OutputControlPointID,
    uint patch_id : SV_PrimitiveID
) {
    HullOut hout;
    hout.pos_local = p[i].pos_local;
    return hout;
}
// =====================================================================
// Cubic Bezier surface helper with Bernstein basis functions
// =====================================================================
float4
bernstein_basis (float t) {
    float inv_t = 1.0f - t;
 
    return float4(
        inv_t * inv_t * inv_t,
        3.0f * t * inv_t * inv_t,
        3.0f * t * t * inv_t,
        t * t * t
    );

}
float3
cubic_bezier_sum (OutputPatch<HullOut, 16> bez_patch, float4 basis_u, float4 basis_v) {
    float3 sum = float3(0.0f, 0.0f, 0.0f);
    sum = basis_v.x * (
        basis_u.x * bez_patch[0].pos_local +
        basis_u.y * bez_patch[1].pos_local +
        basis_u.z * bez_patch[2].pos_local +
        basis_u.w * bez_patch[3].pos_local);

    sum += basis_v.y * (
        basis_u.x * bez_patch[4].pos_local +
        basis_u.y * bez_patch[5].pos_local +
        basis_u.z * bez_patch[6].pos_local +
        basis_u.w * bez_patch[7].pos_local);

    sum += basis_v.z * (
        basis_u.x * bez_patch[8].pos_local +
        basis_u.y * bez_patch[9].pos_local +
        basis_u.z * bez_patch[10].pos_local +
        basis_u.w * bez_patch[11].pos_local);

    sum += basis_v.w * (
        basis_u.x * bez_patch[12].pos_local +
        basis_u.y * bez_patch[13].pos_local +
        basis_u.z * bez_patch[14].pos_local +
        basis_u.w * bez_patch[15].pos_local);

    return sum;
}
// derivate is useful for tangent vector and normal vector computations
float4
bernstein_basis_derivative (float t) {
    float inv_t = 1.0f - t;

    return float4(
        -3.0f * inv_t * inv_t,
        -6.0f * t * inv_t + 3.0f * inv_t * inv_t,
         6.0f * t * inv_t - 3.0f * t * t,
         3.0f * t * t
    );
}
[domain("quad")]
DomainOut
bezier_ds (
    PatchTess tess,
    float2 uv : SV_DomainLocation,
    OutputPatch<HullOut, 16> bez_patch
) {
    DomainOut ret;

    float4 basis_u = bernstein_basis(uv.x);
    float4 basis_v = bernstein_basis(uv.y);

    float3 p = cubic_bezier_sum(bez_patch, basis_u, basis_v);

    float4 pos_world = mul(float4(p, 1.0f), global_world);
    ret.pos_homogenous_clip_space = mul(pos_world, global_view_proj);

    return ret;
}

