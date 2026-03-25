#pragma once

#include "renderer/Buffer.h"
#include "renderer/Image.h"

#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

#include <string>
#include <vector>

namespace glory {

class Device;

// ── TextureUploadBatch ──────────────────────────────────────────────────────
// Collects texture upload commands (layout transitions + buffer→image copies)
// into a single command buffer so they can be flushed with one GPU submission
// instead of one vkQueueWaitIdle per texture.
class TextureUploadBatch {
public:
  explicit TextureUploadBatch(const Device &device);
  ~TextureUploadBatch();

  TextureUploadBatch(const TextureUploadBatch &) = delete;
  TextureUploadBatch &operator=(const TextureUploadBatch &) = delete;

  // Record a transition + copy + transition for one texture image.
  // The staging buffer is kept alive internally until flush().
  void stageUpload(VkImage image, const void *pixels,
                   uint32_t width, uint32_t height, VkFormat format,
                   VmaAllocator allocator);

  // Submit the command buffer and wait for all uploads to complete.
  // After flush(), the batch can be reused by calling stageUpload() again.
  void flush();

  bool empty() const { return m_uploadCount == 0; }

private:
  const Device &m_device;
  VkCommandPool m_pool = VK_NULL_HANDLE;  // private pool for thread safety
  VkCommandBuffer m_cmd = VK_NULL_HANDLE;
  std::vector<Buffer> m_stagingBuffers;
  uint32_t m_uploadCount = 0;
  bool m_recording = false;

  void ensureRecording();
};

class Texture {
public:
  Texture() = default;
  Texture(const Device &device, const std::string &filepath);
  Texture(Image &&image);
  ~Texture();

  Texture(const Texture &) = delete;
  Texture &operator=(const Texture &) = delete;
  Texture(Texture &&other) noexcept;
  Texture &operator=(Texture &&other) noexcept;

  VkImageView getImageView() const { return m_image.getImageView(); }
  VkImage     getImage()     const { return m_image.getImage(); }
  VkSampler getSampler() const { return m_sampler; }

  // 1x1 white fallback texture
  static Texture createDefault(const Device &device);
  
  /// Create a texture that can be used as a color attachment and sampled.
  static Texture createRenderable(const Device &device, uint32_t width, uint32_t height,
                                  VkFormat format = VK_FORMAT_R8G8B8A8_UNORM);
  // Procedural checkerboard (tileSize in pixels, total = tileSize*tiles*2)
  static Texture createCheckerboard(const Device &device, uint32_t tiles = 8,
                                    uint32_t tileSize = 16,
                                    uint32_t colorA = 0xFFCCCCCC,
                                    uint32_t colorB = 0xFF444444);

  // Flat normal map (default — no perturbation)
  static Texture createFlatNormal(const Device &device);
  // Procedural brick normal map (256x256)
  static Texture createBrickNormal(const Device &device);
  // Procedural marble texture (256x256, SRGB)
  static Texture createMarble(const Device &device);
  static Texture createWood(const Device &device);
  static Texture createLava(const Device &device);
  static Texture createRock(const Device &device);
  static Texture createBrushedMetal(const Device &device);
  static Texture createTiles(const Device &device);
  static Texture createCircuit(const Device &device);
  static Texture createHexGrid(const Device &device);
  static Texture createGradient(const Device &device);
  static Texture createNoise(const Device &device);

  /// Create texture from raw RGBA pixel data (generic factory).
  /// If batch is non-null, the upload is deferred; call batch->flush() later.
  static Texture createFromPixels(const Device &device, const uint32_t *pixels,
                                  uint32_t width, uint32_t height,
                                  VkFormat format = VK_FORMAT_R8G8B8A8_SRGB,
                                  TextureUploadBatch *batch = nullptr);

private:
  Image m_image;
  VkSampler m_sampler = VK_NULL_HANDLE;
  VkDevice m_vkDevice = VK_NULL_HANDLE;

  void createSampler(const Device &device);
  void destroy();
};

} // namespace glory
