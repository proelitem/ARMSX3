// ARMSX3: OpenXR presentation. See xr_session.h for the shape of it.

#include "stdafx.h"
#include "xr_session.h"

#include "Emu/RSX/VK/VKHelpers.h"
#include "Emu/RSX/VK/vkutils/device.h"
#include "Emu/RSX/VK/vkutils/swapchain.h"
#include "Emu/RSX/VK/vkutils/xr_hooks.h"
#include "util/logs.hpp"

// The loader is dlopen()ed and every entry point fetched through xrGetInstanceProcAddr, so the
// headers must not declare prototypes that would otherwise have to link.
#define XR_NO_PROTOTYPES 1
#define XR_USE_PLATFORM_ANDROID 1
#define XR_USE_GRAPHICS_API_VULKAN 1
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <dlfcn.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

LOG_CHANNEL(xr_log, "XR");

namespace armsx3::xr
{
	namespace
	{
		// Entry points, resolved once. Instance-level ones are fetched again against the real
		// instance, since the loader answers XR_NULL_HANDLE only for the three pre-instance calls.
#define ARMSX3_XR_FUNCTIONS(X) \
	X(xrEnumerateInstanceExtensionProperties) \
	X(xrCreateInstance) \
	X(xrDestroyInstance) \
	X(xrGetInstanceProperties) \
	X(xrResultToString) \
	X(xrGetSystem) \
	X(xrGetSystemProperties) \
	X(xrEnumerateViewConfigurationViews) \
	X(xrCreateSession) \
	X(xrDestroySession) \
	X(xrBeginSession) \
	X(xrEndSession) \
	X(xrRequestExitSession) \
	X(xrCreateReferenceSpace) \
	X(xrDestroySpace) \
	X(xrEnumerateSwapchainFormats) \
	X(xrCreateSwapchain) \
	X(xrDestroySwapchain) \
	X(xrEnumerateSwapchainImages) \
	X(xrAcquireSwapchainImage) \
	X(xrWaitSwapchainImage) \
	X(xrReleaseSwapchainImage) \
	X(xrWaitFrame) \
	X(xrBeginFrame) \
	X(xrEndFrame) \
	X(xrPollEvent) \
	X(xrGetVulkanGraphicsRequirements2KHR) \
	X(xrCreateVulkanInstanceKHR) \
	X(xrGetVulkanGraphicsDevice2KHR) \
	X(xrCreateVulkanDeviceKHR)

		struct entry_points
		{
			PFN_xrGetInstanceProcAddr xrGetInstanceProcAddr = nullptr;
#define ARMSX3_XR_DECLARE(name) PFN_##name name = nullptr;
			ARMSX3_XR_FUNCTIONS(ARMSX3_XR_DECLARE)
#undef ARMSX3_XR_DECLARE

			bool resolve(XrInstance instance)
			{
				bool ok = true;
#define ARMSX3_XR_RESOLVE(name) \
				if (xrGetInstanceProcAddr(instance, #name, reinterpret_cast<PFN_xrVoidFunction*>(&name)) != XR_SUCCESS || !name) \
				{ \
					if (instance != XR_NULL_HANDLE) { xr_log.error("OpenXR entry point missing: %s", #name); ok = false; } \
				}
				ARMSX3_XR_FUNCTIONS(ARMSX3_XR_RESOLVE)
#undef ARMSX3_XR_RESOLVE
				return ok;
			}
		};

		// Everything that outlives one swapchain: loader, instance, system, the hooks, and the
		// session once the renderer's device exists. One of each per process; the renderer is
		// single-instance too.
		struct runtime_state
		{
			std::mutex mutex;

			void* loader_handle = nullptr;
			entry_points fn;

			JavaVM* vm = nullptr;
			jobject activity = nullptr; // global reference

			XrInstance instance = XR_NULL_HANDLE;
			XrSystemId system_id = XR_NULL_SYSTEM_ID;

			// The VkInstance the runtime created for the renderer (hook_create_instance). The
			// session binding needs it, and physical_device keeps its parent to itself.
			VkInstance vk_instance = VK_NULL_HANDLE;
			XrGraphicsRequirementsVulkan2KHR graphics_requirements{ XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR };

			screen_config screen;

			// The session belongs to the swapchain that created it (it needs the VkDevice), but
			// it is recorded here so stop() can tell whether the renderer has let go.
			std::atomic<bool> swapchain_alive{ false };
			std::atomic<bool> active{ false };

			vk::xr::hooks hooks{};
		};

		runtime_state g_state;

		const char* result_name(XrResult result)
		{
			static thread_local char buffer[XR_MAX_RESULT_STRING_SIZE];
			if (g_state.instance != XR_NULL_HANDLE && g_state.fn.xrResultToString &&
				g_state.fn.xrResultToString(g_state.instance, result, buffer) == XR_SUCCESS)
			{
				return buffer;
			}

			std::snprintf(buffer, sizeof(buffer), "XrResult(%d)", static_cast<int>(result));
			return buffer;
		}

		bool check(XrResult result, const char* what)
		{
			if (XR_SUCCEEDED(result))
			{
				return true;
			}

			xr_log.error("%s failed: %s", what, result_name(result));
			return false;
		}

		// ---- Vulkan creation, through the runtime (XR_KHR_vulkan_enable2) ------------------
		//
		// pfnGetInstanceProcAddr is the renderer's own dispatch entry: on Android that is the
		// pointer vk_android_loader resolved, i.e. the system driver or the adrenotools one. The
		// runtime creates its Vulkan objects through it, so the ICD that renders is the ICD that
		// composes. With XR_KHR_vulkan_enable (v1) the runtime would have loaded libvulkan.so on
		// its own and the two could disagree, which is the one way a custom Turnip could not be
		// made to work here at all.

		VkResult hook_create_instance(const VkInstanceCreateInfo* info, VkInstance* out)
		{
			XrVulkanInstanceCreateInfoKHR create_info{ XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR };
			create_info.systemId = g_state.system_id;
			create_info.createFlags = 0;
			create_info.pfnGetInstanceProcAddr = vkGetInstanceProcAddr;
			create_info.vulkanCreateInfo = info;
			create_info.vulkanAllocator = nullptr;

			VkResult vk_result = VK_ERROR_INITIALIZATION_FAILED;
			const XrResult xr_result = g_state.fn.xrCreateVulkanInstanceKHR(g_state.instance, &create_info, out, &vk_result);

			if (!check(xr_result, "xrCreateVulkanInstanceKHR"))
			{
				return vk_result != VK_SUCCESS ? vk_result : VK_ERROR_INITIALIZATION_FAILED;
			}

			if (vk_result != VK_SUCCESS)
			{
				xr_log.error("xrCreateVulkanInstanceKHR: the runtime's vkCreateInstance returned %d", static_cast<int>(vk_result));
			}
			else
			{
				g_state.vk_instance = *out;
			}

			return vk_result;
		}

		VkPhysicalDevice hook_select_physical_device(VkInstance instance)
		{
			XrVulkanGraphicsDeviceGetInfoKHR get_info{ XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR };
			get_info.systemId = g_state.system_id;
			get_info.vulkanInstance = instance;

			VkPhysicalDevice pdev = VK_NULL_HANDLE;
			if (!check(g_state.fn.xrGetVulkanGraphicsDevice2KHR(g_state.instance, &get_info, &pdev), "xrGetVulkanGraphicsDevice2KHR"))
			{
				return VK_NULL_HANDLE;
			}

			return pdev;
		}

		VkResult hook_create_device(VkPhysicalDevice pdev, const VkDeviceCreateInfo* info, VkDevice* out)
		{
			XrVulkanDeviceCreateInfoKHR create_info{ XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR };
			create_info.systemId = g_state.system_id;
			create_info.createFlags = 0;
			create_info.pfnGetInstanceProcAddr = vkGetInstanceProcAddr;
			create_info.vulkanPhysicalDevice = pdev;
			create_info.vulkanCreateInfo = info;
			create_info.vulkanAllocator = nullptr;

			VkResult vk_result = VK_ERROR_INITIALIZATION_FAILED;
			const XrResult xr_result = g_state.fn.xrCreateVulkanDeviceKHR(g_state.instance, &create_info, out, &vk_result);

			if (!check(xr_result, "xrCreateVulkanDeviceKHR"))
			{
				return vk_result != VK_SUCCESS ? vk_result : VK_ERROR_INITIALIZATION_FAILED;
			}

			if (vk_result != VK_SUCCESS)
			{
				xr_log.error("xrCreateVulkanDeviceKHR: the runtime's vkCreateDevice returned %d", static_cast<int>(vk_result));
			}

			return vk_result;
		}

		// ---- The swapchain -----------------------------------------------------------------

		struct swapchain_image_XR
		{
			VkImage value = VK_NULL_HANDLE;
		};

		class swapchain final : public vk::abstract_swapchain_impl<swapchain_image_XR>
		{
			XrSession m_session = XR_NULL_HANDLE;
			XrSpace m_space = XR_NULL_HANDLE;
			XrSwapchain m_swapchain = XR_NULL_HANDLE;

			// Session state machine, driven by xrPollEvent from acquire().
			XrSessionState m_session_state = XR_SESSION_STATE_UNKNOWN;
			bool m_session_running = false;
			bool m_session_lost = false;

			// Frame pairing. xrWaitFrame/xrBeginFrame run in acquire(), xrEndFrame in present().
			// A frame the renderer acquired and never presented (a dropped flip) is ended with no
			// layers by the next acquire, which is what the spec allows and what keeps the pairs
			// balanced.
			XrFrameState m_frame_state{ XR_TYPE_FRAME_STATE };
			bool m_frame_begun = false;
			bool m_image_acquired = false;
			u32 m_acquired_index = 0;

			// What the compositor is told the image holds. The XrSwapchain is created in the sRGB
			// variant so the compositor reads the bytes as display-encoded, which is what the
			// renderer writes (its output pass produces gamma-encoded colour, as for any monitor).
			// The renderer's own views of the images use the UNORM twin (m_surface_format), which
			// the MUTABLE_FORMAT usage bit permits, so nothing re-encodes on write. A runtime with
			// no sRGB format gets UNORM for both and a brighter picture, logged as such.
			int64_t m_xr_format = 0;

			bool m_logged_waiting_for_session = false;

			bool ensure_session()
			{
				if (m_session != XR_NULL_HANDLE)
				{
					return true;
				}

				XrGraphicsBindingVulkan2KHR binding{ XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR };
				binding.instance = g_state.vk_instance;
				binding.physicalDevice = dev.gpu();
				binding.device = dev;
				binding.queueFamilyIndex = dev.get_graphics_queue_family();
				binding.queueIndex = 0;

				XrSessionCreateInfo session_info{ XR_TYPE_SESSION_CREATE_INFO };
				session_info.next = &binding;
				session_info.createFlags = 0;
				session_info.systemId = g_state.system_id;

				if (!check(g_state.fn.xrCreateSession(g_state.instance, &session_info, &m_session), "xrCreateSession"))
				{
					m_session = XR_NULL_HANDLE;
					return false;
				}

				XrReferenceSpaceCreateInfo space_info{ XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
				space_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
				space_info.poseInReferenceSpace.orientation = { 0.f, 0.f, 0.f, 1.f };
				space_info.poseInReferenceSpace.position = { 0.f, 0.f, 0.f };

				if (!check(g_state.fn.xrCreateReferenceSpace(m_session, &space_info, &m_space), "xrCreateReferenceSpace"))
				{
					m_space = XR_NULL_HANDLE;
					return false;
				}

				xr_log.success("OpenXR session created on graphics queue family %u", binding.queueFamilyIndex);
				return true;
			}

			bool pick_formats()
			{
				u32 count = 0;
				if (!check(g_state.fn.xrEnumerateSwapchainFormats(m_session, 0, &count, nullptr), "xrEnumerateSwapchainFormats") || count == 0)
				{
					return false;
				}

				std::vector<int64_t> formats(count);
				if (!check(g_state.fn.xrEnumerateSwapchainFormats(m_session, count, &count, formats.data()), "xrEnumerateSwapchainFormats"))
				{
					return false;
				}

				const auto offered = [&](VkFormat f)
				{
					return std::find(formats.begin(), formats.end(), static_cast<int64_t>(f)) != formats.end();
				};

				struct candidate { VkFormat srgb; VkFormat unorm; };
				static constexpr candidate s_candidates[] =
				{
					{ VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_B8G8R8A8_UNORM },
					{ VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_R8G8B8A8_UNORM },
				};

				for (const auto& c : s_candidates)
				{
					if (offered(c.srgb))
					{
						m_xr_format = c.srgb;
						m_surface_format = c.unorm;
						return true;
					}
				}

				for (const auto& c : s_candidates)
				{
					if (offered(c.unorm))
					{
						xr_log.warning("The OpenXR runtime offers no sRGB swapchain format; the picture will be composed as linear and look brighter than it should.");
						m_xr_format = c.unorm;
						m_surface_format = c.unorm;
						return true;
					}
				}

				xr_log.error("The OpenXR runtime offers no 8-bit RGBA swapchain format the renderer can present to.");
				return false;
			}

			void destroy_xr_swapchain()
			{
				if (m_swapchain != XR_NULL_HANDLE)
				{
					// An image still acquired would make the destroy fail on some runtimes.
					if (m_image_acquired)
					{
						XrSwapchainImageReleaseInfo release_info{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
						g_state.fn.xrReleaseSwapchainImage(m_swapchain, &release_info);
						m_image_acquired = false;
					}

					g_state.fn.xrDestroySwapchain(m_swapchain);
					m_swapchain = XR_NULL_HANDLE;
				}

				swapchain_images.clear();
			}

			// xrPollEvent, and the session state transitions the renderer has to answer.
			void poll_events()
			{
				XrEventDataBuffer event{ XR_TYPE_EVENT_DATA_BUFFER };

				while (g_state.fn.xrPollEvent(g_state.instance, &event) == XR_SUCCESS)
				{
					switch (event.type)
					{
					case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED:
					{
						const auto& changed = *reinterpret_cast<const XrEventDataSessionStateChanged*>(&event);
						if (changed.session != m_session)
						{
							break;
						}

						m_session_state = changed.state;
						xr_log.notice("OpenXR session state %d", static_cast<int>(m_session_state));

						switch (m_session_state)
						{
						case XR_SESSION_STATE_READY:
						{
							XrSessionBeginInfo begin_info{ XR_TYPE_SESSION_BEGIN_INFO };
							begin_info.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
							if (check(g_state.fn.xrBeginSession(m_session, &begin_info), "xrBeginSession"))
							{
								m_session_running = true;
								xr_log.success("OpenXR session running");
							}
							break;
						}
						case XR_SESSION_STATE_STOPPING:
						{
							// Any frame in flight has to be ended before the session can.
							end_frame_without_layers();
							check(g_state.fn.xrEndSession(m_session), "xrEndSession");
							m_session_running = false;
							xr_log.notice("OpenXR session stopped (headset idle or app in background)");
							break;
						}
						case XR_SESSION_STATE_EXITING:
						case XR_SESSION_STATE_LOSS_PENDING:
						{
							m_session_running = false;
							m_session_lost = true;
							xr_log.warning("OpenXR session is going away (%s); presentation ends here.",
								m_session_state == XR_SESSION_STATE_EXITING ? "exit requested" : "loss pending");
							break;
						}
						default:
							break;
						}
						break;
					}
					case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
					{
						m_session_running = false;
						m_session_lost = true;
						xr_log.error("OpenXR instance loss pending; presentation ends here.");
						break;
					}
					default:
						break;
					}

					event = XrEventDataBuffer{ XR_TYPE_EVENT_DATA_BUFFER };
				}
			}

			void end_frame_without_layers()
			{
				if (!m_frame_begun)
				{
					return;
				}

				if (m_image_acquired)
				{
					XrSwapchainImageReleaseInfo release_info{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
					g_state.fn.xrReleaseSwapchainImage(m_swapchain, &release_info);
					m_image_acquired = false;
				}

				XrFrameEndInfo end_info{ XR_TYPE_FRAME_END_INFO };
				end_info.displayTime = m_frame_state.predictedDisplayTime;
				end_info.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
				end_info.layerCount = 0;
				end_info.layers = nullptr;
				g_state.fn.xrEndFrame(m_session, &end_info);
				m_frame_begun = false;
			}

			// An empty submission on the graphics queue, to signal or consume the renderer's
			// binary semaphores: the runtime's acquire/release carry no VkSemaphore, and the
			// renderer's frame contexts assume one signal per acquire and one wait per present.
			// Under the renderer's submit lock, because the RSX offloader submits on this queue
			// from its own thread.
			void queue_sync(VkSemaphore wait, VkSemaphore signal, VkFence fence)
			{
				const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;

				VkSubmitInfo submit{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
				submit.waitSemaphoreCount = wait != VK_NULL_HANDLE ? 1 : 0;
				submit.pWaitSemaphores = &wait;
				submit.pWaitDstStageMask = &wait_stage;
				submit.commandBufferCount = 0;
				submit.pCommandBuffers = nullptr;
				submit.signalSemaphoreCount = signal != VK_NULL_HANDLE ? 1 : 0;
				submit.pSignalSemaphores = &signal;

				vk::acquire_global_submit_lock();
				const VkResult result = vkQueueSubmit(dev.get_graphics_queue(), 1, &submit, fence);
				vk::release_global_submit_lock();

				if (result != VK_SUCCESS)
				{
					xr_log.error("Semaphore bridge submit returned %d", static_cast<int>(result));
				}
			}

		protected:
			void init_swapchain_images(vk::render_device& /*device*/, u32 /*preferred_count*/) override
			{
				u32 count = 0;
				if (!check(g_state.fn.xrEnumerateSwapchainImages(m_swapchain, 0, &count, nullptr), "xrEnumerateSwapchainImages") || count == 0)
				{
					fmt::throw_exception("OpenXR runtime returned 0 images for the swapchain");
				}

				std::vector<XrSwapchainImageVulkan2KHR> images(count, XrSwapchainImageVulkan2KHR{ XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR });
				if (!check(g_state.fn.xrEnumerateSwapchainImages(m_swapchain, count, &count,
					reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data())), "xrEnumerateSwapchainImages"))
				{
					fmt::throw_exception("OpenXR runtime refused to hand out its swapchain images");
				}

				swapchain_images.resize(count);
				for (u32 i = 0; i < count; ++i)
				{
					swapchain_images[i].value = images[i].image;
				}
			}

		public:
			swapchain(vk::physical_device& gpu, u32 graphics_queue, u32 transfer_queue)
				// The graphics queue is also the "present" queue: the session is bound to it and
				// that is where the layer is handed over. Not headless, so the renderer takes its
				// normal present path (acquire, blit, present) rather than the DMA readback one.
				: abstract_swapchain_impl(gpu, graphics_queue, graphics_queue, transfer_queue, VK_FORMAT_B8G8R8A8_UNORM)
			{
				g_state.swapchain_alive = true;
			}

			~swapchain() override
			{
				g_state.swapchain_alive = false;
			}

			void create(display_handle_t& /*handle*/) override
			{}

			bool init() override
			{
				// Called with m_width / m_height set by swapchain_base::init(w, h); recreating
				// the XrSwapchain is the resize path, the session persists.
				if (!ensure_session())
				{
					return false;
				}

				if (!m_xr_format && !pick_formats())
				{
					return false;
				}

				destroy_xr_swapchain();

				XrSwapchainCreateInfo swapchain_info{ XR_TYPE_SWAPCHAIN_CREATE_INFO };
				swapchain_info.createFlags = 0;
				swapchain_info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT |
					XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_MUTABLE_FORMAT_BIT;
				swapchain_info.format = m_xr_format;
				swapchain_info.sampleCount = 1;
				swapchain_info.width = m_width;
				swapchain_info.height = m_height;
				swapchain_info.faceCount = 1;
				swapchain_info.arraySize = 1;
				swapchain_info.mipCount = 1;

				if (!check(g_state.fn.xrCreateSwapchain(m_session, &swapchain_info, &m_swapchain), "xrCreateSwapchain"))
				{
					m_swapchain = XR_NULL_HANDLE;
					return false;
				}

				init_swapchain_images(dev, 0);
				m_frame_begun = false;
				m_image_acquired = false;

				xr_log.success("OpenXR swapchain %ux%u, %u images, format %lld (renderer views as %d)",
					m_width, m_height, ::size32(swapchain_images), static_cast<long long>(m_xr_format), static_cast<int>(m_surface_format));
				return true;
			}

			void destroy(bool /*full*/ = true) override
			{
				end_frame_without_layers();
				destroy_xr_swapchain();

				if (m_space != XR_NULL_HANDLE)
				{
					g_state.fn.xrDestroySpace(m_space);
					m_space = XR_NULL_HANDLE;
				}

				if (m_session != XR_NULL_HANDLE)
				{
					if (m_session_running)
					{
						g_state.fn.xrEndSession(m_session);
						m_session_running = false;
					}

					g_state.fn.xrDestroySession(m_session);
					m_session = XR_NULL_HANDLE;
				}

				if (VkDevice pdev = dev)
				{
					dev.destroy();
				}
			}

			VkResult acquire_next_swapchain_image(VkSemaphore semaphore, u64 /*timeout*/, u32* result, VkFence fence) override
			{
				poll_events();

				if (m_session_lost)
				{
					// Nothing to present to any more. The renderer treats this as a swapchain it
					// cannot rebuild and drops frames until the emulator stops, which the app does
					// from its side.
					std::this_thread::sleep_for(std::chrono::milliseconds(50));
					return VK_ERROR_SURFACE_LOST_KHR;
				}

				if (!m_session_running)
				{
					// Headset off, app behind the system UI, or the runtime not ready yet. The
					// renderer spins on TIMEOUT, so pace it here and say so once.
					if (!m_logged_waiting_for_session)
					{
						xr_log.notice("Waiting for the OpenXR session to run before presenting.");
						m_logged_waiting_for_session = true;
					}

					std::this_thread::sleep_for(std::chrono::milliseconds(10));
					return VK_TIMEOUT;
				}
				m_logged_waiting_for_session = false;

				// A frame the renderer began and never presented.
				end_frame_without_layers();

				XrFrameWaitInfo wait_info{ XR_TYPE_FRAME_WAIT_INFO };
				m_frame_state = XrFrameState{ XR_TYPE_FRAME_STATE };
				if (!check(g_state.fn.xrWaitFrame(m_session, &wait_info, &m_frame_state), "xrWaitFrame"))
				{
					return VK_TIMEOUT;
				}

				XrFrameBeginInfo begin_info{ XR_TYPE_FRAME_BEGIN_INFO };
				if (!check(g_state.fn.xrBeginFrame(m_session, &begin_info), "xrBeginFrame"))
				{
					return VK_TIMEOUT;
				}
				m_frame_begun = true;

				XrSwapchainImageAcquireInfo acquire_info{ XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
				u32 index = 0;
				if (!check(g_state.fn.xrAcquireSwapchainImage(m_swapchain, &acquire_info, &index), "xrAcquireSwapchainImage"))
				{
					return VK_TIMEOUT;
				}
				m_image_acquired = true;
				m_acquired_index = index;

				XrSwapchainImageWaitInfo image_wait{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
				image_wait.timeout = 1'000'000'000; // 1 s; the compositor releases images at display rate
				if (!check(g_state.fn.xrWaitSwapchainImage(m_swapchain, &image_wait), "xrWaitSwapchainImage"))
				{
					return VK_TIMEOUT;
				}

				// The runtime guarantees the image is ready; tell the renderer's frame context so
				// through the semaphore (and fence) it handed us, as vkAcquireNextImageKHR would.
				if (semaphore != VK_NULL_HANDLE || fence != VK_NULL_HANDLE)
				{
					queue_sync(VK_NULL_HANDLE, semaphore, fence);
				}

				*result = index;
				return VK_SUCCESS;
			}

			void end_frame(vk::command_buffer& /*cmd*/, u32 /*index*/) override
			{}

			VkResult present(VkSemaphore semaphore, u32 index) override
			{
				// Consume the renderer's "rendering done" semaphore. Queue order then puts the
				// compositor's own synchronization, which the runtime submits on this same queue at
				// release, after our blit.
				if (semaphore != VK_NULL_HANDLE)
				{
					queue_sync(semaphore, VK_NULL_HANDLE, VK_NULL_HANDLE);
				}

				if (!m_frame_begun || !m_session_running)
				{
					// Presenting a frame whose xrBeginFrame never happened (the session stopped in
					// between). Nothing more to do; the semaphore is already consumed.
					return VK_SUCCESS;
				}

				if (m_image_acquired)
				{
					if (index != m_acquired_index)
					{
						xr_log.error("Present of image %u while image %u is the one acquired", index, m_acquired_index);
					}

					XrSwapchainImageReleaseInfo release_info{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
					check(g_state.fn.xrReleaseSwapchainImage(m_swapchain, &release_info), "xrReleaseSwapchainImage");
					m_image_acquired = false;
				}

				// One quad, both eyes, straight ahead at the configured distance. The LOCAL space
				// is gravity-aligned and re-centred by the system's recentre gesture, so the
				// screen behaves like a cinema screen rather than following the head.
				const float width_m = g_state.screen.width_m;
				const float height_m = width_m * static_cast<float>(m_height) / static_cast<float>(std::max<u32>(m_width, 1));

				XrCompositionLayerQuad quad{ XR_TYPE_COMPOSITION_LAYER_QUAD };
				quad.layerFlags = 0;
				quad.space = m_space;
				quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
				quad.subImage.swapchain = m_swapchain;
				quad.subImage.imageRect.offset = { 0, 0 };
				quad.subImage.imageRect.extent = { static_cast<int32_t>(m_width), static_cast<int32_t>(m_height) };
				quad.subImage.imageArrayIndex = 0;
				quad.pose.orientation = { 0.f, 0.f, 0.f, 1.f };
				quad.pose.position = { 0.f, 0.f, -g_state.screen.distance_m };
				quad.size = { width_m, height_m };

				const XrCompositionLayerBaseHeader* layers[] = { reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad) };

				XrFrameEndInfo end_info{ XR_TYPE_FRAME_END_INFO };
				end_info.displayTime = m_frame_state.predictedDisplayTime;
				end_info.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
				end_info.layerCount = 1;
				end_info.layers = layers;

				const XrResult result = g_state.fn.xrEndFrame(m_session, &end_info);
				m_frame_begun = false;

				if (result == XR_ERROR_SESSION_NOT_RUNNING)
				{
					// Raced a STOPPING we have not polled yet; harmless, the next acquire sees it.
					return VK_SUCCESS;
				}

				check(result, "xrEndFrame");
				return VK_SUCCESS;
			}

			VkImage get_image(u32 index) override
			{
				return swapchain_images[index].value;
			}

			// What the runtime hands the image over in, and wants it back in.
			VkImageLayout get_optimal_present_layout() const override
			{
				return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
			}
		};

		vk::swapchain_base* hook_create_swapchain(vk::physical_device& gpu, u32 graphics_queue, u32 transfer_queue)
		{
			return new swapchain(gpu, graphics_queue, transfer_queue);
		}

		// ---- Instance bring-up ---------------------------------------------------------------

		bool load_loader()
		{
			if (g_state.loader_handle)
			{
				return true;
			}

			::dlerror();
			g_state.loader_handle = ::dlopen("libopenxr_loader.so", RTLD_NOW | RTLD_LOCAL);
			if (!g_state.loader_handle)
			{
				const char* error = ::dlerror();
				xr_log.error("libopenxr_loader.so could not be loaded (%s). Is the OpenXR loader AAR in this build?", error ? error : "no reason given");
				return false;
			}

			g_state.fn.xrGetInstanceProcAddr = reinterpret_cast<PFN_xrGetInstanceProcAddr>(::dlsym(g_state.loader_handle, "xrGetInstanceProcAddr"));
			if (!g_state.fn.xrGetInstanceProcAddr)
			{
				xr_log.error("libopenxr_loader.so has no xrGetInstanceProcAddr");
				return false;
			}

			return true;
		}

		bool initialize_loader(JavaVM* vm, jobject activity)
		{
			PFN_xrInitializeLoaderKHR initialize_loader = nullptr;
			g_state.fn.xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR", reinterpret_cast<PFN_xrVoidFunction*>(&initialize_loader));
			if (!initialize_loader)
			{
				xr_log.error("xrInitializeLoaderKHR is missing: this loader cannot run on Android");
				return false;
			}

			XrLoaderInitInfoAndroidKHR init_info{ XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR };
			init_info.applicationVM = vm;
			init_info.applicationContext = activity;
			return check(initialize_loader(reinterpret_cast<const XrLoaderInitInfoBaseHeaderKHR*>(&init_info)), "xrInitializeLoaderKHR");
		}

		bool extension_available(const std::vector<XrExtensionProperties>& list, const char* name)
		{
			return std::any_of(list.begin(), list.end(), [name](const XrExtensionProperties& p)
			{
				return std::strcmp(p.extensionName, name) == 0;
			});
		}

		bool create_instance(JavaVM* vm, jobject activity)
		{
			// Pre-instance entry points.
			g_state.fn.resolve(XR_NULL_HANDLE);
			if (!g_state.fn.xrEnumerateInstanceExtensionProperties || !g_state.fn.xrCreateInstance)
			{
				xr_log.error("The OpenXR loader exposes no pre-instance entry points");
				return false;
			}

			u32 count = 0;
			if (!check(g_state.fn.xrEnumerateInstanceExtensionProperties(nullptr, 0, &count, nullptr), "xrEnumerateInstanceExtensionProperties"))
			{
				return false;
			}

			std::vector<XrExtensionProperties> available(count, XrExtensionProperties{ XR_TYPE_EXTENSION_PROPERTIES });
			if (!check(g_state.fn.xrEnumerateInstanceExtensionProperties(nullptr, count, &count, available.data()), "xrEnumerateInstanceExtensionProperties"))
			{
				return false;
			}

			for (const auto& ext : available)
			{
				xr_log.notice("OpenXR runtime extension: %s (v%u)", ext.extensionName, ext.extensionVersion);
			}

			static constexpr const char* s_required[] =
			{
				XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME,
				XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME,
			};

			std::vector<const char*> extensions;
			for (const char* name : s_required)
			{
				if (!extension_available(available, name))
				{
					xr_log.error("The OpenXR runtime lacks %s, which this presentation path needs", name);
					return false;
				}

				extensions.push_back(name);
			}

			XrInstanceCreateInfoAndroidKHR android_info{ XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR };
			android_info.applicationVM = vm;
			android_info.applicationActivity = activity;

			XrInstanceCreateInfo instance_info{ XR_TYPE_INSTANCE_CREATE_INFO };
			instance_info.next = &android_info;
			instance_info.createFlags = 0;
			std::strncpy(instance_info.applicationInfo.applicationName, "ARMSX3", XR_MAX_APPLICATION_NAME_SIZE - 1);
			instance_info.applicationInfo.applicationVersion = 1;
			std::strncpy(instance_info.applicationInfo.engineName, "RPCS3", XR_MAX_ENGINE_NAME_SIZE - 1);
			instance_info.applicationInfo.engineVersion = 1;
			// 1.0: every shipping headset runtime answers it, and nothing here needs 1.1.
			instance_info.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
			instance_info.enabledApiLayerCount = 0;
			instance_info.enabledApiLayerNames = nullptr;
			instance_info.enabledExtensionCount = ::size32(extensions);
			instance_info.enabledExtensionNames = extensions.data();

			if (!check(g_state.fn.xrCreateInstance(&instance_info, &g_state.instance), "xrCreateInstance"))
			{
				g_state.instance = XR_NULL_HANDLE;
				return false;
			}

			if (!g_state.fn.resolve(g_state.instance))
			{
				return false;
			}

			XrInstanceProperties properties{ XR_TYPE_INSTANCE_PROPERTIES };
			if (check(g_state.fn.xrGetInstanceProperties(g_state.instance, &properties), "xrGetInstanceProperties"))
			{
				xr_log.success("OpenXR runtime: %s %u.%u.%u", properties.runtimeName,
					XR_VERSION_MAJOR(properties.runtimeVersion), XR_VERSION_MINOR(properties.runtimeVersion), XR_VERSION_PATCH(properties.runtimeVersion));
			}

			XrSystemGetInfo system_info{ XR_TYPE_SYSTEM_GET_INFO };
			system_info.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
			if (!check(g_state.fn.xrGetSystem(g_state.instance, &system_info, &g_state.system_id), "xrGetSystem"))
			{
				return false;
			}

			XrSystemProperties system_properties{ XR_TYPE_SYSTEM_PROPERTIES };
			if (check(g_state.fn.xrGetSystemProperties(g_state.instance, g_state.system_id, &system_properties), "xrGetSystemProperties"))
			{
				xr_log.success("OpenXR system: %s (vendor %u), max layers %u, max swapchain %ux%u",
					system_properties.systemName, system_properties.vendorId,
					system_properties.graphicsProperties.maxLayerCount,
					system_properties.graphicsProperties.maxSwapchainImageWidth,
					system_properties.graphicsProperties.maxSwapchainImageHeight);

				// A quad larger than the runtime allows is refused at xrCreateSwapchain; clamp
				// now, keeping 16:9.
				const u32 max_w = system_properties.graphicsProperties.maxSwapchainImageWidth;
				const u32 max_h = system_properties.graphicsProperties.maxSwapchainImageHeight;
				if (max_w && g_state.screen.width_px > max_w)
				{
					g_state.screen.width_px = max_w;
					g_state.screen.height_px = max_w * 9 / 16;
				}
				if (max_h && g_state.screen.height_px > max_h)
				{
					g_state.screen.height_px = max_h;
					g_state.screen.width_px = max_h * 16 / 9;
				}
			}

			u32 view_count = 0;
			if (check(g_state.fn.xrEnumerateViewConfigurationViews(g_state.instance, g_state.system_id, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &view_count, nullptr), "xrEnumerateViewConfigurationViews") && view_count)
			{
				std::vector<XrViewConfigurationView> views(view_count, XrViewConfigurationView{ XR_TYPE_VIEW_CONFIGURATION_VIEW });
				if (check(g_state.fn.xrEnumerateViewConfigurationViews(g_state.instance, g_state.system_id, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, view_count, &view_count, views.data()), "xrEnumerateViewConfigurationViews"))
				{
					xr_log.notice("OpenXR recommended per-eye image: %ux%u (max %ux%u)",
						views[0].recommendedImageRectWidth, views[0].recommendedImageRectHeight,
						views[0].maxImageRectWidth, views[0].maxImageRectHeight);
				}
			}

			// Required before xrCreateSession under XR_KHR_vulkan_enable2, and it also says which
			// Vulkan versions the runtime will accept.
			if (!check(g_state.fn.xrGetVulkanGraphicsRequirements2KHR(g_state.instance, g_state.system_id, &g_state.graphics_requirements), "xrGetVulkanGraphicsRequirements2KHR"))
			{
				return false;
			}

			xr_log.notice("OpenXR runtime accepts Vulkan %u.%u to %u.%u",
				XR_VERSION_MAJOR(g_state.graphics_requirements.minApiVersionSupported), XR_VERSION_MINOR(g_state.graphics_requirements.minApiVersionSupported),
				XR_VERSION_MAJOR(g_state.graphics_requirements.maxApiVersionSupported), XR_VERSION_MINOR(g_state.graphics_requirements.maxApiVersionSupported));

			return true;
		}

		// JavaVM::AttachCurrentThread takes JNIEnv** on Android and void** in the JDK's jni.h;
		// deducing the parameter keeps the call correct on both (the JDK one only matters for a
		// host syntax check).
		template <typename Env>
		jint attach_current_thread(JavaVM* vm, JNIEnv** env, jint (JavaVM::*attach)(Env**, void*))
		{
			return (vm->*attach)(reinterpret_cast<Env**>(env), nullptr);
		}

		void destroy_instance_locked()
		{
			if (g_state.instance != XR_NULL_HANDLE)
			{
				g_state.fn.xrDestroyInstance(g_state.instance);
				g_state.instance = XR_NULL_HANDLE;
			}
			g_state.system_id = XR_NULL_SYSTEM_ID;

			if (g_state.activity && g_state.vm)
			{
				JNIEnv* env = nullptr;
				bool attached = false;
				if (g_state.vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_EDETACHED)
				{
					attached = attach_current_thread(g_state.vm, &env, &JavaVM::AttachCurrentThread) == JNI_OK;
				}
				if (env)
				{
					env->DeleteGlobalRef(g_state.activity);
				}
				if (attached)
				{
					g_state.vm->DetachCurrentThread();
				}
			}
			g_state.activity = nullptr;
			g_state.vm = nullptr;

			// The loader stays loaded: dlclose of a library that registered thread-locals and
			// a broker connection is not worth the risk for the few KB it would free.
		}
	}

	bool start(JavaVM* vm, jobject activity, const screen_config& config)
	{
		std::lock_guard lock(g_state.mutex);

		if (g_state.active)
		{
			xr_log.warning("OpenXR presentation already started");
			return true;
		}

		if (!vm || !activity)
		{
			xr_log.error("start() needs the JavaVM and an activity");
			return false;
		}

		g_state.screen = config;
		g_state.vm = vm;
		g_state.activity = activity;

		if (!load_loader() || !initialize_loader(vm, activity) || !create_instance(vm, activity))
		{
			destroy_instance_locked();
			return false;
		}

		g_state.hooks.create_instance = hook_create_instance;
		g_state.hooks.select_physical_device = hook_select_physical_device;
		g_state.hooks.create_device = hook_create_device;
		g_state.hooks.create_swapchain = hook_create_swapchain;
		vk::xr::set_hooks(&g_state.hooks);

		g_state.active = true;
		xr_log.success("OpenXR presentation installed: %ux%u quad, %.2f m wide at %.2f m",
			g_state.screen.width_px, g_state.screen.height_px, g_state.screen.width_m, g_state.screen.distance_m);
		return true;
	}

	void stop()
	{
		std::lock_guard lock(g_state.mutex);

		if (!g_state.active)
		{
			return;
		}

		// The renderer's swapchain holds the session. It is destroyed with the renderer when
		// the emulator stops; the app calls this after that, but the join is asynchronous, so
		// give it a moment rather than destroy the instance under a live session.
		for (u32 waited_ms = 0; g_state.swapchain_alive && waited_ms < 5000; waited_ms += 20)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(20));
		}

		if (g_state.swapchain_alive)
		{
			xr_log.error("stop() called while the renderer still presents to OpenXR; the instance is kept alive and leaked rather than pulled from under it.");
			vk::xr::set_hooks(nullptr);
			g_state.active = false;
			return;
		}

		vk::xr::set_hooks(nullptr);
		g_state.active = false;
		destroy_instance_locked();
		xr_log.notice("OpenXR presentation stopped");
	}

	bool active()
	{
		return g_state.active.load();
	}

	void recommended_extent(u32& width, u32& height)
	{
		width = g_state.screen.width_px;
		height = g_state.screen.height_px;
	}
}
