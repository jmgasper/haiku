/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

// Headless Vulkan test for Mesa's v3dv on Haiku: no loader and no window
// system, the driver library is linked directly (vk_icdGetInstanceProcAddr).
// It clears a colour image in a render pass, copies it into a buffer and
// checks every pixel; then it does the same with a triangle drawn over the
// clear (hand-assembled SPIR-V, there is no shader compiler in the lab).
//   rpi4_vk_probe [clear|triangle]   (default: both)


#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>


extern PFN_vkVoidFunction vk_icdGetInstanceProcAddr(VkInstance instance,
	const char* name);

#define WIDTH	256
#define HEIGHT	256

static VkInstance sInstance;
static VkPhysicalDevice sPhysicalDevice;
static VkDevice sDevice;
static VkQueue sQueue;
static uint32_t sQueueFamily;
static PFN_vkGetDeviceProcAddr sGetDeviceProcAddr;

#define INSTANCE_FN(name) \
	PFN_##name name = (PFN_##name)vk_icdGetInstanceProcAddr(sInstance, #name); \
	if (name == NULL) { printf("FAIL: no " #name "\n"); exit(1); }
#define DEVICE_FN(name) \
	PFN_##name name = (PFN_##name)sGetDeviceProcAddr(sDevice, #name); \
	if (name == NULL) { printf("FAIL: no " #name "\n"); exit(1); }
#define CHECK(call) \
	do { \
		VkResult result_ = (call); \
		if (result_ != VK_SUCCESS) { \
			printf("FAIL: " #call ": %d\n", (int)result_); \
			exit(1); \
		} \
	} while (0)


// SPIR-V, assembled by hand.
//
// vertex:   layout(location = 0) in vec4 position;
//           void main() { gl_Position = position; }
// fragment: layout(location = 0) out vec4 color;
//           void main() { color = vec4(1.0, 0.0, 0.0, 1.0); }

#define OP(count, opcode) (((uint32_t)(count) << 16) | (opcode))

static const uint32_t kVertexShader[] = {
	0x07230203, 0x00010000, 0, 20, 0,
	OP(2, 17), 1,							// OpCapability Shader
	OP(3, 14), 0, 1,						// OpMemoryModel Logical GLSL450
	// OpEntryPoint Vertex %4 "main" %10 %12
	OP(7, 15), 0, 4, 0x6e69616d, 0x00000000, 10, 12,
	OP(5, 72), 8, 0, 11, 0,					// OpMemberDecorate %8 0 BuiltIn Position
	OP(3, 71), 8, 2,						// OpDecorate %8 Block (struct)
	OP(4, 71), 12, 30, 0,					// OpDecorate %12 Location 0
	OP(2, 19), 2,							// %2 = OpTypeVoid
	OP(3, 33), 3, 2,						// %3 = OpTypeFunction %2
	OP(3, 22), 5, 32,						// %5 = OpTypeFloat 32
	OP(4, 23), 6, 5, 4,						// %6 = OpTypeVector %5 4
	OP(3, 30), 8, 6,						// %8 = OpTypeStruct %6
	OP(4, 32), 9, 3, 8,						// %9 = OpTypePointer Output %8
	OP(4, 59), 9, 10, 3,					// %10 = OpVariable %9 Output
	OP(4, 32), 11, 1, 6,					// %11 = OpTypePointer Input %6
	OP(4, 59), 11, 12, 1,					// %12 = OpVariable %11 Input
	OP(4, 21), 13, 32, 1,					// %13 = OpTypeInt 32 1
	OP(4, 43), 13, 14, 0,					// %14 = OpConstant %13 0
	OP(4, 32), 15, 3, 6,					// %15 = OpTypePointer Output %6
	OP(5, 54), 2, 4, 0, 3,					// %4 = OpFunction %2 None %3
	OP(2, 248), 16,							// %16 = OpLabel
	OP(4, 61), 6, 17, 12,					// %17 = OpLoad %6 %12
	OP(5, 65), 15, 18, 10, 14,				// %18 = OpAccessChain %15 %10 %14
	OP(3, 62), 18, 17,						// OpStore %18 %17
	OP(1, 253),								// OpReturn
	OP(1, 56),								// OpFunctionEnd
};

static const uint32_t kFragmentShader[] = {
	0x07230203, 0x00010000, 0, 16, 0,
	OP(2, 17), 1,							// OpCapability Shader
	OP(3, 14), 0, 1,						// OpMemoryModel Logical GLSL450
	// OpEntryPoint Fragment %4 "main" %9
	OP(6, 15), 4, 4, 0x6e69616d, 0x00000000, 9,
	OP(3, 16), 4, 7,						// OpExecutionMode %4 OriginUpperLeft
	OP(4, 71), 9, 30, 0,					// OpDecorate %9 Location 0
	OP(2, 19), 2,							// %2 = OpTypeVoid
	OP(3, 33), 3, 2,						// %3 = OpTypeFunction %2
	OP(3, 22), 5, 32,						// %5 = OpTypeFloat 32
	OP(4, 23), 6, 5, 4,						// %6 = OpTypeVector %5 4
	OP(4, 32), 8, 3, 6,						// %8 = OpTypePointer Output %6
	OP(4, 59), 8, 9, 3,						// %9 = OpVariable %8 Output
	OP(4, 43), 5, 10, 0x3f800000,			// %10 = OpConstant %5 1.0
	OP(4, 43), 5, 11, 0x00000000,			// %11 = OpConstant %5 0.0
	// %12 = OpConstantComposite %6 %10 %11 %11 %10
	OP(7, 44), 6, 12, 10, 11, 11, 10,
	OP(5, 54), 2, 4, 0, 3,					// %4 = OpFunction %2 None %3
	OP(2, 248), 13,							// %13 = OpLabel
	OP(3, 62), 9, 12,						// OpStore %9 %12
	OP(1, 253),								// OpReturn
	OP(1, 56),								// OpFunctionEnd
};


static uint32_t
find_memory_type(uint32_t typeBits, VkMemoryPropertyFlags wanted)
{
	INSTANCE_FN(vkGetPhysicalDeviceMemoryProperties);
	VkPhysicalDeviceMemoryProperties properties;
	vkGetPhysicalDeviceMemoryProperties(sPhysicalDevice, &properties);
	for (uint32_t i = 0; i < properties.memoryTypeCount; i++) {
		if ((typeBits & (1u << i)) != 0
			&& (properties.memoryTypes[i].propertyFlags & wanted) == wanted) {
			return i;
		}
	}
	printf("FAIL: no memory type\n");
	exit(1);
}


static int
render(int triangle)
{
	DEVICE_FN(vkCreateImage);
	DEVICE_FN(vkGetImageMemoryRequirements);
	DEVICE_FN(vkAllocateMemory);
	DEVICE_FN(vkBindImageMemory);
	DEVICE_FN(vkCreateImageView);
	DEVICE_FN(vkCreateBuffer);
	DEVICE_FN(vkGetBufferMemoryRequirements);
	DEVICE_FN(vkBindBufferMemory);
	DEVICE_FN(vkMapMemory);
	DEVICE_FN(vkCreateRenderPass);
	DEVICE_FN(vkCreateFramebuffer);
	DEVICE_FN(vkCreateCommandPool);
	DEVICE_FN(vkAllocateCommandBuffers);
	DEVICE_FN(vkBeginCommandBuffer);
	DEVICE_FN(vkCmdBeginRenderPass);
	DEVICE_FN(vkCmdEndRenderPass);
	DEVICE_FN(vkCmdCopyImageToBuffer);
	DEVICE_FN(vkEndCommandBuffer);
	DEVICE_FN(vkCreateFence);
	DEVICE_FN(vkQueueSubmit);
	DEVICE_FN(vkWaitForFences);
	DEVICE_FN(vkCreateShaderModule);
	DEVICE_FN(vkCreatePipelineLayout);
	DEVICE_FN(vkCreateGraphicsPipelines);
	DEVICE_FN(vkCmdBindPipeline);
	DEVICE_FN(vkCmdBindVertexBuffers);
	DEVICE_FN(vkCmdDraw);

	// the colour image
	VkImageCreateInfo imageInfo = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.imageType = VK_IMAGE_TYPE_2D,
		.format = VK_FORMAT_R8G8B8A8_UNORM,
		.extent = { WIDTH, HEIGHT, 1 },
		.mipLevels = 1,
		.arrayLayers = 1,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.tiling = VK_IMAGE_TILING_OPTIMAL,
		.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
			| VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	};
	VkImage image;
	CHECK(vkCreateImage(sDevice, &imageInfo, NULL, &image));
	VkMemoryRequirements requirements;
	vkGetImageMemoryRequirements(sDevice, image, &requirements);
	VkMemoryAllocateInfo allocateInfo = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = requirements.size,
		.memoryTypeIndex = find_memory_type(requirements.memoryTypeBits, 0),
	};
	VkDeviceMemory imageMemory;
	CHECK(vkAllocateMemory(sDevice, &allocateInfo, NULL, &imageMemory));
	CHECK(vkBindImageMemory(sDevice, image, imageMemory, 0));

	VkImageViewCreateInfo viewInfo = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		.image = image,
		.viewType = VK_IMAGE_VIEW_TYPE_2D,
		.format = VK_FORMAT_R8G8B8A8_UNORM,
		.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
	};
	VkImageView view;
	CHECK(vkCreateImageView(sDevice, &viewInfo, NULL, &view));

	// the buffer the picture is read back through, and the vertices
	const VkMemoryPropertyFlags kMappable
		= VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
			| VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	VkBufferCreateInfo bufferInfo = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		.size = WIDTH * HEIGHT * 4,
		.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
	};
	VkBuffer buffer;
	CHECK(vkCreateBuffer(sDevice, &bufferInfo, NULL, &buffer));
	vkGetBufferMemoryRequirements(sDevice, buffer, &requirements);
	allocateInfo.allocationSize = requirements.size;
	allocateInfo.memoryTypeIndex = find_memory_type(
		requirements.memoryTypeBits, kMappable);
	VkDeviceMemory bufferMemory;
	CHECK(vkAllocateMemory(sDevice, &allocateInfo, NULL, &bufferMemory));
	CHECK(vkBindBufferMemory(sDevice, buffer, bufferMemory, 0));
	uint8_t* pixels;
	CHECK(vkMapMemory(sDevice, bufferMemory, 0, VK_WHOLE_SIZE, 0,
		(void**)&pixels));
	memset(pixels, 0xa5, WIDTH * HEIGHT * 4);

	// the lower left half in clip space; y points down in Vulkan
	static const float kVertices[] = {
		-1, -1, 0, 1,
		-1, 1, 0, 1,
		1, 1, 0, 1,
	};
	bufferInfo.size = sizeof(kVertices);
	bufferInfo.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
	VkBuffer vertexBuffer;
	CHECK(vkCreateBuffer(sDevice, &bufferInfo, NULL, &vertexBuffer));
	vkGetBufferMemoryRequirements(sDevice, vertexBuffer, &requirements);
	allocateInfo.allocationSize = requirements.size;
	allocateInfo.memoryTypeIndex = find_memory_type(
		requirements.memoryTypeBits, kMappable);
	VkDeviceMemory vertexMemory;
	CHECK(vkAllocateMemory(sDevice, &allocateInfo, NULL, &vertexMemory));
	CHECK(vkBindBufferMemory(sDevice, vertexBuffer, vertexMemory, 0));
	void* vertexData;
	CHECK(vkMapMemory(sDevice, vertexMemory, 0, VK_WHOLE_SIZE, 0, &vertexData));
	memcpy(vertexData, kVertices, sizeof(kVertices));

	// one subpass that clears the image and leaves it ready for the copy
	VkAttachmentDescription attachment = {
		.format = VK_FORMAT_R8G8B8A8_UNORM,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
		.storeOp = VK_ATTACHMENT_STORE_OP_STORE,
		.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
		.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	};
	VkAttachmentReference reference = { 0,
		VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
	VkSubpassDescription subpass = {
		.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
		.colorAttachmentCount = 1,
		.pColorAttachments = &reference,
	};
	VkRenderPassCreateInfo renderPassInfo = {
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
		.attachmentCount = 1,
		.pAttachments = &attachment,
		.subpassCount = 1,
		.pSubpasses = &subpass,
	};
	VkRenderPass renderPass;
	CHECK(vkCreateRenderPass(sDevice, &renderPassInfo, NULL, &renderPass));

	VkFramebufferCreateInfo framebufferInfo = {
		.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
		.renderPass = renderPass,
		.attachmentCount = 1,
		.pAttachments = &view,
		.width = WIDTH,
		.height = HEIGHT,
		.layers = 1,
	};
	VkFramebuffer framebuffer;
	CHECK(vkCreateFramebuffer(sDevice, &framebufferInfo, NULL, &framebuffer));

	VkPipeline pipeline = VK_NULL_HANDLE;
	if (triangle) {
		VkShaderModuleCreateInfo moduleInfo = {
			.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
			.codeSize = sizeof(kVertexShader),
			.pCode = kVertexShader,
		};
		VkShaderModule vertexModule, fragmentModule;
		CHECK(vkCreateShaderModule(sDevice, &moduleInfo, NULL, &vertexModule));
		moduleInfo.codeSize = sizeof(kFragmentShader);
		moduleInfo.pCode = kFragmentShader;
		CHECK(vkCreateShaderModule(sDevice, &moduleInfo, NULL,
			&fragmentModule));

		VkPipelineLayoutCreateInfo layoutInfo = {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		};
		VkPipelineLayout layout;
		CHECK(vkCreatePipelineLayout(sDevice, &layoutInfo, NULL, &layout));

		VkPipelineShaderStageCreateInfo stages[2] = {
			{
				.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
				.stage = VK_SHADER_STAGE_VERTEX_BIT,
				.module = vertexModule,
				.pName = "main",
			},
			{
				.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
				.stage = VK_SHADER_STAGE_FRAGMENT_BIT,
				.module = fragmentModule,
				.pName = "main",
			},
		};
		VkVertexInputBindingDescription binding = { 0, 16,
			VK_VERTEX_INPUT_RATE_VERTEX };
		VkVertexInputAttributeDescription attribute = { 0, 0,
			VK_FORMAT_R32G32B32A32_SFLOAT, 0 };
		VkPipelineVertexInputStateCreateInfo vertexInput = {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
			.vertexBindingDescriptionCount = 1,
			.pVertexBindingDescriptions = &binding,
			.vertexAttributeDescriptionCount = 1,
			.pVertexAttributeDescriptions = &attribute,
		};
		VkPipelineInputAssemblyStateCreateInfo inputAssembly = {
			.sType
				= VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
			.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
		};
		VkViewport viewport = { 0, 0, WIDTH, HEIGHT, 0, 1 };
		VkRect2D scissor = { { 0, 0 }, { WIDTH, HEIGHT } };
		VkPipelineViewportStateCreateInfo viewportState = {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
			.viewportCount = 1,
			.pViewports = &viewport,
			.scissorCount = 1,
			.pScissors = &scissor,
		};
		VkPipelineRasterizationStateCreateInfo rasterization = {
			.sType
				= VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
			.polygonMode = VK_POLYGON_MODE_FILL,
			.cullMode = VK_CULL_MODE_NONE,
			.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
			.lineWidth = 1,
		};
		VkPipelineMultisampleStateCreateInfo multisample = {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
			.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
		};
		VkPipelineColorBlendAttachmentState blendAttachment = {
			.colorWriteMask = 0xf,
		};
		VkPipelineColorBlendStateCreateInfo blend = {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
			.attachmentCount = 1,
			.pAttachments = &blendAttachment,
		};
		VkGraphicsPipelineCreateInfo pipelineInfo = {
			.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
			.stageCount = 2,
			.pStages = stages,
			.pVertexInputState = &vertexInput,
			.pInputAssemblyState = &inputAssembly,
			.pViewportState = &viewportState,
			.pRasterizationState = &rasterization,
			.pMultisampleState = &multisample,
			.pColorBlendState = &blend,
			.layout = layout,
			.renderPass = renderPass,
		};
		CHECK(vkCreateGraphicsPipelines(sDevice, VK_NULL_HANDLE, 1,
			&pipelineInfo, NULL, &pipeline));
		printf("PASS: pipeline with hand-assembled SPIR-V\n");
	}

	VkCommandPoolCreateInfo poolInfo = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		.queueFamilyIndex = sQueueFamily,
	};
	VkCommandPool pool;
	CHECK(vkCreateCommandPool(sDevice, &poolInfo, NULL, &pool));
	VkCommandBufferAllocateInfo commandInfo = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = pool,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		.commandBufferCount = 1,
	};
	VkCommandBuffer commands;
	CHECK(vkAllocateCommandBuffers(sDevice, &commandInfo, &commands));

	VkCommandBufferBeginInfo beginInfo = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
	};
	CHECK(vkBeginCommandBuffer(commands, &beginInfo));
	VkClearValue clear = { .color = { .float32 = { 0.0f, 0.0f, 1.0f, 1.0f } } };
	VkRenderPassBeginInfo passInfo = {
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
		.renderPass = renderPass,
		.framebuffer = framebuffer,
		.renderArea = { { 0, 0 }, { WIDTH, HEIGHT } },
		.clearValueCount = 1,
		.pClearValues = &clear,
	};
	vkCmdBeginRenderPass(commands, &passInfo, VK_SUBPASS_CONTENTS_INLINE);
	if (triangle) {
		vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
		VkDeviceSize offset = 0;
		vkCmdBindVertexBuffers(commands, 0, 1, &vertexBuffer, &offset);
		vkCmdDraw(commands, 3, 1, 0, 0);
	}
	vkCmdEndRenderPass(commands);
	VkBufferImageCopy region = {
		.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
		.imageExtent = { WIDTH, HEIGHT, 1 },
	};
	vkCmdCopyImageToBuffer(commands, image,
		VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &region);
	CHECK(vkEndCommandBuffer(commands));

	VkFenceCreateInfo fenceInfo = {
		.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
	};
	VkFence fence;
	CHECK(vkCreateFence(sDevice, &fenceInfo, NULL, &fence));
	VkSubmitInfo submitInfo = {
		.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		.commandBufferCount = 1,
		.pCommandBuffers = &commands,
	};
	CHECK(vkQueueSubmit(sQueue, 1, &submitInfo, fence));
	CHECK(vkWaitForFences(sDevice, 1, &fence, VK_TRUE, 10000000000ull));

	int red = 0, blue = 0, other = 0;
	for (int i = 0; i < WIDTH * HEIGHT; i++) {
		const uint8_t* p = pixels + 4 * i;
		if (p[0] == 255 && p[1] == 0 && p[2] == 0 && p[3] == 255)
			red++;
		else if (p[0] == 0 && p[1] == 0 && p[2] == 255 && p[3] == 255)
			blue++;
		else
			other++;
	}
	printf("%s: %d red, %d blue, %d other of %d; first pixel %u %u %u %u\n",
		triangle ? "triangle" : "clear", red, blue, other, WIDTH * HEIGHT,
		pixels[0], pixels[1], pixels[2], pixels[3]);

	int ok;
	if (triangle) {
		// half the picture, give or take the diagonal
		ok = other == 0 && red > WIDTH * HEIGHT / 2 - WIDTH
			&& red < WIDTH * HEIGHT / 2 + WIDTH;
	} else
		ok = blue == WIDTH * HEIGHT;
	printf("%s: %s\n", ok ? "PASS" : "FAIL",
		triangle ? "triangle over a clear" : "clear only");
	return ok;
}


int
main(int argc, char** argv)
{
	setvbuf(stdout, NULL, _IONBF, 0);

	PFN_vkCreateInstance vkCreateInstance = (PFN_vkCreateInstance)
		vk_icdGetInstanceProcAddr(NULL, "vkCreateInstance");
	if (vkCreateInstance == NULL) {
		printf("FAIL: the driver has no vkCreateInstance\n");
		return 1;
	}

	VkApplicationInfo application = {
		.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
		.pApplicationName = "rpi4_vk_probe",
		.apiVersion = VK_API_VERSION_1_1,
	};
	VkInstanceCreateInfo instanceInfo = {
		.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
		.pApplicationInfo = &application,
	};
	CHECK(vkCreateInstance(&instanceInfo, NULL, &sInstance));
	printf("PASS: instance\n");

	INSTANCE_FN(vkEnumeratePhysicalDevices);
	INSTANCE_FN(vkGetPhysicalDeviceProperties);
	INSTANCE_FN(vkGetPhysicalDeviceQueueFamilyProperties);
	INSTANCE_FN(vkCreateDevice);
	INSTANCE_FN(vkGetDeviceProcAddr);
	sGetDeviceProcAddr = vkGetDeviceProcAddr;

	uint32_t count = 1;
	VkResult result = vkEnumeratePhysicalDevices(sInstance, &count,
		&sPhysicalDevice);
	if ((result != VK_SUCCESS && result != VK_INCOMPLETE) || count == 0) {
		printf("FAIL: no physical device (%d)\n", (int)result);
		return 1;
	}

	VkPhysicalDeviceProperties properties;
	vkGetPhysicalDeviceProperties(sPhysicalDevice, &properties);
	printf("device: %s\nVulkan %u.%u.%u, driver version %u.%u.%u\n",
		properties.deviceName, VK_VERSION_MAJOR(properties.apiVersion),
		VK_VERSION_MINOR(properties.apiVersion),
		VK_VERSION_PATCH(properties.apiVersion),
		VK_VERSION_MAJOR(properties.driverVersion),
		VK_VERSION_MINOR(properties.driverVersion),
		VK_VERSION_PATCH(properties.driverVersion));

	VkQueueFamilyProperties families[4];
	count = 4;
	vkGetPhysicalDeviceQueueFamilyProperties(sPhysicalDevice, &count, families);
	sQueueFamily = 0;
	for (uint32_t i = 0; i < count; i++) {
		if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0) {
			sQueueFamily = i;
			break;
		}
	}

	float priority = 1;
	VkDeviceQueueCreateInfo queueInfo = {
		.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
		.queueFamilyIndex = sQueueFamily,
		.queueCount = 1,
		.pQueuePriorities = &priority,
	};
	VkDeviceCreateInfo deviceInfo = {
		.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
		.queueCreateInfoCount = 1,
		.pQueueCreateInfos = &queueInfo,
	};
	CHECK(vkCreateDevice(sPhysicalDevice, &deviceInfo, NULL, &sDevice));
	DEVICE_FN(vkGetDeviceQueue);
	vkGetDeviceQueue(sDevice, sQueueFamily, 0, &sQueue);
	printf("PASS: device and queue\n");

	int ok = 1;
	if (argc < 2 || strcmp(argv[1], "clear") == 0)
		ok &= render(0);
	if (argc < 2 || strcmp(argv[1], "triangle") == 0)
		ok &= render(1);

	printf("%s\n", ok ? "VK PROBE PASSED" : "VK PROBE FAILED");
	return ok ? 0 : 1;
}
