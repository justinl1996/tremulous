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

The graph is built with a crouching human, so it reaches into vents and
under low ceilings; links a standing human doesn't fit through are marked
for crouching. Aliens that walk on walls also get links up walls onto
ledges they can't otherwise reach.

Paths are found with A* over the graph. Besides distance, a route costs
more for going through enemy structures, past enemy defences and where
the team keeps dying, and each bot can ask for some noise so they don't
all take the same way.
===========================================================================
*/

#include "g_local.h"

#define NAV_MAX_NODES       16384
#define NAV_MAX_LINKS       12
#define NAV_GRID            48.0f
#define NAV_HASH_SIZE       4096
#define NAV_STEP            18.0f   // STEPSIZE in bg_pmove.c
#define NAV_JUMP_MIN        24.0f   // jumps are tried from this high...
#define NAV_JUMP_MAX        64.0f   // ...up to this, and each link records the lowest that works
#define NAV_JUMP_SHIFT      16      // NavTryStep returns the jump height above the flags
#define NAV_CLIMB_MIN       40.0f   // walls lower than this aren't worth climbing
#define NAV_MAX_DROP        256.0f
#define NAV_WALK_NORMAL     0.7f    // MIN_WALK_NORMAL in bg_local.h
#define NAV_LEVEL_DIST      30.0f   // nodes in one cell closer than this vertically are the same node
#define NAV_MASK            ( CONTENTS_SOLID | CONTENTS_PLAYERCLIP | CONTENTS_BOTCLIP )
#define NAV_BAD_CONTENTS    ( CONTENTS_LAVA | CONTENTS_SLIME | CONTENTS_NODROP )
#define NAV_MAX_HURT        64
#define NAV_MAX_REQUEUE     2048
#define NAV_HEAP_SIZE       ( NAV_MAX_NODES * 4 )
#define NAV_SEARCH_LIMIT    10000
#define NAV_CLIMB_MAX       384.0f  // tallest wall climbers are sent up
#define NAV_MAX_HOP         64.0f   // nothing jumps onto anything taller
#define NAV_ENEMY_COST      400.0f  // extra route length for an enemy structure in the way
#define NAV_NUM_REGIONS     1024
#define NAV_REGION_CELLS    5       // regions are this many cells across

// structures are kept per team: aliens are 0, humans 1
#define NAV_TEAM( t )       ( ( t ) == TEAM_HUMANS ? 1 : 0 )

typedef struct
{
  vec3_t  origin;
  int     cellX, cellY;
  short   links[ NAV_MAX_LINKS ];
  byte    linkFlags[ NAV_MAX_LINKS ];
  byte    linkFails[ NAV_MAX_LINKS ];   // times bots got stuck trying it
  byte    linkJump[ NAV_MAX_LINKS ];    // how high a jump the link needs
  byte    linkFit[ NAV_MAX_LINKS ][ 2 ];  // size classes that get past each team's structures
  byte    linkHop[ NAV_MAX_LINKS ][ 2 ];  // how high to jump to get over them, 255 if no use
  short   region;
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

static float          navStandTop;   // a standing human, the graph is built crouching
static byte           nodeClear[ NAV_MAX_NODES ];   // NavNodeClearance, 0xff until worked out

// structures go up and come down during a game, so rather than build them
// into the graph, nodes and links they're in the way of are marked blocked
static byte           nodeBlocked[ NAV_MAX_NODES ];  // 1 << team whose structure is there
static byte           nodeThreat[ NAV_MAX_NODES ][ 2 ];  // enemy defences covering it

// what each team has learnt about the map: where they've died lately and
// when they were last in each area
static float          regionDanger[ 2 ][ NAV_NUM_REGIONS ];
static int            regionVisited[ 2 ][ NAV_NUM_REGIONS ];
static int            nextDangerDecay;

// which parts of the graph connect to each other, by walking and by
// walking and climbing
static int            nodeComponent[ 2 ][ NAV_MAX_NODES ];
static qboolean       navComponentsDirty;
static int            nextComponentTime;

// nodes in a teleporter or jump pad can only be left through it
static byte           nodePortal[ NAV_MAX_NODES ];
static int            portalNodes;    // numNodes when portals were last linked
static int            nextPortalTime;
static int            obstacleSignature;
static qboolean       navObstaclesDirty;
static int            nextObstacleTime;
static int            navVersion;

// scratch space for G_BotNavWouldBlock
static int            footStamp[ NAV_MAX_NODES ];
static int            visitStamp[ NAV_MAX_NODES ];
static int            wouldBlockStamp;
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

// areas a few cells across, for remembering things about parts of the map
static int NavRegionOf( int x, int y, float z )
{
  int rx = (int)floor( x / (float)NAV_REGION_CELLS );
  int ry = (int)floor( y / (float)NAV_REGION_CELLS );
  int rz = (int)floor( z / 192.0f );

  return ( ( rx * 73856093 ) ^ ( ry * 19349663 ) ^ ( rz * 83492791 ) ) & ( NAV_NUM_REGIONS - 1 );
}

int G_BotNavRegion( const vec3_t origin )
{
  return NavRegionOf( NavCell( origin[ 0 ] ), NavCell( origin[ 1 ] ), origin[ 2 ] );
}

// a fixed random number for each area and seed
static float NavNoise( int region, int seed )
{
  unsigned int h = (unsigned int)region * 2654435761u ^ (unsigned int)seed * 40503u;

  h ^= h >> 13;
  h *= 0x5bd1e995u;
  h ^= h >> 15;
  return ( h & 0xffff ) / 65535.0f;
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
  node->region = NavRegionOf( x, y, origin[ 2 ] );

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
  node->linkFlags[ node->numLinks ] = flags & 0xff;
  node->linkJump[ node->numLinks ] = flags >> NAV_JUMP_SHIFT;
  node->linkFails[ node->numLinks ] = 0;
  node->linkFit[ node->numLinks ][ 0 ] = node->linkFit[ node->numLinks ][ 1 ] = NAV_NUM_SIZES;
  node->linkHop[ node->numLinks ][ 0 ] = node->linkHop[ node->numLinks ][ 1 ] = 0;
  node->numLinks++;

  // structures need checking against it
  navObstaclesDirty = qtrue;
  navComponentsDirty = qtrue;
}

static qboolean NavHasLink( int from, int to )
{
  int i;

  for( i = 0; i < nodes[ from ].numLinks; i++ )
  {
    if( nodes[ from ].links[ i ] == to )
      return qtrue;
  }

  return qfalse;
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
static byte     moverLastState[ MAX_GENTITIES ];

// nodes whose floor has gone, a trapdoor that dropped away
static byte     nodeDead[ NAV_MAX_NODES ];
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

  // too high to step over, even if we land at the same height (a rail). Not
  // every class jumps as high, so find the lowest jump that does it
  if( NavTryWalk( from, to, NAV_JUMP_MAX, ground ) || NavTryWalk( from, to, 40.0f, ground ) )
  {
    float h;

    for( h = NAV_JUMP_MIN; h <= NAV_JUMP_MAX; h += 8.0f )
    {
      if( NavTryWalk( from, to, h, ground ) )
      {
        *flags = NAVLINK_JUMP | ( (int)h << NAV_JUMP_SHIFT );
        return qtrue;
      }
    }
  }

  return qfalse;
}

// make the same move a link was found with (walk, stairs or a jump of the
// same height) with whatever box is set, rather than search them all again
static qboolean NavTryLike( const vec3_t from, const vec3_t to, int flags, vec3_t ground )
{
  vec3_t mid, midGround;

  if( flags & NAVLINK_JUMP )
    return NavTryWalk( from, to, flags >> NAV_JUMP_SHIFT, ground );

  if( NavTryWalk( from, to, NAV_STEP, ground ) )
    return qtrue;

  VectorAdd( from, to, mid );
  VectorScale( mid, 0.5f, mid );
  return NavTryWalk( from, mid, NAV_STEP, midGround ) &&
         NavTryWalk( midGround, to, NAV_STEP, ground );
}

// bigger aliens don't fit everywhere a human does. The largest box that can
// make the same move is recorded on each link. Through low gaps each size is
// as tall as the shortest class in it (a crouching human for the smallest)
static const float navSizeWidth[ NAV_NUM_SIZES ] = { 15.0f, 18.0f, 20.0f, 22.0f, 26.0f, 29.0f, 32.0f };
static const float navSizeTop[ NAV_NUM_SIZES ] = { 32.0f, 32.0f, 32.0f, 32.0f, 32.0f, 52.0f, 68.0f };
static const float navSizeLowTop[ NAV_NUM_SIZES ] = { 16.0f, 12.0f, 16.0f, 18.0f, 31.0f, 52.0f, 68.0f };

int G_BotNavSizeClass( const vec3_t mins, const vec3_t maxs )
{
  float width = MAX( maxs[ 0 ], -mins[ 0 ] );
  float height = maxs[ 2 ] - mins[ 2 ];
  int   size;

  for( size = 0; size < NAV_NUM_SIZES - 1; size++ )
  {
    if( width <= navSizeWidth[ size ] && height <= navSizeTop[ size ] + 24.0f )
      break;
  }

  return size;
}

// nodes are where a human fits, which can be closer to a wall than something
// bigger can get. Move a point out from the wall as far as a bigger player
// would stay off it (with navMins/navMaxs set to its size)
static qboolean NavNudge( vec3_t point, const vec3_t humanMins, const vec3_t humanMaxs, float shift )
{
  static const float  dirs[ 8 ][ 2 ] =
  {
    { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 },
    { 0.7071f, 0.7071f }, { 0.7071f, -0.7071f }, { -0.7071f, 0.7071f }, { -0.7071f, -0.7071f }
  };
  trace_t tr;
  vec3_t  p, q;
  int     i;

  VectorCopy( point, p );
  p[ 2 ] += NAV_STEP;
  trap_Trace( &tr, p, navMins, navMaxs, p, ENTITYNUM_NONE, NAV_MASK );
  if( !tr.startsolid && !tr.allsolid )
    return qtrue;

  for( i = 0; i < 8; i++ )
  {
    VectorSet( q, p[ 0 ] + dirs[ i ][ 0 ] * shift, p[ 1 ] + dirs[ i ][ 1 ] * shift, p[ 2 ] );

    // somewhere a human can walk to from the node, not through a wall
    trap_Trace( &tr, p, humanMins, humanMaxs, q, ENTITYNUM_NONE, NAV_MASK );
    if( tr.startsolid || tr.fraction < 1.0f )
      continue;

    trap_Trace( &tr, q, navMins, navMaxs, q, ENTITYNUM_NONE, NAV_MASK );
    if( tr.startsolid || tr.allsolid )
      continue;

    point[ 0 ] = q[ 0 ];
    point[ 1 ] = q[ 1 ];
    return qtrue;
  }

  return qfalse;
}

// stepping physics with a wide box finds stairs harder than they are, so
// failing that, does it fit along the way a human goes, a step up from it?
static qboolean NavSweepFits( const vec3_t from, const vec3_t to, float landZ, float lift,
                              const vec3_t humanMins, const vec3_t humanMaxs, float shift )
{
  trace_t tr;
  vec3_t  points[ 3 ];
  int     i;

  // bots jump anything more than a step up, so clear the whole rise
  lift = MAX( lift, landZ - from[ 2 ] + 8.0f );

  VectorCopy( from, points[ 0 ] );
  VectorSet( points[ 2 ], to[ 0 ], to[ 1 ], landZ );
  VectorAdd( points[ 0 ], points[ 2 ], points[ 1 ] );
  VectorScale( points[ 1 ], 0.5f, points[ 1 ] );

  for( i = 0; i < 3; i++ )
  {
    if( !NavNudge( points[ i ], humanMins, humanMaxs, shift ) )
      return qfalse;
    points[ i ][ 2 ] += lift;
  }

  for( i = 0; i < 2; i++ )
  {
    trap_Trace( &tr, points[ i ], navMins, navMaxs, points[ i + 1 ], ENTITYNUM_NONE, NAV_MASK );
    if( tr.startsolid || tr.allsolid || tr.fraction < 1.0f )
      return qfalse;
  }

  return qtrue;
}

// the biggest size that fits anywhere near a node, as the most any link
// from it can take. Worked out once, it saves testing every size on each link
static int NavNodeClearance( int n )
{
  vec3_t  savedMins, savedMaxs, p;
  int     size;

  if( nodeClear[ n ] != 0xff )
    return nodeClear[ n ];

  VectorCopy( navMins, savedMins );
  VectorCopy( navMaxs, savedMaxs );

  for( size = NAV_NUM_SIZES - 1; size > 0; size-- )
  {
    VectorSet( navMins, -navSizeWidth[ size ] + 1.0f, -navSizeWidth[ size ] + 1.0f, savedMins[ 2 ] );
    VectorSet( navMaxs, navSizeWidth[ size ] - 1.0f, navSizeWidth[ size ] - 1.0f, navSizeLowTop[ size ] );
    VectorCopy( nodes[ n ].origin, p );

    if( NavNudge( p, savedMins, savedMaxs, navSizeWidth[ size ] - navSizeWidth[ 0 ] + 1.0f ) )
      break;
  }

  VectorCopy( savedMins, navMins );
  VectorCopy( savedMaxs, navMaxs );
  nodeClear[ n ] = size;
  return size;
}

static int NavLinkSize( const vec3_t from, const vec3_t to, float landZ, qboolean low, int flags,
                        int maxSize )
{
  vec3_t  savedMins, savedMaxs, ground, a, b;
  float   shift, lift = ( flags & NAVLINK_JUMP ) ? ( flags >> NAV_JUMP_SHIFT ) : NAV_STEP;
  int     size;

  VectorCopy( navMins, savedMins );
  VectorCopy( navMaxs, savedMaxs );

  // most of a map is open, so try the biggest first
  for( size = maxSize; size > 0; size-- )
  {
    VectorSet( navMins, -navSizeWidth[ size ] + 1.0f, -navSizeWidth[ size ] + 1.0f, savedMins[ 2 ] );
    VectorSet( navMaxs, navSizeWidth[ size ] - 1.0f, navSizeWidth[ size ] - 1.0f,
               low ? navSizeLowTop[ size ] : navSizeTop[ size ] );

    VectorCopy( from, a );
    VectorCopy( to, b );
    shift = navSizeWidth[ size ] - navSizeWidth[ 0 ] + 1.0f;
    // room along the way a human goes is usually enough, and cheaper to
    // check than stepping the bigger box through it. On stairs a wider box
    // rests on a higher step than a human does
    if( NavSweepFits( from, to, landZ, lift, savedMins, savedMaxs, shift ) ||
        ( NavNudge( a, savedMins, savedMaxs, shift ) && NavNudge( b, savedMins, savedMaxs, shift ) &&
          NavTryLike( a, b, flags, ground ) &&
          fabs( ground[ 2 ] - landZ ) < NAV_LEVEL_DIST + NAV_STEP ) )
      break;
  }

  VectorCopy( savedMins, navMins );
  VectorCopy( savedMaxs, navMaxs );
  return size;
}

// can a standing human make the move the graph found crouching?
static qboolean NavStandingFits( const vec3_t from, const vec3_t to, float landZ, int flags )
{
  vec3_t    ground;
  float     saved = navMaxs[ 2 ];
  qboolean  fits;

  navMaxs[ 2 ] = navStandTop;
  fits = NavTryLike( from, to, flags, ground ) && fabs( ground[ 2 ] - landZ ) < NAV_LEVEL_DIST;
  navMaxs[ 2 ] = saved;

  return fits;
}

// link two nodes a player can walk, step or jump between
static void NavAddWalkLink( int n, int m, const vec3_t to, float landZ, int flags )
{
  qboolean low;

  if( m < 0 || NavHasLink( n, m ) )
    return;

  // only worth testing sizes for a link we don't have yet
  low = !NavStandingFits( nodes[ n ].origin, to, landZ, flags );
  if( low )
    flags |= NAVLINK_CROUCH;

  flags |= NavLinkSize( nodes[ n ].origin, to, landZ, low, flags,
                       MIN( NavNodeClearance( n ), NavNodeClearance( m ) ) ) << NAVLINK_SIZE_SHIFT;
  NavAddLink( n, m, flags );
}

// does a bigger climber fit up the same wall?
static qboolean NavClimbFits( const vec3_t start, const vec3_t dir, float height )
{
  trace_t tr;
  vec3_t  mins, maxs, a, b, c;

  VectorSet( mins, -navSizeWidth[ 3 ] + 1.0f, -navSizeWidth[ 3 ] + 1.0f, navMins[ 2 ] );
  VectorSet( maxs, navSizeWidth[ 3 ] - 1.0f, navSizeWidth[ 3 ] - 1.0f, navSizeLowTop[ 3 ] );

  // it can't get as close to the wall
  VectorMA( start, navMins[ 0 ] - mins[ 0 ], dir, a );
  VectorCopy( a, b );
  b[ 2 ] += height;
  VectorMA( b, NAV_GRID, dir, c );

  trap_Trace( &tr, a, mins, maxs, b, ENTITYNUM_NONE, NAV_MASK );
  if( tr.startsolid || tr.allsolid || tr.fraction < 1.0f )
    return qfalse;

  trap_Trace( &tr, b, mins, maxs, c, ENTITYNUM_NONE, NAV_MASK );
  return !tr.startsolid && !tr.allsolid && tr.fraction >= 1.0f;
}

// aliens that walk on walls can go straight up one onto a ledge
static void NavTryClimb( int n, const vec3_t to )
{
  trace_t tr;
  vec3_t  from, dir, start, p, over, ground;
  float   h, height;
  int     m;

  VectorCopy( nodes[ n ].origin, from );
  VectorSubtract( to, from, dir );
  dir[ 2 ] = 0.0f;
  VectorNormalize( dir );

  // a wall right in front that can be walked on. Wall walking only takes
  // to the world
  VectorMA( from, NAV_GRID, dir, p );
  trap_Trace( &tr, from, navMins, navMaxs, p, ENTITYNUM_NONE, NAV_MASK );
  if( tr.startsolid || tr.allsolid || tr.fraction >= 0.5f || tr.entityNum != ENTITYNUM_WORLD ||
      tr.plane.normal[ 2 ] >= NAV_WALK_NORMAL || DotProduct( tr.plane.normal, dir ) > -0.7f ||
      ( tr.surfaceFlags & ( SURF_SKY | SURF_SLICK ) ) )
    return;

  VectorCopy( tr.endpos, start );

  // how far up it goes before there's a ceiling
  VectorCopy( start, p );
  p[ 2 ] += NAV_CLIMB_MAX;
  trap_Trace( &tr, start, navMins, navMaxs, p, ENTITYNUM_NONE, NAV_MASK );
  if( tr.startsolid || tr.allsolid )
    return;
  height = tr.fraction * NAV_CLIMB_MAX;

  // the first height we can get over it
  for( h = NAV_CLIMB_MIN + 8.0f; h <= height; h += 24.0f )
  {
    VectorCopy( start, p );
    p[ 2 ] += h;
    VectorMA( p, NAV_GRID, dir, over );
    if( !NavTraceMove( p, over ) )
      continue;

    // a ledge to stand on rather than a fence to drop over, and one we
    // couldn't just jump onto
    if( !NavGroundAt( over, ground ) || ground[ 2 ] < from[ 2 ] + NAV_CLIMB_MIN ||
        ground[ 2 ] > p[ 2 ] + 1.0f )
      return;

    m = NavFindNodeInCell( NavCell( ground[ 0 ] ), NavCell( ground[ 1 ] ), ground[ 2 ] );
    if( m < 0 )
      m = NavAddNode( ground );
    if( m < 0 || NavHasLink( n, m ) )
      return;

    NavAddLink( n, m, NAVLINK_CLIMB |
                ( ( NavClimbFits( start, dir, h ) ? 3 : 0 ) << NAVLINK_SIZE_SHIFT ) );
    return;
  }
}

static void NavExpandNode( int n )
{
  int     i, m, flags;
  vec3_t  to, ground;
  int     x, y;

  if( nodeDead[ n ] )
    return;

  for( i = 0; i < 8; i++ )
  {
    // the node array may not move, but read through the index anyway
    x = nodes[ n ].cellX + navDirs[ i ][ 0 ];
    y = nodes[ n ].cellY + navDirs[ i ][ 1 ];
    VectorSet( to, x * NAV_GRID, y * NAV_GRID, nodes[ n ].origin[ 2 ] );

    if( !NavTryStep( nodes[ n ].origin, to, ground, &flags ) )
    {
      // straight at a wall, see if it can be climbed
      if( i < 4 )
        NavTryClimb( n, to );
      continue;
    }

    m = NavFindNodeInCell( x, y, ground[ 2 ] );
    if( m < 0 )
      m = NavAddNode( ground );

    NavAddWalkLink( n, m, to, ground[ 2 ], flags );
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
  wouldBlockStamp = 0;
  memset( footStamp, 0, sizeof( footStamp ) );
  memset( visitStamp, 0, sizeof( visitStamp ) );
  obstacleSignature = 0;
  nextObstacleTime = 0;
  navVersion = 0;
  navObstaclesDirty = qfalse;
  memset( nodeBlocked, 0, sizeof( nodeBlocked ) );
  memset( nodeThreat, 0, sizeof( nodeThreat ) );
  memset( regionDanger, 0, sizeof( regionDanger ) );
  memset( regionVisited, 0, sizeof( regionVisited ) );
  nextDangerDecay = 0;
  navComponentsDirty = qfalse;
  nextComponentTime = 0;
  memset( nodeComponent, 0, sizeof( nodeComponent ) );
  memset( nodePortal, 0, sizeof( nodePortal ) );
  memset( nodeClear, 0xff, sizeof( nodeClear ) );
  portalNodes = 0;
  nextPortalTime = 0;
  memset( moverWasOpen, 0, sizeof( moverWasOpen ) );
  memset( moverLastState, 0, sizeof( moverLastState ) );
  memset( nodeDead, 0, sizeof( nodeDead ) );

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
  vec3_t    cmaxs;
  int       i;

  // a slightly slimmer box than a human, so doorways aren't missed, and
  // crouching so low gaps are found too
  BG_ClassBoundingBox( PCL_HUMAN, navMins, navMaxs, cmaxs, NULL, NULL );
  navMins[ 0 ] += 1.0f;
  navMins[ 1 ] += 1.0f;
  navMaxs[ 0 ] -= 1.0f;
  navMaxs[ 1 ] -= 1.0f;
  navStandTop = navMaxs[ 2 ];
  navMaxs[ 2 ] = cmaxs[ 2 ];

  NavCollectHurtTriggers( );

  for( i = MAX_CLIENTS, ent = g_entities + MAX_CLIENTS; i < level.num_entities; i++, ent++ )
  {
    if( ent->inuse && ent->s.eType == ET_MOVER )
    {
      moverWasOpen[ i ] = NavMoverPassable( ent );
      moverLastState[ i ] = ent->moverState;
    }
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

  if( client->pers.teamSelection == TEAM_ALIENS || client->pers.teamSelection == TEAM_HUMANS )
    regionVisited[ NAV_TEAM( client->pers.teamSelection ) ][ G_BotNavRegion( client->ps.origin ) ] =
      level.time;

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

    if( !NavHasLink( last, n ) &&
        NavTryStep( nodes[ last ].origin, nodes[ n ].origin, ground, &flags ) &&
        fabs( ground[ 2 ] - nodes[ n ].origin[ 2 ] ) < NAV_LEVEL_DIST )
      NavAddWalkLink( last, n, nodes[ n ].origin, nodes[ n ].origin[ 2 ], flags );
  }
}

// when a door that something else opens has opened, look for routes through it
// a door has moved: anything that stood on it, check there's still a floor.
// Some maps drop floors away from under players at a stage
static void NavCheckFloors( gentity_t *ent )
{
  vec3_t  mins, maxs, start, ground;
  int     n, i;

  // where it was and where it is, the door's origin is pos1 when closed
  for( i = 0; i < 3; i++ )
  {
    mins[ i ] = MIN( ent->r.absmin[ i ], ent->r.mins[ i ] + ent->pos1[ i ] ) - 32.0f;
    maxs[ i ] = MAX( ent->r.absmax[ i ], ent->r.maxs[ i ] + ent->pos1[ i ] ) + 32.0f;
  }
  maxs[ 2 ] += -navMins[ 2 ] + NAV_STEP;

  for( n = 0; n < numNodes; n++ )
  {
    float *o = nodes[ n ].origin;

    if( o[ 0 ] < mins[ 0 ] || o[ 0 ] > maxs[ 0 ] || o[ 1 ] < mins[ 1 ] || o[ 1 ] > maxs[ 1 ] ||
        o[ 2 ] < mins[ 2 ] || o[ 2 ] > maxs[ 2 ] )
      continue;

    VectorCopy( o, start );
    start[ 2 ] += NAV_STEP;
    nodeDead[ n ] = !NavGroundAt( start, ground ) || ground[ 2 ] < o[ 2 ] - NAV_LEVEL_DIST;
  }

  if( bot_debug.integer )
  {
    int dead = 0;

    for( n = 0; n < numNodes; n++ )
      dead += nodeDead[ n ];
    G_Printf( "navfloors: %s %s moved to state %d, %d nodes without a floor\n", ent->classname,
              ent->targetname, ent->moverState, dead );
  }

  // routes through them need finding again
  navVersion++;
}

static void NavCheckDoors( void )
{
  gentity_t *ent;
  int       i, n;
  qboolean  open;

  for( i = MAX_CLIENTS, ent = g_entities + MAX_CLIENTS; i < level.num_entities; i++, ent++ )
  {
    if( !ent->inuse || ent->s.eType != ET_MOVER )
      continue;

    // only doors something else opens are part of the graph
    if( ent->moverState != moverLastState[ i ] && ent->targetname && ent->targetname[ 0 ] )
      NavCheckFloors( ent );
    moverLastState[ i ] = ent->moverState;

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

// does a player moving from a to b hit the box? (slab test against the box
// grown by the player's size)
static qboolean NavSegmentHitsBox( const vec3_t a, const vec3_t b,
                                   const vec3_t mins, const vec3_t maxs )
{
  float tmin = 0.0f, tmax = 1.0f, d, t1, t2, tmp;
  int   i;

  for( i = 0; i < 3; i++ )
  {
    d = b[ i ] - a[ i ];
    if( fabs( d ) < 0.001f )
    {
      if( a[ i ] < mins[ i ] || a[ i ] > maxs[ i ] )
        return qfalse;
      continue;
    }

    t1 = ( mins[ i ] - a[ i ] ) / d;
    t2 = ( maxs[ i ] - a[ i ] ) / d;
    if( t1 > t2 )
    {
      tmp = t1;
      t1 = t2;
      t2 = tmp;
    }

    if( t1 > tmin )
      tmin = t1;
    if( t2 < tmax )
      tmax = t2;
    if( tmin > tmax )
      return qfalse;
  }

  return qtrue;
}

// the box a size class's origin can't be in without touching a structure
static void NavGrowBox( const vec3_t absmin, const vec3_t absmax, int size,
                        vec3_t mins, vec3_t maxs )
{
  float w = navSizeWidth[ size ] - 1.0f;

  VectorSet( mins, absmin[ 0 ] - w, absmin[ 1 ] - w, absmin[ 2 ] - navSizeTop[ size ] );
  VectorSet( maxs, absmax[ 0 ] + w, absmax[ 1 ] + w, absmax[ 2 ] - navMins[ 2 ] );
}

// how high the top of a structure is for its own team: aliens shrink
// barricades by jumping on them
static float NavStructureTop( gentity_t *ent )
{
  vec3_t mins, maxs;

  if( ent->s.modelindex != BA_A_BARRICADE )
    return ent->r.absmax[ 2 ];

  BG_BuildableBoundingBox( BA_A_BARRICADE, mins, maxs );
  return ent->r.currentOrigin[ 2 ] + (int)( maxs[ 2 ] * BARRICADE_SHRINKPROP );
}

// is it low enough to jump on, with room to stand on top?
static qboolean NavHoppable( gentity_t *ent, float top )
{
  trace_t tr;
  vec3_t  p;

  if( top - ent->r.absmin[ 2 ] > NAV_MAX_HOP )
    return qfalse;

  VectorSet( p, ( ent->r.absmin[ 0 ] + ent->r.absmax[ 0 ] ) * 0.5f,
                ( ent->r.absmin[ 1 ] + ent->r.absmax[ 1 ] ) * 0.5f,
                top - navMins[ 2 ] + 1.0f );
  trap_Trace( &tr, p, navMins, navMaxs, p, ent->s.number, MASK_PLAYERSOLID & ~CONTENTS_BODY );

  return !tr.startsolid && !tr.allsolid;
}

static void NavBlockStructure( gentity_t *ent )
{
  int       team = NAV_TEAM( ent->buildableTeam );
  vec3_t    mins[ NAV_NUM_SIZES ], maxs[ NAV_NUM_SIZES ];
  vec3_t    standMins, standMaxs;
  float     top = NavStructureTop( ent );
  qboolean  hoppable = NavHoppable( ent, top );
  int       x, y, n, i, size, hop;
  int       x0, x1, y0, y1;
  float     *a, *b;

  for( size = 0; size < NAV_NUM_SIZES; size++ )
    NavGrowBox( ent->r.absmin, ent->r.absmax, size, mins[ size ], maxs[ size ] );

  // where a standing human can't be
  VectorSubtract( ent->r.absmin, navMaxs, standMins );
  standMins[ 2 ] = ent->r.absmin[ 2 ] - navStandTop;
  VectorSubtract( ent->r.absmax, navMins, standMaxs );

  // links reach at most a cell and a bit, so look further out
  x0 = NavCell( mins[ NAV_NUM_SIZES - 1 ][ 0 ] ) - 2;
  x1 = NavCell( maxs[ NAV_NUM_SIZES - 1 ][ 0 ] ) + 2;
  y0 = NavCell( mins[ NAV_NUM_SIZES - 1 ][ 1 ] ) - 2;
  y1 = NavCell( maxs[ NAV_NUM_SIZES - 1 ][ 1 ] ) + 2;

  for( x = x0; x <= x1; x++ )
  {
    for( y = y0; y <= y1; y++ )
    {
      for( n = hashHeads[ NavHash( x, y ) ]; n >= 0; n = nodes[ n ].hashNext )
      {
        if( nodes[ n ].cellX != x || nodes[ n ].cellY != y )
          continue;

        a = nodes[ n ].origin;
        if( a[ 0 ] >= standMins[ 0 ] && a[ 0 ] <= standMaxs[ 0 ] &&
            a[ 1 ] >= standMins[ 1 ] && a[ 1 ] <= standMaxs[ 1 ] &&
            a[ 2 ] >= standMins[ 2 ] && a[ 2 ] <= standMaxs[ 2 ] )
          nodeBlocked[ n ] |= 1 << ent->buildableTeam;

        for( i = 0; i < nodes[ n ].numLinks; i++ )
        {
          b = nodes[ nodes[ n ].links[ i ] ].origin;

          // the boxes grow with size, so if the biggest misses they all do
          if( !NavSegmentHitsBox( a, b, mins[ NAV_NUM_SIZES - 1 ], maxs[ NAV_NUM_SIZES - 1 ] ) )
            continue;

          for( size = 0; size < NAV_NUM_SIZES - 1; size++ )
          {
            if( NavSegmentHitsBox( a, b, mins[ size ], maxs[ size ] ) )
              break;
          }

          if( size < nodes[ n ].linkFit[ i ][ team ] )
            nodes[ n ].linkFit[ i ][ team ] = size;

          // jumping from the lower end, onto the top
          hop = 255;
          if( hoppable )
            hop = MAX( 1, MIN( 254, (int)( top - ( MIN( a[ 2 ], b[ 2 ] ) + navMins[ 2 ] ) ) ) );
          if( hop > nodes[ n ].linkHop[ i ][ team ] )
            nodes[ n ].linkHop[ i ][ team ] = hop;
        }
      }
    }
  }
}

// mark the area a defence covers as dangerous for the other team
static void NavAddThreat( gentity_t *ent, float range, int amount )
{
  int     team = NAV_TEAM( ent->buildableTeam == TEAM_ALIENS ? TEAM_HUMANS : TEAM_ALIENS );
  int     x, y, n;
  int     x0 = NavCell( ent->r.currentOrigin[ 0 ] - range );
  int     x1 = NavCell( ent->r.currentOrigin[ 0 ] + range );
  int     y0 = NavCell( ent->r.currentOrigin[ 1 ] - range );
  int     y1 = NavCell( ent->r.currentOrigin[ 1 ] + range );
  vec3_t  center;
  trace_t tr;

  VectorAdd( ent->r.absmin, ent->r.absmax, center );
  VectorScale( center, 0.5f, center );

  for( x = x0; x <= x1; x++ )
  {
    for( y = y0; y <= y1; y++ )
    {
      for( n = hashHeads[ NavHash( x, y ) ]; n >= 0; n = nodes[ n ].hashNext )
      {
        if( nodes[ n ].cellX != x || nodes[ n ].cellY != y ||
            DistanceSquared( nodes[ n ].origin, center ) > Square( range ) )
          continue;

        trap_Trace( &tr, center, NULL, NULL, nodes[ n ].origin, ent->s.number, MASK_SOLID );
        if( tr.fraction < 1.0f )
          continue;

        nodeThreat[ n ][ team ] = MIN( 255, nodeThreat[ n ][ team ] + amount );
      }
    }
  }
}

// can players walk over the top of it? (telenodes, medistations, boosters)
static qboolean NavStepOver( gentity_t *ent )
{
  return ent->r.absmax[ 2 ] - ent->r.absmin[ 2 ] <= NAV_STEP + 1.0f;
}

// re-mark what's blocked when structures have been built or destroyed
static void NavUpdateObstacles( void )
{
  gentity_t *ent;
  int       i, n, signature = 0;

  for( i = MAX_CLIENTS, ent = g_entities + MAX_CLIENTS; i < level.num_entities; i++, ent++ )
  {
    if( !ent->inuse || ent->s.eType != ET_BUILDABLE || !ent->r.linked || !ent->r.contents )
      continue;

    // barricades change height, so only their position counts
    signature = signature * 31 + i * 7 + (int)ent->r.absmin[ 0 ] +
                (int)ent->r.absmin[ 1 ] * 3 + (int)ent->r.absmin[ 2 ] * 5 +
                ent->spawned * 11 + ent->powered * 13;
  }

  if( signature == obstacleSignature && !navObstaclesDirty )
    return;

  // new links only need checking, routes already found are still good
  if( signature != obstacleSignature )
    navVersion++;
  obstacleSignature = signature;
  navObstaclesDirty = qfalse;

  memset( nodeBlocked, 0, numNodes * sizeof( nodeBlocked[ 0 ] ) );
  memset( nodeThreat, 0, numNodes * sizeof( nodeThreat[ 0 ] ) );
  for( n = 0; n < numNodes; n++ )
  {
    for( i = 0; i < nodes[ n ].numLinks; i++ )
    {
      nodes[ n ].linkFit[ i ][ 0 ] = nodes[ n ].linkFit[ i ][ 1 ] = NAV_NUM_SIZES;
      nodes[ n ].linkHop[ i ][ 0 ] = nodes[ n ].linkHop[ i ][ 1 ] = 0;
    }
  }

  for( i = MAX_CLIENTS, ent = g_entities + MAX_CLIENTS; i < level.num_entities; i++, ent++ )
  {
    if( !ent->inuse || ent->s.eType != ET_BUILDABLE || !ent->r.linked || !ent->r.contents )
      continue;

    if( !NavStepOver( ent ) )
      NavBlockStructure( ent );

    if( !ent->spawned || ent->health <= 0 ||
        ( ent->buildableTeam == TEAM_HUMANS && !ent->powered ) )
      continue;

    switch( ent->s.modelindex )
    {
      case BA_H_MGTURRET: NavAddThreat( ent, MGTURRET_RANGE, 40 ); break;
      case BA_H_TESLAGEN: NavAddThreat( ent, TESLAGEN_RANGE + 50.0f, 60 ); break;
      case BA_A_ACIDTUBE: NavAddThreat( ent, ACIDTUBE_RANGE, 30 ); break;
      case BA_A_TRAPPER:  NavAddThreat( ent, TRAPPER_RANGE, 15 ); break;
      case BA_A_HIVE:     NavAddThreat( ent, HIVE_SENSE_RANGE, 20 ); break;
      default: break;
    }
  }

}

/*
===============
Components

Label which nodes connect to which, ignoring structures, so goals in parts
of the map nobody can get to aren't chosen. Links one way down are counted
as both ways, which is close enough
===============
*/
static int NavFindRoot( int *parent, int n )
{
  while( parent[ n ] != n )
  {
    parent[ n ] = parent[ parent[ n ] ];
    n = parent[ n ];
  }

  return n;
}

static void NavLabelComponents( void )
{
  int c, n, i, a, b;

  for( c = 0; c < 2; c++ )
  {
    int *parent = nodeComponent[ c ];

    for( n = 0; n < numNodes; n++ )
      parent[ n ] = n;

    for( n = 0; n < numNodes; n++ )
    {
      for( i = 0; i < nodes[ n ].numLinks; i++ )
      {
        if( !c && ( nodes[ n ].linkFlags[ i ] & NAVLINK_CLIMB ) )
          continue;

        a = NavFindRoot( parent, n );
        b = NavFindRoot( parent, nodes[ n ].links[ i ] );
        if( a != b )
          parent[ a ] = b;
      }
    }

    for( n = 0; n < numNodes; n++ )
      parent[ n ] = NavFindRoot( parent, n );
  }
}

qboolean G_BotNavConnected( const vec3_t a, const vec3_t b, qboolean climb )
{
  int na = G_BotNavNearestNode( a );
  int nb = G_BotNavNearestNode( b );

  // with no graph there yet, assume so
  if( na < 0 || nb < 0 )
    return qtrue;

  return nodeComponent[ climb ? 1 : 0 ][ na ] == nodeComponent[ climb ? 1 : 0 ][ nb ];
}

/*
===============
Portals

Teleporters and jump pads take players somewhere they can't walk to. Nodes
inside one are linked to where it leads, and can't be walked out of
===============
*/

// the nearest node on the ground below a point, adding one if need be
static int NavLandingNode( const vec3_t point )
{
  vec3_t start, ground;
  int    n;

  VectorCopy( point, start );
  start[ 2 ] += NAV_STEP;
  if( !NavGroundAt( start, ground ) )
    return -1;

  n = NavFindNodeInCell( NavCell( ground[ 0 ] ), NavCell( ground[ 1 ] ), ground[ 2 ] );
  return n >= 0 ? n : NavAddNode( ground );
}

// follow a jump pad's throw until it lands
static int NavPushLanding( const vec3_t start, const vec3_t velocity )
{
  trace_t tr;
  vec3_t  pos, next, vel;
  float   gravity = MAX( g_gravity.value, 1.0f );
  int     step;

  VectorCopy( start, pos );
  VectorCopy( velocity, vel );

  for( step = 0; step < 120; step++ )
  {
    VectorMA( pos, 0.05f, vel, next );
    vel[ 2 ] -= gravity * 0.05f;

    trap_Trace( &tr, pos, navMins, navMaxs, next, ENTITYNUM_NONE, NAV_MASK );
    if( tr.startsolid || tr.allsolid )
      return -1;

    if( tr.fraction < 1.0f )
    {
      // hit a wall, slide down it
      if( tr.plane.normal[ 2 ] < NAV_WALK_NORMAL )
      {
        VectorCopy( tr.endpos, pos );
        vel[ 0 ] = vel[ 1 ] = 0.0f;
        if( vel[ 2 ] > 0.0f )
          vel[ 2 ] = 0.0f;
        continue;
      }

      return NavLandingNode( tr.endpos );
    }

    VectorCopy( next, pos );
  }

  return -1;
}

static void NavLinkPortal( gentity_t *trigger, int dest )
{
  int     x, y, n;
  int     x0 = NavCell( trigger->r.absmin[ 0 ] + navMins[ 0 ] );
  int     x1 = NavCell( trigger->r.absmax[ 0 ] + navMaxs[ 0 ] );
  int     y0 = NavCell( trigger->r.absmin[ 1 ] + navMins[ 1 ] );
  int     y1 = NavCell( trigger->r.absmax[ 1 ] + navMaxs[ 1 ] );
  float   *o;

  if( dest < 0 )
    return;

  for( x = x0; x <= x1; x++ )
  {
    for( y = y0; y <= y1; y++ )
    {
      for( n = hashHeads[ NavHash( x, y ) ]; n >= 0; n = nodes[ n ].hashNext )
      {
        o = nodes[ n ].origin;
        if( nodes[ n ].cellX != x || nodes[ n ].cellY != y || n == dest ||
            o[ 0 ] + navMaxs[ 0 ] < trigger->r.absmin[ 0 ] || o[ 0 ] + navMins[ 0 ] > trigger->r.absmax[ 0 ] ||
            o[ 1 ] + navMaxs[ 1 ] < trigger->r.absmin[ 1 ] || o[ 1 ] + navMins[ 1 ] > trigger->r.absmax[ 1 ] ||
            o[ 2 ] + navMaxs[ 2 ] < trigger->r.absmin[ 2 ] || o[ 2 ] + navMins[ 2 ] > trigger->r.absmax[ 2 ] )
          continue;

        nodePortal[ n ] = 1;
        NavAddLink( n, dest, NAVLINK_PORTAL | ( ( NAV_NUM_SIZES - 1 ) << NAVLINK_SIZE_SHIFT ) );
      }
    }
  }
}

static void NavLinkPortals( void )
{
  gentity_t *ent, *target;
  vec3_t    center;
  int       i;

  for( i = MAX_CLIENTS, ent = g_entities + MAX_CLIENTS; i < level.num_entities; i++, ent++ )
  {
    if( !ent->inuse || !ent->classname || !ent->target || !ent->target[ 0 ] )
      continue;

    VectorAdd( ent->r.absmin, ent->r.absmax, center );
    VectorScale( center, 0.5f, center );
    center[ 2 ] = ent->r.absmin[ 2 ] - navMins[ 2 ] + 1.0f;

    if( !Q_stricmp( ent->classname, "trigger_teleport" ) )
    {
      if( ( target = G_PickTarget( ent->target ) ) )
        NavLinkPortal( ent, NavLandingNode( target->s.origin ) );
    }
    else if( !Q_stricmp( ent->classname, "trigger_push" ) )
      NavLinkPortal( ent, NavPushLanding( center, ent->s.origin2 ) );
    else if( !Q_stricmp( ent->classname, "trigger_multiple" ) ||
             !Q_stricmp( ent->classname, "trigger_once" ) )
    {
      // a trigger that fires a target_push
      target = G_PickTarget( ent->target );
      if( target && target->classname && !Q_stricmp( target->classname, "target_push" ) )
        NavLinkPortal( ent, NavPushLanding( center, target->s.origin2 ) );
    }
  }
}

/*
===============
G_BotNavWouldBlock

Would a structure in this box cut off the walkable area around it? The
nodes and links it would block are taken out, then the nodes left on its
edge must still be able to reach each other nearby
===============
*/
qboolean G_BotNavWouldBlock( const vec3_t absmin, const vec3_t absmax )
{
  vec3_t  mins, maxs;
  int     edge[ 64 ], numEdge = 0;
  int     queue[ 1024 ], head = 0, tail = 0;
  int     x, y, n, i, m;
  int     cx0, cx1, cy0, cy1;

  VectorSubtract( absmin, navMaxs, mins );
  VectorSubtract( absmax, navMins, maxs );

  wouldBlockStamp++;
  cx0 = NavCell( mins[ 0 ] ) - 1;
  cx1 = NavCell( maxs[ 0 ] ) + 1;
  cy0 = NavCell( mins[ 1 ] ) - 1;
  cy1 = NavCell( maxs[ 1 ] ) + 1;

  // nodes the structure would sit on
  for( x = cx0; x <= cx1; x++ )
  {
    for( y = cy0; y <= cy1; y++ )
    {
      for( n = hashHeads[ NavHash( x, y ) ]; n >= 0; n = nodes[ n ].hashNext )
      {
        if( nodes[ n ].cellX == x && nodes[ n ].cellY == y &&
            nodes[ n ].origin[ 0 ] >= mins[ 0 ] && nodes[ n ].origin[ 0 ] <= maxs[ 0 ] &&
            nodes[ n ].origin[ 1 ] >= mins[ 1 ] && nodes[ n ].origin[ 1 ] <= maxs[ 1 ] &&
            nodes[ n ].origin[ 2 ] >= mins[ 2 ] && nodes[ n ].origin[ 2 ] <= maxs[ 2 ] )
          footStamp[ n ] = wouldBlockStamp;
      }
    }
  }

  // nodes around it that would lose a link
  for( x = cx0 - 1; x <= cx1 + 1; x++ )
  {
    for( y = cy0 - 1; y <= cy1 + 1; y++ )
    {
      for( n = hashHeads[ NavHash( x, y ) ]; n >= 0; n = nodes[ n ].hashNext )
      {
        if( nodes[ n ].cellX != x || nodes[ n ].cellY != y ||
            footStamp[ n ] == wouldBlockStamp || nodeBlocked[ n ] ||
            fabs( nodes[ n ].origin[ 2 ] - ( mins[ 2 ] + maxs[ 2 ] ) * 0.5f ) > 128.0f )
          continue;

        for( i = 0; i < nodes[ n ].numLinks; i++ )
        {
          m = nodes[ n ].links[ i ];
          if( footStamp[ m ] == wouldBlockStamp ||
              NavSegmentHitsBox( nodes[ n ].origin, nodes[ m ].origin, mins, maxs ) )
            break;
        }

        if( i < nodes[ n ].numLinks && numEdge < ARRAY_LEN( edge ) )
          edge[ numEdge++ ] = n;
      }
    }
  }

  if( numEdge < 2 )
    return qfalse;

  // walk around it from one edge node, staying close by
  visitStamp[ edge[ 0 ] ] = wouldBlockStamp;
  queue[ tail++ ] = edge[ 0 ];

  while( head < tail )
  {
    n = queue[ head++ ];

    for( i = 0; i < nodes[ n ].numLinks; i++ )
    {
      m = nodes[ n ].links[ i ];

      if( visitStamp[ m ] == wouldBlockStamp || footStamp[ m ] == wouldBlockStamp ||
          nodeBlocked[ m ] || ( nodes[ n ].linkFlags[ i ] & NAVLINK_CLIMB ) ||
          !nodes[ n ].linkFit[ i ][ 0 ] || !nodes[ n ].linkFit[ i ][ 1 ] ||
          NavSegmentHitsBox( nodes[ n ].origin, nodes[ m ].origin, mins, maxs ) )
        continue;

      // a detour much longer than this doesn't count
      if( nodes[ m ].cellX < cx0 - 6 || nodes[ m ].cellX > cx1 + 6 ||
          nodes[ m ].cellY < cy0 - 6 || nodes[ m ].cellY > cy1 + 6 )
        continue;

      if( tail >= ARRAY_LEN( queue ) )
        break;

      visitStamp[ m ] = wouldBlockStamp;
      queue[ tail++ ] = m;
    }
  }

  for( i = 1; i < numEdge; i++ )
  {
    if( visitStamp[ edge[ i ] ] != wouldBlockStamp )
      return qtrue;
  }

  return qfalse;
}

/*
===============
G_BotNavOpenGround

Is there walkable floor all around a structure's footprint, rather than it
being on a bridge, a ledge or in a corridor?
===============
*/
qboolean G_BotNavOpenGround( const vec3_t absmin, const vec3_t absmax )
{
  int     x, y, n, cells = 0, floored = 0;
  int     x0 = NavCell( absmin[ 0 ] - NAV_GRID ), x1 = NavCell( absmax[ 0 ] + NAV_GRID );
  int     y0 = NavCell( absmin[ 1 ] - NAV_GRID ), y1 = NavCell( absmax[ 1 ] + NAV_GRID );
  float   floorZ = absmin[ 2 ] - navMins[ 2 ];

  for( x = x0; x <= x1; x++ )
  {
    for( y = y0; y <= y1; y++ )
    {
      cells++;
      for( n = hashHeads[ NavHash( x, y ) ]; n >= 0; n = nodes[ n ].hashNext )
      {
        if( nodes[ n ].cellX == x && nodes[ n ].cellY == y &&
            fabs( nodes[ n ].origin[ 2 ] - floorZ ) < NAV_LEVEL_DIST )
        {
          floored++;
          break;
        }
      }
    }
  }

  return floored * 10 >= cells * 9;
}

int G_BotNavVersion( void )
{
  return navVersion;
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

  if( level.time >= nextObstacleTime )
  {
    nextObstacleTime = level.time + 500;
    NavUpdateObstacles( );
  }

  if( navComponentsDirty && level.time >= nextComponentTime )
  {
    nextComponentTime = level.time + 1000;
    navComponentsDirty = qfalse;
    NavLabelComponents( );
  }

  // teleporters once the graph has reached them, and again as it grows
  if( expandCursor >= numNodes && numNodes != portalNodes && level.time >= nextPortalTime )
  {
    nextPortalTime = level.time + 10000;
    portalNodes = numNodes;
    NavUnlinkMovers( );
    NavLinkPortals( );
    NavRelinkMovers( );
  }

  // the team forgets where it died after a minute or so
  if( level.time >= nextDangerDecay )
  {
    nextDangerDecay = level.time + 1000;
    for( i = 0; i < NAV_NUM_REGIONS; i++ )
    {
      regionDanger[ 0 ][ i ] *= 0.98f;
      regionDanger[ 1 ][ i ] *= 0.98f;
    }
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
    len = Com_sprintf( line, sizeof( line ), "%d %.0f %.0f %.0f%s", n,
                       nodes[ n ].origin[ 0 ], nodes[ n ].origin[ 1 ], nodes[ n ].origin[ 2 ],
                       nodeBlocked[ n ] ? " B" : "" );
    for( i = 0; i < nodes[ n ].numLinks && len < sizeof( line ) - 32; i++ )
    {
      len += Com_sprintf( line + len, sizeof( line ) - len, " %d:%d",
                          nodes[ n ].links[ i ], nodes[ n ].linkFlags[ i ] );

      // sizes that get past each team's structures, and how high they are
      if( nodes[ n ].linkFit[ i ][ 0 ] < NAV_NUM_SIZES || nodes[ n ].linkFit[ i ][ 1 ] < NAV_NUM_SIZES )
        len += Com_sprintf( line + len, sizeof( line ) - len, "/%d.%d.%d.%d",
                            nodes[ n ].linkFit[ i ][ 0 ], nodes[ n ].linkHop[ i ][ 0 ],
                            nodes[ n ].linkFit[ i ][ 1 ], nodes[ n ].linkHop[ i ][ 1 ] );
    }
    Q_strcat( line, sizeof( line ), "\n" );
    trap_FS_Write( line, strlen( line ), f );
  }

  // and the structures, for working out what's in the way
  {
    gentity_t *ent;

    for( n = MAX_CLIENTS, ent = g_entities + MAX_CLIENTS; n < level.num_entities; n++, ent++ )
    {
      if( !ent->inuse || ent->s.eType != ET_BUILDABLE )
        continue;

      Com_sprintf( line, sizeof( line ), "buildable %s %d %.0f %.0f %.0f %.0f %.0f %.0f %.0f %.0f %.0f\n",
                   BG_Buildable( ent->s.modelindex )->name, ent->buildableTeam,
                   ent->r.currentOrigin[ 0 ], ent->r.currentOrigin[ 1 ], ent->r.currentOrigin[ 2 ],
                   ent->r.absmin[ 0 ], ent->r.absmin[ 1 ], ent->r.absmin[ 2 ],
                   ent->r.absmax[ 0 ], ent->r.absmax[ 1 ], ent->r.absmax[ 2 ] );
      trap_FS_Write( line, strlen( line ), f );
    }
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
    G_Printf( "usage: navtrace x1 y1 z1 x2 y2 z2 [class]\n" );
    return;
  }

  for( i = 0; i < 3; i++ )
  {
    trap_Argv( 1 + i, arg, sizeof( arg ) );
    start[ i ] = atof( arg );
    trap_Argv( 4 + i, arg, sizeof( arg ) );
    end[ i ] = atof( arg );
  }

  if( trap_Argc( ) > 7 )
  {
    trap_Argv( 7, arg, sizeof( arg ) );
    BG_ClassBoundingBox( BG_ClassByName( arg )->number, mins, maxs, NULL, NULL, NULL );
  }
  else
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
G_BotNavPathTest

navpath x1 y1 z1 x2 y2 z2 [class]: find a route as a bot of that class on
its team would and report where it leads, for debugging
===============
*/
void G_BotNavPathTest( void )
{
  char        arg[ MAX_TOKEN_CHARS ];
  vec3_t      start, end, mins, maxs;
  int         path[ 256 ], length, i, n;
  navQuery_t  query;
  class_t     cl = PCL_HUMAN;
  float       jump;

  if( trap_Argc( ) < 7 )
  {
    G_Printf( "usage: navpath x1 y1 z1 x2 y2 z2 [class]\n" );
    return;
  }

  for( i = 0; i < 3; i++ )
  {
    trap_Argv( 1 + i, arg, sizeof( arg ) );
    start[ i ] = atof( arg );
    trap_Argv( 4 + i, arg, sizeof( arg ) );
    end[ i ] = atof( arg );
  }

  if( trap_Argc( ) > 7 )
  {
    trap_Argv( 7, arg, sizeof( arg ) );
    cl = BG_ClassByName( arg )->number;
  }

  BG_ClassBoundingBox( cl, mins, maxs, NULL, NULL, NULL );
  jump = BG_Class( cl )->jumpMagnitude;

  memset( &query, 0, sizeof( query ) );
  query.size = G_BotNavSizeClass( mins, maxs );
  query.team = ( cl == PCL_HUMAN || cl == PCL_HUMAN_BSUIT ) ? TEAM_HUMANS : TEAM_ALIENS;
  query.climb = BG_ClassHasAbility( cl, SCA_WALLCLIMBER );
  query.jump = query.hop = jump * jump / ( 2.0f * MAX( g_gravity.value, 1.0f ) ) + 6.0f;

  length = G_BotNavFindPath( start, end, path, ARRAY_LEN( path ), &query );
  G_Printf( "%s, size %d: %d nodes", BG_Class( cl )->name, query.size, length );
  if( length > 0 )
  {
    n = path[ length - 1 ];
    G_Printf( ", ends at (%.0f %.0f %.0f), %.0f from the goal", nodes[ n ].origin[ 0 ],
              nodes[ n ].origin[ 1 ], nodes[ n ].origin[ 2 ], Distance( nodes[ n ].origin, end ) );
  }
  G_Printf( "\n" );

  // and the route itself: node, position, and the flags of the link there
  if( trap_Argc( ) > 8 )
  {
    for( i = 0; i < length; i++ )
    {
      n = path[ i ];
      G_Printf( "  %d (%.0f %.0f %.0f) %d\n", n, nodes[ n ].origin[ 0 ], nodes[ n ].origin[ 1 ],
                nodes[ n ].origin[ 2 ], i ? G_BotNavLinkFlags( path[ i - 1 ], n, &query ) : 0 );
    }
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
    if( nodeDead[ n ] || fabs( nodes[ n ].origin[ 2 ] - origin[ 2 ] ) > 96.0f )
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

// pick a random node anywhere within radius of a point
qboolean G_BotNavNodeAround( const vec3_t point, float radius, vec3_t out )
{
  int n, count = 0;

  for( n = 0; n < numNodes; n++ )
  {
    if( nodeBlocked[ n ] || nodeDead[ n ] || DistanceSquared( nodes[ n ].origin, point ) > Square( radius ) )
      continue;

    if( rand( ) % ++count == 0 )
      VectorCopy( nodes[ n ].origin, out );
  }

  return count > 0;
}

/*
===============
G_BotNavExplore

Somewhere the team hasn't been for a while, between minDist and maxDist away
===============
*/
qboolean G_BotNavExplore( team_t team, const vec3_t origin, float minDist, float maxDist, vec3_t out )
{
  int   i, n, best = -1, here;
  float d, score, bestScore = 0.0f;

  if( numNodes == 0 || ( team != TEAM_ALIENS && team != TEAM_HUMANS ) )
    return qfalse;

  // only somewhere we can walk to
  here = G_BotNavNearestNode( origin );
  if( here < 0 )
    return qfalse;

  for( i = 0; i < 64; i++ )
  {
    n = rand( ) % numNodes;
    if( nodeBlocked[ n ] || nodeDead[ n ] || nodeComponent[ 0 ][ n ] != nodeComponent[ 0 ][ here ] )
      continue;

    d = Distance( nodes[ n ].origin, origin );
    if( d < minDist || d > maxDist )
      continue;

    // how long since anyone went there, give or take
    score = MIN( level.time - regionVisited[ NAV_TEAM( team ) ][ nodes[ n ].region ], 180000 ) +
            ( rand( ) % 30000 );
    if( best < 0 || score > bestScore )
    {
      best = n;
      bestScore = score;
    }
  }

  if( best < 0 )
    return qfalse;

  VectorCopy( nodes[ best ].origin, out );
  return qtrue;
}

// a team member died here, routes keep away for a while
void G_BotNavAddDanger( team_t team, const vec3_t origin, float amount )
{
  float *danger;

  if( team != TEAM_ALIENS && team != TEAM_HUMANS )
    return;

  danger = &regionDanger[ NAV_TEAM( team ) ][ G_BotNavRegion( origin ) ];
  *danger = MIN( *danger + amount, 2000.0f );
}

// how high the ceiling is above a point, 0 if there's none a wall climber could hang from
float G_BotNavCeiling( const vec3_t origin, float maxHeight )
{
  trace_t tr;
  vec3_t  end;

  VectorCopy( origin, end );
  end[ 2 ] += maxHeight;
  trap_Trace( &tr, origin, NULL, NULL, end, ENTITYNUM_NONE, MASK_SOLID );

  if( tr.fraction >= 1.0f || tr.startsolid || tr.entityNum != ENTITYNUM_WORLD ||
      tr.plane.normal[ 2 ] > -0.7f || ( tr.surfaceFlags & ( SURF_SKY | SURF_SLICK ) ) )
    return 0.0f;

  return tr.endpos[ 2 ] - origin[ 2 ];
}

// a bot got stuck on a link. Rather than forget it, which could cut off an
// area it's the only way into, make routes avoid it when they can
void G_BotNavLinkFailed( int from, int to )
{
  navNode_t *node;
  int       i;

  if( from < 0 || to < 0 || from >= numNodes || to >= numNodes )
    return;

  node = &nodes[ from ];
  for( i = 0; i < node->numLinks; i++ )
  {
    if( node->links[ i ] == to && node->linkFails[ i ] < 4 )
      node->linkFails[ i ]++;
  }
}

// is our own team's structure in the way of a link for something our size?
static qboolean NavLinkObstructed( int n, int i, const navQuery_t *query )
{
  if( query->team != TEAM_ALIENS && query->team != TEAM_HUMANS )
    return !nodes[ n ].linkFit[ i ][ 0 ] || !nodes[ n ].linkFit[ i ][ 1 ];

  return nodes[ n ].linkFit[ i ][ NAV_TEAM( query->team ) ] <= query->size;
}

// the link's flags, with NAVLINK_HOP if it means jumping onto our own structures
int G_BotNavLinkFlags( int from, int to, const navQuery_t *query )
{
  int i;

  if( from < 0 || to < 0 )
    return 0;

  for( i = 0; i < nodes[ from ].numLinks; i++ )
  {
    if( nodes[ from ].links[ i ] == to )
      return nodes[ from ].linkFlags[ i ] |
             ( query && NavLinkObstructed( from, i, query ) ? NAVLINK_HOP : 0 );
  }

  return 0;
}

static qboolean NavVisible( const vec3_t a, const vec3_t b )
{
  trace_t tr;

  trap_Trace( &tr, a, NULL, NULL, b, ENTITYNUM_NONE, NAV_MASK );
  return tr.fraction >= 1.0f;
}

// can something of this size walk straight from a point to a node?
static qboolean NavFits( const vec3_t from, const vec3_t to, int size )
{
  trace_t tr;
  vec3_t  mins, maxs, a, b;

  VectorSet( mins, -navSizeWidth[ size ] + 1.0f, -navSizeWidth[ size ] + 1.0f, navMins[ 2 ] );
  VectorSet( maxs, navSizeWidth[ size ] - 1.0f, navSizeWidth[ size ] - 1.0f, navSizeTop[ size ] );
  VectorCopy( from, a );
  VectorCopy( to, b );
  a[ 2 ] += NAV_STEP;
  b[ 2 ] += NAV_STEP;

  trap_Trace( &tr, a, mins, maxs, b, ENTITYNUM_NONE, NAV_MASK );
  return !tr.allsolid && tr.fraction >= 1.0f;
}

// the closest few nodes to a point, visible ones first and nearest first.
// If none are in sight, the single closest node a little further out
static int NavNearestNodes( const vec3_t origin, int *out, int maxOut, int size, float maxRise )
{
  int     cx = NavCell( origin[ 0 ] );
  int     cy = NavCell( origin[ 1 ] );
  int     x, y, n, r, count;
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

          // inside a structure, or the floor's gone, so no use as a start or end point
          if( nodeBlocked[ n ] || nodeDead[ n ] )
            continue;

          // nor anywhere above us we can't get up to
          if( fabs( nodes[ n ].origin[ 2 ] - origin[ 2 ] ) > 128.0f ||
              nodes[ n ].origin[ 2 ] - origin[ 2 ] > maxRise )
            continue;

          // a node up on a ledge above us is further away than it looks
          d = DistanceSquared( nodes[ n ].origin, origin ) +
              Square( MAX( 0.0f, fabs( nodes[ n ].origin[ 2 ] - origin[ 2 ] ) - NAV_STEP ) * 4.0f );

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

    count = 0;
    for( i = 0; i < numCandidates && count < maxOut; i++ )
    {
      if( NavVisible( origin, nodes[ candidates[ i ] ].origin ) &&
          ( size < 0 || NavFits( origin, nodes[ candidates[ i ] ].origin, size ) ) )
        out[ count++ ] = candidates[ i ];
    }

    // nothing we fit straight to, settle for something in sight
    if( !count && size >= 0 )
    {
      for( i = 0; i < numCandidates && count < maxOut; i++ )
      {
        if( NavVisible( origin, nodes[ candidates[ i ] ].origin ) )
          out[ count++ ] = candidates[ i ];
      }
    }

    if( count )
      return count;

    if( best >= 0 && r > 1 )
    {
      out[ 0 ] = best;
      return 1;
    }
  }

  return 0;
}

int G_BotNavNearestNode( const vec3_t origin )
{
  int node;

  return NavNearestNodes( origin, &node, 1, -1, 128.0f ) ? node : -1;
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
NavFindPathSized

Fills path with the nodes to visit from start towards goal and returns the
number of nodes. If the goal can't be reached, the path leads to the
closest reachable node instead. Returns 0 if there is no usable path.
===============
*/
static int NavFindPathSized( const vec3_t start, const vec3_t goal, int *path, int maxPath,
                             const navQuery_t *query )
{
  int     size = query->size;
  int     own = -1, enemy = -1;
  int     startNode, goalNode;
  int     starts[ 4 ], numStarts;
  int     n, m, i, expanded, flags;
  int     closest;
  float   closestDist, h, g, cost;
  int     length;

  if( !navInitialised || numNodes == 0 )
    return 0;

  if( query->team == TEAM_ALIENS || query->team == TEAM_HUMANS )
  {
    own = NAV_TEAM( query->team );
    enemy = 1 - own;
  }

  // start from a few nodes around us, the nearest one may be boxed in by
  // our own structures when there's an easy way out in another direction
  // somewhere we can step or jump up to, or fly or climb to
  numStarts = NavNearestNodes( start, starts, ARRAY_LEN( starts ), size,
                               query->climb ? 128.0f : MAX( query->jump, 0.0f ) + NAV_STEP );
  if( !numStarts )
    numStarts = NavNearestNodes( start, starts, ARRAY_LEN( starts ), size, 128.0f );
  startNode = numStarts ? starts[ 0 ] : -1;
  goalNode = G_BotNavNearestNode( goal );
  if( startNode < 0 || goalNode < 0 )
    return 0;

  searchStamp++;
  heapSize = 0;

  closest = startNode;
  closestDist = Distance( nodes[ startNode ].origin, nodes[ goalNode ].origin );

  for( i = 0; i < numStarts; i++ )
  {
    n = starts[ i ];
    gScore[ n ] = Distance( start, nodes[ n ].origin );
    cameFrom[ n ] = -1;
    openStamp[ n ] = searchStamp;
    h = Distance( nodes[ n ].origin, nodes[ goalNode ].origin );
    HeapPush( n, gScore[ n ] + h );

    if( h < closestDist )
    {
      closest = n;
      closestDist = h;
    }
  }
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
      flags = nodes[ n ].linkFlags[ i ];

      if( closedStamp[ m ] == searchStamp || nodeDead[ m ] ||
          ( ( flags & NAVLINK_SIZE_MASK ) >> NAVLINK_SIZE_SHIFT ) < size ||
          ( ( flags & NAVLINK_JUMP ) && nodes[ n ].linkJump[ i ] > query->jump ) ||
          ( ( flags & NAVLINK_CLIMB ) && !query->climb ) ||
          ( nodePortal[ n ] && !( flags & NAVLINK_PORTAL ) ) )
        continue;

      // teleporters are instant, jump pads near enough
      if( flags & NAVLINK_PORTAL )
        cost = NAV_GRID * 2.0f;
      else
        cost = Distance( nodes[ n ].origin, nodes[ m ].origin );
      if( flags & NAVLINK_JUMP )
        cost += NAV_GRID;
      if( flags & NAVLINK_CLIMB )
        cost = cost * 1.5f + NAV_GRID;

      // crouching is slow, but small aliens like a tunnel or a vent: it's
      // somewhere the bigger enemies can't follow
      if( ( flags & NAVLINK_CROUCH ) && query->team == TEAM_HUMANS )
        cost *= 2.0f;
      else if( query->team == TEAM_ALIENS && query->size <= 1 &&
               ( ( flags & NAVLINK_CROUCH ) || ( ( flags & NAVLINK_SIZE_MASK ) >> NAVLINK_SIZE_SHIFT ) <= 2 ) )
        cost *= 0.7f;

      // our own structures are in the way unless we can jump on them, the
      // enemy's are there to be destroyed but that takes a while
      if( NavLinkObstructed( n, i, query ) )
      {
        if( own >= 0 && nodes[ n ].linkHop[ i ][ own ] &&
            nodes[ n ].linkHop[ i ][ own ] <= query->hop )
          cost += NAV_GRID * 2.0f;
        else if( query->squeeze )
          cost += NAV_ENEMY_COST;
        else
          continue;
      }

      if( own >= 0 )
      {
        if( nodes[ n ].linkFit[ i ][ enemy ] <= size )
          cost += NAV_ENEMY_COST;

        if( query->caution > 0.0f )
          cost += query->caution * ( nodeThreat[ m ][ own ] * 4.0f +
                                     regionDanger[ own ][ nodes[ m ].region ] * 0.5f );
      }

      // lean towards some areas over others, so bots spread out
      if( query->noise > 0.0f )
        cost *= 1.0f + query->noise * NavNoise( nodes[ m ].region, query->seed );

      // links bots have got stuck on are a last resort
      g = gScore[ n ] + cost + nodes[ n ].linkFails[ i ] * 500.0f;

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

/*
===============
G_BotNavFindPath

As NavFindPathSized. If our own structures have us boxed in, try again
squeezing past them, and if nothing our size leads anywhere at all from
here, the route for a human sized player is used instead
===============
*/
int G_BotNavFindPath( const vec3_t start, const vec3_t goal, int *path, int maxPath,
                      const navQuery_t *query )
{
  int         length, smallLength;
  int         smallPath[ 128 ];
  navQuery_t  small;
  vec3_t      end;
  float       dist, smallDist;

  length = NavFindPathSized( start, goal, path, maxPath, query );

  // our own structures have us boxed in, though there may well be room to
  // squeeze past them
  if( length <= 1 && !query->squeeze &&
      ( !length || Distance( nodes[ path[ 0 ] ].origin, goal ) > 200.0f ) )
  {
    small = *query;
    small.squeeze = qtrue;
    length = NavFindPathSized( start, goal, path, maxPath, &small );
  }

  if( query->size == 0 || length > 1 )
    return length;

  dist = 1e9f;
  if( length > 0 )
    dist = Distance( nodes[ path[ length - 1 ] ].origin, goal );

  if( dist < 200.0f )
    return length;

  small = *query;
  small.size = 0;
  smallLength = NavFindPathSized( start, goal, smallPath, MIN( maxPath, ARRAY_LEN( smallPath ) ), &small );
  if( !smallLength )
    return length;

  VectorCopy( nodes[ smallPath[ smallLength - 1 ] ].origin, end );
  smallDist = Distance( end, goal );
  if( smallDist + 200.0f >= dist )
    return length;

  memcpy( path, smallPath, smallLength * sizeof( int ) );
  return smallLength;
}
