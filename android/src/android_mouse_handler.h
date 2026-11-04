#pragma once

// ARMSX3: a mouse the app drives.
//
// The PS3's lightguns are emulated as USB devices that take their aim from the mouse handler
// (GunCon 3: Emu/Io/GunCon3.cpp reads MouseHandlerBase::GetMice()[index].x_pos/y_pos). Android
// has no mouse handler in this build -- the Qt basic one is desktop code and the null one reports
// no mice -- so a lightgun could never be aimed here. This handler reports MAX_MICE connected
// mice that never move on their own; the app moves them through _armsx3_mouseAim: a touch on
// the panel, or the Touch controller's aim ray hitting the virtual screen in the headset, or
// player 2's aim arriving over remote play. One mouse per player, so the mouse setting is
// forced to "raw" (index = controller) when this handler is installed; see init_mouse_handler.

#include "Emu/Io/MouseHandler.h"

class android_mouse_handler final : public MouseHandlerBase
{
	using MouseHandlerBase::MouseHandlerBase;

public:
	void Init(const u32 max_connect) override
	{
		std::lock_guard lock(mutex);

		m_info = {};
		m_info.max_connect = std::min<u32>(max_connect, MAX_MICE);
		m_info.now_connect = m_info.max_connect;
		m_info.info = 0; // not intercepted
		for (u32 i = 0; i < m_info.max_connect; i++)
		{
			m_info.status[i] = CELL_MOUSE_STATUS_CONNECTED;
			m_info.vendor_id[i] = 0x0B9A; // reads as a Namco device next to the GunCon it aims
			m_info.product_id[i] = 0x0001 + i;
		}
		m_info.is_null_handler = false;

		m_mice.clear();
		m_mice.resize(m_info.max_connect);
		for (auto& mouse : m_mice)
		{
			// Centre, so a gun that was never aimed points at the middle rather than a corner.
			mouse.x_pos = 0.5f;
			mouse.y_pos = 0.5f;
			mouse.x_pos_pixels = k_range / 2;
			mouse.y_pos_pixels = k_range / 2;
		}
	}

	// Normalised aim, 0..1, origin top-left (what GunCon3 and the touch layers both speak).
	void aim(u32 index, f32 x, f32 y)
	{
		const s32 px = static_cast<s32>(std::clamp(x, 0.f, 1.f) * k_range);
		const s32 py = static_cast<s32>(std::clamp(y, 0.f, 1.f) * k_range);
		Move(index, px, py, k_range, k_range);
	}

	static constexpr s32 k_range = 4096;
};
