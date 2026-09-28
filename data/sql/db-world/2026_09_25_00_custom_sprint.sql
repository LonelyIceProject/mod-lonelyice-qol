-- Bind custom Sprint script to spell 56354
DELETE FROM `spell_script_names` WHERE `spell_id` = 56354 AND `ScriptName` = 'spell_custom_sprint';
INSERT INTO `spell_script_names` (`spell_id`, `ScriptName`) VALUES (56354, 'spell_custom_sprint');
