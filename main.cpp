// Auto Deco - one-click procedural level decorator for the Geometry Dash editor.
//
// How it works
//   1. Reads your level and finds every solid block, slope and hazard.
//   2. Works out where the level has "surfaces": exposed block faces, outer
//      corners, space around hazards and empty background areas.
//   3. Fills those spots with decoration objects from a palette
//      (your currently selected deco objects, or IDs from the mod settings).
//
// Only objects whose type is "Decoration" are ever placed, so a wrong ID can
// never add portals, triggers or hazards to your level.

#include <Geode/Geode.hpp>
#include <Geode/modify/EditorPauseLayer.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <random>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

using namespace geode::prelude;

namespace {

    // ---- tweakables ------------------------------------------------------
    constexpr float kCell = 30.f;          // one block is 30 units
    constexpr float kPi = 3.14159265358979f;
    constexpr float kButtonInsetX = 80.f;  // distance of the buttons from the right screen edge
    constexpr float kButtonY = 62.f;       // height of the "Auto Deco" button (Undo sits below it)

    // ---- style presets ---------------------------------------------------
    struct StyleProfile {
        float faceChance;        // chance per exposed block face
        float cornerChance;      // chance per outer corner
        float hazardChance;      // chance per hazard (deco placed next to it)
        float backgroundChance;  // chance per background slot
        float minScale;
        float maxScale;
        float rotationJitter;    // degrees of random tilt when aligning to surfaces
        int maxPerCell;          // max deco objects per 30x30 cell
    };

    StyleProfile profileFor(std::string const& name) {
        if (name == "Clean") return { 0.15f, 0.45f, 0.30f, 0.08f, 0.50f, 0.90f, 8.f, 1 };
        if (name == "Chaotic") return { 0.60f, 1.00f, 0.90f, 0.50f, 0.30f, 1.50f, 180.f, 3 };
        return { 0.32f, 0.75f, 0.55f, 0.22f, 0.40f, 1.10f, 20.f, 2 }; // "Detailed"
    }

    // ---- grid helpers ----------------------------------------------------
    int cellOf(float v) {
        return static_cast<int>(std::floor(v / kCell));
    }

    int64_t cellKey(int cx, int cy) {
        return (static_cast<int64_t>(cx) << 32) | static_cast<uint32_t>(cy);
    }

    float cellCenter(int c) {
        return static_cast<float>(c) * kCell + kCell * 0.5f;
    }

    // GD rotation: 0 = up, 90 = right, 180 = down, 270 = left
    float faceAngle(float nx, float ny) {
        return std::atan2(nx, ny) * 180.f / kPi;
    }

    // If this doesn't compile against your bindings version, replace the body
    // with:  obj->setScale(s);
    void applyScale(GameObject* obj, float s) {
        obj->updateCustomScaleX(s);
        obj->updateCustomScaleY(s);
    }

    enum class AnchorKind { Face, Corner, Hazard, Background };

    struct Anchor {
        AnchorKind kind;
        float x, y;    // reference point (cell centre, or slot position for background)
        float nx, ny;  // outward direction
    };

    struct DecoResult {
        bool ok = false;
        std::string message;
    };

    // ---- palette ---------------------------------------------------------
    std::vector<int> parseIds(std::string const& text) {
        std::vector<int> ids;
        char const* p = text.c_str();
        while (*p) {
            char* end = nullptr;
            long v = std::strtol(p, &end, 10);
            if (end != p) {
                if (v > 0) ids.push_back(static_cast<int>(v));
                p = end;
            }
            else {
                ++p;
            }
        }
        return ids;
    }

    std::vector<int> buildPalette(LevelEditorLayer* lel, std::string& source) {
        std::vector<int> ids;
        auto add = [&](int id) {
            if (std::find(ids.begin(), ids.end(), id) == ids.end()) ids.push_back(id);
        };

        // 1) whatever deco the user currently has selected
        if (lel->m_editorUI) {
            if (CCArray* selected = lel->m_editorUI->getSelectedObjects()) {
                CCArrayExt<GameObject*> list = selected;
                for (auto* obj : list) {
                    if (obj && obj->m_objectType == GameObjectType::Decoration) {
                        add(obj->m_objectID);
                    }
                }
            }
        }
        if (!ids.empty()) {
            source = "your selected deco";
            return ids;
        }

        // 2) IDs typed into the mod settings (validated: decoration only)
        auto custom = parseIds(Mod::get()->getSettingValue<std::string>("palette-ids"));
        for (int id : custom) {
            GameObject* probe = GameObject::createWithKey(id);
            if (probe && probe->m_objectType == GameObjectType::Decoration) add(id);
        }
        if (!ids.empty()) source = "the Palette IDs setting";
        return ids;
    }

    // ---- the decorator ---------------------------------------------------
    DecoResult runAutoDeco(LevelEditorLayer* lel) {
        DecoResult res;
        if (!lel || !lel->m_objects) {
            res.message = "The editor isn't ready yet.";
            return res;
        }

        std::string paletteSource;
        std::vector<int> palette = buildPalette(lel, paletteSource);
        if (palette.empty()) {
            res.message =
                "I need a palette to decorate with.\n\n"
                "1. Close the pause menu\n"
                "2. Select a few DECORATION objects you like (gears, glows, particles...)\n"
                "3. Open the pause menu and press Auto Deco again\n\n"
                "You can also type object IDs into the mod's Palette IDs setting.";
            return res;
        }

        // settings
        Mod* mod = Mod::get();
        std::string const styleName = mod->getSettingValue<std::string>("style");
        float const density = static_cast<float>(mod->getSettingValue<int64_t>("density"));
        size_t const maxObjects = static_cast<size_t>(
            std::max<int64_t>(1, mod->getSettingValue<int64_t>("max-objects"))
        );
        int64_t const seedSetting = mod->getSettingValue<int64_t>("seed");
        bool const align = mod->getSettingValue<bool>("align-to-surfaces");
        bool const background = mod->getSettingValue<bool>("background-scatter");

        StyleProfile const prof = profileFor(styleName);
        float const dens = std::clamp(density / 50.f, 0.f, 2.f);

        uint32_t const seed = seedSetting != 0
            ? static_cast<uint32_t>(seedSetting)
            : static_cast<uint32_t>(std::random_device{}() % 999999u + 1u);
        std::mt19937 rng(seed);

        auto rnd = [&](float a, float b) {
            return std::uniform_real_distribution<float>(a, b)(rng);
        };
        auto chance = [&](float p) {
            return std::uniform_real_distribution<float>(0.f, 1.f)(rng) < std::min(1.f, p);
        };

        // 1) scan the level
        std::unordered_set<int64_t> solid, blocked;
        std::vector<std::pair<int, int>> solidCells, hazardCells;
        int minCx = std::numeric_limits<int>::max(), maxCx = std::numeric_limits<int>::min();
        int minCy = std::numeric_limits<int>::max(), maxCy = std::numeric_limits<int>::min();

        CCArrayExt<GameObject*> objects = lel->m_objects;
        for (auto* obj : objects) {
            if (!obj) continue;
            auto const type = obj->m_objectType;
            bool const isSolid = type == GameObjectType::Solid;
            bool const isHazard = type == GameObjectType::Hazard;
            bool const isSlope = type == GameObjectType::Slope;
            if (!isSolid && !isHazard && !isSlope) continue;

            CCPoint const pos = obj->getPosition();
            int const cx = cellOf(pos.x);
            int const cy = cellOf(pos.y);
            auto const k = cellKey(cx, cy);

            blocked.insert(k);
            if (isSolid && solid.insert(k).second) solidCells.emplace_back(cx, cy);
            if (isHazard) hazardCells.emplace_back(cx, cy);

            minCx = std::min(minCx, cx);
            maxCx = std::max(maxCx, cx);
            minCy = std::min(minCy, cy);
            maxCy = std::max(maxCy, cy);
        }

        if (blocked.empty()) {
            res.message = "I couldn't find any blocks or hazards to decorate around.\n"
                          "Build some of your level first, then press Auto Deco.";
            return res;
        }

        auto isBlocked = [&](int cx, int cy) { return blocked.count(cellKey(cx, cy)) > 0; };

        // 2) collect anchors
        std::vector<Anchor> anchors;
        anchors.reserve(solidCells.size() * 2 + hazardCells.size());

        constexpr float kDiag = 0.70710678f;
        for (auto const& [cx, cy] : solidCells) {
            float const x = cellCenter(cx);
            float const y = cellCenter(cy);
            bool const up = !isBlocked(cx, cy + 1);
            bool const down = !isBlocked(cx, cy - 1);
            bool const right = !isBlocked(cx + 1, cy);
            bool const left = !isBlocked(cx - 1, cy);

            if (up) anchors.push_back({ AnchorKind::Face, x, y, 0.f, 1.f });
            if (down) anchors.push_back({ AnchorKind::Face, x, y, 0.f, -1.f });
            if (right) anchors.push_back({ AnchorKind::Face, x, y, 1.f, 0.f });
            if (left) anchors.push_back({ AnchorKind::Face, x, y, -1.f, 0.f });

            if (up && right && !isBlocked(cx + 1, cy + 1)) anchors.push_back({ AnchorKind::Corner, x, y, kDiag, kDiag });
            if (up && left && !isBlocked(cx - 1, cy + 1)) anchors.push_back({ AnchorKind::Corner, x, y, -kDiag, kDiag });
            if (down && right && !isBlocked(cx + 1, cy - 1)) anchors.push_back({ AnchorKind::Corner, x, y, kDiag, -kDiag });
            if (down && left && !isBlocked(cx - 1, cy - 1)) anchors.push_back({ AnchorKind::Corner, x, y, -kDiag, -kDiag });
        }

        for (auto const& [cx, cy] : hazardCells) {
            anchors.push_back({ AnchorKind::Hazard, cellCenter(cx), cellCenter(cy), 0.f, 0.f });
        }

        if (background) {
            constexpr float kStep = 90.f;
            float const x0 = static_cast<float>(minCx) * kCell - 120.f;
            float const x1 = static_cast<float>(maxCx + 1) * kCell + 120.f;
            float const y0 = static_cast<float>(minCy) * kCell - 60.f;
            float const y1 = static_cast<float>(maxCy + 1) * kCell + 240.f;
            for (float x = x0; x <= x1; x += kStep) {
                for (float y = y0; y <= y1; y += kStep) {
                    anchors.push_back({ AnchorKind::Background, x, y, 0.f, 0.f });
                }
            }
        }

        // shuffle so the object cap doesn't favour one end of the level
        std::shuffle(anchors.begin(), anchors.end(), rng);

        // 3) place decoration
        std::unordered_map<int64_t, int> perCell;
        size_t placed = 0;
        std::string const tag = "autodeco-placed"_spr;

        auto tryPlace = [&](float x, float y, float rot, float scale) {
            if (placed >= maxObjects) return;
            int const cx = cellOf(x);
            int const cy = cellOf(y);
            auto const k = cellKey(cx, cy);
            if (blocked.count(k)) return;
            int& used = perCell[k];
            if (used >= prof.maxPerCell) return;

            int const id = palette[rng() % palette.size()];
            GameObject* obj = lel->createObject(id, CCPoint { x, y }, false);
            if (!obj) return;

            obj->setRotation(rot);
            applyScale(obj, scale);
            obj->setUserObject(tag, CCBool::create(true)); // lets "Undo Deco" find it later
            ++used;
            ++placed;
        };

        constexpr int kDx[4] = { 0, 0, 1, -1 };
        constexpr int kDy[4] = { 1, -1, 0, 0 };

        for (auto const& a : anchors) {
            if (placed >= maxObjects) break;

            switch (a.kind) {
                case AnchorKind::Face: {
                    if (!chance(prof.faceChance * dens)) break;
                    float const out = kCell * 0.5f + rnd(2.f, 14.f);
                    float const along = rnd(-12.f, 12.f);
                    float const x = a.x + a.nx * out - a.ny * along;
                    float const y = a.y + a.ny * out + a.nx * along;
                    float const rot = align
                        ? faceAngle(a.nx, a.ny) + rnd(-prof.rotationJitter, prof.rotationJitter)
                        : rnd(0.f, 360.f);
                    tryPlace(x, y, rot, rnd(prof.minScale, prof.maxScale));
                    break;
                }
                case AnchorKind::Corner: {
                    if (!chance(prof.cornerChance * dens)) break;
                    float const out = kCell * 0.7071f + rnd(2.f, 12.f);
                    float const x = a.x + a.nx * out;
                    float const y = a.y + a.ny * out;
                    float const rot = align
                        ? faceAngle(a.nx, a.ny) + rnd(-prof.rotationJitter, prof.rotationJitter)
                        : rnd(0.f, 360.f);
                    tryPlace(x, y, rot, rnd(prof.minScale, prof.maxScale) * 1.3f);
                    break;
                }
                case AnchorKind::Hazard: {
                    if (!chance(prof.hazardChance * dens)) break;
                    int const cx = cellOf(a.x);
                    int const cy = cellOf(a.y);
                    int const start = static_cast<int>(rng() % 4);
                    for (int i = 0; i < 4; ++i) {
                        int const d = (start + i) % 4;
                        if (isBlocked(cx + kDx[d], cy + kDy[d])) continue;
                        float const x = a.x + static_cast<float>(kDx[d]) * kCell + rnd(-8.f, 8.f);
                        float const y = a.y + static_cast<float>(kDy[d]) * kCell + rnd(-8.f, 8.f);
                        tryPlace(x, y, rnd(0.f, 360.f), rnd(prof.minScale, prof.maxScale) * 1.2f);
                        break;
                    }
                    break;
                }
                case AnchorKind::Background: {
                    if (!chance(prof.backgroundChance * dens)) break;
                    float const x = a.x + rnd(-35.f, 35.f);
                    float const y = a.y + rnd(-35.f, 35.f);
                    tryPlace(x, y, rnd(0.f, 360.f), rnd(prof.minScale, prof.maxScale) * 1.8f);
                    break;
                }
            }
        }

        res.ok = true;
        res.message =
            "Placed " + std::to_string(placed) + " decoration objects.\n"
            "Palette: " + std::to_string(palette.size()) + " object type(s) from " + paletteSource + "\n"
            "Style: " + styleName + "   Seed: " + std::to_string(seed) +
            (placed >= maxObjects ? "\n\nHit the Max Objects cap - raise it in settings for more." : "") +
            "\n\nNot happy? Press Undo Deco, then run it again (or change the style / seed).";
        return res;
    }

} // namespace

// ---- editor pause menu buttons ------------------------------------------
class $modify(AutoDecoPauseLayer, EditorPauseLayer) {
    bool init(LevelEditorLayer* editor) {
        if (!EditorPauseLayer::init(editor)) return false;

        auto const winSize = CCDirector::get()->getWinSize();

        auto menu = CCMenu::create();
        menu->setID("autodeco-menu"_spr);
        menu->setPosition({ 0.f, 0.f });

        auto decoSpr = ButtonSprite::create("Auto Deco");
        decoSpr->setScale(0.8f);
        auto decoBtn = CCMenuItemSpriteExtra::create(
            decoSpr, this, menu_selector(AutoDecoPauseLayer::onAutoDeco)
        );
        decoBtn->setPosition({ winSize.width - kButtonInsetX, kButtonY });
        menu->addChild(decoBtn);

        auto undoSpr = ButtonSprite::create("Undo Deco");
        undoSpr->setScale(0.7f);
        auto undoBtn = CCMenuItemSpriteExtra::create(
            undoSpr, this, menu_selector(AutoDecoPauseLayer::onUndoDeco)
        );
        undoBtn->setPosition({ winSize.width - kButtonInsetX, kButtonY - 34.f });
        menu->addChild(undoBtn);

        this->addChild(menu);
        return true;
    }

    void onAutoDeco(CCObject*) {
        DecoResult const result = runAutoDeco(LevelEditorLayer::get());
        FLAlertLayer::create("Auto Deco", result.message, "OK")->show();
    }

    void onUndoDeco(CCObject*) {
        auto lel = LevelEditorLayer::get();
        if (!lel || !lel->m_objects) return;

        std::string const tag = "autodeco-placed"_spr;
        std::vector<GameObject*> toRemove;
        CCArrayExt<GameObject*> objects = lel->m_objects;
        for (auto* obj : objects) {
            if (obj && obj->getUserObject(tag)) toRemove.push_back(obj);
        }
        for (auto* obj : toRemove) {
            lel->removeObject(obj, true);
        }

        FLAlertLayer::create(
            "Auto Deco",
            toRemove.empty()
                ? std::string("Nothing to undo. (Only deco placed this editor session can be undone.)")
                : "Removed " + std::to_string(toRemove.size()) + " decoration objects.",
            "OK"
        )->show();
    }
};
