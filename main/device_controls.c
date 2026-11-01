#include "device_controls.h"

enum
{
    KEY_SHORT = 1U << 0,
    KEY_LONG = 1U << 1,
};

static void init_key(device_control_key_t *key, bool pressed, uint32_t now)
{
    *key = (device_control_key_t){
        .candidate = pressed,
        .pressed = pressed,
        .armed = !pressed,
        .candidate_at = now,
        .pressed_at = now,
    };
}

void device_controls_init(device_controls_t *controls, bool m_pressed,
                          bool q_pressed, uint32_t now_ms)
{
    if (!controls)
    {
        return;
    }
    *controls = (device_controls_t){0};
    init_key(&controls->middle, m_pressed, now_ms);
    init_key(&controls->q, q_pressed, now_ms);
}

static uint32_t update_key(device_control_key_t *key, bool raw, uint32_t now)
{
    uint32_t events = 0;
    if (raw != key->candidate)
    {
        key->candidate = raw;
        key->candidate_at = now;
    }
    if (key->pressed != raw &&
        (uint32_t)(now - key->candidate_at) >= DEVICE_CONTROLS_DEBOUNCE_MS)
    {
        key->pressed = raw;
        if (raw)
        {
            key->pressed_at = now;
            key->long_sent = false;
        }
        else if (!key->armed)
        {
            // A held boot/wake key must first become idle before it can act.
            key->armed = true;
            key->long_sent = false;
        }
        else if (!key->long_sent)
        {
            // A slow polling loop may miss the long threshold before release.
            // Classify using release onset, excluding its 30 ms debounce time.
            if ((uint32_t)(key->candidate_at - key->pressed_at) >=
                DEVICE_CONTROLS_LONG_PRESS_MS)
            {
                key->long_sent = true;
                events |= KEY_LONG;
            }
            else
            {
                events |= KEY_SHORT;
            }
        }
    }
    if (key->armed && key->pressed && raw && !key->long_sent &&
        (uint32_t)(now - key->pressed_at) >= DEVICE_CONTROLS_LONG_PRESS_MS)
    {
        key->long_sent = true;
        events |= KEY_LONG;
    }
    return events;
}

unsigned int device_controls_hold_percent(const device_control_key_t *key,
                                         uint32_t now_ms)
{
    if (!key || !key->armed || !key->pressed)
    {
        return 0;
    }
    uint32_t elapsed = (uint32_t)(now_ms - key->pressed_at);
    return elapsed >= DEVICE_CONTROLS_LONG_PRESS_MS
               ? 100U : elapsed * 100U / DEVICE_CONTROLS_LONG_PRESS_MS;
}

uint32_t device_controls_update(device_controls_t *controls, bool m_pressed,
                               bool q_pressed, uint32_t now_ms)
{
    if (!controls)
    {
        return DEVICE_CONTROL_NONE;
    }
    uint32_t events = DEVICE_CONTROL_NONE;
    uint32_t middle = update_key(&controls->middle, m_pressed, now_ms);
    if (middle & KEY_SHORT)
    {
        events |= DEVICE_CONTROL_M_SHORT_RELEASE;
    }
    if (middle & KEY_LONG)
    {
        events |= DEVICE_CONTROL_M_LONG_PRESS;
    }

    uint32_t q = update_key(&controls->q, q_pressed, now_ms);
    if ((q & KEY_LONG) && !controls->q_pending)
    {
        controls->q_pending = true;
        events |= DEVICE_CONTROL_Q_LONG_PRESS;
    }
    if (q_pressed || controls->q.pressed)
    {
        // Raw re-press cancels the release timer immediately, even before its
        // debounce completes, so a USB reboot cannot occur with Q held.
        controls->q_releasing = false;
    }
    else if (controls->q_pending)
    {
        if (!controls->q_releasing)
        {
            controls->q_releasing = true;
            controls->q_released_at = now_ms;
        }
        if ((uint32_t)(now_ms - controls->q_released_at) >=
            DEVICE_CONTROLS_Q_RELEASE_MS)
        {
            controls->q_pending = false;
            controls->q_releasing = false;
            events |= DEVICE_CONTROL_Q_LONG_RELEASED;
        }
    }
    return events;
}
