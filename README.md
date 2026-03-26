# mod-hotspot
Módulo de Hot Spots para AzerothCore.

## Tipos de Hot Spot

| Tipo | Nombre | Descripción |
|------|--------|-------------|
| 1 | **Invasión** | Spawnea mobs No-muertos escalados al nivel del jugador. Multiplica XP. |
| 2 | **Minería** | Spawnea nodos de mineral escalados al nivel del jugador. |
| 3 | **Herboristería** | Spawnea plantas escaladas al nivel del jugador. |

## Características
- **Invasión (tipo 1):** Mobs No-muertos con estadísticas escaladas, loot dinámico (pociones, paños, dinero) y multiplicador de XP configurable.
- **Minería (tipo 2):** Nodos de mineral temporales (5 min) que aparecen mientras el jugador está en la zona.
- **Herboristería (tipo 3):** Plantas temporales (5 min) que aparecen mientras el jugador está en la zona.
- Los tipos 2 y 3 usan la entrada `gameobject_entry` de la tabla. Con `0` se elige automáticamente por nivel del jugador.

## Comandos In-game
- `.hotspot add <tipo> <población_máxima>` — Crea un Hot Spot en tu posición actual.
  - Ej: `.hotspot add 1 15` → zona de invasión con hasta 15 mobs
  - Ej: `.hotspot add 2 8`  → zona de minería con hasta 8 nodos
  - Ej: `.hotspot add 3 6`  → zona de herboristería con hasta 6 plantas
- `.hotspot delete` — Elimina el Hot Spot en el que estás parado.
- `.hotspot reload` — Recarga todos los Hot Spots desde la base de datos.

## Instalación
1. Copia la carpeta en `modules/`.
2. Ejecuta `sql/mod_hotspot.sql` en tu base de datos `world`.
3. Compila el core.

## Tabla `mod_hotspot`

| Columna | Descripción |
|---------|-------------|
| `type` | 1=Invasión, 2=Minería, 3=Herboristería |
| `creature_entry` | Entry de criatura para tipo 1. `0` = auto por nivel. |
| `gameobject_entry` | Entry de GO para tipos 2 y 3. `0` = auto por nivel. |
| `max_population` | Máximo de mobs/nodos simultáneos en la zona. |
| `xp_multiplier` | Multiplicador de XP (solo tipo 1). |
| `radius` | Radio de la zona en unidades de juego. |

> **Nota:** Los entries de GameObjects (minería/herboristería) generados automáticamente
> son aproximados. Puedes verificarlos en tu DB con:
> `SELECT entry, name FROM gameobject_template WHERE type = 3 AND name LIKE '%veta%';`
