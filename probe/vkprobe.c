// Vulkan capabilities the NVN replay relies on (S10e, Mali-G76): API version, the extensions it might use, BC / ASTC / D24S8 support, host-pointer import alignment and limits
#include <stdio.h>
#include <string.h>
#include <vulkan/vulkan.h>
int main(void) {
    VkApplicationInfo ai = {VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_1};
    VkInstanceCreateInfo ci = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &ai};
    VkInstance inst; if (vkCreateInstance(&ci, 0, &inst)) { puts("no instance"); return 1; }
    uint32_t n = 1; VkPhysicalDevice pd; vkEnumeratePhysicalDevices(inst, &n, &pd);
    VkPhysicalDeviceProperties p; vkGetPhysicalDeviceProperties(pd, &p);
    printf("%s api %u.%u.%u driver %#x\n", p.deviceName, VK_VERSION_MAJOR(p.apiVersion), VK_VERSION_MINOR(p.apiVersion), VK_VERSION_PATCH(p.apiVersion), p.driverVersion);
    printf("maxUBORange %u minUBOAlign %llu maxBoundSets %u maxPerStageSamplers %u maxVertexAttribs %u maxColorAtt %u nonCoherentAtom %llu\n", p.limits.maxUniformBufferRange,
           (unsigned long long)p.limits.minUniformBufferOffsetAlignment, p.limits.maxBoundDescriptorSets, p.limits.maxPerStageDescriptorSampledImages, p.limits.maxVertexInputAttributes,
           p.limits.maxColorAttachments, (unsigned long long)p.limits.nonCoherentAtomSize);
    uint32_t ne = 0; vkEnumerateDeviceExtensionProperties(pd, 0, &ne, 0); VkExtensionProperties e[256]; if (ne > 256) ne = 256; vkEnumerateDeviceExtensionProperties(pd, 0, &ne, e);
    for (uint32_t i = 0; i < ne; i++) printf("ext %s\n", e[i].extensionName);
    VkPhysicalDeviceFeatures f; vkGetPhysicalDeviceFeatures(pd, &f);
    printf("BC %d ASTC %d ETC2 %d depthClamp %d fillNonSolid %d wideLines %d samplerAniso %d dualSrcBlend %d logicOp %d indepBlend %d fullDrawIndexU32 %d\n", f.textureCompressionBC, f.textureCompressionASTC_LDR,
           f.textureCompressionETC2, f.depthClamp, f.fillModeNonSolid, f.wideLines, f.samplerAnisotropy, f.dualSrcBlend, f.logicOp, f.independentBlend, f.fullDrawIndexUint32);
    VkFormat fm[] = {VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_X8_D24_UNORM_PACK32, VK_FORMAT_BC7_UNORM_BLOCK, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R32G32B32_SFLOAT, VK_FORMAT_A2B10G10R10_UNORM_PACK32};
    for (unsigned i = 0; i < sizeof fm / sizeof *fm; i++) { VkFormatProperties fp; vkGetPhysicalDeviceFormatProperties(pd, fm[i], &fp); printf("fmt %d opt %#x buf %#x\n", fm[i], fp.optimalTilingFeatures, fp.bufferFeatures); }
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) printf("memtype %u flags %#x heap %u\n", i, mp.memoryTypes[i].propertyFlags, mp.memoryTypes[i].heapIndex);
    PFN_vkGetPhysicalDeviceProperties2 gp2 = (PFN_vkGetPhysicalDeviceProperties2)vkGetInstanceProcAddr(inst, "vkGetPhysicalDeviceProperties2");
    VkPhysicalDeviceExternalMemoryHostPropertiesEXT hp = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT};
    VkPhysicalDeviceProperties2 p2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &hp};
    if (gp2) { gp2(pd, &p2); printf("minImportedHostPointerAlignment %llu\n", (unsigned long long)hp.minImportedHostPointerAlignment); }
    return 0;
}
