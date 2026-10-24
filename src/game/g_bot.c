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
  botstatus                   what every bot is doing, and abilities used so far
  navdump                     write the navigation graph to nav/<map>.txt
  navtrace x y z x y z [class] trace a player box and report what it hits
  navpath x y z x y z [class] find a route between two points for a class

Cvars:
  bot_minplayers  keep at least this many players on each team, using bots
  bot_skill       default skill for new bots (1-5)
  bot_humanAimHandicap  skill levels (0-10) taken off a human bot's aim and
                  reaction when its target is a real player, not another bot
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
#define NAV_STEP_HEIGHT       18.0f   // STEPSIZE in bg_pmove.c
#define BOT_MIN_SKILL         1
#define BOT_MAX_SKILL         5

typedef enum
{
  BG_NONE,
  BG_ATTACK,      // head for the enemy base
  BG_HUNT,        // chase where an enemy was last seen
  BG_ARMOURY,     // humans: buy weapons, armour and ammo
  BG_HEAL,        // retreat somewhere to recover health
  BG_REGROUP,     // aliens: join up with a teammate before attacking
  BG_ROAM,        // look around parts of the map the team hasn't been to lately
  BG_DEFEND       // enemies in our base, go back and see them off
} botGoal_t;

// how BotPathDirection wants us to move
#define BOT_MOVE_JUMP     1
#define BOT_MOVE_CROUCH   2
#define BOT_MOVE_CLIMB    4

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
  qboolean  pathShort;        // the path stops well short of where we're going
  qboolean  pathRetried;      // and we've looked again from the end of it
  qboolean  offGraph;         // nowhere near the graph, heading for offGraphPoint
  vec3_t    offGraphPoint;
  int       goalFailTime;     // when a goal last turned out to be out of reach
  int       idleUntil;        // goals keep failing, wander about until then
  int       routeSeed;        // which way this bot leans when there's a choice of routes
  vec3_t    lastFramePos;     // to notice being teleported

  // to come at the goal from another side, go via here first
  vec3_t    viaPoint;
  qboolean  viaActive;

  // enemy
  int       enemy;
  int       enemySightTime;
  int       enemyLastSeen;
  vec3_t    enemyLastPos;
  qboolean  enemyVisible;
  int       nextEnemyTime;

  // aim
  vec3_t    aimError;       // degrees
  vec3_t    aimMiss;        // units right and up of the target, real players only
  int       nextAimErrorTime;

  // movement
  vec3_t    lastPos;
  vec3_t    lastMoveDir;      // where we last tried to go, for bot_debug
  signed char lastMove[ 3 ];  // and the moves we made
  int       groundFail;       // why we last wouldn't step forward
  int       nextStuckTime;
  int       stuckCount;
  int       unstuckUntil;
  float     strafeDir;
  int       nextStrafeTime;

  // steering round something in the way
  vec3_t    avoidDir;
  int       avoidUntil;

  int       wallUntil;        // aliens: walk on walls until then
  int       noWallUntil;      // and not before then, after dropping off
  int       noCeilingUntil;   // crawling along ceilings didn't work out, walk for a while
  int       crouchUntil;      // humans: stay crouched until then
  qboolean  sprinting;        // humans: sprint until stamina runs low, then rest
  qboolean  wasOnWall;
  qboolean  wasOnGround;
  vec3_t    wallSpot;         // the wall we're heading for to get up to the ceiling
  int       wallCheckTime;    // on a wall: when to see if we're getting closer
  float     wallCheckDist;
  int       nextWallSearch;
  int       nextDodgeTime;
  int       nextHopTime;      // humans: jumping about to put off aliens up close
  int       hoverUntil;       // humans: keep up in the air on the jetpack until then
  int       nextHealCheck;    // when to next see if we're hurt enough to go and heal
  int       nextReachCheck;   // big aliens: can we still get to the enemy from here?
  int       reachFailures;
  vec3_t    reachOrigin;
  int       nextRetreatCheck; // and whether running for it would do any good
  qboolean  retreatHopeless;
  int       nextMedkitTime;

  // stuck in the same place again and again: wander off, and in the end
  // give up and respawn
  vec3_t    trapOrigin;
  int       trapStart;
  int       trapCount;

  // moving about but getting nowhere, like pacing round an alcove
  vec3_t    progressOrigin;
  int       progressTime;
  int       statCircling;
  vec3_t    wanderDir;
  int       wanderUntil;
  int       nextJetCommandTime;

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
  qboolean    buildCrowded;   // nowhere spaced out could be found, so close to others will do
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
  int         pathHopTime;    // the jump is onto our own structures
  int         tunnelTime;     // when we were last in a tunnel

  // humans fire in bursts rather than holding the trigger down
  int         burstUntil;
  int         burstPauseUntil;

  int         regroupUntil;   // aliens: wait for company until then

  qboolean    wasAlive;       // for bot_debug death reports
  vec3_t      lastGround;
  int         lastGroundTime;
  int         spawnTime;
  int         lastClass, lastWallTime, lastJumpTime;

  // what the bot has been up to, for botstatus
  int         statStuck;
  int         statSprint, statCrouch, statWall, statCeiling, statJet, statTunnel;   // msec
  int         statJumps, statHops, statClimbs, statDodges;
  int         statRetreats, statMedkits, statLastStands;
  byte        statRegions[ 128 ];   // areas visited, a bit each
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
static qboolean   botWasAlive[ MAX_CLIENTS ];
static int        botNextStatsTime;

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

static qboolean BotIsBuilderWeapon( weapon_t weapon );

// small aliens can't survive base defences, they're better off hunting players
static qboolean BotIsWeakAlien( gentity_t *ent )
{
  class_t c = ent->client->ps.stats[ STAT_CLASS ];

  return ent->client->pers.teamSelection == TEAM_ALIENS &&
         ( c == PCL_ALIEN_LEVEL0 || c == PCL_ALIEN_LEVEL1 ||
           c == PCL_ALIEN_LEVEL1_UPG || c == PCL_ALIEN_BUILDER0 ||
           c == PCL_ALIEN_BUILDER0_UPG );
}

static void BotCommand( gentity_t *ent, const char *cmd )
{
  trap_BotClientCommand( ent - g_entities, cmd );
}

static qboolean BotCanClimb( gentity_t *self )
{
  return BG_ClassHasAbility( self->client->ps.stats[ STAT_CLASS ], SCA_WALLCLIMBER );
}

// a jetpack only works while the reactor stands
static qboolean BotCanFly( gentity_t *self )
{
  return BG_InventoryContainsUpgrade( UP_JETPACK, self->client->ps.stats ) && G_Reactor( );
}

// on a wall or ceiling, not just ready to grab one
static qboolean BotOnWall( gentity_t *self )
{
  playerState_t *ps = &self->client->ps;
  vec3_t        normal;

  if( !( ps->stats[ STAT_STATE ] & SS_WALLCLIMBING ) || ps->groundEntityNum == ENTITYNUM_NONE )
    return qfalse;

  BG_GetClientNormal( ps, normal );
  return normal[ 2 ] < 0.7f;
}

// how this bot looks for routes
static void BotNavQuery( gentity_t *self, botState_t *bs, navQuery_t *query )
{
  gclient_t *client = self->client;
  class_t   cl = client->ps.stats[ STAT_CLASS ];
  float     jump = BG_Class( cl )->jumpMagnitude;

  memset( query, 0, sizeof( *query ) );
  query->size = G_BotNavSizeClass( self->r.mins, self->r.maxs );
  query->team = client->pers.teamSelection;
  // a jetpack gets up walls as well as claws do
  query->climb = BotCanClimb( self ) || BotCanFly( self );
  query->seed = bs->routeSeed;

  // how high a jump takes us, with a little to spare for stepping up at the
  // top of it. Humans need the stamina
  query->jump = jump * jump / ( 2.0f * MAX( g_gravity.value, 1.0f ) ) + 6.0f;
  if( query->team == TEAM_HUMANS &&
      client->ps.stats[ STAT_STAMINA ] < STAMINA_SLOW_LEVEL + STAMINA_JUMP_TAKE + 100 )
    query->jump = 0.0f;
  query->hop = query->jump;

  // errands are best done quickly, fighters spread out and keep clear of
  // the enemy's defences and anywhere the team keeps dying
  if( BotIsBuilderWeapon( client->ps.weapon ) || bs->goal == BG_HEAL || bs->goal == BG_ARMOURY )
  {
    query->noise = 0.15f;
    query->caution = 0.5f;
  }
  else
  {
    query->noise = 0.5f + bs->skill * 0.1f;
    query->caution = BotIsWeakAlien( self ) ? 2.0f : 1.0f;
  }
}

// pmove turns a wall walker's view to match the surface it's on, so turn
// the view we want back the other way. Only the direction we face can be
// matched, the roll is up to the surface
static void BotSetCmdAngles( gentity_t *self, usercmd_t *cmd, const vec3_t view )
{
  playerState_t *ps = &self->client->ps;
  vec3_t        angles, axis[ 3 ], local[ 3 ];

  VectorSet( angles, view[ PITCH ], view[ YAW ], 0.0f );

  if( ps->stats[ STAT_STATE ] & SS_WALLCLIMBING )
  {
    AnglesToAxis( angles, axis );
    if( BG_RotateAxis( ps->grapplePoint, axis, local, qtrue,
                       ps->eFlags & EF_WALLCLIMBCEILING ) )
      vectoangles( local[ 0 ], angles );
  }

  cmd->angles[ PITCH ] = ANGLE2SHORT( angles[ PITCH ] ) - ps->delta_angles[ PITCH ];
  cmd->angles[ YAW ] = ANGLE2SHORT( angles[ YAW ] ) - ps->delta_angles[ YAW ];
  cmd->angles[ ROLL ] = 0;
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

      // eggs and the overmind are what win games, and grangers soon put
      // defences back up, so humans go for those first
      score = d;
      if( team == TEAM_HUMANS &&
          ( other->s.modelindex == BA_A_SPAWN || other->s.modelindex == BA_A_OVERMIND ) )
        score *= 0.5f;

      if( !best || score < bestScore )
      {
        best = other;
        bestScore = score;
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

// half the humans fly about with a jetpack rather than save up for a
// battlesuit, which takes its place on the back
static qboolean BotJetpacker( gentity_t *self )
{
  return ( self - g_entities ) & 1;
}

static upgrade_t BotWantedUpgrade( gentity_t *self )
{
  gclient_t *client = self->client;
  int       *stats = client->ps.stats;
  static const upgrade_t wanted[ ] = { UP_LIGHTARMOUR, UP_HELMET, UP_JETPACK };
  int       i;

  // a battlesuit takes the place of the other armour
  if( BG_InventoryContainsUpgrade( UP_BATTLESUIT, stats ) )
    return UP_NONE;

  if( !BotJetpacker( self ) && !BG_InventoryContainsUpgrade( UP_JETPACK, stats ) &&
      BG_UpgradeIsAllowed( UP_BATTLESUIT ) &&
      BG_UpgradeAllowedInStage( UP_BATTLESUIT, g_humanStage.integer ) &&
      client->pers.credit >= BG_Upgrade( UP_BATTLESUIT )->price )
    return UP_BATTLESUIT;

  for( i = 0; i < ARRAY_LEN( wanted ); i++ )
  {
    upgrade_t u = wanted[ i ];

    if( BG_InventoryContainsUpgrade( u, stats ) || !BG_UpgradeIsAllowed( u ) ||
        !BG_UpgradeAllowedInStage( u, g_humanStage.integer ) ||
        ( u == UP_JETPACK && !BotJetpacker( self ) ) )
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

static gentity_t *BotTeamCore( team_t team );
static gentity_t *BotRandomEnemyBuildable( gentity_t *self );

// could we get to the enemy as this class? Some maps only let the big aliens
// out of their base through gaps they don't fit, or past barricades they
// can't jump onto
static qboolean BotClassCanReach( gentity_t *self, botState_t *bs, class_t cl )
{
  gentity_t   *target = BotTeamCore( TEAM_HUMANS );
  vec3_t      mins, maxs, end;
  navQuery_t  query;
  int         path[ BOT_MAX_PATH ], n;
  float       jump = BG_Class( cl )->jumpMagnitude;

  if( !target )
    target = BotRandomEnemyBuildable( self );
  if( !target || !G_BotNavNumNodes( ) )
    return qtrue;

  BG_ClassBoundingBox( cl, mins, maxs, NULL, NULL, NULL );
  BotNavQuery( self, bs, &query );
  query.size = G_BotNavSizeClass( mins, maxs );
  query.climb = BG_ClassHasAbility( cl, SCA_WALLCLIMBER );
  query.jump = query.hop = jump * jump / ( 2.0f * MAX( g_gravity.value, 1.0f ) ) + 6.0f;
  query.noise = 0.0f;

  // no bigger than we are now, we'll fit wherever we can go already
  if( cl != self->client->ps.stats[ STAT_CLASS ] &&
      query.size <= G_BotNavSizeClass( self->r.mins, self->r.maxs ) )
    return qtrue;

  n = G_BotNavFindPath( self->r.currentOrigin, target->r.currentOrigin, path, ARRAY_LEN( path ), &query );
  if( !n )
    return qfalse;

  G_BotNavNodeOrigin( path[ n - 1 ], end );
  return Distance( end, target->r.currentOrigin ) < 300.0f;
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

    if( !BotClassCanReach( self, bs, c ) )
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

    if( ent->buildableTeam == team || ent->health <= 0 ||
        !G_BotNavConnected( self->r.currentOrigin, ent->r.currentOrigin, BotCanClimb( self ) ) )
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

// sometimes come at a far off goal from the side rather than straight on
static void BotPlanFlank( gentity_t *self, botState_t *bs )
{
  vec3_t  mid, side, via;
  float   d;

  bs->viaActive = qfalse;

  d = Distance( self->r.currentOrigin, bs->goalOrigin );
  if( d < 1000.0f || BotRandom( ) > 0.2f + bs->skill * 0.05f )
    return;

  VectorAdd( self->r.currentOrigin, bs->goalOrigin, mid );
  VectorScale( mid, 0.5f, mid );
  VectorSet( side, self->r.currentOrigin[ 1 ] - bs->goalOrigin[ 1 ],
                   bs->goalOrigin[ 0 ] - self->r.currentOrigin[ 0 ], 0.0f );
  VectorNormalize( side );
  VectorMA( mid, ( ( rand( ) & 1 ) ? 1.0f : -1.0f ) * d * ( 0.25f + BotRandom( ) * 0.25f ), side, mid );

  if( G_BotNavNodeAround( mid, 400.0f, via ) )
  {
    VectorCopy( via, bs->viaPoint );
    bs->viaActive = qtrue;
  }
}

// head somewhere the team hasn't been for a while
static qboolean BotChooseRoam( gentity_t *self, botState_t *bs )
{
  vec3_t spot;

  if( !G_BotNavExplore( self->client->pers.teamSelection, self->r.currentOrigin,
                        500.0f, 3000.0f, spot ) )
    return qfalse;

  bs->goal = BG_ROAM;
  bs->goalEntity = -1;
  VectorCopy( spot, bs->goalOrigin );
  bs->nextGoalTime = level.time + 25000 + rand( ) % 15000;
  bs->viaActive = qfalse;
  return qtrue;
}

static void BotChooseGoalFor( gentity_t *self, botState_t *bs );

// hurt badly enough to go back and heal: aliens heal faster by their
// structures, humans at a medistation once their medkit's gone
static qboolean BotNeedsHeal( gentity_t *self )
{
  gclient_t *client = self->client;
  int       health = client->ps.stats[ STAT_HEALTH ];
  int       maxHealth = client->ps.stats[ STAT_MAX_HEALTH ];

  if( health >= maxHealth * 0.2f )
    return qfalse;

  return client->pers.teamSelection == TEAM_ALIENS ||
         !BG_InventoryContainsUpgrade( UP_MEDKIT, client->ps.stats );
}

static void BotChooseGoal( gentity_t *self, botState_t *bs )
{
  botGoal_t oldGoal = bs->goal;
  int       oldEntity = bs->goalEntity;
  vec3_t    oldOrigin;

  VectorCopy( bs->goalOrigin, oldOrigin );
  BotChooseGoalFor( self, bs );

  if( bs->goal == BG_HEAL && oldGoal != BG_HEAL )
    bs->statRetreats++;

  // somewhere new, so pick a new way there
  if( bs->goal != oldGoal || bs->goalEntity != oldEntity ||
      ( bs->goalEntity < 0 && DistanceSquared( bs->goalOrigin, oldOrigin ) > Square( 300.0f ) ) )
  {
    bs->routeSeed = rand( );
    bs->nextPathTime = 0;

    if( bs->goal == BG_ATTACK )
      BotPlanFlank( self, bs );
    else
      bs->viaActive = qfalse;
  }
}

// an enemy player in our base, near the core or a spawn, closest to the core
static gentity_t *BotBaseThreat( team_t team )
{
  buildable_t core = ( team == TEAM_HUMANS ) ? BA_H_REACTOR : BA_A_OVERMIND;
  buildable_t spawn = ( team == TEAM_HUMANS ) ? BA_H_SPAWN : BA_A_SPAWN;
  gentity_t   *best = NULL;
  float       d, bestDist = 0.0f;
  int         i, j;

  for( i = 0; i < botNumPlayers; i++ )
  {
    gentity_t *enemy = botPlayers[ i ];

    if( enemy->client->pers.teamSelection == team ||
        enemy->client->pers.teamSelection == TEAM_NONE )
      continue;

    for( j = 0; j < botNumBuildables; j++ )
    {
      gentity_t *ent = botBuildables[ j ];

      if( ent->buildableTeam != team ||
          ( ent->s.modelindex != core && ent->s.modelindex != spawn ) )
        continue;

      d = Distance( enemy->r.currentOrigin, ent->r.currentOrigin );
      if( d > 700.0f )
        continue;

      if( ent->s.modelindex == core )
        d *= 0.5f;
      if( !best || d < bestDist )
      {
        best = enemy;
        bestDist = d;
      }
    }
  }

  return best;
}

static void BotChooseGoalFor( gentity_t *self, botState_t *bs )
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

  if( team == TEAM_ALIENS && BotNeedsHeal( self ) )
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

  if( team == TEAM_HUMANS && BotNeedsHeal( self ) )
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

  // enemies in our base: anybody not too far away goes back to defend it
  {
    gentity_t *threat = BotBaseThreat( team );

    if( threat && Distance( threat->r.currentOrigin, self->r.currentOrigin ) < 2500.0f )
    {
      bs->goal = BG_DEFEND;
      bs->goalEntity = threat->s.number;
      VectorCopy( threat->r.currentOrigin, bs->goalOrigin );
      bs->nextGoalTime = level.time + 2000;
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

  // keep exploring until we get there
  if( bs->goal == BG_ROAM && level.time < bs->nextGoalTime &&
      Distance( self->r.currentOrigin, bs->goalOrigin ) > 150.0f )
    return;

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

  // now and then look around the map rather than heading straight for the
  // enemy, the small aliens more than the rest
  if( BotRandom( ) < ( BotIsWeakAlien( self ) ? 0.35f : 0.15f ) && BotChooseRoam( self, bs ) )
    return;

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

      if( other->client->pers.teamSelection == team || BotGoalBlocked( bs, other ) ||
          !G_BotNavConnected( self->r.currentOrigin, other->r.currentOrigin, BotCanClimb( self ) ) )
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

// returns the direction to go in, and in moveFlags how (BOT_MOVE_*). Returns
// qfalse when we've got as close as the graph goes and can't see a way on
static qboolean BotPathDirection( gentity_t *self, botState_t *bs,
                                  const vec3_t goalIn, vec3_t dir, int *moveFlags )
{
  vec3_t      origin, node, delta, goal;
  float       horizontal;
  navQuery_t  query;
  int         flags, i;

  VectorCopy( self->r.currentOrigin, origin );
  VectorCopy( goalIn, goal );
  *moveFlags = 0;

  // going round the side first
  if( bs->viaActive && VectorCompare( goalIn, bs->goalOrigin ) )
  {
    if( Distance( origin, bs->viaPoint ) < 250.0f )
    {
      bs->viaActive = qfalse;
      bs->nextPathTime = 0;
    }
    else
      VectorCopy( bs->viaPoint, goal );
  }

  BotNavQuery( self, bs, &query );

  if( DistanceSquared( goal, bs->pathGoal ) > Square( 128.0f ) )
  {
    bs->pathRetried = qfalse;
    bs->nextPathTime = 0;
  }

  if( level.time >= bs->nextPathTime || bs->pathVersion != G_BotNavVersion( ) )
  {
    bs->pathVersion = G_BotNavVersion( );
    bs->pathLength = G_BotNavFindPath( origin, goal, bs->path, BOT_MAX_PATH, &query );
    bs->pathIndex = 0;
    bs->pathShort = qfalse;
    VectorCopy( goal, bs->pathGoal );
    bs->nextPathTime = level.time + 2000 + rand( ) % 1000;

    if( bs->pathLength > 0 )
    {
      G_BotNavNodeOrigin( bs->path[ bs->pathLength - 1 ], node );
      bs->pathShort = Distance( node, goal ) > 200.0f;
    }
    if( !bs->pathShort )
      bs->pathRetried = qfalse;

    // nowhere on the graph near us, head for somewhere that is
    bs->offGraph = !bs->pathLength && !BotCanWalkTo( self, goal ) &&
                   G_BotNavNodeAround( origin, 400.0f, bs->offGraphPoint );

    // the nearest node may be behind us, start from the furthest of the
    // first few we can walk straight to
    for( i = 1; i < bs->pathLength && i <= 3; i++ )
    {
      if( G_BotNavLinkFlags( bs->path[ i - 1 ], bs->path[ i ], &query ) &
          ( NAVLINK_CLIMB | NAVLINK_HOP | NAVLINK_PORTAL | NAVLINK_JUMP | NAVLINK_CROUCH ) )
        break;
    }
    for( i--; i > 0; i-- )
    {
      G_BotNavNodeOrigin( bs->path[ i ], node );
      if( fabs( node[ 2 ] - origin[ 2 ] ) < 18.0f && BotCanWalkTo( self, node ) )
      {
        bs->pathIndex = i;
        break;
      }
    }
  }

  // skip nodes we've reached
  while( bs->pathIndex < bs->pathLength )
  {
    G_BotNavNodeOrigin( bs->path[ bs->pathIndex ], node );
    VectorSubtract( node, origin, delta );
    horizontal = sqrt( delta[ 0 ] * delta[ 0 ] + delta[ 1 ] * delta[ 1 ] );

    // up on the ceiling, being over it will do. Beside a ramp or stairs, a
    // node well above us isn't reached just because we're under it
    if( horizontal > BOT_NODE_REACHED ||
        ( fabs( delta[ 2 ] ) > 56.0f && !( BotOnWall( self ) && delta[ 2 ] < 0.0f ) ) ||
        ( delta[ 2 ] > 24.0f && !BotOnWall( self ) ) )
      break;

    bs->pathIndex++;
  }

  // cut corners when the next node is in plain reach
  if( bs->pathIndex + 1 < bs->pathLength && level.time >= bs->nextShortcutTime )
  {
    bs->nextShortcutTime = level.time + 300;
    G_BotNavNodeOrigin( bs->path[ bs->pathIndex + 1 ], node );

    if( fabs( node[ 2 ] - origin[ 2 ] ) < 18.0f &&
        !( G_BotNavLinkFlags( bs->path[ bs->pathIndex ], bs->path[ bs->pathIndex + 1 ], &query ) &
           ( NAVLINK_JUMP | NAVLINK_CLIMB | NAVLINK_HOP | NAVLINK_CROUCH | NAVLINK_PORTAL ) ) &&
        BotCanWalkTo( self, node ) )
      bs->pathIndex++;
  }

  if( bs->offGraph )
  {
    VectorSubtract( bs->offGraphPoint, origin, dir );
    if( VectorLength( dir ) < BOT_NODE_REACHED )
      bs->nextPathTime = 0;
    return qtrue;
  }

  if( bs->pathIndex >= bs->pathLength )
  {
    VectorSubtract( goal, origin, dir );

    // the route ran out, if we can't see a way to the goal from here it's
    // not worth pushing against a wall for
    if( bs->pathShort && !BotCanWalkTo( self, goal ) )
    {
      // a detour that doesn't lead anywhere, go straight for the goal
      if( bs->viaActive )
      {
        bs->viaActive = qfalse;
        bs->nextPathTime = 0;
        return qtrue;
      }

      // the search can give up early on a big map, so look again from here
      if( bs->pathLength > 1 && !bs->pathRetried )
      {
        bs->pathRetried = qtrue;
        bs->nextPathTime = 0;
        return qtrue;
      }
      return qfalse;
    }

    return bs->pathLength > 0 || BotCanWalkTo( self, goal );
  }

  G_BotNavNodeOrigin( bs->path[ bs->pathIndex ], node );
  VectorSubtract( node, origin, dir );

  // dropping down to the next node: if there's a wall in the way of going
  // straight there, we've cut the corner, so go back to where the link
  // leaves the ledge from
  if( dir[ 2 ] < -40.0f && bs->pathIndex > 0 &&
      !( G_BotNavLinkFlags( bs->path[ bs->pathIndex - 1 ], bs->path[ bs->pathIndex ], NULL ) &
         NAVLINK_JUMP ) )
  {
    static const vec3_t humanMins = { -15.0f, -15.0f, -24.0f };
    static const vec3_t humanMaxs = { 15.0f, 15.0f, 16.0f };
    trace_t tr;
    vec3_t  prev, flat, start;

    // the link was found with a crouching human, bigger players get pushed
    // off walls into the gap anyway
    // and like it, a step up so low rails don't count
    G_BotNavNodeOrigin( bs->path[ bs->pathIndex - 1 ], prev );
    VectorCopy( origin, start );
    start[ 2 ] += NAV_STEP_HEIGHT;
    VectorSet( flat, node[ 0 ], node[ 1 ], start[ 2 ] );
    trap_Trace( &tr, start, humanMins, humanMaxs, flat, self->s.number, MASK_PLAYERSOLID );

    if( tr.fraction < 1.0f && tr.entityNum == ENTITYNUM_WORLD &&
        Square( prev[ 0 ] - origin[ 0 ] ) + Square( prev[ 1 ] - origin[ 1 ] ) > Square( 16.0f ) &&
        fabs( prev[ 2 ] - origin[ 2 ] ) < 40.0f )
    {
      VectorSubtract( prev, origin, dir );
      return qtrue;
    }
  }

  if( dir[ 2 ] < -18.0f )
    bs->pathDropTime = level.time;

  // how to get along the link we're on, and the next one if it's close
  flags = 0;
  if( bs->pathIndex > 0 )
    flags = G_BotNavLinkFlags( bs->path[ bs->pathIndex - 1 ], bs->path[ bs->pathIndex ], &query );
  if( bs->pathIndex + 1 < bs->pathLength &&
      dir[ 0 ] * dir[ 0 ] + dir[ 1 ] * dir[ 1 ] < Square( 40.0f ) )
    flags |= G_BotNavLinkFlags( bs->path[ bs->pathIndex ], bs->path[ bs->pathIndex + 1 ], &query ) &
             ( NAVLINK_CROUCH | NAVLINK_CLIMB );

  if( ( flags & NAVLINK_CROUCH ) && self->client->pers.teamSelection == TEAM_HUMANS )
    *moveFlags |= BOT_MOVE_CROUCH;

  // a vent or crawlspace, or a gap only the small fit through
  if( bs->pathIndex > 0 &&
      ( ( flags & NAVLINK_CROUCH ) || ( ( flags & NAVLINK_SIZE_MASK ) >> NAVLINK_SIZE_SHIFT ) <= 2 ) )
    bs->tunnelTime = level.time;

  if( ( flags & NAVLINK_CLIMB ) && query.climb )
  {
    *moveFlags |= BOT_MOVE_CLIMB;
    return qtrue;
  }

  // jump links get a jump, and so does climbing more than a step up to the
  // next node in case we've cut a corner and aren't on the marked link.
  // Jumping onto our own structures takes a run up, so jump a little earlier
  if( !( *moveFlags & BOT_MOVE_CROUCH ) &&
      ( ( dir[ 0 ] * dir[ 0 ] + dir[ 1 ] * dir[ 1 ] < Square( 72.0f ) &&
          ( dir[ 2 ] > 18.0f || ( flags & NAVLINK_JUMP ) ) ) ||
        ( ( flags & NAVLINK_HOP ) && BotShouldJumpObstacle( self, dir ) ) ) )
  {
    *moveFlags |= BOT_MOVE_JUMP;
    bs->pathJumpTime = level.time;
    if( flags & NAVLINK_HOP )
      bs->pathHopTime = level.time;
  }

  return qtrue;
}

// what's in the way going this way, ENTITYNUM_NONE if nothing
static int BotBlockerAhead( gentity_t *self, const vec3_t dir, float dist, trace_t *tr )
{
  vec3_t start, end, flat;

  VectorSet( flat, dir[ 0 ], dir[ 1 ], 0.0f );
  if( VectorNormalize( flat ) < 0.01f )
    return ENTITYNUM_NONE;

  // a step up, so the floor and low curbs don't count
  VectorCopy( self->r.currentOrigin, start );
  start[ 2 ] += 4.0f;
  VectorMA( start, dist, flat, end );
  trap_Trace( tr, start, self->r.mins, self->r.maxs, end, self->s.number, MASK_PLAYERSOLID );

  if( tr->startsolid || tr->fraction >= 1.0f )
    return ENTITYNUM_NONE;

  return tr->entityNum;
}

static const char *BotBlockerName( int entityNum )
{
  gentity_t *ent;

  if( entityNum == ENTITYNUM_NONE )
    return "nothing";
  if( entityNum == ENTITYNUM_WORLD )
    return "world";

  ent = &g_entities[ entityNum ];
  if( ent->client )
    return "player";
  if( ent->s.eType == ET_BUILDABLE )
    return BG_Buildable( ent->s.modelindex )->name;
  return ent->classname ? ent->classname : "?";
}

// for bot_debug: where bots get stuck and what on, to find map problems
static void BotReportStuck( gentity_t *self, botState_t *bs, int blocker, const trace_t *tr )
{
  gentity_t *near = NULL;
  float     d, nearDist = 0.0f, endDist = -1.0f;
  vec3_t    end, node;
  int       i, link = 0;

  for( i = 0; i < botNumBuildables; i++ )
  {
    d = Distance( botBuildables[ i ]->r.currentOrigin, self->r.currentOrigin );
    if( !near || d < nearDist )
    {
      near = botBuildables[ i ];
      nearDist = d;
    }
  }

  if( bs->pathLength > 0 )
  {
    G_BotNavNodeOrigin( bs->path[ bs->pathLength - 1 ], end );
    endDist = Distance( end, bs->pathGoal );
  }

  VectorClear( node );
  if( bs->pathIndex < bs->pathLength )
  {
    G_BotNavNodeOrigin( bs->path[ bs->pathIndex ], node );
    if( bs->pathIndex > 0 )
      link = G_BotNavLinkFlags( bs->path[ bs->pathIndex - 1 ], bs->path[ bs->pathIndex ], NULL );
  }

  G_Printf( "botstuck: %s %s at (%.0f %.0f %.0f) goal %d path %d/%d near %s %.0f "
            "hit %s normal %.1f %.1f %.1f to (%.0f %.0f %.0f) end %.0f away%s "
            "move %d %d %d node (%.0f %.0f %.0f) link %d vel %.0f ground %d\n",
            BG_TeamName( self->client->pers.teamSelection ),
            BG_Class( self->client->ps.stats[ STAT_CLASS ] )->name,
            self->r.currentOrigin[ 0 ], self->r.currentOrigin[ 1 ], self->r.currentOrigin[ 2 ],
            bs->goal, bs->pathIndex, bs->pathLength,
            near ? BG_Buildable( near->s.modelindex )->name : "none", nearDist,
            BotBlockerName( blocker ),
            tr->plane.normal[ 0 ], tr->plane.normal[ 1 ], tr->plane.normal[ 2 ],
            bs->pathGoal[ 0 ], bs->pathGoal[ 1 ], bs->pathGoal[ 2 ], endDist,
            BotOnWall( self ) ? " (on wall)" : "",
            bs->lastMove[ 0 ], bs->lastMove[ 1 ], bs->lastMove[ 2 ],
            node[ 0 ], node[ 1 ], node[ 2 ], link, VectorLength( self->client->ps.velocity ),
            bs->groundFail );
}

// is there room to go this way crouched, but not standing?
static qboolean BotCrouchFits( gentity_t *self, const vec3_t dir )
{
  trace_t tr;
  vec3_t  mins, maxs, cmaxs, start, end, flat;

  BG_ClassBoundingBox( self->client->ps.stats[ STAT_CLASS ], mins, maxs, cmaxs, NULL, NULL );
  if( cmaxs[ 2 ] >= maxs[ 2 ] )
    return qfalse;

  VectorSet( flat, dir[ 0 ], dir[ 1 ], 0.0f );
  if( VectorNormalize( flat ) < 0.01f )
    return qfalse;

  VectorCopy( self->r.currentOrigin, start );
  VectorMA( start, 32.0f, flat, end );

  trap_Trace( &tr, start, mins, maxs, end, self->s.number, MASK_PLAYERSOLID );
  if( tr.fraction >= 1.0f )
    return qfalse;

  maxs[ 2 ] = cmaxs[ 2 ];
  trap_Trace( &tr, start, mins, maxs, end, self->s.number, MASK_PLAYERSOLID );
  return !tr.startsolid && tr.fraction >= 1.0f;
}

// decide how to get going again from what's in the way
static void BotUnstick( gentity_t *self, botState_t *bs, int blocker )
{
  gentity_t *ent = ( blocker != ENTITYNUM_NONE && blocker != ENTITYNUM_WORLD ) ?
                   &g_entities[ blocker ] : NULL;
  team_t    team = self->client->pers.teamSelection;

  bs->unstuckUntil = level.time + 600 + rand( ) % 600;
  bs->strafeDir = ( rand( ) & 1 ) ? 1.0f : -1.0f;

  // an enemy structure in the way gets destroyed
  if( ent && ent->s.eType == ET_BUILDABLE && ent->buildableTeam != team && ent->health > 0 &&
      !BotIsBuilderWeapon( self->client->ps.weapon ) )
  {
    bs->enemy = ent->s.number;
    bs->enemyVisible = qtrue;
    bs->enemyLastSeen = level.time;
    bs->enemySightTime = level.time - 1000;
    BotEntityCenter( ent, bs->enemyLastPos );
    return;
  }

  // climbers go over whatever it is, or down off the wall if that's what's
  // stuck
  if( BotCanClimb( self ) )
  {
    if( BotOnWall( self ) && bs->stuckCount > 1 )
    {
      bs->wallUntil = 0;
      bs->noWallUntil = level.time + 2000;
      bs->noCeilingUntil = level.time + 10000;
    }
    else if( level.time >= bs->noWallUntil )
      bs->wallUntil = level.time + 2500;
    return;
  }

  // humans duck under anything low in the way, and every other time try
  // ducking anyway, some overhangs are too awkward to tell
  if( team == TEAM_HUMANS && !ent &&
      ( BotCrouchFits( self, bs->lastMoveDir ) || !( bs->stuckCount & 1 ) ) )
    bs->crouchUntil = level.time + 1500;
}

/*
===============
BotCheckProgress

Moving all the time but staying in the same small area, while where we're
going is further away, means the route is leading us round in circles: a
bot pacing an alcove, or a climber stuck on a wall. Steer every bot away
from the link we're on and find another way
===============
*/
static void BotCheckProgress( gentity_t *self, botState_t *bs, qboolean moving )
{
  qboolean fighting = bs->enemy >= 0 && bs->enemyVisible;

  if( !moving || fighting || level.time < bs->wanderUntil ||
      Distance( self->r.currentOrigin, bs->pathGoal ) < 300.0f ||
      Distance( self->r.currentOrigin, bs->progressOrigin ) > 200.0f )
  {
    VectorCopy( self->r.currentOrigin, bs->progressOrigin );
    bs->progressTime = level.time;
    return;
  }

  if( level.time - bs->progressTime < 6000 )
    return;

  bs->statCircling++;
  if( bot_debug.integer )
  {
    vec3_t node;
    int    link = 0;

    VectorClear( node );
    if( bs->pathIndex < bs->pathLength )
    {
      G_BotNavNodeOrigin( bs->path[ bs->pathIndex ], node );
      if( bs->pathIndex > 0 )
        link = G_BotNavLinkFlags( bs->path[ bs->pathIndex - 1 ], bs->path[ bs->pathIndex ], NULL );
    }

    G_Printf( "botcircling: %s %s at (%.0f %.0f %.0f) goal %d path %d/%d node (%.0f %.0f %.0f) "
              "link %d to (%.0f %.0f %.0f)%s%s\n",
              BG_TeamName( self->client->pers.teamSelection ),
              BG_Class( self->client->ps.stats[ STAT_CLASS ] )->name,
              self->r.currentOrigin[ 0 ], self->r.currentOrigin[ 1 ], self->r.currentOrigin[ 2 ],
              bs->goal, bs->pathIndex, bs->pathLength, node[ 0 ], node[ 1 ], node[ 2 ], link,
              bs->pathGoal[ 0 ], bs->pathGoal[ 1 ], bs->pathGoal[ 2 ],
              BotOnWall( self ) ? " (on wall)" : "",
              level.time < bs->wallUntil ? " (wall mode)" : "" );
  }

  // whatever we were trying here isn't working
  if( bs->pathIndex > 0 && bs->pathIndex < bs->pathLength )
  {
    G_BotNavLinkFailed( bs->path[ bs->pathIndex - 1 ], bs->path[ bs->pathIndex ] );
    G_BotNavLinkFailed( bs->path[ bs->pathIndex - 1 ], bs->path[ bs->pathIndex ] );
  }
  bs->routeSeed = rand( );
  bs->nextPathTime = 0;
  bs->viaActive = qfalse;
  bs->avoidUntil = 0;

  // climbers get off the wall and walk for a while
  if( BotCanClimb( self ) )
  {
    bs->wallUntil = 0;
    bs->noWallUntil = level.time + 3000;
    bs->noCeilingUntil = level.time + 15000;
  }

  // the same again: give up on the goal
  if( ++bs->routeFailures >= 2 )
  {
    if( bs->goalEntity >= 0 )
    {
      bs->blockedEntity = bs->goalEntity;
      bs->blockedUntil = level.time + 30000;
    }
    bs->goal = BG_NONE;
    bs->goalEntity = -1;
    bs->routeFailures = 0;
  }

  // and count towards respawning if nothing gets us out
  if( Distance( self->r.currentOrigin, bs->trapOrigin ) > 200.0f )
  {
    VectorCopy( self->r.currentOrigin, bs->trapOrigin );
    bs->trapStart = level.time;
    bs->trapCount = 0;
  }
  bs->trapCount += 3;

  VectorCopy( self->r.currentOrigin, bs->progressOrigin );
  bs->progressTime = level.time;

  if( bs->trapCount >= 20 && level.time - bs->trapStart > 40000 )
  {
    if( bot_debug.integer )
      G_Printf( "bottrapped: %s at (%.0f %.0f %.0f), respawning\n", self->client->pers.netname,
                self->r.currentOrigin[ 0 ], self->r.currentOrigin[ 1 ], self->r.currentOrigin[ 2 ] );
    bs->trapCount = 0;
    BotCommand( self, "kill" );
  }
}

static void BotCheckStuck( gentity_t *self, botState_t *bs, qboolean moving )
{
  trace_t tr;
  int     blocker;

  if( level.time < bs->nextStuckTime )
    return;

  bs->nextStuckTime = level.time + 1000;

  // pressing up against an enemy we're fighting isn't being stuck, nor is
  // a human holding its range and dodging about
  if( moving && bs->enemy >= 0 && bs->enemyVisible &&
      ( self->client->pers.teamSelection == TEAM_HUMANS ||
        Distance( g_entities[ bs->enemy ].r.currentOrigin, self->r.currentOrigin ) < 150.0f ) )
    moving = qfalse;

  BotCheckProgress( self, bs, moving );

  if( moving && Distance( self->r.currentOrigin, bs->lastPos ) < 32.0f )
  {
    bs->stuckCount++;
    bs->statStuck++;

    if( Distance( self->r.currentOrigin, bs->trapOrigin ) > 150.0f )
    {
      VectorCopy( self->r.currentOrigin, bs->trapOrigin );
      bs->trapStart = level.time;
      bs->trapCount = 0;
    }
    bs->trapCount++;

    memset( &tr, 0, sizeof( tr ) );
    blocker = BotBlockerAhead( self, bs->lastMoveDir, 24.0f, &tr );

    if( bot_debug.integer )
      BotReportStuck( self, bs, blocker, &tr );

    BotUnstick( self, bs, blocker );

    // nothing's working here: head off somewhere at random for a bit,
    // forgetting where we were going, and as a last resort respawn
    if( bs->trapCount >= 6 && level.time >= bs->wanderUntil )
    {
      float yaw = BotRandom( ) * 2.0f * M_PI;

      VectorSet( bs->wanderDir, cos( yaw ), sin( yaw ), 0.0f );
      bs->wanderUntil = level.time + 1500 + rand( ) % 1500;
      bs->goal = BG_NONE;
      bs->goalEntity = -1;
      bs->viaActive = qfalse;
      bs->nextPathTime = 0;
      if( BotCanClimb( self ) && level.time >= bs->noWallUntil )
        bs->wallUntil = bs->wanderUntil;
    }

    if( bs->trapCount >= 20 && level.time - bs->trapStart > 40000 )
    {
      if( bot_debug.integer )
        G_Printf( "bottrapped: %s at (%.0f %.0f %.0f), respawning\n", self->client->pers.netname,
                  self->r.currentOrigin[ 0 ], self->r.currentOrigin[ 1 ], self->r.currentOrigin[ 2 ] );
      bs->trapCount = 0;
      BotCommand( self, "kill" );
      return;
    }

    if( bs->stuckCount >= 3 )
    {
      // give up on this route, and steer everyone away from the same link
      if( bs->pathIndex > 0 && bs->pathIndex < bs->pathLength )
        G_BotNavLinkFailed( bs->path[ bs->pathIndex - 1 ], bs->path[ bs->pathIndex ] );
      if( bs->pathIndex < bs->pathLength )
        bs->pathIndex++;
      bs->nextPathTime = 0;
      bs->stuckCount = 0;
      bs->viaActive = qfalse;

      // and on the goal itself if no route works
      if( ++bs->routeFailures >= 3 && bs->goalEntity >= 0 )
      {
        bs->blockedEntity = bs->goalEntity;
        bs->blockedUntil = level.time + 30000;
        bs->goal = BG_NONE;
        bs->goalEntity = -1;
        bs->routeFailures = 0;
      }
      else if( bs->routeFailures >= 3 && bs->goal == BG_ROAM )
      {
        bs->goal = BG_NONE;
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

    if( G_BotNavInHazard( tr.endpos ) )
    {
      botStates[ self - g_entities ].groundFail = 2;
      return qfalse;
    }

    // a drop the route takes us down has been checked already, just short
    // of the edge can be deeper than where we'll land. But not bottomless:
    // floors can go from under a route
    if( botStates[ self - g_entities ].pathDropTime == level.time )
    {
      if( tr.fraction >= 1.0f )
      {
        trace_t deep;
        vec3_t  bottom;

        VectorCopy( start, bottom );
        bottom[ 2 ] -= 600.0f;
        trap_Trace( &deep, start, probeMins, probeMaxs, bottom, self->s.number, MASK_DEADSOLID );
        if( deep.fraction >= 1.0f || G_BotNavInHazard( deep.endpos ) )
        {
          botStates[ self - g_entities ].groundFail = 1;
          return qfalse;
        }
      }
      continue;
    }

    if( tr.fraction >= 1.0f )
    {
      botStates[ self - g_entities ].groundFail = 1;
      return qfalse;
    }

    // too steep to stand on means sliding down whatever is below, though
    // a slope going up from here is no danger. One falling away from us
    // faces the way we're going
    if( tr.plane.normal[ 2 ] < 0.7f &&
        ( tr.endpos[ 2 ] < self->r.currentOrigin[ 2 ] + self->r.mins[ 2 ] - NAV_STEP_HEIGHT ||
          tr.plane.normal[ 0 ] * dir[ 0 ] + tr.plane.normal[ 1 ] * dir[ 1 ] > 0.0f ) )
    {
      botStates[ self - g_entities ].groundFail = 3;
      return qfalse;
    }
  }

  return qtrue;
}

// a jump carries us further than a step, so check further ahead
static qboolean BotSafeToJump( gentity_t *self, usercmd_t *cmd, const vec3_t viewAngles )
{
  vec3_t  forward, right, angles, dir, start, end;
  trace_t tr;
  int     i;

  if( !cmd->forwardmove && !cmd->rightmove )
    return qtrue;

  VectorSet( angles, 0.0f, viewAngles[ YAW ], 0.0f );
  AngleVectors( angles, forward, right, NULL );

  // jumps the route asks for have been checked already, as long as we're
  // on the route: off to the side of it there may be nothing to land on
  if( botStates[ self - g_entities ].pathJumpTime == level.time )
  {
    VectorScale( forward, cmd->forwardmove, dir );
    VectorMA( dir, cmd->rightmove, right, dir );
    dir[ 2 ] = 0.0f;
    VectorNormalize( dir );

    for( i = 1; i <= 2; i++ )
    {
      VectorMA( self->r.currentOrigin, 60.0f * i, dir, start );
      VectorCopy( start, end );
      end[ 2 ] -= 600.0f;
      trap_Trace( &tr, start, NULL, NULL, end, self->s.number, MASK_DEADSOLID );
      if( !tr.startsolid && ( tr.fraction >= 1.0f || G_BotNavInHazard( tr.endpos ) ) )
        return qfalse;
    }
    return qtrue;
  }

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
  VectorCopy( flat, botStates[ self - g_entities ].lastMoveDir );

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
// turnScale slows the turn below what the bot's skill allows
static void BotTurn( botState_t *bs, vec3_t view, const vec3_t desired, float msec,
                     float turnScale )
{
  float maxTurn = ( 200.0f + bs->skill * 80.0f ) * turnScale * msec / 1000.0f;
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
Moving
===============
*/
static float BotEnemyDistance( team_t team, const vec3_t point );

static qboolean BotWantsWall( gentity_t *self, botState_t *bs, int moveFlags )
{
  return BotCanClimb( self ) && level.time >= bs->noWallUntil &&
         ( ( moveFlags & BOT_MOVE_CLIMB ) || level.time < bs->wallUntil );
}

// face the way we're going: level when walking, straight at it on walls
static void BotFaceMove( gentity_t *self, botState_t *bs, const vec3_t moveDir, int moveFlags,
                         vec3_t desired )
{
  vec3_t  normal, along;
  float   length;

  if( !BotWantsWall( self, bs, moveFlags ) )
  {
    vectoangles( moveDir, desired );
    desired[ PITCH ] = 0.0f;
    return;
  }

  // walking on a wall we go the way we face along it, so somewhere straight
  // out from the wall gets us nowhere. Go up it to the ceiling and across,
  // or if it's below us let go
  BG_GetClientNormal( &self->client->ps, normal );
  length = VectorLength( moveDir );
  VectorMA( moveDir, -DotProduct( moveDir, normal ), normal, along );

  if( BotOnWall( self ) && fabs( normal[ 2 ] ) < 0.7f && length > 1.0f &&
      VectorLength( along ) < length * 0.4f )
  {
    if( moveDir[ 2 ] < -32.0f )
    {
      bs->wallUntil = 0;
      bs->noWallUntil = level.time + 1500;
      vectoangles( moveDir, desired );
      desired[ PITCH ] = 0.0f;
      return;
    }

    VectorSet( along, 0.0f, 0.0f, 1.0f );
    vectoangles( along, desired );
    return;
  }

  vectoangles( moveDir, desired );
}

// rotate a direction about the vertical
static void BotRotateYaw( const vec3_t in, float degrees, vec3_t out )
{
  float s = sin( DEG2RAD( degrees ) ), c = cos( DEG2RAD( degrees ) );

  VectorSet( out, in[ 0 ] * c - in[ 1 ] * s, in[ 0 ] * s + in[ 1 ] * c, 0.0f );
}

// steer round players and our own structures rather than walk into them
static void BotAvoid( gentity_t *self, botState_t *bs, vec3_t moveDir, qboolean jumping )
{
  static const float  turns[ ] = { 45.0f, 90.0f, 135.0f };
  trace_t             tr;
  gentity_t           *ent;
  vec3_t              flat, side;
  int                 blocker, i, j;

  if( level.time < bs->avoidUntil )
  {
    VectorCopy( bs->avoidDir, moveDir );
    return;
  }

  if( jumping )
    return;

  blocker = BotBlockerAhead( self, moveDir, 40.0f, &tr );
  if( blocker == ENTITYNUM_NONE || blocker == ENTITYNUM_WORLD )
    return;

  ent = &g_entities[ blocker ];

  // enemy structures are in the way to be destroyed
  if( ent->s.eType == ET_BUILDABLE && ent->buildableTeam != self->client->pers.teamSelection )
  {
    if( !BotIsBuilderWeapon( self->client->ps.weapon ) && ent->health > 0 &&
        ( bs->enemy < 0 || !bs->enemyVisible ) )
      BotUnstick( self, bs, blocker );
    return;
  }

  VectorSet( flat, moveDir[ 0 ], moveDir[ 1 ], 0.0f );
  VectorNormalize( flat );

  // the first way round that's clear, trying our favoured side first
  for( i = 0; i < ARRAY_LEN( turns ); i++ )
  {
    for( j = 0; j < 2; j++ )
    {
      BotRotateYaw( flat, turns[ i ] * bs->strafeDir * ( j ? -1.0f : 1.0f ), side );
      if( BotBlockerAhead( self, side, 48.0f, &tr ) != ENTITYNUM_NONE )
        continue;

      VectorCopy( side, bs->avoidDir );
      VectorCopy( side, moveDir );
      bs->avoidUntil = level.time + 300 + i * 150;
      return;
    }
  }
}

// the closest wall that's worth climbing, qfalse if there's none near
static qboolean BotFindWall( gentity_t *self, const vec3_t towards, vec3_t out )
{
  trace_t tr;
  vec3_t  dir, end, flat;
  float   best = 0.0f, score;
  int     i;
  qboolean found = qfalse;

  VectorSet( flat, towards[ 0 ], towards[ 1 ], 0.0f );
  VectorNormalize( flat );

  for( i = 0; i < 8; i++ )
  {
    VectorSet( dir, cos( DEG2RAD( i * 45.0f ) ), sin( DEG2RAD( i * 45.0f ) ), 0.0f );
    VectorMA( self->r.currentOrigin, 256.0f, dir, end );
    trap_Trace( &tr, self->r.currentOrigin, self->r.mins, self->r.maxs, end, self->s.number,
                MASK_PLAYERSOLID );

    if( tr.startsolid || tr.fraction >= 1.0f || tr.entityNum != ENTITYNUM_WORLD ||
        fabs( tr.plane.normal[ 2 ] ) > 0.3f || ( tr.surfaceFlags & ( SURF_SKY | SURF_SLICK ) ) )
      continue;

    // close walls, and ones the way we're going
    score = tr.fraction * 256.0f - DotProduct( dir, flat ) * 100.0f;
    if( !found || score < best )
    {
      best = score;
      VectorCopy( tr.endpos, out );
      found = qtrue;
    }
  }

  return found;
}

// is there something to land on below, that isn't a hazard?
static qboolean BotSafeToDrop( gentity_t *self )
{
  trace_t tr;
  vec3_t  end;

  VectorCopy( self->r.currentOrigin, end );
  end[ 2 ] -= 800.0f;
  trap_Trace( &tr, self->r.currentOrigin, self->r.mins, self->r.maxs, end, self->s.number,
              MASK_DEADSOLID );

  return tr.fraction < 1.0f && !G_BotNavInHazard( tr.endpos );
}

/*
===============
BotCeilingCrawl

Dretches and basilisks sneak up on humans along the ceiling, where they
aren't looking. Instead of the next point on the route, head for the
ceiling above it, by way of a wall when there's one to climb
===============
*/
static void BotCeilingCrawl( gentity_t *self, botState_t *bs, vec3_t moveDir, int *moveFlags )
{
  vec3_t  target, delta;
  float   ceiling;

  if( !BotCanClimb( self ) || !BotIsWeakAlien( self ) ||
      BotIsBuilderWeapon( self->client->ps.weapon ) ||
      level.time < bs->noCeilingUntil || level.time < bs->noWallUntil ||
      ( *moveFlags & ( BOT_MOVE_CLIMB | BOT_MOVE_JUMP ) ) )
    return;

  // only worth it near the humans
  if( BotEnemyDistance( TEAM_ALIENS, self->r.currentOrigin ) > 1000.0f )
    return;

  // somewhere to hang from, here and where we're going
  VectorAdd( self->r.currentOrigin, moveDir, target );
  ceiling = G_BotNavCeiling( target, 280.0f );
  if( ceiling < 80.0f || !G_BotNavCeiling( self->r.currentOrigin, 280.0f ) )
    return;

  target[ 2 ] += ceiling - 24.0f;
  bs->wallUntil = level.time + 500;

  // on the floor, find a wall to go up first
  if( !BotOnWall( self ) )
  {
    if( level.time >= bs->nextWallSearch )
    {
      bs->nextWallSearch = level.time + 1000;
      if( !BotFindWall( self, moveDir, bs->wallSpot ) )
      {
        bs->wallUntil = 0;
        bs->noCeilingUntil = level.time + 3000;
        return;
      }
    }

    VectorSubtract( bs->wallSpot, self->r.currentOrigin, moveDir );
    moveDir[ 2 ] = 0.0f;
    return;
  }

  VectorSubtract( target, self->r.currentOrigin, delta );
  VectorCopy( delta, moveDir );
}

static void BotUpdateStats( gentity_t *self, botState_t *bs, usercmd_t *cmd, int msec )
{
  playerState_t *ps = &self->client->ps;
  qboolean      onWall = BotOnWall( self );
  int           region = G_BotNavRegion( ps->origin );

  if( ps->stats[ STAT_STATE ] & SS_SPEEDBOOST && ( cmd->buttons & BUTTON_SPRINT ) )
    bs->statSprint += msec;
  if( ps->pm_flags & PMF_DUCKED )
    bs->statCrouch += msec;
  if( ps->pm_type == PM_JETPACK )
    bs->statJet += msec;
  if( level.time - bs->tunnelTime < 200 )
    bs->statTunnel += msec;
  if( onWall )
    bs->statWall += msec;
  if( onWall && ( ps->eFlags & EF_WALLCLIMBCEILING ) )
    bs->statCeiling += msec;
  if( onWall && !bs->wasOnWall )
    bs->statClimbs++;
  bs->wasOnWall = onWall;

  // we asked for a jump last time and left the ground
  if( bs->lastMove[ 2 ] > 0 && bs->wasOnGround && ps->groundEntityNum == ENTITYNUM_NONE )
  {
    bs->statJumps++;
    if( level.time - bs->pathHopTime < 200 )
      bs->statHops++;
  }
  bs->wasOnGround = ps->groundEntityNum != ENTITYNUM_NONE;

  bs->statRegions[ region >> 3 ] |= 1 << ( region & 7 );
}

// going fast towards an edge we shouldn't go over: letting go of the keys
// isn't enough to stop in time, so push back against it
static void BotBrakeAtEdges( gentity_t *self, usercmd_t *cmd, const vec3_t view )
{
  playerState_t *ps = &self->client->ps;
  vec3_t        vel, angles, forward, right;
  float         speed, fm, rm, scale;

  if( ps->groundEntityNum == ENTITYNUM_NONE || ps->pm_type != PM_NORMAL ||
      ( ps->stats[ STAT_STATE ] & SS_WALLCLIMBING ) )
    return;

  VectorSet( vel, ps->velocity[ 0 ], ps->velocity[ 1 ], 0.0f );
  speed = VectorNormalize( vel );
  if( speed < 120.0f )
    return;

  VectorSet( angles, 0.0f, view[ YAW ], 0.0f );
  AngleVectors( angles, forward, right, NULL );
  fm = DotProduct( vel, forward );
  rm = DotProduct( vel, right );

  if( BotGroundAhead( self, forward, right, fm, rm, speed * 0.15f ) )
    return;

  scale = MAX( fabs( fm ), fabs( rm ) );
  if( scale < 0.01f )
    return;

  cmd->forwardmove = (signed char)( -127.0f * fm / scale );
  cmd->rightmove = (signed char)( -127.0f * rm / scale );
  if( cmd->upmove > 0 )
    cmd->upmove = 0;
  cmd->buttons &= ~( BUTTON_SPRINT | BUTTON_DODGE );
}

/*
===============
BotJetpack

Humans with a jetpack fly up walls the route climbs, and hover out of
reach of aliens that have to get close to hurt them. Returns whether it's
flying, with cmd->upmove set to rise, hold or sink
===============
*/
static qboolean BotJetpack( gentity_t *self, botState_t *bs, usercmd_t *cmd,
                            const vec3_t moveDir, int moveFlags )
{
  playerState_t *ps = &self->client->ps;
  qboolean      active = BG_UpgradeIsActive( UP_JETPACK, ps->stats );
  qboolean      climb = ( moveFlags & BOT_MOVE_CLIMB ) != 0;
  qboolean      want = BotCanFly( self ) && ( climb || level.time < bs->hoverUntil );
  trace_t       tr;
  vec3_t        end;

  if( want != active && level.time >= bs->nextJetCommandTime )
  {
    bs->nextJetCommandTime = level.time + 300;
    BotCommand( self, want ? "itemact jetpack" : "itemdeact jetpack" );
  }

  if( ps->pm_type != PM_JETPACK )
    return qfalse;

  if( climb )
  {
    // up to the ledge, then over it
    cmd->upmove = moveDir[ 2 ] > -8.0f ? 127 : 0;
    return qtrue;
  }

  // hover a little way off the ground, under the ceiling
  VectorCopy( self->r.currentOrigin, end );
  end[ 2 ] -= 128.0f;
  trap_Trace( &tr, self->r.currentOrigin, self->r.mins, self->r.maxs, end, self->s.number, MASK_PLAYERSOLID );
  cmd->upmove = tr.fraction < 0.75f ? 127 : 0;
  return qtrue;
}

/*
===============
BotMove

Turn where we want to go into the usercmd: walls for climbers, steering
round things in the way, crouching, jumping and sprinting
===============
*/
static void BotMove( gentity_t *self, botState_t *bs, usercmd_t *cmd, const vec3_t view,
                     const vec3_t moveDirIn, qboolean moving, int moveFlags, float strafe,
                     qboolean sprint, int msec )
{
  gclient_t *client = self->client;
  vec3_t    moveDir;
  qboolean  wall, crouch, jump;
  int       stamina = client->ps.stats[ STAT_STAMINA ];

  VectorCopy( moveDirIn, moveDir );

  if( level.time < bs->wanderUntil )
  {
    VectorCopy( bs->wanderDir, moveDir );
    moving = qtrue;
  }

  // on a wall with the route heading well down, and no wall to climb,
  // let go: the climbers come to no harm falling
  if( BotOnWall( self ) && moving && moveDir[ 2 ] < -64.0f && !( moveFlags & BOT_MOVE_CLIMB ) &&
      moveDir[ 0 ] * moveDir[ 0 ] + moveDir[ 1 ] * moveDir[ 1 ] < Square( moveDir[ 2 ] * 2.0f ) )
  {
    bs->wallUntil = 0;
    bs->noWallUntil = level.time + 1500;
  }

  // on a wall and getting no closer to where we're going, in a corner or
  // somewhere the surfaces don't lead: let go and walk for a while
  if( BotOnWall( self ) && moving )
  {
    float d = VectorLength( moveDir );

    if( !bs->wallCheckTime )
    {
      bs->wallCheckTime = level.time + 1500;
      bs->wallCheckDist = d;
    }
    else if( level.time >= bs->wallCheckTime )
    {
      if( bs->wallCheckDist - d < 32.0f )
      {
        bs->wallUntil = 0;
        bs->noWallUntil = level.time + 2000;
        bs->noCeilingUntil = level.time + 10000;
      }
      bs->wallCheckTime = level.time + 1500;
      bs->wallCheckDist = d;
    }
  }
  else
    bs->wallCheckTime = 0;

  wall = BotWantsWall( self, bs, moveFlags );

  // never let go of a wall over a pit
  if( !wall && BotOnWall( self ) && !BotSafeToDrop( self ) )
  {
    wall = qtrue;
    bs->noWallUntil = 0;
    bs->wallUntil = level.time + 500;
  }

  // walking on walls goes nowhere if what we want is straight up and
  // there's no wall in front, so find one
  if( wall && moving && !BotOnWall( self ) &&
      moveDir[ 0 ] * moveDir[ 0 ] + moveDir[ 1 ] * moveDir[ 1 ] < Square( moveDir[ 2 ] ) )
  {
    if( level.time >= bs->nextWallSearch )
    {
      bs->nextWallSearch = level.time + 1000;
      if( !BotFindWall( self, moveDir, bs->wallSpot ) )
        VectorCopy( self->r.currentOrigin, bs->wallSpot );
    }

    VectorSubtract( bs->wallSpot, self->r.currentOrigin, moveDir );
    moveDir[ 2 ] = 0.0f;
  }
  crouch = client->pers.teamSelection == TEAM_HUMANS &&
           ( ( moveFlags & BOT_MOVE_CROUCH ) || level.time < bs->crouchUntil );
  jump = ( moveFlags & BOT_MOVE_JUMP ) != 0;

  BotCheckStuck( self, bs, moving );
  bs->groundFail = 0;

  if( level.time < bs->unstuckUntil && !wall )
  {
    strafe = bs->strafeDir;
    if( bs->stuckCount > 1 && !crouch )
      jump = qtrue;
  }

  if( wall )
  {
    // holding crouch sticks us to whatever we walk into, and we go along
    // it whichever way we face
    cmd->upmove = -127;
    if( moving )
    {
      cmd->forwardmove = 127;
      VectorCopy( moveDir, bs->lastMoveDir );
    }
    jump = qfalse;
  }
  else if( moving )
  {
    BotAvoid( self, bs, moveDir, jump );
    BotSetMovement( self, cmd, view, moveDir, strafe );
    if( moveDir[ 0 ] * moveDir[ 0 ] + moveDir[ 1 ] * moveDir[ 1 ] < 1.0f )
      bs->groundFail = 4;

    if( !jump && !crouch && BotShouldJumpObstacle( self, moveDir ) )
      jump = qtrue;
  }
  else if( strafe != 0.0f )
  {
    vec3_t none = { 0.0f, 0.0f, 0.0f };

    BotSetMovement( self, cmd, view, none, strafe );
  }

  if( client->pers.teamSelection == TEAM_HUMANS &&
      BG_InventoryContainsUpgrade( UP_JETPACK, client->ps.stats ) &&
      BotJetpack( self, bs, cmd, moveDir, moveFlags ) )
    crouch = qfalse;
  else if( crouch && !wall )
    cmd->upmove = -127;
  else if( jump && client->ps.groundEntityNum != ENTITYNUM_NONE && BotSafeToJump( self, cmd, view ) )
    cmd->upmove = 127;

  // humans sprint while there's stamina to spare, then rest until it's back
  if( client->pers.teamSelection == TEAM_HUMANS )
  {
    if( bs->sprinting && stamina < STAMINA_BREATHING_LEVEL )
      bs->sprinting = qfalse;
    else if( !bs->sprinting && stamina > STAMINA_MAX * 4 / 10 )
      bs->sprinting = qtrue;

    if( sprint && bs->sprinting && moving && cmd->forwardmove > 0 && !crouch )
      cmd->buttons |= BUTTON_SPRINT;
  }

  if( !wall )
    BotBrakeAtEdges( self, cmd, view );

  BotSetCmdAngles( self, cmd, view );
  BotUpdateStats( self, bs, cmd, msec );
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

// Human bots aim worse at real players than at other bots, so bot against
// bot stays as balanced as it was. The handicap comes off the skill used for
// aiming and reacting, and can take it below 1 for an even worse aim
static float BotAimHandicap( gentity_t *self, gentity_t *enemy )
{
  if( self->client->pers.teamSelection == TEAM_HUMANS &&
      enemy && enemy->client && !( enemy->r.svFlags & SVF_BOT ) )
    return Com_Clamp( 0.0f, 10.0f, bot_humanAimHandicap.value );

  return 0.0f;
}

static float BotAimSkill( gentity_t *self, botState_t *bs, gentity_t *enemy )
{
  return bs->skill - BotAimHandicap( self, enemy );
}

static void BotAttack( gentity_t *self, botState_t *bs, usercmd_t *cmd,
                       gentity_t *enemy, float dist, float aimOff )
{
  playerState_t *ps = &self->client->ps;
  weapon_t      weapon = ps->weapon;
  float         tolerance;
  qboolean      aimed;

  int           reaction = 700 - BotAimSkill( self, bs, enemy ) * 100;

  // humans take a little longer to react to fast, small aliens than aliens
  // do to them
  if( self->client->pers.teamSelection == TEAM_HUMANS && enemy->client )
    reaction += 75;

  if( level.time - bs->enemySightTime < reaction )
    return;

  // humans shoot in bursts, better ones in longer bursts with shorter pauses
  if( self->client->pers.teamSelection == TEAM_HUMANS )
  {
    if( level.time < bs->burstPauseUntil )
      return;

    if( !bs->burstUntil )
      bs->burstUntil = level.time + 500 + bs->skill * 175 + rand( ) % 400;
    else if( level.time >= bs->burstUntil )
    {
      bs->burstUntil = 0;
      bs->burstPauseUntil = level.time + 500 - bs->skill * 60 + rand( ) % 250;
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
  vec3_t      candidate, best[ 4 ], node;
  float       score, bestScore[ 4 ];
  int         numBest = 0, i, j, n;
  int         path[ 128 ];
  navQuery_t  query;

  // somewhere a human could walk to
  memset( &query, 0, sizeof( query ) );
  query.team = team;
  query.jump = 36.0f;

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
    n = G_BotNavFindPath( origin, best[ i ], path, ARRAY_LEN( path ), &query );
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

// how far apart to keep structures of the same kind. Spawns spread out
// can't all be destroyed at once and don't block each other, defences
// cover more ground
static float BotBuildSpacing( buildable_t type )
{
  switch( type )
  {
    case BA_H_SPAWN:
    case BA_A_SPAWN:      return 250.0f;
    case BA_H_MGTURRET:
    case BA_H_TESLAGEN:
    case BA_A_ACIDTUBE:
    case BA_A_TRAPPER:
    case BA_A_HIVE:       return 120.0f;
    default:              return 0.0f;
  }
}

static qboolean BotTooCrowded( team_t team, buildable_t type, const vec3_t point )
{
  float spacing = BotBuildSpacing( type );
  int   i;

  if( spacing <= 0.0f )
    return qfalse;

  for( i = 0; i < botNumBuildables; i++ )
  {
    gentity_t *ent = botBuildables[ i ];

    if( ent->buildableTeam == team && ent->s.modelindex == type &&
        DistanceSquared( ent->r.currentOrigin, point ) < Square( spacing ) )
      return qtrue;
  }

  return qfalse;
}

static qboolean BotPlanBuild( gentity_t *self, botState_t *bs )
{
  team_t        team = self->client->pers.teamSelection;
  botTeamBase_t *base = &botBases[ team ];
  vec3_t        anchor, end;
  float         minDist, maxDist;
  buildable_t   type;
  qboolean      outpost;
  navQuery_t    query;
  int           path[ BOT_MAX_PATH ];
  int           i, n;
  vec3_t        spot;
  qboolean      found;

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

  // somewhere we can get to, away from others of the same kind if we can
  bs->buildCrowded = qfalse;
  for( i = 0, found = qfalse; i < 8; i++ )
  {
    if( !G_BotNavRandomNodeNear( anchor, minDist, maxDist, spot ) )
    {
      // no graph yet, build where we stand
      if( !found && !G_BotNavNumNodes( ) )
      {
        VectorCopy( self->r.currentOrigin, bs->buildSpot );
        found = qtrue;
      }
      break;
    }

    BotNavQuery( self, bs, &query );
    n = G_BotNavFindPath( self->r.currentOrigin, spot, path, ARRAY_LEN( path ), &query );
    if( n <= 0 )
      continue;

    G_BotNavNodeOrigin( path[ n - 1 ], end );
    if( Distance( end, spot ) >= 100.0f )
      continue;

    if( !BotTooCrowded( team, type, spot ) )
    {
      VectorCopy( spot, bs->buildSpot );
      found = qtrue;
      break;
    }

    // crowded, but somewhere to fall back on
    if( !found )
    {
      VectorCopy( spot, bs->buildSpot );
      bs->buildCrowded = qtrue;
      found = qtrue;
    }
  }

  // nowhere we can get to, try again later
  if( !found )
    return qfalse;

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
    // corridor (players walk over low structures like telenodes). Keep
    // apart from others of the same kind, unless there was nowhere else
    if( !G_BotNavOpenGround( mins, maxs ) ||
        ( !bs->buildCrowded &&
          BotTooCrowded( self->client->pers.teamSelection, bs->buildType, origin ) ) ||
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
          if( ++bs->buildSpots >= 4 || !BotPlanBuild( self, bs ) )
          {
            ps->stats[ STAT_BUILDABLE ] = BA_NONE;
            BotGiveUpBuild( bs, 15000 );
          }
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
  qboolean  moving = qfalse;
  int       moveFlags = 0;
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
        BotPathDirection( self, bs, armoury->r.currentOrigin, moveDir, &moveFlags );
        moving = qtrue;
        BotFaceMove( self, bs, moveDir, moveFlags, desired );
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
      BotPathDirection( self, bs, core->r.currentOrigin, moveDir, &moveFlags );
    else
      VectorSubtract( self->r.currentOrigin, enemy->r.currentOrigin, moveDir );

    moving = qtrue;
    BotFaceMove( self, bs, moveDir, moveFlags, desired );
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
          BotPathDirection( self, bs, surplus->r.currentOrigin, moveDir, &moveFlags );
          moving = qtrue;
          BotFaceMove( self, bs, moveDir, moveFlags, desired );
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
        // somewhere we can't get to, try another spot
        if( !BotPathDirection( self, bs, bs->buildSpot, moveDir, &moveFlags ) )
        {
          if( ++bs->buildSpots >= 4 || !BotPlanBuild( self, bs ) )
            BotGiveUpBuild( bs, 5000 );
        }
        moving = qtrue;
        BotFaceMove( self, bs, moveDir, moveFlags, desired );
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
        BotPathDirection( self, bs, repair->r.currentOrigin, moveDir, &moveFlags );
        moving = qtrue;
        BotFaceMove( self, bs, moveDir, moveFlags, desired );
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
      BotPathDirection( self, bs, core->r.currentOrigin, moveDir, &moveFlags );
      moving = qtrue;
      BotFaceMove( self, bs, moveDir, moveFlags, desired );
    }
  }

  BotTurn( bs, view, desired, msec, 1.0f );
  BotMove( self, bs, cmd, view, moveDir, moving, moveFlags, 0.0f, qtrue, msec );
}

/*
===============
BotThinkAlive
===============
*/

// would running for it get us anywhere? Not if we're badly outnumbered, or
// if there's nowhere on the way back out of the enemy's sight: they'd just
// shoot us in the back, so we're better off fighting
static qboolean BotRetreatSurvivable( gentity_t *self, botState_t *bs, gentity_t *enemy )
{
  team_t      team = self->client->pers.teamSelection;
  int         enemies = 0, allies = 0, i, n;
  int         path[ 32 ];
  navQuery_t  query;
  vec3_t      eye, node, last;
  trace_t     tr;
  float       travelled = 0.0f;

  for( i = 0; i < botNumPlayers; i++ )
  {
    gentity_t *other = botPlayers[ i ];
    float     d;

    if( other == self )
      continue;

    d = Distance( other->r.currentOrigin, self->r.currentOrigin );
    if( d > 800.0f )
      continue;

    if( other->client->pers.teamSelection == team )
      allies++;
    else if( d < 300.0f || BotCanSee( self, other ) )
      enemies++;
  }

  if( enemies >= 3 && enemies >= ( allies + 1 ) * 2 )
    return qfalse;

  // structures don't give chase
  if( !enemy->client )
    return qtrue;

  BotNavQuery( self, bs, &query );
  n = G_BotNavFindPath( self->r.currentOrigin, bs->goalOrigin, path, ARRAY_LEN( path ), &query );
  if( !n )
    return qfalse;

  BotEyes( enemy, eye );
  VectorCopy( self->r.currentOrigin, last );
  for( i = 0; i < n && travelled < 600.0f; i++ )
  {
    G_BotNavNodeOrigin( path[ i ], node );
    travelled += Distance( last, node );
    VectorCopy( node, last );

    // round a corner or behind something
    node[ 2 ] += 16.0f;
    trap_Trace( &tr, eye, NULL, NULL, node, enemy->s.number, MASK_SOLID );
    if( tr.fraction < 1.0f )
      return qtrue;
  }

  // or nearly home anyway
  return Distance( last, bs->goalOrigin ) < 300.0f;
}

// heading back to heal with an enemy about: fall back rather than fight,
// unless we're nearly there anyway, it's nearly dead, or we wouldn't make it
static qboolean BotShouldRetreat( gentity_t *self, botState_t *bs, gentity_t *enemy )
{
  if( bs->goal != BG_HEAL ||
      Distance( self->r.currentOrigin, bs->goalOrigin ) < 250.0f )
    return qfalse;

  if( enemy->client &&
      enemy->client->ps.stats[ STAT_HEALTH ] < enemy->client->ps.stats[ STAT_MAX_HEALTH ] / 4 )
    return qfalse;

  if( level.time >= bs->nextRetreatCheck )
  {
    qboolean hopeless = !BotRetreatSurvivable( self, bs, enemy );

    bs->nextRetreatCheck = level.time + 1000;
    if( hopeless && !bs->retreatHopeless )
      bs->statLastStands++;
    bs->retreatHopeless = hopeless;
  }

  return !bs->retreatHopeless;
}

// climbers fight from walls too: up to enemies they can't walk to, and
// dropping onto them from above
static void BotClimbAttack( gentity_t *self, botState_t *bs, gentity_t *enemy, const vec3_t dir )
{
  float horizontal = sqrt( dir[ 0 ] * dir[ 0 ] + dir[ 1 ] * dir[ 1 ] );

  if( BotOnWall( self ) )
  {
    if( dir[ 2 ] < -32.0f && horizontal < 96.0f )
    {
      bs->wallUntil = 0;
      bs->noWallUntil = level.time + 1500;
    }
    else
      bs->wallUntil = level.time + 300;
  }
  else if( dir[ 2 ] > 64.0f && level.time >= bs->noWallUntil &&
           !BotCanWalkTo( self, enemy->r.currentOrigin ) )
    bs->wallUntil = level.time + 1000;
}

// humans dodge aside as an alien closes in, and hop about when one is at
// their feet
static void BotHumanEvade( gentity_t *self, botState_t *bs, usercmd_t *cmd, gentity_t *enemy,
                           float dist, const vec3_t view )
{
  playerState_t *ps = &self->client->ps;
  vec3_t        toEnemy, angles, forward, right;
  float         approach, side;

  if( !enemy->client || enemy->client->pers.teamSelection != TEAM_ALIENS )
    return;

  // with a jetpack, get up out of reach of claws and teeth
  if( BotCanFly( self ) && dist < 400.0f && BotAttackRange( enemy->client->ps.weapon ) < 200.0f )
  {
    bs->hoverUntil = level.time + 2000;
    return;
  }

  if( ps->groundEntityNum == ENTITYNUM_NONE || ( ps->pm_flags & PMF_DUCKED ) )
    return;

  VectorSubtract( enemy->r.currentOrigin, self->r.currentOrigin, toEnemy );
  VectorNormalize( toEnemy );
  approach = -DotProduct( enemy->client->ps.velocity, toEnemy );

  if( level.time >= bs->nextDodgeTime && dist < 350.0f &&
      ps->stats[ STAT_STAMINA ] > STAMINA_BREATHING_LEVEL )
  {
    bs->nextDodgeTime = level.time + 400;

    if( ( approach > 250.0f || ( enemy->client->ps.weapon == WP_ALEVEL3 ||
                                 enemy->client->ps.weapon == WP_ALEVEL3_UPG ) &&
          enemy->client->ps.groundEntityNum == ENTITYNUM_NONE ) &&
        BotRandom( ) < 0.2f + bs->skill * 0.1f )
    {
      // not off a ledge
      side = ( rand( ) & 1 ) ? 1.0f : -1.0f;
      VectorSet( angles, 0.0f, view[ YAW ], 0.0f );
      AngleVectors( angles, forward, right, NULL );
      if( !BotGroundAhead( self, forward, right, 0.0f, side, 100.0f ) )
        side = -side;

      if( BotGroundAhead( self, forward, right, 0.0f, side, 100.0f ) )
      {
        cmd->forwardmove = 0;
        cmd->rightmove = side * 127;
        cmd->upmove = 0;
        cmd->buttons |= BUTTON_DODGE;
        cmd->buttons &= ~BUTTON_SPRINT;
        bs->nextDodgeTime = level.time + 2500 - bs->skill * 200 + rand( ) % 1000;
        bs->statDodges++;
        return;
      }
    }
  }

  if( dist < 120.0f && level.time >= bs->nextHopTime &&
      ps->stats[ STAT_STAMINA ] > STAMINA_MAX * 3 / 10 )
  {
    bs->nextHopTime = level.time + 800 + rand( ) % 1200;
    if( BotRandom( ) < 0.35f && BotSafeToJump( self, cmd, view ) )
      cmd->upmove = 127;
  }
}

// we've got as close as the graph goes to our goal. If we can't see it from
// here, find something else to do for a while
static void BotGoalUnreachable( gentity_t *self, botState_t *bs )
{
  gentity_t *target = bs->goalEntity >= 0 ? &g_entities[ bs->goalEntity ] : NULL;

  // close enough to shoot at, maybe
  if( target && target->inuse && target->s.eType == ET_BUILDABLE &&
      Distance( target->r.currentOrigin, self->r.currentOrigin ) < BOT_BUILDABLE_SIGHT &&
      BotCanSee( self, target ) )
    return;

  if( bot_debug.integer )
    G_Printf( "botunreachable: %s %s at (%.0f %.0f %.0f) goal %d to (%.0f %.0f %.0f)\n",
              BG_TeamName( self->client->pers.teamSelection ),
              BG_Class( self->client->ps.stats[ STAT_CLASS ] )->name,
              self->r.currentOrigin[ 0 ], self->r.currentOrigin[ 1 ], self->r.currentOrigin[ 2 ],
              bs->goal, bs->goalOrigin[ 0 ], bs->goalOrigin[ 1 ], bs->goalOrigin[ 2 ] );

  if( target )
  {
    bs->blockedEntity = bs->goalEntity;
    bs->blockedUntil = level.time + 30000;
  }

  if( bs->goal == BG_HUNT )
    bs->enemy = -1;

  bs->goal = BG_NONE;
  bs->goalEntity = -1;
  bs->viaActive = qfalse;
  bs->routeFailures = 0;

  // rather than straight away picking something else that may be no
  // better, look around for a while, or if that's failing too just wander
  if( level.time - bs->goalFailTime < 2000 )
  {
    bs->idleUntil = level.time + 1500 + rand( ) % 1000;
    bs->unstuckUntil = bs->idleUntil;
    bs->strafeDir = ( rand( ) & 1 ) ? 1.0f : -1.0f;
  }
  else
    BotChooseRoam( self, bs );

  bs->goalFailTime = level.time;
}

static void BotThinkAlive( gentity_t *self, botState_t *bs, usercmd_t *cmd, int msec )
{
  gclient_t *client = self->client;
  team_t    team = client->pers.teamSelection;
  gentity_t *enemy = NULL;
  vec3_t    view, desired, dir, aimPoint, eye, toEnemy;
  vec3_t    moveDir;
  qboolean  moving = qfalse, sprint = qtrue, running = qfalse;
  int       moveFlags = 0;
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

  // badly hurt in the middle of something else: time to go and heal. Or
  // the base is under attack
  if( bs->goal != BG_HEAL && level.time >= bs->nextHealCheck )
  {
    gentity_t *threat = bs->goal != BG_DEFEND ? BotBaseThreat( team ) : NULL;

    bs->nextHealCheck = level.time + 1000;

    if( BotNeedsHeal( self ) ||
        ( threat && Distance( threat->r.currentOrigin, self->r.currentOrigin ) < 2500.0f ) )
      bs->nextGoalTime = 0;
  }

  // keep up with whoever's in the base
  if( bs->goal == BG_DEFEND && bs->goalEntity >= 0 && BotIsAlive( &g_entities[ bs->goalEntity ] ) )
    VectorCopy( g_entities[ bs->goalEntity ].r.currentOrigin, bs->goalOrigin );

  if( level.time >= bs->idleUntil &&
      ( level.time >= bs->nextGoalTime || bs->goal == BG_NONE ||
        bs->goal == BG_HUNT || bs->goal == BG_HEAL || bs->goal == BG_ARMOURY ||
        bs->goal == BG_REGROUP || bs->goal == BG_ROAM ) )
    BotChooseGoal( self, bs );

  // economy
  if( team == TEAM_HUMANS )
  {
    BotShop( self, bs );

    // the medkit heals over time and cures poison
    if( BG_InventoryContainsUpgrade( UP_MEDKIT, client->ps.stats ) &&
        ( client->ps.stats[ STAT_HEALTH ] < client->ps.stats[ STAT_MAX_HEALTH ] / 2 ||
          ( client->ps.stats[ STAT_STATE ] & SS_POISONED ) ) &&
        !( client->ps.stats[ STAT_STATE ] & SS_HEALING_2X ) &&
        level.time >= bs->nextMedkitTime )
    {
      bs->nextMedkitTime = level.time + 1000;
      bs->statMedkits++;
      BotCommand( self, "itemact medkit" );
    }
  }
  else if( team == TEAM_ALIENS && !enemy )
    BotEvolve( self, bs );

  // a big alien that's got itself somewhere it can't get out of to the
  // enemy (a one way drop into a part of the base it won't fit back out
  // of) is no use to anybody: start again
  if( team == TEAM_ALIENS && !enemy && level.time >= bs->nextReachCheck &&
      G_BotNavSizeClass( self->r.mins, self->r.maxs ) > 1 )
  {
    bs->nextReachCheck = level.time + 20000;

    // only if we're stuck in the same part of the map, not just on the
    // other side of it from the enemy
    if( Distance( self->r.currentOrigin, bs->reachOrigin ) > 600.0f ||
        BotClassCanReach( self, bs, client->ps.stats[ STAT_CLASS ] ) )
      bs->reachFailures = 0;
    else if( ++bs->reachFailures >= 2 )
    {
      if( bot_debug.integer )
        G_Printf( "bottrapped: %s %s at (%.0f %.0f %.0f) can't reach the enemy, respawning\n",
                  client->pers.netname, BG_Class( client->ps.stats[ STAT_CLASS ] )->name,
                  self->r.currentOrigin[ 0 ], self->r.currentOrigin[ 1 ], self->r.currentOrigin[ 2 ] );
      bs->reachFailures = 0;
      BotCommand( self, "kill" );
    }
    VectorCopy( self->r.currentOrigin, bs->reachOrigin );
  }

  // the command may have changed our class or killed us
  if( !BotIsAlive( self ) )
    return;

  if( enemy )
  {
    float handicap = BotAimHandicap( self, enemy );
    float aimSkill = bs->skill - handicap;

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
      float spread = ( BOT_MAX_SKILL + 1 - aimSkill ) * 1.5f;

      // fast movers are hard to keep a gun on
      if( team == TEAM_HUMANS && enemy->client )
        spread += VectorLength( enemy->client->ps.velocity ) * 0.0035f;

      bs->nextAimErrorTime = level.time + 300 + rand( ) % 400;
      VectorSet( bs->aimError, crandom( ) * spread * 0.5f, crandom( ) * spread, 0.0f );

      // an angle is only a few units off up close, where a dretch fights,
      // so handicapped bots also aim a distance to the side of the target
      if( handicap > 0.0f )
      {
        float missAngle = random( ) * 2.0f * M_PI;
        float missDist = handicap * 3.0f * ( 0.5f + 0.5f * random( ) );

        VectorSet( bs->aimMiss, cos( missAngle ) * missDist,
                   sin( missAngle ) * missDist, 0.0f );
      }
      else
        VectorClear( bs->aimMiss );
    }

    // react to movement late, so fast targets are hard to track. Aliens
    // only need to be close, humans have to be on target
    if( enemy->client )
      VectorMA( aimPoint, -( BOT_MAX_SKILL + 1 - aimSkill ) *
                          ( team == TEAM_HUMANS ? 0.029f : 0.06f ),
                enemy->client->ps.velocity, aimPoint );

    if( handicap > 0.0f )
    {
      vec3_t right, up;

      VectorSubtract( aimPoint, eye, toEnemy );
      vectoangles( toEnemy, desired );
      AngleVectors( desired, NULL, right, up );
      VectorMA( aimPoint, bs->aimMiss[ 0 ], right, aimPoint );
      VectorMA( aimPoint, bs->aimMiss[ 1 ], up, aimPoint );
    }

    VectorSubtract( aimPoint, eye, toEnemy );
    dist = VectorNormalize( toEnemy );
    vectoangles( toEnemy, desired );
    desired[ PITCH ] = AngleMod( desired[ PITCH ] + bs->aimError[ PITCH ] );
    desired[ YAW ] = AngleMod( desired[ YAW ] + bs->aimError[ YAW ] );

    // dragoons leap in an arc
    if( bs->chargeStart && ( client->ps.weapon == WP_ALEVEL3 ||
                             client->ps.weapon == WP_ALEVEL3_UPG ) )
      desired[ PITCH ] = AngleMod( desired[ PITCH ] - 15.0f );

    // and turn slower, so a fast or close target is hard to keep up with
    BotTurn( bs, view, desired, msec, 1.0f / ( 1.0f + handicap * 0.15f ) );

    // movement in combat
    VectorSubtract( enemy->r.currentOrigin, self->r.currentOrigin, dir );

    if( BotShouldRetreat( self, bs, enemy ) )
    {
      // fall back to heal: humans keep shooting as they back off until
      // there's room to turn and run, aliens run unless it's caught up
      if( !BotPathDirection( self, bs, bs->goalOrigin, moveDir, &moveFlags ) )
        VectorNegate( dir, moveDir );
      moving = qtrue;

      running = ( team == TEAM_ALIENS ) ? dist > BotAttackRange( client->ps.weapon ) : dist > 500.0f;
      sprint = running;
      if( running )
      {
        BotFaceMove( self, bs, moveDir, moveFlags, desired );
        BotTurn( bs, view, desired, msec, 1.0f );
      }
    }
    else if( team == TEAM_ALIENS )
    {
      // charge straight in when we can, otherwise path to them
      if( BotCanWalkTo( self, enemy->r.currentOrigin ) )
        VectorCopy( dir, moveDir );
      else
        BotPathDirection( self, bs, enemy->r.currentOrigin, moveDir, &moveFlags );

      moving = qtrue;

      if( BotCanClimb( self ) )
        BotClimbAttack( self, bs, enemy, dir );

      if( BotWantsWall( self, bs, moveFlags ) )
        VectorCopy( dir, moveDir );
      else
      {
        if( dir[ 2 ] > 48.0f && dist < 200.0f )
          moveFlags |= BOT_MOVE_JUMP;

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
            moveFlags |= BOT_MOVE_JUMP;
        }
      }
    }
    else
    {
      float preferred = BotPreferredRange( client->ps.weapon );

      if( enemy->s.eType == ET_BUILDABLE )
        preferred = MAX( preferred, 250.0f );

      // close the distance quickly, keeping some stamina back for dodging
      sprint = qfalse;
      if( dist > preferred * 1.5f )
      {
        if( BotCanWalkTo( self, enemy->r.currentOrigin ) )
          VectorCopy( dir, moveDir );
        else
          BotPathDirection( self, bs, enemy->r.currentOrigin, moveDir, &moveFlags );
        moving = qtrue;
        sprint = client->ps.stats[ STAT_STAMINA ] > STAMINA_MAX / 2;
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
    if( !running )
    {
      AngleVectors( desired, toEnemy, NULL, NULL );
      BotAttack( self, bs, cmd, enemy, dist, BotAngleTo( view, toEnemy ) );
    }
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
      else if( bs->goal == BG_ROAM )
        closeEnough = 150.0f;
      else if( bs->goal == BG_DEFEND )
        closeEnough = 100.0f;

      if( goalDist > closeEnough )
      {
        if( BotPathDirection( self, bs, bs->goalOrigin, moveDir, &moveFlags ) )
        {
          moving = qtrue;

          if( bs->goal == BG_ATTACK || bs->goal == BG_HUNT || bs->goal == BG_ROAM )
            BotCeilingCrawl( self, bs, moveDir, &moveFlags );
        }
        else
          BotGoalUnreachable( self, bs );
      }
      else if( bs->goal == BG_HUNT )
      {
        // nobody here
        bs->enemy = -1;
        bs->goal = BG_NONE;
      }
      else if( bs->goal == BG_ROAM )
        bs->goal = BG_NONE;
    }

    // look where we're going
    if( moving && VectorLength( moveDir ) > 0.1f )
    {
      BotFaceMove( self, bs, moveDir, moveFlags, desired );
      BotTurn( bs, view, desired, msec, 1.0f );
    }
  }

  BotMove( self, bs, cmd, view, moveDir, moving, moveFlags, strafe, sprint, msec );

  if( enemy && team == TEAM_HUMANS )
    BotHumanEvade( self, bs, cmd, enemy, dist, view );
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

  // teleported or thrown somewhere, the old route is no use
  if( DistanceSquared( client->ps.origin, bs->lastFramePos ) > Square( 200.0f ) )
  {
    bs->nextPathTime = 0;
    bs->pathLength = 0;
  }
  VectorCopy( client->ps.origin, bs->lastFramePos );

  BotThinkAlive( self, bs, cmd, msec );

  bs->lastMove[ 0 ] = cmd->forwardmove;
  bs->lastMove[ 1 ] = cmd->rightmove;
  bs->lastMove[ 2 ] = cmd->upmove;
}

// what bots have been up to: time spent sprinting, crouched and on walls,
// how often they jumped, climbed, dodged and got stuck, and how much of
// the map they've seen
static void BotPrintStats( const char *prefix, botState_t *bs, int count )
{
  int i, regions = 0;

  for( i = 0; i < ARRAY_LEN( bs->statRegions ); i++ )
  {
    byte b = bs->statRegions[ i ];

    for( ; b; b &= b - 1 )
      regions++;
  }

  G_Printf( "%ssprint %ds crouch %ds wall %ds ceiling %ds jetpack %ds tunnels %ds, %d jumps %d hops "
            "%d climbs %d dodges, %d retreats %d last stands %d medkits, stuck %d circling %d, %d areas%s\n", prefix,
            bs->statSprint / 1000, bs->statCrouch / 1000, bs->statWall / 1000,
            bs->statCeiling / 1000, bs->statJet / 1000, bs->statTunnel / 1000, bs->statJumps,
            bs->statHops, bs->statClimbs,
            bs->statDodges, bs->statRetreats, bs->statLastStands, bs->statMedkits, bs->statStuck, bs->statCircling, regions, count > 1 ? " visited between them" : "" );
}

// the same for a whole team
static void BotPrintTeamStats( team_t team )
{
  botState_t  total;
  int         i, j, count = 0;

  memset( &total, 0, sizeof( total ) );
  for( i = 0; i < level.maxclients; i++ )
  {
    botState_t *bs = &botStates[ i ];

    if( !level.clients[ i ].pers.isBot || level.clients[ i ].pers.connected == CON_DISCONNECTED ||
        bs->team != team )
      continue;

    count++;
    total.statSprint += bs->statSprint;
    total.statCrouch += bs->statCrouch;
    total.statWall += bs->statWall;
    total.statCeiling += bs->statCeiling;
    total.statJet += bs->statJet;
    total.statTunnel += bs->statTunnel;
    total.statJumps += bs->statJumps;
    total.statHops += bs->statHops;
    total.statClimbs += bs->statClimbs;
    total.statDodges += bs->statDodges;
    total.statStuck += bs->statStuck;
    total.statCircling += bs->statCircling;
    total.statRetreats += bs->statRetreats;
    total.statMedkits += bs->statMedkits;
    total.statLastStands += bs->statLastStands;
    for( j = 0; j < ARRAY_LEN( total.statRegions ); j++ )
      total.statRegions[ j ] |= bs->statRegions[ j ];
  }

  if( count )
    BotPrintStats( va( "botstats: %s %d bots, ", BG_TeamName( team ), count ), &total, count );
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
  bs->routeSeed = rand( );
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
  memset( botWasAlive, 0, sizeof( botWasAlive ) );
  botNextStatsTime = 0;
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

  // where anybody dies, their team learns to keep away from for a while
  for( i = 0; i < level.maxclients; i++ )
  {
    gclient_t *client = &level.clients[ i ];
    qboolean  alive = BotIsAlive( &g_entities[ i ] );

    if( botWasAlive[ i ] && !alive && client->pers.connected == CON_CONNECTED )
      G_BotNavAddDanger( client->pers.teamSelection, client->ps.origin, 300.0f );
    botWasAlive[ i ] = alive;
  }

  if( bot_debug.integer && level.time >= botNextStatsTime )
  {
    if( botNextStatsTime )
    {
      BotPrintTeamStats( TEAM_ALIENS );
      BotPrintTeamStats( TEAM_HUMANS );
    }
    botNextStatsTime = level.time + 60000;
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
        G_Printf( "botdeath: %s %s at (%.0f %.0f %.0f) vel (%.0f %.0f %.0f) goal %d enemy %d%s mod %d "
                  "last ground (%.0f %.0f %.0f) %dms ago, alive %dms%s%s\n",
                  client->pers.netname, BG_Class( bs->lastClass )->name,
                  client->ps.origin[ 0 ], client->ps.origin[ 1 ],
                  client->ps.origin[ 2 ], client->ps.velocity[ 0 ], client->ps.velocity[ 1 ],
                  client->ps.velocity[ 2 ], bs->goal, bs->enemy,
                  bs->enemyVisible ? " (visible)" : "", client->lasthurt_mod,
                  bs->lastGround[ 0 ], bs->lastGround[ 1 ], bs->lastGround[ 2 ],
                  level.time - bs->lastGroundTime, level.time - bs->spawnTime,
                  level.time - bs->lastWallTime < 3000 ? " (was on a wall)" : "",
                  level.time - bs->lastJumpTime < 3000 ? " (jumped)" : "" );
      if( alive && !bs->wasAlive )
        bs->spawnTime = level.time;
      if( alive && client->ps.groundEntityNum != ENTITYNUM_NONE )
      {
        VectorCopy( client->ps.origin, bs->lastGround );
        bs->lastGroundTime = level.time;
      }
      if( alive )
      {
        bs->lastClass = client->ps.stats[ STAT_CLASS ];
        if( BotOnWall( ent ) )
          bs->lastWallTime = level.time;
        if( client->pers.cmd.upmove > 0 || ( client->pers.cmd.buttons & BUTTON_DODGE ) )
          bs->lastJumpTime = level.time;
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
  static const char *goalNames[ ] = { "none", "attack", "hunt", "armoury", "heal", "regroup", "roam",
                                       "defend" };
  int i;

  G_Printf( "navigation graph: %d nodes\n", G_BotNavNumNodes( ) );
  BotPrintTeamStats( TEAM_ALIENS );
  BotPrintTeamStats( TEAM_HUMANS );

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
    BotPrintStats( "   ", bs, 1 );

    if( bs->pathIndex < bs->pathLength )
    {
      vec3_t node;
      int    j;

      G_Printf( "   path:" );
      for( j = MAX( 0, bs->pathIndex - 1 ); j < bs->pathLength && j < bs->pathIndex + 3; j++ )
      {
        G_BotNavNodeOrigin( bs->path[ j ], node );
        G_Printf( " %d(%.0f %.0f %.0f)%s", bs->path[ j ], node[ 0 ], node[ 1 ], node[ 2 ],
                  j > 0 && ( G_BotNavLinkFlags( bs->path[ j - 1 ], bs->path[ j ], NULL ) & NAVLINK_JUMP ) ?
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
