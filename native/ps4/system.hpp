#pragma once

#include <cstdint>

namespace ps4::system {

// Opens /data/PS4React/<name> (or /temp0 fallback) and mirrors log() there.
// Call first, before any SDK call that could fault.
void start_log(const char* name = "react.log");

// Writes one line to stdout, the PS4 kernel log and the log file.
void log(const char* format, ...) __attribute__((format(printf, 1, 2)));

// Loads the internal system modules a normal title needs before using VideoOut,
// Pad, UserService and AudioOut. Safe to call more than once.
bool initialize_services();

// Monotonic microseconds from the kernel process clock.
std::int64_t monotonic_us();

void sleep_us(std::uint32_t microseconds);

// Hides the system splash screen after the first presented frame.
void hide_splash();

[[noreturn]] void park();

// Tries the system close request while the app still owns its display and
// focus. Returns true when the shell accepted it.
bool request_close();

// Navigates to the dashboard and parks. Used when the close request is
// refused; the shell suspends the app in the background.
[[noreturn]] void go_home();

// Fallback used after teardown: navigate home and park.
[[noreturn]] void exit_app();

} // namespace ps4::system
