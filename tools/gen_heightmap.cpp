// Procedural heightmap generator — produces a 256x256 grayscale PNG
// with MOBA-appropriate terrain: gentle hills, river valleys, flat bases.

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

static float smoothstep(float edge0, float edge1, float x) {
  float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
  return t * t * (3.0f - 2.0f * t);
}

static float noise2D(int x, int y) {
  int n = x + y * 57;
  n = (n << 13) ^ n;
  return 1.0f - ((n * (n * n * 15731 + 789221) + 1376312589) & 0x7FFFFFFF) /
                    1073741824.0f;
}

static float lerp(float a, float b, float t) { return a + (b - a) * t; }

static float smoothNoise(float x, float y) {
  int ix = static_cast<int>(std::floor(x));
  int iy = static_cast<int>(std::floor(y));
  float fx = x - ix;
  float fy = y - iy;
  fx = smoothstep(0, 1, fx);
  fy = smoothstep(0, 1, fy);

  float v00 = noise2D(ix, iy);
  float v10 = noise2D(ix + 1, iy);
  float v01 = noise2D(ix, iy + 1);
  float v11 = noise2D(ix + 1, iy + 1);

  return lerp(lerp(v00, v10, fx), lerp(v01, v11, fx), fy);
}

static float fbm(float x, float y, int octaves = 4) {
  float value = 0.0f;
  float amplitude = 1.0f;
  float frequency = 1.0f;
  float maxAmp = 0.0f;
  for (int i = 0; i < octaves; ++i) {
    value += smoothNoise(x * frequency, y * frequency) * amplitude;
    maxAmp += amplitude;
    amplitude *= 0.5f;
    frequency *= 2.0f;
  }
  return value / maxAmp;
}

int main() {
  const int W = 256, H = 256;
  std::vector<uint8_t> pixels(W * H);

  for (int y = 0; y < H; ++y) {
    for (int x = 0; x < W; ++x) {
      float fx = static_cast<float>(x) / W;
      float fy = static_cast<float>(y) / H;

      // Base terrain noise
      float h = fbm(fx * 8.0f + 3.7f, fy * 8.0f + 1.3f, 5);
      h = h * 0.5f + 0.5f; // map to [0, 1]

      // River valley: diagonal from bottom-left to top-right
      float riverDist = std::abs((fx - fy));
      float riverMask = smoothstep(0.0f, 0.08f, riverDist);
      h *= riverMask;
      // Add slight river noise
      float riverNoise = fbm(fx * 16.0f, fy * 16.0f, 3) * 0.02f;
      h += riverNoise * (1.0f - riverMask);

      // Flatten base areas (Blue: bottom-left corner, Red: top-right)
      float blueBaseDist = std::sqrt(fx * fx + fy * fy);
      float redBaseDist =
          std::sqrt((1.0f - fx) * (1.0f - fx) + (1.0f - fy) * (1.0f - fy));
      float baseFlatRadius = 0.12f;
      float baseFadeRadius = 0.2f;

      float blueFlatten =
          1.0f - smoothstep(baseFlatRadius, baseFadeRadius, blueBaseDist);
      float redFlatten =
          1.0f - smoothstep(baseFlatRadius, baseFadeRadius, redBaseDist);

      h = lerp(h, 0.15f, blueFlatten);
      h = lerp(h, 0.15f, redFlatten);

      // Clamp and scale to 0-255
      h = std::clamp(h, 0.0f, 1.0f);
      pixels[y * W + x] = static_cast<uint8_t>(h * 255.0f);
    }
  }

  const char *outPath = "heightmap.png";
  if (argc > 1)
    outPath = argv[1];

  if (!stbi_write_png(outPath, W, H, 1, pixels.data(), W)) {
    std::fprintf(stderr, "Failed to write heightmap PNG\n");
    return 1;
  }

  std::printf("Generated %dx%d heightmap: %s\n", W, H, outPath);
  return 0;
}
