/*
 * tools/sweetvk: the first-light test for the Turnip ICD - one compute
 * dispatch on the Adreno 630, through out/libvulkan_freedreno.dylib and the
 * SweetGPU user client, with no Vulkan loader in the way (the same
 * vk_icdGetInstanceProcAddr dance tools/sweetmtl's bridge does).
 *
 *   vk_compute [path]      (default /usr/lib/sweetmtl/libvulkan_freedreno.dylib)
 *
 * It can only pass on the phone: the ICD's only transport is the SweetGPU
 * kernel, and the test refuses a device that is not the A630 by name. On the
 * Mac it just builds (see tools/sweetvk/README.md).
 *
 * What it proves, in order: the ICD loads and exports the interface; the msm
 * emulation's GEM_NEW/GEM_INFO/GET_PARAM/SUBMIT/WAIT_FENCE answer Turnip's
 * device bring-up well enough to report a physical device; a storage-buffer
 * dispatch runs; and the GPU's writes come back to the CPU as 2*x+1. Every
 * step prints a line, so the phone's console shows where it stopped.
 *
 * The SPIR-V is the compiled test/vk_compute.comp, embedded so the phone needs
 * no shader tools:
 *   glslangValidator -V --target-env vulkan1.1 -o vk_compute.spv vk_compute.comp
 */
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* Mesa ships the Vulkan headers (include/vulkan/); build.sh points -I there. */
#include <vulkan/vulkan.h>

#define DEFAULT_ICD "/usr/lib/sweetmtl/libvulkan_freedreno.dylib"
#define N 64u                   /* u32 elements in each buffer */
#define TIMEOUT_NS 5000000000ull

/* test/vk_compute.comp: out.x[i] = in.x[i] * 2u + 1u, local_size_x = 64. */
static const uint32_t kernel_spv[] = {
   0x07230203, 0x00010300, 0x0008000b, 0x00000026, 0x00000000, 0x00020011,
   0x00000001, 0x0006000b, 0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e,
   0x00000000, 0x0003000e, 0x00000000, 0x00000001, 0x0006000f, 0x00000005,
   0x00000004, 0x6e69616d, 0x00000000, 0x0000000b, 0x00060010, 0x00000004,
   0x00000011, 0x00000040, 0x00000001, 0x00000001, 0x00030003, 0x00000002,
   0x000001c2, 0x00040005, 0x00000004, 0x6e69616d, 0x00000000, 0x00030005,
   0x00000008, 0x00000069, 0x00080005, 0x0000000b, 0x475f6c67, 0x61626f6c,
   0x766e496c, 0x7461636f, 0x496e6f69, 0x00000044, 0x00030005, 0x00000011,
   0x0074754f, 0x00040006, 0x00000011, 0x00000000, 0x00000078, 0x00040005,
   0x00000013, 0x6274756f, 0x00000000, 0x00030005, 0x00000018, 0x00006e49,
   0x00040006, 0x00000018, 0x00000000, 0x00000078, 0x00030005, 0x0000001a,
   0x00626e69, 0x00040047, 0x0000000b, 0x0000000b, 0x0000001c, 0x00040047,
   0x00000010, 0x00000006, 0x00000004, 0x00030047, 0x00000011, 0x00000002,
   0x00040048, 0x00000011, 0x00000000, 0x00000019, 0x00050048, 0x00000011,
   0x00000000, 0x00000023, 0x00000000, 0x00030047, 0x00000013, 0x00000019,
   0x00040047, 0x00000013, 0x00000021, 0x00000001, 0x00040047, 0x00000013,
   0x00000022, 0x00000000, 0x00040047, 0x00000017, 0x00000006, 0x00000004,
   0x00030047, 0x00000018, 0x00000002, 0x00040048, 0x00000018, 0x00000000,
   0x00000018, 0x00050048, 0x00000018, 0x00000000, 0x00000023, 0x00000000,
   0x00030047, 0x0000001a, 0x00000018, 0x00040047, 0x0000001a, 0x00000021,
   0x00000000, 0x00040047, 0x0000001a, 0x00000022, 0x00000000, 0x00040047,
   0x00000025, 0x0000000b, 0x00000019, 0x00020013, 0x00000002, 0x00030021,
   0x00000003, 0x00000002, 0x00040015, 0x00000006, 0x00000020, 0x00000000,
   0x00040020, 0x00000007, 0x00000007, 0x00000006, 0x00040017, 0x00000009,
   0x00000006, 0x00000003, 0x00040020, 0x0000000a, 0x00000001, 0x00000009,
   0x0004003b, 0x0000000a, 0x0000000b, 0x00000001, 0x0004002b, 0x00000006,
   0x0000000c, 0x00000000, 0x00040020, 0x0000000d, 0x00000001, 0x00000006,
   0x0003001d, 0x00000010, 0x00000006, 0x0003001e, 0x00000011, 0x00000010,
   0x00040020, 0x00000012, 0x0000000c, 0x00000011, 0x0004003b, 0x00000012,
   0x00000013, 0x0000000c, 0x00040015, 0x00000014, 0x00000020, 0x00000001,
   0x0004002b, 0x00000014, 0x00000015, 0x00000000, 0x0003001d, 0x00000017,
   0x00000006, 0x0003001e, 0x00000018, 0x00000017, 0x00040020, 0x00000019,
   0x0000000c, 0x00000018, 0x0004003b, 0x00000019, 0x0000001a, 0x0000000c,
   0x00040020, 0x0000001c, 0x0000000c, 0x00000006, 0x0004002b, 0x00000006,
   0x0000001f, 0x00000002, 0x0004002b, 0x00000006, 0x00000021, 0x00000001,
   0x0004002b, 0x00000006, 0x00000024, 0x00000040, 0x0006002c, 0x00000009,
   0x00000025, 0x00000024, 0x00000021, 0x00000021, 0x00050036, 0x00000002,
   0x00000004, 0x00000000, 0x00000003, 0x000200f8, 0x00000005, 0x0004003b,
   0x00000007, 0x00000008, 0x00000007, 0x00050041, 0x0000000d, 0x0000000e,
   0x0000000b, 0x0000000c, 0x0004003d, 0x00000006, 0x0000000f, 0x0000000e,
   0x0003003e, 0x00000008, 0x0000000f, 0x0004003d, 0x00000006, 0x00000016,
   0x00000008, 0x0004003d, 0x00000006, 0x0000001b, 0x00000008, 0x00060041,
   0x0000001c, 0x0000001d, 0x0000001a, 0x00000015, 0x0000001b, 0x0004003d,
   0x00000006, 0x0000001e, 0x0000001d, 0x00050084, 0x00000006, 0x00000020,
   0x0000001e, 0x0000001f, 0x00050080, 0x00000006, 0x00000022, 0x00000020,
   0x00000021, 0x00060041, 0x0000001c, 0x00000023, 0x00000013, 0x00000015,
   0x00000016, 0x0003003e, 0x00000023, 0x00000022, 0x000100fd, 0x00010038,
};

static PFN_vkGetInstanceProcAddr icd_gipa;
static const char *prog = "vk_compute";

static int
fail(const char *what)
{
	fprintf(stderr, "FAIL %s: %s\n", prog, what);
	return 1;
}

/* The ICD's entry point: NULL for the global commands, the instance after. */
static PFN_vkVoidFunction
gp(VkInstance instance, const char *name)
{
	PFN_vkVoidFunction p = icd_gipa(instance, name);
	if (p == NULL) {
		fprintf(stderr, "FAIL %s: the ICD has no %s\n", prog, name);
		exit(1);
	}
	return p;
}

#define PROC(instance, name) ((PFN_##name)gp(instance, #name))

static int
check(VkResult r, const char *what)
{
	if (r == VK_SUCCESS)
		return 0;
	fprintf(stderr, "FAIL %s: %s: VkResult %d\n", prog, what, (int)r);
	return 1;
}

int
main(int argc, char **argv)
{
	const char *path = argc > 1 ? argv[1] : DEFAULT_ICD;
	void *lib = dlopen(path, RTLD_NOW | RTLD_LOCAL);
	if (lib == NULL) {
		fprintf(stderr, "FAIL %s: dlopen(%s): %s\n", prog, path, dlerror());
		return 1;
	}
	icd_gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vk_icdGetInstanceProcAddr");
	if (icd_gipa == NULL)
		return fail("the ICD does not export vk_icdGetInstanceProcAddr");
	printf("%s: %s, ICD %s\n", prog, "one compute dispatch on the Adreno 630", path);

	/* ---- instance ---- */
	VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
	app.pApplicationName = "vk_compute";
	app.applicationVersion = 1;
	app.pEngineName = "sweetvk";
	app.engineVersion = 1;
	app.apiVersion = VK_API_VERSION_1_1;
	VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
	ici.pApplicationInfo = &app;
	VkInstance instance;
	if (check(PROC(NULL, vkCreateInstance)(&ici, NULL, &instance), "vkCreateInstance"))
		return fail("the ICD's msm backend did not come up");

	/* ---- the device must be this phone's A630 ---- */
	uint32_t count = 0;
	if (check(PROC(instance, vkEnumeratePhysicalDevices)(instance, &count, NULL),
	          "vkEnumeratePhysicalDevices(count)"))
		return 1;
	if (count == 0)
		return fail("the ICD reports no physical device");
	VkPhysicalDevice devices[8];
	if (count > 8)
		count = 8;
	if (check(PROC(instance, vkEnumeratePhysicalDevices)(instance, &count, devices),
	          "vkEnumeratePhysicalDevices"))
		return 1;
	VkPhysicalDevice pdev = devices[0];

	VkPhysicalDeviceProperties props;
	PROC(instance, vkGetPhysicalDeviceProperties)(pdev, &props);
	printf("%s: device \"%s\", API %u.%u.%u, driver %u.%u.%u\n", prog, props.deviceName,
	       VK_API_VERSION_MAJOR(props.apiVersion), VK_API_VERSION_MINOR(props.apiVersion),
	       VK_API_VERSION_PATCH(props.apiVersion),
	       VK_API_VERSION_MAJOR(props.driverVersion), VK_API_VERSION_MINOR(props.driverVersion),
	       VK_API_VERSION_PATCH(props.driverVersion));
	if (strstr(props.deviceName, "630") == NULL)
		return fail("the device is not the Adreno 630 (this test only means anything on the phone)");

	VkPhysicalDeviceMemoryProperties memprops;
	PROC(instance, vkGetPhysicalDeviceMemoryProperties)(pdev, &memprops);
	printf("%s: %u memory types, heap 0 %llu MiB, flags %x\n", prog, memprops.memoryTypeCount,
	       (unsigned long long)(memprops.memoryHeaps[0].size >> 20),
	       memprops.memoryHeaps[0].flags);
	uint32_t type = UINT32_MAX;
	for (uint32_t i = 0; i < memprops.memoryTypeCount; i++) {
		VkMemoryPropertyFlags f = memprops.memoryTypes[i].propertyFlags;
		if ((f & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
		    (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
			type = i;
			break;
		}
	}
	if (type == UINT32_MAX)
		return fail("no host-visible coherent memory type");

	/* ---- device and queue ---- */
	uint32_t nq = 0;
	PROC(instance, vkGetPhysicalDeviceQueueFamilyProperties)(pdev, &nq, NULL);
	VkQueueFamilyProperties queues[8];
	if (nq == 0 || nq > 8)
		return fail("no queue family");
	PROC(instance, vkGetPhysicalDeviceQueueFamilyProperties)(pdev, &nq, queues);
	if (!(queues[0].queueFlags & VK_QUEUE_COMPUTE_BIT))
		return fail("queue family 0 has no compute");
	const float priority = 1.0f;
	VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
	qci.queueFamilyIndex = 0;
	qci.queueCount = 1;
	qci.pQueuePriorities = &priority;
	VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
	dci.queueCreateInfoCount = 1;
	dci.pQueueCreateInfos = &qci;
	VkDevice dev;
	if (check(PROC(instance, vkCreateDevice)(pdev, &dci, NULL, &dev), "vkCreateDevice"))
		return 1;
	VkQueue queue;
	PROC(instance, vkGetDeviceQueue)(dev, 0, 0, &queue);

	/* ---- one buffer in, one buffer out ---- */
	VkBuffer buffer[2];
	VkDeviceMemory memory[2];
	void *map[2] = { NULL, NULL };
	const VkDeviceSize size = N * sizeof(uint32_t);
	for (unsigned i = 0; i < 2; i++) {
		VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
		bci.size = size;
		bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
		bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		if (check(PROC(instance, vkCreateBuffer)(dev, &bci, NULL, &buffer[i]), "vkCreateBuffer"))
			return 1;
		VkMemoryRequirements req;
		PROC(instance, vkGetBufferMemoryRequirements)(dev, buffer[i], &req);
		if (!(req.memoryTypeBits & (1u << type)))
			return fail("the buffer cannot use host-visible coherent memory");
		VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
		mai.allocationSize = req.size;
		mai.memoryTypeIndex = type;
		if (check(PROC(instance, vkAllocateMemory)(dev, &mai, NULL, &memory[i]), "vkAllocateMemory"))
			return 1;
		if (check(PROC(instance, vkBindBufferMemory)(dev, buffer[i], memory[i], 0),
		          "vkBindBufferMemory"))
			return 1;
		if (check(PROC(instance, vkMapMemory)(dev, memory[i], 0, VK_WHOLE_SIZE, 0, &map[i]),
		          "vkMapMemory"))
			return 1;
	}
	uint32_t *in = map[0], *out = map[1];
	for (uint32_t i = 0; i < N; i++)
		in[i] = 3 * i + 7;
	memset(out, 0, N * sizeof(uint32_t));

	/* ---- pipeline ---- */
	VkShaderModuleCreateInfo smci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
	smci.codeSize = sizeof(kernel_spv);
	smci.pCode = kernel_spv;
	VkShaderModule shader;
	if (check(PROC(instance, vkCreateShaderModule)(dev, &smci, NULL, &shader), "vkCreateShaderModule"))
		return 1;
	VkDescriptorSetLayoutBinding bindings[2];
	memset(bindings, 0, sizeof(bindings));
	for (unsigned i = 0; i < 2; i++) {
		bindings[i].binding = i;
		bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		bindings[i].descriptorCount = 1;
		bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	}
	VkDescriptorSetLayoutCreateInfo dsci = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
	dsci.bindingCount = 2;
	dsci.pBindings = bindings;
	VkDescriptorSetLayout dsl;
	if (check(PROC(instance, vkCreateDescriptorSetLayout)(dev, &dsci, NULL, &dsl),
	          "vkCreateDescriptorSetLayout"))
		return 1;
	VkPipelineLayoutCreateInfo plci = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
	plci.setLayoutCount = 1;
	plci.pSetLayouts = &dsl;
	VkPipelineLayout layout;
	if (check(PROC(instance, vkCreatePipelineLayout)(dev, &plci, NULL, &layout),
	          "vkCreatePipelineLayout"))
		return 1;
	VkComputePipelineCreateInfo cpci = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
	cpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
	cpci.stage.module = shader;
	cpci.stage.pName = "main";
	cpci.layout = layout;
	VkPipeline pipeline;
	if (check(PROC(instance, vkCreateComputePipelines)(dev, VK_NULL_HANDLE, 1, &cpci, NULL, &pipeline),
	          "vkCreateComputePipelines"))
		return 1;
	printf("%s: compute pipeline ready\n", prog);

	/* ---- descriptors ---- */
	VkDescriptorPoolSize pool_size = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2 };
	VkDescriptorPoolCreateInfo dpci = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
	dpci.maxSets = 1;
	dpci.poolSizeCount = 1;
	dpci.pPoolSizes = &pool_size;
	VkDescriptorPool pool;
	if (check(PROC(instance, vkCreateDescriptorPool)(dev, &dpci, NULL, &pool),
	          "vkCreateDescriptorPool"))
		return 1;
	VkDescriptorSetAllocateInfo dsai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
	dsai.descriptorPool = pool;
	dsai.descriptorSetCount = 1;
	dsai.pSetLayouts = &dsl;
	VkDescriptorSet set;
	if (check(PROC(instance, vkAllocateDescriptorSets)(dev, &dsai, &set), "vkAllocateDescriptorSets"))
		return 1;
	VkDescriptorBufferInfo info[2];
	VkWriteDescriptorSet writes[2];
	memset(writes, 0, sizeof(writes));
	for (unsigned i = 0; i < 2; i++) {
		info[i].buffer = buffer[i];
		info[i].offset = 0;
		info[i].range = size;
		writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[i].dstSet = set;
		writes[i].dstBinding = i;
		writes[i].descriptorCount = 1;
		writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		writes[i].pBufferInfo = &info[i];
	}
	PROC(instance, vkUpdateDescriptorSets)(dev, 2, writes, 0, NULL);

	/* ---- one dispatch ---- */
	VkCommandPoolCreateInfo cci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
	cci.queueFamilyIndex = 0;
	VkCommandPool cpool;
	if (check(PROC(instance, vkCreateCommandPool)(dev, &cci, NULL, &cpool), "vkCreateCommandPool"))
		return 1;
	VkCommandBufferAllocateInfo cbai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
	cbai.commandPool = cpool;
	cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	cbai.commandBufferCount = 1;
	VkCommandBuffer cb;
	if (check(PROC(instance, vkAllocateCommandBuffers)(dev, &cbai, &cb), "vkAllocateCommandBuffers"))
		return 1;
	VkCommandBufferBeginInfo cbbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
	cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	if (check(PROC(instance, vkBeginCommandBuffer)(cb, &cbbi), "vkBeginCommandBuffer"))
		return 1;
	PROC(instance, vkCmdBindPipeline)(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
	PROC(instance, vkCmdBindDescriptorSets)(cb, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, NULL);
	PROC(instance, vkCmdDispatch)(cb, 1, 1, 1);
	if (check(PROC(instance, vkEndCommandBuffer)(cb), "vkEndCommandBuffer"))
		return 1;
	VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
	VkFence fence;
	if (check(PROC(instance, vkCreateFence)(dev, &fci, NULL, &fence), "vkCreateFence"))
		return 1;
	VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
	si.commandBufferCount = 1;
	si.pCommandBuffers = &cb;
	if (check(PROC(instance, vkQueueSubmit)(queue, 1, &si, fence), "vkQueueSubmit"))
		return 1;
	printf("%s: submitted, waiting for the 630\n", prog);
	VkResult r = PROC(instance, vkWaitForFences)(dev, 1, &fence, VK_TRUE, TIMEOUT_NS);
	if (check(r, "vkWaitForFences"))
		return fail(r == VK_TIMEOUT ? "the dispatch did not finish in 5 s" : "the fence failed");
	if (check(PROC(instance, vkQueueWaitIdle)(queue), "vkQueueWaitIdle"))
		return 1;

	/* The memory is host-visible coherent, so the driver says no cache
	 * maintenance is needed; ask for an invalidate anyway so a stale line in
	 * this process cannot fake the result on hardware. */
	VkMappedMemoryRange range = { VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE };
	range.memory = memory[1];
	range.offset = 0;
	range.size = VK_WHOLE_SIZE;
	PROC(instance, vkInvalidateMappedMemoryRanges)(dev, 1, &range);

	uint32_t bad = 0;
	for (uint32_t i = 0; i < N; i++) {
		uint32_t want = in[i] * 2u + 1u;
		if (out[i] != want) {
			if (bad < 4)
				fprintf(stderr, "FAIL %s: out[%u] = %u, want %u\n", prog, i, out[i], want);
			bad++;
		}
	}
	if (bad) {
		fprintf(stderr, "FAIL %s: %u of %u elements wrong\n", prog, bad, N);
		return 1;
	}
	printf("%s: PASS %u/%u elements: out[i] = in[i]*2+1 on \"%s\"\n", prog, N, N, props.deviceName);

	PROC(instance, vkDeviceWaitIdle)(dev);
	for (unsigned i = 0; i < 2; i++) {
		PROC(instance, vkUnmapMemory)(dev, memory[i]);
		PROC(instance, vkDestroyBuffer)(dev, buffer[i], NULL);
		PROC(instance, vkFreeMemory)(dev, memory[i], NULL);
	}
	PROC(instance, vkDestroyFence)(dev, fence, NULL);
	PROC(instance, vkDestroyCommandPool)(dev, cpool, NULL);
	PROC(instance, vkDestroyDescriptorPool)(dev, pool, NULL);
	PROC(instance, vkDestroyPipeline)(dev, pipeline, NULL);
	PROC(instance, vkDestroyPipelineLayout)(dev, layout, NULL);
	PROC(instance, vkDestroyDescriptorSetLayout)(dev, dsl, NULL);
	PROC(instance, vkDestroyShaderModule)(dev, shader, NULL);
	PROC(instance, vkDestroyDevice)(dev, NULL);
	PROC(instance, vkDestroyInstance)(instance, NULL);
	dlclose(lib);
	return 0;
}
