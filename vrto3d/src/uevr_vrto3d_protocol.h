/*
 * uevr_vrto3d_protocol.h - Shared Memory Protocol Definition
 * 
 * VERSION: 3.3
 *
 * SINGLE SOURCE OF TRUTH for the shared memory layout between UEVR and VRto3D.
 * Both projects should include this EXACT file to prevent struct misalignment.
 *
 * UEVR includes this as:  #include "uevr_vrto3d_protocol.h"
 * VRto3D includes this as: #include "uevr_vrto3d_protocol.h"
 *
 * LICENSE: This file is dual-licensed to be compatible with both:
 *   - UEVR (MIT License)
 *   - VRto3D (LGPL v3)
 *
 * CHANGELOG:
 *   v3.3 - Added stereo_depth_hint field (4 bytes from reserved)
 *        - UEVR writes its stereo_depth so VRto3D can match overlay IPD to game
 *        - Wire-compatible with v3.2: new field occupies previously-zeroed area
 *   v3.2 - Added monitor_mode + is_monitor_display fields (2 bytes from reserved)
 *        - UEVR sets monitor_mode=1 when monitor mode is active
 *        - VRto3D sets is_monitor_display=1 when outputting to monitor (not HMD)
 *        - Wire-compatible with v3.1: new fields occupy previously-zeroed area
 *   v3.1 - Added profile modifier fields (48 bytes from reserved)
 *        - Added UEVR_FLAG_MODIFIERS feature flag
 *        - Reserved shrinks from 72 to 24 bytes
 *        - Wire-compatible with v3.0: modifiers occupy previously-zeroed area
 *        - Fixed _pad1 alignment (was 1 byte, now 5 bytes)
 *        - Made this a standalone header both projects share
 *   v3.0 - Multiplier mode (depth_multiplier instead of suggested_depth)
 *   v2.0 - Absolute values (deprecated)
 */

#ifndef UEVR_VRTO3D_PROTOCOL_H
#define UEVR_VRTO3D_PROTOCOL_H

#include <cstdint>

/* ========================================================================== */
/* PROTOCOL CONSTANTS                                                         */
/* ========================================================================== */

#define UEVR_VRTO3D_MAGIC          0x55455652   /* "UEVR" in ASCII            */
#define UEVR_VRTO3D_VERSION        3            /* Protocol version           */
#define UEVR_VRTO3D_STRUCT_SIZE    256          /* Total struct size in bytes  */
#define UEVR_VRTO3D_SHMEM_NAME    "UEVR_VRto3D_SharedData"

/* Staleness threshold: data older than this (ms) is considered disconnected  */
#define UEVR_VRTO3D_STALE_MS       1000

/* Feature flags (bitfield in SharedData.flags)                               */
#define UEVR_FLAG_MULTIPLIER_MODE  0x01   /* Uses multipliers, not absolutes  */
#define UEVR_FLAG_SCENE_AWARE      0x02   /* Sends scene_type field           */
#define UEVR_FLAG_FOV_COMP         0x04   /* Supports FOV compensation        */
#define UEVR_FLAG_MODIFIERS        0x08   /* v3.1: Profile modifier fields    */
#define UEVR_FLAG_AIM_CORRECTION   0x10   /* v3.2: stereo_aim_correction field */

/* ========================================================================== */
/* ENUMS                                                                       */
/* ========================================================================== */

/* Scene type - tells VRto3D what context UEVR is in */
enum UEVR_SceneType : uint8_t {
    UEVR_SCENE_NORMAL   = 0,
    UEVR_SCENE_CUTSCENE = 1,
    UEVR_SCENE_MENU     = 2,
    UEVR_SCENE_VEHICLE  = 3,
    UEVR_SCENE_LOADING  = 4
};

/* Zoom mode - tells VRto3D what kind of zoom UEVR detected */
enum UEVR_ZoomMode : uint8_t {
    UEVR_ZOOM_NONE           = 0,
    UEVR_ZOOM_AIM_DOWN_SIGHT = 1,
    UEVR_ZOOM_SCOPE          = 2
};

/* ========================================================================== */
/* SHARED MEMORY STRUCTURE - 256 BYTES, PACKED                                 */
/*                                                                             */
/* BOTH SIDES MUST USE THIS EXACT LAYOUT.                                      */
/*                                                                             */
/* Offset map:                                                                 */
/*   0-15    HEADER             (16 bytes)                                     */
/*   16-39   FOV / ZOOM         (24 bytes)  UEVR -> VRto3D                    */
/*   40-55   DEPTH CONTROL      (16 bytes)  UEVR -> VRto3D                    */
/*   56-71   TIMING             (16 bytes)  UEVR -> VRto3D                    */
/*   72-103  VRTO3D STATE       (32 bytes)  VRto3D -> UEVR                    */
/*   104-119 VRTO3D STATUS      (16 bytes)  VRto3D -> UEVR                    */
/*   120-183 PROFILE INFO       (64 bytes)  UEVR -> VRto3D                    */
/*   184-231 PROFILE MODIFIERS  (48 bytes)  VRto3D -> UEVR  [v3.1]           */
/*   232-243 AIM + COMMANDS     (12 bytes)  UEVR -> VRto3D  [v3.2]           */
/*   244-245 MONITOR MODE       (2 bytes)   Bidirectional   [v3.2]           */
/*   246-249 STEREO DEPTH HINT  (4 bytes)   UEVR -> VRto3D  [v3.3]           */
/*   250-255 RESERVED           (6 bytes)   Future use                         */
/* ========================================================================== */

#pragma pack(push, 1)
typedef struct UEVR_VRto3D_SharedData {

    /* ----- HEADER (16 bytes) -------------------------------------------- */
    uint32_t magic;                  /* Must be UEVR_VRTO3D_MAGIC            */
    uint32_t version;                /* UEVR_VRTO3D_VERSION                  */
    uint32_t struct_size;            /* sizeof(UEVR_VRto3D_SharedData) = 256 */
    uint32_t flags;                  /* Bitfield: UEVR_FLAG_*                */

    /* ----- UEVR -> VRTO3D: FOV / ZOOM (24 bytes) ----------------------- */
    float    game_fov;               /* Game camera FOV in degrees           */
    float    base_fov;               /* Calibrated "normal" FOV              */
    float    fov_scale;              /* Projection scale (0.5 = 2x zoom)    */
    float    zoom_factor;            /* Magnification (2.0 = 2x zoom)       */
    uint8_t  is_zooming;             /* 1 if zoom is active                  */
    uint8_t  is_valid;               /* 1 if FOV reading is trustworthy      */
    uint8_t  zoom_mode;              /* UEVR_ZoomMode enum                   */
    uint8_t  _pad1[5];              /* Align to 24 bytes for this section   */

    /* ----- UEVR -> VRTO3D: DEPTH CONTROL (16 bytes) -------------------- */
    float    depth_multiplier;       /* 0.05 - 1.0 (1.0 = no change)        */
    float    convergence_multiplier; /* Reserved, usually 1.0                */
    uint8_t  scene_type;             /* UEVR_SceneType enum                  */
    uint8_t  auto_depth_request;     /* 1 if UEVR wants auto-depth applied   */
    uint8_t  _pad2[2];              /* Padding                              */
    float    world_scale;            /* UEVR world scale                     */

    /* ----- UEVR -> VRTO3D: TIMING (16 bytes) --------------------------- */
    uint64_t uevr_timestamp;         /* GetTickCount64() from UEVR           */
    uint32_t uevr_frame_count;       /* UEVR frame counter                   */
    float    uevr_frametime;         /* Frame time in seconds                */

    /* ----- VRTO3D -> UEVR: CURRENT STATE (32 bytes) -------------------- */
    float    vrto3d_depth;           /* VRto3D's current depth               */
    float    vrto3d_convergence;     /* VRto3D's current convergence         */
    float    vrto3d_fov;             /* VRto3D's current FOV                 */
    float    vrto3d_fov_adjustment;  /* FOV delta from convergence           */
    float    vrto3d_aspect_ratio;    /* Display aspect ratio                 */
    float    vrto3d_ipd;             /* IPD setting                          */
    float    vrto3d_hmd_height;      /* HMD height                           */
    uint8_t  vrto3d_sbs_mode;       /* 0 = TaB, 1 = SbS                    */
    uint8_t  _pad3[3];              /* Padding                              */

    /* ----- VRTO3D -> UEVR: STATUS (16 bytes) --------------------------- */
    uint8_t  vrto3d_connected;       /* 1 if VRto3D is reading this memory   */
    uint8_t  vrto3d_auto_depth_active; /* 1 if applying depth multiplier     */
    uint8_t  vrto3d_profile_loaded;  /* 1 if VRto3D has a game profile       */
    uint8_t  vrto3d_listener_enabled;/* 1 if Ctrl+F11 auto-depth is ON       */
    uint8_t  _pad4[4];              /* Padding                              */
    uint64_t vrto3d_timestamp;       /* GetTickCount64() from VRto3D         */

    /* ----- PROFILE INFO (64 bytes) -------------------------------------- */
    char     uevr_profile_name[32];  /* Current UEVR profile name            */
    char     game_exe_name[32];      /* Game executable name                 */

    /* ----- v3.1: PROFILE MODIFIERS (48 bytes) VRto3D -> UEVR ----------- */
    /*                                                                       */
    /* Written by VRto3D when a game profile has "uevr_modifiers" section.   */
    /* All float fields use 0.0 as sentinel for "not overridden."            */
    /* v3.0 peers see zeroes here and ignore them safely.                    */
    /* Check (flags & UEVR_FLAG_MODIFIERS) && has_modifiers before reading.  */
    /*                                                                       */
    uint8_t  has_modifiers;          /* 1 if profile has uevr_modifiers      */
    uint8_t  _pad5[3];              /* Alignment padding                    */
    float    mod_depth_strength;     /* Override depth curve strength         */
    float    mod_depth_min_floor;    /* Override global minimum depth         */
    float    mod_ads_floor;          /* Override ADS depth floor              */
    float    mod_scope_floor;        /* Override scope depth floor            */
    float    mod_cutscene_floor;     /* Override cutscene depth floor         */
    float    mod_base_power;         /* Override depth base power exponent    */
    float    mod_extra_power;        /* Override depth extra power            */
    float    mod_dead_zone;          /* Override dead zone zoom factor        */
    float    mod_transition_speed;   /* Override smoothing speed multiplier   */
    float    mod_zoom_threshold;     /* Override zoom detection threshold     */
    float    mod_base_fov_override;  /* Override base FOV (0 = auto-detect)  */

    /* ----- v3.2: AIM CORRECTION + COMMANDS (12 bytes) UEVR -> VRto3D ---- */
    float    stereo_aim_correction;  /* Zoom-scaled lateral aim correction    */
    float    stereo_aim_base;        /* Constant lateral aim correction base  */
    uint32_t command_seq;            /* Sequence number for depth commands    */

    /* ----- v3.2: MONITOR MODE (2 bytes) --------------------------------- */
    /*                                                                       */
    /* monitor_mode: set by UEVR when monitor mode is active.                */
    /* is_monitor_display: set by VRto3D when it's a monitor driver          */
    /*   (not a real VR HMD). UEVR reads this to auto-enable bMonitorMode.   */
    /* v3.1 peers see zeroes here and are unaffected.                        */
    /*                                                                       */
    uint8_t  monitor_mode;           /* 1 if UEVR monitor mode is active     */
    uint8_t  is_monitor_display;     /* 1 if VRto3D is outputting to monitor  */

    /* ----- v3.3: STEREO DEPTH HINT (4 bytes) UEVR -> VRto3D ------------ */
    /*                                                                       */
    /* UEVR's stereo_depth (IPD/screen_width calibration value).             */
    /* In monitor mode, VRto3D uses this as base depth instead of            */
    /* config.depth so overlay IPD matches game world stereo separation.     */
    /* 0.0 = not provided (v3.2 peers see zero, use config.depth).           */
    /*                                                                       */
    float    stereo_depth_hint;      /* UEVR stereo depth for overlay IPD    */

    /* ----- RESERVED (6 bytes) ------------------------------------------- */
    uint8_t  reserved[6];            /* Zero-filled, for future fields       */

} UEVR_VRto3D_SharedData;
#pragma pack(pop)

/* Compile-time size check - if this fires, the struct layout is wrong */
#ifndef __cplusplus
_Static_assert(sizeof(UEVR_VRto3D_SharedData) == 256,
    "UEVR_VRto3D_SharedData must be exactly 256 bytes");
#else
static_assert(sizeof(UEVR_VRto3D_SharedData) == 256,
    "UEVR_VRto3D_SharedData must be exactly 256 bytes");
#endif

/* ========================================================================== */
/* HELPERS                                                                     */
/* ========================================================================== */

/* Check if UEVR data is fresh */
static inline int uevr_vrto3d_is_uevr_fresh(const UEVR_VRto3D_SharedData* d, uint64_t now_ms) {
    if (!d || d->magic != UEVR_VRTO3D_MAGIC) return 0;
    if (!d->is_valid) return 0;
    return (now_ms - d->uevr_timestamp) < UEVR_VRTO3D_STALE_MS;
}

/* Check if VRto3D data is fresh */
static inline int uevr_vrto3d_is_vrto3d_fresh(const UEVR_VRto3D_SharedData* d, uint64_t now_ms) {
    if (!d || d->magic != UEVR_VRTO3D_MAGIC) return 0;
    if (!d->vrto3d_connected) return 0;
    return (now_ms - d->vrto3d_timestamp) < UEVR_VRTO3D_STALE_MS;
}

/* Check if profile modifiers are present (v3.1) */
static inline int uevr_vrto3d_has_modifiers(const UEVR_VRto3D_SharedData* d) {
    if (!d) return 0;
    return (d->flags & UEVR_FLAG_MODIFIERS) && d->has_modifiers;
}

#endif /* UEVR_VRTO3D_PROTOCOL_H */
