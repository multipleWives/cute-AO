#pragma once

#ifndef CUTE_AO_AE_H
#define CUTE_AO_AE_H

typedef unsigned char u_char;
typedef unsigned short u_short;
typedef unsigned short u_int16;
typedef unsigned long u_long;
typedef short int int16;

#define PF_DEEP_COLOR_AWARE 1

#include "AEConfig.h"

#ifdef AE_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

#include "entry.h"
#include "AE_Effect.h"
#include "AE_EffectCB.h"
#include "AE_EffectCBSuites.h"
#include "AE_Macros.h"
#include "AE_GeneralPlug.h"
#include "Param_Utils.h"

#define CUTE_AO_MAJOR_VERSION 1
#define CUTE_AO_MINOR_VERSION 0
#define CUTE_AO_BUG_VERSION 0
#define CUTE_AO_STAGE_VERSION PF_Stage_RELEASE
#define CUTE_AO_BUILD_VERSION 1

enum {
    NEO_INPUT = 0,
    NEO_DEPTH,
    NEO_INTENSITY,
    NEO_THICKNESS,
    NEO_RADIUS,
    NEO_RENDER_SCALE,
    NEO_RAY_STEPS,
    NEO_SAMPLE_COUNT,
    NEO_FADE_START,
    NEO_FADE_END,
    NEO_BLUR_SHARPNESS,
    NEO_ENABLE_TAA,
    NEO_TEMPORAL_STABILITY,
    NEO_ENABLE_DEPTH_MULTIPLIER,
    NEO_DEPTH_MULTIPLIER,
    NEO_DEPTH_THRESHOLD,
    NEO_FOV,
    NEO_OCCLUSION_COLOR,
    NEO_VIEW_MODE,
    NEO_DEPTH_ENCODING,
    NEO_DEPTH_SMOOTHING,
    NEO_NUM_PARAMS
};

enum {
    NEO_DEPTH_DISK_ID = 1,
    NEO_INTENSITY_DISK_ID,
    NEO_THICKNESS_DISK_ID,
    NEO_RADIUS_DISK_ID,
    NEO_RENDER_SCALE_DISK_ID,
    NEO_RAY_STEPS_DISK_ID,
    NEO_SAMPLE_COUNT_DISK_ID,
    NEO_FADE_START_DISK_ID,
    NEO_FADE_END_DISK_ID,
    NEO_BLUR_SHARPNESS_DISK_ID,
    NEO_ENABLE_TAA_DISK_ID,
    NEO_TEMPORAL_STABILITY_DISK_ID,
    NEO_ENABLE_DEPTH_MULTIPLIER_DISK_ID,
    NEO_DEPTH_MULTIPLIER_DISK_ID,
    NEO_DEPTH_THRESHOLD_DISK_ID,
    NEO_FOV_DISK_ID,
    NEO_OCCLUSION_COLOR_DISK_ID,
    NEO_VIEW_MODE_DISK_ID,
    NEO_DEPTH_ENCODING_DISK_ID,
    NEO_DEPTH_SMOOTHING_DISK_ID
};

extern "C" {
DllExport PF_Err EffectMain(
    PF_Cmd cmd,
    PF_InData* in_data,
    PF_OutData* out_data,
    PF_ParamDef* params[],
    PF_LayerDef* output,
    void* extra);
}

#endif
