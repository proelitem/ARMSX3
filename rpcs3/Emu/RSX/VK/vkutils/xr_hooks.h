#pragma once

// ARMSX3: presentation through an XR runtime (OpenXR), without the core knowing OpenXR.
//
// A standalone headset's compositor does not take a VkSwapchainKHR. The runtime hands the
// application VkImages of its own, expects the VkInstance and VkDevice to be created through
// it (XR_KHR_vulkan_enable2), and names the VkPhysicalDevice the session must run on. All of
// that is platform and runtime specific, so it lives with the Android glue (android/src/xr/),
// and the renderer only consults this table at the four places where the decision is made:
// instance creation, physical device choice, device creation and swapchain construction.
//
// No hooks installed means the normal WSI path, byte for byte. The split mirrors
// vk_android_loader.h / adrenotools: the Qualcomm specific part stays app side, the core
// keeps a function table.
//
// Header only on purpose: nothing here needs a translation unit, and keeping it out of the
// rpcs3_emu source list means the desktop build never sees it change.

#include "../VulkanAPI.h"
#include "util/types.hpp"

namespace vk
{
	class physical_device;
	class swapchain_base;

	namespace xr
	{
		struct hooks
		{
			// vkCreateInstance stand-in. The XR runtime adds whatever instance extensions it needs
			// to `info` before creating (xrCreateVulkanInstanceKHR), so the renderer's own
			// extension list is passed through unchanged. Returns the VkResult of the creation.
			VkResult (*create_instance)(const VkInstanceCreateInfo* info, VkInstance* out) = nullptr;

			// The VkPhysicalDevice the runtime requires (xrGetVulkanGraphicsDevice2KHR), or
			// VK_NULL_HANDLE to let the renderer pick as usual. A headset has one GPU, but the
			// spec still makes this the runtime's call.
			VkPhysicalDevice (*select_physical_device)(VkInstance instance) = nullptr;

			// vkCreateDevice stand-in (xrCreateVulkanDeviceKHR). Same contract as create_instance.
			VkResult (*create_device)(VkPhysicalDevice pdev, const VkDeviceCreateInfo* info, VkDevice* out) = nullptr;

			// The swapchain the renderer presents into. It owns the render_device, like every
			// other swapchain_base; the queue families are the ones the renderer selected.
			// transfer_queue may be umax when the GPU has no separate transfer family.
			swapchain_base* (*create_swapchain)(physical_device& gpu, u32 graphics_queue, u32 transfer_queue) = nullptr;
		};

		// The table in force, or nullptr for plain WSI presentation. Set before the renderer
		// thread starts and cleared after it has stopped; never swapped under a live renderer.
		inline const hooks* g_hooks = nullptr;

		inline void set_hooks(const hooks* table)
		{
			g_hooks = table;
		}

		inline const hooks* get_hooks()
		{
			return g_hooks;
		}

		inline bool active()
		{
			return g_hooks != nullptr;
		}
	}
}
