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
		// Display refresh rate to ask the runtime for, when it offers one that close. The PS3
		// outputs 59.94 Hz (50 for PAL): 60 shows each game frame once, or twice at 30 fps,
		// with none of the 3:2 cadence a 72 or 90 Hz display adds. 0 keeps the runtime's default.
		float refresh_hz = 60.f;
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

	// Lightgun mode: the right controller's aim ray, where it meets the virtual screen, drives
	// the app's mouse 0 (the GunCon 3 of pad 1 reads it), and the right trigger becomes Cross,
	// the GunCon's trigger. On or off at any time.
	void set_lightgun(bool on);
	bool lightgun();

	// A trigger the controllers do not know about: a WonderFitter's shot frames (held for a
	// moment after each frame by the app, since the gun sends no release) or a Bluetooth gun's
	// button. Fires with the gun hand, and takes part in its calibration gesture.
	void set_gun_trigger(bool down);
	// Which controller is the gun: -1 follows the last trigger pulled (default), 0 left, 1 right.
	// Fixed when the controller rides in a physical gun, whose own trigger is the one pulled.
	void set_gun_hand(int hand);

	// The extent the quad swapchain is created at. What GSFrameBase::client_width/height
	// report while active(), so the present framebuffer and the layer agree.
	//
	// stereo_output: the game is outputting one image per eye (rsx::avconf::stereo_enabled).
	// The swapchain is then twice as wide, the renderer composes the eyes side by side at full
	// size (stereo_render_mode_options::side_by_side_full, set by the swapchain), and present()
	// hands each half to its eye as its own quad layer.
	void recommended_extent(u32& width, u32& height, bool stereo_output);
}
