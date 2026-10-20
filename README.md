# Neco's ZPMod
A Zombie Plague multiplayer mod for Half-Life 1.

## Installation
1. Extract the `.zip` release into your Half-Life **valve** folder `C:\Program Files (x86)\Steam\steamapps\common\Half-Life\valve`
2. Open `liblist.gam` inside the **valve** folder
3. Find this line `gamedll "dlls\hl.dll"` or sometimes `gamedll "dlls\mp.dll"`
4. Replace it with `gamedll "dlls\zpserver.dll"`

## Bots (Parabot)
[Parabot](https://github.com/nekonomicon/Parabot) is patched for this mod in the
[parabot-for-necos-zpmod](https://github.com/nurislamaibekuly/parabot-for-necos-zpmod)
fork, included as the `external/parabot` submodule (fetch it with
`git submodule update --init external/parabot`). The normal build compiles it
(`BUILD_PARABOT=ON`, requires `BUILD_SERVER=ON`). It detects this mod
(`zpserver` library) and loads it in place of `hl`.

Install with the rest of the build (`cmake --install build`), which places
`parabot` next to `zpserver` in `valve/dlls` and its language files in
`valve/addons/parabot`.

To enable bots as the game dll (proxy mode), edit `valve/liblist.gam`:

```
gamedll "dlls\parabot.dll"
gamedll_linux "dlls/parabot.so"
gamedll_osx "dlls/parabot.dylib"
```

Bots are configured in `valve/addons/parabot/config/zpmod/parabot.cfg`
and `characters.cfg` (created on first run). Set `ServerMode = Off` and
`NumBots` for a fixed number of bots, `ServerMode = On` to have bots come
and go like real players. Navigation data and logs are written to
`valve/addons/parabot/navpoints` and `valve/addons/parabot/log`.

Admins can also manage bots in game: `!bots <n>` sets the bot count,
`!addbot [n]` and `!removebot [n]` adjust it. The change applies
immediately and updates `NumBots` so it sticks across the 5-second bot
check. This needs admin rights (`zp_admin_pass` password or
`zpmod_admins.ini` allow list, same as the other `!` commands). From the
server console or RCON the same is available as `pb_botcount <n>`,
`pb_addbot [n]` and `pb_removebot [n]`.

To go back to a bot-free server, restore the `zpserver` lines in
`liblist.gam`.

## Credits
[FWGS (Flying With Gauss)](https://github.com/FWGS) for the [HLSDK](https://github.com/FWGS/hlsdk-portable) base

[nekonomicon](https://github.com/nekonomicon/Parabot) for Parabot

And you for playing the game