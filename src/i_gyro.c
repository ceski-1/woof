//----------------------------------------------------------------------------
//
//  Copyright (c) 2018-2021 Julian "Jibb" Smart
//  Copyright (c) 2021-2024 Nicolas Lessard
//  Copyright (c) 2024 ceski
//
//  Permission is hereby granted, free of charge, to any person obtaining
//  a copy of this software and associated documentation files (the
//  "Software"), to deal in the Software without restriction, including
//  without limitation the rights to use, copy, modify, merge, publish,
//  distribute, sublicense, and/or sell copies of the Software, and to
//  permit persons to whom the Software is furnished to do so, subject to
//  the following conditions:
//
//  The above copyright notice and this permission notice shall be
//  included in all copies or substantial portions of the Software.
//
//  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
//  EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
//  MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
//  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
//  CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
//  TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
//  SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
//
//----------------------------------------------------------------------------

#include <string.h>

#include "i_gyro.h"
#include "i_printf.h"
#include "g_game.h"
#include "i_gamepad.h"
#include "i_timer.h"
#include "i_video.h"
#include "m_config.h"
#include "m_input.h"

#define CALBITS 24 // 8.24 fixed-point.
#define CALUNIT (1 << CALBITS)
#define CAL2CFG(x) ((x) * (double)CALUNIT)
#define CFG2CAL(x) ((x) / (double)CALUNIT)

#define DEFAULT_ACCEL 1.0f // Default accelerometer magnitude.

#define SMOOTH_HALF_TIME 4.0f // 1/0.25

#define SHAKINESS_MAX_THRESH 0.4f
#define SHAKINESS_MIN_THRESH 0.01f

#define COR_STILL_RATE 1.0f
#define COR_SHAKY_RATE 0.1f

#define COR_GYRO_FACTOR 0.1f
#define COR_GYRO_MIN_THRESH 0.05f
#define COR_GYRO_MAX_THRESH 0.25f

#define COR_MIN_SPEED 0.01f

#define SIDE_THRESH 0.125f

#define RELAX_FACTOR_45 1.4142135f // 45 degrees
#define RELAX_FACTOR_60 2.0943952f // 60 degrees

#define SGNF2(x) ((x) < 0.0f ? -1.0f : 1.0f) // Doesn't return zero.

typedef enum
{
    SPACE_LOCAL_TURN,
    SPACE_LOCAL_LEAN,
    SPACE_PLAYER_TURN,
    SPACE_PLAYER_LEAN,

    NUM_SPACES
} space_t;

boolean gyro_enable;
static int gyro_space;
static int gyro_button_action;
static int gyro_stick_action;
static int gyro_turn_sensitivity;
static int gyro_look_sensitivity;
static int gyro_acceleration;
static int gyro_accel_min_threshold;
static int gyro_accel_max_threshold;
static int gyro_smooth_threshold;
static int gyro_smooth_time;
static int gyro_tightening;
static fixed_t gyro_calibration_a;
static fixed_t gyro_calibration_x;
static fixed_t gyro_calibration_y;
static fixed_t gyro_calibration_z;

float gyro_axes[NUM_GYRO_AXES];
motion_t motion;

typedef struct
{
    gyro_calibration_state_t state;
    int start_time;
    int finish_time;
    int accel_count;
    int gyro_count;
    float accel_sum;
    vec gyro_sum;
} calibration_t;

calibration_t cal;

gyro_calibration_state_t I_GetGyroCalibrationState(void)
{
    return cal.state;
}

boolean I_DefaultGyroCalibration(void)
{
    return (!gyro_calibration_a && !gyro_calibration_x && !gyro_calibration_y
            && !gyro_calibration_z);
}

void I_ClearGyroCalibration(void)
{
    memset(&cal, 0, sizeof(cal));
    motion.accel_magnitude = DEFAULT_ACCEL;
    motion.gyro_offset = (vec){0.0f, 0.0f, 0.0f};
    gyro_calibration_a = 0;
    gyro_calibration_x = 0;
    gyro_calibration_y = 0;
    gyro_calibration_z = 0;
}

static void LoadCalibration(void)
{
    motion.accel_magnitude = CFG2CAL(gyro_calibration_a) + DEFAULT_ACCEL;
    motion.gyro_offset.x = CFG2CAL(gyro_calibration_x);
    motion.gyro_offset.y = CFG2CAL(gyro_calibration_y);
    motion.gyro_offset.z = CFG2CAL(gyro_calibration_z);

    I_Printf(VB_DEBUG, "Gyro calibration loaded: a: %f x: %f y: %f z: %f",
             motion.accel_magnitude, motion.gyro_offset.x, motion.gyro_offset.y,
             motion.gyro_offset.z);
}

static void SaveCalibration(void)
{
    gyro_calibration_a = CAL2CFG(motion.accel_magnitude - DEFAULT_ACCEL);
    gyro_calibration_x = CAL2CFG(motion.gyro_offset.x);
    gyro_calibration_y = CAL2CFG(motion.gyro_offset.y);
    gyro_calibration_z = CAL2CFG(motion.gyro_offset.z);
}

static void ProcessAccelCalibration(void)
{
    cal.accel_count++;
    cal.accel_sum += vec_length(&motion.accel);
}

static void ProcessGyroCalibration(void)
{
    cal.gyro_count++;
    cal.gyro_sum = vec_add(&cal.gyro_sum, &motion.gyro);
}

static void PostProcessCalibration(void)
{
    if (!cal.accel_count || !cal.gyro_count)
    {
        return;
    }

    motion.accel_magnitude = cal.accel_sum / cal.accel_count;
    motion.accel_magnitude = BETWEEN(0.0f, 2.0f, motion.accel_magnitude);

    motion.gyro_offset = vec_scale(&cal.gyro_sum, 1.0f / cal.gyro_count);
    motion.gyro_offset = vec_clamp(-1.0f, 1.0f, &motion.gyro_offset);

    SaveCalibration();

    I_Printf(VB_DEBUG,
             "Gyro calibration updated: a: %f x: %f y: %f z: %f accel_count: "
             "%d gyro_count: %d",
             motion.accel_magnitude, motion.gyro_offset.x, motion.gyro_offset.y,
             motion.gyro_offset.z, cal.accel_count, cal.gyro_count);
}

void I_UpdateGyroCalibrationState(void)
{
    switch (cal.state)
    {
        case GYRO_CALIBRATION_INACTIVE:
            if (!motion.calibrating)
            {
                motion.calibrating = true;
                I_ClearGyroCalibration();
                cal.state = GYRO_CALIBRATION_ACTIVE;
                cal.start_time = I_GetTimeMS();
            }
            break;

        case GYRO_CALIBRATION_ACTIVE:
            if (I_GetTimeMS() - cal.start_time > 1000)
            {
                motion.calibrating = false;
                PostProcessCalibration();
                cal.state = GYRO_CALIBRATION_COMPLETE;
                cal.finish_time = I_GetTimeMS();
            }
            break;

        case GYRO_CALIBRATION_COMPLETE:
            if (I_GetTimeMS() - cal.finish_time > 1000)
            {
                cal.state = GYRO_CALIBRATION_INACTIVE;
            }
            break;
    }
}

static boolean GyroActive(void)
{
    // Camera stick action has priority over gyro button action.
    if (motion.stick_moving)
    {
        switch (motion.stick_action)
        {
            case ACTION_DISABLE:
                return false;

            case ACTION_ENABLE:
                return true;

            default:
                break;
        }
    }

    switch (motion.button_action)
    {
        case ACTION_DISABLE:
            return !M_InputGameActive(input_gyro);

        case ACTION_ENABLE:
            return M_InputGameActive(input_gyro);

        default:
            break;
    }

    return true;
}

//
// I_CalcGyroAxes
//

void I_CalcGyroAxes(boolean strafe)
{
    if (GyroActive())
    {
        gyro_axes[GYRO_TURN] = !strafe ? RAD2TIC(gyro_axes[GYRO_TURN]) : 0.0f;

        if (padlook)
        {
            gyro_axes[GYRO_LOOK] = RAD2TIC(gyro_axes[GYRO_LOOK]);

            if (correct_aspect_ratio)
            {
                gyro_axes[GYRO_LOOK] /= 1.2f;
            }
        }
        else
        {
            gyro_axes[GYRO_LOOK] = 0.0f;
        }
    }
    else
    {
        gyro_axes[GYRO_TURN] = 0.0f;
        gyro_axes[GYRO_LOOK] = 0.0f;
    }
}

static void (*AccelerateGyro)(void);

static void AccelerateGyro_Skip(void)
{
    motion.gyro.x *= motion.min_pitch_sens;
    motion.gyro.y *= motion.min_yaw_sens;
}

static void AccelerateGyro_Full(void)
{
    float magnitude = LENGTH_F(motion.gyro.x, motion.gyro.y);
    magnitude -= motion.accel_min_thresh;
    magnitude = MAX(0.0f, magnitude);

    const float denom = motion.accel_max_thresh - motion.accel_min_thresh;
    float accel;
    if (denom <= 0.0f)
    {
        accel = magnitude > 0.0f ? 1.0f : 0.0f;
    }
    else
    {
        accel = magnitude / denom;
        accel = BETWEEN(0.0f, 1.0f, accel);
    }
    const float no_accel = (1.0f - accel);

    motion.gyro.x *=
        (motion.min_pitch_sens * no_accel + motion.max_pitch_sens * accel);

    motion.gyro.y *=
        (motion.min_yaw_sens * no_accel + motion.max_yaw_sens * accel);
}

static void (*TightenGyro)(void);

static void TightenGyro_Skip(void)
{
    // no-op
}

static void TightenGyro_Full(void)
{
    const float magnitude = LENGTH_F(motion.gyro.x, motion.gyro.y);

    if (magnitude < motion.tightening)
    {
        const float factor = magnitude / motion.tightening;
        motion.gyro.x *= factor;
        motion.gyro.y *= factor;
    }
}

static void GetSmoothedGyro(float smooth_factor, float *smooth_pitch,
                            float *smooth_yaw)
{
    motion.index = (motion.index + (NUM_SAMPLES - 1)) % NUM_SAMPLES;
    motion.pitch_samples[motion.index] = motion.gyro.x * smooth_factor;
    motion.yaw_samples[motion.index] = motion.gyro.y * smooth_factor;

    const float delta_time =
        BETWEEN(1.0e-6f, motion.smooth_time, motion.delta_time);
    int max_samples = lroundf((float)motion.smooth_time / delta_time);
    max_samples = BETWEEN(1, NUM_SAMPLES, max_samples);

    *smooth_pitch = motion.pitch_samples[motion.index] / max_samples;
    *smooth_yaw = motion.yaw_samples[motion.index] / max_samples;

    for (int i = 1; i < max_samples; i++)
    {
        const int index = (motion.index + i) % NUM_SAMPLES;
        *smooth_pitch += motion.pitch_samples[index] / max_samples;
        *smooth_yaw += motion.yaw_samples[index] / max_samples;
    }
}

static float GetRawFactorGyro(float magnitude)
{
    const float denom = motion.upper_smooth - motion.lower_smooth;

    if (denom <= 0.0f)
    {
        return (magnitude < motion.lower_smooth ? 0.0f : 1.0f);
    }
    else
    {
        const float raw_factor = (magnitude - motion.lower_smooth) / denom;
        return BETWEEN(0.0f, 1.0f, raw_factor);
    }
}

static void (*SmoothGyro)(void);

static void SmoothGyro_Skip(void)
{
    // no-op
}

static void SmoothGyro_Full(void)
{
    const float magnitude = LENGTH_F(motion.gyro.x, motion.gyro.y);
    const float raw_factor = GetRawFactorGyro(magnitude);
    const float raw_pitch = motion.gyro.x * raw_factor;
    const float raw_yaw = motion.gyro.y * raw_factor;

    const float smooth_factor = 1.0f - raw_factor;
    float smooth_pitch, smooth_yaw;
    GetSmoothedGyro(smooth_factor, &smooth_pitch, &smooth_yaw);

    motion.gyro.x = raw_pitch + smooth_pitch;
    motion.gyro.y = raw_yaw + smooth_yaw;
}

//
// Gyro Space
//
// Based on gyro space implementations by Julian "Jibb" Smart.
// http://gyrowiki.jibbsmart.com/blog:player-space-gyro-and-alternatives-explained
//

static void (*ApplyGyroSpace)(void);

static void ApplyGyroSpace_LocalTurn(void)
{
    // no-op
}

static void ApplyGyroSpace_LocalLean(void)
{
    motion.gyro.y = -motion.gyro.z;
}

static void ApplyGyroSpace_PlayerTurn(void)
{
    const vec grav_norm = vec_normalize(&motion.gravity);
    const float world_yaw =
        motion.gyro.y * grav_norm.y + motion.gyro.z * grav_norm.z;

    const float world_part = fabsf(world_yaw) * RELAX_FACTOR_60;
    const float gyro_part = LENGTH_F(motion.gyro.y, motion.gyro.z);

    motion.gyro.y = -SGNF2(world_yaw) * MIN(world_part, gyro_part);
}

static void ApplyGyroSpace_PlayerLean(void)
{
    // Controller orientation info for smoothing over boundaries.
    const vec grav_norm = vec_normalize(&motion.gravity);
    const float flatness = fabsf(grav_norm.y);
    const float upness = fabsf(grav_norm.z);
    float side_reduction = (MAX(flatness, upness) - SIDE_THRESH) / SIDE_THRESH;
    side_reduction = BETWEEN(0.0f, 1.0f, side_reduction);

    // Project local pitch axis onto gravity plane.
    const float grav_dot_pitch_axis = grav_norm.x;
    const vec grav_norm_scaled = vec_scale(&grav_norm, grav_dot_pitch_axis);
    vec pitch_vector = (vec){1.0f, 0.0f, 0.0f};
    pitch_vector = vec_subtract(&pitch_vector, &grav_norm_scaled);

    // Normalize and ignore zero vector (pitch and gravity are parallel).
    if (!is_zero_vec(&pitch_vector))
    {
        vec roll_vector = vec_crossproduct(&pitch_vector, &grav_norm);

        if (!is_zero_vec(&roll_vector))
        {
            roll_vector = vec_normalize(&roll_vector);

            const float world_roll =
                motion.gyro.y * roll_vector.y + motion.gyro.z * roll_vector.z;

            const float world_part = fabsf(world_roll) * RELAX_FACTOR_45;
            const float gyro_part = LENGTH_F(motion.gyro.y, motion.gyro.z);

            motion.gyro.y = -SGNF2(world_roll) * MIN(world_part, gyro_part)
                            * side_reduction;
        }
    }
}

//
// CalcGravity
//
// Based on sensor fusion implementation by Julian "Jibb" Smart.
// http://gyrowiki.jibbsmart.com/blog:finding-gravity-with-sensor-fusion
//

static void (*CalcGravityVector)(void);

static void CalcGravityVector_Skip(void)
{
    // no-op
}

static void CalcGravityVector_Full(void)
{
    // Convert gyro input to reverse rotation.
    const float angle_speed = vec_length(&motion.gyro);
    const float angle = angle_speed * motion.delta_time;
    const vec negative_gyro = vec_negative(&motion.gyro);
    const quat reverse_rotation = angle_axis(angle, &negative_gyro);

    // Rotate gravity vector.
    motion.gravity = vec_rotate(&motion.gravity, &reverse_rotation);

    // Check accelerometer magnitude now.
    const float accel_magnitude = vec_length(&motion.accel);
    if (accel_magnitude <= 0.0f)
    {
        return;
    }
    const vec accel_norm = vec_scale(&motion.accel, 1.0f / accel_magnitude);

    // Shakiness/smoothness.
    motion.smooth_accel = vec_rotate(&motion.smooth_accel, &reverse_rotation);
    const float smooth_factor = exp2f(-motion.delta_time * SMOOTH_HALF_TIME);
    motion.shakiness *= smooth_factor;
    const vec delta_accel = vec_subtract(&motion.accel, &motion.smooth_accel);
    const float delta_accel_magnitude = vec_length(&delta_accel);
    motion.shakiness = MAX(motion.shakiness, delta_accel_magnitude);
    motion.smooth_accel =
        vec_lerp(&motion.accel, &motion.smooth_accel, smooth_factor);

    // Find the difference between gravity and raw acceleration.
    const vec new_gravity = vec_scale(&accel_norm, -motion.accel_magnitude);
    const vec gravity_delta = vec_subtract(&new_gravity, &motion.gravity);
    const vec gravity_direction = vec_normalize(&gravity_delta);

    // Calculate correction rate.
    float still_or_shaky = (motion.shakiness - SHAKINESS_MIN_THRESH)
                           / (SHAKINESS_MAX_THRESH - SHAKINESS_MIN_THRESH);
    still_or_shaky = BETWEEN(0.0f, 1.0f, still_or_shaky);
    float correction_rate =
        COR_STILL_RATE + (COR_SHAKY_RATE - COR_STILL_RATE) * still_or_shaky;

    // Limit correction rate in proportion to gyro rate.
    const float angle_speed_adjusted = angle_speed * COR_GYRO_FACTOR;
    const float correction_limit = MAX(angle_speed_adjusted, COR_MIN_SPEED);
    if (correction_rate > correction_limit)
    {
        const float gravity_delta_magnitude = vec_length(&gravity_delta);
        float close_factor = (gravity_delta_magnitude - COR_GYRO_MIN_THRESH)
                             / (COR_GYRO_MAX_THRESH - COR_GYRO_MIN_THRESH);
        close_factor = BETWEEN(0.0f, 1.0f, close_factor);
        correction_rate = correction_limit
                          + (correction_rate - correction_limit) * close_factor;
    }

    // Apply correction to gravity vector.
    const vec correction =
        vec_scale(&gravity_direction, correction_rate * motion.delta_time);
    if (vec_lengthsquared(&correction) < vec_lengthsquared(&gravity_delta))
    {
        motion.gravity = vec_add(&motion.gravity, &correction);
    }
    else
    {
        motion.gravity = vec_scale(&accel_norm, -motion.accel_magnitude);
    }
}

static void ApplyCalibration(void)
{
    motion.gyro = vec_subtract(&motion.gyro, &motion.gyro_offset);
}

//
// I_UpdateGyroData
//
// Based on gyro implementation by Julian "Jibb" Smart.
// http://gyrowiki.jibbsmart.com/blog:good-gyro-controls-part-1:the-gyro-is-a-mouse
//

void I_UpdateGyroData(const event_t *ev)
{
    motion.delta_time = ev->data1.f;
    motion.gyro.x = ev->data2.f; // pitch
    motion.gyro.y = ev->data3.f; // yaw
    motion.gyro.z = ev->data4.f; // roll

    if (motion.calibrating)
    {
        ProcessGyroCalibration();
    }

    ApplyCalibration();
    CalcGravityVector();
    ApplyGyroSpace();

    SmoothGyro();
    TightenGyro();
    AccelerateGyro();

    gyro_axes[GYRO_LOOK] += motion.gyro.x * motion.delta_time;
    gyro_axes[GYRO_TURN] += motion.gyro.y * motion.delta_time;
}

//
// I_UpdateAccelData
//

void I_UpdateAccelData(const float *data)
{
    motion.accel.x = data[0];
    motion.accel.y = data[1];
    motion.accel.z = data[2];

    if (motion.calibrating)
    {
        ProcessAccelCalibration();
    }
}

static void ResetGyroData(void)
{
    motion.delta_time = 0.0f;
    motion.gyro = (vec){0.0f, 0.0f, 0.0f};
    motion.accel = (vec){0.0f, 0.0f, 0.0f};
}

void I_ResetGyroAxes(void)
{
    memset(gyro_axes, 0, sizeof(gyro_axes));
}

void I_ResetGyro(void)
{
    I_ResetGyroAxes();
    ResetGyroData();
    motion.gravity = (vec){0.0f, 0.0f, 0.0f};
    motion.smooth_accel = (vec){0.0f, 0.0f, 0.0f};
    motion.shakiness = 0.0f;
    motion.stick_moving = false;
    motion.index = 0;
    memset(motion.pitch_samples, 0, sizeof(motion.pitch_samples));
    memset(motion.yaw_samples, 0, sizeof(motion.yaw_samples));
}

void I_RefreshGyroSettings(void)
{
    switch (gyro_space)
    {
        case SPACE_LOCAL_TURN:
            CalcGravityVector = CalcGravityVector_Skip;
            ApplyGyroSpace = ApplyGyroSpace_LocalTurn;
            break;

        case SPACE_LOCAL_LEAN:
            CalcGravityVector = CalcGravityVector_Skip;
            ApplyGyroSpace = ApplyGyroSpace_LocalLean;
            break;

        case SPACE_PLAYER_LEAN:
            CalcGravityVector = CalcGravityVector_Full;
            ApplyGyroSpace = ApplyGyroSpace_PlayerLean;
            break;

        default: // SPACE_PLAYER_TURN
            CalcGravityVector = CalcGravityVector_Full;
            ApplyGyroSpace = ApplyGyroSpace_PlayerTurn;
            break;
    }

    motion.button_action = gyro_button_action;
    motion.stick_action = gyro_stick_action;

    AccelerateGyro =
        (gyro_acceleration > 10) ? AccelerateGyro_Full : AccelerateGyro_Skip;
    motion.min_pitch_sens = gyro_look_sensitivity / 10.0f;
    motion.min_yaw_sens = gyro_turn_sensitivity / 10.0f;
    motion.max_pitch_sens = motion.min_pitch_sens * gyro_acceleration / 10.0f;
    motion.max_yaw_sens = motion.min_yaw_sens * gyro_acceleration / 10.0f;
    motion.accel_min_thresh = gyro_accel_min_threshold * PI_F / 180.0f;
    motion.accel_max_thresh = gyro_accel_max_threshold * PI_F / 180.0f;

    SmoothGyro = (gyro_smooth_threshold && gyro_smooth_time) ? SmoothGyro_Full
                                                             : SmoothGyro_Skip;
    motion.upper_smooth = gyro_smooth_threshold * PI_F / 180.0f;
    motion.lower_smooth = motion.upper_smooth * 0.5f;
    motion.smooth_time = gyro_smooth_time / 1000.0f;

    TightenGyro = gyro_tightening ? TightenGyro_Full : TightenGyro_Skip;
    motion.tightening = gyro_tightening * PI_F / 180.0f;

    LoadCalibration();
}

void I_BindGyroVaribales(void)
{
    BIND_BOOL_GENERAL(gyro_enable, false,
        "Enable gamepad gyro aiming");
    BIND_NUM_GENERAL(gyro_space, SPACE_PLAYER_TURN, 0, NUM_SPACES - 1,
        "Gyro space (0 = Local Turn; 1 = Local Lean; 2 = Player Turn; "
        "3 = Player Lean)");
    BIND_NUM_GENERAL(gyro_button_action,
        ACTION_ENABLE, ACTION_NONE, NUM_ACTIONS - 1,
        "Gyro button action (0 = None; 1 = Disable Gyro; 2 = Enable Gyro)");
    BIND_NUM_GENERAL(gyro_stick_action,
        ACTION_NONE, ACTION_NONE, NUM_ACTIONS - 1,
        "Camera stick action (0 = None; 1 = Disable Gyro; 2 = Enable Gyro)");
    BIND_NUM_GENERAL(gyro_turn_sensitivity, 10, 0, 100,
        "Gyro turn sensitivity (0 = 0.0x; 100 = 10.0x)");
    BIND_NUM_GENERAL(gyro_look_sensitivity, 10, 0, 100,
        "Gyro look sensitivity (0 = 0.0x; 100 = 10.0x)");
    BIND_NUM_GENERAL(gyro_acceleration, 20, 10, 40,
        "Gyro acceleration multiplier (10 = 1.0x; 40 = 4.0x)");
    BIND_NUM(gyro_accel_min_threshold, 0, 0, 200,
        "Lower threshold for applying gyro acceleration [degrees/second]");
    BIND_NUM(gyro_accel_max_threshold, 75, 0, 200,
        "Upper threshold for applying gyro acceleration [degrees/second]");
    BIND_NUM_GENERAL(gyro_smooth_threshold, 5, 0, 50,
        "Gyro smoothing threshold [degrees/second]");
    BIND_NUM(gyro_smooth_time, 125, 0, 500,
        "Gyro smoothing time [milliseconds]");
    BIND_NUM(gyro_tightening, 0, 0, 50,
        "Gyro tightening threshold [degrees/second]");

    BIND_NUM(gyro_calibration_a, 0, UL, UL, "Accelerometer calibration");
    BIND_NUM(gyro_calibration_x, 0, UL, UL, "Gyro calibration (x-axis)");
    BIND_NUM(gyro_calibration_y, 0, UL, UL, "Gyro calibration (y-axis)");
    BIND_NUM(gyro_calibration_z, 0, UL, UL, "Gyro calibration (z-axis)");
}