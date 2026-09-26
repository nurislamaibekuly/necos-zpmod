#include "extdll.h"
#include "util.h"
#include "cbase.h"
#include "enginecallback.h"
#include "game.h"
#include "edict.h"
#include "decals.h"
#include "weapons.h"
#include "player.h"
#include "shake.h"
#include "zpmod.h"
#include <math.h>

#define ZP_SUPPLYBOX_MODEL          "models/zpmod/supplybox.mdl"
#define ZP_SUPPLYBOX_ICON           "sprites/zpmod/icon_supplybox.spr"
#define ZP_SUPPLYBOX_SND_DROP       "zpmod/supplybox_drop.wav"
#define ZP_SUPPLYBOX_SND_PICKUP     "zpmod/supplybox_pickup.wav"

#define ZP_SUPPLYBOX_MAX_ROUND      4
#define ZP_SUPPLYBOX_PER_WAVE       2
#define ZP_SUPPLYBOX_FIRST_WAVE     30.0f
#define ZP_SUPPLYBOX_WAVE_INTERVAL  30.0f
#define ZP_SUPPLYBOX_BOX_GAP        0.5f
#define ZP_SUPPLYBOX_LIFETIME       90.0f
#define ZP_SUPPLYBOX_TOUCH_GRACE    0.75f
#define ZP_SUPPLYBOX_COOLDOWN       2.0f
#define ZP_SUPPLYBOX_KILL_Z         -4096.0f
#define ZP_SUPPLYBOX_MAX_POINTS     64
#define ZP_SUPPLYBOX_PLACE_ATTEMPTS 14
#define ZP_SUPPLYBOX_SPAWN_JITTER_MIN 64.0f
#define ZP_SUPPLYBOX_SPAWN_JITTER_MAX 384.0f
#define ZP_SUPPLYBOX_SPAWN_MIN_DIST 320.0f
#define ZP_SUPPLYBOX_SPAWN_MAX_DIST 1100.0f
#define ZP_SUPPLYBOX_JITTER_MIN     24.0f
#define ZP_SUPPLYBOX_JITTER_MAX     256.0f
#define ZP_SUPPLYBOX_PROBE_UP       96.0f
#define ZP_SUPPLYBOX_PROBE_DOWN     256.0f
#define ZP_SUPPLYBOX_CLEAR_HEIGHT   72.0f
#define ZP_SUPPLYBOX_FLOOR_EPSILON  0.5f
#define ZP_SUPPLYBOX_DROP_FALLBACK   0.4f
#define ZP_SUPPLYBOX_TICK            0.01f

#define ZP_DEG_TO_RAD               0.0174532925f

enum ZPSupplyBoxReward
{
	ZPBOX_HEALTH = 0,
	ZPBOX_ARMOR,
	ZPBOX_AMMO,
	ZPBOX_ADRENALINE,
	ZPBOX_MOLOTOV,
	ZPBOX_FROST,
	ZPBOX_RIFLE,
	ZPBOX_REWARD_COUNT
};

static int    s_spawnedThisRound = 0;
static int    s_pendingThisWave = 0;
static float  s_nextWaveTime = 0.0f;
static float  s_nextBoxTime = 0.0f;
static float  s_pickupCooldown[33] = { 0 };
static int    s_testReward = 0;

// Runtime-swappable beacon sprite so the beacon art can be A/B tested against
// known-good additive sprites without a rebuild (see zp_supplyboxsprite).
static char  s_beaconSprite[64] = ZP_SUPPLYBOX_ICON;

static const char *s_pRewardNames[ZPBOX_REWARD_COUNT] =
{
	"health",
	"armor",
	"ammo",
	"adrenaline",
	"molotov",
	"frost",
	"rifle"
};

static void ZPSupplyBoxRewardHud( CBasePlayer *pPlayer, edict_t *ed, const char *label, int r, int g, int b )
{
	hudtextparms_t info;
	memset( &info, 0, sizeof( info ) );
	info.channel = 4;
	info.x = 0.14f;
	info.y = 0.72f;
	info.a1 = 255;
	info.fadeinTime = 0.05f;
	info.fadeoutTime = 0.4f;
	info.holdTime = 2.0f;
	info.r1 = r;
	info.g1 = g;
	info.b1 = b;

	UTIL_HudMessage( CBaseEntity::Instance( ed ), info, label );
}

// Wall-projection marker, ported from the [ZE] SupplyBox AMXX plugin.
//
// This does not defeat the depth buffer. It traces a line from the player's
// eye to the box, then places the marker at the point where that line meets
// geometry, minus a small margin. The marker therefore sits in open air
// between the camera and the wall being looked at, so nothing occludes it. It
// only reads as see-through because it lands on the visible surface, lined up
// with the hidden box. That is also why it needs the view-cone gate: the marker
// is only meaningful while you are facing the target.
//
// This was originally sent as a resent TE_SPRITE temp entity, matching the
// AMXX reference exactly. That was tried at every combination of
// MSG_PVS/MSG_ONE/MSG_ONE_UNRELIABLE and every send rate from 10Hz to 100Hz,
// with a verified-correct model index and projected position, and it never
// displayed except on a paused frame - a genuine property of resending
// TE_SPRITE continuously on this build, not something fixable from the call
// site (TE_SPRITE is documented as a one-shot "plays 1 cycle" effect, not a
// sustained one). A persistent entity sidesteps the problem: its position
// goes out as ordinary networked entity state, which is the one form already
// confirmed to display reliably.
//
// Trade-off: one marker entity per player slot, not per (player, box) pair,
// so only the nearest in-view box is shown at a time rather than every box
// simultaneously. It is also technically visible to any client whose PVS
// includes it, not only the player it was computed for - since it always sits
// a short distance in front of that player's own eyes, another player
// standing close by could occasionally glimpse it too. Both are acceptable
// for a location marker; a literal per-box-per-player version would cost
// maxplayers * maxboxes edicts for no real benefit here.
#define ZP_SUPPLYBOX_ICON_UPDATE    0.03f
#define ZP_SUPPLYBOX_ICON_MARGIN    10.0f
#define ZP_SUPPLYBOX_ICON_MIN_DIST  16.0f
#define ZP_SUPPLYBOX_ICON_HEIGHT    40.0f
#define ZP_SUPPLYBOX_ICON_CONE      0.35f
// pev->scale on a sprite entity is a multiplier of its native pixel size in
// world units (1.0 = original size), not the TE_SPRITE scale byte's
// byte/100 percentage - the two are not interchangeable units.
#define ZP_SUPPLYBOX_MARKER_SCALE   0.15f

static float s_iconNext[33] = { 0 };

class CZPSupplyBoxMarker : public CBaseEntity
{
public:
	void Spawn( void );
	void Hide( void );
	void ShowAt( const Vector &pos );
};

LINK_ENTITY_TO_CLASS( zp_supplybox_marker, CZPSupplyBoxMarker )

void CZPSupplyBoxMarker::Spawn( void )
{
	pev->classname = MAKE_STRING( "zp_supplybox_marker" );
	pev->solid = SOLID_NOT;
	pev->movetype = MOVETYPE_NONE;
	pev->takedamage = DAMAGE_NO;
	pev->health = 1.0f;

	pev->rendermode = kRenderGlow;
	pev->renderfx = kRenderFxNoDissipation;
	pev->renderamt = 255.0f;
	pev->rendercolor = Vector( 255.0f, 255.0f, 255.0f );
	pev->scale = ZP_SUPPLYBOX_MARKER_SCALE;
	pev->effects |= EF_NODRAW;

	SET_MODEL( ENT( pev ), s_beaconSprite );
	UTIL_SetSize( pev, Vector( -1.0f, -1.0f, -1.0f ), Vector( 1.0f, 1.0f, 1.0f ) );
}

void CZPSupplyBoxMarker::Hide( void )
{
	pev->effects |= EF_NODRAW;
}

void CZPSupplyBoxMarker::ShowAt( const Vector &pos )
{
	pev->effects &= ~EF_NODRAW;
	UTIL_SetOrigin( pev, pos );
}

static CZPSupplyBoxMarker *s_pMarker[33] = { NULL };

static CZPSupplyBoxMarker *ZPSupplyBoxGetMarker( int slot )
{
	if( s_pMarker[slot] && s_pMarker[slot]->edict() && !s_pMarker[slot]->edict()->free && !FNullEnt( s_pMarker[slot]->edict() ) )
		return s_pMarker[slot];

	CZPSupplyBoxMarker *pMarker = GetClassPtr( (CZPSupplyBoxMarker *)NULL );
	pMarker->Spawn();
	s_pMarker[slot] = pMarker;
	return pMarker;
}

static bool ZPSupplyBoxProjectMarker( CBasePlayer *pPlayer, const Vector &boxOrigin, Vector &out )
{
	Vector eye = pPlayer->pev->origin + pPlayer->pev->view_ofs;
	Vector target = boxOrigin + Vector( 0.0f, 0.0f, ZP_SUPPLYBOX_ICON_HEIGHT );

	Vector toTarget = target - eye;
	float dist = toTarget.Length();
	if( dist < 1.0f )
		return false;

	Vector dir = toTarget / dist;

	UTIL_MakeAimVectors( pPlayer->pev->v_angle );
	if( DotProduct( gpGlobals->v_forward, dir ) < ZP_SUPPLYBOX_ICON_CONE )
		return false;

	TraceResult tr;
	UTIL_TraceLine( eye, target, ignore_monsters, pPlayer->edict(), &tr );

	float toWall = tr.flFraction * dist - ZP_SUPPLYBOX_ICON_MARGIN;
	if( toWall > dist )
		toWall = dist;
	if( toWall < ZP_SUPPLYBOX_ICON_MIN_DIST )
		toWall = ZP_SUPPLYBOX_ICON_MIN_DIST;

	out = eye + dir * toWall;
	return true;
}

void ZPSupplyBoxIconUpdate( void )
{
	if( !MODEL_INDEX( s_beaconSprite ) )
		return;

	for( int slot = 1; slot <= gpGlobals->maxClients; slot++ )
	{
		edict_t *ed = INDEXENT( slot );
		if( !ed || ed->free || FNullEnt( ed ) || !ZPIsPlayerConnected( ed ) || ZPIsDead( ed ) || ZPIsZombie( ed ) )
		{
			if( s_pMarker[slot] ) s_pMarker[slot]->Hide();
			continue;
		}

		CBasePlayer *pPlayer = (CBasePlayer *)GET_PRIVATE( ed );
		if( !pPlayer || !pPlayer->IsAlive() )
		{
			if( s_pMarker[slot] ) s_pMarker[slot]->Hide();
			continue;
		}

		if( ZP_SUPPLYBOX_ICON_UPDATE > 0.0f && gpGlobals->time < s_iconNext[ slot ] )
			continue;
		s_iconNext[ slot ] = gpGlobals->time + ZP_SUPPLYBOX_ICON_UPDATE;

		Vector eye = pPlayer->pev->origin + pPlayer->pev->view_ofs;
		bool found = false;
		Vector bestPos;
		float bestDist = 0.0f;

		for( int i = gpGlobals->maxClients + 1; i < gpGlobals->maxEntities; i++ )
		{
			edict_t *box = INDEXENT( i );
			if( !box || box->free || FNullEnt( box ) || !FClassnameIs( box, "zp_supplybox" ) )
				continue;

			Vector pos;
			if( !ZPSupplyBoxProjectMarker( pPlayer, box->v.origin, pos ) )
				continue;

			float d = ( box->v.origin - eye ).Length();
			if( !found || d < bestDist )
			{
				found = true;
				bestDist = d;
				bestPos = pos;
			}
		}

		if( found )
			ZPSupplyBoxGetMarker( slot )->ShowAt( bestPos );
		else if( s_pMarker[slot] )
			s_pMarker[slot]->Hide();
	}
}

// ZPSupplyBoxIconUpdate() needs a guaranteed high-frequency tick independent
// of whatever cadence the mod's main frame hook happens to call
// ZPSupplyBoxThink() at (that hook is written for the wave/spawn timers,
// which only need second-level precision, not per-frame). This entity gives
// the marker its own fast think so it isn't hostage to that.
class CZPSupplyBoxIconTicker : public CBaseEntity
{
public:
	void Spawn( void );
	void EXPORT TickThink( void );
};

LINK_ENTITY_TO_CLASS( zp_supplybox_iconticker, CZPSupplyBoxIconTicker )

void CZPSupplyBoxIconTicker::Spawn( void )
{
	pev->classname = MAKE_STRING( "zp_supplybox_iconticker" );
	pev->solid = SOLID_NOT;
	pev->movetype = MOVETYPE_NONE;
	pev->effects |= EF_NODRAW;

	SetThink( &CZPSupplyBoxIconTicker::TickThink );
	pev->nextthink = gpGlobals->time + ZP_SUPPLYBOX_ICON_UPDATE;
}

void CZPSupplyBoxIconTicker::TickThink( void )
{
	pev->nextthink = gpGlobals->time + ZP_SUPPLYBOX_ICON_UPDATE;
	ZPSupplyBoxIconUpdate();
}

static void ZPSupplyBoxEnsureIconTicker( void )
{
	static CBaseEntity *s_pTicker = NULL;
	if( s_pTicker && s_pTicker->edict() && !s_pTicker->edict()->free && !FNullEnt( s_pTicker->edict() ) )
		return;

	edict_t *pTickerEdict = CREATE_NAMED_ENTITY( MAKE_STRING( "zp_supplybox_iconticker" ) );
	if( pTickerEdict && !FNullEnt( pTickerEdict ) )
	{
		CBaseEntity *pTicker = CBaseEntity::Instance( pTickerEdict );
		if( pTicker && pTicker->pev )
		{
			DispatchSpawn( pTickerEdict );
			s_pTicker = pTicker;
		}
	}
}

class CZPSupplyBox : public CBaseEntity
{
public:
	void Spawn( void );
	void EXPORT BoxThink( void );
	void EXPORT BoxTouch( CBaseEntity *pOther );
	void GrantReward( CBasePlayer *pPlayer, edict_t *ed );

	int   m_iReward;
	float m_flRemove;
	float m_flGrace;
	float m_flSpawn;
	bool  m_bDropped;
	bool  m_bIgnoreRoundState;
	int   m_iLogged;

	void DrawPickupFlash( void );
};

LINK_ENTITY_TO_CLASS( zp_supplybox, CZPSupplyBox )

void CZPSupplyBox::Spawn( void )
{
	pev->classname = MAKE_STRING( "zp_supplybox" );
	pev->solid = SOLID_TRIGGER;
	pev->movetype = MOVETYPE_TOSS;
	pev->takedamage = DAMAGE_NO;
	pev->health = 1.0f;
	pev->iuser1 = m_iReward;

	SET_MODEL( ENT( pev ), ZP_SUPPLYBOX_MODEL );
	UTIL_SetSize( pev, Vector( -16.0f, -16.0f, 0.0f ), Vector( 16.0f, 16.0f, 36.0f ) );

	pev->angles = Vector( 0.0f, RANDOM_FLOAT( 0.0f, 360.0f ), 0.0f );
	pev->velocity = Vector( RANDOM_FLOAT( -40.0f, 40.0f ), RANDOM_FLOAT( -40.0f, 40.0f ), 0.0f );

	m_flRemove = gpGlobals->time + ZP_SUPPLYBOX_LIFETIME;
	m_flGrace = gpGlobals->time + ZP_SUPPLYBOX_TOUCH_GRACE;
	m_flSpawn = gpGlobals->time;
	m_bDropped = false;
	m_iLogged = 0;

	SetTouch( &CZPSupplyBox::BoxTouch );
	SetThink( &CZPSupplyBox::BoxThink );
	pev->nextthink = gpGlobals->time + ZP_SUPPLYBOX_TICK;

	ZP_Trace( "ZPSUPPLY spawn edict=%d model=%s modelindex=%d icon=%s iconindex=%d roundstate=%d ignoreround=%d\n",
		entindex(), STRING( pev->model ), pev->modelindex,
		s_beaconSprite, MODEL_INDEX( s_beaconSprite ), g_round.state, m_bIgnoreRoundState ? 1 : 0 );
}

void CZPSupplyBox::DrawPickupFlash( void )
{
	int spriteIndex = MODEL_INDEX( s_beaconSprite );
	if( !spriteIndex )
		return;

	MESSAGE_BEGIN( MSG_PVS, SVC_TEMPENTITY, pev->origin );
		WRITE_BYTE( TE_SPRITE );
		WRITE_COORD( pev->origin.x );
		WRITE_COORD( pev->origin.y );
		WRITE_COORD( pev->origin.z + 20.0f );
		WRITE_SHORT( spriteIndex );
		WRITE_BYTE( 20 );   // scale
		WRITE_BYTE( 255 );  // brightness
	MESSAGE_END();
}

void CZPSupplyBox::BoxThink( void )
{
	pev->nextthink = gpGlobals->time + ZP_SUPPLYBOX_TICK;

	if( ( !m_bIgnoreRoundState && g_round.state != RS_ACTIVE ) || gpGlobals->time > m_flRemove || pev->origin.z < ZP_SUPPLYBOX_KILL_Z )
	{
		UTIL_Remove( this );
		return;
	}

	if( !m_bDropped && ( ( pev->flags & FL_ONGROUND ) || gpGlobals->time > m_flSpawn + ZP_SUPPLYBOX_DROP_FALLBACK ) )
	{
		m_bDropped = true;
		EMIT_AMBIENT_SOUND( ENT( pev ), pev->origin, ZP_SUPPLYBOX_SND_DROP, 1.0f, ATTN_NONE, 0, PITCH_NORM );
		ZP_Trace( "ZPSUPPLY drop sound edict=%d ground=%d\n", entindex(),
			( pev->flags & FL_ONGROUND ) ? 1 : 0 );
	}
}

void CZPSupplyBox::BoxTouch( CBaseEntity *pOther )
{
	if( !pOther || !pOther->IsPlayer() )
		return;
	if( gpGlobals->time < m_flGrace )
		return;

	edict_t *ed = pOther->edict();
	if( !ZPIsPlayerConnected( ed ) || !ZPIsHuman( ed ) || ZPIsDead( ed ) )
		return;

	int slot = ENTINDEX( ed );
	if( slot < 1 || slot > 32 )
		return;
	if( s_pickupCooldown[slot] > gpGlobals->time )
		return;

	CBasePlayer *pPlayer = (CBasePlayer *)GET_PRIVATE( ed );
	if( !pPlayer || !pPlayer->IsAlive() )
		return;

	s_pickupCooldown[slot] = gpGlobals->time + ZP_SUPPLYBOX_COOLDOWN;

	GrantReward( pPlayer, ed );

	EMIT_SOUND( ENT( pev ), CHAN_ITEM, ZP_SUPPLYBOX_SND_PICKUP, 1.0f, ATTN_NORM );
	UTIL_ParticleEffect( pev->origin + Vector( 0.0f, 0.0f, 18.0f ), Vector( 0.0f, 0.0f, 70.0f ), 70, 26 );

	DrawPickupFlash();

	UTIL_Remove( this );
}

void CZPSupplyBox::GrantReward( CBasePlayer *pPlayer, edict_t *ed )
{
	int slot = ENTINDEX( ed );
	const char *label = "SUPPLY BOX";
	const char *sfx = ZP_SUPPLYBOX_SND_PICKUP;
	int r = 255, g = 215, b = 60;

	switch( m_iReward )
	{
	case ZPBOX_HEALTH:
		pPlayer->pev->health += 50.0f;
		if( pPlayer->pev->health > pPlayer->pev->max_health )
			pPlayer->pev->health = pPlayer->pev->max_health;
		label = "SUPPLY: +50 HEALTH";
		sfx = "items/suitcharge1.wav";
		r = 80; g = 255; b = 90;
		break;

	case ZPBOX_ARMOR:
		pPlayer->pev->armorvalue += 50.0f;
		if( pPlayer->pev->armorvalue > 100.0f )
			pPlayer->pev->armorvalue = 100.0f;
		pPlayer->pev->armortype = 0.5f;
		label = "SUPPLY: +50 ARMOR";
		sfx = "items/suitcharge1.wav";
		r = 80; g = 200; b = 255;
		break;

	case ZPBOX_AMMO:
		pPlayer->GiveNamedItem( "ammo_9mmclip" );
		pPlayer->GiveNamedItem( "ammo_buckshot" );
		pPlayer->GiveNamedItem( "ammo_357" );
		pPlayer->GiveNamedItem( "ammo_gaussclip" );
		pPlayer->GiveNamedItem( "ammo_rpgclip" );
		pPlayer->GiveNamedItem( "ammo_crossbow" );
		label = "SUPPLY: AMMO PACK";
		sfx = "items/suitcharge1.wav";
		r = 255; g = 255; b = 160;
		break;

	case ZPBOX_ADRENALINE:
		g_players[slot].adrenalineUntil = gpGlobals->time + 10.0f;
		label = "SUPPLY: ADRENALINE +10s";
		sfx = "items/suitchargeno1.wav";
		r = 255; g = 200; b = 60;
		break;

	case ZPBOX_MOLOTOV:
		ZPMolotovBlast( pev->origin + Vector( 0.0f, 0.0f, 16.0f ) );
		label = "SUPPLY: NAPALM BLAST";
		r = 255; g = 140; b = 30;
		break;

	case ZPBOX_FROST:
		ZPFrostBlast( pev->origin + Vector( 0.0f, 0.0f, 16.0f ) );
		label = "SUPPLY: FROST BLAST";
		r = 120; g = 200; b = 255;
		break;

	case ZPBOX_RIFLE:
		pPlayer->GiveNamedItem( "weapon_9mmAR" );
		pPlayer->GiveNamedItem( "ammo_9mmAR" );
		pPlayer->GiveNamedItem( "ammo_9mmAR" );
		pPlayer->SelectItem( "weapon_9mmAR" );
		label = "SUPPLY: ASSAULT RIFLE";
		sfx = "items/gunpickup2.wav";
		r = 120; g = 255; b = 120;
		break;
	}

	if( sfx && sfx[0] )
		EMIT_SOUND( ed, CHAN_ITEM, sfx, 1.0f, ATTN_NORM );

	UTIL_ScreenFade( pPlayer, Vector( (float)r, (float)g, (float)b ), 0.4f, 0.2f, 255, FFADE_IN );
	UTIL_ParticleEffect( pPlayer->pev->origin + Vector( 0.0f, 0.0f, 32.0f ), Vector( 0.0f, 0.0f, 60.0f ), 60, 24 );
	ZPSupplyBoxRewardHud( pPlayer, ed, label, r, g, b );
}

class CZPSupplyBoxSpawnPoint : public CBaseEntity
{
public:
	void Spawn( void );
};

LINK_ENTITY_TO_CLASS( info_zp_supplybox, CZPSupplyBoxSpawnPoint )

void CZPSupplyBoxSpawnPoint::Spawn( void )
{
	pev->solid = SOLID_NOT;
	pev->movetype = MOVETYPE_NONE;
	pev->takedamage = DAMAGE_NO;
	pev->health = 1.0f;
	pev->effects |= EF_NODRAW;
}

static void ZPSupplyBoxClearCooldowns( void )
{
	for( int i = 0; i < 33; i++ )
		s_pickupCooldown[i] = 0.0f;
}

static void ZPSupplyBoxRemoveAll( void )
{
	for( int i = gpGlobals->maxEntities - 1; i > gpGlobals->maxClients; i-- )
	{
		edict_t *ed = INDEXENT( i );
		if( !ed || ed->free || FNullEnt( ed ) )
			continue;
		if( FClassnameIs( ed, "zp_supplybox" ) )
			UTIL_Remove( CBaseEntity::Instance( ed ) );
	}
}

static bool ZPSupplyBoxFindFloor( const Vector &spot, Vector &out, edict_t *pIgnore )
{
	TraceResult tr;
	UTIL_TraceLine( spot + Vector( 0.0f, 0.0f, ZP_SUPPLYBOX_PROBE_UP ),
		spot - Vector( 0.0f, 0.0f, ZP_SUPPLYBOX_PROBE_DOWN ),
		ignore_monsters, pIgnore ? pIgnore : ENT( 0 ), &tr );

	if( tr.flFraction >= 1.0f )
		return false;

	out = tr.vecEndPos + Vector( 0.0f, 0.0f, ZP_SUPPLYBOX_FLOOR_EPSILON );

	if( UTIL_PointContents( out ) == CONTENTS_SOLID )
		return false;
	if( UTIL_PointContents( out + Vector( 0.0f, 0.0f, ZP_SUPPLYBOX_CLEAR_HEIGHT * 0.5f ) ) == CONTENTS_SOLID )
		return false;

	return true;
}

static bool ZPSupplyBoxIsSupplyPoint( const char *cls )
{
	return !strcmp( cls, "info_zp_supplybox" );
}

static bool ZPSupplyBoxIsPlayerSpawn( const char *cls )
{
	return !strcmp( cls, "info_player_deathmatch" )
		|| !strcmp( cls, "info_player_start" )
		|| !strcmp( cls, "info_player_team1" )
		|| !strcmp( cls, "info_player_team2" )
		|| !strcmp( cls, "info_player_coop" )
		|| !strcmp( cls, "info_player_ddz" );
}

static bool ZPSupplyBoxTryAnchors( const Vector *points, int count, float jitterMin, float jitterMax, int attempts, Vector &origin )
{
	if( count <= 0 )
		return false;

	for( int attempt = 0; attempt < attempts; attempt++ )
	{
		Vector spot = points[RANDOM_LONG( 0, count - 1 )];
		float yaw = RANDOM_FLOAT( 0.0f, 360.0f );
		float dist = RANDOM_FLOAT( jitterMin, jitterMax );
		spot.x += cos( yaw * ZP_DEG_TO_RAD ) * dist;
		spot.y += sin( yaw * ZP_DEG_TO_RAD ) * dist;

		if( ZPSupplyBoxFindFloor( spot, origin, NULL ) )
			return true;
	}

	return false;
}

static bool ZPSupplyBoxPickOrigin( Vector &origin )
{
	Vector supplyPoints[ZP_SUPPLYBOX_MAX_POINTS];
	Vector spawnPoints[ZP_SUPPLYBOX_MAX_POINTS];
	int supplyCount = 0;
	int spawnCount = 0;

	for( int i = gpGlobals->maxClients + 1; i < gpGlobals->maxEntities; i++ )
	{
		edict_t *ed = INDEXENT( i );
		if( !ed || ed->free || FNullEnt( ed ) )
			continue;

		const char *cls = STRING( ed->v.classname );
		if( !cls || !cls[0] )
			continue;

		if( supplyCount < ZP_SUPPLYBOX_MAX_POINTS && ZPSupplyBoxIsSupplyPoint( cls ) )
			supplyPoints[supplyCount++] = ed->v.origin;
		else if( spawnCount < ZP_SUPPLYBOX_MAX_POINTS && ZPSupplyBoxIsPlayerSpawn( cls ) )
			spawnPoints[spawnCount++] = ed->v.origin;
	}

	if( ZPSupplyBoxTryAnchors( supplyPoints, supplyCount, ZP_SUPPLYBOX_JITTER_MIN, ZP_SUPPLYBOX_JITTER_MAX, ZP_SUPPLYBOX_PLACE_ATTEMPTS, origin ) )
		return true;

	if( ZPSupplyBoxTryAnchors( spawnPoints, spawnCount, ZP_SUPPLYBOX_SPAWN_JITTER_MIN, ZP_SUPPLYBOX_SPAWN_JITTER_MAX, ZP_SUPPLYBOX_PLACE_ATTEMPTS, origin ) )
		return true;

	for( int attempt = 0; attempt < ZP_SUPPLYBOX_PLACE_ATTEMPTS; attempt++ )
	{
		edict_t *ed = INDEXENT( RANDOM_LONG( 1, gpGlobals->maxClients ) );
		if( !ZPIsPlayerConnected( ed ) || ZPIsDead( ed ) )
			continue;
		if( !ZPIsHuman( ed ) && !ZPIsZombie( ed ) )
			continue;

		CBasePlayer *p = (CBasePlayer *)GET_PRIVATE( ed );
		if( !p || !p->IsAlive() )
			continue;

		float yaw = RANDOM_FLOAT( 0.0f, 360.0f );
		Vector forward( cos( yaw * ZP_DEG_TO_RAD ), sin( yaw * ZP_DEG_TO_RAD ), 0.0f );
		Vector spot = ed->v.origin + forward * RANDOM_FLOAT( ZP_SUPPLYBOX_SPAWN_MIN_DIST, ZP_SUPPLYBOX_SPAWN_MAX_DIST );

		if( ZPSupplyBoxFindFloor( spot, origin, ed ) )
			return true;
	}

	for( int i = 1; i <= gpGlobals->maxClients; i++ )
	{
		edict_t *ed = INDEXENT( i );
		if( !ZPIsPlayerConnected( ed ) || ZPIsDead( ed ) )
			continue;
		origin = ed->v.origin + Vector( 0.0f, 0.0f, 8.0f );
		return true;
	}

	return false;
}

static CZPSupplyBox *ZPSupplyBoxSpawnAt( const Vector &origin, int iReward, bool bIgnoreRoundState )
{
	CZPSupplyBox *pBox = GetClassPtr( (CZPSupplyBox *)NULL );
	pBox->m_iReward = iReward;
	pBox->m_bIgnoreRoundState = bIgnoreRoundState;
	pBox->Spawn();
	pBox->m_bIgnoreRoundState = bIgnoreRoundState;
	UTIL_SetOrigin( pBox->pev, origin );
	pBox->pev->velocity = Vector( RANDOM_FLOAT( -40.0f, 40.0f ), RANDOM_FLOAT( -40.0f, 40.0f ), 0.0f );
	pBox->pev->nextthink = gpGlobals->time + ZP_SUPPLYBOX_TICK;
	return pBox;
}

static void ZPSupplyBoxSpawnOne( void )
{
	Vector origin;
	if( !ZPSupplyBoxPickOrigin( origin ) )
		return;

	int iReward = RANDOM_LONG( 0, ZPBOX_REWARD_COUNT - 1 );
	ZPSupplyBoxSpawnAt( origin, iReward, false );

	s_spawnedThisRound++;

	ZP_Trace( "ZPSUPPLY spawned box reward=%d (%s) at (%.0f %.0f %.0f) spawned=%d\n",
		iReward, s_pRewardNames[iReward], origin.x, origin.y, origin.z, s_spawnedThisRound );
}

void ZPSupplyBoxCommand( void )
{
	Vector origin;
	if( !ZPSupplyBoxPickOrigin( origin ) )
	{
		ZP_Trace( "ZPSUPPLY test spawn failed: no valid spot\n" );
		UTIL_ClientPrintAll( HUD_PRINTCENTER, "Supply box: no valid spawn spot" );
		return;
	}

	int iReward = s_testReward;
	s_testReward = ( s_testReward + 1 ) % ZPBOX_REWARD_COUNT;

	ZPSupplyBoxSpawnAt( origin, iReward, true );

	char msg[96];
	snprintf( msg, sizeof( msg ), "Supply box: %s (test)", s_pRewardNames[iReward] );
	UTIL_ClientPrintAll( HUD_PRINTCENTER, msg );

	ZP_Trace( "ZPSUPPLY test box reward=%d (%s) at (%.0f %.0f %.0f)\n",
		iReward, s_pRewardNames[iReward], origin.x, origin.y, origin.z );
}

void ZPSupplyBoxClearCommand( void )
{
	ZPSupplyBoxRemoveAll();
	UTIL_ClientPrintAll( HUD_PRINTCENTER, "Supply boxes cleared" );
	ZP_Trace( "ZPSUPPLY test clear\n" );
}

void ZPSupplyBoxSpriteCommand( void )
{
	const char *arg = ( CMD_ARGC() > 1 ) ? CMD_ARGV( 1 ) : NULL;
	if( !arg || !arg[0] )
	{
		char msg[128];
		snprintf( msg, sizeof( msg ), "Beacon sprite: %s (index %d)", s_beaconSprite, MODEL_INDEX( s_beaconSprite ) );
		UTIL_ClientPrintAll( HUD_PRINTCENTER, msg );
		ZP_Trace( "ZPSUPPLY beacon sprite = '%s' index=%d\n", s_beaconSprite, MODEL_INDEX( s_beaconSprite ) );
		return;
	}

	int index = PRECACHE_MODEL( arg );
	if( !index )
	{
		UTIL_ClientPrintAll( HUD_PRINTCENTER, "Beacon sprite: precache failed" );
		ZP_Trace( "ZPSUPPLY beacon sprite precache FAILED for '%s'\n", arg );
		return;
	}

	strncpy( s_beaconSprite, arg, sizeof( s_beaconSprite ) - 1 );
	s_beaconSprite[sizeof( s_beaconSprite ) - 1] = 0;

	char msg[128];
	snprintf( msg, sizeof( msg ), "Beacon sprite -> %s (%d)", s_beaconSprite, index );
	UTIL_ClientPrintAll( HUD_PRINTCENTER, msg );
	ZP_Trace( "ZPSUPPLY beacon sprite = '%s' index=%d\n", s_beaconSprite, index );
}

void ZPSupplyBoxPrecache( void )
{
	PRECACHE_MODEL( ZP_SUPPLYBOX_MODEL );
	PRECACHE_MODEL( s_beaconSprite );
	PRECACHE_SOUND( ZP_SUPPLYBOX_SND_DROP );
	PRECACHE_SOUND( ZP_SUPPLYBOX_SND_PICKUP );
}

void ZPSupplyBoxInit( void )
{
	s_spawnedThisRound = 0;
	s_pendingThisWave = 0;
	s_nextWaveTime = 0.0f;
	s_nextBoxTime = 0.0f;
	s_testReward = 0;
	strncpy( s_beaconSprite, ZP_SUPPLYBOX_ICON, sizeof( s_beaconSprite ) - 1 );
	s_beaconSprite[sizeof( s_beaconSprite ) - 1] = 0;
	ZPSupplyBoxClearCooldowns();

	g_engfuncs.pfnAddServerCommand( "zp_supplybox", ZPSupplyBoxCommand );
	g_engfuncs.pfnAddServerCommand( "zp_supplyboxclear", ZPSupplyBoxClearCommand );
	g_engfuncs.pfnAddServerCommand( "zp_supplyboxsprite", ZPSupplyBoxSpriteCommand );

	ZPSupplyBoxEnsureIconTicker();
}

void ZPSupplyBoxRoundStart( void )
{
	ZPSupplyBoxRemoveAll();
	ZPSupplyBoxClearCooldowns();
	s_spawnedThisRound = 0;
	s_pendingThisWave = 0;
	s_nextWaveTime = gpGlobals->time + ZP_SUPPLYBOX_FIRST_WAVE;
	s_nextBoxTime = 0.0f;
}

void ZPSupplyBoxRoundReset( void )
{
	ZPSupplyBoxRemoveAll();
	ZPSupplyBoxClearCooldowns();
	s_spawnedThisRound = 0;
	s_pendingThisWave = 0;
	s_nextWaveTime = 0.0f;
	s_nextBoxTime = 0.0f;
}

void ZPSupplyBoxThink( void )
{
	// The marker needs a guaranteed high-frequency tick independent of this
	// function's own call cadence - see CZPSupplyBoxIconTicker above.
	ZPSupplyBoxEnsureIconTicker();

	if( g_round.state != RS_ACTIVE )
		return;
	if( s_spawnedThisRound >= ZP_SUPPLYBOX_MAX_ROUND )
		return;

	if( s_pendingThisWave <= 0 && gpGlobals->time >= s_nextWaveTime )
	{
		int remaining = ZP_SUPPLYBOX_MAX_ROUND - s_spawnedThisRound;
		s_pendingThisWave = ( remaining < ZP_SUPPLYBOX_PER_WAVE ) ? remaining : ZP_SUPPLYBOX_PER_WAVE;
		s_nextWaveTime = gpGlobals->time + ZP_SUPPLYBOX_WAVE_INTERVAL;
		s_nextBoxTime = gpGlobals->time;
	}

	if( s_pendingThisWave > 0 && gpGlobals->time >= s_nextBoxTime )
	{
		s_pendingThisWave--;
		s_nextBoxTime = gpGlobals->time + ZP_SUPPLYBOX_BOX_GAP;
		ZPSupplyBoxSpawnOne();
	}
}