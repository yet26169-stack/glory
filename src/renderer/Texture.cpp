#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include "renderer/Buffer.h"
#include "renderer/Device.h"
#include "renderer/Texture.h"
#include "renderer/VkCheck.h"

#include <spdlog/spdlog.h>

#include <cmath>
#include <cstring>
#include <stdexcept>

namespace glory {

namespace {
void transitionImageLayout(const Device &device, VkImage image,
                           VkImageLayout oldLayout, VkImageLayout newLayout) {
  VkCommandBufferAllocateInfo allocInfo{};
  allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  allocInfo.commandPool = device.getTransferCommandPool();
  allocInfo.commandBufferCount = 1;

  VkCommandBuffer cmd;
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
  barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  barrier.subresourceRange.baseMipLevel = 0;
  barrier.subresourceRange.levelCount = 1;
  barrier.subresourceRange.baseArrayLayer = 0;
  barrier.subresourceRange.layerCount = 1;

  VkPipelineStageFlags srcStage, dstStage;

  if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED &&
      newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    srcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    dstStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
  } else if (oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL &&
             newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    srcStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    dstStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
  } else {
    throw std::runtime_error("Unsupported layout transition");
  }

  vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1,
                       &barrier);

  vkEndCommandBuffer(cmd);

  VkSubmitInfo submitInfo{};
  submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submitInfo.commandBufferCount = 1;
  submitInfo.pCommandBuffers = &cmd;

  vkQueueSubmit(device.getGraphicsQueue(), 1, &submitInfo, VK_NULL_HANDLE);
  vkQueueWaitIdle(device.getGraphicsQueue());

  vkFreeCommandBuffers(device.getDevice(), device.getTransferCommandPool(), 1,
                       &cmd);
}

void copyBufferToImage(const Device &device, VkBuffer buffer, VkImage image,
                       uint32_t width, uint32_t height) {
  VkCommandBufferAllocateInfo allocInfo{};
  allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  allocInfo.commandPool = device.getTransferCommandPool();
  allocInfo.commandBufferCount = 1;

  VkCommandBuffer cmd;
  vkAllocateCommandBuffers(device.getDevice(), &allocInfo, &cmd);

  VkCommandBufferBeginInfo beginInfo{};
  beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(cmd, &beginInfo);

  VkBufferImageCopy region{};
  region.bufferOffset = 0;
  region.bufferRowLength = 0;
  region.bufferImageHeight = 0;
  region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  region.imageSubresource.mipLevel = 0;
  region.imageSubresource.baseArrayLayer = 0;
  region.imageSubresource.layerCount = 1;
  region.imageOffset = {0, 0, 0};
  region.imageExtent = {width, height, 1};

  vkCmdCopyBufferToImage(cmd, buffer, image,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

  vkEndCommandBuffer(cmd);

  VkSubmitInfo submitInfo{};
  submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submitInfo.commandBufferCount = 1;
  submitInfo.pCommandBuffers = &cmd;

  vkQueueSubmit(device.getGraphicsQueue(), 1, &submitInfo, VK_NULL_HANDLE);
  vkQueueWaitIdle(device.getGraphicsQueue());

  vkFreeCommandBuffers(device.getDevice(), device.getTransferCommandPool(), 1,
                       &cmd);
}
} // anonymous namespace

// ── Texture from file ───────────────────────────────────────────────────────
Texture::Texture(const Device &device, const std::string &filepath)
    : m_vkDevice(device.getDevice()) {
  int texWidth, texHeight, texChannels;
  stbi_uc *pixels = stbi_load(filepath.c_str(), &texWidth, &texHeight,
                              &texChannels, STBI_rgb_alpha);
  if (!pixels)
    throw std::runtime_error("Failed to load texture: " + filepath);

  VkDeviceSize imageSize = static_cast<VkDeviceSize>(texWidth) * texHeight * 4;

  Buffer staging(device.getAllocator(), imageSize,
                 VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY);
  void *mapped = staging.map();
  std::memcpy(mapped, pixels, static_cast<size_t>(imageSize));
  staging.unmap();
  stbi_image_free(pixels);

  m_image = Image(device, static_cast<uint32_t>(texWidth),
                  static_cast<uint32_t>(texHeight), VK_FORMAT_R8G8B8A8_SRGB,
                  VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                  VK_IMAGE_ASPECT_COLOR_BIT);

  transitionImageLayout(device, m_image.getImage(), VK_IMAGE_LAYOUT_UNDEFINED,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  copyBufferToImage(device, staging.getBuffer(), m_image.getImage(),
                    static_cast<uint32_t>(texWidth),
                    static_cast<uint32_t>(texHeight));
  transitionImageLayout(device, m_image.getImage(),
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

  createSampler(device);
  spdlog::info("Texture loaded: {} ({}x{})", filepath, texWidth, texHeight);
}

Texture::~Texture() { destroy(); }

Texture::Texture(Texture &&other) noexcept
    : m_image(std::move(other.m_image)), m_sampler(other.m_sampler),
      m_vkDevice(other.m_vkDevice) {
  other.m_sampler = VK_NULL_HANDLE;
  other.m_vkDevice = VK_NULL_HANDLE;
}

Texture &Texture::operator=(Texture &&other) noexcept {
  if (this != &other) {
    destroy();
    m_image = std::move(other.m_image);
    m_sampler = other.m_sampler;
    m_vkDevice = other.m_vkDevice;
    other.m_sampler = VK_NULL_HANDLE;
    other.m_vkDevice = VK_NULL_HANDLE;
  }
  return *this;
}

void Texture::destroy() {
  if (m_sampler != VK_NULL_HANDLE && m_vkDevice != VK_NULL_HANDLE) {
    vkDestroySampler(m_vkDevice, m_sampler, nullptr);
    m_sampler = VK_NULL_HANDLE;
  }
}

// ── Default 1x1 white texture ───────────────────────────────────────────────
Texture Texture::createDefault(const Device &device) {
  Texture tex;
  tex.m_vkDevice = device.getDevice();

  uint32_t white = 0xFFFFFFFF;
  VkDeviceSize size = 4;

  Buffer staging(device.getAllocator(), size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                 VMA_MEMORY_USAGE_CPU_ONLY);
  void *mapped = staging.map();
  std::memcpy(mapped, &white, 4);
  staging.unmap();

  tex.m_image =
      Image(device, 1, 1, VK_FORMAT_R8G8B8A8_SRGB,
            VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT);

  transitionImageLayout(device, tex.m_image.getImage(),
                        VK_IMAGE_LAYOUT_UNDEFINED,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  copyBufferToImage(device, staging.getBuffer(), tex.m_image.getImage(), 1, 1);
  transitionImageLayout(device, tex.m_image.getImage(),
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

  tex.createSampler(device);
  spdlog::info("Default 1x1 white texture created");
  return tex;
}

// ── Procedural checkerboard texture ─────────────────────────────────────────
Texture Texture::createCheckerboard(const Device &device, uint32_t tiles,
                                    uint32_t tileSize, uint32_t colorA,
                                    uint32_t colorB) {
  Texture tex;
  tex.m_vkDevice = device.getDevice();

  uint32_t width = tiles * tileSize * 2;
  uint32_t height = width;
  VkDeviceSize size = static_cast<VkDeviceSize>(width) * height * 4;

  std::vector<uint32_t> pixels(width * height);
  for (uint32_t y = 0; y < height; ++y) {
    for (uint32_t x = 0; x < width; ++x) {
      bool isA = ((x / tileSize) + (y / tileSize)) % 2 == 0;
      pixels[y * width + x] = isA ? colorA : colorB;
    }
  }

  Buffer staging(device.getAllocator(), size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                 VMA_MEMORY_USAGE_CPU_ONLY);
  void *mapped = staging.map();
  std::memcpy(mapped, pixels.data(), static_cast<size_t>(size));
  staging.unmap();

  tex.m_image =
      Image(device, width, height, VK_FORMAT_R8G8B8A8_SRGB,
            VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT);

  transitionImageLayout(device, tex.m_image.getImage(),
                        VK_IMAGE_LAYOUT_UNDEFINED,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  copyBufferToImage(device, staging.getBuffer(), tex.m_image.getImage(), width,
                    height);
  transitionImageLayout(device, tex.m_image.getImage(),
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

  tex.createSampler(device);
  spdlog::info("Checkerboard texture created ({}x{})", width, height);
  return tex;
}

Texture Texture::createFlatNormal(const Device &device) {
  Texture tex;
  tex.m_vkDevice = device.getDevice();

  // 1x1 flat normal: tangent-space (0,0,1) → encoded as (128,128,255,255)
  uint32_t pixel = 0xFFFF8080; // ABGR
  VkDeviceSize size = 4;

  Buffer staging(device.getAllocator(), size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                 VMA_MEMORY_USAGE_CPU_ONLY);
  void *mapped = staging.map();
  std::memcpy(mapped, &pixel, 4);
  staging.unmap();

  tex.m_image =
      Image(device, 1, 1, VK_FORMAT_R8G8B8A8_UNORM,
            VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT);

  transitionImageLayout(device, tex.m_image.getImage(),
                        VK_IMAGE_LAYOUT_UNDEFINED,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  copyBufferToImage(device, staging.getBuffer(), tex.m_image.getImage(), 1, 1);
  transitionImageLayout(device, tex.m_image.getImage(),
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

  tex.createSampler(device);
  spdlog::info("Flat normal map created (1x1)");
  return tex;
}

Texture Texture::createBrickNormal(const Device &device) {
  Texture tex;
  tex.m_vkDevice = device.getDevice();

  constexpr uint32_t W = 256, H = 256;
  VkDeviceSize size = static_cast<VkDeviceSize>(W) * H * 4;

  // Generate heightmap: bricks at height 1.0, mortar at 0.0
  std::vector<float> heightmap(W * H);
  constexpr float brickW = 64.0f; // 4 bricks across
  constexpr float brickH = 32.0f; // 8 rows
  constexpr float mortar = 3.0f;

  for (uint32_t y = 0; y < H; ++y) {
    for (uint32_t x = 0; x < W; ++x) {
      int row = static_cast<int>(static_cast<float>(y) / brickH);
      float xOff = (row % 2 == 0) ? 0.0f : brickW * 0.5f;
      float lx = std::fmod(static_cast<float>(x) + xOff, brickW);
      float ly = std::fmod(static_cast<float>(y), brickH);

      float edgeX = std::min(lx, brickW - lx);
      float edgeY = std::min(ly, brickH - ly);
      float edge = std::min(edgeX, edgeY);

      float h = (edge < mortar) ? edge / mortar : 1.0f;
      heightmap[y * W + x] = h;
    }
  }

  // Derive normals from heightmap using central differences
  std::vector<uint32_t> pixels(W * H);
  constexpr float strength = 2.0f;

  for (uint32_t y = 0; y < H; ++y) {
    for (uint32_t x = 0; x < W; ++x) {
      float hL = heightmap[y * W + ((x + W - 1) % W)];
      float hR = heightmap[y * W + ((x + 1) % W)];
      float hD = heightmap[((y + H - 1) % H) * W + x];
      float hU = heightmap[((y + 1) % H) * W + x];

      float dx = (hL - hR) * strength;
      float dy = (hD - hU) * strength;
      float dz = 1.0f;
      float len = std::sqrt(dx * dx + dy * dy + dz * dz);
      dx /= len;
      dy /= len;
      dz /= len;

      // Map [-1,1] → [0,255]
      uint8_t r = static_cast<uint8_t>((dx * 0.5f + 0.5f) * 255.0f);
      uint8_t g = static_cast<uint8_t>((dy * 0.5f + 0.5f) * 255.0f);
      uint8_t b = static_cast<uint8_t>((dz * 0.5f + 0.5f) * 255.0f);
      pixels[y * W + x] = static_cast<uint32_t>(r) |
                          (static_cast<uint32_t>(g) << 8) |
                          (static_cast<uint32_t>(b) << 16) | (0xFFu << 24);
    }
  }

  Buffer staging(device.getAllocator(), size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                 VMA_MEMORY_USAGE_CPU_ONLY);
  void *mapped = staging.map();
  std::memcpy(mapped, pixels.data(), static_cast<size_t>(size));
  staging.unmap();

  // Normal maps must use UNORM (not SRGB) to avoid gamma-mangling the normal
  // vectors
  tex.m_image =
      Image(device, W, H, VK_FORMAT_R8G8B8A8_UNORM,
            VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT);

  transitionImageLayout(device, tex.m_image.getImage(),
                        VK_IMAGE_LAYOUT_UNDEFINED,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  copyBufferToImage(device, staging.getBuffer(), tex.m_image.getImage(), W, H);
  transitionImageLayout(device, tex.m_image.getImage(),
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

  tex.createSampler(device);
  spdlog::info("Brick normal map created ({}x{})", W, H);
  return tex;
}

// ── Procedural marble texture ───────────────────────────────────────────────
Texture Texture::createMarble(const Device &device) {
  Texture tex;
  tex.m_vkDevice = device.getDevice();

  constexpr uint32_t W = 256, H = 256;
  VkDeviceSize size = static_cast<VkDeviceSize>(W) * H * 4;

  // Simple value-noise hash for pseudo-random noise
  auto hash = [](int x, int y) -> float {
    int n = x + y * 57;
    n = (n << 13) ^ n;
    return 1.0f -
           static_cast<float>((n * (n * n * 15731 + 789221) + 1376312589) &
                              0x7FFFFFFF) /
               1073741824.0f;
  };

  auto smoothNoise = [&](float fx, float fy) -> float {
    int ix = static_cast<int>(fx);
    int iy = static_cast<int>(fy);
    float fracX = fx - static_cast<float>(ix);
    float fracY = fy - static_cast<float>(iy);
    float v00 = hash(ix, iy);
    float v10 = hash(ix + 1, iy);
    float v01 = hash(ix, iy + 1);
    float v11 = hash(ix + 1, iy + 1);
    float i0 = v00 + fracX * (v10 - v00);
    float i1 = v01 + fracX * (v11 - v01);
    return i0 + fracY * (i1 - i0);
  };

  auto turbulence = [&](float x, float y, float s) -> float {
    float val = 0.0f, scale = s;
    while (scale >= 1.0f) {
      val += smoothNoise(x / scale, y / scale) * scale;
      scale /= 2.0f;
    }
    return val / s;
  };

  std::vector<uint32_t> pixels(W * H);
  for (uint32_t y = 0; y < H; ++y) {
    for (uint32_t x = 0; x < W; ++x) {
      float fx = static_cast<float>(x);
      float fy = static_cast<float>(y);
      float t = turbulence(fx, fy, 64.0f);
      float marble = std::sin((fx + fy) * 0.05f + t * 6.0f) * 0.5f + 0.5f;

      // Veined marble: cream base with dark veins
      uint8_t r =
          static_cast<uint8_t>(std::min(255.0f, (180.0f + marble * 75.0f)));
      uint8_t g =
          static_cast<uint8_t>(std::min(255.0f, (170.0f + marble * 70.0f)));
      uint8_t b =
          static_cast<uint8_t>(std::min(255.0f, (160.0f + marble * 60.0f)));

      pixels[y * W + x] = static_cast<uint32_t>(r) |
                          (static_cast<uint32_t>(g) << 8) |
                          (static_cast<uint32_t>(b) << 16) | (0xFFu << 24);
    }
  }

  Buffer staging(device.getAllocator(), size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                 VMA_MEMORY_USAGE_CPU_ONLY);
  void *mapped = staging.map();
  std::memcpy(mapped, pixels.data(), static_cast<size_t>(size));
  staging.unmap();

  tex.m_image =
      Image(device, W, H, VK_FORMAT_R8G8B8A8_SRGB,
            VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT);

  transitionImageLayout(device, tex.m_image.getImage(),
                        VK_IMAGE_LAYOUT_UNDEFINED,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  copyBufferToImage(device, staging.getBuffer(), tex.m_image.getImage(), W, H);
  transitionImageLayout(device, tex.m_image.getImage(),
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

  tex.createSampler(device);
  spdlog::info("Marble texture created ({}x{})", W, H);
  return tex;
}

Texture Texture::createWood(const Device &device) {
  Texture tex;
  tex.m_vkDevice = device.getDevice();

  constexpr uint32_t W = 256, H = 256;
  VkDeviceSize size = static_cast<VkDeviceSize>(W) * H * 4;

  auto hash = [](int x, int y) -> float {
    int n = x + y * 57;
    n = (n << 13) ^ n;
    return 1.0f -
           static_cast<float>((n * (n * n * 15731 + 789221) + 1376312589) &
                              0x7FFFFFFF) /
               1073741824.0f;
  };

  auto smoothNoise = [&](float fx, float fy) -> float {
    int ix = static_cast<int>(fx);
    int iy = static_cast<int>(fy);
    float fracX = fx - static_cast<float>(ix);
    float fracY = fy - static_cast<float>(iy);
    float v00 = hash(ix, iy), v10 = hash(ix + 1, iy);
    float v01 = hash(ix, iy + 1), v11 = hash(ix + 1, iy + 1);
    float i0 = v00 + fracX * (v10 - v00);
    float i1 = v01 + fracX * (v11 - v01);
    return i0 + fracY * (i1 - i0);
  };

  std::vector<uint32_t> pixels(W * H);
  for (uint32_t y = 0; y < H; ++y) {
    for (uint32_t x = 0; x < W; ++x) {
      float fx = static_cast<float>(x) - 128.0f;
      float fy = static_cast<float>(y) - 128.0f;

      // Distance from center with noise perturbation for organic rings
      float dist = std::sqrt(fx * fx + fy * fy) * 0.15f;
      float noise = smoothNoise(static_cast<float>(x) * 0.05f,
                                static_cast<float>(y) * 0.05f) *
                    3.0f;
      float ring = std::sin(dist + noise) * 0.5f + 0.5f;

      // Warm wood tones: dark brown to light tan
      uint8_t r =
          static_cast<uint8_t>(std::min(255.0f, 100.0f + ring * 100.0f));
      uint8_t g = static_cast<uint8_t>(std::min(255.0f, 55.0f + ring * 80.0f));
      uint8_t b = static_cast<uint8_t>(std::min(255.0f, 25.0f + ring * 40.0f));

      pixels[y * W + x] = static_cast<uint32_t>(r) |
                          (static_cast<uint32_t>(g) << 8) |
                          (static_cast<uint32_t>(b) << 16) | (0xFFu << 24);
    }
  }

  Buffer staging(device.getAllocator(), size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                 VMA_MEMORY_USAGE_CPU_ONLY);
  void *mapped = staging.map();
  std::memcpy(mapped, pixels.data(), static_cast<size_t>(size));
  staging.unmap();

  tex.m_image =
      Image(device, W, H, VK_FORMAT_R8G8B8A8_SRGB,
            VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT);

  transitionImageLayout(device, tex.m_image.getImage(),
                        VK_IMAGE_LAYOUT_UNDEFINED,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  copyBufferToImage(device, staging.getBuffer(), tex.m_image.getImage(), W, H);
  transitionImageLayout(device, tex.m_image.getImage(),
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

  tex.createSampler(device);
  spdlog::info("Wood texture created ({}x{})", W, H);
  return tex;
}

Texture Texture::createLava(const Device &device) {
  Texture tex;
  tex.m_vkDevice = device.getDevice();

  constexpr uint32_t W = 256, H = 256;
  VkDeviceSize size = static_cast<VkDeviceSize>(W) * H * 4;

  auto hash = [](int x, int y) -> float {
    int n = x + y * 57;
    n = (n << 13) ^ n;
    return 1.0f -
           static_cast<float>((n * (n * n * 15731 + 789221) + 1376312589) &
                              0x7FFFFFFF) /
               1073741824.0f;
  };

  auto smoothNoise = [&](float fx, float fy) -> float {
    int ix = static_cast<int>(fx);
    int iy = static_cast<int>(fy);
    float fracX = fx - static_cast<float>(ix);
    float fracY = fy - static_cast<float>(iy);
    float v00 = hash(ix, iy), v10 = hash(ix + 1, iy);
    float v01 = hash(ix, iy + 1), v11 = hash(ix + 1, iy + 1);
    float i0 = v00 + fracX * (v10 - v00);
    float i1 = v01 + fracX * (v11 - v01);
    return i0 + fracY * (i1 - i0);
  };

  auto turbulence = [&](float x, float y, float scale) -> float {
    float val = 0.0f, s = scale;
    while (s >= 1.0f) {
      val += smoothNoise(x / s, y / s) * s;
      s *= 0.5f;
    }
    return val / scale;
  };

  std::vector<uint32_t> pixels(W * H);
  for (uint32_t y = 0; y < H; ++y) {
    for (uint32_t x = 0; x < W; ++x) {
      float fx = static_cast<float>(x);
      float fy = static_cast<float>(y);

      // Turbulence-based lava veins
      float t = turbulence(fx, fy, 64.0f);
      float vein = std::sin(fx * 0.05f + fy * 0.03f + t * 6.0f) * 0.5f + 0.5f;
      vein = vein * vein; // sharpen veins

      // Dark rock base with bright glowing veins
      float r_f = 30.0f + vein * 225.0f;             // dark→bright red
      float g_f = 10.0f + vein * vein * 180.0f;      // orange glow in veins
      float b_f = 5.0f + vein * vein * vein * 60.0f; // faint yellow core

      uint8_t r = static_cast<uint8_t>(std::min(255.0f, r_f));
      uint8_t g = static_cast<uint8_t>(std::min(255.0f, g_f));
      uint8_t b = static_cast<uint8_t>(std::min(255.0f, b_f));

      pixels[y * W + x] = static_cast<uint32_t>(r) |
                          (static_cast<uint32_t>(g) << 8) |
                          (static_cast<uint32_t>(b) << 16) | (0xFFu << 24);
    }
  }

  Buffer staging(device.getAllocator(), size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                 VMA_MEMORY_USAGE_CPU_ONLY);
  void *mapped = staging.map();
  std::memcpy(mapped, pixels.data(), static_cast<size_t>(size));
  staging.unmap();

  tex.m_image =
      Image(device, W, H, VK_FORMAT_R8G8B8A8_SRGB,
            VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT);

  transitionImageLayout(device, tex.m_image.getImage(),
                        VK_IMAGE_LAYOUT_UNDEFINED,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  copyBufferToImage(device, staging.getBuffer(), tex.m_image.getImage(), W, H);
  transitionImageLayout(device, tex.m_image.getImage(),
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

  tex.createSampler(device);
  spdlog::info("Lava texture created ({}x{})", W, H);
  return tex;
}

Texture Texture::createRock(const Device &device) {
  Texture tex;
  tex.m_vkDevice = device.getDevice();

  constexpr uint32_t W = 256, H = 256;
  VkDeviceSize size = static_cast<VkDeviceSize>(W) * H * 4;

  auto hash = [](int x, int y) -> float {
    int n = x + y * 57;
    n = (n << 13) ^ n;
    return 1.0f -
           static_cast<float>((n * (n * n * 15731 + 789221) + 1376312589) &
                              0x7FFFFFFF) /
               1073741824.0f;
  };

  auto smoothNoise = [&](float fx, float fy) -> float {
    int ix = static_cast<int>(std::floor(fx));
    int iy = static_cast<int>(std::floor(fy));
    float fracX = fx - static_cast<float>(ix);
    float fracY = fy - static_cast<float>(iy);
    float v00 = hash(ix, iy), v10 = hash(ix + 1, iy);
    float v01 = hash(ix, iy + 1), v11 = hash(ix + 1, iy + 1);
    float i0 = v00 + fracX * (v10 - v00);
    float i1 = v01 + fracX * (v11 - v01);
    return i0 + fracY * (i1 - i0);
  };

  auto fbm = [&](float x, float y) -> float {
    float val = 0.0f, amp = 1.0f, freq = 1.0f, total = 0.0f;
    for (int i = 0; i < 5; ++i) {
      val += std::abs(smoothNoise(x * freq, y * freq)) * amp;
      total += amp;
      amp *= 0.5f;
      freq *= 2.1f;
    }
    return val / total;
  };

  std::vector<uint32_t> pixels(W * H);
  for (uint32_t y = 0; y < H; ++y) {
    for (uint32_t x = 0; x < W; ++x) {
      float fx = static_cast<float>(x) / 32.0f;
      float fy = static_cast<float>(y) / 32.0f;

      float n = fbm(fx + 3.7f, fy + 1.3f);
      // Crevice darkening: sharpen noise for crack-like detail
      float crack = 1.0f - std::pow(n, 0.6f) * 0.4f;

      // Grey-brown mottled stone
      float base = 0.35f + n * 0.3f;
      float r_f = base * 155.0f * crack + 30.0f;
      float g_f = base * 145.0f * crack + 25.0f;
      float b_f = base * 130.0f * crack + 20.0f;

      // Add subtle warm/cool variation
      float tint = smoothNoise(fx * 0.5f + 10.0f, fy * 0.5f + 10.0f);
      r_f += tint * 12.0f;
      b_f -= tint * 8.0f;

      uint8_t r = static_cast<uint8_t>(std::clamp(r_f, 0.0f, 255.0f));
      uint8_t g = static_cast<uint8_t>(std::clamp(g_f, 0.0f, 255.0f));
      uint8_t b = static_cast<uint8_t>(std::clamp(b_f, 0.0f, 255.0f));

      pixels[y * W + x] = static_cast<uint32_t>(r) |
                          (static_cast<uint32_t>(g) << 8) |
                          (static_cast<uint32_t>(b) << 16) | (0xFFu << 24);
    }
  }

  Buffer staging(device.getAllocator(), size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                 VMA_MEMORY_USAGE_CPU_ONLY);
  void *mapped = staging.map();
  std::memcpy(mapped, pixels.data(), static_cast<size_t>(size));
  staging.unmap();

  tex.m_image =
      Image(device, W, H, VK_FORMAT_R8G8B8A8_SRGB,
            VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT);

  transitionImageLayout(device, tex.m_image.getImage(),
                        VK_IMAGE_LAYOUT_UNDEFINED,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  copyBufferToImage(device, staging.getBuffer(), tex.m_image.getImage(), W, H);
  transitionImageLayout(device, tex.m_image.getImage(),
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

  tex.createSampler(device);
  spdlog::info("Rock texture created ({}x{})", W, H);
  return tex;
}

Texture Texture::createBrushedMetal(const Device &device) {
  Texture tex;
  tex.m_vkDevice = device.getDevice();

  constexpr uint32_t W = 256, H = 256;
  VkDeviceSize size = static_cast<VkDeviceSize>(W) * H * 4;

  auto hash1d = [](int x) -> float {
    x = (x << 13) ^ x;
    return 1.0f -
           static_cast<float>((x * (x * x * 15731 + 789221) + 1376312589) &
                              0x7FFFFFFF) /
               1073741824.0f;
  };

  std::vector<uint32_t> pixels(W * H);
  for (uint32_t y = 0; y < H; ++y) {
    for (uint32_t x = 0; x < W; ++x) {
      // Horizontal directional streaks
      float streak = 0.0f;
      for (int oct = 0; oct < 4; ++oct) {
        int freq = 1 << (oct + 2);
        int iy = static_cast<int>(y) * freq / static_cast<int>(H);
        float n = hash1d(iy + oct * 1337) * 0.5f + 0.5f;
        streak += n / static_cast<float>(1 << oct);
      }
      streak = streak * 0.5f + 0.25f;

      // Fine horizontal grain noise
      float grain =
          hash1d(static_cast<int>(x) * 7919 + static_cast<int>(y) * 104729);
      grain = grain * 0.08f;

      float base = 0.65f + streak * 0.3f + grain;
      // Silver-grey tone
      float r_f = base * 200.0f;
      float g_f = base * 205.0f;
      float b_f = base * 215.0f;

      uint8_t r = static_cast<uint8_t>(std::clamp(r_f, 0.0f, 255.0f));
      uint8_t g = static_cast<uint8_t>(std::clamp(g_f, 0.0f, 255.0f));
      uint8_t b = static_cast<uint8_t>(std::clamp(b_f, 0.0f, 255.0f));

      pixels[y * W + x] = static_cast<uint32_t>(r) |
                          (static_cast<uint32_t>(g) << 8) |
                          (static_cast<uint32_t>(b) << 16) | (0xFFu << 24);
    }
  }

  Buffer staging(device.getAllocator(), size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                 VMA_MEMORY_USAGE_CPU_ONLY);
  void *mapped = staging.map();
  std::memcpy(mapped, pixels.data(), static_cast<size_t>(size));
  staging.unmap();

  tex.m_image =
      Image(device, W, H, VK_FORMAT_R8G8B8A8_SRGB,
            VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT);

  transitionImageLayout(device, tex.m_image.getImage(),
                        VK_IMAGE_LAYOUT_UNDEFINED,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  copyBufferToImage(device, staging.getBuffer(), tex.m_image.getImage(), W, H);
  transitionImageLayout(device, tex.m_image.getImage(),
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

  tex.createSampler(device);
  spdlog::info("Brushed metal texture created ({}x{})", W, H);
  return tex;
}

Texture Texture::createTiles(const Device &device) {
  Texture tex;
  tex.m_vkDevice = device.getDevice();

  constexpr uint32_t W = 256, H = 256;
  constexpr uint32_t TILE = 32; // tile size in pixels
  constexpr uint32_t GROUT = 2; // grout width
  VkDeviceSize size = static_cast<VkDeviceSize>(W) * H * 4;

  auto hash2 = [](int x, int y) -> float {
    int n = x * 73 + y * 179;
    n = (n << 13) ^ n;
    return static_cast<float>((n * (n * n * 15731 + 789221) + 1376312589) &
                              0x7FFFFFFF) /
           2147483648.0f;
  };

  std::vector<uint32_t> pixels(W * H);
  for (uint32_t y = 0; y < H; ++y) {
    for (uint32_t x = 0; x < W; ++x) {
      uint32_t tx = x / TILE;
      uint32_t ty = y / TILE;
      uint32_t lx = x % TILE;
      uint32_t ly = y % TILE;

      bool isGrout = (lx < GROUT || ly < GROUT);

      if (isGrout) {
        // Dark grey grout
        pixels[y * W + x] = 0xFF3A3632;
      } else {
        // Per-tile color variation (warm terracotta range)
        float h = hash2(static_cast<int>(tx), static_cast<int>(ty));
        float r_f = 160.0f + h * 60.0f;
        float g_f = 120.0f + h * 40.0f;
        float b_f = 90.0f + h * 30.0f;

        // Subtle per-pixel noise within tile
        float pn = hash2(static_cast<int>(x) * 31, static_cast<int>(y) * 37);
        r_f += (pn - 0.5f) * 15.0f;
        g_f += (pn - 0.5f) * 10.0f;
        b_f += (pn - 0.5f) * 8.0f;

        uint8_t r = static_cast<uint8_t>(std::clamp(r_f, 0.0f, 255.0f));
        uint8_t g = static_cast<uint8_t>(std::clamp(g_f, 0.0f, 255.0f));
        uint8_t b = static_cast<uint8_t>(std::clamp(b_f, 0.0f, 255.0f));

        pixels[y * W + x] = static_cast<uint32_t>(r) |
                            (static_cast<uint32_t>(g) << 8) |
                            (static_cast<uint32_t>(b) << 16) | (0xFFu << 24);
      }
    }
  }

  Buffer staging(device.getAllocator(), size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                 VMA_MEMORY_USAGE_CPU_ONLY);
  void *mapped = staging.map();
  std::memcpy(mapped, pixels.data(), static_cast<size_t>(size));
  staging.unmap();

  tex.m_image =
      Image(device, W, H, VK_FORMAT_R8G8B8A8_SRGB,
            VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT);

  transitionImageLayout(device, tex.m_image.getImage(),
                        VK_IMAGE_LAYOUT_UNDEFINED,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  copyBufferToImage(device, staging.getBuffer(), tex.m_image.getImage(), W, H);
  transitionImageLayout(device, tex.m_image.getImage(),
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

  tex.createSampler(device);
  spdlog::info("Tiles texture created ({}x{})", W, H);
  return tex;
}

Texture Texture::createCircuit(const Device &device) {
  Texture tex;
  tex.m_vkDevice = device.getDevice();

  constexpr uint32_t W = 256, H = 256;
  VkDeviceSize size = static_cast<VkDeviceSize>(W) * H * 4;

  auto hash2 = [](int x, int y) -> uint32_t {
    int n = x * 73 + y * 179 + 37;
    n = (n << 13) ^ n;
    return static_cast<uint32_t>((n * (n * n * 15731 + 789221) + 1376312589) &
                                 0x7FFFFFFF);
  };

  // PCB green base: 0xFF1B5E20 (dark green, ABGR)
  const uint32_t pcbColor = 0xFF1A4D1C;
  const uint32_t traceColor = 0xFF30A8C0; // copper trace (ABGR: ~copper)
  const uint32_t padColor = 0xFF40C8D8;   // bright copper pad
  const uint32_t viaColor = 0xFF808080;   // silver via

  std::vector<uint32_t> pixels(W * H, pcbColor);

  // Draw horizontal and vertical traces on a 32-pixel grid
  for (uint32_t y = 0; y < H; ++y) {
    for (uint32_t x = 0; x < W; ++x) {
      uint32_t gx = x / 32;
      uint32_t gy = y / 32;
      uint32_t lx = x % 32;
      uint32_t ly = y % 32;

      uint32_t h = hash2(static_cast<int>(gx), static_cast<int>(gy));

      // Horizontal trace through some cells
      if ((h & 0x3) == 0 && ly >= 14 && ly <= 17) {
        pixels[y * W + x] = traceColor;
      }
      // Vertical trace through some cells
      if ((h & 0xC) == 0 && lx >= 14 && lx <= 17) {
        pixels[y * W + x] = traceColor;
      }
      // Corner pads at grid intersections
      if (lx < 6 && ly < 6 && (h & 0x10)) {
        int dx = static_cast<int>(lx) - 3;
        int dy = static_cast<int>(ly) - 3;
        if (dx * dx + dy * dy <= 9) {
          pixels[y * W + x] = padColor;
        }
      }
      // Center vias (small dots)
      if ((h & 0x60) == 0x60) {
        int dx = static_cast<int>(lx) - 16;
        int dy = static_cast<int>(ly) - 16;
        if (dx * dx + dy * dy <= 4) {
          pixels[y * W + x] = viaColor;
        }
      }
      // Diagonal traces for variety
      if ((h & 0x180) == 0x180) {
        int diag = static_cast<int>(lx) - static_cast<int>(ly);
        if (std::abs(diag) <= 1 && lx > 4 && lx < 28 && ly > 4 && ly < 28) {
          pixels[y * W + x] = traceColor;
        }
      }
    }
  }

  Buffer staging(device.getAllocator(), size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                 VMA_MEMORY_USAGE_CPU_ONLY);
  void *mapped = staging.map();
  std::memcpy(mapped, pixels.data(), static_cast<size_t>(size));
  staging.unmap();

  tex.m_image =
      Image(device, W, H, VK_FORMAT_R8G8B8A8_SRGB,
            VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT);

  transitionImageLayout(device, tex.m_image.getImage(),
                        VK_IMAGE_LAYOUT_UNDEFINED,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  copyBufferToImage(device, staging.getBuffer(), tex.m_image.getImage(), W, H);
  transitionImageLayout(device, tex.m_image.getImage(),
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

  tex.createSampler(device);
  spdlog::info("Circuit texture created ({}x{})", W, H);
  return tex;
}

Texture Texture::createHexGrid(const Device &device) {
  Texture tex;
  tex.m_vkDevice = device.getDevice();

  constexpr uint32_t W = 256, H = 256;
  VkDeviceSize size = static_cast<VkDeviceSize>(W) * H * 4;
  std::vector<uint32_t> pixels(W * H);

  auto hash = [](int x, int y) -> uint32_t {
    uint32_t h = uint32_t(x * 374761393 + y * 668265263 + 1274126177);
    h = (h ^ (h >> 13)) * 1274126177u;
    return h;
  };

  const float sqrt3 = 1.7320508075688772f;
  const float cellSize = 20.0f;

  for (uint32_t py = 0; py < H; py++) {
    for (uint32_t px = 0; px < W; px++) {
      float fx = static_cast<float>(px);
      float fy = static_cast<float>(py);

      float q = (fx * (2.0f / 3.0f)) / cellSize;
      float r = ((-fx / 3.0f) + (sqrt3 / 3.0f) * fy) / cellSize;

      float x = q, z = r, y2 = -x - z;
      float rx = std::round(x), ry = std::round(y2), rz = std::round(z);
      float dx = std::abs(rx - x), dy = std::abs(ry - y2),
            dz = std::abs(rz - z);
      if (dx > dy && dx > dz)
        rx = -ry - rz;
      else if (dy > dz)
        ry = -rx - rz;
      else
        rz = -rx - ry;

      int hx = static_cast<int>(rx), hz = static_cast<int>(rz);

      float cx = cellSize * (3.0f / 2.0f) * hx;
      float cy = cellSize * (sqrt3 * (hz + hx * 0.5f));

      float ddx = fx - cx, ddy = fy - cy;
      float dist = std::sqrt(ddx * ddx + ddy * ddy);
      float edge = dist / (cellSize * 0.866f);

      uint32_t h = hash(hx, hz);
      float hue = (h & 0xFF) / 255.0f;
      float cr = 0.1f + 0.25f * std::sin(hue * 6.28318f);
      float cg = 0.2f + 0.3f * std::sin(hue * 6.28318f + 2.094f);
      float cb = 0.3f + 0.35f * std::sin(hue * 6.28318f + 4.189f);

      float brightness;
      if (edge > 0.88f) {
        brightness = 0.15f;
      } else if (edge > 0.80f) {
        float t = (edge - 0.80f) / 0.08f;
        brightness = 1.0f - t * 0.85f;
      } else {
        brightness = 0.7f + 0.3f * (1.0f - edge / 0.80f);
      }

      uint8_t R = uint8_t(std::min(255.0f, cr * brightness * 255.0f));
      uint8_t G = uint8_t(std::min(255.0f, cg * brightness * 255.0f));
      uint8_t B = uint8_t(std::min(255.0f, cb * brightness * 255.0f));
      pixels[py * W + px] = (255u << 24) | (B << 16) | (G << 8) | R;
    }
  }

  Buffer staging(device.getAllocator(), size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                 VMA_MEMORY_USAGE_CPU_ONLY);
  void *mapped = staging.map();
  std::memcpy(mapped, pixels.data(), static_cast<size_t>(size));
  staging.unmap();

  tex.m_image =
      Image(device, W, H, VK_FORMAT_R8G8B8A8_SRGB,
            VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT);

  transitionImageLayout(device, tex.m_image.getImage(),
                        VK_IMAGE_LAYOUT_UNDEFINED,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  copyBufferToImage(device, staging.getBuffer(), tex.m_image.getImage(), W, H);
  transitionImageLayout(device, tex.m_image.getImage(),
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

  tex.createSampler(device);
  spdlog::info("HexGrid texture created ({}x{})", W, H);
  return tex;
}

Texture Texture::createGradient(const Device &device) {
  Texture tex;
  tex.m_vkDevice = device.getDevice();

  constexpr uint32_t W = 256, H = 256;
  VkDeviceSize size = static_cast<VkDeviceSize>(W) * H * 4;
  std::vector<uint32_t> pixels(W * H);

  for (uint32_t py = 0; py < H; py++) {
    for (uint32_t px = 0; px < W; px++) {
      float fx = (static_cast<float>(px) / (W - 1)) * 2.0f - 1.0f;
      float fy = (static_cast<float>(py) / (H - 1)) * 2.0f - 1.0f;
      float dist = std::sqrt(fx * fx + fy * fy);
      float t = std::min(dist, 1.0f);

      // Warm center (gold/orange) → cool edge (deep blue/purple)
      float r = 1.0f * (1.0f - t * 0.8f) + 0.15f * t;
      float g = 0.75f * (1.0f - t) + 0.1f * t;
      float b = 0.2f * (1.0f - t) + 0.6f * t;

      // Add subtle concentric ring pattern
      float ring = std::sin(dist * 25.0f) * 0.05f;
      r += ring;
      g += ring * 0.5f;
      b += ring * 0.3f;

      r = std::max(0.0f, std::min(1.0f, r));
      g = std::max(0.0f, std::min(1.0f, g));
      b = std::max(0.0f, std::min(1.0f, b));

      uint8_t R = static_cast<uint8_t>(r * 255.0f);
      uint8_t G = static_cast<uint8_t>(g * 255.0f);
      uint8_t B = static_cast<uint8_t>(b * 255.0f);
      pixels[py * W + px] = (255u << 24) | (B << 16) | (G << 8) | R;
    }
  }

  Buffer staging(device.getAllocator(), size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                 VMA_MEMORY_USAGE_CPU_ONLY);
  void *mapped = staging.map();
  std::memcpy(mapped, pixels.data(), static_cast<size_t>(size));
  staging.unmap();

  tex.m_image =
      Image(device, W, H, VK_FORMAT_R8G8B8A8_SRGB,
            VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT);

  transitionImageLayout(device, tex.m_image.getImage(),
                        VK_IMAGE_LAYOUT_UNDEFINED,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  copyBufferToImage(device, staging.getBuffer(), tex.m_image.getImage(), W, H);
  transitionImageLayout(device, tex.m_image.getImage(),
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

  tex.createSampler(device);
  spdlog::info("Gradient texture created ({}x{})", W, H);
  return tex;
}

// ── Generic pixel upload factory ────────────────────────────────────────────
Texture Texture::createFromPixels(const Device &device, const uint32_t *pixels,
                                  uint32_t width, uint32_t height,
                                  VkFormat format) {
  Texture tex;
  tex.m_vkDevice = device.getDevice();

  VkDeviceSize size = static_cast<VkDeviceSize>(width) * height * 4;

  Buffer staging(device.getAllocator(), size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                 VMA_MEMORY_USAGE_CPU_ONLY);
  void *mapped = staging.map();
  std::memcpy(mapped, pixels, static_cast<size_t>(size));
  staging.unmap();

  tex.m_image =
      Image(device, width, height, format,
            VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT);

  transitionImageLayout(device, tex.m_image.getImage(),
                        VK_IMAGE_LAYOUT_UNDEFINED,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  copyBufferToImage(device, staging.getBuffer(), tex.m_image.getImage(), width,
                    height);
  transitionImageLayout(device, tex.m_image.getImage(),
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

  tex.createSampler(device);
  return tex;
}

// ── Sampler ─────────────────────────────────────────────────────────────────
void Texture::createSampler(const Device &device) {
  VkPhysicalDeviceProperties props;
  vkGetPhysicalDeviceProperties(device.getPhysicalDevice(), &props);

  VkSamplerCreateInfo ci{};
  ci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
  ci.magFilter = VK_FILTER_LINEAR;
  ci.minFilter = VK_FILTER_LINEAR;
  ci.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
  ci.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
  ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
  ci.anisotropyEnable = VK_TRUE;
  ci.maxAnisotropy = props.limits.maxSamplerAnisotropy;
  ci.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
  ci.unnormalizedCoordinates = VK_FALSE;
  ci.compareEnable = VK_FALSE;
  ci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;

  VK_CHECK(vkCreateSampler(m_vkDevice, &ci, nullptr, &m_sampler),
           "Failed to create texture sampler");
}

} // namespace glory
