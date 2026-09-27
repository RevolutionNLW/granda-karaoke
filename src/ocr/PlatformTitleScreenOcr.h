#pragma once

#include "library/TitleScreenText.h"

#include <memory>

// The local OCR engine for this platform, or nullptr when there is none.
// macOS uses Apple Vision. Windows (Windows.Media.Ocr) is not implemented
// yet: results can be imported there from a Mac-generated export instead.
std::unique_ptr<TitleScreenOcrEngine> createPlatformTitleScreenOcr();
