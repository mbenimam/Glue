#pragma once
// Image files <-> RGBA8 pixel arrays. Pure CPU code (stb): independent of any graphics API.
#include <cstdint>
#include <string>
#include <vector>

namespace glue::gfx {

// Decodes png/jpg/...; rows are returned bottom row first (the convention of render-target readback).
bool loadImageRgba8(const std::string& path, int& width, int& height, std::vector<uint8_t>& pixels);
// Decodes a Radiance .hdr (or any format stb can read) to linear float RGBA, bottom row first.
// Alpha is 1. Used for environment maps: values above 1 are kept.
bool loadImageRgbaF32(const std::string& path, int& width, int& height, std::vector<float>& pixels);
// Writes a PNG from bottom-row-first RGBA8 pixels; alpha is forced to 255.
bool savePngRgba8(const std::string& path, int width, int height, std::vector<uint8_t> pixels);

} // namespace glue::gfx
