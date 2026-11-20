// Offscreen PNG renders of the decoration artwork (CI inspection).
#pragma once
#include <string>

#include "platform.h"

namespace snow::win {

// Writes <dir>\decor-<theme>.png for every theme; returns 0 when all succeeded.
// COM must be initialised on the calling thread.
int RenderDecorSnapshots(const std::wstring& dir, const char* buildId);

}  // namespace snow::win
