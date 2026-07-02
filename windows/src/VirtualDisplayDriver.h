#pragma once

#include <string>

bool HasActiveVirtualDisplay();
bool IsProcessElevated();
std::string EnsureVirtualDisplayDriverForExtendedMode();
std::string EnsureVirtualDisplayDriverInstalled();
