#include "terrain/TerrainTextures.h"
#include "renderer/Buffer.h"
#include "renderer/Device.h"
#include "renderer/Image.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace glory {

// ── Utility: layout transitions + buffer-to-image copy ──────────────────────
// (Replicates pattern from Texture.cpp — these are file-scope helpers)

static void transitionLayout(const Device &device, VkImage image,
                             VkImageLayout oldLayout, VkImageLayout newLayout) {
  VkCommandBuffer cmd;
  VkCommandBufferAllocateInfo allocInfo{};
  allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  allocInfo.commandPool = device.getTransferCommandPool();
  allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  allocInfo.commandBufferCount = 1;
  vkAllocateCommandBuffers(device.getDevice(), &allocInfo, &cmd);

  VkCommandBufferBeginInfo beginInfo{};
  beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(cmd, &beginInfo);

  VkImageMemoryBarrier barrier{};
  barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  barrier.oldLayout = oldLayout;
  barrier.newLayout = newLayout;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = image;
  barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

  VkPipelineStageFlags srcStage, dstStage;
  if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED &&
      newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    srcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    dstStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
  } else {
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    srcStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    dstStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
  }

  vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1,
                       &barrier);

  vkEndCommandBuffer(cmd);
  VkSubmitInfo submit{};
  submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &cmd;
  vkQueueSubmit(device.getGraphicsQueue(), 1, &submit, VK_NULL_HANDLE);
  vkQueueWaitIdle(device.getGraphicsQueue());
  vkFreeCommandBuffers(device.getDevice(), device.getTransferCommandPool(), 1,
                       &cmd);
}

static void copyBufToImg(const Device &device, VkBuffer buf, VkImage img,
                         uint32_t w, uint32_t h) {
  VkCommandBuffer cmd;
  VkCommandBufferAllocateInfo allocInfo{};
  allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  allocInfo.commandPool = device.getTransferCommandPool();
  allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  allocInfo.commandBufferCount = 1;
  vkAllocateCommandBuffers(device.getDevice(), &allocInfo, &cmd);

  VkCommandBufferBeginInfo beginInfo{};
  beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(cmd, &beginInfo);

  VkBufferImageCopy region{};
  region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.imageExtent = {w, h, 1};
  vkCmdCopyBufferToImage(cmd, buf, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                         &region);

  vkEndCommandBuffer(cmd);
  VkSubmitInfo submit{};
  submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &cmd;
  vkQueueSubmit(device.getGraphicsQueue(), 1, &submit, VK_NULL_HANDLE);
  vkQueueWaitIdle(device.getGraphicsQueue());
  vkFreeCommandBuffers(device.getDevice(), device.getTransferCommandPool(), 1,
                       &cmd);
}

/// Upload RGBA pixels to a Texture, setting image + sampler.
static Texture uploadTexture(const Device &device,
                             const std::vector<uint32_t> &pixels, uint32_t w,
                             uint32_t h, VkFormat format) {
  Texture tex;
  // Access private members through the factory pattern used by existing code:
  // We'll create the texture using the same public API pattern.
  // Since Texture doesn't expose a generic "create from pixels" method,
  // we use the approach from createCheckerboard: extend Texture with a new
  // static. But to avoid modifying Texture.h, we create Image and sampler
  // directly here and return a Texture via move semantics.

  // This is the same technique used inside Texture.cpp's static factories.
  // Unfortunately Texture's members are private, so we need to add a public
  // factory. For now, we'll work around by using the existing Texture class's
  // move semantics.
  (void)tex;
  (void)pixels;
  (void)w;
  (void)h;
  (void)format;
  (void)device;
  return tex; // placeholder
}

// ── Noise helpers ───────────────────────────────────────────────────────────

static float noiseHash(int x, int y) {
  int n = x + y * 57;
  n = (n << 13) ^ n;
  return 1.0f - static_cast<float>((n * (n * n * 15731 + 789221) + 1376312589) &
                                   0x7FFFFFFF) /
                    1073741824.0f;
}

static float smoothNoise(float fx, float fy) {
  int ix = static_cast<int>(std::floor(fx));
  int iy = static_cast<int>(std::floor(fy));
  float fracX = fx - static_cast<float>(ix);
  float fracY = fy - static_cast<float>(iy);
  float v00 = noiseHash(ix, iy), v10 = noiseHash(ix + 1, iy);
  float v01 = noiseHash(ix, iy + 1), v11 = noiseHash(ix + 1, iy + 1);
  float i0 = v00 + fracX * (v10 - v00);
  float i1 = v01 + fracX * (v11 - v01);
  return i0 + fracY * (i1 - i0);
}

static float fbm(float x, float y, int octaves = 4) {
  float val = 0.0f, amp = 1.0f, freq = 1.0f, total = 0.0f;
  for (int i = 0; i < octaves; ++i) {
    val += smoothNoise(x * freq, y * freq) * amp;
    total += amp;
    amp *= 0.5f;
    freq *= 2.0f;
  }
  return val / total;
}

static inline uint32_t packRGBA(uint8_t r, uint8_t g, uint8_t b,
                                uint8_t a = 255) {
  return static_cast<uint32_t>(r) | (static_cast<uint32_t>(g) << 8) |
         (static_cast<uint32_t>(b) << 16) | (static_cast<uint32_t>(a) << 24);
}

// ── Texture generation ──────────────────────────────────────────────────────

void TerrainTextures::generate(const Device &device) {
  // ── 1. Grass texture (256×256, SRGB) ────────────────────────────────────
  {
    constexpr uint32_t W = 256, H = 256;
    std::vector<uint32_t> pixels(W * H);
    for (uint32_t y = 0; y < H; ++y) {
      for (uint32_t x = 0; x < W; ++x) {
        float fx = static_cast<float>(x) / 32.0f;
        float fy = static_cast<float>(y) / 32.0f;
        float n1 = fbm(fx + 5.3f, fy + 2.7f, 5);
        float n2 = smoothNoise(fx * 4.0f, fy * 4.0f);
        float noise = n1 * 0.7f + n2 * 0.3f;
        noise = noise * 0.5f + 0.5f;

        uint8_t r = static_cast<uint8_t>(
            std::clamp(40.0f + noise * 60.0f, 0.0f, 255.0f));
        uint8_t g = static_cast<uint8_t>(
            std::clamp(90.0f + noise * 100.0f, 0.0f, 255.0f));
        uint8_t b = static_cast<uint8_t>(
            std::clamp(30.0f + noise * 40.0f, 0.0f, 255.0f));
        pixels[y * W + x] = packRGBA(r, g, b);
      }
    }
    grass = Texture::createFromPixels(device, pixels.data(), W, H,
                                      VK_FORMAT_R8G8B8A8_SRGB);
    spdlog::info("Terrain grass texture generated ({}x{})", W, H);
  }

  // ── 2. Dirt/Path texture (256×256, SRGB) ────────────────────────────────
  {
    constexpr uint32_t W = 256, H = 256;
    std::vector<uint32_t> pixels(W * H);
    for (uint32_t y = 0; y < H; ++y) {
      for (uint32_t x = 0; x < W; ++x) {
        float fx = static_cast<float>(x) / 32.0f;
        float fy = static_cast<float>(y) / 32.0f;
        float n = fbm(fx + 1.1f, fy + 3.3f, 5);
        n = n * 0.5f + 0.5f;

        uint8_t r =
            static_cast<uint8_t>(std::clamp(120.0f + n * 60.0f, 0.0f, 255.0f));
        uint8_t g =
            static_cast<uint8_t>(std::clamp(85.0f + n * 45.0f, 0.0f, 255.0f));
        uint8_t b =
            static_cast<uint8_t>(std::clamp(50.0f + n * 30.0f, 0.0f, 255.0f));
        pixels[y * W + x] = packRGBA(r, g, b);
      }
    }
    dirt = Texture::createFromPixels(device, pixels.data(), W, H,
                                     VK_FORMAT_R8G8B8A8_SRGB);
    spdlog::info("Terrain dirt texture generated ({}x{})", W, H);
  }

  // ── 3. Stone texture (256×256, SRGB) ────────────────────────────────────
  {
    constexpr uint32_t W = 256, H = 256;
    std::vector<uint32_t> pixels(W * H);
    for (uint32_t y = 0; y < H; ++y) {
      for (uint32_t x = 0; x < W; ++x) {
        float fx = static_cast<float>(x) / 32.0f;
        float fy = static_cast<float>(y) / 32.0f;
        float n = fbm(fx + 7.7f, fy + 4.4f, 5);
        n = n * 0.5f + 0.5f;
        float crack = 1.0f - std::pow(std::abs(n - 0.5f) * 2.0f, 0.5f) * 0.3f;

        uint8_t r = static_cast<uint8_t>(
            std::clamp((100.0f + n * 80.0f) * crack, 0.0f, 255.0f));
        uint8_t g = static_cast<uint8_t>(
            std::clamp((95.0f + n * 75.0f) * crack, 0.0f, 255.0f));
        uint8_t b = static_cast<uint8_t>(
            std::clamp((90.0f + n * 70.0f) * crack, 0.0f, 255.0f));
        pixels[y * W + x] = packRGBA(r, g, b);
      }
    }
    stone = Texture::createFromPixels(device, pixels.data(), W, H,
                                      VK_FORMAT_R8G8B8A8_SRGB);
    spdlog::info("Terrain stone texture generated ({}x{})", W, H);
  }

  // ── 4. Splat map (512×512, UNORM RGBA) ──────────────────────────────────
  // R=grass, G=dirt(lanes), B=stone(bases), A=water(river)
  {
    constexpr uint32_t W = 512, H = 512;
    std::vector<uint32_t> pixels(W * H);
    for (uint32_t y = 0; y < H; ++y) {
      for (uint32_t x = 0; x < W; ++x) {
        float fx = static_cast<float>(x) / W;
        float fy = static_cast<float>(y) / H;

        float grassW = 1.0f, dirtW = 0.0f, stoneW = 0.0f, waterW = 0.0f;

        // River: diagonal band from (0,0) to (1,1)
        float riverDist = std::abs(fx - fy);
        float riverNoise = smoothNoise(fx * 16.0f, fy * 16.0f) * 0.02f;
        riverDist += riverNoise;
        if (riverDist < 0.04f) {
          waterW = 1.0f;
          grassW = 0.0f;
        } else if (riverDist < 0.07f) {
          float t = (riverDist - 0.04f) / 0.03f;
          waterW = 1.0f - t;
          grassW = t;
        }

        // Lanes: 3 dirt paths (top, mid, bot)
        // Mid lane: diagonal from (0.11,0.11) to (0.89, 0.89) — along center
        float midDist = std::abs(fx - fy);
        // Top lane: along top edge then right edge
        float topDist = std::min(fy, 1.0f - fx);
        // Bot lane: along left edge then bottom edge
        float botDist = std::min(fx, 1.0f - fy);

        float laneDirt = 0.0f;
        float laneWidth = 0.03f;
        if (midDist < laneWidth)
          laneDirt = std::max(laneDirt, 1.0f - midDist / laneWidth);
        if (topDist < laneWidth)
          laneDirt = std::max(laneDirt, 1.0f - topDist / laneWidth);
        if (botDist < laneWidth)
          laneDirt = std::max(laneDirt, 1.0f - botDist / laneWidth);

        dirtW = laneDirt * (1.0f - waterW);
        grassW = std::max(0.0f, grassW - dirtW);

        // Base areas: stone near corners
        float blueDist = std::sqrt(fx * fx + fy * fy);
        float redDist =
            std::sqrt((1.0f - fx) * (1.0f - fx) + (1.0f - fy) * (1.0f - fy));
        if (blueDist < 0.12f) {
          stoneW = 1.0f;
          grassW = 0.0f;
          dirtW = 0.0f;
        } else if (blueDist < 0.18f) {
          float t = (blueDist - 0.12f) / 0.06f;
          stoneW = 1.0f - t;
          grassW *= t;
        }
        if (redDist < 0.12f) {
          stoneW = 1.0f;
          grassW = 0.0f;
          dirtW = 0.0f;
        } else if (redDist < 0.18f) {
          float t = (redDist - 0.12f) / 0.06f;
          stoneW = std::max(stoneW, 1.0f - t);
          grassW *= t;
        }

        // Normalize RGB weights
        float total = grassW + dirtW + stoneW;
        if (total > 0.001f) {
          grassW /= total;
          dirtW /= total;
          stoneW /= total;
        }

        uint8_t r =
            static_cast<uint8_t>(std::clamp(grassW * 255.0f, 0.0f, 255.0f));
        uint8_t g =
            static_cast<uint8_t>(std::clamp(dirtW * 255.0f, 0.0f, 255.0f));
        uint8_t b =
            static_cast<uint8_t>(std::clamp(stoneW * 255.0f, 0.0f, 255.0f));
        uint8_t a =
            static_cast<uint8_t>(std::clamp(waterW * 255.0f, 0.0f, 255.0f));
        pixels[y * W + x] = packRGBA(r, g, b, a);
      }
    }
    splatMap = Texture::createFromPixels(device, pixels.data(), W, H,
                                         VK_FORMAT_R8G8B8A8_UNORM);
    spdlog::info("Terrain splat map generated ({}x{})", W, H);
  }

  // ── 5. Team map (256×256, UNORM — gradient Blue→Red) ────────────────────
  {
    constexpr uint32_t W = 256, H = 256;
    std::vector<uint32_t> pixels(W * H);
    for (uint32_t y = 0; y < H; ++y) {
      for (uint32_t x = 0; x < W; ++x) {
        float fx = static_cast<float>(x) / (W - 1);
        float fy = static_cast<float>(y) / (H - 1);
        // Diagonal gradient: 0 at Blue corner (0,0), 1 at Red corner (1,1)
        float t = (fx + fy) * 0.5f;
        t = std::clamp(t, 0.0f, 1.0f);
        uint8_t val = static_cast<uint8_t>(t * 255.0f);
        pixels[y * W + x] = packRGBA(val, val, val, 255);
      }
    }
    teamMap = Texture::createFromPixels(device, pixels.data(), W, H,
                                        VK_FORMAT_R8G8B8A8_UNORM);
    spdlog::info("Terrain team map generated ({}x{})", W, H);
  }

  // ── 6. Normal map (256×256, UNORM) ──────────────────────────────────────
  {
    constexpr uint32_t W = 256, H = 256;
    // Generate a heightmap first
    std::vector<float> heights(W * H);
    for (uint32_t y = 0; y < H; ++y) {
      for (uint32_t x = 0; x < W; ++x) {
        float fx = static_cast<float>(x) / 32.0f;
        float fy = static_cast<float>(y) / 32.0f;
        heights[y * W + x] = fbm(fx + 10.0f, fy + 10.0f, 4) * 0.5f + 0.5f;
      }
    }
    // Derive normals
    std::vector<uint32_t> pixels(W * H);
    constexpr float strength = 2.0f;
    for (uint32_t y = 0; y < H; ++y) {
      for (uint32_t x = 0; x < W; ++x) {
        float hL = heights[y * W + ((x + W - 1) % W)];
        float hR = heights[y * W + ((x + 1) % W)];
        float hD = heights[((y + H - 1) % H) * W + x];
        float hU = heights[((y + 1) % H) * W + x];

        float dx = (hL - hR) * strength;
        float dy = (hD - hU) * strength;
        float dz = 1.0f;
        float len = std::sqrt(dx * dx + dy * dy + dz * dz);
        dx /= len;
        dy /= len;
        dz /= len;

        uint8_t r = static_cast<uint8_t>((dx * 0.5f + 0.5f) * 255.0f);
        uint8_t g = static_cast<uint8_t>((dy * 0.5f + 0.5f) * 255.0f);
        uint8_t b = static_cast<uint8_t>((dz * 0.5f + 0.5f) * 255.0f);
        pixels[y * W + x] = packRGBA(r, g, b);
      }
    }
    normalMap = Texture::createFromPixels(device, pixels.data(), W, H,
                                          VK_FORMAT_R8G8B8A8_UNORM);
    spdlog::info("Terrain normal map generated ({}x{})", W, H);
  }
}

} // namespace glory
