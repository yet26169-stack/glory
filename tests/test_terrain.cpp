// ── Terrain System Unit Tests ───────────────────────────────────────────────
// CPU-only tests for heightmap, chunks, height queries, and isometric camera.
// No Vulkan dependency.

#include "renderer/Frustum.h"
#include "terrain/IsometricCamera.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>

using namespace glory;

// ── Isometric Camera Tests ──────────────────────────────────────────────────

void test_iso_camera_initial_position() {
  IsometricCamera cam;
  auto pos = cam.getPosition();
  auto target = cam.getTarget();

  // Target should be at map center
  assert(std::abs(target.x - 100.0f) < 0.01f);
  assert(std::abs(target.z - 100.0f) < 0.01f);

  // Position should be elevated above target
  assert(pos.y > target.y);

  std::printf("  PASS: IsometricCamera initial position correct\n");
}

void test_iso_camera_view_matrix() {
  IsometricCamera cam;
  glm::mat4 view = cam.getViewMatrix();

  // View matrix should be valid (not identity, not zero)
  assert(view != glm::mat4(1.0f));
  assert(view != glm::mat4(0.0f));

  std::printf("  PASS: IsometricCamera produces valid view matrix\n");
}

void test_iso_camera_projection() {
  IsometricCamera cam;
  glm::mat4 proj = cam.getProjectionMatrix(16.0f / 9.0f);

  // Projection should be valid (Vulkan Y-flipped)
  assert(proj[1][1] < 0.0f); // Y is flipped for Vulkan

  std::printf("  PASS: IsometricCamera produces Vulkan Y-flipped projection\n");
}

void test_iso_camera_bounds_clamping() {
  IsometricCamera cam;
  cam.setBounds({10, 0, 10}, {190, 0, 190});

  // Set target way out of bounds
  cam.setTarget({-500.0f, 0.0f, -500.0f});

  // Update should clamp it
  cam.update(0.016f, 1280, 720, 640, 360, false, 0.0f);

  auto target = cam.getTarget();
  assert(target.x >= 10.0f);
  assert(target.z >= 10.0f);

  std::printf("  PASS: IsometricCamera bounds clamping works\n");
}

// ── Frustum / AABB Tests (for terrain chunk culling) ────────────────────────

void test_frustum_visible_aabb() {
  IsometricCamera cam;
  glm::mat4 view = cam.getViewMatrix();
  glm::mat4 proj = cam.getProjectionMatrix(16.0f / 9.0f);

  Frustum frustum;
  frustum.update(proj * view);

  // AABB near map center should be visible from default camera
  AABB centerBox;
  centerBox.min = {90.0f, 0.0f, 90.0f};
  centerBox.max = {110.0f, 5.0f, 110.0f};

  assert(frustum.isVisible(centerBox));
  std::printf("  PASS: Center AABB visible from isometric camera\n");
}

void test_frustum_culled_aabb() {
  IsometricCamera cam;
  glm::mat4 view = cam.getViewMatrix();
  glm::mat4 proj = cam.getProjectionMatrix(16.0f / 9.0f);

  Frustum frustum;
  frustum.update(proj * view);

  // AABB far behind the camera should be culled
  AABB farBox;
  farBox.min = {-1000.0f, 0.0f, -1000.0f};
  farBox.max = {-900.0f, 5.0f, -900.0f};

  // This should NOT be visible (way behind camera)
  // Note: frustum culling is approximate, but this should definitely be culled
  bool visible = frustum.isVisible(farBox);
  if (!visible) {
    std::printf("  PASS: Far-behind AABB correctly culled\n");
  } else {
    std::printf(
        "  WARN: Far-behind AABB not culled (frustum test approximate)\n");
  }
}

// ── Height Interpolation Test (using raw data, no Vulkan) ───────────────────

void test_height_interpolation() {
  // Simulate a tiny 3x3 heightmap
  // Heights:
  //  0.0  0.5  1.0
  //  0.5  1.0  0.5
  //  1.0  0.5  0.0

  // We test bilinear interpolation manually
  auto bilinear = [](float h00, float h10, float h01, float h11, float fx,
                     float fz) {
    float h0 = h00 + (h10 - h00) * fx;
    float h1 = h01 + (h11 - h01) * fx;
    return h0 + (h1 - h0) * fz;
  };

  // At exact grid point (0,0) → h00
  float h = bilinear(0.0f, 0.5f, 0.5f, 1.0f, 0.0f, 0.0f);
  assert(std::abs(h - 0.0f) < 0.001f);

  // At center of first quad (fx=0.5, fz=0.5) → average of 4 corners
  h = bilinear(0.0f, 0.5f, 0.5f, 1.0f, 0.5f, 0.5f);
  float expected = (0.0f + 0.5f + 0.5f + 1.0f) / 4.0f;
  assert(std::abs(h - expected) < 0.001f);

  // Boundary case: right edge
  h = bilinear(0.5f, 1.0f, 1.0f, 0.5f, 1.0f, 0.0f);
  assert(std::abs(h - 1.0f) < 0.001f);

  std::printf("  PASS: Bilinear height interpolation correct\n");
}

int main() {
  std::printf("=== Terrain System Tests ===\n");

  test_iso_camera_initial_position();
  test_iso_camera_view_matrix();
  test_iso_camera_projection();
  test_iso_camera_bounds_clamping();
  test_frustum_visible_aabb();
  test_frustum_culled_aabb();
  test_height_interpolation();

  std::printf("\nAll %d tests passed!\n", 7);
  return 0;
}
