#include "run.hpp"
#include <algorithm>
#include <optional>
#include <sstream>

namespace ftl {
namespace {
std::string equipmentDescription(const EquipmentDefinition& e,
                                 TextLibrary& texts,
                                 const std::string& locale,
                                 bool advanced) {
    return e.descriptionText(texts, locale, advanced);
}

} // namespace

void Run::retireCrewDrone(CombatDrone& drone) {
    if (drone.blueprint.empty()) {
        return;
    }
    const auto& type = catalog_.equipment(drone.blueprint).type;
    // Equipment pointer erasure does not destroy factory-owned actors or
    // remove their SpaceManager registration. Keep one physical owner.
    if (type == "REPAIR" || type == "BATTLE" || type == "BOARDER") {
        retiredCrewDrones_.push_back(std::move(drone));
    } else if (type == "DEFENSE" || type == "COMBAT" || type == "SHIP_REPAIR" || type == "SHIELD") {
        retiredSpaceDrones_.push_back(std::move(drone));
    } else {
        return;
    }
    drone = {};
}

Run::InventoryFactory Run::makeInventoryFactory(const std::string& id) {
    InventoryFactory factory;
    const auto& blueprint = catalog_.equipment(id);
    if (blueprint.kind == "weapon") {
        initializeWeaponFactory(factory.weapon, blueprint, eventRandom_);
    } else if (blueprint.kind == "drone") {
        factory.drone.blueprint = id;
        initializeCrewDroneAnimation(factory.drone, eventRandom_);
    }
    return factory;
}

void Run::detachInventoryWeapon(int index) {
    if (weaponPower_.at(index)) {
        if (auto* controller = system(3)) {
            const int required = catalog_.equipment(ship_.weapons.at(index)).power;
            const int bonus = combatWeapons_.at(index).bonusPower;
            releaseBatteryPower(*controller, std::max(0, required - bonus));
            controller->power = std::max(0, controller->power - required);
            controller->bonusPower = std::max(0, controller->bonusPower - bonus);
        }
    }
    // RemoveWeapon60fe90 depowers without the player input lock checks.
    // SetPowered40c800 retains the same factory charge, aim and animation.
    weaponPower_.at(index) = false;
    weaponDesired_.at(index) = false;
}

bool Run::inventoryMove(const std::string& kind, int index, bool equip) {
    ensureInventorySlots();
    if (!docked() || index < 0) {
        return false;
    }
    if (equip) {
        if (index >= static_cast<int>(cargo_.size())) {
            return false;
        }
        const auto id = cargo_[index];
        const auto& item = catalog_.equipment(id);
        if (item.kind == "weapon" && system(3) &&
            ship_.weapons.size() < std::size_t(ship_.weaponSlots)) {
            *std::ranges::find(weaponSlots_, -1) = static_cast<int>(ship_.weapons.size());
            ship_.weapons.push_back(id);
            weaponPower_.push_back(false);
            weaponDesired_.push_back(false);
            combatWeapons_.push_back(std::move(cargoFactories_.at(index).weapon));
        } else if (item.kind == "drone" && system(4) &&
                   ship_.drones.size() < std::size_t(ship_.droneSlots)) {
            *std::ranges::find(droneSlots_, -1) = static_cast<int>(ship_.drones.size());
            ship_.drones.push_back(id);
            combatDrones_.push_back(std::move(cargoFactories_.at(index).drone));
        } else if (item.kind == "aug" && ship_.augments.size() < 3 &&
                   (item.stacking ||
                    std::ranges::find(ship_.augments, id) == ship_.augments.end())) {
            ship_.augments.push_back(id);
        } else {
            return false;
        }
        cargo_.erase(cargo_.begin() + index);
        cargoFactories_.erase(cargoFactories_.begin() + index);
    } else {
        if (kind == "augment" || cargo_.size() >= 4) {
            return false;
        }
        auto* items = kind == "weapon"    ? &ship_.weapons
                      : kind == "drone"   ? &ship_.drones
                      : kind == "augment" ? &ship_.augments
                                          : nullptr;
        if (!items || index >= static_cast<int>(items->size())) {
            return false;
        }
        InventoryFactory factory;
        if (kind == "weapon") {
            detachInventoryWeapon(index);
            factory.weapon = std::move(combatWeapons_[index]);
            combatWeapons_.erase(combatWeapons_.begin() + index);
            weaponPower_.erase(weaponPower_.begin() + index);
            weaponDesired_.erase(weaponDesired_.begin() + index);
            cancelAim();
        } else if (kind == "drone") {
            detachInventoryDrone(index);
            reindexDroneReferences(0, index, true);
            factory.drone = std::move(combatDrones_[index]);
            combatDrones_.erase(combatDrones_.begin() + index);
        }
        eraseInventorySlot(kind, index);
        cargo_.push_back((*items)[index]);
        cargoFactories_.push_back(std::move(factory));
        items->erase(items->begin() + index);
    }
    commitCombatEquipmentReward();
    return true;
}

void Run::removeEquippedItem(const std::string& id) {
    // ShipManager::RemoveItem496740 checks augment first, then installed
    // weapon/drone vectors; cargo and overflow do not participate.
    const auto augment = std::ranges::find(ship_.augments, id);
    if (augment != ship_.augments.end()) {
        ship_.augments.erase(augment);
        return;
    }
    const auto weapon = std::ranges::find(ship_.weapons, id);
    if (weapon != ship_.weapons.end()) {
        const int index = static_cast<int>(weapon - ship_.weapons.begin());
        detachInventoryWeapon(index);
        eraseInventorySlot("weapon", index);
        weaponPower_.erase(weaponPower_.begin() + index);
        weaponDesired_.erase(weaponDesired_.begin() + index);
        combatWeapons_.erase(combatWeapons_.begin() + index);
        ship_.weapons.erase(weapon);
        cancelAim();
        return;
    }
    const auto drone = std::ranges::find(ship_.drones, id);
    if (drone == ship_.drones.end()) {
        return;
    }
    const int index = static_cast<int>(drone - ship_.drones.begin());
    detachInventoryDrone(index);
    reindexDroneReferences(0, index, true);
    eraseInventorySlot("drone", index);
    retireCrewDrone(combatDrones_[index]);
    combatDrones_.erase(combatDrones_.begin() + index);
    ship_.drones.erase(drone);
}

bool Run::inventoryCanHold(const std::string& id) const {
    const auto& item = catalog_.equipment(id);
    if (item.kind == "aug") {
        return ship_.augments.size() < 3 &&
               (item.stacking || std::ranges::find(ship_.augments, id) == ship_.augments.end());
    }
    return cargo_.size() < 4 ||
           (item.kind == "weapon" && system(3) &&
            ship_.weapons.size() < std::size_t(ship_.weaponSlots)) ||
           (item.kind == "drone" && system(4) &&
            ship_.drones.size() < std::size_t(ship_.droneSlots));
}

bool Run::inventoryCompletelyFull(const std::string& kind) const {
    // Equipment::IsCompletelyFull 0x5282e0 looks for an empty box that can hold
    // the type. OnInit 0x529880 appends overcapacityBox and overAugBox to
    // vEquipmentBoxes, so an empty overflow box still counts. Weapon and drone
    // boxes hold items only with their system (WeaponEquipBox::CanHoldWeapon
    // 0x72c720, DroneEquipBox::CanHoldDrone 0x728620).
    if (kind == "aug") {
        return ship_.augments.size() >= 3 && !overflowAugment_.empty();
    }
    const bool equipped = kind == "weapon"
                              ? system(3) && ship_.weapons.size() < std::size_t(ship_.weaponSlots)
                              : system(4) && ship_.drones.size() < std::size_t(ship_.droneSlots);
    return !equipped && cargo_.size() >= 4 && !overflowEquipment_.empty();
}

std::string Run::rewardEquipment(const std::string& id) {
    // WorldManager::ModifyResources 0x5f0290: a held non-stacking augment opens
    // AUGMENT_FULL; otherwise a completely full equipment screen opens
    // EQUIP_FULL instead of AddWeapon/AddAugment/AddDrone. Both followups pay
    // 25 scrap and add nothing; the overflow box keeps its earlier item.
    const auto& item = catalog_.equipment(id);
    if (item.kind == "aug" && !item.stacking &&
        std::ranges::find(ship_.augments, id) != ship_.augments.end()) {
        return "AUGMENT_FULL";
    }
    if (inventoryCompletelyFull(item.kind)) {
        return "EQUIP_FULL";
    }
    if (!inventoryAdd(id, true)) {
        throw std::runtime_error("reward_equipment_not_added");
    }
    return {};
}

bool Run::inventoryAdd(const std::string& id, bool overflow, const InventoryFactory* existing) {
    ensureInventorySlots();
    const auto& definition = catalog_.equipment(id);
    // Equipment::AddWeapon529030/AddDrone construct before selecting the
    // equipped, cargo or overflow box. Moves transfer these same factories.
    auto factory = existing ? *existing : makeInventoryFactory(id);
    if (definition.kind == "aug" && !definition.stacking &&
        std::ranges::find(ship_.augments, id) != ship_.augments.end()) {
        return false;
    }
    if (!inventoryCanHold(id)) {
        auto& extra = catalog_.equipment(id).kind == "aug" ? overflowAugment_ : overflowEquipment_;
        if (overflow) {
            // AddWeapon529030/AddDrone528470 always replace the overflow
            // box after exhausting ordinary boxes. Factory-owned actors live
            // on after an older overflow pointer is overwritten.
            if (definition.kind != "aug") {
                retireCrewDrone(overflowFactory_.drone);
            }
            extra = id;
            if (definition.kind != "aug") {
                overflowFactory_ = std::move(factory);
            }
            panel_ = "equipment";
            return true;
        }
        if (!existing) {
            retireCrewDrone(factory.drone);
        }
        return false;
    }
    const auto& item = catalog_.equipment(id);
    if (item.kind == "aug") {
        ship_.augments.push_back(id);
    } else if (item.kind == "weapon" && system(3) &&
               ship_.weapons.size() < std::size_t(ship_.weaponSlots)) {
        *std::ranges::find(weaponSlots_, -1) = static_cast<int>(ship_.weapons.size());
        ship_.weapons.push_back(id);
        weaponPower_.push_back(false);
        weaponDesired_.push_back(false);
        combatWeapons_.push_back(std::move(factory.weapon));
    } else if (item.kind == "drone" && system(4) &&
               ship_.drones.size() < std::size_t(ship_.droneSlots)) {
        *std::ranges::find(droneSlots_, -1) = static_cast<int>(ship_.drones.size());
        ship_.drones.push_back(id);
        combatDrones_.push_back(std::move(factory.drone));
    } else {
        cargo_.push_back(id);
        cargoFactories_.push_back(std::move(factory));
    }
    return true;
}

bool Run::inventorySwap(int cargoIndex, const std::string& kind, int index) {
    if (!docked() || cargoIndex < 0 || cargoIndex >= static_cast<int>(cargo_.size()) || index < 0) {
        return false;
    }
    const auto& item = catalog_.equipment(cargo_[cargoIndex]);
    auto* items = kind == "weapon" ? &ship_.weapons : kind == "drone" ? &ship_.drones : nullptr;
    if (!items || item.kind != kind || index >= static_cast<int>(items->size())) {
        return false;
    }
    if (kind == "weapon") {
        detachInventoryWeapon(index);
        std::swap(combatWeapons_[index], cargoFactories_.at(cargoIndex).weapon);
        cancelAim();
    } else {
        detachInventoryDrone(index);
        reindexDroneReferences(0, index);
        std::swap(combatDrones_[index], cargoFactories_.at(cargoIndex).drone);
    }
    std::swap(cargo_[cargoIndex], (*items)[index]);
    return true;
}

Rect Run::inventoryOverflowRect(bool augment, bool itemOnly) const {
    const float offset =
        (panel_ == "store" ? std::max(283.f, managementPanelX()) : managementPanelX()) - 334;
    const auto body =
        texts_.get(augment ? "overcapacity_aug_text1" : "overcapacity_item_text1", locale_);
    const float height = textLineHeight(52, body);
    const float bodyHeight =
        std::round(height * static_cast<float>(wrappedText(52, body, 235).size()));
    const auto title =
        texts_.get(augment ? "overcapacity_aug_title" : "overcapacity_item_title", locale_);
    const float titleInsert = title.find('\n') == std::string::npos ? 0.f : 24.f;
    if (itemOnly) {
        return {augment ? 78.f + offset : 139.f + offset,
                185 + titleInsert + 65 + bodyHeight + (augment ? 4.f : 0.f),
                augment ? 250.f : 109.f,
                augment ? 55.f : 76.f};
    }
    const auto lower =
        texts_.get(augment ? "overcapacity_aug_text2" : "overcapacity_item_text2", locale_);
    const float lowerHeight =
        lower.empty() ? 0
                      : std::round(textLineHeight(52, lower) *
                                   static_cast<float>(wrappedText(52, lower, 235).size()));
    return {59 + offset,
            185,
            272,
            94 + titleInsert + bodyHeight + lowerHeight + (augment ? 65.f : 100.f) - 20};
}

void Run::renderEquipmentInfo(const EquipmentDefinition& bp, float infoX, float infoY) {
    const auto title = bp.title.reference ? texts_.get(bp.title.value, locale_) : bp.title.value;
    const auto description = equipmentDescription(bp, texts_, locale_, advanced_);
    const auto info =
        [&](const std::string& header, const std::string& body, float py, float minimum) {
            const bool secondary = header.empty();
            const auto lines = wrappedText(secondary ? 9 : 10, body, secondary ? 307.f : 305.f);
            const auto titleLines =
                header.empty() ? std::vector<std::string>{} : wrappedText(16, header, 305);
            const float textY = secondary ? 22.f
                                          : 28.f + textLineHeight(16, header) *
                                                       static_cast<float>(titleLines.size());
            const float height =
                secondary
                    ? std::max(60.f,
                               textLineHeight(9, body) * static_cast<float>(lines.size()) + 35) +
                          10
                    : minimum;
            renderer_.windowFrame({infoX, py, 337, height});
            float titleY = py + 13;
            for (const auto& line : titleLines) {
                text(line, infoX + 18, titleY, 16, false, {255, 255, 255, 255});
                titleY += textLineHeight(16, header);
            }
            float lineY = py + textY;
            for (const auto& line : lines) {
                text(line,
                     infoX + (secondary ? 20 : 18),
                     lineY,
                     secondary ? 9 : 10,
                     false,
                     {255, 255, 255, 255});
                lineY += textLineHeight(secondary ? 9 : 10, body);
            }
            return height;
        };
    const float height = info(title,
                              description,
                              infoY,
                              bp.kind == "weapon"  ? 266.f
                              : bp.kind == "drone" ? 176.f
                                                   : 150.f);
    if (!bp.tip.value.empty()) {
        const auto tip = bp.tip.reference ? texts_.get(bp.tip.value, locale_) : bp.tip.value;
        info("", tip, infoY + height + 3, 70);
    }
}

std::string Run::inventoryDraggedItem() const {
    if (inventoryDragIndex_ < 0) {
        return {};
    }
    const auto* items = inventoryDragKind_ == "weapon"    ? &ship_.weapons
                        : inventoryDragKind_ == "drone"   ? &ship_.drones
                        : inventoryDragKind_ == "augment" ? &ship_.augments
                        : inventoryDragKind_ == "cargo"   ? &cargo_
                                                          : nullptr;
    if (items && inventoryDragIndex_ < static_cast<int>(items->size())) {
        return (*items)[std::size_t(inventoryDragIndex_)];
    }
    if (inventoryDragKind_ == "overflow") {
        return overflowEquipment_;
    }
    if (inventoryDragKind_ == "overflow_aug") {
        return overflowAugment_;
    }
    return {};
}

std::string Run::inventoryHoveredItem() const {
    // Equipment::MouseMove 0x525580 selectedEquipBox: the box under the mouse.
    const float offset =
        (panel_ == "store" ? std::max(283.f, managementPanelX()) : managementPanelX()) - 334;
    for (bool augment : {false, true}) {
        const auto& overflow = augment ? overflowAugment_ : overflowEquipment_;
        if (!overflow.empty() && inventoryOverflowRect(augment).contains(mouseX_, mouseY_)) {
            return overflow;
        }
    }
    for (const auto& kind : {"weapon", "drone", "augment", "cargo"}) {
        const std::string_view name = kind;
        const int limit = name == "weapon"    ? (system(3) ? ship_.weaponSlots : 0)
                          : name == "drone"   ? (system(4) ? ship_.droneSlots : 0)
                          : name == "augment" ? 3
                                              : 4;
        for (int i = 0; i < limit; ++i) {
            const float x = name == "cargo" ? 364.f + offset + 130.f * float(i % 2)
                            : name == "augment"
                                ? 664.f + offset
                                : 397.f + offset + 58.f * float(4 - limit) + 117.f * float(i);
            const float y = name == "cargo"    ? 378.f + 80.f * float(i / 2)
                            : name == "weapon" ? 155.f
                            : name == "drone"  ? 265.f
                                               : 378.f + 60.f * float(i);
            if (!Rect{x, y, name == "augment" ? 250.f : 109.f, name == "augment" ? 55.f : 76.f}
                     .contains(mouseX_, mouseY_)) {
                continue;
            }
            if (name == "weapon" || name == "drone") {
                const int dense = logicalArmamentIndex(kind, i);
                const auto& items = name == "weapon" ? ship_.weapons : ship_.drones;
                return dense >= 0 ? items[std::size_t(dense)] : std::string{};
            }
            const auto& items = name == "augment" ? ship_.augments : cargo_;
            return i < static_cast<int>(items.size()) ? items[std::size_t(i)] : std::string{};
        }
    }
    return {};
}

void Run::inventoryRender() {
    const float offset =
        (panel_ == "store" ? std::max(283.f, managementPanelX()) : managementPanelX()) - 334;
    const std::string draggedItem = inventoryDraggedItem();
    const std::string draggedKind =
        draggedItem.empty() ? std::string{} : catalog_.equipment(draggedItem).kind;
    // Equipment::OnLoop 0x524fb0 colours every box while dragging. A box that
    // can take the held weapon or drone (WeaponEquipBox/DroneEquipBox
    // CanHold* need the system; a cargo EquipmentBox 0x52b0f0/0x52b130 needs
    // to be empty or hold the same category), or any cargo box when the held
    // item came from a cargo box, is unblocked and COLOR_GREEN (100,255,100);
    // an augment is unblocked only by an empty augment box (or any augment box
    // when dragging the over-capacity augment) and stays white. Every other
    // box is blocked and COLOR_RED (255,50,50). Boxes keyed "overflow" are the
    // over-capacity cargo box, "overflow_aug" the over-capacity augment box.
    const auto dropColor = [&](const std::string& box,
                               const std::string& id) -> std::optional<SDL_Color> {
        if (draggedItem.empty()) {
            return std::nullopt;
        }
        const bool boxCargo = box == "cargo" || box == "overflow";
        const bool sourceCargo = inventoryDragKind_ == "cargo" || inventoryDragKind_ == "overflow";
        const auto contents = id.empty() ? std::string{} : catalog_.equipment(id).kind;
        bool open = false;
        if (draggedKind == "weapon" || draggedKind == "drone") {
            open = box == draggedKind ? system(draggedKind == "weapon" ? 3 : 4) != nullptr
                   : boxCargo         ? contents.empty() || contents == draggedKind
                                      : false;
        } else if (draggedKind == "aug") {
            open = box == "augment" && id.empty();
            if (!open && inventoryDragKind_ == "overflow_aug") {
                open = box == "augment" || box == "overflow_aug";
            }
        }
        if (!open && sourceCargo && boxCargo) {
            open = true;
        }
        if (!open) {
            return SDL_Color{255, 50, 50, 255};
        }
        if (draggedKind == "weapon" || draggedKind == "drone") {
            return SDL_Color{100, 255, 100, 255};
        }
        return std::nullopt;
    };
    // EquipmentBox::OnRender 0x52b3d0: a non-white overlayColor draws
    // box_overlay_red.png tinted with it at alpha 0.5, except on augment boxes
    // (GetType 3).
    const auto dropOverlay = [&](const std::string& box, const std::string& id, float x, float y) {
        const auto color = dropColor(box, id);
        if (!color || box == "augment" || box == "overflow_aug") {
            return;
        }
        auto& art = renderer_.image("img/upgradeUI/Equipment/box_overlay_red.png");
        renderer_.sprite(art,
                         {x, y, static_cast<float>(art.w), static_cast<float>(art.h)},
                         {0, 0, static_cast<float>(art.w), static_cast<float>(art.h)},
                         0,
                         {color->r, color->g, color->b, 128});
    };
    const auto slot = [&](const std::string& kind,
                          int index,
                          const std::string& id,
                          float x,
                          float y,
                          bool enabled,
                          bool heldIcon = false) {
        const bool sourceHeld = !heldIcon && inventoryDragIndex_ == index && !draggedItem.empty() &&
                                draggedItem == id &&
                                (inventoryDragKind_ == kind || inventoryDragKind_ == "overflow" ||
                                 inventoryDragKind_ == "overflow_aug");
        // EquipmentBox::GetType/UpdateBoxImage 0x52b160/0x52b790:
        // cargo boxes acquire their category from contents, and a held source
        // becomes a genuinely empty base box until its release is resolved.
        const auto contentKind = !id.empty() && !sourceHeld ? catalog_.equipment(id).kind : "";
        const std::string art = kind == "weapon" || contentKind == "weapon" ? "box_weapons"
                                : kind == "drone" || contentKind == "drone" ? "box_drones"
                                : kind == "augment"                         ? "box_augment"
                                                                            : "box_base";
        const bool hovered =
            Rect{x, y, kind == "augment" ? 250.f : 109.f, kind == "augment" ? 55.f : 76.f}.contains(
                mouseX_, mouseY_);
        const bool filled = !id.empty() && !sourceHeld;
        const auto selected =
            "img/upgradeUI/Equipment/" + art + (filled ? "_selected.png" : "_off_selected.png");
        // Equipment::MouseMove 0x525580 sets bGlow on the hovered box only when
        // it holds an item, or, while dragging, when the box is not blocked.
        const auto drop = heldIcon ? std::nullopt : dropColor(kind, id);
        const bool blocked = drop && drop->g == 50;
        const bool glow = enabled && hovered && (draggedItem.empty() ? filled : !blocked);
        if (!heldIcon || kind == "augment") {
            renderer_.draw(glow && archive_.contains(selected)
                               ? selected
                               : "img/upgradeUI/Equipment/" + art +
                                     (enabled && filled ? "_on.png" : "_off.png"),
                           x,
                           y);
        }
        if (!heldIcon && kind == "cargo" && index >= 0 && inventorySelection_ == index) {
            renderer_.draw(selected, x, y);
        }
        if (!id.empty() && !sourceHeld) {
            const auto& bp = catalog_.equipment(id);
            const auto label = bp.shortTitle.reference ? texts_.get(bp.shortTitle.value, locale_)
                                                       : bp.shortTitle.value;
            if (kind == "augment") {
                const auto title =
                    bp.title.reference ? texts_.get(bp.title.value, locale_) : bp.title.value;
                text(title.empty() ? label : title, x + 117, y + 14, 13, true);
            } else {
                if (!heldIcon) {
                    text(label.empty() ? id : label, x + 60, y + 58, 6, true);
                }
                if (bp.kind == "weapon" && !bp.imagePath.empty() && bp.frameWidth > 0 &&
                    bp.frameHeight > 0) {
                    SDL_FPoint pivot{static_cast<float>(bp.frameWidth) / 2,
                                     static_cast<float>(bp.frameHeight) / 2};
                    renderer_.sprite(
                        renderer_.image(bp.imagePath),
                        {x + 63 - pivot.x,
                         y + 35 - pivot.y,
                         static_cast<float>(bp.frameWidth),
                         static_cast<float>(bp.frameHeight)},
                        {static_cast<float>(bp.frameX + bp.chargedFrame * bp.frameWidth),
                         static_cast<float>(bp.frameY),
                         static_cast<float>(bp.frameWidth),
                         static_cast<float>(bp.frameHeight)},
                        90,
                        {255, 255, 255, 255},
                        true,
                        &pivot);
                } else if (bp.kind == "drone") {
                    renderDroneIcon(bp, x + 60, y + 32);
                }
            }
            if (Rect{x, y, kind == "augment" ? 250.f : 109.f, kind == "augment" ? 55.f : 76.f}
                    .contains(mouseX_, mouseY_)) {
                renderEquipmentInfo(bp, 934 + offset, 127);
            }
        }
    };
    text(texts_.get("equipment_frame_weapons", locale_),
         352 + offset,
         129,
         62,
         false,
         {25, 49, 51, 255});
    text(texts_.get("equipment_frame_drones", locale_),
         352 + offset,
         239,
         62,
         false,
         {25, 49, 51, 255});
    text(texts_.get("equipment_frame_cargo", locale_),
         352 + offset,
         349,
         62,
         false,
         {25, 49, 51, 255});
    text(texts_.get("equipment_frame_augments", locale_),
         641 + offset,
         349,
         62,
         false,
         {25, 49, 51, 255});
    for (int i = 0; system(3) && i < ship_.weaponSlots; ++i) {
        const int dense = logicalArmamentIndex("weapon", i);
        slot("weapon",
             i,
             dense >= 0 ? ship_.weapons[dense] : "",
             397.f + offset + 58.f * (4 - ship_.weaponSlots) + 117.f * i,
             155,
             system(3) != nullptr);
        dropOverlay("weapon",
                    dense >= 0 ? ship_.weapons[dense] : "",
                    397.f + offset + 58.f * (4 - ship_.weaponSlots) + 117.f * i,
                    155);
    }
    for (int i = 0; system(4) && i < ship_.droneSlots; ++i) {
        const int dense = logicalArmamentIndex("drone", i);
        slot("drone",
             i,
             dense >= 0 ? ship_.drones[dense] : "",
             397.f + offset + 58.f * (4 - ship_.droneSlots) + 117.f * i,
             265,
             system(4) != nullptr);
        dropOverlay("drone",
                    dense >= 0 ? ship_.drones[dense] : "",
                    397.f + offset + 58.f * (4 - ship_.droneSlots) + 117.f * i,
                    265);
    }
    if (!system(3)) {
        text(texts_.get("equipment_no_system", locale_), 635 + offset, 178, 63, true);
    }
    if (!system(4)) {
        text(texts_.get("equipment_no_system", locale_), 635 + offset, 288, 63, true);
    }
    for (int i = 0; i < 3; ++i) {
        slot("augment",
             i,
             i < static_cast<int>(ship_.augments.size()) ? ship_.augments[i] : "",
             664 + offset,
             378.f + 60.f * i,
             true);
    }
    for (int i = 0; i < 4; ++i) {
        slot("cargo",
             i,
             i < static_cast<int>(cargo_.size()) ? cargo_[i] : "",
             364.f + offset + 130.f * (i % 2),
             378.f + 80.f * (i / 2),
             true);
        dropOverlay("cargo",
                    i < static_cast<int>(cargo_.size()) ? cargo_[i] : "",
                    364.f + offset + 130.f * (i % 2),
                    378.f + 80.f * (i / 2));
    }
    for (bool augment : {false, true}) {
        const auto& overflow = augment ? overflowAugment_ : overflowEquipment_;
        if (overflow.empty()) {
            continue;
        }
        // DropBox::OnInit 0x4da1f0 / OnRender 0x4d8e70; Equipment0x529880.
        const auto title =
            texts_.get(augment ? "overcapacity_aug_title" : "overcapacity_item_title", locale_);
        const auto body =
            texts_.get(augment ? "overcapacity_aug_text1" : "overcapacity_item_text1", locale_);
        const auto lower =
            texts_.get(augment ? "overcapacity_aug_text2" : "overcapacity_item_text2", locale_);
        const float lineHeight = textLineHeight(52, body);
        const float bodyHeight = lineHeight * static_cast<float>(wrappedText(52, body, 235).size());
        const float lowerHeight =
            lower.empty() ? 0 : lineHeight * static_cast<float>(wrappedText(52, lower, 235).size());
        const float titleInsert = title.find('\n') == std::string::npos ? 0.f : 24.f;
        const float bodySpace = augment ? 65.f : 100.f;
        const float insert = bodySpace + bodyHeight + lowerHeight - 20;
        auto& art = renderer_.image("img/dropbox_on.png");
        const float width = static_cast<float>(art.w);
        const float height = static_cast<float>(art.h);
        renderer_.sprite(art, {59 + offset, 185, width, 32}, {0, 0, width, 32});
        renderer_.sprite(art, {59 + offset, 217, width, titleInsert + 1}, {0, 32, width, 1});
        renderer_.sprite(art, {59 + offset, 218 + titleInsert, width, 31}, {0, 33, width, 31});
        renderer_.sprite(
            art, {59 + offset, 249 + titleInsert, width, insert + 1}, {0, 64, width, 1});
        renderer_.sprite(art,
                         {59 + offset, 250 + titleInsert + insert, width, height - 65},
                         {0, 65, width, height - 65});
        const auto titleBreak = title.find('\n');
        if (titleBreak == std::string::npos) {
            text(title, 195 + offset, 197, 63, true, {25, 49, 51, 255});
        } else {
            text(title.substr(0, titleBreak), 195 + offset, 195, 63, true, {25, 49, 51, 255});
            text(title.substr(titleBreak + 1), 195 + offset, 223, 63, true, {25, 49, 51, 255});
        }
        float bodyY = 242 + titleInsert;
        for (const auto& line : wrappedText(52, body, 235)) {
            text(line, 195 + offset, bodyY, 52, true);
            bodyY += lineHeight;
        }
        if (!lower.empty()) {
            float lowerY = 242 + titleInsert + bodyHeight + bodySpace;
            for (const auto& line : wrappedText(52, lower, 235)) {
                text(line, 195 + offset, lowerY, 52, true);
                lowerY += lineHeight;
            }
        }
        const auto area = inventoryOverflowRect(augment, true);
        slot(augment ? "augment" : "cargo", 0, overflow, area.x, area.y, true);
        dropOverlay(augment ? "overflow_aug" : "overflow", overflow, area.x, area.y);
    }
    if (!draggedItem.empty()) {
        // Equipment::OnRender 0x525dd0 draws the held icon at currentMouse;
        // the source box is rendered empty while the item remains owned there.
        const auto& item = catalog_.equipment(draggedItem);
        slot(item.kind == "aug" ? "augment" : "cargo",
             -1,
             draggedItem,
             mouseX_ - (item.kind == "aug"      ? 0.f
                        : item.kind == "weapon" ? 63.f
                                                : 60.f),
             mouseY_ - (item.kind == "aug"      ? 0.f
                        : item.kind == "weapon" ? 35.f
                                                : 29.f),
             true,
             true);
    }
}

void Run::inventoryClick(float x, float y) {
    const float offset =
        (panel_ == "store" ? std::max(283.f, managementPanelX()) : managementPanelX()) - 334;
    if (pendingStoreOffer_ >= 0) {
        return;
    }
    ensureInventorySlots();
    for (int i = 0; i < 4; ++i) {
        if (!Rect{364.f + offset + 130.f * (i % 2), 378.f + 80.f * (i / 2), 109, 76}.contains(x,
                                                                                              y)) {
            continue;
        }
        if (!overflowEquipment_.empty()) {
            if (catalog_.equipment(overflowEquipment_).kind != "aug") {
                if (i < static_cast<int>(cargo_.size())) {
                    std::swap(cargo_[i], overflowEquipment_);
                    std::swap(cargoFactories_.at(i), overflowFactory_);
                } else if (cargo_.size() < 4) {
                    cargo_.push_back(overflowEquipment_);
                    cargoFactories_.push_back(std::move(overflowFactory_));
                    overflowEquipment_.clear();
                    overflowFactory_ = {};
                }
            }
        } else if (i < static_cast<int>(cargo_.size())) {
            inventorySelection_ = inventorySelection_ == i ? -1 : i;
        }
        return;
    }
    for (const auto& kind : {"weapon", "drone", "augment"}) {
        auto& overflow =
            std::string_view(kind) == "augment" ? overflowAugment_ : overflowEquipment_;
        const auto& items = std::string_view(kind) == "weapon"  ? ship_.weapons
                            : std::string_view(kind) == "drone" ? ship_.drones
                                                                : ship_.augments;
        const int limit = std::string_view(kind) == "weapon"  ? ship_.weaponSlots
                          : std::string_view(kind) == "drone" ? ship_.droneSlots
                                                              : 3;
        for (int i = 0; i < limit; ++i) {
            const float sx = std::string_view(kind) == "augment"
                                 ? 664.f + offset
                                 : 397.f + offset + 58.f * (4 - limit) + 117.f * i;
            const float sy = std::string_view(kind) == "weapon"  ? 155.f
                             : std::string_view(kind) == "drone" ? 265.f
                                                                 : 378.f + 60.f * i;
            if (!Rect{sx,
                      sy,
                      std::string_view(kind) == "augment" ? 250.f : 109.f,
                      std::string_view(kind) == "augment" ? 55.f : 76.f}
                     .contains(x, y)) {
                continue;
            }
            const int dense =
                std::string_view(kind) == "augment" ? i : logicalArmamentIndex(kind, i);
            if (!overflow.empty()) {
                const auto wanted = std::string_view(kind) == "augment" ? "aug" : kind;
                const auto& bp = catalog_.equipment(overflow);
                if (bp.kind != wanted) {
                    return;
                }
                if (dense >= 0 && dense < static_cast<int>(items.size())) {
                    if (std::string_view(kind) == "augment") {
                        if (bp.stacking ||
                            std::ranges::find(ship_.augments, overflow) == ship_.augments.end()) {
                            std::swap(ship_.augments[i], overflow);
                        }
                    } else {
                        // Temporarily use cargo to share the native power/charge cleanup.
                        cargo_.push_back(overflow);
                        cargoFactories_.push_back(overflowFactory_);
                        if (inventorySwap(static_cast<int>(cargo_.size()) - 1, kind, dense)) {
                            overflow = cargo_.back();
                            overflowFactory_ = std::move(cargoFactories_.back());
                        }
                        cargo_.pop_back();
                        cargoFactories_.pop_back();
                    }
                } else if (inventoryCanHold(overflow)) {
                    const auto id = overflow;
                    if (inventoryAdd(id, false, &overflowFactory_)) {
                        overflow.clear();
                        if (std::string_view(kind) != "augment") {
                            overflowFactory_ = {};
                        }
                    }
                }
            } else if (inventorySelection_ >= 0) {
                if (dense >= 0 && dense < static_cast<int>(items.size())) {
                    inventorySwap(inventorySelection_, kind, dense);
                } else {
                    const auto wanted = std::string_view(kind) == "augment" ? "aug" : kind;
                    if (catalog_.equipment(cargo_[inventorySelection_]).kind != wanted) {
                        return;
                    }
                    inventoryMove({}, inventorySelection_, true);
                }
                inventorySelection_ = -1;
            } else if (dense >= 0 && dense < static_cast<int>(items.size())) {
                inventoryMove(kind, dense, false);
            }
            return;
        }
    }
}

bool Run::inventoryPointerDown(float x, float y) {
    const float offset =
        (panel_ == "store" ? std::max(283.f, managementPanelX()) : managementPanelX()) - 334;
    if (pendingStoreOffer_ >= 0) {
        return false;
    }
    ensureInventorySlots();
    if (panel_ != "equipment" && !(panel_ == "store" && storeSelling_)) {
        return false;
    }
    inventoryDragKind_.clear();
    inventoryDragIndex_ = -1;
    if (!overflowAugment_.empty() && inventoryOverflowRect(true).contains(x, y)) {
        inventoryDragKind_ = "overflow_aug";
        inventoryDragIndex_ = 0;
        return true;
    }
    if (!overflowEquipment_.empty() && inventoryOverflowRect().contains(x, y)) {
        inventoryDragKind_ = "overflow";
        inventoryDragIndex_ = 0;
        return true;
    }
    for (const auto& kind : {"weapon", "drone", "augment", "cargo"}) {
        const auto& items = std::string_view(kind) == "weapon"    ? ship_.weapons
                            : std::string_view(kind) == "drone"   ? ship_.drones
                            : std::string_view(kind) == "augment" ? ship_.augments
                                                                  : cargo_;
        const int limit = std::string_view(kind) == "weapon"    ? ship_.weaponSlots
                          : std::string_view(kind) == "drone"   ? ship_.droneSlots
                          : std::string_view(kind) == "augment" ? 3
                                                                : 4;
        for (int i = 0; i < static_cast<int>(items.size()); ++i) {
            const int visual =
                (std::string_view(kind) == "weapon" || std::string_view(kind) == "drone")
                    ? logicalArmamentSlot(kind, i)
                    : i;
            const float sx = std::string_view(kind) == "cargo" ? 364.f + offset + 130.f * (i % 2)
                             : std::string_view(kind) == "augment"
                                 ? 664.f + offset
                                 : 397.f + offset + 58.f * (4 - limit) + 117.f * visual;
            const float sy = std::string_view(kind) == "cargo"    ? 378.f + 80.f * (i / 2)
                             : std::string_view(kind) == "weapon" ? 155.f
                             : std::string_view(kind) == "drone"  ? 265.f
                                                                  : 378.f + 60.f * i;
            if (Rect{sx,
                     sy,
                     std::string_view(kind) == "augment" ? 250.f : 109.f,
                     std::string_view(kind) == "augment" ? 55.f : 76.f}
                    .contains(x, y)) {
                inventoryDragKind_ = kind;
                inventoryDragIndex_ = i;
                return true;
            }
        }
    }
    return false;
}

void Run::inventoryPointerUp(float x, float y) {
    const float offset =
        (panel_ == "store" ? std::max(283.f, managementPanelX()) : managementPanelX()) - 334;
    ensureInventorySlots();
    if (inventoryDragIndex_ < 0) {
        return;
    }
    const auto kind = inventoryDragKind_;
    const int source = inventoryDragIndex_;
    inventoryDragKind_.clear();
    inventoryDragIndex_ = -1;
    if (!docked() || pendingStoreOffer_ >= 0) {
        return;
    }
    if (panel_ == "store" && storeSelling_ && Rect{59 + offset, 185, 260, 199}.contains(x, y)) {
        if (kind == "overflow" || kind == "overflow_aug") {
            auto& overflow = kind == "overflow_aug" ? overflowAugment_ : overflowEquipment_;
            if (!overflow.empty()) {
                const auto previous = resourceAmounts();
                scrap_ += catalog_.equipment(overflow).cost / 2;
                audio_.sound("buy");
                overflow.clear();
                if (kind == "overflow") {
                    retireCrewDrone(overflowFactory_.drone);
                    overflowFactory_ = {};
                }
                noteResourceChanges(previous);
            }
        } else {
            storeAction("sell:" + kind + ":" + std::to_string(source), "click");
        }
        return;
    }
    if (kind == "overflow" || kind == "overflow_aug") {
        inventoryClick(x, y);
        return;
    }
    const bool extraAug = kind == "augment";
    auto& overflow = extraAug ? overflowAugment_ : overflowEquipment_;
    if (!overflow.empty() && inventoryOverflowRect(extraAug).contains(x, y)) {
        const auto& item = catalog_.equipment(overflow);
        if (kind == "cargo" && item.kind != "aug") {
            std::swap(cargo_[source], overflow);
            std::swap(cargoFactories_.at(source), overflowFactory_);
        } else if (kind == "augment" && item.kind == "aug") {
            if (item.stacking ||
                std::ranges::find(ship_.augments, overflow) == ship_.augments.end()) {
                std::swap(ship_.augments[source], overflow);
            }
        } else if (item.kind == kind) {
            cargo_.push_back(overflow);
            cargoFactories_.push_back(overflowFactory_);
            if (inventorySwap(static_cast<int>(cargo_.size()) - 1, kind, source)) {
                overflow = cargo_.back();
                overflowFactory_ = std::move(cargoFactories_.back());
            }
            cargo_.pop_back();
            cargoFactories_.pop_back();
        }
        return;
    }
    for (const auto& destination : {"weapon", "drone", "augment", "cargo"}) {
        auto* items = std::string_view(destination) == "weapon"    ? &ship_.weapons
                      : std::string_view(destination) == "drone"   ? &ship_.drones
                      : std::string_view(destination) == "augment" ? &ship_.augments
                                                                   : &cargo_;
        const int limit = std::string_view(destination) == "weapon"    ? ship_.weaponSlots
                          : std::string_view(destination) == "drone"   ? ship_.droneSlots
                          : std::string_view(destination) == "augment" ? 3
                                                                       : 4;
        for (int i = 0; i < limit; ++i) {
            const float sx = std::string_view(destination) == "cargo"
                                 ? 364.f + offset + 130.f * (i % 2)
                             : std::string_view(destination) == "augment"
                                 ? 664.f + offset
                                 : 397.f + offset + 58.f * (4 - limit) + 117.f * i;
            const float sy = std::string_view(destination) == "cargo"    ? 378.f + 80.f * (i / 2)
                             : std::string_view(destination) == "weapon" ? 155.f
                             : std::string_view(destination) == "drone"  ? 265.f
                                                                         : 378.f + 60.f * i;
            if (!Rect{sx,
                      sy,
                      std::string_view(destination) == "augment" ? 250.f : 109.f,
                      std::string_view(destination) == "augment" ? 55.f : 76.f}
                     .contains(x, y)) {
                continue;
            }
            const bool armament = std::string_view(destination) == "weapon" ||
                                  std::string_view(destination) == "drone";
            const int dense = armament ? logicalArmamentIndex(destination, i)
                                       : (i < static_cast<int>(items->size()) ? i : -1);
            if (kind == destination) {
                if (armament) {
                    if (kind == "weapon") {
                        if (source < static_cast<int>(weaponPower_.size()) &&
                            weaponPower_[source]) {
                            weaponPower(source, false);
                        }
                        if (dense >= 0 && weaponPower_[dense]) {
                            weaponPower(dense, false);
                        }
                        cancelAim();
                    } else if (combatDrones_[source].deployed ||
                               (dense >= 0 && combatDrones_[dense].deployed)) {
                        return;
                    }
                    if (dense >= 0) {
                        swapArmaments(source, dense, kind == "drone");
                    } else {
                        for (int index = source; index + 1 < static_cast<int>(items->size());
                             ++index) {
                            swapArmaments(index, index + 1, kind == "drone");
                        }
                    }
                } else if (source < static_cast<int>(items->size()) && dense >= 0 &&
                           dense != source) {
                    std::swap((*items)[source], (*items)[dense]);
                    if (kind == "cargo") {
                        std::swap(cargoFactories_.at(source), cargoFactories_.at(dense));
                    }
                }
            } else if (kind == "cargo") {
                if (dense >= 0) {
                    inventorySwap(source, destination, dense);
                } else if (catalog_.equipment(cargo_[source]).kind == destination) {
                    inventoryMove({}, source, true);
                }
            } else if (std::string_view(destination) == "cargo" && kind != "augment") {
                if (dense >= 0) {
                    inventorySwap(dense, kind, source);
                } else {
                    inventoryMove(kind, source, false);
                }
            }
            return;
        }
    }
}

void Run::ensureInventorySlots() {
    const auto normalize = [](std::vector<int>& slots, int count, std::size_t size) {
        slots.assign(std::size_t(count), -1);
        for (std::size_t i = 0; i < size && i < slots.size(); ++i) {
            slots[i] = static_cast<int>(i);
        }
    };
    normalize(weaponSlots_, ship_.weaponSlots, ship_.weapons.size());
    normalize(droneSlots_, ship_.droneSlots, ship_.drones.size());
}

int Run::logicalArmamentIndex(const std::string& kind, int slot) const {
    const auto& items = kind == "weapon" ? ship_.weapons : ship_.drones;
    return slot >= 0 && slot < static_cast<int>(items.size()) ? slot : -1;
}

int Run::logicalArmamentSlot(const std::string& kind, int denseIndex) const {
    const auto& items = kind == "weapon" ? ship_.weapons : ship_.drones;
    return denseIndex >= 0 && denseIndex < static_cast<int>(items.size()) ? denseIndex : -1;
}

void Run::eraseInventorySlot(const std::string& kind, int denseIndex) {
    if (kind != "weapon" && kind != "drone") {
        return;
    }
    ensureInventorySlots();
    auto& mapping = kind == "weapon" ? weaponSlots_ : droneSlots_;
    const auto& items = kind == "weapon" ? ship_.weapons : ship_.drones;
    const int remaining = static_cast<int>(items.size()) - 1;
    for (std::size_t index = 0; index < mapping.size(); ++index) {
        mapping[index] = static_cast<int>(index) < remaining ? static_cast<int>(index) : -1;
    }
    (void)denseIndex;
}
} // namespace ftl
