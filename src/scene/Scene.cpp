#include "scene/Scene.h"
#include "animation/CPUSkinning.h"

#include <cmath>
#include <spdlog/spdlog.h>

namespace glory {

entt::entity Scene::createEntity(const std::string &name) {
  auto entity = m_registry.create();
  m_registry.emplace<TagComponent>(entity, TagComponent{name});
  m_registry.emplace<TransformComponent>(entity);
  return entity;
}

void Scene::destroyEntity(entt::entity entity) { m_registry.destroy(entity); }

uint32_t Scene::addMesh(Model model) {
  uint32_t idx = static_cast<uint32_t>(m_meshes.size());
  m_meshes.push_back(std::move(model));
  return idx;
}

uint32_t Scene::addTexture(Texture texture) {
  uint32_t idx = static_cast<uint32_t>(m_textures.size());
  m_textures.push_back(std::move(texture));
  return idx;
}

uint32_t Scene::addMaterial(Material material) {
  uint32_t idx = static_cast<uint32_t>(m_materials.size());
  m_materials.push_back(std::move(material));
  return idx;
}

uint32_t Scene::addDynamicMesh(DynamicMesh mesh) {
  uint32_t idx = static_cast<uint32_t>(m_dynamicMeshes.size());
  m_dynamicMeshes.push_back(std::move(mesh));
  return idx;
}

uint32_t Scene::addStaticSkinnedMesh(StaticSkinnedMesh mesh) {
  uint32_t idx = static_cast<uint32_t>(m_staticSkinnedMeshes.size());
  m_staticSkinnedMeshes.push_back(std::move(mesh));
  return idx;
}

void Scene::update(float deltaTime, uint32_t currentFrame) {
  // Rotation animation
  auto rotView = m_registry.view<TransformComponent, RotateComponent>();
  for (auto [entity, transform, rotate] : rotView.each())
    transform.rotation += rotate.axis * rotate.speed * deltaTime;

  // Orbit animation
  auto orbitView = m_registry.view<TransformComponent, OrbitComponent>();
  for (auto [entity, transform, orbit] : orbitView.each()) {
    orbit.phase += orbit.speed * deltaTime;
    transform.position.x = orbit.center.x + orbit.radius * std::cos(orbit.phase);
    transform.position.z = orbit.center.z + orbit.radius * std::sin(orbit.phase);
    transform.position.y = orbit.center.y + orbit.height;
  }

  // CPU-skinned mesh animation
  auto animView = m_registry.view<SkeletonComponent, AnimationComponent, DynamicMeshComponent>();
  for (auto [entity, skelComp, animComp, dynComp] : animView.each()) {
    animComp.player.refreshSkeleton(&skelComp.skeleton);
    animComp.player.update(deltaTime);
    if (dynComp.dynamicMeshIndex < m_dynamicMeshes.size() &&
        !skelComp.bindPoseVertices.empty() && !skelComp.skinVertices.empty()) {
      auto& dynMesh = m_dynamicMeshes[dynComp.dynamicMeshIndex];
      applyCPUSkinning(skelComp.bindPoseVertices[0], skelComp.skinVertices[0],
                       animComp.player.getSkinningMatrices(), animComp.skinnedVertices);
      dynMesh.updateVertices(currentFrame, animComp.skinnedVertices);
    }
  }

  // GPU-skinned: advance animation only (bone matrices uploaded by Renderer)
  auto gpuAnimView = m_registry.view<SkeletonComponent, AnimationComponent, GPUSkinnedMeshComponent>();
  for (auto [entity, skelComp, animComp, ssm] : gpuAnimView.each()) {
    animComp.player.refreshSkeleton(&skelComp.skeleton);
    animComp.player.update(deltaTime);
  }
}

bool Scene::getFirstLight(glm::vec3 &outPos, glm::vec3 &outColor) const {
  auto view = m_registry.view<TransformComponent, LightComponent>();
  for (auto [entity, t, l] : view.each()) {
    outPos   = t.position;
    outColor = l.color * l.intensity;
    return true;
  }
  return false;
}

uint32_t Scene::getAllLights(std::vector<std::pair<glm::vec3, glm::vec3>> &outLights) const {
  outLights.clear();
  auto view = m_registry.view<TransformComponent, LightComponent>();
  for (auto [entity, t, l] : view.each())
    outLights.emplace_back(t.position, l.color * l.intensity);
  return static_cast<uint32_t>(outLights.size());
}

} // namespace glory
