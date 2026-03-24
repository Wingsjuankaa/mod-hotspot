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
#include <vector>
#include <mutex>
#include <algorithm>
#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

enum HotSpotSpells
{
    SPELL_HOTSPOT_XP = 61782
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
};

class HotSpotMgr
{
public:
    static HotSpotMgr* instance() { static HotSpotMgr instance; return &instance; }

    void LoadFromDB()
    {
        std::lock_guard<std::mutex> lock(_mutex);
        hotspots.clear();

        QueryResult result = WorldDatabase.Query("SELECT id, map_id, x, y, z, radius, xp_multiplier, respawn_multiplier, type, creature_entry, max_population FROM mod_hotspot WHERE active = 1");
        if (!result) return;

        do {
            Field* fields = result->Fetch();
            hotspots.push_back({
                fields[0].Get<uint32>(),
                fields[1].Get<uint32>(),
                fields[2].Get<float>(),
                fields[3].Get<float>(),
                fields[4].Get<float>(),
                fields[5].Get<float>(),
                fields[6].Get<float>(),
                fields[7].Get<float>(),
                fields[8].Get<uint8>(),
                fields[9].Get<uint32>(),
                fields[10].Get<uint32>()
            });
        } while (result->NextRow());

        LOG_INFO("module", "HotSpotMgr: {} spots cargados.", (uint32)hotspots.size());
    }

    bool GetHotSpotAt(uint32 mapId, float x, float y, float z, HotSpotData& data, uint8 type = 0) const
    {
        std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(_mutex));
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
        hotspots.erase(std::remove_if(hotspots.begin(), hotspots.end(), [id](const HotSpotData& s) { return s.id == id; }), hotspots.end());
    }

private:
    std::vector<HotSpotData> hotspots;
    mutable std::mutex _mutex;
};

// Helper para elegir un No-muerto según el nivel
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

// --- COMANDOS ---
using namespace Acore::ChatCommands;

class HotSpot_CommandScript : public CommandScript
{
public:
    HotSpot_CommandScript() : CommandScript("HotSpot_CommandScript") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable hotSpotCommandTable = {
            { "add", HandleHotSpotAddCommand, SEC_ADMINISTRATOR, Console::No },
            { "delete", HandleHotSpotDeleteCommand, SEC_ADMINISTRATOR, Console::No },
            { "reload", HandleHotSpotReloadCommand, SEC_ADMINISTRATOR, Console::No }
        };
        static ChatCommandTable commandTable = { { "hotspot", hotSpotCommandTable } };
        return commandTable;
    }

    static bool HandleHotSpotReloadCommand(ChatHandler* handler) { HotSpotMgr::instance()->LoadFromDB(); handler->SendSysMessage("Hot Spots recargados."); return true; }

    static bool HandleHotSpotAddCommand(ChatHandler* handler, uint32 population)
    {
        Player* player = handler->GetSession()->GetPlayer();
        if (!player) return false;

        float radius = 50.0f;
        WorldDatabase.DirectExecute(Acore::StringFormat("INSERT INTO mod_hotspot (map_id, x, y, z, radius, xp_multiplier, respawn_multiplier, type, active, max_population) VALUES ({}, {}, {}, {}, {}, 3.0, 0.05, 1, 1, {})",
            player->GetMapId(), player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(), radius, population));

        HotSpotMgr::instance()->LoadFromDB();
        handler->PSendSysMessage("Hot Spot de invasión creado con {} mobs máximos.", population);
        return true;
    }

    static bool HandleHotSpotDeleteCommand(ChatHandler* handler)
    {
        Player* player = handler->GetSession()->GetPlayer();
        HotSpotData spot;
        if (HotSpotMgr::instance()->GetHotSpotAt(player->GetMapId(), player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(), spot))
        {
            WorldDatabase.DirectExecute(Acore::StringFormat("DELETE FROM mod_hotspot WHERE id = {}", spot.id));
            HotSpotMgr::instance()->RemoveFromMemory(spot.id);
            handler->PSendSysMessage("Hot Spot (ID: {}) eliminado.", spot.id);
            return true;
        }
        return false;
    }
};

// --- SCRIPTS ---
class HotSpotPlayerScript : public PlayerScript
{
public:
    HotSpotPlayerScript() : PlayerScript("HotSpotPlayerScript") {}

    // Temporizador para el spawn
    uint32 spawnTimer = 2000; 

    void OnPlayerUpdate(Player* player, uint32 diff) override
    {
        if (!sConfigMgr->GetOption<bool>("HotSpot.Enable", true) || !player->IsAlive() || player->IsGameMaster())
            return;

        HotSpotData spot;
        if (HotSpotMgr::instance()->GetHotSpotAt(player->GetMapId(), player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(), spot, 1))
        {
            if (!player->HasAura(SPELL_HOTSPOT_XP))
            {
                player->CastSpell(player, SPELL_HOTSPOT_XP, true);
                std::string msg = Acore::StringFormat("|cffffff00¡INVASIÓN DETECTADA!|r\n|cff00ff00Sobrevive y gana x{:.1f} XP.|r", spot.xp_mult);
                WorldPacket data;
                ChatHandler::BuildChatPacket(data, CHAT_MSG_RAID_WARNING, LANG_UNIVERSAL, nullptr, nullptr, msg);
                player->GetSession()->SendPacket(&data);
            }

            if (spawnTimer <= diff)
            {
                spawnTimer = 3000;

                uint32 count = 0;
                std::list<Creature*> creatures;
                player->GetCreatureListWithEntryInGrid(creatures, (spot.creature_entry ? spot.creature_entry : GetUndeadEntryForLevel(player->GetLevel())), spot.radius);
                
                for (Creature* c : creatures)
                    if (c->IsAlive() && c->IsSummon()) count++;

                if (count < spot.max_population)
                {
                    uint32 entry = spot.creature_entry ? spot.creature_entry : GetUndeadEntryForLevel(player->GetLevel());
                    
                    float angle = rand_norm() * 2 * (float)M_PI;
                    float dist = rand_norm() * spot.radius;
                    float spawnX = player->GetPositionX() + dist * std::cos(angle);
                    float spawnY = player->GetPositionY() + dist * std::sin(angle);
                    float spawnZ = player->GetMap()->GetHeight(player->GetPhaseMask(), spawnX, spawnY, player->GetPositionZ() + 10.0f);

                    // CORRECCIÓN: TEMPSUMMON_CORPSE_TIMED_DESPAWN para que el cuerpo no desaparezca al instante
                    if (Creature* undead = player->SummonCreature(entry, spawnX, spawnY, spawnZ, 0, TEMPSUMMON_CORPSE_TIMED_DESPAWN, 60000))
                    {
                        uint8 level = player->GetLevel();
                        undead->SetLevel(level);
                        
                        // Forzar estadísticas para el nivel del jugador
                        CreatureTemplate const* cInfo = undead->GetCreatureTemplate();
                        if (CreatureBaseStats const* stats = sObjectMgr->GetCreatureBaseStats(level, cInfo->unit_class))
                        {
                            uint32 health = stats->GenerateHealth(cInfo);
                            // Usamos TOTAL_VALUE para asegurar que sobreescriba cualquier otro cálculo
                            undead->SetStatFlatModifier(UNIT_MOD_HEALTH, TOTAL_VALUE, (float)health);
                            
                            if (uint32 mana = stats->GenerateMana(cInfo))
                            {
                                undead->SetStatFlatModifier(UNIT_MOD_MANA, TOTAL_VALUE, (float)mana);
                            }
                        }

                        undead->UpdateAllStats();
                        undead->SetFullHealth();
                        
                        // Importante para el crédito de la muerte: El invocador debe ser el dueño del "tap"
                        undead->SetInCombatWith(player);
                        if (undead->AI()) undead->AI()->AttackStart(player);
                    }
                }
            }
            else spawnTimer -= diff;
        }
        else if (player->HasAura(SPELL_HOTSPOT_XP))
        {
            player->RemoveAura(SPELL_HOTSPOT_XP);
            WorldPacket data;
            ChatHandler::BuildChatPacket(data, CHAT_MSG_RAID_WARNING, LANG_UNIVERSAL, nullptr, nullptr, "|cffff0000La invasión ha cesado.|r");
            player->GetSession()->SendPacket(&data);
        }
    }

    void OnPlayerGiveXP(Player* player, uint32& amount, Unit*, uint8) override
    {
        if (player->HasAura(SPELL_HOTSPOT_XP))
        {
            HotSpotData spot;
            if (HotSpotMgr::instance()->GetHotSpotAt(player->GetMapId(), player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(), spot, 1))
                amount = uint32(amount * spot.xp_mult);
        }
    }
};

class HotSpotUnitScript : public UnitScript
{
public:
    HotSpotUnitScript() : UnitScript("HotSpotUnitScript") {}

    void OnUnitDeath(Unit* unit, Unit*) override
    {
        Creature* creature = unit->ToCreature();
        if (!creature || !creature->IsSummon()) return;

        HotSpotData spot;
        if (HotSpotMgr::instance()->GetHotSpotAt(creature->GetMapId(), creature->GetPositionX(), creature->GetPositionY(), creature->GetPositionZ(), spot, 1))
            creature->SetCorpseDelay(3);
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
