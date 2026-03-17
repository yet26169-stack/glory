#include "core/GameState.h"
#include "renderer/Renderer.h"
#include "combat/HeroDefinition.h"
#include "window/Window.h"

#include <imgui.h>
#include <spdlog/spdlog.h>

namespace glory {

// ── GameStateMachine ─────────────────────────────────────────────────────────

GameStateMachine::GameStateMachine(Renderer& renderer, Window& window)
    : m_renderer(renderer), m_window(window)
{
    m_states[GameStateType::MAIN_MENU]   = std::make_unique<MainMenuState>();
    m_states[GameStateType::HERO_SELECT] = std::make_unique<HeroSelectState>();
    m_states[GameStateType::LOADING]     = std::make_unique<LoadingState>();
    m_states[GameStateType::IN_GAME]     = std::make_unique<InGameState>();
    m_states[GameStateType::POST_GAME]   = std::make_unique<PostGameState>();

    doTransition(GameStateType::MAIN_MENU);
}

void GameStateMachine::transition(GameStateType state) {
    m_pendingType = state;
    m_hasPending  = true;
}

void GameStateMachine::update(float dt) {
    if (m_hasPending) {
        m_hasPending = false;
        doTransition(m_pendingType);
    }
    if (m_current) m_current->update(*this, dt);
}

void GameStateMachine::render(float alpha) {
    if (m_current) m_current->render(*this, alpha);
}

void GameStateMachine::doTransition(GameStateType newState) {
    if (m_current) m_current->exit(*this);
    m_currentType = newState;
    m_current = m_states[newState].get();
    if (m_current) m_current->enter(*this);
    spdlog::info("GameState -> {}", static_cast<int>(newState));
}

// ── MainMenuState ────────────────────────────────────────────────────────────

void MainMenuState::enter(GameStateMachine& sm) {
    sm.renderer().setMenuMode(true);
}

void MainMenuState::update(GameStateMachine& /*sm*/, float /*dt*/) {}

void MainMenuState::render(GameStateMachine& sm, float /*alpha*/) {
    sm.renderer().setMenuRenderer([&sm] {
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);

        ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration
                               | ImGuiWindowFlags_NoMove
                               | ImGuiWindowFlags_NoResize
                               | ImGuiWindowFlags_NoSavedSettings;

        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.1f, 0.1f, 0.12f, 1.0f));

        if (ImGui::Begin("MainMenu", nullptr, flags)) {
            float w = ImGui::GetWindowWidth();
            float h = ImGui::GetWindowHeight();

            // Title
            ImGui::SetCursorPosY(h * 0.25f);
            const char* title = "GLORY ENGINE";
            float tw = ImGui::CalcTextSize(title).x;
            ImGui::SetCursorPosX((w - tw) * 0.5f);
            ImGui::Text("%s", title);

            // Subtitle
            ImGui::SetCursorPosY(h * 0.30f);
            const char* sub = "Alpha Client";
            float sw = ImGui::CalcTextSize(sub).x;
            ImGui::SetCursorPosX((w - sw) * 0.5f);
            ImGui::TextDisabled("%s", sub);

            // Play button
            ImVec2 playSize(240, 60);
            ImGui::SetCursorPos(ImVec2((w - playSize.x) * 0.5f, h * 0.45f));
            if (ImGui::Button("PLAY", playSize)) {
                sm.transition(GameStateType::HERO_SELECT);
            }

            // Quit button
            ImVec2 quitSize(240, 40);
            ImGui::SetCursorPos(ImVec2((w - quitSize.x) * 0.5f, h * 0.55f));
            if (ImGui::Button("QUIT", quitSize)) {
                sm.requestQuit();
            }

            // Footer
            const char* footer = "Press TAB in-game for Debug Tools";
            float fw = ImGui::CalcTextSize(footer).x;
            ImGui::SetCursorPos(ImVec2((w - fw) * 0.5f, h - 40));
            ImGui::TextDisabled("%s", footer);
        }
        ImGui::End();
        ImGui::PopStyleColor();
    });

    sm.renderer().renderFrame(0.0f);
}

void MainMenuState::exit(GameStateMachine& sm) {
    sm.renderer().setMenuRenderer(nullptr);
}

// ── HeroSelectState ──────────────────────────────────────────────────────────

void HeroSelectState::enter(GameStateMachine& sm) {
    sm.renderer().setMenuMode(true);
    m_selectedHero = -1;
}

void HeroSelectState::update(GameStateMachine& /*sm*/, float /*dt*/) {}

void HeroSelectState::render(GameStateMachine& sm, float /*alpha*/) {
    sm.renderer().setMenuRenderer([this, &sm] {
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);

        ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration
                               | ImGuiWindowFlags_NoMove
                               | ImGuiWindowFlags_NoResize
                               | ImGuiWindowFlags_NoSavedSettings;

        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.1f, 0.1f, 0.12f, 1.0f));

        if (ImGui::Begin("HeroSelect", nullptr, flags)) {
            float w = ImGui::GetWindowWidth();
            float h = ImGui::GetWindowHeight();

            // Title
            ImGui::SetCursorPosY(h * 0.08f);
            const char* title = "SELECT YOUR HERO";
            float tw = ImGui::CalcTextSize(title).x;
            ImGui::SetCursorPosX((w - tw) * 0.5f);
            ImGui::Text("%s", title);

            // Build hero list from registry, fall back to hardcoded if empty
            const auto& registry = sm.renderer().getHeroRegistry();
            const bool useRegistry = registry.count() > 0;

            // Hardcoded fallback names/colors
            const char* fallbackNames[HERO_COUNT] = {
                "Warrior", "Mage", "Ranger", "Tank",
                "Assassin", "Support", "Summoner", "Berserker"
            };
            ImVec4 fallbackColors[HERO_COUNT] = {
                {0.8f, 0.2f, 0.2f, 1.0f}, {0.2f, 0.3f, 0.9f, 1.0f},
                {0.2f, 0.8f, 0.3f, 1.0f}, {0.8f, 0.8f, 0.2f, 1.0f},
                {0.6f, 0.2f, 0.8f, 1.0f}, {0.2f, 0.8f, 0.8f, 1.0f},
                {0.9f, 0.5f, 0.1f, 1.0f}, {0.9f, 0.4f, 0.7f, 1.0f},
            };

            const int heroCount = useRegistry
                ? static_cast<int>(registry.count())
                : HERO_COUNT;

            constexpr float cardW = 100.0f;
            constexpr float cardH = 100.0f;
            constexpr float gap   = 16.0f;
            constexpr int cols = 4;
            float gridW = cols * cardW + (cols - 1) * gap;
            float startX = (w - gridW) * 0.5f;
            float startY = h * 0.20f;

            for (int i = 0; i < heroCount; ++i) {
                int col = i % cols;
                int row = i / cols;
                float x = startX + col * (cardW + gap);
                float y = startY + row * (cardH + gap + 20.0f);

                ImGui::SetCursorPos(ImVec2(x, y));

                const char* heroName;
                ImVec4 heroColor;
                if (useRegistry) {
                    const auto& def = registry.all()[i];
                    heroName = def.name.c_str();
                    heroColor = ImVec4(def.portraitColor.x, def.portraitColor.y,
                                       def.portraitColor.z, def.portraitColor.w);
                } else {
                    heroName = fallbackNames[i];
                    heroColor = fallbackColors[i];
                }

                bool selected = (m_selectedHero == i);
                if (selected) {
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(1.0f, 1.0f, 1.0f, 0.3f));
                    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 3.0f);
                    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1.0f, 0.84f, 0.0f, 1.0f));
                } else {
                    ImGui::PushStyleColor(ImGuiCol_Button, heroColor);
                }

                ImGui::PushID(i);
                if (ImGui::Button("##hero", ImVec2(cardW, cardH))) {
                    m_selectedHero = i;
                }
                ImGui::PopID();

                if (selected) {
                    ImGui::PopStyleColor(2);
                    ImGui::PopStyleVar();
                } else {
                    ImGui::PopStyleColor();
                }

                // Label beneath card
                float labelW = ImGui::CalcTextSize(heroName).x;
                ImGui::SetCursorPos(ImVec2(x + (cardW - labelW) * 0.5f, y + cardH + 4.0f));
                ImGui::Text("%s", heroName);
            }

            // Lock In button
            ImVec2 lockSize(200, 50);
            ImGui::SetCursorPos(ImVec2((w - lockSize.x) * 0.5f, h * 0.75f));
            bool canLock = (m_selectedHero >= 0);
            if (!canLock) ImGui::BeginDisabled();
            if (ImGui::Button("LOCK IN", lockSize)) {
                if (useRegistry && m_selectedHero < static_cast<int>(registry.count())) {
                    const auto& heroId = registry.all()[m_selectedHero].heroId;
                    sm.setSelectedHeroId(heroId);
                    spdlog::info("Hero locked: {} ({})", registry.all()[m_selectedHero].name, heroId);
                } else {
                    spdlog::info("Hero locked: {}", fallbackNames[m_selectedHero]);
                }
                sm.transition(GameStateType::LOADING);
            }
            if (!canLock) ImGui::EndDisabled();

            // Back button
            ImVec2 backSize(120, 36);
            ImGui::SetCursorPos(ImVec2((w - backSize.x) * 0.5f, h * 0.82f));
            if (ImGui::Button("BACK", backSize)) {
                sm.transition(GameStateType::MAIN_MENU);
            }
        }
        ImGui::End();
        ImGui::PopStyleColor();
    });

    sm.renderer().renderFrame(0.0f);
}

void HeroSelectState::exit(GameStateMachine& sm) {
    sm.renderer().setMenuRenderer(nullptr);
}

// ── LoadingState ─────────────────────────────────────────────────────────────

void LoadingState::enter(GameStateMachine& sm) {
    sm.renderer().setMenuMode(true);
    m_sceneBuilt = false;
    m_timer = 0.0f;

    sm.renderer().setSelectedHeroId(sm.selectedHeroId());
    sm.renderer().buildScene();
    m_sceneBuilt = true;
}

void LoadingState::update(GameStateMachine& sm, float dt) {
    m_timer += dt;
    if (m_sceneBuilt) {
        sm.transition(GameStateType::IN_GAME);
    }
}

void LoadingState::render(GameStateMachine& sm, float /*alpha*/) {
    sm.renderer().setMenuRenderer([this] {
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);

        ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration
                               | ImGuiWindowFlags_NoMove
                               | ImGuiWindowFlags_NoResize
                               | ImGuiWindowFlags_NoSavedSettings;

        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.05f, 0.05f, 0.08f, 1.0f));

        if (ImGui::Begin("Loading", nullptr, flags)) {
            float w = ImGui::GetWindowWidth();
            float h = ImGui::GetWindowHeight();

            const char* text = "LOADING...";
            float tw = ImGui::CalcTextSize(text).x;
            ImGui::SetCursorPos(ImVec2((w - tw) * 0.5f, h * 0.45f));
            ImGui::Text("%s", text);

            // Simple progress bar
            float barW = 300.0f;
            float progress = m_sceneBuilt ? 1.0f : std::min(m_timer * 2.0f, 0.9f);
            ImGui::SetCursorPos(ImVec2((w - barW) * 0.5f, h * 0.52f));
            ImGui::ProgressBar(progress, ImVec2(barW, 20.0f), "");
        }
        ImGui::End();
        ImGui::PopStyleColor();
    });

    sm.renderer().renderFrame(0.0f);
}

void LoadingState::exit(GameStateMachine& sm) {
    sm.renderer().setMenuRenderer(nullptr);
}

// ── InGameState ──────────────────────────────────────────────────────────────

void InGameState::enter(GameStateMachine& sm) {
    sm.renderer().setMenuMode(false);
    spdlog::info("Entering gameplay");
}

void InGameState::update(GameStateMachine& sm, float dt) {
    sm.renderer().simulateStep(dt);
}

void InGameState::render(GameStateMachine& sm, float alpha) {
    sm.renderer().renderFrame(alpha);
}

void InGameState::exit(GameStateMachine& /*sm*/) {}

// ── PostGameState ────────────────────────────────────────────────────────────

void PostGameState::enter(GameStateMachine& sm) {
    sm.renderer().setMenuMode(true);
}

void PostGameState::update(GameStateMachine& /*sm*/, float /*dt*/) {}

void PostGameState::render(GameStateMachine& sm, float /*alpha*/) {
    sm.renderer().setMenuRenderer([&sm] {
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);

        ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration
                               | ImGuiWindowFlags_NoMove
                               | ImGuiWindowFlags_NoResize
                               | ImGuiWindowFlags_NoSavedSettings;

        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.1f, 0.1f, 0.12f, 1.0f));

        if (ImGui::Begin("PostGame", nullptr, flags)) {
            float w = ImGui::GetWindowWidth();
            float h = ImGui::GetWindowHeight();

            const char* result = sm.isVictory() ? "VICTORY" : "DEFEAT";
            float rw = ImGui::CalcTextSize(result).x;
            ImGui::SetCursorPos(ImVec2((w - rw) * 0.5f, h * 0.35f));
            ImGui::Text("%s", result);

            ImVec2 btnSize(240, 50);
            ImGui::SetCursorPos(ImVec2((w - btnSize.x) * 0.5f, h * 0.55f));
            if (ImGui::Button("RETURN TO MENU", btnSize)) {
                sm.transition(GameStateType::MAIN_MENU);
            }
        }
        ImGui::End();
        ImGui::PopStyleColor();
    });

    sm.renderer().renderFrame(0.0f);
}

void PostGameState::exit(GameStateMachine& sm) {
    sm.renderer().setMenuRenderer(nullptr);
}

} // namespace glory
