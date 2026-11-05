#pragma once
#include <cstdint>
#include <string>
#include <vector>
namespace msu2 {
struct Image {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgb; // packed top-down RGB888
};
Image read_ppm(const std::string& path);
void write_ppm(const std::string& path, const Image& image);
Image resize_crop(const Image& image, int width, int height);
Image make_pattern(const std::string& name, unsigned tick = 0);
void to_rgb565(const Image& image, std::vector<std::uint8_t>& output);
}
