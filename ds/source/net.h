// Wi-Fi + HTTP for the QuizFabric DS client.
//
// Wi-Fi connects with the FIRMWARE's saved settings (DSi System Settings,
// or the DS WFC config): Wifi_AutoConnect. Poll net_wifi_state() each frame;
// the app shows a "Connecting to Wi-Fi..." screen until it reports 1.
//
// HTTP is a blocking GET that polls with FIONREAD (dswifi has no select()).
// The progress callback runs per received chunk and may return false to
// abort, so the caller can poll keys inside it for a cancel button.
#pragma once

#include <stdbool.h>

void net_init(void);           // powers the radio and auto-connects
int  net_wifi_state(void);     // 0 = connecting, 1 = associated, -1 = failed
void net_disconnect(void);
const char* net_ip(void);      // dotted quad, or "0.0.0.0"

unsigned net_ticks_ms(void);   // free-running millisecond counter

// Waits for vertical blank by POLLING the VCOUNT register (no IRQs - the
// calico WFI-based swiWaitForVBlank misbehaves under melonDS and needs the
// VBLANK IRQ enabled). Also advances net_ticks_ms by one frame (~16ms).
void net_wait_vblank(void);

typedef bool (*NetProgress)(void* user, int received, int total);

// Returns the body size, or -1 on failure, -2 when cancelled. `out` is
// NUL-terminated on success. Progress is skipped during the header phase.
int net_http_get(const char* host, int port, const char* path,
                 char* out, int outsz, int timeoutMs,
                 NetProgress progress, void* user);

// Percent-encodes `in` for a URL path/query (spaces -> %20, others passed
// through like the 3DS client). Returns bytes written, always NUL-terminated.
int net_urlencode(const char* in, char* out, int outsz);
