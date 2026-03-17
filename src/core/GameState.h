#pragma once

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

namespace glory {

class Renderer;
class Window;
class NetworkGameLoop;

enum class GameStateType {
    MAIN_MENU,
    HERO_SELECT,
    LOADING,
    IN_GAME,
    POST_GAME
};

class GameStateMachine;

class IGameState {
public:
    virtual ~IGameState() = default;
    virtual void enter(GameStateMachine& sm) = 0;
    virtual void update(GameStateMachine& sm, float dt) = 0;
    virtual void render(GameStateMachine& sm, float alpha) = 0;
    virtual void exit(GameStateMachine& sm) = 0;
};

class GameStateMachine {
public:
    GameStateMachine(Renderer& renderer, Window& window);

    void transition(GameStateType state);
    void update(float dt);
    void render(float alpha);

    Renderer& renderer() { return m_renderer; }
    Window&   window()   { return m_window; }

    GameStateType currentState() const { return m_currentType; }
    bool shouldQuit() const { return m_quit; }
    void requestQuit() { m_quit = true; }

    void setVictory(bool v) { m_victory = v; }
    bool isVictory() const  { return m_victory; }

    void setSelectedHeroId(const std::string& id) { m_selectedHeroId = id; }
    const std::string& selectedHeroId() const { return m_selectedHeroId; }

    // Network integration
    void setNetworkGameLoop(NetworkGameLoop* netLoop) { m_netLoop = netLoop; }
    NetworkGameLoop* netLoop() { return m_netLoop; }

private:
    void doTransition(GameStateType newState);

    Renderer& m_renderer;
    Window&   m_window;
    NetworkGameLoop* m_netLoop = nullptr;
    std::unordered_map<GameStateType, std::unique_ptr<IGameState>> m_states;
    IGameState*    m_current     = nullptr;
    GameStateType  m_currentType = GameStateType::MAIN_MENU;
    GameStateType  m_pendingType = GameStateType::MAIN_MENU;
    bool           m_hasPending  = false;
    bool           m_quit        = false;
    bool           m_victory     = false;
    std::string    m_selectedHeroId;
};

// ── Concrete states ──────────────────────────────────────────────────────────

class MainMenuState : public IGameState {
public:
    void enter(GameStateMachine& sm) override;
    void update(GameStateMachine& sm, float dt) override;
    void render(GameStateMachine& sm, float alpha) override;
    void exit(GameStateMachine& sm) override;
};

class HeroSelectState : public IGameState {
public:
    void enter(GameStateMachine& sm) override;
    void update(GameStateMachine& sm, float dt) override;
    void render(GameStateMachine& sm, float alpha) override;
    void exit(GameStateMachine& sm) override;
private:
    int m_selectedHero = -1;
    bool m_locked = false;             // local hero locked in
    bool m_waitingForServer = false;   // waiting for server confirmation
    static constexpr int HERO_COUNT = 8;
};

class LoadingState : public IGameState {
public:
    void enter(GameStateMachine& sm) override;
    void update(GameStateMachine& sm, float dt) override;
    void render(GameStateMachine& sm, float alpha) override;
    void exit(GameStateMachine& sm) override;
private:
    bool m_sceneBuilt = false;
    bool m_notifiedServer = false;  // told server we finished loading
    float m_timer = 0.0f;
};

class InGameState : public IGameState {
public:
    void enter(GameStateMachine& sm) override;
    void update(GameStateMachine& sm, float dt) override;
    void render(GameStateMachine& sm, float alpha) override;
    void exit(GameStateMachine& sm) override;
};

class PostGameState : public IGameState {
public:
    void enter(GameStateMachine& sm) override;
    void update(GameStateMachine& sm, float dt) override;
    void render(GameStateMachine& sm, float alpha) override;
    void exit(GameStateMachine& sm) override;
};

} // namespace glory
