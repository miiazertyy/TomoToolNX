#pragma once
// u2net_infer.h
// Runs u2netp background removal using a preprocessed model binary.
// Model binary produced by tools/preprocess_u2net.py.

#include "texture_processor.h"
#include <string>

namespace U2Net {

// Called once per graph node during inference: done nodes processed, total = graph size.
using ProgressCb = void(*)(int done, int total, void* userdata);

// Runs u2netp inference on img, zeroing alpha where background is detected.
// model_path: path to u2netp.bin on the filesystem (e.g. "romfs:/u2netp.bin")
// cb / userdata: optional per-node progress callback for animated UI.
// Returns empty string on success, error message on failure.
std::string RemoveBackground(RgbaImage& img, const char* model_path,
                              ProgressCb cb = nullptr, void* userdata = nullptr);

} // namespace U2Net
