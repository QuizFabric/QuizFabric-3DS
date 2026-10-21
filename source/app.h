// Shared error reporting, owned by main.c.
#pragma once

// Shows the "an error has occurred" screen with the given reason (or
// "Reason: Unknown" when the message is empty). Call when a mode fails
// fatally; the caller should then clean up and let the main loop leave the
// mode - the error screen stays up.
void app_error(const char* fmt, ...);
