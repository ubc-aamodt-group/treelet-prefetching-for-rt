// Copyright (c) 2022, Mohammadreza Saed, Yuan Hsi Chou, Lufei Liu, Tor M. Aamodt,
// The University of British Columbia
// All rights reserved.

// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:

// Redistributions of source code must retain the above copyright notice, this
// list of conditions and the following disclaimer.
// Redistributions in binary form must reproduce the above copyright notice,
// this list of conditions and the following disclaimer in the documentation
// and/or other materials provided with the distribution. Neither the name of
// The University of British Columbia nor the names of its contributors may be
// used to endorse or promote products derived from this software without
// specific prior written permission.

// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.

#include "vulkan_ray_tracing.h"
#include "vulkan_rt_thread_data.h"

#include <iostream>
#include <vector>
#include <string>
#include <fstream>
#include <cmath>
#define BOOST_FILESYSTEM_VERSION 3
#define BOOST_FILESYSTEM_NO_DEPRECATED 
#include <boost/filesystem.hpp>

namespace fs = boost::filesystem;

#define __CUDA_RUNTIME_API_H__
// clang-format off
#include "host_defines.h"
#include "builtin_types.h"
#include "driver_types.h"
#include "../../libcuda/cuda_api.h"
#include "cudaProfiler.h"
// clang-format on
#if (CUDART_VERSION < 8000)
#include "__cudaFatFormat.h"
#endif

#include "../../libcuda/gpgpu_context.h"
#include "../../libcuda/cuda_api_object.h"
#include "../gpgpu-sim/gpu-sim.h"
#include "../cuda-sim/ptx_loader.h"
#include "../cuda-sim/cuda-sim.h"
#include "../cuda-sim/ptx_ir.h"
#include "../cuda-sim/ptx_parser.h"
#include "../gpgpusim_entrypoint.h"
#include "../stream_manager.h"
#include "../abstract_hardware_model.h"
#include "vulkan_acceleration_structure_util.h"
#include "../gpgpu-sim/vector-math.h"

#if defined(MESA_USE_LVPIPE_DRIVER)
#include "lvp_private.h"
#endif 
//#include "intel_image_util.h"
#include "astc_decomp.h"

// #define HAVE_PTHREAD
// #define UTIL_ARCH_LITTLE_ENDIAN 1
// #define UTIL_ARCH_BIG_ENDIAN 0
// #define signbit signbit

// #define UINT_MAX 65535
// #define GLuint MESA_GLuint
// // #include "isl/isl.h"
// // #include "isl/isl_tiled_memcpy.c"
// #include "vulkan/anv_private.h"
// #undef GLuint

// #undef HAVE_PTHREAD
// #undef UTIL_ARCH_LITTLE_ENDIAN
// #undef UTIL_ARCH_BIG_ENDIAN
// #undef signbit

// #include "vulkan/anv_public.h"

#if defined(MESA_USE_INTEL_DRIVER)
#include "intel_image.h"
#elif defined(MESA_USE_LVPIPE_DRIVER)
// #include "lvp_image.h"
#endif

// #include "anv_include.h"

VkRayTracingPipelineCreateInfoKHR* VulkanRayTracing::pCreateInfos = NULL;
VkAccelerationStructureGeometryKHR* VulkanRayTracing::pGeometries = NULL;
uint32_t VulkanRayTracing::geometryCount = 0;
VkAccelerationStructureKHR VulkanRayTracing::topLevelAS = NULL;
std::vector<std::vector<Descriptor> > VulkanRayTracing::descriptors;
std::ofstream VulkanRayTracing::imageFile;
std::map<std::string, std::string> outputImages;
bool VulkanRayTracing::firstTime = true;
std::vector<shader_stage_info> VulkanRayTracing::shaders;
// RayDebugGPUData VulkanRayTracing::rayDebugGPUData[2000][2000] = {0};
struct DESCRIPTOR_SET_STRUCT* VulkanRayTracing::descriptorSet = NULL;
void* VulkanRayTracing::launcher_descriptorSets[MAX_DESCRIPTOR_SETS][MAX_DESCRIPTOR_SET_BINDINGS] = {NULL};
void* VulkanRayTracing::launcher_deviceDescriptorSets[MAX_DESCRIPTOR_SETS][MAX_DESCRIPTOR_SET_BINDINGS] = {NULL};
std::vector<void*> VulkanRayTracing::child_addrs_from_driver;
bool VulkanRayTracing::dumped = false;

bool use_external_launcher = false;
const bool dump_trace = false;

// Treelets
std::map<StackEntry, std::vector<StackEntry>> VulkanRayTracing::treelet_roots;
std::map<uint8_t*, std::vector<StackEntry>> VulkanRayTracing::treelet_roots_addr_only;
std::map<StackEntry, std::vector<StackEntry>> VulkanRayTracing::treelet_child_map;
std::map<uint8_t*, std::vector<StackEntry>> VulkanRayTracing::treelet_addr_only_child_map;
std::map<uint8_t*, uint8_t*> VulkanRayTracing::node_map_addr_only;

std::map<uint8_t*, StackEntry> VulkanRayTracing::parent_map; // map <node, it's parent>
std::map<uint8_t*, std::vector<StackEntry>> VulkanRayTracing::children_map; // map <node, it's children>
std::map<uint8_t*, StackEntry> VulkanRayTracing::node_info; // map <node, it's info>
std::map<uint8_t*, StackEntry> VulkanRayTracing::parent_map_device_offset; // map <node device address, it's parent's device address + info>
std::map<uint8_t*, StackEntry> VulkanRayTracing::node_info_device_offset; // map <node device address, it's info>
std::deque<std::pair<StackEntry, int>> VulkanRayTracing::reverse_stack; // BVH nodes in reverse order for bottom up treelet formation


bool VulkanRayTracing::_init_ = false;
warp_intersection_table *** VulkanRayTracing::intersection_table;
IntersectionTableType VulkanRayTracing::intersectionTableType = IntersectionTableType::Baseline;

float get_norm(float4 v)
{
    return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z + v.w * v.w);
}
float get_norm(float3 v)
{
    return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
}

float4 normalized(float4 v)
{
    float norm = get_norm(v);
    return {v.x / norm, v.y / norm, v.z / norm, v.w / norm};
}
float3 normalized(float3 v)
{
    float norm = get_norm(v);
    return {v.x / norm, v.y / norm, v.z / norm};
}

Ray make_transformed_ray(Ray &ray, float4x4 matrix, float *worldToObject_tMultiplier)
{
    Ray transformedRay;
    float4 transformedOrigin4 = matrix * float4({ray.get_origin().x, ray.get_origin().y, ray.get_origin().z, 1});
    float4 transformedDirection4 = matrix * float4({ray.get_direction().x, ray.get_direction().y, ray.get_direction().z, 0});

    float3 transformedOrigin = {transformedOrigin4.x / transformedOrigin4.w, transformedOrigin4.y / transformedOrigin4.w, transformedOrigin4.z / transformedOrigin4.w};
    float3 transformedDirection = {transformedDirection4.x, transformedDirection4.y, transformedDirection4.z};
    *worldToObject_tMultiplier = get_norm(transformedDirection);
    transformedDirection = normalized(transformedDirection);

    transformedRay.make_ray(transformedOrigin, transformedDirection, ray.get_tmin() * (*worldToObject_tMultiplier), ray.get_tmax() * (*worldToObject_tMultiplier));
    return transformedRay;
}

float magic_max7(float a0, float a1, float b0, float b1, float c0, float c1, float d)
{
	float t1 = MIN_MAX(a0, a1, d);
	float t2 = MIN_MAX(b0, b1, t1);
	float t3 = MIN_MAX(c0, c1, t2);
	return t3;
}

float magic_min7(float a0, float a1, float b0, float b1, float c0, float c1, float d)
{
	float t1 = MAX_MIN(a0, a1, d);
	float t2 = MAX_MIN(b0, b1, t1);
	float t3 = MAX_MIN(c0, c1, t2);
	return t3;
}

float3 get_t_bound(float3 box, float3 origin, float3 idirection)
{
    // // Avoid div by zero, returns 1/2^80, an extremely small number
    // const float ooeps = exp2f(-80.0f);

    // // Calculate inverse direction
    // float3 idir;
    // idir.x = 1.0f / (fabsf(direction.x) > ooeps ? direction.x : copysignf(ooeps, direction.x));
    // idir.y = 1.0f / (fabsf(direction.y) > ooeps ? direction.y : copysignf(ooeps, direction.y));
    // idir.z = 1.0f / (fabsf(direction.z) > ooeps ? direction.z : copysignf(ooeps, direction.z));

    // Calculate bounds
    float3 result;
    result.x = (box.x - origin.x) * idirection.x;
    result.y = (box.y - origin.y) * idirection.y;
    result.z = (box.z - origin.z) * idirection.z;

    // Return
    return result;
}

float3 calculate_idir(float3 direction) {
    // Avoid div by zero, returns 1/2^80, an extremely small number
    const float ooeps = exp2f(-80.0f);

    // Calculate inverse direction
    float3 idir;
    // TODO: is this wrong?
    idir.x = 1.0f / (fabsf(direction.x) > ooeps ? direction.x : copysignf(ooeps, direction.x));
    idir.y = 1.0f / (fabsf(direction.y) > ooeps ? direction.y : copysignf(ooeps, direction.y));
    idir.z = 1.0f / (fabsf(direction.z) > ooeps ? direction.z : copysignf(ooeps, direction.z));

    // idir.x = fabsf(direction.x) > ooeps ? 1.0f / direction.x : copysignf(ooeps, direction.x);
    // idir.y = fabsf(direction.y) > ooeps ? 1.0f / direction.y : copysignf(ooeps, direction.y);
    // idir.z = fabsf(direction.z) > ooeps ? 1.0f / direction.z : copysignf(ooeps, direction.z);
    return idir;
}

bool ray_box_test(float3 low, float3 high, float3 idirection, float3 origin, float tmin, float tmax, float& thit)
{
	// const float3 lo = Low * InvDir - Ood;
	// const float3 hi = High * InvDir - Ood;
    float3 lo = get_t_bound(low, origin, idirection);
    float3 hi = get_t_bound(high, origin, idirection);

    // QUESTION: max value does not match rtao benchmark, rtao benchmark converts float to int with __float_as_int
    // i.e. __float_as_int: -110.704826 => -1025677090, -24.690834 => -1044019502

	// const float slabMin = tMinFermi(lo.x, hi.x, lo.y, hi.y, lo.z, hi.z, TMin);
	// const float slabMax = tMaxFermi(lo.x, hi.x, lo.y, hi.y, lo.z, hi.z, TMax);
    float min = magic_max7(lo.x, hi.x, lo.y, hi.y, lo.z, hi.z, tmin);
    float max = magic_min7(lo.x, hi.x, lo.y, hi.y, lo.z, hi.z, tmax);

	// OutIntersectionDist = slabMin;
    thit = min;

	// return slabMin <= slabMax;
    return (min <= max);
}



std::ofstream print_tree;
void traverse_tree(volatile uint8_t* address, bool isTopLevel = true, bool isLeaf = false, bool isRoot = true)
{
    if(isRoot)
    {
        GEN_RT_BVH topBVH;
        GEN_RT_BVH_unpack(&topBVH, (uint8_t*)address);

        uint8_t* topRootAddr = (uint8_t*)address + topBVH.RootNodeOffset;

        if (print_tree.is_open())
        {
            print_tree << "traversing bvh , isTopLevel = " << isTopLevel << (void *)(address) << ", RootNodeOffset = (" << topBVH.RootNodeOffset << std::endl;
        }

        traverse_tree(topRootAddr, isTopLevel, false, false);
    }
    
    else if(!isLeaf) // internal nodes
    {
        struct GEN_RT_BVH_INTERNAL_NODE node;
        GEN_RT_BVH_INTERNAL_NODE_unpack(&node, address);

        if (print_tree.is_open())
        {
            uint8_t *child_addrs[6];
            child_addrs[0] = address + (node.ChildOffset * 64);
            for(int i = 0; i < 5; i++)
                child_addrs[i + 1] = child_addrs[i] + node.ChildSize[i] * 64;
            
            print_tree << "traversing internal node " << (void *)address;
            print_tree << ", isTopLevel = " << isTopLevel << ", child offset = " << node.ChildOffset << ", node type = " << node.NodeType;
            print_tree << ", child size = (" << node.ChildSize[0] << ", " << node.ChildSize[1] << ", " << node.ChildSize[2] << ", " << node.ChildSize[3] << ", " << node.ChildSize[4] << ", " << node.ChildSize[5] << ")";
            print_tree << ", child type = (" << node.ChildType[0] << ", " << node.ChildType[1] << ", " << node.ChildType[2] << ", " << node.ChildType[3] << ", " << node.ChildType[4] << ", " << node.ChildType[5] << ")";
            print_tree << ", child addresses = (" << (void*)(child_addrs[0]) << ", " << (void*)(child_addrs[1]) << ", " << (void*)(child_addrs[2]) << ", " << (void*)(child_addrs[3]) << ", " << (void*)(child_addrs[4]) << ", " << (void*)(child_addrs[5]) << ")";
            print_tree << std::endl;
        }

        uint8_t *child_addr = address + (node.ChildOffset * 64);
        for(int i = 0; i < 6; i++)
        {
            if(node.ChildSize[i] > 0)
            {
                if(node.ChildType[i] != NODE_TYPE_INTERNAL)
                    isLeaf = true;
                else
                    isLeaf = false;

                traverse_tree(child_addr, isTopLevel, isLeaf, false);
            }

            child_addr += node.ChildSize[i] * 64;
        }
    }

    else // leaf nodes
    {
        if(isTopLevel)
        {
            GEN_RT_BVH_INSTANCE_LEAF instanceLeaf;
            GEN_RT_BVH_INSTANCE_LEAF_unpack(&instanceLeaf, address);

            float4x4 worldToObjectMatrix = instance_leaf_matrix_to_float4x4(&instanceLeaf.WorldToObjectm00);
            float4x4 objectToWorldMatrix = instance_leaf_matrix_to_float4x4(&instanceLeaf.ObjectToWorldm00);

            assert(instanceLeaf.BVHAddress != NULL);

            if (print_tree.is_open())
            {
                print_tree << "traversing top level leaf node " << (void *)address << ", instanceID = " << instanceLeaf.InstanceID << ", BVHAddress = " << instanceLeaf.BVHAddress << ", ShaderIndex = " << instanceLeaf.ShaderIndex << std::endl;
            }

            traverse_tree(address + instanceLeaf.BVHAddress, false, false, true);
        }
        else
        {
            struct GEN_RT_BVH_PRIMITIVE_LEAF_DESCRIPTOR leaf_descriptor;
            GEN_RT_BVH_PRIMITIVE_LEAF_DESCRIPTOR_unpack(&leaf_descriptor, address);
            
            if (leaf_descriptor.LeafType == TYPE_QUAD)
            {
                struct GEN_RT_BVH_QUAD_LEAF leaf;
                GEN_RT_BVH_QUAD_LEAF_unpack(&leaf, address);

                float3 p[3];
                for(int i = 0; i < 3; i++)
                {
                    p[i].x = leaf.QuadVertex[i].X;
                    p[i].y = leaf.QuadVertex[i].Y;
                    p[i].z = leaf.QuadVertex[i].Z;
                }

                assert(leaf.PrimitiveIndex1Delta == 0);

                if (print_tree.is_open())
                {
                    print_tree << "quad node " << (void*)address << " ";
                    print_tree << "primitiveID = " << leaf.PrimitiveIndex0 << "\n";

                    print_tree << "p[0] = (" << p[0].x << ", " << p[0].y << ", " << p[0].z << ") ";
                    print_tree << "p[1] = (" << p[1].x << ", " << p[1].y << ", " << p[1].z << ") ";
                    print_tree << "p[2] = (" << p[2].x << ", " << p[2].y << ", " << p[2].z << ") ";
                    print_tree << "p[3] = (" << p[3].x << ", " << p[3].y << ", " << p[3].z << ")" << std::endl;
                }
            }
            else
            {
                struct GEN_RT_BVH_PROCEDURAL_LEAF leaf;
                GEN_RT_BVH_PROCEDURAL_LEAF_unpack(&leaf, address);

                if (print_tree.is_open())
                {
                    print_tree << "PROCEDURAL node " << (void*)address << " ";
                    print_tree << "NumPrimitives = " << leaf.NumPrimitives << ", LastPrimitive = " << leaf.LastPrimitive << ", PrimitiveIndex[0]" << leaf.PrimitiveIndex[0] << "\n";
                }
            }
        }
    }
}

void VulkanRayTracing::init(uint32_t launch_width, uint32_t launch_height)
{
    if(_init_)
        return;
    _init_ = true;

    gpgpu_context *ctx;
    ctx = GPGPU_Context();
    CUctx_st *context = GPGPUSim_Context(ctx);

    uint32_t width;
    uint32_t height;
    warp_pixel_mapping mapping = WARP_8X4;
    switch (mapping) {
        case WARP_32X1:
            width = (launch_width + 31) / 32;
            height = launch_height;
            break;
        case WARP_16X2:
            width = (launch_width + 15) / 16;
            height = (launch_height + 1) / 2;
            break;
        case WARP_8X4:
            width = (launch_width + 7) / 8;
            height = (launch_height + 3) / 4;
            break;
        default:
            abort();
    }
    // uint32_t width = (launch_width + 31) / 32;
    // uint32_t height = launch_height;

    if(ctx->the_gpgpusim->g_the_gpu->getShaderCoreConfig()->m_rt_intersection_table_type == 0)
        intersectionTableType = IntersectionTableType::Baseline;
    else if(ctx->the_gpgpusim->g_the_gpu->getShaderCoreConfig()->m_rt_intersection_table_type == 1)
        intersectionTableType = IntersectionTableType::Function_Call_Coalescing;
    else
        assert(0);

    if(intersectionTableType == IntersectionTableType::Baseline)
    {
        intersection_table = new Baseline_warp_intersection_table**[width];
        for(int i = 0; i < width; i++)
        {
            intersection_table[i] = new Baseline_warp_intersection_table*[height];
            for(int j = 0; j < height; j++)
                intersection_table[i][j] = new Baseline_warp_intersection_table();
        }
    }
    else
    {
        intersection_table = new Coalescing_warp_intersection_table**[width];
        for(int i = 0; i < width; i++)
        {
            intersection_table[i] = new Coalescing_warp_intersection_table*[height];
            for(int j = 0; j < height; j++)
                intersection_table[i][j] = new Coalescing_warp_intersection_table();
        }

    }
}


bool VulkanRayTracing::isTreeletRoot(StackEntry node)
{
    return treelet_roots.count(node);
}


bool VulkanRayTracing::isTreeletRoot(uint8_t* addr)
{
    return treelet_roots_addr_only.count(addr);
}


uint8_t* VulkanRayTracing::addrToTreeletID(uint8_t* addr) // returns the treelet ID/address that a node belongs to
{
    assert(node_map_addr_only.count(addr));
    return node_map_addr_only[addr];
    // if (treelet_roots_addr_only.count(addr)) // if node is a key then its a treelet root, so return itself
    // {
    //     return addr;
    // }
    // else // node is not a treelet root
    // {
    //     for (auto const& root : treelet_roots_addr_only)
    //     {
    //         for (auto node : root.second)
    //         {
    //             if (addr == node.addr)
    //                 return root.first;
    //         }
    //     }

    //     // Node not found in treelet map
    //     std::cout << "Node " << (void*)addr << " not found in treelet map" << std::endl;
    //     return NULL;
    // }

}


void VulkanRayTracing::buildNodeToRootMap()
{
    std::map<uint8_t*, uint8_t*> node_map; // map <node, it's treelet root>

    for (auto root : treelet_roots_addr_only)
    {
        node_map[root.first] = root.first; // add the root to the node map;
        for (auto node : root.second)
        {
            node_map[node.addr] = root.first; // add the nodes to the node map
        }
    }

    node_map_addr_only = node_map;
}


std::vector<StackEntry> VulkanRayTracing::treeletIDToChildren(StackEntry treelet_root) // returns child treelet IDs of a given treelet
{
    return treelet_child_map[treelet_root];
}


std::vector<StackEntry> VulkanRayTracing::treeletIDToChildren(uint8_t* treelet_root) // returns child treelet IDs of a given treelet
{
    return treelet_addr_only_child_map[treelet_root];
}


float VulkanRayTracing::calculateSAH(float3 lo, float3 hi)
{
    float x = hi.x - lo.x;
    float y = hi.y - lo.y;
    float z = hi.z - lo.z;
    return 2 * ( x*y + y*z + x*z );
}


// double VulkanRayTracing::calculateScore()
// {

// }


void VulkanRayTracing::parentPointerPass(VkAccelerationStructureKHR _topLevelAS, int64_t device_offset)
{
    std::map<uint8_t*, StackEntry> parent_map; // map <node, it's parent>
    std::map<uint8_t*, StackEntry> node_info; // map <node, it's info>
    std::map<uint8_t*, StackEntry> parent_map_device_offset; // map <node device address, it's parent's device address + info>
    std::map<uint8_t*, StackEntry> node_info_device_offset; // map <node device address, it's info>

    std::deque<StackEntry> stack;
    std::deque<std::pair<StackEntry, int>> reverse_stack; // Stack entry, tree level: stores nodes in reverse order

    int tree_level = 0;

    GEN_RT_BVH topBVH; //TODO: test hit with world before traversal
    GEN_RT_BVH_unpack(&topBVH, (uint8_t*)_topLevelAS);
    parent_map[(uint8_t*)_topLevelAS] = StackEntry(nullptr, false, false, 0); // root node has no parent
    node_info[(uint8_t*)_topLevelAS] = StackEntry((uint8_t*)_topLevelAS, true, false, GEN_RT_BVH_length * 4);
    parent_map_device_offset[(uint8_t*)_topLevelAS + device_offset] = StackEntry(nullptr, false, false, 0); // root node has no parent
    node_info_device_offset[(uint8_t*)_topLevelAS + device_offset] = StackEntry((uint8_t*)_topLevelAS + device_offset, true, false, GEN_RT_BVH_length * 4);
    reverse_stack.push_front(std::make_pair(parent_map[(uint8_t*)_topLevelAS], tree_level));

    tree_level++;

    uint8_t* topRootAddr = (uint8_t*)_topLevelAS + topBVH.RootNodeOffset;
    stack.push_back(StackEntry(topRootAddr, true, false, GEN_RT_BVH_INTERNAL_NODE_length * 4));
    parent_map[topRootAddr] = StackEntry((uint8_t*)_topLevelAS, true, false, GEN_RT_BVH_length * 4);
    node_info[topRootAddr] = StackEntry(topRootAddr, true, false, GEN_RT_BVH_INTERNAL_NODE_length * 4);
    parent_map_device_offset[topRootAddr + device_offset] = StackEntry((uint8_t*)_topLevelAS + device_offset, true, false, GEN_RT_BVH_length * 4);
    node_info_device_offset[topRootAddr + device_offset] = StackEntry(topRootAddr + device_offset, true, false, GEN_RT_BVH_INTERNAL_NODE_length * 4);
    reverse_stack.push_front(std::make_pair(parent_map[topRootAddr], tree_level));

    StackEntry current_node;
    
    while (!stack.empty())
    {
        current_node = stack.front();
        stack.pop_front();

        if (current_node.topLevel && !current_node.leaf) // Top level internal node
        {
            struct GEN_RT_BVH_INTERNAL_NODE node;
            GEN_RT_BVH_INTERNAL_NODE_unpack(&node, current_node.addr);

            tree_level++;

            uint8_t *child_addr = current_node.addr + (node.ChildOffset * 64);
            for(int i = 0; i < 6; i++)
            {
                if (node.ChildSize[i] > 0)
                {
                    if(node.ChildType[i] != NODE_TYPE_INTERNAL)
                    {
                        assert(node.ChildType[i] == NODE_TYPE_INSTANCE);
                        stack.push_front(StackEntry(child_addr, true, true));

                        parent_map[child_addr] = StackEntry(current_node.addr, current_node.topLevel, current_node.leaf, current_node.size);
                        node_info[child_addr] = StackEntry(current_node.addr, true, true, GEN_RT_BVH_INSTANCE_LEAF_length * 4);
                        parent_map_device_offset[child_addr + device_offset] = StackEntry(current_node.addr + device_offset, current_node.topLevel, current_node.leaf, current_node.size);
                        node_info_device_offset[child_addr + device_offset] = StackEntry(child_addr + device_offset, true, true, GEN_RT_BVH_INSTANCE_LEAF_length * 4);
                        reverse_stack.push_front(std::make_pair(parent_map[child_addr], tree_level));
                    }
                    else
                    {
                        stack.push_front(StackEntry(child_addr, true, false));

                        parent_map[child_addr] = StackEntry(current_node.addr, current_node.topLevel, current_node.leaf, current_node.size);
                        node_info[child_addr] = StackEntry(child_addr, true, false, GEN_RT_BVH_INTERNAL_NODE_length * 4);
                        parent_map_device_offset[child_addr + device_offset] = StackEntry(current_node.addr + device_offset, current_node.topLevel, current_node.leaf, current_node.size);
                        node_info_device_offset[child_addr + device_offset] = StackEntry(child_addr + device_offset, true, false, GEN_RT_BVH_INTERNAL_NODE_length * 4);
                        reverse_stack.push_front(std::make_pair(parent_map[child_addr], tree_level));
                    }
                }
                child_addr += node.ChildSize[i] * 64;
            }
        }
        else if (current_node.topLevel && current_node.leaf) // Top level leaf node
        {
            uint8_t* leaf_addr = current_node.addr;

            GEN_RT_BVH_INSTANCE_LEAF instanceLeaf;
            GEN_RT_BVH_INSTANCE_LEAF_unpack(&instanceLeaf, leaf_addr);
            
            assert(instanceLeaf.BVHAddress != NULL);
            GEN_RT_BVH botLevelASAddr;
            GEN_RT_BVH_unpack(&botLevelASAddr, (uint8_t *)(leaf_addr + instanceLeaf.BVHAddress));

            tree_level++;

            uint8_t * botLevelRootAddr;
            botLevelRootAddr = ((uint8_t *)((uint64_t)leaf_addr + instanceLeaf.BVHAddress)) + botLevelASAddr.RootNodeOffset;
            stack.push_front(StackEntry(botLevelRootAddr, false, false));
            parent_map[botLevelRootAddr] =StackEntry(current_node.addr, current_node.topLevel, current_node.leaf, current_node.size);
            node_info[botLevelRootAddr] = StackEntry(botLevelRootAddr, false, false, GEN_RT_BVH_INTERNAL_NODE_length * 4);
            parent_map_device_offset[botLevelRootAddr + device_offset] = StackEntry(current_node.addr + device_offset, current_node.topLevel, current_node.leaf, current_node.size);
            node_info_device_offset[botLevelRootAddr + device_offset] = StackEntry(botLevelRootAddr + device_offset, false, false, GEN_RT_BVH_INTERNAL_NODE_length * 4);
            reverse_stack.push_front(std::make_pair(parent_map[botLevelRootAddr], tree_level));
        }
        else if (!current_node.topLevel && !current_node.leaf) // Bottom level internal node
        {
            struct GEN_RT_BVH_INTERNAL_NODE node;
            GEN_RT_BVH_INTERNAL_NODE_unpack(&node, current_node.addr);

            tree_level++;

            uint8_t *child_addr = current_node.addr + (node.ChildOffset * 64);
            for(int i = 0; i < 6; i++)
            {
                if (node.ChildSize[i] > 0)
                {
                    if(node.ChildType[i] != NODE_TYPE_INTERNAL)
                    {
                        //assert(node.ChildType[i] == NODE_TYPE_INSTANCE);
                        stack.push_front(StackEntry(child_addr, false, true));

                        parent_map[child_addr] = StackEntry(current_node.addr, current_node.topLevel, current_node.leaf, current_node.size);
                        node_info[child_addr] = StackEntry(child_addr, false, true, GEN_RT_BVH_length * 4);
                        parent_map_device_offset[child_addr + device_offset] = StackEntry(current_node.addr + device_offset, current_node.topLevel, current_node.leaf, current_node.size);
                        node_info_device_offset[child_addr + device_offset] = StackEntry(child_addr + device_offset, false, true, GEN_RT_BVH_length * 4);
                        reverse_stack.push_front(std::make_pair(parent_map[child_addr], tree_level));
                    }
                    else
                    {
                        stack.push_front(StackEntry(child_addr, false, false));

                        parent_map[child_addr] = StackEntry(current_node.addr, current_node.topLevel, current_node.leaf, current_node.size);
                        node_info[child_addr] = StackEntry(child_addr, false, false, GEN_RT_BVH_INTERNAL_NODE_length * 4);
                        parent_map_device_offset[child_addr + device_offset] = StackEntry(current_node.addr + device_offset, current_node.topLevel, current_node.leaf, current_node.size);
                        node_info_device_offset[child_addr + device_offset] = StackEntry(child_addr + device_offset, false, false, GEN_RT_BVH_INTERNAL_NODE_length * 4);
                        reverse_stack.push_front(std::make_pair(parent_map[child_addr], tree_level));
                    }
                }
                child_addr += node.ChildSize[i] * 64;
            }
        }
        else if (!current_node.topLevel && current_node.leaf) // Bottom level leaf node
        {
            assert(parent_map.count(current_node.addr) != 0);
            assert(node_info.count(current_node.addr) != 0);
            // uint8_t* leaf_addr = current_node.addr;

            // struct GEN_RT_BVH_PRIMITIVE_LEAF_DESCRIPTOR leaf_descriptor;
            // GEN_RT_BVH_PRIMITIVE_LEAF_DESCRIPTOR_unpack(&leaf_descriptor, leaf_addr);
            // transactions.push_back(MemoryTransactionRecord((uint8_t*)((uint64_t)leaf_addr + device_offset), GEN_RT_BVH_PRIMITIVE_LEAF_DESCRIPTOR_length * 4, TransactionType::BVH_PRIMITIVE_LEAF_DESCRIPTOR));

            // if (leaf_descriptor.LeafType == TYPE_QUAD)
            // {
            //     parent_map[child_addr] = StackEntry(current_node.addr, current_node.topLevel, current_node.leaf, current_node.size);
            //     node_info[child_addr] = StackEntry(child_addr, false, false, GEN_RT_BVH_INTERNAL_NODE_length * 4);
            //     parent_map_device_offset[child_addr + device_offset] = StackEntry(current_node.addr + device_offset, current_node.topLevel, current_node.leaf, current_node.size);
            //     node_info_device_offset[child_addr + device_offset] = StackEntry(child_addr + device_offset, false, false, GEN_RT_BVH_INTERNAL_NODE_length * 4);
            //     //GEN_RT_BVH_QUAD_LEAF_length
            // }
            // else
            // {
            //     //GEN_RT_BVH_PROCEDURAL_LEAF_length
            // }
        }
        else
        {
            assert(0);
        }
    }


    // Build a map to query a list of child nodes given a parent node
    std::map<uint8_t*, std::vector<StackEntry>> children_map;
    for (auto node : parent_map)
    {
        if (node.second.addr != 0)
        {
            children_map[node.second.addr].push_back(node_info[node.first]);
        }
    }

    VulkanRayTracing::parent_map = parent_map;
    VulkanRayTracing::children_map = children_map;
    VulkanRayTracing::node_info = node_info;
    VulkanRayTracing::parent_map_device_offset = parent_map_device_offset;
    VulkanRayTracing::node_info_device_offset = node_info_device_offset;
    VulkanRayTracing::reverse_stack = reverse_stack;

    std::cout << "Parent Pointer Pass  and Reverse Stack Done" << std::endl;

    // for (auto node : node_info)
    // {
    //     printf("Node: 0x%x Parent: 0x%x Size: %d\n", node.first, parent_map[node.first].addr, node.second.size);
    // }
}


void VulkanRayTracing::createTreeletsBottomUp(VkAccelerationStructureKHR _topLevelAS, int64_t device_offset, int maxBytesPerTreelet)
{
    int remaining_bytes = maxBytesPerTreelet;
    std::map<StackEntry, std::vector<StackEntry>> completed_treelet_roots; // <address, list of nodes in treelet>

    StackEntry current_treelet_root;

    while (!reverse_stack.empty())
    {
        StackEntry current_node = reverse_stack.front().first;
        int current_tree_level = reverse_stack.front().second;
        reverse_stack.pop_front();

        bool keep_searching = true;
        std::vector<StackEntry> nodes_in_current_treelet;
        if (remaining_bytes >= current_node.size)
        {
            // Subtract current node size from remaining bytes
            remaining_bytes -= current_node.size;
            nodes_in_current_treelet.push_back(current_node);

            // Set current_node as the temporary treelet root
            current_treelet_root = current_node;
        }
        else
        {
            // Current treelet is full, add it to the list of completed treelets
            completed_treelet_roots[current_treelet_root] = nodes_in_current_treelet;
            keep_searching = false;
            remaining_bytes = maxBytesPerTreelet;
        }

        // Check parent of current_node
        while (remaining_bytes >= 0 && keep_searching)
        {
            // Check if current_node has a parent node
            StackEntry parent_node = parent_map[current_node.addr];
            if (parent_node.addr != nullptr)
            {
                int parent_node_size = parent_node.size;
                int children_sizes = 0;

                for (auto child : children_map[parent_node.addr])
                {
                    if (child.addr != current_node.addr) // find the size of all children except the current node since the current node is already accounted for above
                        children_sizes += child.size;
                }

                if (remaining_bytes >= (parent_node_size + children_sizes))
                {
                    // Subtract parent node size from remaining bytes
                    remaining_bytes -= (parent_node_size + children_sizes);
                
                    // Set current_node as the new temporary treelet root
                    current_treelet_root = parent_node;

                    // Add nodes to the current treelet
                    nodes_in_current_treelet.push_back(parent_node);
                    for (auto child : children_map[parent_node.addr])
                    {
                        if (child.addr != current_node.addr)
                            nodes_in_current_treelet.push_back(child);
                    }

                    // Remove the other children and the parent node from the stack
                    for (auto it = reverse_stack.begin(); it != reverse_stack.end(); it++)
                    {
                        if (it->first.addr == parent_node.addr)
                        {
                            reverse_stack.erase(it);
                            break;
                        }
                    }

                    for (auto child : children_map[parent_node.addr])
                    {
                        for (auto it = reverse_stack.begin(); it != reverse_stack.end(); it++)
                        {
                            if (it->first.addr == child.addr)
                            {
                                reverse_stack.erase(it);
                                break;
                            }
                        }
                    }

                    // Set current_node to the parent node
                    current_node = parent_node;
                }
                else
                {
                    // Current treelet is full, add it to the list of completed treelets
                    completed_treelet_roots[current_treelet_root] = nodes_in_current_treelet;
                    keep_searching = false;
                    remaining_bytes = maxBytesPerTreelet;
                }
            }
            else
            {
                completed_treelet_roots[current_treelet_root] = nodes_in_current_treelet;
                keep_searching = false;
                remaining_bytes = maxBytesPerTreelet;
                break;
            }
            
        }
        


        // Check if parent fits in remaining bytes

    }
}


void VulkanRayTracing::createTreelets(VkAccelerationStructureKHR _topLevelAS, int64_t device_offset, int maxBytesPerTreelet)
{
    unsigned total_bvh_size = 0;
    int remaining_bytes = maxBytesPerTreelet;
    std::map<StackEntry, std::vector<StackEntry>> completed_treelet_roots; // <address, placeholder>, just to look up if an address is a treelet root node or not
    std::map<uint8_t*, std::vector<StackEntry>> completed_treelet_roots_addr_only; // <address, placeholder>, just to look up if an address is a treelet root node or not

    std::map<StackEntry, std::vector<StackEntry>> treelet_root_child_map;
    std::map<uint8_t*, std::vector<StackEntry>> treelet_root_addr_only_child_map; // this shows which treelet roots are connected to other treelet roots, to see the parent child relationship between treelet roots?

    std::deque<StackEntry> treelet_roots_pending_work_queue;
    std::deque<float> treelet_roots_pending_work_queue_sah;
    std::deque<int> treelet_roots_pending_work_queue_nodesize;

    std::deque<StackEntry> stack; // currently_considered_nodes
    std::deque<float> sah_stack; // records the SAH of currently_considered_nodes
    std::deque<int> nodesize_stack; //records node size of currently_considered_nodes

    std::vector<StackEntry> nodes_in_current_treelet; // list of nodes that belong to the current treelet

    unsigned total_nodes_accessed = 0;
    std::map<uint8_t*, unsigned> tree_level_map;

    // Start with top root node
    treelet_roots_pending_work_queue.push_back(StackEntry((uint8_t*)_topLevelAS, true, false, GEN_RT_BVH_length * 4));
    treelet_roots_pending_work_queue_sah.push_back(1.0);
    treelet_roots_pending_work_queue_nodesize.push_back(GEN_RT_BVH_length * 4 + GEN_RT_BVH_INTERNAL_NODE_length * 4);

    GEN_RT_BVH topBVH; //TODO: test hit with world before traversal
    GEN_RT_BVH_unpack(&topBVH, (uint8_t*)_topLevelAS);
    remaining_bytes -= GEN_RT_BVH_length * 4; //_topLevelAS
    assert(remaining_bytes >= 0);
    total_bvh_size += GEN_RT_BVH_length * 4;
    nodes_in_current_treelet.push_back(StackEntry((uint8_t*)_topLevelAS, true, false, GEN_RT_BVH_length * 4));

    uint8_t* topRootAddr = (uint8_t*)_topLevelAS + topBVH.RootNodeOffset;
    stack.push_back(StackEntry(topRootAddr, true, false, GEN_RT_BVH_INTERNAL_NODE_length * 4));
    sah_stack.push_back(1.0); // placeholder
    nodesize_stack.push_back(GEN_RT_BVH_INTERNAL_NODE_length * 4);
    tree_level_map[topRootAddr] = 1;

    while (!treelet_roots_pending_work_queue.empty())
    {
        StackEntry next_node(NULL, false, false);
        uint8_t *node_addr = NULL;
        uint8_t *next_node_addr = NULL;
        float node_sah = -1.0;
        float next_node_sah = -1.0;
        int node_size = -1;
        int next_node_size = -1;

        next_node = stack.front();
        next_node_addr = stack.front().addr;
        stack.pop_front();

        next_node_sah = sah_stack.front();
        sah_stack.pop_front();

        next_node_size = nodesize_stack.front();
        nodesize_stack.pop_front();

        while (next_node_addr > 0)
        {
            if (next_node_addr > 0 && next_node.topLevel && !next_node.leaf)
            {
                node_addr = next_node_addr;
                next_node_addr = NULL;
                struct GEN_RT_BVH_INTERNAL_NODE node;
                GEN_RT_BVH_INTERNAL_NODE_unpack(&node, node_addr);
                remaining_bytes -= GEN_RT_BVH_INTERNAL_NODE_length * 4;
                assert(remaining_bytes >= 0);
                total_bvh_size += GEN_RT_BVH_INTERNAL_NODE_length * 4;
                nodes_in_current_treelet.push_back(next_node);
                total_nodes_accessed++;

                for(int i = 0; i < 6; i++)
                {
                    if (node.ChildSize[i] > 0)
                    {
                        // Calculating children bounds
                        float3 lo, hi;
                        set_child_bounds(&node, i, &lo, &hi);

                        float child_sah = calculateSAH(lo, hi);
                        sah_stack.push_back(child_sah);

                        // std::cout << "node " << (void*) node_addr << ", child number " << i << ", ";
                        // std::cout << "lo = (" << lo.x << ", " << lo.y << ", " << lo.z << "), ";
                        // std::cout << "hi = (" << hi.x << ", " << hi.y << ", " << hi.z << "), SAH = " << child_sah << std::endl;
                    }
                    else
                    {
                        // std::cout << "No children in node " << (void*) node_addr << std::endl;
                    }
                }

                uint8_t *child_addr = node_addr + (node.ChildOffset * 64);
                for(int i = 0; i < 6; i++)
                {
                    if (node.ChildSize[i] > 0)
                    {
                        if(node.ChildType[i] != NODE_TYPE_INTERNAL)
                        {
                            assert(node.ChildType[i] == NODE_TYPE_INSTANCE); // top level leaf
                            stack.push_back(StackEntry(child_addr, true, true, GEN_RT_BVH_INSTANCE_LEAF_length * 4));
                            nodesize_stack.push_back(GEN_RT_BVH_INSTANCE_LEAF_length * 4 + GEN_RT_BVH_length * 4); // reason for adding GEN_RT_BVH_length * 4 is because the 2 nodes are traversed back to back
                            assert(tree_level_map.find(node_addr) != tree_level_map.end());
                            tree_level_map[child_addr] = tree_level_map[node_addr] + 1;
                        }
                        else
                        {
                            stack.push_back(StackEntry(child_addr, true, false, GEN_RT_BVH_INTERNAL_NODE_length * 4));
                            nodesize_stack.push_back(GEN_RT_BVH_INTERNAL_NODE_length * 4);
                            assert(tree_level_map.find(node_addr) != tree_level_map.end());
                            tree_level_map[child_addr] = tree_level_map[node_addr] + 1;
                        }
                    }
                    child_addr += node.ChildSize[i] * 64;
                }
            }
            // traverse top level leaf nodes
            else if (next_node_addr > 0 && next_node.topLevel && next_node.leaf)
            {
                //assert(stack.front().topLevel);

                // uint8_t* leaf_addr = stack.front().addr;
                // stack.pop_front();
                // float leaf_sah = sah_stack.front();
                // sah_stack.pop_front();
                // int leaf_nodesize = nodesize_stack.front();
                // nodesize_stack.pop_front();

                uint8_t* leaf_addr = next_node_addr;
                float leaf_sah = next_node_sah;
                int leaf_nodesize = next_node_size;

                GEN_RT_BVH_INSTANCE_LEAF instanceLeaf;
                GEN_RT_BVH_INSTANCE_LEAF_unpack(&instanceLeaf, leaf_addr);
                remaining_bytes -= GEN_RT_BVH_INSTANCE_LEAF_length * 4;
                assert(remaining_bytes >= 0);
                total_bvh_size += GEN_RT_BVH_INSTANCE_LEAF_length * 4;
                nodes_in_current_treelet.push_back(next_node);
                total_nodes_accessed++;

                float4x4 worldToObjectMatrix = instance_leaf_matrix_to_float4x4(&instanceLeaf.WorldToObjectm00);
                float4x4 objectToWorldMatrix = instance_leaf_matrix_to_float4x4(&instanceLeaf.ObjectToWorldm00);

                assert(instanceLeaf.BVHAddress != NULL);
                GEN_RT_BVH botLevelASAddr;
                GEN_RT_BVH_unpack(&botLevelASAddr, (uint8_t *)(leaf_addr + instanceLeaf.BVHAddress));
                remaining_bytes -= GEN_RT_BVH_length * 4;
                nodes_in_current_treelet.push_back(StackEntry((uint8_t *)(leaf_addr + instanceLeaf.BVHAddress), true, true, GEN_RT_BVH_length * 4));
                assert(remaining_bytes >= 0);
                total_bvh_size += GEN_RT_BVH_length * 4;

                // std::ofstream offsetfile;
                // offsetfile.open("offsets.txt", std::ios::app);
                // offsetfile << (int64_t)instanceLeaf.BVHAddress << std::endl;

                // std::ofstream leaf_addr_file;
                // leaf_addr_file.open("leaf.txt", std::ios::app);
                // leaf_addr_file << (int64_t)((uint64_t)leaf_addr - (uint64_t)_topLevelAS) << std::endl;

                uint8_t * botLevelRootAddr = ((uint8_t *)(leaf_addr + instanceLeaf.BVHAddress)) + botLevelASAddr.RootNodeOffset;
                stack.push_back(StackEntry(botLevelRootAddr, false, false, GEN_RT_BVH_INTERNAL_NODE_length * 4));
                sah_stack.push_back(1.0);
                nodesize_stack.push_back(GEN_RT_BVH_INTERNAL_NODE_length * 4);
                assert(tree_level_map.find(leaf_addr) != tree_level_map.end());
                tree_level_map[botLevelRootAddr] = tree_level_map[leaf_addr];
            }
            // traverse bottom level tree
            else if (next_node_addr > 0 && !next_node.topLevel && !next_node.leaf)
            {
                // uint8_t* node_addr = NULL;
                // uint8_t* next_node_addr = stack.front().addr;
                // stack.pop_front();
                

                // traverse bottom level internal nodes
                node_addr = next_node_addr;
                next_node_addr = NULL;

                // if(node_addr == *(++path.rbegin()))
                //     printf("this is where things go wrong\n");

                struct GEN_RT_BVH_INTERNAL_NODE node;
                GEN_RT_BVH_INTERNAL_NODE_unpack(&node, node_addr);
                remaining_bytes -= GEN_RT_BVH_INTERNAL_NODE_length * 4;
                assert(remaining_bytes >= 0);
                total_bvh_size += GEN_RT_BVH_INTERNAL_NODE_length * 4;
                nodes_in_current_treelet.push_back(next_node);
                total_nodes_accessed++;

                // if (debugTraversal)
                // {
                //     traversalFile << "traversing bot level internal node " << (void *)node_addr << "\n";
                // }

                // bool child_hit[6];
                // float thit[6];
                for(int i = 0; i < 6; i++)
                {
                    if (node.ChildSize[i] > 0)
                    {
                        // float3 idir = calculate_idir(objectRay.get_direction()); //TODO: this works wierd if one of ray dimensions is 0
                        float3 lo, hi;
                        set_child_bounds(&node, i, &lo, &hi);

                        float child_sah = calculateSAH(lo, hi);
                        sah_stack.push_back(child_sah);

                        // std::cout << "node " << (void*) node_addr << ", child number " << i << ", ";
                        // std::cout << "lo = (" << lo.x << ", " << lo.y << ", " << lo.z << "), ";
                        // std::cout << "hi = (" << hi.x << ", " << hi.y << ", " << hi.z << "), SAH = " << child_sah << std::endl;
                    }
                    else
                    {
                        // std::cout << "No children in node " << (void*) node_addr << std::endl;
                    }
                }

                uint8_t *child_addr = node_addr + (node.ChildOffset * 64);
                for(int i = 0; i < 6; i++)
                {
                    if(node.ChildSize[i] > 0)
                    {
                        // if (debugTraversal)
                        // {
                        //     traversalFile << "add child node " << (void *)child_addr << ", child number " << i << ", type " << node.ChildType[i] << ", to stack" << std::endl;
                        // }

                        if(node.ChildType[i] != NODE_TYPE_INTERNAL)
                        {
                            //stack.push_back(StackEntry(child_addr, false, true, GEN_RT_BVH_PRIMITIVE_LEAF_DESCRIPTOR_length * 4 + GEN_RT_BVH_length * 4));
                            //nodesize_stack.push_back(GEN_RT_BVH_PRIMITIVE_LEAF_DESCRIPTOR_length * 4 + GEN_RT_BVH_length * 4); // reason for adding GEN_RT_BVH_length * 4 is because the 2 nodes are traversed back to back
                            stack.push_back(StackEntry(child_addr, false, true, GEN_RT_BVH_length * 4));
                            nodesize_stack.push_back(GEN_RT_BVH_length * 4); // both leaf descriptor and quad/procedural leaf use the same addresss
                            assert(tree_level_map.find(node_addr) != tree_level_map.end());
                            tree_level_map[child_addr] = tree_level_map[node_addr] + 1;
                        }
                        else
                        {
                            stack.push_back(StackEntry(child_addr, false, false, GEN_RT_BVH_INTERNAL_NODE_length * 4));
                            nodesize_stack.push_back(GEN_RT_BVH_INTERNAL_NODE_length * 4);
                            assert(tree_level_map.find(node_addr) != tree_level_map.end());
                            tree_level_map[child_addr] = tree_level_map[node_addr] + 1;
                        }
                    }
                    child_addr += node.ChildSize[i] * 64;
                }
            }
            // traverse bottom level leaf nodes
            else if (next_node_addr > 0 && !next_node.topLevel && next_node.leaf)
            {
                uint8_t* leaf_addr = next_node_addr;
                // stack.pop_front();
                struct GEN_RT_BVH_PRIMITIVE_LEAF_DESCRIPTOR leaf_descriptor;
                GEN_RT_BVH_PRIMITIVE_LEAF_DESCRIPTOR_unpack(&leaf_descriptor, leaf_addr);
                //remaining_bytes -= GEN_RT_BVH_PRIMITIVE_LEAF_DESCRIPTOR_length * 4;
                //assert(remaining_bytes >= 0);
                //total_bvh_size += GEN_RT_BVH_PRIMITIVE_LEAF_DESCRIPTOR_length * 4;
                nodes_in_current_treelet.push_back(next_node);

                if (leaf_descriptor.LeafType == TYPE_QUAD)
                {
                    struct GEN_RT_BVH_QUAD_LEAF leaf;
                    GEN_RT_BVH_QUAD_LEAF_unpack(&leaf, leaf_addr);

                    float3 p[3];
                    for(int i = 0; i < 3; i++)
                    {
                        p[i].x = leaf.QuadVertex[i].X;
                        p[i].y = leaf.QuadVertex[i].Y;
                        p[i].z = leaf.QuadVertex[i].Z;
                    }

                    remaining_bytes -= GEN_RT_BVH_QUAD_LEAF_length * 4;
                    assert(remaining_bytes >= 0);
                    total_bvh_size += GEN_RT_BVH_QUAD_LEAF_length * 4;
                    total_nodes_accessed++;
                }
                else
                {
                    struct GEN_RT_BVH_PROCEDURAL_LEAF leaf;
                    GEN_RT_BVH_PROCEDURAL_LEAF_unpack(&leaf, leaf_addr);
                    remaining_bytes -= GEN_RT_BVH_PROCEDURAL_LEAF_length * 4;
                    assert(remaining_bytes >= 0);
                    total_bvh_size += GEN_RT_BVH_PROCEDURAL_LEAF_length * 4;
                    total_nodes_accessed++;
                }
            }

            // Check if first node on stack fits in the treelet
            if (remaining_bytes - nodesize_stack.front() >= 0 && !nodesize_stack.empty())
            {
                // If fits, pop the first node in the stack and set it as the next node to process
                next_node = stack.front();
                next_node_addr = stack.front().addr;
                stack.pop_front();

                next_node_sah = sah_stack.front();
                sah_stack.pop_front();

                next_node_size = nodesize_stack.front();
                nodesize_stack.pop_front();

                assert(stack.size() == sah_stack.size() && stack.size() == nodesize_stack.size());
            }
            else if (remaining_bytes - nodesize_stack.front() < 0 && !nodesize_stack.empty()) // Node doesn't fit in current treelet
            {
                // Move the first treelet node in treelet_roots_pending_work_queue to completed_treelet_roots
                StackEntry completed_treelet = treelet_roots_pending_work_queue.front();
                StackEntry completed_treelet_device_addr = treelet_roots_pending_work_queue.front();
                completed_treelet_device_addr.addr += device_offset;
                // std::cout << "Treelet root " << (void*)completed_treelet.addr << " done: size limit reached" << std::endl;
                std::vector<StackEntry> node_list = nodes_in_current_treelet;
                for (int i = 0; i < node_list.size(); i++)
                {
                    node_list[i].addr += device_offset;
                }
                completed_treelet_roots[completed_treelet_device_addr] = node_list;
                completed_treelet_roots_addr_only[completed_treelet_device_addr.addr] = node_list;
                treelet_roots_pending_work_queue.pop_front();
                nodes_in_current_treelet.clear();
                
                // Add all nodes in stack to treelet_roots_pending_work_queue
                std::vector<StackEntry> children;
                std::vector<StackEntry> children_device_addr;

                for (int i = 0; i < stack.size(); i++)
                {
                    treelet_roots_pending_work_queue.push_back(stack[i]);
                    treelet_roots_pending_work_queue_sah.push_back(sah_stack[i]);
                    treelet_roots_pending_work_queue_nodesize.push_back(nodesize_stack[i]);

                    if (i != 0) // Don't add the first element which is the node itself
                    {
                        children.push_back(stack[i]);
                        StackEntry tmp = stack[i];
                        tmp.addr += device_offset;
                        children_device_addr.push_back(tmp);
                    }
                }

                treelet_root_child_map[completed_treelet_device_addr] = children_device_addr;
                treelet_root_addr_only_child_map[completed_treelet_device_addr.addr ] = children_device_addr;

                // Clear out the current stack and push first node in treelet_roots_pending_work_queue to the stacks
                stack.clear();
                sah_stack.clear();
                nodesize_stack.clear();

                stack.push_back(treelet_roots_pending_work_queue.front());
                sah_stack.push_back(treelet_roots_pending_work_queue_sah.front());
                nodesize_stack.push_back(treelet_roots_pending_work_queue_nodesize.front());

                // Set next node to NULL
                next_node = StackEntry(NULL, false, false);
                next_node_addr = NULL;
                next_node_sah = -1.0;
                next_node_size = -1;

                // Reset remaining_bytes
                remaining_bytes = maxBytesPerTreelet;

                
            }
            else if (nodesize_stack.empty()) // If stack is empty, current treelet is done
            {
                assert(stack.empty());
                assert(sah_stack.empty());
                assert(nodesize_stack.empty());

                // Move the first treelet node in treelet_roots_pending_work_queue to completed_treelet_roots
                StackEntry completed_treelet = treelet_roots_pending_work_queue.front();
                StackEntry completed_treelet_device_addr = treelet_roots_pending_work_queue.front();
                completed_treelet_device_addr.addr += device_offset;
                // std::cout << "Treelet root " << (void*)completed_treelet.addr << " done: no more nodes to include" << std::endl;
                std::vector<StackEntry> node_list = nodes_in_current_treelet;
                for (int i = 0; i < node_list.size(); i++)
                {
                    node_list[i].addr += device_offset;
                }
                completed_treelet_roots[completed_treelet_device_addr] = node_list;
                completed_treelet_roots_addr_only[completed_treelet_device_addr.addr] = node_list;
                treelet_roots_pending_work_queue.pop_front();
                nodes_in_current_treelet.clear();

                std::vector<StackEntry> children = {};
                treelet_root_child_map[completed_treelet_device_addr] = children;
                treelet_root_addr_only_child_map[completed_treelet_device_addr.addr] = children;

                // Set next node to NULL
                next_node = StackEntry(NULL, false, false);
                next_node_addr = NULL;
                next_node_sah = -1.0;
                next_node_size = -1;

                // Reset remaining_bytes
                remaining_bytes = maxBytesPerTreelet;

                // Push first item in treelet_roots_pending_work_queue onto the stack
                stack.clear();
                sah_stack.clear();
                nodesize_stack.clear();

                stack.push_back(treelet_roots_pending_work_queue.front());
                sah_stack.push_back(treelet_roots_pending_work_queue_sah.front());
                nodesize_stack.push_back(treelet_roots_pending_work_queue_nodesize.front());
            }
            else
            {
                std::cout << "Unknown treelet case" << std::endl;
                assert(false);
            }

            // // Check what nodes to add to the treelet given the remaining bytes
            // // replace this with better algorithm to find what nodes to include in treelet
            // int size_limit = remaining_bytes;
            // int idx = 0;
            // for (auto node_size : nodesize_stack)
            // {
            //     size_limit -= node_size;
            //     if (size_limit >= 0)
            //         idx++;
            //     else
            //         break;
            // }

            // // if all nodes fit, add their children to the stack, then push all their children onto the stack
            // if (idx == stack.size())
            // {
            //         // Pop the first node in the stack and add its children to the stack
            //         next_node_addr = stack.front().addr;
            //         stack.pop_front();

            //         next_node_sah = sah_stack.front();
            //         sah_stack.pop_front();

            //         next_node_size = nodesize_stack.front();
            //         nodesize_stack.pop_front();

            //         remaining_bytes -= next_node_size;
            // }
            // else // Not all nodes fit in the remaining bytes
            // {

            // }
        }
    }

    // Remove dupes in completed_treelet_roots_addr_only
    for (auto root_node : completed_treelet_roots_addr_only)
    {
        std::vector <StackEntry> dupe_free_node_list;
        for (int i = 0; i < root_node.second.size(); i++)
        {
            bool found = false;
            for (int j = 0; j < dupe_free_node_list.size(); j++)
            {
                if (root_node.second[i].addr == dupe_free_node_list[j].addr)
                {
                    found = true;
                    break;
                }
            }

            if (!found)
                dupe_free_node_list.push_back(root_node.second[i]);
        }
        completed_treelet_roots_addr_only[root_node.first] = dupe_free_node_list;
    }

    std::cout << "Treelet formation done" << std::endl;
    treelet_roots = completed_treelet_roots;
    treelet_roots_addr_only = completed_treelet_roots_addr_only;

    treelet_child_map = treelet_root_child_map;
    treelet_addr_only_child_map = treelet_root_addr_only_child_map;
    std::cout << "Total BVH Size: " << total_bvh_size << " bytes" << std::endl;
    std::cout << "Treelet Size: " << maxBytesPerTreelet << " bytes" << std::endl;
    std::cout << "Treelet Count: " << treelet_roots_addr_only.size() << std::endl;

    buildNodeToRootMap();

    // BVH stats
    std::map<StackEntry, int> checkdupes;
    unsigned treesize = 0;
    for (auto root : treelet_roots) 
    {
        //assert(root.first.size != 0);
        //treesize += root.first.size;
        //assert(!checkdupes.count(root.first));
        checkdupes[root.first]++;

        for (auto children : root.second) 
        {
            //assert(children.size != 0);
            //treesize += children.size;
            //assert(!checkdupes.count(children));
            checkdupes[children]++;
        }
    }
    unsigned dupecount = 0;
    for (auto node : checkdupes) {
        treesize += node.first.size;
        if (node.second > 1) {
            dupecount++;
            //printf("addr: %d, top level: %d, leaf node: %d, size: %d, dupe count: %d\n", node.first.addr, node.first.topLevel, node.first.leaf, node.first.size, node.second);
        }
    }
    std::cout << "Duplicate nodes: " << dupecount << std::endl;
    std::cout << "Verified Total BVH Size: " << treesize << " bytes" << std::endl;
    std::cout << "Node Count: " << checkdupes.size() << std::endl;
    std::cout << "Treelet Roots: " << treelet_roots.size() << std::endl;

    unsigned treesize_with_only_treelet_roots = 0;
    for (auto root : treelet_roots) {
        treesize_with_only_treelet_roots += root.first.size;
    }
    std::cout << "Tree Size With Only Treelet Roots: " << treesize_with_only_treelet_roots << " bytes" << std::endl;

    // Packed size
    std::map<StackEntry, unsigned> root_node_sizes;
    for (auto root : treelet_roots) {
        for (auto children : root.second) {
            root_node_sizes[root.first] += children.size;
        }
    }
    assert(treelet_roots.size() == root_node_sizes.size());

    int cachelines = 0;
    for (auto root : root_node_sizes) {
        cachelines += (root.second + 128 - 1) / 128;
    }
    std::cout << "Cachelines: " << cachelines << std::endl;
    std::cout << "Packed size: " << cachelines * 128 << "B" << std::endl;
    std::cout << "Size expansion: " << ((double)(cachelines * 128) - (double)treesize) / (double)treesize * (double)100 << "%" << std::endl;
}

unsigned rayCount = 0;
static unsigned VulkanRayTracing::accessedDataSize = 0;

bool parentPointerPassDone = false;
bool treeletsFormed = false;
bool treeletsFormed2 = false;
bool debugTraversal = false;
bool found_AS = false;
VkAccelerationStructureKHR topLevelAS_first = NULL;

void VulkanRayTracing::traceRayWithTreelets(VkAccelerationStructureKHR _topLevelAS,
				   uint rayFlags,
                   uint cullMask,
                   uint sbtRecordOffset,
                   uint sbtRecordStride,
                   uint missIndex,
                   float3 origin,
                   float Tmin,
                   float3 direction,
                   float Tmax,
                   int payload,
                   const ptx_instruction *pI,
                   ptx_thread_info *thread)
{
    // std::cout << "traceRayWithTreelets" << std::endl;
    // printf("## calling trceRay function. rayFlags = %d, cullMask = %d, sbtRecordOffset = %d, sbtRecordStride = %d, missIndex = %d, origin = (%f, %f, %f), Tmin = %f, direction = (%f, %f, %f), Tmax = %f, payload = %d\n",
    //         rayFlags, cullMask, sbtRecordOffset, sbtRecordStride, missIndex, origin.x, origin.y, origin.z, Tmin, direction.x, direction.y, direction.z, Tmax, payload);

    if (!use_external_launcher && !dumped) 
    {
        dump_AS(VulkanRayTracing::descriptorSet, _topLevelAS);
        std::cout << "Trace dumped" << std::endl;
        dumped = true;
    }

    // Convert device address back to host address for func sim. This will break if the device address was modified then passed to traceRay. Should be fixable if I also record the size when I malloc then I can check the bounds of the device address.
    uint8_t* deviceAddress = nullptr;
    int64_t device_offset = 0;
    if (use_external_launcher)
    {
        deviceAddress = (uint8_t*)_topLevelAS;
        bool addressFound = false;
        for (int i = 0; i < MAX_DESCRIPTOR_SETS; i++)
        {
            for (int j = 0; j < MAX_DESCRIPTOR_SET_BINDINGS; j++)
            {
                if (launcher_deviceDescriptorSets[i][j] == (void*)_topLevelAS)
                {
                    _topLevelAS = launcher_descriptorSets[i][j];
                    addressFound = true;
                    break;
                }
            }
            if (addressFound)
                break;
        }
        if (!addressFound)
            abort();
    
        // Calculate offset between host and device for memory transactions
        device_offset = (uint64_t)deviceAddress - (uint64_t)_topLevelAS;
    }


    // Parent Pointer Pass
    if (!parentPointerPassDone)
    {
        parentPointerPass(_topLevelAS, device_offset);
        parentPointerPassDone = true;
    }

    // // Create Treelets Bottom Up
    // if (!treeletsFormed2)
    // {
    //     createTreeletsBottomUp(_topLevelAS, device_offset, GPGPU_Context()->the_gpgpusim->g_the_gpu->get_config().max_treelet_size); // 48*1024 aila2010 paper
    //     treeletsFormed2 = true;
    // }

    // Form Treelets
    if (!treeletsFormed)
    {
        createTreelets(_topLevelAS, device_offset, GPGPU_Context()->the_gpgpusim->g_the_gpu->get_config().max_treelet_size); // 48*1024 aila2010 paper
        treeletsFormed = true;
    }

    

    //int result = isTreeletRoot((uint8_t*)_topLevelAS); // test

    // if(!found_AS)
    // {
    //     found_AS = true;
    //     topLevelAS_first = _topLevelAS;
    //     print_tree.open("bvh_tree.txt");
    //     traverse_tree((uint8_t*)_topLevelAS);
    //     print_tree.close();
    // }
    // else
    // {
    //     assert(topLevelAS_first != NULL);
    //     assert(topLevelAS_first == _topLevelAS);
    // }

    Traversal_data traversal_data;

    traversal_data.ray_world_direction = direction;
    traversal_data.ray_world_origin = origin;
    traversal_data.sbtRecordOffset = sbtRecordOffset;
    traversal_data.sbtRecordStride = sbtRecordStride;
    traversal_data.missIndex = missIndex;
    traversal_data.Tmin = Tmin;
    traversal_data.Tmax = Tmax;

    std::ofstream traversalFile;

    if (debugTraversal)
    {
        traversalFile.open("traversal.txt");
        traversalFile << "starting traversal\n";
        traversalFile << "origin = (" << origin.x << ", " << origin.y << ", " << origin.z << "), ";
        traversalFile << "direction = (" << direction.x << ", " << direction.y << ", " << direction.z << "), ";
        traversalFile << "tmin = " << Tmin << ", tmax = " << Tmax << std::endl << std::endl;
    }


    bool terminateOnFirstHit = rayFlags & SpvRayFlagsTerminateOnFirstHitKHRMask;
    bool skipClosestHitShader = rayFlags & SpvRayFlagsSkipClosestHitShaderKHRMask;

    std::vector<MemoryTransactionRecord> transactions;
    std::vector<MemoryStoreTransactionRecord> store_transactions;

    gpgpu_context *ctx = GPGPU_Context();

    if (terminateOnFirstHit) ctx->func_sim->g_n_anyhit_rays++;
    else ctx->func_sim->g_n_closesthit_rays++;

    unsigned total_nodes_accessed = 0;
    std::map<uint8_t*, unsigned> tree_level_map;
    
	// Create ray
    rayCount++; // ray id starts from 1
	Ray ray;
	ray.make_ray(origin, direction, Tmin, Tmax, rayCount);
    thread->add_ray_properties(ray);

	// Set thit to max
    float min_thit = ray.dir_tmax.w;
    struct GEN_RT_BVH_QUAD_LEAF closest_leaf;
    struct GEN_RT_BVH_INSTANCE_LEAF closest_instanceLeaf;    
    float4x4 closest_worldToObject, closest_objectToWorld;
    Ray closest_objectRay;
    float min_thit_object;

	// Get bottom-level AS
    //uint8_t* topLevelASAddr = get_anv_accel_address((VkAccelerationStructureKHR)_topLevelAS);
    GEN_RT_BVH topBVH; //TODO: test hit with world before traversal
    GEN_RT_BVH_unpack(&topBVH, (uint8_t*)_topLevelAS);
    transactions.push_back(MemoryTransactionRecord((uint8_t*)((uint64_t)_topLevelAS + device_offset), GEN_RT_BVH_length * 4, TransactionType::BVH_STRUCTURE));
    ctx->func_sim->g_rt_mem_access_type[static_cast<int>(TransactionType::BVH_STRUCTURE)]++;
    
    uint8_t* topRootAddr = (uint8_t*)_topLevelAS + topBVH.RootNodeOffset;

    // Get min/max
    if (!ctx->func_sim->g_rt_world_set) {
        struct GEN_RT_BVH_INTERNAL_NODE node;
        GEN_RT_BVH_INTERNAL_NODE_unpack(&node, topRootAddr);
        for(int i = 0; i < 6; i++) {
            if (node.ChildSize[i] > 0) {
                float3 idir = calculate_idir(ray.get_direction()); //TODO: this works wierd if one of ray dimensions is 0
                float3 lo, hi;
                set_child_bounds(&node, i, &lo, &hi);
                ctx->func_sim->g_rt_world_min = min(ctx->func_sim->g_rt_world_min, lo);
                ctx->func_sim->g_rt_world_max = min(ctx->func_sim->g_rt_world_max, hi);
            }
        }
        ctx->func_sim->g_rt_world_set = true;
    }

    uint8_t* current_treelet_root = (uint8_t*)_topLevelAS + device_offset; // the first treelet root is always the root node
    std::list<StackEntry> current_treelet_stack;
    std::list<StackEntry> other_treelet_stack;
    tree_level_map[topRootAddr] = 1;
    
    {
        float3 lo, hi;
        lo.x = topBVH.BoundsMin.X;
        lo.y = topBVH.BoundsMin.Y;
        lo.z = topBVH.BoundsMin.Z;
        hi.x = topBVH.BoundsMax.X;
        hi.y = topBVH.BoundsMax.Y;
        hi.z = topBVH.BoundsMax.Z;

        float thit;
        if(ray_box_test(lo, hi, calculate_idir(ray.get_direction()), ray.get_origin(), ray.get_tmin(), ray.get_tmax(), thit)) {
            if (current_treelet_root == addrToTreeletID(topRootAddr + device_offset))
                current_treelet_stack.push_back(StackEntry(topRootAddr, true, false));
            else
                other_treelet_stack.push_back(StackEntry(topRootAddr, true, false));
        }
    }

    while (!current_treelet_stack.empty() || !other_treelet_stack.empty()) 
    {
        StackEntry current_node;

        // If current_treelet_stack is empty, move the first entry in other_treelet_stack to current_treelet_stack
        if (current_treelet_stack.empty()) 
        {
            assert(!other_treelet_stack.empty());
            current_treelet_stack.push_front(other_treelet_stack.front());
            other_treelet_stack.pop_front() ;  
        }

        current_node = current_treelet_stack.front();
        current_treelet_stack.pop_front();

        if (current_node.topLevel && !current_node.leaf) // Top level internal node
        {
            struct GEN_RT_BVH_INTERNAL_NODE node;
            GEN_RT_BVH_INTERNAL_NODE_unpack(&node, current_node.addr);
            transactions.push_back(MemoryTransactionRecord((uint8_t*)((uint64_t)current_node.addr + device_offset), GEN_RT_BVH_INTERNAL_NODE_length * 4, TransactionType::BVH_INTERNAL_NODE));
            ctx->func_sim->g_rt_mem_access_type[static_cast<int>(TransactionType::BVH_INTERNAL_NODE)]++;
            total_nodes_accessed++;

            if (debugTraversal)
            {
                traversalFile << "traversing top level internal node " << (void *)current_node.addr;
                traversalFile << ", child offset = " << node.ChildOffset << ", node type = " << node.NodeType;
                traversalFile << ", child size = (" << node.ChildSize[0] << ", " << node.ChildSize[1] << ", " << node.ChildSize[2] << ", " << node.ChildSize[3] << ", " << node.ChildSize[4] << ", " << node.ChildSize[5] << ")";
                traversalFile << ", child type = (" << node.ChildType[0] << ", " << node.ChildType[1] << ", " << node.ChildType[2] << ", " << node.ChildType[3] << ", " << node.ChildType[4] << ", " << node.ChildType[5] << ")";
                traversalFile << std::endl;
            }

            bool child_hit[6];
            float thit[6];
            for(int i = 0; i < 6; i++)
            {
                if (node.ChildSize[i] > 0)
                {
                    float3 idir = calculate_idir(ray.get_direction()); //TODO: this works wierd if one of ray dimensions is 0
                    float3 lo, hi;
                    set_child_bounds(&node, i, &lo, &hi);

                    child_hit[i] = ray_box_test(lo, hi, idir, ray.get_origin(), ray.get_tmin(), ray.get_tmax(), thit[i]);
                    if(child_hit[i] && thit[i] >= min_thit)
                        child_hit[i] = false;

                    
                    if (debugTraversal)
                    {
                        if(child_hit[i])
                            traversalFile << "hit child number " << i << ", ";
                        else
                            traversalFile << "missed child number " << i << ", ";
                        traversalFile << "lo = (" << lo.x << ", " << lo.y << ", " << lo.z << "), ";
                        traversalFile << "hi = (" << hi.x << ", " << hi.y << ", " << hi.z << ")" << std::endl;
                    }
                }
                else
                    child_hit[i] = false;
            }

            uint8_t *child_addr = current_node.addr + (node.ChildOffset * 64);
            for(int i = 0; i < 6; i++)
            {
                if(child_hit[i])
                {
                    if (debugTraversal)
                    {
                        traversalFile << "add child node " << (void *)child_addr << ", child number " << i << ", type " << node.ChildType[i] << ", to stack" << std::endl;
                    }
                    if(node.ChildType[i] != NODE_TYPE_INTERNAL)
                    {
                        assert(node.ChildType[i] == NODE_TYPE_INSTANCE);
                        if (current_treelet_root == addrToTreeletID(child_addr + device_offset))
                            current_treelet_stack.push_front(StackEntry(child_addr, true, true));
                        else
                            other_treelet_stack.push_front(StackEntry(child_addr, true, true));
                        assert(tree_level_map.find(current_node.addr) != tree_level_map.end());
                        tree_level_map[child_addr] = tree_level_map[current_node.addr] + 1;
                    }
                    else
                    {
                        // the if(next_node_addr == NULL) in the original shouldn't happen
                        if (current_treelet_root == addrToTreeletID(child_addr + device_offset))
                            current_treelet_stack.push_front(StackEntry(child_addr, true, false));
                        else
                            other_treelet_stack.push_front(StackEntry(child_addr, true, false));
                        assert(tree_level_map.find(current_node.addr) != tree_level_map.end());
                        tree_level_map[child_addr] = tree_level_map[current_node.addr] + 1;
                    }
                }
                else
                {
                    if (debugTraversal)
                    {
                        traversalFile << "ignoring missed node " << (void *)child_addr << ", child number " << i << ", type " << node.ChildType[i] << std::endl;
                    }
                }
                child_addr += node.ChildSize[i] * 64;
            }

            if (debugTraversal)
            {
                traversalFile << std::endl;
            }
        }
        else if (current_node.topLevel && current_node.leaf) // Top level leaf node
        {
            uint8_t* leaf_addr = current_node.addr;

            GEN_RT_BVH_INSTANCE_LEAF instanceLeaf;
            GEN_RT_BVH_INSTANCE_LEAF_unpack(&instanceLeaf, leaf_addr);
            transactions.push_back(MemoryTransactionRecord((uint8_t*)((uint64_t)leaf_addr + device_offset), GEN_RT_BVH_INSTANCE_LEAF_length * 4, TransactionType::BVH_INSTANCE_LEAF));
            ctx->func_sim->g_rt_mem_access_type[static_cast<int>(TransactionType::BVH_INSTANCE_LEAF)]++;
            total_nodes_accessed++;


            if (debugTraversal)
            {
                traversalFile << "traversing top level leaf node " << (void *)leaf_addr << ", instanceID = " << instanceLeaf.InstanceID << ", BVHAddress = " << instanceLeaf.BVHAddress << ", ShaderIndex = " << instanceLeaf.ShaderIndex << std::endl;
            }


            float4x4 worldToObjectMatrix = instance_leaf_matrix_to_float4x4(&instanceLeaf.WorldToObjectm00);
            float4x4 objectToWorldMatrix = instance_leaf_matrix_to_float4x4(&instanceLeaf.ObjectToWorldm00);

            assert(instanceLeaf.BVHAddress != NULL);
            GEN_RT_BVH botLevelASAddr;
            GEN_RT_BVH_unpack(&botLevelASAddr, (uint8_t *)(leaf_addr + instanceLeaf.BVHAddress));
            transactions.push_back(MemoryTransactionRecord((uint8_t*)((uint64_t)leaf_addr + instanceLeaf.BVHAddress + device_offset), GEN_RT_BVH_length * 4, TransactionType::BVH_STRUCTURE));
            ctx->func_sim->g_rt_mem_access_type[static_cast<int>(TransactionType::BVH_STRUCTURE)]++;

            if (debugTraversal)
            {
                traversalFile << "bot level bvh " << (void *)(leaf_addr + instanceLeaf.BVHAddress) << ", RootNodeOffset = (" << botLevelASAddr.RootNodeOffset << std::endl;
            }

            float worldToObject_tMultiplier;
            Ray objectRay = make_transformed_ray(ray, worldToObjectMatrix, &worldToObject_tMultiplier);
            
            uint8_t * botLevelRootAddr ;
            botLevelRootAddr = ((uint8_t *)((uint64_t)leaf_addr + instanceLeaf.BVHAddress)) + botLevelASAddr.RootNodeOffset;
            if (current_treelet_root == addrToTreeletID(botLevelRootAddr + device_offset))
                current_treelet_stack.push_front(StackEntry(botLevelRootAddr, false, false, worldToObject_tMultiplier, instanceLeaf, worldToObjectMatrix, objectToWorldMatrix, objectRay));
            else
                other_treelet_stack.push_front(StackEntry(botLevelRootAddr, false, false, worldToObject_tMultiplier, instanceLeaf, worldToObjectMatrix, objectToWorldMatrix, objectRay));
            assert(tree_level_map.find(leaf_addr) != tree_level_map.end());
            tree_level_map[botLevelRootAddr] = tree_level_map[leaf_addr];

            if (debugTraversal)
            {
                traversalFile << "bot level root address = " << (void*)botLevelRootAddr << std::endl;
                traversalFile << "warped ray to object coordinates, origin = (" << objectRay.get_origin().x << ", " << objectRay.get_origin().y << ", " << objectRay.get_origin().z << "), ";
                traversalFile << "direction = (" << objectRay.get_direction().x << ", " << objectRay.get_direction().y << ", " << objectRay.get_direction().z << "), ";
                traversalFile << "tmin = " << objectRay.get_tmin() << ", tmax = " << objectRay.get_tmax() << std::endl << std::endl;
            }
        }
        else if (!current_node.topLevel && !current_node.leaf) // Bottom level internal node
        {
            uint8_t* next_node_addr = current_node.addr;
            uint8_t* node_addr = next_node_addr;

            struct GEN_RT_BVH_INTERNAL_NODE node;
            GEN_RT_BVH_INTERNAL_NODE_unpack(&node, node_addr);
            transactions.push_back(MemoryTransactionRecord((uint8_t*)((uint64_t)node_addr + device_offset), GEN_RT_BVH_INTERNAL_NODE_length * 4, TransactionType::BVH_INTERNAL_NODE));
            ctx->func_sim->g_rt_mem_access_type[static_cast<int>(TransactionType::BVH_INTERNAL_NODE)]++;
            total_nodes_accessed++;

            if (debugTraversal)
            {
                traversalFile << "traversing bot level internal node " << (void *)node_addr;
                traversalFile << ", child offset = " << node.ChildOffset << ", node type = " << node.NodeType;
                traversalFile << ", child size = (" << node.ChildSize[0] << ", " << node.ChildSize[1] << ", " << node.ChildSize[2] << ", " << node.ChildSize[3] << ", " << node.ChildSize[4] << ", " << node.ChildSize[5] << ")";
                traversalFile << ", child type = (" << node.ChildType[0] << ", " << node.ChildType[1] << ", " << node.ChildType[2] << ", " << node.ChildType[3] << ", " << node.ChildType[4] << ", " << node.ChildType[5] << ")";
                traversalFile << std::endl;
            }

            bool child_hit[6];
            float thit[6];
            for(int i = 0; i < 6; i++)
            {
                if (node.ChildSize[i] > 0)
                {
                    float3 idir = calculate_idir(current_node.objectRay.get_direction()); //TODO: this works wierd if one of ray dimensions is 0
                    float3 lo, hi;
                    set_child_bounds(&node, i, &lo, &hi);

                    child_hit[i] = ray_box_test(lo, hi, idir, current_node.objectRay.get_origin(), current_node.objectRay.get_tmin(), current_node.objectRay.get_tmax(), thit[i]);
                    if(child_hit[i] && thit[i] >= min_thit * current_node.worldToObject_tMultiplier)
                        child_hit[i] = false;

                    if (debugTraversal)
                    {
                        if(child_hit[i])
                            traversalFile << "hit child number " << i << ", ";
                        else
                            traversalFile << "missed child number " << i << ", ";
                        traversalFile << "lo = (" << lo.x << ", " << lo.y << ", " << lo.z << "), ";
                        traversalFile << "hi = (" << hi.x << ", " << hi.y << ", " << hi.z << ")" << std::endl;
                    }
                }
                else
                    child_hit[i] = false;
            }

            uint8_t *child_addr = node_addr + (node.ChildOffset * 64);
            for(int i = 0; i < 6; i++)
            {
                if(child_hit[i])
                {
                    if (debugTraversal)
                    {
                        traversalFile << "add child node " << (void *)child_addr << ", child number " << i << ", type " << node.ChildType[i] << ", to stack" << std::endl;
                    }

                    if(node.ChildType[i] != NODE_TYPE_INTERNAL)
                    {
                        if (current_treelet_root == addrToTreeletID(child_addr + device_offset))
                            current_treelet_stack.push_front(StackEntry(child_addr, false, true, current_node.worldToObject_tMultiplier, current_node.instanceLeaf, current_node.worldToObjectMatrix, current_node.objectToWorldMatrix, current_node.objectRay));
                        else
                            other_treelet_stack.push_front(StackEntry(child_addr, false, true, current_node.worldToObject_tMultiplier, current_node.instanceLeaf, current_node.worldToObjectMatrix, current_node.objectToWorldMatrix, current_node.objectRay));
                        assert(tree_level_map.find(node_addr) != tree_level_map.end());
                        tree_level_map[child_addr] = tree_level_map[node_addr] + 1;
                    }
                    else
                    {
                        // Don' think if(next_node_addr == 0) will happen
                        if (current_treelet_root == addrToTreeletID(child_addr + device_offset))
                            current_treelet_stack.push_front(StackEntry(child_addr, false, false, current_node.worldToObject_tMultiplier, current_node.instanceLeaf, current_node.worldToObjectMatrix, current_node.objectToWorldMatrix, current_node.objectRay));
                        else
                            other_treelet_stack.push_front(StackEntry(child_addr, false, false, current_node.worldToObject_tMultiplier, current_node.instanceLeaf, current_node.worldToObjectMatrix, current_node.objectToWorldMatrix, current_node.objectRay));
                        assert(tree_level_map.find(node_addr) != tree_level_map.end());
                        tree_level_map[child_addr] = tree_level_map[node_addr] + 1;
                    }
                }
                else
                {
                    if (debugTraversal)
                    {
                        traversalFile << "ignoring missed node " << (void *)child_addr << ", child number " << i << ", type " << node.ChildType[i] << std::endl;
                    }
                }
                child_addr += node.ChildSize[i] * 64;
            }

            if (debugTraversal)
            {
                traversalFile << std::endl;
            }
        }
        else if (!current_node.topLevel && current_node.leaf) // Bottom level leaf node
        {
            uint8_t* leaf_addr = current_node.addr;

            struct GEN_RT_BVH_PRIMITIVE_LEAF_DESCRIPTOR leaf_descriptor;
            GEN_RT_BVH_PRIMITIVE_LEAF_DESCRIPTOR_unpack(&leaf_descriptor, leaf_addr);
            transactions.push_back(MemoryTransactionRecord((uint8_t*)((uint64_t)leaf_addr + device_offset), GEN_RT_BVH_PRIMITIVE_LEAF_DESCRIPTOR_length * 4, TransactionType::BVH_PRIMITIVE_LEAF_DESCRIPTOR));
            ctx->func_sim->g_rt_mem_access_type[static_cast<int>(TransactionType::BVH_PRIMITIVE_LEAF_DESCRIPTOR)]++;

            if (leaf_descriptor.LeafType == TYPE_QUAD)
            {
                struct GEN_RT_BVH_QUAD_LEAF leaf;
                GEN_RT_BVH_QUAD_LEAF_unpack(&leaf, leaf_addr);

                // if(leaf.PrimitiveIndex0 == 9600)
                // {
                //     leaf.QuadVertex[2].Z = -0.001213;
                // }

                float3 p[3];
                for(int i = 0; i < 3; i++)
                {
                    p[i].x = leaf.QuadVertex[i].X;
                    p[i].y = leaf.QuadVertex[i].Y;
                    p[i].z = leaf.QuadVertex[i].Z;
                }

                // Triangle intersection algorithm
                float thit;
                bool hit = VulkanRayTracing::mt_ray_triangle_test(p[0], p[1], p[2], current_node.objectRay, &thit);

                assert(leaf.PrimitiveIndex1Delta == 0);

                if (debugTraversal)
                {
                    if(hit)
                        traversalFile << "hit quad node " << (void *)leaf_addr << " with thit " << thit << " ";
                    else
                        traversalFile << "miss quad node " << leaf_addr << " ";
                    traversalFile << "primitiveID = " << leaf.PrimitiveIndex0 << ", InstanceID = " << current_node.instanceLeaf.InstanceID << "\n";

                    traversalFile << "p[0] = (" << p[0].x << ", " << p[0].y << ", " << p[0].z << ") ";
                    traversalFile << "p[1] = (" << p[1].x << ", " << p[1].y << ", " << p[1].z << ") ";
                    traversalFile << "p[2] = (" << p[2].x << ", " << p[2].y << ", " << p[2].z << ") ";
                    traversalFile << "p[3] = (" << p[3].x << ", " << p[3].y << ", " << p[3].z << ")" << std::endl;
                }

                float world_thit = thit / current_node.worldToObject_tMultiplier;

                //TODO: why the Tmin Tmax consition wasn't handled in the object coordinates?
                if(hit && Tmin <= world_thit && world_thit <= Tmax && world_thit < min_thit)
                {
                    if (debugTraversal)
                    {
                        traversalFile << "quad node " << (void *)leaf_addr << ", primitiveID " << leaf.PrimitiveIndex0 << " is the closest hit. world_thit " << thit / current_node.worldToObject_tMultiplier;
                    }

                    min_thit = thit / current_node.worldToObject_tMultiplier;
                    min_thit_object = thit;
                    closest_leaf = leaf;
                    closest_instanceLeaf = current_node.instanceLeaf;
                    closest_worldToObject = current_node.worldToObjectMatrix;
                    closest_objectToWorld = current_node.objectToWorldMatrix;
                    closest_objectRay = current_node.objectRay;
                    min_thit_object = thit;
                    thread->add_ray_intersect();
                    transactions.push_back(MemoryTransactionRecord((uint8_t*)((uint64_t)leaf_addr + device_offset), GEN_RT_BVH_QUAD_LEAF_length * 4, TransactionType::BVH_QUAD_LEAF_HIT));
                    ctx->func_sim->g_rt_mem_access_type[static_cast<int>(TransactionType::BVH_QUAD_LEAF_HIT)]++;
                    total_nodes_accessed++;

                    if(terminateOnFirstHit)
                    {
                        current_treelet_stack.clear();
                        other_treelet_stack.clear();
                    }
                }
                else {
                    transactions.push_back(MemoryTransactionRecord((uint8_t*)((uint64_t)leaf_addr + device_offset), GEN_RT_BVH_QUAD_LEAF_length * 4, TransactionType::BVH_QUAD_LEAF));
                    ctx->func_sim->g_rt_mem_access_type[static_cast<int>(TransactionType::BVH_QUAD_LEAF)]++;
                    total_nodes_accessed++;
                }
                if (debugTraversal)
                {
                    traversalFile << std::endl;
                }
            }
            else
            {
                struct GEN_RT_BVH_PROCEDURAL_LEAF leaf;
                GEN_RT_BVH_PROCEDURAL_LEAF_unpack(&leaf, leaf_addr);
                transactions.push_back(MemoryTransactionRecord((uint8_t*)((uint64_t)leaf_addr + device_offset), GEN_RT_BVH_PROCEDURAL_LEAF_length * 4, TransactionType::BVH_PROCEDURAL_LEAF));
                ctx->func_sim->g_rt_mem_access_type[static_cast<int>(TransactionType::BVH_PROCEDURAL_LEAF)]++;
                total_nodes_accessed++;

                uint32_t hit_group_index = current_node.instanceLeaf.InstanceContributionToHitGroupIndex;

                warp_intersection_table* table = intersection_table[thread->get_ctaid().x][thread->get_ctaid().y];
                auto intersectionTransactions = table->add_intersection(hit_group_index, thread->get_tid().x, leaf.PrimitiveIndex[0], current_node.instanceLeaf.InstanceID, pI, thread); // TODO: switch these to device addresses
                
                // transactions.insert(transactions.end(), intersectionTransactions.first.begin(), intersectionTransactions.first.end());
                for(auto & newTransaction : intersectionTransactions.first)
                {
                    bool found = false;
                    for(auto & transaction : transactions)
                        if(transaction.address == newTransaction.address)
                        {
                            found = true;
                            break;
                        }
                    if(!found)
                        transactions.push_back(newTransaction);

                }
                store_transactions.insert(store_transactions.end(), intersectionTransactions.second.begin(), intersectionTransactions.second.end());
            }
        }
        else
        {
            assert(0); // shouldn't happen
        }
    }

    if (min_thit < ray.dir_tmax.w)
    {
        traversal_data.hit_geometry = true;
        ctx->func_sim->g_rt_num_hits++;
        traversal_data.closest_hit.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
        traversal_data.closest_hit.geometry_index = closest_leaf.LeafDescriptor.GeometryIndex;
        traversal_data.closest_hit.primitive_index = closest_leaf.PrimitiveIndex0;
        traversal_data.closest_hit.instance_index = closest_instanceLeaf.InstanceID;
        float3 intersection_point = ray.get_origin() + make_float3(ray.get_direction().x * min_thit, ray.get_direction().y * min_thit, ray.get_direction().z * min_thit);
        float3 rayatinter = ray.at(min_thit);
        // assert(intersection_point.x == ray.at(min_thit).x && intersection_point.y == ray.at(min_thit).y && intersection_point.z == ray.at(min_thit).z);
        traversal_data.closest_hit.intersection_point = intersection_point;
        traversal_data.closest_hit.worldToObjectMatrix = closest_worldToObject;
        traversal_data.closest_hit.objectToWorldMatrix = closest_objectToWorld;
        traversal_data.closest_hit.world_min_thit = min_thit;

        float3 p[3];
        for(int i = 0; i < 3; i++)
        {
            p[i].x = closest_leaf.QuadVertex[i].X;
            p[i].y = closest_leaf.QuadVertex[i].Y;
            p[i].z = closest_leaf.QuadVertex[i].Z;
        }
        float3 object_intersection_point = closest_objectRay.get_origin() + make_float3(closest_objectRay.get_direction().x * min_thit_object, closest_objectRay.get_direction().y * min_thit_object, closest_objectRay.get_direction().z * min_thit_object);
        //closest_objectRay.at(min_thit_object);
        float3 barycentric = Barycentric(object_intersection_point, p[0], p[1], p[2]);
        traversal_data.closest_hit.barycentric_coordinates = barycentric;
        thread->RT_thread_data->set_hitAttribute(barycentric, pI, thread);

        // store_transactions.push_back(MemoryStoreTransactionRecord(&traversal_data, sizeof(traversal_data), StoreTransactionType::Traversal_Results));
    }
    else
    {
        traversal_data.hit_geometry = false;
    }

    memory_space *mem = thread->get_global_memory();
    Traversal_data* device_traversal_data = (Traversal_data*) VulkanRayTracing::gpgpusim_alloc(sizeof(Traversal_data));
    mem->write(device_traversal_data, sizeof(Traversal_data), &traversal_data, thread, pI);
    thread->RT_thread_data->traversal_data.push_back(device_traversal_data);
    
    thread->set_rt_transactions(transactions);
    thread->set_rt_store_transactions(store_transactions);

    // rayCount++; // ray id starts from 1
    printf("RayID,%d", rayCount);
    for (auto transaction : transactions)
    {
        printf(",0x%x", VulkanRayTracing::addrToTreeletID((uint8_t*)transaction.address));
        accessedDataSize += transaction.size;
    }
    printf("\n");

    if (debugTraversal)
    {
        traversalFile.close();
    }

    if (total_nodes_accessed > ctx->func_sim->g_max_nodes_per_ray) {
        ctx->func_sim->g_max_nodes_per_ray = total_nodes_accessed;
    }
    ctx->func_sim->g_tot_nodes_per_ray += total_nodes_accessed;

    unsigned level = 0;
    for (auto it=tree_level_map.begin(); it!=tree_level_map.end(); it++) {
        if (it->second > level) {
            level = it->second;
        }
    }
    if (level > ctx->func_sim->g_max_tree_depth) {
        ctx->func_sim->g_max_tree_depth = level;
    }

    // Print out the transactions
    std::ofstream memoryTransactionsFile;

    if (false)
    {
        memoryTransactionsFile.open("memorytransactions.txt", std::ios_base::app);
        memoryTransactionsFile << "m_hw_tid:" << thread->get_hw_tid() << std::endl;
        memoryTransactionsFile << "Cycle:" << GPGPU_Context()->the_gpgpusim->g_the_gpu->gpu_sim_cycle << std::endl;

        for(auto item : transactions)
        {
            memoryTransactionsFile << (void *)item.address << "," << item.size << "," << (int)item.type << std::endl;
        }

        memoryTransactionsFile << std::endl;
    }
}

void VulkanRayTracing::traceRay(VkAccelerationStructureKHR _topLevelAS,
				   uint rayFlags,
                   uint cullMask,
                   uint sbtRecordOffset,
                   uint sbtRecordStride,
                   uint missIndex,
                   float3 origin,
                   float Tmin,
                   float3 direction,
                   float Tmax,
                   int payload,
                   const ptx_instruction *pI,
                   ptx_thread_info *thread)
{
    // printf("## calling trceRay function. rayFlags = %d, cullMask = %d, sbtRecordOffset = %d, sbtRecordStride = %d, missIndex = %d, origin = (%f, %f, %f), Tmin = %f, direction = (%f, %f, %f), Tmax = %f, payload = %d\n",
    //         rayFlags, cullMask, sbtRecordOffset, sbtRecordStride, missIndex, origin.x, origin.y, origin.z, Tmin, direction.x, direction.y, direction.z, Tmax, payload);

    if (dump_trace && !dumped) 
    {
        dump_AS(VulkanRayTracing::descriptorSet, _topLevelAS);
        std::cout << "Trace dumped" << std::endl;
        dumped = true;
    }

    // Convert device address back to host address for func sim. This will break if the device address was modified then passed to traceRay. Should be fixable if I also record the size when I malloc then I can check the bounds of the device address.
    uint8_t* deviceAddress = nullptr;
    int64_t device_offset = 0;
    if (use_external_launcher)
    {
        deviceAddress = (uint8_t*)_topLevelAS;
        bool addressFound = false;
        for (int i = 0; i < MAX_DESCRIPTOR_SETS; i++)
        {
            for (int j = 0; j < MAX_DESCRIPTOR_SET_BINDINGS; j++)
            {
                if (launcher_deviceDescriptorSets[i][j] == (void*)_topLevelAS)
                {
                    _topLevelAS = launcher_descriptorSets[i][j];
                    addressFound = true;
                    break;
                }
            }
            if (addressFound)
                break;
        }
        if (!addressFound)
            abort();
    
        // Calculate offset between host and device for memory transactions
        device_offset = (uint64_t)deviceAddress - (uint64_t)_topLevelAS;
    }


    // Form Treelets
    if (!treeletsFormed)
    {
        createTreelets(_topLevelAS, device_offset, GPGPU_Context()->the_gpgpusim->g_the_gpu->get_config().max_treelet_size); // 48*1024 aila2010 paper
        treeletsFormed = true;
    }

    //int result = isTreeletRoot((uint8_t*)_topLevelAS); // test

    // if(!found_AS)
    // {
    //     found_AS = true;
    //     topLevelAS_first = _topLevelAS;
    //     print_tree.open("bvh_tree.txt");
    //     traverse_tree((uint8_t*)_topLevelAS);
    //     print_tree.close();
    // }
    // else
    // {
    //     assert(topLevelAS_first != NULL);
    //     assert(topLevelAS_first == _topLevelAS);
    // }

    Traversal_data traversal_data;

    traversal_data.ray_world_direction = direction;
    traversal_data.ray_world_origin = origin;
    traversal_data.sbtRecordOffset = sbtRecordOffset;
    traversal_data.sbtRecordStride = sbtRecordStride;
    traversal_data.missIndex = missIndex;
    traversal_data.Tmin = Tmin;
    traversal_data.Tmax = Tmax;

    std::ofstream traversalFile;

    if (debugTraversal)
    {
        traversalFile.open("traversal.txt");
        traversalFile << "starting traversal\n";
        traversalFile << "origin = (" << origin.x << ", " << origin.y << ", " << origin.z << "), ";
        traversalFile << "direction = (" << direction.x << ", " << direction.y << ", " << direction.z << "), ";
        traversalFile << "tmin = " << Tmin << ", tmax = " << Tmax << std::endl << std::endl;
    }


    bool terminateOnFirstHit = rayFlags & SpvRayFlagsTerminateOnFirstHitKHRMask;
    bool skipClosestHitShader = rayFlags & SpvRayFlagsSkipClosestHitShaderKHRMask;

    std::vector<MemoryTransactionRecord> transactions;
    std::vector<MemoryStoreTransactionRecord> store_transactions;

    gpgpu_context *ctx = GPGPU_Context();

    if (terminateOnFirstHit) ctx->func_sim->g_n_anyhit_rays++;
    else ctx->func_sim->g_n_closesthit_rays++;

    unsigned total_nodes_accessed = 0;
    std::map<uint8_t*, unsigned> tree_level_map;
    
	// Create ray
	Ray ray;
	ray.make_ray(origin, direction, Tmin, Tmax);
    thread->add_ray_properties(ray);

	// Set thit to max
    float min_thit = ray.dir_tmax.w;
    struct GEN_RT_BVH_QUAD_LEAF closest_leaf;
    struct GEN_RT_BVH_INSTANCE_LEAF closest_instanceLeaf;    
    float4x4 closest_worldToObject, closest_objectToWorld;
    Ray closest_objectRay;
    float min_thit_object;

	// Get bottom-level AS
    //uint8_t* topLevelASAddr = get_anv_accel_address((VkAccelerationStructureKHR)_topLevelAS);
    GEN_RT_BVH topBVH; //TODO: test hit with world before traversal
    GEN_RT_BVH_unpack(&topBVH, (uint8_t*)_topLevelAS);
    transactions.push_back(MemoryTransactionRecord((uint8_t*)((uint64_t)_topLevelAS + device_offset), GEN_RT_BVH_length * 4, TransactionType::BVH_STRUCTURE));
    ctx->func_sim->g_rt_mem_access_type[static_cast<int>(TransactionType::BVH_STRUCTURE)]++;
    
    uint8_t* topRootAddr = (uint8_t*)_topLevelAS + topBVH.RootNodeOffset;

    // Get min/max
    if (!ctx->func_sim->g_rt_world_set) {
        struct GEN_RT_BVH_INTERNAL_NODE node;
        GEN_RT_BVH_INTERNAL_NODE_unpack(&node, topRootAddr);
        for(int i = 0; i < 6; i++) {
            if (node.ChildSize[i] > 0) {
                float3 idir = calculate_idir(ray.get_direction()); //TODO: this works wierd if one of ray dimensions is 0
                float3 lo, hi;
                set_child_bounds(&node, i, &lo, &hi);
                ctx->func_sim->g_rt_world_min = min(ctx->func_sim->g_rt_world_min, lo);
                ctx->func_sim->g_rt_world_max = min(ctx->func_sim->g_rt_world_max, hi);
            }
        }
        ctx->func_sim->g_rt_world_set = true;
    }

    std::list<StackEntry> stack;
    tree_level_map[topRootAddr] = 1;
    
    {
        float3 lo, hi;
        lo.x = topBVH.BoundsMin.X;
        lo.y = topBVH.BoundsMin.Y;
        lo.z = topBVH.BoundsMin.Z;
        hi.x = topBVH.BoundsMax.X;
        hi.y = topBVH.BoundsMax.Y;
        hi.z = topBVH.BoundsMax.Z;

        float thit;
        if(ray_box_test(lo, hi, calculate_idir(ray.get_direction()), ray.get_origin(), ray.get_tmin(), ray.get_tmax(), thit))
            stack.push_back(StackEntry(topRootAddr, true, false));
    }

    while (!stack.empty())
    {
        uint8_t *node_addr = NULL;
        uint8_t *next_node_addr = NULL;

        // traverse top level internal nodes
        assert(stack.back().topLevel);
        
        if(!stack.back().leaf)
        {
            next_node_addr = stack.back().addr;
            stack.pop_back();
        }

        while (next_node_addr > 0)
        {
            node_addr = next_node_addr;
            next_node_addr = NULL;
            struct GEN_RT_BVH_INTERNAL_NODE node;
            GEN_RT_BVH_INTERNAL_NODE_unpack(&node, node_addr);
            transactions.push_back(MemoryTransactionRecord((uint8_t*)((uint64_t)node_addr + device_offset), GEN_RT_BVH_INTERNAL_NODE_length * 4, TransactionType::BVH_INTERNAL_NODE));
            ctx->func_sim->g_rt_mem_access_type[static_cast<int>(TransactionType::BVH_INTERNAL_NODE)]++;
            total_nodes_accessed++;

            if (debugTraversal)
            {
                traversalFile << "traversing top level internal node " << (void *)node_addr;
                traversalFile << ", child offset = " << node.ChildOffset << ", node type = " << node.NodeType;
                traversalFile << ", child size = (" << node.ChildSize[0] << ", " << node.ChildSize[1] << ", " << node.ChildSize[2] << ", " << node.ChildSize[3] << ", " << node.ChildSize[4] << ", " << node.ChildSize[5] << ")";
                traversalFile << ", child type = (" << node.ChildType[0] << ", " << node.ChildType[1] << ", " << node.ChildType[2] << ", " << node.ChildType[3] << ", " << node.ChildType[4] << ", " << node.ChildType[5] << ")";
                traversalFile << std::endl;
            }

            bool child_hit[6];
            float thit[6];
            for(int i = 0; i < 6; i++)
            {
                if (node.ChildSize[i] > 0)
                {
                    float3 idir = calculate_idir(ray.get_direction()); //TODO: this works wierd if one of ray dimensions is 0
                    float3 lo, hi;
                    set_child_bounds(&node, i, &lo, &hi);

                    child_hit[i] = ray_box_test(lo, hi, idir, ray.get_origin(), ray.get_tmin(), ray.get_tmax(), thit[i]);
                    if(child_hit[i] && thit[i] >= min_thit)
                        child_hit[i] = false;

                    
                    if (debugTraversal)
                    {
                        if(child_hit[i])
                            traversalFile << "hit child number " << i << ", ";
                        else
                            traversalFile << "missed child number " << i << ", ";
                        traversalFile << "lo = (" << lo.x << ", " << lo.y << ", " << lo.z << "), ";
                        traversalFile << "hi = (" << hi.x << ", " << hi.y << ", " << hi.z << ")" << std::endl;
                    }
                }
                else
                    child_hit[i] = false;
            }

            uint8_t *child_addr = node_addr + (node.ChildOffset * 64);
            for(int i = 0; i < 6; i++)
            {
                if(child_hit[i])
                {
                    if (debugTraversal)
                    {
                        traversalFile << "add child node " << (void *)child_addr << ", child number " << i << ", type " << node.ChildType[i] << ", to stack" << std::endl;
                    }
                    if(node.ChildType[i] != NODE_TYPE_INTERNAL)
                    {
                        assert(node.ChildType[i] == NODE_TYPE_INSTANCE);
                        stack.push_back(StackEntry(child_addr, true, true));
                        assert(tree_level_map.find(node_addr) != tree_level_map.end());
                        tree_level_map[child_addr] = tree_level_map[node_addr] + 1;
                    }
                    else
                    {
                        if(next_node_addr == NULL) {
                            next_node_addr = child_addr; // TODO: sort by thit
                            assert(tree_level_map.find(node_addr) != tree_level_map.end());
                            tree_level_map[child_addr] = tree_level_map[node_addr] + 1;
                        }
                        else {
                            stack.push_back(StackEntry(child_addr, true, false));
                            assert(tree_level_map.find(node_addr) != tree_level_map.end());
                            tree_level_map[child_addr] = tree_level_map[node_addr] + 1;
                        }
                    }
                }
                else
                {
                    if (debugTraversal)
                    {
                        traversalFile << "ignoring missed node " << (void *)child_addr << ", child number " << i << ", type " << node.ChildType[i] << std::endl;
                    }
                }
                child_addr += node.ChildSize[i] * 64;
            }

            if (debugTraversal)
            {
                traversalFile << std::endl;
            }
        }

        // traverse top level leaf nodes
        while (!stack.empty() && stack.back().leaf)
        {
            assert(stack.back().topLevel);

            uint8_t* leaf_addr = stack.back().addr;
            stack.pop_back();

            GEN_RT_BVH_INSTANCE_LEAF instanceLeaf;
            GEN_RT_BVH_INSTANCE_LEAF_unpack(&instanceLeaf, leaf_addr);
            transactions.push_back(MemoryTransactionRecord((uint8_t*)((uint64_t)leaf_addr + device_offset), GEN_RT_BVH_INSTANCE_LEAF_length * 4, TransactionType::BVH_INSTANCE_LEAF));
            ctx->func_sim->g_rt_mem_access_type[static_cast<int>(TransactionType::BVH_INSTANCE_LEAF)]++;
            total_nodes_accessed++;


            if (debugTraversal)
            {
                traversalFile << "traversing top level leaf node " << (void *)leaf_addr << ", instanceID = " << instanceLeaf.InstanceID << ", BVHAddress = " << instanceLeaf.BVHAddress << ", ShaderIndex = " << instanceLeaf.ShaderIndex << std::endl;
            }


            float4x4 worldToObjectMatrix = instance_leaf_matrix_to_float4x4(&instanceLeaf.WorldToObjectm00);
            float4x4 objectToWorldMatrix = instance_leaf_matrix_to_float4x4(&instanceLeaf.ObjectToWorldm00);

            assert(instanceLeaf.BVHAddress != NULL);
            GEN_RT_BVH botLevelASAddr;
            GEN_RT_BVH_unpack(&botLevelASAddr, (uint8_t *)(leaf_addr + instanceLeaf.BVHAddress));
            transactions.push_back(MemoryTransactionRecord((uint8_t*)((uint64_t)leaf_addr + instanceLeaf.BVHAddress + device_offset), GEN_RT_BVH_length * 4, TransactionType::BVH_STRUCTURE));
            ctx->func_sim->g_rt_mem_access_type[static_cast<int>(TransactionType::BVH_STRUCTURE)]++;

            if (debugTraversal)
            {
                traversalFile << "bot level bvh " << (void *)(leaf_addr + instanceLeaf.BVHAddress) << ", RootNodeOffset = (" << botLevelASAddr.RootNodeOffset << std::endl;
            }

            // std::ofstream offsetfile;
            // offsetfile.open("offsets.txt", std::ios::app);
            // offsetfile << (int64_t)instanceLeaf.BVHAddress << std::endl;

            // std::ofstream leaf_addr_file;
            // leaf_addr_file.open("leaf.txt", std::ios::app);
            // leaf_addr_file << (int64_t)((uint64_t)leaf_addr - (uint64_t)_topLevelAS) << std::endl;

            float worldToObject_tMultiplier;
            Ray objectRay = make_transformed_ray(ray, worldToObjectMatrix, &worldToObject_tMultiplier);
            
            uint8_t * botLevelRootAddr ;
            botLevelRootAddr = ((uint8_t *)((uint64_t)leaf_addr + instanceLeaf.BVHAddress)) + botLevelASAddr.RootNodeOffset;
            stack.push_back(StackEntry(botLevelRootAddr, false, false));
            assert(tree_level_map.find(leaf_addr) != tree_level_map.end());
            tree_level_map[botLevelRootAddr] = tree_level_map[leaf_addr];

            if (debugTraversal)
            {
                traversalFile << "bot level root address = " << (void*)botLevelRootAddr << std::endl;
                traversalFile << "warped ray to object coordinates, origin = (" << objectRay.get_origin().x << ", " << objectRay.get_origin().y << ", " << objectRay.get_origin().z << "), ";
                traversalFile << "direction = (" << objectRay.get_direction().x << ", " << objectRay.get_direction().y << ", " << objectRay.get_direction().z << "), ";
                traversalFile << "tmin = " << objectRay.get_tmin() << ", tmax = " << objectRay.get_tmax() << std::endl << std::endl;
            }

            // traverse bottom level tree
            while (!stack.empty() && !stack.back().topLevel)
            {
                uint8_t* node_addr = NULL;
                uint8_t* next_node_addr = stack.back().addr;
                stack.pop_back();
                

                // traverse bottom level internal nodes
                while (next_node_addr > 0)
                {
                    node_addr = next_node_addr;
                    next_node_addr = NULL;

                    // if(node_addr == *(++path.rbegin()))
                    //     printf("this is where things go wrong\n");

                    struct GEN_RT_BVH_INTERNAL_NODE node;
                    GEN_RT_BVH_INTERNAL_NODE_unpack(&node, node_addr);
                    transactions.push_back(MemoryTransactionRecord((uint8_t*)((uint64_t)node_addr + device_offset), GEN_RT_BVH_INTERNAL_NODE_length * 4, TransactionType::BVH_INTERNAL_NODE));
                    ctx->func_sim->g_rt_mem_access_type[static_cast<int>(TransactionType::BVH_INTERNAL_NODE)]++;
                    total_nodes_accessed++;

                    if (debugTraversal)
                    {
                        traversalFile << "traversing bot level internal node " << (void *)node_addr;
                        traversalFile << ", child offset = " << node.ChildOffset << ", node type = " << node.NodeType;
                        traversalFile << ", child size = (" << node.ChildSize[0] << ", " << node.ChildSize[1] << ", " << node.ChildSize[2] << ", " << node.ChildSize[3] << ", " << node.ChildSize[4] << ", " << node.ChildSize[5] << ")";
                        traversalFile << ", child type = (" << node.ChildType[0] << ", " << node.ChildType[1] << ", " << node.ChildType[2] << ", " << node.ChildType[3] << ", " << node.ChildType[4] << ", " << node.ChildType[5] << ")";
                        traversalFile << std::endl;
                    }

                    bool child_hit[6];
                    float thit[6];
                    for(int i = 0; i < 6; i++)
                    {
                        if (node.ChildSize[i] > 0)
                        {
                            float3 idir = calculate_idir(objectRay.get_direction()); //TODO: this works wierd if one of ray dimensions is 0
                            float3 lo, hi;
                            set_child_bounds(&node, i, &lo, &hi);

                            child_hit[i] = ray_box_test(lo, hi, idir, objectRay.get_origin(), objectRay.get_tmin(), objectRay.get_tmax(), thit[i]);
                            if(child_hit[i] && thit[i] >= min_thit * worldToObject_tMultiplier)
                                child_hit[i] = false;

                            if (debugTraversal)
                            {
                                if(child_hit[i])
                                    traversalFile << "hit child number " << i << ", ";
                                else
                                    traversalFile << "missed child number " << i << ", ";
                                traversalFile << "lo = (" << lo.x << ", " << lo.y << ", " << lo.z << "), ";
                                traversalFile << "hi = (" << hi.x << ", " << hi.y << ", " << hi.z << ")" << std::endl;
                            }
                        }
                        else
                            child_hit[i] = false;
                    }

                    uint8_t *child_addr = node_addr + (node.ChildOffset * 64);
                    for(int i = 0; i < 6; i++)
                    {
                        if(child_hit[i])
                        {
                            if (debugTraversal)
                            {
                                traversalFile << "add child node " << (void *)child_addr << ", child number " << i << ", type " << node.ChildType[i] << ", to stack" << std::endl;
                            }

                            if(node.ChildType[i] != NODE_TYPE_INTERNAL)
                            {
                                stack.push_back(StackEntry(child_addr, false, true));
                                assert(tree_level_map.find(node_addr) != tree_level_map.end());
                                tree_level_map[child_addr] = tree_level_map[node_addr] + 1;
                            }
                            else
                            {
                                if(next_node_addr == 0) {
                                    next_node_addr = child_addr; // TODO: sort by thit
                                    assert(tree_level_map.find(node_addr) != tree_level_map.end());
                                    tree_level_map[child_addr] = tree_level_map[node_addr] + 1;
                                }
                                else {
                                    stack.push_back(StackEntry(child_addr, false, false));
                                    assert(tree_level_map.find(node_addr) != tree_level_map.end());
                                    tree_level_map[child_addr] = tree_level_map[node_addr] + 1;
                                }
                            }
                        }
                        else
                        {
                            if (debugTraversal)
                            {
                                traversalFile << "ignoring missed node " << (void *)child_addr << ", child number " << i << ", type " << node.ChildType[i] << std::endl;
                            }
                        }
                        child_addr += node.ChildSize[i] * 64;
                    }

                    if (debugTraversal)
                    {
                        traversalFile << std::endl;
                    }
                }

                // traverse bottom level leaf nodes
                while(!stack.empty() && !stack.back().topLevel && stack.back().leaf)
                {
                    uint8_t* leaf_addr = stack.back().addr;
                    stack.pop_back();
                    struct GEN_RT_BVH_PRIMITIVE_LEAF_DESCRIPTOR leaf_descriptor;
                    GEN_RT_BVH_PRIMITIVE_LEAF_DESCRIPTOR_unpack(&leaf_descriptor, leaf_addr);
                    transactions.push_back(MemoryTransactionRecord((uint8_t*)((uint64_t)leaf_addr + device_offset), GEN_RT_BVH_PRIMITIVE_LEAF_DESCRIPTOR_length * 4, TransactionType::BVH_PRIMITIVE_LEAF_DESCRIPTOR));
                    ctx->func_sim->g_rt_mem_access_type[static_cast<int>(TransactionType::BVH_PRIMITIVE_LEAF_DESCRIPTOR)]++;

                    if (leaf_descriptor.LeafType == TYPE_QUAD)
                    {
                        struct GEN_RT_BVH_QUAD_LEAF leaf;
                        GEN_RT_BVH_QUAD_LEAF_unpack(&leaf, leaf_addr);

                        // if(leaf.PrimitiveIndex0 == 9600)
                        // {
                        //     leaf.QuadVertex[2].Z = -0.001213;
                        // }

                        float3 p[3];
                        for(int i = 0; i < 3; i++)
                        {
                            p[i].x = leaf.QuadVertex[i].X;
                            p[i].y = leaf.QuadVertex[i].Y;
                            p[i].z = leaf.QuadVertex[i].Z;
                        }

                        // Triangle intersection algorithm
                        float thit;
                        bool hit = VulkanRayTracing::mt_ray_triangle_test(p[0], p[1], p[2], objectRay, &thit);

                        assert(leaf.PrimitiveIndex1Delta == 0);

                        if (debugTraversal)
                        {
                            if(hit)
                                traversalFile << "hit quad node " << (void *)leaf_addr << " with thit " << thit << " ";
                            else
                                traversalFile << "miss quad node " << leaf_addr << " ";
                            traversalFile << "primitiveID = " << leaf.PrimitiveIndex0 << ", InstanceID = " << instanceLeaf.InstanceID << "\n";

                            traversalFile << "p[0] = (" << p[0].x << ", " << p[0].y << ", " << p[0].z << ") ";
                            traversalFile << "p[1] = (" << p[1].x << ", " << p[1].y << ", " << p[1].z << ") ";
                            traversalFile << "p[2] = (" << p[2].x << ", " << p[2].y << ", " << p[2].z << ") ";
                            traversalFile << "p[3] = (" << p[3].x << ", " << p[3].y << ", " << p[3].z << ")" << std::endl;
                        }

                        float world_thit = thit / worldToObject_tMultiplier;

                        //TODO: why the Tmin Tmax consition wasn't handled in the object coordinates?
                        if(hit && Tmin <= world_thit && world_thit <= Tmax && world_thit < min_thit)
                        {
                            if (debugTraversal)
                            {
                                traversalFile << "quad node " << (void *)leaf_addr << ", primitiveID " << leaf.PrimitiveIndex0 << " is the closest hit. world_thit " << thit / worldToObject_tMultiplier;
                            }

                            min_thit = thit / worldToObject_tMultiplier;
                            min_thit_object = thit;
                            closest_leaf = leaf;
                            closest_instanceLeaf = instanceLeaf;
                            closest_worldToObject = worldToObjectMatrix;
                            closest_objectToWorld = objectToWorldMatrix;
                            closest_objectRay = objectRay;
                            min_thit_object = thit;
                            thread->add_ray_intersect();
                            transactions.push_back(MemoryTransactionRecord((uint8_t*)((uint64_t)leaf_addr + device_offset), GEN_RT_BVH_QUAD_LEAF_length * 4, TransactionType::BVH_QUAD_LEAF_HIT));
                            ctx->func_sim->g_rt_mem_access_type[static_cast<int>(TransactionType::BVH_QUAD_LEAF_HIT)]++;
                            total_nodes_accessed++;

                            if(terminateOnFirstHit)
                            {
                                stack.clear();
                            }
                        }
                        else {
                            transactions.push_back(MemoryTransactionRecord((uint8_t*)((uint64_t)leaf_addr + device_offset), GEN_RT_BVH_QUAD_LEAF_length * 4, TransactionType::BVH_QUAD_LEAF));
                            ctx->func_sim->g_rt_mem_access_type[static_cast<int>(TransactionType::BVH_QUAD_LEAF)]++;
                            total_nodes_accessed++;
                        }
                        if (debugTraversal)
                        {
                            traversalFile << std::endl;
                        }
                    }
                    else
                    {
                        struct GEN_RT_BVH_PROCEDURAL_LEAF leaf;
                        GEN_RT_BVH_PROCEDURAL_LEAF_unpack(&leaf, leaf_addr);
                        transactions.push_back(MemoryTransactionRecord((uint8_t*)((uint64_t)leaf_addr + device_offset), GEN_RT_BVH_PROCEDURAL_LEAF_length * 4, TransactionType::BVH_PROCEDURAL_LEAF));
                        ctx->func_sim->g_rt_mem_access_type[static_cast<int>(TransactionType::BVH_PROCEDURAL_LEAF)]++;
                        total_nodes_accessed++;

                        uint32_t hit_group_index = instanceLeaf.InstanceContributionToHitGroupIndex;

                        warp_intersection_table* table = intersection_table[thread->get_ctaid().x][thread->get_ctaid().y];
                        auto intersectionTransactions = table->add_intersection(hit_group_index, thread->get_tid().x, leaf.PrimitiveIndex[0], instanceLeaf.InstanceID, pI, thread); // TODO: switch these to device addresses
                        
                        // transactions.insert(transactions.end(), intersectionTransactions.first.begin(), intersectionTransactions.first.end());
                        for(auto & newTransaction : intersectionTransactions.first)
                        {
                            bool found = false;
                            for(auto & transaction : transactions)
                                if(transaction.address == newTransaction.address)
                                {
                                    found = true;
                                    break;
                                }
                            if(!found)
                                transactions.push_back(newTransaction);

                        }
                        store_transactions.insert(store_transactions.end(), intersectionTransactions.second.begin(), intersectionTransactions.second.end());
                    }
                }
            }
        }
    }

    if (min_thit < ray.dir_tmax.w)
    {
        traversal_data.hit_geometry = true;
        ctx->func_sim->g_rt_num_hits++;
        traversal_data.closest_hit.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
        traversal_data.closest_hit.geometry_index = closest_leaf.LeafDescriptor.GeometryIndex;
        traversal_data.closest_hit.primitive_index = closest_leaf.PrimitiveIndex0;
        traversal_data.closest_hit.instance_index = closest_instanceLeaf.InstanceID;
        float3 intersection_point = ray.get_origin() + make_float3(ray.get_direction().x * min_thit, ray.get_direction().y * min_thit, ray.get_direction().z * min_thit);
        float3 rayatinter = ray.at(min_thit);
        // assert(intersection_point.x == ray.at(min_thit).x && intersection_point.y == ray.at(min_thit).y && intersection_point.z == ray.at(min_thit).z);
        traversal_data.closest_hit.intersection_point = intersection_point;
        traversal_data.closest_hit.worldToObjectMatrix = closest_worldToObject;
        traversal_data.closest_hit.objectToWorldMatrix = closest_objectToWorld;
        traversal_data.closest_hit.world_min_thit = min_thit;

        VSIM_DPRINTF("gpgpusim: Ray hit geomID %d primID %d\n", traversal_data.closest_hit.geometry_index, traversal_data.closest_hit.primitive_index);
        float3 p[3];
        for(int i = 0; i < 3; i++)
        {
            p[i].x = closest_leaf.QuadVertex[i].X;
            p[i].y = closest_leaf.QuadVertex[i].Y;
            p[i].z = closest_leaf.QuadVertex[i].Z;
        }
        float3 object_intersection_point = closest_objectRay.get_origin() + make_float3(closest_objectRay.get_direction().x * min_thit_object, closest_objectRay.get_direction().y * min_thit_object, closest_objectRay.get_direction().z * min_thit_object);
        //closest_objectRay.at(min_thit_object);
        float3 barycentric = Barycentric(object_intersection_point, p[0], p[1], p[2]);
        traversal_data.closest_hit.barycentric_coordinates = barycentric;
        thread->RT_thread_data->set_hitAttribute(barycentric, pI, thread);

        // store_transactions.push_back(MemoryStoreTransactionRecord(&traversal_data, sizeof(traversal_data), StoreTransactionType::Traversal_Results));
    }
    else
    {
        VSIM_DPRINTF("gpgpusim: Ray missed.\n");
        traversal_data.hit_geometry = false;
    }

    memory_space *mem = thread->get_global_memory();
    Traversal_data* device_traversal_data = (Traversal_data*) VulkanRayTracing::gpgpusim_alloc(sizeof(Traversal_data));
    mem->write(device_traversal_data, sizeof(Traversal_data), &traversal_data, thread, pI);
    thread->RT_thread_data->traversal_data.push_back(device_traversal_data);
    
    thread->set_rt_transactions(transactions);
    thread->set_rt_store_transactions(store_transactions);

    if (debugTraversal)
    {
        traversalFile.close();
    }

    if (total_nodes_accessed > ctx->func_sim->g_max_nodes_per_ray) {
        ctx->func_sim->g_max_nodes_per_ray = total_nodes_accessed;
    }
    ctx->func_sim->g_tot_nodes_per_ray += total_nodes_accessed;

    unsigned level = 0;
    for (auto it=tree_level_map.begin(); it!=tree_level_map.end(); it++) {
        if (it->second > level) {
            level = it->second;
        }
    }
    if (level > ctx->func_sim->g_max_tree_depth) {
        ctx->func_sim->g_max_tree_depth = level;
    }

    // Print out the transactions
    std::ofstream memoryTransactionsFile;

    if (false)
    {
        memoryTransactionsFile.open("memorytransactions.txt", std::ios_base::app);
        memoryTransactionsFile << "m_hw_tid:" << thread->get_hw_tid() << std::endl;
        memoryTransactionsFile << "Cycle:" << GPGPU_Context()->the_gpgpusim->g_the_gpu->gpu_sim_cycle << std::endl;

        for(auto item : transactions)
        {
            memoryTransactionsFile << (void *)item.address << "," << item.size << "," << (int)item.type << std::endl;
        }

        memoryTransactionsFile << std::endl;
    }
}

void VulkanRayTracing::endTraceRay(const ptx_instruction *pI, ptx_thread_info *thread)
{
    assert(thread->RT_thread_data->traversal_data.size() > 0);
    thread->RT_thread_data->traversal_data.pop_back();
    warp_intersection_table* table = intersection_table[thread->get_ctaid().x][thread->get_ctaid().y];
    table->clear(pI, thread);
}

bool VulkanRayTracing::mt_ray_triangle_test(float3 p0, float3 p1, float3 p2, Ray ray_properties, float* thit)
{
    // Moller Trumbore algorithm (from scratchapixel.com)
    float3 v0v1 = p1 - p0;
    float3 v0v2 = p2 - p0;
    float3 pvec = cross(ray_properties.get_direction(), v0v2);
    float det = dot(v0v1, pvec);

    float idet = 1 / det;

    float3 tvec = ray_properties.get_origin() - p0;
    float u = dot(tvec, pvec) * idet;

    if (u < 0 || u > 1) return false;

    float3 qvec = cross(tvec, v0v1);
    float v = dot(ray_properties.get_direction(), qvec) * idet;

    if (v < 0 || (u + v) > 1) return false;

    *thit = dot(v0v2, qvec) * idet;
    return true;
}

float3 VulkanRayTracing::Barycentric(float3 p, float3 a, float3 b, float3 c)
{
    //source: https://gamedev.stackexchange.com/questions/23743/whats-the-most-efficient-way-to-find-barycentric-coordinates
    float3 v0 = b - a;
    float3 v1 = c - a;
    float3 v2 = p - a;
    float d00 = dot(v0, v0);
    float d01 = dot(v0, v1);
    float d11 = dot(v1, v1);
    float d20 = dot(v2, v0);
    float d21 = dot(v2, v1);
    float denom = d00 * d11 - d01 * d01;
    float v = (d11 * d20 - d01 * d21) / denom;
    float w = (d00 * d21 - d01 * d20) / denom;
    float u = 1.0f - v - w;

    return {v, w, u};
}

void VulkanRayTracing::load_descriptor(const ptx_instruction *pI, ptx_thread_info *thread)
{

}


void VulkanRayTracing::setPipelineInfo(VkRayTracingPipelineCreateInfoKHR* pCreateInfos)
{
    VulkanRayTracing::pCreateInfos = pCreateInfos;
	std::cout << "gpgpusim: set pipeline" << std::endl;
}


void VulkanRayTracing::setGeometries(VkAccelerationStructureGeometryKHR* pGeometries, uint32_t geometryCount)
{
    VulkanRayTracing::pGeometries = pGeometries;
    VulkanRayTracing::geometryCount = geometryCount;
	std::cout << "gpgpusim: set geometry" << std::endl;
}

void VulkanRayTracing::setAccelerationStructure(VkAccelerationStructureKHR accelerationStructure)
{
    GEN_RT_BVH topBVH; //TODO: test hit with world before traversal
    GEN_RT_BVH_unpack(&topBVH, (uint8_t *)accelerationStructure);




    std::cout << "gpgpusim: set AS" << std::endl;
    VulkanRayTracing::topLevelAS = accelerationStructure;
}

std::string base_name(std::string & path)
{
  return path.substr(path.find_last_of("/") + 1);
}

void VulkanRayTracing::setDescriptorSet(struct DESCRIPTOR_SET_STRUCT *set)
{
    if (VulkanRayTracing::descriptorSet == NULL) {
        printf("gpgpusim: set descriptor set 0x%x\n", set);
        VulkanRayTracing::descriptorSet = set;
    }
    // TODO: Figure out why it sets the descriptor set twice
    else {
        printf("gpgpusim: descriptor set already set; ignoring update.\n");
    }
}

static bool invoked = false;

void copyHardCodedShaders()
{
    std::ifstream  src;
    std::ofstream  dst;

    // src.open("/home/mrs/emerald-ray-tracing/hardcodeShader/MESA_SHADER_MISS_2.ptx", std::ios::binary);
    // dst.open("/home/mrs/emerald-ray-tracing/mesagpgpusimShaders/MESA_SHADER_MISS_2.ptx", std::ios::binary);
    // dst << src.rdbuf();
    // src.close();
    // dst.close();
    
    // src.open("/home/mrs/emerald-ray-tracing/hardcodeShader/MESA_SHADER_CLOSEST_HIT_2.ptx", std::ios::binary);
    // dst.open("/home/mrs/emerald-ray-tracing/mesagpgpusimShaders/MESA_SHADER_CLOSEST_HIT_2.ptx", std::ios::binary);
    // dst << src.rdbuf();
    // src.close();
    // dst.close();

    // src.open("/home/mrs/emerald-ray-tracing/hardcodeShader/MESA_SHADER_RAYGEN_0.ptx", std::ios::binary);
    // dst.open("/home/mrs/emerald-ray-tracing/mesagpgpusimShaders/MESA_SHADER_RAYGEN_0.ptx", std::ios::binary);
    // dst << src.rdbuf();
    // src.close();
    // dst.close();

    // src.open("/home/mrs/emerald-ray-tracing/hardcodeShader/MESA_SHADER_INTERSECTION_4.ptx", std::ios::binary);
    // dst.open("/home/mrs/emerald-ray-tracing/mesagpgpusimShaders/MESA_SHADER_INTERSECTION_4.ptx", std::ios::binary);
    // dst << src.rdbuf();
    // src.close();
    // dst.close();

    // {
    //     std::ifstream  src("/home/mrs/emerald-ray-tracing/MESA_SHADER_MISS_0.ptx", std::ios::binary);
    //     std::ofstream  dst("/home/mrs/emerald-ray-tracing/mesagpgpusimShaders/MESA_SHADER_MISS_1.ptx",   std::ios::binary);
    //     dst << src.rdbuf();
    //     src.close();
    //     dst.close();
    // }
}

uint32_t VulkanRayTracing::registerShaders(char * shaderPath, gl_shader_stage shaderType)
{
    printf("gpgpusim: register shaders\n");
    copyHardCodedShaders();

    VulkanRayTracing::invoke_gpgpusim();
    gpgpu_context *ctx;
    ctx = GPGPU_Context();
    CUctx_st *context = GPGPUSim_Context(ctx);

    // Register all the ptx files in $MESA_ROOT/gpgpusimShaders by looping through them
    // std::vector <std::string> ptx_list;

    // Add ptx file names in gpgpusimShaders folder to ptx_list
    char *mesa_root = getenv("MESA_ROOT");
    char *gpgpusim_root = getenv("GPGPUSIM_ROOT");
    // char *filePath = "gpgpusimShaders/";
    // char fullPath[200];
    // snprintf(fullPath, sizeof(fullPath), "%s%s", mesa_root, filePath);
    // std::string fullPathString(fullPath);

    // for (auto &p : fs::recursive_directory_iterator(fullPathString))
    // {
    //     if (p.path().extension() == ".ptx")
    //     {
    //         //std::cout << p.path().string() << '\n';
    //         ptx_list.push_back(p.path().string());
    //     }
    // }

    std::string fullpath(shaderPath);
    std::string fullfilename = base_name(fullpath);
    std::string filenameNoExt;
    size_t start = fullfilename.find_first_not_of('.', 0);
    size_t end = fullfilename.find('.', start);
    filenameNoExt = fullfilename.substr(start, end - start);
    std::string idInString = filenameNoExt.substr(filenameNoExt.find_last_of("_") + 1);
    // Register each ptx file in ptx_list
    shader_stage_info shader;
    //shader.ID = VulkanRayTracing::shaders.size();
    shader.ID = std::stoi(idInString);
    shader.type = shaderType;
    shader.function_name = (char*)malloc(200 * sizeof(char));

    std::string deviceFunction;

    switch(shaderType) {
        case MESA_SHADER_RAYGEN:
            // shader.function_name = "raygen_" + std::to_string(shader.ID);
            strcpy(shader.function_name, "raygen_");
            strcat(shader.function_name, std::to_string(shader.ID).c_str());
            deviceFunction = "MESA_SHADER_RAYGEN";
            break;
        case MESA_SHADER_ANY_HIT:
            // shader.function_name = "anyhit_" + std::to_string(shader.ID);
            strcpy(shader.function_name, "anyhit_");
            strcat(shader.function_name, std::to_string(shader.ID).c_str());
            deviceFunction = "";
            assert(0);
            break;
        case MESA_SHADER_CLOSEST_HIT:
            // shader.function_name = "closesthit_" + std::to_string(shader.ID);
            strcpy(shader.function_name, "closesthit_");
            strcat(shader.function_name, std::to_string(shader.ID).c_str());
            deviceFunction = "MESA_SHADER_CLOSEST_HIT";
            break;
        case MESA_SHADER_MISS:
            // shader.function_name = "miss_" + std::to_string(shader.ID);
            strcpy(shader.function_name, "miss_");
            strcat(shader.function_name, std::to_string(shader.ID).c_str());
            deviceFunction = "MESA_SHADER_MISS";
            break;
        case MESA_SHADER_INTERSECTION:
            // shader.function_name = "intersection_" + std::to_string(shader.ID);
            strcpy(shader.function_name, "intersection_");
            strcat(shader.function_name, std::to_string(shader.ID).c_str());
            deviceFunction = "MESA_SHADER_INTERSECTION";
            break;
        case MESA_SHADER_CALLABLE:
            // shader.function_name = "callable_" + std::to_string(shader.ID);
            strcpy(shader.function_name, "callable_");
            strcat(shader.function_name, std::to_string(shader.ID).c_str());
            deviceFunction = "";
            assert(0);
            break;
    }
    deviceFunction += "_func" + std::to_string(shader.ID) + "_main";
    // deviceFunction += "_main";

    symbol_table *symtab;
    unsigned num_ptx_versions = 0;
    unsigned max_capability = 20;
    unsigned selected_capability = 20;
    bool found = false;
    
    unsigned long long fat_cubin_handle = shader.ID;

    // PTX File
    //std::cout << itr << std::endl;
    symtab = ctx->gpgpu_ptx_sim_load_ptx_from_filename(shaderPath);
    context->add_binary(symtab, fat_cubin_handle);
    // need to add all the magic registers to ptx.l to special_register, reference ayub ptx.l:225

    // PTX info
    // Run the python script and get ptxinfo
    std::cout << "GPGPUSIM: Generating PTXINFO for" << shaderPath << "info" << std::endl;
    char command[400];
    snprintf(command, sizeof(command), "python3 %s/scripts/generate_rt_ptxinfo.py %s", gpgpusim_root, shaderPath);
    int result = system(command);
    if (result != 0) {
        printf("GPGPU-Sim PTX: ERROR ** while loading PTX (b) %d\n", result);
        printf("               Ensure ptxas is in your path.\n");
        exit(1);
    }
    
    char ptxinfo_filename[400];
    snprintf(ptxinfo_filename, sizeof(ptxinfo_filename), "%sinfo", shaderPath);
    ctx->gpgpu_ptx_info_load_from_external_file(ptxinfo_filename); // TODO: make a version where it just loads my ptxinfo instead of generating a new one

    context->register_function(fat_cubin_handle, shader.function_name, deviceFunction.c_str());

    VulkanRayTracing::shaders.push_back(shader);

    return shader.ID;

    // if (itr.find("RAYGEN") != std::string::npos)
    // {
    //     printf("############### registering %s\n", shaderPath);
    //     context->register_function(fat_cubin_handle, "raygen_shader", "MESA_SHADER_RAYGEN_main");
    // }

    // if (itr.find("MISS") != std::string::npos)
    // {
    //     printf("############### registering %s\n", shaderPath);
    //     context->register_function(fat_cubin_handle, "miss_shader", "MESA_SHADER_MISS_main");
    // }

    // if (itr.find("CLOSEST") != std::string::npos)
    // {
    //     printf("############### registering %s\n", shaderPath);
    //     context->register_function(fat_cubin_handle, "closest_hit_shader", "MESA_SHADER_CLOSEST_HIT_main");
    // }
}


void VulkanRayTracing::invoke_gpgpusim()
{
    printf("gpgpusim: invoking gpgpusim\n");
    gpgpu_context *ctx;
    ctx = GPGPU_Context();
    CUctx_st *context = GPGPUSim_Context(ctx);

    if(!invoked)
    {
        //registerShaders();
        invoked = true;
    }
}

// int CmdTraceRaysKHRID = 0;

const bool writeImageBinary = true;

void VulkanRayTracing::vkCmdTraceRaysKHR(
                      void *raygen_sbt,
                      void *miss_sbt,
                      void *hit_sbt,
                      void *callable_sbt,
                      bool is_indirect,
                      uint32_t launch_width,
                      uint32_t launch_height,
                      uint32_t launch_depth,
                      uint64_t launch_size_addr) {
    printf("gpgpusim: launching cmd trace ray\n");
    // launch_width = 224;
    // launch_height = 160;
    init(launch_width, launch_height);
    
    // Dump Descriptor Sets
    if (dump_trace) 
    {
        dump_descriptor_sets(VulkanRayTracing::descriptorSet);
        dump_callparams_and_sbt(raygen_sbt, miss_sbt, hit_sbt, callable_sbt, is_indirect, launch_width, launch_height, launch_depth, launch_size_addr);
    }

    // CmdTraceRaysKHRID++;
    // if(CmdTraceRaysKHRID != 1)
    //     return;
    // launch_width = 420;
    // launch_height = 320;

    if(writeImageBinary && !imageFile.is_open())
    {
        char* imageFileName;
        char defaultFileName[40] = "image.binary";
        if(getenv("VULKAN_IMAGE_FILE_NAME"))
            imageFileName = getenv("VULKAN_IMAGE_FILE_NAME");
        else
            imageFileName = defaultFileName;
        imageFile.open(imageFileName, std::ios::out | std::ios::binary);
        
        // imageFile.open("image.txt", std::ios::out);
    }
    // memset(((uint8_t*)descriptors[0][1].address), uint8_t(127), launch_height * launch_width * 4);
    // return;

    // {
    //     std::ifstream infile("debug_printf.log");
    //     std::string line;
    //     while (std::getline(infile, line))
    //     {
    //         if(line == "")
    //             continue;

    //         RayDebugGPUData data;
    //         // sscanf(line.c_str(), "LaunchID:(%d,%d), InstanceCustomIndex = %d, primitiveID = %d, v0 = (%f, %f, %f), v1 = (%f, %f, %f), v2 = (%f, %f, %f), hitAttribute = (%f, %f), normalWorld = (%f, %f, %f), objectIntersection = (%f, %f, %f), worldIntersection = (%f, %f, %f), objectNormal = (%f, %f, %f), worldNormal = (%f, %f, %f), NdotL = %f",
    //         //             &data.launchIDx, &data.launchIDy, &data.instanceCustomIndex, &data.primitiveID, &data.v0pos.x, &data.v0pos.y, &data.v0pos.z, &data.v1pos.x, &data.v1pos.y, &data.v1pos.z, &data.v2pos.x, &data.v2pos.y, &data.v2pos.z, &data.attribs.x, &data.attribs.y, &data.N.x, &data.N.y, &data.N.z, &data.P_object.x, &data.P_object.y, &data.P_object.z, &data.P.x, &data.P.y, &data.P.z, &data.N_object.x, &data.N_object.y, &data.N_object.z, &data.N.x, &data.N.y, &data.N.z, &data.NdotL);
    //         sscanf(line.c_str(), "launchID = (%d, %d), hitValue = (%f, %f, %f)",
    //                     &data.launchIDx, &data.launchIDy, &data.hitValue.x, &data.hitValue.y, &data.hitValue.z);
    //         data.valid = true;
    //         assert(data.launchIDx < 2000 && data.launchIDy < 2000);
    //         // printf("#### (%d, %d)\n", data.launchIDx, data.launchIDy);
    //         // fflush(stdout);
    //         rayDebugGPUData[data.launchIDx][data.launchIDy] = data;

    //     }
    // }

    assert(launch_depth == 1);

#if defined(MESA_USE_INTEL_DRIVER)
    struct DESCRIPTOR_STRUCT desc;
    desc.image_view = NULL;
#endif

    gpgpu_context *ctx;
    ctx = GPGPU_Context();
    CUctx_st *context = GPGPUSim_Context(ctx);

    unsigned long shaderId = *(uint64_t*)raygen_sbt;
    int index = 0;
    for (int i = 0; i < shaders.size(); i++) {
        if (shaders[i].ID == 0){
            index = i;
            break;
        }
    }
    ctx->func_sim->g_total_shaders = shaders.size();

    shader_stage_info raygen_shader = shaders[index];
    function_info *entry = context->get_kernel(raygen_shader.function_name);
    // printf("################ number of args = %d\n", entry->num_args());

    if (entry->is_pdom_set()) {
        printf("GPGPU-Sim PTX: PDOM analysis already done for %s \n",
            entry->get_name().c_str());
    } else {
        printf("GPGPU-Sim PTX: finding reconvergence points for \'%s\'...\n",
            entry->get_name().c_str());
        /*
        * Some of the instructions like printf() gives the gpgpusim the wrong
        * impression that it is a function call. As printf() doesnt have a body
        * like functions do, doing pdom analysis for printf() causes a crash.
        */
        if (entry->get_function_size() > 0) entry->do_pdom();
        entry->set_pdom();
    }

    // check that number of args and return match function requirements
    //if (pI->has_return() ^ entry->has_return()) {
    //    printf(
    //        "GPGPU-Sim PTX: Execution error - mismatch in number of return values "
    //        "between\n"
    //        "               call instruction and function declaration\n");
    //    abort();
    //}
    unsigned n_return = entry->has_return();
    unsigned n_args = entry->num_args();
    //unsigned n_operands = pI->get_num_operands();

    // launch_width = 192;
    // launch_height = 32;

    dim3 blockDim = dim3(1, 1, 1);
    dim3 gridDim = dim3(1, launch_height, launch_depth);
    warp_pixel_mapping mapping = WARP_8X4;

    switch (mapping) {
        case WARP_32X1:
            if(launch_width <= 32) {
                blockDim.x = launch_width;
                gridDim.x = 1;
            }
            else {
                blockDim.x = 32;
                gridDim.x = launch_width / 32;
                if(launch_width % 32 != 0)
                    gridDim.x++;
            }
            break;
        case WARP_16X2:
            if(launch_width <= 16) {
                blockDim.x = launch_width;
                gridDim.x = 1;
            }
            else {
                blockDim.x = 16;
                gridDim.x = launch_width / 16;
                if(launch_width % 16 != 0)
                    gridDim.x++;
            }
            if(launch_height <= 2) {
                blockDim.y = launch_height;
                gridDim.y = 1;
            }
            else {
                blockDim.y = 2;
                gridDim.y = launch_height / 2;
                if(launch_height % 2 != 0)
                    gridDim.y++;
            }
            break;
        case WARP_8X4:
            if(launch_width <= 8) {
                blockDim.x = launch_width;
                gridDim.x = 1;
            }
            else {
                blockDim.x = 8;
                gridDim.x = launch_width / 8;
                if(launch_width % 8 != 0)
                    gridDim.x++;
            }
            if(launch_height <= 4) {
                blockDim.y = launch_height;
                gridDim.y = 1;
            }
            else {
                blockDim.y = 4;
                gridDim.y = launch_height / 4;
                if(launch_height % 4 != 0)
                    gridDim.y++;
            }
            break;
        default:
            abort();
    }
    printf("gpgpusim: launch dimensions %d x %d x %d\n", gridDim.x, gridDim.y, gridDim.z);

    std::cout << "\n================================ Kernel Dimensions ================================" << std::endl;
    std::cout << "blockDim: (" << blockDim.x << ", " << blockDim.y << ", " << blockDim.z << ")" << " gridDim: (" << gridDim.x << ", " << gridDim.y << ", " << gridDim.z << ")\n" << std::endl;


    gpgpu_ptx_sim_arg_list_t args;
    // kernel_info_t *grid = ctx->api->gpgpu_cuda_ptx_sim_init_grid(
    //   raygen_shader.function_name, args, dim3(4, 128, 1), dim3(32, 1, 1), context);
    kernel_info_t *grid = ctx->api->gpgpu_cuda_ptx_sim_init_grid(
      raygen_shader.function_name, args, gridDim, blockDim, context);
    grid->vulkan_metadata.raygen_sbt = raygen_sbt;
    grid->vulkan_metadata.miss_sbt = miss_sbt;
    grid->vulkan_metadata.hit_sbt = hit_sbt;
    grid->vulkan_metadata.callable_sbt = callable_sbt;
    grid->vulkan_metadata.launch_width = launch_width;
    grid->vulkan_metadata.launch_height = launch_height;
    grid->vulkan_metadata.launch_depth = launch_depth;
    
    printf("gpgpusim: SBT: raygen %p, miss %p, hit %p, callable %p\n", 
            raygen_sbt, miss_sbt, hit_sbt, callable_sbt);
            
    struct CUstream_st *stream = 0;
    stream_operation op(grid, ctx->func_sim->g_ptx_sim_mode, stream);
    ctx->the_gpgpusim->g_stream_manager->push(op);

    //printf("%d\n", descriptors[0][1].address);

    fflush(stdout);

    while(!op.is_done() && !op.get_kernel()->done()) {
        printf("waiting for op to finish\n");
        sleep(1);
        continue;
    }
    // for (unsigned i = 0; i < entry->num_args(); i++) {
    //     std::pair<size_t, unsigned> p = entry->get_param_config(i);
    //     cudaSetupArgumentInternal(args[i], p.first, p.second);
    // }
}

void VulkanRayTracing::callMissShader(const ptx_instruction *pI, ptx_thread_info *thread) {
    gpgpu_context *ctx;
    ctx = GPGPU_Context();
    CUctx_st *context = GPGPUSim_Context(ctx);

    memory_space *mem = thread->get_global_memory();
    Traversal_data* traversal_data = thread->RT_thread_data->traversal_data.back();

    int32_t current_shader_counter = -1;
    mem->write(&(traversal_data->current_shader_counter), sizeof(traversal_data->current_shader_counter), &current_shader_counter, thread, pI);

    uint32_t missIndex;
    mem->read(&(traversal_data->missIndex), sizeof(traversal_data->missIndex), &missIndex);

    uint32_t shaderID = *((uint32_t *)(thread->get_kernel().vulkan_metadata.miss_sbt) + 8 * missIndex);
    VSIM_DPRINTF("gpgpusim: Calling Miss Shader at ID %d\n", shaderID);

    shader_stage_info miss_shader = shaders[shaderID];

    function_info *entry = context->get_kernel(miss_shader.function_name);
    callShader(pI, thread, entry);
}

void VulkanRayTracing::callClosestHitShader(const ptx_instruction *pI, ptx_thread_info *thread) {
    gpgpu_context *ctx;
    ctx = GPGPU_Context();
    CUctx_st *context = GPGPUSim_Context(ctx);

    memory_space *mem = thread->get_global_memory();
    Traversal_data* traversal_data = thread->RT_thread_data->traversal_data.back();

    bool hit_geometry;
    mem->read(&(traversal_data->hit_geometry), sizeof(bool), &hit_geometry);
    assert(hit_geometry);

    int32_t current_shader_counter = -1;
    mem->write(&(traversal_data->current_shader_counter), sizeof(traversal_data->current_shader_counter), &current_shader_counter, thread, pI);

    VkGeometryTypeKHR geometryType;
    mem->read(&(traversal_data->closest_hit.geometryType), sizeof(traversal_data->closest_hit.geometryType), &geometryType);

    shader_stage_info closesthit_shader;
    if(geometryType == VK_GEOMETRY_TYPE_TRIANGLES_KHR) {
        uint32_t shaderID = *((uint64_t *)(thread->get_kernel().vulkan_metadata.hit_sbt));
        closesthit_shader = shaders[shaderID];
        VSIM_DPRINTF("gpgpusim: Calling Closest Hit Shader at ID %d\n", shaderID);

    }
    else {
        int32_t hitGroupIndex;
        mem->read(&(traversal_data->closest_hit.hitGroupIndex), sizeof(traversal_data->closest_hit.hitGroupIndex), &hitGroupIndex);
        uint32_t shaderID = *((uint64_t *)(thread->get_kernel().vulkan_metadata.hit_sbt) + 8 * hitGroupIndex);
        closesthit_shader = shaders[shaderID];
        VSIM_DPRINTF("gpgpusim: Calling Closest Hit Shader at ID %d\n", shaderID);
    }

    function_info *entry = context->get_kernel(closesthit_shader.function_name);
    callShader(pI, thread, entry);
}

void VulkanRayTracing::callIntersectionShader(const ptx_instruction *pI, ptx_thread_info *thread, uint32_t shader_counter) {
    VSIM_DPRINTF("gpgpusim: Calling Intersection Shader\n");
    gpgpu_context *ctx;
    ctx = GPGPU_Context();
    CUctx_st *context = GPGPUSim_Context(ctx);
    
    memory_space *mem = thread->get_global_memory();
    Traversal_data* traversal_data = thread->RT_thread_data->traversal_data.back();
    mem->write(&(traversal_data->current_shader_counter), sizeof(traversal_data->current_shader_counter), &shader_counter, thread, pI);

    warp_intersection_table* table = VulkanRayTracing::intersection_table[thread->get_ctaid().x][thread->get_ctaid().y];
    uint32_t hitGroupIndex = table->get_hitGroupIndex(shader_counter, thread->get_tid().x, pI, thread);

    shader_stage_info intersection_shader = shaders[*((uint64_t *)(thread->get_kernel().vulkan_metadata.hit_sbt) + 8 * hitGroupIndex + 1)];
    function_info *entry = context->get_kernel(intersection_shader.function_name);
    callShader(pI, thread, entry);
}

void VulkanRayTracing::callAnyHitShader(const ptx_instruction *pI, ptx_thread_info *thread) {
    VSIM_DPRINTF("gpgpusim: Calling Any Hit Shader\n");
    gpgpu_context *ctx;
    ctx = GPGPU_Context();
    CUctx_st *context = GPGPUSim_Context(ctx);

    assert(0);
}

void VulkanRayTracing::callShader(const ptx_instruction *pI, ptx_thread_info *thread, function_info *target_func) {
    static unsigned call_uid_next = 1;

  if (target_func->is_pdom_set()) {
    // printf("GPGPU-Sim PTX: PDOM analysis already done for %s \n",
    //        target_func->get_name().c_str());
  } else {
    printf("GPGPU-Sim PTX: finding reconvergence points for \'%s\'...\n",
           target_func->get_name().c_str());
    /*
     * Some of the instructions like printf() gives the gpgpusim the wrong
     * impression that it is a function call. As printf() doesnt have a body
     * like functions do, doing pdom analysis for printf() causes a crash.
     */
    if (target_func->get_function_size() > 0) target_func->do_pdom();
    target_func->set_pdom();
  }

  thread->set_npc(target_func->get_start_PC());

  // check that number of args and return match function requirements
  if (pI->has_return() ^ target_func->has_return()) {
    printf(
        "GPGPU-Sim PTX: Execution error - mismatch in number of return values "
        "between\n"
        "               call instruction and function declaration\n");
    abort();
  }
  unsigned n_return = target_func->has_return();
  unsigned n_args = target_func->num_args();
  unsigned n_operands = pI->get_num_operands();

  // TODO: why this fails?
//   if (n_operands != (n_return + 1 + n_args)) {
//     printf(
//         "GPGPU-Sim PTX: Execution error - mismatch in number of arguements "
//         "between\n"
//         "               call instruction and function declaration\n");
//     abort();
//   }

  // handle intrinsic functions
//   std::string fname = target_func->get_name();
//   if (fname == "vprintf") {
//     gpgpusim_cuda_vprintf(pI, thread, target_func);
//     return;
//   }
// #if (CUDART_VERSION >= 5000)
//   // Jin: handle device runtime apis for CDP
//   else if (fname == "cudaGetParameterBufferV2") {
//     target_func->gpgpu_ctx->device_runtime->gpgpusim_cuda_getParameterBufferV2(
//         pI, thread, target_func);
//     return;
//   } else if (fname == "cudaLaunchDeviceV2") {
//     target_func->gpgpu_ctx->device_runtime->gpgpusim_cuda_launchDeviceV2(
//         pI, thread, target_func);
//     return;
//   } else if (fname == "cudaStreamCreateWithFlags") {
//     target_func->gpgpu_ctx->device_runtime->gpgpusim_cuda_streamCreateWithFlags(
//         pI, thread, target_func);
//     return;
//   }
// #endif

  // read source arguements into register specified in declaration of function
  arg_buffer_list_t arg_values;
  copy_args_into_buffer_list(pI, thread, target_func, arg_values);

  // record local for return value (we only support a single return value)
  const symbol *return_var_src = NULL;
  const symbol *return_var_dst = NULL;
  if (target_func->has_return()) {
    return_var_dst = pI->dst().get_symbol();
    return_var_src = target_func->get_return_var();
  }

  gpgpu_sim *gpu = thread->get_gpu();
  unsigned callee_pc = 0, callee_rpc = 0;
  /*if (gpu->simd_model() == POST_DOMINATOR)*/ { //MRS_TODO: why this fails?
    thread->get_core()->get_pdom_stack_top_info(thread->get_hw_wid(),
                                                &callee_pc, &callee_rpc);
    assert(callee_pc == thread->get_pc());
  }

  thread->callstack_push(callee_pc + pI->inst_size(), callee_rpc,
                         return_var_src, return_var_dst, call_uid_next++);

  copy_buffer_list_into_frame(thread, arg_values);

  thread->set_npc(target_func);
}

void VulkanRayTracing::setDescriptor(uint32_t setID, uint32_t descID, void *address, uint32_t size, VkDescriptorType type)
{
    printf("gpgpusim: set descriptor\n");
    if(descriptors.size() <= setID)
        descriptors.resize(setID + 1);
    if(descriptors[setID].size() <= descID)
        descriptors[setID].resize(descID + 1);
    
    descriptors[setID][descID].setID = setID;
    descriptors[setID][descID].descID = descID;
    descriptors[setID][descID].address = address;
    descriptors[setID][descID].size = size;
    descriptors[setID][descID].type = type;
}


void VulkanRayTracing::setDescriptorSetFromLauncher(void *address, void *deviceAddress, uint32_t setID, uint32_t descID)
{
    launcher_deviceDescriptorSets[setID][descID] = deviceAddress;
    launcher_descriptorSets[setID][descID] = address;
}

void* VulkanRayTracing::getDescriptorAddress(uint32_t setID, uint32_t binding)
{
#if defined(MESA_USE_INTEL_DRIVER)
    if (use_external_launcher)
    {
        return launcher_deviceDescriptorSets[setID][binding];
        // return launcher_descriptorSets[setID][binding];
    }
    else 
    {
        // assert(setID < descriptors.size());
        // assert(binding < descriptors[setID].size());

        struct anv_descriptor_set* set = VulkanRayTracing::descriptorSet;

        const struct anv_descriptor_set_binding_layout *bind_layout = &set->layout->binding[binding];
        struct anv_descriptor *desc = &set->descriptors[bind_layout->descriptor_index];
        void *desc_map = set->desc_mem.map + bind_layout->descriptor_offset;

        assert(desc->type == bind_layout->type);

        switch (desc->type)
        {
            case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
            {
                return (void *)(desc);
            }
            case VK_DESCRIPTOR_TYPE_SAMPLER:
            case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
            case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
            case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
            {
                return desc;
            }

            case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
            case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
                assert(0);
                break;

            case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
            case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
            case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
            case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
            {
                if (desc->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC ||
                    desc->type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC)
                {
                    // MRS_TODO: account for desc->offset?
                    return anv_address_map(desc->buffer->address);
                }
                else
                {
                    struct anv_buffer_view *bview = &set->buffer_views[bind_layout->buffer_view_index];
                    return anv_address_map(bview->address);
                }
            }

            case VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK_EXT:
                assert(0);
                break;

            case VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR:
            case VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_NV:
            {
                struct anv_address_range_descriptor *desc_data = desc_map;
                return (void *)(desc_data->address);
            }

            default:
                assert(0);
                break;
        }

        // return descriptors[setID][binding].address;
    }
#elif defined(MESA_USE_LVPIPE_DRIVER)
    VSIM_DPRINTF("gpgpusim: getDescriptorAddress for binding %d\n", binding);
    struct lvp_descriptor_set* set = VulkanRayTracing::descriptorSet;
    const struct lvp_descriptor_set_binding_layout *bind_layout = &set->layout->binding[binding];
    struct lvp_descriptor *desc = &set->descriptors[bind_layout->descriptor_index];

    switch (desc->type) {
        case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
            VSIM_DPRINTF("gpgpusim: storage image; descriptor address %p\n", desc);
            return (void *) desc;
            break;
        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
            VSIM_DPRINTF("gpgpusim: uniform buffer; buffer mem address %p\n", (void *) desc->info.ubo.pmem);
            return (void *) desc->info.ubo.pmem;
            break;
        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
            VSIM_DPRINTF("gpgpusim: storage buffer; buffer mem address %p\n", (void *) desc->info.ssbo.pmem);
            return (void *) desc->info.ssbo.pmem;
            break;
        case VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR:
            VSIM_DPRINTF("gpgpusim: accel struct; root address %p\n", (void *)desc->info.ubo.pmem + desc->info.ubo.buffer_offset);
            return (void *)desc->info.ubo.pmem + desc->info.ubo.buffer_offset;
            break;
        default:
            VSIM_DPRINTF("gpgpusim: unimplemented descriptor type\n");
            abort();
    }
#endif
}

void VulkanRayTracing::getTexture(struct DESCRIPTOR_STRUCT *desc, 
                                    float x, float y, float lod, 
                                    float &c0, float &c1, float &c2, float &c3, 
                                    std::vector<ImageMemoryTransactionRecord>& transactions,
                                    uint64_t launcher_offset)
{
#if defined(MESA_USE_INTEL_DRIVER)
    Pixel pixel;

    if (use_external_launcher)
    {
        pixel = get_interpolated_pixel((anv_image_view*) desc, (anv_sampler*) desc, x, y, transactions, launcher_offset); // cast back to metadata later
    }
    else 
    {
        struct anv_image_view *image_view =  desc->image_view;
        struct anv_sampler *sampler = desc->sampler;

        const struct anv_image *image = image_view->image;
        assert(image->n_planes == 1);
        assert(image->samples == 1);
        assert(image->tiling == VK_IMAGE_TILING_OPTIMAL);
        assert(image->planes[0].surface.isl.tiling == ISL_TILING_Y0);
        assert(sampler->conversion == NULL);

        pixel = get_interpolated_pixel(image_view, sampler, x, y, transactions);
    }

    TXL_DPRINTF("Setting transaction type to TEXTURE_LOAD\n");
    for(int i = 0; i < transactions.size(); i++)
        transactions[i].type = ImageTransactionType::TEXTURE_LOAD;
    
    c0 = pixel.c0;
    c1 = pixel.c1;
    c2 = pixel.c2;
    c3 = pixel.c3;


    // uint8_t* address = anv_address_map(image->planes[0].address);

    // for(int x = 0; x < image->extent.width; x++)
    // {
    //     for(int y = 0; y < image->extent.height; y++)
    //     {
    //         int blockX = x / 8;
    //         int blockY = y / 8;

    //         uint32_t offset = (blockX + blockY * (image->extent.width / 8)) * (128 / 8);

    //         uint8_t dst_colors[100];
    //         basisu::astc::decompress(dst_colors, address + offset, true, 8, 8);
    //         uint8_t* pixel_color = &dst_colors[0] + (x % 8 + (y % 8) * 8) * 4;

    //         uint32_t bit_map_offset = x + y * image->extent.width;

    //         float data[4];
    //         data[0] = pixel_color[0] / 255.0;
    //         data[1] = pixel_color[1] / 255.0;
    //         data[2] = pixel_color[2] / 255.0;
    //         data[3] = pixel_color[3] / 255.0;
    //         imageFile.write((char*) data, 3 * sizeof(float));
    //         imageFile.write((char*) (&bit_map_offset), sizeof(uint32_t));
    //         imageFile.flush();
    //     }
    // }
#elif defined(MESA_USE_LVPIPE_DRIVER)
    printf("gpgpusim: getTexture not implemented for lavapipe.\n");
    abort();
#endif
}

void VulkanRayTracing::image_load(struct DESCRIPTOR_STRUCT *desc, uint32_t x, uint32_t y, float &c0, float &c1, float &c2, float &c3)
{
#if defined(MESA_USE_INTEL_DRIVER)
    ImageMemoryTransactionRecord transaction;

    struct anv_image_view *image_view =  desc->image_view;
    struct anv_sampler *sampler = desc->sampler;

    const struct anv_image *image = image_view->image;
    assert(image->n_planes == 1);
    assert(image->samples == 1);
    assert(image->tiling == VK_IMAGE_TILING_OPTIMAL);
    assert(image->planes[0].surface.isl.tiling == ISL_TILING_Y0);
    assert(sampler->conversion == NULL);

    Pixel pixel = load_image_pixel(image, x, y, 0, transaction);

    transaction.type = ImageTransactionType::IMAGE_LOAD;
    
    c0 = pixel.c0;
    c1 = pixel.c1;
    c2 = pixel.c2;
    c3 = pixel.c3;

#elif defined(MESA_USE_LVPIPE_DRIVER)
    VSIM_DPRINTF("gpgpusim: image_load not implemented for lavapipe.\n");
    abort();

#endif
}

void VulkanRayTracing::image_store(struct DESCRIPTOR_STRUCT* desc, uint32_t gl_LaunchIDEXT_X, uint32_t gl_LaunchIDEXT_Y, uint32_t gl_LaunchIDEXT_Z, uint32_t gl_LaunchIDEXT_W, 
              float hitValue_X, float hitValue_Y, float hitValue_Z, float hitValue_W, const ptx_instruction *pI, ptx_thread_info *thread)
{
#if defined(MESA_USE_INTEL_DRIVER)
    ImageMemoryTransactionRecord transaction;
    Pixel pixel = Pixel(hitValue_X, hitValue_Y, hitValue_Z, hitValue_W);

    VkFormat vk_format;
    if (use_external_launcher)
    {
        storage_image_metadata *metadata = (storage_image_metadata*) desc;
        vk_format = metadata->format;
        store_image_pixel((anv_image*) desc, gl_LaunchIDEXT_X, gl_LaunchIDEXT_Y, 0, pixel, transaction);
    }
    else
    {
        assert(desc->sampler == NULL);

        struct anv_image_view *image_view = desc->image_view;
        assert(image_view != NULL);
        struct anv_image * image = image_view->image;

        vk_format = image->vk_format;

        store_image_pixel(image, gl_LaunchIDEXT_X, gl_LaunchIDEXT_Y, 0, pixel, transaction);
    }

    
    transaction.type = ImageTransactionType::IMAGE_STORE;

    if(writeImageBinary && vk_format != VK_FORMAT_R32G32B32A32_SFLOAT)
    {
        uint32_t image_width = thread->get_kernel().vulkan_metadata.launch_width;
        uint32_t offset = 0;
        offset += gl_LaunchIDEXT_Y * image_width;
        offset += gl_LaunchIDEXT_X;

        float data[4];
        data[0] = hitValue_X;
        data[1] = hitValue_Y;
        data[2] = hitValue_Z;
        data[3] = hitValue_W;
        imageFile.write((char*) data, 3 * sizeof(float));
        imageFile.write((char*) (&offset), sizeof(uint32_t));
        imageFile.flush();

        // imageFile << "(" << gl_LaunchIDEXT_X << ", " << gl_LaunchIDEXT_Y << ") : (";
        // imageFile << hitValue_X << ", " << hitValue_Y << ", " << hitValue_Z << ", " << hitValue_W << ")\n";
    }

    TXL_DPRINTF("Setting transaction for image_store\n");
    thread->set_txl_transactions(transaction);

    // // if(std::abs(hitValue_X - rayDebugGPUData[gl_LaunchIDEXT_X][gl_LaunchIDEXT_Y].hitValue.x) > 0.0001 || 
    // //     std::abs(hitValue_Y - rayDebugGPUData[gl_LaunchIDEXT_X][gl_LaunchIDEXT_Y].hitValue.y) > 0.0001 ||
    // //     std::abs(hitValue_Z - rayDebugGPUData[gl_LaunchIDEXT_X][gl_LaunchIDEXT_Y].hitValue.z) > 0.0001)
    // //     {
    // //         printf("wrong value. (%d, %d): (%f, %f, %f)\n"
    // //                 , gl_LaunchIDEXT_X, gl_LaunchIDEXT_Y, hitValue_X, hitValue_Y, hitValue_Z);
    // //     }
    
    // // if (gl_LaunchIDEXT_X == 1070 && gl_LaunchIDEXT_Y == 220)
    // //     printf("this one has wrong value\n");

    // // if(hitValue_X > 1 || hitValue_Y > 1 || hitValue_Z > 1)
    // // {
    // //     printf("this one has wrong value.\n");
    // // }
#elif defined(MESA_USE_LVPIPE_DRIVER)
    assert(desc->type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);

    struct lvp_image *image = (struct lvp_image *)desc->info.image_view.image;
    VkFormat vk_format = image->vk.format;
    assert(image != NULL);
    VSIM_DPRINTF("gpgpusim: image_store to %s at %p\n", image->vk.base.object_name, image);

    Pixel pixel = Pixel(hitValue_X, hitValue_Y, hitValue_Z, hitValue_W);

    // Store image
    if(writeImageBinary)
    {
        std::string image_name(image->vk.base.object_name);
        if (outputImages.find(image_name) == outputImages.end()) {
            // Get timestamp string
            time_t curr_time;
            time(&curr_time);
            char *timestamp = ctime(&curr_time);
            char *s = timestamp;
            while (*s) {
                if (*s == ' ' || *s == '\t' || *s == ':') *s = '-';
                if (*s == '\n' || *s == '\r') *s = 0;
                s++;
            }
            std::string timestamp_str(timestamp);
            outputImages[image_name] = image_name + timestamp_str + ".txt";
            printf("gpgpusim: saving image %s to %s\n", image_name, outputImages[image_name]);
        }
        std::string image_file_name = outputImages[image_name];
        FILE* fp;
        fp = fopen(image_file_name.c_str(), "a+");
        fprintf(fp, "[%3d, %3d]: ", gl_LaunchIDEXT_X, gl_LaunchIDEXT_Y);
        fprintf(fp, "rgba(%3.0f, %3.0f, %3.0f, %5.3f)\n", hitValue_X * 255, hitValue_Y * 255, hitValue_Z * 255, hitValue_W);
        fclose(fp);
    }

    // Setup transaction record for timing model
    ImageMemoryTransactionRecord transaction;
    transaction.type = ImageTransactionType::IMAGE_STORE;

    VkImageTiling tiling = image->vk.tiling;
    uint32_t width = image->vk.extent.width;
    uint32_t height = image->vk.extent.height;
    uint32_t pixelX = gl_LaunchIDEXT_X;
    uint32_t pixelY = gl_LaunchIDEXT_Y;

    transaction.address = (void *)image;

    // Size of image_store content depends on data type
    switch (vk_format) {
        case VK_FORMAT_R32G32B32A32_SFLOAT:
            transaction.size = 16;
            break; 

        case VK_FORMAT_B8G8R8A8_UNORM:
            transaction.size = 4;
            break;

        default:
            printf("gpgpusim: unsupported image format option %d\n", vk_format);
            abort();
    }

    switch (tiling) {
        // Just an arbitrary tiling (TODO: Find a better tiling option)
        case VK_IMAGE_TILING_OPTIMAL:
        {
            uint32_t tileWidth = 16;
            uint32_t tileHeight = 16;

            uint32_t nTileX = ceil(width / tileWidth);
            uint32_t tileX = floor(pixelX / tileWidth);
            uint32_t tileY = floor(pixelY / tileHeight);

            uint32_t tileOffset = tileWidth * tileHeight * (tileY * nTileX + tileX);
            uint32_t pixelOffset = (pixelY % tileHeight) * tileWidth + (pixelX % tileWidth);

            transaction.address = (void *) image + ((tileOffset + pixelOffset) * transaction.size);
            break;
        }
        // Linear
        case VK_IMAGE_TILING_LINEAR:
        {
            uint32_t offset = pixelY * width + pixelX;
            transaction.address = (void *) image + offset * transaction.size;
            break;
        }
        default:
        {
            printf("gpgpusim: unsupported image tiling option %d\n", tiling);
            abort();
        }
    }


    TXL_DPRINTF("Setting transaction for image_store\n");
    thread->set_txl_transactions(transaction);

    // store_image_pixel(image, gl_LaunchIDEXT_X, gl_LaunchIDEXT_Y, 0, pixel, transaction);
#endif
}

// variable_decleration_entry* VulkanRayTracing::get_variable_decleration_entry(std::string name, ptx_thread_info *thread)
// {
//     std::vector<variable_decleration_entry>& table = thread->RT_thread_data->variable_decleration_table;
//     for (int i = 0; i < table.size(); i++) {
//         if (table[i].name == name) {
//             assert (table[i].address != NULL);
//             return &(table[i]);
//         }
//     }
//     return NULL;
// }

// void VulkanRayTracing::add_variable_decleration_entry(uint64_t type, std::string name, uint64_t address, uint32_t size, ptx_thread_info *thread)
// {
//     variable_decleration_entry entry;

//     entry.type = type;
//     entry.name = name;
//     entry.address = address;
//     entry.size = size;
//     thread->RT_thread_data->variable_decleration_table.push_back(entry);
// }


void VulkanRayTracing::dumpTextures(struct DESCRIPTOR_STRUCT *desc, uint32_t setID, uint32_t binding, VkDescriptorType type)
{
#if defined(MESA_USE_INTEL_DRIVER)
    DESCRIPTOR_STRUCT *desc_offset = ((DESCRIPTOR_STRUCT*)((void*)desc)); // offset for raytracing_extended
    struct anv_image_view *image_view =  desc_offset->image_view;
    struct anv_sampler *sampler = desc_offset->sampler;

    const struct anv_image *image = image_view->image;
    assert(image->n_planes == 1);
    assert(image->samples == 1);
    assert(image->tiling == VK_IMAGE_TILING_OPTIMAL);
    assert(image->planes[0].surface.isl.tiling == ISL_TILING_Y0);
    assert(sampler->conversion == NULL);

    uint8_t* address = anv_address_map(image->planes[0].address);
    uint32_t image_extent_width = image->extent.width;
    uint32_t image_extent_height = image->extent.height;
    VkFormat format = image->vk_format;
    uint64_t size = image->size;

    VkFilter filter;
    if(sampler->conversion == NULL)
        filter = VK_FILTER_NEAREST;

    // Data to dump
    FILE *fp;
    char *mesa_root = getenv("MESA_ROOT");
    char *filePath = "gpgpusimShaders/";
    char *extension = ".vkdescrptorsettexturedata";

    int VkDescriptorTypeNum;

    switch (type)
    {
        case VK_DESCRIPTOR_TYPE_SAMPLER:
            VkDescriptorTypeNum = 0;
            break;
        case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
            VkDescriptorTypeNum = 1;
            break;
        case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
            VkDescriptorTypeNum = 2;
            break;
        case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
            VkDescriptorTypeNum = 10;
            break;
        default:
            abort(); // should not be here!
    }

    // Texture data
    char fullPath[200];
    snprintf(fullPath, sizeof(fullPath), "%s%s%d_%d.vktexturedata", mesa_root, filePath, setID, binding);
    // File name format: setID_descID.vktexturedata

    fp = fopen(fullPath, "wb+");
    fwrite(address, 1, size, fp);
    fclose(fp);

    // Texture metadata
    snprintf(fullPath, sizeof(fullPath), "%s%s%d_%d.vktexturemetadata", mesa_root, filePath, setID, binding);
    fp = fopen(fullPath, "w+");
    // File name format: setID_descID.vktexturemetadata

    fprintf(fp, "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d", size, 
                                                 image_extent_width, 
                                                 image_extent_height, 
                                                 format, 
                                                 VkDescriptorTypeNum, 
                                                 image->n_planes, 
                                                 image->samples, 
                                                 image->tiling, 
                                                 image->planes[0].surface.isl.tiling,
                                                 image->planes[0].surface.isl.row_pitch_B,
                                                 filter);
    fclose(fp);
#elif defined(MESA_USE_LVPIPE_DRIVER)
    printf("gpgpusim: dumpTextures not implemented for lavapipe.\n");
    abort();

#endif

}


void VulkanRayTracing::dumpStorageImage(struct DESCRIPTOR_STRUCT *desc, uint32_t setID, uint32_t binding, VkDescriptorType type)
{
#if defined(MESA_USE_INTEL_DRIVER)
    assert(type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);

    assert(desc->sampler == NULL);

    struct anv_image_view *image_view = desc->image_view;
    assert(image_view != NULL);
    struct anv_image * image = image_view->image;
    assert(image->n_planes == 1);
    assert(image->samples == 1);

    void* mem_address = anv_address_map(image->planes[0].address);

    VkFormat format = image->vk_format;
    VkImageTiling tiling = image->tiling;
    isl_tiling isl_tiling_mode = image->planes[0].surface.isl.tiling;
    uint32_t row_pitch_B  = image->planes[0].surface.isl.row_pitch_B;

    uint32_t width = image->extent.width;
    uint32_t height = image->extent.height;

    // Dump storage image metadata
    FILE *fp;
    char *mesa_root = getenv("MESA_ROOT");
    char *filePath = "gpgpusimShaders/";
    char *extension = ".vkdescrptorsetdata";

    int VkDescriptorTypeNum = 3;

    char fullPath[200];
    snprintf(fullPath, sizeof(fullPath), "%s%s%d_%d.vkstorageimagemetadata", mesa_root, filePath, setID, binding);
    fp = fopen(fullPath, "w+");
    // File name format: setID_descID.vktexturemetadata

    fprintf(fp, "%d,%d,%d,%d,%d,%d,%d,%d,%d",   width, 
                                                height, 
                                                format, 
                                                VkDescriptorTypeNum, 
                                                image->n_planes, 
                                                image->samples, 
                                                tiling, 
                                                isl_tiling_mode,
                                                row_pitch_B);
    fclose(fp);
#elif defined(MESA_USE_LVPIPE_DRIVER)
    printf("gpgpusim: dumpStorageImage not implemented for lavapipe.\n");
    abort();

#endif
}


void VulkanRayTracing::dump_descriptor_set_for_AS(uint32_t setID, uint32_t descID, void *address, uint32_t desc_size, VkDescriptorType type, uint32_t backwards_range, uint32_t forward_range, bool split_files, VkAccelerationStructureKHR _topLevelAS)
{
    FILE *fp;
    char *mesa_root = getenv("MESA_ROOT");
    char *filePath = "gpgpusimShaders/";
    char *extension = ".vkdescrptorsetdata";

    int VkDescriptorTypeNum;

    switch (type)
    {
        case VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR:
            VkDescriptorTypeNum = 1000150000;
            break;
        case VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_NV:
            VkDescriptorTypeNum = 1000165000;
            break;
        default:
            abort(); // should not be here!
    }

    char fullPath[200];
    int result;

    int64_t max_backwards; // negative number
    int64_t min_backwards; // negative number
    int64_t min_forwards;
    int64_t max_forwards;
    int64_t back_buffer_amount = 0; //20kB buffer just in case
    int64_t front_buffer_amount = 1024*20; //20kB buffer just in case
    findOffsetBounds(max_backwards, min_backwards, min_forwards, max_forwards, _topLevelAS);

    bool haveBackwards = (max_backwards != 0) && (min_backwards != 0);
    bool haveForwards = (min_forwards != 0) && (max_forwards != 0);
    
    if (split_files) // Used when the AS is too far apart between top tree and BVHAddress and cant just dump the whole thing
    {
        // Main Top Level
        snprintf(fullPath, sizeof(fullPath), "%s%s%d_%d.asmain", mesa_root, filePath, setID, descID);
        fp = fopen(fullPath, "wb+");
        result = fwrite(address, 1, desc_size, fp);
        assert(result == desc_size);
        fclose(fp);

        // Bot level whose address is smaller than top level
        if (haveBackwards)
        {
            snprintf(fullPath, sizeof(fullPath), "%s%s%d_%d.asback", mesa_root, filePath, setID, descID);
            fp = fopen(fullPath, "wb+");
            result = fwrite(address + max_backwards, 1, min_backwards - max_backwards + back_buffer_amount, fp);
            assert(result == min_backwards - max_backwards + back_buffer_amount);
            fclose(fp);
        }

        // Bot level whose address is larger than top level
        if (haveForwards)
        {
            snprintf(fullPath, sizeof(fullPath), "%s%s%d_%d.asfront", mesa_root, filePath, setID, descID);
            fp = fopen(fullPath, "wb+");
            result = fwrite(address + min_forwards, 1, max_forwards - min_forwards + front_buffer_amount, fp);
            assert(result == max_forwards - min_forwards + front_buffer_amount);
            fclose(fp);
        }

        // AS metadata
        snprintf(fullPath, sizeof(fullPath), "%s%s%d_%d.asmetadata", mesa_root, filePath, setID, descID);
        fp = fopen(fullPath, "w+");
        fprintf(fp, "%d,%d,%ld,%ld,%ld,%ld,%ld,%ld,%d,%d", desc_size,
                                                            VkDescriptorTypeNum,
                                                            max_backwards,
                                                            min_backwards,
                                                            min_forwards,
                                                            max_forwards,
                                                            back_buffer_amount,
                                                            front_buffer_amount,
                                                            haveBackwards,
                                                            haveForwards);
        fclose(fp);

        
        // uint64_t total_size = (desc_size + backwards_range + forward_range);
        // uint64_t chunk_size = 1024*1024*20; // 20MB chunks
        // int totalFiles =  (total_size + chunk_size) / chunk_size; // rounds up

        // for (int i = 0; i < totalFiles; i++)
        // {
        //     // if split_files is 1, then look at the next number to see what the file part number is
        //     snprintf(fullPath, sizeof(fullPath), "%s%s%d_%d_%d_%d_%d_%d_%d_%d%s", mesa_root, filePath, setID, descID, desc_size, VkDescriptorTypeNum, backwards_range, forward_range, split_files, i, extension);
        //     fp = fopen(fullPath, "wb+");
        //     int result = fwrite(address-(uint64_t)backwards_range + chunk_size * i, 1, chunk_size, fp);
        //     printf("File part %d, %d bytes written, starting address 0x%.12" PRIXPTR "\n", i, result, (uintptr_t)(address-(uint64_t)backwards_range + chunk_size * i));
        //     fclose(fp);
        // }
    }
    else 
    {
        snprintf(fullPath, sizeof(fullPath), "%s%s%d_%d_%d_%d_%d_%d%s", mesa_root, filePath, setID, descID, desc_size, VkDescriptorTypeNum, backwards_range, forward_range, extension);
        // File name format: setID_descID_SizeInBytes_VkDescriptorType_desired_range.vkdescrptorsetdata

        fp = fopen(fullPath, "wb+");
        int result = fwrite(address-(uint64_t)backwards_range, 1, desc_size + backwards_range + forward_range, fp);
        fclose(fp);
    }
}


void VulkanRayTracing::dump_descriptor_set(uint32_t setID, uint32_t descID, void *address, uint32_t size, VkDescriptorType type)
{
    FILE *fp;
    char *mesa_root = getenv("MESA_ROOT");
    char *filePath = "gpgpusimShaders/";
    char *extension = ".vkdescrptorsetdata";

    int VkDescriptorTypeNum;

    switch (type)
    {
        case VK_DESCRIPTOR_TYPE_SAMPLER:
            VkDescriptorTypeNum = 0;
            break;
        case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
            VkDescriptorTypeNum = 1;
            break;
        case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
            VkDescriptorTypeNum = 2;
            break;
        case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
            VkDescriptorTypeNum = 3;
            break;
        case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
            VkDescriptorTypeNum = 4;
            break;
        case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
            VkDescriptorTypeNum = 5;
            break;
        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
            VkDescriptorTypeNum = 6;
            break;
        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
            VkDescriptorTypeNum = 7;
            break;
        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
            VkDescriptorTypeNum = 8;
            break;
        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
            VkDescriptorTypeNum = 9;
            break;
        case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
            VkDescriptorTypeNum = 10;
            break;
        case VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK_EXT:
            VkDescriptorTypeNum = 1000138000;
            break;
        case VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR:
            VkDescriptorTypeNum = 1000150000;
            break;
        case VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_NV:
            VkDescriptorTypeNum = 1000165000;
            break;
        case VK_DESCRIPTOR_TYPE_MUTABLE_VALVE:
            VkDescriptorTypeNum = 1000351000;
            break;
        case VK_DESCRIPTOR_TYPE_MAX_ENUM:
            VkDescriptorTypeNum = 0x7FFFFFF;
            break;
        default:
            abort(); // should not be here!
    }

    char fullPath[200];
    snprintf(fullPath, sizeof(fullPath), "%s%s%d_%d_%d_%d%s", mesa_root, filePath, setID, descID, size, VkDescriptorTypeNum, extension);
    // File name format: setID_descID_SizeInBytes_VkDescriptorType.vkdescrptorsetdata

    fp = fopen(fullPath, "wb+");
    fwrite(address, 1, size, fp);
    fclose(fp);
}


void VulkanRayTracing::dump_descriptor_sets(struct DESCRIPTOR_SET_STRUCT *set)
{
#if defined(MESA_USE_INTEL_DRIVER)
   for(int i = 0; i < set->descriptor_count; i++)
   {
       if(i == 3 || i > 9)
       {
            // for some reason raytracing_extended skipped binding = 3
            // and somehow they have 34 descriptor sets but only 10 are used
            // so we just skip those
            continue;
       }

        struct DESCRIPTOR_SET_STRUCT* set = VulkanRayTracing::descriptorSet;

        const struct DESCRIPTOR_LAYOUT_STRUCT *bind_layout = &set->layout->binding[i];
        struct DESCRIPTOR_STRUCT *desc = &set->descriptors[bind_layout->descriptor_index];
        void *desc_map = set->desc_mem.map + bind_layout->descriptor_offset;

        assert(desc->type == bind_layout->type);

        switch (desc->type)
        {
            case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
            {
                //return (void *)(desc);
                dumpStorageImage(desc, 0, i, desc->type);
                break;
            }
            case VK_DESCRIPTOR_TYPE_SAMPLER:
            case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
            case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
            case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
            {
                //return desc;
                dumpTextures(desc, 0, i, desc->type);
                break;
            }

            case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
            case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
                assert(0);
                break;

            case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
            case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
            case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
            case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
            {
                if (desc->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC ||
                    desc->type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC)
                {
                    // MRS_TODO: account for desc->offset?
                    //return anv_address_map(desc->buffer->address);
                    dump_descriptor_set(0, i, anv_address_map(desc->buffer->address), set->descriptors[i].buffer->size, set->descriptors[i].type);
                    break;
                }
                else
                {
                    struct anv_buffer_view *bview = &set->buffer_views[bind_layout->buffer_view_index];
                    //return anv_address_map(bview->address);
                    dump_descriptor_set(0, i, anv_address_map(bview->address), bview->range, set->descriptors[i].type);
                    break;
                }
            }

            case VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK_EXT:
                assert(0);
                break;

            case VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR:
            case VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_NV:
            {
                struct anv_address_range_descriptor *desc_data = desc_map;
                //return (void *)(desc_data->address);
                //dump_descriptor_set_for_AS(0, i, (void *)(desc_data->address), desc_data->range, set->descriptors[i].type, 1024*1024*10, 1024*1024*10, true);
                break;
            }

            default:
                assert(0);
                break;
        }
   }
#elif defined(MESA_USE_LVPIPE_DRIVER)
    printf("gpgpusim: dump_descriptor_sets not implemented for lavapipe.\n");
    abort();

#endif
}

void VulkanRayTracing::dump_AS(struct DESCRIPTOR_SET_STRUCT *set, VkAccelerationStructureKHR _topLevelAS)
{
#if defined(MESA_USE_INTEL_DRIVER)
   for(int i = 0; i < set->descriptor_count; i++)
   {
       if(i == 3 || i > 9)
       {
            // for some reason raytracing_extended skipped binding = 3
            // and somehow they have 34 descriptor sets but only 10 are used
            // so we just skip those
            continue;
       }

        struct DESCRIPTOR_SET_STRUCT* set = VulkanRayTracing::descriptorSet;

        const struct DESCRIPTOR_LAYOUT_STRUCT *bind_layout = &set->layout->binding[i];
        struct DESCRIPTOR_STRUCT *desc = &set->descriptors[bind_layout->descriptor_index];
        void *desc_map = set->desc_mem.map + bind_layout->descriptor_offset;

        assert(desc->type == bind_layout->type);

        switch (desc->type)
        {
            case VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR:
            case VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_NV:
            {
                struct anv_address_range_descriptor *desc_data = desc_map;
                //return (void *)(desc_data->address);
                dump_descriptor_set_for_AS(0, i, (void *)(desc_data->address), desc_data->range, set->descriptors[i].type, 1024*1024*10, 1024*1024*10, true, _topLevelAS);
                break;
            }

            default:
                break;
        }
    }
#elif defined(MESA_USE_LVPIPE_DRIVER)
    printf("gpgpusim: dump_AS not implemented for lavapipe.\n");
    abort();

#endif
}

void VulkanRayTracing::dump_callparams_and_sbt(void *raygen_sbt, void *miss_sbt, void *hit_sbt, void *callable_sbt, bool is_indirect, uint32_t launch_width, uint32_t launch_height, uint32_t launch_depth, uint32_t launch_size_addr)
{
    FILE *fp;
    char *mesa_root = getenv("MESA_ROOT");
    char *filePath = "gpgpusimShaders/";

    char call_params_filename [200];
    int trace_rays_call_count = 0; // just a placeholder for now
    snprintf(call_params_filename, sizeof(call_params_filename), "%s%s%d.callparams", mesa_root, filePath, trace_rays_call_count);
    fp = fopen(call_params_filename, "w+");
    fprintf(fp, "%d,%d,%d,%d,%lu", is_indirect, launch_width, launch_height, launch_depth, launch_size_addr);
    fclose(fp);

    // TODO: Is the size always 32?
    int sbt_size = 64 *sizeof(uint64_t);
    if (raygen_sbt) {
        char raygen_sbt_filename [200];
        snprintf(raygen_sbt_filename, sizeof(raygen_sbt_filename), "%s%s%d.raygensbt", mesa_root, filePath, trace_rays_call_count);
        fp = fopen(raygen_sbt_filename, "wb+");
        fwrite(raygen_sbt, 1, sbt_size, fp); // max is 32 bytes according to struct anv_rt_shader_group.handle
        fclose(fp);
    }

    if (miss_sbt) {
        char miss_sbt_filename [200];
        snprintf(miss_sbt_filename, sizeof(miss_sbt_filename), "%s%s%d.misssbt", mesa_root, filePath, trace_rays_call_count);
        fp = fopen(miss_sbt_filename, "wb+");
        fwrite(miss_sbt, 1, sbt_size, fp); // max is 32 bytes according to struct anv_rt_shader_group.handle
        fclose(fp);
    }

    if (hit_sbt) {
        char hit_sbt_filename [200];
        snprintf(hit_sbt_filename, sizeof(hit_sbt_filename), "%s%s%d.hitsbt", mesa_root, filePath, trace_rays_call_count);
        fp = fopen(hit_sbt_filename, "wb+");
        fwrite(hit_sbt, 1, sbt_size, fp); // max is 32 bytes according to struct anv_rt_shader_group.handle
        fclose(fp);
    }

    if (callable_sbt) {
        char callable_sbt_filename [200];
        snprintf(callable_sbt_filename, sizeof(callable_sbt_filename), "%s%s%d.callablesbt", mesa_root, filePath, trace_rays_call_count);
        fp = fopen(callable_sbt_filename, "wb+");
        fwrite(callable_sbt, 1, sbt_size, fp); // max is 32 bytes according to struct anv_rt_shader_group.handle
        fclose(fp);
    }
}

void VulkanRayTracing::setStorageImageFromLauncher(void *address, 
                                                void *deviceAddress, 
                                                uint32_t setID, 
                                                uint32_t descID, 
                                                uint32_t width,
                                                uint32_t height,
                                                VkFormat format,
                                                uint32_t VkDescriptorTypeNum,
                                                uint32_t n_planes,
                                                uint32_t n_samples,
                                                VkImageTiling tiling,
                                                uint32_t isl_tiling_mode, 
                                                uint32_t row_pitch_B)
{
    storage_image_metadata *storage_image = new storage_image_metadata;
    storage_image->address = address;
    storage_image->setID = setID;
    storage_image->descID = descID;
    storage_image->width = width;
    storage_image->height = height;
    storage_image->format = format;
    storage_image->VkDescriptorTypeNum = VkDescriptorTypeNum;
    storage_image->n_planes = n_planes;
    storage_image->n_samples = n_samples;
    storage_image->tiling = tiling;
    storage_image->isl_tiling_mode = isl_tiling_mode; 
    storage_image->row_pitch_B = row_pitch_B;
    storage_image->deviceAddress = deviceAddress;

    launcher_descriptorSets[setID][descID] = (void*) storage_image;
    launcher_deviceDescriptorSets[setID][descID] = (void*) storage_image;
}

void VulkanRayTracing::setTextureFromLauncher(void *address, 
                                            void *deviceAddress, 
                                            uint32_t setID, 
                                            uint32_t descID, 
                                            uint64_t size,
                                            uint32_t width,
                                            uint32_t height,
                                            VkFormat format,
                                            uint32_t VkDescriptorTypeNum,
                                            uint32_t n_planes,
                                            uint32_t n_samples,
                                            VkImageTiling tiling,
                                            uint32_t isl_tiling_mode,
                                            uint32_t row_pitch_B,
                                            uint32_t filter)
{
    texture_metadata *texture = new texture_metadata;
    texture->address = address;
    texture->setID = setID;
    texture->descID = descID;
    texture->size = size;
    texture->width = width;
    texture->height = height;
    texture->format = format;
    texture->VkDescriptorTypeNum = VkDescriptorTypeNum;
    texture->n_planes = n_planes;
    texture->n_samples = n_samples;
    texture->tiling = tiling;
    texture->isl_tiling_mode = isl_tiling_mode;
    texture->row_pitch_B = row_pitch_B;
    texture->filter = filter;
    texture->deviceAddress = deviceAddress;

    launcher_descriptorSets[setID][descID] = (void*) texture;
    launcher_deviceDescriptorSets[setID][descID] = (void*) texture;
}

void VulkanRayTracing::pass_child_addr(void *address)
{
    child_addrs_from_driver.push_back(address);
}

void VulkanRayTracing::findOffsetBounds(int64_t &max_backwards, int64_t &min_backwards, int64_t &min_forwards, int64_t &max_forwards, VkAccelerationStructureKHR _topLevelAS)
{
    // uint64_t current_min_backwards = 0;
    // uint64_t current_max_backwards = 0;
    // uint64_t current_min_forwards = 0;
    // uint64_t current_max_forwards = 0;
    int64_t offset;

    std::vector<int64_t> positive_offsets;
    std::vector<int64_t> negative_offsets;

    for (auto addr : child_addrs_from_driver)
    {
        offset = (uint64_t)addr - (uint64_t)_topLevelAS;
        if (offset >= 0)
            positive_offsets.push_back(offset);
        else
            negative_offsets.push_back(offset);
    }

    sort(positive_offsets.begin(), positive_offsets.end());
    sort(negative_offsets.begin(), negative_offsets.end());

    if (negative_offsets.size() > 0)
    {
        max_backwards = negative_offsets.front();
        min_backwards = negative_offsets.back();
    }
    else
    {
        max_backwards = 0;
        min_backwards = 0;
    }

    if (positive_offsets.size() > 0)
    {
        min_forwards = positive_offsets.front();
        max_forwards = positive_offsets.back();
    }
    else
    {
        min_forwards = 0;
        max_forwards = 0;
    }
}


void* VulkanRayTracing::gpgpusim_alloc(uint32_t size)
{
    if(!use_external_launcher) {
        return malloc(size);
    }
    else {
        gpgpu_context *ctx = GPGPU_Context();
        CUctx_st *context = GPGPUSim_Context(ctx);
        void* devPtr = context->get_device()->get_gpgpu()->gpu_malloc(size);
        if (g_debug_execution >= 3) {
            printf("GPGPU-Sim PTX: gpgpusim_allocing %zu bytes starting at 0x%llx..\n",
                size, (unsigned long long)devPtr);
            ctx->api->g_mallocPtr_Size[(unsigned long long)devPtr] = size;
        }
        assert(devPtr);
        return devPtr;
    }
}