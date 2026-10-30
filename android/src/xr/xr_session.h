#pragma once

// ARMSX3: presentation through OpenXR, for standalone headsets (Meta Quest first).
//
// What this is: the PS3's video output shown on a virtual screen in front of the player,
// through the headset's compositor, instead of on an Android Surface. One quad layer visible
// to both eyes; the stereoscopic variant (one image per eye, from games with native 3D output)
// builds on it later.
//
// How it fits the renderer: the core never sees OpenXR. It consults the vk::xr::hooks table
// (rpcs3/Emu/RSX/VK/vkutils/xr_hooks.h) at instance, physical device, device and swapchain
// creation, and this file is what fills that table. The swapchain it supplies is an ordinary
// vk::swapchain_base: acquire is xrWaitFrame / xrBeginFrame / xrAcquireSwapchainImage,
// present is xrReleaseSwapchainImage / xrEndFrame. The RSX thread drives all of it, as it
// drives a VkSwapchainKHR.
//
// Lifecycle, from the app: start() before the game boots, on the activity that is immersive
// (Horizon OS only composes OpenXR layers for an activity carrying the VR intent category);
// stop() after the emulator has stopped. The loader (libopenxr_loader.so, from the Khronos
// AAR) is loaded at run time, so a build of the core without the AAR beside it simply reports
// that OpenXR is unavailable.

#include "util/types.hpp"

#include <jni.h>

namespace armsx3::xr
{
	// Where the screen hangs. Metres, in the LOCAL reference space (origin at the headset's
	// position when the session started, gravity-aligned). 16:9 is the PS3's aspect; the height
	// follows the width.
	struct screen_config
	{
		float distance_m = 2.0f;
		float width_m    = 2.4f;
		u32 width_px     = 1920;
		u32 height_px    = 1080;
	};

	// Bring up the loader, the XrInstance and the system, and install the renderer hooks.
	// `activity` must stay valid until stop(): a global reference is taken here.
	// Returns false, with the reason in the log, when OpenXR cannot be used; nothing is then
	// installed and the renderer presents to the Android Surface as usual.
	bool start(JavaVM* vm, jobject activity, const screen_config& config = {});

	// Uninstall the hooks and tear down the XrInstance. Waits (bounded) for the renderer's
	// swapchain to be gone first: the session it holds belongs to this instance.
	void stop();

	// True between a successful start() and stop(): the renderer is presenting to OpenXR.
	bool active();

	// The extent the quad swapchain is created at. What GSFrameBase::client_width/height
	// report while active(), so the present framebuffer and the layer agree.
	void recommended_extent(u32& width, u32& height);
}
