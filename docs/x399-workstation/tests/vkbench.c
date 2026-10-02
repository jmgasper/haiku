/* GPU benchmark for the nvidia_rm + NVK stack.
 *
 * Measures what the GPU actually does: compute throughput with a chain of
 * fused multiply-adds, fill rate with a fragment shader covering a 1920x1080
 * target, and copy bandwidth in video memory and from host memory.
 *
 * usage: vkbench [compute|fill|depth|vbo|submit|hostimport|scanout|detile|readback|copy|upload]...  (default: all of them)
 *
 * The SPIR-V shaders are loaded from a "shaders" directory next to the binary,
 * or from the directory in VKBENCH_SHADERS.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include <inttypes.h>
#include <vulkan/vulkan.h>

#include "vk_haiku_scanout.h"

#define CHECK(x, what) do { \
	VkResult _r = (x); \
	if (_r != VK_SUCCESS) { fprintf(stderr, "FAIL %s: %d\n", what, _r); exit(1); } \
} while (0)

static VkInstance sInstance;
static VkPhysicalDevice sPhysicalDevice;
static VkDevice sDevice;
static VkQueue sQueue;
static uint32_t sQueueFamily;
static VkCommandPool sCommandPool;
static VkPhysicalDeviceMemoryProperties sMemoryProperties;
static const char *sShaderDir = "shaders";

static double now_s(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static uint32_t find_memory(uint32_t mask, VkMemoryPropertyFlags want)
{
	for (uint32_t i = 0; i < sMemoryProperties.memoryTypeCount; i++) {
		if ((mask & (1u << i)) != 0
			&& (sMemoryProperties.memoryTypes[i].propertyFlags & want) == want)
			return i;
	}
	fprintf(stderr, "FAIL no memory type for %#x\n", want);
	exit(1);
}

struct buffer {
	VkBuffer buffer;
	VkDeviceMemory memory;
	VkDeviceSize size;
	void *map;
};

static struct buffer create_buffer(VkDeviceSize size, VkBufferUsageFlags usage,
	VkMemoryPropertyFlags props)
{
	struct buffer b = { .size = size };
	VkBufferCreateInfo bci = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		.size = size,
		.usage = usage,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
	};
	CHECK(vkCreateBuffer(sDevice, &bci, NULL, &b.buffer), "vkCreateBuffer");

	VkMemoryRequirements req;
	vkGetBufferMemoryRequirements(sDevice, b.buffer, &req);
	VkMemoryAllocateInfo mai = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = req.size,
		.memoryTypeIndex = find_memory(req.memoryTypeBits, props),
	};
	CHECK(vkAllocateMemory(sDevice, &mai, NULL, &b.memory), "vkAllocateMemory");
	CHECK(vkBindBufferMemory(sDevice, b.buffer, b.memory, 0), "vkBindBufferMemory");

	if ((props & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0)
		CHECK(vkMapMemory(sDevice, b.memory, 0, VK_WHOLE_SIZE, 0, &b.map), "vkMapMemory");

	return b;
}

static void destroy_buffer(struct buffer *b)
{
	if (b->map != NULL)
		vkUnmapMemory(sDevice, b->memory);
	vkDestroyBuffer(sDevice, b->buffer, NULL);
	vkFreeMemory(sDevice, b->memory, NULL);
}

static VkShaderModule load_shader(const char *name)
{
	char path[512];
	snprintf(path, sizeof(path), "%s/%s", sShaderDir, name);
	FILE *file = fopen(path, "rb");
	if (file == NULL) {
		fprintf(stderr, "FAIL cannot open %s\n", path);
		exit(1);
	}
	fseek(file, 0, SEEK_END);
	long size = ftell(file);
	fseek(file, 0, SEEK_SET);
	uint32_t *code = malloc(size);
	if (code == NULL || fread(code, 1, size, file) != (size_t)size) {
		fprintf(stderr, "FAIL cannot read %s\n", path);
		exit(1);
	}
	fclose(file);

	VkShaderModuleCreateInfo smci = {
		.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
		.codeSize = size,
		.pCode = code,
	};
	VkShaderModule module;
	CHECK(vkCreateShaderModule(sDevice, &smci, NULL, &module), "vkCreateShaderModule");
	free(code);
	return module;
}

static VkCommandBuffer begin_commands(void)
{
	VkCommandBufferAllocateInfo cbai = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = sCommandPool,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		.commandBufferCount = 1,
	};
	VkCommandBuffer cb;
	CHECK(vkAllocateCommandBuffers(sDevice, &cbai, &cb), "vkAllocateCommandBuffers");
	VkCommandBufferBeginInfo cbbi = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
	};
	CHECK(vkBeginCommandBuffer(cb, &cbbi), "vkBeginCommandBuffer");
	return cb;
}

// Submit the commands and wait for the GPU to finish them, returning how long
// that took.
static double run_commands(VkCommandBuffer cb)
{
	CHECK(vkEndCommandBuffer(cb), "vkEndCommandBuffer");

	VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
	VkFence fence;
	CHECK(vkCreateFence(sDevice, &fci, NULL, &fence), "vkCreateFence");

	VkSubmitInfo si = {
		.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		.commandBufferCount = 1,
		.pCommandBuffers = &cb,
	};
	double start = now_s();
	CHECK(vkQueueSubmit(sQueue, 1, &si, fence), "vkQueueSubmit");
	CHECK(vkWaitForFences(sDevice, 1, &fence, VK_TRUE, 30ull * 1000 * 1000 * 1000),
		"vkWaitForFences");
	double elapsed = now_s() - start;

	vkDestroyFence(sDevice, fence, NULL);
	vkFreeCommandBuffers(sDevice, sCommandPool, 1, &cb);
	return elapsed;
}


// #pragma mark - benchmarks

static void bench_compute(void)
{
	const uint32_t kInvocations = 256 * 1024;	// 1024 workgroups of 256
	const uint32_t kIters = 2048;
	// Each iteration of the loop does 4 fma on vec4 values: 4 * 4 * 2 flops.
	const double kFlopsPerIter = 32.0;

	struct buffer out = create_buffer(kInvocations * sizeof(float),
		VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

	VkDescriptorSetLayoutBinding binding = {
		.binding = 0,
		.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
		.descriptorCount = 1,
		.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
	};
	VkDescriptorSetLayoutCreateInfo dslci = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		.bindingCount = 1,
		.pBindings = &binding,
	};
	VkDescriptorSetLayout setLayout;
	CHECK(vkCreateDescriptorSetLayout(sDevice, &dslci, NULL, &setLayout),
		"vkCreateDescriptorSetLayout");

	VkPushConstantRange range = {
		.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
		.size = sizeof(uint32_t),
	};
	VkPipelineLayoutCreateInfo plci = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.setLayoutCount = 1,
		.pSetLayouts = &setLayout,
		.pushConstantRangeCount = 1,
		.pPushConstantRanges = &range,
	};
	VkPipelineLayout layout;
	CHECK(vkCreatePipelineLayout(sDevice, &plci, NULL, &layout), "vkCreatePipelineLayout");

	VkShaderModule shader = load_shader("fma.comp.spv");
	VkComputePipelineCreateInfo cpci = {
		.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
		.stage = {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			.stage = VK_SHADER_STAGE_COMPUTE_BIT,
			.module = shader,
			.pName = "main",
		},
		.layout = layout,
	};
	VkPipeline pipeline;
	CHECK(vkCreateComputePipelines(sDevice, VK_NULL_HANDLE, 1, &cpci, NULL, &pipeline),
		"vkCreateComputePipelines");

	VkDescriptorPoolSize poolSize = {
		.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
		.descriptorCount = 1,
	};
	VkDescriptorPoolCreateInfo dpci = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
		.maxSets = 1,
		.poolSizeCount = 1,
		.pPoolSizes = &poolSize,
	};
	VkDescriptorPool pool;
	CHECK(vkCreateDescriptorPool(sDevice, &dpci, NULL, &pool), "vkCreateDescriptorPool");

	VkDescriptorSetAllocateInfo dsai = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		.descriptorPool = pool,
		.descriptorSetCount = 1,
		.pSetLayouts = &setLayout,
	};
	VkDescriptorSet set;
	CHECK(vkAllocateDescriptorSets(sDevice, &dsai, &set), "vkAllocateDescriptorSets");

	VkDescriptorBufferInfo dbi = { .buffer = out.buffer, .range = VK_WHOLE_SIZE };
	VkWriteDescriptorSet write = {
		.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		.dstSet = set,
		.descriptorCount = 1,
		.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
		.pBufferInfo = &dbi,
	};
	vkUpdateDescriptorSets(sDevice, 1, &write, 0, NULL);

	double best = 0;
	for (int pass = 0; pass < 5; pass++) {
		VkCommandBuffer cb = begin_commands();
		vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
		vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, NULL);
		vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(kIters), &kIters);
		vkCmdDispatch(cb, kInvocations / 256, 1, 1);
		double elapsed = run_commands(cb);

		double flops = (double)kInvocations * kIters * kFlopsPerIter;
		double gflops = flops / elapsed / 1e9;
		if (pass == 0) {
			printf("compute: %u invocations x %u iterations\n", kInvocations, kIters);
		}
		printf("  pass %d: %6.1f ms, %8.1f GFLOP/s\n", pass, elapsed * 1000.0, gflops);
		if (gflops > best)
			best = gflops;
	}
	float result = ((float*)out.map)[0];
	printf("  result[0] = %g (%s)\n", result, result > 0 ? "shader ran" : "SUSPICIOUS");
	printf("  best: %.1f GFLOP/s\n", best);

	vkDestroyPipeline(sDevice, pipeline, NULL);
	vkDestroyShaderModule(sDevice, shader, NULL);
	vkDestroyDescriptorPool(sDevice, pool, NULL);
	vkDestroyPipelineLayout(sDevice, layout, NULL);
	vkDestroyDescriptorSetLayout(sDevice, setLayout, NULL);
	destroy_buffer(&out);
}

// `useDepth` adds a depth attachment and depth testing; `useVertexBuffer`
// feeds the triangle from a vertex buffer instead of generating it from the
// vertex index.  Both are paths a real OpenGL driver uses on every draw.
static void bench_fill_ex(bool useDepth, bool useVertexBuffer, const char *name)
{
	const uint32_t kWidth = 1920, kHeight = 1080;
	const uint32_t kDrawsPerPass = 200;
	const VkFormat kFormat = VK_FORMAT_R8G8B8A8_UNORM;
	const VkFormat kDepthFormat = VK_FORMAT_D24_UNORM_S8_UINT;

	VkImageCreateInfo ici = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.imageType = VK_IMAGE_TYPE_2D,
		.format = kFormat,
		.extent = { kWidth, kHeight, 1 },
		.mipLevels = 1,
		.arrayLayers = 1,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.tiling = VK_IMAGE_TILING_OPTIMAL,
		.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	};
	VkImage image;
	CHECK(vkCreateImage(sDevice, &ici, NULL, &image), "vkCreateImage");

	VkMemoryRequirements req;
	vkGetImageMemoryRequirements(sDevice, image, &req);
	VkMemoryAllocateInfo mai = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = req.size,
		.memoryTypeIndex = find_memory(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
	};
	VkDeviceMemory imageMemory;
	CHECK(vkAllocateMemory(sDevice, &mai, NULL, &imageMemory), "vkAllocateMemory");
	CHECK(vkBindImageMemory(sDevice, image, imageMemory, 0), "vkBindImageMemory");

	VkImage depthImage = VK_NULL_HANDLE;
	VkDeviceMemory depthMemory = VK_NULL_HANDLE;
	VkImageView depthView = VK_NULL_HANDLE;
	if (useDepth) {
		VkImageCreateInfo dici = ici;
		dici.format = kDepthFormat;
		dici.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
		CHECK(vkCreateImage(sDevice, &dici, NULL, &depthImage), "vkCreateImage(depth)");

		VkMemoryRequirements dreq;
		vkGetImageMemoryRequirements(sDevice, depthImage, &dreq);
		VkMemoryAllocateInfo dmai = {
			.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
			.allocationSize = dreq.size,
			.memoryTypeIndex = find_memory(dreq.memoryTypeBits,
				VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
		};
		CHECK(vkAllocateMemory(sDevice, &dmai, NULL, &depthMemory), "vkAllocateMemory(depth)");
		CHECK(vkBindImageMemory(sDevice, depthImage, depthMemory, 0), "vkBindImageMemory(depth)");

		VkImageViewCreateInfo divci = {
			.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
			.image = depthImage,
			.viewType = VK_IMAGE_VIEW_TYPE_2D,
			.format = kDepthFormat,
			.subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,
				0, 1, 0, 1 },
		};
		CHECK(vkCreateImageView(sDevice, &divci, NULL, &depthView), "vkCreateImageView(depth)");
	}

	// A triangle in a vertex buffer, for the vertex input path.
	struct buffer vertices = { 0 };
	if (useVertexBuffer) {
		static const float kTriangle[9] = {
			-1.0f, -1.0f, 0.5f,
			 3.0f, -1.0f, 0.5f,
			-1.0f,  3.0f, 0.5f,
		};
		vertices = create_buffer(sizeof(kTriangle), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
		memcpy(vertices.map, kTriangle, sizeof(kTriangle));
	}

	VkImageViewCreateInfo ivci = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		.image = image,
		.viewType = VK_IMAGE_VIEW_TYPE_2D,
		.format = kFormat,
		.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
	};
	VkImageView view;
	CHECK(vkCreateImageView(sDevice, &ivci, NULL, &view), "vkCreateImageView");

	VkPushConstantRange range = {
		.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
		.size = sizeof(uint32_t),
	};
	VkPipelineLayoutCreateInfo plci = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.pushConstantRangeCount = 1,
		.pPushConstantRanges = &range,
	};
	VkPipelineLayout layout;
	CHECK(vkCreatePipelineLayout(sDevice, &plci, NULL, &layout), "vkCreatePipelineLayout");

	VkShaderModule vert = load_shader(useVertexBuffer ? "vbo.vert.spv" : "fill.vert.spv");
	VkShaderModule frag = load_shader("fill.frag.spv");
	VkPipelineShaderStageCreateInfo stages[2] = {
		{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			.stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vert, .pName = "main",
		}, {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			.stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = frag, .pName = "main",
		},
	};
	VkVertexInputBindingDescription vertexBinding = {
		.binding = 0, .stride = 3 * sizeof(float),
		.inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
	};
	VkVertexInputAttributeDescription vertexAttribute = {
		.location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = 0,
	};
	VkPipelineVertexInputStateCreateInfo vertexInput = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
		.vertexBindingDescriptionCount = useVertexBuffer ? 1 : 0,
		.pVertexBindingDescriptions = &vertexBinding,
		.vertexAttributeDescriptionCount = useVertexBuffer ? 1 : 0,
		.pVertexAttributeDescriptions = &vertexAttribute,
	};
	VkPipelineDepthStencilStateCreateInfo depthState = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
		.depthTestEnable = VK_TRUE,
		.depthWriteEnable = VK_TRUE,
		.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL,
	};
	VkPipelineInputAssemblyStateCreateInfo inputAssembly = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
		.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
	};
	VkViewport viewport = { 0, 0, (float)kWidth, (float)kHeight, 0, 1 };
	VkRect2D scissor = { { 0, 0 }, { kWidth, kHeight } };
	VkPipelineViewportStateCreateInfo viewportState = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
		.viewportCount = 1, .pViewports = &viewport,
		.scissorCount = 1, .pScissors = &scissor,
	};
	VkPipelineRasterizationStateCreateInfo raster = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
		.polygonMode = VK_POLYGON_MODE_FILL,
		.cullMode = VK_CULL_MODE_NONE,
		.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
		.lineWidth = 1.0f,
	};
	VkPipelineMultisampleStateCreateInfo multisample = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
		.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
	};
	VkPipelineColorBlendAttachmentState blendAttachment = {
		.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
			| VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
	};
	VkPipelineColorBlendStateCreateInfo blend = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
		.attachmentCount = 1,
		.pAttachments = &blendAttachment,
	};
	VkPipelineRenderingCreateInfo rendering = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
		.colorAttachmentCount = 1,
		.pColorAttachmentFormats = &kFormat,
		.depthAttachmentFormat = useDepth ? kDepthFormat : VK_FORMAT_UNDEFINED,
		.stencilAttachmentFormat = useDepth ? kDepthFormat : VK_FORMAT_UNDEFINED,
	};
	VkGraphicsPipelineCreateInfo gpci = {
		.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
		.pNext = &rendering,
		.stageCount = 2,
		.pStages = stages,
		.pVertexInputState = &vertexInput,
		.pInputAssemblyState = &inputAssembly,
		.pViewportState = &viewportState,
		.pRasterizationState = &raster,
		.pMultisampleState = &multisample,
		.pDepthStencilState = useDepth ? &depthState : NULL,
		.pColorBlendState = &blend,
		.layout = layout,
	};
	VkPipeline pipeline;
	CHECK(vkCreateGraphicsPipelines(sDevice, VK_NULL_HANDLE, 1, &gpci, NULL, &pipeline),
		"vkCreateGraphicsPipelines");

	struct buffer readback = create_buffer(4,
		VK_BUFFER_USAGE_TRANSFER_DST_BIT,
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

	printf("%s: %ux%u, %u full screen triangles per pass\n", name, kWidth, kHeight, kDrawsPerPass);
	double best = 0;
	for (int pass = 0; pass < 5; pass++) {
		VkCommandBuffer cb = begin_commands();

		VkImageMemoryBarrier toAttachment = {
			.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
			.srcAccessMask = 0,
			.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
			.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
			.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
			.image = image,
			.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
		};
		vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
			VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, NULL, 0, NULL, 1, &toAttachment);

		VkRenderingAttachmentInfo depthAttachment = {
			.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
			.imageView = depthView,
			.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
			.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
			.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
			.clearValue = { .depthStencil = { 1.0f, 0 } },
		};
		if (useDepth) {
			VkImageMemoryBarrier toDepth = {
				.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
				.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
				.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
				.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
				.image = depthImage,
				.subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,
					0, 1, 0, 1 },
			};
			vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
				VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT, 0, 0, NULL, 0, NULL, 1, &toDepth);
		}

		VkRenderingAttachmentInfo attachment = {
			.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
			.imageView = view,
			.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
			.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
			.storeOp = VK_ATTACHMENT_STORE_OP_STORE,
			.clearValue = { .color = { .float32 = { 0, 0, 0, 1 } } },
		};
		VkRenderingInfo renderingInfo = {
			.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
			.renderArea = { { 0, 0 }, { kWidth, kHeight } },
			.layerCount = 1,
			.colorAttachmentCount = 1,
			.pColorAttachments = &attachment,
			.pDepthAttachment = useDepth ? &depthAttachment : NULL,
			.pStencilAttachment = useDepth ? &depthAttachment : NULL,
		};
		vkCmdBeginRendering(cb, &renderingInfo);
		vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
		if (useVertexBuffer) {
			VkDeviceSize offset = 0;
			vkCmdBindVertexBuffers(cb, 0, 1, &vertices.buffer, &offset);
		}
		for (uint32_t i = 0; i < kDrawsPerPass; i++) {
			vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(i), &i);
			vkCmdDraw(cb, 3, 1, 0, 0);
		}
		vkCmdEndRendering(cb);

		VkImageMemoryBarrier toSource = {
			.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
			.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
			.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
			.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
			.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			.image = image,
			.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
		};
		vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
			VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &toSource);

		VkBufferImageCopy copy = {
			.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
			.imageOffset = { 100, 100, 0 },
			.imageExtent = { 1, 1, 1 },
		};
		vkCmdCopyImageToBuffer(cb, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			readback.buffer, 1, &copy);

		double elapsed = run_commands(cb);
		double pixels = (double)kWidth * kHeight * kDrawsPerPass;
		double gpixels = pixels / elapsed / 1e9;
		printf("  pass %d: %6.1f ms, %6.2f Gpixel/s, %8.1f fps at %ux%u\n",
			pass, elapsed * 1000.0, gpixels, kDrawsPerPass / elapsed, kWidth, kHeight);
		if (gpixels > best)
			best = gpixels;
	}
	uint8_t *pixel = readback.map;
	printf("  pixel(100,100) = %u %u %u %u (%s)\n", pixel[0], pixel[1], pixel[2], pixel[3],
		pixel[3] == 255 && (pixel[0] | pixel[1] | pixel[2]) != 0 ? "shaded" : "SUSPICIOUS");
	printf("  best: %.2f Gpixel/s\n", best);

	destroy_buffer(&readback);
	if (useVertexBuffer)
		destroy_buffer(&vertices);
	if (useDepth) {
		vkDestroyImageView(sDevice, depthView, NULL);
		vkDestroyImage(sDevice, depthImage, NULL);
		vkFreeMemory(sDevice, depthMemory, NULL);
	}
	vkDestroyPipeline(sDevice, pipeline, NULL);
	vkDestroyShaderModule(sDevice, vert, NULL);
	vkDestroyShaderModule(sDevice, frag, NULL);
	vkDestroyPipelineLayout(sDevice, layout, NULL);
	vkDestroyImageView(sDevice, view, NULL);
	vkDestroyImage(sDevice, image, NULL);
	vkFreeMemory(sDevice, imageMemory, NULL);
}

static void bench_fill(void)
{
	bench_fill_ex(false, false, "fill");
}

static void bench_depth(void)
{
	bench_fill_ex(true, false, "depth");
}

static void bench_vbo(void)
{
	bench_fill_ex(false, true, "vbo");
}

// How a frame reaches the screen today: the GPU copies the rendered image into
// host memory, and the CPU then reads it out of that mapping.
static void bench_readback(void)
{
	const uint32_t kWidth = 1920, kHeight = 1080;
	const VkDeviceSize kSize = (VkDeviceSize)kWidth * kHeight * 4;
	const VkFormat kFormat = VK_FORMAT_R8G8B8A8_UNORM;

	VkImageCreateInfo ici = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.imageType = VK_IMAGE_TYPE_2D,
		.format = kFormat,
		.extent = { kWidth, kHeight, 1 },
		.mipLevels = 1,
		.arrayLayers = 1,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.tiling = VK_IMAGE_TILING_OPTIMAL,
		.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	};
	VkImage image;
	CHECK(vkCreateImage(sDevice, &ici, NULL, &image), "vkCreateImage");
	VkMemoryRequirements req;
	vkGetImageMemoryRequirements(sDevice, image, &req);
	VkMemoryAllocateInfo mai = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = req.size,
		.memoryTypeIndex = find_memory(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
	};
	VkDeviceMemory imageMemory;
	CHECK(vkAllocateMemory(sDevice, &mai, NULL, &imageMemory), "vkAllocateMemory");
	CHECK(vkBindImageMemory(sDevice, image, imageMemory, 0), "vkBindImageMemory");

	struct buffer host = create_buffer(kSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	// The same copy, but staying in video memory: that is what presenting
	// straight into the screen's frame buffer would cost.
	struct buffer local = create_buffer(kSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
		VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	void *copy = malloc(kSize);

	printf("readback: %ux%u (%llu MiB per frame)\n", kWidth, kHeight,
		(unsigned long long)(kSize / (1024 * 1024)));
	double bestGpu = 0, bestCpu = 0;
	for (int pass = 0; pass < 5; pass++) {
		VkCommandBuffer cb = begin_commands();
		VkImageMemoryBarrier toSource = {
			.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
			.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
			.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
			.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			.image = image,
			.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
		};
		vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
			VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &toSource);
		VkBufferImageCopy region = {
			.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
			.imageExtent = { kWidth, kHeight, 1 },
		};
		vkCmdCopyImageToBuffer(cb, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			host.buffer, 1, &region);
		double gpu = run_commands(cb);

		VkCommandBuffer cb2 = begin_commands();
		vkCmdPipelineBarrier(cb2, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
			VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &toSource);
		vkCmdCopyImageToBuffer(cb2, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			local.buffer, 1, &region);
		double gpuLocal = run_commands(cb2);

		double start = now_s();
		memcpy(copy, host.map, kSize);
		double cpu = now_s() - start;

		printf("  pass %d: to host %6.2f ms (%5.1f GB/s), cpu read %6.2f ms (%5.2f GB/s)"
			", to video memory %6.2f ms (%5.1f GB/s)\n", pass, gpu * 1000.0,
			kSize / gpu / 1e9, cpu * 1000.0, kSize / cpu / 1e9,
			gpuLocal * 1000.0, kSize / gpuLocal / 1e9);
		if (kSize / gpu / 1e9 > bestGpu)
			bestGpu = kSize / gpu / 1e9;
		if (kSize / cpu / 1e9 > bestCpu)
			bestCpu = kSize / cpu / 1e9;
	}
	printf("  best: gpu %.1f GB/s, cpu read %.2f GB/s\n", bestGpu, bestCpu);

	free(copy);
	destroy_buffer(&host);
	destroy_buffer(&local);
	vkDestroyImage(sDevice, image, NULL);
	vkFreeMemory(sDevice, imageMemory, NULL);
}

// How long a submission takes when it has nothing to do: the floor under
// every frame.
static void bench_submit(void)
{
	printf("submit: empty command buffers\n");
	double best = 1e9;
	for (int pass = 0; pass < 5; pass++) {
		const int kCount = 20;
		double start = now_s();
		for (int i = 0; i < kCount; i++) {
			VkCommandBuffer cb = begin_commands();
			run_commands(cb);
		}
		double each = (now_s() - start) / kCount;
		printf("  pass %d: %6.3f ms per submission\n", pass, each * 1000.0);
		if (each < best)
			best = each;
	}
	printf("  best: %.3f ms per submission\n", best * 1000.0);
}

// The presentation path we want: the GPU writes the frame straight into memory
// the application already owns, with no copy through the CPU.
static void bench_hostimport(void)
{
	const uint32_t kWidth = 1920, kHeight = 1080;
	const VkDeviceSize kSize = (VkDeviceSize)kWidth * kHeight * 4;

	PFN_vkGetMemoryHostPointerPropertiesEXT getProperties =
		(PFN_vkGetMemoryHostPointerPropertiesEXT)vkGetDeviceProcAddr(sDevice,
			"vkGetMemoryHostPointerPropertiesEXT");
	if (getProperties == NULL) {
		printf("hostimport: VK_EXT_external_memory_host is not available\n");
		return;
	}

	// Memory of our own, page aligned, standing in for a window's bitmap.
	void *bits = NULL;
	if (posix_memalign(&bits, 4096, kSize) != 0 || bits == NULL) {
		printf("hostimport: out of memory\n");
		return;
	}
	memset(bits, 0, kSize);

	VkMemoryHostPointerPropertiesEXT props = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT,
	};
	VkResult r = getProperties(sDevice,
		VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, bits, &props);
	if (r != VK_SUCCESS) {
		printf("hostimport: the driver will not describe the pointer: %d\n", r);
		free(bits);
		return;
	}

	uint32_t type = UINT32_MAX;
	for (uint32_t i = 0; i < 32; i++) {
		if (props.memoryTypeBits & (1u << i)) { type = i; break; }
	}
	VkImportMemoryHostPointerInfoEXT import = {
		.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT,
		.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
		.pHostPointer = bits,
	};
	VkMemoryAllocateInfo mai = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.pNext = &import,
		.allocationSize = kSize,
		.memoryTypeIndex = type,
	};
	VkDeviceMemory memory;
	r = vkAllocateMemory(sDevice, &mai, NULL, &memory);
	if (r != VK_SUCCESS) {
		printf("hostimport: import failed: %d\n", r);
		free(bits);
		return;
	}

	VkBufferCreateInfo bci = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		.size = kSize,
		.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
	};
	VkBuffer buffer;
	CHECK(vkCreateBuffer(sDevice, &bci, NULL, &buffer), "vkCreateBuffer");
	CHECK(vkBindBufferMemory(sDevice, buffer, memory, 0), "vkBindBufferMemory");

	// Fill a buffer in video memory and copy that into our memory: the same
	// direction a frame travels when it is presented.
	struct buffer staging = create_buffer(kSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	memset(staging.map, 0x5a, kSize);
	struct buffer src = create_buffer(kSize,
		VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
		VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	{
		VkCommandBuffer cb = begin_commands();
		VkBufferCopy region = { .size = kSize };
		vkCmdCopyBuffer(cb, staging.buffer, src.buffer, 1, &region);
		run_commands(cb);
	}

	printf("hostimport: %llu MiB of the application's own memory\n",
		(unsigned long long)(kSize / (1024 * 1024)));
	double best = 0;
	for (int pass = 0; pass < 5; pass++) {
		VkCommandBuffer cb = begin_commands();
		VkBufferCopy region = { .size = kSize };
		vkCmdCopyBuffer(cb, src.buffer, buffer, 1, &region);
		double elapsed = run_commands(cb);
		double gbs = kSize / elapsed / 1e9;
		printf("  pass %d: %6.2f ms, %5.1f GB/s\n", pass, elapsed * 1000.0, gbs);
		if (gbs > best)
			best = gbs;
	}

	const uint8_t *written = bits;
	bool ok = written[0] == 0x5a && written[kSize / 2] == 0x5a
		&& written[kSize - 1] == 0x5a;
	printf("  the GPU wrote our memory: %s\n", ok ? "yes" : "NO");
	printf("  best: %.1f GB/s\n", best);

	destroy_buffer(&src);
	destroy_buffer(&staging);
	vkDestroyBuffer(sDevice, buffer, NULL);
	vkFreeMemory(sDevice, memory, NULL);
	free(bits);
}

// Draw into the screen itself. The accelerant publishes its frame buffer and
// the driver shares it, so the GPU can put a frame there without it crossing
// the bus - the whole point of the exercise. The test paints visible bands so
// that the monitor confirms what the numbers claim.
static void bench_scanout(void)
{
	VkImportScanoutMemoryHAIKU scanout = {
		.sType = VK_STRUCTURE_TYPE_IMPORT_SCANOUT_MEMORY_HAIKU,
	};
	uint32_t type = find_memory(~0u, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	VkMemoryAllocateInfo mai = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.pNext = &scanout,
		.allocationSize = 0,
		.memoryTypeIndex = type,
	};
	VkDeviceMemory memory;
	VkResult r = vkAllocateMemory(sDevice, &mai, NULL, &memory);
	if (r != VK_SUCCESS) {
		printf("scanout: the driver would not hand over the screen: %d\n", r);
		return;
	}
	printf("scanout: the screen is %" PRIu32 "x%" PRIu32 ", %" PRIu32
		" bytes per row, %llu MiB\n", scanout.width, scanout.height,
		scanout.rowPitch, (unsigned long long)(scanout.size / (1024 * 1024)));

	VkBufferCreateInfo bci = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		.size = scanout.size,
		.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
	};
	VkBuffer screen;
	CHECK(vkCreateBuffer(sDevice, &bci, NULL, &screen), "vkCreateBuffer");
	CHECK(vkBindBufferMemory(sDevice, screen, memory, 0), "vkBindBufferMemory");

	// A frame of the same size sitting in video memory, as a rendered frame
	// would be, and a band of it copied over the screen.
	struct buffer frame = create_buffer(scanout.size,
		VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
		VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

	const uint32_t kBandHeight = 200;
	const uint32_t kBandY = 700;
	const VkDeviceSize bandBytes = (VkDeviceSize)kBandHeight * scanout.rowPitch;
	const VkDeviceSize bandOffset = (VkDeviceSize)kBandY * scanout.rowPitch;

	{
		VkCommandBuffer cb = begin_commands();
		vkCmdFillBuffer(cb, frame.buffer, 0, scanout.size, 0xff20c020);
		run_commands(cb);
	}

	printf("  a %" PRIu32 "-row band, %llu KiB, straight into the screen\n",
		kBandHeight, (unsigned long long)(bandBytes / 1024));
	double best = 0;
	for (int pass = 0; pass < 5; pass++) {
		VkCommandBuffer cb = begin_commands();
		VkBufferCopy region = {
			.srcOffset = bandOffset,
			.dstOffset = bandOffset,
			.size = bandBytes,
		};
		vkCmdCopyBuffer(cb, frame.buffer, screen, 1, &region);
		double elapsed = run_commands(cb);
		double gbs = bandBytes / elapsed / 1e9;
		printf("    pass %d: %6.3f ms, %5.1f GB/s\n", pass, elapsed * 1000.0, gbs);
		if (gbs > best)
			best = gbs;
	}
	printf("  best: %.1f GB/s\n", best);

	// And the whole screen, which is what a full-screen present costs.
	double bestFull = 0;
	for (int pass = 0; pass < 5; pass++) {
		VkCommandBuffer cb = begin_commands();
		VkBufferCopy region = { .size = scanout.size };
		vkCmdCopyBuffer(cb, frame.buffer, screen, 1, &region);
		double elapsed = run_commands(cb);
		double gbs = scanout.size / elapsed / 1e9;
		if (gbs > bestFull)
			bestFull = gbs;
		if (pass == 4) {
			printf("  the whole screen: %6.3f ms, %5.1f GB/s"
				" (%.0f frames per second)\n",
				elapsed * 1000.0, bestFull, 1.0 / elapsed);
		}
	}

	destroy_buffer(&frame);
	vkDestroyBuffer(sDevice, screen, NULL);
	vkFreeMemory(sDevice, memory, NULL);
}

// Where the cost of moving a rendered frame actually sits: the same pixels
// copied tiled to tiled, tiled to linear, and linear to linear, all inside
// video memory.
static void bench_detile(void)
{
	const uint32_t kWidth = 1920, kHeight = 1080;
	const VkDeviceSize kSize = (VkDeviceSize)kWidth * kHeight * 4;
	const VkFormat kFormat = VK_FORMAT_R8G8B8A8_UNORM;

	VkImage images[3];
	VkDeviceMemory memories[3];
	// 0 and 1 are tiled, 2 is linear.
	for (int i = 0; i < 3; i++) {
		VkImageCreateInfo ici = {
			.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
			.imageType = VK_IMAGE_TYPE_2D,
			.format = kFormat,
			.extent = { kWidth, kHeight, 1 },
			.mipLevels = 1,
			.arrayLayers = 1,
			.samples = VK_SAMPLE_COUNT_1_BIT,
			.tiling = i == 2 ? VK_IMAGE_TILING_LINEAR : VK_IMAGE_TILING_OPTIMAL,
			.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
			.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		};
		CHECK(vkCreateImage(sDevice, &ici, NULL, &images[i]), "vkCreateImage");
		VkMemoryRequirements req;
		vkGetImageMemoryRequirements(sDevice, images[i], &req);
		VkMemoryAllocateInfo mai = {
			.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
			.allocationSize = req.size,
			.memoryTypeIndex = find_memory(req.memoryTypeBits,
				VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
		};
		CHECK(vkAllocateMemory(sDevice, &mai, NULL, &memories[i]), "vkAllocateMemory");
		CHECK(vkBindImageMemory(sDevice, images[i], memories[i], 0), "vkBindImageMemory");
	}

	struct buffer dst = create_buffer(kSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
		VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	struct buffer src = create_buffer(kSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
		VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

	printf("detile: %ux%u (%llu MiB), all within video memory\n", kWidth, kHeight,
		(unsigned long long)(kSize / (1024 * 1024)));

	static const char *kNames[] = {
		"tiled -> tiled image", "tiled -> linear buffer",
		"linear image -> buffer", "buffer -> buffer",
	};
	// Several copies per submission, so what is measured is the copy rate and
	// not the cost of submitting.
	const int kPerPass = 8;
	for (int which = 0; which < 4; which++) {
		double best = 0;
		for (int pass = 0; pass < 3; pass++) {
			VkCommandBuffer cb = begin_commands();
			for (int i = 0; i < 3; i++) {
				VkImageMemoryBarrier bar = {
					.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
					.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
					.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
					.newLayout = VK_IMAGE_LAYOUT_GENERAL,
					.image = images[i],
					.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
				};
				vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
					VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &bar);
			}

			VkBufferImageCopy region = {
				.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
				.imageExtent = { kWidth, kHeight, 1 },
			};
			for (int n = 0; n < kPerPass; n++)
			switch (which) {
			case 0: {
				VkImageCopy copy = {
					.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
					.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
					.extent = { kWidth, kHeight, 1 },
				};
				vkCmdCopyImage(cb, images[0], VK_IMAGE_LAYOUT_GENERAL,
					images[1], VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
				break;
			}
			case 1:
				vkCmdCopyImageToBuffer(cb, images[0], VK_IMAGE_LAYOUT_GENERAL,
					dst.buffer, 1, &region);
				break;
			case 2:
				vkCmdCopyImageToBuffer(cb, images[2], VK_IMAGE_LAYOUT_GENERAL,
					dst.buffer, 1, &region);
				break;
			default: {
				VkBufferCopy bc = { .size = kSize };
				vkCmdCopyBuffer(cb, src.buffer, dst.buffer, 1, &bc);
				break;
			}
			}
			double elapsed = run_commands(cb);
			double gbs = (double)kSize * kPerPass / elapsed / 1e9;
			if (gbs > best)
				best = gbs;
		}
		printf("  %-24s %6.1f GB/s\n", kNames[which], best);
	}

	destroy_buffer(&dst);
	destroy_buffer(&src);
	for (int i = 0; i < 3; i++) {
		vkDestroyImage(sDevice, images[i], NULL);
		vkFreeMemory(sDevice, memories[i], NULL);
	}
}

static void bench_copy(void)
{
	const VkDeviceSize kSize = 128 * 1024 * 1024;
	const uint32_t kCopiesPerPass = 8;

	struct buffer src = create_buffer(kSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
		VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	struct buffer dst = create_buffer(kSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
		VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

	printf("copy: %llu MiB in video memory, %u copies per pass\n",
		(unsigned long long)(kSize / (1024 * 1024)), kCopiesPerPass);
	double best = 0;
	for (int pass = 0; pass < 5; pass++) {
		VkCommandBuffer cb = begin_commands();
		VkBufferCopy copy = { .size = kSize };
		for (uint32_t i = 0; i < kCopiesPerPass; i++)
			vkCmdCopyBuffer(cb, src.buffer, dst.buffer, 1, &copy);
		double elapsed = run_commands(cb);

		// A copy reads and writes the whole buffer.
		double bytes = (double)kSize * kCopiesPerPass * 2;
		double gbs = bytes / elapsed / 1e9;
		printf("  pass %d: %6.1f ms, %6.1f GB/s\n", pass, elapsed * 1000.0, gbs);
		if (gbs > best)
			best = gbs;
	}
	printf("  best: %.1f GB/s\n", best);

	destroy_buffer(&src);
	destroy_buffer(&dst);
}

static void bench_upload(void)
{
	const VkDeviceSize kSize = 64 * 1024 * 1024;
	const uint32_t kCopiesPerPass = 4;

	struct buffer host = create_buffer(kSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	struct buffer device = create_buffer(kSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
		VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	memset(host.map, 0xa5, kSize);

	printf("upload: %llu MiB from host memory, %u copies per pass\n",
		(unsigned long long)(kSize / (1024 * 1024)), kCopiesPerPass);
	double best = 0;
	for (int pass = 0; pass < 5; pass++) {
		VkCommandBuffer cb = begin_commands();
		VkBufferCopy copy = { .size = kSize };
		for (uint32_t i = 0; i < kCopiesPerPass; i++)
			vkCmdCopyBuffer(cb, host.buffer, device.buffer, 1, &copy);
		double elapsed = run_commands(cb);

		double gbs = (double)kSize * kCopiesPerPass / elapsed / 1e9;
		printf("  pass %d: %6.1f ms, %6.1f GB/s\n", pass, elapsed * 1000.0, gbs);
		if (gbs > best)
			best = gbs;
	}
	printf("  best: %.1f GB/s\n", best);

	destroy_buffer(&host);
	destroy_buffer(&device);
}


int main(int argc, char **argv)
{
	setvbuf(stdout, NULL, _IONBF, 0);

	const char *shaderDir = getenv("VKBENCH_SHADERS");
	if (shaderDir != NULL)
		sShaderDir = shaderDir;

	VkApplicationInfo app = {
		.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
		.pApplicationName = "vkbench",
		.apiVersion = VK_API_VERSION_1_3,
	};
	VkInstanceCreateInfo ici = {
		.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
		.pApplicationInfo = &app,
	};
	CHECK(vkCreateInstance(&ici, NULL, &sInstance), "vkCreateInstance");

	uint32_t count = 0;
	vkEnumeratePhysicalDevices(sInstance, &count, NULL);
	if (count == 0) {
		fprintf(stderr, "FAIL no Vulkan device\n");
		return 1;
	}
	VkPhysicalDevice devices[8];
	count = count > 8 ? 8 : count;
	vkEnumeratePhysicalDevices(sInstance, &count, devices);
	sPhysicalDevice = devices[0];

	VkPhysicalDeviceProperties props;
	vkGetPhysicalDeviceProperties(sPhysicalDevice, &props);
	vkGetPhysicalDeviceMemoryProperties(sPhysicalDevice, &sMemoryProperties);
	printf("device: %s (Vulkan %u.%u.%u)\n", props.deviceName,
		VK_API_VERSION_MAJOR(props.apiVersion), VK_API_VERSION_MINOR(props.apiVersion),
		VK_API_VERSION_PATCH(props.apiVersion));
	for (uint32_t i = 0; i < sMemoryProperties.memoryHeapCount; i++) {
		printf("  heap %u: %llu MiB%s\n", i,
			(unsigned long long)(sMemoryProperties.memoryHeaps[i].size / (1024 * 1024)),
			(sMemoryProperties.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0
				? " (video memory)" : "");
	}

	vkGetPhysicalDeviceQueueFamilyProperties(sPhysicalDevice, &count, NULL);
	VkQueueFamilyProperties families[8];
	count = count > 8 ? 8 : count;
	vkGetPhysicalDeviceQueueFamilyProperties(sPhysicalDevice, &count, families);
	sQueueFamily = UINT32_MAX;
	for (uint32_t i = 0; i < count; i++) {
		if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0) {
			sQueueFamily = i;
			break;
		}
	}
	if (sQueueFamily == UINT32_MAX) {
		fprintf(stderr, "FAIL no graphics queue\n");
		return 1;
	}

	float priority = 1.0f;
	VkDeviceQueueCreateInfo qci = {
		.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
		.queueFamilyIndex = sQueueFamily,
		.queueCount = 1,
		.pQueuePriorities = &priority,
	};
	VkPhysicalDeviceVulkan13Features features13 = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
		.dynamicRendering = VK_TRUE,
	};
	// Enable importing our own memory, if the driver offers it.
	uint32_t extCount = 0;
	vkEnumerateDeviceExtensionProperties(sPhysicalDevice, NULL, &extCount, NULL);
	VkExtensionProperties *exts = calloc(extCount, sizeof(*exts));
	vkEnumerateDeviceExtensionProperties(sPhysicalDevice, NULL, &extCount, exts);
	const char *wanted[1];
	uint32_t wantedCount = 0;
	for (uint32_t i = 0; i < extCount; i++) {
		if (strcmp(exts[i].extensionName, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME) == 0)
			wanted[wantedCount++] = VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME;
	}

	VkDeviceCreateInfo dci = {
		.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
		.pNext = &features13,
		.queueCreateInfoCount = 1,
		.pQueueCreateInfos = &qci,
		.enabledExtensionCount = wantedCount,
		.ppEnabledExtensionNames = wanted,
	};
	CHECK(vkCreateDevice(sPhysicalDevice, &dci, NULL, &sDevice), "vkCreateDevice");
	free(exts);
	vkGetDeviceQueue(sDevice, sQueueFamily, 0, &sQueue);

	VkCommandPoolCreateInfo cpci = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
		.queueFamilyIndex = sQueueFamily,
	};
	CHECK(vkCreateCommandPool(sDevice, &cpci, NULL, &sCommandPool), "vkCreateCommandPool");

	bool all = argc < 2;
	for (int i = 1; i < argc || all; i++) {
		const char *what = all ? NULL : argv[i];
		if (all || strcmp(what, "compute") == 0)
			bench_compute();
		if (all || strcmp(what, "fill") == 0)
			bench_fill();
		if (all || strcmp(what, "depth") == 0)
			bench_depth();
		if (all || strcmp(what, "vbo") == 0)
			bench_vbo();
		if (all || strcmp(what, "submit") == 0)
			bench_submit();
		if (all || strcmp(what, "hostimport") == 0)
			bench_hostimport();
		if (strcmp(what, "scanout") == 0)
			bench_scanout();
		if (all || strcmp(what, "detile") == 0)
			bench_detile();
		if (all || strcmp(what, "readback") == 0)
			bench_readback();
		if (all || strcmp(what, "copy") == 0)
			bench_copy();
		if (all || strcmp(what, "upload") == 0)
			bench_upload();
		if (all)
			break;
	}

	vkDestroyCommandPool(sDevice, sCommandPool, NULL);
	vkDestroyDevice(sDevice, NULL);
	vkDestroyInstance(sInstance, NULL);
	return 0;
}
