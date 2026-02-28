/*
 * uevr_receiver.hpp - UEVR Bridge Receiver for VRto3D (Cooperative Mode)
 * Protocol v3.3
 *
 * INSTALLATION:
 *   1. Copy this file to: vrto3d/src/uevr_receiver.hpp
 *   2. Include in hmd_device_driver.cpp
 *   3. Follow integration steps in INTEGRATION_GUIDE.md
 *
 * v3.1 CHANGES:
 *   - Profile modifier fields in shared memory (48 bytes from reserved)
 *   - VRto3D-side exponential smoothing for depth transitions
 *   - write_modifiers() for per-game JSON profile overrides
 *   - Backward compatible: v3.0 UEVR ignores modifier fields
 *   - Uses uevr_vrto3d_protocol.h as single source of truth for struct layout
 *
 * KEY DESIGN:
 *   - VRto3D's profile is the MASTER for depth/convergence
 *   - UEVR sends MULTIPLIERS (not absolute values)
 *   - VRto3D calculates: actual_depth = base_depth * multiplier
 *   - NO conflicts with VRto3D's saved settings!
 */

#pragma once

#include <Windows.h>
#include <cstdint>
#include <cmath>
#include <string>
#include <algorithm>

// Single source of truth for the shared memory layout
#include "uevr_vrto3d_protocol.h"

namespace uevr {

// Map protocol constants to this namespace
constexpr uint32_t UEVR_MAGIC = UEVR_VRTO3D_MAGIC;
constexpr const char* SHARED_MEM_NAME = UEVR_VRTO3D_SHMEM_NAME;

// Use the protocol header's struct definition
using SharedData = UEVR_VRto3D_SharedData;

// Compile-time verification that the protocol struct is still 256 bytes
static_assert(sizeof(SharedData) == 256, "SharedData must be 256 bytes!");

// C++ scoped enum wrappers around the protocol's C-style enums
enum class SceneType : uint8_t {
    Normal   = UEVR_SCENE_NORMAL,
    Cutscene = UEVR_SCENE_CUTSCENE,
    Menu     = UEVR_SCENE_MENU,
    Vehicle  = UEVR_SCENE_VEHICLE,
    Loading  = UEVR_SCENE_LOADING
};

// ============================================================================
// PROFILE MODIFIERS (for JSON profile "uevr_modifiers" section)
// ============================================================================

struct ProfileModifiers {
    bool active = false;
    float depth_strength = 0.0f;       // 0 = not overridden
    float depth_min_floor = 0.0f;
    float ads_floor = 0.0f;
    float scope_floor = 0.0f;
    float cutscene_floor = 0.0f;
    float base_power = 0.0f;
    float extra_power = 0.0f;
    float dead_zone = 0.0f;
    float transition_speed = 0.0f;
    float zoom_threshold = 0.0f;
    float base_fov_override = 0.0f;
    // v3.4: Per-mode convergence blend overrides (0 = use defaults)
    float blend_ads = 0.0f;            // Override ADS blend (default 0.85)
    float blend_scope = 0.0f;          // Override Scope blend (default 0.55)
    float blend_passive = 0.0f;        // Override passive blend (default 0.70)
};

// ============================================================================
// UEVR RECEIVER CLASS
// ============================================================================

class Receiver {
public:
    // ----- Singleton -----
    static Receiver& instance() {
        static Receiver inst;
        return inst;
    }

    // ----- Lifecycle -----

    bool init() {
        if (m_data) return true;

        m_mapping = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, SHARED_MEM_NAME);
        if (!m_mapping) return false;

        m_data = static_cast<SharedData*>(MapViewOfFile(
            m_mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedData)));

        if (!m_data) {
            cleanup();
            return false;
        }
        if (m_data->magic != UEVR_MAGIC) {
            // v3.4: Track protocol mismatch for later logging by caller
            m_last_magic_mismatch = m_data->magic;
            cleanup();
            return false;
        }

        return true;
    }

    void shutdown() {
        if (m_data) {
            m_data->vrto3d_connected = 0;
            m_data->vrto3d_auto_depth_active = 0;
        }
        cleanup();
    }

    bool is_connected() const { return m_data != nullptr; }

    // v3.4: Protocol mismatch check — non-zero if last init() saw wrong magic
    uint32_t get_last_magic_mismatch() const { return m_last_magic_mismatch; }

    // ----- Raw field accessors (for debug logging) -----
    uint32_t raw_magic() const { return m_data ? m_data->magic : 0; }
    uint32_t raw_version() const { return m_data ? m_data->version : 0; }
    uint8_t  raw_is_valid() const { return m_data ? m_data->is_valid : 0; }
    uint8_t  raw_is_zooming() const { return m_data ? m_data->is_zooming : 0; }
    float    raw_depth_multiplier() const { return m_data ? m_data->depth_multiplier : 1.0f; }
    float    raw_game_fov() const { return m_data ? m_data->game_fov : 0.0f; }
    uint64_t raw_timestamp() const { return m_data ? m_data->uevr_timestamp : 0; }

    // ----- Main Update (call every frame) -----

    /**
     * Update VRto3D state and check for UEVR data.
     * Call this every frame from VRto3D's render loop.
     */
    void update(float depth, float convergence, float fov, float fov_adj,
                uint8_t sbs_mode, bool profile_loaded = false) {
        if (!m_data && !init()) return;

        if (m_data->magic != UEVR_MAGIC) {
            cleanup();
            return;
        }

        // v3.3: In monitor mode, use UEVR's stereo_depth_hint as base depth
        // so overlay IPD (Prop_UserIpdMeters_Float) matches game world stereo.
        // When hint is 0 or unavailable, fall back to config.depth as before.
        float hint = m_data->stereo_depth_hint;
        if (m_data->monitor_mode && std::isfinite(hint) && hint > 0.001f && hint < 2.0f) {
            m_base_depth = hint;
        } else {
            m_base_depth = depth;
        }
        m_base_convergence = convergence;

        // Write VRto3D CONFIG state back to UEVR (intentionally config.depth, not hint).
        // UEVR's debug readout compares this against stereo_depth to show the mismatch.
        m_data->vrto3d_depth = depth;
        m_data->vrto3d_convergence = convergence;
        m_data->vrto3d_fov = fov;
        m_data->vrto3d_fov_adjustment = fov_adj;
        m_data->vrto3d_sbs_mode = sbs_mode;
        m_data->vrto3d_connected = 1;
        m_data->vrto3d_listener_enabled = m_auto_depth_enabled ? 1 : 0;
        m_data->vrto3d_profile_loaded = profile_loaded ? 1 : 0;
        m_data->is_monitor_display = 1;  // v3.2: VRto3D is always a monitor driver
        m_data->vrto3d_timestamp = GetTickCount64();
    }

    // ----- Data Validity -----

    bool has_valid_data() const {
        if (!m_data) return false;
        if (!m_data->is_valid) return false;

        uint64_t now = GetTickCount64();
        return (now - m_data->uevr_timestamp) < UEVR_VRTO3D_STALE_MS;
    }

    // ----- UEVR State Queries -----

    bool is_zooming() const {
        return has_valid_data() && m_data->is_zooming;
    }

    float get_fov_scale() const {
        return has_valid_data() ? m_data->fov_scale : 1.0f;
    }

    float get_zoom_factor() const {
        return has_valid_data() ? m_data->zoom_factor : 1.0f;
    }

    uint8_t get_zoom_mode() const {
        return has_valid_data() ? m_data->zoom_mode : 0;
    }

    SceneType get_scene_type() const {
        return has_valid_data()
            ? static_cast<SceneType>(m_data->scene_type)
            : SceneType::Normal;
    }

    bool is_cutscene() const { return get_scene_type() == SceneType::Cutscene; }
    bool is_menu() const { return get_scene_type() == SceneType::Menu; }

    /// v3.2: Read monitor_mode flag from UEVR bridge
    bool get_monitor_mode() const {
        return has_valid_data() && m_data->monitor_mode != 0;
    }

    /// v3.3: Get stereo depth hint from UEVR (0 = not provided)
    float get_stereo_depth_hint() const {
        if (!has_valid_data()) return 0.0f;
        float hint = m_data->stereo_depth_hint;
        return (std::isfinite(hint) && hint > 0.0f && hint < 2.0f) ? hint : 0.0f;
    }

    float get_world_scale() const {
        return has_valid_data() ? m_data->world_scale : 1.0f;
    }

    float get_convergence_multiplier() const {
        return has_valid_data() ? m_data->convergence_multiplier : 1.0f;
    }

    /**
     * v3.4: Get stereo aim correction (zoom-scaled) from UEVR.
     * Lateral shift applied during ADS/scope, scaled by zoom intensity.
     */
    float get_stereo_aim_correction() const {
        return has_valid_data() ? m_data->stereo_aim_correction : 0.0f;
    }

    /**
     * v3.4: Get stereo aim base offset from UEVR.
     * Constant lateral shift applied at all times (hip-fire + scoped).
     */
    float get_stereo_aim_base() const {
        return has_valid_data() ? m_data->stereo_aim_base : 0.0f;
    }

    // ----- UEVR Profile Info -----

    std::string get_uevr_profile_name() const {
        if (!m_data) return "";
        return std::string(m_data->uevr_profile_name);
    }

    std::string get_game_exe_name() const {
        if (!m_data) return "";
        return std::string(m_data->game_exe_name);
    }

    // ================================================================
    // DEPTH CALCULATION
    // ================================================================

    /**
     * Get the raw depth multiplier UEVR is requesting.
     * Returns 1.0 if no change requested, < 1.0 if reduced depth wanted.
     * NOTE: In v3.1, UEVR already smooths on its side. This returns
     * the pre-smoothed value from UEVR. Use get_smoothed_depth() for
     * additional VRto3D-side smoothing if desired.
     */
    float get_depth_multiplier() const {
        if (!has_valid_data()) return 1.0f;
        float mult = m_data->depth_multiplier;
        return (std::max)(0.05f, (std::min)(mult, 1.0f));
    }

    /**
     * Is UEVR requesting a depth change?
     */
    bool is_requesting_depth_change() const {
        if (!m_auto_depth_enabled) return false;
        if (!has_valid_data()) return false;

        float mult = get_depth_multiplier();
        return std::abs(mult - 1.0f) > 0.01f;
    }

    /**
     * Calculate the effective depth to use.
     * VRto3D's base_depth * UEVR's raw multiplier.
     */
    float get_effective_depth() const {
        return m_base_depth * get_depth_multiplier();
    }

    /**
     * Get depth for rendering - primary method for hmd_device_driver.
     * Uses smoothed depth if auto-depth active, otherwise base depth.
     *
     * @param delta_time  Frame time in seconds (for smoothing)
     */
    float get_depth_for_rendering(float delta_time) {
        float target_mult = 1.0f;  // Default: return to base depth

        if (m_auto_depth_enabled && has_valid_data()) {
            target_mult = get_depth_multiplier();
        }

        // UEVR already applies asymmetric exponential smoothing (GameFOV.cpp),
        // so we DON'T re-smooth here — that caused phase lag and extended
        // the jitter window. We only apply light safety smoothing when
        // returning to baseline (target_mult == 1.0 && multiplier != 1.0)
        // to prevent a snap if UEVR disconnects or sends a sudden reset.
        if (target_mult == 1.0f && std::abs(m_smoothed_multiplier - 1.0f) > 0.01f) {
            // Disconnected or reset — smooth the return to baseline
            float alpha = 1.0f - std::exp(-m_smooth_rate * delta_time);
            alpha = (std::max)(0.0f, (std::min)(alpha, 1.0f));
            m_smoothed_multiplier += alpha * (1.0f - m_smoothed_multiplier);
            if (std::abs(m_smoothed_multiplier - 1.0f) < 0.001f) {
                m_smoothed_multiplier = 1.0f;
            }
        } else {
            // UEVR is actively sending smoothed values — pass through directly
            m_smoothed_multiplier = target_mult;
        }

        return m_base_depth * m_smoothed_multiplier;
    }

    /**
     * Get the current smoothed multiplier value (for debug display).
     */
    float get_smoothed_multiplier() const {
        return m_smoothed_multiplier;
    }

    // ================================================================
    // CONVERGENCE CALCULATION
    // ================================================================

    /**
     * v3.1: Get convergence for rendering with scene-aware modulation.
     *
     * Reads UEVR's convergence_multiplier, scene_type, and world_scale
     * to calculate context-appropriate convergence with smoothing.
     *
     * Disconnect recovery: if UEVR data goes stale, target smoothly
     * returns to base convergence (target_mult = 1.0).
     *
     * @param delta_time  Frame time in seconds
     * @return Smoothed convergence value (base_convergence * smoothed_mult)
     */
    float get_convergence_for_rendering(float delta_time) {
        float target_mult = 1.0f;

        if (m_auto_convergence_enabled && has_valid_data()) {
            // Read scene-based convergence multiplier from UEVR
            float uevr_mult = get_convergence_multiplier();
            if (uevr_mult > 0.01f) target_mult = uevr_mult;

            // v3.3: Unified dynamic convergence — all three parameters communicate.
            // Scene-type modulation for Cutscene/Menu (full world_scale).
            // Normal gameplay: FOV-proportional + throttled world_scale during zoom.
            if (std::abs(target_mult - 1.0f) < 0.01f) {
                auto scene = get_scene_type();
                float ws = (std::max)(0.1f, (std::min)(get_world_scale(), 10.0f));
                switch (scene) {
                    case SceneType::Cutscene:
                        target_mult = 1.10f;
                        if (std::abs(ws - 1.0f) > 0.05f) target_mult *= std::sqrt(ws);
                        target_mult = (std::max)(0.5f, (std::min)(target_mult, 2.0f));
                        break;
                    case SceneType::Menu:
                        target_mult = 0.90f;
                        if (std::abs(ws - 1.0f) > 0.05f) target_mult *= std::sqrt(ws);
                        target_mult = (std::max)(0.3f, (std::min)(target_mult, 2.0f));
                        break;
                    default: {
                        // v3.4: Depth-tracking convergence — mode-aware, ws-responsive.
                        //
                        // Convergence tracks depth_mult via pow(dm, blend) where blend
                        // varies by zoom mode and world scale:
                        //   ADS  (aim-critical):     blend ~0.85 → tight tracking, aim locked
                        //   Scope (comfort-critical): blend ~0.55 → allow flattening
                        //   None (passive zoom):     blend ~0.70 → balanced default
                        //
                        // Net eyeOffset ratio = pow(depth_mult, 1.0 - blend)
                        //   ADS at 2x:   pow(0.504, 0.15) = 0.905 → 90% eyeOffset
                        //   Scope at 2x:  pow(0.504, 0.45) = 0.726 → 73% eyeOffset
                        //   Scope at 8x:  pow(0.028, 0.45) = 0.157 → 16% (very flat)

                        float dm = get_depth_multiplier();
                        float fs = get_fov_scale();

                        bool depth_active = (dm < 0.99f);
                        bool fov_active = (fs > 0.01f && fs < 0.99f);

                        if (depth_active) {
                            // Dynamic blend based on zoom mode (shm offset 34)
                            // v3.4: Per-mode blends configurable via uevr_modifiers
                            uint8_t zm = get_zoom_mode();
                            float blend;
                            if (zm == 1) {        // ADS — aim is king
                                blend = (m_blend_ads > 0.0f) ? m_blend_ads : 0.85f;
                            } else if (zm == 2) { // Scope — comfort is king
                                blend = (m_blend_scope > 0.0f) ? m_blend_scope : 0.55f;
                            } else {              // Passive zoom or unknown
                                blend = (m_blend_passive > 0.0f) ? m_blend_passive : 0.70f;
                            }

                            // World-scale boost: log10(10)=1.0 → +0.10 boost
                            float ws_log = std::log10((std::max)(ws, 1.0f));
                            blend += ws_log * 0.08f;    // v3.5: reduced from 0.10 (over-correcting in high-ws games)
                            blend = (std::max)(0.40f, (std::min)(blend, 0.92f)); // v3.5: tightened from 0.95 (allow more flattening)

                            target_mult = std::pow(dm, blend);

                            // Zoom-gated ws geometry correction (zero at 1x zoom)
                            if (ws > 1.5f && fov_active) {
                                float zoom_intensity = 1.0f - fs;
                                float ws_correction = 1.0f + zoom_intensity * (std::sqrt(ws) - 1.0f) * 0.05f;
                                target_mult *= ws_correction;
                            }
                        } else if (fov_active) {
                            // Fallback: depth disabled but FOV shows zoom
                            target_mult = std::pow(fs, -0.08f); // v3.5: stronger fallback (from -0.05)
                        }

                        target_mult = (std::max)(0.05f, (std::min)(target_mult, 2.0f));
                        break;
                    }
                }
            }
        }
        // else: target_mult stays 1.0 (disabled or disconnected — smooth recovery)

        // v3.4: Asymmetric smoothing — reversed direction for depth-tracking.
        // Attacking = convergence DECREASING (zooming in, tracking depth down).
        // Release = convergence returning to base (zooming out, back to 1.0).
        bool conv_attacking = (target_mult < m_smoothed_conv_mult);
        float conv_rate = conv_attacking ? m_conv_attack_rate : m_conv_release_rate;
        float alpha = 1.0f - std::exp(-conv_rate * delta_time);
        alpha = (std::max)(0.0f, (std::min)(alpha, 1.0f));
        m_smoothed_conv_mult += alpha * (target_mult - m_smoothed_conv_mult);

        if (std::abs(m_smoothed_conv_mult - target_mult) < 0.001f) {
            m_smoothed_conv_mult = target_mult;
        }

        return m_base_convergence * m_smoothed_conv_mult;
    }

    /**
     * Get the current smoothed convergence multiplier (for debug display).
     */
    float get_smoothed_conv_multiplier() const {
        return m_smoothed_conv_mult;
    }

    /**
     * Set the VRto3D-side smoothing rate (Hz).
     * Higher = faster response. Default: 8 Hz.
     */
    void set_smooth_rate(float rate_hz) {
        m_smooth_rate = (std::max)(1.0f, rate_hz);
    }

    // ================================================================
    // v3.1: PROFILE MODIFIERS
    // ================================================================

    /**
     * Write profile modifiers to shared memory.
     * Called from hmd_device_driver when a profile with
     * "uevr_modifiers" is loaded.
     *
     * @param mods  Modifier struct from JSON parsing
     */
    void write_modifiers(const ProfileModifiers& mods) {
        if (!m_data) return;

        m_data->has_modifiers = mods.active ? 1 : 0;
        m_data->mod_depth_strength = mods.depth_strength;
        m_data->mod_depth_min_floor = mods.depth_min_floor;
        m_data->mod_ads_floor = mods.ads_floor;
        m_data->mod_scope_floor = mods.scope_floor;
        m_data->mod_cutscene_floor = mods.cutscene_floor;
        m_data->mod_base_power = mods.base_power;
        m_data->mod_extra_power = mods.extra_power;
        m_data->mod_dead_zone = mods.dead_zone;
        m_data->mod_transition_speed = mods.transition_speed;
        m_data->mod_zoom_threshold = mods.zoom_threshold;
        m_data->mod_base_fov_override = mods.base_fov_override;
        // v3.4: Store blend overrides locally (not in shared memory — VRto3D-only)
        m_blend_ads = mods.blend_ads;
        m_blend_scope = mods.blend_scope;
        m_blend_passive = mods.blend_passive;
    }

    /**
     * Clear all profile modifiers (e.g., when profile changes).
     */
    void clear_modifiers() {
        if (!m_data) return;

        m_data->has_modifiers = 0;
        m_data->mod_depth_strength = 0.0f;
        m_data->mod_depth_min_floor = 0.0f;
        m_data->mod_ads_floor = 0.0f;
        m_data->mod_scope_floor = 0.0f;
        m_data->mod_cutscene_floor = 0.0f;
        m_data->mod_base_power = 0.0f;
        m_data->mod_extra_power = 0.0f;
        m_data->mod_dead_zone = 0.0f;
        m_data->mod_transition_speed = 0.0f;
        m_data->mod_zoom_threshold = 0.0f;
        m_data->mod_base_fov_override = 0.0f;
        m_blend_ads = 0.0f;
        m_blend_scope = 0.0f;
        m_blend_passive = 0.0f;
    }

    // ================================================================
    // BASE VALUE MANAGEMENT
    // ================================================================

    void set_base_depth(float d) { m_base_depth = d; }
    float get_base_depth() const { return m_base_depth; }

    void set_base_convergence(float c) { m_base_convergence = c; }
    float get_base_convergence() const { return m_base_convergence; }

    // ================================================================
    // AUTO-CONVERGENCE TOGGLE (Ctrl+F2)
    // ================================================================

    void set_auto_convergence_enabled(bool enabled) { m_auto_convergence_enabled = enabled; }
    bool is_auto_convergence_enabled() const { return m_auto_convergence_enabled; }
    void toggle_auto_convergence() { m_auto_convergence_enabled = !m_auto_convergence_enabled; }

    // ================================================================
    // WORLD-SCALE AUTO-STEREO (for games without VRto3D profiles)
    // ================================================================

    /**
     * Calculate auto-depth/convergence from UEVR's world_to_meters for games
     * without a VRto3D profile. Targets eyeOffset ≈ 0.04 (center of 0.03-0.05
     * comfort zone derived from 136+ hand-tuned profiles).
     *
     * @param out_depth       Calculated depth value
     * @param out_convergence Calculated convergence value
     * @return true if valid world_scale data was available
     */
    bool calculate_auto_stereo(float& out_depth, float& out_convergence) const {
        if (!has_valid_data()) return false;

        float ws = get_world_scale();
        if (ws < 0.1f) return false;  // Invalid/zero world scale

        // Scale factor relative to standard UE world_to_meters (100)
        // Target eyeOffset = 0.04 → with conv=1.0: depth = 0.08 at standard scale
        float scale_factor = ws / 100.0f;
        out_depth = (std::max)(0.02f, (std::min)(0.08f * scale_factor, 0.50f));
        out_convergence = 1.0f;  // Conservative — works well across games

        return true;
    }

    // ================================================================
    // UEVR DEPTH COMMAND HELPERS
    // ================================================================

    /**
     * v3.2: Sequence-aware command retrieval.
     * Only returns a command if command_seq differs from last seen.
     * This prevents lost commands when UEVR's next update_all()
     * overwrites auto_depth_request before VRto3D polls it.
     */
    uint8_t get_depth_request() const {
        if (!has_valid_data() || !m_data) return 0;
        uint8_t cmd = m_data->auto_depth_request;
        if (cmd < 2) return 0;  // 0=none, 1=auto-depth flag, not a command

        // v3.2: Check sequence number for reliable delivery
        uint32_t seq = m_data->command_seq;
        if (seq != 0 && seq == m_last_command_seq) return 0;  // Already processed

        return cmd;
    }

    void clear_depth_request() {
        if (m_data) {
            m_last_command_seq = m_data->command_seq;  // Mark as processed
            m_data->auto_depth_request = 0;
        }
    }

    // ================================================================
    // AUTO-DEPTH TOGGLE (Ctrl+F1)
    // ================================================================

    void set_auto_depth_enabled(bool enabled) {
        m_auto_depth_enabled = enabled;
        if (m_data) {
            m_data->vrto3d_listener_enabled = enabled ? 1 : 0;
            m_data->vrto3d_auto_depth_active =
                (enabled && is_requesting_depth_change()) ? 1 : 0;
        }
    }

    bool is_auto_depth_enabled() const { return m_auto_depth_enabled; }

    void toggle_auto_depth() {
        set_auto_depth_enabled(!m_auto_depth_enabled);
    }

    void mark_depth_active(bool active) {
        if (m_data) {
            m_data->vrto3d_auto_depth_active = active ? 1 : 0;
        }
    }

private:
    Receiver() = default;
    ~Receiver() { shutdown(); }
    Receiver(const Receiver&) = delete;
    Receiver& operator=(const Receiver&) = delete;

    void cleanup() {
        if (m_data) {
            UnmapViewOfFile(m_data);
            m_data = nullptr;
        }
        if (m_mapping) {
            CloseHandle(m_mapping);
            m_mapping = nullptr;
        }
    }

    HANDLE m_mapping = nullptr;
    SharedData* m_data = nullptr;

    // VRto3D's authoritative values
    float m_base_depth = 0.4f;
    float m_base_convergence = 4.0f;

    // v3.1: VRto3D-side smoothing (unified rate for both depth & convergence)
    float m_smoothed_multiplier = 1.0f;
    float m_smooth_rate = 8.0f;  // Hz (8 = ~0.4s time constant — synchronized)

    // v3.3: Convergence smoothing — asymmetric rates synced with depth smoother
    float m_smoothed_conv_mult = 1.0f;
    float m_conv_attack_rate = 12.0f;   // Hz — entering zoom (synced with depth attack)
    float m_conv_release_rate = 10.0f;  // Hz — exiting zoom (synced with depth release)

    // v3.2: Command sequence tracking for reliable delivery
    mutable uint32_t m_last_command_seq = 0;

    // Feature toggles
    bool m_auto_depth_enabled = true;        // Ctrl+F1 state
    bool m_auto_convergence_enabled = true;  // Ctrl+F2 state

    // v3.4: Per-mode convergence blend overrides (from profile modifiers)
    float m_blend_ads = 0.0f;       // 0 = use default 0.85
    float m_blend_scope = 0.0f;     // 0 = use default 0.55
    float m_blend_passive = 0.0f;   // 0 = use default 0.70

    // v3.4: Protocol mismatch tracking (for caller to log)
    uint32_t m_last_magic_mismatch = 0;
};

// Convenience function
inline Receiver& receiver() {
    return Receiver::instance();
}

} // namespace uevr
