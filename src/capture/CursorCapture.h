#pragma once
#include "capture/ICapturer.h"

// Snapshot of the current system cursor (used before DXGI reports its first pointer shape).
bool captureCurrentCursor(CursorShape& out);
