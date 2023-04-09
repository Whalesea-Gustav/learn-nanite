#include <algorithm>
#include <type_traits>
#include <vk.h>
#include <window.h>
#include <mesh.h>
#include <mesh_simplify.h>
#include <vector>
#include <chrono>
#include "camera.h"
#include <cluster.h>
#include <fmt/core.h>
#include <assert.h>

using namespace std;
using namespace vk;
using namespace vk_win;

const u32 width=1920;
const u32 height=1080;

using namespace std::chrono;
class Timer{
public:
    Timer(){
        reset();
    }
    void reset(){
        start = high_resolution_clock::now();
    }
    long long us(){
    //当前时钟减去开始时钟的count
        return duration_cast<microseconds>(high_resolution_clock::now() - start).count();
    }
private:
    time_point<high_resolution_clock> start;
};

u32 as_uint(f32 x){
    return *((u32*)&x);
};

int main(){
    Timer timer;

    timer.reset();
    fmt::print("loading mesh: ");
    Mesh mesh;
    mesh.load("asset/Font_Reconstructed.stl");
    fmt::print("{} us\n",timer.us());
    auto& [pos,idx]=mesh;

    timer.reset();
    fmt::print("simplifying: ");
    //单纯去除重点
    MeshSimplifier simplifier(pos.data(),pos.size(),idx.data(),idx.size());
    simplifier.simplify(idx.size());
    pos.resize(simplifier.remaining_num_vert());
    idx.resize(simplifier.remaining_num_tri()*3);
    fmt::print("{} us\n",timer.us());
    fmt::print("verts: {}, tris: {}\n\n",pos.size(),idx.size()/3);

    vector<Cluster> clusters;
    vector<ClusterGroup> cluster_groups;

    timer.reset();
    fmt::print("clustering triangles: ");
    cluster_triangles(pos,idx,clusters);
    fmt::print("{} us\n\n",timer.us());
    u32 level_offset=0,mip_level=0;

    fmt::print("begin build virtual mesh\n\n");
    while(1){
        fmt::print("### level {} ###\n",mip_level);
        fmt::print("num clusters: {}\n",clusters.size()-level_offset);

        f32 maxsz=0,minsz=100000,avgsz=0;
        for(u32 i=level_offset;i<clusters.size();i++){
            auto& cluster=clusters[i];
            assert(cluster.verts.size()<256);
            f32 sz=cluster.indexes.size()/3.0;
            if(sz>maxsz) maxsz=sz;
            if(sz<minsz) minsz=sz;
            avgsz+=sz;
        }
        avgsz/=clusters.size()-level_offset;
        fmt::print("cluster size: max={}, min={}, avg={}\n",maxsz,minsz,avgsz);

        u32 num_level_clusters=clusters.size()-level_offset;
        if(num_level_clusters<=1) break;

        u32 prev_cluster_num=clusters.size();
        u32 prev_group_num=cluster_groups.size();

        timer.reset();
        fmt::print("groupinging clusters: ");
        group_clusters(
            clusters,
            level_offset,
            num_level_clusters,
            cluster_groups,
            mip_level
        );
        fmt::print("{} us\n",timer.us());
        fmt::print("num groups: {}\n",cluster_groups.size()-prev_group_num);

        timer.reset();
        fmt::print("building parent clusters: ");
        for(u32 i=prev_group_num;i<cluster_groups.size();i++){
            build_parent_clusters(cluster_groups[i],clusters);
        }
        fmt::print("{} us\n",timer.us());

        level_offset=prev_cluster_num;
        mip_level++;

        fmt::print("\n");
    }
    fmt::print("end build virtual mesh\n");
    fmt::print("total clusters: {}\n\n",clusters.size());


    // f32 maxsz=0,minsz=100000,avgsz=0;
    // for(auto& cluster:clusters){
    //     assert(cluster.verts.size()<256);
    //     f32 sz=cluster.indexes.size()/3.0;
    //     if(sz>maxsz) maxsz=sz;
    //     if(sz<minsz) minsz=sz;
    //     avgsz+=sz;
    // }
    // avgsz/=clusters.size();
    // fmt::print("cluster size: max={}, min={}, avg={}\n\n",maxsz,minsz,avgsz);

    // timer.reset();
    // group_clusters(clusters,0,clusters.size(),cluster_groups,0);
    // u32 prev_group_num=cluster_groups.size();
    // fmt::print("level 0 num groups: {}\n",cluster_groups.size());
    // fmt::print("group clusters in level 0: {} us\n\n",timer.us());

    // timer.reset();
    // for(auto& group:cluster_groups){
    //     build_parent_clusters(group,clusters);
    // }
    // fmt::print("build parent clusters level0: {} us\n",timer.us());
    // fmt::print("level 1 num clusters: {}\n",clusters.size()-level_offset);

    // maxsz=0,minsz=100000,avgsz=0;
    // for(u32 i=level_offset;i<clusters.size();i++){
    //     auto& cluster=clusters[i];
    //     assert(cluster.verts.size()<256);
    //     f32 sz=cluster.indexes.size()/3.0;
    //     if(sz>maxsz) maxsz=sz;
    //     if(sz<minsz) minsz=sz;
    //     avgsz+=sz;
    // }
    // avgsz/=clusters.size()-level_offset;
    // fmt::print("cluster size: max={}, min={}, avg={}\n\n",maxsz,minsz,avgsz);

    // timer.reset();
    // group_clusters(clusters,level_offset,clusters.size()-level_offset,cluster_groups,1);
    // fmt::print("level 1 num groups: {}\n",cluster_groups.size()-prev_group_num);
    // fmt::print("group clusters in level 1: {} us\n\n",timer.us());


    vk::init();
    Window window=Window::create(width,height,"virtual mesh viewer");
    window.build_vk_surface(vk::instance());
    vk::init_surface(window.surface,window.width,window.height);

    timer.reset();
    vector<Buffer> packed_clusters;

    for(auto& cluster:clusters){
        vector<u32> packed_data;

        packed_data.push_back(cluster.indexes.size()/3); // num_tri
        packed_data.push_back(cluster.verts.size()); // num_vert
        packed_data.push_back(cluster.group_id);
        packed_data.push_back(cluster.mip_level);

        for(u32 i=0;i<cluster.indexes.size()/3;i++){ //tri data
            u32 i0=cluster.indexes[i*3];
            u32 i1=cluster.indexes[i*3+1];
            u32 i2=cluster.indexes[i*3+2];
            assert(i0<256&&i1<256&&i2<256);

            u32 packed_tri=(i0|(i1<<8)|(i2<<16));
            packed_data.push_back(packed_tri);
        }
        for(vec3 p:cluster.verts){
            packed_data.push_back(as_uint(p.x));
            packed_data.push_back(as_uint(p.y));
            packed_data.push_back(as_uint(p.z));
        }
        vector<u32> t(cluster.indexes.size());
        for(u32 i:cluster.external_edges) t[i]=1;
        for(u32 x:t) packed_data.push_back(x);

        auto cluster_buffer=Buffer::from_iter(
            BufferAllocateInfo{
                .buffer_usage=vk::BufferUsage::StorageBuffer,
                .memory_usage=vk::MemoryUsage::Upload
            },
            packed_data
        )
        .unwrap();
        packed_clusters.push_back(cluster_buffer);
    }

    fmt::print("build packed buffer: {} us\n",timer.us());

    vk::write_bindless_set(
        vk::num_swapchain_image(), //offset
        packed_clusters.data(),
        packed_clusters.size(),
        DescriptorType::StorageBuffer
    );

    struct FrameContext{
        mat4 vp_mat;
        u32 view_mode;// 0:tri 1:cluster 2:group
        u32 level;
    };
    vector<Buffer> frame_context_buffers(vk::num_swapchain_image());
    vector<pair<u32,u32>> id(vk::num_swapchain_image());
    for(u32 i=0;i<vk::num_swapchain_image();i++){
        id[i]={i,vk::num_swapchain_image()};
    }

    for(auto& buffer:frame_context_buffers){
        buffer=Buffer::from_raw(
            BufferAllocateInfo{
                .buffer_usage=vk::BufferUsage::StorageBuffer,
                .memory_usage=vk::MemoryUsage::Upload
            },
            nullptr,
            sizeof(FrameContext),
            0
        ).unwrap();
    }

    vk::write_bindless_set(
        0, //offset
        frame_context_buffers.data(),
        frame_context_buffers.size(),
        DescriptorType::StorageBuffer
    );

    auto depth_buffer=Image::AttachmentImage(
        width,
        height,
        Format::D32_SFLOAT,
        ImageUsage::DepthStencilAttachment
    ).unwrap();

    auto pipeline=GraphicsPipeline::new_()
        .render_pass(RenderingCreateInfo{
            .color_attachment_formats={vk::swapchain_image_format()},
            .depth_attachment_format=Format::D32_SFLOAT
        })
        .push_constant_size(8)
        .input_assembly_state(PrimitiveTopology::TriangleList)
        .vertex_shader(ShaderModule::from_file("shader/viewer_vert.spv").unwrap())
        .viewport_state(ViewportState::Default({Viewport::dimension(width,height)}))
        .depth_stencil_state(DepthStencilState::reverse_z_test())
        .fragment_shader(ShaderModule::from_file("shader/viewer_frag.spv").unwrap())
        .build().unwrap();

    auto cmd_allocator=CommandBufferAllocator::new_().unwrap();
    vector<CommandBuffer> cmds(vk::num_swapchain_image());

    u32 swapchain_idx=0;

    for(auto& cmd:cmds){
        cmd=CommandBuffer::new_(cmd_allocator).unwrap()
            .begin(CommandBufferUsage::SimultaneousUse).unwrap()
            .pipeline_barrier(Dependency{
                .image_barriers={ImageBarrier{
                    .image=vk::swapchain_image(swapchain_idx),
                    .dst_stage=PipelineStage::COLOR_ATTACHMENT_OUTPUT,
                    .dst_access=AccessFlag::COLOR_ATTACHMENT_WRITE,
                    .new_layout=ImageLayout::AttachmentOptimal,
                }}
            })
            .begin_rendering(RenderingInfo{
                .render_area_extent={.x=window.width,.y=window.height},
                .color_attachments={RenderingAttachmentInfo{
                    .image_view=swapchain_image(swapchain_idx).image_view,
                    .load_op=LoadOp::Clear,
                }},
                .depth_attachment=Some(RenderingAttachmentInfo{
                    .image_view=depth_buffer.image_view,
                    .load_op=LoadOp::Clear,
                })
            })
            .push_constant(pipeline.pipeline_layout,8,&id[swapchain_idx])
            .bind_graphics_pipeline(pipeline.handle)
            .bind_descriptor_sets(PipelineBindPoint::Graphics,pipeline.pipeline_layout,vk::bindless_set())
            .draw(128*3,clusters.size(),0,0)
            .end_rendering()
            .pipeline_barrier(Dependency{
                .image_barriers={ImageBarrier{
                    .image=vk::swapchain_image(swapchain_idx),
                    .src_stage=PipelineStage::COLOR_ATTACHMENT_OUTPUT,
                    .src_access=AccessFlag::COLOR_ATTACHMENT_WRITE,
                    .old_layout=ImageLayout::AttachmentOptimal,
                    .new_layout=ImageLayout::PresentSrc
                }}
            })
            .build().unwrap();
        
        swapchain_idx++;
    }

    struct FrameSync{
        Semaphore acq_img=vk::create_semaphore().unwrap();
        Semaphore cmd_exec=vk::create_semaphore().unwrap();
        Fence cpu_wait_cmd=vk::create_fence(true).unwrap();
    }sync[3];
    u32 frame_idx=0,frame_cnt=0;

    Camera camera;
    camera.position=vec3{0,0,10};
    camera.yaw=-90;
    camera.pitch=0;

    window.set_cursor_disabled();
    bool is_cursor_disabled=true;
    bool is_key_downs[100];
    dvec2 lst_cursor_pos;
    u32 view_mode=0;
    u32 level=0;

    timer.reset();

    while(!window.should_close()){
        window.poll_events();

        f32 tick_time=timer.us()/1000.f;
        timer.reset();

        if(window.is_key_down('W')) camera.move_front(tick_time);
        if(window.is_key_down('S')) camera.move_front(-tick_time);
        if(window.is_key_down('A')) camera.move_right(-tick_time);
        if(window.is_key_down('D')) camera.move_right(tick_time);

        if(window.is_key_begin_press('J')) view_mode=(view_mode+3-1)%3;
        if(window.is_key_begin_press('K')) view_mode=(view_mode+1)%3;

        if(window.is_key_begin_press('U')) level=(level+mip_level-1)%mip_level;
        if(window.is_key_begin_press('I')) level=(level+1)%mip_level;

        if(window.is_key_begin_press('B')){
            window.set_cursor_normal();
            lst_cursor_pos=window.get_cursor_pos();
            is_cursor_disabled=false;
        }
        if(window.is_key_begin_release('B')){
            window.set_cursor_disabled();
            is_cursor_disabled=true;
        }

        // bool is_key_down=window.is_key_down('J');
        // if(!is_key_downs['J']&&is_key_down){
        //     view_mode=(view_mode+3-1)%3;
        //     is_key_downs['J']=true;
        // }
        // if(is_key_downs['J']&&!is_key_down){
        //     is_key_downs['J']=false;
        // }

        // is_key_down=window.is_key_down('K');
        // if(!is_key_downs['K']&&is_key_down){
        //     view_mode=(view_mode+1)%3;
        //     is_key_downs['K']=true;
        // }
        // if(is_key_downs['K']&&!is_key_down){
        //     is_key_downs['K']=false;
        // }

        // is_key_down=window.is_key_down('B');
        // if(is_cursor_disabled&&is_key_down){
        //     window.set_cursor_normal();
        //     lst_cursor_pos=window.get_cursor_pos();
        //     is_cursor_disabled=false;
        // }
        // if(!is_cursor_disabled&&!is_key_down){
        //     window.set_cursor_disabled();
        //     is_cursor_disabled=true;
        // }

        dvec2 cursor_pos=window.get_cursor_pos();
        if(frame_cnt==0) lst_cursor_pos=cursor_pos;
        if(is_cursor_disabled)
            camera.rotate_view(cursor_pos.x-lst_cursor_pos.x,cursor_pos.y-lst_cursor_pos.y);
        lst_cursor_pos=cursor_pos;

        mat4 v_mat=camera.view_mat();
        mat4 p_mat=camera.projection_mat(Camera::radians(40),(f32)window.width/window.height);
        mat4 vp_mat=mul(p_mat,v_mat);

        vk::acquire_next_image(Some(sync[frame_idx].acq_img),None(),swapchain_idx);
        vk::wait_for_fence(sync[frame_idx].cpu_wait_cmd);
        vk::reset_fence(sync[frame_idx].cpu_wait_cmd);

        FrameContext frame_context{vp_mat,view_mode,level};
        frame_context_buffers[swapchain_idx].update(&frame_context,sizeof(FrameContext));

        vk::queue_submit(
            SubmitInfo{
                .waiting={sync[frame_idx].acq_img},
                .command_buffers={cmds[swapchain_idx]},
                .signal={sync[frame_idx].cmd_exec}
            },
            sync[frame_idx].cpu_wait_cmd
        );

        vk::queue_present(PresentInfo{
            .waiting={sync[frame_idx].cmd_exec},
            .swapchain_image_idx=swapchain_idx
        });

        frame_idx=(frame_idx+1)%3;
        frame_cnt++;
    }

    vk::cleanup();

    return 0;
}