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
#include <vector>
#include <mutex>
#include <algorithm>

// ID del Hechizo visible (Recluta a un Amigo / Triple XP)
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
};

class HotSpotMgr
{
public:
    static HotSpotMgr* instance() { static HotSpotMgr instance; return &instance; }

    void LoadFromDB()
    {
        std::lock_guard<std::mutex> lock(_mutex);
        hotspots.clear();

        QueryResult result = WorldDatabase.Query("SELECT id, map_id, x, y, z, radius, xp_multiplier, respawn_multiplier, type FROM mod_hotspot WHERE active = 1");
        if (!result)
        {
            LOG_INFO("module", "HotSpotMgr: No hay spots activos en la base de datos.");
            return;
        }

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
                fields[8].Get<uint8>()
            });
        } while (result->NextRow());

        LOG_INFO("module", "HotSpotMgr: {} spots cargados correctamente.", (uint32)hotspots.size());
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
        hotspots.erase(std::remove_if(hotspots.begin(), hotspots.end(), [id](const HotSpotData& s) {
            return s.id == id;
        }), hotspots.end());
    }

private:
    std::vector<HotSpotData> hotspots;
    mutable std::mutex _mutex;
};

// --- COMANDOS ---
using namespace Acore::ChatCommands;

class HotSpot_CommandScript : public CommandScript
{
public:
    HotSpot_CommandScript() : CommandScript("HotSpot_CommandScript") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable hotSpotCommandTable =
        {
            { "add", HandleHotSpotAddCommand, SEC_ADMINISTRATOR, Console::No },
            { "delete", HandleHotSpotDeleteCommand, SEC_ADMINISTRATOR, Console::No },
            { "reload", HandleHotSpotReloadCommand, SEC_ADMINISTRATOR, Console::No }
        };

        static ChatCommandTable commandTable = { { "hotspot", hotSpotCommandTable } };
        return commandTable;
    }

    static bool HandleHotSpotReloadCommand(ChatHandler* handler)
    {
        HotSpotMgr::instance()->LoadFromDB();
        handler->SendSysMessage("Hot Spots recargados desde la base de datos.");
        return true;
    }

    static bool HandleHotSpotAddCommand(ChatHandler* handler, uint8 type, float radius)
    {
        Player* player = handler->GetSession()->GetPlayer();
        if (!player) return false;

        uint32 mapId = player->GetMapId();
        float x = player->GetPositionX(), y = player->GetPositionY(), z = player->GetPositionZ();
        float xpMult = 3.0f;
        float respawnMult = 0.05f;

        WorldDatabase.DirectExecute(Acore::StringFormat("INSERT INTO mod_hotspot (map_id, x, y, z, radius, xp_multiplier, respawn_multiplier, type, active) VALUES ({}, {}, {}, {}, {}, {}, {}, {}, 1)",
            mapId, x, y, z, radius, xpMult, respawnMult, type));

        handler->PSendSysMessage("Hot Spot creado en la base de datos. Recargando...");
        HotSpotMgr::instance()->LoadFromDB();
        
        return true;
    }

    static bool HandleHotSpotDeleteCommand(ChatHandler* handler)
    {
        Player* player = handler->GetSession()->GetPlayer();
        if (!player) return false;

        HotSpotData spot;
        if (!HotSpotMgr::instance()->GetHotSpotAt(player->GetMapId(), player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(), spot))
        {
            handler->SendSysMessage("No se encontró ningún Hot Spot en tu ubicación actual.");
            return false;
        }

        WorldDatabase.DirectExecute(Acore::StringFormat("DELETE FROM mod_hotspot WHERE id = {}", spot.id));
        HotSpotMgr::instance()->RemoveFromMemory(spot.id);

        if (player->HasAura(SPELL_HOTSPOT_XP))
            player->RemoveAura(SPELL_HOTSPOT_XP);

        handler->PSendSysMessage("Hot Spot (ID: {}) eliminado de memoria y base de datos.", spot.id);
        return true;
    }
};

// --- SCRIPTS ---
class HotSpotPlayerScript : public PlayerScript
{
public:
    HotSpotPlayerScript() : PlayerScript("HotSpotPlayerScript") {}

    void OnPlayerUpdate(Player* player, uint32 /*diff*/) override
    {
        if (!sConfigMgr->GetOption<bool>("HotSpot.Enable", true) || !player->IsAlive())
            return;

        HotSpotData spot;
        if (HotSpotMgr::instance()->GetHotSpotAt(player->GetMapId(), player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(), spot, 1))
        {
            if (!player->HasAura(SPELL_HOTSPOT_XP))
            {
                player->CastSpell(player, SPELL_HOTSPOT_XP, true);
                
                // Mensaje visual impactante (Raid Warning style) con formato correcto de llaves
                std::string msg = Acore::StringFormat("|cffffff00¡HAS ENTRADO EN UN HOTSPOT!|r\n|cff00ff00Experiencia x{:.1f} y Respawn rápido activo.|r", spot.xp_mult);
                
                WorldPacket data;
                ChatHandler::BuildChatPacket(data, CHAT_MSG_RAID_WARNING, LANG_UNIVERSAL, nullptr, nullptr, msg);
                player->GetSession()->SendPacket(&data);

                ChatHandler(player->GetSession()).PSendSysMessage("¡Bono de Hot Spot activo! (Experiencia x%.1f)", spot.xp_mult);
            }
        }
        else
        {
            if (player->HasAura(SPELL_HOTSPOT_XP))
            {
                player->RemoveAura(SPELL_HOTSPOT_XP);
                
                // Mensaje de salida
                WorldPacket data;
                ChatHandler::BuildChatPacket(data, CHAT_MSG_RAID_WARNING, LANG_UNIVERSAL, nullptr, nullptr, "|cffff0000Has salido del HotSpot.|r");
                player->GetSession()->SendPacket(&data);
                
                ChatHandler(player->GetSession()).SendSysMessage("Has salido del Hot Spot.");
            }
        }
    }

    void OnPlayerGiveXP(Player* player, uint32& amount, Unit* victim, uint8) override
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
        if (!sConfigMgr->GetOption<bool>("HotSpot.Enable", true))
            return;

        Creature* creature = unit->ToCreature();
        if (!creature || creature->IsPet() || creature->IsSummon())
            return;

        HotSpotData spot;
        if (HotSpotMgr::instance()->GetHotSpotAt(creature->GetMapId(), creature->GetPositionX(), creature->GetPositionY(), creature->GetPositionZ(), spot, 1))
        {
            // Respawn en 1 segundo
            creature->SetRespawnTime(1);
            creature->SetCorpseDelay(3);
            creature->SaveRespawnTime();
        }
        else
        {
            // Restaurar CorpseDelay por defecto si ya no está en hotspot
            uint32 defaultDelay = 60;
            if (CreatureTemplate const* cinfo = creature->GetCreatureTemplate())
            {
                switch (cinfo->rank)
                {
                    case CREATURE_ELITE_RARE:      defaultDelay = sWorld->getIntConfig(CONFIG_CORPSE_DECAY_RARE); break;
                    case CREATURE_ELITE_ELITE:     defaultDelay = sWorld->getIntConfig(CONFIG_CORPSE_DECAY_ELITE); break;
                    case CREATURE_ELITE_RAREELITE: defaultDelay = sWorld->getIntConfig(CONFIG_CORPSE_DECAY_RAREELITE); break;
                    case CREATURE_ELITE_WORLDBOSS: defaultDelay = sWorld->getIntConfig(CONFIG_CORPSE_DECAY_WORLDBOSS); break;
                    default:                       defaultDelay = sWorld->getIntConfig(CONFIG_CORPSE_DECAY_NORMAL); break;
                }
            }
            creature->SetCorpseDelay(defaultDelay);
        }
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
