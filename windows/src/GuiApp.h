#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <atomic>
#include <string>

// Shared state written by the streaming thread; read by the GUI thread.
struct GuiState {
    std::atomic<bool>  connected        {false};
    std::atomic<bool>  streaming        {false};
    std::atomic<int>   fps              {0};
    std::atomic<int>   bitrateKbps      {0};
    std::atomic<int>   capW             {0};
    std::atomic<int>   capH             {0};
    // Set by GUI "Start" button when launched without CLI args (one-click mode).
    std::atomic<bool>  guiStartRequested{false};
    // True once main() has configured the streaming params (GUI-only launch).
    std::atomic<bool>  waitingForStart  {false};
    char               mode[32]    = "USB";   // written once before GUI starts
    char               statusMsg[256] = "Starting…";
};

extern GuiState g_gui;

// Launch a Win32 dashboard window on a background thread.
// Returns immediately; window lives until the process exits.
void GuiLaunch();
