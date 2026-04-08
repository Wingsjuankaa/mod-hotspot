-- ============================================================
-- INSTALACIÓN LIMPIA: borra y recrea la tabla desde cero
-- ============================================================
DROP TABLE IF EXISTS `mod_hotspot`;

CREATE TABLE `mod_hotspot` (
  `id` INT AUTO_INCREMENT PRIMARY KEY,
  `map_id` INT NOT NULL,
  `zone_id` INT NOT NULL DEFAULT 0 COMMENT 'ZoneId del jugador al crear el spot. 0 = fallback posicional.',
  `x` FLOAT NOT NULL,
  `y` FLOAT NOT NULL,
  `z` FLOAT NOT NULL,
  `radius` FLOAT NOT NULL DEFAULT 50.0,
  `xp_multiplier` FLOAT NOT NULL DEFAULT 3.0,
  `respawn_multiplier` FLOAT NOT NULL DEFAULT 0.05,
  `type` TINYINT NOT NULL DEFAULT 1 COMMENT '1=Invasión 2=Minería 3=Herboristería',
  `active` TINYINT(1) NOT NULL DEFAULT 1,
  `creature_entry` INT NOT NULL DEFAULT 0 COMMENT 'Entry de criatura para tipo 1. 0 = auto según nivel.',
  `max_population` INT NOT NULL DEFAULT 10,
  `gameobject_entry` INT NOT NULL DEFAULT 0 COMMENT 'Entry de GO para tipos 2 y 3. 0 = auto (scan de zona).',
  `rotate` TINYINT(1) NOT NULL DEFAULT 1 COMMENT '1 = participa en la rotación automática. 0 = estado permanente (controlled manually).',
  `comment` VARCHAR(255) DEFAULT NULL
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- ============================================================
-- MIGRACIÓN: agregar columnas en instalación existente
-- (solo si no quieres perder los datos actuales)
-- ============================================================
-- ALTER TABLE `mod_hotspot`
--   ADD COLUMN IF NOT EXISTS `zone_id` INT NOT NULL DEFAULT 0
--     COMMENT 'ZoneId del jugador al crear el spot. 0 = fallback posicional.'
--     AFTER `map_id`,
--   ADD COLUMN IF NOT EXISTS `gameobject_entry` INT NOT NULL DEFAULT 0
--     COMMENT 'Entry de GO para tipos 2 y 3. 0 = auto (scan de zona).'
--     AFTER `max_population`,
--   ADD COLUMN IF NOT EXISTS `rotate` TINYINT(1) NOT NULL DEFAULT 1
--     COMMENT '1 = participa en la rotación automática. 0 = estado permanente.'
--     AFTER `gameobject_entry`;
