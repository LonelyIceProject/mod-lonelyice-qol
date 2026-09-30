<p align="center"><img src="logo.png" width="128" alt="logo"></p>

# mod-lonelyice-qol

Small quality-of-life changes for a single-player server with bots.

## Features

- **Sprint for everyone**: every character (and bot) learns Sprint (+50% speed for 20 s, 35 s cooldown), a spell
  of its own described in `data/patches.json`. The [LonelyIce](https://github.com/LonelyIceProject/lonelyice)
  installer gives it a free spell id, adds it to the server and builds the client patch from your own client.
- **Auto-loot for real players**: money goes straight to the bag and is split only between the real players
  of the group (bots take no share); quest items and drops needed by an incomplete quest are picked up while
  the player still needs them. Everything else stays on the corpse for normal loot rules.

## Requirements

[LonelyIceProject/mod-playerbots](https://github.com/LonelyIceProject/mod-playerbots) (bots use the sprint too).
## Install

This module is written for [LonelyIceProject/azerothcore-wotlk](https://github.com/LonelyIceProject/azerothcore-wotlk),
a fork of AzerothCore with runtime plugins, and builds in two ways.

**As a plugin** (the core built with `-DWITH_DYNAMIC_LINKING=ON`):

```
cmake -S azerothcore-wotlk -B build -DWITH_DYNAMIC_LINKING=ON -DWITH_PLAYERBOTS_HOOKS=ON ^
      -DAC_PLUGIN_ABI=lonelyice-ac-2 "-DAC_PLUGIN_SOURCE_DIRS=<path>/mod-playerbots;<path>/mod-lonelyice-qol"
cmake --build build --config RelWithDebInfo
```

The plugin is laid out in `bin/<config>/plugins/lonelyice.qol/`. Copy that folder into the server's `plugins` folder
(`PluginsDir` in worldserver.conf); [LonelyIce](https://github.com/LonelyIceProject/lonelyice) does this for you.

**As a classic static module**: clone into `modules/mod-lonelyice-qol` of the core and rebuild.
## Support

LonelyIce is free, with no ads and no paid features. If it is useful to you, you can
[buy me a coffee](https://buymeacoffee.com/darthgelum): it pays for the server, code signing and development time.

<a href="https://buymeacoffee.com/darthgelum"><img src=".github/buy-me-a-coffee.png" alt="Buy me a coffee" width="303"></a>

## License

GNU General Public License v2.0 or later, see [LICENSE](LICENSE). Part of the
[LonelyIce](https://github.com/LonelyIceProject/lonelyice) single-player project.
