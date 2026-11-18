"""Run the real custom weapon methods against a small engine stub.

Usage: python3 tests/test_custom_weapon_timing.py (requires a C++ compiler).
Both CLIENT_WEAPONS configurations are exercised without launching the game.
"""
from pathlib import Path
import os
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "dlls/zpmod/zpmod_weapons.cpp").read_text()


def definition(source, start):
    begin = source.index(start)
    brace = source.index("{", begin)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[begin:end]


STUB = r'''
#include <cassert>
#include <cmath>
#include <algorithm>
#include <cstring>
using BOOL = int;
#define TRUE 1
#define FALSE 0
#define Q_min std::min
#define Q_max std::max
#define ENT(x) (x)
#define EMIT_SOUND_DYN(...) ((void)0)
#define UTIL_MakeVectors(...) ((void)0)
#define RANDOM_LONG(a,b) (a)
#define EF_MUZZLEFLASH 1
#define PLAYER_ATTACK1 1
#define LOUD_GUN_VOLUME 1000
#define NORMAL_GUN_FLASH 256
#define AUTOAIM_5DEGREES 5
#define AUTOAIM_2DEGREES 2
#define BULLET_PLAYER_357 1
#define BULLET_PLAYER_MP5 2
#define BULLET_PLAYER_BUCKSHOT 3
#define CHAN_WEAPON 1
#define CHAN_ITEM 2
#define ATTN_NORM 1
struct Vector {
    float x=0, y=0, z=0;
    Vector operator+(Vector) const { return {}; }
};
Vector g_vecZero, VECTOR_CONE_2DEGREES, VECTOR_CONE_3DEGREES,
       VECTOR_CONE_4DEGREES, VECTOR_CONE_8DEGREES;
struct Globals { float time=100, frametime=0.01f; } globals, *gpGlobals=&globals;
float UTIL_WeaponTimeBase() { return CLIENT_WEAPONS ? 0.0f : gpGlobals->time; }
struct Vars { int waterlevel=0, effects=0, button=0; float fov=0; Vector v_angle, punchangle; };
struct Player {
    Vars vars, *pev=&vars;
    int m_rgAmmo[32]={}, m_iFOV=0, m_iWeaponVolume=0, m_iWeaponFlash=0;
    float m_flNextAttack=0;
    void SetAnimation(int) {}
    void TabulateAmmo() {}
    Vector GetGunPosition() { return {}; }
    Vector GetAutoaimVector(int) { return {}; }
    template<class... T> void FireBullets(T...) {}
};
#define IN_ATTACK 1
#define IN_ATTACK2 2
#define IN_RELOAD 4
#define WEAPON_NOCLIP -1
#define ITEM_FLAG_NOAUTORELOAD 1
#define ITEM_FLAG_NOAUTOSWITCHEMPTY 2
class CBasePlayerWeapon {
public:
    Player player, *m_pPlayer=&player;
    int m_iClip=0, m_iPrimaryAmmoType=1, m_fInSpecialReload=0, m_fInReload=0;
    BOOL m_fFireOnEmpty=FALSE;
    float m_flNextPrimaryAttack=0, m_flNextSecondaryAttack=0,
          m_flTimeWeaponIdle=0, m_flLastFireTime=0;
    int lastAnim=-1, animations=0, capacity=8;
    virtual ~CBasePlayerWeapon()=default;
    virtual void PrimaryAttack() {}
    virtual void SecondaryAttack() {}
    virtual void Reload() {}
    virtual void WeaponIdle() {}
    virtual void Holster(int=0) { m_fInReload=FALSE; }
    void ItemPostFrame();
    int DefaultReload(int, int, float, int=0);
    BOOL DefaultDeploy(const char*, const char*, int, const char*) { return TRUE; }
    BOOL UseDecrement() { return FALSE; }
    BOOL ShouldWeaponIdle() { return FALSE; }
    BOOL IsUseable() { return m_iClip || player.m_rgAmmo[1]; }
    int iMaxClip() { return capacity; }
    int iFlags() { return 0; }
    const char *pszAmmo1() { return "ammo"; }
    const char *pszAmmo2() { return nullptr; }
    int PrimaryAmmoIndex() { return 1; }
    int SecondaryAmmoIndex() { return 2; }
    void SendWeaponAnim(int anim, int=0, int=0) { lastAnim=anim; ++animations; }
    void ResetEmptySound() {}
    void PlayEmptySound() {}
};
struct Rules { BOOL GetNextBestWeapon(Player*, CBasePlayerWeapon*) { return FALSE; } } rules, *g_pGameRules=&rules;
BOOL CanAttack(float when, float now, BOOL) { return when <= now; }
void CSMoE_ApplyKnockback(Player*, float) {}
'''

CHECKS = r'''
template<class T> void fireCycle(float interval, int capacity) {
    gpGlobals->time=100;
    T gun;
    gun.capacity=capacity;
    gun.m_iClip=capacity;
    gun.player.m_rgAmmo[1]=capacity;
    gun.Deploy();
    assert(gun.m_flTimeWeaponIdle > 100);
    assert(gun.player.m_flNextAttack > UTIL_WeaponTimeBase());
    gpGlobals->time=102;
    gun.player.m_flNextAttack=UTIL_WeaponTimeBase();
    gun.player.pev->button=IN_ATTACK;
    gun.ItemPostFrame();
    assert(gun.m_iClip == capacity-1);
    gpGlobals->time=102+interval/2;
    gun.ItemPostFrame();
    assert(gun.m_iClip == capacity-1);
    gpGlobals->time=102+interval+0.001f;
    gun.ItemPostFrame();
    assert(gun.m_iClip == capacity-2);
    gun.player.pev->button=0;
    gpGlobals->time=110;
    gun.WeaponIdle();
    assert(gun.lastAnim == 0);
    assert(gun.m_flTimeWeaponIdle > 110);
    gun.m_fInSpecialReload=2;
    gun.Holster();
    assert(gun.m_fInSpecialReload == 0);
}
int main() {
    fireCycle<CWeaponDeagleD>(0.14f,28);
    fireCycle<CWeaponMP7A1D>(0.072f,80);
    fireCycle<CWeaponM1887>(0.70f,8);
    fireCycle<CWeaponThompson>(0.095f,50);
    fireCycle<CWeaponAS50>(0.45f,5);
    fireCycle<CWeaponMG3>(0.088f,200);
    CWeaponM1887 shotgun;
    shotgun.player.m_rgAmmo[1]=8;
    gpGlobals->time=200;
    shotgun.Reload();
    assert(shotgun.m_fInSpecialReload == 1);
    // Simulate holding reload: insertion must take time, and conserve shells.
    for(int frame=1; frame<=600; ++frame) {
        gpGlobals->time=200+frame*0.01f;
        shotgun.Reload();
        assert(shotgun.m_iClip + shotgun.player.m_rgAmmo[1] == 8);
        if(frame<105) assert(shotgun.m_iClip == 0);
    }
    assert(shotgun.m_iClip == 8);
    shotgun.WeaponIdle();
    assert(shotgun.m_fInSpecialReload == 0);
    assert(shotgun.m_flNextPrimaryAttack > gpGlobals->time);
    // Automatic empty reload must also advance through the normal frame loop.
    CWeaponM1887 automatic;
    automatic.player.m_rgAmmo[1]=8;
    for(int frame=0; frame<=700; ++frame) {
        gpGlobals->time=300+frame*0.01f;
        automatic.player.m_flNextAttack=UTIL_WeaponTimeBase();
        automatic.ItemPostFrame();
    }
    assert(automatic.m_iClip == 8 && automatic.player.m_rgAmmo[1] == 0);
    // Magazine reload must finish before returning to idle.
    CWeaponMG3 mg;
    mg.capacity=200;
    mg.player.m_rgAmmo[1]=250;
    mg.Reload();
    float done=gpGlobals->time+4.7f;
    assert(mg.m_flTimeWeaponIdle >= done-0.001f);
    gpGlobals->time=done+0.01f;
    mg.player.m_flNextAttack=UTIL_WeaponTimeBase();
    mg.ItemPostFrame();
    assert(mg.m_iClip == 200 && mg.player.m_rgAmmo[1] == 50);
    assert(!mg.m_fInReload);
    CWeaponAS50 sniper;
    sniper.capacity=5;
    sniper.m_iClip=5;
    sniper.SecondaryAttack();
    assert(sniper.player.m_iFOV == 38);
    assert(sniper.m_flNextSecondaryAttack > gpGlobals->time);
    sniper.Reload();
    assert(sniper.player.m_iFOV == 38); // No ammo: reload did not start.
    sniper.m_iClip=0;
    sniper.player.m_rgAmmo[1]=5;
    sniper.Reload();
    assert(sniper.player.m_iFOV == 0);
}
'''


def main():
    base_source = (ROOT / "dlls/weapons.cpp").read_text()
    parts = [STUB]
    for name in ["BOOL CBasePlayerWeapon::DefaultReload(", "void CBasePlayerWeapon::ItemPostFrame("]:
        parts.append(definition(base_source, name))
    parts.append(definition(SOURCE, "class CSMoEWeapon") + ";")
    parts.extend(re.findall(r"enum \w+_e\s*\{[^}]+\};", SOURCE))
    classes = re.findall(r"class (CWeapon\w+) : public CSMoEWeapon", SOURCE)
    for cls in classes:
        declaration = definition(SOURCE, "class " + cls)
        # These tests only need gameplay, not map spawning or asset registration.
        declaration = re.sub(r"\s*(?:void (?:Spawn|Precache)|int (?:GetItemInfo|AddToPlayer))\([^;]+;", "", declaration)
        parts.append(declaration + ";")
        for method in ["Deploy", "PrimaryAttack", "Reload", "WeaponIdle", "SecondaryAttack", "Holster"]:
            match = re.search(r"(?:BOOL|void) " + cls + "::" + method + r"\(", SOURCE)
            if match:
                parts.append(definition(SOURCE, match.group()))
    parts.append(CHECKS)
    with tempfile.TemporaryDirectory(prefix="zpmod-weapons-") as folder:
        src = Path(folder) / "timing.cpp"
        src.write_text("\n".join(parts))
        for prediction in [0, 1]:
            binary = Path(folder) / f"timing-{prediction}"
            subprocess.run([os.environ.get("CXX", "c++"), "-std=c++11", f"-DCLIENT_WEAPONS={prediction}", str(src), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)
            print(f"PASS: fire rates, idle, reload, switching, zoom (CLIENT_WEAPONS={prediction})")


if __name__ == "__main__":
    main()
