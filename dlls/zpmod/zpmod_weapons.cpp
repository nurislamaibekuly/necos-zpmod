#include "extdll.h"
#include "util.h"
#include "cbase.h"
#include "weapons.h"
#include "player.h"
#include "zpmod.h"

// ---------------------------------------------------------------------------
// Custom CSMoE Weapons for Neco Zombie Plague (server-driven animations,
// sounds, fireBullets & zombie knockback — works natively on vanilla Xash3D)
// ---------------------------------------------------------------------------

static void CSMoE_ApplyKnockback( CBaseEntity *pOwner, float force )
{
	if( !pOwner || force <= 0.0f ) return;

	UTIL_MakeVectors( pOwner->pev->v_angle + pOwner->pev->punchangle );
	Vector vecSrc = pOwner->pev->origin + pOwner->pev->view_ofs;
	Vector vecEnd = vecSrc + gpGlobals->v_forward * 2048.0f;
	TraceResult tr;
	UTIL_TraceLine( vecSrc, vecEnd, dont_ignore_monsters, pOwner->edict(), &tr );
	if( tr.flFraction < 1.0f && tr.pHit && ZPIsZombie( tr.pHit ) )
	{
		Vector dir = gpGlobals->v_forward;
		dir.z = 0.0f;
		if( dir.Length() > 0.01f ) dir = dir.Normalize();
		float lift = (tr.pHit->v.flags & FL_ONGROUND) ? 60.0f : 20.0f;
		tr.pHit->v.velocity = tr.pHit->v.velocity + dir * force + Vector( 0, 0, lift );
	}
}

// ===========================================================================
// 1. Dual Desert Eagle (weapon_deagled)
// ===========================================================================
enum deagled_e
{
	DEAGLED_IDLE = 0,
	DEAGLED_IDLE_LEFTEMPTY,
	DEAGLED_SHOOTLEFT1,
	DEAGLED_SHOOTLEFT2,
	DEAGLED_SHOOTLEFT3,
	DEAGLED_SHOOTLEFT4,
	DEAGLED_SHOOTLEFT5,
	DEAGLED_SHOOTLEFTLAST,
	DEAGLED_SHOOTRIGHT1,
	DEAGLED_SHOOTRIGHT2,
	DEAGLED_SHOOTRIGHT3,
	DEAGLED_SHOOTRIGHT4,
	DEAGLED_SHOOTRIGHT5,
	DEAGLED_SHOOTRIGHTLAST,
	DEAGLED_RELOAD,
	DEAGLED_DRAW
};

class CWeaponDeagleD : public CBasePlayerWeapon
{
public:
	void Spawn( void );
	void Precache( void );
	int GetItemInfo( ItemInfo *p );
	int AddToPlayer( CBasePlayer *pPlayer );
	BOOL Deploy( void );
	void PrimaryAttack( void );
	void Reload( void );
	void WeaponIdle( void );
};

LINK_ENTITY_TO_CLASS( weapon_deagled, CWeaponDeagleD )

void CWeaponDeagleD::Spawn( void )
{
	Precache();
	m_iId = WEAPON_DEAGLED;
	SET_MODEL( ENT( pev ), "models/zpmod/w_ddeagle.mdl" );
	m_iDefaultAmmo = 28;
	FallInit();
}

void CWeaponDeagleD::Precache( void )
{
	PRECACHE_MODEL( "models/zpmod/v_ddeagle.mdl" );
	PRECACHE_MODEL( "models/zpmod/p_ddeagle.mdl" );
	PRECACHE_MODEL( "models/zpmod/w_ddeagle.mdl" );
	PRECACHE_GENERIC( "sprites/weapon_deagled.txt" );
	PRECACHE_SOUND( "weapons/dde-1.wav" );
	PRECACHE_SOUND( "weapons/DDe_Clipoff.wav" );
	PRECACHE_SOUND( "weapons/DDe_twirl.wav" );
	PRECACHE_SOUND( "weapons/DDe_Clipout.wav" );
	PRECACHE_SOUND( "weapons/DDe_clipin.wav" );
	PRECACHE_SOUND( "weapons/DDe_Load.wav" );
}

int CWeaponDeagleD::GetItemInfo( ItemInfo *p )
{
	p->pszName = STRING( pev->classname );
	p->pszAmmo1 = "50AE";
	p->iMaxAmmo1 = _50AE_MAX_CARRY;
	p->pszAmmo2 = NULL;
	p->iMaxAmmo2 = -1;
	p->iMaxClip = 28;
	p->iSlot = 1;
	p->iPosition = 2;
	p->iId = m_iId = WEAPON_DEAGLED;
	p->iWeight = 18;
	p->iFlags = 0;
	return 1;
}

int CWeaponDeagleD::AddToPlayer( CBasePlayer *pPlayer )
{
	if( CBasePlayerWeapon::AddToPlayer( pPlayer ) )
	{
		MESSAGE_BEGIN( MSG_ONE, gmsgWeapPickup, NULL, pPlayer->pev );
			WRITE_BYTE( m_iId );
		MESSAGE_END();
		return TRUE;
	}
	return FALSE;
}

BOOL CWeaponDeagleD::Deploy( void )
{
	return DefaultDeploy( "models/zpmod/v_ddeagle.mdl", "models/zpmod/p_ddeagle.mdl", DEAGLED_DRAW, "onehanded" );
}

void CWeaponDeagleD::PrimaryAttack( void )
{
	if( m_pPlayer->pev->waterlevel == 3 || m_iClip <= 0 )
	{
		if( m_iClip <= 0 && !m_fFireOnEmpty ) { Reload(); return; }
		PlayEmptySound();
		m_flNextPrimaryAttack = UTIL_WeaponTimeBase() + 0.15f;
		return;
	}

	m_iClip--;
	m_pPlayer->pev->effects |= EF_MUZZLEFLASH;
	m_pPlayer->SetAnimation( PLAYER_ATTACK1 );

	UTIL_MakeVectors( m_pPlayer->pev->v_angle + m_pPlayer->pev->punchangle );
	Vector vecSrc = m_pPlayer->GetGunPosition();
	Vector vecAim = m_pPlayer->GetAutoaimVector( AUTOAIM_5DEGREES );
	m_pPlayer->FireBullets( 1, vecSrc, vecAim, VECTOR_CONE_2DEGREES, 8192, BULLET_PLAYER_357, 2, 54, m_pPlayer->pev );
	CSMoE_ApplyKnockback( m_pPlayer, 150.0f );

	int anim = (m_iClip & 1) ? DEAGLED_SHOOTLEFT1 : DEAGLED_SHOOTRIGHT1;
	if( m_iClip == 1 ) anim = DEAGLED_SHOOTLEFTLAST;
	if( m_iClip == 0 ) anim = DEAGLED_SHOOTRIGHTLAST;
	SendWeaponAnim( anim, 0 );

	EMIT_SOUND_DYN( ENT( m_pPlayer->pev ), CHAN_WEAPON, "weapons/dde-1.wav", 0.9f, ATTN_NORM, 0, RANDOM_LONG( 96, 104 ) );
	m_pPlayer->pev->punchangle.x -= 2.0f;
	m_flNextPrimaryAttack = UTIL_WeaponTimeBase() + 0.14f;
	m_flTimeWeaponIdle = UTIL_WeaponTimeBase() + 2.0f;
}

void CWeaponDeagleD::Reload( void )
{
	DefaultReload( 28, DEAGLED_RELOAD, 3.8f );
}

void CWeaponDeagleD::WeaponIdle( void )
{
	if( m_flTimeWeaponIdle > UTIL_WeaponTimeBase() ) return;
	SendWeaponAnim( (m_iClip == 1) ? DEAGLED_IDLE_LEFTEMPTY : DEAGLED_IDLE, 0 );
	m_flTimeWeaponIdle = UTIL_WeaponTimeBase() + 8.0f;
}

// ===========================================================================
// 2. Dual MP7A1 (weapon_mp7a1d)
// ===========================================================================
class CWeaponMP7A1D : public CBasePlayerWeapon
{
public:
	void Spawn( void );
	void Precache( void );
	int GetItemInfo( ItemInfo *p );
	int AddToPlayer( CBasePlayer *pPlayer );
	BOOL Deploy( void );
	void PrimaryAttack( void );
	void Reload( void );
	void WeaponIdle( void );
};

LINK_ENTITY_TO_CLASS( weapon_mp7a1d, CWeaponMP7A1D )

void CWeaponMP7A1D::Spawn( void )
{
	Precache();
	m_iId = WEAPON_MP7A1D;
	SET_MODEL( ENT( pev ), "models/zpmod/w_dmp7a1.mdl" );
	m_iDefaultAmmo = 80;
	FallInit();
}

void CWeaponMP7A1D::Precache( void )
{
	PRECACHE_MODEL( "models/zpmod/v_dmp7a1.mdl" );
	PRECACHE_MODEL( "models/zpmod/p_dmp7a1.mdl" );
	PRECACHE_MODEL( "models/zpmod/w_dmp7a1.mdl" );
	PRECACHE_GENERIC( "sprites/weapon_mp7a1d.txt" );
	PRECACHE_SOUND( "weapons/dmp7-1.wav" );
	PRECACHE_SOUND( "weapons/Dmp7_drop.wav" );
	PRECACHE_SOUND( "weapons/Dmp7_draw.wav" );
	PRECACHE_SOUND( "weapons/mp7_foley2.wav" );
	PRECACHE_SOUND( "weapons/mp7_foley4.wav" );
}

int CWeaponMP7A1D::GetItemInfo( ItemInfo *p )
{
	p->pszName = STRING( pev->classname );
	p->pszAmmo1 = "46MM";
	p->iMaxAmmo1 = _46MM_MAX_CARRY;
	p->pszAmmo2 = NULL;
	p->iMaxAmmo2 = -1;
	p->iMaxClip = 80;
	p->iSlot = 1;
	p->iPosition = 3;
	p->iId = m_iId = WEAPON_MP7A1D;
	p->iWeight = 19;
	p->iFlags = 0;
	return 1;
}

int CWeaponMP7A1D::AddToPlayer( CBasePlayer *pPlayer )
{
	if( CBasePlayerWeapon::AddToPlayer( pPlayer ) )
	{
		MESSAGE_BEGIN( MSG_ONE, gmsgWeapPickup, NULL, pPlayer->pev );
			WRITE_BYTE( m_iId );
		MESSAGE_END();
		return TRUE;
	}
	return FALSE;
}

BOOL CWeaponMP7A1D::Deploy( void )
{
	return DefaultDeploy( "models/zpmod/v_dmp7a1.mdl", "models/zpmod/p_dmp7a1.mdl", DEAGLED_DRAW, "mp5" );
}

void CWeaponMP7A1D::PrimaryAttack( void )
{
	if( m_pPlayer->pev->waterlevel == 3 || m_iClip <= 0 )
	{
		if( m_iClip <= 0 && !m_fFireOnEmpty ) { Reload(); return; }
		PlayEmptySound();
		m_flNextPrimaryAttack = UTIL_WeaponTimeBase() + 0.12f;
		return;
	}

	m_iClip--;
	m_pPlayer->pev->effects |= EF_MUZZLEFLASH;
	m_pPlayer->SetAnimation( PLAYER_ATTACK1 );

	UTIL_MakeVectors( m_pPlayer->pev->v_angle + m_pPlayer->pev->punchangle );
	Vector vecSrc = m_pPlayer->GetGunPosition();
	Vector vecAim = m_pPlayer->GetAutoaimVector( AUTOAIM_5DEGREES );
	m_pPlayer->FireBullets( 1, vecSrc, vecAim, VECTOR_CONE_4DEGREES, 8192, BULLET_PLAYER_MP5, 3, 28, m_pPlayer->pev );
	CSMoE_ApplyKnockback( m_pPlayer, 65.0f );

	int anim = (m_iClip & 1) ? (DEAGLED_SHOOTLEFT1 + (m_iClip % 4)) : (DEAGLED_SHOOTRIGHT1 + (m_iClip % 4));
	SendWeaponAnim( anim, 0 );

	EMIT_SOUND_DYN( ENT( m_pPlayer->pev ), CHAN_WEAPON, "weapons/dmp7-1.wav", 0.8f, ATTN_NORM, 0, RANDOM_LONG( 97, 105 ) );
	m_pPlayer->pev->punchangle.x -= 0.7f;
	m_flNextPrimaryAttack = UTIL_WeaponTimeBase() + 0.072f;
	m_flTimeWeaponIdle = UTIL_WeaponTimeBase() + 2.0f;
}

void CWeaponMP7A1D::Reload( void )
{
	DefaultReload( 80, DEAGLED_RELOAD, 3.4f );
}

void CWeaponMP7A1D::WeaponIdle( void )
{
	if( m_flTimeWeaponIdle > UTIL_WeaponTimeBase() ) return;
	SendWeaponAnim( DEAGLED_IDLE, 0 );
	m_flTimeWeaponIdle = UTIL_WeaponTimeBase() + 8.0f;
}

// ===========================================================================
// 3. Winchester M1887 (weapon_m1887)
// ===========================================================================
enum m1887_e
{
	M1887_IDLE = 0,
	M1887_FIRE1,
	M1887_FIRE2,
	M1887_FIRE3,
	M1887_FIRE4,
	M1887_FIRE5,
	M1887_RELOAD,
	M1887_AFTER_RELOAD,
	M1887_START_RELOAD,
	M1887_DRAW
};

class CWeaponM1887 : public CBasePlayerWeapon
{
public:
	void Spawn( void );
	void Precache( void );
	int GetItemInfo( ItemInfo *p );
	int AddToPlayer( CBasePlayer *pPlayer );
	BOOL Deploy( void );
	void PrimaryAttack( void );
	void Reload( void );
	void WeaponIdle( void );
};

LINK_ENTITY_TO_CLASS( weapon_m1887, CWeaponM1887 )

void CWeaponM1887::Spawn( void )
{
	Precache();
	m_iId = WEAPON_M1887;
	SET_MODEL( ENT( pev ), "models/zpmod/w_m1887.mdl" );
	m_iDefaultAmmo = 8;
	FallInit();
}

void CWeaponM1887::Precache( void )
{
	PRECACHE_MODEL( "models/zpmod/v_m1887.mdl" );
	PRECACHE_MODEL( "models/zpmod/p_m1887.mdl" );
	PRECACHE_MODEL( "models/zpmod/w_m1887.mdl" );
	PRECACHE_GENERIC( "sprites/weapon_m1887.txt" );
	PRECACHE_SOUND( "weapons/m1887-1.wav" );
	PRECACHE_SOUND( "weapons/m1887_Insert.wav" );
	PRECACHE_SOUND( "weapons/m1887_ReStart.wav" );
	PRECACHE_SOUND( "weapons/m1887_Draw.wav" );
	PRECACHE_SOUND( "weapons/m1887_reafter.wav" );
}

int CWeaponM1887::GetItemInfo( ItemInfo *p )
{
	p->pszName = STRING( pev->classname );
	p->pszAmmo1 = "buckshot";
	p->iMaxAmmo1 = BUCKSHOT_MAX_CARRY;
	p->pszAmmo2 = NULL;
	p->iMaxAmmo2 = -1;
	p->iMaxClip = 8;
	p->iSlot = 2;
	p->iPosition = 3;
	p->iId = m_iId = WEAPON_M1887;
	p->iWeight = 20;
	p->iFlags = 0;
	return 1;
}

int CWeaponM1887::AddToPlayer( CBasePlayer *pPlayer )
{
	if( CBasePlayerWeapon::AddToPlayer( pPlayer ) )
	{
		MESSAGE_BEGIN( MSG_ONE, gmsgWeapPickup, NULL, pPlayer->pev );
			WRITE_BYTE( m_iId );
		MESSAGE_END();
		return TRUE;
	}
	return FALSE;
}

BOOL CWeaponM1887::Deploy( void )
{
	return DefaultDeploy( "models/zpmod/v_m1887.mdl", "models/zpmod/p_m1887.mdl", M1887_DRAW, "shotgun" );
}

void CWeaponM1887::PrimaryAttack( void )
{
	if( m_pPlayer->pev->waterlevel == 3 || m_iClip <= 0 )
	{
		if( m_iClip <= 0 && !m_fFireOnEmpty ) { Reload(); return; }
		PlayEmptySound();
		m_flNextPrimaryAttack = UTIL_WeaponTimeBase() + 0.15f;
		return;
	}

	m_iClip--;
	m_fInSpecialReload = 0;
	m_pPlayer->pev->effects |= EF_MUZZLEFLASH;
	m_pPlayer->SetAnimation( PLAYER_ATTACK1 );

	UTIL_MakeVectors( m_pPlayer->pev->v_angle + m_pPlayer->pev->punchangle );
	Vector vecSrc = m_pPlayer->GetGunPosition();
	Vector vecAim = m_pPlayer->GetAutoaimVector( AUTOAIM_5DEGREES );
	m_pPlayer->FireBullets( 9, vecSrc, vecAim, VECTOR_CONE_8DEGREES, 2048, BULLET_PLAYER_BUCKSHOT, 0, 26, m_pPlayer->pev );
	CSMoE_ApplyKnockback( m_pPlayer, 360.0f );

	SendWeaponAnim( M1887_FIRE1 + RANDOM_LONG( 0, 4 ), 0 );
	EMIT_SOUND_DYN( ENT( m_pPlayer->pev ), CHAN_WEAPON, "weapons/m1887-1.wav", 0.95f, ATTN_NORM, 0, RANDOM_LONG( 96, 104 ) );
	m_pPlayer->pev->punchangle.x -= 4.5f;
	m_flNextPrimaryAttack = UTIL_WeaponTimeBase() + 0.70f;
	m_flTimeWeaponIdle = UTIL_WeaponTimeBase() + 2.0f;
}

void CWeaponM1887::Reload( void )
{
	if( m_pPlayer->m_rgAmmo[m_iPrimaryAmmoType] <= 0 || m_iClip == 8 )
		return;
	if( m_flNextPrimaryAttack > UTIL_WeaponTimeBase() )
		return;

	if( m_fInSpecialReload == 0 )
	{
		SendWeaponAnim( M1887_START_RELOAD, 0 );
		m_fInSpecialReload = 1;
		m_pPlayer->m_flNextAttack = UTIL_WeaponTimeBase() + 0.55f;
		m_flTimeWeaponIdle = UTIL_WeaponTimeBase() + 0.55f;
		m_flNextPrimaryAttack = UTIL_WeaponTimeBase() + 0.6f;
	}
	else if( m_fInSpecialReload == 1 )
	{
		if( m_flTimeWeaponIdle > UTIL_WeaponTimeBase() ) return;
		m_fInSpecialReload = 2;
		SendWeaponAnim( M1887_RELOAD, 0 );
		m_flNextReload = UTIL_WeaponTimeBase() + 0.45f;
		m_flTimeWeaponIdle = UTIL_WeaponTimeBase() + 0.45f;
	}
	else
	{
		m_iClip++;
		m_pPlayer->m_rgAmmo[m_iPrimaryAmmoType]--;
		m_fInSpecialReload = 1;
	}
}

void CWeaponM1887::WeaponIdle( void )
{
	if( m_flTimeWeaponIdle > UTIL_WeaponTimeBase() ) return;

	if( m_iClip == 0 && m_fInSpecialReload == 0 && m_pPlayer->m_rgAmmo[m_iPrimaryAmmoType] )
	{
		Reload();
	}
	else if( m_fInSpecialReload != 0 )
	{
		if( m_iClip < 8 && m_pPlayer->m_rgAmmo[m_iPrimaryAmmoType] )
		{
			Reload();
		}
		else
		{
			SendWeaponAnim( M1887_AFTER_RELOAD, 0 );
			EMIT_SOUND_DYN( ENT( m_pPlayer->pev ), CHAN_ITEM, "weapons/m1887_reafter.wav", 0.85f, ATTN_NORM, 0, 100 );
			m_fInSpecialReload = 0;
			m_flTimeWeaponIdle = UTIL_WeaponTimeBase() + 1.5f;
		}
	}
	else
	{
		SendWeaponAnim( M1887_IDLE, 0 );
		m_flTimeWeaponIdle = UTIL_WeaponTimeBase() + 6.0f;
	}
}

// ===========================================================================
// 4. Thompson Chicago Gold (weapon_thompson)
// ===========================================================================
enum thompson_e
{
	THOMPSON_IDLE = 0,
	THOMPSON_SHOOT1,
	THOMPSON_SHOOT2,
	THOMPSON_RELOAD,
	THOMPSON_DRAW
};

class CWeaponThompson : public CBasePlayerWeapon
{
public:
	void Spawn( void );
	void Precache( void );
	int GetItemInfo( ItemInfo *p );
	int AddToPlayer( CBasePlayer *pPlayer );
	BOOL Deploy( void );
	void PrimaryAttack( void );
	void Reload( void );
	void WeaponIdle( void );
};

LINK_ENTITY_TO_CLASS( weapon_thompson, CWeaponThompson )

void CWeaponThompson::Spawn( void )
{
	Precache();
	m_iId = WEAPON_THOMPSON;
	SET_MODEL( ENT( pev ), "models/zpmod/w_thompson.mdl" );
	m_iDefaultAmmo = 50;
	FallInit();
}

void CWeaponThompson::Precache( void )
{
	PRECACHE_MODEL( "models/zpmod/v_thompson.mdl" );
	PRECACHE_MODEL( "models/zpmod/p_thompson.mdl" );
	PRECACHE_MODEL( "models/zpmod/w_thompson.mdl" );
	PRECACHE_GENERIC( "sprites/weapon_thompson.txt" );
	PRECACHE_SOUND( "weapons/thompsongold-1.wav" );
	PRECACHE_SOUND( "weapons/thompsongold_draw.wav" );
	PRECACHE_SOUND( "weapons/thompsongold_clipout.wav" );
	PRECACHE_SOUND( "weapons/thompsongold_clipin.wav" );
}

int CWeaponThompson::GetItemInfo( ItemInfo *p )
{
	p->pszName = STRING( pev->classname );
	p->pszAmmo1 = "45acp";
	p->iMaxAmmo1 = _45ACP_MAX_CARRY;
	p->pszAmmo2 = NULL;
	p->iMaxAmmo2 = -1;
	p->iMaxClip = 50;
	p->iSlot = 2;
	p->iPosition = 4;
	p->iId = m_iId = WEAPON_THOMPSON;
	p->iWeight = 21;
	p->iFlags = 0;
	return 1;
}

int CWeaponThompson::AddToPlayer( CBasePlayer *pPlayer )
{
	if( CBasePlayerWeapon::AddToPlayer( pPlayer ) )
	{
		MESSAGE_BEGIN( MSG_ONE, gmsgWeapPickup, NULL, pPlayer->pev );
			WRITE_BYTE( m_iId );
		MESSAGE_END();
		return TRUE;
	}
	return FALSE;
}

BOOL CWeaponThompson::Deploy( void )
{
	return DefaultDeploy( "models/zpmod/v_thompson.mdl", "models/zpmod/p_thompson.mdl", THOMPSON_DRAW, "mp5" );
}

void CWeaponThompson::PrimaryAttack( void )
{
	if( m_pPlayer->pev->waterlevel == 3 || m_iClip <= 0 )
	{
		if( m_iClip <= 0 && !m_fFireOnEmpty ) { Reload(); return; }
		PlayEmptySound();
		m_flNextPrimaryAttack = UTIL_WeaponTimeBase() + 0.15f;
		return;
	}

	m_iClip--;
	m_pPlayer->pev->effects |= EF_MUZZLEFLASH;
	m_pPlayer->SetAnimation( PLAYER_ATTACK1 );

	UTIL_MakeVectors( m_pPlayer->pev->v_angle + m_pPlayer->pev->punchangle );
	Vector vecSrc = m_pPlayer->GetGunPosition();
	Vector vecAim = m_pPlayer->GetAutoaimVector( AUTOAIM_5DEGREES );
	m_pPlayer->FireBullets( 1, vecSrc, vecAim, VECTOR_CONE_3DEGREES, 8192, BULLET_PLAYER_MP5, 2, 34, m_pPlayer->pev );
	CSMoE_ApplyKnockback( m_pPlayer, 95.0f );

	SendWeaponAnim( (m_iClip & 1) ? THOMPSON_SHOOT1 : THOMPSON_SHOOT2, 0 );
	EMIT_SOUND_DYN( ENT( m_pPlayer->pev ), CHAN_WEAPON, "weapons/thompsongold-1.wav", 0.85f, ATTN_NORM, 0, RANDOM_LONG( 97, 104 ) );
	m_pPlayer->pev->punchangle.x -= 1.0f;
	m_flNextPrimaryAttack = UTIL_WeaponTimeBase() + 0.095f;
	m_flTimeWeaponIdle = UTIL_WeaponTimeBase() + 2.0f;
}

void CWeaponThompson::Reload( void )
{
	DefaultReload( 50, THOMPSON_RELOAD, 3.4f );
}

void CWeaponThompson::WeaponIdle( void )
{
	if( m_flTimeWeaponIdle > UTIL_WeaponTimeBase() ) return;
	SendWeaponAnim( THOMPSON_IDLE, 0 );
	m_flTimeWeaponIdle = UTIL_WeaponTimeBase() + 6.0f;
}

// ===========================================================================
// 5. AS50 Anti-Materiel Sniper (weapon_as50)
// ===========================================================================
enum as50_e
{
	AS50_IDLE = 0,
	AS50_SHOOT1,
	AS50_SHOOT2,
	AS50_SHOOT3,
	AS50_RELOAD,
	AS50_DRAW
};

class CWeaponAS50 : public CBasePlayerWeapon
{
public:
	void Spawn( void );
	void Precache( void );
	int GetItemInfo( ItemInfo *p );
	int AddToPlayer( CBasePlayer *pPlayer );
	BOOL Deploy( void );
	void Holster( int skiplocal = 0 );
	void PrimaryAttack( void );
	void SecondaryAttack( void );
	void Reload( void );
	void WeaponIdle( void );
};

LINK_ENTITY_TO_CLASS( weapon_as50, CWeaponAS50 )

void CWeaponAS50::Spawn( void )
{
	Precache();
	m_iId = WEAPON_AS50;
	SET_MODEL( ENT( pev ), "models/zpmod/w_as50.mdl" );
	m_iDefaultAmmo = 5;
	FallInit();
}

void CWeaponAS50::Precache( void )
{
	PRECACHE_MODEL( "models/zpmod/v_as50.mdl" );
	PRECACHE_MODEL( "models/zpmod/p_as50.mdl" );
	PRECACHE_MODEL( "models/zpmod/w_as50.mdl" );
	PRECACHE_GENERIC( "sprites/weapon_as50.txt" );
	PRECACHE_SOUND( "weapons/as50-1.wav" );
	PRECACHE_SOUND( "weapons/as50_clipout1.wav" );
	PRECACHE_SOUND( "weapons/as50_clipout2.wav" );
	PRECACHE_SOUND( "weapons/as50_clipin1.wav" );
	PRECACHE_SOUND( "weapons/as50_clipin2.wav" );
	PRECACHE_SOUND( "weapons/as50_draw.wav" );
	PRECACHE_SOUND( "weapons/zoom.wav" );
}

int CWeaponAS50::GetItemInfo( ItemInfo *p )
{
	p->pszName = STRING( pev->classname );
	p->pszAmmo1 = "50BMG";
	p->iMaxAmmo1 = _50BMG_MAX_CARRY;
	p->pszAmmo2 = NULL;
	p->iMaxAmmo2 = -1;
	p->iMaxClip = 5;
	p->iSlot = 4;
	p->iPosition = 4;
	p->iId = m_iId = WEAPON_AS50;
	p->iWeight = 24;
	p->iFlags = 0;
	return 1;
}

int CWeaponAS50::AddToPlayer( CBasePlayer *pPlayer )
{
	if( CBasePlayerWeapon::AddToPlayer( pPlayer ) )
	{
		MESSAGE_BEGIN( MSG_ONE, gmsgWeapPickup, NULL, pPlayer->pev );
			WRITE_BYTE( m_iId );
		MESSAGE_END();
		return TRUE;
	}
	return FALSE;
}

BOOL CWeaponAS50::Deploy( void )
{
	return DefaultDeploy( "models/zpmod/v_as50.mdl", "models/zpmod/p_as50.mdl", AS50_DRAW, "bow" );
}

void CWeaponAS50::Holster( int skiplocal )
{
	if( m_pPlayer && m_pPlayer->m_iFOV != 0 )
	{
		m_fInZoom = FALSE;
		m_pPlayer->pev->fov = m_pPlayer->m_iFOV = 0;
	}
	CBasePlayerWeapon::Holster( skiplocal );
}

void CWeaponAS50::SecondaryAttack( void )
{
	if( m_pPlayer->m_iFOV == 0 )
	{
		m_fInZoom = TRUE;
		m_pPlayer->pev->fov = m_pPlayer->m_iFOV = 38;
	}
	else if( m_pPlayer->m_iFOV == 38 )
	{
		m_fInZoom = TRUE;
		m_pPlayer->pev->fov = m_pPlayer->m_iFOV = 14;
	}
	else
	{
		m_fInZoom = FALSE;
		m_pPlayer->pev->fov = m_pPlayer->m_iFOV = 0;
	}
	EMIT_SOUND_DYN( ENT( m_pPlayer->pev ), CHAN_ITEM, "weapons/zoom.wav", 0.3f, ATTN_NORM, 0, 100 );
	m_flNextSecondaryAttack = UTIL_WeaponTimeBase() + 0.3f;
}

void CWeaponAS50::PrimaryAttack( void )
{
	if( m_pPlayer->pev->waterlevel == 3 || m_iClip <= 0 )
	{
		if( m_iClip <= 0 && !m_fFireOnEmpty ) { Reload(); return; }
		PlayEmptySound();
		m_flNextPrimaryAttack = UTIL_WeaponTimeBase() + 0.2f;
		return;
	}

	m_iClip--;
	m_pPlayer->pev->effects |= EF_MUZZLEFLASH;
	m_pPlayer->SetAnimation( PLAYER_ATTACK1 );

	UTIL_MakeVectors( m_pPlayer->pev->v_angle + m_pPlayer->pev->punchangle );
	Vector vecSrc = m_pPlayer->GetGunPosition();
	Vector vecAim = m_pPlayer->GetAutoaimVector( AUTOAIM_2DEGREES );
	Vector spread = (m_pPlayer->m_iFOV != 0) ? g_vecZero : VECTOR_CONE_3DEGREES;
	m_pPlayer->FireBullets( 1, vecSrc, vecAim, spread, 8192, BULLET_PLAYER_357, 1, 165, m_pPlayer->pev );
	CSMoE_ApplyKnockback( m_pPlayer, 420.0f );

	SendWeaponAnim( AS50_SHOOT1 + RANDOM_LONG( 0, 2 ), 0 );
	EMIT_SOUND_DYN( ENT( m_pPlayer->pev ), CHAN_WEAPON, "weapons/as50-1.wav", 0.95f, ATTN_NORM, 0, RANDOM_LONG( 97, 103 ) );
	m_pPlayer->pev->punchangle.x -= 4.0f;
	m_flNextPrimaryAttack = UTIL_WeaponTimeBase() + 0.45f;
	m_flTimeWeaponIdle = UTIL_WeaponTimeBase() + 2.5f;
}

void CWeaponAS50::Reload( void )
{
	if( m_pPlayer->m_iFOV != 0 )
	{
		m_fInZoom = FALSE;
		m_pPlayer->pev->fov = m_pPlayer->m_iFOV = 0;
	}
	DefaultReload( 5, AS50_RELOAD, 3.0f );
}

void CWeaponAS50::WeaponIdle( void )
{
	if( m_flTimeWeaponIdle > UTIL_WeaponTimeBase() ) return;
	SendWeaponAnim( AS50_IDLE, 0 );
	m_flTimeWeaponIdle = UTIL_WeaponTimeBase() + 6.0f;
}

// ===========================================================================
// 6. MG3 Machine Gun (weapon_mg3)
// ===========================================================================
enum mg3_e
{
	MG3_IDLE1 = 0,
	MG3_SHOOT1,
	MG3_SHOOT2,
	MG3_RELOAD,
	MG3_DRAW
};

class CWeaponMG3 : public CBasePlayerWeapon
{
public:
	void Spawn( void );
	void Precache( void );
	int GetItemInfo( ItemInfo *p );
	int AddToPlayer( CBasePlayer *pPlayer );
	BOOL Deploy( void );
	void PrimaryAttack( void );
	void Reload( void );
	void WeaponIdle( void );
};

LINK_ENTITY_TO_CLASS( weapon_mg3, CWeaponMG3 )

void CWeaponMG3::Spawn( void )
{
	Precache();
	m_iId = WEAPON_MG3;
	SET_MODEL( ENT( pev ), "models/zpmod/w_mg3.mdl" );
	m_iDefaultAmmo = 200;
	FallInit();
}

void CWeaponMG3::Precache( void )
{
	PRECACHE_MODEL( "models/zpmod/v_mg3.mdl" );
	PRECACHE_MODEL( "models/zpmod/p_mg3.mdl" );
	PRECACHE_MODEL( "models/zpmod/w_mg3.mdl" );
	PRECACHE_GENERIC( "sprites/weapon_mg3.txt" );
	PRECACHE_SOUND( "weapons/mg3-1.wav" );
	PRECACHE_SOUND( "weapons/mg3_open.wav" );
	PRECACHE_SOUND( "weapons/mg3_clipout.wav" );
	PRECACHE_SOUND( "weapons/mg3_clipin.wav" );
	PRECACHE_SOUND( "weapons/mg3_close.wav" );
	PRECACHE_SOUND( "weapons/mg3_cliplock.wav" );
}

int CWeaponMG3::GetItemInfo( ItemInfo *p )
{
	p->pszName = STRING( pev->classname );
	p->pszAmmo1 = "556Nato";
	p->iMaxAmmo1 = _556NATO_MAX_CARRY;
	p->pszAmmo2 = NULL;
	p->iMaxAmmo2 = -1;
	p->iMaxClip = 200;
	p->iSlot = 3;
	p->iPosition = 4;
	p->iId = m_iId = WEAPON_MG3;
	p->iWeight = 25;
	p->iFlags = 0;
	return 1;
}

int CWeaponMG3::AddToPlayer( CBasePlayer *pPlayer )
{
	if( CBasePlayerWeapon::AddToPlayer( pPlayer ) )
	{
		MESSAGE_BEGIN( MSG_ONE, gmsgWeapPickup, NULL, pPlayer->pev );
			WRITE_BYTE( m_iId );
		MESSAGE_END();
		return TRUE;
	}
	return FALSE;
}

BOOL CWeaponMG3::Deploy( void )
{
	return DefaultDeploy( "models/zpmod/v_mg3.mdl", "models/zpmod/p_mg3.mdl", MG3_DRAW, "mp5" );
}

void CWeaponMG3::PrimaryAttack( void )
{
	if( m_pPlayer->pev->waterlevel == 3 || m_iClip <= 0 )
	{
		if( m_iClip <= 0 && !m_fFireOnEmpty ) { Reload(); return; }
		PlayEmptySound();
		m_flNextPrimaryAttack = UTIL_WeaponTimeBase() + 0.15f;
		return;
	}

	m_iClip--;
	m_pPlayer->pev->effects |= EF_MUZZLEFLASH;
	m_pPlayer->SetAnimation( PLAYER_ATTACK1 );

	UTIL_MakeVectors( m_pPlayer->pev->v_angle + m_pPlayer->pev->punchangle );
	Vector vecSrc = m_pPlayer->GetGunPosition();
	Vector vecAim = m_pPlayer->GetAutoaimVector( AUTOAIM_5DEGREES );
	m_pPlayer->FireBullets( 1, vecSrc, vecAim, VECTOR_CONE_4DEGREES, 8192, BULLET_PLAYER_MP5, 2, 38, m_pPlayer->pev );
	CSMoE_ApplyKnockback( m_pPlayer, 110.0f );

	SendWeaponAnim( (m_iClip & 1) ? MG3_SHOOT1 : MG3_SHOOT2, 0 );
	EMIT_SOUND_DYN( ENT( m_pPlayer->pev ), CHAN_WEAPON, "weapons/mg3-1.wav", 0.85f, ATTN_NORM, 0, RANDOM_LONG( 97, 103 ) );
	m_pPlayer->pev->punchangle.x -= 0.9f;
	m_flNextPrimaryAttack = UTIL_WeaponTimeBase() + 0.088f;
	m_flTimeWeaponIdle = UTIL_WeaponTimeBase() + 2.0f;
}

void CWeaponMG3::Reload( void )
{
	DefaultReload( 200, MG3_RELOAD, 4.5f );
}

void CWeaponMG3::WeaponIdle( void )
{
	if( m_flTimeWeaponIdle > UTIL_WeaponTimeBase() ) return;
	SendWeaponAnim( MG3_IDLE1, 0 );
	m_flTimeWeaponIdle = UTIL_WeaponTimeBase() + 6.0f;
}
