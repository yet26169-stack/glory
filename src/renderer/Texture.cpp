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

// ═════════════════════════════════════════════════════════════════════════════
// TextureUploadBatch — records transitions + copies into one command buffer
// ═════════════════════════════════════════════════════════════════════════════

TextureUploadBatch::TextureUploadBatch(const Device &device) : m_device(device) {
  // Create a private command pool so this batch can safely record on a
  // background thread without interfering with the main thread's pool.
  VkCommandPoolCreateInfo poolInfo{};
  poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
  poolInfo.queueFamilyIndex = device.getQueueFamilies().graphicsFamily.value();
  VK_CHECK(vkCreateCommandPool(device.getDevice(), &poolInfo, nullptr, &m_pool),
           "TextureUploadBatch: failed to create command pool");
}

TextureUploadBatch::~TextureUploadBatch() {
  if (!empty()) {
    spdlog::warn("TextureUploadBatch destroyed with {} unflushed uploads", m_uploadCount);
  }
  if (m_pool != VK_NULL_HANDLE) {
    // Destroying the pool implicitly frees all command buffers from it.
    vkDestroyCommandPool(m_device.getDevice(), m_pool, nullptr);
  }
}

void TextureUploadBatch::ensureRecording() {
  if (m_recording) return;

  VkCommandBufferAllocateInfo allocInfo{};
  allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  allocInfo.commandPool = m_pool;
  allocInfo.commandBufferCount = 1;
  VK_CHECK(vkAllocateCommandBuffers(m_device.getDevice(), &allocInfo, &m_cmd),
           "TextureUploadBatch: failed to allocate command buffer");

  VkCommandBufferBeginInfo beginInfo{};
  beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  VK_CHECK(vkBeginCommandBuffer(m_cmd, &beginInfo),
           "TextureUploadBatch: failed to begin command buffer");

  m_recording = true;
}

void TextureUploadBatch::stageUpload(VkImage image, const void *pixels,
                                     uint32_t width, uint32_t height,
                                     VkFormat format, VmaAllocator allocator) {
  ensureRecording();

  VkDeviceSize imageSize = static_cast<VkDeviceSize>(width) * height * 4;

  m_stagingBuffers.emplace_back(allocator, imageSize,
                                VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                VMA_MEMORY_USAGE_CPU_ONLY);
  Buffer &staging = m_stagingBuffers.back();
  void *mapped = staging.map();
  std::memcpy(mapped, pixels, static_cast<size_t>(imageSize));
  staging.unmap();
  staging.flush();

  // Barrier: UNDEFINED → TRANSFER_DST
  VkImageMemoryBarrier2 toTransfer{};
  toTransfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
  toTransfer.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  toTransfer.image = image;
  toTransfer.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  toTransfer.srcStageMask  = VK_PIPELINE_STAGE_2_NONE;
  toTransfer.srcAccessMask = VK_ACCESS_2_NONE;
  toTransfer.dstStageMask  = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
  toTransfer.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;

  VkDependencyInfo depPre{};
  depPre.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
  depPre.imageMemoryBarrierCount = 1;
  depPre.pImageMemoryBarriers = &toTransfer;
  vkCmdPipelineBarrier2(m_cmd, &depPre);

  // Copy staging → image
  VkBufferImageCopy region{};
  region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.imageExtent = {width, height, 1};
  vkCmdCopyBufferToImage(m_cmd, staging.getBuffer(), image,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

  // Barrier: TRANSFER_DST → SHADER_READ_ONLY
  VkImageMemoryBarrier2 toShader{};
  toShader.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
  toShader.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  toShader.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  toShader.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  toShader.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  toShader.image = image;
  toShader.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  toShader.srcStageMask  = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
  toShader.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
  toShader.dstStageMask  = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
  toShader.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;

  VkDependencyInfo depPost{};
  depPost.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
  depPost.imageMemoryBarrierCount = 1;
  depPost.pImageMemoryBarriers = &toShader;
  vkCmdPipelineBarrier2(m_cmd, &depPost);

  ++m_uploadCount;
}

void TextureUploadBatch::flush() {
  if (!m_recording || m_uploadCount == 0) return;

  VK_CHECK(vkEndCommandBuffer(m_cmd), "TextureUploadBatch: failed to end command buffer");

  VkCommandBufferSubmitInfo cmdInfo{};
  cmdInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
  cmdInfo.commandBuffer = m_cmd;

  VkSubmitInfo2 submitInfo{};
  submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
  submitInfo.commandBufferInfoCount = 1;
  submitInfo.pCommandBufferInfos = &cmdInfo;

  VK_CHECK(m_device.submitGraphics(1, &submitInfo), "TextureUploadBatch: submit failed");
  m_device.graphicsQueueWaitIdle();

  spdlog::info("TextureUploadBatch: flushed {} uploads in 1 GPU submission",
               m_uploadCount);

  // Release staging memory
  m_stagingBuffers.clear();

  // Reset pool instead of freeing individual command buffers — faster.
  vkResetCommandPool(m_device.getDevice(), m_pool, 0);
  m_cmd = VK_NULL_HANDLE;
  m_uploadCount = 0;
  m_recording = false;
}

namespace {
void transitionImageLayout(const Device &device, VkImage image,
                           VkImageLayout oldLayout, VkImageLayout newLayout) {
  // Always use the graphics queue for layout transitions.
  auto poolLock = device.lockGraphicsPool();
  VkCommandPool pool  = device.getGraphicsCommandPool();

  VkCommandBufferAllocateInfo allocInfo{};
  allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  allocInfo.commandPool = pool;
  allocInfo.commandBufferCount = 1;

  VkCommandBuffer cmd;
  vkAllocateCommandBuffers(device.getDevice(), &allocInfo, &cmd);

  VkCommandBufferBeginInfo beginInfo{};
  beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(cmd, &beginInfo);

  VkImageMemoryBarrier2 barrier{};
  barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
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

  if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED &&
      newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
    barrier.srcStageMask  = VK_PIPELINE_STAGE_2_NONE;
    barrier.srcAccessMask = VK_ACCESS_2_NONE;
    barrier.dstStageMask  = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
  } else if (oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL &&
             newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
    barrier.srcStageMask  = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    barrier.dstStageMask  = VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_NONE;
  } else {
    throw std::runtime_error("Unsupported layout transition");
  }

  VkDependencyInfo depInfo{};
  depInfo.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
  depInfo.imageMemoryBarrierCount = 1;
  depInfo.pImageMemoryBarriers    = &barrier;
  vkCmdPipelineBarrier2(cmd, &depInfo);

  vkEndCommandBuffer(cmd);

  VkCommandBufferSubmitInfo cmdInfo{};
  cmdInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
  cmdInfo.commandBuffer = cmd;

  VkSubmitInfo2 submitInfo{};
  submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
  submitInfo.commandBufferInfoCount = 1;
  submitInfo.pCommandBufferInfos = &cmdInfo;

  device.submitGraphics(1, &submitInfo);
  device.graphicsQueueWaitIdle();

  vkFreeCommandBuffers(device.getDevice(), pool, 1, &cmd);
}

void copyBufferToImage(const Device &device, VkBuffer buffer, VkImage image,
                       uint32_t width, uint32_t height) {
  auto poolLock = device.lockTransferPool();
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

  VkCommandBufferSubmitInfo cmdInfo{};
  cmdInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
  cmdInfo.commandBuffer = cmd;

  VkSubmitInfo2 submitInfo{};
  submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
  submitInfo.commandBufferInfoCount = 1;
  submitInfo.pCommandBufferInfos = &cmdInfo;

  device.submitTransfer(1, &submitInfo);
  device.transferQueueWaitIdle();

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
  staging.flush();
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

Texture::Texture(Image &&image)
    : m_image(std::move(image)) {
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
  staging.flush();

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

Texture Texture::createRenderable(const Device &device, uint32_t width, uint32_t height,
                                  VkFormat format) {
  Texture tex;
  tex.m_vkDevice = device.getDevice();
  tex.m_image = Image(device, width, height, format,
                      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                      VK_IMAGE_USAGE_SAMPLED_BIT |
                      VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                      VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                      VK_IMAGE_ASPECT_COLOR_BIT);
  tex.createSampler(device);
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
  staging.flush();

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
  staging.flush();

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
  staging.flush();

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


// ── Generic pixel upload factory ────────────────────────────────────────────
Texture Texture::createFromPixels(const Device &device, const uint32_t *pixels,
                                  uint32_t width, uint32_t height,
                                  VkFormat format, TextureUploadBatch *batch) {
  Texture tex;
  tex.m_vkDevice = device.getDevice();

  tex.m_image =
      Image(device, width, height, format,
            VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT);

  if (batch) {
    // Deferred: record into the batch command buffer (no GPU stall now)
    batch->stageUpload(tex.m_image.getImage(), pixels, width, height,
                       format, device.getAllocator());
  } else {
    // Immediate: legacy path with per-texture GPU stalls
    VkDeviceSize size = static_cast<VkDeviceSize>(width) * height * 4;

    Buffer staging(device.getAllocator(), size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                   VMA_MEMORY_USAGE_CPU_ONLY);
    void *mapped = staging.map();
    std::memcpy(mapped, pixels, static_cast<size_t>(size));
    staging.unmap();
    staging.flush();

    transitionImageLayout(device, tex.m_image.getImage(),
                          VK_IMAGE_LAYOUT_UNDEFINED,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    copyBufferToImage(device, staging.getBuffer(), tex.m_image.getImage(), width,
                      height);
    transitionImageLayout(device, tex.m_image.getImage(),
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  }

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
