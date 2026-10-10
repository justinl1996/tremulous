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
Bot navigation

Tremulous maps don't ship with the AAS files that the ioquake3 botlib needs,
so the bots build their own navigation graph at runtime instead.

Starting from seed points (spawns, buildables and player positions) the
graph is flood filled across a regular grid: for every node we try to step,
climb or jump into each of the eight neighbouring cells using player sized
traces, creating nodes wherever there is walkable ground. The flood fill is
time sliced across server frames so it never causes a hitch.

The graph also grows from where players go: anywhere a player stands that
has no node gets one, which reaches areas no seed led to. Doors that open
themselves are ignored while building; doors something else opens (like
stage doors) are treated as walls until they open.

Paths are found with A* over the graph.
===========================================================================
*/

#include "g_local.h"

#define NAV_MAX_NODES       12288
#define NAV_MAX_LINKS       10
#define NAV_GRID            48.0f
#define NAV_HASH_SIZE       4096
#define NAV_STEP            18.0f   // STEPSIZE in bg_pmove.c
#define NAV_JUMP            40.0f   // comfortably below a human's jump height
#define NAV_MAX_DROP        256.0f
#define NAV_WALK_NORMAL     0.7f    // MIN_WALK_NORMAL in bg_local.h
#define NAV_LEVEL_DIST      40.0f   // nodes in one cell closer than this vertically are the same node
#define NAV_MASK            ( CONTENTS_SOLID | CONTENTS_PLAYERCLIP | CONTENTS_BOTCLIP )
#define NAV_BAD_CONTENTS    ( CONTENTS_LAVA | CONTENTS_SLIME | CONTENTS_NODROP )
#define NAV_MAX_HURT        64
#define NAV_MAX_REQUEUE     2048
#define NAV_HEAP_SIZE       ( NAV_MAX_NODES * 2 )
#define NAV_SEARCH_LIMIT    6000

typedef struct
{
  vec3_t  origin;
  int     cellX, cellY;
  short   links[ NAV_MAX_LINKS ];
  byte    linkFlags[ NAV_MAX_LINKS ];
  int     numLinks;
  int     hashNext;
} navNode_t;

typedef struct
{
  int     node;
  float   f;
} navHeapEntry_t;

static navNode_t      nodes[ NAV_MAX_NODES ];
static int            numNodes;
static int            hashHeads[ NAV_HASH_SIZE ];
static int            expandCursor;
static qboolean       navInitialised;
static qboolean       navFullWarned;
static vec3_t         navMins, navMaxs;

static vec3_t         hurtMins[ NAV_MAX_HURT ], hurtMaxs[ NAV_MAX_HURT ];
static int            numHurt;

static int            observedNode[ MAX_CLIENTS ];
static int            nextObserveTime;
static int            buildMsec;
static qboolean       buildReported;

// A* scratch space
static float          gScore[ NAV_MAX_NODES ];
static int            cameFrom[ NAV_MAX_NODES ];
static int            openStamp[ NAV_MAX_NODES ];
static int            closedStamp[ NAV_MAX_NODES ];
static int            searchStamp;
static navHeapEntry_t heap[ NAV_HEAP_SIZE ];
static int            heapSize;
static int            reversePath[ NAV_MAX_NODES ];

static const int      navDirs[ 8 ][ 2 ] =
{
  { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 },
  { 1, 1 }, { 1, -1 }, { -1, 1 }, { -1, -1 }
};

/*
===============
Grid helpers
===============
*/
static int NavCell( float v )
{
  return (int)floor( v / NAV_GRID + 0.5f );
}

static int NavHash( int x, int y )
{
  return ( ( x * 73856093 ) ^ ( y * 19349663 ) ) & ( NAV_HASH_SIZE - 1 );
}

static int NavFindNodeInCell( int x, int y, float z )
{
  int n;

  for( n = hashHeads[ NavHash( x, y ) ]; n >= 0; n = nodes[ n ].hashNext )
  {
    if( nodes[ n ].cellX == x && nodes[ n ].cellY == y &&
        fabs( nodes[ n ].origin[ 2 ] - z ) < NAV_LEVEL_DIST )
      return n;
  }

  return -1;
}

static int NavAddNode( const vec3_t origin )
{
  int       x = NavCell( origin[ 0 ] );
  int       y = NavCell( origin[ 1 ] );
  int       n, h;
  navNode_t *node;

  n = NavFindNodeInCell( x, y, origin[ 2 ] );
  if( n >= 0 )
    return n;

  if( numNodes >= NAV_MAX_NODES )
  {
    if( !navFullWarned )
    {
      G_Printf( "Bot navigation: node limit (%d) reached\n", NAV_MAX_NODES );
      navFullWarned = qtrue;
    }
    return -1;
  }

  n = numNodes++;
  node = &nodes[ n ];
  VectorCopy( origin, node->origin );
  node->cellX = x;
  node->cellY = y;
  node->numLinks = 0;

  h = NavHash( x, y );
  node->hashNext = hashHeads[ h ];
  hashHeads[ h ] = n;

  return n;
}

static void NavAddLink( int from, int to, int flags )
{
  navNode_t *node;
  int       i;

  if( from < 0 || to < 0 || from == to )
    return;

  node = &nodes[ from ];
  for( i = 0; i < node->numLinks; i++ )
  {
    if( node->links[ i ] == to )
      return;
  }

  if( node->numLinks >= NAV_MAX_LINKS )
    return;

  node->links[ node->numLinks ] = to;
  node->linkFlags[ node->numLinks ] = flags;
  node->numLinks++;
}

/*
===============
Trace helpers
===============
*/

// doors open as players approach, so the graph is built with those taken out
// of the world; only the ones that were linked are put back afterwards
static int      unlinkedMovers[ MAX_GENTITIES ];
static int      numUnlinkedMovers;

// doors that something else opens (stage doors and the like) remember whether
// they were open, so the graph can be extended when they open
static byte     moverWasOpen[ MAX_GENTITIES ];
static int      nextMoverCheckTime;

// nodes to expand again because a door near them opened
static int      requeue[ NAV_MAX_REQUEUE ];
static int      numRequeue;

static qboolean NavMoverPassable( gentity_t *ent )
{
  // doors without a name open themselves when a player walks up
  if( !ent->targetname || !ent->targetname[ 0 ] )
    return qtrue;

  switch( ent->moverState )
  {
    case MOVER_POS2:
    case MOVER_1TO2:
    case ROTATOR_POS2:
    case ROTATOR_1TO2:
    case MODEL_POS2:
    case MODEL_1TO2:
      return qtrue;

    default:
      return qfalse;
  }
}

static void NavUnlinkMovers( void )
{
  gentity_t *ent;
  int       i;

  numUnlinkedMovers = 0;
  for( i = MAX_CLIENTS, ent = g_entities + MAX_CLIENTS; i < level.num_entities; i++, ent++ )
  {
    if( ent->inuse && ent->s.eType == ET_MOVER && ent->r.linked &&
        NavMoverPassable( ent ) )
    {
      trap_UnlinkEntity( ent );
      unlinkedMovers[ numUnlinkedMovers++ ] = i;
    }
  }
}

static void NavRelinkMovers( void )
{
  int i;

  for( i = 0; i < numUnlinkedMovers; i++ )
    trap_LinkEntity( &g_entities[ unlinkedMovers[ i ] ] );

  numUnlinkedMovers = 0;
}

// trace a player sized box
static qboolean NavTraceMove( const vec3_t start, const vec3_t end )
{
  trace_t tr;

  trap_Trace( &tr, start, navMins, navMaxs, end, ENTITYNUM_NONE, NAV_MASK );

  return !tr.startsolid && !tr.allsolid && tr.fraction >= 1.0f;
}

static qboolean NavInHurt( const vec3_t p )
{
  int i;

  for( i = 0; i < numHurt; i++ )
  {
    if( p[ 0 ] >= hurtMins[ i ][ 0 ] && p[ 0 ] <= hurtMaxs[ i ][ 0 ] &&
        p[ 1 ] >= hurtMins[ i ][ 1 ] && p[ 1 ] <= hurtMaxs[ i ][ 1 ] &&
        p[ 2 ] + navMins[ 2 ] <= hurtMaxs[ i ][ 2 ] &&
        p[ 2 ] + navMaxs[ 2 ] >= hurtMins[ i ][ 2 ] )
      return qtrue;
  }

  return qfalse;
}

qboolean G_BotNavInHazard( const vec3_t origin )
{
  return NavInHurt( origin );
}

// find walkable ground below point, out is the origin of a player standing there
static qboolean NavGroundAt( const vec3_t point, vec3_t out )
{
  trace_t tr;
  vec3_t  end;
  vec3_t  feet;

  VectorCopy( point, end );
  end[ 2 ] -= NAV_MAX_DROP;

  trap_Trace( &tr, point, navMins, navMaxs, end, ENTITYNUM_NONE, NAV_MASK );

  if( tr.startsolid || tr.allsolid || tr.fraction >= 1.0f )
    return qfalse;

  if( tr.plane.normal[ 2 ] < NAV_WALK_NORMAL )
    return qfalse;

  VectorCopy( tr.endpos, out );

  VectorCopy( out, feet );
  feet[ 2 ] += navMins[ 2 ] + 1.0f;
  if( trap_PointContents( feet, ENTITYNUM_NONE ) & NAV_BAD_CONTENTS )
    return qfalse;

  if( NavInHurt( out ) )
    return qfalse;

  return qtrue;
}

// try to move horizontally from a standing position, stepping up if needed
static qboolean NavTryWalk( const vec3_t from, const vec3_t to, float height, vec3_t ground )
{
  vec3_t a, b;

  VectorCopy( from, a );
  a[ 2 ] += height;
  VectorSet( b, to[ 0 ], to[ 1 ], a[ 2 ] );

  if( height > NAV_STEP && !NavTraceMove( from, a ) )
    return qfalse;

  if( !NavTraceMove( a, b ) )
    return qfalse;

  return NavGroundAt( b, ground );
}

static qboolean NavTryStep( const vec3_t from, const vec3_t to, vec3_t ground, int *flags )
{
  vec3_t mid, midGround;

  *flags = 0;

  if( NavTryWalk( from, to, NAV_STEP, ground ) )
    return qtrue;

  // stairs and ramps can rise more than a step over a whole cell
  VectorAdd( from, to, mid );
  VectorScale( mid, 0.5f, mid );
  if( NavTryWalk( from, mid, NAV_STEP, midGround ) &&
      NavTryWalk( midGround, to, NAV_STEP, ground ) )
    return qtrue;

  // too high to step over, even if we land at the same height (a rail)
  if( NavTryWalk( from, to, NAV_JUMP, ground ) )
  {
    *flags = NAVLINK_JUMP;
    return qtrue;
  }

  return qfalse;
}

static void NavExpandNode( int n )
{
  int     i, m, flags;
  vec3_t  to, ground;
  int     x, y;

  for( i = 0; i < 8; i++ )
  {
    // the node array may not move, but read through the index anyway
    x = nodes[ n ].cellX + navDirs[ i ][ 0 ];
    y = nodes[ n ].cellY + navDirs[ i ][ 1 ];
    VectorSet( to, x * NAV_GRID, y * NAV_GRID, nodes[ n ].origin[ 2 ] );

    if( !NavTryStep( nodes[ n ].origin, to, ground, &flags ) )
      continue;

    m = NavFindNodeInCell( x, y, ground[ 2 ] );
    if( m < 0 )
      m = NavAddNode( ground );

    NavAddLink( n, m, flags );
  }
}

/*
===============
Seeding
===============
*/
static int NavSeed( const vec3_t point )
{
  vec3_t start, ground;

  VectorCopy( point, start );
  start[ 2 ] += NAV_STEP;

  if( !NavGroundAt( start, ground ) )
    return -1;

  return NavAddNode( ground );
}

static void NavSeedAround( gentity_t *ent )
{
  vec3_t  p;
  float   r;
  int     i;

  r = MAX( ent->r.maxs[ 0 ] - ent->r.mins[ 0 ],
           ent->r.maxs[ 1 ] - ent->r.mins[ 1 ] ) * 0.5f + NAV_GRID;

  for( i = 0; i < 8; i++ )
  {
    VectorSet( p, ent->r.currentOrigin[ 0 ] + navDirs[ i ][ 0 ] * r,
                  ent->r.currentOrigin[ 1 ] + navDirs[ i ][ 1 ] * r,
                  ent->r.currentOrigin[ 2 ] + ent->r.maxs[ 2 ] );
    NavSeed( p );
  }
}

// does this trigger fire a target_kill?
static qboolean NavTriggerKills( gentity_t *trigger )
{
  gentity_t *ent;
  int       i;

  if( !trigger->target || !trigger->target[ 0 ] )
    return qfalse;

  for( i = MAX_CLIENTS, ent = g_entities + MAX_CLIENTS; i < level.num_entities; i++, ent++ )
  {
    if( ent->inuse && ent->classname && ent->targetname &&
        !Q_stricmp( ent->classname, "target_kill" ) &&
        !Q_stricmp( ent->targetname, trigger->target ) )
      return qtrue;
  }

  return qfalse;
}

static void NavCollectHurtTriggers( void )
{
  gentity_t *ent;
  int       i;

  numHurt = 0;
  for( i = MAX_CLIENTS, ent = g_entities + MAX_CLIENTS; i < level.num_entities; i++, ent++ )
  {
    if( !ent->inuse || !ent->classname )
      continue;

    if( Q_stricmp( ent->classname, "trigger_hurt" ) &&
        !( ( !Q_stricmp( ent->classname, "trigger_multiple" ) ||
             !Q_stricmp( ent->classname, "trigger_once" ) ) && NavTriggerKills( ent ) ) )
      continue;

    if( numHurt >= NAV_MAX_HURT )
      break;

    VectorCopy( ent->r.absmin, hurtMins[ numHurt ] );
    VectorCopy( ent->r.absmax, hurtMaxs[ numHurt ] );
    numHurt++;
  }
}

void G_BotNavInit( void )
{
  int i;

  numNodes = 0;
  expandCursor = 0;
  navInitialised = qfalse;
  navFullWarned = qfalse;
  numHurt = 0;
  searchStamp = 0;
  nextObserveTime = 0;
  buildMsec = 0;
  buildReported = qfalse;
  numRequeue = 0;
  nextMoverCheckTime = 0;
  memset( moverWasOpen, 0, sizeof( moverWasOpen ) );

  for( i = 0; i < NAV_HASH_SIZE; i++ )
    hashHeads[ i ] = -1;

  for( i = 0; i < MAX_CLIENTS; i++ )
    observedNode[ i ] = -1;

  for( i = 0; i < NAV_MAX_NODES; i++ )
  {
    openStamp[ i ] = 0;
    closedStamp[ i ] = 0;
  }
}

static void NavStart( void )
{
  gentity_t *ent;
  int       i;

  // a slightly slimmer box than a human, so doorways aren't missed
  BG_ClassBoundingBox( PCL_HUMAN, navMins, navMaxs, NULL, NULL, NULL );
  navMins[ 0 ] += 1.0f;
  navMins[ 1 ] += 1.0f;
  navMaxs[ 0 ] -= 1.0f;
  navMaxs[ 1 ] -= 1.0f;

  NavCollectHurtTriggers( );

  for( i = MAX_CLIENTS, ent = g_entities + MAX_CLIENTS; i < level.num_entities; i++, ent++ )
  {
    if( ent->inuse && ent->s.eType == ET_MOVER )
      moverWasOpen[ i ] = NavMoverPassable( ent );
  }

  NavUnlinkMovers( );

  for( i = 0, ent = g_entities; i < level.num_entities; i++, ent++ )
  {
    if( !ent->inuse )
      continue;

    if( ent->s.eType == ET_BUILDABLE )
      NavSeedAround( ent );
    else if( ent->classname &&
             ( !Q_stricmp( ent->classname, "info_player_deathmatch" ) ||
               !Q_stricmp( ent->classname, "info_player_start" ) ) )
      NavSeed( ent->r.currentOrigin );
  }

  NavRelinkMovers( );

  navInitialised = qtrue;
  G_Printf( "Bot navigation: building graph from %d seeds\n", numNodes );
}

/*
===============
G_BotNavObserve

Record where players go so the graph grows into places the seeds didn't
reach; new nodes get expanded like any other
===============
*/
static void NavObserve( gentity_t *ent )
{
  int       clientNum = ent - g_entities;
  gclient_t *client = ent->client;
  int       n, last;
  vec3_t    ground;
  float     dist;

  if( client->pers.connected != CON_CONNECTED ||
      client->sess.spectatorState != SPECTATOR_NOT ||
      client->ps.stats[ STAT_HEALTH ] <= 0 ||
      client->ps.pm_type != PM_NORMAL )
  {
    observedNode[ clientNum ] = -1;
    return;
  }

  if( client->ps.groundEntityNum == ENTITYNUM_NONE ||
      ( client->ps.eFlags & EF_WALLCLIMB ) )
    return;

  n = NavFindNodeInCell( NavCell( client->ps.origin[ 0 ] ),
                         NavCell( client->ps.origin[ 1 ] ),
                         client->ps.origin[ 2 ] );

  if( n < 0 )
  {
    // only remember spots a human sized player could stand on
    VectorCopy( client->ps.origin, ground );
    ground[ 2 ] += NAV_STEP;
    if( !NavGroundAt( ground, ground ) ||
        fabs( ground[ 2 ] - client->ps.origin[ 2 ] ) > NAV_STEP )
      return;

    n = NavAddNode( ground );
    if( n < 0 )
      return;
  }

  last = observedNode[ clientNum ];
  observedNode[ clientNum ] = n;

  if( last < 0 || last == n )
    return;

  dist = Distance( nodes[ last ].origin, nodes[ n ].origin );
  if( dist > NAV_GRID * 3.0f )
    return;

  // whoever we watched may have been knocked about, or an alien that jumps
  // far higher than a human, so only keep moves a human could make
  {
    vec3_t  ground;
    int     flags;

    if( NavTryStep( nodes[ last ].origin, nodes[ n ].origin, ground, &flags ) &&
        fabs( ground[ 2 ] - nodes[ n ].origin[ 2 ] ) < NAV_LEVEL_DIST )
      NavAddLink( last, n, flags );
  }
}

// when a door that something else opens has opened, look for routes through it
static void NavCheckDoors( void )
{
  gentity_t *ent;
  int       i, n;
  qboolean  open;

  for( i = MAX_CLIENTS, ent = g_entities + MAX_CLIENTS; i < level.num_entities; i++, ent++ )
  {
    if( !ent->inuse || ent->s.eType != ET_MOVER )
      continue;

    open = NavMoverPassable( ent );
    if( open && !moverWasOpen[ i ] )
    {
      for( n = 0; n < numNodes && numRequeue < NAV_MAX_REQUEUE; n++ )
      {
        if( nodes[ n ].origin[ 0 ] > ent->r.absmin[ 0 ] - 150.0f &&
            nodes[ n ].origin[ 0 ] < ent->r.absmax[ 0 ] + 150.0f &&
            nodes[ n ].origin[ 1 ] > ent->r.absmin[ 1 ] - 150.0f &&
            nodes[ n ].origin[ 1 ] < ent->r.absmax[ 1 ] + 150.0f &&
            nodes[ n ].origin[ 2 ] > ent->r.absmin[ 2 ] - 150.0f &&
            nodes[ n ].origin[ 2 ] < ent->r.absmax[ 2 ] + 150.0f )
          requeue[ numRequeue++ ] = n;
      }
    }

    moverWasOpen[ i ] = open;
  }
}

void G_BotNavFrame( int budgetMsec )
{
  int        start, i;

  if( !navInitialised )
  {
    // buildables placed by the map finish spawning a moment after the level starts
    if( level.time - level.startTime < 1000 )
      return;

    NavStart( );
  }

  if( level.time >= nextObserveTime )
  {
    nextObserveTime = level.time + 200;
    for( i = 0; i < level.maxclients; i++ )
    {
      if( g_entities[ i ].inuse && g_entities[ i ].client )
        NavObserve( &g_entities[ i ] );
    }
  }

  if( level.time >= nextMoverCheckTime )
  {
    nextMoverCheckTime = level.time + 1000;
    NavCheckDoors( );
  }

  if( expandCursor >= numNodes && !numRequeue )
    return;

  start = trap_Milliseconds( );
  NavUnlinkMovers( );
  while( numRequeue > 0 && trap_Milliseconds( ) - start < budgetMsec )
    NavExpandNode( requeue[ --numRequeue ] );
  while( expandCursor < numNodes &&
         trap_Milliseconds( ) - start < budgetMsec )
    NavExpandNode( expandCursor++ );
  NavRelinkMovers( );
  buildMsec += trap_Milliseconds( ) - start;

  if( expandCursor >= numNodes && !buildReported )
  {
    // nodes learnt from players later on are expanded quietly
    G_Printf( "Bot navigation: graph complete, %d nodes in %d msec\n",
              numNodes, buildMsec );
    buildReported = qtrue;
  }
}

/*
===============
G_BotNavDump

Write the graph to nav/<map>.txt for debugging: one line per node with
its origin followed by its links ("target:flags")
===============
*/
void G_BotNavDump( void )
{
  fileHandle_t  f;
  char          map[ MAX_QPATH ];
  char          line[ 512 ];
  int           n, i, len;

  trap_Cvar_VariableStringBuffer( "mapname", map, sizeof( map ) );
  if( trap_FS_FOpenFile( va( "nav/%s.txt", map ), &f, FS_WRITE ) < 0 )
  {
    G_Printf( "navdump: couldn't open nav/%s.txt\n", map );
    return;
  }

  for( n = 0; n < numNodes; n++ )
  {
    len = Com_sprintf( line, sizeof( line ), "%d %.0f %.0f %.0f", n,
                       nodes[ n ].origin[ 0 ], nodes[ n ].origin[ 1 ], nodes[ n ].origin[ 2 ] );
    for( i = 0; i < nodes[ n ].numLinks && len < sizeof( line ) - 16; i++ )
      len += Com_sprintf( line + len, sizeof( line ) - len, " %d:%d",
                          nodes[ n ].links[ i ], nodes[ n ].linkFlags[ i ] );
    Q_strcat( line, sizeof( line ), "\n" );
    trap_FS_Write( line, strlen( line ), f );
  }

  trap_FS_FCloseFile( f );
  G_Printf( "navdump: wrote %d nodes to nav/%s.txt\n", numNodes, map );
}

/*
===============
G_BotNavTrace

navtrace x1 y1 z1 x2 y2 z2: trace a player box between two points with the
player and navigation masks and report what's hit, for debugging
===============
*/
void G_BotNavTrace( void )
{
  char    arg[ MAX_TOKEN_CHARS ];
  vec3_t  start, end, mins, maxs;
  trace_t tr;
  int     i, pass;

  if( trap_Argc( ) < 7 )
  {
    G_Printf( "usage: navtrace x1 y1 z1 x2 y2 z2\n" );
    return;
  }

  for( i = 0; i < 3; i++ )
  {
    trap_Argv( 1 + i, arg, sizeof( arg ) );
    start[ i ] = atof( arg );
    trap_Argv( 4 + i, arg, sizeof( arg ) );
    end[ i ] = atof( arg );
  }

  BG_ClassBoundingBox( PCL_HUMAN, mins, maxs, NULL, NULL, NULL );

  for( pass = 0; pass < 2; pass++ )
  {
    trap_Trace( &tr, start, mins, maxs, end, ENTITYNUM_NONE,
                pass ? NAV_MASK : MASK_PLAYERSOLID );
    G_Printf( "%s: fraction %.2f startsolid %d end (%.0f %.0f %.0f) normal (%.2f %.2f %.2f) "
              "entity %d %s contents 0x%x surface 0x%x\n",
              pass ? "nav mask" : "player mask", tr.fraction, tr.startsolid,
              tr.endpos[ 0 ], tr.endpos[ 1 ], tr.endpos[ 2 ],
              tr.plane.normal[ 0 ], tr.plane.normal[ 1 ], tr.plane.normal[ 2 ],
              tr.entityNum, tr.entityNum < ENTITYNUM_MAX_NORMAL ?
              ( g_entities[ tr.entityNum ].classname ? g_entities[ tr.entityNum ].classname : "?" ) :
              "world", tr.contents, tr.surfaceFlags );
  }
}

/*
===============
Queries
===============
*/
int G_BotNavNumNodes( void )
{
  return numNodes;
}

void G_BotNavNodeOrigin( int node, vec3_t out )
{
  VectorCopy( nodes[ node ].origin, out );
}

// pick a random node between minDist and maxDist from origin, on roughly the same level
qboolean G_BotNavRandomNodeNear( const vec3_t origin, float minDist, float maxDist, vec3_t out )
{
  int   n, count = 0;
  float d;

  for( n = 0; n < numNodes; n++ )
  {
    if( fabs( nodes[ n ].origin[ 2 ] - origin[ 2 ] ) > 96.0f )
      continue;

    d = Distance( nodes[ n ].origin, origin );
    if( d < minDist || d > maxDist )
      continue;

    // reservoir sampling, every candidate is equally likely
    if( rand( ) % ++count == 0 )
      VectorCopy( nodes[ n ].origin, out );
  }

  return count > 0;
}

// forget a link bots keep getting stuck on
void G_BotNavRemoveLink( int from, int to )
{
  navNode_t *node;
  int       i;

  if( from < 0 || to < 0 || from >= numNodes || to >= numNodes )
    return;

  node = &nodes[ from ];
  for( i = 0; i < node->numLinks; i++ )
  {
    if( node->links[ i ] == to )
    {
      node->numLinks--;
      node->links[ i ] = node->links[ node->numLinks ];
      node->linkFlags[ i ] = node->linkFlags[ node->numLinks ];
      return;
    }
  }
}

int G_BotNavLinkFlags( int from, int to )
{
  int i;

  if( from < 0 || to < 0 )
    return 0;

  for( i = 0; i < nodes[ from ].numLinks; i++ )
  {
    if( nodes[ from ].links[ i ] == to )
      return nodes[ from ].linkFlags[ i ];
  }

  return 0;
}

static qboolean NavVisible( const vec3_t a, const vec3_t b )
{
  trace_t tr;

  trap_Trace( &tr, a, NULL, NULL, b, ENTITYNUM_NONE, NAV_MASK );
  return tr.fraction >= 1.0f;
}

int G_BotNavNearestNode( const vec3_t origin )
{
  int     cx = NavCell( origin[ 0 ] );
  int     cy = NavCell( origin[ 1 ] );
  int     x, y, n, r, tries;
  int     best;
  float   d, bestDist;
  int     candidates[ 4 ];
  float   candidateDist[ 4 ];
  int     numCandidates, i, j;

  for( r = 1; r <= 4; r += 3 )
  {
    best = -1;
    bestDist = 0.0f;
    numCandidates = 0;

    for( x = cx - r; x <= cx + r; x++ )
    {
      for( y = cy - r; y <= cy + r; y++ )
      {
        for( n = hashHeads[ NavHash( x, y ) ]; n >= 0; n = nodes[ n ].hashNext )
        {
          if( nodes[ n ].cellX != x || nodes[ n ].cellY != y )
            continue;

          if( fabs( nodes[ n ].origin[ 2 ] - origin[ 2 ] ) > 128.0f )
            continue;

          d = DistanceSquared( nodes[ n ].origin, origin );

          if( best < 0 || d < bestDist )
          {
            best = n;
            bestDist = d;
          }

          // keep the closest few, sorted, to check for visibility
          for( i = 0; i < numCandidates; i++ )
          {
            if( d < candidateDist[ i ] )
              break;
          }

          if( i < 4 )
          {
            for( j = MIN( numCandidates, 3 ); j > i; j-- )
            {
              candidates[ j ] = candidates[ j - 1 ];
              candidateDist[ j ] = candidateDist[ j - 1 ];
            }
            candidates[ i ] = n;
            candidateDist[ i ] = d;
            if( numCandidates < 4 )
              numCandidates++;
          }
        }
      }
    }

    for( tries = 0; tries < numCandidates; tries++ )
    {
      if( NavVisible( origin, nodes[ candidates[ tries ] ].origin ) )
        return candidates[ tries ];
    }

    if( best >= 0 && r > 1 )
      return best;
  }

  return -1;
}

/*
===============
A*
===============
*/
static void HeapPush( int node, float f )
{
  int i, parent;
  navHeapEntry_t tmp;

  if( heapSize >= NAV_HEAP_SIZE )
    return;

  i = heapSize++;
  heap[ i ].node = node;
  heap[ i ].f = f;

  while( i > 0 )
  {
    parent = ( i - 1 ) / 2;
    if( heap[ parent ].f <= heap[ i ].f )
      break;

    tmp = heap[ parent ];
    heap[ parent ] = heap[ i ];
    heap[ i ] = tmp;
    i = parent;
  }
}

static int HeapPop( void )
{
  int node, i, child;
  navHeapEntry_t tmp;

  node = heap[ 0 ].node;
  heap[ 0 ] = heap[ --heapSize ];

  i = 0;
  for( ;; )
  {
    child = i * 2 + 1;
    if( child >= heapSize )
      break;

    if( child + 1 < heapSize && heap[ child + 1 ].f < heap[ child ].f )
      child++;

    if( heap[ i ].f <= heap[ child ].f )
      break;

    tmp = heap[ child ];
    heap[ child ] = heap[ i ];
    heap[ i ] = tmp;
    i = child;
  }

  return node;
}

/*
===============
G_BotNavFindPath

Fills path with the nodes to visit from start towards goal and returns the
number of nodes. If the goal can't be reached, the path leads to the
closest reachable node instead. Returns 0 if there is no usable path.
===============
*/
int G_BotNavFindPath( const vec3_t start, const vec3_t goal, int *path, int maxPath )
{
  int     startNode, goalNode;
  int     n, m, i, expanded;
  int     closest;
  float   closestDist, h, g;
  int     length;

  if( !navInitialised || numNodes == 0 )
    return 0;

  startNode = G_BotNavNearestNode( start );
  goalNode = G_BotNavNearestNode( goal );
  if( startNode < 0 || goalNode < 0 )
    return 0;

  searchStamp++;
  heapSize = 0;

  gScore[ startNode ] = 0.0f;
  cameFrom[ startNode ] = -1;
  openStamp[ startNode ] = searchStamp;
  HeapPush( startNode, Distance( nodes[ startNode ].origin, nodes[ goalNode ].origin ) );

  closest = startNode;
  closestDist = Distance( nodes[ startNode ].origin, nodes[ goalNode ].origin );
  expanded = 0;

  while( heapSize > 0 && expanded < NAV_SEARCH_LIMIT )
  {
    n = HeapPop( );

    if( closedStamp[ n ] == searchStamp )
      continue;
    closedStamp[ n ] = searchStamp;
    expanded++;

    if( n == goalNode )
    {
      closest = n;
      break;
    }

    h = Distance( nodes[ n ].origin, nodes[ goalNode ].origin );
    if( h < closestDist )
    {
      closest = n;
      closestDist = h;
    }

    for( i = 0; i < nodes[ n ].numLinks; i++ )
    {
      m = nodes[ n ].links[ i ];
      if( closedStamp[ m ] == searchStamp )
        continue;

      g = gScore[ n ] + Distance( nodes[ n ].origin, nodes[ m ].origin );
      if( nodes[ n ].linkFlags[ i ] & NAVLINK_JUMP )
        g += NAV_GRID;

      if( openStamp[ m ] == searchStamp && g >= gScore[ m ] )
        continue;

      openStamp[ m ] = searchStamp;
      gScore[ m ] = g;
      cameFrom[ m ] = n;
      HeapPush( m, g + Distance( nodes[ m ].origin, nodes[ goalNode ].origin ) );
    }
  }

  // walk back from the best node we found
  length = 0;
  for( n = closest; n >= 0 && length < NAV_MAX_NODES; n = cameFrom[ n ] )
    reversePath[ length++ ] = n;

  for( i = 0; i < length && i < maxPath; i++ )
    path[ i ] = reversePath[ length - 1 - i ];

  return i;
}
