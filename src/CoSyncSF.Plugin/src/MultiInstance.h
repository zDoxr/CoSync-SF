#pragma once

#include <string>

namespace cosync::plugin
{
    // Starfield refuses to start while another copy is running: during startup
    // it calls FindWindowA("Starfield", nullptr), and if a window is found it
    // calls SetForegroundWindow on it and exits (Starfield.exe 1.16.244,
    // call site 0x14189923C). For local multi-instance testing only, this
    // redirects Starfield.exe's import of user32!FindWindowA so that lookup of
    // the "Starfield" window class reports "not found"; every other lookup is
    // passed through unchanged.
    //
    // Enabled only when the environment variable COSYNC_MULTI_INSTANCE=1 is set
    // (scripts/launch-instances.ps1 sets it). Must run before the game's check,
    // i.e. from SFSEPlugin_Load. Returns true if the patch was installed;
    // `detail` explains the outcome either way.
    bool InstallMultiInstancePatchIfRequested(std::string& detail);
}
