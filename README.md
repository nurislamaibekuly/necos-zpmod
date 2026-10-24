# Neco's ZPMod
A Zombie Plague multiplayer mod for Half-Life 1.

## Installation
1. Extract the `.zip` release into your Half-Life **valve** folder `C:\Program Files (x86)\Steam\steamapps\common\Half-Life\valve`
2. Open `liblist.gam` inside the **valve** folder
3. Find this line `gamedll "dlls\hl.dll"` or sometimes `gamedll "dlls\mp.dll"`
4. Replace it with `gamedll "dlls\zpserver.dll"`

## Bots (YaPB)
[YaPB](https://github.com/yapb/yapb) is patched for this mod in the
[hahayoutookind/yapb](https://github.com/hahayoutookind/yapb) fork (Half-Life
support), included as the `external/yapb` submodule (fetch it with
`git submodule update --init --recursive external/yapb`). The normal build compiles it
(`BUILD_YAPB=ON`, requires `BUILD_SERVER=ON`). It detects this mod
(`zpserver` library), loads it in place of `hl`, and reads the mod's
`team` userinfo key so bots know humans from zombies.

Install with the rest of the build (`cmake --install build`), which places
`yapb` next to `zpserver` in `valve/dlls` and the default configs in
`valve/addons/yapb`.

To enable bots as the game dll (proxy mode), edit `valve/liblist.gam`:

```
gamedll "dlls\yapb.dll"
gamedll_linux "dlls/yapb.so"
gamedll_osx "dlls/yapb.dylib"
```

Bots are configured in `valve/addons/yapb/conf/yapb.cfg` (created on first
run). Set `yb_quota` to the number of bots, or use `yb_quota_mode` `fill`
(keep N players in total) or `match` (keep N bots per human). Waypoints are
generated automatically the first time a map runs (watch for "Starting map
analysis"); the graph is saved under `valve/addons/yapb/data/graph` and logs
under `valve/addons/yapb/data/logs`.

Admins can also manage bots in game: `!bots <n>` sets the bot count,
`!addbot [n]` and `!removebot [n]` adjust it. The change applies to
`yb_quota` immediately and the quota manager adds or kicks bots to match.
This needs admin rights (`zp_admin_pass` password or `zpmod_admins.ini`
allow list, same as the other `!` commands). From the server console or RCON
use `yb_quota <n>`, `addbot` and `removebots`.

To go back to a bot-free server, restore the `zpserver` lines in
`liblist.gam`.

## Credits
[FWGS (Flying With Gauss)](https://github.com/FWGS) for the [HLSDK](https://github.com/FWGS/hlsdk-portable) base

[YaPB Project](https://github.com/yapb/yapb) and
[hahayoutookind](https://github.com/hahayoutookind/yapb) for the Half-Life YaPB fork

[nekonomicon](https://github.com/nekonomicon/Parabot) for Parabot (the previous bot)

And you for playing the game