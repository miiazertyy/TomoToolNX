#pragma once

#include <functional>
#include <string>

// MTP (Media Transfer Protocol) file-sharing server.
// Exposes the SD card as a USB storage device when the Switch is connected
// to a PC. Uses libhaze (vendored in libs/libhaze/ — run 'make setup' once).

namespace MtpServer {

using LogFn = std::function<void(const std::string& msg, bool ok)>;

// Set a log callback before calling Init() to receive status messages.
void SetLogCallback(LogFn fn);

// Start the MTP server. Returns true on success.
// Must be called once; safe to call even if already running (returns false).
bool Init();

// Returns true while the MTP server is active.
bool IsRunning();

// Returns true when a PC has an active MTP session open.
bool IsSessionOpen();

// Call once per second from the main loop to detect unclean disconnects.
// Windows often drops USB without sending CloseSession; this catches that.
void PollUsbState();

// Stop the MTP server and release USB resources.
void Exit();

} // namespace MtpServer
