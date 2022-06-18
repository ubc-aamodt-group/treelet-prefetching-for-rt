#ifndef VULKAN_RAY_TRACING_H
#define VULKAN_RAY_TRACING_H

#include "vulkan/vulkan.h"
#include "vulkan/vulkan_intel.h"

#include "vulkan/anv_acceleration_structure.h"
#include "intersection_table.h"
#include "compiler/spirv/spirv.h"

// #include "ptx_ir.h"
#include "ptx_ir.h"
#include "../../libcuda/gpgpu_context.h"
#include "../abstract_hardware_model.h"
#include "compiler/shader_enums.h"
#include <fstream>
#include <cmath>

#define MAX(a,b) (((a)>(b))?(a):(b))
#define MIN(a,b) (((a)<(b))?(a):(b))
#define MIN_MAX(a,b,c) MAX(MIN((a), (b)), (c))
#define MAX_MIN(a,b,c) MIN(MAX((a), (b)), (c))

// enum class TransactionType {
//     BVH_STRUCTURE,
//     BVH_INTERNAL_NODE,
//     BVH_INSTANCE_LEAF,
//     BVH_PRIMITIVE_LEAF_DESCRIPTOR,
//     BVH_QUAD_LEAF,
//     BVH_PROCEDURAL_LEAF,
//     Intersection_Table_Load,
// };

// typedef struct MemoryTransactionRecord {
//     MemoryTransactionRecord(void* address, uint32_t size, TransactionType type)
//     : address(address), size(size), type(type) {}
//     void* address;
//     uint32_t size;
//     TransactionType type;
// } MemoryTransactionRecord;
// typedef struct float4 {
//     float x, y, z, w;
// } float4;

// enum class StoreTransactionType {
//     Intersection_Table_Store,
//     Traversal_Results,
// };

// typedef struct MemoryStoreTransactionRecord {
//     MemoryStoreTransactionRecord(void* address, uint32_t size, StoreTransactionType type)
//     : address(address), size(size), type(type) {}
//     void* address;
//     uint32_t size;
//     StoreTransactionType type;
// } MemoryStoreTransactionRecord;



extern bool use_external_launcher;

typedef struct float4x4 {
  float m[4][4];

  float4 operator*(const float4& _vec) const
  {
    float vec[] = {_vec.x, _vec.y, _vec.z, _vec.w};
    float res[] = {0, 0, 0, 0};
    for(int i = 0; i < 4; i++)
        for(int j = 0; j < 4; j++)
            res[i] += this->m[j][i] * vec[j];
    return {res[0], res[1], res[2], res[3]};
  }
} float4x4;

typedef struct RayDebugGPUData
{
    bool valid;
    int launchIDx;
    int launchIDy;
    int instanceCustomIndex;
    int primitiveID;
    float3 v0pos;
    float3 v1pos;
    float3 v2pos;
    float3 attribs;
    float3 P_object;
    float3 P; //world intersection point
    float3 N_object;
    float3 N;
    float NdotL;
    float3 hitValue;
} RayDebugGPUData;

// float4 operator*(const float4& _vec, const float4x4& matrix)
// {
//     float vec[] = {_vec.x, _vec.y, _vec.z, _vec.w};
//     float res[] = {0, 0, 0, 0};
//     for(int i = 0; i < 4; i++)
//         for(int j = 0; j < 4; j++)
//             res[i] += matrix.m[j][i] * vec[j];
//     return {res[0], res[1], res[2], res[3]};
// }


typedef struct Descriptor
{
    uint32_t setID;
    uint32_t descID;
    void *address;
    uint32_t size;
    VkDescriptorType type;
} Descriptor;

typedef struct variable_decleration_entry{
  uint64_t type;
  std::string name;
  uint64_t address;
  uint32_t size;
} variable_decleration_entry;

typedef struct Hit_data{
    VkGeometryTypeKHR geometryType;
    float world_min_thit;
    uint32_t geometry_index;
    uint32_t primitive_index;
    float3 intersection_point;
    float3 barycentric_coordinates;
    int32_t hitGroupIndex; // Shader ID of the closest hit for procedural geometries

    uint32_t instance_index;
    float4x4 worldToObjectMatrix;
    float4x4 objectToWorldMatrix;
} Hit_data;

typedef struct shader_stage_info {
    uint32_t ID;
    gl_shader_stage type;
    char* function_name;
} shader_stage_info;

typedef struct Traversal_data {
    bool hit_geometry;
    Hit_data closest_hit;
    float3 ray_world_direction;
    float3 ray_world_origin;
    float Tmin;
    float Tmax;
    int32_t current_shader_counter; // set to shader_counter in call_intersection and -1 in call_miss and call_closest_hit

    uint32_t rayFlags;
    uint32_t cullMask;
    uint32_t sbtRecordOffset;
    uint32_t sbtRecordStride;
    uint32_t missIndex;
} Traversal_data;


typedef struct StackEntry {
    uint8_t* addr;
    bool topLevel;
    bool leaf;
    StackEntry(uint8_t* addr, bool topLevel, bool leaf): addr(addr), topLevel(topLevel), leaf(leaf) {}

    bool operator<(const StackEntry &o)  const
    {
        return addr < o.addr;
    }

    bool operator==(const StackEntry &o) const
    {
        return addr == o.addr && topLevel == o.topLevel && leaf == o.leaf;
    }
} StackEntry;

typedef struct Vulkan_RT_thread_data {
    std::vector<variable_decleration_entry> variable_decleration_table;

    std::vector<Traversal_data> traversal_data;


    variable_decleration_entry* get_variable_decleration_entry(uint64_t type, std::string name, uint32_t size) {
        if(type == 8192)
            return get_hitAttribute();
        
        for (int i = 0; i < variable_decleration_table.size(); i++) {
            if (variable_decleration_table[i].name == name) {
                assert (variable_decleration_table[i].address != NULL);
                return &(variable_decleration_table[i]);
            }
        }
        return NULL;
    }

    uint64_t add_variable_decleration_entry(uint64_t type, std::string name, uint32_t size) {
        variable_decleration_entry entry;
        entry.type = type;
        entry.name = name;
        entry.address = (uint64_t) malloc(size);
        entry.size = size;
        variable_decleration_table.push_back(entry);

        return entry.address;
    }

    variable_decleration_entry* get_hitAttribute() {
        variable_decleration_entry* hitAttribute = NULL;
        for (int i = 0; i < variable_decleration_table.size(); i++) {
            if (variable_decleration_table[i].type == 8192) {
                assert (variable_decleration_table[i].address != NULL);
                assert (hitAttribute == NULL); // There should be only 1 hitAttribute
                hitAttribute = &(variable_decleration_table[i]);
            }
        }
        return hitAttribute;
    }

    void set_hitAttribute(float3 barycentric) {
        variable_decleration_entry* hitAttribute = get_hitAttribute();
        float* address;
        if(hitAttribute == NULL) {
            address = (float*)add_variable_decleration_entry(8192, "attribs", 36);
        }
        else {
            assert (hitAttribute->type == 8192);
            assert (hitAttribute->address != NULL);
            // hitAttribute->name = name;
            address = (float*)(hitAttribute->address);
        }
        address[0] = barycentric.x;
        address[1] = barycentric.y;
        address[2] = barycentric.z;
    }
} Vulkan_RT_thread_data;

// For launcher
typedef struct storage_image_metadata
{
    void *address;
    uint32_t setID;
    uint32_t descID;
    uint32_t width;
    uint32_t height;
    VkFormat format;
    uint32_t VkDescriptorTypeNum;
    uint32_t n_planes;
    uint32_t n_samples;
    VkImageTiling tiling;
    uint32_t isl_tiling_mode; 
    uint32_t row_pitch_B;
} storage_image_metadata;

typedef struct texture_metadata
{
    void *address;
    uint32_t setID;
    uint32_t descID;
    uint64_t size;
    uint32_t width;
    uint32_t height;
    VkFormat format;
    uint32_t VkDescriptorTypeNum;
    uint32_t n_planes;
    uint32_t n_samples;
    VkImageTiling tiling;
    uint32_t isl_tiling_mode;
    uint32_t row_pitch_B;
    VkFilter filter;
} texture_metadata;


struct anv_descriptor_set;
struct anv_descriptor;

class VulkanRayTracing
{
private:
    static VkRayTracingPipelineCreateInfoKHR* pCreateInfos;
    static VkAccelerationStructureGeometryKHR* pGeometries;
    static uint32_t geometryCount;
    static VkAccelerationStructureKHR topLevelAS;
    static std::vector<std::vector<Descriptor> > descriptors;
    static std::ofstream imageFile;
    static bool firstTime;
    static struct anv_descriptor_set *descriptorSet;

    // For Launcher
    static void* launcher_descriptorSets[1][10];
    static std::vector<void*> child_addrs_from_driver;
    static void *child_addr_from_driver;
    static bool _init_;
public:
    // static RayDebugGPUData rayDebugGPUData[2000][2000];
    static warp_intersection_table*** intersection_table;
    static const IntersectionTableType intersectionTableType = IntersectionTableType::Baseline;

    // Treelets
    static std::map<StackEntry, std::vector<StackEntry>> treelet_roots; // <treelet node, vector of children that belong to this treelet>, just to look up if an address is a treelet root node or not
    static std::map<uint8_t*, std::vector<StackEntry>> treelet_roots_addr_only; // <address, vector of children that belong to this treelet>, just to look up if an address is a treelet root node or not
    static std::map<StackEntry, std::vector<StackEntry>> treelet_child_map; // Key: a treelet root node; Value: vector of child treelets of this treelet node
    static std::map<uint8_t*, std::vector<StackEntry>> treelet_addr_only_child_map; // Key: a treelet root node address; Value: vector of child treelets of this treelet node
    static std::map<uint8_t*, uint8_t*> node_map_addr_only;

private:
    static bool mt_ray_triangle_test(float3 p0, float3 p1, float3 p2, Ray ray_properties, float* thit);
    static float3 Barycentric(float3 p, float3 a, float3 b, float3 c);
    static std::vector<shader_stage_info> shaders;

    static void init(uint32_t launch_width, uint32_t launch_height);


public:
    static void traceRay( // called by raygen shader
                       VkAccelerationStructureKHR _topLevelAS,
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
                       ptx_thread_info *thread);
    static void endTraceRay(const ptx_instruction *pI, ptx_thread_info *thread);
    
    static void load_descriptor(const ptx_instruction *pI, ptx_thread_info *thread);

    static void setPipelineInfo(VkRayTracingPipelineCreateInfoKHR* pCreateInfos);
    static void setGeometries(VkAccelerationStructureGeometryKHR* pGeometries, uint32_t geometryCount);
    static void setAccelerationStructure(VkAccelerationStructureKHR accelerationStructure);
    static void setDescriptorSet(struct anv_descriptor_set *set);
    static void invoke_gpgpusim();
    static uint32_t registerShaders(char * shaderPath, gl_shader_stage shaderType);
    static void vkCmdTraceRaysKHR( // called by vulkan application
                      void *raygen_sbt,
                      void *miss_sbt,
                      void *hit_sbt,
                      void *callable_sbt,
                      bool is_indirect,
                      uint32_t launch_width,
                      uint32_t launch_height,
                      uint32_t launch_depth,
                      uint64_t launch_size_addr);
    static void callShader(const ptx_instruction *pI, ptx_thread_info *thread, function_info *target_func);
    static void callMissShader(const ptx_instruction *pI, ptx_thread_info *thread);
    static void callClosestHitShader(const ptx_instruction *pI, ptx_thread_info *thread);
    static void callIntersectionShader(const ptx_instruction *pI, ptx_thread_info *thread, uint32_t shader_counter);
    static void callAnyHitShader(const ptx_instruction *pI, ptx_thread_info *thread);
    static void setDescriptor(uint32_t setID, uint32_t descID, void *address, uint32_t size, VkDescriptorType type);
    static void* getDescriptorAddress(uint32_t setID, uint32_t binding);

    static void image_store(struct anv_descriptor* desc, uint32_t gl_LaunchIDEXT_X, uint32_t gl_LaunchIDEXT_Y, uint32_t gl_LaunchIDEXT_Z, uint32_t gl_LaunchsIDEXT_W, 
              float hitValue_X, float hitValue_Y, float hitValue_Z, float hitValue_W, const ptx_instruction *pI, ptx_thread_info *thread);
    static void getTexture(struct anv_descriptor *desc, float x, float y, float lod, float &c0, float &c1, float &c2, float &c3, std::vector<ImageMemoryTransactionRecord>& transactions, uint64_t launcher_offset = 0);
    static void image_load(struct anv_descriptor *desc, uint32_t x, uint32_t y, float &c0, float &c1, float &c2, float &c3);

    static void dump_descriptor_set(uint32_t setID, uint32_t descID, void *address, uint32_t size, VkDescriptorType type);
    static void dump_descriptor_set_for_AS(uint32_t setID, uint32_t descID, void *address, uint32_t desc_size, VkDescriptorType type, uint32_t backwards_range, uint32_t forward_range, bool split_files);
    static void dump_descriptor_sets(struct anv_descriptor_set *set);
    static void dump_callparams_and_sbt(void *raygen_sbt, void *miss_sbt, void *hit_sbt, void *callable_sbt, bool is_indirect, uint32_t launch_width, uint32_t launch_height, uint32_t launch_depth, uint32_t launch_size_addr);
    static void dumpTextures(struct anv_descriptor *desc, uint32_t setID, uint32_t binding, VkDescriptorType type);
    static void dumpStorageImage(struct anv_descriptor *desc, uint32_t setID, uint32_t binding, VkDescriptorType type);
    static void setDescriptorSetFromLauncher(void *address, uint32_t setID, uint32_t descID);
    static void setStorageImageFromLauncher(void *address, 
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
                                            uint32_t row_pitch_B);
    static void setTextureFromLauncher(void *address, 
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
                                       uint32_t filter);
    static void pass_child_addr(void *address);
    static void findOffsetBounds(int64_t &max_backwards, int64_t &min_backwards, int64_t &min_forwards, int64_t &max_forwards);
    static void createTreelets(VkAccelerationStructureKHR _topLevelAS, int maxBytesPerTreelet);
    static float calculateSAH(float3 lo, float3 hi);
    static bool isTreeletRoot(StackEntry node);
    static bool isTreeletRoot(uint8_t* addr);
    static uint8_t* addrToTreeletID(uint8_t* addr);
    static std::vector<StackEntry> treeletIDToChildren(StackEntry treelet_root);
    static std::vector<StackEntry> treeletIDToChildren(uint8_t* treelet_root);
    static void buildNodeToRootMap();
};

#endif /* VULKAN_RAY_TRACING_H */
