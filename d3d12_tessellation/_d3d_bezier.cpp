#include "headers/common.h"

#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <dxgidebug.h>

#include <dxcapi.h>

#include "headers/utils.h"
#include "headers/game_timer.h"
#include "headers/dds_loader.h"

#include <imgui/imgui.h>
#include <imgui/imgui_impl_win32.h>
#include <imgui/imgui_impl_dx12.h>

#if defined(_DEBUG)
#define ENABLE_DEBUG_LAYER 1
#else
#define ENABLE_DEBUG_LAYER 0
#endif

#define ENABLE_DEARIMGUI

#pragma warning (disable: 28182)    // pointer can be NULL.
#pragma warning (disable: 6011)     // dereferencing a potentially null pointer
#pragma warning (disable: 26495)    // not initializing struct members

#define NUM_BACKBUFFERS         2
#define NUM_QUEUING_FRAMES      3

enum RENDER_LAYER : int {
    LAYER_BASIC_TESS = 0,
    LAYER_BEZIER_TESS = 1,

    _COUNT_RENDERCOMPUTE_LAYER
};
enum ALL_RENDERITEMS {
    RITEM_QUAD_PATCH_4CP = 0,
    RITEM_QUAD_PATCH_16CP = 1,

    _COUNT_RENDERITEM
};
enum SHADERS_CODE {
    SHADER_DEFAULT_VS = 0,
    SHADER_OPAQUE_PS = 1,
    SHADER_4CP_HS = 2,
    SHADER_16CP_HS = 3,
    SHADER_BASIC_DS = 4,
    SHADER_BEZIER_DS = 5,

    _COUNT_SHADERS
};
enum GEOM_INDEX {
    GEOM_QUAD_PATCH_4CP = 0,
    GEOM_QUAD_PATCH_16CP = 1,

    _COUNT_GEOM
};
enum MAT_INDEX {
    MAT_WHITE = 0,

    _COUNT_MATERIAL
};
enum TEX_INDEX {
    TEX_WHITE1X1 = 0,

    _COUNT_TEX
};
enum SAMPLER_INDEX {
    SAMPLER_POINT_WRAP = 0,
    SAMPLER_POINT_CLAMP = 1,
    SAMPLER_LINEAR_WRAP = 2,
    SAMPLER_LINEAR_CLAMP = 3,
    SAMPLER_ANISOTROPIC_WRAP = 4,
    SAMPLER_ANISOTROPIC_CLAMP = 5,

    _COUNT_SAMPLER
};
struct SceneContext {
    // camera settings (spherical coordinate)
    float theta;
    float phi;
    float radius;

    // light (sun) settings
    float sun_theta;
    float sun_phi;

    // mouse position
    POINT mouse;

    // world view projection matrices
    XMFLOAT3   eye_pos;
    XMFLOAT4X4 view;
    XMFLOAT4X4 proj;

    // display-related data
    UINT width;
    UINT height;
    float aspect_ratio;
};

int global_tess_switch = 2;
GameTimer global_timer;
bool global_paused;
bool global_resizing;
SceneContext global_scene_ctx;

#if defined(ENABLE_DEARIMGUI)
bool global_imgui_enabled = true;
#else
bool global_imgui_enabled = false;
#endif // defined(ENABLE_DEARIMGUI)


struct RenderItemArray {
    RenderItem  ritems[_COUNT_RENDERITEM];
    uint32_t    size;
};
struct D3DRenderContext {

    bool msaa4x_state;
    UINT msaa4x_quality;

    // Used formats
    struct {
        DXGI_FORMAT backbuffer_format;
        DXGI_FORMAT depthstencil_format;
    };

    // Pipeline stuff
    D3D12_VIEWPORT                  viewport;
    D3D12_RECT                      scissor_rect;
    //IDXGISwapChain3 *               swapchain3;
    IDXGISwapChain *                swapchain;
    ID3D12Device *                  device;
    ID3D12RootSignature *           root_signature;
    ID3D12PipelineState *           psos[_COUNT_RENDERCOMPUTE_LAYER];

    // Command objects
    ID3D12CommandQueue *            cmd_queue;
    ID3D12CommandAllocator *        direct_cmd_list_alloc;
    ID3D12GraphicsCommandList *     direct_cmd_list;

    UINT                            rtv_descriptor_size;
    UINT                            cbv_srv_uav_descriptor_size;

    ID3D12DescriptorHeap *          rtv_heap;
    ID3D12DescriptorHeap *          dsv_heap;
    ID3D12DescriptorHeap *          srv_heap;

    PassConstants                   main_pass_constants;
    UINT                            pass_cbv_offset;

    // List of all the render items.
    RenderItemArray                 all_ritems;
    // Render items divided by PSO.
    RenderItemArray                 basictess_ritems;
    RenderItemArray                 beziersurf_ritems;

    MeshGeometry                    geom[_COUNT_GEOM];

    // Synchronization stuff
    UINT                            frame_index;
    HANDLE                          fence_event;
    ID3D12Fence *                   fence;
    FrameResource                   frame_resources[NUM_QUEUING_FRAMES];
    UINT64                          main_current_fence;

    // Each swapchain backbuffer needs a render target
    ID3D12Resource *                render_targets[NUM_BACKBUFFERS];
    UINT                            backbuffer_index;

    ID3D12Resource *                depth_stencil_buffer;

    Material                        materials[_COUNT_MATERIAL];
    Texture                         textures[_COUNT_TEX];
    IDxcBlob *                      shaders[_COUNT_SHADERS];
};
static void
load_texture (
    ID3D12Device * device,
    ID3D12GraphicsCommandList * cmd_list,
    wchar_t const * tex_path,
    Texture * out_texture
) {

    uint8_t * ddsData;
    D3D12_SUBRESOURCE_DATA * subresources;
    UINT n_subresources = 0;

    LoadDDSTextureFromFile(device, tex_path, &out_texture->resource, &ddsData, &subresources, &n_subresources);

    UINT64 upload_buffer_size = get_required_intermediate_size(out_texture->resource, 0,
                                                               n_subresources);

   // Create the GPU upload buffer.
    D3D12_HEAP_PROPERTIES heap_props = {};
    heap_props.Type = D3D12_HEAP_TYPE_UPLOAD;
    heap_props.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heap_props.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    heap_props.CreationNodeMask = 1;
    heap_props.VisibleNodeMask = 1;

    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Alignment = 0;
    desc.Width = upload_buffer_size;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    // TODO(omid): do we need to set 4x MSAA here? 
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = D3D12_RESOURCE_FLAG_NONE;

    device->CreateCommittedResource(
        &heap_props,
        D3D12_HEAP_FLAG_NONE,
        &desc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&out_texture->upload_heap)
    );

    // Use Heap-allocating UpdateSubresources implementation for variable number of subresources (which is the case for textures).
    update_subresources_heap(
        cmd_list, out_texture->resource, out_texture->upload_heap,
        0, 0, n_subresources, subresources
    );

    resource_usage_transition(
        cmd_list, out_texture->resource,
        D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
    );

    ::free(subresources);
    ::free(ddsData);
}
static void
create_materials (Material out_materials []) {

    strcpy_s(out_materials[MAT_WHITE].name, "whitemat");
    out_materials[MAT_WHITE].mat_cbuffer_index = 0;
    out_materials[MAT_WHITE].diffuse_srvheap_index = 0;
    out_materials[MAT_WHITE].diffuse_albedo = XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);
    out_materials[MAT_WHITE].fresnel_r0 = XMFLOAT3(0.05f, 0.05f, 0.05f);
    out_materials[MAT_WHITE].roughness = 0.2f;
    out_materials[MAT_WHITE].mat_transform = Identity4x4();
    out_materials[MAT_WHITE].n_frames_dirty = NUM_QUEUING_FRAMES;
}
static float
calc_hill_height (float x, float z) {
    return 0.3f * (z * sinf(0.1f * x) + x * cosf(0.1f * z));
}
static XMFLOAT3
calc_hill_normal (float x, float z) {
    // n = (-df/dx, 1, -df/dz)
    XMFLOAT3 n(
        -0.03f * z * cosf(0.1f * x) - 0.3f * cosf(0.1f * z),
        1.0f,
        -0.3f * sinf(0.1f * x) + 0.03f * x * sinf(0.1f * z)
    );

    XMVECTOR unit_normal = XMVector3Normalize(XMLoadFloat3(&n));
    XMStoreFloat3(&n, unit_normal);

    return n;
}
/*
 4_CONTROL_POINT_PATCHLIST
*/
static void
create_quad_patch_geometry_4cp (D3DRenderContext * render_ctx) {

    DirectX::XMFLOAT3 vertices[4] = {};
    vertices[0] = XMFLOAT3(-10.0f, 0.0f, +10.0f);
    vertices[1] = XMFLOAT3(+10.0f, 0.0f, +10.0f);
    vertices[2] = XMFLOAT3(-10.0f, 0.0f, -10.0f);
    vertices[3] = XMFLOAT3(+10.0f, 0.0f, -10.0f);

    uint16_t indices[4] = {0, 1, 2, 3};

    UINT nvtx = _countof(vertices);
    UINT nidx = _countof(indices);

    SubmeshGeometry quad_submesh = {};
    quad_submesh.index_count = nidx;
    quad_submesh.start_index_location = 0;
    quad_submesh.base_vertex_location = 0;

    UINT vb_byte_size = nvtx * sizeof(DirectX::XMFLOAT3);
    UINT ib_byte_size = nidx * sizeof(uint16_t);

    // -- Fill out geom
    D3DCreateBlob(vb_byte_size, &render_ctx->geom[GEOM_QUAD_PATCH_4CP].vb_cpu);
    if (vertices)
        CopyMemory(render_ctx->geom[GEOM_QUAD_PATCH_4CP].vb_cpu->GetBufferPointer(), vertices, vb_byte_size);

    D3DCreateBlob(ib_byte_size, &render_ctx->geom[GEOM_QUAD_PATCH_4CP].ib_cpu);
    if (indices)
        CopyMemory(render_ctx->geom[GEOM_QUAD_PATCH_4CP].ib_cpu->GetBufferPointer(), indices, ib_byte_size);

    create_default_buffer(render_ctx->device, render_ctx->direct_cmd_list, vertices, vb_byte_size, &render_ctx->geom[GEOM_QUAD_PATCH_4CP].vb_gpu, &render_ctx->geom[GEOM_QUAD_PATCH_4CP].vb_uploader);
    create_default_buffer(render_ctx->device, render_ctx->direct_cmd_list, indices, ib_byte_size, &render_ctx->geom[GEOM_QUAD_PATCH_4CP].ib_gpu, &render_ctx->geom[GEOM_QUAD_PATCH_4CP].ib_uploader);

    render_ctx->geom[GEOM_QUAD_PATCH_4CP].vb_byte_stide = sizeof(DirectX::XMFLOAT3);
    render_ctx->geom[GEOM_QUAD_PATCH_4CP].vb_byte_size = vb_byte_size;
    render_ctx->geom[GEOM_QUAD_PATCH_4CP].ib_byte_size = ib_byte_size;
    render_ctx->geom[GEOM_QUAD_PATCH_4CP].index_format = DXGI_FORMAT_R16_UINT;

    render_ctx->geom[GEOM_QUAD_PATCH_4CP].submesh_names[0] = "quadpatch4cp";
    render_ctx->geom[GEOM_QUAD_PATCH_4CP].submesh_geoms[0] = quad_submesh;
}
/*
 16_CONTROL_POINT_PATCHLIST
*/
static void
create_quad_patch_geometry_16cp (D3DRenderContext * render_ctx) {

    DirectX::XMFLOAT3 vertices[16] = {};
    // Row 0
    vertices[0] = XMFLOAT3(-10.0f, -10.0f, +15.0f);
    vertices[1] = XMFLOAT3(-5.0f, 0.0f, +15.0f);
    vertices[2] = XMFLOAT3(+5.0f, 0.0f, +15.0f);
    vertices[3] = XMFLOAT3(+10.0f, 0.0f, +15.0f);

    // Row 1
    vertices[4] = XMFLOAT3(-15.0f, 0.0f, +5.0f);
    vertices[5] = XMFLOAT3(-5.0f, 0.0f, +5.0f);
    vertices[6] = XMFLOAT3(+5.0f, 20.0f, +5.0f);
    vertices[7] = XMFLOAT3(+15.0f, 0.0f, +5.0f);

    // Row 2
    vertices[8] = XMFLOAT3(-15.0f, 0.0f, -5.0f);
    vertices[9] = XMFLOAT3(-5.0f, 0.0f, -5.0f);
    vertices[10] = XMFLOAT3(+5.0f, 0.0f, -5.0f);
    vertices[11] = XMFLOAT3(+15.0f, 0.0f, -5.0f);

    // Row 3
    vertices[12] = XMFLOAT3(-10.0f, 10.0f, -15.0f);
    vertices[13] = XMFLOAT3(-5.0f, 0.0f, -15.0f);
    vertices[14] = XMFLOAT3(+5.0f, 0.0f, -15.0f);
    vertices[15] = XMFLOAT3(+25.0f, 10.0f, -15.0f);

    uint16_t indices[16] = {
         0,  1,  2,  3,
         4,  5,  6,  7,
         8,  9, 10, 11,
        12, 13, 14, 15
    };

    UINT nvtx = _countof(vertices);
    UINT nidx = _countof(indices);

    SubmeshGeometry quad_submesh = {};
    quad_submesh.index_count = nidx;
    quad_submesh.start_index_location = 0;
    quad_submesh.base_vertex_location = 0;

    UINT vb_byte_size = nvtx * sizeof(DirectX::XMFLOAT3);
    UINT ib_byte_size = nidx * sizeof(uint16_t);

    // -- Fill out geom
    D3DCreateBlob(vb_byte_size, &render_ctx->geom[GEOM_QUAD_PATCH_16CP].vb_cpu);
    if (vertices)
        CopyMemory(render_ctx->geom[GEOM_QUAD_PATCH_16CP].vb_cpu->GetBufferPointer(), vertices, vb_byte_size);

    D3DCreateBlob(ib_byte_size, &render_ctx->geom[GEOM_QUAD_PATCH_16CP].ib_cpu);
    if (indices)
        CopyMemory(render_ctx->geom[GEOM_QUAD_PATCH_16CP].ib_cpu->GetBufferPointer(), indices, ib_byte_size);

    create_default_buffer(render_ctx->device, render_ctx->direct_cmd_list, vertices, vb_byte_size, &render_ctx->geom[GEOM_QUAD_PATCH_16CP].vb_gpu, &render_ctx->geom[GEOM_QUAD_PATCH_16CP].vb_uploader);
    create_default_buffer(render_ctx->device, render_ctx->direct_cmd_list, indices, ib_byte_size, &render_ctx->geom[GEOM_QUAD_PATCH_16CP].ib_gpu, &render_ctx->geom[GEOM_QUAD_PATCH_16CP].ib_uploader);

    render_ctx->geom[GEOM_QUAD_PATCH_16CP].vb_byte_stide = sizeof(DirectX::XMFLOAT3);
    render_ctx->geom[GEOM_QUAD_PATCH_16CP].vb_byte_size = vb_byte_size;
    render_ctx->geom[GEOM_QUAD_PATCH_16CP].ib_byte_size = ib_byte_size;
    render_ctx->geom[GEOM_QUAD_PATCH_16CP].index_format = DXGI_FORMAT_R16_UINT;

    render_ctx->geom[GEOM_QUAD_PATCH_16CP].submesh_names[0] = "quadpatch16cp";
    render_ctx->geom[GEOM_QUAD_PATCH_16CP].submesh_geoms[0] = quad_submesh;
}
static void
create_render_items (D3DRenderContext * render_ctx) {
    render_ctx->all_ritems.ritems[RITEM_QUAD_PATCH_4CP].world = Identity4x4();
    render_ctx->all_ritems.ritems[RITEM_QUAD_PATCH_4CP].tex_transform = Identity4x4();
    render_ctx->all_ritems.ritems[RITEM_QUAD_PATCH_4CP].obj_cbuffer_index = 0;
    render_ctx->all_ritems.ritems[RITEM_QUAD_PATCH_4CP].geometry = &render_ctx->geom[GEOM_QUAD_PATCH_4CP];
    render_ctx->all_ritems.ritems[RITEM_QUAD_PATCH_4CP].mat = &render_ctx->materials[MAT_WHITE];
    render_ctx->all_ritems.ritems[RITEM_QUAD_PATCH_4CP].primitive_type = D3D_PRIMITIVE_TOPOLOGY_4_CONTROL_POINT_PATCHLIST;
    render_ctx->all_ritems.ritems[RITEM_QUAD_PATCH_4CP].index_count = render_ctx->geom[GEOM_QUAD_PATCH_4CP].submesh_geoms[0].index_count;
    render_ctx->all_ritems.ritems[RITEM_QUAD_PATCH_4CP].start_index_loc = render_ctx->geom[GEOM_QUAD_PATCH_4CP].submesh_geoms[0].start_index_location;
    render_ctx->all_ritems.ritems[RITEM_QUAD_PATCH_4CP].base_vertex_loc = render_ctx->geom[GEOM_QUAD_PATCH_4CP].submesh_geoms[0].base_vertex_location;
    render_ctx->all_ritems.ritems[RITEM_QUAD_PATCH_4CP].n_frames_dirty = NUM_QUEUING_FRAMES;
    render_ctx->all_ritems.ritems[RITEM_QUAD_PATCH_4CP].mat->n_frames_dirty = NUM_QUEUING_FRAMES;
    render_ctx->all_ritems.ritems[RITEM_QUAD_PATCH_4CP].grid_spatial_step = 1;
    render_ctx->all_ritems.ritems[RITEM_QUAD_PATCH_4CP].displacement_map_texel_size = {1.0f, 1.0f};
    render_ctx->all_ritems.ritems[RITEM_QUAD_PATCH_4CP].initialized = true;
    render_ctx->all_ritems.size++;
    render_ctx->basictess_ritems.ritems[render_ctx->basictess_ritems.size++] =
        render_ctx->all_ritems.ritems[RITEM_QUAD_PATCH_4CP];

    render_ctx->all_ritems.ritems[RITEM_QUAD_PATCH_16CP].world = Identity4x4();
    render_ctx->all_ritems.ritems[RITEM_QUAD_PATCH_16CP].tex_transform = Identity4x4();
    render_ctx->all_ritems.ritems[RITEM_QUAD_PATCH_16CP].obj_cbuffer_index = 1;
    render_ctx->all_ritems.ritems[RITEM_QUAD_PATCH_16CP].geometry = &render_ctx->geom[GEOM_QUAD_PATCH_16CP];
    render_ctx->all_ritems.ritems[RITEM_QUAD_PATCH_16CP].mat = &render_ctx->materials[MAT_WHITE];
    render_ctx->all_ritems.ritems[RITEM_QUAD_PATCH_16CP].primitive_type = D3D_PRIMITIVE_TOPOLOGY_16_CONTROL_POINT_PATCHLIST;
    render_ctx->all_ritems.ritems[RITEM_QUAD_PATCH_16CP].index_count = render_ctx->geom[GEOM_QUAD_PATCH_16CP].submesh_geoms[0].index_count;
    render_ctx->all_ritems.ritems[RITEM_QUAD_PATCH_16CP].start_index_loc = render_ctx->geom[GEOM_QUAD_PATCH_16CP].submesh_geoms[0].start_index_location;
    render_ctx->all_ritems.ritems[RITEM_QUAD_PATCH_16CP].base_vertex_loc = render_ctx->geom[GEOM_QUAD_PATCH_16CP].submesh_geoms[0].base_vertex_location;
    render_ctx->all_ritems.ritems[RITEM_QUAD_PATCH_16CP].n_frames_dirty = NUM_QUEUING_FRAMES;
    render_ctx->all_ritems.ritems[RITEM_QUAD_PATCH_16CP].mat->n_frames_dirty = NUM_QUEUING_FRAMES;
    render_ctx->all_ritems.ritems[RITEM_QUAD_PATCH_16CP].grid_spatial_step = 1;
    render_ctx->all_ritems.ritems[RITEM_QUAD_PATCH_16CP].displacement_map_texel_size = {1.0f, 1.0f};
    render_ctx->all_ritems.ritems[RITEM_QUAD_PATCH_16CP].initialized = true;
    render_ctx->all_ritems.size++;
    render_ctx->beziersurf_ritems.ritems[render_ctx->beziersurf_ritems.size++] =
        render_ctx->all_ritems.ritems[RITEM_QUAD_PATCH_16CP];
}
// -- indexed drawing
static void
draw_render_items (
    ID3D12GraphicsCommandList * cmd_list,
    ID3D12Resource * object_cbuffer,
    ID3D12Resource * mat_cbuffer,
    UINT64 descriptor_increment_size,
    ID3D12DescriptorHeap * srv_heap,
    RenderItemArray * ritem_array,
    UINT current_frame_index
) {
    UINT objcb_byte_size = (UINT64)sizeof(ObjectConstants);
    UINT matcb_byte_size = (UINT64)sizeof(MaterialConstants);
    for (size_t i = 0; i < ritem_array->size; ++i) {
        if (ritem_array->ritems[i].initialized) {
            D3D12_VERTEX_BUFFER_VIEW vbv = Mesh_GetVertexBufferView(ritem_array->ritems[i].geometry);
            D3D12_INDEX_BUFFER_VIEW ibv = Mesh_GetIndexBufferView(ritem_array->ritems[i].geometry);
            cmd_list->IASetVertexBuffers(0, 1, &vbv);
            cmd_list->IASetIndexBuffer(&ibv);
            cmd_list->IASetPrimitiveTopology(ritem_array->ritems[i].primitive_type);

            D3D12_GPU_DESCRIPTOR_HANDLE tex = srv_heap->GetGPUDescriptorHandleForHeapStart();
            tex.ptr += descriptor_increment_size * ritem_array->ritems[i].mat->diffuse_srvheap_index;

            D3D12_GPU_VIRTUAL_ADDRESS objcb_address = object_cbuffer->GetGPUVirtualAddress();
            objcb_address += (UINT64)ritem_array->ritems[i].obj_cbuffer_index * objcb_byte_size;

            D3D12_GPU_VIRTUAL_ADDRESS matcb_address = mat_cbuffer->GetGPUVirtualAddress();
            matcb_address += (UINT64)ritem_array->ritems[i].mat->mat_cbuffer_index * matcb_byte_size;

            cmd_list->SetGraphicsRootDescriptorTable(0, tex);
            cmd_list->SetGraphicsRootConstantBufferView(1, objcb_address);
            cmd_list->SetGraphicsRootConstantBufferView(3, matcb_address);
            cmd_list->DrawIndexedInstanced(ritem_array->ritems[i].index_count, 1, ritem_array->ritems[i].start_index_loc, ritem_array->ritems[i].base_vertex_loc, 0);
        }
    }
}
static void
create_descriptor_heaps (D3DRenderContext * render_ctx) {

    // Create Shader Resource View descriptor heap
    D3D12_DESCRIPTOR_HEAP_DESC srv_heap_desc = {};
    srv_heap_desc.NumDescriptors = _COUNT_TEX +
        1;      /* imgui descriptor     */
    srv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srv_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    render_ctx->device->CreateDescriptorHeap(&srv_heap_desc, IID_PPV_ARGS(&render_ctx->srv_heap));

    // Fill out the heap with actual descriptors
    D3D12_CPU_DESCRIPTOR_HANDLE descriptor_cpu_handle = render_ctx->srv_heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc = {};

    // crate texture
    ID3D12Resource * white1x1_tex = render_ctx->textures[TEX_WHITE1X1].resource;
    memset(&srv_desc, 0, sizeof(srv_desc)); // reset desc
    srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv_desc.Format = white1x1_tex->GetDesc().Format;
    srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv_desc.Texture2D.MostDetailedMip = 0;
    srv_desc.Texture2D.MipLevels = white1x1_tex->GetDesc().MipLevels;
    srv_desc.Texture2D.ResourceMinLODClamp = 0.0f;
    //descriptor_cpu_handle.ptr += render_ctx->cbv_srv_uav_descriptor_size;   // next descriptor
    render_ctx->device->CreateShaderResourceView(white1x1_tex, &srv_desc, descriptor_cpu_handle);


    // Create Render Target View Descriptor Heap
    D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_desc = {};
    rtv_heap_desc.NumDescriptors = NUM_BACKBUFFERS + 1 /* offscreen render-target */;
    rtv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE::D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtv_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAGS::D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    render_ctx->device->CreateDescriptorHeap(&rtv_heap_desc, IID_PPV_ARGS(&render_ctx->rtv_heap));

    // Create Depth Stencil View Descriptor Heap
    D3D12_DESCRIPTOR_HEAP_DESC dsv_heap_desc;
    dsv_heap_desc.NumDescriptors = 1;
    dsv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    dsv_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    dsv_heap_desc.NodeMask = 0;
    render_ctx->device->CreateDescriptorHeap(&dsv_heap_desc, IID_PPV_ARGS(&render_ctx->dsv_heap));

}
static void
get_static_samplers (D3D12_STATIC_SAMPLER_DESC out_samplers []) {
    // 0: PointWrap
    out_samplers[SAMPLER_POINT_WRAP] = {};
    out_samplers[SAMPLER_POINT_WRAP].ShaderRegister = 0;
    out_samplers[SAMPLER_POINT_WRAP].Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    out_samplers[SAMPLER_POINT_WRAP].AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    out_samplers[SAMPLER_POINT_WRAP].AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    out_samplers[SAMPLER_POINT_WRAP].AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    out_samplers[SAMPLER_POINT_WRAP].MipLODBias = 0;
    out_samplers[SAMPLER_POINT_WRAP].MaxAnisotropy = 16;
    out_samplers[SAMPLER_POINT_WRAP].ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    out_samplers[SAMPLER_POINT_WRAP].BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
    out_samplers[SAMPLER_POINT_WRAP].MinLOD = 0.f;
    out_samplers[SAMPLER_POINT_WRAP].MaxLOD = D3D12_FLOAT32_MAX;
    out_samplers[SAMPLER_POINT_WRAP].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    out_samplers[SAMPLER_POINT_WRAP].RegisterSpace = 0;

    // 1: PointClamp
    out_samplers[SAMPLER_POINT_CLAMP] = {};
    out_samplers[SAMPLER_POINT_CLAMP].ShaderRegister = 1;
    out_samplers[SAMPLER_POINT_CLAMP].Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    out_samplers[SAMPLER_POINT_CLAMP].AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    out_samplers[SAMPLER_POINT_CLAMP].AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    out_samplers[SAMPLER_POINT_CLAMP].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    out_samplers[SAMPLER_POINT_CLAMP].MipLODBias = 0;
    out_samplers[SAMPLER_POINT_CLAMP].MaxAnisotropy = 16;
    out_samplers[SAMPLER_POINT_CLAMP].ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    out_samplers[SAMPLER_POINT_CLAMP].BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
    out_samplers[SAMPLER_POINT_CLAMP].MinLOD = 0.f;
    out_samplers[SAMPLER_POINT_CLAMP].MaxLOD = D3D12_FLOAT32_MAX;
    out_samplers[SAMPLER_POINT_CLAMP].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    out_samplers[SAMPLER_POINT_CLAMP].RegisterSpace = 0;

    // 2: LinearWrap
    out_samplers[SAMPLER_LINEAR_WRAP] = {};
    out_samplers[SAMPLER_LINEAR_WRAP].ShaderRegister = 2;
    out_samplers[SAMPLER_LINEAR_WRAP].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    out_samplers[SAMPLER_LINEAR_WRAP].AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    out_samplers[SAMPLER_LINEAR_WRAP].AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    out_samplers[SAMPLER_LINEAR_WRAP].AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    out_samplers[SAMPLER_LINEAR_WRAP].MipLODBias = 0;
    out_samplers[SAMPLER_LINEAR_WRAP].MaxAnisotropy = 16;
    out_samplers[SAMPLER_LINEAR_WRAP].ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    out_samplers[SAMPLER_LINEAR_WRAP].BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
    out_samplers[SAMPLER_LINEAR_WRAP].MinLOD = 0.f;
    out_samplers[SAMPLER_LINEAR_WRAP].MaxLOD = D3D12_FLOAT32_MAX;
    out_samplers[SAMPLER_LINEAR_WRAP].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    out_samplers[SAMPLER_LINEAR_WRAP].RegisterSpace = 0;

    // 3: LinearClamp
    out_samplers[SAMPLER_LINEAR_CLAMP] = {};
    out_samplers[SAMPLER_LINEAR_CLAMP].ShaderRegister = 3;
    out_samplers[SAMPLER_LINEAR_CLAMP].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    out_samplers[SAMPLER_LINEAR_CLAMP].AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    out_samplers[SAMPLER_LINEAR_CLAMP].AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    out_samplers[SAMPLER_LINEAR_CLAMP].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    out_samplers[SAMPLER_LINEAR_CLAMP].MipLODBias = 0;
    out_samplers[SAMPLER_LINEAR_CLAMP].MaxAnisotropy = 16;
    out_samplers[SAMPLER_LINEAR_CLAMP].ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    out_samplers[SAMPLER_LINEAR_CLAMP].BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
    out_samplers[SAMPLER_LINEAR_CLAMP].MinLOD = 0.f;
    out_samplers[SAMPLER_LINEAR_CLAMP].MaxLOD = D3D12_FLOAT32_MAX;
    out_samplers[SAMPLER_LINEAR_CLAMP].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    out_samplers[SAMPLER_LINEAR_CLAMP].RegisterSpace = 0;

    // 4: AnisotropicWrap
    out_samplers[SAMPLER_ANISOTROPIC_WRAP] = {};
    out_samplers[SAMPLER_ANISOTROPIC_WRAP].ShaderRegister = 4;
    out_samplers[SAMPLER_ANISOTROPIC_WRAP].Filter = D3D12_FILTER_ANISOTROPIC;
    out_samplers[SAMPLER_ANISOTROPIC_WRAP].AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    out_samplers[SAMPLER_ANISOTROPIC_WRAP].AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    out_samplers[SAMPLER_ANISOTROPIC_WRAP].AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    out_samplers[SAMPLER_ANISOTROPIC_WRAP].MipLODBias = 0.0f;
    out_samplers[SAMPLER_ANISOTROPIC_WRAP].MaxAnisotropy = 8;
    out_samplers[SAMPLER_ANISOTROPIC_WRAP].ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    out_samplers[SAMPLER_ANISOTROPIC_WRAP].BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
    out_samplers[SAMPLER_ANISOTROPIC_WRAP].MinLOD = 0.f;
    out_samplers[SAMPLER_ANISOTROPIC_WRAP].MaxLOD = D3D12_FLOAT32_MAX;
    out_samplers[SAMPLER_ANISOTROPIC_WRAP].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    out_samplers[SAMPLER_ANISOTROPIC_WRAP].RegisterSpace = 0;

    // 5: AnisotropicClamp
    out_samplers[SAMPLER_ANISOTROPIC_CLAMP] = {};
    out_samplers[SAMPLER_ANISOTROPIC_CLAMP].ShaderRegister = 5;
    out_samplers[SAMPLER_ANISOTROPIC_CLAMP].Filter = D3D12_FILTER_ANISOTROPIC;
    out_samplers[SAMPLER_ANISOTROPIC_CLAMP].AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    out_samplers[SAMPLER_ANISOTROPIC_CLAMP].AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    out_samplers[SAMPLER_ANISOTROPIC_CLAMP].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    out_samplers[SAMPLER_ANISOTROPIC_CLAMP].MipLODBias = 0.0f;
    out_samplers[SAMPLER_ANISOTROPIC_CLAMP].MaxAnisotropy = 8;
    out_samplers[SAMPLER_ANISOTROPIC_CLAMP].ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    out_samplers[SAMPLER_ANISOTROPIC_CLAMP].BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
    out_samplers[SAMPLER_ANISOTROPIC_CLAMP].MinLOD = 0.f;
    out_samplers[SAMPLER_ANISOTROPIC_CLAMP].MaxLOD = D3D12_FLOAT32_MAX;
    out_samplers[SAMPLER_ANISOTROPIC_CLAMP].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    out_samplers[SAMPLER_ANISOTROPIC_CLAMP].RegisterSpace = 0;
}
static void
create_root_signature (ID3D12Device * device, ID3D12RootSignature ** root_signature) {
    D3D12_DESCRIPTOR_RANGE tex_table = {};
    tex_table.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    tex_table.NumDescriptors = 1;
    tex_table.BaseShaderRegister = 0;
    tex_table.RegisterSpace = 0;
    tex_table.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER slot_root_params[4] = {};
    // NOTE(omid): Perfomance tip! Order from most frequent to least frequent.
    slot_root_params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    slot_root_params[0].DescriptorTable.NumDescriptorRanges = 1;
    slot_root_params[0].DescriptorTable.pDescriptorRanges = &tex_table;
    slot_root_params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    slot_root_params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    slot_root_params[1].Descriptor.ShaderRegister = 0;
    slot_root_params[1].Descriptor.RegisterSpace = 0;
    slot_root_params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    slot_root_params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    slot_root_params[2].Descriptor.ShaderRegister = 1;
    slot_root_params[2].Descriptor.RegisterSpace = 0;
    slot_root_params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    slot_root_params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    slot_root_params[3].Descriptor.ShaderRegister = 2;
    slot_root_params[3].Descriptor.RegisterSpace = 0;
    slot_root_params[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_STATIC_SAMPLER_DESC samplers[_COUNT_SAMPLER] = {};
    get_static_samplers(samplers);

    // A root signature is an array of root parameters.
    D3D12_ROOT_SIGNATURE_DESC root_sig_desc = {};
    root_sig_desc.NumParameters = _countof(slot_root_params);
    root_sig_desc.pParameters = slot_root_params;
    root_sig_desc.NumStaticSamplers = _COUNT_SAMPLER;
    root_sig_desc.pStaticSamplers = samplers;
    root_sig_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ID3DBlob * serialized_root_sig = nullptr;
    ID3DBlob * error_blob = nullptr;
    D3D12SerializeRootSignature(&root_sig_desc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized_root_sig, &error_blob);

    if (error_blob) {
        ::OutputDebugStringA((char*)error_blob->GetBufferPointer());
    }

    device->CreateRootSignature(0, serialized_root_sig->GetBufferPointer(), serialized_root_sig->GetBufferSize(), IID_PPV_ARGS(root_signature));
}
static HRESULT
compile_shader(wchar_t * path, wchar_t const * entry_point, wchar_t const * shader_model, DxcDefine defines [], int n_defines, IDxcBlob ** out_shader_ptr) {
    // -- using DXC shader compiler [https://asawicki.info/news_1719_two_shader_compilers_of_direct3d_12]
    HRESULT ret = E_FAIL;

    IDxcLibrary * dxc_lib = nullptr;
    ret = DxcCreateInstance(CLSID_DxcLibrary, IID_PPV_ARGS(&dxc_lib));
    // if (FAILED(ret)) Handle error
    IDxcCompiler * dxc_compiler = nullptr;
    ret = DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&dxc_compiler));
    uint32_t code_page = CP_UTF8;
    IDxcBlobEncoding * shader_blob_encoding = nullptr;
    IDxcOperationResult * dxc_res = nullptr;

    ret = dxc_lib->CreateBlobFromFile(path, &code_page, &shader_blob_encoding);
    if (shader_blob_encoding) {
        IDxcIncludeHandler * include_handler = nullptr;
        dxc_lib->CreateIncludeHandler(&include_handler);

        ret = dxc_compiler->Compile(shader_blob_encoding, path, entry_point, shader_model, nullptr, 0, defines, n_defines, include_handler, &dxc_res);
        dxc_res->GetStatus(&ret);
        dxc_res->GetResult(out_shader_ptr);
        if (FAILED(ret)) {
            if (dxc_res) {
                IDxcBlobEncoding * error_blob_encoding = nullptr;
                ret = dxc_res->GetErrorBuffer(&error_blob_encoding);
                if (SUCCEEDED(ret) && error_blob_encoding) {
                    OutputDebugStringA((const char*)error_blob_encoding->GetBufferPointer());
                    return(0);
                }
            }
            // Handle compilation error...
        }
        include_handler->Release();
    }
    shader_blob_encoding->Release();
    dxc_compiler->Release();
    dxc_lib->Release();

    _ASSERT_EXPR(*out_shader_ptr, _T("Shader Compilation Failed"));
    return ret;
}
static void
create_pso (D3DRenderContext * render_ctx) {
    // -- Create vertex-input-layout Elements

    D3D12_INPUT_ELEMENT_DESC std_input_desc[1];
    std_input_desc[0] = {};
    std_input_desc[0].SemanticName = "POSITION";
    std_input_desc[0].SemanticIndex = 0;
    std_input_desc[0].Format = DXGI_FORMAT_R32G32B32_FLOAT;
    std_input_desc[0].InputSlot = 0;
    std_input_desc[0].AlignedByteOffset = 0;
    std_input_desc[0].InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;

    //
    // -- Create PSO for Opaque objs
    //
    D3D12_BLEND_DESC def_blend_desc = {};
    def_blend_desc.AlphaToCoverageEnable = FALSE;
    def_blend_desc.IndependentBlendEnable = FALSE;
    def_blend_desc.RenderTarget[0].BlendEnable = FALSE;
    def_blend_desc.RenderTarget[0].LogicOpEnable = FALSE;
    def_blend_desc.RenderTarget[0].SrcBlend = D3D12_BLEND_ONE;
    def_blend_desc.RenderTarget[0].DestBlend = D3D12_BLEND_ZERO;
    def_blend_desc.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    def_blend_desc.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    def_blend_desc.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ZERO;
    def_blend_desc.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    def_blend_desc.RenderTarget[0].LogicOp = D3D12_LOGIC_OP_NOOP;
    def_blend_desc.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    D3D12_RASTERIZER_DESC def_rasterizer_desc = {};
    def_rasterizer_desc.FillMode = D3D12_FILL_MODE_SOLID;
    def_rasterizer_desc.CullMode = D3D12_CULL_MODE_BACK;
    def_rasterizer_desc.FrontCounterClockwise = false;
    def_rasterizer_desc.DepthBias = 0;
    def_rasterizer_desc.DepthBiasClamp = 0.0f;
    def_rasterizer_desc.SlopeScaledDepthBias = 0.0f;
    def_rasterizer_desc.DepthClipEnable = TRUE;
    def_rasterizer_desc.ForcedSampleCount = 0;
    def_rasterizer_desc.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
    def_rasterizer_desc.MultisampleEnable = render_ctx->msaa4x_state;

    /* Depth Stencil Description */
    D3D12_DEPTH_STENCIL_DESC ds_desc = {};
    ds_desc.DepthEnable = TRUE;
    ds_desc.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    ds_desc.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    ds_desc.StencilEnable = FALSE;
    ds_desc.StencilReadMask = D3D12_DEFAULT_STENCIL_READ_MASK;
    ds_desc.StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK;
    D3D12_DEPTH_STENCILOP_DESC def_stencil_op = {D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_COMPARISON_FUNC_ALWAYS};
    ds_desc.FrontFace = def_stencil_op;
    ds_desc.BackFace = def_stencil_op;

    // -- pso for basic tessellation of a quad patch with 4 control points
    D3D12_GRAPHICS_PIPELINE_STATE_DESC basic_pso_desc = {};
    basic_pso_desc.pRootSignature = render_ctx->root_signature;
    basic_pso_desc.VS.pShaderBytecode = render_ctx->shaders[SHADER_DEFAULT_VS]->GetBufferPointer();
    basic_pso_desc.VS.BytecodeLength = render_ctx->shaders[SHADER_DEFAULT_VS]->GetBufferSize();
    basic_pso_desc.PS.pShaderBytecode = render_ctx->shaders[SHADER_OPAQUE_PS]->GetBufferPointer();
    basic_pso_desc.PS.BytecodeLength = render_ctx->shaders[SHADER_OPAQUE_PS]->GetBufferSize();
    basic_pso_desc.HS.pShaderBytecode = render_ctx->shaders[SHADER_4CP_HS]->GetBufferPointer();
    basic_pso_desc.HS.BytecodeLength = render_ctx->shaders[SHADER_4CP_HS]->GetBufferSize();
    basic_pso_desc.DS.pShaderBytecode = render_ctx->shaders[SHADER_BASIC_DS]->GetBufferPointer();
    basic_pso_desc.DS.BytecodeLength = render_ctx->shaders[SHADER_BASIC_DS]->GetBufferSize();
    basic_pso_desc.BlendState = def_blend_desc;
    basic_pso_desc.SampleMask = UINT_MAX;
    basic_pso_desc.RasterizerState = def_rasterizer_desc;
    basic_pso_desc.RasterizerState.FillMode = D3D12_FILL_MODE_WIREFRAME; // to see patch tessellation output
    basic_pso_desc.DepthStencilState = ds_desc;
    basic_pso_desc.DSVFormat = render_ctx->depthstencil_format;
    basic_pso_desc.InputLayout.pInputElementDescs = std_input_desc;
    basic_pso_desc.InputLayout.NumElements = ARRAY_COUNT(std_input_desc);
    basic_pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE::D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH;
    basic_pso_desc.NumRenderTargets = 1;
    basic_pso_desc.RTVFormats[0] = render_ctx->backbuffer_format;
    basic_pso_desc.SampleDesc.Count = render_ctx->msaa4x_state ? 4 : 1;
    basic_pso_desc.SampleDesc.Quality = render_ctx->msaa4x_state ? (render_ctx->msaa4x_quality - 1) : 0;

    render_ctx->device->CreateGraphicsPipelineState(&basic_pso_desc, IID_PPV_ARGS(&render_ctx->psos[LAYER_BASIC_TESS]));

    // -- pso for bezier tessellation (cubic bezier surface with 16 control points)
    D3D12_GRAPHICS_PIPELINE_STATE_DESC bezier_pso_desc = basic_pso_desc;
    bezier_pso_desc.HS.pShaderBytecode = render_ctx->shaders[SHADER_16CP_HS]->GetBufferPointer();
    bezier_pso_desc.HS.BytecodeLength = render_ctx->shaders[SHADER_16CP_HS]->GetBufferSize();
    bezier_pso_desc.DS.pShaderBytecode = render_ctx->shaders[SHADER_BEZIER_DS]->GetBufferPointer();
    bezier_pso_desc.DS.BytecodeLength = render_ctx->shaders[SHADER_BEZIER_DS]->GetBufferSize();
    render_ctx->device->CreateGraphicsPipelineState(&bezier_pso_desc, IID_PPV_ARGS(&render_ctx->psos[LAYER_BEZIER_TESS]));
}
static void
handle_mouse_move (SceneContext * scene_ctx, WPARAM wParam, int x, int y) {

    if ((wParam & MK_LBUTTON) != 0) {
        // make each pixel correspond to a quarter of a degree
        float dx = DirectX::XMConvertToRadians(0.25f * (float)(x - scene_ctx->mouse.x));
        float dy = DirectX::XMConvertToRadians(0.25f * (float)(y - scene_ctx->mouse.y));

        // update angles (to orbit camera around)
        scene_ctx->theta += dx;
        scene_ctx->phi += dy;

        // clamp phi
        scene_ctx->phi = CLAMP_VALUE(scene_ctx->phi, 0.1f, XM_PI - 0.1f);
    } else if ((wParam & MK_RBUTTON) != 0) {
        // make each pixel correspond to a 0.2 unit in scene
        float dx = 0.2f * (float)(x - scene_ctx->mouse.x);
        float dy = 0.2f * (float)(y - scene_ctx->mouse.y);

        // update camera radius
        scene_ctx->radius += dx - dy;

        // clamp radius
        scene_ctx->radius = CLAMP_VALUE(scene_ctx->radius, 5.0f, 150.0f);
    }

    scene_ctx->mouse.x = x;
    scene_ctx->mouse.y = y;
}
static void
update_camera (SceneContext * sc) {
    // Convert Spherical to Cartesian coordinates.
    sc->eye_pos.x = sc->radius * sinf(sc->phi) * cosf(sc->theta);
    sc->eye_pos.z = sc->radius * sinf(sc->phi) * sinf(sc->theta);
    sc->eye_pos.y = sc->radius * cosf(sc->phi);

    // Build the view matrix.
    XMVECTOR pos = XMVectorSet(sc->eye_pos.x, sc->eye_pos.y, sc->eye_pos.z, 1.0f);
    XMVECTOR target = XMVectorZero();
    XMVECTOR up = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);

    XMMATRIX view = XMMatrixLookAtLH(pos, target, up);
    DirectX::XMStoreFloat4x4(&sc->view, view);
}
static void
update_obj_cbuffers (D3DRenderContext * render_ctx) {
    UINT frame_index = render_ctx->frame_index;
    UINT cbuffer_size = sizeof(ObjectConstants);
    // Only update the cbuffer data if the constants have changed.  
    // This needs to be tracked per frame resource.
    for (unsigned i = 0; i < render_ctx->all_ritems.size; i++) {
        if (
            render_ctx->all_ritems.ritems[i].n_frames_dirty > 0 &&
            render_ctx->all_ritems.ritems[i].initialized
            ) {
            UINT obj_index = render_ctx->all_ritems.ritems[i].obj_cbuffer_index;
            XMMATRIX world = DirectX::XMLoadFloat4x4(&render_ctx->all_ritems.ritems[i].world);
            XMMATRIX tex_transform = DirectX::XMLoadFloat4x4(&render_ctx->all_ritems.ritems[i].tex_transform);

            ObjectConstants obj_cbuffer = {};
            DirectX::XMStoreFloat4x4(&obj_cbuffer.world, XMMatrixTranspose(world));
            DirectX::XMStoreFloat4x4(&obj_cbuffer.tex_transform, XMMatrixTranspose(tex_transform));
            obj_cbuffer.displacement_texel_size = render_ctx->all_ritems.ritems[i].displacement_map_texel_size;
            obj_cbuffer.grid_spatial_step = render_ctx->all_ritems.ritems[i].grid_spatial_step;

            uint8_t * obj_ptr = render_ctx->frame_resources[frame_index].obj_cb_data_ptr + ((UINT64)obj_index * cbuffer_size);
            memcpy(obj_ptr, &obj_cbuffer, cbuffer_size);

            // Next FrameResource need to be updated too.
            render_ctx->all_ritems.ritems[i].n_frames_dirty--;
        }
    }
}
static void
update_mat_cbuffers (D3DRenderContext * render_ctx) {
    UINT frame_index = render_ctx->frame_index;
    UINT cbuffer_size = sizeof(MaterialConstants);
    for (int i = 0; i < _COUNT_MATERIAL; ++i) {
        // Only update the cbuffer data if the constants have changed.  If the cbuffer
        // data changes, it needs to be updated for each FrameResource.
        Material * mat = &render_ctx->materials[i];
        if (mat->n_frames_dirty > 0) {
            XMMATRIX mat_transform = DirectX::XMLoadFloat4x4(&mat->mat_transform);

            MaterialConstants mat_constants;
            mat_constants.diffuse_albedo = render_ctx->materials[i].diffuse_albedo;
            mat_constants.fresnel_r0 = render_ctx->materials[i].fresnel_r0;
            mat_constants.roughness = render_ctx->materials[i].roughness;
            DirectX::XMStoreFloat4x4(&mat_constants.mat_transform, XMMatrixTranspose(mat_transform));

            uint8_t * mat_ptr = render_ctx->frame_resources[frame_index].mat_cb_data_ptr + ((UINT64)mat->mat_cbuffer_index * cbuffer_size);
            memcpy(mat_ptr, &mat_constants, cbuffer_size);

            // Next FrameResource need to be updated too.
            mat->n_frames_dirty--;
        }
    }
}
static void
update_pass_cbuffers (D3DRenderContext * render_ctx, GameTimer * timer) {

    XMMATRIX view = DirectX::XMLoadFloat4x4(&global_scene_ctx.view);
    XMMATRIX proj = DirectX::XMLoadFloat4x4(&global_scene_ctx.proj);

    XMMATRIX view_proj = XMMatrixMultiply(view, proj);
    XMVECTOR det_view = XMMatrixDeterminant(view);
    XMMATRIX inv_view = XMMatrixInverse(&det_view, view);
    XMVECTOR det_proj = XMMatrixDeterminant(proj);
    XMMATRIX inv_proj = XMMatrixInverse(&det_proj, proj);
    XMVECTOR det_view_proj = XMMatrixDeterminant(view_proj);
    XMMATRIX inv_view_proj = XMMatrixInverse(&det_view_proj, view_proj);

    DirectX::XMStoreFloat4x4(&render_ctx->main_pass_constants.view, XMMatrixTranspose(view));
    DirectX::XMStoreFloat4x4(&render_ctx->main_pass_constants.inverse_view, XMMatrixTranspose(inv_view));
    DirectX::XMStoreFloat4x4(&render_ctx->main_pass_constants.proj, XMMatrixTranspose(proj));
    DirectX::XMStoreFloat4x4(&render_ctx->main_pass_constants.inverse_proj, XMMatrixTranspose(inv_proj));
    DirectX::XMStoreFloat4x4(&render_ctx->main_pass_constants.view_proj, XMMatrixTranspose(view_proj));
    DirectX::XMStoreFloat4x4(&render_ctx->main_pass_constants.inverse_view_proj, XMMatrixTranspose(inv_view_proj));
    render_ctx->main_pass_constants.eye_posw = global_scene_ctx.eye_pos;

    render_ctx->main_pass_constants.render_target_size = XMFLOAT2((float)global_scene_ctx.width, (float)global_scene_ctx.height);
    render_ctx->main_pass_constants.inverse_render_target_size = XMFLOAT2(1.0f / global_scene_ctx.width, 1.0f / global_scene_ctx.height);
    render_ctx->main_pass_constants.nearz = 1.0f;
    render_ctx->main_pass_constants.farz = 1000.0f;
    render_ctx->main_pass_constants.delta_time = timer->delta_time;
    render_ctx->main_pass_constants.total_time = Timer_GetTotalTime(timer);
    render_ctx->main_pass_constants.ambient_light = {.25f, .25f, .35f, 1.0f};

    render_ctx->main_pass_constants.lights[0].direction = {0.57735f, -0.57735f, 0.57735f};
    render_ctx->main_pass_constants.lights[0].strength = {0.6f, 0.6f, 0.6f};
    render_ctx->main_pass_constants.lights[1].direction = {-0.57735f, -0.57735f, 0.57735f};
    render_ctx->main_pass_constants.lights[1].strength = {0.3f, 0.3f, 0.3f};
    render_ctx->main_pass_constants.lights[2].direction = {0.0f, -0.707f, -0.707f};
    render_ctx->main_pass_constants.lights[2].strength = {0.15f, 0.15f, 0.15f};

    uint8_t * pass_ptr = render_ctx->frame_resources[render_ctx->frame_index].pass_cb_data_ptr;
    memcpy(pass_ptr, &render_ctx->main_pass_constants, sizeof(PassConstants));
}
static HRESULT
move_to_next_frame (D3DRenderContext * render_ctx, UINT * out_frame_index, UINT * out_backbuffer_index) {

    HRESULT ret = E_FAIL;
    UINT frame_index = *out_frame_index;

    // -- 1. schedule a signal command in the queue
    UINT64 const current_fence_value = render_ctx->frame_resources[frame_index].fence;
    ret = render_ctx->cmd_queue->Signal(render_ctx->fence, current_fence_value);
    CHECK_AND_FAIL(ret);

    // -- 2. update frame index
    //*out_backbuffer_index = render_ctx->swapchain3->GetCurrentBackBufferIndex();
    *out_backbuffer_index = (*out_backbuffer_index + 1) % NUM_BACKBUFFERS;
    *out_frame_index = (render_ctx->frame_index + 1) % NUM_QUEUING_FRAMES;

    // -- 3. if the next frame is not ready to be rendered yet, wait until it is ready
    if (render_ctx->fence->GetCompletedValue() < render_ctx->frame_resources[frame_index].fence) {
        ret = render_ctx->fence->SetEventOnCompletion(render_ctx->frame_resources[frame_index].fence, render_ctx->fence_event);
        CHECK_AND_FAIL(ret);
        WaitForSingleObjectEx(render_ctx->fence_event, INFINITE /*return only when the object is signaled*/, false);
    }

    // -- 3. set the fence value for the next frame
    render_ctx->frame_resources[frame_index].fence = current_fence_value + 1;

    return ret;
}
static void
flush_command_queue (D3DRenderContext * render_ctx) {
    // Advance the fence value to mark commands up to this fence point.
    render_ctx->main_current_fence++;

    // Add an instruction to the command queue to set a new fence point.  Because we 
    // are on the GPU timeline, the new fence point won't be set until the GPU finishes
    // processing all the commands prior to this Signal().
    render_ctx->cmd_queue->Signal(render_ctx->fence, render_ctx->main_current_fence);

    // Wait until the GPU has completed commands up to this fence point.
    if (render_ctx->fence->GetCompletedValue() < render_ctx->main_current_fence) {
        HANDLE event_handle = CreateEvent(nullptr, FALSE, FALSE, nullptr);

        // Fire event when GPU hits current fence.  
        render_ctx->fence->SetEventOnCompletion(render_ctx->main_current_fence, event_handle);

        // Wait until the GPU hits current fence event is fired.
        if (event_handle != 0) {
            WaitForSingleObject(event_handle, INFINITE);
            CloseHandle(event_handle);
        }
    }
}
static HRESULT
draw_main (D3DRenderContext * render_ctx) {
    HRESULT ret = E_FAIL;
    UINT frame_index = render_ctx->frame_index;
    UINT backbuffer_index = render_ctx->backbuffer_index;
    ID3D12Resource * backbuffer = render_ctx->render_targets[backbuffer_index];
    ID3D12GraphicsCommandList * cmdlist = render_ctx->direct_cmd_list;

    // Populate command list

    // -- reset cmd_allocator and cmd_list
    render_ctx->frame_resources[frame_index].cmd_list_alloc->Reset();

    // When ExecuteCommandList() is called on a particular command 
    // list, that command list can then be reset at any time and must be before 
    // re-recording.
    ret = cmdlist->Reset(render_ctx->frame_resources[frame_index].cmd_list_alloc, render_ctx->psos[LAYER_BASIC_TESS]);

    ID3D12DescriptorHeap * descriptor_heaps [] = {render_ctx->srv_heap};
    cmdlist->SetDescriptorHeaps(_countof(descriptor_heaps), descriptor_heaps);

    // -- set viewport and scissor
    cmdlist->RSSetViewports(1, &render_ctx->viewport);
    cmdlist->RSSetScissorRects(1, &render_ctx->scissor_rect);

    // -- indicate that the backbuffer will be used as the render target
    resource_usage_transition(
        cmdlist,
        backbuffer,
        D3D12_RESOURCE_STATE_PRESENT,
        D3D12_RESOURCE_STATE_RENDER_TARGET
    );

    // -- get CPU descriptor handle that represents the start of the rtv heap
    D3D12_CPU_DESCRIPTOR_HANDLE dsv_handle = render_ctx->dsv_heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle = render_ctx->rtv_heap->GetCPUDescriptorHandleForHeapStart();
    rtv_handle.ptr = SIZE_T(INT64(rtv_handle.ptr) + INT64(render_ctx->backbuffer_index) * INT64(render_ctx->rtv_descriptor_size));    // -- apply initial offset

    cmdlist->ClearRenderTargetView(rtv_handle, (float *)&render_ctx->main_pass_constants.fog_color, 0, nullptr);
    cmdlist->ClearDepthStencilView(dsv_handle, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);
    cmdlist->OMSetRenderTargets(1, &rtv_handle, true, &dsv_handle);

    cmdlist->SetGraphicsRootSignature(render_ctx->root_signature);

    // Bind per-pass constant buffer.  We only need to do this once per-pass.
    ID3D12Resource * pass_cb = render_ctx->frame_resources[frame_index].pass_cb;
    cmdlist->SetGraphicsRootConstantBufferView(2, pass_cb->GetGPUVirtualAddress());

    // 1. basic tessellation
    if (1 == global_tess_switch) {
        cmdlist->SetPipelineState(render_ctx->psos[LAYER_BASIC_TESS]);
        draw_render_items(
            cmdlist,
            render_ctx->frame_resources[frame_index].obj_cb,
            render_ctx->frame_resources[frame_index].mat_cb,
            render_ctx->cbv_srv_uav_descriptor_size,
            render_ctx->srv_heap,
            &render_ctx->basictess_ritems, frame_index
        );
    }
    // 2. bezier surface
    if (2 == global_tess_switch) {
        cmdlist->SetPipelineState(render_ctx->psos[LAYER_BEZIER_TESS]);
        draw_render_items(
            cmdlist,
            render_ctx->frame_resources[frame_index].obj_cb,
            render_ctx->frame_resources[frame_index].mat_cb,
            render_ctx->cbv_srv_uav_descriptor_size,
            render_ctx->srv_heap,
            &render_ctx->beziersurf_ritems, frame_index
        );
    }

    if (global_imgui_enabled)
        ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), cmdlist);

    // -- indicate that the backbuffer will now be used to present
    resource_usage_transition(cmdlist, backbuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);

    // -- finish populating command list
    cmdlist->Close();

    ID3D12CommandList * cmd_lists [] = {cmdlist};
    render_ctx->cmd_queue->ExecuteCommandLists(ARRAY_COUNT(cmd_lists), cmd_lists);

    render_ctx->swapchain->Present(1 /*sync interval*/, 0 /*present flag*/);

    return ret;
}
static void
SceneContext_Init (SceneContext * scene_ctx, int w, int h) {
    _ASSERT_EXPR(scene_ctx, "scene_ctx not valid");
    memset(scene_ctx, 0, sizeof(SceneContext));

    scene_ctx->width = w;
    scene_ctx->height = h;
    scene_ctx->theta = 0.8f * XM_PI;
    scene_ctx->phi = 0.4f * XM_PI;
    scene_ctx->radius = 50.0f;
    scene_ctx->sun_theta = 1.25f * XM_PI;
    scene_ctx->sun_phi = XM_PIDIV4;
    scene_ctx->aspect_ratio = (float)scene_ctx->width / (float)scene_ctx->height;
    scene_ctx->eye_pos = {0.0f, 0.0f, 0.0f};
    scene_ctx->view = Identity4x4();
    XMMATRIX p = DirectX::XMMatrixPerspectiveFovLH(0.25f * XM_PI, scene_ctx->aspect_ratio, 1.0f, 1000.0f);
    XMStoreFloat4x4(&scene_ctx->proj, p);
}
static void
RenderContext_Init (D3DRenderContext * render_ctx) {
    _ASSERT_EXPR(render_ctx, "render-ctx not valid");
    memset(render_ctx, 0, sizeof(D3DRenderContext));

    render_ctx->viewport.TopLeftX = 0;
    render_ctx->viewport.TopLeftY = 0;
    render_ctx->viewport.Width = (float)global_scene_ctx.width;
    render_ctx->viewport.Height = (float)global_scene_ctx.height;
    render_ctx->viewport.MinDepth = 0.0f;
    render_ctx->viewport.MaxDepth = 1.0f;
    render_ctx->scissor_rect.left = 0;
    render_ctx->scissor_rect.top = 0;
    render_ctx->scissor_rect.right = global_scene_ctx.width;
    render_ctx->scissor_rect.bottom = global_scene_ctx.height;

    // -- initialize fog data
    render_ctx->main_pass_constants.fog_color = {0.7f, 0.7f, 0.7f, 1.0f};
    render_ctx->main_pass_constants.fog_start = 5.0f;
    render_ctx->main_pass_constants.fog_range = 150.0f;

    // -- initialize light data
    render_ctx->main_pass_constants.lights[0].strength = {.5f,.5f,.5f};
    render_ctx->main_pass_constants.lights[0].falloff_start = 1.0f;
    render_ctx->main_pass_constants.lights[0].direction = {0.0f, -1.0f, 0.0f};
    render_ctx->main_pass_constants.lights[0].falloff_end = 10.0f;
    render_ctx->main_pass_constants.lights[0].position = {0.0f, 0.0f, 0.0f};
    render_ctx->main_pass_constants.lights[0].spot_power = 64.0f;

    render_ctx->main_pass_constants.lights[1].strength = {.5f,.5f,.5f};
    render_ctx->main_pass_constants.lights[1].falloff_start = 1.0f;
    render_ctx->main_pass_constants.lights[1].direction = {0.0f, -1.0f, 0.0f};
    render_ctx->main_pass_constants.lights[1].falloff_end = 10.0f;
    render_ctx->main_pass_constants.lights[1].position = {0.0f, 0.0f, 0.0f};
    render_ctx->main_pass_constants.lights[1].spot_power = 64.0f;

    render_ctx->main_pass_constants.lights[2].strength = {.5f,.5f,.5f};
    render_ctx->main_pass_constants.lights[2].falloff_start = 1.0f;
    render_ctx->main_pass_constants.lights[2].direction = {0.0f, -1.0f, 0.0f};
    render_ctx->main_pass_constants.lights[2].falloff_end = 10.0f;
    render_ctx->main_pass_constants.lights[2].position = {0.0f, 0.0f, 0.0f};
    render_ctx->main_pass_constants.lights[2].spot_power = 64.0f;

    // -- specify formats
    render_ctx->backbuffer_format   = DXGI_FORMAT_R8G8B8A8_UNORM;
    render_ctx->depthstencil_format = DXGI_FORMAT_D24_UNORM_S8_UINT;

    // -- 4x MSAA enabled ?
    render_ctx->msaa4x_state = false;
    _ASSERT_EXPR(false == render_ctx->msaa4x_state, _T("Don't enable 4x MSAA for now"));
}
static void
d3d_resize (D3DRenderContext * render_ctx) {
    int w = global_scene_ctx.width;
    int h = global_scene_ctx.height;

    if (render_ctx &&
        render_ctx->device &&
        render_ctx->direct_cmd_list_alloc &&
        render_ctx->swapchain
        ) {
        // Flush before changing any resources.
        flush_command_queue(render_ctx);

        render_ctx->direct_cmd_list->Reset(render_ctx->direct_cmd_list_alloc, nullptr);

        // Release the previous resources we will be recreating.
        for (int i = 0; i < NUM_BACKBUFFERS; ++i)
            render_ctx->render_targets[i]->Release();
        render_ctx->depth_stencil_buffer->Release();

        // Resize the swap chain.
        render_ctx->swapchain->ResizeBuffers(
            NUM_BACKBUFFERS,
            w, h,
            render_ctx->backbuffer_format,
            DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH
        );

        render_ctx->backbuffer_index = 0;

        D3D12_CPU_DESCRIPTOR_HANDLE rtv_heap_handle = render_ctx->rtv_heap->GetCPUDescriptorHandleForHeapStart();
        for (UINT i = 0; i < NUM_BACKBUFFERS; i++) {
            render_ctx->swapchain->GetBuffer(i, IID_PPV_ARGS(&render_ctx->render_targets[i]));
            render_ctx->device->CreateRenderTargetView(render_ctx->render_targets[i], nullptr, rtv_heap_handle);
            rtv_heap_handle.ptr += render_ctx->rtv_descriptor_size;
        }

        // Create the depth/stencil buffer and view.
        D3D12_RESOURCE_DESC depth_stencil_desc;
        depth_stencil_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        depth_stencil_desc.Alignment = 0;
        depth_stencil_desc.Width = w;
        depth_stencil_desc.Height = h;
        depth_stencil_desc.DepthOrArraySize = 1;
        depth_stencil_desc.MipLevels = 1;

        // NOTE(omid): Note that we create the depth buffer resource with a typeless format.  
        depth_stencil_desc.Format = DXGI_FORMAT_R24G8_TYPELESS;
        depth_stencil_desc.SampleDesc.Count = render_ctx->msaa4x_state ? 4 : 1;
        depth_stencil_desc.SampleDesc.Quality = render_ctx->msaa4x_state ? (render_ctx->msaa4x_quality - 1) : 0;
        depth_stencil_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        depth_stencil_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

        D3D12_CLEAR_VALUE opt_clear;
        opt_clear.Format = render_ctx->depthstencil_format;
        opt_clear.DepthStencil.Depth = 1.0f;
        opt_clear.DepthStencil.Stencil = 0;

        D3D12_HEAP_PROPERTIES def_heap = {};
        def_heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        def_heap.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
        def_heap.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
        def_heap.CreationNodeMask = 1;
        def_heap.VisibleNodeMask = 1;
        render_ctx->device->CreateCommittedResource(
            &def_heap,
            D3D12_HEAP_FLAG_NONE,
            &depth_stencil_desc,
            D3D12_RESOURCE_STATE_COMMON,
            &opt_clear,
            IID_PPV_ARGS(&render_ctx->depth_stencil_buffer)
        );

        // Create descriptor to mip level 0 of entire resource using the format of the resource.
        D3D12_DEPTH_STENCIL_VIEW_DESC dsv_desc;
        dsv_desc.Flags = D3D12_DSV_FLAG_NONE;
        dsv_desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        dsv_desc.Format = render_ctx->depthstencil_format;
        dsv_desc.Texture2D.MipSlice = 0;
        render_ctx->device->CreateDepthStencilView(render_ctx->depth_stencil_buffer, &dsv_desc, render_ctx->dsv_heap->GetCPUDescriptorHandleForHeapStart());

        // Transition the resource from its initial state to be used as a depth buffer.
        resource_usage_transition(render_ctx->direct_cmd_list, render_ctx->depth_stencil_buffer, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_DEPTH_WRITE);

        // Execute the resize commands.
        render_ctx->direct_cmd_list->Close();
        ID3D12CommandList* cmds_list [] = {render_ctx->direct_cmd_list};
        render_ctx->cmd_queue->ExecuteCommandLists(_countof(cmds_list), cmds_list);

        // Wait until resize is complete.
        flush_command_queue(render_ctx);

        // Update the viewport transform to cover the client area.
        render_ctx->viewport.TopLeftX = 0;
        render_ctx->viewport.TopLeftY = 0;
        render_ctx->viewport.Width    = static_cast<float>(w);
        render_ctx->viewport.Height   = static_cast<float>(h);
        render_ctx->viewport.MinDepth = 0.0f;
        render_ctx->viewport.MaxDepth = 1.0f;

        render_ctx->scissor_rect = {0, 0, w, h};

        // The window resized, so update the aspect ratio and recompute the projection matrix.
        global_scene_ctx.aspect_ratio = static_cast<float>(w) / h;
        XMMATRIX p = DirectX::XMMatrixPerspectiveFovLH(0.25f * XM_PI, global_scene_ctx.aspect_ratio, 1.0f, 1000.0f);
        XMStoreFloat4x4(&global_scene_ctx.proj, p);
    }
}
// Forward declare message handler from imgui_impl_win32.cpp
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

static LRESULT CALLBACK
main_win_cb (HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    // Handle imgui window
    if (ImGui_ImplWin32_WndProcHandler(hwnd, uMsg, wParam, lParam))
        return true;

    // Handle passed user data (render_ctx)
    D3DRenderContext * _render_ctx = nullptr;
    if (uMsg == WM_CREATE) {
        CREATESTRUCT * ptr_create = reinterpret_cast<CREATESTRUCT *>(lParam);
        _render_ctx = reinterpret_cast<D3DRenderContext *>(ptr_create->lpCreateParams);
        SetWindowLongPtr(hwnd, GWLP_USERDATA, (LONG_PTR)_render_ctx);
    } else {
        LONG_PTR ptr = GetWindowLongPtr(hwnd, GWLP_USERDATA);
        _render_ctx = reinterpret_cast<D3DRenderContext *>(ptr);
    }

    LRESULT ret = 0;
    switch (uMsg) {

    case WM_ACTIVATE: {
        if (LOWORD(wParam) == WA_INACTIVE) {
            global_paused = true;
            Timer_Stop(&global_timer);
        } else {
            global_paused = false;
            Timer_Start(&global_timer);
        }
    } break;

    case WM_LBUTTONDOWN:
    case WM_MBUTTONDOWN:
    case WM_RBUTTONDOWN: {
        global_scene_ctx.mouse.x = GET_X_LPARAM(lParam);
        global_scene_ctx.mouse.y = GET_Y_LPARAM(lParam);
        SetCapture(hwnd);
    } break;
    case WM_LBUTTONUP:
    case WM_MBUTTONUP:
    case WM_RBUTTONUP: {
        ReleaseCapture();
    } break;
    case WM_MOUSEMOVE: {
        handle_mouse_move(&global_scene_ctx, wParam, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
    } break;
    case WM_SIZE: {
        global_scene_ctx.width = LOWORD(lParam);
        global_scene_ctx.height = HIWORD(lParam);
        if (_render_ctx) {
            if (wParam == SIZE_MINIMIZED) {
                global_paused = true;
            } else if (wParam == SIZE_MAXIMIZED) {
                global_paused = false;
                d3d_resize(_render_ctx);
            } else if (wParam == SIZE_RESTORED) {
                // TODO(omid): handle restore from minimize/maximize 
                if (global_resizing) {
                    // don't do nothing until resizing finished
                } else {
                    d3d_resize(_render_ctx);
                }
            }
        }
    } break;
    // WM_EXITSIZEMOVE is sent when the user grabs the resize bars.
    case WM_ENTERSIZEMOVE: {
        global_paused = true;
        global_resizing  = true;
        Timer_Stop(&global_timer);
    } break;
    // WM_EXITSIZEMOVE is sent when the user releases the resize bars.
    // Here we reset everything based on the new window dimensions.
    case WM_EXITSIZEMOVE: {
        global_paused = false;
        global_resizing  = false;
        Timer_Start(&global_timer);
        d3d_resize(_render_ctx);
    } break;
    case WM_DESTROY: {
        PostQuitMessage(0);
    } break;
    // Catch this message so to prevent the window from becoming too small.
    case WM_GETMINMAXINFO:
    {
        ((MINMAXINFO*)lParam)->ptMinTrackSize.x = 200;
        ((MINMAXINFO*)lParam)->ptMinTrackSize.y = 200;
    }
    break;
    default: {
        ret = DefWindowProc(hwnd, uMsg, wParam, lParam);
    } break;
    }
    return ret;
}

INT WINAPI
WinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE, _In_ LPSTR, _In_ INT) {

    SceneContext_Init(&global_scene_ctx, 1280, 720);
    D3DRenderContext * render_ctx = (D3DRenderContext *)::malloc(sizeof(D3DRenderContext));
    RenderContext_Init(render_ctx);

    // ========================================================================================================
#pragma region Windows_Setup
    WNDCLASS wc = {};
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
    wc.lpfnWndProc = main_win_cb;
    wc.hInstance = hInstance;
    wc.lpszClassName = _T("d3d12_win32");

    _ASSERT_EXPR(RegisterClass(&wc), "could not register window class");

    // Compute window rectangle dimensions based on requested client area dimensions.
    RECT R = {0, 0, (long int)global_scene_ctx.width, (long int)global_scene_ctx.height};
    AdjustWindowRect(&R, WS_OVERLAPPEDWINDOW, false);
    int width  = R.right - R.left;
    int height = R.bottom - R.top;

    HWND hwnd = CreateWindowEx(
        0,                                              // Optional window styles.
        wc.lpszClassName,                               // Window class
        _T("Tessellation app"),                    // Window title
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,               // Window style
        CW_USEDEFAULT, CW_USEDEFAULT, width, height,    // Size and position settings
        0 /* Parent window */, 0 /* Menu */, hInstance  /* Instance handle */,
        render_ctx                                      /* Additional application data */
    );

    _ASSERT_EXPR(hwnd, _T("could not create window"));

#pragma endregion Windows_Setup

    // ========================================================================================================
#pragma region Enable_Debug_Layer
    UINT dxgiFactoryFlags = 0;
#if ENABLE_DEBUG_LAYER > 0
    ID3D12Debug * debug_interface_dx = nullptr;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug_interface_dx)))) {
        debug_interface_dx->EnableDebugLayer();
        dxgiFactoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
    }
#endif
#pragma endregion Enable_Debug_Layer

    // ========================================================================================================
#pragma region Initialization

    // Query Adapter (PhysicalDevice)
    IDXGIFactory4 * dxgi_factory = nullptr;
    CHECK_AND_FAIL(CreateDXGIFactory2(dxgiFactoryFlags, IID_PPV_ARGS(&dxgi_factory)));
    //CHECK_AND_FAIL(CreateDXGIFactory1(IID_PPV_ARGS(&dxgi_factory)));

    uint32_t const MaxAdapters = 8;
    IDXGIAdapter * adapters[MaxAdapters] = {};
    IDXGIAdapter * pAdapter;
    for (UINT i = 0; dxgi_factory->EnumAdapters(i, &pAdapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        adapters[i] = pAdapter;
        DXGI_ADAPTER_DESC adapter_desc = {};
        ::printf("GPU Info [%d] :\n", i);
        if (SUCCEEDED(pAdapter->GetDesc(&adapter_desc))) {
            ::printf("\tDescription: %ls\n", adapter_desc.Description);
            ::printf("\tDedicatedVideoMemory: %zu\n", adapter_desc.DedicatedVideoMemory);
        }
    } // WARP -> Windows Advanced Rasterization ...

    // Create Logical Device
    auto res = D3D12CreateDevice(adapters[0], D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&render_ctx->device));
    CHECK_AND_FAIL(res);

    // Release adaptors
    for (unsigned i = 0; i < MaxAdapters; ++i) {
        if (adapters[i] != nullptr) {
            adapters[i]->Release();
        }
    }
    // store CBV_SRV_UAV descriptor increment size
    render_ctx->cbv_srv_uav_descriptor_size = render_ctx->device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE::D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    // store RTV descriptor increment size
    render_ctx->rtv_descriptor_size = render_ctx->device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE::D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    // Check 4X MSAA quality support for our back buffer format.
    D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS quality_levels;
    quality_levels.Format = render_ctx->backbuffer_format;
    quality_levels.SampleCount = 4;
    quality_levels.Flags = D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_NONE;
    quality_levels.NumQualityLevels = 0;
    render_ctx->device->CheckFeatureSupport(
        D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS,
        &quality_levels,
        sizeof(quality_levels)
    );
    render_ctx->msaa4x_quality = quality_levels.NumQualityLevels;
    _ASSERT_EXPR(render_ctx->msaa4x_quality > 0, "Unexpected MSAA quality level.");

#pragma region Create Command Objects
    // Create Command Queue
    D3D12_COMMAND_QUEUE_DESC cmd_q_desc = {};
    cmd_q_desc.Type = D3D12_COMMAND_LIST_TYPE::D3D12_COMMAND_LIST_TYPE_DIRECT;
    cmd_q_desc.Flags = D3D12_COMMAND_QUEUE_FLAGS::D3D12_COMMAND_QUEUE_FLAG_NONE;
    render_ctx->device->CreateCommandQueue(&cmd_q_desc, IID_PPV_ARGS(&render_ctx->cmd_queue));

    // Create Command Allocator
    render_ctx->device->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        IID_PPV_ARGS(&render_ctx->direct_cmd_list_alloc)
    );

    // Create Command List
    if (render_ctx->direct_cmd_list_alloc) {
        render_ctx->device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT,
            render_ctx->direct_cmd_list_alloc,
            render_ctx->psos[LAYER_BASIC_TESS], IID_PPV_ARGS(&render_ctx->direct_cmd_list)
        );

        // Reset the command list to prep for initialization commands.
        // NOTE(omid): Command list needs to be closed before calling Reset.
        render_ctx->direct_cmd_list->Close();
        render_ctx->direct_cmd_list->Reset(render_ctx->direct_cmd_list_alloc, nullptr);
    }
#pragma endregion

    DXGI_MODE_DESC backbuffer_desc = {};
    backbuffer_desc.Width = global_scene_ctx.width;
    backbuffer_desc.Height = global_scene_ctx.height;
    backbuffer_desc.Format = render_ctx->backbuffer_format;
    backbuffer_desc.RefreshRate.Numerator = 60;
    backbuffer_desc.RefreshRate.Denominator = 1;
    backbuffer_desc.Scaling = DXGI_MODE_SCALING_UNSPECIFIED;
    backbuffer_desc.ScanlineOrdering = DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED;

    DXGI_SAMPLE_DESC sampler_desc = {};
    if (render_ctx->msaa4x_state) {
        sampler_desc.Count = 1;
        sampler_desc.Quality = 0;
    } else {
        sampler_desc.Count = render_ctx->msaa4x_state ? 4 : 1;
        sampler_desc.Quality = render_ctx->msaa4x_state ? (render_ctx->msaa4x_quality - 1) : 0;
    }

    // Create Swapchain
    DXGI_SWAP_CHAIN_DESC swapchain_desc = {};
    swapchain_desc.BufferDesc = backbuffer_desc;
    swapchain_desc.SampleDesc = sampler_desc;
    swapchain_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapchain_desc.BufferCount = NUM_BACKBUFFERS;
    swapchain_desc.OutputWindow = hwnd;
    swapchain_desc.Windowed = TRUE;
    swapchain_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swapchain_desc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;

    if (render_ctx->cmd_queue)
        CHECK_AND_FAIL(dxgi_factory->CreateSwapChain(render_ctx->cmd_queue, &swapchain_desc, &render_ctx->swapchain));

// ========================================================================================================
#pragma region Load Textures
    // crate
    strcpy_s(render_ctx->textures[TEX_WHITE1X1].name, "white1x1");
    wcscpy_s(render_ctx->textures[TEX_WHITE1X1].filename, L"../Textures/white1x1.dds");
    load_texture(
        render_ctx->device, render_ctx->direct_cmd_list,
        render_ctx->textures[TEX_WHITE1X1].filename,
        &render_ctx->textures[TEX_WHITE1X1]
    );
#pragma endregion

    create_descriptor_heaps(render_ctx);

#pragma region Dsv_Creation
// Create the depth/stencil buffer and view.
    D3D12_RESOURCE_DESC ds_desc;
    ds_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    ds_desc.Alignment = 0;
    ds_desc.Width = global_scene_ctx.width;
    ds_desc.Height = global_scene_ctx.height;
    ds_desc.DepthOrArraySize = 1;
    ds_desc.MipLevels = 1;

    // NOTE(omid): SSAO requires an SRV to the depth buffer to read from 
    // the depth buffer.  Therefore, because we need to create two views to the same resource:
    //   1. SRV format: DXGI_FORMAT_R24_UNORM_X8_TYPELESS
    //   2. DSV Format: DXGI_FORMAT_D24_UNORM_S8_UINT
    // we need to create the depth buffer resource with a typeless format.  
    ds_desc.Format = DXGI_FORMAT_R24G8_TYPELESS;
    ds_desc.SampleDesc.Count = render_ctx->msaa4x_state ? 4 : 1;
    ds_desc.SampleDesc.Quality = render_ctx->msaa4x_state ? (render_ctx->msaa4x_quality - 1) : 0;
    ds_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    ds_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_HEAP_PROPERTIES ds_heap_props = {};
    ds_heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;
    ds_heap_props.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    ds_heap_props.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    ds_heap_props.CreationNodeMask = 1;
    ds_heap_props.VisibleNodeMask = 1;

    D3D12_CLEAR_VALUE opt_clear;
    opt_clear.Format = render_ctx->depthstencil_format;
    opt_clear.DepthStencil.Depth = 1.0f;
    opt_clear.DepthStencil.Stencil = 0;
    render_ctx->device->CreateCommittedResource(
        &ds_heap_props,
        D3D12_HEAP_FLAG_NONE,
        &ds_desc,
        D3D12_RESOURCE_STATE_COMMON,
        &opt_clear,
        IID_PPV_ARGS(&render_ctx->depth_stencil_buffer)
    );

    // Create descriptor to mip level 0 of entire resource using the format of the resource.
    D3D12_DEPTH_STENCIL_VIEW_DESC dsv_desc;
    dsv_desc.Flags = D3D12_DSV_FLAG_NONE;
    dsv_desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    dsv_desc.Format = render_ctx->depthstencil_format;
    dsv_desc.Texture2D.MipSlice = 0;
    render_ctx->device->CreateDepthStencilView(
        render_ctx->depth_stencil_buffer,
        &dsv_desc,
        render_ctx->dsv_heap->GetCPUDescriptorHandleForHeapStart()
    );
#pragma endregion Dsv_Creation

#pragma region Create RTV
    // -- create frame resources: rtv for each frame
    D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle_start = render_ctx->rtv_heap->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < NUM_BACKBUFFERS; ++i) {
        render_ctx->swapchain->GetBuffer(i, IID_PPV_ARGS(&render_ctx->render_targets[i]));
        D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle = {};
        cpu_handle.ptr = rtv_handle_start.ptr + ((UINT64)i * render_ctx->rtv_descriptor_size);
        // -- create a rtv for each frame
        render_ctx->device->CreateRenderTargetView(render_ctx->render_targets[i], nullptr, cpu_handle);
    }
#pragma endregion

#pragma region Create CBuffers
    UINT obj_cb_size = sizeof(ObjectConstants);
    UINT mat_cb_size = sizeof(MaterialConstants);
    UINT pass_cb_size = sizeof(PassConstants);
    for (UINT i = 0; i < NUM_QUEUING_FRAMES; ++i) {
        // -- create a cmd-allocator for each frame
        res = render_ctx->device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE::D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&render_ctx->frame_resources[i].cmd_list_alloc));

        // -- create cbuffers as upload_buffer
        create_upload_buffer(render_ctx->device, (UINT64)obj_cb_size * _COUNT_RENDERITEM, &render_ctx->frame_resources[i].obj_cb_data_ptr, &render_ctx->frame_resources[i].obj_cb);
        // Initialize cb data
        ::memcpy(render_ctx->frame_resources[i].obj_cb_data_ptr, &render_ctx->frame_resources[i].obj_cb_data, sizeof(render_ctx->frame_resources[i].obj_cb_data));

        create_upload_buffer(render_ctx->device, (UINT64)mat_cb_size * _COUNT_MATERIAL, &render_ctx->frame_resources[i].mat_cb_data_ptr, &render_ctx->frame_resources[i].mat_cb);
        // Initialize cb data
        ::memcpy(render_ctx->frame_resources[i].mat_cb_data_ptr, &render_ctx->frame_resources[i].mat_cb_data, sizeof(render_ctx->frame_resources[i].mat_cb_data));

        create_upload_buffer(render_ctx->device, pass_cb_size * 1, &render_ctx->frame_resources[i].pass_cb_data_ptr, &render_ctx->frame_resources[i].pass_cb);
        // Initialize cb data
        ::memcpy(render_ctx->frame_resources[i].pass_cb_data_ptr, &render_ctx->frame_resources[i].pass_cb_data, sizeof(render_ctx->frame_resources[i].pass_cb_data));
    }
#pragma endregion

    // ========================================================================================================
#pragma region Root_Signature_Creation
    create_root_signature(render_ctx->device, &render_ctx->root_signature);
#pragma endregion Root_Signature_Creation

    // Load and compile shaders

#pragma region Compile Shaders
    TCHAR tessellation_shaders_path [] = _T("./shaders/tessellation.hlsl");
    TCHAR bezier_shaders_path [] = _T("./shaders/bezier_tessellation.hlsl");

    {   // basic tessellation shaders
        compile_shader(tessellation_shaders_path, _T("pass_through_vs"), _T("vs_6_0"), nullptr, 0, &render_ctx->shaders[SHADER_DEFAULT_VS]);
        compile_shader(tessellation_shaders_path, _T("pass_through_hs_4cp"), _T("hs_6_0"), nullptr, 0, &render_ctx->shaders[SHADER_4CP_HS]);
        compile_shader(tessellation_shaders_path, _T("basic_ds"), _T("ds_6_0"), nullptr, 0, &render_ctx->shaders[SHADER_BASIC_DS]);
        compile_shader(tessellation_shaders_path, _T("pixel_shader"), _T("ps_6_0"), nullptr, 0, &render_ctx->shaders[SHADER_OPAQUE_PS]);
    }
    {   // bezier shaders
        compile_shader(bezier_shaders_path, _T("pass_through_hs_16cp"), _T("hs_6_0"), nullptr, 0, &render_ctx->shaders[SHADER_16CP_HS]);
        compile_shader(bezier_shaders_path, _T("bezier_ds"), _T("ds_6_0"), nullptr, 0, &render_ctx->shaders[SHADER_BEZIER_DS]);
    }

#pragma endregion Compile Shaders

    create_pso(render_ctx);

#pragma region Shapes_And_Renderitem_Creation
    create_quad_patch_geometry_4cp(render_ctx);     // basic tessellation
    create_quad_patch_geometry_16cp(render_ctx);    // cubic bezier surface
    create_materials(render_ctx->materials);
    create_render_items(render_ctx);

#pragma endregion Shapes_And_Renderitem_Creation

    // NOTE(omid): Before closing/executing command list specify the depth-stencil-buffer transition from its initial state to be used as a depth buffer.
    resource_usage_transition(render_ctx->direct_cmd_list, render_ctx->depth_stencil_buffer, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_DEPTH_WRITE);

    // -- close the command list and execute it to begin inital gpu setup
    CHECK_AND_FAIL(render_ctx->direct_cmd_list->Close());
    ID3D12CommandList * cmd_lists [] = {render_ctx->direct_cmd_list};
    render_ctx->cmd_queue->ExecuteCommandLists(ARRAY_COUNT(cmd_lists), cmd_lists);

    //----------------
    // Create fence
    // create synchronization objects and wait until assets have been uploaded to the GPU.

    UINT frame_index = render_ctx->frame_index;
    CHECK_AND_FAIL(render_ctx->device->CreateFence(render_ctx->frame_resources[frame_index].fence, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&render_ctx->fence)));

    ++render_ctx->frame_resources[frame_index].fence;

    // Create an event handle to use for frame synchronization.
    render_ctx->fence_event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (nullptr == render_ctx->fence_event) {
        // map the error code to an HRESULT value.
        CHECK_AND_FAIL(HRESULT_FROM_WIN32(GetLastError()));
    }

    // Wait for the command list to execute; 
    // we just want to wait for setup to complete before continuing.
    flush_command_queue(render_ctx);

#pragma endregion

#pragma region Imgui Setup
    bool * ptr_open = nullptr;
    ImGuiWindowFlags window_flags = 0;
    if (global_imgui_enabled) {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO(); (void)io;
        io.Fonts->AddFontDefault();
        ImGui::StyleColorsDark();

        // calculate imgui cpu & gpu handles on location on srv_heap
        D3D12_CPU_DESCRIPTOR_HANDLE imgui_cpu_handle = render_ctx->srv_heap->GetCPUDescriptorHandleForHeapStart();
        imgui_cpu_handle.ptr += (render_ctx->cbv_srv_uav_descriptor_size * (
            _COUNT_TEX
            ));

        D3D12_GPU_DESCRIPTOR_HANDLE imgui_gpu_handle = render_ctx->srv_heap->GetGPUDescriptorHandleForHeapStart();
        imgui_gpu_handle.ptr += (render_ctx->cbv_srv_uav_descriptor_size * (
            _COUNT_TEX
            ));

            // Setup Platform/Renderer backends
        ImGui_ImplWin32_Init(hwnd);
        ImGui_ImplDX12_Init(
            render_ctx->device, NUM_QUEUING_FRAMES,
            render_ctx->backbuffer_format, render_ctx->srv_heap,
            imgui_cpu_handle,
            imgui_gpu_handle
        );

        // Setup imgui window flags
        window_flags |= ImGuiWindowFlags_NoScrollbar;
        window_flags |= ImGuiWindowFlags_MenuBar;
        window_flags |= ImGuiWindowFlags_NoMove;
        window_flags |= ImGuiWindowFlags_NoCollapse;
        window_flags |= ImGuiWindowFlags_NoNav;
        window_flags |= ImGuiWindowFlags_NoBringToFrontOnFocus;
        window_flags |= ImGuiWindowFlags_NoResize;

    }
#pragma endregion

        // ========================================================================================================
#pragma region Main_Loop
    global_paused = false;
    global_resizing = false;
    Timer_Init(&global_timer);
    Timer_Reset(&global_timer);

    MSG msg = {};
    while (msg.message != WM_QUIT) {
        if (PeekMessageA(&msg, 0, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        } else {
#pragma region Imgui window
            if (global_imgui_enabled) {
                ImGui_ImplDX12_NewFrame();
                ImGui_ImplWin32_NewFrame();
                ImGui::NewFrame();
                ImGui::Begin("Settings", ptr_open, window_flags);

                ImGui::RadioButton("Basic Tessellation", &global_tess_switch, 1);
                ImGui::Text(
                    "Tessellating a quad patch with 4 control points\n"
                    "based on distance from camera."
                );
                ImGui::RadioButton("Bezier Surface", &global_tess_switch, 2);
                ImGui::Text(
                    "A Bezier surface using cubic Bernstein bases,\n"
                    "with a 16 CPs quad patch and uniform tessellations."
                );

                ImGui::Text("\n\n");
                ImGui::Separator();
                ImGui::Text("Application average %.3f ms/frame (%.1f FPS)", 1000.0f / ImGui::GetIO().Framerate, ImGui::GetIO().Framerate);

                ImGui::End();
                ImGui::Render();

            }
#pragma endregion
            Timer_Tick(&global_timer);

            if (!global_paused) {
                update_camera(&global_scene_ctx);

                update_obj_cbuffers(render_ctx);
                update_mat_cbuffers(render_ctx);
                update_pass_cbuffers(render_ctx, &global_timer);

                CHECK_AND_FAIL(draw_main(render_ctx));

                CHECK_AND_FAIL(move_to_next_frame(render_ctx, &render_ctx->frame_index, &render_ctx->backbuffer_index));
            } else {
                Sleep(100);
            }
        }
    }
#pragma endregion

    // ========================================================================================================
#pragma region Cleanup_And_Debug
    flush_command_queue(render_ctx);

    // Cleanup Imgui
    if (global_imgui_enabled) {
        ImGui_ImplDX12_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
    }

    // release queuing frame resources
    for (size_t i = 0; i < NUM_QUEUING_FRAMES; i++) {
        flush_command_queue(render_ctx);    // TODO(omid): Address the cbuffers release issue 
        render_ctx->frame_resources[i].obj_cb->Unmap(0, nullptr);
        render_ctx->frame_resources[i].mat_cb->Unmap(0, nullptr);
        render_ctx->frame_resources[i].pass_cb->Unmap(0, nullptr);
        render_ctx->frame_resources[i].obj_cb->Release();
        render_ctx->frame_resources[i].mat_cb->Release();
        render_ctx->frame_resources[i].pass_cb->Release();

        render_ctx->frame_resources[i].cmd_list_alloc->Release();
    }
    CloseHandle(render_ctx->fence_event);
    render_ctx->fence->Release();

    for (unsigned i = 0; i < _COUNT_GEOM; i++) {
        render_ctx->geom[i].ib_uploader->Release();
        render_ctx->geom[i].vb_uploader->Release();
        render_ctx->geom[i].vb_gpu->Release();
        render_ctx->geom[i].ib_gpu->Release();
    }   // is this a bug in d3d12sdklayers.dll ?

    for (int i = 0; i < _COUNT_RENDERCOMPUTE_LAYER; ++i)
        render_ctx->psos[i]->Release();

    for (unsigned i = 0; i < _COUNT_SHADERS; ++i)
        render_ctx->shaders[i]->Release();

    render_ctx->root_signature->Release();

    // release swapchain backbuffers resources
    for (unsigned i = 0; i < NUM_BACKBUFFERS; ++i)
        render_ctx->render_targets[i]->Release();

    render_ctx->dsv_heap->Release();
    render_ctx->rtv_heap->Release();
    render_ctx->srv_heap->Release();

    render_ctx->depth_stencil_buffer->Release();

    for (unsigned i = 0; i < _COUNT_TEX; i++) {
        render_ctx->textures[i].upload_heap->Release();
        render_ctx->textures[i].resource->Release();
    }

    //render_ctx->swapchain3->Release();
    render_ctx->swapchain->Release();
    render_ctx->direct_cmd_list->Release();
    render_ctx->direct_cmd_list_alloc->Release();
    render_ctx->cmd_queue->Release();
    render_ctx->device->Release();
    dxgi_factory->Release();


#if (ENABLE_DEBUG_LAYER > 0)
    debug_interface_dx->Release();
#endif

// -- advanced debugging and reporting live objects [from https://walbourn.github.io/dxgi-debug-device/]

    typedef HRESULT (WINAPI * LPDXGIGETDEBUGINTERFACE)(REFIID, void **);

    //HMODULE dxgidebug_dll = LoadLibraryEx( L"dxgidebug_dll.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32 );
    HMODULE dxgidebug_dll = LoadLibrary(L"DXGIDebug.dll");
    if (dxgidebug_dll) {
        auto dxgiGetDebugInterface = reinterpret_cast<LPDXGIGETDEBUGINTERFACE>(
            reinterpret_cast<void*>(GetProcAddress(dxgidebug_dll, "DXGIGetDebugInterface")));

        IDXGIDebug1 * dxgi_debugger = nullptr;
        DXGIGetDebugInterface1(0, IID_PPV_ARGS(&dxgi_debugger));
        dxgi_debugger->ReportLiveObjects(
            DXGI_DEBUG_ALL,
            DXGI_DEBUG_RLO_DETAIL
            /* DXGI_DEBUG_RLO_FLAGS(DXGI_DEBUG_RLO_SUMMARY | DXGI_DEBUG_RLO_IGNORE_INTERNAL) */
        );
        dxgi_debugger->Release();
        FreeLibrary(dxgidebug_dll);

        // -- consume var to avoid warning
        dxgiGetDebugInterface = dxgiGetDebugInterface;
    }
#pragma endregion Cleanup_And_Debug

    return 0;
}

