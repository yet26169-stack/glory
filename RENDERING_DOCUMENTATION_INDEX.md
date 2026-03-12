# Glory Engine - Rendering & VFX System Documentation Index

## 📚 Documentation Files

### 1. **VFX_QUICK_START.md** ⭐ START HERE
- **Length**: ~330 lines
- **Time to read**: 5-10 minutes
- **Best for**: Quick reference, common recipes, getting started immediately

**Key Sections**:
- TL;DR: Adding VFX in 5 minutes
- Understanding the pipeline
- Key parameters explained
- Common effect recipes (fireball, shield, explosion, healing)
- Debug overlay controls
- Fog adjustment
- Troubleshooting

**When to use**:
- You need to add a new VFX effect **now**
- You want to understand parameters (emitRate, gravity, spreadAngle, etc.)
- You're debugging a specific VFX issue
- You need example JSON configurations

---

### 2. **RENDERING_VFX_SYSTEM_DEEP_DIVE.md** 📖 COMPREHENSIVE REFERENCE
- **Length**: ~1,100 lines (35 KB)
- **Time to read**: 30-60 minutes (or reference as needed)
- **Best for**: Complete understanding, technical implementation, troubleshooting

**Key Sections**:
1. Shader Files Inventory (12 shaders explained)
2. VFX System Architecture
3. Core Types & Data Structures
4. SPSC Event Queue (lock-free communication)
5. ParticleSystem (single emitter)
6. VFXRenderer (master orchestrator)
7. Main Renderer Integration
8. Debug UI & Overlay Rendering
9. Fog Rendering System
10. Shader Infrastructure for New VFX
11. Adding New VFX (attack slashes, shields, projectiles)
12. Rendering Debug UI Overlays
13. Integration Points & Extension
14. Performance Considerations
15. Troubleshooting & Common Issues
16. Summary Tables

**When to use**:
- You want to **understand how the system works end-to-end**
- You need to **debug complex issues**
- You're **implementing new rendering features**
- You want to **optimize performance**
- You need to **extend the shader system**

---

### 3. **RENDERING_SUMMARY.txt** 📋 QUICK CHECKLIST
- **Length**: ~360 lines
- **Time to read**: 5-15 minutes
- **Best for**: Quick reference, file locations, limits, troubleshooting

**Key Sections**:
- Executive checklist
- Shader files inventory
- VFX system architecture overview
- Rendering pipeline features
- Debug UI & overlays
- Fog system parameters
- Data-driven systems (JSON)
- Key source files with locations
- Performance stats & limits
- Troubleshooting checklist
- "Questions? Check..." index

**When to use**:
- You need a **quick reference** while coding
- You're **searching for a specific file**
- You want to **verify limits** (max particles, emitters, etc.)
- You're **troubleshooting** and need a checklist
- You need a **one-page overview**

---

### 4. **Vulkan_VFX_TDD_for_MOBA.md** 🏗️ DESIGN DOCUMENT (Existing)
- **Length**: ~240 lines
- **Best for**: Original design, architecture decisions, roadmap

**Key Sections**:
- Overview & architecture
- Per-frame update order
- GPU Particle Layout
- VFX Event Queue
- Compute shader algorithm
- Billboard vertex shader
- Descriptor set layout
- Memory strategy
- Ability system state machine
- Data-driven emitter JSON
- CC priority hierarchy
- File reference
- Roadmap

**When to use**:
- You want to understand the **original design decisions**
- You're interested in the **architectural rationale**
- You want to check the **development roadmap**
- You need to understand the **ability system integration**

---

## 🎯 Quick Navigation by Task

### "I need to add a new particle effect"
→ **VFX_QUICK_START.md** (Section: "TL;DR: Adding New VFX in 5 Minutes")

### "Particles aren't rendering"
→ **RENDERING_SUMMARY.txt** (Section: "Troubleshooting Checklist")  
→ **RENDERING_VFX_SYSTEM_DEEP_DIVE.md** (Section: 12, "Troubleshooting & Common Issues")

### "I want to customize particle behavior"
→ **RENDERING_VFX_SYSTEM_DEEP_DIVE.md** (Section: 7, "Shader Infrastructure for New VFX")

### "How do I add attack slashes/shields?"
→ **VFX_QUICK_START.md** (Section: "Adding Attack Slashes & Shields")  
→ **RENDERING_VFX_SYSTEM_DEEP_DIVE.md** (Section: 8.1-8.3, "Adding New VFX")

### "How do I adjust fog?"
→ **VFX_QUICK_START.md** (Section: "Fog Adjustment")  
→ **RENDERING_VFX_SYSTEM_DEEP_DIVE.md** (Section: 6, "Fog Rendering")

### "I want to draw debug overlays"
→ **RENDERING_VFX_SYSTEM_DEEP_DIVE.md** (Section: 9, "Rendering Debug UI Overlays")

### "I want to understand the whole system"
→ Start with **VFX_QUICK_START.md** (5 min)  
→ Then **RENDERING_VFX_SYSTEM_DEEP_DIVE.md** (45 min)

### "I need to find a specific source file"
→ **RENDERING_SUMMARY.txt** (Section: "Key Source Files")  
→ **RENDERING_VFX_SYSTEM_DEEP_DIVE.md** (Section: 13, "Summary Table: Files & Purposes")

### "What are the performance limits?"
→ **RENDERING_SUMMARY.txt** (Section: "Performance Stats")  
→ **RENDERING_VFX_SYSTEM_DEEP_DIVE.md** (Section: 11, "Performance Considerations")

---

## 📊 File Organization Reference

### All Shader Files (12 total)
```
/Users/donkey/Development/1/Glory/shaders/
├── particle.vert              ← Billboard expansion
├── particle.frag              ← Texture sampling
├── particle_sim.comp          ← GPU simulation
├── triangle.vert              ← PBR model vertex
├── triangle.frag              ← PBR with fog
├── skinned.vert               ← GPU skinning
├── debug.vert/frag            ← Debug lines
├── grid.vert/frag             ← Infinite grid
└── click_indicator.vert/frag  ← Click UI
```

### VFX System Source Files
```
/Users/donkey/Development/1/Glory/src/vfx/
├── VFXTypes.h                 ← Data structures
├── VFXEventQueue.h            ← Lock-free queue
├── ParticleSystem.h/cpp       ← Single emitter
└── VFXRenderer.h/cpp          ← Master orchestrator
```

### Rendering System Source Files
```
/Users/donkey/Development/1/Glory/src/renderer/
├── Renderer.h/cpp             ← Main loop
├── Pipeline.h/cpp             ← Render pass
├── Descriptors.h              ← Fog params, light data
├── ClickIndicatorRenderer.h/cpp
└── Device.h, Buffer.h, Texture.h, etc.
```

### Debug/Visualization
```
/Users/donkey/Development/1/Glory/src/nav/
└── DebugRenderer.h/cpp        ← Debug shapes & lines
```

### Data-Driven Assets
```
/Users/donkey/Development/1/Glory/assets/
├── vfx/                       ← Particle definitions (JSON)
├── abilities/                 ← Ability definitions (JSON)
└── textures/particles/        ← Particle atlases (PNG)
```

---

## 🔑 Key Concepts (One Sentence Each)

| Concept | Definition |
|---------|-----------|
| **GpuParticle** | 64-byte struct (pos, vel, color, params) stored in SSBO |
| **EmitterDef** | JSON config (emitRate, gravity, colors, spread, etc.) |
| **VFXEvent** | Spawn/Destroy/Move command from game thread to render thread |
| **SPSC Queue** | Lock-free 256-slot ring buffer (Single Producer, Single Consumer) |
| **ParticleSystem** | Active emitter instance with SSBO and CPU emission state |
| **VFXRenderer** | Master orchestrator managing compute + graphics pipelines |
| **Billboard** | Particle rendered as camera-facing quad in vertex shader |
| **Compute Shader** | GPU kernel (particle_sim.comp) simulates all particles in-place |
| **Descriptor Set** | Vulkan resource binding (SSBO + texture sampler) per emitter |
| **Memory Barrier** | Synchronization between compute write and vertex read |
| **PBR** | Physically-Based Rendering (Cook-Torrance BRDF) |
| **Fog** | Exponential distance fade with color blending |
| **DebugRenderer** | Immediate-mode line/shape drawing for debug visualization |
| **ClickIndicator** | Sprite-based UI animation for click feedback |

---

## 📝 Common Workflows

### Workflow 1: Add a New Particle Effect (5 min)
1. Open **VFX_QUICK_START.md**, Section "TL;DR"
2. Create `assets/vfx/vfx_my_effect.json`
3. Reference in ability JSON
4. Done! System loads and fires automatically

### Workflow 2: Debug Why Particles Won't Render (15 min)
1. Check **RENDERING_SUMMARY.txt**, "Troubleshooting Checklist"
2. If still stuck, read **RENDERING_VFX_SYSTEM_DEEP_DIVE.md**, Section 12
3. Verify particle count, descriptor bindings, pipeline config

### Workflow 3: Customize Particle Shader (30 min)
1. Read **RENDERING_VFX_SYSTEM_DEEP_DIVE.md**, Section 7.2
2. Modify `shaders/particle.vert` or `particle.frag`
3. Recompile SPIR-V (glslc or similar)
4. Restart engine

### Workflow 4: Add Debug Overlay (20 min)
1. Read **RENDERING_VFX_SYSTEM_DEEP_DIVE.md**, Section 9
2. Use `m_debugRenderer.drawLine/Circle/AABB/Sphere()`
3. Call `m_debugRenderer.render(cmd, viewProj)` during render pass

### Workflow 5: Understand Performance (30 min)
1. Read **RENDERING_SUMMARY.txt**, "Performance Stats"
2. Read **RENDERING_VFX_SYSTEM_DEEP_DIVE.md**, Section 11
3. Check current limits (2048 particles/emitter, 32 emitters)

---

## 🚀 Getting Started

### For New Developers (30 min)
1. Read **VFX_QUICK_START.md** completely (5 min)
2. Skim **RENDERING_VFX_SYSTEM_DEEP_DIVE.md** sections 1-5 (15 min)
3. Look at example JSONs in `assets/vfx/` (5 min)
4. Create a simple test effect (5 min)

### For Rendering Engineers (2 hours)
1. Read **VFX_QUICK_START.md** (10 min)
2. Read **RENDERING_VFX_SYSTEM_DEEP_DIVE.md** completely (60 min)
3. Study shader code: `shaders/particle_*.{vert,frag,comp}` (30 min)
4. Study source code: `src/vfx/VFXRenderer.cpp`, `src/renderer/Renderer.cpp` (20 min)

### For Performance Optimization (1 hour)
1. Read **RENDERING_SUMMARY.txt**, "Performance Stats" (10 min)
2. Read **RENDERING_VFX_SYSTEM_DEEP_DIVE.md**, Section 11 (20 min)
3. Profile with current limits (15 min)
4. Experiment with adjustments (15 min)

---

## 📞 Support

**Can't find what you're looking for?**

Search the files for keywords:
- **"fog"** → RENDERING_SUMMARY.txt or RENDERING_VFX_SYSTEM_DEEP_DIVE.md Section 6
- **"particle"** → RENDERING_VFX_SYSTEM_DEEP_DIVE.md Section 2-5
- **"shader"** → RENDERING_VFX_SYSTEM_DEEP_DIVE.md Section 1
- **"debug"** → RENDERING_VFX_SYSTEM_DEEP_DIVE.md Section 9
- **"performance"** → RENDERING_VFX_SYSTEM_DEEP_DIVE.md Section 11
- **"troubleshoot"** → RENDERING_SUMMARY.txt or RENDERING_VFX_SYSTEM_DEEP_DIVE.md Section 12

---

## 📄 Document Statistics

| Document | Lines | Size | Focus | Audience |
|----------|-------|------|-------|----------|
| VFX_QUICK_START.md | 333 | 7.4 KB | Practical | Developers |
| RENDERING_VFX_SYSTEM_DEEP_DIVE.md | 1,102 | 35 KB | Technical | Engineers |
| RENDERING_SUMMARY.txt | 357 | 13 KB | Reference | Everyone |
| Vulkan_VFX_TDD_for_MOBA.md | 244 | - | Architecture | Tech Leads |
| **TOTAL** | **2,036** | **55+ KB** | **Complete** | **All Levels** |

---

**Last Updated**: March 7, 2025  
**Engine**: Glory (Vulkan / C++20 / entt ECS)  
**Status**: Documentation Complete ✓
