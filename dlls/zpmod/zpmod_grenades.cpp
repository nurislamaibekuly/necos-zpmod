#include "extdll.h"
#include "util.h"
#include "cbase.h"
#include "enginecallback.h"
#include "game.h"
#include "edict.h"
#include "decals.h"
#include "weapons.h"
#include "player.h"
#include "zpmod.h"

// Zombie Plague style throwables for humans (CS 1.6 style).
//  - Molotov: ignites on impact, burns zombies on contact (lingering fire)
//  - Freeze grenade: flash-freezes zombies solid for a few seconds (blue shell)
// Both are handed to humans on spawn. Zombies can never equip or throw them
// and the effects only ever hurt/impair zombies.
//
//  - Infection bomb: zombie-only throwable that converts every human inside
//    the blast radius (last human alive is killed instead), ZP5.0 style.
//
// The view/player/world models are 4-sequence GoldSrc grenade rigs:
//   idle=0, pullpin=1, throw=2, deploy=3

#define ZP_MOLOTOV_DEFAULT_GIVE		1
#define ZP_FREEZEBOMB_DEFAULT_GIVE	1
#define ZP_INFECTIONBOMB_DEFAULT_GIVE	1
#define ZP_MOLOTOV_MAX_CARRY		1
#define ZP_FREEZEBOMB_MAX_CARRY		1
#define ZP_INFECTIONBOMB_MAX_CARRY	1

#define ZP_MOLOTOV_DAMAGE			110.0f
#define ZP_MOLOTOV_RADIUS			240.0f
#define ZP_MOLOTOV_FUSE				2.2f
#define ZP_FIRE_DURATION			4.0f	// how long the lingering fire patch itself stays on the ground
#define ZP_FIRE_RADIUS				150.0f
#define ZP_FIRE_TICK				0.5f
#define ZP_FIRE_DMG					18.0f
#define ZP_ZOMBIE_BURN_DURATION		5.0f	// how long a zombie that touched fire keeps burning, even after leaving it

#define ZP_FREEZE_RADIUS			300.0f
#define ZP_FREEZE_TIME				10.0f

#define ZP_INFECTION_RADIUS			240.0f

// safety net for the hold-to-throw state machine: if the attack button's
// release is never seen (lag, alt-tab, weapon switch, menu), force the
// throw after this many seconds instead of leaving the weapon stuck.
#define ZP_GRENADE_MAX_HOLD			5.0f

//---------------------------------------------------------------
// persistent zombie burning: once a zombie is touched by molotov fire
// (initial blast or lingering patch), it keeps taking damage for
// ZP_ZOMBIE_BURN_DURATION seconds even after leaving the flames.
// Tracked by client slot rather than on the player class itself, since
// we don't want to touch player.h for this.
//---------------------------------------------------------------
#define ZP_MAX_BURN_SLOTS 33

static float      g_flZombieBurnUntil[ZP_MAX_BURN_SLOTS];
static float       g_flZombieNextBurnTick[ZP_MAX_BURN_SLOTS];
static entvars_t *g_pevZombieBurnAttacker[ZP_MAX_BURN_SLOTS];

static CBaseEntity *s_pTicker = NULL;

// Called on every ServerActivate / map load. The previous level's ticker
// entity was destroyed by the map change and its edict memory may be reused
// for new entities (or freed entirely), so both the singleton pointer and the
// burn bookkeeping MUST be dropped here, otherwise the first molotov/freeze
// explosion of the next level dereferences stale state.
void ZPModGrenadeInit( void )
{
	for( int i = 0; i < ZP_MAX_BURN_SLOTS; i++ )
	{
		g_flZombieBurnUntil[i] = 0.0f;
		g_flZombieNextBurnTick[i] = 0.0f;
		g_pevZombieBurnAttacker[i] = NULL;
	}

	s_pTicker = NULL;
}

static void ZPIgniteZombie( int slot, entvars_t *attacker, float duration )
{
	if( slot < 1 || slot >= ZP_MAX_BURN_SLOTS )
		return;

	// (re)igniting refreshes the timer rather than stacking it
	g_flZombieBurnUntil[slot] = gpGlobals->time + duration;
	g_pevZombieBurnAttacker[slot] = attacker;

	if( g_flZombieNextBurnTick[slot] < gpGlobals->time )
		g_flZombieNextBurnTick[slot] = gpGlobals->time + ZP_FIRE_TICK;
}

// singleton entity that ticks damage/fx for every currently-burning zombie,
// independent of any specific molotov patch's lifetime.
class CZPBurnTicker : public CBaseEntity
{
public:
	void Spawn( void );
	void EXPORT TickThink( void );
};

LINK_ENTITY_TO_CLASS( zp_burn_ticker, CZPBurnTicker )

void CZPBurnTicker::Spawn( void )
{
	pev->classname = MAKE_STRING( "zp_burn_ticker" );
	pev->solid = SOLID_NOT;
	pev->effects |= EF_NODRAW;
	SetThink( &CZPBurnTicker::TickThink );
	pev->nextthink = gpGlobals->time + 0.1f;
}

void CZPBurnTicker::TickThink( void )
{
	pev->nextthink = gpGlobals->time + 0.1f;

	for( int i = 1; i <= gpGlobals->maxClients && i < ZP_MAX_BURN_SLOTS; i++ )
	{
		if( g_flZombieBurnUntil[i] <= gpGlobals->time )
			continue;

		edict_t *ed = INDEXENT( i );
		if( !ZPIsPlayerConnected( ed ) || !ZPIsZombie( ed ) || ZPIsDead( ed ) )
		{
			g_flZombieBurnUntil[i] = 0.0f; // stop tracking disconnected/dead/un-zombified slots
			continue;
		}

		CBasePlayer *p = (CBasePlayer *)GET_PRIVATE( ed );
		if( !p || !p->IsAlive() )
			continue;

		// small fire sprite on the burning zombie so it reads visually even
		// after it has walked out of the flame patch
		MESSAGE_BEGIN( MSG_PVS, SVC_TEMPENTITY, ed->v.origin );
			WRITE_BYTE( TE_SPRITE );
			WRITE_COORD( ed->v.origin.x );
			WRITE_COORD( ed->v.origin.y );
			WRITE_COORD( ed->v.origin.z + RANDOM_FLOAT( 0, 24 ) );
			WRITE_SHORT( MODEL_INDEX( "sprites/fire.spr" ) );
			WRITE_BYTE( 14 );  // scale
			WRITE_BYTE( 200 ); // brightness
		MESSAGE_END();

		if( gpGlobals->time >= g_flZombieNextBurnTick[i] )
		{
			g_flZombieNextBurnTick[i] += ZP_FIRE_TICK;
			entvars_t *attacker = g_pevZombieBurnAttacker[i] ? g_pevZombieBurnAttacker[i] : VARS( INDEXENT( 0 ) );
			p->TakeDamage( attacker, attacker, ZP_FIRE_DMG, DMG_BURN );
		}
	}
}

static void ZPEnsureBurnTicker( void )
{
	if( s_pTicker && !FNullEnt( s_pTicker->edict() ) )
		return;

	s_pTicker = GetClassPtr( (CZPBurnTicker *)NULL );
	s_pTicker->Spawn();
}

//---------------------------------------------------------------
// shared projectile: flies, then explodes the moment it lands so
// the effect targets only zombies.
//---------------------------------------------------------------
class CZPGrenade : public CBaseEntity
{
public:
	enum Type
	{
		MOLOTOV = 0,
		FREEZE = 1,
		INFECTION = 2
	};

	void Spawn( void );
	void Precache( void );
	void EXPORT GrenadeThink( void );
	void EXPORT FireThink( void );
	void EXPORT GrenadeTouch( CBaseEntity *pOther );

	static CZPGrenade *Shoot( entvars_t *pevOwner, int iType, Vector vecStart, Vector vecVelocity );
	void Explode( void );
	void ExplodeMolotov( void );
	void ExplodeFreeze( void );
	void ExplodeInfection( void );

	int m_iType;
	float m_flGroundTime;
	float m_flPrime;
	float m_flDie;
	float m_flNextDamage;

private:
	entvars_t *m_pevAttacker;
};

LINK_ENTITY_TO_CLASS( zp_grenade, CZPGrenade )

void CZPGrenade::Spawn( void )
{
	Precache();
	pev->movetype = MOVETYPE_BOUNCE;
	pev->solid = SOLID_BBOX;

	if( m_iType == MOLOTOV )
		SET_MODEL( ENT( pev ), "models/zpmod/w_molotov.mdl" );
	else if( m_iType == INFECTION )
		SET_MODEL( ENT( pev ), "models/zpmod/w_hegrenade.mdl" );
	else
		SET_MODEL( ENT( pev ), "models/zpmod/w_freezebomb.mdl" );

	pev->dmg = m_iType == MOLOTOV ? ZP_MOLOTOV_DAMAGE : 0;
	UTIL_SetSize( pev, Vector( 0, 0, 0 ), Vector( 0, 0, 0 ) );
	pev->gravity = 0.5f;
	pev->friction = 0.8f;
}

void CZPGrenade::Precache( void )
{
	PRECACHE_MODEL( "models/zpmod/w_molotov.mdl" );
	PRECACHE_MODEL( "models/zpmod/w_freezebomb.mdl" );
	PRECACHE_MODEL( "models/zpmod/w_hegrenade.mdl" );
	PRECACHE_MODEL( "sprites/explode1.spr" );
	PRECACHE_MODEL( "sprites/fire.spr" );
	PRECACHE_MODEL( "sprites/shockwave.spr" );
	PRECACHE_SOUND( "zpmod/zombi_bomb_exp.wav" );
	PRECACHE_SOUND( "zpmod/zombi_bomb_bounce_1.wav" );
	PRECACHE_SOUND( "zpmod/zombi_bomb_bounce_2.wav" );
}

CZPGrenade *CZPGrenade::Shoot( entvars_t *pevOwner, int iType, Vector vecStart, Vector vecVelocity )
{
	CZPGrenade *pGrenade = GetClassPtr( (CZPGrenade *)NULL );
	pGrenade->m_iType = iType;
	pGrenade->Spawn();
	UTIL_SetOrigin( pGrenade->pev, vecStart );
	pGrenade->pev->velocity = vecVelocity;

	ZP_Trace("ZPGREN Shoot type=%d edict=%d src=(%.0f %.0f %.0f) vel=(%.0f %.0f %.0f) model=%s\n",
		iType, pGrenade->entindex(), vecStart.x, vecStart.y, vecStart.z,
		vecVelocity.x, vecVelocity.y, vecVelocity.z,
		STRING(pGrenade->pev->model));
	pGrenade->pev->angles = UTIL_VecToAngles( pGrenade->pev->velocity );
	pGrenade->pev->owner = ENT( pevOwner );
	pGrenade->m_pevAttacker = pevOwner;
	pGrenade->m_flGroundTime = 0.0f;
	pGrenade->m_flPrime = gpGlobals->time + 0.25f;

	// tumble through the air
	pGrenade->pev->avelocity.x = RANDOM_FLOAT( -200, -500 );

	pGrenade->SetTouch( &CZPGrenade::GrenadeTouch );
	pGrenade->SetThink( &CZPGrenade::GrenadeThink );
	pGrenade->pev->nextthink = gpGlobals->time + 0.05f;

	return pGrenade;
}

void CZPGrenade::GrenadeTouch( CBaseEntity *pOther )
{
	if( !pOther )
		return;
	if( gpGlobals->time < m_flPrime )
		return;
	if( pOther->edict() == pev->owner )
		return;
	if( FClassnameIs( pOther->pev, "zp_grenade" ) )
		return;

	if( pOther->IsBSPModel() || pOther->IsPlayer() )
	{
		Explode();
		return;
	}

	// bounce off pickup-able items, just play the clink
	if( m_iType == INFECTION )
	{
		EMIT_SOUND( ENT( pev ), CHAN_WEAPON, RANDOM_LONG( 0, 1 ) == 0 ? "zpmod/zombi_bomb_bounce_1.wav" : "zpmod/zombi_bomb_bounce_2.wav", 0.9f, ATTN_NORM );
		return;
	}
	int r = RANDOM_LONG( 0, 2 );
	EMIT_SOUND( ENT( pev ), CHAN_WEAPON, r == 0 ? "weapons/grenade_hit1.wav" : r == 1 ? "weapons/grenade_hit2.wav" : "weapons/grenade_hit3.wav", 0.9f, ATTN_NORM );
}

void CZPGrenade::GrenadeThink( void )
{
	pev->nextthink = gpGlobals->time + 0.05f;

	// fuse safety so it always goes off even if it never lands
	if( gpGlobals->time > m_flPrime + ZP_MOLOTOV_FUSE )
	{
		Explode();
		return;
	}

	if( pev->waterlevel > 1 )
	{
		Explode();
		return;
	}

	if( pev->flags & FL_ONGROUND )
	{
		m_flGroundTime += 0.05f;
		if( m_flGroundTime > 0.1f )
		{
			Explode();
			return;
		}
	}
	else
	{
		m_flGroundTime = 0.0f;
	}
}

void CZPGrenade::Explode( void )
{
	if( m_iType == MOLOTOV )
		ExplodeMolotov();
	else if( m_iType == INFECTION )
		ExplodeInfection();
	else
		ExplodeFreeze();
}

static void ZPGrenadeDamageZombies( entvars_t *attacker, const Vector &origin, float radius, float dmg, int bits, bool bIgnite = false )
{
	if( !attacker )
		attacker = VARS( INDEXENT( 0 ) );

	for( int i = 1; i <= gpGlobals->maxClients; i++ )
	{
		edict_t *ed = INDEXENT( i );
		if( !ZPIsPlayerConnected( ed ) || !ZPIsZombie( ed ) || ZPIsDead( ed ) )
			continue;

		CBasePlayer *p = (CBasePlayer *)GET_PRIVATE( ed );
		if( !p || !p->IsAlive() )
			continue;

		float dist = ( ed->v.origin - origin ).Length();
		if( dist > radius )
			continue;

		if( bIgnite )
			ZPIgniteZombie( i, attacker, ZP_ZOMBIE_BURN_DURATION );

		float d = dmg * ( 1.0f - dist / radius );
		p->TakeDamage( attacker, attacker, d, bits );
	}

	if( bIgnite )
		ZPEnsureBurnTicker();
}

// like ZPGrenadeDamageZombies but only (re)ignites zombies in range - no
// direct damage. Used by the lingering fire patch: the patch itself just
// keeps refreshing the burn timer for anyone standing in it, and the
// persistent CZPBurnTicker singleton does the actual damage ticks so the
// burn keeps going after a zombie walks out of the fire.
static void ZPGrenadeIgniteZombiesInRadius( entvars_t *attacker, const Vector &origin, float radius )
{
	if( !attacker )
		attacker = VARS( INDEXENT( 0 ) );

	for( int i = 1; i <= gpGlobals->maxClients; i++ )
	{
		edict_t *ed = INDEXENT( i );
		if( !ZPIsPlayerConnected( ed ) || !ZPIsZombie( ed ) || ZPIsDead( ed ) )
			continue;

		if( ( ed->v.origin - origin ).Length() > radius )
			continue;

		ZPIgniteZombie( i, attacker, ZP_ZOMBIE_BURN_DURATION );
	}

	ZPEnsureBurnTicker();
}

void CZPGrenade::ExplodeMolotov( void )
{
	Vector origin = pev->origin;
	entvars_t *attacker = m_pevAttacker ? m_pevAttacker : VARS( INDEXENT( 0 ) );

	EMIT_SOUND( ENT( pev ), CHAN_WEAPON, "weapons/explode3.wav", 1.0f, ATTN_NORM );

	MESSAGE_BEGIN( MSG_PVS, SVC_TEMPENTITY, origin );
		WRITE_BYTE( TE_EXPLOSION );
		WRITE_COORD( origin.x );
		WRITE_COORD( origin.y );
		WRITE_COORD( origin.z );
		WRITE_SHORT( MODEL_INDEX( "sprites/explode1.spr" ) );
		WRITE_BYTE( 22 );  // scale
		WRITE_BYTE( 10 );  // framerate
		WRITE_BYTE( 0 );   // flags
	MESSAGE_END();

	MESSAGE_BEGIN( MSG_PVS, SVC_TEMPENTITY, origin );
		WRITE_BYTE( TE_DLIGHT );
		WRITE_COORD( origin.x );
		WRITE_COORD( origin.y );
		WRITE_COORD( origin.z + 8 );
		WRITE_BYTE( 28 );  // radius
		WRITE_BYTE( 255 ); // r
		WRITE_BYTE( 140 ); // g
		WRITE_BYTE( 30 );  // b
		WRITE_BYTE( 8 );   // life
		WRITE_BYTE( 5 );   // decay
	MESSAGE_END();

	// scorch the floor
	TraceResult tr;
	UTIL_TraceLine( origin, origin - Vector( 0, 0, 64 ), ignore_monsters, ENT( pev ), &tr );
	if( tr.flFraction < 1.0f )
		UTIL_DecalTrace( &tr, DECAL_SCORCH1 );

	ZPGrenadeDamageZombies( attacker, origin, ZP_MOLOTOV_RADIUS, ZP_MOLOTOV_DAMAGE, DMG_BURN, true /* bIgnite */ );

	// become a lingering burning patch so it keeps hurting any zombie walking in
	pev->movetype = MOVETYPE_NONE;
	pev->solid = SOLID_NOT;
	pev->effects = EF_NODRAW;
	pev->velocity = Vector( 0, 0, 0 );
	m_flDie = gpGlobals->time + ZP_FIRE_DURATION;
	m_flNextDamage = gpGlobals->time;
	SetThink( &CZPGrenade::FireThink );
	pev->nextthink = gpGlobals->time + 0.1f;
}

void CZPGrenade::FireThink( void )
{
	pev->nextthink = gpGlobals->time + 0.1f;

	if( gpGlobals->time > m_flDie )
	{
		UTIL_Remove( this );
		return;
	}

	Vector flame = pev->origin + Vector( RANDOM_FLOAT( -45, 45 ), RANDOM_FLOAT( -45, 45 ), RANDOM_FLOAT( 0, 48 ) );
	MESSAGE_BEGIN( MSG_PVS, SVC_TEMPENTITY, pev->origin );
		WRITE_BYTE( TE_SPRITE );
		WRITE_COORD( flame.x );
		WRITE_COORD( flame.y );
		WRITE_COORD( flame.z );
		WRITE_SHORT( MODEL_INDEX( "sprites/fire.spr" ) );
		WRITE_BYTE( RANDOM_LONG( 18, 28 ) );  // scale
		WRITE_BYTE( 255 );                     // brightness
	MESSAGE_END();

	if( gpGlobals->time >= m_flNextDamage )
	{
		m_flNextDamage += ZP_FIRE_TICK;
		entvars_t *attacker = m_pevAttacker ? m_pevAttacker : VARS( INDEXENT( 0 ) );
		ZPGrenadeIgniteZombiesInRadius( attacker, pev->origin, ZP_FIRE_RADIUS );
	}
}

void CZPGrenade::ExplodeFreeze( void )
{
	Vector origin = pev->origin;

	EMIT_SOUND( ENT( pev ), CHAN_WEAPON, "weapons/explode4.wav", 1.0f, ATTN_NORM );

	MESSAGE_BEGIN( MSG_PVS, SVC_TEMPENTITY, origin );
		WRITE_BYTE( TE_EXPLOSION );
		WRITE_COORD( origin.x );
		WRITE_COORD( origin.y );
		WRITE_COORD( origin.z );
		WRITE_SHORT( MODEL_INDEX( "sprites/shockwave.spr" ) );
		WRITE_BYTE( 30 );  // scale
		WRITE_BYTE( 10 );  // framerate
		WRITE_BYTE( 0 );   // flags
	MESSAGE_END();

	MESSAGE_BEGIN( MSG_PVS, SVC_TEMPENTITY, origin );
		WRITE_BYTE( TE_DLIGHT );
		WRITE_COORD( origin.x );
		WRITE_COORD( origin.y );
		WRITE_COORD( origin.z + 8 );
		WRITE_BYTE( 30 );  // radius
		WRITE_BYTE( 150 ); // r
		WRITE_BYTE( 210 ); // g
		WRITE_BYTE( 255 ); // b
		WRITE_BYTE( 8 );   // life
		WRITE_BYTE( 5 );   // decay
	MESSAGE_END();

	MESSAGE_BEGIN( MSG_PVS, SVC_TEMPENTITY, origin );
		WRITE_BYTE( TE_SPRITE );
		WRITE_COORD( origin.x );
		WRITE_COORD( origin.y );
		WRITE_COORD( origin.z + 20 );
		WRITE_SHORT( MODEL_INDEX( "sprites/shockwave.spr" ) );
		WRITE_BYTE( 30 );  // scale
		WRITE_BYTE( 200 ); // brightness
	MESSAGE_END();

	// chill the floor a touch for flavour
	TraceResult tr;
	UTIL_TraceLine( origin, origin - Vector( 0, 0, 64 ), ignore_monsters, ENT( pev ), &tr );
	if( tr.flFraction < 1.0f )
		UTIL_DecalTrace( &tr, DECAL_SCORCH2 );

	// freeze every zombie in range (existing frozenUntil machinery
	// pins them in place with a blue shell)
	for( int i = 1; i <= gpGlobals->maxClients; i++ )
	{
		edict_t *ed = INDEXENT( i );
		if( !ZPIsPlayerConnected( ed ) || !ZPIsZombie( ed ) || ZPIsDead( ed ) )
			continue;

		CBasePlayer *p = (CBasePlayer *)GET_PRIVATE( ed );
		if( !p || !p->IsAlive() )
			continue;

		float dist = ( ed->v.origin - origin ).Length();
		if( dist > ZP_FREEZE_RADIUS )
			continue;

		g_players[i].frozenUntil = gpGlobals->time + ZP_FREEZE_TIME;

		// frost puff on the frozen victim
		Vector frostPuff = ed->v.origin + Vector( 0, 0, 40 );
		MESSAGE_BEGIN( MSG_PVS, SVC_TEMPENTITY, frostPuff );
			WRITE_BYTE( TE_SPRITE );
			WRITE_COORD( frostPuff.x );
			WRITE_COORD( frostPuff.y );
			WRITE_COORD( frostPuff.z );
			WRITE_SHORT( MODEL_INDEX( "sprites/shockwave.spr" ) );
			WRITE_BYTE( 16 );
			WRITE_BYTE( 200 );
		MESSAGE_END();
	}

	UTIL_Remove( this );
}

static int ZPCountAliveHumans( void )
{
	int count = 0;
	for( int i = 1; i <= gpGlobals->maxClients; i++ )
	{
		edict_t *ed = INDEXENT( i );
		if( !ZPIsPlayerConnected( ed ) || !ZPIsHuman( ed ) || ZPIsDead( ed ) )
			continue;
		CBasePlayer *p = (CBasePlayer *)GET_PRIVATE( ed );
		if( !p || !p->IsAlive() )
			continue;
		count++;
	}
	return count;
}

void CZPGrenade::ExplodeInfection( void )
{
	Vector origin = pev->origin;
	entvars_t *attacker = m_pevAttacker ? m_pevAttacker : VARS( INDEXENT( 0 ) );

	EMIT_SOUND( ENT( pev ), CHAN_WEAPON, "zpmod/zombi_bomb_exp.wav", 1.0f, ATTN_NORM );

	// green flash, ZP5.0 style
	MESSAGE_BEGIN( MSG_PVS, SVC_TEMPENTITY, origin );
		WRITE_BYTE( TE_DLIGHT );
		WRITE_COORD( origin.x );
		WRITE_COORD( origin.y );
		WRITE_COORD( origin.z + 8 );
		WRITE_BYTE( 30 );  // radius
		WRITE_BYTE( 30 );  // r
		WRITE_BYTE( 200 ); // g
		WRITE_BYTE( 30 );  // b
		WRITE_BYTE( 8 );   // life
		WRITE_BYTE( 5 );   // decay
	MESSAGE_END();

	// expanding green rings
	for( int ring = 1; ring <= 3; ring++ )
	{
		MESSAGE_BEGIN( MSG_PVS, SVC_TEMPENTITY, origin );
			WRITE_BYTE( TE_BEAMCYLINDER );
			WRITE_COORD( origin.x );
			WRITE_COORD( origin.y );
			WRITE_COORD( origin.z );
			WRITE_COORD( origin.x );
			WRITE_COORD( origin.y );
			WRITE_COORD( origin.z + 385.0f * ring );
			WRITE_SHORT( MODEL_INDEX( "sprites/shockwave.spr" ) );
			WRITE_BYTE( 0 );   // startframe
			WRITE_BYTE( 0 );   // framerate
			WRITE_BYTE( 4 );   // life
			WRITE_BYTE( 60 );  // width
			WRITE_BYTE( 0 );   // noise
			WRITE_BYTE( 0 );   // r
			WRITE_BYTE( 200 ); // g
			WRITE_BYTE( 0 );   // b
			WRITE_BYTE( 200 ); // brightness
			WRITE_BYTE( 0 );   // speed
		MESSAGE_END();
	}

	ZP_Trace("ZPGREN INFECTION explode at (%.0f %.0f %.0f)\n", origin.x, origin.y, origin.z);

	int humansLeft = ZPCountAliveHumans();

	for( int i = 1; i <= gpGlobals->maxClients; i++ )
	{
		edict_t *ed = INDEXENT( i );
		if( !ZPIsPlayerConnected( ed ) || !ZPIsHuman( ed ) || ZPIsDead( ed ) )
			continue;

		CBasePlayer *p = (CBasePlayer *)GET_PRIVATE( ed );
		if( !p || !p->IsAlive() )
			continue;

		float dist = ( ed->v.origin - origin ).Length();
		if( dist > ZP_INFECTION_RADIUS )
			continue;

		ZP_Trace("ZPGREN INFECTION target slot=%d dist=%.0f humansLeft=%d\n", i, dist, humansLeft);

		if( humansLeft > 1 )
		{
			ZPInfectPlayer( ed, true );
			humansLeft--;
			continue;
		}

		// last human alive is killed instead of infected, ZP5.0 style
		p->TakeDamage( attacker, attacker, 10000.0f, DMG_GENERIC );
	}

	UTIL_Remove( this );
}

//---------------------------------------------------------------
// base throw weapon
//---------------------------------------------------------------
class CZPThrowGrenade : public CBasePlayerWeapon
{
public:
	void PrimaryAttack( void );
	void ItemPostFrame( void );
	void Throw( void );
	void WeaponIdle( void );
	BOOL CanHolster( void ) { return ( m_flStartThrow == 0 ); }
	void Holster( int skiplocal = 0 );
	virtual BOOL Deploy( void );

	virtual const char *ViewModelPath( void ) const = 0;
	virtual const char *PlayerModelPath( void ) const = 0;
	virtual int PullPinAnim( void ) const = 0;
	virtual int ThrowAnim( void ) const { return 2; }
	virtual int DeployAnim( void ) const { return 3; }
	virtual int IdleAnim( void ) const { return 0; }
	virtual void ShootProjectile( entvars_t *pevOwner, const Vector &vecSrc, const Vector &vecThrow ) = 0;

	// who may use this throwable (humans for molotov/freezebomb,
	// zombies for the infection bomb)
	virtual bool UseAllowed( edict_t *ed ) const { return ZPIsHuman( ed ); }

	// per-weapon sound hooks (NULL = silent, keeps molotov/freezebomb behaviour)
	virtual const char *DeploySound( void ) const { return NULL; }
	virtual const char *PullSound( void ) const { return NULL; }
	virtual const char *ThrowSound( void ) const { return NULL; }

protected:
	float m_flStartThrow;
	float m_flReleaseThrow;
};

BOOL CZPThrowGrenade::Deploy( void )
{
	m_flReleaseThrow = -1;
	if( DeploySound() )
		EMIT_SOUND( ENT( m_pPlayer->pev ), CHAN_WEAPON, DeploySound(), 1.0f, ATTN_NORM );
	return DefaultDeploy( ViewModelPath(), PlayerModelPath(), DeployAnim(), "crowbar" );
}

void CZPThrowGrenade::Holster( int skiplocal /* = 0 */ )
{
	m_pPlayer->m_flNextAttack = UTIL_WeaponTimeBase() + 0.5f;

	if( m_pPlayer->m_rgAmmo[m_iPrimaryAmmoType] )
	{
		SendWeaponAnim( 0 );
	}
	else
	{
		// no more grenades!
		m_pPlayer->pev->weapons &= ~( 1 << m_iId );
		DestroyItem();
	}

	if( m_flStartThrow )
	{
		m_flStartThrow = 0.0f;
		m_flReleaseThrow = 0.0f;
	}

	EMIT_SOUND( ENT( m_pPlayer->pev ), CHAN_WEAPON, "common/null.wav", 1.0f, ATTN_NORM );
}

void CZPThrowGrenade::PrimaryAttack( void )
{
	if( m_pPlayer && !UseAllowed( m_pPlayer->edict() ) )
		return; // wrong team for this throwable

	if( m_flStartThrow || m_pPlayer->m_rgAmmo[m_iPrimaryAmmoType] <= 0 )
		return;

	// Start cooking: pull the pin and wait for the button to be released
	// (or for ZP_GRENADE_MAX_HOLD to elapse) before the grenade actually
	// leaves the player's hand. See ItemPostFrame().
	m_flStartThrow = gpGlobals->time;

	SendWeaponAnim( PullPinAnim() );
	if( PullSound() )
		EMIT_SOUND( ENT( m_pPlayer->pev ), CHAN_WEAPON, PullSound(), 1.0f, ATTN_NORM );
}

void CZPThrowGrenade::ItemPostFrame( void )
{
	if( m_flStartThrow )
	{
		// Still cooking. Throw the instant the attack button is released,
		// or after ZP_GRENADE_MAX_HOLD regardless of button state. The
		// timeout is purely a safety net: it means a missed button-up
		// event (lag, alt-tab, weapon switch, a menu eating input) can
		// only ever delay the throw by a few seconds, never leave
		// m_flStartThrow stuck forever the way the old release-driven
		// version could.
		bool bReleased = !( m_pPlayer->pev->button & IN_ATTACK );
		bool bTimedOut = ( gpGlobals->time - m_flStartThrow ) >= ZP_GRENADE_MAX_HOLD;

		if( bReleased || bTimedOut )
			Throw();

		return; // don't fall through to the normal attack/idle dispatch while cooking
	}

	CBasePlayerWeapon::ItemPostFrame();
}

void CZPThrowGrenade::Throw( void )
{
	// NOTE: deliberately NOT adding pev->punchangle here. punchangle is the
	// view-kick from taking damage, and it isn't clamped - getting hit by
	// zombies mid-throw (most likely exactly when you're standing still
	// fighting rather than walking away) could skew this enough to throw
	// the grenade back at yourself instead of where you were aiming.
	Vector angThrow = m_pPlayer->pev->v_angle;

	if( angThrow.x < 0.0f )
		angThrow.x = -10.0f + angThrow.x * ( ( 90.0f - 10.0f ) / 90.0f );
	else
		angThrow.x = -10.0f + angThrow.x * ( ( 90.0f + 10.0f ) / 90.0f );

	float flVel = ( 90.0f - angThrow.x ) * 6.5f;
	if( flVel > 1000.0f )
		flVel = 1000.0f;

	UTIL_MakeVectors( angThrow );

	Vector vecSrc = m_pPlayer->pev->origin + m_pPlayer->pev->view_ofs + gpGlobals->v_forward * 16.0f;

	Vector vecThrow = gpGlobals->v_forward * flVel + m_pPlayer->pev->velocity;

	ZP_Trace("ZPGREN THROW ammo=%d vel=%f\n",
		m_pPlayer->m_rgAmmo[m_iPrimaryAmmoType], flVel);

	ShootProjectile( m_pPlayer->pev, vecSrc, vecThrow );

	SendWeaponAnim( ThrowAnim() );
	if( ThrowSound() )
		EMIT_SOUND( ENT( m_pPlayer->pev ), CHAN_WEAPON, ThrowSound(), 1.0f, ATTN_NORM );

	// player "shoot" animation
	m_pPlayer->SetAnimation( PLAYER_ATTACK1 );

	m_pPlayer->m_rgAmmo[m_iPrimaryAmmoType]--;

	m_flStartThrow = 0.0f;
	m_flNextPrimaryAttack = GetNextAttackDelay( 0.5f );
	m_flTimeWeaponIdle = UTIL_WeaponTimeBase() + 0.5f;

	if( !m_pPlayer->m_rgAmmo[m_iPrimaryAmmoType] )
	{
		// just threw last grenade, let the throw animation finish then retire
		m_flTimeWeaponIdle = m_flNextSecondaryAttack = m_flNextPrimaryAttack = GetNextAttackDelay( 0.5f );
	}
}

void CZPThrowGrenade::WeaponIdle( void )
{
	if( m_flTimeWeaponIdle > UTIL_WeaponTimeBase() )
		return;

	if( m_pPlayer->m_rgAmmo[m_iPrimaryAmmoType] )
	{
		SendWeaponAnim( DeployAnim() );
		m_flTimeWeaponIdle = UTIL_WeaponTimeBase() + UTIL_SharedRandomFloat( m_pPlayer->random_seed, 10.0f, 15.0f );
	}
	else
	{
		RetireWeapon();
	}
}

//---------------------------------------------------------------
// molotov
//---------------------------------------------------------------
enum molotov_anim_e
{
	MOLOTOV_IDLE = 0,
	MOLOTOV_PINPULL,
	MOLOTOV_THROW,
	MOLOTOV_DEPLOY
};

class CWeaponMolotov : public CZPThrowGrenade
{
public:
	void Spawn( void );
	void Precache( void );
	int GetItemInfo( ItemInfo *p );

	const char *ViewModelPath( void ) const { return "models/zpmod/v_molotov.mdl"; }
	const char *PlayerModelPath( void ) const { return "models/zpmod/p_molotov.mdl"; }
	int PullPinAnim( void ) const { return MOLOTOV_PINPULL; }
	int ThrowAnim( void ) const { return MOLOTOV_THROW; }
	int DeployAnim( void ) const { return MOLOTOV_DEPLOY; }
	int IdleAnim( void ) const { return MOLOTOV_IDLE; }
	void ShootProjectile( entvars_t *pevOwner, const Vector &vecSrc, const Vector &vecThrow );
};

LINK_ENTITY_TO_CLASS( weapon_molotov, CWeaponMolotov )

void CWeaponMolotov::Spawn( void )
{
	Precache();
	m_iId = WEAPON_MOLOTOV;
	SET_MODEL( ENT( pev ), "models/zpmod/w_molotov.mdl" );
	m_iDefaultAmmo = ZP_MOLOTOV_DEFAULT_GIVE;
	FallInit();
}

void CWeaponMolotov::Precache( void )
{
	PRECACHE_MODEL( "models/zpmod/v_molotov.mdl" );
	PRECACHE_MODEL( "models/zpmod/p_molotov.mdl" );
	PRECACHE_MODEL( "models/zpmod/w_molotov.mdl" );
	PRECACHE_MODEL( "sprites/explode1.spr" );
	PRECACHE_MODEL( "sprites/fire.spr" );
	PRECACHE_GENERIC( "sprites/weapon_molotov.txt" );
	PRECACHE_SOUND( "weapons/explode3.wav" );
	PRECACHE_SOUND( "weapons/explode4.wav" );
	PRECACHE_SOUND( "weapons/grenade_hit1.wav" );
	PRECACHE_SOUND( "weapons/grenade_hit2.wav" );
	PRECACHE_SOUND( "weapons/grenade_hit3.wav" );
}

int CWeaponMolotov::GetItemInfo( ItemInfo *p )
{
	p->pszName = STRING( pev->classname );
	p->pszAmmo1 = "Molotov";
	p->iMaxAmmo1 = ZP_MOLOTOV_MAX_CARRY;
	p->pszAmmo2 = NULL;
	p->iMaxAmmo2 = -1;
	p->iMaxClip = WEAPON_NOCLIP;
	p->iSlot = 0;
	p->iPosition = 2;
	p->iId = m_iId = WEAPON_MOLOTOV;
	p->iWeight = MOLOTOV_WEIGHT;
	p->iFlags = ITEM_FLAG_LIMITINWORLD | ITEM_FLAG_EXHAUSTIBLE;

	return 1;
}

void CWeaponMolotov::ShootProjectile( entvars_t *pevOwner, const Vector &vecSrc, const Vector &vecThrow )
{
	CZPGrenade::Shoot( pevOwner, CZPGrenade::MOLOTOV, vecSrc, vecThrow );
}

//---------------------------------------------------------------
// freeze grenade
//---------------------------------------------------------------
enum freezebomb_anim_e
{
	FREEZEBOMB_IDLE = 0,
	FREEZEBOMB_PINPULL,
	FREEZEBOMB_THROW,
	FREEZEBOMB_DEPLOY
};

class CWeaponFreezebomb : public CZPThrowGrenade
{
public:
	void Spawn( void );
	void Precache( void );
	int GetItemInfo( ItemInfo *p );

	const char *ViewModelPath( void ) const { return "models/zpmod/v_freezebomb.mdl"; }
	const char *PlayerModelPath( void ) const { return "models/zpmod/p_freezebomb.mdl"; }
	int PullPinAnim( void ) const { return FREEZEBOMB_PINPULL; }
	int ThrowAnim( void ) const { return FREEZEBOMB_THROW; }
	int DeployAnim( void ) const { return FREEZEBOMB_DEPLOY; }
	int IdleAnim( void ) const { return FREEZEBOMB_IDLE; }
	void ShootProjectile( entvars_t *pevOwner, const Vector &vecSrc, const Vector &vecThrow );
};

LINK_ENTITY_TO_CLASS( weapon_freezebomb, CWeaponFreezebomb )

void CWeaponFreezebomb::Spawn( void )
{
	Precache();
	m_iId = WEAPON_FREEZEBOMB;
	SET_MODEL( ENT( pev ), "models/zpmod/w_freezebomb.mdl" );
	m_iDefaultAmmo = ZP_FREEZEBOMB_DEFAULT_GIVE;
	FallInit();
}

void CWeaponFreezebomb::Precache( void )
{
	PRECACHE_MODEL( "models/zpmod/v_freezebomb.mdl" );
	PRECACHE_MODEL( "models/zpmod/p_freezebomb.mdl" );
	PRECACHE_MODEL( "models/zpmod/w_freezebomb.mdl" );
	PRECACHE_MODEL( "sprites/shockwave.spr" );
	PRECACHE_GENERIC( "sprites/weapon_freezebomb.txt" );
	PRECACHE_SOUND( "weapons/explode4.wav" );
	PRECACHE_SOUND( "weapons/explode3.wav" );
	PRECACHE_SOUND( "weapons/grenade_hit1.wav" );
	PRECACHE_SOUND( "weapons/grenade_hit2.wav" );
	PRECACHE_SOUND( "weapons/grenade_hit3.wav" );
}

int CWeaponFreezebomb::GetItemInfo( ItemInfo *p )
{
	p->pszName = STRING( pev->classname );
	p->pszAmmo1 = "FreezeBomb";
	p->iMaxAmmo1 = ZP_FREEZEBOMB_MAX_CARRY;
	p->pszAmmo2 = NULL;
	p->iMaxAmmo2 = -1;
	p->iMaxClip = WEAPON_NOCLIP;
	p->iSlot = 0;
	p->iPosition = 3;
	p->iId = m_iId = WEAPON_FREEZEBOMB;
	p->iWeight = FREEZEBOMB_WEIGHT;
	p->iFlags = ITEM_FLAG_LIMITINWORLD | ITEM_FLAG_EXHAUSTIBLE;

	return 1;
}

void CWeaponFreezebomb::ShootProjectile( entvars_t *pevOwner, const Vector &vecSrc, const Vector &vecThrow )
{
	CZPGrenade::Shoot( pevOwner, CZPGrenade::FREEZE, vecSrc, vecThrow );
}

//---------------------------------------------------------------
// infection bomb (zombies only)
//---------------------------------------------------------------
enum infectionbomb_anim_e
{
	INFECTIONBOMB_IDLE = 0,
	INFECTIONBOMB_PINPULL,
	INFECTIONBOMB_THROW,
	INFECTIONBOMB_DEPLOY
};

class CWeaponInfectionBomb : public CZPThrowGrenade
{
public:
	void Spawn( void );
	void Precache( void );
	int GetItemInfo( ItemInfo *p );

	const char *ViewModelPath( void ) const { return "models/zpmod/v_hegrenade.mdl"; }
	const char *PlayerModelPath( void ) const { return "models/zpmod/p_hegrenade.mdl"; }
	int PullPinAnim( void ) const { return INFECTIONBOMB_PINPULL; }
	int ThrowAnim( void ) const { return INFECTIONBOMB_THROW; }
	int DeployAnim( void ) const { return INFECTIONBOMB_DEPLOY; }
	int IdleAnim( void ) const { return INFECTIONBOMB_IDLE; }
	void ShootProjectile( entvars_t *pevOwner, const Vector &vecSrc, const Vector &vecThrow );

	bool UseAllowed( edict_t *ed ) const { return ZPIsZombie( ed ); }

	const char *DeploySound( void ) const { return "zpmod/zombi_bomb_deploy.wav"; }
	const char *PullSound( void ) const { return "zpmod/zombi_bomb_pull_1.wav"; }
	const char *ThrowSound( void ) const { return "zpmod/zombi_bomb_throw.wav"; }
};

LINK_ENTITY_TO_CLASS( weapon_infectionbomb, CWeaponInfectionBomb )

void CWeaponInfectionBomb::Spawn( void )
{
	Precache();
	m_iId = WEAPON_INFECTIONBOMB;
	SET_MODEL( ENT( pev ), "models/zpmod/w_hegrenade.mdl" );
	m_iDefaultAmmo = ZP_INFECTIONBOMB_DEFAULT_GIVE;
	FallInit();
}

void CWeaponInfectionBomb::Precache( void )
{
	PRECACHE_MODEL( "models/zpmod/v_hegrenade.mdl" );
	PRECACHE_MODEL( "models/zpmod/p_hegrenade.mdl" );
	PRECACHE_MODEL( "models/zpmod/w_hegrenade.mdl" );
	PRECACHE_MODEL( "sprites/shockwave.spr" );
	PRECACHE_GENERIC( "sprites/weapon_infectionbomb.txt" );
	PRECACHE_SOUND( "zpmod/zombi_bomb_deploy.wav" );
	PRECACHE_SOUND( "zpmod/zombi_bomb_pull_1.wav" );
	PRECACHE_SOUND( "zpmod/zombi_bomb_throw.wav" );
	PRECACHE_SOUND( "zpmod/zombi_bomb_exp.wav" );
	PRECACHE_SOUND( "zpmod/zombi_bomb_bounce_1.wav" );
	PRECACHE_SOUND( "zpmod/zombi_bomb_bounce_2.wav" );
}

int CWeaponInfectionBomb::GetItemInfo( ItemInfo *p )
{
	p->pszName = STRING( pev->classname );
	p->pszAmmo1 = "InfectionBomb";
	p->iMaxAmmo1 = ZP_INFECTIONBOMB_MAX_CARRY;
	p->pszAmmo2 = NULL;
	p->iMaxAmmo2 = -1;
	p->iMaxClip = WEAPON_NOCLIP;
	p->iSlot = 0;
	p->iPosition = 4;
	p->iId = m_iId = WEAPON_INFECTIONBOMB;
	p->iWeight = INFECTIONBOMB_WEIGHT;
	p->iFlags = ITEM_FLAG_LIMITINWORLD | ITEM_FLAG_EXHAUSTIBLE;

	return 1;
}

void CWeaponInfectionBomb::ShootProjectile( entvars_t *pevOwner, const Vector &vecSrc, const Vector &vecThrow )
{
	CZPGrenade::Shoot( pevOwner, CZPGrenade::INFECTION, vecSrc, vecThrow );
}