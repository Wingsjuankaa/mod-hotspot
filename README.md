# mod-hotspot
Módulo de Hot Spots para AzerothCore.

## Características
- **Zonas de Experiencia:** Multiplica la experiencia ganada en áreas delimitadas.
- **Hiperspawn:** Los mobs reaparecen casi instantáneamente (1 seg) después de morir y desaparecer el cuerpo (3 seg).
- **Buff Visual:** Los jugadores reciben un aura visual y regeneración de maná mientras están en la zona.
- **Comandos In-game:**
  - `.hotspot add 1 <radio>`: Crea un Hot Spot en tu posición.
  - `.hotspot delete`: Elimina los Hot Spots cercanos.

## Instalación
1. Copia la carpeta en `modules/`.
2. Ejecuta el SQL en tu base de datos `world`.
3. Compila el core.
