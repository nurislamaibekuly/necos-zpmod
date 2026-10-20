#include "extdll.h"
#include "util.h"
#include "cbase.h"
#include "enginecallback.h"
#include "weapons.h"
#include "player.h"
#include "zpmod.h"

// zpmod: buildable turret kit.
//   Humans get one kit per infection round (infection start + ZPMakeHuman).
//   Primary attack traces 128 units and, on a flat surface, spawns a
//   zp_turret (see turret.cpp) that shows a hologram for 20 seconds while
//   it builds, then comes online and shoots zombies only.
//   The kit itself uses the tripmine view/player/world models; the
//   tripmine_e sequence layout applies 1:1 (idle=0, arm=2, draw=6).

#define TURRET_KIT_DEFAULT_GIVE		1
#define TURRET_KIT_PLACE_RANGE		128.0f
#define TURRET_KIT_MIN_FLOOR_Z		0.7f	// placement surface must be near-horizontal
#define TURRET_KIT_MAX_ACTIVE		1		// one live turret per builder

enum turretkit_e
{
	TURRET_KIT_IDLE1 = 0,	// TRIPMINE_IDLE1
	TURRET_KIT_IDLE2,		// TRIPMINE_IDLE2
	TURRET_KIT_ARM1,		// TRIPMINE_ARM1
	TURRET_KIT_ARM2,		// TRIPMINE_ARM2
	TURRET_KIT_FIDGET,		// TRIPMINE_FIDGET
	TURRET_KIT_HOLSTER,		// TRIPMINE_HOLSTER
	TURRET_KIT_DRAW			// TRIPMINE_DRAW
};

class CWeaponTurretKit : public CBasePlayerWeapon
{
public:
	void Spawn( void );
	void Precache( void );
	int GetItemInfo( ItemInfo *p );
	BOOL Deploy( void );
	void Holster( int skiplocal = 0 );
	void PrimaryAttack( void );
	void WeaponIdle( void );
	void ScheduleEmptyRetire( void );
	void RetireThink( void );

protected:
	bool m_bRetirePending;
};

LINK_ENTITY_TO_CLASS( weapon_turret, CWeaponTurretKit )

void CWeaponTurretKit::Spawn( void )
{
	Precache();
	m_iId = WEAPON_TURRET;
	// no w_tripmine.mdl ships with the game; the stock tripmine uses the
	// view model as its dropped/world entity model too
	SET_MODEL( ENT( pev ), "models/v_tripmine.mdl" );
	m_iDefaultAmmo = TURRET_KIT_DEFAULT_GIVE;
	FallInit();
}

void CWeaponTurretKit::Precache( void )
{
	PRECACHE_MODEL( "models/v_tripmine.mdl" );
	PRECACHE_MODEL( "models/p_tripmine.mdl" );
	PRECACHE_GENERIC( "sprites/weapon_turret.txt" );
	PRECACHE_SOUND( "weapons/mine_activate.wav" );
	// runs zp_turret's Precache: sentry.mdl + every turret sound, so the
	// first mid-round placement never has to precache cold
	UTIL_PrecacheOther( "zp_turret" );
}

int CWeaponTurretKit::GetItemInfo( ItemInfo *p )
{
	p->pszName = STRING( pev->classname );
	p->pszAmmo1 = "Turret";
	p->iMaxAmmo1 = TURRET_KIT_MAX_CARRY;
	p->pszAmmo2 = NULL;
	p->iMaxAmmo2 = -1;
	p->iMaxClip = WEAPON_NOCLIP;
	p->iSlot = 0;
	p->iPosition = 1;
	p->iId = m_iId = WEAPON_TURRET;
	p->iWeight = TURRET_KIT_WEIGHT;
	p->iFlags = ITEM_FLAG_LIMITINWORLD | ITEM_FLAG_EXHAUSTIBLE;

	return 1;
}

BOOL CWeaponTurretKit::Deploy( void )
{
	m_bRetirePending = false;
	return DefaultDeploy( "models/v_tripmine.mdl", "models/p_tripmine.mdl", TURRET_KIT_DRAW, "trip" );
}

void CWeaponTurretKit::Holster( int skiplocal /* = 0 */ )
{
	if( m_pPlayer == NULL )
	{
		SetThink( &CBaseEntity::SUB_Remove );
		pev->nextthink = gpGlobals->time + 0.1f;
		return;
	}

	m_pPlayer->m_flNextAttack = UTIL_WeaponTimeBase() + 0.5f;

	if( m_pPlayer->m_rgAmmo[m_iPrimaryAmmoType] )
	{
		SendWeaponAnim( TURRET_KIT_HOLSTER );
	}
	else
	{
		// Never DestroyItem() from Holster() -- see zpmod_grenades.cpp:
		// unlink the HUD bit now and retire from next frame's think queue.
		m_pPlayer->pev->weapons &= ~( 1 << m_iId );
		ScheduleEmptyRetire();
	}

	EMIT_SOUND( ENT( m_pPlayer->pev ), CHAN_WEAPON, "common/null.wav", 1.0f, ATTN_NORM );
}

void CWeaponTurretKit::ScheduleEmptyRetire( void )
{
	if( m_bRetirePending || !m_pPlayer )
		return;

	m_bRetirePending = true;
	pev->nextthink = gpGlobals->time + 0.01f;
	SetThink( &CWeaponTurretKit::RetireThink );
}

void CWeaponTurretKit::RetireThink( void )
{
	SetThink( NULL );
	pev->nextthink = 0.0f;
	m_bRetirePending = false;

	// runs at the top of a server frame: safe to unlink + remove here
	if( m_pPlayer && m_pPlayer->m_rgAmmo[m_iPrimaryAmmoType] == 0 )
	{
		m_pPlayer->pev->weapons &= ~( 1 << m_iId );
		m_pPlayer->RemovePlayerItem( this, false );
		m_pPlayer = NULL;
	}

	SetTouch( NULL );
	SetThink( &CBaseEntity::SUB_Remove );
	pev->nextthink = gpGlobals->time + 0.1f;
}

void CWeaponTurretKit::PrimaryAttack( void )
{
	if( !m_pPlayer )
		return;

	int idx = ENTINDEX( m_pPlayer->edict() );

	if( !ZPIsHuman( m_pPlayer->edict() ) )
	{
		ZP_Trace( "ZPTURRET place refused by=%d zombie\n", idx );
		return; // zombies can never place turrets
	}

	if( g_round.state != RS_ACTIVE )
	{
		ClientPrint( m_pPlayer->pev, HUD_PRINTTALK, "Wait for the round to start" );
		m_flNextPrimaryAttack = GetNextAttackDelay( 0.5f );
		return;
	}

	if( idx >= 1 && idx <= gpGlobals->maxClients &&
		g_players[idx].frozenUntil > gpGlobals->time )
		return; // frozen players can't build either

	if( m_pPlayer->m_rgAmmo[m_iPrimaryAmmoType] <= 0 )
	{
		// spent kit still selected (e.g. the holder keeps the attack button
		// held): ItemPostFrame never reaches WeaponIdle, so retire it here.
		ZP_Trace( "ZPTURRET place refused by=%d ammo=0\n", idx );
		m_pPlayer->pev->weapons &= ~( 1 << m_iId );
		ScheduleEmptyRetire();
		RetireWeapon();
		return;
	}

	// one live turret per builder (kits are single-use, so this only bites
	// on re-give paths like ZPMakeHuman while an old turret is still up)
	int live = 0;
	for( int i = gpGlobals->maxClients + 1; i < gpGlobals->maxEntities; i++ )
	{
		edict_t *ed = INDEXENT( i );
		if( !ed || ed->free )
			continue;
		if( ZPTurretBuilder( ed ) == m_pPlayer->edict() )
			live++;
	}
	if( live >= TURRET_KIT_MAX_ACTIVE )
	{
		ZP_Trace( "ZPTURRET place refused by=%d live=%d\n", idx, live );
		ClientPrint( m_pPlayer->pev, HUD_PRINTTALK, "Your turret is already deployed" );
		m_flNextPrimaryAttack = GetNextAttackDelay( 0.5f );
		return;
	}

	UTIL_MakeVectors( m_pPlayer->pev->v_angle + m_pPlayer->pev->punchangle );
	Vector vecSrc = m_pPlayer->GetGunPosition();

	TraceResult tr;
	UTIL_TraceLine( vecSrc, vecSrc + gpGlobals->v_forward * TURRET_KIT_PLACE_RANGE,
		ignore_monsters, ENT( m_pPlayer->pev ), &tr );

	if( tr.flFraction >= 1.0f || !tr.pHit )
	{
		ZP_Trace( "ZPTURRET place refused by=%d nosurface frac=%.2f\n", idx, tr.flFraction );
		ClientPrint( m_pPlayer->pev, HUD_PRINTTALK, "Aim at a surface to place the turret" );
		m_flNextPrimaryAttack = GetNextAttackDelay( 0.5f );
		return;
	}

	if( tr.vecPlaneNormal.z < TURRET_KIT_MIN_FLOOR_Z )
	{
		ZP_Trace( "ZPTURRET place refused by=%d slope=%.2f\n", idx, tr.vecPlaneNormal.z );
		ClientPrint( m_pPlayer->pev, HUD_PRINTTALK, "Turrets need a flat surface" );
		m_flNextPrimaryAttack = GetNextAttackDelay( 0.5f );
		return;
	}

	CBaseEntity *pHit = CBaseEntity::Instance( tr.pHit );
	if( pHit )
	{
		const char *cls = STRING( pHit->pev->classname );
		if( pHit->pev->flags & FL_CONVEYOR )
		{
			ClientPrint( m_pPlayer->pev, HUD_PRINTTALK, "Can't place a turret here" );
			m_flNextPrimaryAttack = GetNextAttackDelay( 0.5f );
			return;
		}
		if( cls && !strncmp( cls, "zp_turret", 9 ) )
		{
			ClientPrint( m_pPlayer->pev, HUD_PRINTTALK, "Can't stack turrets" );
			m_flNextPrimaryAttack = GetNextAttackDelay( 0.5f );
			return;
		}
	}

	// shipped monster_sentry placements in stock maps all sit exactly on
	// the floor (BSP probe: origin_z == floor_z), so the model origin is at
	// the base -- a one-unit lift only avoids starting inside the brush.
	Vector vecPos = tr.vecEndPos + tr.vecPlaneNormal * 1.0f;
	Vector vecAngles = Vector( 0, m_pPlayer->pev->v_angle.y, 0 );

	CBaseEntity::Create( "zp_turret", vecPos, vecAngles, m_pPlayer->edict() );

	m_pPlayer->m_rgAmmo[m_iPrimaryAmmoType]--;
	m_pPlayer->SetAnimation( PLAYER_ATTACK1 );
	SendWeaponAnim( TURRET_KIT_ARM1 );
	EMIT_SOUND( ENT( m_pPlayer->pev ), CHAN_WEAPON, "weapons/mine_activate.wav", 1.0f, ATTN_NORM );

	ClientPrint( m_pPlayer->pev, HUD_PRINTTALK, "Turret placed: building..." );

	ZP_Trace( "ZPTURRET kit placed by=%d pos=%.1f %.1f %.1f ammo=%d\n",
		idx, vecPos.x, vecPos.y, vecPos.z,
		m_pPlayer->m_rgAmmo[m_iPrimaryAmmoType] );

	m_flNextPrimaryAttack = GetNextAttackDelay( 1.0f );
	m_flTimeWeaponIdle = UTIL_WeaponTimeBase() + 1.0f;

	if( !m_pPlayer->m_rgAmmo[m_iPrimaryAmmoType] )
	{
		// last kit placed: hold the arm animation, then WeaponIdle() retires
		m_flTimeWeaponIdle = m_flNextSecondaryAttack = m_flNextPrimaryAttack = GetNextAttackDelay( 1.0f );
	}
}

// Owner-side teardown: a turret belongs to the human who placed it, so when
// that player is infected or dies the gun goes with them. ZPCleanupWorld
// would otherwise only sweep it at the next round transition, leaving a live
// turret shooting zombies for the rest of the round after its builder turned.
void ZPRemoveTurretsOf( edict_t *pBuilder )
{
	if( !pBuilder )
		return;

	int removed = 0;
	for( int i = gpGlobals->maxClients + 1; i < gpGlobals->maxEntities; i++ )
	{
		edict_t *ed = INDEXENT( i );
		if( !ed || ed->free )
			continue;
		if( ZPTurretBuilder( ed ) != pBuilder )
			continue;

		UTIL_Remove( CBaseEntity::Instance( ed ) );
		removed++;
	}

	if( removed )
		ZP_Trace( "ZPTURRET removed %d turret(s) of builder=%d\n", removed, ENTINDEX( pBuilder ) );
}

void CWeaponTurretKit::WeaponIdle( void )
{
	if( !m_pPlayer )
		return;

	if( m_flTimeWeaponIdle > UTIL_WeaponTimeBase() )
		return;

	if( m_pPlayer->m_rgAmmo[m_iPrimaryAmmoType] )
	{
		// land on the hold clip once; IDLE1 loops on the client, so never
		// re-send it (same rule as the grenade hold clips) -- that would
		// snap the loop back to frame 0 every idle period
		if( m_pPlayer->pev->weaponanim != TURRET_KIT_IDLE1 &&
			m_pPlayer->pev->weaponanim != TURRET_KIT_IDLE2 &&
			m_pPlayer->pev->weaponanim != TURRET_KIT_FIDGET )
		{
			SendWeaponAnim( TURRET_KIT_IDLE1 );
		}

		m_flTimeWeaponIdle = UTIL_WeaponTimeBase() + UTIL_SharedRandomFloat( m_pPlayer->random_seed, 10.0f, 15.0f );
	}
	else
	{
		// spent kit: drop from HUD + unlink via the safe next-frame retire
		m_pPlayer->pev->weapons &= ~( 1 << m_iId );
		ScheduleEmptyRetire();
		RetireWeapon();
	}
}
