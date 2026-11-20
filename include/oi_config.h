#pragma once

#include <stdbool.h>

// Settings read from /data/GoldHEN/OrbisInstrumentalizer.ini when the game starts.
// Anything missing or invalid keeps its default.
typedef struct {
    int gh_tilt_threshold;   // Guitar Hero guitars: accelerometer value that counts as tilted
    int rb_tilt_threshold;   // Rock Band guitars: tilt axis value that counts as tilted
    int tilt_hold_ms;        // how long the tilt has to stay past the threshold to count
    int whammy_deadzone;     // whammy values below this (0-255) read as released
    int drum_min_velocity;   // Guitar Hero drum hits softer than this (1-127) are ignored
    bool calibration_log;    // log raw tilt/whammy/velocity ranges every few seconds
    bool latency_log;        // log input latency statistics every few seconds
} OIConfig;

extern OIConfig oi_config;

// loads the settings file, writing one with the defaults if there isn't one yet
void OIConfigLoad(void);
