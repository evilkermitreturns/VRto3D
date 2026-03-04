/*
 * This file is part of VRto3D.
 *
 * VRto3D is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * VRto3D is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with VRto3D. If not, see <http://www.gnu.org/licenses/>.
 */

#define WIN32_LEAN_AND_MEAN
#include "hmd_device_driver.h"
#include "vrto3dlib/key_mappings.h"
#include "vrto3dlib/json_manager.h"
#include "vrto3dlib/app_id_mgr.h"
#include "vrto3dlib/overlay_mgr.h"
#include "vrto3dlib/win32_helper.hpp"
#include "driverlog.h"
#include "vrmath.h"

#include <string>
#include <sstream>
#include <ctime>

#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment (lib, "WSock32.Lib")
#include <windows.h>
#include <xinput.h>
#include <fstream>
#include <Shlobj.h>
#include <nlohmann/json.hpp>


// Load settings from default.vrsettings
static const char *stereo_main_settings_section = "driver_vrto3d";

MockControllerDeviceDriver::MockControllerDeviceDriver()
{
    // Keep track of whether Activate() has been called
    is_active_ = false;
    curr_pose_ = { 0 };
    app_name_ = "";
    prev_name_ = "";
    app_pid_ = 0;

    auto* vrs = vr::VRSettings();
    JsonManager json_manager;
    json_manager.EnsureDefaultConfigExists();

    char model_number[ 1024 ];
    vrs->GetString( stereo_main_settings_section, "model_number", model_number, sizeof( model_number ) );
    stereo_model_number_ = model_number;
    char serial_number[ 1024 ];
    vrs->GetString( stereo_main_settings_section, "serial_number", serial_number, sizeof( serial_number ) );
    stereo_serial_number_ = serial_number;

    DriverLog( "VRto3D Model Number: %s", stereo_model_number_.c_str() );
    DriverLog( "VRto3D Serial Number: %s", stereo_serial_number_.c_str() );

    SwitchToXinpuGetStateEx();

    // Display settings
    StereoDisplayDriverConfiguration display_configuration{};
    display_configuration.window_x = 0;
    display_configuration.window_y = 0;
    json_manager.LoadParamsFromJson(display_configuration);

    // Profile settings
    json_manager.LoadProfileFromJson(DEF_CFG, display_configuration);

    // Instantiate our display component
    stereo_display_component_ = std::make_unique< StereoDisplayComponent >( display_configuration );

    DriverLog("Default Config Loaded\n");
}


//-----------------------------------------------------------------------------
// Purpose: Initialize all settings and notify SteamVR
//-----------------------------------------------------------------------------
vr::EVRInitError MockControllerDeviceDriver::Activate( uint32_t unObjectId )
{
    device_index_ = unObjectId;
    is_active_ = true;
    is_on_top_ = false;
    man_on_top_ = false;
    take_screenshot_ = false;
    use_auto_depth_ = true;
    app_updated_ = false;
    no_profile_ = false;

    stereo_display_component_->Init(device_index_);

    // A list of properties available is contained in vr::ETrackedDeviceProperty.
    auto* vrp = vr::VRProperties();
    auto* vrs = vr::VRSettings();
    vr::PropertyContainerHandle_t container = vrp->TrackedDeviceToPropertyContainer( device_index_ );
    vrp->SetStringProperty( container, vr::Prop_ModelNumber_String, stereo_model_number_.c_str() );
    vrp->SetStringProperty( container, vr::Prop_ManufacturerName_String, "VRto3D");
    vrp->SetStringProperty( container, vr::Prop_TrackingFirmwareVersion_String, "1.0");
    vrp->SetStringProperty( container, vr::Prop_HardwareRevision_String, "1.0");

    // Display settings
    vrp->SetFloatProperty( container, vr::Prop_UserIpdMeters_Float, stereo_display_component_->GetConfig().depth);
    vrp->SetFloatProperty( container, vr::Prop_UserHeadToEyeDepthMeters_Float, 0.f);
    vrp->SetFloatProperty(container, vr::Prop_DisplayFrequency_Float, stereo_display_component_->GetConfig().display_frequency);
    vrp->SetFloatProperty( container, vr::Prop_SecondsFromVsyncToPhotons_Float, stereo_display_component_->GetConfig().display_latency);
    vrp->SetFloatProperty( container, vr::Prop_SecondsFromPhotonsToVblank_Float, 0.0);
    vrp->SetBoolProperty( container, vr::Prop_ReportsTimeSinceVSync_Bool, false);
    vrp->SetBoolProperty( container, vr::Prop_IsOnDesktop_Bool, false);
    vrp->SetBoolProperty( container, vr::Prop_DisplayDebugMode_Bool, true);
    vrp->SetBoolProperty( container, vr::Prop_HasDriverDirectModeComponent_Bool, false);
    if (stereo_display_component_->GetConfig().dash_enable)
    {
        vrp->SetFloatProperty(container, vr::Prop_DashboardScale_Float, 1.0f);
    }
    else
    {
        vrp->SetFloatProperty(container, vr::Prop_DashboardScale_Float, 0.0f);
    }

    // Set the chaperone JSON property
    // Get the current time
    std::time_t t = std::time(nullptr);
    std::tm tm;
    localtime_s(&tm, &t);
    // Construct the JSON string with variables
    std::stringstream ss;
    ss << R"(
        {
           "jsonid" : "chaperone_info",
           "universes" : [
              {
                 "collision_bounds" : [
                    [
                       [ -1.0, 0.0, -1.0 ],
                       [ -1.0, 3.0, -1.0 ],
                       [ -1.0, 3.0, 1.0 ],
                       [ -1.0, 0.0, 1.0 ]
                    ],
                    [
                       [ -1.0, 0.0, 1.0 ],
                       [ -1.0, 3.0, 1.0 ],
                       [ 1.0, 3.0, 1.0 ],
                       [ 1.0, 0.0, 1.0 ]
                    ],
                    [
                       [ 1.0, 0.0, 1.0 ],
                       [ 1.0, 3.0, 1.0 ],
                       [ 1.0, 3.0, -1.0 ],
                       [ 1.0, 0.0, -1.0 ]
                    ],
                    [
                       [ 1.0, 0.0, -1.0 ],
                       [ 1.0, 3.0, -1.0 ],
                       [ -1.0, 3.0, -1.0 ],
                       [ -1.0, 0.0, -1.0 ]
                    ]
                 ],
                 "play_area" : [ 2.0, 2.0 ],
                 "seated" : {
                    "translation" : [ 0.0, 0.5, 0.0 ],
                    "yaw" : 0.0
                 },
                 "standing" : {
                    "translation" : [ 0.0, 1.0, 0.0 ],
                    "yaw" : 0.0
                 },
                 "time" : ")" << std::put_time(&tm, "%a %b %d %H:%M:%S %Y") << R"(",
                 "universeID" : "64"
              }
           ],
           "version" : 5
        }
        )";
    // Convert the stringstream to a string
    std::string chaperoneJson = ss.str();
    // Set the chaperone JSON property
    vrp->SetStringProperty(container, vr::Prop_DriverProvidedChaperoneJson_String, chaperoneJson.c_str());
    vrp->SetUint64Property(container, vr::Prop_CurrentUniverseId_Uint64, 64);
    vrs->SetInt32(vr::k_pch_CollisionBounds_Section, vr::k_pch_CollisionBounds_Style_Int32, vr::COLLISION_BOUNDS_STYLE_NONE);
    vrs->SetBool(vr::k_pch_CollisionBounds_Section, vr::k_pch_CollisionBounds_GroundPerimeterOn_Bool, false);

    // Miscellaneous settings
    vrp->SetBoolProperty( container, vr::Prop_WillDriftInYaw_Bool, false);
    vrp->SetBoolProperty( container, vr::Prop_DeviceIsWireless_Bool, false);
    vrp->SetBoolProperty( container, vr::Prop_DeviceIsCharging_Bool, false);
    vrp->SetBoolProperty( container, vr::Prop_ContainsProximitySensor_Bool, false);
    vrp->SetBoolProperty( container, vr::Prop_DeviceCanPowerOff_Bool, false);

    // set proximity senser to always on, always head present
    vr::VRInputComponentHandle_t  prox;
    vr::VRDriverInput()->CreateBooleanComponent(container, "/proximity", &prox);
    vr::VRDriverInput()->UpdateBooleanComponent(prox, true, 0.0);
    
    // Miscellaneous settings
    vrs->SetBool(vr::k_pch_DirectMode_Section, vr::k_pch_DirectMode_Enable_Bool, false);
    vrs->SetFloat(vr::k_pch_Power_Section, vr::k_pch_Power_TurnOffScreensTimeout_Float, 86400.0f);
    vrs->SetBool(vr::k_pch_Power_Section, vr::k_pch_Power_PauseCompositorOnStandby_Bool, false);
    vrs->SetBool(vr::k_pch_Dashboard_Section, vr::k_pch_Dashboard_EnableDashboard_Bool, stereo_display_component_->GetConfig().dash_enable);
    vrs->SetBool(vr::k_pch_Dashboard_Section, vr::k_pch_Dashboard_ArcadeMode_Bool, !stereo_display_component_->GetConfig().dash_enable);
    vrs->SetBool(vr::k_pch_Dashboard_Section, "allowAppQuitting", stereo_display_component_->GetConfig().dash_enable);
    vrs->SetBool(vr::k_pch_Dashboard_Section, "autoShowGameTheater", false);
    vrs->SetBool(vr::k_pch_Dashboard_Section, "showDesktop", false);
    vrs->SetBool(vr::k_pch_Dashboard_Section, "showPowerOptions", false);
    vrs->SetBool(vr::k_pch_Dashboard_Section, "inputCaptureEnabled", false);
    vrs->SetBool(vr::k_pch_SteamVR_Section, vr::k_pch_SteamVR_EnableHomeApp, stereo_display_component_->GetConfig().dash_enable);
    vrs->SetBool(vr::k_pch_SteamVR_Section, vr::k_pch_SteamVR_MirrorViewVisibility_Bool, false);
    vrs->SetBool(vr::k_pch_SteamVR_Section, vr::k_pch_SteamVR_EnableSafeMode, false);
    vrs->SetBool(vr::k_pch_SteamVR_Section, vr::k_pch_SteamVR_DisplayDebug_Bool, false);
    vrs->SetBool(vr::k_pch_SteamVR_Section, vr::k_pch_SteamVR_MotionSmoothing_Bool, false);
    vrs->SetBool(vr::k_pch_SteamVR_Section, vr::k_pch_SteamVR_DisableAsyncReprojection_Bool, !stereo_display_component_->GetConfig().async_enable);
    vrs->SetBool(vr::k_pch_SteamVR_Section, vr::k_pch_SteamVR_AllowSupersampleFiltering_Bool, false);
    vrs->SetBool(vr::k_pch_SteamVR_Section, vr::k_pch_SteamVR_SupersampleManualOverride_Bool, true);
    vrs->SetBool(vr::k_pch_SteamVR_Section, vr::k_pch_SteamVR_ForceFadeOnBadTracking_Bool, false);
    vrs->SetBool(vr::k_pch_SteamVR_Section, vr::k_pch_SteamVR_ActivateMultipleDrivers_Bool, true);
    vrs->SetString(vr::k_pch_SteamVR_Section, vr::k_pch_SteamVR_ForcedDriverKey_String, "vrto3d");
    
    // Thread setup
    pose_thread_ = std::thread(&MockControllerDeviceDriver::PoseUpdateThread, this);
    hotkey_thread_ = std::thread(&MockControllerDeviceDriver::PollHotkeysThread, this);
    focus_thread_ = std::thread(&MockControllerDeviceDriver::FocusUpdateThread, this);
    depth_thread_ = std::thread(&MockControllerDeviceDriver::AutoDepthThread, this);
    if (stereo_display_component_->GetConfig().use_open_track) {
        open_track_att_ = HmdQuaternion_Identity;
        track_thread_ = std::thread(&MockControllerDeviceDriver::OpenTrackThread, this);
    }

    HANDLE thread_handle = pose_thread_.native_handle();

    // Set the thread priority
    if (!SetThreadPriority(thread_handle, THREAD_PRIORITY_HIGHEST)) {
        // Handle error if setting priority fails
        DriverLog("Failed to set thread priority: %d\n", GetLastError());
    }

    DriverLog("Activation Complete\n");

    return vr::VRInitError_None;
}

//-----------------------------------------------------------------------------
// Purpose: Return StereoDisplayComponent as vr::IVRDisplayComponent
//-----------------------------------------------------------------------------
void *MockControllerDeviceDriver::GetComponent( const char *pchComponentNameAndVersion )
{
    if ( strcmp( pchComponentNameAndVersion, vr::IVRDisplayComponent_Version ) == 0 )
    {
        return stereo_display_component_.get();
    }

    return nullptr;
}

//-----------------------------------------------------------------------------
// Purpose: This is called by vrserver when a debug request has been made from an application to the driver.
// What is in the response and request is up to the application and driver to figure out themselves.
//-----------------------------------------------------------------------------
void MockControllerDeviceDriver::DebugRequest( const char *pchRequest, char *pchResponseBuffer, uint32_t unResponseBufferSize )
{
    if ( unResponseBufferSize >= 1 )
        pchResponseBuffer[ 0 ] = 0;
}


//-----------------------------------------------------------------------------
// Purpose: Receive OpenTrack 3DoF updates
//-----------------------------------------------------------------------------
void MockControllerDeviceDriver::OpenTrackThread()
{
    SOCKET socket_s;
    struct sockaddr_in from = {};
    int from_len = sizeof(from);
    struct TOpenTrack {
        double X;
        double Y;
        double Z;
        double Yaw;
        double Pitch;
        double Roll;
    };
    TOpenTrack open_track;
    auto ot_port = stereo_display_component_->GetConfig().open_track_port;

    WSADATA wsaData;
    int iResult = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (iResult != 0) {
        DriverLog("WSAStartup failed: %d", iResult);
    }
    else {
        struct sockaddr_in local = {};
        local.sin_family = AF_INET;
        local.sin_port = htons(ot_port);
        local.sin_addr.s_addr = INADDR_ANY;

        socket_s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (socket_s == INVALID_SOCKET) {
            DriverLog("Socket creation failed: %d", WSAGetLastError());
            WSACleanup();  // Cleanup only if socket creation fails
        }
        else {
            // Set non-blocking mode
            u_long nonblocking_enabled = 1;
            if (ioctlsocket(socket_s, FIONBIO, &nonblocking_enabled) == SOCKET_ERROR) {
                DriverLog("Failed to set non-blocking mode: %d", WSAGetLastError());
                closesocket(socket_s);
                WSACleanup();
            }
            else if (bind(socket_s, (struct sockaddr*)&local, sizeof(local)) == SOCKET_ERROR) {
                DriverLog("Bind failed: %d", WSAGetLastError());
                closesocket(socket_s);
                WSACleanup();
            }
        }
    }

    while (is_active_) {
        //Read UDP socket with OpenTrack data
        memset(&open_track, 0, sizeof(open_track));
        auto bytes_read = recvfrom(socket_s, (char*)(&open_track), sizeof(open_track), 0, (sockaddr*)&from, &from_len);

        if (bytes_read > 0) {
            std::unique_lock<std::shared_mutex> lock(trk_mutex_);
            open_track_att_ = HmdQuaternion_FromEulerAngles(DEG_TO_RAD(open_track.Roll), DEG_TO_RAD(open_track.Pitch), DEG_TO_RAD(open_track.Yaw));
            lock.unlock();
        }
        else std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    closesocket(socket_s);
    WSACleanup();
}


//-----------------------------------------------------------------------------
// Purpose: Static Pose with pitch & yaw adjustment
//-----------------------------------------------------------------------------
void MockControllerDeviceDriver::PoseUpdateThread()
{
    static float currentPitch = 0.0f; // Keep track of the current pitch
    static vr::HmdQuaternion_t currentYawQuat = { 1.0f, 0.0f, 0.0f, 0.0f }; // Initial yaw quaternion
    static float lastPitch = 0.0f;
    static float lastYaw = 0.0f;
    static float lastHmdYaw = 0.0f;
    static vr::DriverPose_t lastPose = { 0 };

    auto lastTime = std::chrono::high_resolution_clock::now();

    while (is_active_)
    {
        // Calculate delta time FIRST (needed for v3.1 depth smoothing)
        auto currentTime = std::chrono::high_resolution_clock::now();
        auto deltaTime = std::chrono::duration_cast<std::chrono::duration<float>>(currentTime - lastTime).count();
        lastTime = currentTime;

        // === UEVR BRIDGE UPDATE ===
        if (stereo_display_component_) {
            static int uevr_log_counter = 0;
            static bool uevr_was_connected = false;

            float current_depth = stereo_display_component_->GetDepth();
            float current_convergence = stereo_display_component_->GetConvergence();
            float current_fov = stereo_display_component_->GetFoV();

            // v3.4: FOV compensation moved AFTER new_sep and new_conv are computed (see below)

            bool connected = uevr::receiver().is_connected();
            if (connected != uevr_was_connected) {
                DriverLog("UEVR Bridge: %s\n", connected ? "CONNECTED" : "DISCONNECTED");
                // v3.4: Log protocol mismatch if connection failed due to wrong magic
                if (!connected) {
                    uint32_t mismatch = uevr::receiver().get_last_magic_mismatch();
                    if (mismatch != 0) {
                        DriverLog("UEVR protocol mismatch: expected 0x%08X, got 0x%08X. Check UEVR build.\n",
                            UEVR_VRTO3D_MAGIC, mismatch);
                    }
                }
                uevr_was_connected = connected;
            }

            // === AUTO-DEPTH (unified — no hard threshold switch) ===
            // Always use smoothed depth path to prevent compositor jolt
            // on threshold crossing when depth_multiplier approaches 1.0.
            float new_sep;

            if (uevr::receiver().is_connected() && uevr::receiver().has_valid_data()) {
                // Always use smoothed path — even when returning to 1.0
                new_sep = uevr::receiver().get_depth_for_rendering(deltaTime);

                bool depth_active = std::abs(uevr::receiver().get_depth_multiplier() - 1.0f) > 0.01f;
                uevr::receiver().mark_depth_active(depth_active);

                // v3.4: Log depth state transitions (active/inactive)
                static bool prev_depth_active = false;
                if (depth_active != prev_depth_active) {
                    DriverLog("UEVR depth: %s (eff=%.4f mult=%.3f)\n",
                        depth_active ? "ACTIVE" : "INACTIVE",
                        new_sep, uevr::receiver().get_depth_multiplier());
                    prev_depth_active = depth_active;
                }

                if (depth_active && uevr_log_counter++ % 200 == 0) {
                    DriverLog("UEVR depth: eff=%.4f base=%.4f mult=%.3f\n",
                        new_sep, current_depth, uevr::receiver().get_depth_multiplier());
                }
            } else {
                new_sep = current_depth;
            }

            stereo_display_component_->SetUEVREffectiveDepth(new_sep);

            // Monitor mode: latch on valid data, don't revert on stale (lesson 20)
            if (uevr::receiver().is_connected() && uevr::receiver().has_valid_data()) {
                stereo_display_component_->SetMonitorMode(uevr::receiver().get_monitor_mode());

                // v3.3: Log stereo depth hint — first occurrence + periodic refresh (~30s)
                static bool hint_logged = false;
                static int hint_log_cooldown = 0;
                float hint = uevr::receiver().get_stereo_depth_hint();
                if (hint > 0.001f) {
                    if (!hint_logged || ++hint_log_cooldown >= 2700) { // ~30s at 90fps
                        float prop_ipd = stereo_display_component_->GetConfig().depth;
                        DriverLog("UEVR stereo_depth_hint: %.4f  config.depth: %.4f  Prop_IPD: %.4f  ratio: %.2fx\n",
                            hint, current_depth, prop_ipd, hint / (std::max)(current_depth, 0.001f));
                        hint_logged = true;
                        hint_log_cooldown = 0;
                    }
                }
            }
            // === END AUTO-DEPTH ===

            // === DEFERRED AUTO-DEPTH FOR NO-PROFILE GAMES ===
            static bool auto_depth_applied = false;
            if (no_profile_.load() && !auto_depth_applied && uevr::receiver().is_connected()) {
                float ad = 0.0f, ac = 0.0f;
                if (uevr::receiver().calculate_auto_stereo(ad, ac)) {
                    auto cfg = stereo_display_component_->GetConfig();
                    cfg.depth = ad;
                    cfg.convergence = ac;
                    stereo_display_component_->LoadSettings(cfg);
                    DriverLog("Deferred auto-stereo: d=%.4f c=%.2f ws=%.1f\n",
                        ad, ac, uevr::receiver().get_world_scale());
                    BeepSuccess();
                    app_updated_ = true;
                    no_profile_ = false;
                    auto_depth_applied = true;
                }
            }
            if (app_updated_.load()) auto_depth_applied = false;
            // === END DEFERRED AUTO-DEPTH ===

            // === UEVR DEPTH COMMANDS (Calibrate, +3D, -3D) ===
            uint8_t depth_cmd = uevr::receiver().get_depth_request();
            if (depth_cmd >= 2 && depth_cmd <= 9) {
                if (depth_cmd == 2) {  // Calibrate from world_scale
                    float ad = 0.0f, ac = 0.0f;
                    if (uevr::receiver().calculate_auto_stereo(ad, ac)) {
                        auto cfg = stereo_display_component_->GetConfig();
                        cfg.depth = ad;
                        cfg.convergence = ac;
                        stereo_display_component_->LoadSettings(cfg);
                        DriverLog("Calibrate 3D: d=%.4f c=%.2f ws=%.1f\n",
                            ad, ac, uevr::receiver().get_world_scale());
                        BeepSuccess();
                        app_updated_ = true;
                    }
                } else if (depth_cmd == 3) {  // -3D: Decrease depth 20%
                    float new_d = stereo_display_component_->GetDepth() * 0.8f;
                    new_d = (std::max)(0.005f, new_d);
                    stereo_display_component_->AdjustDepth(new_d, false);
                    uevr::receiver().set_base_depth(new_d);
                    BeepSuccess();
                    app_updated_ = true;
                } else if (depth_cmd == 4) {  // +3D: Increase depth 20%
                    float new_d = stereo_display_component_->GetDepth() * 1.2f;
                    new_d = (std::min)(1.0f, new_d);
                    stereo_display_component_->AdjustDepth(new_d, false);
                    uevr::receiver().set_base_depth(new_d);
                    BeepSuccess();
                    app_updated_ = true;
                } else if (depth_cmd == 5) {  // --3D: Big decrease depth 40%
                    float new_d = stereo_display_component_->GetDepth() * 0.6f;
                    new_d = (std::max)(0.005f, new_d);
                    stereo_display_component_->AdjustDepth(new_d, false);
                    uevr::receiver().set_base_depth(new_d);
                    BeepSuccess();
                    app_updated_ = true;
                } else if (depth_cmd == 6) {  // ++3D: Big increase depth 40%
                    float new_d = stereo_display_component_->GetDepth() * 1.4f;
                    new_d = (std::min)(1.0f, new_d);
                    stereo_display_component_->AdjustDepth(new_d, false);
                    uevr::receiver().set_base_depth(new_d);
                    BeepSuccess();
                    app_updated_ = true;
                } else if (depth_cmd == 8) {  // ---3D: Huge decrease depth 60%
                    float new_d = stereo_display_component_->GetDepth() * 0.4f;
                    new_d = (std::max)(0.005f, new_d);
                    stereo_display_component_->AdjustDepth(new_d, false);
                    uevr::receiver().set_base_depth(new_d);
                    BeepSuccess();
                    app_updated_ = true;
                } else if (depth_cmd == 9) {  // +++3D: Huge increase depth 60%
                    float new_d = stereo_display_component_->GetDepth() * 1.6f;
                    new_d = (std::min)(1.0f, new_d);
                    stereo_display_component_->AdjustDepth(new_d, false);
                    uevr::receiver().set_base_depth(new_d);
                    BeepSuccess();
                    app_updated_ = true;
                }
                uevr::receiver().clear_depth_request();
            }
            // === END UEVR DEPTH COMMANDS ===

            // === SETTLED-STATE PROJECTION UPDATES ===
            //
            // GetProjectionRaw() reads depth/convergence atomics directly,
            // so smooth per-frame rendering happens WITHOUT ResetProjection().
            //
            // ResetProjection() is a heavyweight compositor re-sync that
            // fires VREvent_LensDistortionChanged. Calling it during active
            // transitions causes visible stepping (the v3.1 jitter source).
            //
            // NEW APPROACH: Fire ResetProjection ONLY when values SETTLE:
            //   (a) First time — always fire
            //   (b) Big jump >20% — immediate fire
            //   (c) Settled — both depth+conv < 0.1% change for 10 frames
            //   (d) Staleness safety — if committed drifts >5% for 2s, force
            //
            // Atomics are updated EVERY frame for smooth per-frame rendering.
            // ResetProjection only commits metadata when transition is done.
            //
            static float committed_sep = -1.0f;
            static float committed_conv = -1.0f;
            static int settled_frames = 0;
            static float prev_frame_sep = -1.0f;
            static float prev_frame_conv = -1.0f;
            static auto stale_timer_start = std::chrono::steady_clock::now();
            static bool stale_timer_active = false;

            constexpr float SETTLED_THRESHOLD = 0.001f;     // <0.1% frame-over-frame = settled
            constexpr int   SETTLED_FRAME_COUNT = 10;        // 10 frames of stability
            constexpr float BIG_JUMP_THRESHOLD = 0.20f;      // >20% = immediate fire
            constexpr float STALE_DRIFT_THRESHOLD = 0.05f;   // >5% drift from committed
            constexpr auto  STALE_TIMEOUT = std::chrono::seconds(2);

            // === SCENE-AWARE AUTO-CONVERGENCE ===
            float new_conv = uevr::receiver().get_convergence_for_rendering(deltaTime);
            stereo_display_component_->SetUEVREffectiveConvergence(new_conv);
            stereo_display_component_->SetAimCorrection(uevr::receiver().get_stereo_aim_correction());
            stereo_display_component_->SetAimBase(uevr::receiver().get_stereo_aim_base());

            // v3.4: Convergence debug log (~every 200 frames when active)
            {
                float conv_mult = uevr::receiver().get_smoothed_conv_multiplier();
                bool conv_active = std::abs(conv_mult - 1.0f) > 0.01f;
                static uint32_t conv_log_counter = 0;
                if (conv_active && conv_log_counter++ % 200 == 0) {
                    DriverLog("UEVR conv: eff=%.2f mult=%.3f dm=%.3f zm=%d fs=%.3f ws=%.1f\n",
                        new_conv, conv_mult,
                        uevr::receiver().get_depth_multiplier(),
                        uevr::receiver().get_zoom_mode(),
                        uevr::receiver().get_fov_scale(),
                        uevr::receiver().get_world_scale());
                }
            }

            // v3.4: FOV compensation using ACTUAL effective values (moved from before new_sep/new_conv were available)
            float fov_adj = 0.0f;
            if (current_convergence > 0.001f && new_conv > 0.001f) {
                float base_offset = current_depth * 0.5f / current_convergence;
                float eff_offset = new_sep * 0.5f / new_conv;
                float delta = eff_offset - base_offset;
                if (std::abs(delta) > 0.0001f) {
                    fov_adj = 2.0f * std::atan(std::abs(delta)) * (180.0f / 3.14159265f);
                    if (delta < 0.0f) fov_adj = -fov_adj;  // Narrower offset = narrower FOV
                }
            }

            uevr::receiver().update(
                current_depth,
                current_convergence,
                current_fov,
                fov_adj,
                stereo_display_component_->GetConfig().tab_enable ? 0 : 1,
                !no_profile_.load()
            );

            bool need_projection_reset = false;

            // First-time initialization
            if (committed_sep < 0.0f) {
                stereo_display_component_->SetUEVREffectiveIPD(new_sep);
                committed_sep = new_sep;
                committed_conv = new_conv;
                prev_frame_sep = new_sep;
                prev_frame_conv = new_conv;
                need_projection_reset = true;
            } else {
                // Check for big jump (immediate fire)
                float rel_sep = (committed_sep > 0.0f) ? std::abs(new_sep - committed_sep) / committed_sep : 0.0f;
                float rel_conv = (committed_conv > 0.0f) ? std::abs(new_conv - committed_conv) / committed_conv : 0.0f;

                if (rel_sep > BIG_JUMP_THRESHOLD || rel_conv > BIG_JUMP_THRESHOLD) {
                    stereo_display_component_->SetUEVREffectiveIPD(new_sep);
                    committed_sep = new_sep;
                    committed_conv = new_conv;
                    need_projection_reset = true;
                    settled_frames = 0;
                    stale_timer_active = false;
                } else {
                    // Check frame-over-frame stability
                    float frame_delta_sep = (prev_frame_sep > 0.0f) ? std::abs(new_sep - prev_frame_sep) / prev_frame_sep : 0.0f;
                    float frame_delta_conv = (prev_frame_conv > 0.0f) ? std::abs(new_conv - prev_frame_conv) / prev_frame_conv : 0.0f;

                    if (frame_delta_sep < SETTLED_THRESHOLD && frame_delta_conv < SETTLED_THRESHOLD) {
                        settled_frames++;
                    } else {
                        settled_frames = 0;
                    }

                    // Settled: both stable for N frames AND committed value has drifted
                    if (settled_frames >= SETTLED_FRAME_COUNT) {
                        float drift_sep = (committed_sep > 0.0f) ? std::abs(new_sep - committed_sep) / committed_sep : 0.0f;
                        float drift_conv = (committed_conv > 0.0f) ? std::abs(new_conv - committed_conv) / committed_conv : 0.0f;

                        if (drift_sep > SETTLED_THRESHOLD || drift_conv > SETTLED_THRESHOLD) {
                            stereo_display_component_->SetUEVREffectiveIPD(new_sep);
                            committed_sep = new_sep;
                            committed_conv = new_conv;
                            need_projection_reset = true;
                            stale_timer_active = false;
                        }
                        settled_frames = 0;  // Reset after check
                    }

                    // Staleness safety: if committed value drifts >5% for >2s, force commit
                    float stale_drift = (std::max)(rel_sep, rel_conv);
                    if (stale_drift > STALE_DRIFT_THRESHOLD) {
                        if (!stale_timer_active) {
                            stale_timer_start = std::chrono::steady_clock::now();
                            stale_timer_active = true;
                        } else {
                            auto elapsed = std::chrono::steady_clock::now() - stale_timer_start;
                            if (elapsed >= STALE_TIMEOUT) {
                                stereo_display_component_->SetUEVREffectiveIPD(new_sep);
                                committed_sep = new_sep;
                                committed_conv = new_conv;
                                need_projection_reset = true;
                                stale_timer_active = false;
                                settled_frames = 0;
                            }
                        }
                    } else {
                        stale_timer_active = false;
                    }
                }
            }

            prev_frame_sep = new_sep;
            prev_frame_conv = new_conv;

            if (need_projection_reset) {
                stereo_display_component_->ResetProjection();
            }

            // v3.4: ResetProjection frequency diagnostics
            static uint32_t reset_proj_count = 0;
            static uint32_t diag_frame_count = 0;
            if (need_projection_reset) reset_proj_count++;
            if (++diag_frame_count % 1000 == 0) {
                if (reset_proj_count > 0) {
                    DriverLog("ResetProjection: %u in last 1000 frames (%.1f%%)\n",
                        reset_proj_count, reset_proj_count * 0.1f);
                }
                reset_proj_count = 0;
            }
        }
        // === END UEVR BRIDGE UPDATE ===

        XINPUT_STATE state;
        ZeroMemory(&state, sizeof(XINPUT_STATE));
        bool got_xinput = (_XInputGetState(0, &state) == ERROR_SUCCESS);

        auto config = stereo_display_component_->GetConfig();

        vr::DriverPose_t pose = { 0 };

        pose.qWorldFromDriverRotation = HmdQuaternion_Identity;
        pose.qDriverFromHeadRotation = HmdQuaternion_Identity;
        pose.qRotation = HmdQuaternion_Identity;

        // Adjust pitch based on controller input
        if (config.pitch_enable && got_xinput)
        {
            float normalizedY = state.Gamepad.sThumbRY / 32767.0f;

            // Apply deadzone
            if (std::abs(normalizedY) < config.ctrl_deadzone)
            {
                normalizedY = 0.0f;
            }
            else
            {
                if (normalizedY > 0)
                    normalizedY = (normalizedY - config.ctrl_deadzone) / (1.0f - config.ctrl_deadzone);
                else
                    normalizedY = (normalizedY + config.ctrl_deadzone) / (1.0f - config.ctrl_deadzone);
            }

            // Scale Pitch
            currentPitch += (normalizedY * config.ctrl_sensitivity);
            if (currentPitch > 90.0f) currentPitch = 90.0f;
            if (currentPitch < -90.0f) currentPitch = -90.0f;
        }

        // Apply yaw offset if applicable
        if (config.hmd_yaw != lastHmdYaw)
        {
            currentYawQuat = QuaternionFromAxisAngle(0.0f, 1.0f, 0.0f, DEG_TO_RAD(config.hmd_yaw));
            lastHmdYaw = config.hmd_yaw;
        }

        // Adjust yaw based on controller input
        if (config.yaw_enable && got_xinput)
        {
            float normalizedX = state.Gamepad.sThumbRX / 32767.0f;

            // Apply deadzone
            if (std::abs(normalizedX) < config.ctrl_deadzone)
            {
                normalizedX = 0.0f;
            }
            else
            {
                if (normalizedX > 0)
                    normalizedX = (normalizedX - config.ctrl_deadzone) / (1.0f - config.ctrl_deadzone);
                else
                    normalizedX = (normalizedX + config.ctrl_deadzone) / (1.0f - config.ctrl_deadzone);
            }

            // Scale Yaw
            float yawAdjustment = -normalizedX * config.ctrl_sensitivity;

            // Create a quaternion for the yaw adjustment
            vr::HmdQuaternion_t yawQuatAdjust = QuaternionFromAxisAngle(0.0f, 1.0f, 0.0f, DEG_TO_RAD(yawAdjustment));

            // Update the current yaw quaternion
            currentYawQuat = HmdQuaternion_Normalize(yawQuatAdjust * currentYawQuat);
        }

        // Reset Pose to origin
        if (config.pose_reset)
        {
            currentPitch = 0.0f;
            currentYawQuat = QuaternionFromAxisAngle(0.0f, 1.0f, 0.0f, DEG_TO_RAD(config.hmd_yaw));
            stereo_display_component_->SetReset();
        }

        float pitchRadians = DEG_TO_RAD(currentPitch);
        float yawRadians = 2.0f * acos(currentYawQuat.w);

        // Recompose the rotation quaternion from pitch and yaw
        vr::HmdQuaternion_t pitchQuaternion = QuaternionFromAxisAngle(1.0f, 0.0f, 0.0f, pitchRadians);

        if (config.use_open_track)
        {
            std::unique_lock<std::shared_mutex> lock(trk_mutex_);
            pose.qRotation = HmdQuaternion_Normalize(currentYawQuat * pitchQuaternion * open_track_att_);
            lock.unlock();
        }
        else
        {
            pose.qRotation = HmdQuaternion_Normalize(currentYawQuat * pitchQuaternion);
        }

        // Calculate the new position relative to the current pitch & yaw
        pose.vecPosition[0] = config.hmd_x + config.pitch_radius * cos(pitchRadians) * sin(yawRadians) - config.pitch_radius * sin(yawRadians);
        pose.vecPosition[1] = config.hmd_height - config.pitch_radius * sin(pitchRadians);
        pose.vecPosition[2] = config.hmd_y + config.pitch_radius * cos(pitchRadians) * cos(yawRadians) - config.pitch_radius * cos(yawRadians);
        if (pose.vecPosition[1] < config.hmd_height - 1.0)
        {
            pose.vecPosition[1] = config.hmd_height - 1.0;
        }

        // v3.4: Clamp deltaTime to prevent velocity/acceleration spikes on frame stalls
        float safe_dt = (std::max)(deltaTime, 0.001f);

        // Calculate velocity using known update interval
        pose.vecVelocity[0] = (pose.vecPosition[0] - lastPose.vecPosition[0]) / safe_dt;
        pose.vecVelocity[1] = (pose.vecPosition[1] - lastPose.vecPosition[1]) / safe_dt;
        pose.vecVelocity[2] = (pose.vecPosition[2] - lastPose.vecPosition[2]) / safe_dt;
        pose.vecAngularVelocity[0] = AngleDifference(pitchRadians, lastPitch) / safe_dt; // Pitch angular velocity
        pose.vecAngularVelocity[1] = AngleDifference(yawRadians, lastYaw) / safe_dt; // Yaw angular velocity
        pose.vecAngularVelocity[2] = 0.0f;

        // Calculate acceleration based on change in velocity
        pose.vecAcceleration[0] = (pose.vecVelocity[0] - lastPose.vecVelocity[0]) / safe_dt;
        pose.vecAcceleration[1] = (pose.vecVelocity[1] - lastPose.vecVelocity[1]) / safe_dt;
        pose.vecAcceleration[2] = (pose.vecVelocity[2] - lastPose.vecVelocity[2]) / safe_dt;
        pose.vecAngularAcceleration[0] = (pose.vecAngularVelocity[0] - lastPose.vecAngularVelocity[0]) / safe_dt;
        pose.vecAngularAcceleration[1] = (pose.vecAngularVelocity[1] - lastPose.vecAngularVelocity[1]) / safe_dt;
        pose.vecAngularAcceleration[2] = 0.0f;

        pose.poseIsValid = true;
        pose.deviceIsConnected = true;
        pose.result = vr::TrackingResult_Running_OK;
        pose.shouldApplyHeadModel = false;
        pose.willDriftInYaw = false;
        pose.poseTimeOffset = 0;

        // Update the pose
        pose_mutex_.lock();
        curr_pose_ = pose;
        pose_mutex_.unlock();

        // Update for next iteration
        lastPitch = pitchRadians;
        lastYaw = yawRadians;
        lastPose = pose;

        vr::VRServerDriverHost()->TrackedDevicePoseUpdated(device_index_, pose, sizeof(vr::DriverPose_t));

        // XInput polling limit is 125Hz
        auto endTime = std::chrono::high_resolution_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - currentTime);
        std::this_thread::sleep_for(std::chrono::milliseconds(8) - elapsed);
    }
}


//-----------------------------------------------------------------------------
// Purpose: Return current pose
//-----------------------------------------------------------------------------
vr::DriverPose_t MockControllerDeviceDriver::GetPose()
{
    std::lock_guard<std::mutex> lock(pose_mutex_);
    return curr_pose_;
}


//-----------------------------------------------------------------------------
// Purpose: Update HMD position, Depth, Convergence, and user binds
//-----------------------------------------------------------------------------
void MockControllerDeviceDriver::PollHotkeysThread() {
    struct {
        int shot = 0;
        int hmd = 0;
        int top = 0;
        int save = 0;
        int depth = 0;
        int overlay = 0;
    } sleep;

    const int sleep_time = static_cast<int>(floor(1000.0 / stereo_display_component_->GetConfig().display_frequency));
    std::string overlay_msg;
    HWND overlay_hwnd = nullptr;

    InitGDIPlus();

    auto setOverlay = [&](const std::string& msg) {
        overlay_msg = msg;
        sleep.overlay = stereo_display_component_->GetConfig().sleep_count_max * 3;
    };
    auto fmt = [&](const std::string& label, float value, int precision = 3) {
        std::ostringstream ss;
        ss << label << std::fixed << std::setprecision(precision) << value;
        return ss.str();
    };
    auto fmtDepthConv = [&]() {
        std::ostringstream ss;
        ss << "Depth: " << std::fixed << std::setprecision(3) << stereo_display_component_->GetDepth()
            << " Conv: " << stereo_display_component_->GetConvergence();
        return ss.str();
    };

    while (is_active_) {
        auto cfg = stereo_display_component_->GetConfig();

        if (!cfg.disable_hotkeys) {
            // Ctrl+F3 Decrease Depth
            if (isCtrlDown() && isDown(VK_F3)) {
                stereo_display_component_->AdjustDepth(-0.001f, true);
                uevr::receiver().set_base_depth(stereo_display_component_->GetDepth());
                if (isDown(VK_SHIFT)) stereo_display_component_->ResetProjection();
                setOverlay(fmtDepthConv());
            }
            // Ctrl+F4 Increase Depth
            else if (isCtrlDown() && isDown(VK_F4)) {
                stereo_display_component_->AdjustDepth(0.001f, true);
                uevr::receiver().set_base_depth(stereo_display_component_->GetDepth());
                if (isDown(VK_SHIFT)) stereo_display_component_->ResetProjection();
                setOverlay(fmtDepthConv());
            }
            // Ctrl+F5 Decrease Convergence
            else if (isCtrlDown() && isDown(VK_F5)) {
                stereo_display_component_->AdjustConvergence(0.005f, true);
                setOverlay(fmtDepthConv());
            }
            // Ctrl+F6 Increase Convergence
            else if (isCtrlDown() && isDown(VK_F6)) {
                stereo_display_component_->AdjustConvergence(-0.005f, true);
                setOverlay(fmtDepthConv());
            }
            // Ctrl+F7 Store settings into game profile
            if (isCtrlDown() && isDown(VK_F7) && sleep.save == 0) {
                if (!prev_name_.empty()) {
                    cfg.depth = stereo_display_component_->GetDepth();
                    cfg.convergence = stereo_display_component_->GetConvergence();
                    JsonManager().SaveProfileToJson(prev_name_ + "_config.json", cfg);
                    BeepSuccess();
                    setOverlay("Saved " + prev_name_ + "_config.json profile");
                }
                else {
                    BeepFailure();
                    setOverlay("Failed to save profile");
                }
                sleep.save = cfg.sleep_count_max;
            }
            // Ctrl+F9 Save HMD Position & Yaw
            if (isCtrlDown() && isDown(VK_F9) && sleep.hmd == 0) {
                JsonManager().SaveHmdOffsets(cfg);
                BeepSuccess();
                setOverlay("Saved HMD Offsets");
                sleep.hmd = cfg.sleep_count_max;
            }
            else if (sleep.hmd > 0) {
                --sleep.hmd;
            }
            // Ctrl+F10 Reload settings from Game Profile or (+Shift) Default Profile
            else if (isCtrlDown() && isDown(VK_F10) && sleep.save == 0) {
                std::string path = "";
                if (isDown(VK_SHIFT)) {
                    path = DEF_CFG;
                    app_name_ = "";
                }
                else if (!prev_name_.empty()) {
                    path = prev_name_ + "_config.json";
                    app_name_ = prev_name_;
                }
                if (JsonManager().LoadProfileFromJson(path, cfg)) {
                    stereo_display_component_->LoadSettings(cfg);
                    SetAsync(cfg.async_enable);
                    DriverLog("Loaded %s profile\n", path.c_str());
                    BeepSuccess();
                    setOverlay("Loaded " + path + " profile");
                }
                else {
                    BeepFailure();
                    setOverlay("Failed to load profile");
                }
                sleep.save = cfg.sleep_count_max;
            }
            else if (sleep.save > 0) {
                --sleep.save;
            }
            // Ctrl+F1 Toggle Auto Depth
            if (isCtrlDown() && isDown(VK_F1) && sleep.depth == 0) {
                use_auto_depth_ = !use_auto_depth_;
                uevr::receiver().set_auto_depth_enabled(use_auto_depth_.load());
                BeepSuccess();
                setOverlay(use_auto_depth_.load() ? "Auto Depth: ON" : "Auto Depth: OFF");
                sleep.depth = cfg.sleep_count_max;
            }
            // Ctrl+F2 Toggle Auto Convergence
            else if (isCtrlDown() && isDown(VK_F2) && sleep.depth == 0) {
                uevr::receiver().toggle_auto_convergence();
                BeepSuccess();
                setOverlay(uevr::receiver().is_auto_convergence_enabled() ?
                    "Auto Convergence: ON" : "Auto Convergence: OFF");
                sleep.depth = cfg.sleep_count_max;
            }
            else if (sleep.depth > 0) {
                --sleep.depth;
            }
        }
        // Ctrl+F8 Toggle Always On Top
        if (isCtrlDown() && isDown(VK_F8) && sleep.top == 0) {
            is_on_top_ = !is_on_top_;
            man_on_top_ = is_on_top_.load();
            sleep.top = cfg.sleep_count_max;
        }
        else if (sleep.top > 0) {
            --sleep.top;
        }
        // Ctrl+F12 Take Screenshot
        if (isCtrlDown() && isDown(VK_F12) && sleep.shot == 0) {
            take_screenshot_ = true;
            sleep.shot = cfg.sleep_count_max;
        }
        else if (sleep.shot > 0) {
            --sleep.shot;
        }
        // Ctrl+- Decrease Sensitivity
        if (isCtrlDown() && isDown(VK_OEM_MINUS)) {
            stereo_display_component_->AdjustSensitivity(-0.01f);
            setOverlay(fmt("Ctrl Sensitivity: ", cfg.ctrl_sensitivity, 2));
        }
        // Ctrl++ Increase Sensitivity
        else if (isCtrlDown() && isDown(VK_OEM_PLUS)) {
            stereo_display_component_->AdjustSensitivity(0.01f);
            setOverlay(fmt("Ctrl Sensitivity: ", cfg.ctrl_sensitivity, 2));
        }
        // Ctrl+[ Decrease Pitch Radius
        if (isCtrlDown() && isDown(VK_OEM_4)) {
            stereo_display_component_->AdjustRadius(-0.01f);
            setOverlay(fmt("Pitch Radius: ", cfg.pitch_radius, 2));
        }
        // Ctrl+] Increase Pitch Radius
        else if (isCtrlDown() && isDown(VK_OEM_6)) {
            stereo_display_component_->AdjustRadius(0.01f);
            setOverlay(fmt("Pitch Radius: ", cfg.pitch_radius, 2));
        }

        // Check User binds
        auto hotkey_str = stereo_display_component_->CheckUserSettings();

        // Check for Position Adjustment
        auto pos_str = stereo_display_component_->CheckPositionInput();

        if (!hotkey_str.empty()) {
            setOverlay(hotkey_str);
        }
        else if (!pos_str.empty()) {
            setOverlay(pos_str);
        }

        // Check for new profile load
        if (app_updated_)
        {
            setOverlay("Loaded " + app_name_ + "_config.json profile");
            app_updated_ = false;
        }
        // Check for no profile
        else if (no_profile_)
        {
            setOverlay("No profile found for " + app_name_);
            no_profile_ = false;
        }

        // Draw Overlay if applicable
        if (!overlay_hwnd) {
            overlay_hwnd = FindWindow(NULL, L"Headset Window");
        }
        else if (sleep.overlay > 0) {
            DrawOverlayText(overlay_hwnd, overlay_msg, cfg.window_height);
            --sleep.overlay;
        }

        // Sleep for ~ 1 frame
        std::this_thread::sleep_for(std::chrono::milliseconds(sleep_time));
    }
}


//-----------------------------------------------------------------------------
// Purpose: Keep Headset Window on top if set
//-----------------------------------------------------------------------------
void MockControllerDeviceDriver::FocusUpdateThread()
{
    static int sleep_time = 1000;
    static HWND vr_window = NULL;
    static HWND ww_window = NULL;
    static HWND main_window = NULL;
    static HWND top_window = NULL;
    static HWND game_window = NULL;
    static LONG ex_style = 0;
    static DWORD vr_pid = GetCurrentThreadId();
    static DWORD last_pid = 0;
    static bool was_on_top = false;
    static bool was_focused = false;

    while (is_active_)
    {
        // Get handle for the VR window
        if (vr_window == NULL) {
            vr_window = FindWindow(NULL, L"Headset Window");
            if (vr_window != NULL) {
                ex_style = GetWindowLong(vr_window, GWL_EXSTYLE);
            }
        }

        // Keep VR display always on top for 3D rendering
        if (is_on_top_ && IsProcessRunning(app_pid_)) {
            if (ww_window == NULL && !was_on_top) {
                ww_window = FindWindow(NULL, L"WibbleWobble");
                if (ww_window != NULL) {
                    if (vr_window != NULL) {
                        SetWindowPos(main_window, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
                        SetWindowLong(main_window, GWL_EXSTYLE, (ex_style | WS_EX_LAYERED) & ~WS_EX_TRANSPARENT);
                        std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    }
                    ex_style = GetWindowLong(ww_window, GWL_EXSTYLE);
                }
            }
            top_window = GetTopWindow(GetDesktopWindow());
            main_window = ww_window != NULL ? ww_window : vr_window;
            if (main_window != NULL && main_window != top_window) {
                SetWindowPos(main_window, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
                SetWindowLong(main_window, GWL_EXSTYLE, ex_style | (WS_EX_LAYERED | WS_EX_TRANSPARENT));
                std::this_thread::sleep_for(std::chrono::milliseconds(50));

                // Recenter VR
                if (last_pid != app_pid_) {
                    PostMessage(vr_window, WM_KEYDOWN, 'Z', 0);
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    PostMessage(vr_window, WM_KEYUP, 'Z', 0);
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    last_pid = app_pid_;
                }
            }
            was_on_top = true;
        }
        // Unfocus and check to see if the game is still running to re-enable focus
        else if (main_window != NULL && was_on_top) {
            SetWindowPos(main_window, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
            SetWindowLong(main_window, GWL_EXSTYLE, (ex_style | WS_EX_LAYERED) & ~WS_EX_TRANSPARENT);
            if (man_on_top_)
            {
                std::this_thread::sleep_for(std::chrono::seconds(15));
                is_on_top_ = IsProcessRunning(app_pid_);
            }
            was_on_top = is_on_top_;
            was_focused = false;
        }

        // Focus the Game Window
        if (is_on_top_ && !was_focused)
        {
            game_window = GetHWNDFromPID(app_pid_);
            ForceFocus(game_window, vr_pid, app_pid_);
            was_focused = true;
        }

        // Take Screenshot
        if (vr_window != NULL && take_screenshot_) {
            ForceFocus(vr_window, vr_pid, vr_pid);
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            PostMessage(vr_window, WM_KEYDOWN, 'S', 0);
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            PostMessage(vr_window, WM_KEYUP, 'S', 0);
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            ForceFocus(game_window, vr_pid, app_pid_);
            take_screenshot_ = false;
            BeepSuccess();
        }

        // Sleep for 1s
        std::this_thread::sleep_for(std::chrono::milliseconds(sleep_time));
    }
}


//-----------------------------------------------------------------------------
// Purpose: Receive Depth Values from Game
//-----------------------------------------------------------------------------
void MockControllerDeviceDriver::AutoDepthThread() {
    const std::string pipeName = R"(\\.\pipe\AutoDepth)";

    while (is_active_) {
        // Create the named pipe in NON-BLOCKING mode
        HANDLE hPipe = CreateNamedPipeA(
            pipeName.c_str(),
            PIPE_ACCESS_INBOUND,        // Read-only access
            PIPE_TYPE_MESSAGE |         // Message-type pipe
            PIPE_READMODE_MESSAGE |     // Message read mode
            PIPE_NOWAIT,                // Non-blocking mode (prevents blocking when client disconnects)
            1,                          // Max instances
            512,                        // Output buffer size
            512,                        // Input buffer size
            0,                          // Default timeout
            NULL                        // Default security attributes
        );

        if (hPipe == INVALID_HANDLE_VALUE) {
            DriverLog("Failed to create named pipe. Error: %d\n", GetLastError());
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }

        DriverLog("Waiting for depth client to connect...\n");

        while (is_active_) {
            // Check if a client is trying to connect
            BOOL connected = ConnectNamedPipe(hPipe, NULL);
            DWORD err = GetLastError();

            if (connected || err == ERROR_PIPE_CONNECTED) {
                DriverLog("Client connected! Receiving values...\n");
                break; // Proceed to read data
            }
            else if (err == ERROR_NO_DATA || err == ERROR_PIPE_LISTENING) {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }
            else {
                DriverLog("Failed to connect to client. Error: %d\n", err);
                CloseHandle(hPipe);
                break; // Recreate pipe
            }

            // Check if we should exit immediately
            if (!is_active_) {
                DriverLog("Exiting connection loop...\n");
                CloseHandle(hPipe);
                return;
            }
        }

        char buffer[16];
        DWORD bytesRead;

        // Read data continuously until client disconnects
        while (is_active_) {
            BOOL success = ReadFile(hPipe, buffer, sizeof(buffer) - 1, &bytesRead, NULL);
            if (!success || bytesRead == 0) {
                DWORD readErr = GetLastError();
                if (readErr == ERROR_NO_DATA || readErr == ERROR_PIPE_LISTENING) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                    continue;
                }

                DriverLog("Depth client disconnected. Reconnecting...\n");
                break; // Exit inner loop and recreate the pipe
            }

            if (use_auto_depth_)
            {
                // Convert to float
                buffer[bytesRead] = '\0';
                float depthValue = strtof(buffer, nullptr);
                stereo_display_component_->AdjustDepth(depthValue, false);
            }
        }

        // Close the current pipe handle (ready to reconnect)
        CloseHandle(hPipe);
    }
}


//-----------------------------------------------------------------------------
// Purpose: Load Game Specific Settings from Documents\My games\vrto3d\app_name_config.json
//-----------------------------------------------------------------------------
void MockControllerDeviceDriver::LoadSettings(const std::string& app_name, uint32_t app_pid, vr::EVREventType status)
{
    if ((app_name != app_name_ || app_pid != app_pid_) && status == vr::VREvent_ProcessConnected)
    {
        app_name_ = app_name;
        prev_name_ = app_name;
        app_pid_ = app_pid;
        auto config = stereo_display_component_->GetConfig();

        // Attempt to read the JSON settings file
        if (JsonManager().LoadProfileFromJson(app_name + "_config.json", config))
        {
            stereo_display_component_->LoadSettings(config);
            DriverLog("Loaded %s profile\n", app_name.c_str());
            BeepSuccess();
            app_updated_ = true;
            if (config.auto_focus) {
                std::this_thread::sleep_for(std::chrono::seconds(8));
                is_on_top_ = true;
                man_on_top_ = true;
            }
        }
        else {
            // No VRto3D profile — try auto-depth from UEVR world_scale
            float auto_depth = 0.0f, auto_conv = 0.0f;
            if (uevr::receiver().is_connected() &&
                uevr::receiver().calculate_auto_stereo(auto_depth, auto_conv))
            {
                config.depth = auto_depth;
                config.convergence = auto_conv;
                stereo_display_component_->LoadSettings(config);
                DriverLog("Auto-stereo from world_scale: d=%.4f c=%.2f ws=%.1f\n",
                    auto_depth, auto_conv, uevr::receiver().get_world_scale());
                BeepSuccess();
                app_updated_ = true;
            }
            else {
                BeepFailure();
                no_profile_ = true;
            }
        }

        SetAsync(config.async_enable);
    }
    else if (status == vr::VREvent_ProcessDisconnected)
    {
        is_on_top_ = false;
    }
}


//-----------------------------------------------------------------------------
// Purpose: Scan for App IDs and set Async Reprojection On/Off
//-----------------------------------------------------------------------------
void MockControllerDeviceDriver::SetAsync(bool enable)
{
    auto app_ids = AppIdMgr().GetSteamAppIDs();
    for (const std::string& app_id : app_ids) {
        vr::VRSettings()->SetBool(app_id.c_str(), vr::k_pch_SteamVR_DisableAsyncReprojection_Bool, !enable);
        DriverLog("%s Async Reprojection for appkey: %s",
            enable ? "Enabled" : "Disabled",
            app_id.c_str());
    }
}


//-----------------------------------------------------------------------------
// Purpose: Stub for Standby mode
//-----------------------------------------------------------------------------
void MockControllerDeviceDriver::EnterStandby()
{
    DriverLog( "HMD has been put into standby." );
}

//-----------------------------------------------------------------------------
// Purpose: Shutdown process
//-----------------------------------------------------------------------------
void MockControllerDeviceDriver::Deactivate()
{
    if ( is_active_.exchange( false ) )
    {
        pose_thread_.join();
        hotkey_thread_.join();
        focus_thread_.join();
        depth_thread_.join();
        if (track_thread_.joinable()) {
            track_thread_.join();
        }
    }

    // unassign our controller index (we don't want to be calling vrserver anymore after Deactivate() has been called
    uevr::receiver().shutdown();
    device_index_ = vr::k_unTrackedDeviceIndexInvalid;
}


//-----------------------------------------------------------------------------
// DISPLAY DRIVER METHOD DEFINITIONS
//-----------------------------------------------------------------------------

StereoDisplayComponent::StereoDisplayComponent( const StereoDisplayDriverConfiguration &config )
    : config_( config ), depth_(config.depth), convergence_(config.convergence), fov_(config.fov),
      uevr_effective_depth_(config.depth), uevr_effective_convergence_(config.convergence)
{
}


//-----------------------------------------------------------------------------
// Purpose: Initialize the Stereo Display Component
//-----------------------------------------------------------------------------
void StereoDisplayComponent::Init(uint32_t device_index) {
    device_index_ = device_index;



}

//-----------------------------------------------------------------------------
    // Purpose: Set effective depth from UEVR multiplier calculation
    //-----------------------------------------------------------------------------
void StereoDisplayComponent::SetUEVREffectiveDepth(float depth) {
    uevr_effective_depth_.store(depth, std::memory_order_relaxed);
}

void StereoDisplayComponent::SetUEVREffectiveIPD(float ipd) {
    vr::PropertyContainerHandle_t container = vr::VRProperties()->TrackedDeviceToPropertyContainer(device_index_);
    vr::VRProperties()->SetFloatProperty(container, vr::Prop_UserIpdMeters_Float, ipd);
}

void StereoDisplayComponent::SetUEVREffectiveConvergence(float convergence) {
    uevr_effective_convergence_.store(convergence, std::memory_order_relaxed);
}

float StereoDisplayComponent::GetUEVREffectiveConvergence() {
    return uevr_effective_convergence_.load(std::memory_order_relaxed);
}

void StereoDisplayComponent::SetAimCorrection(float correction) {
    uevr_aim_correction_.store(correction, std::memory_order_relaxed);
}

void StereoDisplayComponent::SetAimBase(float base) {
    uevr_aim_base_.store(base, std::memory_order_relaxed);
}

void StereoDisplayComponent::SetMonitorMode(bool enable) {
    monitor_mode_.store(enable, std::memory_order_relaxed);
}

bool StereoDisplayComponent::IsMonitorMode() const {
    return monitor_mode_.load(std::memory_order_relaxed);
}



//-----------------------------------------------------------------------------
// Purpose: To inform vrcompositor if this display is considered an on-desktop display.
//-----------------------------------------------------------------------------
bool StereoDisplayComponent::IsDisplayOnDesktop()
{
    return false;
}

//-----------------------------------------------------------------------------
// Purpose: To as vrcompositor to search for this display.
//-----------------------------------------------------------------------------
bool StereoDisplayComponent::IsDisplayRealDisplay()
{
    return false;
}

//-----------------------------------------------------------------------------
// Purpose: To inform the rest of the vr system what the recommended target size should be
//-----------------------------------------------------------------------------
void StereoDisplayComponent::GetRecommendedRenderTargetSize( uint32_t *pnWidth, uint32_t *pnHeight )
{
    std::shared_lock<std::shared_mutex> lock(cfg_mutex_);
    *pnWidth = config_.render_width;
    *pnHeight = config_.render_height;
}

//-----------------------------------------------------------------------------
// Purpose: Render in SbS or TaB Stereo3D
//-----------------------------------------------------------------------------
void StereoDisplayComponent::GetEyeOutputViewport( vr::EVREye eEye, uint32_t *pnX, uint32_t *pnY, uint32_t *pnWidth, uint32_t *pnHeight )
{
    std::shared_lock<std::shared_mutex> lock(cfg_mutex_);
    if (config_.reverse_enable)
    {
        eEye = static_cast<vr::EVREye>(!static_cast<bool> (eEye));
    }

    // Monitor mode: clean full-height SBS, no VR-specific cropping (lesson 28, 63)
    if (monitor_mode_.load(std::memory_order_relaxed))
    {
        uint32_t half = config_.window_width / 2;
        *pnY = 0;
        *pnHeight = config_.window_height;
        if (eEye == vr::Eye_Left) {
            *pnX = 0;
            *pnWidth = half;
        } else {
            *pnX = half;
            *pnWidth = config_.window_width - half;  // Remainder for exact coverage
        }
        return;
    }

    // Use Top and Bottom Rendering
    if (config_.tab_enable)
    {
        *pnX = 0;
        // Each eye will have full width
        *pnWidth = config_.window_width;
        // Each eye will have half height
        *pnHeight = (config_.window_height - config_.framepack_offset) / 2;
        if (eEye == vr::Eye_Left)
        {
            // Left eye viewport on the top half of the window
            *pnY = 0;
        }
        else
        {
            // Right eye viewport on the bottom half of the window
            *pnY = *pnHeight + config_.framepack_offset;
        }
    }

    // Use Side by Side Rendering
    else
    {
        // Each eye will have half height for virtual desktop
        if (config_.vd_fsbs_hack)
        {
            *pnY = config_.window_height / 4;
            *pnHeight = config_.window_height / 2;
        }
        // Each eye will have full height
        else
        {
            *pnY = 0;
            *pnHeight = config_.window_height;
        }
        // Each eye will have half width
        *pnWidth = config_.window_width / 2;
        if (eEye == vr::Eye_Left)
        {
            // Left eye viewport on the left half of the window
            *pnX = 0;
        }
        else
        {
            // Right eye viewport on the right half of the window
            *pnX = config_.window_width / 2;
        }
    }
}

//-----------------------------------------------------------------------------
// Purpose: Utilize the desired FoV, Aspect Ratio, and Convergence settings
//-----------------------------------------------------------------------------
void StereoDisplayComponent::GetProjectionRaw( vr::EVREye eEye, float *pfLeft, float *pfRight, float *pfTop, float *pfBottom )
{
    std::shared_lock<std::shared_mutex> lock(cfg_mutex_);

    // Convert horizontal FOV from degrees to radians
    float horFovRadians = tan((GetFoV() * (M_PI / 180.0f)) / 2);

    // Calculate vertical FOV in radians
    float verFovRadians = horFovRadians / config_.aspect_ratio;

    // Monitor mode: symmetric frustum — UEVR handles convergence via projection [2][0]
    if (monitor_mode_.load(std::memory_order_relaxed)) {
        *pfTop = -verFovRadians;
        *pfBottom = verFovRadians;
        *pfLeft = -horFovRadians;
        *pfRight = horFovRadians;
        return;
    }

    // VR mode: asymmetric frustum with depth/convergence baked in
    // IPD-based horizontal offset - use UEVR effective values for both depth and convergence
    // PoseUpdateThread keeps these in sync (depth reduced when zoomed, convergence scene-aware)
    float sep = uevr_effective_depth_.load();
    float conv = uevr_effective_convergence_.load(std::memory_order_relaxed);
    if (conv < 0.001f) conv = 0.001f;  // Prevent division by zero
    float eyeOffset = (eEye == vr::Eye_Left) ? sep * 0.5f / conv : -sep * 0.5f / conv;

    // v3.5: Aim correction via ANTISYMMETRIC frustum shear
    // Research-backed: ΔP_02 = ±(IPD_base - IPD_ads) / (2 * Z_focus)
    // Applies correction PER-EYE with opposite signs to shift convergence,
    // NOT the viewport. Gun stays centered; only where the two eyes converge changes.
    float aim_base = uevr_aim_base_.load(std::memory_order_relaxed);
    float aim_zoom = uevr_aim_correction_.load(std::memory_order_relaxed);
    float base_depth = depth_.load(std::memory_order_relaxed);

    // zoom_factor = 1 - depth_multiplier (how far IPD has been reduced)
    // Dead zone + soft ramp to eliminate jitter at zoom boundary
    float raw_zf = (base_depth > 0.001f) ? 1.0f - (sep / base_depth) : 0.0f;
    raw_zf = (std::max)(0.0f, (std::min)(raw_zf, 1.0f));
    constexpr float ZF_DEAD = 0.02f;   // Ignore when barely zoomed (dm > 0.98)
    constexpr float ZF_RAMP = 0.05f;   // Soft ramp from 0.02 to 0.05
    float zoom_factor;
    if (raw_zf < ZF_DEAD) {
        zoom_factor = 0.0f;
    } else if (raw_zf < ZF_RAMP) {
        zoom_factor = (raw_zf - ZF_DEAD) / (ZF_RAMP - ZF_DEAD) * ZF_RAMP;
    } else {
        zoom_factor = raw_zf;
    }

    // Differential correction: applied with SAME sign convention as eyeOffset
    // Left eye (+) gets +correction → shears LEFT frustum right
    // Right eye (-) gets -correction → shears RIGHT frustum left
    // Net: convergence point shifts, viewport centers DON'T move
    float aim_correction = aim_base + (aim_zoom * zoom_factor);
    float eye_sign = (eEye == vr::Eye_Left) ? 1.0f : -1.0f;
    float corrected_eyeOffset = eyeOffset + (eye_sign * aim_correction);

    // Set frustum bounds
    *pfTop = -verFovRadians;
    *pfBottom = verFovRadians;
    *pfLeft = -horFovRadians + corrected_eyeOffset;
    *pfRight = horFovRadians + corrected_eyeOffset;
}

//-----------------------------------------------------------------------------
// Purpose: Don't distort any coordinates for Stereo3D
//-----------------------------------------------------------------------------
vr::DistortionCoordinates_t StereoDisplayComponent::ComputeDistortion( vr::EVREye eEye, float fU, float fV )
{
    vr::DistortionCoordinates_t coordinates{};
    coordinates.rfBlue[ 0 ] = fU;
    coordinates.rfBlue[ 1 ] = fV;
    coordinates.rfGreen[ 0 ] = fU;
    coordinates.rfGreen[ 1 ] = fV;
    coordinates.rfRed[ 0 ] = fU;
    coordinates.rfRed[ 1 ] = fV;
    return coordinates;
}
bool StereoDisplayComponent::ComputeInverseDistortion(vr::HmdVector2_t* pResult, vr::EVREye eEye, uint32_t unChannel, float fU, float fV)
{
    return false;
}

//-----------------------------------------------------------------------------
// Purpose: To inform vrcompositor what the window bounds for this virtual HMD are.
//-----------------------------------------------------------------------------
void StereoDisplayComponent::GetWindowBounds( int32_t *pnX, int32_t *pnY, uint32_t *pnWidth, uint32_t *pnHeight )
{
    std::shared_lock<std::shared_mutex> lock(cfg_mutex_);
    *pnX = config_.window_x;
    *pnY = config_.window_y;
    *pnWidth = config_.window_width;
    *pnHeight = config_.window_height;
}

//-----------------------------------------------------------------------------
// Purpose: To provide access to settings
//-----------------------------------------------------------------------------
StereoDisplayDriverConfiguration StereoDisplayComponent::GetConfig()
{
    std::shared_lock<std::shared_mutex> lock(cfg_mutex_);
    return config_;
}


//-----------------------------------------------------------------------------
// Purpose: To update the Depth value
//-----------------------------------------------------------------------------
void StereoDisplayComponent::AdjustDepth(float new_depth, bool is_delta)
{
    float cur_depth = GetDepth();
    if (is_delta) {
        new_depth += cur_depth;
        new_depth = (new_depth < 0) ? 0 : new_depth;
    }
    while (!depth_.compare_exchange_weak(cur_depth, new_depth, std::memory_order_relaxed));
    vr::PropertyContainerHandle_t container = vr::VRProperties()->TrackedDeviceToPropertyContainer(device_index_);
    vr::VRProperties()->SetFloatProperty(container, vr::Prop_UserIpdMeters_Float, new_depth);
}


//-----------------------------------------------------------------------------
// Purpose: To update the Convergence value
//-----------------------------------------------------------------------------
void StereoDisplayComponent::AdjustConvergence(float new_conv, bool is_delta)
{
    float cur_conv = GetConvergence();
    if (is_delta) {
        new_conv += cur_conv;
        new_conv = (new_conv < 0.001) ? 0.001 : new_conv;
    }
    if (NearlyEqual(cur_conv, new_conv))
        return;
    while (!convergence_.compare_exchange_weak(cur_conv, new_conv, std::memory_order_relaxed));
    ResetProjection();
}


//-----------------------------------------------------------------------------
// Purpose: To update the FoV value
//-----------------------------------------------------------------------------
void StereoDisplayComponent::AdjustFoV(float new_fov)
{
    float cur_fov = GetFoV();
    if (NearlyEqual(cur_fov, new_fov))
        return;
    while (!fov_.compare_exchange_weak(cur_fov, new_fov, std::memory_order_relaxed));
    ResetProjection();
}


//-----------------------------------------------------------------------------
// Purpose: Get Depth value
//-----------------------------------------------------------------------------
float StereoDisplayComponent::GetDepth()
{
    return depth_.load(std::memory_order_relaxed);
}


//-----------------------------------------------------------------------------
// Purpose: Get Convergence value
//-----------------------------------------------------------------------------
float StereoDisplayComponent::GetConvergence()
{
    return convergence_.load(std::memory_order_relaxed);
}


//-----------------------------------------------------------------------------
// Purpose: Get FoV value
//-----------------------------------------------------------------------------
float StereoDisplayComponent::GetFoV()
{
    return fov_.load(std::memory_order_relaxed);
}


//-----------------------------------------------------------------------------
// Purpose: Check User Settings and act on them
//-----------------------------------------------------------------------------
std::string StereoDisplayComponent::CheckUserSettings()
{
    static int sleep_ctrl = 0;
    static int sleep_rest = 0;
    std::string overlay_msg = "";
    
    DWORD xstate;
    bool got_xinput = GetXInputButtonState(xstate);

    auto config = GetConfig();

    // Toggle Pitch and Yaw control
    if ((config.ctrl_xinput && got_xinput &&
        ((xstate & config.ctrl_toggle_key) == config.ctrl_toggle_key))
        || (!config.ctrl_xinput && isDown(config.ctrl_toggle_key)))
    {
        if (config.ctrl_type == HOLD && !config.ctrl_held)
        {
            config.ctrl_held = true;
            config.pitch_enable = false;
            config.yaw_enable = false;
        }
        else if ((config.ctrl_type == TOGGLE || config.ctrl_type == SWITCH) && sleep_ctrl < 1)
        {
            sleep_ctrl = config.sleep_count_max;
            if (config.pitch_set) {
                config.pitch_enable = !config.pitch_enable;
            }
            if (config.yaw_set) {
                config.yaw_enable = !config.yaw_enable;
            }
        }
    }
    else if (config.ctrl_type == HOLD && config.ctrl_held)
    {
        config.ctrl_held = false;
        config.pitch_enable = config.pitch_set;
        config.yaw_enable = config.yaw_set;
    }
    if (sleep_ctrl > 0) {
        sleep_ctrl--;
    }

    // Reset HMD position
    if (((config.reset_xinput && got_xinput &&
        ((xstate & config.pose_reset_key) == config.pose_reset_key))
        || (!config.reset_xinput && isDown(config.pose_reset_key)))
        && sleep_rest == 0)
    {
        sleep_rest = config.sleep_count_max;
        if (!config.pose_reset) {
            config.pose_reset = true;
        }
    }
    else if (sleep_rest > 0) {
        sleep_rest--;
    }

    DepthConvBackend b{
      +[](void* ctx)->float { return static_cast<StereoDisplayComponent*>(ctx)->GetDepth(); },
      +[](void* ctx)->float { return static_cast<StereoDisplayComponent*>(ctx)->GetConvergence(); },
      +[](void* ctx, float v) { static_cast<StereoDisplayComponent*>(ctx)->AdjustDepth(v, false); },
      +[](void* ctx, float v) { static_cast<StereoDisplayComponent*>(ctx)->AdjustConvergence(v, false); },
      +[](void* ctx)->float { return static_cast<StereoDisplayComponent*>(ctx)->GetFoV(); },
      +[](void* ctx, float v) { static_cast<StereoDisplayComponent*>(ctx)->AdjustFoV(v); },
      nullptr,
      this
    };

    auto msg = ApplyUserSettingsHotkeys(config, got_xinput, xstate, b);
    if (!msg.empty()) overlay_msg = std::move(msg);

    // Update the config
    std::unique_lock<std::shared_mutex> lock(cfg_mutex_);
    config_ = config;

    return overlay_msg;
}


//-----------------------------------------------------------------------------
// Purpose: Move HMD origin position with keyboard input
//-----------------------------------------------------------------------------
std::string StereoDisplayComponent::CheckPositionInput() {
    if (!isCtrlDown())
        return "";

    const float step = 0.01f;
    auto config = GetConfig();

    if (isDown(VK_HOME)) {
            config.hmd_y -= step;       // Forward
    }
    else if (isDown(VK_END)) {
            config.hmd_y += step;       // Backward
    }
    else if (isDown(VK_DELETE)) {
        config.hmd_x -= step;       // Left
    }
    else if (isDown(VK_NEXT)) {
        if (isDown(VK_SHIFT)) {
            config.hmd_height -= step;  // Down
        }
        else {
            config.hmd_x += step;       // Right
        }
    }
    else if (isDown(VK_PRIOR)) {
        if (isDown(VK_SHIFT)) {
            config.hmd_height += step;  // Up
        }
        else {
            config.hmd_yaw -= step * 10;     // Yaw CW
        }
    }
    else if (isDown(VK_INSERT)) {
        config.hmd_yaw += step * 10;     // Yaw CCW
    }
    else {
        return "";
    }

    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2);
    oss << "Pos X:" << config.hmd_x
        << " Y:" << config.hmd_y
        << " Z:" << config.hmd_height
        << " Yaw:" << config.hmd_yaw;

    std::unique_lock<std::shared_mutex> lock(cfg_mutex_);
    config_ = config;

    return oss.str();
}


//-----------------------------------------------------------------------------
// Purpose: Adjust XInput Right Stick sensitivity
//-----------------------------------------------------------------------------
void StereoDisplayComponent::AdjustSensitivity(float delta)
{
    std::unique_lock<std::shared_mutex> lock(cfg_mutex_);
    if (config_.pitch_enable || config_.yaw_enable)
    {
        config_.ctrl_sensitivity += delta;
        if (config_.ctrl_sensitivity < 0.0f)
            config_.ctrl_sensitivity = 0.0f;
    }
}


//-----------------------------------------------------------------------------
// Purpose: Adjust XInput Pitch Radius
//-----------------------------------------------------------------------------
void StereoDisplayComponent::AdjustRadius(float delta)
{
    std::unique_lock<std::shared_mutex> lock(cfg_mutex_);
    if (config_.pitch_enable)
    {
        config_.pitch_radius += delta;
        if (config_.pitch_radius < 0.0f)
            config_.pitch_radius = 0.0f;
    }
}


//-----------------------------------------------------------------------------
// Purpose: Toggle Reset off
//-----------------------------------------------------------------------------
void StereoDisplayComponent::SetReset()
{
    std::unique_lock<std::shared_mutex> lock(cfg_mutex_);
    config_.pose_reset = false;
}


//-----------------------------------------------------------------------------
// Purpose: Load Game Specific Settings from Documents\My games\vrto3d\app_name_config.json
//-----------------------------------------------------------------------------
void StereoDisplayComponent::LoadSettings(StereoDisplayDriverConfiguration& config)
{
    // Apply loaded settings
    AdjustDepth(config.depth, false);
    AdjustConvergence(config.convergence, false);
    uevr::receiver().set_base_depth(config.depth);
    
    std::unique_lock<std::shared_mutex> lock(cfg_mutex_);
    config_ = config;
    lock.unlock();
    ResetProjection();
}


//-----------------------------------------------------------------------------
// Purpose: Reset per-eye FoV calculation
//-----------------------------------------------------------------------------
void StereoDisplayComponent::ResetProjection()
{
    // Regenerate the Projection
    vr::HmdRect2_t eyeLeft, eyeRight;
    GetProjectionRaw(vr::Eye_Left, &eyeLeft.vTopLeft.v[0], &eyeLeft.vBottomRight.v[0], &eyeLeft.vTopLeft.v[1], &eyeLeft.vBottomRight.v[1]);
    GetProjectionRaw(vr::Eye_Right, &eyeRight.vTopLeft.v[0], &eyeRight.vBottomRight.v[0], &eyeRight.vTopLeft.v[1], &eyeRight.vBottomRight.v[1]);
    vr::VREvent_Data_t temp;
    vr::VRServerDriverHost()->SetDisplayProjectionRaw(device_index_, eyeLeft, eyeRight);
    vr::VRServerDriverHost()->VendorSpecificEvent(device_index_, vr::VREvent_LensDistortionChanged, temp, 0.0f);
}
