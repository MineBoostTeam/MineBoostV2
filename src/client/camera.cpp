// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2010-2013 celeron55, Perttu Ahola <celeron55@gmail.com>

#include "camera.h"
#include "debug.h"
#include "client.h"
#include "config.h"
#include "map.h"
#include "clientmap.h"     // MapDrawControl
#include "player.h"
#include <cmath>
#include <unordered_map>
#include <algorithm>
#include "client/renderingengine.h"
#include "client/content_cao.h"
#include "client/clientenvironment.h"
#include "client/friendlist.h"
#include "client/mineboost_presence.h"
#include "client/texturesource.h"
#include "porting.h"
#include "settings.h"
#include "wieldmesh.h"
#include "noise.h"         // easeCurve
#include "mtevent.h"
#include "nodedef.h"
#include "util/numeric.h"
#include "constants.h"
#include "fontengine.h"
#include "script/scripting_client.h"
#include "gettext.h"
#include <SViewFrustum.h>
#include <IGUIFont.h>
#include <IVideoDriver.h>

#define CAMERA_OFFSET_STEP 200
#define WIELDMESH_OFFSET_X 55.0f
#define WIELDMESH_OFFSET_Y -35.0f
#define WIELDMESH_AMPLITUDE_X 7.0f
#define WIELDMESH_AMPLITUDE_Y 10.0f

Camera::Camera(MapDrawControl &draw_control, Client *client, RenderingEngine *rendering_engine):
	m_draw_control(draw_control),
	m_client(client),
	m_player_light_color(0xFFFFFFFF)
{
	auto smgr = rendering_engine->get_scene_manager();
	// note: making the camera node a child of the player node
	// would lead to unexpected behavior, so we don't do that.
	m_playernode = smgr->addEmptySceneNode(smgr->getRootSceneNode());
	m_headnode = smgr->addEmptySceneNode(m_playernode);
	m_cameranode = smgr->addCameraSceneNode(smgr->getRootSceneNode());
	m_cameranode->bindTargetAndRotation(true);

	// This needs to be in its own scene manager. It is drawn after
	// all other 3D scene nodes and before the GUI.
	m_wieldmgr = smgr->createNewSceneManager();
	m_wieldmgr->addCameraSceneNode();
	m_wieldnode = new WieldMeshSceneNode(m_wieldmgr, -1);
	m_wieldnode->setItem(ItemStack(), m_client);
	m_wieldnode->drop(); // m_wieldmgr grabbed it

	/* TODO: Add a callback function so these can be updated when a setting
	 *       changes.  At this point in time it doesn't matter (e.g. /set
	 *       is documented to change server settings only)
	 *
	 * TODO: Local caching of settings is not optimal and should at some stage
	 *       be updated to use a global settings object for getting thse values
	 *       (as opposed to the this local caching). This can be addressed in
	 *       a later release.
	 */
	m_cache_fall_bobbing_amount = g_settings->getFloat("fall_bobbing_amount", 0.0f, 100.0f);
	m_cache_view_bobbing_amount = g_settings->getFloat("view_bobbing_amount", 0.0f, 7.9f);
	// 45 degrees is the lowest FOV that doesn't cause the server to treat this
	// as a zoom FOV and load world beyond the set server limits.
	m_cache_fov                 = g_settings->getFloat("fov", 45.0f, 160.0f);
	m_arm_inertia               = g_settings->getBool("arm_inertia");
	m_nametags.clear();
	m_show_nametag_backgrounds  = g_settings->getBool("show_nametag_backgrounds");
}

Camera::~Camera()
{
	m_wieldmgr->drop();
}

void Camera::notifyFovChange()
{
	LocalPlayer *player = m_client->getEnv().getLocalPlayer();
	assert(player);

	PlayerFovSpec spec = player->getFov();

	// Remember old FOV in case a transition is wanted
	f32 m_old_fov_degrees = m_fov_transition_active
		? m_curr_fov_degrees // FOV is overridden with transition
		: m_server_sent_fov
			? m_target_fov_degrees // FOV is overridden without transition
			: m_cache_fov; // FOV is not overridden

	m_server_sent_fov = spec.fov > 0.0f;
	m_target_fov_degrees = m_server_sent_fov
		? spec.is_multiplier
			? m_cache_fov * spec.fov // apply multiplier to client-set FOV
			: spec.fov // absolute override
		: m_cache_fov; // reset to client-set FOV

	m_fov_transition_active = spec.transition_time > 0.0f;
	if (m_fov_transition_active) {
		m_transition_time = spec.transition_time;
		m_fov_diff = m_target_fov_degrees - m_old_fov_degrees;
	}
}

// Returns the fractional part of x
inline f32 my_modf(f32 x)
{
	float dummy;
	return std::modf(x, &dummy);
}

void Camera::step(f32 dtime)
{
	if(m_view_bobbing_fall > 0)
	{
		m_view_bobbing_fall -= 3 * dtime;
		if(m_view_bobbing_fall <= 0)
			m_view_bobbing_fall = -1; // Mark the effect as finished
	}

	bool was_under_zero = m_wield_change_timer < 0;
	m_wield_change_timer = MYMIN(m_wield_change_timer + dtime, 0.125);

	if (m_wield_change_timer >= 0 && was_under_zero) {
		m_wieldnode->setItem(m_wield_item_next, m_client);
		m_wieldnode->setNodeLightColor(m_player_light_color);
	}

	if (m_view_bobbing_state != 0)
	{
		//f32 offset = dtime * m_view_bobbing_speed * 0.035;
		f32 offset = dtime * m_view_bobbing_speed * 0.030;
		if (m_view_bobbing_state == 2) {
			// Animation is getting turned off
			if (m_view_bobbing_anim < 0.25) {
				m_view_bobbing_anim -= offset;
			} else if (m_view_bobbing_anim > 0.75) {
				m_view_bobbing_anim += offset;
			} else if (m_view_bobbing_anim < 0.5) {
				m_view_bobbing_anim += offset;
				if (m_view_bobbing_anim > 0.5)
					m_view_bobbing_anim = 0.5;
			} else {
				m_view_bobbing_anim -= offset;
				if (m_view_bobbing_anim < 0.5)
					m_view_bobbing_anim = 0.5;
			}

			if (m_view_bobbing_anim <= 0 || m_view_bobbing_anim >= 1 ||
					fabs(m_view_bobbing_anim - 0.5) < 0.01) {
				m_view_bobbing_anim = 0;
				m_view_bobbing_state = 0;
			}
		}
		else {
			float was = m_view_bobbing_anim;
			m_view_bobbing_anim = my_modf(m_view_bobbing_anim + offset);
			bool step = (was == 0 ||
					(was < 0.5f && m_view_bobbing_anim >= 0.5f) ||
					(was > 0.5f && m_view_bobbing_anim <= 0.5f));
			if(step) {
				m_client->getEventManager()->put(new SimpleTriggerEvent(MtEvent::VIEW_BOBBING_STEP));
			}
		}
	}

	if (m_digging_button != -1) {
		f32 offset = dtime * 3.5f;
		float m_digging_anim_was = m_digging_anim;
		m_digging_anim += offset;
		if (m_digging_anim >= 1)
		{
			m_digging_anim = 0;
			m_digging_button = -1;
		}
		float lim = 0.15;
		if(m_digging_anim_was < lim && m_digging_anim >= lim)
		{
			if (m_digging_button == 0) {
				m_client->getEventManager()->put(new SimpleTriggerEvent(MtEvent::CAMERA_PUNCH_LEFT));
			} else if(m_digging_button == 1) {
				m_client->getEventManager()->put(new SimpleTriggerEvent(MtEvent::CAMERA_PUNCH_RIGHT));
			}
		}
	}
}

static inline v2f dir(const v2f &pos_dist)
{
	f32 x = pos_dist.X - WIELDMESH_OFFSET_X;
	f32 y = pos_dist.Y - WIELDMESH_OFFSET_Y;

	f32 x_abs = std::fabs(x);
	f32 y_abs = std::fabs(y);

	if (x_abs >= y_abs) {
		y *= (1.0f / x_abs);
		x /= x_abs;
	}

	if (y_abs >= x_abs) {
		x *= (1.0f / y_abs);
		y /= y_abs;
	}

	return v2f(std::fabs(x), std::fabs(y));
}

void Camera::addArmInertia(f32 player_yaw)
{
	m_cam_vel.X = std::fabs(rangelim(m_last_cam_pos.X - player_yaw,
		-100.0f, 100.0f) / 0.016f) * 0.01f;
	m_cam_vel.Y = std::fabs((m_last_cam_pos.Y - m_camera_direction.Y) / 0.016f);
	f32 gap_X = std::fabs(WIELDMESH_OFFSET_X - m_wieldmesh_offset.X);
	f32 gap_Y = std::fabs(WIELDMESH_OFFSET_Y - m_wieldmesh_offset.Y);

	if (m_cam_vel.X > 1.0f || m_cam_vel.Y > 1.0f) {
		/*
		    The arm moves relative to the camera speed,
		    with an acceleration factor.
		*/

		if (m_cam_vel.X > 1.0f) {
			if (m_cam_vel.X > m_cam_vel_old.X)
				m_cam_vel_old.X = m_cam_vel.X;

			f32 acc_X = 0.12f * (m_cam_vel.X - (gap_X * 0.1f));
			m_wieldmesh_offset.X += m_last_cam_pos.X < player_yaw ? acc_X : -acc_X;

			if (m_last_cam_pos.X != player_yaw)
				m_last_cam_pos.X = player_yaw;

			m_wieldmesh_offset.X = rangelim(m_wieldmesh_offset.X,
				WIELDMESH_OFFSET_X - (WIELDMESH_AMPLITUDE_X * 0.5f),
				WIELDMESH_OFFSET_X + (WIELDMESH_AMPLITUDE_X * 0.5f));
		}

		if (m_cam_vel.Y > 1.0f) {
			if (m_cam_vel.Y > m_cam_vel_old.Y)
				m_cam_vel_old.Y = m_cam_vel.Y;

			f32 acc_Y = 0.12f * (m_cam_vel.Y - (gap_Y * 0.1f));
			m_wieldmesh_offset.Y +=
				m_last_cam_pos.Y > m_camera_direction.Y ? acc_Y : -acc_Y;

			if (m_last_cam_pos.Y != m_camera_direction.Y)
				m_last_cam_pos.Y = m_camera_direction.Y;

			m_wieldmesh_offset.Y = rangelim(m_wieldmesh_offset.Y,
				WIELDMESH_OFFSET_Y - (WIELDMESH_AMPLITUDE_Y * 0.5f),
				WIELDMESH_OFFSET_Y + (WIELDMESH_AMPLITUDE_Y * 0.5f));
		}

		m_arm_dir = dir(m_wieldmesh_offset);
	} else {
		/*
		    Now the arm gets back to its default position when the camera stops,
		    following a vector, with a smooth deceleration factor.
		*/

		f32 dec_X = 0.35f * (std::min(15.0f, m_cam_vel_old.X) * (1.0f +
			(1.0f - m_arm_dir.X))) * (gap_X / 20.0f);

		f32 dec_Y = 0.25f * (std::min(15.0f, m_cam_vel_old.Y) * (1.0f +
			(1.0f - m_arm_dir.Y))) * (gap_Y / 15.0f);

		if (gap_X < 0.1f)
			m_cam_vel_old.X = 0.0f;

		m_wieldmesh_offset.X -=
			m_wieldmesh_offset.X > WIELDMESH_OFFSET_X ? dec_X : -dec_X;

		if (gap_Y < 0.1f)
			m_cam_vel_old.Y = 0.0f;

		m_wieldmesh_offset.Y -=
			m_wieldmesh_offset.Y > WIELDMESH_OFFSET_Y ? dec_Y : -dec_Y;
	}
}

void Camera::update(LocalPlayer* player, f32 frametime, f32 tool_reload_ratio)
{
	// Get player position
	// Smooth the movement when walking up stairs
	v3f old_player_position = m_playernode->getPosition();
	v3f player_position = player->getPosition();

	f32 yaw = player->getYaw();
	f32 pitch = player->getPitch();

	// This is worse than `LocalPlayer::getPosition()` but
	// mods expect the player head to be at the parent's position
	// plus eye height.
	if (player->getParent())
		player_position = player->getParent()->getPosition();

	// Smooth the camera movement after the player instantly moves upward due to stepheight.
	// The smoothing usually continues until the camera position reaches the player position.
	float player_stepheight = player->getCAO() ? player->getCAO()->getStepHeight() : HUGE_VALF;
	float upward_movement = player_position.Y - old_player_position.Y;
	if (upward_movement < 0.01f || upward_movement > player_stepheight) {
		m_stepheight_smooth_active = false;
	} else if (player->touching_ground) {
		m_stepheight_smooth_active = true;
	}
	if (m_stepheight_smooth_active) {
		f32 oldy = old_player_position.Y;
		f32 newy = player_position.Y;
		f32 t = std::exp(-23 * frametime);
		player_position.Y = oldy * t + newy * (1-t);
	}

	// Set player node transformation
	m_playernode->setPosition(player_position);
	m_playernode->setRotation(v3f(0, -1 * yaw, 0));
	m_playernode->updateAbsolutePosition();

	// Get camera tilt timer (hurt animation)
	float cameratilt = fabs(fabs(player->hurt_tilt_timer-0.75)-0.75);

	// Fall bobbing animation
	float fall_bobbing = 0;
	if(player->camera_impact >= 1 && m_camera_mode < CAMERA_MODE_THIRD)
	{
		if(m_view_bobbing_fall == -1) // Effect took place and has finished
			player->camera_impact = m_view_bobbing_fall = 0;
		else if(m_view_bobbing_fall == 0) // Initialize effect
			m_view_bobbing_fall = 1;

		// Convert 0 -> 1 to 0 -> 1 -> 0
		fall_bobbing = m_view_bobbing_fall < 0.5 ? m_view_bobbing_fall * 2 : -(m_view_bobbing_fall - 0.5) * 2 + 1;
		// Smoothen and invert the above
		fall_bobbing = sin(fall_bobbing * 0.5 * M_PI) * -1;
		// Amplify according to the intensity of the impact
		if (player->camera_impact > 0.0f)
			fall_bobbing *= (1 - rangelim(50 / player->camera_impact, 0, 1)) * 5;

		fall_bobbing *= m_cache_fall_bobbing_amount;
	}

	// Calculate and translate the head SceneNode offsets
	{
		v3f eye_offset = player->getEyeOffset();
		switch(m_camera_mode) {
		case CAMERA_MODE_FIRST:
			eye_offset += player->eye_offset_first;
			break;
		case CAMERA_MODE_THIRD:
			eye_offset += player->eye_offset_third;
			break;
		case CAMERA_MODE_THIRD_FRONT:
			eye_offset.X += player->eye_offset_third_front.X;
			eye_offset.Y += player->eye_offset_third_front.Y;
			eye_offset.Z -= player->eye_offset_third_front.Z;
			break;
		}

		// Set head node transformation
		eye_offset.Y += cameratilt * -player->hurt_tilt_strength + fall_bobbing;
		m_headnode->setPosition(eye_offset);
		m_headnode->setRotation(v3f(pitch, 0,
			cameratilt * player->hurt_tilt_strength));
		m_headnode->updateAbsolutePosition();
	}

	// Compute relative camera position and target
	v3f rel_cam_pos = v3f(0,0,0);
	v3f rel_cam_target = v3f(0,0,1);
	v3f rel_cam_up = v3f(0,1,0);

	if (m_cache_view_bobbing_amount != 0.0f && m_view_bobbing_anim != 0.0f &&
		m_camera_mode < CAMERA_MODE_THIRD) {
		f32 bobfrac = my_modf(m_view_bobbing_anim * 2);
		f32 bobdir = (m_view_bobbing_anim < 0.5) ? 1.0 : -1.0;

		f32 bobknob = 1.2;
		f32 bobtmp = std::sin(std::pow(bobfrac, bobknob) * M_PI);

		v3f bobvec = v3f(
			0.3 * bobdir * std::sin(bobfrac * M_PI),
			-0.28 * bobtmp * bobtmp,
			0.);

		rel_cam_pos += bobvec * m_cache_view_bobbing_amount;
		rel_cam_target += bobvec * m_cache_view_bobbing_amount;
		rel_cam_up.rotateXYBy(-0.03 * bobdir * bobtmp * M_PI * m_cache_view_bobbing_amount);
	}

	// Compute absolute camera position and target
	m_headnode->getAbsoluteTransformation().transformVect(m_camera_position, rel_cam_pos);
	m_camera_direction = m_headnode->getAbsoluteTransformation()
			.rotateAndScaleVect(rel_cam_target - rel_cam_pos);

	v3f abs_cam_up = m_headnode->getAbsoluteTransformation()
			.rotateAndScaleVect(rel_cam_up);

	// Separate camera position for calculation
	v3f my_cp = m_camera_position;

	// Reposition the camera for third person view
	if (m_camera_mode > CAMERA_MODE_FIRST)
	{
		if (m_camera_mode == CAMERA_MODE_THIRD_FRONT)
			m_camera_direction *= -1;

		my_cp.Y += 2;

		// Calculate new position
		bool abort = false;
		for (int i = BS; i <= BS * 2.75; i++) {
			my_cp.X = m_camera_position.X + m_camera_direction.X * -i;
			my_cp.Z = m_camera_position.Z + m_camera_direction.Z * -i;
			if (i > 12)
				my_cp.Y = m_camera_position.Y + (m_camera_direction.Y * -i);

			// Prevent camera positioned inside nodes
			const NodeDefManager *nodemgr = m_client->ndef();
			MapNode n = m_client->getEnv().getClientMap()
				.getNode(floatToInt(my_cp, BS));

			const ContentFeatures& features = nodemgr->get(n);
			if (features.walkable) {
				my_cp.X += m_camera_direction.X*-1*-BS/2;
				my_cp.Z += m_camera_direction.Z*-1*-BS/2;
				my_cp.Y += m_camera_direction.Y*-1*-BS/2;
				abort = true;
				break;
			}
		}

		// If node blocks camera position don't move y to heigh
		if (abort && my_cp.Y > player_position.Y+BS*2)
			my_cp.Y = player_position.Y+BS*2;
	}

	// Update offset if too far away from the center of the map
	m_camera_offset.X += CAMERA_OFFSET_STEP*
			(((s16)(my_cp.X/BS) - m_camera_offset.X)/CAMERA_OFFSET_STEP);
	m_camera_offset.Y += CAMERA_OFFSET_STEP*
			(((s16)(my_cp.Y/BS) - m_camera_offset.Y)/CAMERA_OFFSET_STEP);
	m_camera_offset.Z += CAMERA_OFFSET_STEP*
			(((s16)(my_cp.Z/BS) - m_camera_offset.Z)/CAMERA_OFFSET_STEP);

	// Set camera node transformation
	m_cameranode->setPosition(my_cp-intToFloat(m_camera_offset, BS));
	m_cameranode->updateAbsolutePosition();
	m_cameranode->setUpVector(abs_cam_up);
	// *100.0 helps in large map coordinates
	m_cameranode->setTarget(my_cp-intToFloat(m_camera_offset, BS) + 100 * m_camera_direction);

	// update the camera position in third-person mode to render blocks behind player
	// and correctly apply liquid post FX.
	if (m_camera_mode != CAMERA_MODE_FIRST)
		m_camera_position = my_cp;

	/*
	 * Apply server-sent FOV, instantaneous or smooth transition.
	 * If not, check for zoom and set to zoom FOV.
	 * Otherwise, default to m_cache_fov.
	 */
	if (m_fov_transition_active) {
		// Smooth FOV transition
		// Dynamically calculate FOV delta based on frametimes
		f32 delta = (frametime / m_transition_time) * m_fov_diff;
		m_curr_fov_degrees += delta;

		// Mark transition as complete if target FOV has been reached
		if ((m_fov_diff > 0.0f && m_curr_fov_degrees >= m_target_fov_degrees) ||
				(m_fov_diff < 0.0f && m_curr_fov_degrees <= m_target_fov_degrees)) {
			m_fov_transition_active = false;
			m_curr_fov_degrees = m_target_fov_degrees;
		}
	} else if (m_server_sent_fov) {
		// Instantaneous FOV change
		m_curr_fov_degrees = m_target_fov_degrees;
	} else if (player->getPlayerControl().zoom && player->getZoomFOV() > 0.001f) {
		// Player requests zoom, apply zoom FOV
		m_curr_fov_degrees = player->getZoomFOV();
	} else {
		// Set to client's selected FOV
		m_curr_fov_degrees = m_cache_fov;
	}

	if (g_settings->getBool("fov_custom")) {
		m_curr_fov_degrees = rangelim(g_settings->getFloat("fov_custom.data"), 1.0f, 160.0f);
	} else {
		m_curr_fov_degrees = rangelim(m_curr_fov_degrees, 1.0f, 160.0f);
	}

	// FOV and aspect ratio
	const v2u32 &window_size = RenderingEngine::getWindowSize();
	m_aspect = (f32) window_size.X / (f32) window_size.Y;
	m_fov_y = m_curr_fov_degrees * M_PI / 180.0;
	// Increase vertical FOV on lower aspect ratios (<16:10)
	m_fov_y *= core::clamp(sqrt(16./10. / m_aspect), 1.0, 1.4);
	m_fov_x = 2 * atan(m_aspect * tan(0.5 * m_fov_y));
	m_cameranode->setAspectRatio(m_aspect);
	m_cameranode->setFOV(m_fov_y);

	// Make new matrices and frustum
	m_cameranode->updateMatrices();

	if (m_arm_inertia)
		addArmInertia(yaw);

	// Position the wielded item
	v3f wield_position = v3f(m_wieldmesh_offset.X, m_wieldmesh_offset.Y, 65);
	v3f wield_rotation = v3f(-100, 120, -100);
	wield_position.Y += std::abs(m_wield_change_timer)*320 - 40;
	// Hand/wielditem swing animation style -- see "hand_anim_style" in
	// src/gui/custom_menu/Menu.cpp (HandView picker panel). "vanilla" is
	// the original Luanti swing untouched; the rest are alternate feels
	// inspired by the swing-style options in clients like Lunarchy.
	// Declared here (moved up from just below the block right after this
	// one) so the reload-dip block can check it too.
	std::string hand_anim_style = g_settings->get("hand_anim_style");

	// Vanilla's own post-swing "settle" dip, tied to tool_reload_ratio.
	// This used to run completely unconditionally, regardless of
	// hand_anim_style -- which meant every custom style (Tilt included)
	// got this exact vanilla downward dip bleeding into roughly the
	// first 5% and the entire second half of every swing cycle (that's
	// what "m_digging_anim < 0.05 || m_digging_anim > 0.5" covers), on
	// top of whatever that style's own branch below was doing. For Tilt
	// specifically, whose whole point is that the hand doesn't move
	// through space at all, that showed up as the hand suddenly dropping
	// partway through an attack -- the vanilla arc "slipping through".
	// Gated to vanilla only now, same as it always visually was for that
	// style anyway.
	if (hand_anim_style == "vanilla" && (m_digging_anim < 0.05 || m_digging_anim > 0.5))
	{
		f32 frac = 1.0;
		if(m_digging_anim > 0.5)
			frac = 2.0 * (m_digging_anim - 0.5);
		// This value starts from 1 and settles to 0
		f32 ratiothing = std::pow((1.0f - tool_reload_ratio), 0.5f);
		f32 ratiothing2 = (easeCurve(ratiothing*0.5))*2.0;
		wield_position.Y -= frac * 25.0f * std::pow(ratiothing2, 1.7f);
		wield_position.X -= frac * 35.0f * std::pow(ratiothing2, 1.1f);
		wield_rotation.Y += frac * 70.0f * std::pow(ratiothing2, 1.4f);
	}

	if (m_digging_button != -1 && hand_anim_style != "static")
	{
		f32 digfrac = m_digging_anim;
		v3f target_rotation(80, 30, 100);

		if (hand_anim_style == "fast") {
			// Compressed into the first ~55% of the cycle and with a
			// noticeably wider arc, then holds at the swung pose for the
			// remainder -- a snappy, clipped swing rather than a slow
			// full arc.
			digfrac = std::min(m_digging_anim * 1.8f, 1.0f);
			wield_position.X -= 65 * std::sin(std::pow(digfrac, 0.7f) * M_PI);
			wield_position.Y += 24 * std::sin(digfrac * 1.8 * M_PI);
			wield_position.Z += 25 * 0.5;
			core::quaternion quat_begin(wield_rotation * core::DEGTORAD);
			core::quaternion quat_end(target_rotation * core::DEGTORAD);
			core::quaternion quat_slerp;
			quat_slerp.slerp(quat_begin, quat_end, std::sin(digfrac * M_PI));
			quat_slerp.toEuler(wield_rotation);
			wield_rotation *= core::RADTODEG;
		} else if (hand_anim_style == "sway") {
			// Vanilla arc, plus a wide pendulum sway on top -- extra Z
			// roll, some X rotation wobble, and a bigger sideways
			// travel -- for a loose, exaggerated swing that reads
			// clearly different from the vanilla arc at a glance.
			wield_position.X -= 50 * std::sin(std::pow(digfrac, 0.8f) * M_PI);
			wield_position.Y += 24 * std::sin(digfrac * 1.8 * M_PI);
			wield_position.Z += 25 * 0.5;
			core::quaternion quat_begin(wield_rotation * core::DEGTORAD);
			core::quaternion quat_end(target_rotation * core::DEGTORAD);
			core::quaternion quat_slerp;
			quat_slerp.slerp(quat_begin, quat_end, std::sin(digfrac * M_PI));
			quat_slerp.toEuler(wield_rotation);
			wield_rotation *= core::RADTODEG;
			wield_rotation.Z += 38.0f * std::sin(digfrac * M_PI);
			wield_rotation.X += 16.0f * std::sin(digfrac * 2.0f * M_PI);
			wield_position.X -= 26.0f * std::sin(digfrac * M_PI);
		} else if (hand_anim_style == "chime") {
			// Bouncier arc with a fast, clearly audible-feeling
			// double-bounce layered onto both the vertical motion and
			// the roll, popularized by "chime"-style swings.
			wield_position.X -= 50 * std::sin(std::pow(digfrac, 0.8f) * M_PI);
			wield_position.Y += 24 * std::sin(digfrac * 1.8 * M_PI);
			wield_position.Y += 15.0f * std::sin(digfrac * M_PI * 5.0f);
			wield_position.Z += 25 * 0.5;
			core::quaternion quat_begin(wield_rotation * core::DEGTORAD);
			core::quaternion quat_end(target_rotation * core::DEGTORAD);
			core::quaternion quat_slerp;
			quat_slerp.slerp(quat_begin, quat_end, std::sin(digfrac * M_PI));
			quat_slerp.toEuler(wield_rotation);
			wield_rotation *= core::RADTODEG;
			wield_rotation.Z += 10.0f * std::sin(digfrac * M_PI * 5.0f);
		} else if (hand_anim_style == "old") {
			// Genuinely "classic"/blocky swing: a plain triangular
			// (linear out, linear back) envelope instead of the sine
			// ease everyone else uses, and a straight per-axis lerp
			// instead of a slerp -- so it reads as a stiffer, more
			// mechanical swing rather than a softened version of the
			// vanilla one.
			f32 tri = digfrac < 0.5f ? (digfrac * 2.0f) : (2.0f - digfrac * 2.0f);
			wield_position.X -= 65 * tri;
			wield_position.Y += 30 * tri;
			wield_position.Z += 25 * 0.5;
			wield_rotation = wield_rotation * (1.0f - tri) + target_rotation * tri;
		} else if (hand_anim_style == "punch") {
			// An original "hit" style: instead of swinging sideways
			// like the others, the item thrusts sharply toward the
			// camera and dips down, with a quick rotational kick, then
			// eases back out over the remainder of the cycle -- reads
			// as a single percussive jab rather than a pendulum swing.
			f32 out_t = std::min(digfrac / 0.4f, 1.0f);
			f32 back_t = digfrac <= 0.4f ? 0.0f : (digfrac - 0.4f) / 0.6f;
			f32 outEase = std::sin(out_t * (f32)M_PI * 0.5f);
			f32 backEase = std::sin(back_t * (f32)M_PI * 0.5f);
			f32 punch = outEase * (1.0f - backEase);

			wield_position.X -= 30.0f * punch;
			wield_position.Y -= 18.0f * punch;
			wield_position.Z += 40.0f * punch + 25 * 0.5f;

			v3f punch_rotation = wield_rotation + v3f(-25, 10, -15);
			core::quaternion quat_begin(wield_rotation * core::DEGTORAD);
			core::quaternion quat_end(punch_rotation * core::DEGTORAD);
			core::quaternion quat_slerp;
			quat_slerp.slerp(quat_begin, quat_end, punch);
			quat_slerp.toEuler(wield_rotation);
			wield_rotation *= core::RADTODEG;
		} else if (hand_anim_style == "tilt") {
			// "Tilt": matches lilacbyte/lunarchy's LagOptimizer swing
			// (https://github.com/lilacbyte/lunarchy, src/client/camera.cpp
			// -- the "lag_optimizer.no_hand_animation" branch of
			// Camera::updateViewingRange()/update(), specifically its
			// "else if (m_digging_button != -1)" case). Unlike every
			// other style here, the wielded item's position doesn't move
			// through a swing arc at all -- only its rotation tilts
			// toward target_rotation and back, eased by the same
			// sin(digfrac * PI) curve as vanilla's swing.
			core::quaternion quat_begin2(wield_rotation * core::DEGTORAD);
			core::quaternion quat_end2(target_rotation * core::DEGTORAD);
			core::quaternion quat_slerp2;
			quat_slerp2.slerp(quat_begin2, quat_end2, std::sin(digfrac * M_PI));
			quat_slerp2.toEuler(wield_rotation);
			wield_rotation *= core::RADTODEG;
		} else {
			// "vanilla" (default) -- untouched original Luanti swing.
			wield_position.X -= 50 * std::sin(std::pow(digfrac, 0.8f) * M_PI);
			wield_position.Y += 24 * std::sin(digfrac * 1.8 * M_PI);
			wield_position.Z += 25 * 0.5;
			core::quaternion quat_begin(wield_rotation * core::DEGTORAD);
			core::quaternion quat_end(target_rotation * core::DEGTORAD);
			core::quaternion quat_slerp;
			quat_slerp.slerp(quat_begin, quat_end, std::sin(digfrac * M_PI));
			quat_slerp.toEuler(wield_rotation);
			wield_rotation *= core::RADTODEG;
		}

		// Remembered so the "just finished swinging" branch below can
		// ease back out of exactly this pose, instead of the item just
		// teleporting back to rest the instant m_digging_anim wraps back
		// to 0 (which happens after a fixed ~0.29s regardless of the
		// tool's actual attack speed -- see the "offset = dtime * 3.5f"
		// in Camera::step() above).
		m_last_swing_wield_position = wield_position;
		m_last_swing_wield_rotation = wield_rotation;
		m_was_swinging = true;
	} else {
		if (!g_settings->getBool("no_view_bob")) {
			f32 bobfrac = my_modf(m_view_bobbing_anim);
			wield_position.X -= std::sin(bobfrac*M_PI*2.0) * 3.0;
			wield_position.Y += std::sin(my_modf(bobfrac*2.0)*M_PI) * 3.0;
		}

		// This is the first frame the swing animation itself isn't
		// playing anymore -- note how far into the punch cooldown we
		// are right now, so the ease-back below knows how much of that
		// cooldown is left to spread the return motion across.
		if (m_was_swinging) {
			m_swing_end_reload_ratio = tool_reload_ratio;
			m_was_swinging = false;
		}

		// Ease the weapon back from wherever the swing left it to the
		// idle/bob pose computed just above, over whatever's left of the
		// punch cooldown (tool_reload_ratio, same value the reload-dip
		// above uses) -- rather than the abrupt snap-back this used to
		// be. "static" never entered the swing branch above in the
		// first place (nothing moved, nothing to ease back from), so
		// skip it here too.
		if (hand_anim_style != "static" && tool_reload_ratio < 1.0f) {
			f32 denom = std::max(0.001f, 1.0f - m_swing_end_reload_ratio);
			f32 t = core::clamp(
				(tool_reload_ratio - m_swing_end_reload_ratio) / denom, 0.0f, 1.0f);
			wield_position = m_last_swing_wield_position * (1.0f - t) + wield_position * t;
			wield_rotation = m_last_swing_wield_rotation * (1.0f - t) + wield_rotation * t;
		}
	}
	// "static" never plays a swing/return-ease during attacks or item use
	// (the hand just sits at its rest pose for those) but still gets the
	// idle walking bob below like every other style, unless "no_view_bob"
	// ("NoViewBob" in the HandView panel) is also on -- that one only
	// ever suppresses *this* idle walking wobble specifically. The
	// digging/punch/tilt/etc. swing animations above (attack and item
	// use) are a completely separate branch of this if/else chain and
	// are untouched by it.

	if (g_settings->getBool("left_hand")) {
		wield_position.X = -wield_position.X;

		core::quaternion quat(wield_rotation * core::DEGTORAD);

		quat.X = -quat.X;
		quat.W = -quat.W;

		core::quaternion pitchFix(v3f(180, 0, 180) * core::DEGTORAD);
		quat = pitchFix * quat;

		quat.toEuler(wield_rotation);
		wield_rotation *= core::RADTODEG;
	}

	// HandView: an additional, independent offset + scale on top of
	// everything above -- lets the hand/wielditem be repositioned or
	// resized without touching the swing animation itself. Off by
	// default (all zero/1.0), so vanilla feel is unchanged unless the
	// user actually drags/resizes it (see "HandView" in
	// src/gui/custom_menu/Menu.cpp -- draggable + scroll-to-resize in
	// "Move HUD" edit mode, same as the other HUD elements).
	if (g_settings->getBool("handview_enabled")) {
		wield_position.X += g_settings->getFloat("handview_offset_x");
		wield_position.Y += g_settings->getFloat("handview_offset_y");
		wield_position.Z += g_settings->getFloat("handview_offset_z");
	}

	m_wieldnode->setPosition(wield_position);
	m_wieldnode->setRotation(wield_rotation);

	if (g_settings->getBool("handview_enabled")) {
		f32 handview_scale = rangelim(g_settings->getFloat("handview_scale"), 0.3f, 3.0f);
		m_wieldnode->setScale(v3f(handview_scale, handview_scale, handview_scale));
	} else if (m_wieldnode->getScale() != v3f(1.0f, 1.0f, 1.0f)) {
		// Settings was just turned off -- snap back to normal size
		// instead of getting stuck at whatever scale was last set.
		m_wieldnode->setScale(v3f(1.0f, 1.0f, 1.0f));
	}

	m_player_light_color = player->light_color;
	m_wieldnode->setNodeLightColor(m_player_light_color);

	// Set render distance
	updateViewingRange();

	// If the player is walking, swimming, or climbing,
	// view bobbing is enabled and free_move is off,
	// start (or continue) the view bobbing animation.
	const v3f &speed = player->getSpeed();
	const bool movement_XZ = std::hypot(speed.X, speed.Z) > BS;
	const bool movement_Y = std::abs(speed.Y) > BS;

	const bool walking = movement_XZ && player->touching_ground;
	const bool swimming = (movement_XZ || player->swimming_vertical) && player->in_liquid;
	const bool climbing = movement_Y && player->is_climbing;
	const bool flying = g_settings->getBool("free_move")
		&& m_client->checkLocalPrivilege("fly");
	if ((walking || swimming || climbing) && !flying) {
		// Start animation
		m_view_bobbing_state = 1;
		m_view_bobbing_speed = MYMIN(speed.getLength(), 70);
	} else if (m_view_bobbing_state == 1) {
		// Stop animation
		m_view_bobbing_state = 2;
		m_view_bobbing_speed = 60;
	}
}

void Camera::updateViewingRange()
{
	f32 viewing_range = g_settings->getFloat("viewing_range");

	m_cameranode->setNearValue(0.1f * BS);

	m_draw_control.wanted_range = std::fmin(adjustDist(viewing_range, getFovMax()), 4000);
	if (m_draw_control.range_all) {
		m_cameranode->setFarValue(100000.0);
		return;
	}
	m_cameranode->setFarValue((viewing_range < 2000) ? 2000 * BS : viewing_range * BS);
}

void Camera::setDigging(s32 button)
{
	if (m_digging_button == -1)
		m_digging_button = button;
}

void Camera::wield(const ItemStack &item)
{
	if (item.name != m_wield_item_next.name ||
			item.metadata != m_wield_item_next.metadata) {
		m_wield_item_next = item;
		if (m_wield_change_timer > 0)
			m_wield_change_timer = -m_wield_change_timer;
		else if (m_wield_change_timer == 0)
			m_wield_change_timer = -0.001;
	}
}

void Camera::drawWieldedTool(irr::core::matrix4* translation)
{
	// Clear Z buffer so that the wielded tool stays in front of world geometry
	m_wieldmgr->getVideoDriver()->clearBuffers(video::ECBF_DEPTH);

	// Draw the wielded node (in a separate scene manager)
	scene::ICameraSceneNode* cam = m_wieldmgr->getActiveCamera();
	cam->setAspectRatio(m_cameranode->getAspectRatio());
	cam->setFOV(72.0*M_PI/180.0);
	cam->setNearValue(10);
	cam->setFarValue(1000);
	if (translation != NULL)
	{
		irr::core::matrix4 startMatrix = cam->getAbsoluteTransformation();
		irr::core::vector3df focusPoint = (cam->getTarget()
				- cam->getAbsolutePosition()).setLength(1)
				+ cam->getAbsolutePosition();

		irr::core::vector3df camera_pos =
				(startMatrix * *translation).getTranslation();
		cam->setPosition(camera_pos);
		cam->updateAbsolutePosition();
		cam->setTarget(focusPoint);
	}
	m_wieldmgr->drawAll();
}

void Camera::drawNametags()
{
	core::matrix4 trans = m_cameranode->getProjectionMatrix();
	trans *= m_cameranode->getViewMatrix();

	gui::IGUIFont *font = g_fontengine->getFont();
	video::IVideoDriver *driver = RenderingEngine::get_video_driver();
	v2u32 screensize = driver->getScreenSize();

	static const video::SColor friend_color(255, 60, 230, 110);
	static const video::SColor friend_bg(190, 15, 60, 30);

	for (const Nametag *nametag : m_nametags) {
		// Nametags are hidden in GenericCAO::updateNametag()

		// Match against the real player name, not `nametag->text`: servers
		// are free to prefix/style the displayed nametag (rank tags, clan
		// tags, colors, ...), and matching on that text silently broke
		// friend highlighting/badges whenever a server did so. Fall back
		// to the display text only for the rare case owner_name wasn't set
		// (e.g. non-player entities, which won't match anything anyway).
		const std::string &match_name =
			nametag->owner_name.empty() ? nametag->text : nametag->owner_name;
		bool is_friend = FriendList::get().isFriend(match_name);

		v3f pos = nametag->parent_node->getAbsolutePosition() + nametag->pos * BS;
		f32 transformed_pos[4] = { pos.X, pos.Y, pos.Z, 1.0f };
		trans.multiplyWith1x4Matrix(transformed_pos);
		if (transformed_pos[3] > 0) {
			std::wstring nametag_colorless =
				unescape_translate(utf8_to_wide(nametag->text));

			std::wstring display_text =
				translate_string(utf8_to_wide(nametag->text));
			if (is_friend) {
				f32 distance = (pos - (m_camera_position -
					intToFloat(m_camera_offset, BS))).getLength() / BS;
				std::wstring suffix =
					L" [" + std::to_wstring((int)distance) + L"m]";
				nametag_colorless += suffix;
				display_text += suffix;
			}

			core::dimension2d<u32> textsize = font->getDimension(
				nametag_colorless.c_str());
			f32 zDiv = transformed_pos[3] == 0.0f ? 1.0f :
				core::reciprocal(transformed_pos[3]);
			v2s32 screen_pos;
			screen_pos.X = screensize.X *
				(0.5 * transformed_pos[0] * zDiv + 0.5) - textsize.Width / 2;
			screen_pos.Y = screensize.Y *
				(0.5 - transformed_pos[1] * zDiv * 0.5) - textsize.Height / 2;
			core::rect<s32> size(0, 0, textsize.Width, textsize.Height);

			video::SColor textcolor = is_friend ? friend_color : nametag->textcolor;
			auto bgcolor = is_friend ? friend_bg :
				nametag->getBgColor(m_show_nametag_backgrounds);
			if (bgcolor.getAlpha() != 0) {
				core::rect<s32> bg_size(-2, 0, textsize.Width + 2, textsize.Height);
				driver->draw2DRectangle(bgcolor, bg_size + screen_pos);
			}

			font->draw(display_text.c_str(), size + screen_pos, textcolor);
		}
	}
}

void Camera::drawFriendESP()
{
	const auto &friends = FriendList::get().getAll();
	if (friends.empty())
		return; // Nothing to do, avoid the active object scan below.

	core::matrix4 trans = m_cameranode->getProjectionMatrix();
	trans *= m_cameranode->getViewMatrix();

	video::IVideoDriver *driver = RenderingEngine::get_video_driver();
	v2u32 screensize = driver->getScreenSize();

	static const video::SColor box_outline(255, 60, 230, 110);
	static const video::SColor box_fill(60, 60, 230, 110);

	// Projects a world position to screen space. Returns false if the
	// point lies behind the camera.
	auto project = [&](const v3f &pos, v2s32 &out) -> bool {
		f32 transformed_pos[4] = { pos.X, pos.Y, pos.Z, 1.0f };
		trans.multiplyWith1x4Matrix(transformed_pos);
		if (transformed_pos[3] <= 0)
			return false;
		f32 zDiv = transformed_pos[3] == 0.0f ? 1.0f : core::reciprocal(transformed_pos[3]);
		out.X = screensize.X * (0.5f * transformed_pos[0] * zDiv + 0.5f);
		out.Y = screensize.Y * (0.5f - transformed_pos[1] * zDiv * 0.5f);
		return true;
	};

	std::unordered_map<u16, ClientActiveObject*> objects;
	m_client->getEnv().getAllActiveObjects(objects);

	for (const auto &pair : objects) {
		ClientActiveObject *obj = pair.second;
		GenericCAO *cao = dynamic_cast<GenericCAO*>(obj);
		if (!cao || !cao->isPlayer())
			continue;

		if (!FriendList::get().isFriend(cao->getName()))
			continue;

		v3f base_pos = cao->getPosition() - intToFloat(m_camera_offset, BS);

		// Project the corners of the player's selection box to screen
		// space and highlight it, so friends stand out clearly even
		// through walls.
		aabb3f box(v3f(0.0f, 0.0f, 0.0f), v3f(0.0f, 0.0f, 0.0f));
		if (!cao->getSelectionBox(&box))
			continue;

		v2s32 min_pt(0x7fffffff, 0x7fffffff);
		v2s32 max_pt(-0x7fffffff, -0x7fffffff);
		bool any_corner_visible = false;

		for (int i = 0; i < 8; i++) {
			v3f corner(
				(i & 1) ? box.MaxEdge.X : box.MinEdge.X,
				(i & 2) ? box.MaxEdge.Y : box.MinEdge.Y,
				(i & 4) ? box.MaxEdge.Z : box.MinEdge.Z);
			v2s32 screen_pt;
			if (!project(base_pos + corner, screen_pt))
				continue;
			any_corner_visible = true;
			min_pt.X = std::min(min_pt.X, screen_pt.X);
			min_pt.Y = std::min(min_pt.Y, screen_pt.Y);
			max_pt.X = std::max(max_pt.X, screen_pt.X);
			max_pt.Y = std::max(max_pt.Y, screen_pt.Y);
		}

		if (!any_corner_visible)
			continue;

		core::rect<s32> box_rect(min_pt.X, min_pt.Y, max_pt.X, max_pt.Y);
		box_rect.repair();
		driver->draw2DRectangle(box_fill, box_rect);
		driver->draw2DRectangleOutline(box_rect, box_outline, 2);
	}
}

void Camera::drawMineBoostBadges()
{
	// Skip the expensive "copy+walk every active object, dynamic_cast
	// each one" work below entirely on the (very common -- most players,
	// most sessions, most servers) frames where nobody's ever been seen
	// as a MineBoost user at all: a single empty-map check replaces a
	// full active-object-map copy plus a dynamic_cast<GenericCAO*> per
	// object, every single frame, whenever there's nothing to show.
	const unsigned long long now_ms = porting::getTimeMs();
	if (!MineBoostPresence::get().hasAnyRecentUsers())
		return;

	// ITextureSource::getTexture() caches internally (and, unlike a raw
	// pointer we'd cache ourselves here, gets invalidated correctly if
	// the texture source/driver is ever recreated -- e.g. across a
	// disconnect/reconnect), so there's no need to hold onto the result
	// across frames. A previous version of this function did cache it in
	// a function-local static ITexture*, which went stale exactly like
	// that and made every draw2DImage() call below fail with Irrlicht's
	// "Tried to set a texture not owned by this driver" fatal error,
	// spamming it every single frame from then on.
	video::ITexture *badge_tex = m_client->getTextureSource()->getTexture("mineboostv2_badge.png");
	if (!badge_tex)
		return;

	core::matrix4 trans = m_cameranode->getProjectionMatrix();
	trans *= m_cameranode->getViewMatrix();

	video::IVideoDriver *driver = RenderingEngine::get_video_driver();
	v2u32 screensize = driver->getScreenSize();
	gui::IGUIFont *font = g_fontengine->getFont();

	std::unordered_map<u16, ClientActiveObject*> objects;
	m_client->getEnv().getAllActiveObjects(objects);

	for (const auto &pair : objects) {
		ClientActiveObject *obj = pair.second;
		GenericCAO *cao = dynamic_cast<GenericCAO*>(obj);
		if (!cao || !cao->isPlayer() || cao->isLocalPlayer())
			continue;

		if (!MineBoostPresence::get().isMineBoostUser(cao->getName(), now_ms))
			continue;

		// Same "top of the player's own selection box" positioning
		// drawFriendESP() above uses -- deliberately not GenericCAO::
		// updateNametag()'s m_prop.selectionbox/m_prop.nametag, which a
		// server (or a mod) can and does blank out to hide the floating
		// nametag text. Since the badge doesn't depend on nametag text
		// existing at all, it keeps showing either way. 0.55 * BS instead
		// of the nametag's own ~0.3 * BS-ish clearance above the head, so
		// the badge sits with room to spare above wherever the nametag
		// itself ends up, rather than right at the same height.
		v3f base_pos = cao->getPosition() - intToFloat(m_camera_offset, BS);
		aabb3f box(v3f(0.0f, 0.0f, 0.0f), v3f(0.0f, 0.0f, 0.0f));
		if (!cao->getSelectionBox(&box))
			continue;
		v3f pos = base_pos + v3f(0.0f, box.MaxEdge.Y + 0.55f * BS, 0.0f);

		f32 transformed_pos[4] = { pos.X, pos.Y, pos.Z, 1.0f };
		trans.multiplyWith1x4Matrix(transformed_pos);
		if (transformed_pos[3] <= 0)
			continue;
		f32 zDiv = transformed_pos[3] == 0.0f ? 1.0f : core::reciprocal(transformed_pos[3]);
		v2s32 screen_pos;
		screen_pos.X = screensize.X * (0.5f * transformed_pos[0] * zDiv + 0.5f);
		screen_pos.Y = screensize.Y * (0.5f - transformed_pos[1] * zDiv * 0.5f);

		// A bit bigger than plain font-line-height (was getting lost/
		// crowding the nametag right below it), plus a small fixed gap
		// between the badge's bottom edge and the anchor point so it
		// never touches the nametag even at close range.
		s32 badge_size = font ? (s32)(font->getDimension(L"A").Height * 1.7f) : 27;
		s32 gap = std::max(1, badge_size / 10);
		core::rect<s32> badge_rect(
			screen_pos.X - badge_size / 2, screen_pos.Y - badge_size - gap,
			screen_pos.X + badge_size / 2, screen_pos.Y - gap);
		core::dimension2d<u32> badge_tex_size = badge_tex->getOriginalSize();
		core::rect<s32> badge_src(0, 0,
			(s32)badge_tex_size.Width, (s32)badge_tex_size.Height);
		driver->draw2DImage(badge_tex, badge_rect, badge_src,
			nullptr, nullptr, true);
	}
}

Nametag *Camera::addNametag(scene::ISceneNode *parent_node,
		const std::string &text, video::SColor textcolor,
		std::optional<video::SColor> bgcolor, const v3f &pos,
		const std::string &owner_name)
{
	Nametag *nametag = new Nametag(parent_node, text, textcolor, bgcolor, pos, owner_name);
	m_nametags.push_back(nametag);
	return nametag;
}

void Camera::removeNametag(Nametag *nametag)
{
	m_nametags.remove(nametag);
	delete nametag;
}

std::array<core::plane3d<f32>, 4> Camera::getFrustumCullPlanes() const
{
	using irr::scene::SViewFrustum;
	const auto &frustum_planes = m_cameranode->getViewFrustum()->planes;
	return {
		frustum_planes[SViewFrustum::VF_LEFT_PLANE],
		frustum_planes[SViewFrustum::VF_RIGHT_PLANE],
		frustum_planes[SViewFrustum::VF_BOTTOM_PLANE],
		frustum_planes[SViewFrustum::VF_TOP_PLANE],
	};
}
