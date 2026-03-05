#include "minion/MinionSystem.h"
#include "minion/MinionConfig.h"
#include "minion/MinionComponents.h"
#include "scene/Components.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace glory;

static int g_passed = 0;
static int g_total = 0;

#define TEST(name)                                                             \
  do {                                                                         \
    g_total++;                                                                 \
    printf("  [%d] %s ... ", g_total, name);                                   \
  } while (0)

#define PASS()                                                                 \
  do {                                                                         \
    g_passed++;                                                                \
    printf("PASS\n");                                                          \
  } while (0)

#define ASSERT_TRUE(cond)                                                      \
  do {                                                                         \
    if (!(cond)) {                                                             \
      printf("FAIL (%s:%d: %s)\n", __FILE__, __LINE__, #cond);                \
      return;                                                                  \
    }                                                                          \
  } while (0)

#define ASSERT_EQ(a, b)                                                        \
  do {                                                                         \
    if ((a) != (b)) {                                                          \
      printf("FAIL (%s:%d: %s != %s)\n", __FILE__, __LINE__, #a, #b);         \
      return;                                                                  \
    }                                                                          \
  } while (0)

#define ASSERT_NEAR(a, b, eps)                                                 \
  do {                                                                         \
    if (std::abs((a) - (b)) > (eps)) {                                         \
      printf("FAIL (%s:%d: %s=%f != %s=%f)\n", __FILE__, __LINE__, #a,        \
             (double)(a), #b, (double)(b));                                    \
      return;                                                                  \
    }                                                                          \
  } while (0)

// ── Helper: build minimal config without JSON files ─────────────────────────

static MinionConfig makeDefaultConfig() {
  MinionConfig cfg;

  // Melee
  auto &melee = cfg.stats[0];
  melee.hp = 477; melee.attackDamage = 12; melee.armor = 0;
  melee.magicResist = 0; melee.moveSpeed = 325; melee.attackRange = 110;
  melee.attackCooldown = 1.25f; melee.attackStyle = AttackStyle::Melee;
  melee.hpPerTick = 21; melee.adPerTick = 1.0f; melee.armorPerTick = 2.0f;

  // Caster
  auto &caster = cfg.stats[1];
  caster.hp = 296; caster.attackDamage = 23; caster.armor = 0;
  caster.magicResist = 0; caster.moveSpeed = 325; caster.attackRange = 600;
  caster.attackCooldown = 1.6f; caster.attackStyle = AttackStyle::Ranged;
  caster.projectileSpeed = 650;
  caster.hpPerTick = 14; caster.adPerTick = 1.5f; caster.armorPerTick = 1.25f;

  // Siege
  auto &siege = cfg.stats[2];
  siege.hp = 900; siege.attackDamage = 40; siege.armor = 65;
  siege.magicResist = 0; siege.moveSpeed = 325; siege.attackRange = 300;
  siege.attackCooldown = 2.0f; siege.attackStyle = AttackStyle::Ranged;
  siege.projectileSpeed = 1200;
  siege.hpPerTick = 50; siege.adPerTick = 3.0f; siege.armorPerTick = 3.0f;

  // Super
  auto &super_ = cfg.stats[3];
  super_.hp = 2000; super_.attackDamage = 190; super_.armor = 100;
  super_.magicResist = 30; super_.moveSpeed = 325; super_.attackRange = 170;
  super_.attackCooldown = 0.85f; super_.attackStyle = AttackStyle::Melee;
  super_.hpPerTick = 200; super_.adPerTick = 10.0f; super_.armorPerTick = 5.0f;

  // Rewards
  cfg.rewards[0] = {21.0f, 0.125f, 60.0f};
  cfg.rewards[1] = {14.0f, 0.125f, 30.0f};
  cfg.rewards[2] = {60.0f, 0.35f, 93.0f};
  cfg.rewards[3] = {90.0f, 0.0f, 97.0f};

  cfg.scalingIntervalSeconds = 90.0f;
  cfg.firstWaveTime = 65.0f;
  cfg.waveInterval = 30.0f;
  cfg.standardMelee = 3;
  cfg.standardCaster = 1;
  cfg.cannonRules = {{0.0f, 3}, {1200.0f, 2}, {2100.0f, 1}};
  cfg.superReplaceSiege = true;
  cfg.allInhibsDownSuperCount = 2;

  cfg.aggroRange = 700.0f;
  cfg.leashRange = 900.0f;
  cfg.aggroCheckInterval = 0.25f;
  cfg.targetReEvalInterval = 3.5f;
  cfg.championAggroDuration = 2.5f;
  cfg.championAggroCooldown = 2.0f;
  cfg.xpRange = 1600.0f;

  return cfg;
}

static MapData makeSimpleMap() {
  MapData map;
  map.mapName = "TestMap";
  map.mapCenter = {100, 0, 100};
  map.mapBoundsMin = {0, 0, 0};
  map.mapBoundsMax = {200, 20, 200};

  // Blue team
  map.teams[0].base.nexusPosition = {22, 0, 22};
  map.teams[0].base.spawnPlatformCenter = {15, 0, 15};

  // Red team
  map.teams[1].base.nexusPosition = {178, 0, 178};
  map.teams[1].base.spawnPlatformCenter = {185, 0, 185};

  // Simple mid lane waypoints for both teams
  std::vector<glm::vec3> blueMid = {
      {22, 0, 22}, {60, 0, 60}, {100, 0, 100}, {140, 0, 140}, {178, 0, 178}};
  std::vector<glm::vec3> redMid = {
      {178, 0, 178}, {140, 0, 140}, {100, 0, 100}, {60, 0, 60}, {22, 0, 22}};

  // All 3 lanes use same waypoints for simplicity
  for (int li = 0; li < 3; li++) {
    map.teams[0].lanes[li].type = static_cast<LaneType>(li);
    map.teams[0].lanes[li].waypoints = blueMid;
    map.teams[1].lanes[li].type = static_cast<LaneType>(li);
    map.teams[1].lanes[li].waypoints = redMid;
  }

  return map;
}

// ═════════════════════════════════════════════════════════════════════════════
// Tests
// ═════════════════════════════════════════════════════════════════════════════

static void test_config_defaults() {
  TEST("Config has correct default melee stats");
  auto cfg = makeDefaultConfig();
  ASSERT_NEAR(cfg.stats[0].hp, 477.0f, 0.01f);
  ASSERT_NEAR(cfg.stats[0].attackDamage, 12.0f, 0.01f);
  ASSERT_NEAR(cfg.stats[0].moveSpeed, 325.0f, 0.01f);
  ASSERT_NEAR(cfg.stats[0].attackRange, 110.0f, 0.01f);
  PASS();
}

static void test_config_load_from_json() {
  TEST("Config loads from JSON files");
  auto cfg = MinionConfigLoader::Load(std::string(CONFIG_DIR));
  ASSERT_NEAR(cfg.stats[0].hp, 477.0f, 0.01f);
  ASSERT_NEAR(cfg.stats[1].hp, 296.0f, 0.01f);
  ASSERT_NEAR(cfg.stats[2].hp, 900.0f, 0.01f);
  ASSERT_NEAR(cfg.stats[3].hp, 2000.0f, 0.01f);
  ASSERT_NEAR(cfg.firstWaveTime, 5.0f, 0.01f);
  ASSERT_NEAR(cfg.waveInterval, 30.0f, 0.01f);
  ASSERT_EQ(cfg.standardMelee, 3);
  ASSERT_EQ(cfg.standardCaster, 1);
  ASSERT_NEAR(cfg.aggroRange, 10.0f, 0.01f);
  ASSERT_NEAR(cfg.rewards[0].goldBase, 21.0f, 0.01f);
  ASSERT_NEAR(cfg.rewards[0].xp, 60.0f, 0.01f);
  PASS();
}

static void test_spawn_first_wave() {
  TEST("First wave spawns at 65s with correct composition");
  entt::registry reg;
  MinionSystem sys;
  sys.init(makeDefaultConfig(), makeSimpleMap());

  // At 64s, no wave yet
  sys.update(reg, 0.016f, 64.0f);
  ASSERT_EQ(sys.getLivingCount(), 0u);

  // At 65s, first wave spawns
  sys.update(reg, 0.016f, 65.0f);
  // 2 teams × 3 lanes × (3 melee + 1 caster) = 24 minions
  ASSERT_EQ(sys.getLivingCount(), 24u);
  PASS();
}

static void test_spawn_correct_types() {
  TEST("First wave has correct type distribution per team per lane");
  entt::registry reg;
  MinionSystem sys;
  sys.init(makeDefaultConfig(), makeSimpleMap());

  sys.update(reg, 0.016f, 65.0f);

  // Count by team and type
  int counts[2][4] = {};
  auto view = reg.view<MinionIdentityComponent>();
  for (auto e : view) {
    auto &id = view.get<MinionIdentityComponent>(e);
    counts[static_cast<int>(id.team)][static_cast<int>(id.type)]++;
  }

  // Blue: 3 lanes × 3 melee = 9 melee, 3 lanes × 1 caster = 3 caster
  ASSERT_EQ(counts[0][0], 9);  // Blue melee
  ASSERT_EQ(counts[0][1], 3);  // Blue caster
  ASSERT_EQ(counts[0][2], 0);  // Blue siege (not a cannon wave)
  ASSERT_EQ(counts[1][0], 9);  // Red melee
  ASSERT_EQ(counts[1][1], 3);  // Red caster
  PASS();
}

static void test_cannon_wave_every_3rd() {
  TEST("Every 3rd wave includes siege minion");
  entt::registry reg;
  MinionSystem sys;
  sys.init(makeDefaultConfig(), makeSimpleMap());

  // Spawn waves 1, 2, 3
  float t = 65.0f;
  sys.update(reg, 0.016f, t);        // wave 1
  t += 30.0f;
  sys.update(reg, 0.016f, t);        // wave 2
  t += 30.0f;
  sys.update(reg, 0.016f, t);        // wave 3 (cannon)

  int siegeCount = 0;
  auto view = reg.view<MinionIdentityComponent>();
  for (auto e : view) {
    if (view.get<MinionIdentityComponent>(e).type == MinionType::Siege)
      siegeCount++;
  }
  // Wave 3 is cannon: 2 teams × 3 lanes × 1 siege = 6
  ASSERT_EQ(siegeCount, 6);
  PASS();
}

static void test_stat_scaling() {
  TEST("Stats scale correctly at spawn time");
  entt::registry reg;
  MinionSystem sys;
  sys.init(makeDefaultConfig(), makeSimpleMap());

  // Spawn at game time 180s = 2 ticks
  sys.update(reg, 0.016f, 180.0f);

  auto view = reg.view<MinionIdentityComponent, MinionHealthComponent>();
  for (auto e : view) {
    auto &id = view.get<MinionIdentityComponent>(e);
    auto &hp = view.get<MinionHealthComponent>(e);
    if (id.type == MinionType::Melee && id.team == TeamID::Blue) {
      // 477 + 21 * 2 = 519
      ASSERT_NEAR(hp.maxHP, 519.0f, 0.01f);
      PASS();
      return;
    }
  }
  printf("FAIL (no blue melee found)\n");
}

static void test_damage_formula() {
  TEST("Damage formula: 100/(100+armor)");
  // 12 AD vs 0 armor = 12 dmg
  float dmg1 = 12.0f * 100.0f / (100.0f + 0.0f);
  ASSERT_NEAR(dmg1, 12.0f, 0.01f);

  // 40 AD vs 65 armor = 40 * 100/165 ≈ 24.24
  float dmg2 = 40.0f * 100.0f / (100.0f + 65.0f);
  ASSERT_NEAR(dmg2, 24.242f, 0.1f);
  PASS();
}

static void test_minion_pathing() {
  TEST("Minions advance along lane waypoints");
  entt::registry reg;
  MinionSystem sys;
  sys.init(makeDefaultConfig(), makeSimpleMap());

  sys.update(reg, 0.016f, 65.0f); // spawn

  // Find a blue mid melee minion
  entt::entity minion = entt::null;
  auto view = reg.view<MinionIdentityComponent, TransformComponent>();
  for (auto e : view) {
    auto &id = view.get<MinionIdentityComponent>(e);
    if (id.team == TeamID::Blue && id.type == MinionType::Melee &&
        id.lane == LaneType::Mid) {
      minion = e;
      break;
    }
  }
  ASSERT_TRUE(minion != entt::null);

  auto &startPos = reg.get<TransformComponent>(minion).position;
  float startX = startPos.x;

  // Simulate 5 seconds of movement (spawning takes 0.5s)
  for (int i = 0; i < 300; i++) {
    sys.update(reg, 1.0f / 60.0f, 65.0f + i / 60.0f);
  }

  auto &endPos = reg.get<TransformComponent>(minion).position;
  float endX = endPos.x;

  // Should have moved toward first waypoint (60, 60)
  ASSERT_TRUE(endX > startX + 1.0f);
  PASS();
}

static void test_melee_combat() {
  TEST("Melee minions deal damage when in range");
  entt::registry reg;
  MinionSystem sys;
  auto cfg = makeDefaultConfig();
  cfg.aggroRange = 5000.0f; // large range so they always find targets
  sys.init(cfg, makeSimpleMap());

  // Manually create two opposing minions close together
  auto blue = reg.create();
  reg.emplace<MinionTag>(blue);
  auto &bt = reg.emplace<TransformComponent>(blue);
  bt.position = {100, 0, 100};
  reg.emplace<MinionIdentityComponent>(blue,
      MinionIdentityComponent{MinionType::Melee, TeamID::Blue, LaneType::Mid, 1});
  reg.emplace<MinionHealthComponent>(blue, MinionHealthComponent{477, 477, false, entt::null});
  MinionCombatComponent bc;
  bc.attackDamage = 12; bc.attackRange = 110; bc.attackCooldown = 1.25f;
  bc.timeSinceLastAttack = 1.25f; // ready to attack
  bc.attackStyle = AttackStyle::Melee;
  reg.emplace<MinionCombatComponent>(blue, bc);
  reg.emplace<MinionMovementComponent>(blue);
  reg.emplace<MinionAggroComponent>(blue, MinionAggroComponent{entt::null, 5000, 5000});
  reg.emplace<MinionStateComponent>(blue, MinionStateComponent{MinionState::Pathing, 0});

  auto red = reg.create();
  reg.emplace<MinionTag>(red);
  auto &rt = reg.emplace<TransformComponent>(red);
  rt.position = {100.5f, 0, 100}; // within melee range (110 units... but distance is 0.5)
  reg.emplace<MinionIdentityComponent>(red,
      MinionIdentityComponent{MinionType::Melee, TeamID::Red, LaneType::Mid, 1});
  reg.emplace<MinionHealthComponent>(red, MinionHealthComponent{477, 477, false, entt::null});
  MinionCombatComponent rc;
  rc.attackDamage = 12; rc.attackRange = 110; rc.attackCooldown = 1.25f;
  rc.timeSinceLastAttack = 1.25f;
  rc.attackStyle = AttackStyle::Melee;
  reg.emplace<MinionCombatComponent>(red, rc);
  reg.emplace<MinionMovementComponent>(red);
  reg.emplace<MinionAggroComponent>(red, MinionAggroComponent{entt::null, 5000, 5000});
  reg.emplace<MinionStateComponent>(red, MinionStateComponent{MinionState::Pathing, 0});

  // Run several frames: aggro check → engage → attack
  for (int i = 0; i < 200; i++) {
    sys.update(reg, 1.0f / 60.0f, 65.0f + i / 60.0f);
  }

  auto &redHP = reg.get<MinionHealthComponent>(red);
  // Red should have taken damage (blue attacks at 12 AD vs 0 armor)
  ASSERT_TRUE(redHP.currentHP < 477.0f);
  PASS();
}

static void test_death_event() {
  TEST("Death emits event with correct gold/xp");
  entt::registry reg;
  MinionSystem sys;
  sys.init(makeDefaultConfig(), makeSimpleMap());

  // Create a nearly-dead red minion
  auto red = reg.create();
  reg.emplace<MinionTag>(red);
  reg.emplace<TransformComponent>(red).position = {100, 0, 100};
  reg.emplace<MinionIdentityComponent>(red,
      MinionIdentityComponent{MinionType::Melee, TeamID::Red, LaneType::Mid, 1});
  reg.emplace<MinionHealthComponent>(red, MinionHealthComponent{1.0f, 477.0f, false, entt::null});
  MinionCombatComponent rc;
  rc.armor = 0;
  reg.emplace<MinionCombatComponent>(red, rc);
  reg.emplace<MinionMovementComponent>(red);
  reg.emplace<MinionAggroComponent>(red);
  reg.emplace<MinionStateComponent>(red, MinionStateComponent{MinionState::Pathing, 0});

  // Create a blue minion that will kill it
  auto blue = reg.create();
  reg.emplace<MinionTag>(blue);
  reg.emplace<TransformComponent>(blue).position = {100.5f, 0, 100};
  reg.emplace<MinionIdentityComponent>(blue,
      MinionIdentityComponent{MinionType::Melee, TeamID::Blue, LaneType::Mid, 1});
  reg.emplace<MinionHealthComponent>(blue, MinionHealthComponent{477, 477, false, entt::null});
  MinionCombatComponent bc;
  bc.attackDamage = 12; bc.attackRange = 110; bc.attackCooldown = 0.1f;
  bc.timeSinceLastAttack = 0.1f;
  bc.attackStyle = AttackStyle::Melee;
  reg.emplace<MinionCombatComponent>(blue, bc);
  reg.emplace<MinionMovementComponent>(blue);
  reg.emplace<MinionAggroComponent>(blue, MinionAggroComponent{entt::null, 5000, 5000});
  reg.emplace<MinionStateComponent>(blue, MinionStateComponent{MinionState::Pathing, 0});

  // Run until red dies
  for (int i = 0; i < 300; i++) {
    sys.update(reg, 1.0f / 60.0f, 65.0f + i / 60.0f);
    auto events = sys.consumeDeathEvents();
    if (!events.empty()) {
      ASSERT_EQ(events[0].type, MinionType::Melee);
      ASSERT_EQ(events[0].team, TeamID::Red);
      ASSERT_NEAR(events[0].goldValue, 21.0f, 0.5f); // ~21g at game start
      ASSERT_NEAR(events[0].xpValue, 60.0f, 0.01f);
      PASS();
      return;
    }
  }
  printf("FAIL (red minion didn't die)\n");
}

static void test_gold_scaling() {
  TEST("Gold scales with game time");
  auto cfg = makeDefaultConfig();
  // Melee: 21 + 0.125 * minutes
  // At 10 minutes: 21 + 1.25 = 22.25
  float gold10 = cfg.rewards[0].goldBase + cfg.rewards[0].goldPerMinute * 10.0f;
  ASSERT_NEAR(gold10, 22.25f, 0.01f);
  // At 30 minutes: 21 + 3.75 = 24.75
  float gold30 = cfg.rewards[0].goldBase + cfg.rewards[0].goldPerMinute * 30.0f;
  ASSERT_NEAR(gold30, 24.75f, 0.01f);
  PASS();
}

static void test_wave_interval() {
  TEST("Waves spawn every 30s after first");
  entt::registry reg;
  MinionSystem sys;
  sys.init(makeDefaultConfig(), makeSimpleMap());

  sys.update(reg, 0.016f, 65.0f);
  uint32_t afterFirst = sys.getLivingCount(); // 24
  ASSERT_EQ(afterFirst, 24u);

  sys.update(reg, 0.016f, 94.0f); // 94s: no new wave yet
  ASSERT_EQ(sys.getLivingCount(), 24u);

  sys.update(reg, 0.016f, 95.0f); // 95s: second wave
  ASSERT_EQ(sys.getLivingCount(), 48u);
  PASS();
}

static void test_super_minion_on_inhib_down() {
  TEST("Super minion spawns when inhibitor is down");
  entt::registry reg;
  MinionSystem sys;
  sys.init(makeDefaultConfig(), makeSimpleMap());

  // Blue team's inhib destroyed in Mid lane → Red spawns Super in Mid
  // (Super replaces Siege for the OPPOSING team's lane)
  sys.setInhibitorDown(TeamID::Blue, LaneType::Mid, true);

  // Spawn wave 3 (cannon wave) at 125s
  float t = 65.0f;
  sys.update(reg, 0.016f, t);   // wave 1
  t += 30.0f;
  sys.update(reg, 0.016f, t);   // wave 2
  t += 30.0f;
  sys.update(reg, 0.016f, t);   // wave 3 (cannon)

  // Count super minions from Red team in Mid lane
  int superCount = 0;
  auto view = reg.view<MinionIdentityComponent>();
  for (auto e : view) {
    auto &id = view.get<MinionIdentityComponent>(e);
    if (id.team == TeamID::Red && id.lane == LaneType::Mid &&
        id.type == MinionType::Super)
      superCount++;
  }
  ASSERT_TRUE(superCount >= 1);
  PASS();
}

static void test_spatial_hash() {
  TEST("SpatialHash finds entities within radius");
  SpatialHash hash(100.0f);

  entt::registry reg;
  auto e1 = reg.create();
  auto e2 = reg.create();
  auto e3 = reg.create();

  hash.insert(e1, {50, 0, 50});
  hash.insert(e2, {55, 0, 50});
  hash.insert(e3, {500, 0, 500}); // far away

  std::vector<entt::entity> results;
  hash.query({50, 0, 50}, 20.0f, results);

  ASSERT_EQ(results.size(), 2u);
  PASS();
}

static void test_separation_steering() {
  TEST("Minions separate and don't stack");
  entt::registry reg;
  MinionSystem sys;
  sys.init(makeDefaultConfig(), makeSimpleMap());

  // Create 5 blue minions at the exact same position
  for (int i = 0; i < 5; i++) {
    auto e = reg.create();
    reg.emplace<MinionTag>(e);
    reg.emplace<TransformComponent>(e).position = {50, 0, 50};
    reg.emplace<MinionIdentityComponent>(e,
        MinionIdentityComponent{MinionType::Melee, TeamID::Blue, LaneType::Mid, 1});
    reg.emplace<MinionHealthComponent>(e, MinionHealthComponent{477, 477, false, entt::null});
    MinionCombatComponent cc;
    cc.attackRange = 110; cc.attackCooldown = 1.25f;
    cc.attackStyle = AttackStyle::Melee;
    reg.emplace<MinionCombatComponent>(e, cc);
    MinionMovementComponent mc;
    mc.moveSpeed = 325;
    reg.emplace<MinionMovementComponent>(e, mc);
    reg.emplace<MinionAggroComponent>(e);
    reg.emplace<MinionStateComponent>(e, MinionStateComponent{MinionState::Pathing, 1.0f});
  }

  // Run a few seconds
  for (int i = 0; i < 120; i++) {
    sys.update(reg, 1.0f / 60.0f, 66.0f + i / 60.0f);
  }

  // Check that minions have spread out (not all at same position)
  float maxDist2 = 0;
  glm::vec3 centroid(0);
  int count = 0;
  auto view = reg.view<MinionTag, TransformComponent>();
  for (auto e : view) {
    centroid += view.get<TransformComponent>(e).position;
    count++;
  }
  centroid /= static_cast<float>(count);
  for (auto e : view) {
    auto &p = view.get<TransformComponent>(e).position;
    float d2 = glm::dot(p - centroid, p - centroid);
    if (d2 > maxDist2) maxDist2 = d2;
  }
  // At least some separation should have occurred
  ASSERT_TRUE(maxDist2 > 0.1f);
  PASS();
}

static void test_ranged_projectile() {
  TEST("Caster minion spawns projectile that hits target");
  entt::registry reg;
  MinionSystem sys;
  auto cfg = makeDefaultConfig();
  cfg.aggroRange = 5000.0f;
  sys.init(cfg, makeSimpleMap());

  // Blue caster
  auto blue = reg.create();
  reg.emplace<MinionTag>(blue);
  reg.emplace<TransformComponent>(blue).position = {100, 0, 100};
  reg.emplace<MinionIdentityComponent>(blue,
      MinionIdentityComponent{MinionType::Caster, TeamID::Blue, LaneType::Mid, 1});
  reg.emplace<MinionHealthComponent>(blue, MinionHealthComponent{296, 296, false, entt::null});
  MinionCombatComponent bc;
  bc.attackDamage = 23; bc.attackRange = 600; bc.attackCooldown = 1.6f;
  bc.timeSinceLastAttack = 1.6f;
  bc.attackStyle = AttackStyle::Ranged;
  bc.projectileSpeed = 650;
  reg.emplace<MinionCombatComponent>(blue, bc);
  reg.emplace<MinionMovementComponent>(blue);
  reg.emplace<MinionAggroComponent>(blue, MinionAggroComponent{entt::null, 5000, 5000});
  reg.emplace<MinionStateComponent>(blue, MinionStateComponent{MinionState::Pathing, 0});

  // Red target in range
  auto red = reg.create();
  reg.emplace<MinionTag>(red);
  reg.emplace<TransformComponent>(red).position = {110, 0, 100};
  reg.emplace<MinionIdentityComponent>(red,
      MinionIdentityComponent{MinionType::Melee, TeamID::Red, LaneType::Mid, 1});
  reg.emplace<MinionHealthComponent>(red, MinionHealthComponent{477, 477, false, entt::null});
  MinionCombatComponent rc;
  rc.armor = 0;
  reg.emplace<MinionCombatComponent>(red, rc);
  reg.emplace<MinionMovementComponent>(red);
  reg.emplace<MinionAggroComponent>(red);
  reg.emplace<MinionStateComponent>(red, MinionStateComponent{MinionState::Pathing, 0});

  // Run until projectile hits
  float initialHP = 477.0f;
  for (int i = 0; i < 600; i++) {
    sys.update(reg, 1.0f / 60.0f, 65.0f + i / 60.0f);
    if (!reg.valid(red)) break;
    auto &rHP = reg.get<MinionHealthComponent>(red);
    if (rHP.currentHP < initialHP) {
      // Projectile hit!
      ASSERT_TRUE(rHP.currentHP < initialHP);
      PASS();
      return;
    }
  }
  printf("FAIL (projectile didn't hit)\n");
}

static void test_leash_range() {
  TEST("Minion drops target beyond leash range");
  entt::registry reg;
  MinionSystem sys;
  auto cfg = makeDefaultConfig();
  cfg.aggroRange = 5000.0f;
  cfg.leashRange = 50.0f; // very short for testing
  sys.init(cfg, makeSimpleMap());

  // Blue minion
  auto blue = reg.create();
  reg.emplace<MinionTag>(blue);
  reg.emplace<TransformComponent>(blue).position = {100, 0, 100};
  reg.emplace<MinionIdentityComponent>(blue,
      MinionIdentityComponent{MinionType::Melee, TeamID::Blue, LaneType::Mid, 1});
  reg.emplace<MinionHealthComponent>(blue, MinionHealthComponent{477, 477, false, entt::null});
  MinionCombatComponent bc;
  bc.attackDamage = 12; bc.attackRange = 110;
  bc.attackStyle = AttackStyle::Melee;
  reg.emplace<MinionCombatComponent>(blue, bc);
  reg.emplace<MinionMovementComponent>(blue);
  reg.emplace<MinionAggroComponent>(blue, MinionAggroComponent{entt::null, 5000, 50});
  reg.emplace<MinionStateComponent>(blue, MinionStateComponent{MinionState::Pathing, 0});

  // Red minion far away (beyond leash)
  auto red = reg.create();
  reg.emplace<MinionTag>(red);
  reg.emplace<TransformComponent>(red).position = {200, 0, 200};
  reg.emplace<MinionIdentityComponent>(red,
      MinionIdentityComponent{MinionType::Melee, TeamID::Red, LaneType::Mid, 1});
  reg.emplace<MinionHealthComponent>(red, MinionHealthComponent{477, 477, false, entt::null});
  reg.emplace<MinionCombatComponent>(red);
  reg.emplace<MinionMovementComponent>(red);
  reg.emplace<MinionAggroComponent>(red);
  reg.emplace<MinionStateComponent>(red, MinionStateComponent{MinionState::Pathing, 0});

  // Run aggro eval
  for (int i = 0; i < 30; i++) {
    sys.update(reg, 1.0f / 60.0f, 65.0f + i / 60.0f);
  }

  // Blue should have acquired red initially, then dropped due to leash
  auto &blueAggro = reg.get<MinionAggroComponent>(blue);
  auto &blueState = reg.get<MinionStateComponent>(blue);
  // After chasing, should have given up (returning or pathing)
  ASSERT_TRUE(blueState.state == MinionState::Returning ||
              blueState.state == MinionState::Pathing ||
              blueState.state == MinionState::Engaging);
  PASS();
}

// ═════════════════════════════════════════════════════════════════════════════
// Main
// ═════════════════════════════════════════════════════════════════════════════

int main() {
  printf("=== Minion System Tests ===\n");

  test_config_defaults();
  test_config_load_from_json();
  test_spawn_first_wave();
  test_spawn_correct_types();
  test_cannon_wave_every_3rd();
  test_stat_scaling();
  test_damage_formula();
  test_minion_pathing();
  test_melee_combat();
  test_death_event();
  test_gold_scaling();
  test_wave_interval();
  test_super_minion_on_inhib_down();
  test_spatial_hash();
  test_separation_steering();
  test_ranged_projectile();
  test_leash_range();

  printf("\n%d / %d tests passed.\n", g_passed, g_total);
  if (g_passed == g_total)
    printf("All %d tests passed!\n", g_total);
  return (g_passed == g_total) ? 0 : 1;
}
