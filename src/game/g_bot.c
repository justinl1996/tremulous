/*
===========================================================================
Copyright (C) 2026 GrangerHub

This file is part of Tremulous.

Tremulous is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 3 of the License,
or (at your option) any later version.

Tremulous is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Tremulous; if not, see <https://www.gnu.org/licenses/>

===========================================================================
*/

/*
===========================================================================
Bots

Like ioquake3, bots are real clients: the server reserves a slot for them
(trap_BotAllocateClient) and they go through ClientConnect/ClientBegin like
anybody else. Instead of receiving usercmds over the network, the game fills
in a usercmd for every bot each frame and runs it through ClientThink_real.
Team, class, buy and evolve requests are issued as normal client commands
(trap_BotClientCommand) so they follow exactly the same rules as players.

Console commands:
  addbot [name] [aliens|humans] [skill 1-5]
  kickbots [aliens|humans]
  botstatus                   what every bot is doing
  navdump                     write the navigation graph to nav/<map>.txt
  navtrace x y z x y z        trace a player box and report what it hits

Cvars:
  bot_minplayers  keep at least this many players on each team, using bots
  bot_skill       default skill for new bots (1-5)
  bot_builders    bots on each team that build and repair the base
  bot_debug       print where and how bots die or get stuck, to find map problems
  bot_navBudget   msec per frame spent building the navigation graph
===========================================================================
*/

#include "g_local.h"

#define BOT_MAX_PATH          128
#define BOT_THINK_ENEMY       200     // msec between enemy scans
#define BOT_ENEMY_MEMORY      3000    // msec an unseen enemy is still chased
#define BOT_HUMAN_SIGHT       2000.0f
#define BOT_ALIEN_SIGHT       2000.0f
#define BOT_BUILDABLE_SIGHT   1200.0f
#define BOT_NODE_REACHED      28.0f
#define BOT_MIN_SKILL         1
#define BOT_MAX_SKILL         5

typedef enum
{
  BG_NONE,
  BG_ATTACK,      // head for the enemy base
  BG_HUNT,        // chase where an enemy was last seen
  BG_ARMOURY,     // humans: buy weapons, armour and ammo
  BG_HEAL,        // retreat somewhere to recover health
  BG_REGROUP      // aliens: join up with a teammate before attacking
} botGoal_t;

typedef struct
{
  qboolean  inuse;
  int       skill;
  team_t    team;

  // goal
  botGoal_t goal;
  int       goalEntity;
  vec3_t    goalOrigin;
  int       nextGoalTime;

  // path
  int       path[ BOT_MAX_PATH ];
  int       pathLength;
  int       pathIndex;
  vec3_t    pathGoal;
  int       nextPathTime;
  int       nextShortcutTime;
  int       pathVersion;      // obstacles change the graph, repath when they do

  // enemy
  int       enemy;
  int       enemySightTime;
  int       enemyLastSeen;
  vec3_t    enemyLastPos;
  qboolean  enemyVisible;
  int       nextEnemyTime;

  // aim
  vec3_t    aimError;       // degrees
  int       nextAimErrorTime;

  // movement
  vec3_t    lastPos;
  int       nextStuckTime;
  int       stuckCount;
  int       unstuckUntil;
  float     strafeDir;
  int       nextStrafeTime;

  // alternate attacks that need the button held
  int       chargeStart;
  int       nextChargeTime;

  int       nextCommandTime;

  // goals we keep getting stuck on the way to are skipped for a while
  int       routeFailures;
  int       blockedEntity;
  int       blockedUntil;

  // builders
  qboolean    builder;        // spawn with a construction kit / as a granger
  buildable_t buildType;      // what we're on our way to build, BA_NONE if nothing
  vec3_t      buildSpot;
  float       buildYaw;
  int         buildTries;     // directions tried at this spot
  int         buildSpots;     // spots tried for this structure
  int         nextBuildTime;  // when to next look for something to build
  int         nextYawTime;
  int         nextBuildCommandTime;
  qboolean    buildPlaced;    // pressed attack to place it, waiting to see if it went up
  int         reclaimEntity;  // surplus structure we're marking for deconstruction, -1 if none
  int         reclaimUntil;

  // set by BotPathDirection when the route itself drops or jumps, the graph
  // has already checked those are safe
  int         pathDropTime;
  int         pathJumpTime;

  // humans fire in bursts rather than holding the trigger down
  int         burstUntil;
  int         burstPauseUntil;

  int         regroupUntil;   // aliens: wait for company until then

  qboolean    wasAlive;       // for bot_debug death reports
  vec3_t      lastGround;
  int         lastGroundTime;
  int         spawnTime;
} botState_t;

static botState_t botStates[ MAX_CLIENTS ];

// targets gathered once per frame and shared by all the bots
static gentity_t  *botPlayers[ MAX_CLIENTS ];
static int        botNumPlayers;
static gentity_t  *botBuildables[ MAX_GENTITIES ];
static int        botNumBuildables;

static int        botNextMinPlayersCheck;
static int        botNextBuilderCheck;
static int        botLastBuildWork[ NUM_TEAMS ];

static const char *botHumanNames[ ] =
{
  "Ripley", "Hicks", "Hudson", "Vasquez", "Apone", "Drake", "Frost",
  "Gorman", "Bishop", "Dietrich", "Ferro", "Spunkmeyer", "Wierzbowski",
  "Crowe", "Kovacs", "Dallas"
};

static const char *botAlienNames[ ] =
{
  "Skitter", "Chitin", "Mandible", "Brood", "Spawnling", "Creep",
  "Venom", "Carapace", "Hive", "Talon", "Scuttle", "Ichor", "Gnasher",
  "Mire", "Thorax", "Husk"
};

// preferred human weapons, best first
static const weapon_t botHumanWeapons[ ] =
{
  WP_PULSE_RIFLE, WP_CHAINGUN, WP_MASS_DRIVER, WP_LAS_GUN, WP_SHOTGUN
};

// alien classes to evolve into, best first
static const class_t botAlienClasses[ ] =
{
  PCL_ALIEN_LEVEL4, PCL_ALIEN_LEVEL3_UPG, PCL_ALIEN_LEVEL3,
  PCL_ALIEN_LEVEL2_UPG, PCL_ALIEN_LEVEL2, PCL_ALIEN_LEVEL1_UPG,
  PCL_ALIEN_LEVEL1, PCL_ALIEN_LEVEL0   // a granger going back to fighting
};

/*
===============
Helpers
===============
*/
static float BotRandom( void )
{
  return ( rand( ) & 0x7fff ) / (float)0x7fff;
}

static qboolean BotIsAlive( gentity_t *ent )
{
  return ent->client &&
         ent->client->pers.connected == CON_CONNECTED &&
         ent->client->sess.spectatorState == SPECTATOR_NOT &&
         ent->client->ps.stats[ STAT_HEALTH ] > 0;
}

static void BotEntityCenter( gentity_t *ent, vec3_t out )
{
  VectorAdd( ent->r.absmin, ent->r.absmax, out );
  VectorScale( out, 0.5f, out );
}

static void BotEyes( gentity_t *ent, vec3_t out )
{
  BG_GetClientViewOrigin( &ent->client->ps, out );
}

static qboolean BotCanSee( gentity_t *self, gentity_t *other )
{
  trace_t tr;
  vec3_t  eye, center;

  BotEyes( self, eye );
  BotEntityCenter( other, center );

  if( !trap_InPVS( eye, center ) )
    return qfalse;

  trap_Trace( &tr, eye, NULL, NULL, center, self->s.number, MASK_SHOT );
  return tr.entityNum == other->s.number || tr.fraction >= 1.0f;
}

static qboolean BotIsUsableBuildable( gentity_t *ent, buildable_t type, team_t team )
{
  return ent->inuse && ent->s.eType == ET_BUILDABLE &&
         ent->s.modelindex == type && ent->buildableTeam == team &&
         ent->spawned && ent->health > 0 &&
         ( team != TEAM_HUMANS || ent->powered );
}

static gentity_t *BotNearestBuildable( gentity_t *self, team_t team,
                                        buildable_t type )
{
  gentity_t *best = NULL;
  float     d, bestDist = 0.0f;
  int       i;

  for( i = 0; i < botNumBuildables; i++ )
  {
    gentity_t *ent = botBuildables[ i ];

    if( !BotIsUsableBuildable( ent, ( type == BA_NONE ) ?
                               ent->s.modelindex : type, team ) )
      continue;

    d = DistanceSquared( self->r.currentOrigin, ent->r.currentOrigin );
    if( !best || d < bestDist )
    {
      best = ent;
      bestDist = d;
    }
  }

  return best;
}

// small aliens can't survive base defences, they're better off hunting players
static qboolean BotIsWeakAlien( gentity_t *ent )
{
  class_t c = ent->client->ps.stats[ STAT_CLASS ];

  return ent->client->pers.teamSelection == TEAM_ALIENS &&
         ( c == PCL_ALIEN_LEVEL0 || c == PCL_ALIEN_LEVEL1 ||
           c == PCL_ALIEN_LEVEL1_UPG || c == PCL_ALIEN_BUILDER0 ||
           c == PCL_ALIEN_BUILDER0_UPG );
}

static qboolean BotIsBuilderWeapon( weapon_t weapon );

static void BotCommand( gentity_t *ent, const char *cmd )
{
  trap_BotClientCommand( ent - g_entities, cmd );
}

/*
===============
Enemy selection
===============
*/
static void BotFindEnemy( gentity_t *self, botState_t *bs )
{
  team_t    team = self->client->pers.teamSelection;
  gentity_t *best = NULL;
  float     bestScore = 0.0f;
  float     sight, d, score;
  vec3_t    forward, dir;
  int       i;

  sight = ( team == TEAM_HUMANS ) ? BOT_HUMAN_SIGHT : BOT_ALIEN_SIGHT;
  AngleVectors( self->client->ps.viewangles, forward, NULL, NULL );

  for( i = 0; i < botNumPlayers; i++ )
  {
    gentity_t *other = botPlayers[ i ];

    if( other == self || other->client->pers.teamSelection == team ||
        ( other->flags & FL_NOTARGET ) )
      continue;

    VectorSubtract( other->r.currentOrigin, self->r.currentOrigin, dir );
    d = VectorNormalize( dir );
    if( d > sight )
      continue;

    // only notice enemies that are in front of us, close by or shooting at us
    if( DotProduct( dir, forward ) < -0.2f && d > 300.0f &&
        !( self->client->lasthurt_client == other->s.number &&
           level.time - self->lastDamageTime < 2000 ) )
      continue;

    if( !BotCanSee( self, other ) )
      continue;

    score = d;
    if( other->s.number == bs->enemy )
      score *= 0.7f;   // don't flip between targets

    if( !best || score < bestScore )
    {
      best = other;
      bestScore = score;
    }
  }

  // go after structures when there are no players about
  if( !best )
  {
    for( i = 0; i < botNumBuildables; i++ )
    {
      gentity_t *other = botBuildables[ i ];

      if( other->buildableTeam == team || other->health <= 0 )
        continue;

      if( BotIsWeakAlien( self ) && ( other->s.modelindex == BA_H_MGTURRET ||
                                      other->s.modelindex == BA_H_TESLAGEN ) )
        continue;

      d = Distance( other->r.currentOrigin, self->r.currentOrigin );
      if( d > BOT_BUILDABLE_SIGHT )
        continue;

      if( !BotCanSee( self, other ) )
        continue;

      if( !best || d < bestScore )
      {
        best = other;
        bestScore = d;
      }
    }
  }

  if( best )
  {
    if( best->s.number != bs->enemy || !bs->enemyVisible )
      bs->enemySightTime = level.time;

    bs->enemy = best->s.number;
    bs->enemyVisible = qtrue;
    bs->enemyLastSeen = level.time;
    BotEntityCenter( best, bs->enemyLastPos );
    return;
  }

  bs->enemyVisible = qfalse;

  if( bs->enemy >= 0 )
  {
    gentity_t *old = &g_entities[ bs->enemy ];

    if( !old->inuse || old->health <= 0 ||
        ( old->client && !BotIsAlive( old ) ) ||
        level.time - bs->enemyLastSeen > BOT_ENEMY_MEMORY )
      bs->enemy = -1;
  }
}

/*
===============
Shopping and evolving
===============
*/
static weapon_t BotBestAffordableWeapon( gentity_t *self )
{
  gclient_t *client = self->client;
  weapon_t  current = client->ps.stats[ STAT_WEAPON ];
  int       budget, i;

  budget = client->pers.credit;
  if( BG_Weapon( current )->purchasable )
    budget += BG_Weapon( current )->price;

  for( i = 0; i < ARRAY_LEN( botHumanWeapons ); i++ )
  {
    weapon_t w = botHumanWeapons[ i ];

    if( w == current )
      return WP_NONE;   // already have the best we could afford

    if( !BG_WeaponIsAllowed( w ) || !BG_Weapon( w )->purchasable ||
        !BG_WeaponAllowedInStage( w, g_humanStage.integer ) )
      continue;

    if( BG_Weapon( w )->price <= budget )
      return w;
  }

  return WP_NONE;
}

static upgrade_t BotWantedUpgrade( gentity_t *self )
{
  gclient_t *client = self->client;
  int       *stats = client->ps.stats;
  static const upgrade_t wanted[ ] = { UP_LIGHTARMOUR, UP_HELMET };
  int       i;

  // a battlesuit takes the place of the other armour
  if( BG_InventoryContainsUpgrade( UP_BATTLESUIT, stats ) )
    return UP_NONE;

  if( BG_UpgradeIsAllowed( UP_BATTLESUIT ) &&
      BG_UpgradeAllowedInStage( UP_BATTLESUIT, g_humanStage.integer ) &&
      client->pers.credit >= BG_Upgrade( UP_BATTLESUIT )->price )
    return UP_BATTLESUIT;

  for( i = 0; i < ARRAY_LEN( wanted ); i++ )
  {
    upgrade_t u = wanted[ i ];

    if( BG_InventoryContainsUpgrade( u, stats ) || !BG_UpgradeIsAllowed( u ) ||
        !BG_UpgradeAllowedInStage( u, g_humanStage.integer ) )
      continue;

    if( client->pers.credit >= BG_Upgrade( u )->price )
      return u;
  }

  return UP_NONE;
}

static qboolean BotNeedsAmmo( gentity_t *self )
{
  playerState_t           *ps = &self->client->ps;
  const weaponAttributes_t *wa = BG_Weapon( ps->stats[ STAT_WEAPON ] );
  int                     have, max;

  if( wa->infiniteAmmo )
    return qfalse;

  max = wa->maxAmmo * ( wa->maxClips + 1 );
  have = ps->ammo + ps->clips * wa->maxAmmo;

  return have < max / 3;
}

// does the bot hold the wrong kind of weapon for its role?
static qboolean BotNeedsRoleChange( gentity_t *self, botState_t *bs )
{
  qboolean hasKit = self->client->ps.stats[ STAT_WEAPON ] == WP_HBUILD;

  return self->client->pers.teamSelection == TEAM_HUMANS && bs->builder != hasKit;
}

static qboolean BotWantsArmoury( gentity_t *self )
{
  botState_t *bs = &botStates[ self - g_entities ];

  if( BotNeedsRoleChange( self, bs ) )
    return qtrue;

  // builders only come for armour
  if( bs->builder )
    return BotWantedUpgrade( self ) != UP_NONE;

  return BotNeedsAmmo( self ) ||
         BotBestAffordableWeapon( self ) != WP_NONE ||
         BotWantedUpgrade( self ) != UP_NONE;
}

static void BotShop( gentity_t *self, botState_t *bs )
{
  weapon_t  w;
  upgrade_t u;

  if( level.time < bs->nextCommandTime )
    return;

  if( !G_BuildableRange( self->client->ps.origin, 100, BA_H_ARMOURY ) )
    return;

  bs->nextCommandTime = level.time + 300;

  // swapping between the construction kit and a gun sells the old one
  if( BotNeedsRoleChange( self, bs ) )
  {
    if( bs->builder )
      w = WP_HBUILD;
    else if( ( w = BotBestAffordableWeapon( self ) ) == WP_NONE )
      w = WP_MACHINEGUN;

    BotCommand( self, va( "buy %s", BG_Weapon( w )->name ) );
  }
  else if( bs->builder )
  {
    if( ( u = BotWantedUpgrade( self ) ) != UP_NONE )
      BotCommand( self, va( "buy %s", BG_Upgrade( u )->name ) );
  }
  else if( ( w = BotBestAffordableWeapon( self ) ) != WP_NONE )
    BotCommand( self, va( "buy %s", BG_Weapon( w )->name ) );
  else if( ( u = BotWantedUpgrade( self ) ) != UP_NONE )
    BotCommand( self, va( "buy %s", BG_Upgrade( u )->name ) );
  else if( BotNeedsAmmo( self ) )
    BotCommand( self, "buy ammo" );
}

static void BotEvolve( gentity_t *self, botState_t *bs )
{
  gclient_t *client = self->client;
  class_t   current = client->ps.stats[ STAT_CLASS ];
  int       i, choice = -1;

  if( level.time < bs->nextCommandTime || bs->enemyVisible )
    return;

  if( client->ps.groundEntityNum == ENTITYNUM_NONE ||
      ( client->ps.eFlags & EF_WALLCLIMB ) || !G_Overmind( ) )
    return;

  bs->nextCommandTime = level.time + 2000;

  for( i = 0; i < ARRAY_LEN( botAlienClasses ); i++ )
  {
    class_t c = botAlienClasses[ i ];

    if( c == current )
      return;

    if( !BG_ClassIsAllowed( c ) || !BG_ClassAllowedInStage( c, g_alienStage.integer ) )
      continue;

    if( BG_ClassCanEvolveFromTo( current, c, client->pers.credit,
                                 g_alienStage.integer, 0 ) < 0 )
      continue;

    choice = i;
    // sometimes save up rather than taking the first thing we can afford
    if( BotRandom( ) < 0.8f )
      break;
  }

  if( choice >= 0 )
    BotCommand( self, va( "class %s", BG_Class( botAlienClasses[ choice ] )->name ) );
}

/*
===============
Goals
===============
*/
static gentity_t *BotRandomEnemyBuildable( gentity_t *self )
{
  team_t    team = self->client->pers.teamSelection;
  gentity_t *list[ 64 ];
  int       count = 0, priority = 0;
  int       i;

  for( i = 0; i < botNumBuildables && count < ARRAY_LEN( list ); i++ )
  {
    gentity_t *ent = botBuildables[ i ];
    buildable_t type = ent->s.modelindex;
    int         p;

    if( ent->buildableTeam == team || ent->health <= 0 )
      continue;

    // spawns and the main structure win games, so prefer them
    p = ( type == BA_A_SPAWN || type == BA_H_SPAWN ||
          type == BA_A_OVERMIND || type == BA_H_REACTOR ) ? 1 : 0;

    if( p > priority )
    {
      priority = p;
      count = 0;
    }

    if( p == priority )
      list[ count++ ] = ent;
  }

  if( !count )
    return NULL;

  return list[ rand( ) % count ];
}

static qboolean BotGoalBlocked( botState_t *bs, gentity_t *target )
{
  return target && target->s.number == bs->blockedEntity &&
         level.time < bs->blockedUntil;
}

static void BotChooseGoal( gentity_t *self, botState_t *bs )
{
  gclient_t *client = self->client;
  team_t    team = client->pers.teamSelection;
  gentity_t *target = NULL;
  int       health = client->ps.stats[ STAT_HEALTH ];
  int       maxHealth = client->ps.stats[ STAT_MAX_HEALTH ];
  int       i;

  // keep a heal goal until we've recovered
  if( bs->goal == BG_HEAL && health < maxHealth * 0.9f &&
      bs->goalEntity >= 0 && g_entities[ bs->goalEntity ].inuse &&
      g_entities[ bs->goalEntity ].health > 0 )
    return;

  if( team == TEAM_ALIENS && health < maxHealth * 0.4f )
  {
    target = BotNearestBuildable( self, TEAM_ALIENS, BA_A_BOOSTER );
    if( !target )
      target = BotNearestBuildable( self, TEAM_ALIENS, BA_NONE );

    if( target && !BotGoalBlocked( bs, target ) )
    {
      bs->goal = BG_HEAL;
      bs->goalEntity = target->s.number;
      VectorCopy( target->r.currentOrigin, bs->goalOrigin );
      return;
    }
  }

  if( team == TEAM_HUMANS && health < maxHealth * 0.5f &&
      !BG_InventoryContainsUpgrade( UP_MEDKIT, client->ps.stats ) )
  {
    target = BotNearestBuildable( self, TEAM_HUMANS, BA_H_MEDISTAT );
    if( target && !BotGoalBlocked( bs, target ) )
    {
      bs->goal = BG_HEAL;
      bs->goalEntity = target->s.number;
      VectorCopy( target->r.currentOrigin, bs->goalOrigin );
      return;
    }
  }

  if( team == TEAM_HUMANS && BotWantsArmoury( self ) )
  {
    target = BotNearestBuildable( self, TEAM_HUMANS, BA_H_ARMOURY );
    if( target && !BotGoalBlocked( bs, target ) )
    {
      bs->goal = BG_ARMOURY;
      bs->goalEntity = target->s.number;
      VectorCopy( target->r.currentOrigin, bs->goalOrigin );
      return;
    }
  }

  // chase an enemy we've lost sight of
  if( bs->enemy >= 0 && !bs->enemyVisible )
  {
    bs->goal = BG_HUNT;
    bs->goalEntity = -1;
    VectorCopy( bs->enemyLastPos, bs->goalOrigin );
    return;
  }

  // keep attacking the same structure while it stands
  if( bs->goal == BG_ATTACK && bs->goalEntity >= 0 &&
      level.time < bs->nextGoalTime )
  {
    gentity_t *old = &g_entities[ bs->goalEntity ];

    if( old->inuse && old->health > 0 &&
        ( old->s.eType == ET_BUILDABLE || BotIsAlive( old ) ) )
    {
      VectorCopy( old->r.currentOrigin, bs->goalOrigin );
      return;
    }
  }

  // small aliens die alone, so go find a teammate before attacking
  if( BotIsWeakAlien( self ) && level.time < bs->regroupUntil )
  {
    gentity_t *ally = NULL;
    float     d, bestDist = 0.0f;

    for( i = 0; i < botNumPlayers; i++ )
    {
      gentity_t *other = botPlayers[ i ];

      if( other == self || other->client->pers.teamSelection != TEAM_ALIENS ||
          BotIsBuilderWeapon( other->client->ps.weapon ) )
        continue;

      d = DistanceSquared( other->r.currentOrigin, self->r.currentOrigin );
      if( !ally || d < bestDist )
      {
        ally = other;
        bestDist = d;
      }
    }

    if( ally && bestDist > Square( 300.0f ) )
    {
      bs->goal = BG_REGROUP;
      bs->goalEntity = ally->s.number;
      VectorCopy( ally->r.currentOrigin, bs->goalOrigin );
      return;
    }

    // together now, or nobody to wait for
    if( ally )
      bs->regroupUntil = 0;
  }

  // weak aliens roam after players, everybody else goes for the base,
  // skipping anything we've just failed to reach
  if( !BotIsWeakAlien( self ) )
  {
    for( i = 0; i < 4; i++ )
    {
      target = BotRandomEnemyBuildable( self );
      if( !BotGoalBlocked( bs, target ) )
        break;
    }

    if( BotGoalBlocked( bs, target ) )
      target = NULL;
  }

  // no structures left, go after the nearest player instead
  if( !target )
  {
    float d, bestDist = 0.0f;

    for( i = 0; i < botNumPlayers; i++ )
    {
      gentity_t *other = botPlayers[ i ];

      if( other->client->pers.teamSelection == team || BotGoalBlocked( bs, other ) )
        continue;

      d = DistanceSquared( other->r.currentOrigin, self->r.currentOrigin );
      if( !target || d < bestDist )
      {
        target = other;
        bestDist = d;
      }
    }
  }

  // everything is out of reach, keep trying rather than standing around
  if( !target )
    target = BotRandomEnemyBuildable( self );

  if( target && target->s.number != bs->goalEntity )
    bs->routeFailures = 0;

  if( target )
  {
    bs->goal = BG_ATTACK;
    bs->goalEntity = target->s.number;
    VectorCopy( target->r.currentOrigin, bs->goalOrigin );
    bs->nextGoalTime = level.time + 20000 + rand( ) % 10000;
  }
  else
  {
    bs->goal = BG_NONE;
    bs->goalEntity = -1;
  }
}

/*
===============
Movement
===============
*/

// is there room to walk straight to a point?
static qboolean BotCanWalkTo( gentity_t *self, const vec3_t target )
{
  trace_t tr;
  vec3_t  start, end;
  int     pass = self->s.number;
  int     i;

  VectorCopy( self->r.currentOrigin, start );
  start[ 2 ] += 18.0f;
  VectorCopy( target, end );
  end[ 2 ] += 18.0f;

  // structures are in the way, other players will move
  for( i = 0; i < 4; i++ )
  {
    trap_Trace( &tr, start, self->r.mins, self->r.maxs, end, pass, MASK_PLAYERSOLID );

    if( tr.fraction >= 1.0f )
      return qtrue;

    if( tr.startsolid || tr.entityNum >= MAX_CLIENTS )
      return qfalse;

    // carry on past the player from where we hit them
    pass = tr.entityNum;
    VectorCopy( tr.endpos, start );
  }

  return qfalse;
}

// returns the direction to walk in, and whether to jump
static qboolean BotPathDirection( gentity_t *self, botState_t *bs,
                                  const vec3_t goal, vec3_t dir, qboolean *jump )
{
  vec3_t  origin, node, delta;
  float   horizontal;

  VectorCopy( self->r.currentOrigin, origin );
  *jump = qfalse;

  if( level.time >= bs->nextPathTime || bs->pathVersion != G_BotNavVersion( ) ||
      DistanceSquared( goal, bs->pathGoal ) > Square( 128.0f ) )
  {
    bs->pathVersion = G_BotNavVersion( );
    bs->pathLength = G_BotNavFindPath( origin, goal, bs->path, BOT_MAX_PATH,
                                       G_BotNavSizeClass( self->r.mins, self->r.maxs ),
                                       self->client->pers.teamSelection );
    bs->pathIndex = 0;
    VectorCopy( goal, bs->pathGoal );
    bs->nextPathTime = level.time + 2000 + rand( ) % 1000;

    // the nearest node may be behind us
    if( bs->pathLength > 1 )
    {
      G_BotNavNodeOrigin( bs->path[ 1 ], node );
      if( BotCanWalkTo( self, node ) )
        bs->pathIndex = 1;
    }
  }

  // skip nodes we've reached
  while( bs->pathIndex < bs->pathLength )
  {
    G_BotNavNodeOrigin( bs->path[ bs->pathIndex ], node );
    VectorSubtract( node, origin, delta );
    horizontal = sqrt( delta[ 0 ] * delta[ 0 ] + delta[ 1 ] * delta[ 1 ] );

    if( horizontal > BOT_NODE_REACHED || fabs( delta[ 2 ] ) > 56.0f )
      break;

    bs->pathIndex++;
  }

  // cut corners when the next node is in plain reach
  if( bs->pathIndex + 1 < bs->pathLength && level.time >= bs->nextShortcutTime )
  {
    bs->nextShortcutTime = level.time + 300;
    G_BotNavNodeOrigin( bs->path[ bs->pathIndex + 1 ], node );

    if( fabs( node[ 2 ] - origin[ 2 ] ) < 18.0f &&
        !( G_BotNavLinkFlags( bs->path[ bs->pathIndex ],
                              bs->path[ bs->pathIndex + 1 ] ) & NAVLINK_JUMP ) &&
        BotCanWalkTo( self, node ) )
      bs->pathIndex++;
  }

  if( bs->pathIndex >= bs->pathLength )
  {
    VectorSubtract( goal, origin, dir );
    return bs->pathLength > 0 || BotCanWalkTo( self, goal );
  }

  G_BotNavNodeOrigin( bs->path[ bs->pathIndex ], node );
  VectorSubtract( node, origin, dir );

  if( dir[ 2 ] < -18.0f )
    bs->pathDropTime = level.time;

  // jump links get a jump, and so does climbing more than a step up to the
  // next node in case we've cut a corner and aren't on the marked link
  if( dir[ 0 ] * dir[ 0 ] + dir[ 1 ] * dir[ 1 ] < Square( 72.0f ) &&
      ( dir[ 2 ] > 18.0f ||
        ( bs->pathIndex > 0 &&
          ( G_BotNavLinkFlags( bs->path[ bs->pathIndex - 1 ],
                               bs->path[ bs->pathIndex ] ) & NAVLINK_JUMP ) ) ) )
  {
    *jump = qtrue;
    bs->pathJumpTime = level.time;
  }

  return qtrue;
}

// jump over low obstacles in the way
static qboolean BotShouldJumpObstacle( gentity_t *self, const vec3_t dir )
{
  trace_t tr;
  vec3_t  start, end, flat;

  VectorSet( flat, dir[ 0 ], dir[ 1 ], 0.0f );
  if( VectorNormalize( flat ) < 0.1f )
    return qfalse;

  VectorCopy( self->r.currentOrigin, start );
  start[ 2 ] += 18.0f;
  VectorMA( start, 32.0f, flat, end );

  trap_Trace( &tr, start, self->r.mins, self->r.maxs, end, self->s.number,
              MASK_PLAYERSOLID );
  if( tr.fraction >= 1.0f )
    return qfalse;

  start[ 2 ] += 30.0f;
  VectorMA( start, 32.0f, flat, end );
  trap_Trace( &tr, start, self->r.mins, self->r.maxs, end, self->s.number,
              MASK_PLAYERSOLID );

  return tr.fraction >= 1.0f;
}

static void BotCheckStuck( gentity_t *self, botState_t *bs, qboolean moving )
{
  if( level.time < bs->nextStuckTime )
    return;

  bs->nextStuckTime = level.time + 1000;

  // pressing up against an enemy we're fighting isn't being stuck
  if( moving && bs->enemy >= 0 && bs->enemyVisible &&
      Distance( g_entities[ bs->enemy ].r.currentOrigin, self->r.currentOrigin ) < 150.0f )
    moving = qfalse;

  if( moving && Distance( self->r.currentOrigin, bs->lastPos ) < 32.0f )
  {
    bs->stuckCount++;

    if( bot_debug.integer )
    {
      gentity_t *near = NULL;
      float     d, nearDist = 0.0f;
      int       i;

      for( i = 0; i < botNumBuildables; i++ )
      {
        d = Distance( botBuildables[ i ]->r.currentOrigin, self->r.currentOrigin );
        if( !near || d < nearDist )
        {
          near = botBuildables[ i ];
          nearDist = d;
        }
      }

      G_Printf( "botstuck: %s %s at (%.0f %.0f %.0f) goal %d path %d/%d near %s %.0f\n",
                BG_TeamName( self->client->pers.teamSelection ),
                BG_Class( self->client->ps.stats[ STAT_CLASS ] )->name,
                self->r.currentOrigin[ 0 ], self->r.currentOrigin[ 1 ], self->r.currentOrigin[ 2 ],
                bs->goal, bs->pathIndex, bs->pathLength,
                near ? BG_Buildable( near->s.modelindex )->name : "none", nearDist );
    }
    bs->unstuckUntil = level.time + 600 + rand( ) % 600;
    bs->strafeDir = ( rand( ) & 1 ) ? 1.0f : -1.0f;

    if( bs->stuckCount >= 3 )
    {
      // give up on this route, and steer everyone away from the same link
      if( bs->pathIndex > 0 && bs->pathIndex < bs->pathLength )
        G_BotNavLinkFailed( bs->path[ bs->pathIndex - 1 ], bs->path[ bs->pathIndex ] );
      if( bs->pathIndex < bs->pathLength )
        bs->pathIndex++;
      bs->nextPathTime = 0;
      bs->stuckCount = 0;

      // and on the goal itself if no route works
      if( ++bs->routeFailures >= 3 && bs->goalEntity >= 0 )
      {
        bs->blockedEntity = bs->goalEntity;
        bs->blockedUntil = level.time + 30000;
        bs->goal = BG_NONE;
        bs->goalEntity = -1;
        bs->routeFailures = 0;
      }
    }
  }
  else
    bs->stuckCount = 0;

  VectorCopy( self->r.currentOrigin, bs->lastPos );
}

// is it safe to step this way, or is there a long drop or a hazard?
static qboolean BotGroundAhead( gentity_t *self, const vec3_t forward, const vec3_t right,
                                float fm, float rm, float extra )
{
  static const vec3_t probeMins = { -4.0f, -4.0f, 0.0f };
  static const vec3_t probeMaxs = { 4.0f, 4.0f, 4.0f };
  trace_t tr;
  vec3_t  dir, start, end, velocity;
  float   speed, dist[ 2 ];
  int     i;

  VectorScale( forward, fm, dir );
  VectorMA( dir, rm, right, dir );
  dir[ 2 ] = 0.0f;
  if( VectorNormalize( dir ) < 0.01f )
    return qtrue;

  // look further ahead the faster we're going, since we slide before stopping
  VectorCopy( self->client->ps.velocity, velocity );
  velocity[ 2 ] = 0.0f;
  speed = VectorLength( velocity );
  dist[ 0 ] = 24.0f;
  dist[ 1 ] = 24.0f + MIN( speed * 0.3f, 120.0f ) + extra;

  for( i = 0; i < 2; i++ )
  {
    // a thin probe, a full box would catch the lip of a ledge. Start just
    // above step height so low rails and curbs don't count as walls
    VectorMA( self->r.currentOrigin, dist[ i ], dir, start );
    start[ 2 ] += self->r.mins[ 2 ] + 20.0f;
    VectorCopy( start, end );
    end[ 2 ] -= 300.0f;

    trap_Trace( &tr, start, probeMins, probeMaxs, end, self->s.number, MASK_DEADSOLID );

    // a wall in the way can't be fallen off
    if( tr.startsolid || tr.allsolid )
      return qtrue;

    if( tr.fraction >= 1.0f || G_BotNavInHazard( tr.endpos ) )
      return qfalse;

    // too steep to stand on means sliding down whatever is below, though
    // the lip of a drop the route takes us down is often sloped
    if( tr.plane.normal[ 2 ] < 0.7f &&
        botStates[ self - g_entities ].pathDropTime != level.time )
      return qfalse;
  }

  return qtrue;
}

// a jump carries us further than a step, so check further ahead
static qboolean BotSafeToJump( gentity_t *self, usercmd_t *cmd, const vec3_t viewAngles )
{
  vec3_t  forward, right, angles;

  // jumps the route asks for have been checked already
  if( botStates[ self - g_entities ].pathJumpTime == level.time )
    return qtrue;

  if( !cmd->forwardmove && !cmd->rightmove )
    return qtrue;

  VectorSet( angles, 0.0f, viewAngles[ YAW ], 0.0f );
  AngleVectors( angles, forward, right, NULL );

  return BotGroundAhead( self, forward, right, cmd->forwardmove / 127.0f,
                         cmd->rightmove / 127.0f, 80.0f );
}

static void BotSetMovement( gentity_t *self, usercmd_t *cmd, const vec3_t viewAngles,
                            const vec3_t dir, float strafe )
{
  vec3_t  forward, right, flat, angles;
  float   fm, rm, scale;

  VectorSet( angles, 0.0f, viewAngles[ YAW ], 0.0f );
  AngleVectors( angles, forward, right, NULL );

  VectorSet( flat, dir[ 0 ], dir[ 1 ], 0.0f );
  VectorNormalize( flat );

  fm = DotProduct( flat, forward );
  rm = DotProduct( flat, right ) + strafe;

  // don't dodge or back off over a ledge
  if( !BotGroundAhead( self, forward, right, fm, rm, 0.0f ) )
  {
    rm -= strafe;
    if( !BotGroundAhead( self, forward, right, fm, rm, 0.0f ) )
      return;
  }

  scale = MAX( fabs( fm ), fabs( rm ) );
  if( scale < 0.01f )
    return;

  cmd->forwardmove = (signed char)( 127.0f * fm / scale );
  cmd->rightmove = (signed char)( 127.0f * rm / scale );
}

/*
===============
Aiming
===============
*/
static void BotTurn( botState_t *bs, vec3_t view, const vec3_t desired, float msec )
{
  float maxTurn = ( 200.0f + bs->skill * 80.0f ) * msec / 1000.0f;
  float delta;
  int   i;

  for( i = 0; i < 2; i++ )
  {
    delta = AngleSubtract( desired[ i ], view[ i ] );
    if( delta > maxTurn )
      delta = maxTurn;
    else if( delta < -maxTurn )
      delta = -maxTurn;

    view[ i ] = AngleMod( view[ i ] + delta );
  }
}

static float BotAngleTo( const vec3_t view, const vec3_t dir )
{
  vec3_t forward;

  AngleVectors( view, forward, NULL, NULL );
  return RAD2DEG( acos( MAX( -1.0f, MIN( 1.0f, DotProduct( forward, dir ) ) ) ) );
}

/*
===============
Combat
===============
*/
static float BotAttackRange( weapon_t weapon )
{
  switch( weapon )
  {
    case WP_ALEVEL0:       return LEVEL0_BITE_RANGE + 16.0f;
    case WP_ALEVEL1:
    case WP_ALEVEL1_UPG:   return LEVEL1_CLAW_RANGE + 16.0f;
    case WP_ALEVEL2:
    case WP_ALEVEL2_UPG:   return LEVEL2_CLAW_RANGE + 16.0f;
    case WP_ALEVEL3:
    case WP_ALEVEL3_UPG:   return LEVEL3_CLAW_RANGE + 16.0f;
    case WP_ALEVEL4:       return LEVEL4_CLAW_RANGE + 16.0f;
    case WP_ABUILD:
    case WP_ABUILD2:       return ABUILDER_CLAW_RANGE + 16.0f;
    case WP_PAIN_SAW:      return PAINSAW_RANGE + 16.0f;
    case WP_FLAMER:        return 300.0f;
    case WP_SHOTGUN:       return 800.0f;
    case WP_HBUILD:        return 0.0f;
    default:               return 2500.0f;
  }
}

// distance humans like to keep from their target
static float BotPreferredRange( weapon_t weapon )
{
  switch( weapon )
  {
    case WP_PAIN_SAW:      return 40.0f;
    case WP_FLAMER:        return 180.0f;
    case WP_SHOTGUN:       return 250.0f;
    default:               return 450.0f;
  }
}

static void BotAttack( gentity_t *self, botState_t *bs, usercmd_t *cmd,
                       gentity_t *enemy, float dist, float aimOff )
{
  playerState_t *ps = &self->client->ps;
  weapon_t      weapon = ps->weapon;
  float         tolerance;
  qboolean      aimed;

  int           reaction = 700 - bs->skill * 100;

  // humans take longer to react to fast, small aliens than aliens do to them
  if( self->client->pers.teamSelection == TEAM_HUMANS && enemy->client )
    reaction += 200;

  if( level.time - bs->enemySightTime < reaction )
    return;

  // humans shoot in bursts, better ones in longer bursts with shorter pauses
  if( self->client->pers.teamSelection == TEAM_HUMANS )
  {
    if( level.time < bs->burstPauseUntil )
      return;

    if( !bs->burstUntil )
      bs->burstUntil = level.time + 400 + bs->skill * 150 + rand( ) % 400;
    else if( level.time >= bs->burstUntil )
    {
      bs->burstUntil = 0;
      bs->burstPauseUntil = level.time + 650 - bs->skill * 80 + rand( ) % 300;
      return;
    }
  }

  // aim must be within the target's apparent size, with some slack
  tolerance = RAD2DEG( atan2( 40.0f, MAX( dist, 1.0f ) ) ) + 3.0f;
  aimed = aimOff < tolerance;

  switch( weapon )
  {
    case WP_ALEVEL2_UPG:
      if( dist > LEVEL2_CLAW_RANGE + 16.0f && dist < LEVEL2_AREAZAP_RANGE && aimed )
        cmd->buttons |= BUTTON_ATTACK2;
      else if( dist < BotAttackRange( weapon ) && aimed )
        cmd->buttons |= BUTTON_ATTACK;
      return;

    case WP_ALEVEL3:
    case WP_ALEVEL3_UPG:
      // hold to charge a pounce, let go when it's charged
      if( bs->chargeStart )
      {
        if( level.time - bs->chargeStart < LEVEL3_POUNCE_TIME && dist > 100.0f )
          cmd->buttons |= BUTTON_ATTACK2;
        else
        {
          bs->chargeStart = 0;
          bs->nextChargeTime = level.time + 1500;
        }
        return;
      }

      if( dist < BotAttackRange( weapon ) && aimed )
        cmd->buttons |= BUTTON_ATTACK;
      else if( weapon == WP_ALEVEL3_UPG && ps->ammo > 0 &&
               dist > 250.0f && dist < 800.0f && aimed )
        cmd->buttons |= BUTTON_USE_HOLDABLE;
      else if( dist > 150.0f && dist < 600.0f && aimed &&
               level.time >= bs->nextChargeTime &&
               ps->groundEntityNum != ENTITYNUM_NONE )
      {
        bs->chargeStart = level.time;
        cmd->buttons |= BUTTON_ATTACK2;
      }
      return;

    case WP_ALEVEL4:
      // hold to build up a trample charge
      if( bs->chargeStart )
      {
        if( level.time - bs->chargeStart < LEVEL4_TRAMPLE_CHARGE_MAX && dist > 150.0f )
          cmd->buttons |= BUTTON_ATTACK2;
        else
        {
          bs->chargeStart = 0;
          bs->nextChargeTime = level.time + 3000;
        }
        return;
      }

      if( dist < BotAttackRange( weapon ) && aimed )
        cmd->buttons |= BUTTON_ATTACK;
      else if( dist > 300.0f && dist < 900.0f && aimed &&
               level.time >= bs->nextChargeTime )
      {
        bs->chargeStart = level.time;
        cmd->buttons |= BUTTON_ATTACK2;
      }
      return;

    case WP_LUCIFER_CANNON:
      // the secondary fire doesn't need charging
      if( aimed )
        cmd->buttons |= BUTTON_ATTACK2;
      return;

    default:
      if( dist < BotAttackRange( weapon ) && aimed )
        cmd->buttons |= BUTTON_ATTACK;
      return;
  }
}


/*
===============
Building

Each team can have builder bots. They spawn with a construction kit (humans)
or as a granger (aliens), keep the base stocked with structures and, for
humans, repair damaged ones. Structures are placed with the normal build
command, so all the usual placement and build point rules apply.

If the reactor/overmind is destroyed it's rebuilt in place, unless the enemy
is still there; then the base moves to the outpost, a surviving spawn away
from the enemy, or a new spot out of their way. Once the main base is
finished and there are build points to spare, a builder sets up an outpost
(a repeater or egg, a spawn and some defences) away from the main base.
===============
*/
typedef struct
{
  buildable_t type;
  int         wanted;     // build until the team has this many
  qboolean    spare;      // only with spare build points, never worth tearing anything down for
} botBuildOrder_t;

static const botBuildOrder_t botHumanBuildOrder[ ] =
{
  { BA_H_REACTOR, 1 }, { BA_H_SPAWN, 2 }, { BA_H_ARMOURY, 1 },
  { BA_H_MEDISTAT, 1 }, { BA_H_MGTURRET, 3 }, { BA_H_DCC, 1 },
  { BA_H_TESLAGEN, 2 }, { BA_H_SPAWN, 3 }, { BA_H_MGTURRET, 5 },
  { BA_H_TESLAGEN, 4 },
  // spare build points go on more defences
  { BA_H_SPAWN, 4, qtrue }, { BA_H_MGTURRET, 8, qtrue }, { BA_H_TESLAGEN, 6, qtrue }
};

static const botBuildOrder_t botAlienBuildOrder[ ] =
{
  { BA_A_OVERMIND, 1 }, { BA_A_SPAWN, 2 }, { BA_A_BOOSTER, 1 },
  { BA_A_ACIDTUBE, 3 }, { BA_A_SPAWN, 3 }, { BA_A_TRAPPER, 2 },
  { BA_A_HIVE, 1 }, { BA_A_ACIDTUBE, 5 },
  // spare build points go on more defences
  { BA_A_SPAWN, 4, qtrue }, { BA_A_ACIDTUBE, 8, qtrue }, { BA_A_TRAPPER, 4, qtrue },
  { BA_A_HIVE, 2, qtrue }
};

static qboolean BotIsBuilderWeapon( weapon_t weapon )
{
  return weapon == WP_HBUILD || weapon == WP_ABUILD || weapon == WP_ABUILD2;
}

// outposts: a forward spawn with a little defence, built away from the main base
static const botBuildOrder_t botHumanOutpostOrder[ ] =
{
  { BA_H_REPEATER, 1 }, { BA_H_SPAWN, 1 }, { BA_H_MGTURRET, 2 }
};

static const botBuildOrder_t botAlienOutpostOrder[ ] =
{
  { BA_A_SPAWN, 1 }, { BA_A_ACIDTUBE, 2 }, { BA_A_BOOSTER, 1 }, { BA_A_SPAWN, 2 }
};

#define BOT_OUTPOST_RADIUS    400.0f

// where each team builds. The main base follows the reactor/overmind and
// moves somewhere safer when that's destroyed; the outpost is a second base
// put up when there are build points to spare
typedef struct
{
  qboolean  mainSet;
  vec3_t    main;
  qboolean  coreAlive;          // to notice when the core is destroyed

  qboolean  outpostSet;
  vec3_t    outpost;
  int       outpostTime;        // when the outpost site was picked
  int       nextOutpostTime;    // don't pick a new site before then
} botTeamBase_t;

static botTeamBase_t botBases[ NUM_TEAMS ];

// does this structure do its job? Human ones need power
static qboolean BotBuildableWorks( gentity_t *ent )
{
  if( ent->buildableTeam != TEAM_HUMANS )
    return qtrue;

  return ent->powered || ent->s.modelindex == BA_H_REACTOR ||
         ent->s.modelindex == BA_H_REPEATER;
}

// count working structures, near a point if radius is non zero
static int BotCountBuildablesNear( team_t team, buildable_t type,
                                   const vec3_t center, float radius )
{
  int i, count = 0;

  for( i = 0; i < botNumBuildables; i++ )
  {
    gentity_t *ent = botBuildables[ i ];

    if( ent->buildableTeam != team || ent->s.modelindex != type ||
        !BotBuildableWorks( ent ) )
      continue;

    if( radius > 0.0f &&
        DistanceSquared( ent->r.currentOrigin, center ) > Square( radius ) )
      continue;

    count++;
  }

  return count;
}

static int BotCountBuildables( team_t team, buildable_t type )
{
  return BotCountBuildablesNear( team, type, vec3_origin, 0.0f );
}

static gentity_t *BotTeamCore( team_t team )
{
  buildable_t type = ( team == TEAM_HUMANS ) ? BA_H_REACTOR : BA_A_OVERMIND;
  int         i;

  for( i = 0; i < botNumBuildables; i++ )
  {
    if( botBuildables[ i ]->buildableTeam == team &&
        botBuildables[ i ]->s.modelindex == type )
      return botBuildables[ i ];
  }

  return NULL;
}

// the structure the base is built around
static gentity_t *BotBaseCore( gentity_t *self, team_t team )
{
  gentity_t *core;

  core = BotNearestBuildable( self, team,
                              team == TEAM_HUMANS ? BA_H_REACTOR : BA_A_OVERMIND );
  if( !core )
    core = BotNearestBuildable( self, team,
                                team == TEAM_HUMANS ? BA_H_SPAWN : BA_A_SPAWN );
  return core;
}

// how far a point is from the closest enemy structure or player
static float BotEnemyDistance( team_t team, const vec3_t point )
{
  float best = Square( 8192.0f ), d;
  int   i;

  for( i = 0; i < botNumBuildables; i++ )
  {
    if( botBuildables[ i ]->buildableTeam == team )
      continue;

    d = DistanceSquared( botBuildables[ i ]->r.currentOrigin, point );
    if( d < best )
      best = d;
  }

  for( i = 0; i < botNumPlayers; i++ )
  {
    if( botPlayers[ i ]->client->pers.teamSelection == team )
      continue;

    d = DistanceSquared( botPlayers[ i ]->r.currentOrigin, point );
    if( d < best )
      best = d;
  }

  return sqrt( best );
}

// pick a spot between minDist and maxDist from origin, as far from the enemy
// as possible, that we can actually walk to from origin
static qboolean BotPickSite( team_t team, const vec3_t origin, float minDist, float maxDist,
                             float minEnemyDist, vec3_t out )
{
  vec3_t  candidate, best[ 4 ], node;
  float   score, bestScore[ 4 ];
  int     numBest = 0, i, j, n;
  int     path[ 128 ];

  // sample rather than scoring every node, this runs in the qvm
  for( i = 0; i < 300; i++ )
  {
    if( !G_BotNavRandomNodeNear( origin, minDist, maxDist, candidate ) )
      return qfalse;

    score = BotEnemyDistance( team, candidate );
    if( score < minEnemyDist )
      continue;

    // keep the best few
    for( j = 0; j < numBest; j++ )
    {
      if( score > bestScore[ j ] )
        break;
    }

    if( j < 4 )
    {
      for( n = MIN( numBest, 3 ); n > j; n-- )
      {
        VectorCopy( best[ n - 1 ], best[ n ] );
        bestScore[ n ] = bestScore[ n - 1 ];
      }
      VectorCopy( candidate, best[ j ] );
      bestScore[ j ] = score;
      if( numBest < 4 )
        numBest++;
    }
  }

  // check we can get there
  for( i = 0; i < numBest; i++ )
  {
    n = G_BotNavFindPath( origin, best[ i ], path, ARRAY_LEN( path ), 0, team );
    if( !n )
      continue;

    G_BotNavNodeOrigin( path[ n - 1 ], node );
    if( Distance( node, best[ i ] ) < 150.0f )
    {
      VectorCopy( best[ i ], out );
      return qtrue;
    }
  }

  return qfalse;
}

// the core has been destroyed: rebuild it, somewhere else if the enemy is
// still at the old base
static void BotRelocateBase( team_t team )
{
  botTeamBase_t *base = &botBases[ team ];
  buildable_t   spawnType = ( team == TEAM_HUMANS ) ? BA_H_SPAWN : BA_A_SPAWN;
  gentity_t     *best = NULL;
  vec3_t        old, site;
  float         d, bestDist = 0.0f;
  int           i;

  VectorCopy( base->main, old );

  // if the enemy isn't there any more, rebuilding in place brings the rest of
  // the base back (human structures need the reactor's power)
  if( BotEnemyDistance( team, old ) > 1000.0f )
  {
    G_LogPrintf( "Bot: %s base rebuilding in place\n", BG_TeamName( team ) );
    return;
  }

  // the outpost becomes the new base
  if( base->outpostSet &&
      BotCountBuildablesNear( team, spawnType, base->outpost, BOT_OUTPOST_RADIUS ) )
  {
    VectorCopy( base->outpost, base->main );
    base->outpostSet = qfalse;
    base->nextOutpostTime = level.time + 60000;
    G_LogPrintf( "Bot: %s base moving to the outpost\n", BG_TeamName( team ) );
    return;
  }

  // otherwise the surviving spawn furthest from the enemy and the old base
  for( i = 0; i < botNumBuildables; i++ )
  {
    gentity_t *ent = botBuildables[ i ];

    if( ent->buildableTeam != team || ent->s.modelindex != spawnType )
      continue;

    if( Distance( ent->r.currentOrigin, old ) < 600.0f )
      continue;

    d = BotEnemyDistance( team, ent->r.currentOrigin );
    if( !best || d > bestDist )
    {
      best = ent;
      bestDist = d;
    }
  }

  if( best )
  {
    VectorCopy( best->r.currentOrigin, base->main );
    G_LogPrintf( "Bot: %s base moving to a spawn at (%.0f %.0f %.0f)\n", BG_TeamName( team ),
                 base->main[ 0 ], base->main[ 1 ], base->main[ 2 ] );
    return;
  }

  // nowhere ready made, find somewhere quieter within reach of the old base
  if( BotPickSite( team, old, 800.0f, 2500.0f, 800.0f, site ) )
  {
    VectorCopy( site, base->main );
    G_LogPrintf( "Bot: %s base moving to (%.0f %.0f %.0f)\n", BG_TeamName( team ),
                 base->main[ 0 ], base->main[ 1 ], base->main[ 2 ] );
  }

  // failing that, rebuild where it was
}

// higher tier and essential structures are worth tearing down surplus to make room for
static qboolean BotWorthReclaimingFor( buildable_t type )
{
  switch( type )
  {
    case BA_H_DCC:
    case BA_H_TESLAGEN:
    case BA_H_ARMOURY:
    case BA_H_MEDISTAT:
    case BA_H_SPAWN:
    case BA_A_BOOSTER:
    case BA_A_TRAPPER:
    case BA_A_HIVE:
    case BA_A_SPAWN:
      return qtrue;

    default:
      return qfalse;
  }
}

// how many of a structure to keep when looking for surplus to tear down
static int BotKeepCount( buildable_t type )
{
  switch( type )
  {
    case BA_H_MGTURRET:   return 3;
    case BA_H_SPAWN:      return 3;
    case BA_H_TESLAGEN:   return 2;
    case BA_A_ACIDTUBE:   return 3;
    case BA_A_SPAWN:      return 3;
    case BA_A_TRAPPER:    return 2;
    case BA_A_BARRICADE:  return 0;
    default:              return 9999;   // never
  }
}

// a structure we have more of than we need, other than the type we want room for
static gentity_t *BotFindSurplus( team_t team, buildable_t wanted, gentity_t *near )
{
  gentity_t *best = NULL;
  float     d, bestDist = 0.0f;
  int       i;

  for( i = 0; i < botNumBuildables; i++ )
  {
    gentity_t *ent = botBuildables[ i ];

    if( ent->buildableTeam != team || ent->s.modelindex == wanted ||
        ent->deconstruct || !ent->spawned || !BotBuildableWorks( ent ) )
      continue;

    if( BotCountBuildables( team, ent->s.modelindex ) <= BotKeepCount( ent->s.modelindex ) )
      continue;

    d = near ? DistanceSquared( ent->r.currentOrigin, near->r.currentOrigin ) : 0.0f;
    if( !best || d < bestDist )
    {
      best = ent;
      bestDist = d;
    }
  }

  return best;
}

// build points tied up in structures already marked for deconstruction
static int BotMarkedPoints( team_t team )
{
  int i, points = 0;

  for( i = 0; i < botNumBuildables; i++ )
  {
    if( botBuildables[ i ]->buildableTeam == team && botBuildables[ i ]->deconstruct )
      points += BG_Buildable( botBuildables[ i ]->s.modelindex )->buildPoints;
  }

  return points;
}

// the next thing in the order to build. If something important can't be
// afforded but there's surplus to make room, nothing is returned and
// reclaimFor says what to make room for
static buildable_t BotNextFromOrder( team_t team, weapon_t weapon,
                                     const botBuildOrder_t *order, int count,
                                     const vec3_t center, float radius,
                                     buildable_t *reclaimFor )
{
  int i, points, stage, marked;

  if( team == TEAM_HUMANS )
  {
    points = level.humanBuildPoints;
    stage = g_humanStage.integer;
  }
  else
  {
    points = level.alienBuildPoints;
    stage = g_alienStage.integer;
  }

  marked = BotMarkedPoints( team );
  if( reclaimFor )
    *reclaimFor = BA_NONE;

  for( i = 0; i < count; i++ )
  {
    const buildableAttributes_t *ba = BG_Buildable( order[ i ].type );
    int                         available = points;

    if( BotCountBuildablesNear( team, order[ i ].type, center, radius ) >= order[ i ].wanted )
      continue;

    if( !BG_BuildableIsAllowed( order[ i ].type ) ||
        !BG_BuildableAllowedInStage( order[ i ].type, stage ) ||
        !( ( 1 << weapon ) & ba->buildWeapon ) )
      continue;

    // teslas need a dcc
    if( order[ i ].type == BA_H_TESLAGEN && !BotCountBuildables( team, BA_H_DCC ) )
      continue;

    // a repeater's area has its own build points
    if( radius > 0.0f && team == TEAM_HUMANS )
      return order[ i ].type;

    // marked structures make way for important ones
    if( BotWorthReclaimingFor( order[ i ].type ) && !order[ i ].spare )
      available += marked;

    if( ba->buildPoints <= available )
      return order[ i ].type;

    // rather than spend what's left on something less important, make room
    if( reclaimFor && radius <= 0.0f && !order[ i ].spare &&
        BotWorthReclaimingFor( order[ i ].type ) &&
        BotFindSurplus( team, order[ i ].type, NULL ) )
    {
      *reclaimFor = order[ i ].type;
      return BA_NONE;
    }
  }

  return BA_NONE;
}

// what a builder on this team should build next, BA_NONE if nothing.
// outpost is set when it belongs to the outpost rather than the main base,
// reclaimFor (if given) when surplus needs tearing down first
static buildable_t BotChooseBuildableAt( team_t team, weapon_t weapon, qboolean *outpost,
                                         buildable_t *reclaimFor )
{
  botTeamBase_t *base = &botBases[ team ];
  buildable_t   type, reclaim;

  *outpost = qfalse;

  if( team == TEAM_HUMANS )
    type = BotNextFromOrder( team, weapon, botHumanBuildOrder,
                             ARRAY_LEN( botHumanBuildOrder ), vec3_origin, 0.0f, &reclaim );
  else
    type = BotNextFromOrder( team, weapon, botAlienBuildOrder,
                             ARRAY_LEN( botAlienBuildOrder ), vec3_origin, 0.0f, &reclaim );

  if( reclaimFor )
    *reclaimFor = reclaim;

  if( type != BA_NONE || reclaim != BA_NONE || !base->outpostSet )
    return type;

  *outpost = qtrue;
  if( team == TEAM_HUMANS )
    return BotNextFromOrder( team, weapon, botHumanOutpostOrder,
                             ARRAY_LEN( botHumanOutpostOrder ), base->outpost,
                             BOT_OUTPOST_RADIUS, NULL );

  return BotNextFromOrder( team, weapon, botAlienOutpostOrder,
                           ARRAY_LEN( botAlienOutpostOrder ), base->outpost,
                           BOT_OUTPOST_RADIUS, NULL );
}

static buildable_t BotChooseBuildableFor( team_t team, weapon_t weapon )
{
  qboolean outpost;

  return BotChooseBuildableAt( team, weapon, &outpost, NULL );
}

// the best builder weapon the team can field right now
static weapon_t BotTeamBuildWeapon( team_t team )
{
  if( team == TEAM_HUMANS )
    return WP_HBUILD;

  return ( BG_ClassIsAllowed( PCL_ALIEN_BUILDER0_UPG ) &&
           BG_ClassAllowedInStage( PCL_ALIEN_BUILDER0_UPG, g_alienStage.integer ) ) ?
         WP_ABUILD2 : WP_ABUILD;
}

// keep track of where the team's bases are, moving the main one if the core
// is destroyed and picking a spot for an outpost when there's spare build points
static void BotUpdateBases( team_t team )
{
  botTeamBase_t *base = &botBases[ team ];
  gentity_t     *core = BotTeamCore( team );
  buildable_t   spawnType = ( team == TEAM_HUMANS ) ? BA_H_SPAWN : BA_A_SPAWN;
  gentity_t     *spawn;
  int           i, spare;
  qboolean      outpost;

  if( core )
  {
    VectorCopy( core->r.currentOrigin, base->main );
    base->mainSet = qtrue;
    base->coreAlive = qtrue;
  }
  else
  {
    if( base->coreAlive && base->mainSet )
      BotRelocateBase( team );
    base->coreAlive = qfalse;

    // no core yet, start from a spawn
    if( !base->mainSet )
    {
      for( i = 0; i < botNumBuildables; i++ )
      {
        spawn = botBuildables[ i ];
        if( spawn->buildableTeam == team && spawn->s.modelindex == spawnType )
        {
          VectorCopy( spawn->r.currentOrigin, base->main );
          base->mainSet = qtrue;
          break;
        }
      }
    }
    return;
  }

  // give up on an outpost that never got going or has been wiped out
  if( base->outpostSet && level.time - base->outpostTime > 90000 &&
      !BotCountBuildablesNear( team, spawnType, base->outpost, BOT_OUTPOST_RADIUS ) )
  {
    base->outpostSet = qfalse;
    base->nextOutpostTime = level.time + 60000;
  }

  if( base->outpostSet || level.time < base->nextOutpostTime )
    return;

  // only once the main base is done and there's something left to spend
  {
    buildable_t reclaim;

    if( BotChooseBuildableAt( team, BotTeamBuildWeapon( team ), &outpost, &reclaim ) != BA_NONE ||
        reclaim != BA_NONE )
      return;
  }

  spare = ( team == TEAM_HUMANS ) ? level.humanBuildPoints : level.alienBuildPoints;
  if( spare < BG_Buildable( team == TEAM_HUMANS ? BA_H_REPEATER : BA_A_SPAWN )->buildPoints +
              BG_Buildable( team == TEAM_HUMANS ? BA_H_MGTURRET : BA_A_ACIDTUBE )->buildPoints )
    return;

  // humans need it out of the reactor's power so the repeater gets its own
  // build points, aliens just need it clear of the main base
  base->nextOutpostTime = level.time + 30000;
  if( BotPickSite( team, base->main, team == TEAM_HUMANS ? 1100.0f : 900.0f, 2200.0f,
                   1200.0f, base->outpost ) )
  {
    base->outpostSet = qtrue;
    base->outpostTime = level.time;
    G_LogPrintf( "Bot: %s outpost planned at (%.0f %.0f %.0f)\n", BG_TeamName( team ),
                 base->outpost[ 0 ], base->outpost[ 1 ], base->outpost[ 2 ] );
  }
}

static qboolean BotTeamHasBuildWork( team_t team )
{
  int         i;
  qboolean    outpost;
  buildable_t reclaim;

  if( BotChooseBuildableAt( team, BotTeamBuildWeapon( team ), &outpost, &reclaim ) != BA_NONE ||
      reclaim != BA_NONE )
    return qtrue;

  // badly damaged human structures need repairing
  if( team == TEAM_HUMANS )
  {
    for( i = 0; i < botNumBuildables; i++ )
    {
      gentity_t *ent = botBuildables[ i ];

      if( ent->buildableTeam == TEAM_HUMANS && ent->spawned &&
          ent->health < BG_Buildable( ent->s.modelindex )->health * 3 / 4 )
        return qtrue;
    }
  }

  return qfalse;
}

static qboolean BotPlanBuild( gentity_t *self, botState_t *bs )
{
  team_t        team = self->client->pers.teamSelection;
  botTeamBase_t *base = &botBases[ team ];
  vec3_t        anchor;
  float         minDist, maxDist;
  buildable_t   type;
  qboolean      outpost;

  type = BotChooseBuildableAt( team, self->client->ps.weapon, &outpost, NULL );
  if( type == BA_NONE )
    return qfalse;

  if( outpost )
    VectorCopy( base->outpost, anchor );
  else if( base->mainSet )
    VectorCopy( base->main, anchor );
  else
    VectorCopy( self->r.currentOrigin, anchor );

  if( type == BA_H_REACTOR || type == BA_A_OVERMIND ||
      ( outpost && ( type == BA_H_REPEATER ||
                     ( type == BA_A_SPAWN && !BotCountBuildablesNear( team, BA_A_SPAWN,
                                                  anchor, BOT_OUTPOST_RADIUS ) ) ) ) )
  {
    // the structure the rest of the base hangs off goes in the middle
    minDist = 0.0f;
    maxDist = 250.0f;
  }
  else if( outpost )
  {
    // stay inside the repeater's power and the egg's creep
    minDist = 100.0f;
    maxDist = 350.0f;
  }
  // defences spread a little further out than the rest
  else if( type == BA_H_MGTURRET || type == BA_H_TESLAGEN ||
           type == BA_A_ACIDTUBE || type == BA_A_TRAPPER || type == BA_A_HIVE )
  {
    minDist = 200.0f;
    maxDist = 550.0f;
  }
  else
  {
    minDist = 150.0f;
    maxDist = 450.0f;
  }

  if( !G_BotNavRandomNodeNear( anchor, minDist, maxDist, bs->buildSpot ) )
  {
    // no graph yet, build where we stand
    VectorCopy( self->r.currentOrigin, bs->buildSpot );
  }

  bs->buildType = type;
  bs->buildYaw = BotRandom( ) * 360.0f;
  bs->buildTries = 0;
  return qtrue;
}

static void BotGiveUpBuild( botState_t *bs, int delay )
{
  bs->buildType = BA_NONE;
  bs->buildPlaced = qfalse;
  bs->buildSpots = 0;
  bs->nextBuildTime = level.time + delay;
}

// would a structure here leave too narrow a gap to an existing one? A row of
// them can wall off a base even when no single one blocks anything
static qboolean BotTooClose( gentity_t *self, const vec3_t mins, const vec3_t maxs )
{
  // room for a human to walk between, or a tyrant in the alien base
  float gap = ( self->client->pers.teamSelection == TEAM_ALIENS ) ? 70.0f : 40.0f;
  int   i;

  for( i = 0; i < botNumBuildables; i++ )
  {
    gentity_t *ent = botBuildables[ i ];

    // players walk over the low ones
    if( ent->r.absmax[ 2 ] - ent->r.absmin[ 2 ] <= 19.0f )
      continue;

    if( mins[ 0 ] - gap < ent->r.absmax[ 0 ] && maxs[ 0 ] + gap > ent->r.absmin[ 0 ] &&
        mins[ 1 ] - gap < ent->r.absmax[ 1 ] && maxs[ 1 ] + gap > ent->r.absmin[ 1 ] &&
        mins[ 2 ] < ent->r.absmax[ 2 ] && maxs[ 2 ] > ent->r.absmin[ 2 ] )
      return qtrue;
  }

  return qfalse;
}

// try to place the planned structure in the direction we're facing
static qboolean BotTryPlace( gentity_t *self, botState_t *bs, usercmd_t *cmd )
{
  playerState_t   *ps = &self->client->ps;
  vec3_t          origin, normal, angles;
  int             groundEntNum;
  itemBuildError_t err;

  // the build timer is still running from the last structure
  if( ps->stats[ STAT_MISC ] > 0 )
    return qfalse;

  if( ( ps->stats[ STAT_BUILDABLE ] & SB_BUILDABLE_MASK ) != bs->buildType )
  {
    if( level.time >= bs->nextBuildCommandTime )
    {
      bs->nextBuildCommandTime = level.time + 500;
      BotCommand( self, va( "build %s", BG_Buildable( bs->buildType )->name ) );
    }
    return qfalse;
  }

  err = G_CanBuild( self, bs->buildType, (int)BG_Class( ps->stats[ STAT_CLASS ] )->buildDist,
                    origin, normal, angles, &groundEntNum );

  // somewhere it would wall off a corridor or doorway is as bad as no room
  if( err == IBE_NONE || err == IBE_TNODEWARN || err == IBE_RPTNOREAC ||
      err == IBE_RPTPOWERHERE || err == IBE_SPWNWARN )
  {
    vec3_t mins, maxs;

    BG_BuildableBoundingBox( bs->buildType, mins, maxs );
    VectorAdd( origin, mins, mins );
    VectorAdd( origin, maxs, maxs );

    // keep to open floor, away from bridges and ledges, and don't wall off a
    // corridor (players walk over low structures like telenodes)
    if( !G_BotNavOpenGround( mins, maxs ) ||
        ( maxs[ 2 ] - mins[ 2 ] > 19.0f &&
          ( G_BotNavWouldBlock( mins, maxs ) || BotTooClose( self, mins, maxs ) ) ) )
      err = IBE_NOROOM;
  }

  switch( err )
  {
    case IBE_NONE:
    case IBE_TNODEWARN:
    case IBE_RPTNOREAC:
    case IBE_RPTPOWERHERE:
    case IBE_SPWNWARN:
      cmd->buttons |= BUTTON_ATTACK;
      bs->buildPlaced = qtrue;
      return qtrue;

    // nothing we can do about these by moving
    case IBE_NOALIENBP:
    case IBE_NOHUMANBP:
    case IBE_ONEOVERMIND:
    case IBE_ONEREACTOR:
    case IBE_NODCC:
    case IBE_NOOVERMIND:
    case IBE_PERMISSION:
      ps->stats[ STAT_BUILDABLE ] = BA_NONE;
      BotGiveUpBuild( bs, 10000 );
      return qfalse;

    default:
      // try another direction, then another spot
      if( level.time >= bs->nextYawTime )
      {
        bs->nextYawTime = level.time + 250;
        bs->buildYaw = AngleMod( bs->buildYaw + 45.0f );

        if( ++bs->buildTries >= 8 )
        {
          if( ++bs->buildSpots >= 4 )
          {
            ps->stats[ STAT_BUILDABLE ] = BA_NONE;
            BotGiveUpBuild( bs, 15000 );
          }
          else
            BotPlanBuild( self, bs );
        }
      }
      return qfalse;
  }
}

static gentity_t *BotDamagedBuildable( gentity_t *self )
{
  gentity_t *best = NULL;
  float     d, bestDist = Square( 1500.0f );
  int       i;

  for( i = 0; i < botNumBuildables; i++ )
  {
    gentity_t *ent = botBuildables[ i ];

    if( ent->buildableTeam != TEAM_HUMANS || !ent->spawned ||
        ent->health >= BG_Buildable( ent->s.modelindex )->health )
      continue;

    d = DistanceSquared( ent->r.currentOrigin, self->r.currentOrigin );
    if( d < bestDist )
    {
      best = ent;
      bestDist = d;
    }
  }

  return best;
}

static void BotThinkBuilder( gentity_t *self, botState_t *bs, usercmd_t *cmd, int msec )
{
  gclient_t *client = self->client;
  team_t    team = client->pers.teamSelection;
  vec3_t    view, desired, moveDir, dir;
  qboolean  moving = qfalse, jump = qfalse;
  gentity_t *enemy = NULL, *core, *repair;

  VectorCopy( client->ps.viewangles, view );
  VectorClear( moveDir );
  VectorSet( desired, 0.0f, view[ YAW ], 0.0f );

  if( level.time >= bs->nextEnemyTime )
  {
    bs->nextEnemyTime = level.time + BOT_THINK_ENEMY;
    BotFindEnemy( self, bs );
  }

  if( bs->enemy >= 0 && bs->enemyVisible && g_entities[ bs->enemy ].client &&
      Distance( g_entities[ bs->enemy ].r.currentOrigin, self->r.currentOrigin ) < 600.0f )
    enemy = &g_entities[ bs->enemy ];

  core = BotBaseCore( self, team );

  // no longer needed as a builder
  if( !bs->builder && !enemy )
  {
    gentity_t *armoury;

    if( team == TEAM_ALIENS )
    {
      BotEvolve( self, bs );
      if( !BotIsAlive( self ) || !BotIsBuilderWeapon( client->ps.weapon ) )
        return;
    }
    else if( ( armoury = BotNearestBuildable( self, TEAM_HUMANS, BA_H_ARMOURY ) ) )
    {
      BotShop( self, bs );

      if( Distance( armoury->r.currentOrigin, self->r.currentOrigin ) > 64.0f )
      {
        BotPathDirection( self, bs, armoury->r.currentOrigin, moveDir, &jump );
        moving = qtrue;
        vectoangles( moveDir, desired );
        desired[ PITCH ] = 0.0f;
      }
    }

    // with no armoury a human keeps its kit, carry on building meanwhile
  }

  if( moving )
    ;
  else if( enemy )
  {
    // builders can't fight, fall back to the base
    if( core && Distance( core->r.currentOrigin, self->r.currentOrigin ) > 150.0f )
      BotPathDirection( self, bs, core->r.currentOrigin, moveDir, &jump );
    else
      VectorSubtract( self->r.currentOrigin, enemy->r.currentOrigin, moveDir );

    moving = qtrue;
    vectoangles( moveDir, desired );
    desired[ PITCH ] = 0.0f;
    if( team == TEAM_HUMANS &&
        client->ps.stats[ STAT_STAMINA ] > STAMINA_BREATHING_LEVEL + 200 )
      cmd->buttons |= BUTTON_SPRINT;
  }
  else
  {
    // the advanced granger can build more, and evolving into it is free
    if( team == TEAM_ALIENS && client->ps.weapon == WP_ABUILD &&
        BotTeamBuildWeapon( team ) == WP_ABUILD2 && bs->builder &&
        level.time >= bs->nextCommandTime && client->ps.stats[ STAT_MISC ] <= 0 )
    {
      bs->nextCommandTime = level.time + 2000;
      BotCommand( self, va( "class %s", BG_Class( PCL_ALIEN_BUILDER0_UPG )->name ) );
      if( !BotIsAlive( self ) || client->ps.weapon != WP_ABUILD )
        return;
    }

    if( bs->buildType == BA_NONE && bs->reclaimEntity < 0 &&
        level.time >= bs->nextBuildTime )
    {
      bs->buildSpots = 0;
      if( !BotPlanBuild( self, bs ) )
      {
        qboolean    outpost;
        buildable_t reclaim;
        gentity_t   *surplus = NULL;

        // something important needs room: mark surplus for deconstruction
        BotChooseBuildableAt( team, client->ps.weapon, &outpost, &reclaim );
        if( reclaim != BA_NONE )
          surplus = BotFindSurplus( team, reclaim, self );

        if( surplus )
        {
          bs->reclaimEntity = surplus->s.number;
          bs->reclaimUntil = level.time + 20000;
        }
        else
          bs->nextBuildTime = level.time + 3000;
      }
    }

    if( bs->reclaimEntity >= 0 )
    {
      gentity_t *surplus = &g_entities[ bs->reclaimEntity ];
      vec3_t    eye, center;

      if( !surplus->inuse || surplus->s.eType != ET_BUILDABLE ||
          surplus->health <= 0 || surplus->deconstruct || level.time > bs->reclaimUntil )
      {
        // done, or given up; either way plan again straight away
        bs->reclaimEntity = -1;
        bs->nextBuildTime = level.time;
      }
      else
      {
        BotEyes( self, eye );
        BotEntityCenter( surplus, center );

        if( Distance( eye, center ) > 80.0f )
        {
          BotPathDirection( self, bs, surplus->r.currentOrigin, moveDir, &jump );
          moving = qtrue;
          vectoangles( moveDir, desired );
          desired[ PITCH ] = 0.0f;
        }
        else
        {
          VectorSubtract( center, eye, dir );
          VectorNormalize( dir );
          vectoangles( dir, desired );

          if( BotAngleTo( view, dir ) < 8.0f && level.time >= bs->nextBuildCommandTime )
          {
            bs->nextBuildCommandTime = level.time + 500;
            BotCommand( self, "deconstruct" );
          }
        }
      }
    }

    if( bs->reclaimEntity >= 0 )
      ;   // busy marking surplus
    else if( bs->buildType != BA_NONE )
    {
      // placing clears the selected structure once it goes up
      if( bs->buildPlaced &&
          !( client->ps.stats[ STAT_BUILDABLE ] & SB_BUILDABLE_MASK ) )
      {
        BotGiveUpBuild( bs, 1000 );
      }
      else if( Distance( self->r.currentOrigin, bs->buildSpot ) > 40.0f &&
               bs->buildTries == 0 && bs->stuckCount < 2 )
      {
        BotPathDirection( self, bs, bs->buildSpot, moveDir, &jump );
        moving = qtrue;
        vectoangles( moveDir, desired );
        desired[ PITCH ] = 0.0f;
      }
      else
      {
        // at the spot, face the way we're trying to build
        desired[ YAW ] = bs->buildYaw;
        desired[ PITCH ] = 20.0f;

        if( fabs( AngleSubtract( view[ YAW ], bs->buildYaw ) ) < 5.0f )
        {
          if( bs->buildTries == 0 )
            bs->buildTries = 1;
          bs->buildPlaced = qfalse;
          BotTryPlace( self, bs, cmd );
        }
      }
    }
    else if( team == TEAM_HUMANS && ( repair = BotDamagedBuildable( self ) ) )
    {
      // the construction kit repairs whatever it's pointed at
      VectorSubtract( repair->r.currentOrigin, self->r.currentOrigin, dir );

      if( VectorLength( dir ) > 80.0f )
      {
        BotPathDirection( self, bs, repair->r.currentOrigin, moveDir, &jump );
        moving = qtrue;
        vectoangles( moveDir, desired );
        desired[ PITCH ] = 0.0f;
      }
      else
      {
        vec3_t eye, center;

        BotEyes( self, eye );
        BotEntityCenter( repair, center );
        VectorSubtract( center, eye, dir );
        vectoangles( dir, desired );
      }
    }
    else if( core && Distance( core->r.currentOrigin, self->r.currentOrigin ) > 400.0f )
    {
      // nothing to do, stay home
      BotPathDirection( self, bs, core->r.currentOrigin, moveDir, &jump );
      moving = qtrue;
      vectoangles( moveDir, desired );
      desired[ PITCH ] = 0.0f;
    }
  }

  BotTurn( bs, view, desired, msec );
  BotCheckStuck( self, bs, moving );

  if( level.time < bs->unstuckUntil )
    jump = bs->stuckCount > 1;

  if( moving )
  {
    BotSetMovement( self, cmd, view, moveDir,
                    level.time < bs->unstuckUntil ? bs->strafeDir : 0.0f );

    if( !jump && BotShouldJumpObstacle( self, moveDir ) )
      jump = qtrue;
  }

  if( jump && client->ps.groundEntityNum != ENTITYNUM_NONE && BotSafeToJump( self, cmd, view ) )
    cmd->upmove = 127;

  cmd->angles[ PITCH ] = ANGLE2SHORT( view[ PITCH ] ) - client->ps.delta_angles[ PITCH ];
  cmd->angles[ YAW ] = ANGLE2SHORT( view[ YAW ] ) - client->ps.delta_angles[ YAW ];
  cmd->angles[ ROLL ] = ANGLE2SHORT( view[ ROLL ] ) - client->ps.delta_angles[ ROLL ];
}

/*
===============
BotThinkAlive
===============
*/
static void BotThinkAlive( gentity_t *self, botState_t *bs, usercmd_t *cmd, int msec )
{
  gclient_t *client = self->client;
  team_t    team = client->pers.teamSelection;
  gentity_t *enemy = NULL;
  vec3_t    view, desired, dir, aimPoint, eye, toEnemy;
  vec3_t    moveDir;
  qboolean  moving = qfalse, jump = qfalse;
  float     dist = 0.0f, strafe = 0.0f;

  if( BotIsBuilderWeapon( client->ps.weapon ) )
  {
    BotThinkBuilder( self, bs, cmd, msec );
    return;
  }

  VectorCopy( client->ps.viewangles, view );
  VectorClear( moveDir );

  if( level.time >= bs->nextEnemyTime )
  {
    bs->nextEnemyTime = level.time + BOT_THINK_ENEMY;
    BotFindEnemy( self, bs );
  }

  if( bs->enemy >= 0 && bs->enemyVisible )
    enemy = &g_entities[ bs->enemy ];

  if( level.time >= bs->nextGoalTime || bs->goal == BG_NONE ||
      bs->goal == BG_HUNT || bs->goal == BG_HEAL || bs->goal == BG_ARMOURY ||
      bs->goal == BG_REGROUP )
    BotChooseGoal( self, bs );

  // economy
  if( team == TEAM_HUMANS )
  {
    BotShop( self, bs );

    if( BG_InventoryContainsUpgrade( UP_MEDKIT, client->ps.stats ) &&
        client->ps.stats[ STAT_HEALTH ] < client->ps.stats[ STAT_MAX_HEALTH ] / 2 &&
        !( client->ps.stats[ STAT_STATE ] & SS_HEALING_2X ) &&
        level.time >= bs->nextCommandTime )
    {
      bs->nextCommandTime = level.time + 1000;
      BotCommand( self, "itemact medkit" );
    }
  }
  else if( team == TEAM_ALIENS && !enemy )
    BotEvolve( self, bs );

  // the command may have changed our class or killed us
  if( !BotIsAlive( self ) )
    return;

  if( enemy )
  {
    BotEyes( self, eye );
    BotEntityCenter( enemy, aimPoint );

    // the bottom half of a human takes half damage and the head double, so
    // aliens go for the upper body
    if( team == TEAM_ALIENS && enemy->client )
      aimPoint[ 2 ] = enemy->r.absmin[ 2 ] +
                      ( enemy->r.absmax[ 2 ] - enemy->r.absmin[ 2 ] ) * 0.8f;

    // wobble the aim, less for better bots
    if( level.time >= bs->nextAimErrorTime )
    {
      float spread = ( BOT_MAX_SKILL + 1 - bs->skill ) * 1.5f;

      // fast movers are hard to keep a gun on
      if( team == TEAM_HUMANS && enemy->client )
        spread += VectorLength( enemy->client->ps.velocity ) * 0.006f;

      bs->nextAimErrorTime = level.time + 300 + rand( ) % 400;
      VectorSet( bs->aimError, crandom( ) * spread * 0.5f, crandom( ) * spread, 0.0f );
    }

    // react to movement late, so fast targets are hard to track
    if( enemy->client )
      VectorMA( aimPoint, -( BOT_MAX_SKILL + 1 - bs->skill ) * 0.06f,
                enemy->client->ps.velocity, aimPoint );

    VectorSubtract( aimPoint, eye, toEnemy );
    dist = VectorNormalize( toEnemy );
    vectoangles( toEnemy, desired );
    desired[ PITCH ] = AngleMod( desired[ PITCH ] + bs->aimError[ PITCH ] );
    desired[ YAW ] = AngleMod( desired[ YAW ] + bs->aimError[ YAW ] );

    // dragoons leap in an arc
    if( bs->chargeStart && ( client->ps.weapon == WP_ALEVEL3 ||
                             client->ps.weapon == WP_ALEVEL3_UPG ) )
      desired[ PITCH ] = AngleMod( desired[ PITCH ] - 15.0f );

    BotTurn( bs, view, desired, msec );

    // movement in combat
    VectorSubtract( enemy->r.currentOrigin, self->r.currentOrigin, dir );

    if( team == TEAM_ALIENS )
    {
      // charge straight in when we can, otherwise path to them
      if( BotCanWalkTo( self, enemy->r.currentOrigin ) )
        VectorCopy( dir, moveDir );
      else
        BotPathDirection( self, bs, enemy->r.currentOrigin, moveDir, &jump );

      moving = qtrue;

      if( dir[ 2 ] > 48.0f && dist < 200.0f )
        jump = qtrue;

      // weave and hop on the way in so we're harder to hit
      if( dist > 200.0f && enemy->client )
      {
        if( level.time >= bs->nextStrafeTime )
        {
          bs->nextStrafeTime = level.time + 300 + rand( ) % 700;
          bs->strafeDir = ( rand( ) & 1 ) ? 0.7f : -0.7f;
        }
        strafe = bs->strafeDir;

        if( dist < 800.0f && ( client->ps.weapon == WP_ALEVEL0 ||
                               client->ps.weapon == WP_ALEVEL1 ||
                               client->ps.weapon == WP_ALEVEL1_UPG ) &&
            BotRandom( ) < 0.05f )
          jump = qtrue;
      }
    }
    else
    {
      float preferred = BotPreferredRange( client->ps.weapon );

      if( enemy->s.eType == ET_BUILDABLE )
        preferred = MAX( preferred, 250.0f );

      if( dist > preferred * 1.5f )
      {
        if( BotCanWalkTo( self, enemy->r.currentOrigin ) )
          VectorCopy( dir, moveDir );
        else
          BotPathDirection( self, bs, enemy->r.currentOrigin, moveDir, &jump );
        moving = qtrue;
      }
      else if( dist < preferred * 0.6f && enemy->client )
      {
        VectorNegate( dir, moveDir );
        moving = qtrue;
      }

      // dodge from side to side
      if( level.time >= bs->nextStrafeTime )
      {
        bs->nextStrafeTime = level.time + 400 + rand( ) % 900;
        bs->strafeDir = ( rand( ) & 1 ) ? 1.0f : -1.0f;
      }

      if( enemy->client )
      {
        strafe = bs->strafeDir;
        moving = qtrue;
      }
    }

    // fire once we're pointing where we think the target is, the aim
    // wobble means that isn't always quite where it really is
    AngleVectors( desired, toEnemy, NULL, NULL );
    BotAttack( self, bs, cmd, enemy, dist, BotAngleTo( view, toEnemy ) );
  }
  else
  {
    bs->chargeStart = 0;

    if( bs->goal != BG_NONE )
    {
      float goalDist = Distance( self->r.currentOrigin, bs->goalOrigin );
      float closeEnough = 80.0f;

      if( bs->goal == BG_ARMOURY )
        closeEnough = 64.0f;
      else if( bs->goal == BG_HEAL )
      {
        // medistations only heal whoever stands on top of them
        closeEnough = ( team == TEAM_HUMANS ) ? 12.0f : 120.0f;
        if( team == TEAM_HUMANS )
          goalDist = sqrt( Square( self->r.currentOrigin[ 0 ] - bs->goalOrigin[ 0 ] ) +
                           Square( self->r.currentOrigin[ 1 ] - bs->goalOrigin[ 1 ] ) );
      }
      else if( bs->goal == BG_ATTACK )
        closeEnough = 160.0f;

      if( goalDist > closeEnough )
      {
        BotPathDirection( self, bs, bs->goalOrigin, moveDir, &jump );
        moving = qtrue;
      }
      else if( bs->goal == BG_HUNT )
      {
        // nobody here
        bs->enemy = -1;
        bs->goal = BG_NONE;
      }
    }

    // look where we're going
    if( moving && VectorLength( moveDir ) > 0.1f )
    {
      vectoangles( moveDir, desired );
      desired[ PITCH ] = 0.0f;
      BotTurn( bs, view, desired, msec );
    }

    // humans sprint when not fighting, but stop before running out of
    // stamina since that slows them right down
    if( team == TEAM_HUMANS && moving &&
        client->ps.stats[ STAT_STAMINA ] > STAMINA_BREATHING_LEVEL + 200 )
      cmd->buttons |= BUTTON_SPRINT;
  }

  BotCheckStuck( self, bs, moving );

  if( level.time < bs->unstuckUntil )
  {
    strafe = bs->strafeDir;
    if( bs->stuckCount > 1 )
      jump = qtrue;
  }

  if( moving )
  {
    BotSetMovement( self, cmd, view, moveDir, strafe );

    if( !jump && BotShouldJumpObstacle( self, moveDir ) )
      jump = qtrue;
  }
  else if( strafe != 0.0f )
  {
    vec3_t none = { 0.0f, 0.0f, 0.0f };

    BotSetMovement( self, cmd, view, none, strafe );
  }

  if( jump && client->ps.groundEntityNum != ENTITYNUM_NONE && BotSafeToJump( self, cmd, view ) )
    cmd->upmove = 127;

  cmd->angles[ PITCH ] = ANGLE2SHORT( view[ PITCH ] ) - client->ps.delta_angles[ PITCH ];
  cmd->angles[ YAW ] = ANGLE2SHORT( view[ YAW ] ) - client->ps.delta_angles[ YAW ];
  cmd->angles[ ROLL ] = ANGLE2SHORT( view[ ROLL ] ) - client->ps.delta_angles[ ROLL ];
}

/*
===============
BotThink

Fill in the usercmd for a bot
===============
*/
static void BotThink( gentity_t *self, botState_t *bs, usercmd_t *cmd, int msec )
{
  gclient_t *client = self->client;
  team_t    team = client->pers.teamSelection;
  int       clientNum = self - g_entities;
  weapon_t  weapon;

  memset( cmd, 0, sizeof( *cmd ) );
  cmd->serverTime = level.time;

  // keep the current view
  cmd->angles[ PITCH ] = ANGLE2SHORT( client->ps.viewangles[ PITCH ] ) - client->ps.delta_angles[ PITCH ];
  cmd->angles[ YAW ] = ANGLE2SHORT( client->ps.viewangles[ YAW ] ) - client->ps.delta_angles[ YAW ];

  weapon = client->ps.stats[ STAT_WEAPON ];
  if( team == TEAM_HUMANS && !BG_Weapon( weapon )->infiniteAmmo &&
      client->ps.ammo <= 0 && client->ps.clips <= 0 )
    weapon = WP_BLASTER;
  cmd->weapon = weapon;

  if( level.intermissiontime )
  {
    // ready up so we don't hold up the next map
    if( level.time & 512 )
      cmd->buttons |= BUTTON_ATTACK;
    return;
  }

  if( team == TEAM_NONE )
  {
    if( bs->team != TEAM_NONE && level.time >= bs->nextCommandTime )
    {
      bs->nextCommandTime = level.time + 2000;
      BotCommand( self, va( "team %s", bs->team == TEAM_ALIENS ? "aliens" : "humans" ) );

      // the team command refuses uneven teams, bots are only there to fill
      // in so put them on the team they were asked for
      if( client->pers.teamSelection == TEAM_NONE )
        G_ChangeTeam( self, bs->team );
    }
    return;
  }

  if( client->sess.spectatorState != SPECTATOR_NOT )
  {
    spawnQueue_t *queue = ( team == TEAM_ALIENS ) ?
                          &level.alienSpawnQueue : &level.humanSpawnQueue;

    if( !G_SearchSpawnQueue( queue, clientNum ) &&
        level.time >= bs->nextCommandTime )
    {
      const char *spawnAs;

      bs->nextCommandTime = level.time + 1000;

      if( team == TEAM_ALIENS )
      {
        class_t c = PCL_ALIEN_LEVEL0;

        if( bs->builder )
          c = ( BG_ClassIsAllowed( PCL_ALIEN_BUILDER0_UPG ) &&
                BG_ClassAllowedInStage( PCL_ALIEN_BUILDER0_UPG, g_alienStage.integer ) ) ?
              PCL_ALIEN_BUILDER0_UPG : PCL_ALIEN_BUILDER0;
        spawnAs = BG_Class( c )->name;
      }
      else
        spawnAs = BG_Weapon( bs->builder ? WP_HBUILD : WP_MACHINEGUN )->name;

      BotCommand( self, va( "class %s", spawnAs ) );
    }

    bs->enemy = -1;
    bs->goal = BG_NONE;
    bs->pathLength = 0;
    bs->buildType = BA_NONE;
    bs->reclaimEntity = -1;
    bs->regroupUntil = level.time + 15000;
    return;
  }

  if( client->ps.stats[ STAT_HEALTH ] <= 0 )
    return;

  BotThinkAlive( self, bs, cmd, msec );
}

/*
===============
Bot management
===============
*/
static int BotCountTeam( team_t team, qboolean bots )
{
  int i, count = 0;

  for( i = 0; i < level.maxclients; i++ )
  {
    gclient_t *cl = &level.clients[ i ];

    if( cl->pers.connected == CON_DISCONNECTED || cl->pers.isBot != bots )
      continue;

    if( bots ? botStates[ i ].team == team : cl->pers.teamSelection == team )
      count++;
  }

  return count;
}

static qboolean BotNameInUse( const char *name )
{
  int i;

  for( i = 0; i < level.maxclients; i++ )
  {
    if( level.clients[ i ].pers.connected != CON_DISCONNECTED &&
        !Q_stricmp( level.clients[ i ].pers.netname, name ) )
      return qtrue;
  }

  return qfalse;
}

static void BotPickName( team_t team, char *out, int size )
{
  const char  **names = ( team == TEAM_ALIENS ) ? botAlienNames : botHumanNames;
  int         count = ( team == TEAM_ALIENS ) ? ARRAY_LEN( botAlienNames ) :
                                                ARRAY_LEN( botHumanNames );
  int         start = rand( ) % count;
  int         i;

  for( i = 0; i < count; i++ )
  {
    Com_sprintf( out, size, "[bot]%s", names[ ( start + i ) % count ] );
    if( !BotNameInUse( out ) )
      return;
  }

  Com_sprintf( out, size, "[bot]%s%d", names[ start ], rand( ) % 100 );
}

static team_t BotParseTeam( const char *s )
{
  if( !Q_stricmp( s, "aliens" ) || !Q_stricmp( s, "a" ) || !Q_stricmp( s, "alien" ) )
    return TEAM_ALIENS;
  if( !Q_stricmp( s, "humans" ) || !Q_stricmp( s, "h" ) || !Q_stricmp( s, "human" ) )
    return TEAM_HUMANS;
  return TEAM_NONE;
}

qboolean G_AddBot( const char *name, team_t team, int skill )
{
  int       clientNum;
  char      userinfo[ MAX_INFO_STRING ];
  char      botName[ MAX_NAME_LENGTH ];
  const char *denied;

  if( team != TEAM_ALIENS && team != TEAM_HUMANS )
  {
    team = ( BotCountTeam( TEAM_ALIENS, qfalse ) + BotCountTeam( TEAM_ALIENS, qtrue ) <=
             BotCountTeam( TEAM_HUMANS, qfalse ) + BotCountTeam( TEAM_HUMANS, qtrue ) ) ?
           TEAM_ALIENS : TEAM_HUMANS;
  }

  if( skill < BOT_MIN_SKILL || skill > BOT_MAX_SKILL )
    skill = MAX( BOT_MIN_SKILL, MIN( BOT_MAX_SKILL, bot_skill.integer ) );

  if( name && *name )
    Com_sprintf( botName, sizeof( botName ), "[bot]%s", name );
  else
    BotPickName( team, botName, sizeof( botName ) );

  clientNum = trap_BotAllocateClient( );
  if( clientNum < 0 )
  {
    G_Printf( "addbot: no free client slots (raise sv_maxclients)\n" );
    return qfalse;
  }

  userinfo[ 0 ] = '\0';
  Info_SetValueForKey( userinfo, "name", botName );
  Info_SetValueForKey( userinfo, "rate", "25000" );
  Info_SetValueForKey( userinfo, "snaps", "20" );
  Info_SetValueForKey( userinfo, "skill", va( "%d", skill ) );
  Info_SetValueForKey( userinfo, "bot_team", va( "%d", team ) );
  Info_SetValueForKey( userinfo, "cl_guid",
                       va( "B07B07B0%08X%08X%08X", clientNum, level.time, rand( ) ) );
  trap_SetUserinfo( clientNum, userinfo );

  denied = ClientConnect( clientNum, qtrue, qtrue );
  if( denied )
  {
    G_Printf( "addbot: %s\n", denied );
    trap_BotFreeClient( clientNum );
    return qfalse;
  }

  ClientBegin( clientNum );
  return qtrue;
}

/*
===============
G_BotConnect

Called from ClientConnect for bots, including after a map change
===============
*/
void G_BotConnect( int clientNum )
{
  botState_t  *bs = &botStates[ clientNum ];
  char        userinfo[ MAX_INFO_STRING ];

  trap_GetUserinfo( clientNum, userinfo, sizeof( userinfo ) );

  memset( bs, 0, sizeof( *bs ) );
  bs->inuse = qtrue;
  bs->skill = atoi( Info_ValueForKey( userinfo, "skill" ) );
  if( bs->skill < BOT_MIN_SKILL || bs->skill > BOT_MAX_SKILL )
    bs->skill = 3;
  bs->team = atoi( Info_ValueForKey( userinfo, "bot_team" ) );
  if( bs->team != TEAM_ALIENS && bs->team != TEAM_HUMANS )
    bs->team = TEAM_ALIENS;
  bs->enemy = -1;
  bs->goalEntity = -1;
  bs->reclaimEntity = -1;
  bs->strafeDir = 1.0f;
  // give the client a moment to finish joining before acting
  bs->nextCommandTime = level.time + 500 + rand( ) % 1000;

  g_entities[ clientNum ].r.svFlags |= SVF_BOT;
}

void G_BotDisconnect( int clientNum )
{
  memset( &botStates[ clientNum ], 0, sizeof( botStates[ clientNum ] ) );
  g_entities[ clientNum ].r.svFlags &= ~SVF_BOT;
}

static void G_KickBots( team_t team )
{
  int i;

  for( i = 0; i < level.maxclients; i++ )
  {
    if( !level.clients[ i ].pers.isBot ||
        level.clients[ i ].pers.connected == CON_DISCONNECTED )
      continue;

    if( team != TEAM_NONE && botStates[ i ].team != team )
      continue;

    trap_DropClient( i, "was kicked" );
  }
}

// add or remove bots so each team has bot_minplayers players
static void BotCheckMinPlayers( void )
{
  team_t      team;
  int         i, players, bots;

  // after a map change or restart the existing bots reconnect a few frames
  // after the level starts, so don't count until they are back
  if( level.time < botNextMinPlayersCheck || level.intermissiontime ||
      level.time - level.startTime < 3000 )
    return;
  botNextMinPlayersCheck = level.time + 2000;

  if( bot_minplayers.integer <= 0 )
    return;

  for( team = TEAM_ALIENS; team <= TEAM_HUMANS; team++ )
  {
    players = BotCountTeam( team, qfalse );
    bots = BotCountTeam( team, qtrue );

    if( players + bots < bot_minplayers.integer )
    {
      G_AddBot( NULL, team, -1 );
      return;
    }

    if( bots > 0 && players + bots > bot_minplayers.integer )
    {
      for( i = level.maxclients - 1; i >= 0; i-- )
      {
        if( level.clients[ i ].pers.isBot &&
            level.clients[ i ].pers.connected != CON_DISCONNECTED &&
            botStates[ i ].team == team )
        {
          trap_DropClient( i, "was kicked" );
          return;
        }
      }
    }
  }
}

// how suited a bot is to becoming a builder right now, higher is better
static int BotBuilderPreference( int clientNum )
{
  gclient_t *client = &level.clients[ clientNum ];

  if( BotIsBuilderWeapon( client->ps.weapon ) && client->ps.stats[ STAT_HEALTH ] > 0 )
    return 3;   // already holding the tools

  if( client->sess.spectatorState != SPECTATOR_NOT || client->ps.stats[ STAT_HEALTH ] <= 0 )
    return 2;   // will spawn as one

  // humans can swap at an armoury, aliens have to wait until they respawn
  return client->pers.teamSelection == TEAM_HUMANS ? 1 : 0;
}

// switch bots between building and fighting as the base needs it
static void BotAssignBuilders( void )
{
  team_t      team;
  int         i, bots, builders, wanted, pref, best, bestPref;
  qboolean    coreMissing;

  if( level.time < botNextBuilderCheck )
    return;
  botNextBuilderCheck = level.time + 2000;

  for( team = TEAM_ALIENS; team <= TEAM_HUMANS; team++ )
  {
    bots = builders = 0;
    for( i = 0; i < level.maxclients; i++ )
    {
      if( !level.clients[ i ].pers.isBot ||
          level.clients[ i ].pers.connected != CON_CONNECTED ||
          botStates[ i ].team != team )
        continue;

      bots++;
      if( botStates[ i ].builder )
        builders++;
    }

    BotUpdateBases( team );
    coreMissing = !BotTeamCore( team );

    // keep builders around a little after the work runs out so they don't
    // flip back and forth every time something gets damaged
    if( coreMissing || BotTeamHasBuildWork( team ) )
      botLastBuildWork[ team ] = level.time;

    wanted = 0;
    if( botLastBuildWork[ team ] && level.time - botLastBuildWork[ team ] < 15000 )
      wanted = MAX( 0, bot_builders.integer );

    // leave somebody to fight unless the base needs rebuilding
    if( bots < 2 && !coreMissing )
      wanted = 0;
    wanted = MIN( wanted, bots );

    // promote the bots that can switch soonest, demote the slowest
    while( builders != wanted )
    {
      best = -1;
      bestPref = 0;

      for( i = 0; i < level.maxclients; i++ )
      {
        if( !level.clients[ i ].pers.isBot ||
            level.clients[ i ].pers.connected != CON_CONNECTED ||
            botStates[ i ].team != team ||
            botStates[ i ].builder != ( builders > wanted ) )
          continue;

        pref = BotBuilderPreference( i );
        if( builders > wanted )
          pref = -pref;

        if( best < 0 || pref > bestPref )
        {
          best = i;
          bestPref = pref;
        }
      }

      if( best < 0 )
        break;

      botStates[ best ].builder = builders < wanted;
      builders += builders < wanted ? 1 : -1;
    }
  }
}

void G_BotInit( void )
{
  memset( botStates, 0, sizeof( botStates ) );
  botNextMinPlayersCheck = 0;
  botNextBuilderCheck = 0;
  memset( botBases, 0, sizeof( botBases ) );
  memset( botLastBuildWork, 0, sizeof( botLastBuildWork ) );
  G_BotNavInit( );
}

/*
===============
G_BotRunFrame
===============
*/
void G_BotRunFrame( int msec )
{
  gentity_t *ent;
  int       i, numBots = 0;

  BotCheckMinPlayers( );

  for( i = 0; i < level.maxclients; i++ )
  {
    if( level.clients[ i ].pers.isBot &&
        level.clients[ i ].pers.connected == CON_CONNECTED )
      numBots++;
  }

  if( !numBots )
    return;

  if( !level.intermissiontime )
    G_BotNavFrame( MAX( 1, bot_navBudget.integer ) );

  BotAssignBuilders( );

  // gather targets once for everybody
  botNumPlayers = 0;
  botNumBuildables = 0;
  for( i = 0, ent = g_entities; i < level.num_entities; i++, ent++ )
  {
    if( !ent->inuse )
      continue;

    if( i < MAX_CLIENTS )
    {
      if( BotIsAlive( ent ) )
        botPlayers[ botNumPlayers++ ] = ent;
    }
    else if( ent->s.eType == ET_BUILDABLE && ent->health > 0 )
      botBuildables[ botNumBuildables++ ] = ent;
  }

  for( i = 0; i < level.maxclients; i++ )
  {
    gclient_t *client = &level.clients[ i ];

    if( !client->pers.isBot || client->pers.connected != CON_CONNECTED )
      continue;

    ent = &g_entities[ i ];

    if( !botStates[ i ].inuse )
      G_BotConnect( i );

    // report where and why bots die, to find map hazards they walk into
    if( bot_debug.integer )
    {
      botState_t *bs = &botStates[ i ];
      qboolean   alive = BotIsAlive( ent );

      if( bs->wasAlive && !alive )
        G_Printf( "botdeath: %s at (%.0f %.0f %.0f) vel (%.0f %.0f %.0f) goal %d enemy %d%s mod %d "
                  "last ground (%.0f %.0f %.0f) %dms ago, alive %dms\n",
                  client->pers.netname, client->ps.origin[ 0 ], client->ps.origin[ 1 ],
                  client->ps.origin[ 2 ], client->ps.velocity[ 0 ], client->ps.velocity[ 1 ],
                  client->ps.velocity[ 2 ], bs->goal, bs->enemy,
                  bs->enemyVisible ? " (visible)" : "", client->lasthurt_mod,
                  bs->lastGround[ 0 ], bs->lastGround[ 1 ], bs->lastGround[ 2 ],
                  level.time - bs->lastGroundTime, level.time - bs->spawnTime );
      if( alive && !bs->wasAlive )
        bs->spawnTime = level.time;
      if( alive && client->ps.groundEntityNum != ENTITYNUM_NONE )
      {
        VectorCopy( client->ps.origin, bs->lastGround );
        bs->lastGroundTime = level.time;
      }
      bs->wasAlive = alive;
    }

    BotThink( ent, &botStates[ i ], &client->pers.cmd, msec );

    // the think may have issued a command that kicked or moved the bot
    if( !client->pers.isBot || client->pers.connected != CON_CONNECTED )
      continue;

    client->pers.cmd.serverTime = level.time;
    client->lastCmdTime = level.time;
    ClientThink_real( ent );
  }
}

/*
===============
Console commands
===============
*/
void Svcmd_AddBot_f( void )
{
  char    name[ MAX_NAME_LENGTH ] = "";
  char    arg[ MAX_TOKEN_CHARS ];
  team_t  team = TEAM_NONE;
  int     skill = -1;
  int     i;

  // any order: a team name, a skill number, anything else is the name
  for( i = 1; i < trap_Argc( ); i++ )
  {
    trap_Argv( i, arg, sizeof( arg ) );

    if( BotParseTeam( arg ) != TEAM_NONE )
      team = BotParseTeam( arg );
    else if( arg[ 0 ] >= '0' && arg[ 0 ] <= '9' && !arg[ 1 ] )
      skill = atoi( arg );
    else
      Q_strncpyz( name, arg, sizeof( name ) );
  }

  if( !Q_stricmp( name, "random" ) )
    name[ 0 ] = '\0';

  G_AddBot( name, team, skill );
}

void Svcmd_BotStatus_f( void )
{
  static const char *goalNames[ ] = { "none", "attack", "hunt", "armoury", "heal", "regroup" };
  int i;

  G_Printf( "navigation graph: %d nodes\n", G_BotNavNumNodes( ) );

  {
    team_t      team;
    buildable_t next;

    for( team = TEAM_ALIENS; team <= TEAM_HUMANS; team++ )
    {
      botTeamBase_t *base = &botBases[ team ];
      qboolean      outpost;

      buildable_t   reclaim;

      next = BotChooseBuildableAt( team, BotTeamBuildWeapon( team ), &outpost, &reclaim );
      G_Printf( "%s: stage %d, %d build points left (%d marked), next to build: %s%s%s%s\n",
                BG_TeamName( team ),
                ( team == TEAM_HUMANS ? g_humanStage.integer : g_alienStage.integer ) + 1,
                team == TEAM_HUMANS ? level.humanBuildPoints : level.alienBuildPoints,
                BotMarkedPoints( team ),
                next != BA_NONE ? BG_Buildable( next )->name : "nothing",
                outpost ? " (outpost)" : "",
                reclaim != BA_NONE ? ", making room for " : "",
                reclaim != BA_NONE ? BG_Buildable( reclaim )->name : "" );
      if( base->mainSet )
        G_Printf( "  base at (%.0f %.0f %.0f)\n", base->main[ 0 ], base->main[ 1 ], base->main[ 2 ] );
      if( base->outpostSet )
        G_Printf( "  outpost at (%.0f %.0f %.0f)\n",
                  base->outpost[ 0 ], base->outpost[ 1 ], base->outpost[ 2 ] );
    }
  }

  for( i = 0; i < level.maxclients; i++ )
  {
    gclient_t   *client = &level.clients[ i ];
    botState_t  *bs = &botStates[ i ];

    if( !client->pers.isBot || client->pers.connected == CON_DISCONNECTED )
      continue;

    G_Printf( "%2d %-20s skill %d %s %-8s hp %3d cr %4d pos (%5.0f %5.0f %5.0f) "
              "goal %-7s ent %4d path %3d/%-3d enemy %4d%s move %d/%d/%d btn %d spd %.0f\n",
              i, client->pers.netname, bs->skill, BG_TeamName( bs->team ),
              BG_Class( client->ps.stats[ STAT_CLASS ] )->name,
              client->ps.stats[ STAT_HEALTH ], client->pers.credit,
              client->ps.origin[ 0 ], client->ps.origin[ 1 ], client->ps.origin[ 2 ],
              goalNames[ bs->goal ], bs->goalEntity, bs->pathIndex, bs->pathLength,
              bs->enemy, bs->enemyVisible ? " (visible)" : "",
              client->pers.cmd.forwardmove, client->pers.cmd.rightmove,
              client->pers.cmd.upmove, client->pers.cmd.buttons,
              VectorLength( client->ps.velocity ) );

    G_Printf( "   role %s, weapon %s%s%s\n", bs->builder ? "builder" : "fighter",
              BG_Weapon( client->ps.weapon )->name,
              bs->buildType != BA_NONE ? ", building " : "",
              bs->buildType != BA_NONE ? BG_Buildable( bs->buildType )->name : "" );

    if( bs->pathIndex < bs->pathLength )
    {
      vec3_t node;
      int    j;

      G_Printf( "   path:" );
      for( j = MAX( 0, bs->pathIndex - 1 ); j < bs->pathLength && j < bs->pathIndex + 3; j++ )
      {
        G_BotNavNodeOrigin( bs->path[ j ], node );
        G_Printf( " %d(%.0f %.0f %.0f)%s", bs->path[ j ], node[ 0 ], node[ 1 ], node[ 2 ],
                  j > 0 && ( G_BotNavLinkFlags( bs->path[ j - 1 ], bs->path[ j ] ) & NAVLINK_JUMP ) ?
                  "J" : "" );
      }
      G_Printf( "\n" );
    }
  }
}

void Svcmd_KickBots_f( void )
{
  char arg[ MAX_TOKEN_CHARS ];

  trap_Argv( 1, arg, sizeof( arg ) );
  G_KickBots( BotParseTeam( arg ) );
}
