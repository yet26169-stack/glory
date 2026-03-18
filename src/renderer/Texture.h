#pragma once

#include "renderer/Image.h"

#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

#include <string>

namespace glory {

class Device;

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
  static Texture createFromPixels(const Device &device, const uint32_t *pixels,
                                  uint32_t width, uint32_t height,
                                  VkFormat format = VK_FORMAT_R8G8B8A8_SRGB);

private:
  Image m_image;
  VkSampler m_sampler = VK_NULL_HANDLE;
  VkDevice m_vkDevice = VK_NULL_HANDLE;

  void createSampler(const Device &device);
  void destroy();
};

} // namespace glory
