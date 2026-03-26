#include "ScriptMgr.h"
#include "Player.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "Log.h"
#include "Creature.h"
#include "Chat.h"
#include "World.h"
#include "WorldSession.h"
#include "Unit.h"
#include "Map.h"
#include "LootMgr.h"
#include "GameObject.h"
#include <vector>
#include <mutex>
#include <algorithm>
#include <cmath>
#include <unordered_map>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

enum HotSpotSpells
{
    SPELL_HOTSPOT_XP = 61782
};

enum HotSpotType : uint8
{
    HOTSPOT_TYPE_INVASION = 1,
    HOTSPOT_TYPE_MINING   = 2,
    HOTSPOT_TYPE_HERB     = 3
};

struct HotSpotData
{
    uint32 id;
    uint32 map_id;
    float x, y, z, radius;
    float xp_mult;
    float respawn_mult;
    uint8 type;
    uint32 creature_entry;
    uint32 max_population;
    uint32 gameobject_entry; // Usado en tipos 2 (minería) y 3 (herboristería). 0 = auto según nivel.
};

class HotSpotMgr
{
public:
    static HotSpotMgr* instance() { static HotSpotMgr instance; return &instance; }

    void LoadFromDB()
    {
        std::lock_guard<std::mutex> lock(_mutex);
        hotspots.clear();

        QueryResult result = WorldDatabase.Query(
            "SELECT id, map_id, x, y, z, radius, xp_multiplier, respawn_multiplier, type, "
            "creature_entry, max_population, gameobject_entry "
            "FROM mod_hotspot WHERE active = 1");
        if (!result) return;

        do {
            Field* fields = result->Fetch();
            hotspots.push_back({
                fields[0].Get<uint32>(),  // id
                fields[1].Get<uint32>(),  // map_id
                fields[2].Get<float>(),   // x
                fields[3].Get<float>(),   // y
                fields[4].Get<float>(),   // z
                fields[5].Get<float>(),   // radius
                fields[6].Get<float>(),   // xp_mult
                fields[7].Get<float>(),   // respawn_mult
                fields[8].Get<uint8>(),   // type
                fields[9].Get<uint32>(),  // creature_entry
                fields[10].Get<uint32>(), // max_population
                fields[11].Get<uint32>()  // gameobject_entry
            });
        } while (result->NextRow());

        LOG_INFO("module", "HotSpotMgr: {} spots cargados.", (uint32)hotspots.size());
    }

    bool GetHotSpotAt(uint32 mapId, float x, float y, float z, HotSpotData& data, uint8 type = 0) const
    {
        std::lock_guard<std::mutex> lock(_mutex);
        for (const auto& spot : hotspots)
        {
            if (spot.map_id == mapId && (type == 0 || spot.type == type))
            {
                float dx = spot.x - x, dy = spot.y - y, dz = spot.z - z;
                if ((dx*dx + dy*dy + dz*dz) <= spot.radius * spot.radius)
                {
                    data = spot;
                    return true;
                }
            }
        }
        return false;
    }

    void RemoveFromMemory(uint32 id)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        hotspots.erase(std::remove_if(hotspots.begin(), hotspots.end(),
            [id](const HotSpotData& s) { return s.id == id; }), hotspots.end());
    }

private:
    std::vector<HotSpotData> hotspots;
    mutable std::mutex _mutex;
};

// -----------------------------------------------------------------------
// Helpers de entradas de criaturas / objetos según nivel del jugador
// -----------------------------------------------------------------------

// Tipo 1 – Invasión: No-muertos escalados al nivel
uint32 GetUndeadEntryForLevel(uint8 level)
{
    if (level <= 10) return 1530;   // Esqueleto
    if (level <= 20) return 1563;   // Necrófago débil
    if (level <= 30) return 1783;   // Guerrero esquelético
    if (level <= 40) return 1912;   // Necrófago de plaga
    if (level <= 50) return 2420;   // Cadáver putrefacto
    if (level <= 60) return 4416;   // Esqueleto Forjatiniebla
    if (level <= 70) return 16244;  // Ansia resucitada
    return 25697;                   // Bestia de la Plaga (80)
}

// Tipo 2 – Minería: nodos de mineral según nivel
// NOTA: verificar las entradas de GO contra tu base de datos world si alguna no aparece.
uint32 GetMiningEntryForLevel(uint8 level)
{
    if (level <= 20) return 1731;   // Veta de cobre
    if (level <= 30) return 2055;   // Veta de estaño
    if (level <= 40) return 1735;   // Yacimiento de hierro
    if (level <= 50) return 2040;   // Yacimiento de mitril
    if (level <= 60) return 324;    // Veta de torio
    if (level <= 70) return 181248; // Yacimiento de hierro vil (Terrallende)
    return 189979;                  // Yacimiento de saronita (Rasganorte)
}

// Tipo 3 – Herboristería: plantas según nivel
// Entries verificados contra gameobject_template de AzerothCore 3.3.5a.
uint32 GetHerbEntryForLevel(uint8 level)
{
    if (level <= 15) return 1617;   // Silverleaf          (skill 1)
    if (level <= 25) return 1619;   // Earthroot           (skill 15)
    if (level <= 35) return 1621;   // Briarthorn          (skill 70)
    if (level <= 45) return 1622;   // Bruiseweed          (skill 100)
    if (level <= 55) return 2046;   // Goldthorn           (skill 170)
    if (level <= 60) return 176584; // Dreamfoil           (skill 270)
    if (level <= 70) return 181270; // Felweed (Terrallende, skill 300)
    return 190169;                  // Tiger Lily (Rasganorte, skill 375)
}

// -----------------------------------------------------------------------
// Comandos
// -----------------------------------------------------------------------
using namespace Acore::ChatCommands;

class HotSpot_CommandScript : public CommandScript
{
public:
    HotSpot_CommandScript() : CommandScript("HotSpot_CommandScript") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable hotSpotCommandTable = {
            { "add",    HandleHotSpotAddCommand,    SEC_ADMINISTRATOR, Console::No },
            { "delete", HandleHotSpotDeleteCommand, SEC_ADMINISTRATOR, Console::No },
            { "reload", HandleHotSpotReloadCommand, SEC_ADMINISTRATOR, Console::No }
        };
        static ChatCommandTable commandTable = { { "hotspot", hotSpotCommandTable } };
        return commandTable;
    }

    static bool HandleHotSpotReloadCommand(ChatHandler* handler)
    {
        HotSpotMgr::instance()->LoadFromDB();
        handler->SendSysMessage("Hot Spots recargados.");
        return true;
    }

    // Uso: .hotspot add <tipo> <poblacion>
    //   tipo 1 = Invasión (mobs no-muertos)
    //   tipo 2 = Minería  (nodos de mineral)
    //   tipo 3 = Herboristería (plantas)
    static bool HandleHotSpotAddCommand(ChatHandler* handler, uint32 type, uint32 population)
    {
        Player* player = handler->GetSession()->GetPlayer();
        if (!player) return false;

        if (type < 1 || type > 3)
        {
            handler->SendSysMessage("Tipo inválido. Usa: 1=Invasión, 2=Minería, 3=Herboristería");
            return false;
        }

        float radius = 50.0f;
        WorldDatabase.DirectExecute(Acore::StringFormat(
            "INSERT INTO mod_hotspot "
            "(map_id, x, y, z, radius, xp_multiplier, respawn_multiplier, type, active, creature_entry, max_population, gameobject_entry) "
            "VALUES ({}, {}, {}, {}, {}, 3.0, 0.05, {}, 1, 0, {}, 0)",
            player->GetMapId(),
            player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(),
            radius, type, population));

        HotSpotMgr::instance()->LoadFromDB();

        const char* typeNames[] = { "", "Invasión", "Minería", "Herboristería" };
        handler->PSendSysMessage("Hot Spot de {} creado (radio {}, máx {} objetos).",
            typeNames[type], (uint32)radius, population);
        return true;
    }

    static bool HandleHotSpotDeleteCommand(ChatHandler* handler)
    {
        Player* player = handler->GetSession()->GetPlayer();
        HotSpotData spot;
        if (HotSpotMgr::instance()->GetHotSpotAt(
                player->GetMapId(),
                player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(), spot))
        {
            WorldDatabase.DirectExecute(Acore::StringFormat(
                "DELETE FROM mod_hotspot WHERE id = {}", spot.id));
            HotSpotMgr::instance()->RemoveFromMemory(spot.id);
            handler->PSendSysMessage("Hot Spot (ID: {}) eliminado.", spot.id);
            return true;
        }
        handler->SendSysMessage("No hay ningún Hot Spot activo en tu posición.");
        return false;
    }
};

// -----------------------------------------------------------------------
// Script de jugador – lógica principal de los tres tipos
// -----------------------------------------------------------------------
class HotSpotPlayerScript : public PlayerScript
{
public:
    HotSpotPlayerScript() : PlayerScript("HotSpotPlayerScript") {}

    // Liberar timers cuando el jugador desconecta
    void OnPlayerLogout(Player* player) override
    {
        std::lock_guard<std::mutex> lock(_timerMutex);
        uint32 key = player->GetGUID().GetCounter();
        _mobSpawnTimers.erase(key);
        _goSpawnTimers.erase(key);
    }

    void OnPlayerUpdate(Player* player, uint32 diff) override
    {
        if (!sConfigMgr->GetOption<bool>("HotSpot.Enable", true) ||
            !player->IsAlive() || player->IsGameMaster())
            return;

        uint32 key   = player->GetGUID().GetCounter();
        uint32 mapId = player->GetMapId();
        float px = player->GetPositionX();
        float py = player->GetPositionY();
        float pz = player->GetPositionZ();

        // ===================================================================
        // TIPO 1 – INVASIÓN: spawnea mobs no-muertos y multiplica XP
        // ===================================================================
        HotSpotData spot;
        if (HotSpotMgr::instance()->GetHotSpotAt(mapId, px, py, pz, spot, HOTSPOT_TYPE_INVASION))
        {
            // Notificar al jugador la primera vez que entra
            if (!player->HasAura(SPELL_HOTSPOT_XP))
            {
                player->CastSpell(player, SPELL_HOTSPOT_XP, true);
                std::string msg = Acore::StringFormat(
                    "|cffffff00¡INVASIÓN DETECTADA!|r\n|cff00ff00Sobrevive y gana x{:.1f} XP.|r",
                    spot.xp_mult);
                WorldPacket data;
                ChatHandler::BuildChatPacket(data, CHAT_MSG_RAID_WARNING,
                    LANG_UNIVERSAL, nullptr, nullptr, msg);
                player->GetSession()->SendPacket(&data);
            }

            // Timer de spawn por jugador (no compartido)
            uint32 mobTimer;
            {
                std::lock_guard<std::mutex> lock(_timerMutex);
                mobTimer = _mobSpawnTimers[key]; // crea con 0 si no existe → spawn inmediato
            }

            if (mobTimer <= diff)
            {
                {
                    std::lock_guard<std::mutex> lock(_timerMutex);
                    _mobSpawnTimers[key] = 3000;
                }

                uint32 entry = spot.creature_entry
                    ? spot.creature_entry
                    : GetUndeadEntryForLevel(player->GetLevel());

                uint32 count = 0;
                std::list<Creature*> creatures;
                player->GetCreatureListWithEntryInGrid(creatures, entry, spot.radius);
                for (Creature* c : creatures)
                    if (c->IsAlive() && c->IsSummon()) count++;

                if (count < spot.max_population)
                {
                    // Spawnear desde el centro del hotspot para no salirse del radio
                    float angle  = rand_norm() * 2.0f * (float)M_PI;
                    float dist   = rand_norm() * spot.radius;
                    float spawnX = spot.x + dist * std::cos(angle);
                    float spawnY = spot.y + dist * std::sin(angle);
                    float spawnZ = player->GetMap()->GetHeight(
                        player->GetPhaseMask(), spawnX, spawnY, spot.z + 10.0f);

                    if (Creature* undead = player->SummonCreature(
                            entry, spawnX, spawnY, spawnZ, 0,
                            TEMPSUMMON_CORPSE_TIMED_DESPAWN, 60000))
                    {
                        uint8 level = player->GetLevel();
                        undead->SetLevel(level);

                        CreatureTemplate const* cInfo = undead->GetCreatureTemplate();
                        if (CreatureBaseStats const* stats =
                                sObjectMgr->GetCreatureBaseStats(level, cInfo->unit_class))
                        {
                            uint32 health = stats->GenerateHealth(cInfo);
                            undead->SetStatFlatModifier(UNIT_MOD_HEALTH, TOTAL_VALUE, (float)health);

                            if (uint32 mana = stats->GenerateMana(cInfo))
                                undead->SetStatFlatModifier(UNIT_MOD_MANA, TOTAL_VALUE, (float)mana);
                        }

                        undead->UpdateAllStats();
                        undead->SetFullHealth();
                        undead->SetInCombatWith(player);
                        if (undead->AI()) undead->AI()->AttackStart(player);
                    }
                }
            }
            else
            {
                std::lock_guard<std::mutex> lock(_timerMutex);
                _mobSpawnTimers[key] = mobTimer - diff;
            }
        }
        else if (player->HasAura(SPELL_HOTSPOT_XP))
        {
            {
                std::lock_guard<std::mutex> lock(_timerMutex);
                _mobSpawnTimers.erase(key);
            }
            player->RemoveAura(SPELL_HOTSPOT_XP);
            WorldPacket data;
            ChatHandler::BuildChatPacket(data, CHAT_MSG_RAID_WARNING, LANG_UNIVERSAL,
                nullptr, nullptr, "|cffff0000La invasión ha cesado.|r");
            player->GetSession()->SendPacket(&data);
        }

        // ===================================================================
        // TIPO 2 – MINERÍA / TIPO 3 – HERBORISTERÍA: spawnea GameObjects
        // ===================================================================
        HotSpotData spotGO;
        bool inGOSpot = HotSpotMgr::instance()->GetHotSpotAt(mapId, px, py, pz, spotGO, HOTSPOT_TYPE_MINING);
        if (!inGOSpot)
            inGOSpot = HotSpotMgr::instance()->GetHotSpotAt(mapId, px, py, pz, spotGO, HOTSPOT_TYPE_HERB);

        if (inGOSpot)
        {
            uint32 goTimer;
            {
                std::lock_guard<std::mutex> lock(_timerMutex);
                goTimer = _goSpawnTimers[key]; // 0 en primera entrada → spawn inmediato
            }

            if (goTimer <= diff)
            {
                {
                    std::lock_guard<std::mutex> lock(_timerMutex);
                    _goSpawnTimers[key] = 5000; // comprobar cada 5 s
                }

                uint32 goEntry = spotGO.gameobject_entry
                    ? spotGO.gameobject_entry
                    : (spotGO.type == HOTSPOT_TYPE_MINING
                        ? GetMiningEntryForLevel(player->GetLevel())
                        : GetHerbEntryForLevel(player->GetLevel()));

                // Contar nodos ya presentes en el área
                std::list<GameObject*> goList;
                player->GetGameObjectListWithEntryInGrid(goList, goEntry, spotGO.radius);

                LOG_DEBUG("module", "HotSpotMgr: tipo={} nivel={} goEntry={} presentes={}/{}",
                    spotGO.type, player->GetLevel(), goEntry,
                    (uint32)goList.size(), spotGO.max_population);

                if ((uint32)goList.size() < spotGO.max_population)
                {
                    // Spawnear en posición aleatoria dentro del hotspot (desde su centro)
                    float angle  = rand_norm() * 2.0f * (float)M_PI;
                    float dist   = rand_norm() * spotGO.radius;
                    float spawnX = spotGO.x + dist * std::cos(angle);
                    float spawnY = spotGO.y + dist * std::sin(angle);
                    float spawnZ = player->GetMap()->GetHeight(
                        player->GetPhaseMask(), spawnX, spawnY, spotGO.z + 10.0f);

                    // despawnTime en segundos (300 = 5 minutos si no se recolecta antes)
                    if (!player->SummonGameObject(
                            goEntry, spawnX, spawnY, spawnZ,
                            0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 300))
                    {
                        LOG_ERROR("module",
                            "HotSpotMgr: SummonGameObject falló para entry {}. "
                            "Verifica que exista en gameobject_template.", goEntry);
                    }
                }
            }
            else
            {
                std::lock_guard<std::mutex> lock(_timerMutex);
                _goSpawnTimers[key] = goTimer - diff;
            }
        }
        else
        {
            std::lock_guard<std::mutex> lock(_timerMutex);
            _goSpawnTimers.erase(key);
        }
    }

    void OnPlayerGiveXP(Player* player, uint32& amount, Unit*, uint8) override
    {
        if (player->HasAura(SPELL_HOTSPOT_XP))
        {
            HotSpotData spot;
            if (HotSpotMgr::instance()->GetHotSpotAt(
                    player->GetMapId(),
                    player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(),
                    spot, HOTSPOT_TYPE_INVASION))
                amount = uint32(amount * spot.xp_mult);
        }
    }

private:
    // Timers individuales por jugador (counter del GUID → ms restantes)
    // Protegidos con mutex porque OnPlayerUpdate puede ejecutarse desde
    // distintos hilos de mapa en paralelo.
    mutable std::mutex _timerMutex;
    std::unordered_map<uint32, uint32> _mobSpawnTimers; // tipo 1
    std::unordered_map<uint32, uint32> _goSpawnTimers;  // tipos 2 y 3
};

// -----------------------------------------------------------------------
// Loot dinámico para mobs de invasión (tipo 1)
// -----------------------------------------------------------------------
uint32 GetPotionEntryForLevel(uint8 level)
{
    if (level <= 10) return 118;
    if (level <= 20) return 858;
    if (level <= 30) return 929;
    if (level <= 40) return 1710;
    if (level <= 50) return 3928;
    if (level <= 60) return 13446;
    if (level <= 70) return 22829;
    return 33447;
}

uint32 GetClothEntryForLevel(uint8 level)
{
    if (level <= 10) return 2589;
    if (level <= 20) return 2592;
    if (level <= 30) return 4306;
    if (level <= 45) return 4338;
    if (level <= 55) return 14047;
    if (level <= 70) return 21877;
    return 33470;
}

class HotSpotUnitScript : public UnitScript
{
public:
    HotSpotUnitScript() : UnitScript("HotSpotUnitScript") {}

    void OnUnitDeath(Unit* unit, Unit* killer) override
    {
        Creature* creature = unit->ToCreature();
        if (!creature || !creature->IsSummon()) return;

        HotSpotData spot;
        if (!HotSpotMgr::instance()->GetHotSpotAt(
                creature->GetMapId(),
                creature->GetPositionX(), creature->GetPositionY(), creature->GetPositionZ(),
                spot, HOTSPOT_TYPE_INVASION))
            return;

        creature->SetCorpseDelay(3);

        Player* player = killer ? killer->ToPlayer() : nullptr;
        if (!player) return;

        uint8 level = player->GetLevel();
        creature->loot.clear();

        // Dinero escalado (nivel² × 15 cobres)
        creature->loot.gold = (level * level) * 15;

        // Pociones (30 % de probabilidad)
        if (urand(1, 100) <= 30)
            creature->loot.AddItem(LootStoreItem(GetPotionEntryForLevel(level), 0, 100.0f, false, 0, 0, 1, 1));

        // Paños (40 % de probabilidad, 1-3 unidades)
        if (urand(1, 100) <= 40)
            creature->loot.AddItem(LootStoreItem(GetClothEntryForLevel(level), 0, 100.0f, false, 0, 0, 1, 3));
    }
};

class HotSpotWorldScript : public WorldScript
{
public:
    HotSpotWorldScript() : WorldScript("HotSpotWorldScript") {}
    void OnAfterConfigLoad(bool) override { HotSpotMgr::instance()->LoadFromDB(); }
};

void AddHotSpotScripts()
{
    new HotSpot_CommandScript();
    new HotSpotPlayerScript();
    new HotSpotUnitScript();
    new HotSpotWorldScript();
}
