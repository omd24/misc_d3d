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

#if defined _DEBUG
#define ENABLE_DEBUG_LAYER 1
#else
#define ENABLE_DEBUG_LAYER 0
#endif

#pragma warning (disable: 28182)    // pointer can be NULL.
#pragma warning (disable: 6011)     // dereferencing a potentially null pointer
#pragma warning (disable: 26495)    // not initializing struct members

#define NUM_BACKBUFFERS         2
#define NUM_QUEUING_FRAMES      3

enum RENDER_LAYERS : int {
    LAYER_OPAQUE = 0,
    LAYER_TRANSPARENT = 1,
    LAYER_ALPHATESTED = 2,
    LAYER_MIRRORS = 3,
    LAYER_REFLECTIONS = 4,
    LAYER_SHADOW = 5,
    //LAYER_REFLECTED_SHADOW = 6,

    _COUNT_RENDER_LAYER
};
enum ALL_RENDERITEMS {
    RITEM_FLOOR = 0,
    RITEM_WALL = 1,
    RITEM_MIRROR = 2,
    RITEM_SKULL = 3,
    RITEM_REFLECTED_SKULL = 4,
    RITEM_REFLECTED_FLOOR = 5,
    RITEM_REFLECTED_SHADOW = 6,
    RITEM_SHADOWED_SKULL = 7,

    _COUNT_RENDERITEM
};
enum GEOM_INDEX {
    GEOM_ROOM = 0,
    GEOM_SKULL = 1,

    _COUNT_GEOM
};
enum ROOM_SUBMESH_INDEX {
    ROOM_SUBMESH_FLOOR = 0,
    ROOM_SUBMESH_WALL = 1,
    ROOM_SUBMESH_MIRROR = 2
};
enum MAT_INDEX {
    MAT_BRICKS = 0,
    MAT_CHECKER_TILE = 1,
    MAT_ICE_MIRROR = 2,
    MAT_SKULL = 3,
    MAT_SHADOW = 4,

    _COUNT_MATERIAL
};
enum TEX_INDEX {
    TEX_BRICK = 0,
    TEX_CHECKERBOARD = 1,
    TEX_ICE = 2,
    TEX_WHITE1x1 = 3,

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

    // skull translation
    XMFLOAT3 skull_translation;
    //XMFLOAT3 floor_translation;   // do we need to translate floor?
};

GameTimer global_timer;
bool global_running;
bool global_resizing;
bool global_mouse_active;
SceneContext global_scene_ctx;

struct RenderItemArray {
    RenderItem  ritems[_COUNT_RENDERITEM];
    uint32_t    size;
};
struct D3DRenderContext {
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
    ID3D12PipelineState *           psos[_COUNT_RENDER_LAYER];

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
    PassConstants                   reflected_pass_constants;
    UINT                            pass_cbv_offset;

    // List of all the render items.
    RenderItemArray                 all_ritems;
    // Render items divided by PSO.
    RenderItemArray                 opaque_ritems;
    RenderItemArray                 transparent_ritems;
    RenderItemArray                 alphatested_ritems;
    RenderItemArray                 mirrors_ritems;
    RenderItemArray                 reflected_ritems;
    RenderItemArray                 shadow_ritems;
    RenderItemArray                 reflected_shadow_ritems;

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

    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource = out_texture->resource;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

    // Use Heap-allocating UpdateSubresources implementation for variable number of subresources (which is the case for textures).
    update_subresources_heap(
        cmd_list, out_texture->resource, out_texture->upload_heap,
        0, 0, n_subresources, subresources
    );
    cmd_list->ResourceBarrier(1, &barrier);

    ::free(subresources);
    ::free(ddsData);
}
static void
create_materials (Material out_materials []) {
    strcpy_s(out_materials[MAT_BRICKS].name, "bricks");
    out_materials[MAT_BRICKS].mat_cbuffer_index = 0;
    out_materials[MAT_BRICKS].diffuse_srvheap_index = 0;
    out_materials[MAT_BRICKS].diffuse_albedo = XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);
    out_materials[MAT_BRICKS].fresnel_r0 = XMFLOAT3(0.05f, 0.05f, 0.05f);
    out_materials[MAT_BRICKS].roughness = 0.25f;
    out_materials[MAT_BRICKS].mat_transform = Identity4x4();
    out_materials[MAT_BRICKS].n_frames_dirty = NUM_QUEUING_FRAMES;

    strcpy_s(out_materials[MAT_CHECKER_TILE].name, "checkertile");
    out_materials[MAT_CHECKER_TILE].mat_cbuffer_index = 1;
    out_materials[MAT_CHECKER_TILE].diffuse_srvheap_index = 1;
    out_materials[MAT_CHECKER_TILE].diffuse_albedo = XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);
    out_materials[MAT_CHECKER_TILE].fresnel_r0 = XMFLOAT3(0.07f, 0.07f, 0.07f);
    out_materials[MAT_CHECKER_TILE].roughness = 0.3f;
    out_materials[MAT_CHECKER_TILE].mat_transform = Identity4x4();
    out_materials[MAT_CHECKER_TILE].n_frames_dirty = NUM_QUEUING_FRAMES;

    strcpy_s(out_materials[MAT_ICE_MIRROR].name, "icemirror");
    out_materials[MAT_ICE_MIRROR].mat_cbuffer_index = 2;
    out_materials[MAT_ICE_MIRROR].diffuse_srvheap_index = 2;
    out_materials[MAT_ICE_MIRROR].diffuse_albedo = XMFLOAT4(1.0f, 1.0f, 1.0f, 0.3f);
    out_materials[MAT_ICE_MIRROR].fresnel_r0 = XMFLOAT3(0.1f, 0.1f, 0.1f);
    out_materials[MAT_ICE_MIRROR].roughness = 0.5f;
    out_materials[MAT_ICE_MIRROR].mat_transform = Identity4x4();
    out_materials[MAT_ICE_MIRROR].n_frames_dirty = NUM_QUEUING_FRAMES;

    strcpy_s(out_materials[MAT_SKULL].name, "skullmat");
    out_materials[MAT_SKULL].mat_cbuffer_index = 3;
    out_materials[MAT_SKULL].diffuse_srvheap_index = 3;
    out_materials[MAT_SKULL].diffuse_albedo = XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);
    out_materials[MAT_SKULL].fresnel_r0 = XMFLOAT3(0.05f, 0.05f, 0.05f);
    out_materials[MAT_SKULL].roughness = 0.3f;
    out_materials[MAT_SKULL].mat_transform = Identity4x4();
    out_materials[MAT_SKULL].n_frames_dirty = NUM_QUEUING_FRAMES;

    strcpy_s(out_materials[MAT_SHADOW].name, "shadowmat");
    out_materials[MAT_SHADOW].mat_cbuffer_index = 4;
    out_materials[MAT_SHADOW].diffuse_srvheap_index = 3;
    out_materials[MAT_SHADOW].diffuse_albedo = XMFLOAT4(0.0f, 0.0f, 0.0f, 0.5f);
    out_materials[MAT_SHADOW].fresnel_r0 = XMFLOAT3(0.001f, 0.001f, 0.001f);
    out_materials[MAT_SHADOW].roughness = 0.0f;
    out_materials[MAT_SHADOW].mat_transform = Identity4x4();
    out_materials[MAT_SHADOW].n_frames_dirty = NUM_QUEUING_FRAMES;
}
static void
create_shape_geometry (D3DRenderContext * render_ctx) {

    // room geometry [from http://www.d3dcoder.net/d3d12.htm]

    // Create and specify geometry.  For this sample we draw a floor
    // and a wall with a mirror on it.  We put the floor, wall, and
    // mirror geometry in one vertex buffer.
    //
    //   |--------------|
    //   |              |
    //   |----|----|----|
    //   |Wall|Mirr|Wall|
    //   |    | or |    |
    //   /--------------/
    //  /   Floor      /
    // /--------------/

    // required sizes
    int nvtx = 20;
    int nidx = 30;

    Vertex *    vertices = (Vertex *)::malloc(sizeof(Vertex) * nvtx);
    uint16_t *  indices = (uint16_t *)::malloc(sizeof(uint16_t) * nidx);
    int i = 0;

    // Floor: Observe we tile texture coordinates.
    vertices[i++] = {.position = {-3.5f, 0.0f, -10.0f}, .normal = {0.0f, 1.0f, 0.0f}, .texc = {0.0f, 4.0f}}; // 0 
    vertices[i++] = {.position = {-3.5f, 0.0f, 0.0f}, .normal = {0.0f, 1.0f, 0.0f }, .texc = { 0.0f, 0.0f}};
    vertices[i++] = {.position = {7.5f, 0.0f, 0.0f}, .normal = {0.0f, 1.0f, 0.0f  }, .texc = { 4.0f, 0.0f}};
    vertices[i++] = {.position = {7.5f, 0.0f, -10.0f}, .normal = {0.0f, 1.0f, 0.0f}, .texc = { 4.0f, 4.0f}};

    // Wall: Observe we tile texture coordinates, and that we
    // leave a gap in the middle for the mirror.
    vertices[i++] = {.position = {-3.5f, 0.0f, 0.0f}, .normal = { 0.0f, 0.0f, -1.0f}, .texc = { 0.0f, 2.0f}}; // 4
    vertices[i++] = {.position = {-3.5f, 4.0f, 0.0f}, .normal = { 0.0f, 0.0f, -1.0f}, .texc = { 0.0f, 0.0f}};
    vertices[i++] = {.position = {-2.5f, 4.0f, 0.0f}, .normal = { 0.0f, 0.0f, -1.0f}, .texc = { 0.5f, 0.0f}};
    vertices[i++] = {.position = {-2.5f, 0.0f, 0.0f}, .normal = { 0.0f, 0.0f, -1.0f}, .texc = { 0.5f, 2.0f}};

    vertices[i++] = {.position = {2.5f, 0.0f, 0.0f}, .normal = { 0.0f, 0.0f, -1.0f}, .texc = { 0.0f, 2.0f}}; // 8 
    vertices[i++] = {.position = {2.5f, 4.0f, 0.0f}, .normal = { 0.0f, 0.0f, -1.0f}, .texc = { 0.0f, 0.0f}};
    vertices[i++] = {.position = {7.5f, 4.0f, 0.0f}, .normal = { 0.0f, 0.0f, -1.0f}, .texc = { 2.0f, 0.0f}};
    vertices[i++] = {.position = {7.5f, 0.0f, 0.0f}, .normal = { 0.0f, 0.0f, -1.0f}, .texc = { 2.0f, 2.0f}};

    vertices[i++] = {.position = {-3.5f, 4.0f, 0.0f}, .normal = { 0.0f, 0.0f, -1.0f}, .texc = { 0.0f, 1.0f}}; // 12
    vertices[i++] = {.position = {-3.5f, 6.0f, 0.0f}, .normal = { 0.0f, 0.0f, -1.0f}, .texc = { 0.0f, 0.0f}};
    vertices[i++] = {.position = {7.5f, 6.0f, 0.0f }, .normal = { 0.0f, 0.0f, -1.0f}, .texc = { 6.0f, 0.0f}};
    vertices[i++] = {.position = {7.5f, 4.0f, 0.0f }, .normal = { 0.0f, 0.0f, -1.0f}, .texc = { 6.0f, 1.0f}};

    // Mirror
    vertices[i++] = {.position = {-2.5f, 0.0f, 0.0f}, .normal = { 0.0f, 0.0f, -1.0f}, .texc = { 0.0f, 1.0f}}; // 16
    vertices[i++] = {.position = {-2.5f, 4.0f, 0.0f}, .normal = { 0.0f, 0.0f, -1.0f}, .texc = { 0.0f, 0.0f}};
    vertices[i++] = {.position = {2.5f, 4.0f, 0.0f }, .normal = { 0.0f, 0.0f, -1.0f}, .texc = { 1.0f, 0.0f}};
    vertices[i]   = {.position = {2.5f, 0.0f, 0.0f }, .normal = { 0.0f, 0.0f, -1.0f}, .texc = { 1.0f, 1.0f}};

    i = 0;
    // Floor
    indices[i++] = 0; indices[i++] = 1; indices[i++] = 2;
    indices[i++] = 0; indices[i++] = 2; indices[i++] = 3;

    // Walls
    indices[i++] = 4; indices[i++] = 5; indices[i++] = 6;
    indices[i++] = 4; indices[i++] = 6; indices[i++] = 7;

    indices[i++] = 8; indices[i++] = 9; indices[i++] = 10;
    indices[i++] = 8; indices[i++] = 10; indices[i++] = 11;

    indices[i++] = 12; indices[i++] = 13; indices[i++] = 14;
    indices[i++] = 12; indices[i++] = 14; indices[i++] = 15;

    // Mirror
    indices[i++] = 16; indices[i++] = 17; indices[i++] = 18;
    indices[i++] = 16; indices[i++] = 18; indices[i++] = 19;

    SubmeshGeometry floor_submesh = {};
    floor_submesh.index_count = 6;
    floor_submesh.start_index_location = 0;
    floor_submesh.base_vertex_location = 0;

    SubmeshGeometry wall_submesh = {};
    wall_submesh.index_count = 18;
    wall_submesh.start_index_location = 6;
    wall_submesh.base_vertex_location = 0;

    SubmeshGeometry mirror_submesh = {};
    mirror_submesh.index_count = 6;
    mirror_submesh.start_index_location = 24;
    mirror_submesh.base_vertex_location = 0;

    UINT vb_byte_size = nvtx * sizeof(Vertex);
    UINT ib_byte_size = nidx * sizeof(uint16_t);

    // -- Fill out geom
    D3DCreateBlob(vb_byte_size, &render_ctx->geom[GEOM_ROOM].vb_cpu);
    if (vertices)
        CopyMemory(render_ctx->geom[GEOM_ROOM].vb_cpu->GetBufferPointer(), vertices, vb_byte_size);

    D3DCreateBlob(ib_byte_size, &render_ctx->geom[GEOM_ROOM].ib_cpu);
    if (indices)
        CopyMemory(render_ctx->geom[GEOM_ROOM].ib_cpu->GetBufferPointer(), indices, ib_byte_size);

    create_default_buffer(render_ctx->device, render_ctx->direct_cmd_list, vertices, vb_byte_size, &render_ctx->geom[GEOM_ROOM].vb_uploader, &render_ctx->geom[GEOM_ROOM].vb_gpu);
    create_default_buffer(render_ctx->device, render_ctx->direct_cmd_list, indices, ib_byte_size, &render_ctx->geom[GEOM_ROOM].ib_uploader, &render_ctx->geom[GEOM_ROOM].ib_gpu);

    render_ctx->geom[GEOM_ROOM].vb_byte_stide = sizeof(Vertex);
    render_ctx->geom[GEOM_ROOM].vb_byte_size = vb_byte_size;
    render_ctx->geom[GEOM_ROOM].ib_byte_size = ib_byte_size;
    render_ctx->geom[GEOM_ROOM].index_format = DXGI_FORMAT_R16_UINT;

    render_ctx->geom[GEOM_ROOM].submesh_names[ROOM_SUBMESH_FLOOR] = "floor";
    render_ctx->geom[GEOM_ROOM].submesh_geoms[ROOM_SUBMESH_FLOOR] = floor_submesh;

    render_ctx->geom[GEOM_ROOM].submesh_names[ROOM_SUBMESH_WALL] = "wall";
    render_ctx->geom[GEOM_ROOM].submesh_geoms[ROOM_SUBMESH_WALL] = wall_submesh;

    render_ctx->geom[GEOM_ROOM].submesh_names[ROOM_SUBMESH_MIRROR] = "mirror";
    render_ctx->geom[GEOM_ROOM].submesh_geoms[ROOM_SUBMESH_MIRROR] = mirror_submesh;

    // -- cleanup
    free(indices);
    free(vertices);
}
static void
create_skull_geometry (D3DRenderContext * render_ctx) {

#pragma region Read_Data_File
    FILE * f = nullptr;
    errno_t err = fopen_s(&f, "./models/skull.txt", "r");
    if (0 == f || err != 0) {
        printf("could not open file\n");
        return;
    }
    char linebuf[100];
    int cnt = 0;
    unsigned vcount = 0;
    unsigned tcount = 0;
    // -- read 1st line
    if (fgets(linebuf, sizeof(linebuf), f))
        cnt = sscanf_s(linebuf, "%*s %d", &vcount);
    if (cnt != 1) {
        printf("read error\n");
        printf("read line: %s\n", linebuf);
        return;
    }
    // -- read 2nd line
    if (fgets(linebuf, sizeof(linebuf), f))
        cnt = sscanf_s(linebuf, "%*s %d", &tcount);
    if (cnt != 1) {
        printf("read error\n");
        printf("read line: %s\n", linebuf);
        return;
    }
    // -- skip two lines
    fgets(linebuf, sizeof(linebuf), f);
    fgets(linebuf, sizeof(linebuf), f);
    // -- read vertices
    Vertex * vertices = (Vertex *)calloc(vcount, sizeof(Vertex));
    for (unsigned i = 0; i < vcount; i++) {
        fgets(linebuf, sizeof(linebuf), f);
        cnt = sscanf_s(
            linebuf, "%f %f %f %f %f %f",
            &vertices[i].position.x, &vertices[i].position.y, &vertices[i].position.z,
            &vertices[i].normal.x, &vertices[i].normal.y, &vertices[i].normal.z
        );
        if (cnt != 6) {
            printf("read error\n");
            printf("read line: %s\n", linebuf);
            return;
        }

#pragma region skull texture coordinates calculations
        XMVECTOR P = XMLoadFloat3(&vertices[i].position);

        // Project point onto unit sphere and generate spherical texture coordinates.
        XMFLOAT3 shpere_pos;
        XMStoreFloat3(&shpere_pos, XMVector3Normalize(P));

        float theta = atan2f(shpere_pos.z, shpere_pos.x);

        // Put in [0, 2pi].
        if (theta < 0.0f)
            theta += XM_2PI;

        float phi = acosf(shpere_pos.y);

        float u = theta / (2.0f * XM_PI);
        float v = phi / XM_PI;

        vertices[i].texc = {u, v};
#pragma endregion

    }
    // -- skip three lines
    fgets(linebuf, sizeof(linebuf), f);
    fgets(linebuf, sizeof(linebuf), f);
    fgets(linebuf, sizeof(linebuf), f);
    // -- read indices
    uint32_t * indices = (uint32_t *)calloc(tcount * 3, sizeof(uint32_t));
    for (unsigned i = 0; i < tcount; i++) {
        fgets(linebuf, sizeof(linebuf), f);
        cnt = sscanf_s(
            linebuf, "%d %d %d",
            &indices[i * 3 + 0], &indices[i * 3 + 1], &indices[i * 3 + 2]
        );
        if (cnt != 3) {
            printf("read error\n");
            printf("read line: %s\n", linebuf);
            return;
        }
    }

    // -- remember to free heap-allocated memory
    /*
    free(vertices);
    free(indices);
    */
    fclose(f);
#pragma endregion   Read_Data_File

    UINT vb_byte_size = vcount * sizeof(Vertex);
    UINT ib_byte_size = (tcount * 3) * sizeof(uint32_t);

    // -- Fill out render_ctx geom[1] (skull)
    D3DCreateBlob(vb_byte_size, &render_ctx->geom[1].vb_cpu);
    CopyMemory(render_ctx->geom[1].vb_cpu->GetBufferPointer(), vertices, vb_byte_size);

    D3DCreateBlob(ib_byte_size, &render_ctx->geom[1].ib_cpu);
    CopyMemory(render_ctx->geom[1].ib_cpu->GetBufferPointer(), indices, ib_byte_size);

    create_default_buffer(render_ctx->device, render_ctx->direct_cmd_list, vertices, vb_byte_size, &render_ctx->geom[1].vb_uploader, &render_ctx->geom[1].vb_gpu);
    create_default_buffer(render_ctx->device, render_ctx->direct_cmd_list, indices, ib_byte_size, &render_ctx->geom[1].ib_uploader, &render_ctx->geom[1].ib_gpu);

    render_ctx->geom[GEOM_SKULL].vb_byte_stide = sizeof(Vertex);
    render_ctx->geom[GEOM_SKULL].vb_byte_size = vb_byte_size;
    render_ctx->geom[GEOM_SKULL].ib_byte_size = ib_byte_size;
    render_ctx->geom[GEOM_SKULL].index_format = DXGI_FORMAT_R32_UINT;

    SubmeshGeometry submesh = {};
    submesh.index_count = tcount * 3;
    submesh.start_index_location = 0;
    submesh.base_vertex_location = 0;

    render_ctx->geom[GEOM_SKULL].submesh_names[0] = "skull";
    render_ctx->geom[GEOM_SKULL].submesh_geoms[0] = submesh;

    // -- cleanup
    free(vertices);
    free(indices);
}
static void
create_render_items (
    RenderItemArray * all_ritems,
    RenderItemArray * opaque_ritems,
    RenderItemArray * transparent_ritems,
    RenderItemArray * alphatested_ritems,
    RenderItemArray * mirrors_ritems,
    RenderItemArray * reflected_ritems,
    RenderItemArray * shadows_ritems,
    RenderItemArray * reflected_shadow_ritems,
    MeshGeometry * room_geom, MeshGeometry * skull_geom,
    Material materials []
) {
    // floor
    all_ritems->ritems[RITEM_FLOOR].world = Identity4x4();
    all_ritems->ritems[RITEM_FLOOR].tex_transform = Identity4x4();
    all_ritems->ritems[RITEM_FLOOR].obj_cbuffer_index = 0;
    all_ritems->ritems[RITEM_FLOOR].mat = &materials[MAT_CHECKER_TILE];
    all_ritems->ritems[RITEM_FLOOR].geometry = room_geom;
    all_ritems->ritems[RITEM_FLOOR].primitive_type = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    all_ritems->ritems[RITEM_FLOOR].index_count = room_geom->submesh_geoms[ROOM_SUBMESH_FLOOR].index_count;
    all_ritems->ritems[RITEM_FLOOR].start_index_loc = room_geom->submesh_geoms[ROOM_SUBMESH_FLOOR].start_index_location;
    all_ritems->ritems[RITEM_FLOOR].base_vertex_loc = room_geom->submesh_geoms[ROOM_SUBMESH_FLOOR].base_vertex_location;
    all_ritems->ritems[RITEM_FLOOR].n_frames_dirty = NUM_QUEUING_FRAMES;
    all_ritems->ritems[RITEM_FLOOR].mat->n_frames_dirty = NUM_QUEUING_FRAMES;
    all_ritems->ritems[RITEM_FLOOR].initialized = true;
    all_ritems->size++;
    opaque_ritems->ritems[0] = all_ritems->ritems[RITEM_FLOOR];
    opaque_ritems->size++;

    // wall
    all_ritems->ritems[RITEM_WALL].world = Identity4x4();
    all_ritems->ritems[RITEM_WALL].tex_transform = Identity4x4();
    all_ritems->ritems[RITEM_WALL].obj_cbuffer_index = 1;
    all_ritems->ritems[RITEM_WALL].mat = &materials[MAT_BRICKS];
    all_ritems->ritems[RITEM_WALL].geometry = room_geom;
    all_ritems->ritems[RITEM_WALL].primitive_type = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    all_ritems->ritems[RITEM_WALL].index_count = room_geom->submesh_geoms[ROOM_SUBMESH_WALL].index_count;
    all_ritems->ritems[RITEM_WALL].start_index_loc = room_geom->submesh_geoms[ROOM_SUBMESH_WALL].start_index_location;
    all_ritems->ritems[RITEM_WALL].base_vertex_loc = room_geom->submesh_geoms[ROOM_SUBMESH_WALL].base_vertex_location;
    all_ritems->ritems[RITEM_WALL].n_frames_dirty = NUM_QUEUING_FRAMES;
    all_ritems->ritems[RITEM_WALL].mat->n_frames_dirty = NUM_QUEUING_FRAMES;
    all_ritems->ritems[RITEM_WALL].initialized = true;
    all_ritems->size++;
    opaque_ritems->ritems[1] = all_ritems->ritems[RITEM_WALL];
    opaque_ritems->size++;

    // skull
    all_ritems->ritems[RITEM_SKULL].world = Identity4x4();
    all_ritems->ritems[RITEM_SKULL].tex_transform = Identity4x4();
    all_ritems->ritems[RITEM_SKULL].obj_cbuffer_index = 2;
    all_ritems->ritems[RITEM_SKULL].mat = &materials[MAT_SKULL];
    all_ritems->ritems[RITEM_SKULL].geometry = skull_geom;
    all_ritems->ritems[RITEM_SKULL].primitive_type = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    all_ritems->ritems[RITEM_SKULL].index_count = skull_geom->submesh_geoms[0].index_count;
    all_ritems->ritems[RITEM_SKULL].start_index_loc = skull_geom->submesh_geoms[0].start_index_location;
    all_ritems->ritems[RITEM_SKULL].base_vertex_loc = skull_geom->submesh_geoms[0].base_vertex_location;
    all_ritems->ritems[RITEM_SKULL].n_frames_dirty = NUM_QUEUING_FRAMES;
    all_ritems->ritems[RITEM_SKULL].mat->n_frames_dirty = NUM_QUEUING_FRAMES;
    all_ritems->ritems[RITEM_SKULL].initialized = true;
    all_ritems->size++;
    opaque_ritems->ritems[2] = all_ritems->ritems[RITEM_SKULL];
    opaque_ritems->size++;

    // reflected skull will have different world matrix, so it needs to be its own render item.
    all_ritems->ritems[RITEM_REFLECTED_SKULL] = all_ritems->ritems[RITEM_SKULL];
    all_ritems->ritems[RITEM_REFLECTED_SKULL].obj_cbuffer_index = 3;
    all_ritems->size++;
    reflected_ritems->ritems[0] = all_ritems->ritems[RITEM_REFLECTED_SKULL];
    reflected_ritems->size++;
    // reflected skull world matrix calculated later

    // reflected floor.
    all_ritems->ritems[RITEM_REFLECTED_FLOOR] = all_ritems->ritems[RITEM_FLOOR];
    all_ritems->ritems[RITEM_REFLECTED_FLOOR].obj_cbuffer_index = 4;
    all_ritems->size++;
    // calculate reflected_floor world matrix
    XMVECTOR mirror_plane = XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f); // xy plane
    XMMATRIX R = XMMatrixReflect(mirror_plane);
    XMMATRIX floor_world = XMLoadFloat4x4(&all_ritems->ritems[RITEM_REFLECTED_FLOOR].world);
    XMStoreFloat4x4(&all_ritems->ritems[RITEM_REFLECTED_FLOOR].world, floor_world * R);
    reflected_ritems->ritems[1] = all_ritems->ritems[RITEM_REFLECTED_FLOOR];
    reflected_ritems->size++;

    // shadowed skull will have different world matrix, so it needs to be its own render item.
    all_ritems->ritems[RITEM_SHADOWED_SKULL] = all_ritems->ritems[RITEM_SKULL];
    all_ritems->ritems[RITEM_SHADOWED_SKULL].obj_cbuffer_index = 5;
    all_ritems->ritems[RITEM_SHADOWED_SKULL].mat = &materials[MAT_SHADOW];
    all_ritems->size++;
    shadows_ritems->ritems[0] = all_ritems->ritems[RITEM_SHADOWED_SKULL];
    shadows_ritems->size++;

    // reflected shadow of the skull
    all_ritems->ritems[RITEM_REFLECTED_SHADOW] = all_ritems->ritems[RITEM_SHADOWED_SKULL];
    all_ritems->ritems[RITEM_REFLECTED_SHADOW].obj_cbuffer_index = 6;
    all_ritems->size++;
    reflected_shadow_ritems->ritems[0] = all_ritems->ritems[RITEM_REFLECTED_SHADOW];
    reflected_shadow_ritems->size++;

    // mirror
    all_ritems->ritems[RITEM_MIRROR].world = Identity4x4();
    all_ritems->ritems[RITEM_MIRROR].tex_transform = Identity4x4();
    all_ritems->ritems[RITEM_MIRROR].obj_cbuffer_index = 7;
    all_ritems->ritems[RITEM_MIRROR].mat = &materials[MAT_ICE_MIRROR];
    all_ritems->ritems[RITEM_MIRROR].geometry = room_geom;
    all_ritems->ritems[RITEM_MIRROR].primitive_type = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    all_ritems->ritems[RITEM_MIRROR].index_count = room_geom->submesh_geoms[ROOM_SUBMESH_MIRROR].index_count;
    all_ritems->ritems[RITEM_MIRROR].start_index_loc = room_geom->submesh_geoms[ROOM_SUBMESH_MIRROR].start_index_location;
    all_ritems->ritems[RITEM_MIRROR].base_vertex_loc = room_geom->submesh_geoms[ROOM_SUBMESH_MIRROR].base_vertex_location;
    all_ritems->ritems[RITEM_MIRROR].n_frames_dirty = NUM_QUEUING_FRAMES;
    all_ritems->ritems[RITEM_MIRROR].mat->n_frames_dirty = NUM_QUEUING_FRAMES;
    all_ritems->ritems[RITEM_MIRROR].initialized = true;
    all_ritems->size++;
    mirrors_ritems->ritems[0] = all_ritems->ritems[RITEM_MIRROR];
    mirrors_ritems->size++;
    transparent_ritems->ritems[0] = all_ritems->ritems[RITEM_MIRROR];
    transparent_ritems->size++;
}
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
    srv_heap_desc.NumDescriptors = _COUNT_TEX + 1 /* imgui descriptor */;
    srv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srv_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    render_ctx->device->CreateDescriptorHeap(&srv_heap_desc, IID_PPV_ARGS(&render_ctx->srv_heap));

    // Fill out the heap with actual descriptors
    D3D12_CPU_DESCRIPTOR_HANDLE descriptor_cpu_handle = render_ctx->srv_heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc = {};

    // Brick texture
    ID3D12Resource * brick_tex = render_ctx->textures[TEX_BRICK].resource;
    srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv_desc.Format = brick_tex->GetDesc().Format;
    srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv_desc.Texture2D.MostDetailedMip = 0;
    srv_desc.Texture2D.MipLevels = brick_tex->GetDesc().MipLevels;
    srv_desc.Texture2D.ResourceMinLODClamp = 0.0f;
    render_ctx->device->CreateShaderResourceView(brick_tex, &srv_desc, descriptor_cpu_handle);

    // checkerboard texture
    ID3D12Resource * checkerboard_tex = render_ctx->textures[TEX_CHECKERBOARD].resource;
    memset(&srv_desc, 0, sizeof(srv_desc)); // reset desc
    srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv_desc.Format = checkerboard_tex->GetDesc().Format;
    srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv_desc.Texture2D.MostDetailedMip = 0;
    srv_desc.Texture2D.MipLevels = checkerboard_tex->GetDesc().MipLevels;
    srv_desc.Texture2D.ResourceMinLODClamp = 0.0f;
    descriptor_cpu_handle.ptr += render_ctx->cbv_srv_uav_descriptor_size;   // next descriptor
    render_ctx->device->CreateShaderResourceView(checkerboard_tex, &srv_desc, descriptor_cpu_handle);

    // Ice texture
    ID3D12Resource * ice_tex = render_ctx->textures[TEX_ICE].resource;
    memset(&srv_desc, 0, sizeof(srv_desc)); // reset desc
    srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv_desc.Format = ice_tex->GetDesc().Format;
    srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv_desc.Texture2D.MostDetailedMip = 0;
    srv_desc.Texture2D.MipLevels = ice_tex->GetDesc().MipLevels;
    srv_desc.Texture2D.ResourceMinLODClamp = 0.0f;
    descriptor_cpu_handle.ptr += render_ctx->cbv_srv_uav_descriptor_size;   // next descriptor
    render_ctx->device->CreateShaderResourceView(ice_tex, &srv_desc, descriptor_cpu_handle);

    // White texture
    ID3D12Resource * white_tex = render_ctx->textures[TEX_WHITE1x1].resource;
    memset(&srv_desc, 0, sizeof(srv_desc)); // reset desc
    srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv_desc.Format = white_tex->GetDesc().Format;
    srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv_desc.Texture2D.MostDetailedMip = 0;
    srv_desc.Texture2D.MipLevels = white_tex->GetDesc().MipLevels;
    srv_desc.Texture2D.ResourceMinLODClamp = 0.0f;
    descriptor_cpu_handle.ptr += render_ctx->cbv_srv_uav_descriptor_size;   // next descriptor
    render_ctx->device->CreateShaderResourceView(white_tex, &srv_desc, descriptor_cpu_handle);

    // Create Render Target View Descriptor Heap
    D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_desc = {};
    rtv_heap_desc.NumDescriptors = NUM_BACKBUFFERS;
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
    slot_root_params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

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
    root_sig_desc.NumParameters = 4;
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
static void
create_pso (D3DRenderContext * render_ctx, IDxcBlob * vertex_shader_code, IDxcBlob * pixel_shader_code_opaque, IDxcBlob * pixel_shader_code_alphatested) {
    // -- Create vertex-input-layout Elements

    D3D12_INPUT_ELEMENT_DESC input_desc[3];
    input_desc[0] = {};
    input_desc[0].SemanticName = "POSITION";
    input_desc[0].SemanticIndex = 0;
    input_desc[0].Format = DXGI_FORMAT_R32G32B32_FLOAT;
    input_desc[0].InputSlot = 0;
    input_desc[0].AlignedByteOffset = 0;
    input_desc[0].InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;

    input_desc[1] = {};
    input_desc[1].SemanticName = "NORMAL";
    input_desc[1].SemanticIndex = 0;
    input_desc[1].Format = DXGI_FORMAT_R32G32B32_FLOAT;
    input_desc[1].InputSlot= 0;
    input_desc[1].AlignedByteOffset = 12; // bc of the position byte-size
    input_desc[1].InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;

    input_desc[2] = {};
    input_desc[2].SemanticName = "TEXCOORD";
    input_desc[2].SemanticIndex = 0;
    input_desc[2].Format = DXGI_FORMAT_R32G32_FLOAT;
    input_desc[2].InputSlot = 0;
    input_desc[2].AlignedByteOffset = 24; // bc of the position and normal
    input_desc[2].InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;

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

    /* Depth Stencil Description */
    D3D12_DEPTH_STENCIL_DESC def_dss = {};
    def_dss.DepthEnable = TRUE;
    def_dss.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    def_dss.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    def_dss.StencilEnable = FALSE;
    def_dss.StencilReadMask = D3D12_DEFAULT_STENCIL_READ_MASK;
    def_dss.StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK;
    D3D12_DEPTH_STENCILOP_DESC def_stencil_op = {D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_COMPARISON_FUNC_ALWAYS};
    def_dss.FrontFace = def_stencil_op;
    def_dss.BackFace = def_stencil_op;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC opaque_pso_desc = {};
    opaque_pso_desc.pRootSignature = render_ctx->root_signature;
    opaque_pso_desc.VS.pShaderBytecode = vertex_shader_code->GetBufferPointer();
    opaque_pso_desc.VS.BytecodeLength = vertex_shader_code->GetBufferSize();
    opaque_pso_desc.PS.pShaderBytecode = pixel_shader_code_opaque->GetBufferPointer();
    opaque_pso_desc.PS.BytecodeLength = pixel_shader_code_opaque->GetBufferSize();
    opaque_pso_desc.BlendState = def_blend_desc;
    opaque_pso_desc.SampleMask = UINT_MAX;
    opaque_pso_desc.RasterizerState = def_rasterizer_desc;
    opaque_pso_desc.DepthStencilState = def_dss;
    opaque_pso_desc.DSVFormat = render_ctx->depthstencil_format;
    opaque_pso_desc.InputLayout.pInputElementDescs = input_desc;
    opaque_pso_desc.InputLayout.NumElements = ARRAY_COUNT(input_desc);
    opaque_pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE::D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    opaque_pso_desc.NumRenderTargets = 1;
    opaque_pso_desc.RTVFormats[0] = render_ctx->backbuffer_format;
    opaque_pso_desc.SampleDesc.Count = 1;
    opaque_pso_desc.SampleDesc.Quality = 0;

    render_ctx->device->CreateGraphicsPipelineState(&opaque_pso_desc, IID_PPV_ARGS(&render_ctx->psos[LAYER_OPAQUE]));
    //
    // -- Create PSO for Transparent objs
    //
    D3D12_GRAPHICS_PIPELINE_STATE_DESC transparent_pso_desc = opaque_pso_desc;

    D3D12_RENDER_TARGET_BLEND_DESC transparency_blend_desc = {};
    transparency_blend_desc.BlendEnable = true;
    transparency_blend_desc.LogicOpEnable = false;
    transparency_blend_desc.SrcBlend = D3D12_BLEND_SRC_ALPHA;
    transparency_blend_desc.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    transparency_blend_desc.BlendOp = D3D12_BLEND_OP_ADD;
    transparency_blend_desc.SrcBlendAlpha = D3D12_BLEND_ONE;
    transparency_blend_desc.DestBlendAlpha = D3D12_BLEND_ZERO;
    transparency_blend_desc.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    transparency_blend_desc.LogicOp = D3D12_LOGIC_OP_NOOP;
    transparency_blend_desc.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    transparent_pso_desc.BlendState.RenderTarget[0] = transparency_blend_desc;
    render_ctx->device->CreateGraphicsPipelineState(&transparent_pso_desc, IID_PPV_ARGS(&render_ctx->psos[LAYER_TRANSPARENT]));
    //
    // -- Create PSO for AlphaTested objs
    //
    D3D12_GRAPHICS_PIPELINE_STATE_DESC alpha_pso_desc = opaque_pso_desc;
    alpha_pso_desc.PS.pShaderBytecode = pixel_shader_code_alphatested->GetBufferPointer();
    alpha_pso_desc.PS.BytecodeLength = pixel_shader_code_alphatested->GetBufferSize();
    alpha_pso_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    render_ctx->device->CreateGraphicsPipelineState(&alpha_pso_desc, IID_PPV_ARGS(&render_ctx->psos[LAYER_ALPHATESTED]));
    //
    // -- Create PSO for marking stencil mirrors
    //
    D3D12_BLEND_DESC mirror_blend_desc = def_blend_desc;
    mirror_blend_desc.RenderTarget[0].RenderTargetWriteMask = 0;    // disable write to backbuffer

    D3D12_DEPTH_STENCIL_DESC mirror_dss = {};
    mirror_dss.DepthEnable = true;
    mirror_dss.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    mirror_dss.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    mirror_dss.StencilEnable = true;
    mirror_dss.StencilReadMask = 0xff;
    mirror_dss.StencilWriteMask = 0xff;

    mirror_dss.FrontFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
    mirror_dss.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
    mirror_dss.FrontFace.StencilPassOp = D3D12_STENCIL_OP_REPLACE;
    mirror_dss.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;

    // Not rendering backfacing polygons so these don't matter
    mirror_dss.BackFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
    mirror_dss.BackFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
    mirror_dss.BackFace.StencilPassOp = D3D12_STENCIL_OP_REPLACE;
    mirror_dss.BackFace.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC mirror_pso_desc = opaque_pso_desc;
    mirror_pso_desc.BlendState = mirror_blend_desc;
    mirror_pso_desc.DepthStencilState = mirror_dss;
    render_ctx->device->CreateGraphicsPipelineState(&mirror_pso_desc, IID_PPV_ARGS(&render_ctx->psos[LAYER_MIRRORS]));
    //
    // -- Create PSO for stencil reflections
    //
    D3D12_DEPTH_STENCIL_DESC reflect_dss = {};
    reflect_dss.DepthEnable = true;
    reflect_dss.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    reflect_dss.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    reflect_dss.StencilEnable = true;
    reflect_dss.StencilReadMask = 0xff;
    reflect_dss.StencilWriteMask = 0xff;

    reflect_dss.FrontFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
    reflect_dss.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
    reflect_dss.FrontFace.StencilPassOp = D3D12_STENCIL_OP_KEEP;
    reflect_dss.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_EQUAL;

    // Not rendering backfacing polygons so these don't matter
    reflect_dss.BackFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
    reflect_dss.BackFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
    reflect_dss.BackFace.StencilPassOp = D3D12_STENCIL_OP_KEEP;
    reflect_dss.BackFace.StencilFunc = D3D12_COMPARISON_FUNC_EQUAL;

    // correct winding order for reflected objects before creating pso
    D3D12_GRAPHICS_PIPELINE_STATE_DESC reflect_pso_desc = opaque_pso_desc;
    reflect_pso_desc.DepthStencilState = reflect_dss;
    reflect_pso_desc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
    reflect_pso_desc.RasterizerState.FrontCounterClockwise = true;
    render_ctx->device->CreateGraphicsPipelineState(&reflect_pso_desc, IID_PPV_ARGS(&render_ctx->psos[LAYER_REFLECTIONS]));
    //
    // -- Create PSO for shadow objects
    //
    D3D12_DEPTH_STENCIL_DESC shadow_dss = {};
    shadow_dss.DepthEnable = true;
    shadow_dss.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    shadow_dss.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    shadow_dss.StencilEnable = true;
    shadow_dss.StencilReadMask = 0xff;
    shadow_dss.StencilWriteMask = 0xff;

    shadow_dss.FrontFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
    shadow_dss.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
    shadow_dss.FrontFace.StencilPassOp = D3D12_STENCIL_OP_INCR; // to prevent double blending
    shadow_dss.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_EQUAL;

    // Not rendering backfacing polygons so these don't matter
    shadow_dss.BackFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
    shadow_dss.BackFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
    shadow_dss.BackFace.StencilPassOp = D3D12_STENCIL_OP_INCR;
    shadow_dss.BackFace.StencilFunc = D3D12_COMPARISON_FUNC_EQUAL;

    // we draw shadows with transparency, so base it off the transparency description.
    D3D12_GRAPHICS_PIPELINE_STATE_DESC shadow_pso_desc = transparent_pso_desc;
    shadow_pso_desc.DepthStencilState = shadow_dss;
    render_ctx->device->CreateGraphicsPipelineState(&shadow_pso_desc, IID_PPV_ARGS(&render_ctx->psos[LAYER_SHADOW]));

    //
    // -- Create PSO for reflected shadows (need both transparency and reflection stencil)
    //
    // ? can use LAYER_SHADOW pso ?
}
static void
handle_keyboard_input (
    RenderItem * skull,
    RenderItem * reflected_skull,
    RenderItem * reflected_skull_shadow,
    RenderItem * shadowed_skull, XMFLOAT3 * light_dir,
    SceneContext * scene_ctx, GameTimer * gt
) {
    /*
        skull position / translation are handled here!
    */
    float dt = gt->delta_time;

    // Handle user inputs


    // Don't let user move below ground plane.
    scene_ctx->skull_translation.y = (scene_ctx->skull_translation.y > 0.0f) ? scene_ctx->skull_translation.y : 0.0f;

    // Update the new world matrix.
    XMMATRIX skull_rotate = XMMatrixRotationY(0.5f * XM_PI);
    XMMATRIX skull_scale = XMMatrixScaling(0.45f, 0.45f, 0.45f);
    XMMATRIX skull_offset = XMMatrixTranslation(scene_ctx->skull_translation.x, scene_ctx->skull_translation.y, scene_ctx->skull_translation.z);
    XMMATRIX skull_world = skull_rotate * skull_scale * skull_offset;
    XMStoreFloat4x4(&skull->world, skull_world);

    // Update reflection world matrix.
    XMVECTOR mirror_plane = XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f); // xy plane
    XMMATRIX R = XMMatrixReflect(mirror_plane);
    XMStoreFloat4x4(&reflected_skull->world, skull_world * R);

    // Update shadow world matrix.
    XMVECTOR shadow_plane = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f); // xz plane
    XMVECTOR to_main_light = -XMLoadFloat3(light_dir);
    XMMATRIX S = XMMatrixShadow(shadow_plane, to_main_light);
    XMMATRIX shadow_y_offset = XMMatrixTranslation(0.0f, 0.001f, 0.0f);
    XMStoreFloat4x4(&shadowed_skull->world, skull_world * S * shadow_y_offset);

    // Update reflected skull shadow world matrix.
    XMMATRIX reflected_shadow_world = XMLoadFloat4x4(&shadowed_skull->world);
    XMStoreFloat4x4(&reflected_skull_shadow->world, reflected_shadow_world * R);

    skull->n_frames_dirty = NUM_QUEUING_FRAMES;
    reflected_skull->n_frames_dirty = NUM_QUEUING_FRAMES;
    reflected_skull_shadow->n_frames_dirty = NUM_QUEUING_FRAMES;
    shadowed_skull->n_frames_dirty = NUM_QUEUING_FRAMES;

}
static void
handle_mouse_move (SceneContext * scene_ctx, WPARAM wParam, int x, int y) {
    if (global_mouse_active) {
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
    XMStoreFloat4x4(&sc->view, view);
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
            XMMATRIX world = XMLoadFloat4x4(&render_ctx->all_ritems.ritems[i].world);
            XMMATRIX tex_transform = XMLoadFloat4x4(&render_ctx->all_ritems.ritems[i].tex_transform);

            ObjectConstants obj_cbuffer = {};
            XMStoreFloat4x4(&obj_cbuffer.world, XMMatrixTranspose(world));
            XMStoreFloat4x4(&obj_cbuffer.tex_transform, XMMatrixTranspose(tex_transform));

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
            XMMATRIX mat_transform = XMLoadFloat4x4(&mat->mat_transform);

            MaterialConstants mat_constants;
            mat_constants.diffuse_albedo = render_ctx->materials[i].diffuse_albedo;
            mat_constants.fresnel_r0 = render_ctx->materials[i].fresnel_r0;
            mat_constants.roughness = render_ctx->materials[i].roughness;
            XMStoreFloat4x4(&mat_constants.mat_transform, XMMatrixTranspose(mat_transform));

            uint8_t * mat_ptr = render_ctx->frame_resources[frame_index].mat_cb_data_ptr + ((UINT64)mat->mat_cbuffer_index * cbuffer_size);
            memcpy(mat_ptr, &mat_constants, cbuffer_size);

            // Next FrameResource need to be updated too.
            mat->n_frames_dirty--;
        }
    }
}
static void
update_main_pass_cbuffers (D3DRenderContext * render_ctx, GameTimer * timer) {

    XMMATRIX view = XMLoadFloat4x4(&global_scene_ctx.view);
    XMMATRIX proj = XMLoadFloat4x4(&global_scene_ctx.proj);

    XMMATRIX view_proj = XMMatrixMultiply(view, proj);
    XMVECTOR det_view = XMMatrixDeterminant(view);
    XMMATRIX inv_view = XMMatrixInverse(&det_view, view);
    XMVECTOR det_proj = XMMatrixDeterminant(proj);
    XMMATRIX inv_proj = XMMatrixInverse(&det_proj, proj);
    XMVECTOR det_view_proj = XMMatrixDeterminant(view_proj);
    XMMATRIX inv_view_proj = XMMatrixInverse(&det_view_proj, view_proj);

    XMStoreFloat4x4(&render_ctx->main_pass_constants.view, XMMatrixTranspose(view));
    XMStoreFloat4x4(&render_ctx->main_pass_constants.inverse_view, XMMatrixTranspose(inv_view));
    XMStoreFloat4x4(&render_ctx->main_pass_constants.proj, XMMatrixTranspose(proj));
    XMStoreFloat4x4(&render_ctx->main_pass_constants.inverse_proj, XMMatrixTranspose(inv_proj));
    XMStoreFloat4x4(&render_ctx->main_pass_constants.view_proj, XMMatrixTranspose(view_proj));
    XMStoreFloat4x4(&render_ctx->main_pass_constants.inverse_view_proj, XMMatrixTranspose(inv_view_proj));
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
static void
update_reflected_pass_cbuffers (D3DRenderContext * render_ctx, GameTimer * timer) {

    render_ctx->reflected_pass_constants = render_ctx->main_pass_constants;

    XMVECTOR mirror_plane = XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f);
    XMMATRIX R = XMMatrixReflect(mirror_plane);

    // Reflect the lighting.
    for (int i = 0; i < 3; ++i) {
        XMVECTOR light_dir = XMLoadFloat3(&render_ctx->main_pass_constants.lights[i].direction);
        XMVECTOR reflected_light_dir = XMVector3TransformNormal(light_dir, R);
        XMStoreFloat3(&render_ctx->reflected_pass_constants.lights[i].direction, reflected_light_dir);
    }

    // Reflected pass stored in index 1
    uint8_t * pass_ptr = render_ctx->frame_resources[render_ctx->frame_index].pass_cb_data_ptr + (1 * sizeof(PassConstants));
    memcpy(pass_ptr, &render_ctx->reflected_pass_constants, sizeof(PassConstants));
}
static UINT64
move_to_next_frame (D3DRenderContext * render_ctx, UINT * out_frame_index, UINT * out_backbuffer_index) {
    UINT frame_index = *out_frame_index;

    // -- 1. schedule a signal command in the queue
    // i.e., notify the fence when the GPU completes commands up to this fence point.
    UINT64 current_fence_value = render_ctx->frame_resources[frame_index].fence;
    render_ctx->cmd_queue->Signal(render_ctx->fence, current_fence_value);

    // -- 2. update frame index
    //*out_backbuffer_index = render_ctx->swapchain3->GetCurrentBackBufferIndex();
    *out_backbuffer_index = (*out_backbuffer_index + 1) % NUM_BACKBUFFERS;
    *out_frame_index = (render_ctx->frame_index + 1) % NUM_QUEUING_FRAMES;

    // -- 3. if the next frame is not ready to be rendered yet, wait until it is ready
    if (render_ctx->fence->GetCompletedValue() < render_ctx->frame_resources[frame_index].fence) {
        render_ctx->fence->SetEventOnCompletion(render_ctx->frame_resources[frame_index].fence, render_ctx->fence_event);
        WaitForSingleObjectEx(render_ctx->fence_event, INFINITE /*return only when the object is signaled*/, false);
    }

    // -- 4. set the fence value for the next frame
    // i.e., advance the fence value to mark commands up to this fence point.
    render_ctx->frame_resources[frame_index].fence = ++current_fence_value;

    return current_fence_value;
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
static D3D12_RESOURCE_BARRIER
create_barrier (ID3D12Resource * resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    return barrier;
}
static void
draw_main (D3DRenderContext * render_ctx) {
    UINT frame_index = render_ctx->frame_index;
    UINT backbuffer_index = render_ctx->backbuffer_index;

    // Populate command list

    // -- reset cmd_allocator and cmd_list
    render_ctx->frame_resources[frame_index].cmd_list_alloc->Reset();
    render_ctx->direct_cmd_list->Reset(
        render_ctx->frame_resources[frame_index].cmd_list_alloc,
        render_ctx->psos[LAYER_OPAQUE]
    );

    // -- set viewport and scissor
    render_ctx->direct_cmd_list->RSSetViewports(1, &render_ctx->viewport);
    render_ctx->direct_cmd_list->RSSetScissorRects(1, &render_ctx->scissor_rect);

    // -- indicate that the backbuffer will be used as the render target
    D3D12_RESOURCE_BARRIER barrier1 = create_barrier(render_ctx->render_targets[backbuffer_index], D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    render_ctx->direct_cmd_list->ResourceBarrier(1, &barrier1);

    // -- get CPU descriptor handle that represents the start of the rtv heap
    D3D12_CPU_DESCRIPTOR_HANDLE dsv_handle = render_ctx->dsv_heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle = render_ctx->rtv_heap->GetCPUDescriptorHandleForHeapStart();
    rtv_handle.ptr = SIZE_T(INT64(rtv_handle.ptr) + INT64(render_ctx->backbuffer_index) * INT64(render_ctx->rtv_descriptor_size));    // -- apply initial offset

    render_ctx->direct_cmd_list->ClearRenderTargetView(rtv_handle, (float *)&render_ctx->main_pass_constants.fog_color, 0, nullptr);
    render_ctx->direct_cmd_list->ClearDepthStencilView(dsv_handle, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);
    render_ctx->direct_cmd_list->OMSetRenderTargets(1, &rtv_handle, true, &dsv_handle);

    ID3D12DescriptorHeap* descriptor_heaps [] = {render_ctx->srv_heap};
    render_ctx->direct_cmd_list->SetDescriptorHeaps(_countof(descriptor_heaps), descriptor_heaps);

    render_ctx->direct_cmd_list->SetGraphicsRootSignature(render_ctx->root_signature);

    // Bind [default] per-pass constant buffer. We only need to do this once per-pass.
    ID3D12Resource * pass_cb = render_ctx->frame_resources[frame_index].pass_cb;
    render_ctx->direct_cmd_list->SetGraphicsRootConstantBufferView(2, pass_cb->GetGPUVirtualAddress());

    // 1. draw opaque objs first (opaque pso is currently used)
    draw_render_items(
        render_ctx->direct_cmd_list,
        render_ctx->frame_resources[frame_index].obj_cb,
        render_ctx->frame_resources[frame_index].mat_cb,
        render_ctx->cbv_srv_uav_descriptor_size,
        render_ctx->srv_heap,
        &render_ctx->opaque_ritems, frame_index
    );
    // 2. draw mirrors only to stencil buffer, i.e., mark visible mirror pixels in stencil buffer with value 1
    render_ctx->direct_cmd_list->OMSetStencilRef(1);
    render_ctx->direct_cmd_list->SetPipelineState(render_ctx->psos[LAYER_MIRRORS]);
    draw_render_items(
        render_ctx->direct_cmd_list,
        render_ctx->frame_resources[frame_index].obj_cb,
        render_ctx->frame_resources[frame_index].mat_cb,
        render_ctx->cbv_srv_uav_descriptor_size,
        render_ctx->srv_heap,
        &render_ctx->mirrors_ritems, frame_index
    );
    // 3. draw reflections, only into the mirror (only for pixels where stencil buffer is 1)
    // Use a different pass_cb for light reflection!
    render_ctx->direct_cmd_list->SetGraphicsRootConstantBufferView(2, pass_cb->GetGPUVirtualAddress() + (1 * sizeof(PassConstants)));
    render_ctx->direct_cmd_list->SetPipelineState(render_ctx->psos[LAYER_REFLECTIONS]);
    draw_render_items(
        render_ctx->direct_cmd_list,
        render_ctx->frame_resources[frame_index].obj_cb,
        render_ctx->frame_resources[frame_index].mat_cb,
        render_ctx->cbv_srv_uav_descriptor_size,
        render_ctx->srv_heap,
        &render_ctx->reflected_ritems, frame_index
    );
    // 3.1 draw skull shadow reflection
    render_ctx->direct_cmd_list->SetPipelineState(render_ctx->psos[LAYER_SHADOW]);
    draw_render_items(
        render_ctx->direct_cmd_list,
        render_ctx->frame_resources[frame_index].obj_cb,
        render_ctx->frame_resources[frame_index].mat_cb,
        render_ctx->cbv_srv_uav_descriptor_size,
        render_ctx->srv_heap,
        &render_ctx->reflected_shadow_ritems, frame_index
    );

    // 4. draw mirrors, this time into backbuffer (with transparency blending)
    // Restore [default] pass_cb and stencil ref
    render_ctx->direct_cmd_list->SetGraphicsRootConstantBufferView(2, pass_cb->GetGPUVirtualAddress());
    render_ctx->direct_cmd_list->OMSetStencilRef(0);
    render_ctx->direct_cmd_list->SetPipelineState(render_ctx->psos[LAYER_TRANSPARENT]);
    draw_render_items(
        render_ctx->direct_cmd_list,
        render_ctx->frame_resources[frame_index].obj_cb,
        render_ctx->frame_resources[frame_index].mat_cb,
        render_ctx->cbv_srv_uav_descriptor_size,
        render_ctx->srv_heap,
        &render_ctx->transparent_ritems, frame_index
    );

    // 5. draw skull shadows
    render_ctx->direct_cmd_list->SetPipelineState(render_ctx->psos[LAYER_SHADOW]);
    draw_render_items(
        render_ctx->direct_cmd_list,
        render_ctx->frame_resources[frame_index].obj_cb,
        render_ctx->frame_resources[frame_index].mat_cb,
        render_ctx->cbv_srv_uav_descriptor_size,
        render_ctx->srv_heap,
        &render_ctx->shadow_ritems, frame_index
    );

    // Imgui draw call
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), render_ctx->direct_cmd_list);

    // -- indicate that the backbuffer will now be used to present
    D3D12_RESOURCE_BARRIER barrier2 = create_barrier(render_ctx->render_targets[backbuffer_index], D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    render_ctx->direct_cmd_list->ResourceBarrier(1, &barrier2);

    // -- finish populating command list
    render_ctx->direct_cmd_list->Close();

    ID3D12CommandList * cmd_lists [] = {render_ctx->direct_cmd_list};
    render_ctx->cmd_queue->ExecuteCommandLists(ARRAY_COUNT(cmd_lists), cmd_lists);

    render_ctx->swapchain->Present(1 /*sync interval*/, 0 /*present flag*/);
}
static void
SceneContext_Init (SceneContext * scene_ctx, int w, int h) {
    SIMPLE_ASSERT(scene_ctx, "scene_ctx not valid");
    memset(scene_ctx, 0, sizeof(SceneContext));

    scene_ctx->width = w;
    scene_ctx->height = h;
    scene_ctx->theta = 1.24f * XM_PI;
    scene_ctx->phi = 0.42f * XM_PI;
    scene_ctx->radius = 12.0f;
    scene_ctx->sun_theta = 1.25f * XM_PI;
    scene_ctx->sun_phi = XM_PIDIV4;
    scene_ctx->aspect_ratio = (float)scene_ctx->width / (float)scene_ctx->height;
    scene_ctx->eye_pos = {0.0f, 0.0f, 0.0f};
    scene_ctx->view = Identity4x4();
    XMMATRIX p = DirectX::XMMatrixPerspectiveFovLH(0.25f * XM_PI, scene_ctx->aspect_ratio, 1.0f, 1000.0f);
    XMStoreFloat4x4(&scene_ctx->proj, p);

    scene_ctx->skull_translation = {0.0f, 1.0f, -5.0f};
}
static void
RenderContext_Init (D3DRenderContext * render_ctx) {
    SIMPLE_ASSERT(render_ctx, "render-ctx not valid");
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
}
static void
d3d_resize (D3DRenderContext * render_ctx) {
    int w = global_scene_ctx.width;
    int h = global_scene_ctx.height;

    if (
        render_ctx &&
        render_ctx->device &&
        render_ctx->direct_cmd_list_alloc &&
        render_ctx->swapchain
        ) {
            // Flush before changing any resources.
        flush_command_queue(render_ctx);
        //wait_for_gpu(render_ctx);

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

        // Correction 11/12/2016: SSAO chapter requires an SRV to the depth buffer to read from 
        // the depth buffer.  Therefore, because we need to create two views to the same resource:
        //   1. SRV format: DXGI_FORMAT_R24_UNORM_X8_TYPELESS
        //   2. DSV Format: DXGI_FORMAT_D24_UNORM_S8_UINT
        // we need to create the depth buffer resource with a typeless format.  
        depth_stencil_desc.Format = DXGI_FORMAT_R24G8_TYPELESS;
        depth_stencil_desc.SampleDesc.Count = 1;
        depth_stencil_desc.SampleDesc.Quality = 0;
        depth_stencil_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        depth_stencil_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

        D3D12_CLEAR_VALUE opt_clear;
        opt_clear.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
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
        dsv_desc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        dsv_desc.Texture2D.MipSlice = 0;
        render_ctx->device->CreateDepthStencilView(render_ctx->depth_stencil_buffer, &dsv_desc, render_ctx->dsv_heap->GetCPUDescriptorHandleForHeapStart());

        // Transition the resource from its initial state to be used as a depth buffer.
        D3D12_RESOURCE_BARRIER ds_barrier = create_barrier(render_ctx->depth_stencil_buffer, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        render_ctx->direct_cmd_list->ResourceBarrier(1, &ds_barrier);

        // Execute the resize commands.
        render_ctx->direct_cmd_list->Close();
        ID3D12CommandList* cmds_list [] = {render_ctx->direct_cmd_list};
        render_ctx->cmd_queue->ExecuteCommandLists(_countof(cmds_list), cmds_list);

        // Wait until resize is complete.
        flush_command_queue(render_ctx);
        //wait_for_gpu(render_ctx);

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
static void
check_active_item () {
    if (ImGui::IsItemActive() || ImGui::IsItemHovered())
        global_mouse_active = false;
    else
        global_mouse_active = true;
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
        /* WM_PAINT is not handled for now ...
        case WM_PAINT: {

        } break;
        */
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
            if (wParam == SIZE_MAXIMIZED) {
                d3d_resize(_render_ctx);
            } else if (wParam == SIZE_RESTORED) {
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
        global_resizing  = true;
        Timer_Stop(&global_timer);
    } break;
    // WM_EXITSIZEMOVE is sent when the user releases the resize bars.
    // Here we reset everything based on the new window dimensions.
    case WM_EXITSIZEMOVE: {
        global_resizing  = false;
        Timer_Start(&global_timer);
        d3d_resize(_render_ctx);
    } break;
    case WM_DESTROY: {
        global_running = false;
        //PostQuitMessage(0);
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

    SceneContext_Init(&global_scene_ctx, 720, 720);
    D3DRenderContext * render_ctx = (D3DRenderContext *)::malloc(sizeof(D3DRenderContext));
    RenderContext_Init(render_ctx);

    // ========================================================================================================
#pragma region Windows_Setup
    WNDCLASS wc = {};
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
    wc.lpfnWndProc = main_win_cb;
    wc.hInstance = hInstance;
    wc.lpszClassName = _T("d3d12_win32");

    SIMPLE_ASSERT(RegisterClass(&wc), "could not register window class");

    // Compute window rectangle dimensions based on requested client area dimensions.
    RECT R = {0, 0, (long int)global_scene_ctx.width, (long int)global_scene_ctx.height};
    AdjustWindowRect(&R, WS_OVERLAPPEDWINDOW, false);
    int width  = R.right - R.left;
    int height = R.bottom - R.top;

    HWND hwnd = CreateWindowEx(
        0,                                              // Optional window styles.
        wc.lpszClassName,                               // Window class
        _T("Stencil app"),                              // Window title
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,               // Window style
        CW_USEDEFAULT, CW_USEDEFAULT, width, height,    // Size and position settings
        0 /* Parent window */, 0 /* Menu */, hInstance  /* Instance handle */,
        render_ctx                                      /* Additional application data */
    );
    SIMPLE_ASSERT(hwnd, "could not create window");
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
    IDXGIFactory * dxgi_factory = nullptr;
    CHECK_AND_FAIL(CreateDXGIFactory2(dxgiFactoryFlags, IID_PPV_ARGS(&dxgi_factory)));

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
    // store CBV_SRV_UAV descriptor increment size for later
    render_ctx->cbv_srv_uav_descriptor_size = render_ctx->device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE::D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

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
            render_ctx->psos[LAYER_OPAQUE], IID_PPV_ARGS(&render_ctx->direct_cmd_list)
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
    sampler_desc.Count = 1;
    sampler_desc.Quality = 0;

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
        dxgi_factory->CreateSwapChain(render_ctx->cmd_queue, &swapchain_desc, &render_ctx->swapchain);

// ========================================================================================================
#pragma region Load Textures
    // Brick
    strcpy_s(render_ctx->textures[TEX_BRICK].name, "brickstex");
    wcscpy_s(render_ctx->textures[TEX_BRICK].filename, L"../Textures/bricks3.dds");
    load_texture(
        render_ctx->device, render_ctx->direct_cmd_list,
        render_ctx->textures[TEX_BRICK].filename, &render_ctx->textures[TEX_BRICK]
    );
    // Checkerboard
    strcpy_s(render_ctx->textures[TEX_CHECKERBOARD].name, "checkerboardtex");
    wcscpy_s(render_ctx->textures[TEX_CHECKERBOARD].filename, L"../Textures/checkboard.dds");
    load_texture(
        render_ctx->device, render_ctx->direct_cmd_list,
        render_ctx->textures[TEX_CHECKERBOARD].filename, &render_ctx->textures[TEX_CHECKERBOARD]
    );
    // Ice
    strcpy_s(render_ctx->textures[TEX_ICE].name, "icetex");
    wcscpy_s(render_ctx->textures[TEX_ICE].filename, L"../Textures/ice.dds");
    load_texture(
        render_ctx->device, render_ctx->direct_cmd_list,
        render_ctx->textures[TEX_ICE].filename, &render_ctx->textures[TEX_ICE]
    );
    // White1x1
    strcpy_s(render_ctx->textures[TEX_WHITE1x1].name, "white1x1tex");
    wcscpy_s(render_ctx->textures[TEX_WHITE1x1].filename, L"../Textures/white1x1.dds");
    load_texture(
        render_ctx->device, render_ctx->direct_cmd_list,
        render_ctx->textures[TEX_WHITE1x1].filename, &render_ctx->textures[TEX_WHITE1x1]
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

    ds_desc.SampleDesc.Count = 1;
    ds_desc.SampleDesc.Quality = 0;
    ds_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    ds_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_HEAP_PROPERTIES ds_heap_props = {};
    ds_heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;
    ds_heap_props.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    ds_heap_props.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    ds_heap_props.CreationNodeMask = 1;
    ds_heap_props.VisibleNodeMask = 1;

    D3D12_CLEAR_VALUE opt_clear;
    opt_clear.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
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
    dsv_desc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    dsv_desc.Texture2D.MipSlice = 0;
    render_ctx->device->CreateDepthStencilView(
        render_ctx->depth_stencil_buffer,
        &dsv_desc,
        render_ctx->dsv_heap->GetCPUDescriptorHandleForHeapStart()
    );
#pragma endregion Dsv_Creation

#pragma region Rtv_Creation
    // -- create frame resources: rtv, cmd-allocator and cbuffers for each frame
    render_ctx->rtv_descriptor_size = render_ctx->device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE::D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle_start = render_ctx->rtv_heap->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < NUM_BACKBUFFERS; ++i) {
        /*CHECK_AND_FAIL(render_ctx->swapchain3->GetBuffer(i, IID_PPV_ARGS(&render_ctx->render_targets[i])));*/
        render_ctx->swapchain->GetBuffer(i, IID_PPV_ARGS(&render_ctx->render_targets[i]));
        D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle = {};
        cpu_handle.ptr = rtv_handle_start.ptr + ((UINT64)i * render_ctx->rtv_descriptor_size);
        // -- create a rtv for each frame
        render_ctx->device->CreateRenderTargetView(render_ctx->render_targets[i], nullptr, cpu_handle);
    }
#pragma endregion Rtv_Creation

#pragma region Create CBuffers
    UINT obj_cb_size = sizeof(ObjectConstants);
    UINT mat_cb_size = sizeof(MaterialConstants);
    UINT pass_cb_size = sizeof(PassConstants);
    UINT pass_count = 2;    // one default pass_cb (as usual) and one additional pass_cb for light reflection
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

        create_upload_buffer(render_ctx->device, pass_cb_size * pass_count, &render_ctx->frame_resources[i].pass_cb_data_ptr, &render_ctx->frame_resources[i].pass_cb);
        // Initialize cb data
        ::memcpy(render_ctx->frame_resources[i].pass_cb_data_ptr, &render_ctx->frame_resources[i].pass_cb_data, sizeof(render_ctx->frame_resources[i].pass_cb_data));
    }
#pragma endregion

    // ========================================================================================================
#pragma region Root_Signature_Creation
    create_root_signature(render_ctx->device, &render_ctx->root_signature);
#pragma endregion Root_Signature_Creation

    // Load and compile shaders

#pragma region Compile_Shaders
// -- using DXC shader compiler [from https://asawicki.info/news_1719_two_shader_compilers_of_direct3d_12]

    IDxcLibrary * dxc_lib = nullptr;
    HRESULT hr = DxcCreateInstance(CLSID_DxcLibrary, IID_PPV_ARGS(&dxc_lib));
    // if (FAILED(hr)) Handle error
    IDxcCompiler * dxc_compiler = nullptr;
    hr = DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&dxc_compiler));
    // if (FAILED(hr)) Handle error

    wchar_t const * shaders_path = L"./shaders/default.hlsl";
    uint32_t code_page = CP_UTF8;
    IDxcBlobEncoding * shader_blob = nullptr;
    IDxcOperationResult * dxc_res = nullptr;
    IDxcBlob * vertex_shader_code = nullptr;
    IDxcBlob * pixel_shader_code_opaque = nullptr;
    IDxcBlob * pixel_shader_code_alphatest = nullptr;
    hr = dxc_lib->CreateBlobFromFile(shaders_path, &code_page, &shader_blob);
    if (shader_blob) {
        IDxcIncludeHandler * include_handler = nullptr;
        int const n_define_fog = 1;
        DxcDefine defines_fog[n_define_fog] = {};
        defines_fog[0].Name = L"FOG";
        defines_fog[0].Value = L"1";
        int const n_define_alphatest = 2;
        DxcDefine defines_alphatest[n_define_alphatest] = {};
        defines_alphatest[0].Name = L"FOG";
        defines_alphatest[0].Value = L"1";
        defines_alphatest[1].Name = L"ALPHA_TEST";
        defines_alphatest[1].Value = L"1";

        dxc_lib->CreateIncludeHandler(&include_handler);
        hr = dxc_compiler->Compile(shader_blob, shaders_path, L"VertexShader_Main", L"vs_6_0", nullptr, 0, nullptr, 0, include_handler, &dxc_res);
        dxc_res->GetStatus(&hr);
        dxc_res->GetResult(&vertex_shader_code);
        hr = dxc_compiler->Compile(shader_blob, shaders_path, L"PixelShader_Main", L"ps_6_0", nullptr, 0, defines_fog, n_define_fog, include_handler, &dxc_res);
        dxc_res->GetStatus(&hr);
        dxc_res->GetResult(&pixel_shader_code_opaque);
        hr = dxc_compiler->Compile(shader_blob, shaders_path, L"PixelShader_Main", L"ps_6_0", nullptr, 0, defines_alphatest, n_define_alphatest, include_handler, &dxc_res);
        dxc_res->GetStatus(&hr);
        dxc_res->GetResult(&pixel_shader_code_alphatest);
        if (FAILED(hr)) {
            if (dxc_res) {
                IDxcBlobEncoding * errorsBlob = nullptr;
                hr = dxc_res->GetErrorBuffer(&errorsBlob);
                if (SUCCEEDED(hr) && errorsBlob) {
                    OutputDebugStringA((const char*)errorsBlob->GetBufferPointer());
                    return(0);
                }
            }
            // Handle compilation error...
        }
    }
    SIMPLE_ASSERT(vertex_shader_code, "invalid shader");
    SIMPLE_ASSERT(pixel_shader_code_opaque, "invalid shader");
    SIMPLE_ASSERT(pixel_shader_code_alphatest, "invalid shader");

#pragma endregion Compile_Shaders

#pragma region PSO_Creation
    create_pso(render_ctx, vertex_shader_code, pixel_shader_code_opaque, pixel_shader_code_alphatest);
#pragma endregion PSO_Creation

#pragma region Shapes_And_Renderitem_Creation
    create_skull_geometry(render_ctx);
    create_shape_geometry(render_ctx);

    create_materials(render_ctx->materials);
    create_render_items(
        &render_ctx->all_ritems,
        &render_ctx->opaque_ritems,
        &render_ctx->transparent_ritems,
        &render_ctx->alphatested_ritems,
        &render_ctx->mirrors_ritems,
        &render_ctx->reflected_ritems,
        &render_ctx->shadow_ritems,
        &render_ctx->reflected_shadow_ritems,
        &render_ctx->geom[GEOM_ROOM],
        &render_ctx->geom[GEOM_SKULL],
        render_ctx->materials
    );

#pragma endregion Shapes_And_Renderitem_Creation

    // NOTE(omid): Before closing/executing command list specify the depth-stencil-buffer transition from its initial state to be used as a depth buffer.
    D3D12_RESOURCE_BARRIER ds_barrier = create_barrier(render_ctx->depth_stencil_buffer, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    render_ctx->direct_cmd_list->ResourceBarrier(1, &ds_barrier);

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

    // Wait for the command list to execute; we are reusing the same command 
    // list in our main loop but for now, we just want to wait for setup to 
    // complete before continuing.
    flush_command_queue(render_ctx);
    //wait_for_gpu(render_ctx);

#pragma endregion

#pragma region Imgui Setup
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.Fonts->AddFontDefault();
    ImGui::StyleColorsDark();

    // calculate imgui cpu & gpu handles on location on srv_heap
    D3D12_CPU_DESCRIPTOR_HANDLE imgui_cpu_handle = render_ctx->srv_heap->GetCPUDescriptorHandleForHeapStart();
    imgui_cpu_handle.ptr += (render_ctx->cbv_srv_uav_descriptor_size * _COUNT_TEX);

    D3D12_GPU_DESCRIPTOR_HANDLE imgui_gpu_handle = render_ctx->srv_heap->GetGPUDescriptorHandleForHeapStart();
    imgui_gpu_handle.ptr += (render_ctx->cbv_srv_uav_descriptor_size * _COUNT_TEX);

    // Setup Platform/Renderer backends
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX12_Init(
        render_ctx->device, NUM_QUEUING_FRAMES,
        render_ctx->backbuffer_format, render_ctx->srv_heap,
        imgui_cpu_handle,
        imgui_gpu_handle
    );

    // Setup imgui variables
    bool * ptr_open = nullptr;
    ImGuiWindowFlags window_flags = 0;
    window_flags |= ImGuiWindowFlags_NoScrollbar;
    window_flags |= ImGuiWindowFlags_MenuBar;
    //window_flags |= ImGuiWindowFlags_NoMove;
    window_flags |= ImGuiWindowFlags_NoCollapse;
    window_flags |= ImGuiWindowFlags_NoNav;
    window_flags |= ImGuiWindowFlags_NoBringToFrontOnFocus;
    //window_flags |= ImGuiWindowFlags_NoResize;

#pragma endregion

        // ========================================================================================================
#pragma region Main_Loop
    global_running = true;
    global_resizing = false;
    global_mouse_active = true;
    bool beginwnd, coloredit;
    Timer_Init(&global_timer);
    Timer_Reset(&global_timer);
    while (global_running) {
        MSG msg = {};
        while (PeekMessageA(&msg, 0, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }

#pragma region Imgui window
        ImGui_ImplDX12_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        ImGui::Begin("Settings", ptr_open, window_flags);
        beginwnd = ImGui::IsItemActive();

        ImGui::ColorEdit3("BG Color", (float*)&render_ctx->main_pass_constants.fog_color);
        coloredit = ImGui::IsItemActive();

        ImGui::Text("\n\n");
        ImGui::Separator();
        ImGui::Text("Application average %.3f ms/frame (%.1f FPS)", 1000.0f / ImGui::GetIO().Framerate, ImGui::GetIO().Framerate);

        ImGui::End();
        ImGui::Render();
#pragma endregion

        Timer_Tick(&global_timer);
        handle_keyboard_input(
            &render_ctx->all_ritems.ritems[RITEM_SKULL],
            &render_ctx->all_ritems.ritems[RITEM_REFLECTED_SKULL],
            &render_ctx->all_ritems.ritems[RITEM_REFLECTED_SHADOW],
            &render_ctx->all_ritems.ritems[RITEM_SHADOWED_SKULL],
            &render_ctx->main_pass_constants.lights[0].direction,
            &global_scene_ctx, &global_timer
        );
        update_camera(&global_scene_ctx);

        update_obj_cbuffers(render_ctx);
        update_mat_cbuffers(render_ctx);
        update_main_pass_cbuffers(render_ctx, &global_timer);
        update_reflected_pass_cbuffers(render_ctx, &global_timer);

        draw_main(render_ctx);
        render_ctx->main_current_fence = move_to_next_frame(
            render_ctx,
            &render_ctx->frame_index, &render_ctx->backbuffer_index
        );

        // End of the loop updates
        global_mouse_active = !(beginwnd || coloredit);
    }
#pragma endregion

    // ========================================================================================================
#pragma region Cleanup_And_Debug
    flush_command_queue(render_ctx);
    //wait_for_gpu(render_ctx);

    // Cleanup Imgui
    ImGui_ImplDX12_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    CloseHandle(render_ctx->fence_event);

    render_ctx->fence->Release();

    // release queuing frame resources
    for (size_t i = 0; i < NUM_QUEUING_FRAMES; i++) {
        render_ctx->frame_resources[i].obj_cb->Unmap(0, nullptr);
        render_ctx->frame_resources[i].mat_cb->Unmap(0, nullptr);
        render_ctx->frame_resources[i].pass_cb->Unmap(0, nullptr);
        render_ctx->frame_resources[i].obj_cb->Release();
        render_ctx->frame_resources[i].mat_cb->Release();
        render_ctx->frame_resources[i].pass_cb->Release();

        render_ctx->frame_resources[i].cmd_list_alloc->Release();
    }
    for (unsigned i = 0; i < _COUNT_GEOM; i++) {
        render_ctx->geom[i].ib_uploader->Release();
        render_ctx->geom[i].vb_uploader->Release();
        render_ctx->geom[i].vb_gpu->Release();
        render_ctx->geom[i].ib_gpu->Release();
    }   // is this a bug in d3d12sdklayers.dll ?

    for (int i = 0; i < _COUNT_RENDER_LAYER; ++i) {
        render_ctx->psos[i]->Release();
    }

    pixel_shader_code_alphatest->Release();
    pixel_shader_code_opaque->Release();
    vertex_shader_code->Release();

    render_ctx->root_signature->Release();

    // release swapchain backbuffers resources
    for (unsigned i = 0; i < NUM_BACKBUFFERS; ++i) {
        render_ctx->render_targets[i]->Release();
    }

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
