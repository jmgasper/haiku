/* Minimal Vulkan bring-up test: device info, clear an image on the GPU, read it back. */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <vulkan/vulkan.h>

#define CHECK(x, what) do { VkResult r = (x); if (r != VK_SUCCESS) { printf("FAIL %s: %d\n", what, r); return 1; } } while (0)

static uint32_t find_mem(VkPhysicalDevice pd, uint32_t mask, VkMemoryPropertyFlags want)
{
	VkPhysicalDeviceMemoryProperties mp;
	vkGetPhysicalDeviceMemoryProperties(pd, &mp);
	for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
		if ((mask & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want)
			return i;
	return UINT32_MAX;
}

int main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_3 };
	VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
	VkInstance inst;
	CHECK(vkCreateInstance(&ici, NULL, &inst), "vkCreateInstance");

	uint32_t n = 0;
	vkEnumeratePhysicalDevices(inst, &n, NULL);
	printf("physical devices: %u\n", n);
	if (n == 0) return 1;
	VkPhysicalDevice pds[8];
	vkEnumeratePhysicalDevices(inst, &n, pds);
	VkPhysicalDevice pd = pds[0];
	VkPhysicalDeviceProperties props;
	vkGetPhysicalDeviceProperties(pd, &props);
	printf("device: %s api %u.%u.%u driver %#x type %d\n", props.deviceName,
		VK_API_VERSION_MAJOR(props.apiVersion), VK_API_VERSION_MINOR(props.apiVersion),
		VK_API_VERSION_PATCH(props.apiVersion), props.driverVersion, props.deviceType);

	uint32_t qn = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, NULL);
	VkQueueFamilyProperties qf[8];
	vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, qf);
	uint32_t qfi = UINT32_MAX;
	for (uint32_t i = 0; i < qn; i++)
		if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { qfi = i; break; }
	printf("graphics queue family: %u\n", qfi);

	float prio = 1.0f;
	VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = qfi, .queueCount = 1, .pQueuePriorities = &prio };
	VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci };
	VkDevice dev;
	CHECK(vkCreateDevice(pd, &dci, NULL, &dev), "vkCreateDevice");
	VkQueue q;
	vkGetDeviceQueue(dev, qfi, 0, &q);
	printf("device created\n");

	/* host visible buffer to read back */
	VkImageCreateInfo imci = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
		.format = VK_FORMAT_R8G8B8A8_UNORM, .extent = {16, 16, 1}, .mipLevels = 1, .arrayLayers = 1,
		.samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
		.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT };
	VkImage img;
	CHECK(vkCreateImage(dev, &imci, NULL, &img), "vkCreateImage");
	VkMemoryRequirements mr;
	vkGetImageMemoryRequirements(dev, img, &mr);
	VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
		.memoryTypeIndex = find_mem(pd, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
	VkDeviceMemory imem;
	CHECK(vkAllocateMemory(dev, &mai, NULL, &imem), "alloc image memory");
	CHECK(vkBindImageMemory(dev, img, imem, 0), "bind image");

	VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 16 * 16 * 4, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT };
	VkBuffer buf;
	CHECK(vkCreateBuffer(dev, &bci, NULL, &buf), "vkCreateBuffer");
	vkGetBufferMemoryRequirements(dev, buf, &mr);
	mai.allocationSize = mr.size;
	mai.memoryTypeIndex = find_mem(pd, mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
	VkDeviceMemory bmem;
	CHECK(vkAllocateMemory(dev, &mai, NULL, &bmem), "alloc buffer memory");
	CHECK(vkBindBufferMemory(dev, buf, bmem, 0), "bind buffer");

	VkCommandPoolCreateInfo cpci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = qfi };
	VkCommandPool pool;
	CHECK(vkCreateCommandPool(dev, &cpci, NULL, &pool), "command pool");
	VkCommandBufferAllocateInfo cbai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
	VkCommandBuffer cb;
	CHECK(vkAllocateCommandBuffers(dev, &cbai, &cb), "alloc command buffer");
	VkCommandBufferBeginInfo cbbi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
	CHECK(vkBeginCommandBuffer(cb, &cbbi), "begin");
	VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
	VkImageMemoryBarrier bar = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = img, .subresourceRange = range };
	vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &bar);
	VkClearColorValue color = { .float32 = { 0.25f, 0.5f, 0.75f, 1.0f } };
	vkCmdClearColorImage(cb, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &range);
	bar.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	bar.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &bar);
	VkBufferImageCopy copy = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = {16, 16, 1} };
	vkCmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &copy);
	CHECK(vkEndCommandBuffer(cb), "end");
	VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb };
	CHECK(vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE), "submit");
	CHECK(vkQueueWaitIdle(q), "wait idle");
	{
		/* buffer to buffer copy through the copy engine */
		VkBuffer src, dst; VkDeviceMemory smem, dmem;
		VkBufferCreateInfo b2 = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 4096,
			.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT };
		CHECK(vkCreateBuffer(dev, &b2, NULL, &src), "src buffer");
		CHECK(vkCreateBuffer(dev, &b2, NULL, &dst), "dst buffer");
		vkGetBufferMemoryRequirements(dev, src, &mr);
		mai.allocationSize = mr.size;
		mai.memoryTypeIndex = find_mem(pd, mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
		CHECK(vkAllocateMemory(dev, &mai, NULL, &smem), "src mem");
		CHECK(vkAllocateMemory(dev, &mai, NULL, &dmem), "dst mem");
		vkBindBufferMemory(dev, src, smem, 0);
		vkBindBufferMemory(dev, dst, dmem, 0);
		uint8_t *sp, *dp;
		vkMapMemory(dev, smem, 0, VK_WHOLE_SIZE, 0, (void**)&sp);
		for (int i = 0; i < 4096; i++) sp[i] = (uint8_t)(i * 7 + 3);
		vkUnmapMemory(dev, smem);
		VkCommandBuffer cb2;
		CHECK(vkAllocateCommandBuffers(dev, &cbai, &cb2), "alloc cb2");
		CHECK(vkBeginCommandBuffer(cb2, &cbbi), "begin cb2");
		VkBufferCopy bc = { 0, 0, 4096 };
		vkCmdCopyBuffer(cb2, src, dst, 1, &bc);
		CHECK(vkEndCommandBuffer(cb2), "end cb2");
		VkSubmitInfo si2 = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb2 };
		CHECK(vkQueueSubmit(q, 1, &si2, VK_NULL_HANDLE), "submit cb2");
		CHECK(vkQueueWaitIdle(q), "wait cb2");
		usleep(1000000);
		vkMapMemory(dev, dmem, 0, VK_WHOLE_SIZE, 0, (void**)&dp);
		int same = 1;
		for (int i = 0; i < 4096; i++) if (dp[i] != (uint8_t)(i * 7 + 3)) { same = 0; break; }
		printf("buffer copy: %s (dst[0..3] = %u %u %u %u)\n", same ? "OK" : "MISMATCH", dp[0], dp[1], dp[2], dp[3]);
		vkUnmapMemory(dev, dmem);
	}
	usleep(1000000);
	uint8_t *p;
	CHECK(vkMapMemory(dev, bmem, 0, VK_WHOLE_SIZE, 0, (void**)&p), "map");
	printf("pixel[0] = %u %u %u %u (expect 64 128 191 255)\n", p[0], p[1], p[2], p[3]);
	int ok = p[0] >= 63 && p[0] <= 65 && p[1] >= 127 && p[1] <= 129 && p[2] >= 190 && p[2] <= 192;
	vkUnmapMemory(dev, bmem);
	vkDestroyDevice(dev, NULL);
	vkDestroyInstance(inst, NULL);
	printf(ok ? "VKPROBE PASS\n" : "VKPROBE FAIL\n");
	return ok ? 0 : 1;
}
