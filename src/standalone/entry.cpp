#include <layer.hpp>
#include <device.hpp>
#include <image.hpp>
#include <cb.hpp>
#include <queue.hpp>
#include <sync.hpp>
#include <buffer.hpp>
#include <window.hpp>
#include <gui/gui.hpp>
#include <gui/imageViewer.hpp>
#include <util/export.hpp>
#include <util/dl.hpp>
#include <nytl/bytes.hpp>
#include <vkutil/sync.hpp>
#include <gui/gui.hpp>
#include <swa/swa.h>
#include <imgio/image.hpp>
#include <cstdlib>
#include <csignal>

using namespace vil;

VkBool32 VKAPI_PTR messengerCallback(
		VkDebugUtilsMessageSeverityFlagBitsEXT severity,
		VkDebugUtilsMessageTypeFlagsEXT type,
		const VkDebugUtilsMessengerCallbackDataEXT* pData,
		void* userData) {
	(void) type;
	(void) userData;

	auto& data = *pData;

	// check if message is ignored
	std::vector<std::string> ignore = {};
	auto ig = std::find(ignore.begin(), ignore.end(), data.pMessageIdName);
	if(ig != ignore.end()) {
		return false;
	}

	auto level = dlg_level_trace;
	switch(severity) {
		case VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT:
			level = dlg_level_error;
			break;
		case VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT:
			level = dlg_level_warn;
			break;
		case VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT:
			level = dlg_level_info;
			break;
		case VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT:
			level = dlg_level_debug;
			break;
		default:
			break;
	}

	dlg_tags("DebugCallback");
	dlg_log(level, "{} ({})", data.pMessage, data.pMessageIdName);

	for(auto i = 0u; i < data.objectCount; ++i) {
		auto& obj = data.pObjects[i];
		if(obj.pObjectName) {
			dlg_log(level, "    involving '{}'", obj.pObjectName);
		}
	}

	for(auto i = 0u; i < data.queueLabelCount; ++i) {
		auto& label = data.pQueueLabels[i];
		auto name = label.pLabelName ? label.pLabelName : "<unnamed>";
		dlg_log(level, "    queue label '{}'", name);
	}

	for(auto i = 0u; i < data.cmdBufLabelCount; ++i) {
		auto& label = data.pCmdBufLabels[i];
		auto name = label.pLabelName ? label.pLabelName : "<unnamed>";
		dlg_log(level, "    cmd label '{}'", name);
	}

	// debug break
	// std::raise(SIGTRAP);

	return false;
}

struct ViewedImage {
	VkImage img;
	VkDeviceMemory mem;
	vil::Image* vilImg;
};

struct SizeAlign {
	VkDeviceSize size;
	VkDeviceSize align;
};

SizeAlign neededUploadSizeAlign(const vil::Device& dev,
		const imgio::ImageProvider& source) {
	auto fmt = source.format();
	auto [bx, by, bz] = imgio::blockSize(fmt);
	auto fmtSize = imgio::formatElementSize(fmt);
	auto size = source.size();
	auto numElements = ceilDivide(size.x, bx) * ceilDivide(size.y, by) * ceilDivide(size.z, bz);
	auto layerCount = source.layers();
	auto dataSize = layerCount * fmtSize * numElements;
	auto fillLevelCount = source.mipLevels();

	for(auto i = 1u; i < fillLevelCount; ++i) {
		auto isize = size;
		isize.x = std::max(size.x >> i, 1u);
		isize.y = std::max(size.y >> i, 1u);
		isize.z = std::max(size.z >> i, 1u);
		auto numElements = ceilDivide(isize.x, bx) * ceilDivide(isize.y, by) * ceilDivide(isize.z, bz);
		dataSize += layerCount * fmtSize * numElements;
	}

	auto align = std::max<VkDeviceSize>(fmtSize,
		dev.props.limits.optimalBufferCopyOffsetAlignment);
	return {dataSize, align};
}

ViewedImage loadImage(const char* path, VkDevice dev, vil::Device& vilDev, Gui& gui) {
	// create image
	auto provider = imgio::loadImage(path);
	if(!provider) {
		dlg_trace("error loading image '{}'", path);
		return {};
	}

	VkImageCreateInfo ici {};
	ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	ici.format = static_cast<VkFormat>(provider->format());
	ici.arrayLayers = provider->layers();
	ici.mipLevels = provider->mipLevels();
	ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	ici.extent = {provider->size().x, provider->size().y, provider->size().z};
	ici.tiling = VK_IMAGE_TILING_OPTIMAL;
	ici.samples = VK_SAMPLE_COUNT_1_BIT;
	ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT |
		VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
		VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	if(provider->size().z > 1) {
		ici.imageType = VK_IMAGE_TYPE_3D;
	} else if(provider->size().y > 1) {
		ici.imageType = VK_IMAGE_TYPE_2D;
	} else {
		ici.imageType = VK_IMAGE_TYPE_1D;
	}

	VkImage img;
	VK_CHECK(vil::CreateImage(dev, &ici, nullptr, &img));

	VkMemoryRequirements memReqs;
	vil::GetImageMemoryRequirements(dev, img, &memReqs);

	VkMemoryAllocateInfo mai {};
	mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	mai.memoryTypeIndex = findLSB(memReqs.memoryTypeBits);
	mai.allocationSize = memReqs.size;
	VkDeviceMemory mem;
	VK_CHECK(vil::AllocateMemory(dev, &mai, nullptr, &mem));
	VK_CHECK(vil::BindImageMemory(dev, img, mem, 0u));
	auto& vilImg = unwrap(img);

	// = upload =
	// alloc cb
	// create staging buffer
	// record cb while reading data into staging buffer
	// submit & wait for cb

	VkCommandPool cmdPool;
	VkCommandPoolCreateInfo cpci {};
	cpci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	cpci.queueFamilyIndex = gui.usedQueue().family;
	VK_CHECK(vilDev.dispatch.CreateCommandPool(vilDev.handle, &cpci, nullptr, &cmdPool));
	nameHandle(vilDev, cmdPool, "imgUpload");

	VkCommandBufferAllocateInfo cbai {};
	cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	cbai.commandBufferCount = 1u;
	cbai.commandPool = cmdPool;
	cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	VkCommandBuffer cb;
	VK_CHECK(vilDev.dispatch.AllocateCommandBuffers(dev, &cbai, &cb));
	nameHandle(vilDev, cb, "imgUpload");

	auto needed = neededUploadSizeAlign(vilDev, *provider);
	vil::OwnBuffer buf;
	buf.ensure(vilDev, needed.size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, {}, "imgUpload");

	WriteBuf bufData{buf.map, buf.size};
	VkCommandBufferBeginInfo cbi {};
	cbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	vilDev.dispatch.BeginCommandBuffer(cb, &cbi);

	vku::cmdBarrier(vilDev, cb, vilImg.handle, vku::SyncScope::discard(),
		vku::SyncScope::transferWrite());

	auto [bx, by, bz] = imgio::blockSize(provider->format());
	auto fmtSize = imgio::formatElementSize(provider->format());
	for(auto m = 0u; m < provider->mipLevels(); ++m) {
		VkExtent3D ext = ici.extent;
		ext.width = std::max(ext.width >> m, 1u);
		ext.height = std::max(ext.height >> m, 1u);
		ext.depth = std::max(ext.depth >> m, 1u);

		auto numElements = ceilDivide(ext.width, bx) * ceilDivide(ext.height, by) * ceilDivide(ext.depth, bz);
		auto layerSize = fmtSize * numElements;
		auto off = bufData.data() - buf.map;
		for(auto i = 0u; i < provider->layers(); ++i) {
			provider->read(bufData.first(layerSize), m, i);
			skip(bufData, layerSize);
		}

		VkBufferImageCopy copy {};
		copy.imageExtent = ext;
		copy.bufferOffset = off;
		copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		copy.imageSubresource.baseArrayLayer = 0u;
		copy.imageSubresource.layerCount = provider->layers();
		copy.imageSubresource.mipLevel = m;
		vilDev.dispatch.CmdCopyBufferToImage(cb, buf.buf, vilImg.handle,
			VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1u, &copy);
	}

	vku::cmdBarrier(vilDev, cb, vilImg.handle, vku::SyncScope::transferWrite(),
		vku::SyncScope::fragmentRead());

	vilDev.dispatch.EndCommandBuffer(cb);

	// submit & wait for cb
	VkSubmitInfo si {};
	si.commandBufferCount = 1u;
	si.pCommandBuffers = &cb;
	si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	vilDev.dispatch.QueueSubmit(gui.usedQueue().handle, 1u, &si, VK_NULL_HANDLE);
	vilDev.dispatch.QueueWaitIdle(gui.usedQueue().handle);

	vilDev.dispatch.DestroyCommandPool(dev, cmdPool, nullptr);

	return {img, mem, &vilImg};
}

extern "C" VIL_EXPORT int vil_showImageViewer(int argc, const char** argv) {
#if defined(_WIN32) || defined(__CYGWIN__)
	auto libvulkan = vil::dlopen("vulkan-1.dll");
#else // Unix
	auto libvulkan = vil::dlopen("libvulkan.so");
#endif

	auto pfnGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr) vil::dlsym(
		libvulkan, "vkGetInstanceProcAddr");
	dlg_assert(pfnGetInstanceProcAddr);

	auto layers = std::array {
		"VK_LAYER_KHRONOS_validation",
	};

	auto exts = std::vector {
		VK_EXT_DEBUG_UTILS_EXTENSION_NAME,
	};

#define LOAD_INSTANCE_PROC(ini, name) \
	auto pfn##name = (PFN_vk##name) pfnGetInstanceProcAddr(ini, "vk" #name)

	{
		auto foundDebugUtils = false;
		auto foundSurface = false;
		auto foundPlatformExt = false;
		LOAD_INSTANCE_PROC(VK_NULL_HANDLE, EnumerateInstanceExtensionProperties);

		u32 extCount = 0u;
		VK_CHECK(pfnEnumerateInstanceExtensionProperties(nullptr, &extCount, nullptr));
		std::vector<VkExtensionProperties> extProps(extCount);
		VK_CHECK(pfnEnumerateInstanceExtensionProperties(nullptr, &extCount, extProps.data()));

		for(auto& ext : extProps) {
			auto name = ext.extensionName;
			dlg_info("Instance ext: {}, version {}", name, ext.specVersion);

			if(std::strcmp(name, VK_KHR_SURFACE_EXTENSION_NAME) == 0) {
				exts.push_back(VK_KHR_SURFACE_EXTENSION_NAME);
				foundSurface = true;
			} else if(std::strcmp(name, VK_EXT_DEBUG_UTILS_EXTENSION_NAME) == 0) {
				foundDebugUtils = true;
			} else if(std::strcmp(name, "VK_KHR_wayland_surface") == 0) {
				exts.push_back("VK_KHR_wayland_surface");
				foundPlatformExt = true;
			} else if(std::strcmp(name, "VK_KHR_win32_surface") == 0) {
				exts.push_back("VK_KHR_win32_surface");
				foundPlatformExt = true;
			} else if(std::strcmp(name, "VK_KHR_xcb_surface") == 0) {
				exts.push_back("VK_KHR_xcb_surface");
				foundPlatformExt = true;
			}
		}

		dlg_assert(foundDebugUtils);
		dlg_assert(foundSurface);
		dlg_assert(foundPlatformExt);
	}

	VkApplicationInfo appInfo {};
	appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	appInfo.apiVersion = VK_API_VERSION_1_2;

	VkInstanceCreateInfo ici {};
	ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	ici.ppEnabledLayerNames = layers.data();
	ici.enabledLayerCount = layers.size();
	ici.ppEnabledExtensionNames = exts.data();
	ici.enabledExtensionCount = exts.size();
	ici.pApplicationInfo = &appInfo;

	VkDebugUtilsMessengerCreateInfoEXT dumci {};
	dumci.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
	dumci.messageType =
		VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
		VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT |
		VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT;
	dumci.messageSeverity =
		VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT |
		VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT;
	dumci.pfnUserCallback = messengerCallback;
	ici.pNext = &dumci;

	StandaloneInstanceInfo sii {};
	sii.getProcAddr = pfnGetInstanceProcAddr;

	VkInstance ini;
	VK_CHECK(vil::doCreateInstance(&ici, nullptr, &ini, &sii));

	LOAD_INSTANCE_PROC(ini, CreateDebugUtilsMessengerEXT);
	LOAD_INSTANCE_PROC(ini, DestroyDebugUtilsMessengerEXT);
	VkDebugUtilsMessengerEXT debugMessenger;
	pfnCreateDebugUtilsMessengerEXT(ini, &dumci, nullptr, &debugMessenger);

	// create device
	dlg_trace("Creating device");

	LOAD_INSTANCE_PROC(ini, EnumeratePhysicalDevices);
	LOAD_INSTANCE_PROC(ini, GetPhysicalDeviceQueueFamilyProperties);
	LOAD_INSTANCE_PROC(ini, GetPhysicalDeviceFeatures2);

	// TODO
	// LOAD_INSTANCE_PROC(ini, GetDeviceProcAddr);
	auto pfnGetDeviceProcAddr = (PFN_vkGetDeviceProcAddr) vil::dlsym(
		libvulkan, "vkGetDeviceProcAddr");
	dlg_assert(pfnGetInstanceProcAddr);

	u32 phdevCount = 10u;
	VkPhysicalDevice phdevs[10];
	VK_CHECK(pfnEnumeratePhysicalDevices(ini, &phdevCount, phdevs));

	if(phdevCount == 0u) {
		dlg_error("no physical device found");
		return -1;
	}

	auto phdev = phdevs[0];

	u32 qpropsCount = 0u;
	pfnGetPhysicalDeviceQueueFamilyProperties(phdev, &qpropsCount, nullptr);

	std::vector<VkQueueFamilyProperties> qprops(qpropsCount);
	pfnGetPhysicalDeviceQueueFamilyProperties(phdev, &qpropsCount, qprops.data());

	u32 qfamGfx {0xFFFFFFFFu};
	u32 qfamAsyncCompute {0xFFFFFFFFu};
	for(auto i = 0u; i < qprops.size(); ++i) {
		if(qprops[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) {
			qfamGfx = i;
		} else if(qprops[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
			qfamAsyncCompute = i;
		}
	}

	dlg_assert(qfamGfx != 0xFFFFFFFFu);
	if(qfamAsyncCompute == 0xFFFFFFFFu) {
		qfamAsyncCompute = qfamGfx;
	}

	// query supported features
	VkPhysicalDeviceFeatures2 supFeatures {};
	VkPhysicalDeviceVulkan11Features sup11 {};
	VkPhysicalDeviceVulkan12Features sup12 {};

	supFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
	sup11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
	sup12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;

	supFeatures.pNext = &sup11;
	sup11.pNext = &sup12;

	pfnGetPhysicalDeviceFeatures2(phdev, &supFeatures);

	// activate features we might need
	VkPhysicalDeviceFeatures features {};

	VkPhysicalDeviceVulkan11Features features11 {};
	features11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;

	VkPhysicalDeviceVulkan12Features features12 {};
	features12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
	if(sup12.bufferDeviceAddress) {
		features12.bufferDeviceAddress = true;
	}
	if(sup12.timelineSemaphore) {
		features12.timelineSemaphore = true;
	}

	const float prio1[] = {1.f, 1.f};
	VkDeviceQueueCreateInfo qcis[2] {};
	u32 qcCount = 1u;
	qcis[0].sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
	qcis[0].queueCount = 1u;
	qcis[0].queueFamilyIndex = qfamGfx;
	qcis[0].pQueuePriorities = prio1;

	if(qfamAsyncCompute != qfamGfx) {
		++qcCount;
		qcis[1].sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
		qcis[1].queueCount = 1u;
		qcis[1].queueFamilyIndex = qfamAsyncCompute;
		qcis[1].pQueuePriorities = prio1;
	} else {
		// NOTE: can't always do this
		// ++qcis[0].queueCount;
	}

	auto devExts = std::vector {
		VK_KHR_SWAPCHAIN_EXTENSION_NAME,
	};

	VkDeviceCreateInfo dci {};
	dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
	dci.pQueueCreateInfos = qcis;
	dci.queueCreateInfoCount = qcCount;
	dci.pEnabledFeatures = &features;
	dci.enabledExtensionCount = devExts.size();
	dci.ppEnabledExtensionNames = devExts.data();

	dci.pNext = &features11;
	features11.pNext = &features12;

	StandaloneDeviceInfo sdi {};
	sdi.getInstanceProcAddr = pfnGetInstanceProcAddr;
	sdi.getDeviceProcAddr = pfnGetDeviceProcAddr;

	VkDevice dev;
	VK_CHECK(vil::doCreateDevice(phdev, &dci, nullptr, &dev, &sdi));

	// at this point, there should be an instance and device
	// NOTE: we don't strictly need the mutex here as long as integration
	// test is single-threaded but keep it for good practice.
	vil::Device* vilDev {};

	{
		std::lock_guard lock(vil::dataMutex);
		dlg_assert(!vil::devByLoaderTable.empty());
		vilDev = vil::devByLoaderTable.begin()->second;
	}

	const char* name = "test.ktx";
	if(argc > 1) {
		name = argv[1];
	}

	auto* gui = vilDev->gui();
	auto [img, mem, vilImg] = loadImage(name, dev, *vilDev, *gui);
	if(img) {
		if(gui) {
			gui->mode_ = Gui::Mode::image;
		}

		auto& iv = gui->standaloneImageViewer();
		VkImageSubresourceRange subres {};
		subres.layerCount = vilImg->ci.arrayLayers;
		subres.levelCount = vilImg->ci.mipLevels;
		subres.aspectMask = aspects(vilImg->ci.format);
		auto flags = ImageViewer::supportsTransferSrc;

		iv.select(vilImg->handle, vilImg->ci.extent, vilImg->ci.imageType,
			vilImg->ci.format, subres, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, flags);

		vilDev->window->allowClose = true;
		vilDev->window->doMainLoop();
		vilDev->window->doCleanup();
	}

	// destroy image
	vil::DestroyImage(dev, img, nullptr);
	vil::FreeMemory(dev, mem, nullptr);

	// shutdown
	vil::DestroyDevice(dev, nullptr);
	pfnDestroyDebugUtilsMessengerEXT(ini, debugMessenger, nullptr);
	vil::DestroyInstance(ini, nullptr);
	vil::dlclose(libvulkan);

	return 0;
}
