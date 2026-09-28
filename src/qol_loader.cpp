/*
 * This file is part of mod-lonelyice-qol. Copyright (C) LonelyIceProject.
 *
 * This program is free software; you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 */
void AddCustomSprintScripts();
void AddCustomAutoLootScripts();

// Static module entry (modules/mod-lonelyice-qol) and plugin entry (plugin/plugin.cpp).
void Addmod_lonelyice_qolScripts()
{
    AddCustomSprintScripts();
    AddCustomAutoLootScripts();
}
