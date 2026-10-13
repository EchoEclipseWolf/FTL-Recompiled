#pragma once
#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The original FTL 1.6 continue.sav (save version 11) as written by
// WorldManager::SaveGame 0x5f5410 and read by WorldManager::LoadGame. One
// walker (original_save.cpp) both writes and reads it in the exact original
// order, so the native export and a later native import share one format.
// Field names follow the original members; see
// docs/research/original-save-format/ for addresses, transforms and loaders.
namespace ftl::original_save {
inline constexpr int saveVersion = 11;

// FileHelper::writeFloat 0x58ffd0 stores cvttss2si(value * 1000.0f). The raw
// integer is kept so a parsed file serializes back byte for byte.
struct Milli {
    std::int32_t raw = 0;

    static Milli from(float value);
    float value() const;
    friend bool operator==(const Milli&, const Milli&) = default;
};

// Animation::SaveState 0x59b730.
struct Animation {
    int running = 0;
    int loop = 0;
    int currentFrame = 0;
    Milli progress;
    Milli scale{1000};
    Milli x;
    Milli y;
};

// CrewMember::SaveState 0x46d830, written for every crew member except drones
// (CrewDrone::SaveState 0x73dfb0 writes nothing).
struct CrewState {
    Milli stunTime;
    int healthBoost = 0;
    int deathNumber = -1;
    Milli mindDamageBoost{1000};
    int cloneDying = 0;
    // CrewMember::iDeathCount is a static counter shared by every crew member.
    int deathCount = 0;
    // skillsEarned[n] bit 0 (half the cap) and bit 1 (the cap), per skill.
    std::array<std::array<int, 2>, 6> skillsEarned{};
    // CrewAnimation status 6 and its anims[0][6] strip: the teleport strip.
    int teleporting = 0;
    Animation teleportAnimation{1, 0, 0, {}, {1000}, {}, {}};
    int teleportReverse = 0;
};

// CrystalAlien::SaveState 0x46d960 appends these after CrewState.
struct CrystalState {
    Milli powerCooldownFirst;
    Milli powerCooldownSecond;
    int cloned = 0;
};

// One CrewMember record of ShipManager::ExportShip 0x4a4680.
struct Crew {
    std::string name;
    std::string species;
    int drone = 0;
    int health = 0;
    int x = 0;
    int y = 0;
    int room = 0;
    int slot = 0;
    int playerControlled = 0;
    int cloneReady = 0;
    int deathNumber = -1;
    std::vector<int> colors;
    int mindControlled = 0;
    // GetSavedPosition is written slot first, then room (0x4a4cb6).
    int savedSlot = -1;
    int savedRoom = -1;
    // blueprint.skillLevel[n].first: pilot, engines, shields, weapons, repair, combat.
    std::array<int, 6> skills{};
    int male = 0;
    // SCrewStats: repairs, kills, evasions, jumps survived, skill masteries.
    std::array<int, 5> stats{};
    CrewState state;
    CrystalState crystal;
};

// ExportShip's per-system record; ShipSystem::SaveState 0x6152f0 adds the last three.
struct SystemRecord {
    int capacity = 0;
    int power = 0;
    int damage = 0;
    int lockCount = 0;
    int lockTimerX5000 = 0;
    int repairOverTime = 0;
    int damageOverTime = 0;
    int batteryPower = 0;
    int hackLevel = 0;
    int hacked = 0;
    int tempPowerCap = 1000;
    int tempPowerLoss = 0;
    int tempDividePower = 1;
};

// CloneSystem::Save.
struct CloneState {
    Milli timeToClone;
    Milli timeGoal;
    Milli deathTime;
};

// ExportShip's battery block.
struct BatteryState {
    int turnedOn = 0;
    int batteryPower = 0;
    int timerX1000 = 0;
};

// Shields::Save 0x60b910.
struct ShieldState {
    int power = 0;
    int superFirst = 0;
    int superSecond = 0;
    Milli charger;
    int downRunning = 0;
    Milli downProgress;
    int upRunning = 0;
    Milli upProgress;
    int superUpRunning = 0;
    Milli superUpProgress;
    Milli downPointX;
    Milli downPointY;
};

// CloakingSystem::Save.
struct CloakState {
    int turnedOn = 0;
    int timerRunning = 0;
    Milli currentGoal;
    Milli currentTime;
};

struct Fire {
    int damage = 0;
    int start = 0;
    int death = -1;
};

struct Room {
    int oxygen = 100;
    // One entry per floor square, in Spreader<Fire> grid order.
    std::vector<Fire> fires;
    int primarySlot = -1;
    int primaryDirection = 4;
};

struct Breach {
    int x = 0;
    int y = 0;
    int damage = 0;
};

// Door::SaveState 0x4654f0.
struct Door {
    int baseHealth = 4;
    int health = 4;
    int lastBase = 4;
    int open = 0;
    int fakeOpen = 0;
    int blast = 0;
    int hacked = 0;
};

struct Weapon {
    std::string name;
    int powered = 0;
};

struct Drone {
    std::string name;
    int powered = 0;
    int deployed = 0;
    int x = 0;
    int y = 0;
    int room = -1;
    int slot = -1;
    int health = 0;
};

struct MilliPoint {
    Milli x;
    Milli y;
};

// LockdownShard::SaveState 0x480040 (crystal lockdown, cosmetic after a load).
struct LockdownShard {
    MilliPoint position;
    MilliPoint goal;
    Milli speed;
    int arrived = 0;
    int done = 0;
    Milli lifeTime;
    int superFreeze = 0;
    int lockingRoom = 0;
    int reverse = 0;
    Milli progress;
};

// ShipManager::ExportShip 0x4a4680.
struct Ship {
    std::string blueprintName;
    std::string shipName;
    std::string imageFile;
    std::vector<std::pair<std::string, std::string>> customCrew;
    int hostile = 0;
    Milli jumpTimer;
    int jumping = 0;
    Milli jumpAnimationTime;
    int hull = 0;
    int fuel = 0;
    int droneCount = 0;
    int missileCount = 0;
    int scrap = 0;
    std::vector<Crew> crew;
    int reactorPower = 0;
    // Indexed by ShipSystem type 0-15; empty means "not installed" (one 0 int).
    // Artillery (11) may hold several records back to back.
    std::array<std::vector<SystemRecord>, 16> systems;
    CloneState clone;
    BatteryState battery;
    ShieldState shields;
    CloakState cloak;
    std::vector<Room> rooms;
    std::vector<Breach> breaches;
    std::vector<Door> doors;
    std::vector<Door> airlocks;
    Milli cloakingTracker;
    std::vector<LockdownShard> lockdowns;
    std::vector<Weapon> weapons;
    std::vector<Drone> drones;
    std::vector<std::string> augments;

    bool installed(int system) const {
        return !systems.at(static_cast<std::size_t>(system)).empty();
    }
};

struct StoreBox {
    // Store::SaveStore 0x4b3590 writes -1 for an empty box and nothing else.
    int count = -1;
    std::string blueprint;
    int value = 0;
};

struct StoreSection {
    int type = 0;
    std::array<StoreBox, 3> boxes;
};

struct Store {
    std::vector<StoreSection> sections;
    std::array<int, 3> resources{};
};

struct HostileShip {
    std::string eventName;
    std::string autoBlueprint;
    int seed = 0;
};

struct Location {
    int visited = 0;
    std::string spaceImage;
    std::string planetImage;
    int planetX = 0;
    int planetY = 0;
    int planetRotation = 0;
    int known = 0;
    std::optional<HostileShip> ship;
    int fleet = 0;
    int fleetChanging = 0;
    std::optional<Store> store;
};

// StarMap::SaveGame 0x5c7f30 / LoadGame 0x5d7710.
struct StarMap {
    int sectorMapSeed = 0;
    int currentSectorSeed = 0;
    int dangerX = 0;
    int dangerY = 0;
    int pursuitDelay = 0;
    int currentLocation = 0;
    int waitedLast = 0;
    int fuelEventSeed = -1;
    std::string lastEscapeEvent;
    int mapRevealed = 0;
    int bossLevel = 0;
    int bossLocation = 0;
    int bossJumping = 0;
    int reversedPath = 0;
    int arrivedAtBase = 0;
    std::vector<int> sectorsVisited;
    int worldLevel = 0;
    int secretSector = 0;
    std::vector<Location> locations;
    std::vector<std::pair<std::string, int>> addedQuests;
    std::vector<std::string> delayedQuests;
};

// ShipEvent::Save.
struct ShipEvent {
    int present = 0;
    int shipSeed = 0;
    std::string surrender;
    std::string escape;
    std::string destroyed;
    std::string deadCrew;
    std::string gotAway;
};

// ShipAI::Save.
struct ShipAi {
    int surrendered = 0;
    int escaping = 0;
    int destroyed = 0;
    int surrenderThreshold = 0;
    int escapeThreshold = 0;
    Milli escapeTimer;
    int stalemateTriggered = 0;
    Milli stalemateTimer;
    int timesTeleported = 0;
    int crewNeeded = 0;
};

struct Point {
    int x = 0;
    int y = 0;
};

// ProjectileFactory::SaveProjectile 0x40c8b0: GetType tag, then the class body.
// Tags: 0 Projectile/CrewLaser (no body), 1 LaserBlast, 2 Asteroid, 3 Missile,
// 4 BombProjectile, 5 BeamWeapon, 6 PDSFire. See battle.md section 3.
struct Projectile {
    int tag = 1;
    // Projectile::SaveProjectile 0x451260.
    MilliPoint position;
    MilliPoint lastPosition;
    Milli speedMagnitude;
    MilliPoint target;
    Milli heading;
    int ownerId = 0;
    int selfId = 0;
    int damage = 0;
    int shieldPiercing = 0;
    int fireChance = 0;
    int breachChance = 0;
    int ionDamage = 0;
    int systemDamage = 0;
    int crewDamage = 0;
    int hullBuster = 0;
    int damageOwnerId = -1;
    int damageSelfId = -1;
    int lockdown = 0;
    int crystalShard = 0;
    int stunChance = 0;
    int stun = 0;
    Milli lifespan{2000};
    int destinationSpace = 0;
    int currentSpace = 0;
    int targetId = 0;
    int dead = 0;
    std::string deathAnimationName;
    std::string flightAnimationName;
    Animation deathAnimation{0, 0, 0, {}, {1000}, {}, {}};
    Animation flightAnimation{1, 1, 0, {}, {1000}, {}, {}};
    MilliPoint speed;
    int missed = 0;
    int hitTarget = 0;
    std::string hitSolidSound;
    std::string hitShieldSound;
    std::string missSound;
    // cvttss2si of the float degrees; -1 before it is drawn.
    int entryAngle = -1;
    int startedDeath = 0;
    int passedTarget = 0;
    // Targetable::type (+0x08), not the class tag.
    int targetType = 5;
    int broadcastTarget = 0;
    // LaserBlast::SaveProjectile 0x451720.
    Milli spinAngle;
    Milli spinSpeed;
    // BombProjectile::SaveProjectile 0x44a1f0.
    int bombMissed = 0;
    Milli explosiveDelay{400};
    int superShield = 0;
    int superShieldBypass = 0;
    int flightReverse = 0;
    // BeamWeapon::SaveProjectile 0x434af0.
    MilliPoint subEnd;
    MilliPoint subStart;
    MilliPoint shieldEnd;
    MilliPoint finalEnd;
    MilliPoint target2;
    MilliPoint target1;
    Milli beamLifespan{1000};
    Milli length;
    Milli drift;
    MilliPoint lastCollision;
    Milli animationTimer{-1000};
    int lastDamage = 0;
    Milli startHeading;
    Milli timer;
    int piercedShield = 0;
    int oneSpace = 0;
    int damageSuperShield = 0;
    int movingTargetId = -1;
    // PDSFire::SaveProjectile 0x451780.
    MilliPoint startPoint;
    int pdsPassedTarget = 0;
    Milli currentScale{1000};
    int pdsMissed = 0;
    Animation explosion{0, 0, 0, {}, {1000}, {}, {}};
};

struct AsteroidState {
    int initialShields = 0;
    int stateTimerX1000 = 0;
    int state = 0;
    int timerX1000 = 0;
    int currentSpace = 0;
};

// SpaceManager::SaveSpace 0x4166a0.
struct Space {
    int sunLevel = 0;
    int pulsarLevel = 0;
    int pds = 0;
    int environmentTarget = 0;
    std::optional<AsteroidState> asteroids;
    int flashX1000 = 0;
    int flashTimerX1000 = 0;
    Milli randomPdsTimer;
    std::vector<Projectile> projectiles;
};

// SpaceDrone::SaveState 0x41db40.
struct SpaceDroneState {
    Milli destroyedTimer;
    int currentSpace = 0;
    int destinationSpace = 0;
    MilliPoint currentLocation;
    MilliPoint lastLocation;
    MilliPoint destinationLocation;
    MilliPoint pointTarget;
    MilliPoint targetLocation;
    MilliPoint targetSpeed;
    Milli pause;
    Milli additionalPause;
    Milli weaponCooldown;
    Milli currentAngle;
    Milli aimingAngle;
    Milli lastAimingAngle;
    int lifespan = 0;
    int disrupted = 0;
    int poweredLastFrame = 0;
    Milli ionStun;
    Milli desiredAimingAngle;
    int deployedLastFrame = 0;
    MilliPoint speedVector;
    Animation explosion;
};

struct CombatDroneState {
    MilliPoint lastDestination;
    Milli progressToDestination;
    Milli heading;
    Milli oldHeading;
};

struct BoarderDroneState {
    int diedInSpace = 0;
    int deliveredDrone = 0;
    MilliPoint startingPosition;
    int health = 0;
    int x = 0;
    int y = 0;
    int room = 0;
    int slot = 0;
};

// One ExportBattleState drone entry. Which payload exists depends on the
// drone blueprint's type (see Context::droneType).
struct BattleDrone {
    int deployed = 0;
    int powered = 0;
    SpaceDroneState space;
    CombatDroneState combat;
    Milli shieldGlow;
    BoarderDroneState boarder;
};

struct HackingState {
    int currentSystem = 0;
    int dronePowered = 0;
    int droneDeployed = 0;
    int hacking = 0;
    Milli effectFirst;
    Milli effectSecond;
    int lockedOut = 0;
    SpaceDroneState drone;
    MilliPoint finalDestination;
    int arrived = 0;
    int finishedSetup = 0;
    Animation flying;
    Animation extending;
};

struct MindState {
    Milli controlFirst;
    Milli controlSecond;
};

// WeaponAnimation::SaveState 0x456da0.
struct WeaponVisual {
    Animation animation;
    Milli slideProgress{1000};
    int firing = 0;
    int fireShot = 0;
    int boostLevel = -1;
    Animation boostAnimation{0, 0, 0, {}, {1000}, {-1000}, {-1000}};
};

// ProjectileFactory::SaveState 0x40c900.
struct WeaponFactory {
    Milli cooldownFirst;
    Milli cooldownSecond;
    Milli subCooldownFirst;
    Milli subCooldownSecond;
    int boostLevel = 0;
    int chargeLevel = 0;
    std::vector<Point> targets;
    std::vector<Point> lastTargets;
    int autoFiring = 0;
    int fireWhenReady = 0;
    int targetId = -1;
    WeaponVisual visual;
    int lastProjectileId = -1;
    std::vector<Projectile> queued;
};

// ShipManager::SaveSuperDrones 0x492750: flagship surge drones (CombatDrone payload).
struct SuperDrone {
    std::string name;
    SpaceDroneState space;
    CombatDroneState combat;
    int deployed = 0;
    int destroyed = 0;
    int powered = 0;
};

// ShipManager::ExportBattleState 0x496a60.
struct BattleState {
    std::vector<BattleDrone> drones;
    HackingState hacking;
    MindState mind;
    std::vector<WeaponFactory> weapons;
    std::vector<WeaponFactory> artillery;
    std::vector<SuperDrone> superDrones;
};

// BossShip::SaveBoss.
struct Boss {
    int currentStage = 0;
    int nextStage = 1;
    Milli powerGoal{30000};
    Milli powerTime;
    std::vector<int> crewCounts;
};

struct Score {
    int difficulty = 0;
    std::array<int, 4> stats{};
    std::string shipName;
    std::string blueprintName;
    int sector = 1;
    int flag = 0;
    // AchievementTracker::gameFlags (std::map order: sorted by key).
    std::vector<std::pair<std::string, int>> flags;
};

struct SaveGame {
    int version = saveVersion;
    int useSystemRandom = 0;
    int dlc = 1;
    Score score;
    Ship player;
    std::vector<std::string> cargo;
    StarMap starMap;
    ShipEvent shipEvent;
    std::string generatedEvent;
    int lastMainTextLiteral = 0;
    std::string lastMainText;
    int lastSelectedCrewSeed = -1;
    std::vector<int> choiceHistory;
    // Present when WorldManager::SaveGame writes nearbyShip = 1.
    std::optional<Ship> enemy;
    int enemyBoss = 0;
    ShipAi enemyAi;
    Space space;
    BattleState playerBattle;
    BattleState enemyBattle;
    int playerTeleportTargetRoom = 0;
    int enemyTeleportTargetRoom = 0;
    int combatControl = 0;
    Boss boss;
};

struct ShipLayout {
    // Floor squares per room, in room-id order.
    std::vector<int> roomSquares;
    int doors = 0;
    int airlocks = 0;
    // ArtillerySystems built from the blueprint; one record each.
    int artillery = 0;
};

// Facts the file does not carry but the reader needs; the writer checks the
// model against them when supplied.
struct Context {
    std::function<ShipLayout(const std::string& blueprint)> layout;
    // DroneBlueprint <type>: COMBAT, DEFENSE, SHIELD, BOARDER, REPAIR, BATTLE, HACKING,
    // SHIP_REPAIR. SHIP_REPAIR derives from CombatDrone and saves its payload.
    std::function<std::string(const std::string& drone)> droneType;
};

std::vector<std::uint8_t> serialize(const SaveGame& save, const Context* context = nullptr);
SaveGame parse(const std::vector<std::uint8_t>& bytes, const Context& context);

// FileHelper::getUserFolder 0x58ddb0: SHGetFolderPathW(CSIDL_PERSONAL) +
// "\My Games\FasterThanLight\". getSaveFile 0x58e2d0 prefers a "user/continue.sav"
// beside the game's working directory when that file exists. Empty off Windows.
std::filesystem::path defaultSaveFolder();
// True while an FTLGame.exe process exists (Windows only).
bool originalGameRunning();
// Terminates every FTLGame.exe without letting it save (Windows only).
void closeOriginalGame();
// closeOriginalGame, then starts the last closed executable again, or the one
// in the Steam library when none was closed.
void relaunchOriginalGame();
} // namespace ftl::original_save
