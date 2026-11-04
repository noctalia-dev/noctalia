#pragma once

#include <string_view>

namespace wallpaper_shader {

  inline constexpr std::string_view kSamplingSource = R"(
uniform float u_fillMode;
uniform float u_screenWidth;
uniform float u_screenHeight;
uniform vec2 u_spanOffset;
uniform vec2 u_spanMonitorSize;
uniform vec2 u_spanTotalSize;

vec2 calculateWallpaperUV(vec2 uv, float imgWidth, float imgHeight) {
    vec2 transformedUV = uv;

    if (u_fillMode < 0.5) {
        vec2 screenPixel = uv * vec2(u_screenWidth, u_screenHeight);
        vec2 imageOffset = (vec2(u_screenWidth, u_screenHeight) - vec2(imgWidth, imgHeight)) * 0.5;
        vec2 imagePixel = screenPixel - imageOffset;
        transformedUV = imagePixel / vec2(imgWidth, imgHeight);
    } else if (u_fillMode < 1.5) {
        float scale = max(u_screenWidth / imgWidth, u_screenHeight / imgHeight);
        vec2 scaledImageSize = vec2(imgWidth, imgHeight) * scale;
        vec2 offset = (scaledImageSize - vec2(u_screenWidth, u_screenHeight)) / scaledImageSize;
        transformedUV = uv * (vec2(1.0) - offset) + offset * 0.5;
    } else if (u_fillMode < 2.5) {
        float scale = min(u_screenWidth / imgWidth, u_screenHeight / imgHeight);
        vec2 scaledImageSize = vec2(imgWidth, imgHeight) * scale;
        vec2 offset = (vec2(u_screenWidth, u_screenHeight) - scaledImageSize) * 0.5;
        vec2 screenPixel = uv * vec2(u_screenWidth, u_screenHeight);
        vec2 imagePixel = (screenPixel - offset) / scale;
        transformedUV = imagePixel / vec2(imgWidth, imgHeight);
    } else if (u_fillMode < 3.5) {
        // Stretch uses the output UV unchanged.
    } else if (u_fillMode < 4.5) {
        vec2 screenPixel = uv * vec2(u_screenWidth, u_screenHeight);
        transformedUV = screenPixel / vec2(imgWidth, imgHeight);
    } else if (u_spanTotalSize.x <= 0.0 || u_spanTotalSize.y <= 0.0) {
        float scale = max(u_screenWidth / imgWidth, u_screenHeight / imgHeight);
        vec2 scaledImageSize = vec2(imgWidth, imgHeight) * scale;
        vec2 offset = (scaledImageSize - vec2(u_screenWidth, u_screenHeight)) / scaledImageSize;
        transformedUV = uv * (vec2(1.0) - offset) + offset * 0.5;
    } else {
        vec2 imageSize = vec2(imgWidth, imgHeight);
        float scale = max(u_spanTotalSize.x / imageSize.x, u_spanTotalSize.y / imageSize.y);
        vec2 scaledImageSize = imageSize * scale;
        vec2 desktopPixel = u_spanOffset + uv * u_spanMonitorSize;
        vec2 imagePixel = desktopPixel + (scaledImageSize - u_spanTotalSize) * 0.5;
        transformedUV = imagePixel / scaledImageSize;
    }

    return transformedUV;
}

bool wallpaperUVOutside(vec2 uv) {
    return uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0;
}
)";

} // namespace wallpaper_shader
