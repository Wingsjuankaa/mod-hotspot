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
#include "DBCStores.h"
#include "WorldSessionMgr.h"
#include "Formulas.h"
#include <vector>
#include <mutex>
#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <unordered_set>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

enum HotSpotSpells
{
    SPELL_HOTSPOT_XP      = 61782,  // Invasión: aura de XP bonus
    SPELL_HOTSPOT_MINING  = 61783,  // Minería: efecto visual de zona activa
    SPELL_HOTSPOT_HERB    = 61784   // Herboristería: efecto visual de zona activa
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
    uint32 zone_id;
    float x, y, z, radius;
    float xp_mult;
    float respawn_mult;
    uint8 type;
    uint32 creature_entry;
    uint32 max_population;
    uint32 gameobject_entry;
    std::vector<uint32> goVariants;
    bool active; // true = el spot está activo y spawnea contenido
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
            "SELECT id, map_id, zone_id, x, y, z, radius, xp_multiplier, "
            "respawn_multiplier, type, creature_entry, max_population, "
            "gameobject_entry, active "
            "FROM mod_hotspot");
        if (!result) return;

        // Listas de entries de GOs conocidos para scan de mapa.
        // Verificados contra gameobject_template de AzerothCore 3.3.5a.
        static const char* HERB_SCAN_LIST =
            "1617,1618,1619,1620,1621,1622,1623,1624,1628,"
            "2041,2042,2044,2045,2046,2866,"
            "3724,3725,3726,3727,3729,3730,"
            "142140,142142,142144,142145,"
            "176583,176584,176586,176588,"
            "176636,176637,176638,176639,176640,"
            "180164,180165,180166,180167,180168,"
            "181270,181271,181280,183044,183045,"
            "189973,190169,190170,190171,190176,191019";

        static const char* MINING_SCAN_LIST =
            "324,1731,1732,1733,1734,1735,"
            "2040,2047,2054,2055,3763,3764,"
            "103711,103713,105569,"
            "150079,150080,150081,150082,"
            "165658,175404,176643,176645,"
            "181108,181109,181248,181249,"
            "181555,181556,181557,"
            "189978,189979,189980,189981,191133";

        do {
            Field* fields = result->Fetch();

            HotSpotData data;
            data.id              = fields[0].Get<uint32>();
            data.map_id          = fields[1].Get<uint32>();
            data.zone_id         = fields[2].Get<uint32>();
            data.x               = fields[3].Get<float>();
            data.y               = fields[4].Get<float>();
            data.z               = fields[5].Get<float>();
            data.radius          = fields[6].Get<float>();
            data.xp_mult         = fields[7].Get<float>();
            data.respawn_mult    = fields[8].Get<float>();
            data.type            = fields[9].Get<uint8>();
            data.creature_entry  = fields[10].Get<uint32>();
            data.max_population  = fields[11].Get<uint32>();
            data.gameobject_entry = fields[12].Get<uint32>();
            data.active           = fields[13].Get<bool>();

            // Scan de zona: para todos los spots con entry automático (activos o no),
            // así un spot inactivo que se active en caliente ya tiene sus variantes listas.
            if (data.gameobject_entry == 0 &&
                (data.type == HOTSPOT_TYPE_MINING || data.type == HOTSPOT_TYPE_HERB))
            {
                const char* scanList = (data.type == HOTSPOT_TYPE_HERB)
                    ? HERB_SCAN_LIST : MINING_SCAN_LIST;

                QueryResult scan;
                if (data.zone_id > 0)
                {
                    // Scan exacto por zona (columna ZoneId de la tabla gameobject)
                    scan = WorldDatabase.Query(
                        Acore::StringFormat(
                            "SELECT DISTINCT id FROM gameobject "
                            "WHERE map = {} AND ZoneId = {} AND id IN ({})",
                            data.map_id, data.zone_id, scanList));
                }
                else
                {
                    // Fallback: bounding box de ±1500 unidades alrededor del spot
                    scan = WorldDatabase.Query(
                        Acore::StringFormat(
                            "SELECT DISTINCT id FROM gameobject "
                            "WHERE map = {} AND id IN ({}) "
                            "AND position_x BETWEEN {} AND {} "
                            "AND position_y BETWEEN {} AND {}",
                            data.map_id, scanList,
                            data.x - 1500.0f, data.x + 1500.0f,
                            data.y - 1500.0f, data.y + 1500.0f));
                }

                if (scan)
                {
                    do {
                        data.goVariants.push_back(
                            scan->Fetch()[0].Get<uint32>());
                    } while (scan->NextRow());
                }

                LOG_DEBUG("module",
                    "HotSpotMgr: spot {} (mapa {}, zona {}, tipo {}) – {} variante(s).",
                    data.id, data.map_id, data.zone_id, data.type,
                    (uint32)data.goVariants.size());
            }

            hotspots.push_back(std::move(data));
        } while (result->NextRow());

        LOG_INFO("module", "HotSpotMgr: {} spots cargados.", (uint32)hotspots.size());
    }

    bool GetHotSpotAt(uint32 mapId, float x, float y, float z, HotSpotData& data, uint8 type = 0) const
    {
        std::lock_guard<std::mutex> lock(_mutex);
        for (const auto& spot : hotspots)
        {
            if (spot.map_id == mapId && (type == 0 || spot.type == type) && spot.active)
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

    // Busca un spot por ID (para .hotspot go).
    bool GetById(uint32 id, HotSpotData& data) const
    {
        std::lock_guard<std::mutex> lock(_mutex);
        for (const auto& s : hotspots)
        {
            if (s.id == id)
            {
                data = s;
                return true;
            }
        }
        return false;
    }

    // Devuelve copia de todos los spots (para .hotspot list).
    std::vector<HotSpotData> GetAll() const
    {
        std::lock_guard<std::mutex> lock(_mutex);
        return hotspots;
    }

    // Devuelve copia de los spots de un mapa concreto (para .hotspot list zone).
    std::vector<HotSpotData> GetByMap(uint32 mapId) const
    {
        std::lock_guard<std::mutex> lock(_mutex);
        std::vector<HotSpotData> result;
        for (const auto& s : hotspots)
            if (s.map_id == mapId)
                result.push_back(s);
        return result;
    }

    // Activa o desactiva un spot en memoria (para .hotspot enable/disable).
    // Devuelve false si el ID no existe.
    bool SetActive(uint32 id, bool active)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        for (auto& s : hotspots)
        {
            if (s.id == id)
            {
                s.active = active;
                return true;
            }
        }
        return false;
    }

private:
    std::vector<HotSpotData> hotspots;
    mutable std::mutex _mutex;
};

// -----------------------------------------------------------------------
// Helpers de entradas de criaturas / objetos según nivel del jugador
// -----------------------------------------------------------------------

// Tipo 1 – Invasión: No-muertos escalados al nivel del jugador.
// Todos los entries verificados en creature_template: type=6 (undead), rank=0 (normal),
// faction=21 (hostil a todos, Alliance y Horde). El nivel del template no importa
// porque se sobreescribe con SetLevel() en el momento del spawn.
uint32 GetUndeadEntryForLevel(uint8 level)
{
    if (level <= 10) return 1530;   // Rotting Ancestor          (zombie básico, Tirisfal)
    if (level <= 20) return 570;    // Brain Eater               (ghoul, Duskwood)
    if (level <= 30) return 1270;   // Fetid Corpse              (zombie putrefacto)
    if (level <= 40) return 8537;   // Interloper                (no-muerto de Plaguelands)
    if (level <= 50) return 4474;   // Rotting Cadaver           (cadáver, Plaguelands)
    if (level <= 60) return 8523;   // Scourge Soldier           (soldado de la Plaga)
    if (level <= 70) return 25463;  // Soldier of the Frozen Wastes (Scourge, Rasganorte)
    return 26515;                   // Carrion Ghoul             (ghoul, Rasganorte)
}

// Tipo 1 – Invasión: salud escalada por fórmula cuadrática propia.
// Desacopla la salud del ModHealth de cada creature_template para evitar
// saltos bruscos al cambiar de entry entre rangos de nivel.
// Valores de referencia: nivel 10 ~ 250 HP | nivel 60 ~ 6000 HP | nivel 80 ~ 10400 HP
uint32 GetInvasionHealthForLevel(uint8 level)
{
    uint32 lvl = static_cast<uint32>(level);
    return 10 * lvl + (3 * lvl * lvl) / 2;
}

// Tipo 2 – Minería: nodos de mineral según nivel
// Entries verificados contra gameobject_template de AzerothCore 3.3.5a.
// Solo se usa como fallback cuando el scan de mapa no da resultados.
uint32 GetMiningEntryForLevel(uint8 level)
{
    if (level <= 20) return 1731;   // Copper Vein         (skill 1)
    if (level <= 30) return 1732;   // Tin Vein            (skill 65)
    if (level <= 40) return 1735;   // Iron Deposit        (skill 125)
    if (level <= 50) return 2040;   // Mithril Deposit     (skill 175)
    if (level <= 60) return 324;    // Small Thorium Vein  (skill 230)
    if (level <= 70) return 181555; // Fel Iron Deposit    (skill 300, Terrallende)
    return 189980;                  // Saronite Deposit    (skill 400, Rasganorte)
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

    // ── helpers ──────────────────────────────────────────────────────────

    // Columnas de color por tipo
    static const char* TypeColor(uint8 type)
    {
        switch (type)
        {
            case HOTSPOT_TYPE_INVASION: return "|cffff5555";  // rojo
            case HOTSPOT_TYPE_MINING:   return "|cffaabbff";  // azul claro
            case HOTSPOT_TYPE_HERB:     return "|cff55ff88";  // verde
            default:                    return "|cffffffff";
        }
    }

    static const char* TypeLabel(uint8 type)
    {
        switch (type)
        {
            case HOTSPOT_TYPE_INVASION: return "Invasión";
            case HOTSPOT_TYPE_MINING:   return "Minería ";
            case HOTSPOT_TYPE_HERB:     return "Hierba  ";
            default:                    return "?       ";
        }
    }

    static void PrintSpotList(ChatHandler* handler,
                              const std::vector<HotSpotData>& spots,
                              bool showMap)
    {
        for (const auto& s : spots)
        {
            std::string entryInfo;
            if (s.gameobject_entry)
                entryInfo = Acore::StringFormat(
                    "|cffffaa00[GO:{} fijo]|r", s.gameobject_entry);
            else if (!s.goVariants.empty())
                entryInfo = Acore::StringFormat(
                    "|cff44ffff[{} var(s)]|r", (uint32)s.goVariants.size());
            else
                entryInfo = "|cffff8800[fallback nivel]|r";

            // Indicador de estado: verde ON / rojo OFF
            const char* stateStr = s.active
                ? "|cff00ff00[ON] |r"
                : "|cffff4444[OFF]|r";

            std::string line;
            if (showMap)
                line = Acore::StringFormat(
                    " {}  {}[{}]|r  {}{} |r"
                    "  Mapa:|cff00ccff{}|r Zona:|cff00ccff{}|r"
                    "  |cffffffff({:.0f},{:.0f},{:.0f})|r"
                    "  R:{}{:.0f}|r  Pop:{}{} |r  {}",
                    stateStr,
                    TypeColor(s.type), s.id,
                    TypeColor(s.type), TypeLabel(s.type),
                    s.map_id, s.zone_id,
                    s.x, s.y, s.z,
                    "|cffaaaaaa", s.radius,
                    "|cffff8800", s.max_population,
                    entryInfo);
            else
                line = Acore::StringFormat(
                    " {}  {}[{}]|r  {}{} |r"
                    "  Zona:|cff00ccff{}|r"
                    "  |cffffffff({:.0f},{:.0f},{:.0f})|r"
                    "  R:{}{:.0f}|r  Pop:{}{} |r  {}",
                    stateStr,
                    TypeColor(s.type), s.id,
                    TypeColor(s.type), TypeLabel(s.type),
                    s.zone_id,
                    s.x, s.y, s.z,
                    "|cffaaaaaa", s.radius,
                    "|cffff8800", s.max_population,
                    entryInfo);

            handler->SendSysMessage(line);
        }
    }

    // ─────────────────────────────────────────────────────────────────────

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable hotSpotListCommandTable = {
            { "zone", HandleHotSpotListZoneCommand, SEC_ADMINISTRATOR, Console::No  },
            { "",     HandleHotSpotListAllCommand,  SEC_ADMINISTRATOR, Console::Yes },
        };
        static ChatCommandTable hotSpotCommandTable = {
            { "add",     HandleHotSpotAddCommand,     SEC_ADMINISTRATOR, Console::No  },
            { "delete",  HandleHotSpotDeleteCommand,  SEC_ADMINISTRATOR, Console::No  },
            { "enable",  HandleHotSpotEnableCommand,  SEC_ADMINISTRATOR, Console::No  },
            { "disable", HandleHotSpotDisableCommand, SEC_ADMINISTRATOR, Console::No  },
            { "reload",  HandleHotSpotReloadCommand,  SEC_ADMINISTRATOR, Console::No  },
            { "go",      HandleHotSpotGoCommand,      SEC_ADMINISTRATOR, Console::No  },
            { "list",    hotSpotListCommandTable                                       },
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

    // .hotspot go <id>  ─ teleporta al centro del hotspot indicado
    static bool HandleHotSpotGoCommand(ChatHandler* handler, uint32 id)
    {
        Player* player = handler->GetSession()->GetPlayer();

        HotSpotData spot;
        if (!HotSpotMgr::instance()->GetById(id, spot))
        {
            handler->PSendSysMessage(
                "|cffff4444No existe ningún Hot Spot con ID {}.|r", id);
            return false;
        }

        player->TeleportTo(spot.map_id, spot.x, spot.y, spot.z, 0.0f);

        handler->PSendSysMessage(
            "|cff00ff00Teletransportando al {} [ID:{}]"
            " – Mapa:|cff00ccff{}|r Zona:|cff00ccff{}|r"
            " |cffffffff({:.0f},{:.0f},{:.0f})|r",
            TypeLabel(spot.type), spot.id,
            spot.map_id, spot.zone_id,
            spot.x, spot.y, spot.z);

        return true;
    }

    // .hotspot list  ─ muestra todos los hotspots del mundo
    static bool HandleHotSpotListAllCommand(ChatHandler* handler)
    {
        auto spots = HotSpotMgr::instance()->GetAll();

        if (spots.empty())
        {
            handler->SendSysMessage("|cffff4444No hay Hot Spots configurados.|r");
            return true;
        }

        handler->PSendSysMessage(
            "|cffffff00=== Hot Spots (todos) – {} registro(s) ===|r",
            (uint32)spots.size());

        PrintSpotList(handler, spots, /*showMap=*/true);
        return true;
    }

    // .hotspot list zone  ─ muestra sólo los hotspots del mapa actual
    static bool HandleHotSpotListZoneCommand(ChatHandler* handler)
    {
        Player* player = handler->GetSession()->GetPlayer();
        uint32 mapId  = player->GetMapId();
        uint32 zoneId = player->GetZoneId();

        auto spots = HotSpotMgr::instance()->GetByMap(mapId);

        if (spots.empty())
        {
            handler->PSendSysMessage(
                "|cffff4444No hay Hot Spots en el mapa {} (zona {}).|r",
                mapId, zoneId);
            return true;
        }

        handler->PSendSysMessage(
            "|cffffff00=== Hot Spots – Mapa:{} Zona:{} – {} registro(s) ===|r",
            mapId, zoneId, (uint32)spots.size());

        PrintSpotList(handler, spots, /*showMap=*/false);
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
            "(map_id, zone_id, x, y, z, radius, xp_multiplier, respawn_multiplier, "
            "type, active, creature_entry, max_population, gameobject_entry) "
            "VALUES ({}, {}, {}, {}, {}, {}, 3.0, 0.05, {}, 1, 0, {}, 0)",
            player->GetMapId(),
            player->GetZoneId(),
            player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(),
            radius, type, population));

        HotSpotMgr::instance()->LoadFromDB();

        const char* typeNames[] = { "", "Invasión", "Minería", "Herboristería" };
        handler->PSendSysMessage("Hot Spot de {} creado (radio {}, máx {} objetos).",
            typeNames[type], (uint32)radius, population);
        return true;
    }

    // ── helpers de anuncios mundiales ────────────────────────────────────

    // Devuelve el nombre de zona del DBC priorizando español.
    // Orden: esES (6) → esMX (7) → locale del servidor → enUS (0).
    // Así funciona correctamente aunque DBC.Locale esté en enUS.
    static std::string GetZoneName(uint32 zoneId)
    {
        if (zoneId == 0)
            return "tierras desconocidas";

        AreaTableEntry const* area = sAreaTableStore.LookupEntry(zoneId);
        if (!area)
            return Acore::StringFormat("zona {}", zoneId);

        uint32 serverLocale = sWorld->GetDefaultDbcLocale();
        uint32 checkOrder[] = { LOCALE_esES, LOCALE_esMX, serverLocale, LOCALE_enUS };

        for (uint32 locale : checkOrder)
        {
            if (locale < MAX_LOCALES &&
                area->area_name[locale] && *area->area_name[locale])
                return area->area_name[locale];
        }

        return Acore::StringFormat("zona {}", zoneId);
    }

    // Envía un mensaje de chat de sistema a TODOS los jugadores conectados.
    static void BroadcastWorldAnnouncement(const std::string& msg)
    {
        WorldPacket data;
        ChatHandler::BuildChatPacket(data, CHAT_MSG_SYSTEM, LANG_UNIVERSAL,
            nullptr, nullptr, msg);
        sWorldSessionMgr->SendGlobalMessage(&data);
    }

    // .hotspot enable <id>  ─ activa un spot por ID (persiste en BD)
    static bool HandleHotSpotEnableCommand(ChatHandler* handler, uint32 id)
    {
        HotSpotData spot;
        if (!HotSpotMgr::instance()->GetById(id, spot))
        {
            handler->PSendSysMessage(
                "|cffff4444No existe ningún Hot Spot con ID {}.|r", id);
            return false;
        }

        if (spot.active)
        {
            handler->PSendSysMessage(
                "|cffffff00El Hot Spot [ID:{}] ya estaba activo.|r", id);
            return true;
        }

        WorldDatabase.DirectExecute(Acore::StringFormat(
            "UPDATE mod_hotspot SET active = 1 WHERE id = {}", id));
        HotSpotMgr::instance()->SetActive(id, true);

        // Anuncio mundial según tipo de spot
        std::string zoneName = GetZoneName(spot.zone_id);
        std::string announcement;
        if (spot.type == HOTSPOT_TYPE_INVASION)
            announcement = Acore::StringFormat(
                "|cffff4444[ALERTA]|r |cffff8800¡Se ha avistado una invasión"
                " atacando las tierras de {}!|r", zoneName);
        else if (spot.type == HOTSPOT_TYPE_MINING)
            announcement = Acore::StringFormat(
                "|cffffff00[AVISO]|r |cff00ccff¡Exploradores han encontrado"
                " una zona rica en Minerales en {}!|r", zoneName);
        else
            announcement = Acore::StringFormat(
                "|cffffff00[AVISO]|r |cff55ff88¡Exploradores han encontrado"
                " una zona rica en Hierbas en {}!|r", zoneName);
        BroadcastWorldAnnouncement(announcement);

        handler->PSendSysMessage(
            "|cff00ff00Hot Spot {} [ID:{}] activado.|r",
            TypeLabel(spot.type), id);
        return true;
    }

    // .hotspot disable <id>  ─ desactiva un spot por ID (persiste en BD)
    // Los jugadores que estén dentro perderán el aura y los timers en el
    // siguiente tick de OnPlayerUpdate, sin necesidad de reload.
    static bool HandleHotSpotDisableCommand(ChatHandler* handler, uint32 id)
    {
        HotSpotData spot;
        if (!HotSpotMgr::instance()->GetById(id, spot))
        {
            handler->PSendSysMessage(
                "|cffff4444No existe ningún Hot Spot con ID {}.|r", id);
            return false;
        }

        if (!spot.active)
        {
            handler->PSendSysMessage(
                "|cffffff00El Hot Spot [ID:{}] ya estaba inactivo.|r", id);
            return true;
        }

        WorldDatabase.DirectExecute(Acore::StringFormat(
            "UPDATE mod_hotspot SET active = 0 WHERE id = {}", id));
        HotSpotMgr::instance()->SetActive(id, false);

        // Anuncio mundial según tipo de spot
        std::string zoneName = GetZoneName(spot.zone_id);
        std::string announcement;
        if (spot.type == HOTSPOT_TYPE_INVASION)
            announcement = Acore::StringFormat(
                "|cff00ff00[NOTICIA]|r |cffaaffaa¡La invasión en {} ha sido repelida!|r",
                zoneName);
        else if (spot.type == HOTSPOT_TYPE_MINING)
            announcement = Acore::StringFormat(
                "|cffaaaaaa[AVISO]|r Los recursos minerales de {} se han agotado.|r",
                zoneName);
        else
            announcement = Acore::StringFormat(
                "|cffaaaaaa[AVISO]|r Las hierbas de {} han sido recolectadas.|r",
                zoneName);
        BroadcastWorldAnnouncement(announcement);

        handler->PSendSysMessage(
            "|cffff4444Hot Spot {} [ID:{}] desactivado.|r",
            TypeLabel(spot.type), id);
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

    // Envía un marcador SMSG_GOSSIP_POI al cliente para que aparezca
    // en el mapa mundial (al abrir M) mientras el jugador está en el spot.
    static void SendHotSpotMapMarker(Player* player, const HotSpotData& spot)
    {
        uint32 icon;
        std::string label;
        switch (spot.type)
        {
            case HOTSPOT_TYPE_INVASION:
                icon  = 7;
                label = "¡Zona de Invasión!";
                break;
            case HOTSPOT_TYPE_MINING:
                icon  = 7;
                label = "Zona de Minería";
                break;
            case HOTSPOT_TYPE_HERB:
                icon  = 7;
                label = "Zona de Herboristería";
                break;
            default:
                icon  = 7;
                label = "Hot Spot";
                break;
        }

        WorldPacket data(SMSG_GOSSIP_POI, 4 + 4 + 4 + 4 + 4 + label.size() + 1);
        data << uint32(6);       // Flags: 6 = marcador visible y persistente en el mapa
        data << float(spot.x);  // coordenada X del mundo
        data << float(spot.y);  // coordenada Y del mundo
        data << uint32(icon);   // icono (7 = punto amarillo, el más usado en POIs)
        data << uint32(0);      // importancia (tamaño del icono, 0 = normal)
        data << label;
        player->GetSession()->SendPacket(&data);
    }

    // Liberar timers cuando el jugador desconecta
    void OnPlayerLogout(Player* player) override
    {
        std::lock_guard<std::mutex> lock(_timerMutex);
        uint32 key = player->GetGUID().GetCounter();
        _mobSpawnTimers.erase(key);
        _goSpawnTimers.erase(key);
        _inGOSpot.erase(key);
        _inInvasionSpot.erase(key);
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
        bool wasInInvasionSpot;
        {
            std::lock_guard<std::mutex> lock(_timerMutex);
            wasInInvasionSpot = (_inInvasionSpot.find(key) != _inInvasionSpot.end());
        }

        if (HotSpotMgr::instance()->GetHotSpotAt(mapId, px, py, pz, spot, HOTSPOT_TYPE_INVASION))
        {
            // Notificar al jugador la primera vez que entra
            if (!wasInInvasionSpot)
            {
                {
                    std::lock_guard<std::mutex> lock(_timerMutex);
                    _inInvasionSpot.insert(key);
                }
                player->CastSpell(player, SPELL_HOTSPOT_XP, true);
                SendHotSpotMapMarker(player, spot);
                std::string msg = Acore::StringFormat(
                    "|cffffff00¡INVASIÓN DETECTADA!|r\n|cff00ff00Sobrevive y gana x{:.1f} XP.|r",
                    spot.xp_mult);
                WorldPacket data;
                ChatHandler::BuildChatPacket(data, CHAT_MSG_RAID_WARNING,
                    LANG_UNIVERSAL, nullptr, nullptr, msg);
                player->GetSession()->SendPacket(&data);
            }
            else if (!player->HasAura(SPELL_HOTSPOT_XP))
            {
                // El aura expiró mientras el jugador sigue dentro: re-aplicarla sin mostrar el mensaje
                player->CastSpell(player, SPELL_HOTSPOT_XP, true);
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

                        // Mana: se saca del template (no afecta la dificultad percibida)
                        CreatureTemplate const* cInfo = undead->GetCreatureTemplate();
                        if (CreatureBaseStats const* stats =
                                sObjectMgr->GetCreatureBaseStats(level, cInfo->unit_class))
                        {
                            if (uint32 mana = stats->GenerateMana(cInfo))
                                undead->SetStatFlatModifier(UNIT_MOD_MANA, TOTAL_VALUE, (float)mana);
                        }

                        undead->UpdateAllStats();

                        // Salud fija por fórmula cuadrática: desacopla la HP del
                        // ModHealth del creature_template para evitar saltos bruscos
                        // al cambiar de entry entre rangos de nivel.
                        // Nivel 10 ~ 250 HP | nivel 60 ~ 6000 HP | nivel 80 ~ 10400 HP
                        uint32 health = GetInvasionHealthForLevel(level);
                        undead->SetMaxHealth(health);
                        undead->SetHealth(health);
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
                _inInvasionSpot.erase(key);
            }
            player->RemoveAura(SPELL_HOTSPOT_XP);
            WorldPacket data;
            ChatHandler::BuildChatPacket(data, CHAT_MSG_RAID_WARNING, LANG_UNIVERSAL,
                nullptr, nullptr, "|cffff0000La invasión ha cesado.|r");
            player->GetSession()->SendPacket(&data);
        }
        else if (wasInInvasionSpot)
        {
            // El jugador salió del spot pero el aura ya no existía
            std::lock_guard<std::mutex> lock(_timerMutex);
            _mobSpawnTimers.erase(key);
            _inInvasionSpot.erase(key);
        }

        // ===================================================================
        // TIPO 2 – MINERÍA / TIPO 3 – HERBORISTERÍA: spawnea GameObjects
        // ===================================================================
        HotSpotData spotGO;
        bool inGOSpot = HotSpotMgr::instance()->GetHotSpotAt(mapId, px, py, pz, spotGO, HOTSPOT_TYPE_MINING);
        if (!inGOSpot)
            inGOSpot = HotSpotMgr::instance()->GetHotSpotAt(mapId, px, py, pz, spotGO, HOTSPOT_TYPE_HERB);

        // Detectar si el jugador estaba ya en un spot de recolección
        bool wasInGOSpot;
        {
            std::lock_guard<std::mutex> lock(_timerMutex);
            wasInGOSpot = (_inGOSpot.find(key) != _inGOSpot.end());
        }

        if (inGOSpot)
        {
            // Notificar al jugador la primera vez que entra en la zona
            if (!wasInGOSpot)
            {
                {
                    std::lock_guard<std::mutex> lock(_timerMutex);
                    _inGOSpot.insert(key);
                }

                uint32 spellId = (spotGO.type == HOTSPOT_TYPE_MINING)
                    ? SPELL_HOTSPOT_MINING : SPELL_HOTSPOT_HERB;
                player->CastSpell(player, spellId, true);
                SendHotSpotMapMarker(player, spotGO);

                const char* zoneName   = (spotGO.type == HOTSPOT_TYPE_MINING)
                    ? "MINERÍA" : "HERBORISTERÍA";
                const char* zoneDetail = (spotGO.type == HOTSPOT_TYPE_MINING)
                    ? "Filones de mineral disponibles para recolectar."
                    : "Plantas medicinales disponibles para recolectar.";

                std::string goMsg = Acore::StringFormat(
                    "|cffffff00¡ZONA DE {}!|r\n|cff00ff00{}|r",
                    zoneName, zoneDetail);
                WorldPacket goData;
                ChatHandler::BuildChatPacket(goData, CHAT_MSG_RAID_WARNING,
                    LANG_UNIVERSAL, nullptr, nullptr, goMsg);
                player->GetSession()->SendPacket(&goData);
            }

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
                    : (!spotGO.goVariants.empty()
                        // Variedad real del mapa → elegir al azar
                        ? spotGO.goVariants[urand(0, (uint32)spotGO.goVariants.size() - 1)]
                        // Fallback: selección por nivel de jugador
                        : (spotGO.type == HOTSPOT_TYPE_MINING
                            ? GetMiningEntryForLevel(player->GetLevel())
                            : GetHerbEntryForLevel(player->GetLevel())));

                // Contar nodos presentes de TODAS las variantes conocidas
                // para respetar max_population global del hotspot.
                uint32 totalPresent = 0;
                if (!spotGO.goVariants.empty())
                {
                    for (uint32 e : spotGO.goVariants)
                    {
                        std::list<GameObject*> tmp;
                        player->GetGameObjectListWithEntryInGrid(
                            tmp, e, spotGO.radius);
                        totalPresent += (uint32)tmp.size();
                    }
                }
                else
                {
                    std::list<GameObject*> tmp;
                    player->GetGameObjectListWithEntryInGrid(
                        tmp, goEntry, spotGO.radius);
                    totalPresent = (uint32)tmp.size();
                }

                LOG_DEBUG("module",
                    "HotSpotMgr: tipo={} goEntry={} presentes={}/{} variantes={}",
                    spotGO.type, goEntry, totalPresent, spotGO.max_population,
                    (uint32)spotGO.goVariants.size());

                if (totalPresent < spotGO.max_population)
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
            // El jugador salió de la zona de recolección
            if (wasInGOSpot)
            {
                {
                    std::lock_guard<std::mutex> lock(_timerMutex);
                    _inGOSpot.erase(key);
                }

                player->RemoveAura(SPELL_HOTSPOT_MINING);
                player->RemoveAura(SPELL_HOTSPOT_HERB);

                WorldPacket goData;
                ChatHandler::BuildChatPacket(goData, CHAT_MSG_RAID_WARNING, LANG_UNIVERSAL,
                    nullptr, nullptr, "|cffff0000Has abandonado la zona de recolección.|r");
                player->GetSession()->SendPacket(&goData);
            }

            {
                std::lock_guard<std::mutex> lock(_timerMutex);
                _goSpawnTimers.erase(key);
            }
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
    std::unordered_set<uint32>         _inGOSpot;       // jugadores en zona de recolección activa
    std::unordered_set<uint32>         _inInvasionSpot; // jugadores en zona de invasión activa
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
            creature->loot.AddItem(LootStoreItem(GetPotionEntryForLevel(level), 0, 100.0f, false, LOOT_MODE_DEFAULT, 0, 1, 1));

        // Paños (40 % de probabilidad, 1-3 unidades)
        if (urand(1, 100) <= 40)
            creature->loot.AddItem(LootStoreItem(GetClothEntryForLevel(level), 0, 100.0f, false, LOOT_MODE_DEFAULT, 0, 1, 3));

        // El engine comprueba UNIT_DYNFLAG_LOOTABLE ANTES de llamar a OnUnitDeath.
        // Como estas criaturas son TempSummons sin tabla de loot, en ese momento
        // loot.isLooted()==true y la flag nunca se pone. La establecemos aquí
        // explícitamente para que el jugador pueda abrir el cuerpo.
        creature->loot.loot_type = LOOT_CORPSE;
        creature->SetDynamicFlag(UNIT_DYNFLAG_LOOTABLE);

        // XP: player->SummonCreature pone al jugador como owner de la mob,
        // por lo que KillRewarder la clasifica como unidad PvP (_isPvP = true)
        // y no calcula ninguna XP. La damos aquí directamente con nullptr como
        // víctima para saltarnos esa comprobación.
        // El multiplicador xp_mult del spot se aplica directamente porque ya
        // tenemos el spot cargado, evitando depender del aura SPELL_HOTSPOT_XP.
        if (!player->HasPlayerFlag(PLAYER_FLAGS_NO_XP_GAIN) &&
            player->GetLevel() < sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL))
        {
            // Acore::XP::Gain aplica creature_template->ModExperience, que en
            // muchos entries vanilla es < 1.0 y reduce la XP drásticamente.
            // Usamos BaseGain directamente (mob al mismo nivel que el jugador)
            // para obtener la XP "limpia" de un combate normal y luego aplicamos
            // el multiplicador del spot encima.
            ContentLevels contentLvl = GetContentLevelsForMapAndZone(
                player->GetMapId(), player->GetZoneId());
            uint32 baseXp = Acore::XP::BaseGain(level, level, contentLvl);
            baseXp = uint32(baseXp * sWorld->getRate(RATE_XP_KILL));
            uint32 xp = uint32(baseXp * spot.xp_mult);


            if (xp > 0)
                player->GiveXP(xp, nullptr);
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
