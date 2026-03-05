#pragma once

#include "scene/Components.h"
#include "renderer/DynamicMesh.h"
#include "renderer/Model.h"
#include "renderer/StaticSkinnedMesh.h"
#include "renderer/Texture.h"
#include "renderer/Material.h"

#include <entt.hpp>
#include <glm/glm.hpp>

#include <string>
#include <vector>

namespace glory {

class Device;
class TerrainSystem;

class Scene {
public:
    Scene() = default;

    entt::registry&       getRegistry()       { return m_registry; }
    const entt::registry& getRegistry() const { return m_registry; }

    entt::entity createEntity(const std::string& name = "Entity");
    void destroyEntity(entt::entity entity);

    uint32_t addMesh(Model model);
    uint32_t addTexture(Texture texture);
    uint32_t addMaterial(Material material);

    Model&    getMesh(uint32_t index)     { return m_meshes[index]; }
    Texture&  getTexture(uint32_t index)  { return m_textures[index]; }
    Material& getMaterial(uint32_t index) { return m_materials[index]; }

    const std::vector<Model>&    getMeshes()    const { return m_meshes; }
    const std::vector<Texture>&  getTextures()  const { return m_textures; }
    const std::vector<Material>& getMaterials() const { return m_materials; }

    void setTerrainSystem(TerrainSystem* terrain) { m_terrain = terrain; }

    // Dynamic mesh management (legacy CPU-skinned characters)
    uint32_t addDynamicMesh(DynamicMesh mesh);
    DynamicMesh& getDynamicMesh(uint32_t index) { return m_dynamicMeshes[index]; }
    const std::vector<DynamicMesh>& getDynamicMeshes() const { return m_dynamicMeshes; }

    // Static skinned mesh management (GPU-skinned via vertex shader + bone SSBO)
    uint32_t addStaticSkinnedMesh(StaticSkinnedMesh mesh);
    StaticSkinnedMesh& getStaticSkinnedMesh(uint32_t index) { return m_staticSkinnedMeshes[index]; }
    const std::vector<StaticSkinnedMesh>& getStaticSkinnedMeshes() const { return m_staticSkinnedMeshes; }

    void update(float deltaTime, uint32_t currentFrame);
    bool getFirstLight(glm::vec3& outPos, glm::vec3& outColor) const;
    uint32_t getAllLights(std::vector<std::pair<glm::vec3, glm::vec3>>& outLights) const;

private:
    entt::registry         m_registry;
    std::vector<Model>     m_meshes;
    std::vector<Texture>   m_textures;
    std::vector<Material>  m_materials;
    std::vector<DynamicMesh> m_dynamicMeshes;
    std::vector<StaticSkinnedMesh> m_staticSkinnedMeshes;
    TerrainSystem*         m_terrain = nullptr;
};

} // namespace glory
