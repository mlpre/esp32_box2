#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DEVICE_CONTROLS_DEBOUNCE_MS 30U
#define DEVICE_CONTROLS_LONG_PRESS_MS 2000U
#define DEVICE_CONTROLS_Q_RELEASE_MS 100U

typedef enum
{
    DEVICE_CONTROL_NONE = 0,
    DEVICE_CONTROL_M_SHORT_RELEASE = 1U << 0,
    DEVICE_CONTROL_M_LONG_PRESS = 1U << 1,
    DEVICE_CONTROL_Q_LONG_PRESS = 1U << 2,
    DEVICE_CONTROL_Q_LONG_RELEASED = 1U << 3,
} device_control_event_t;

typedef struct
{
    bool candidate;
    bool pressed;
    bool armed;
    bool long_sent;
    uint32_t candidate_at;
    uint32_t pressed_at;
} device_control_key_t;

typedef struct
{
    device_control_key_t middle;
    device_control_key_t q;
    bool q_pending;
    bool q_releasing;
    uint32_t q_released_at;
} device_controls_t;

/* Inputs mean physically pressed, independent of electrical key polarity.
 * A key already held at initialization is ignored until its first release.
 * Initialize once, then preserve this state when entering/leaving standby so
 * releasing the M shutdown gesture cannot be interpreted as a wake gesture.
 */
void device_controls_init(device_controls_t *controls, bool m_pressed,
                          bool q_pressed, uint32_t now_ms);

/* Poll with monotonic milliseconds (uint32_t wraparound is supported).
 * Each event is emitted once. M short press is emitted only upon release;
 * M long press never also emits a short press. The application maps M short to
 * mute while awake and wake while in standby, and M long to standby.
 * Q long press prompts release; Q long released waits another 100 ms after
 * debounced release before the application may save its mode and restart.
 * A re-press during that interval delays Q completion until the next release.
 */
uint32_t device_controls_update(device_controls_t *controls, bool m_pressed,
                               bool q_pressed, uint32_t now_ms);

/* UI helper: hold progress 0..100 for &controls->middle or &controls->q.
 * Stable pressed state is available directly in those public key structures.
 */
unsigned int device_controls_hold_percent(const device_control_key_t *key,
                                         uint32_t now_ms);

#ifdef __cplusplus
}
#endif
