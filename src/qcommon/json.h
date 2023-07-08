/*
===========================================================================
Copyright (C) 2016 James Canete
Copyright (C) 2015-2019 GrangerHub

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 3
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not,  see <http://www.gnu.org/licenses/>.
===========================================================================
*/

#ifndef JSON_H
#define JSON_H

enum
{
	JSONTYPE_STRING, // string
	JSONTYPE_OBJECT, // object
	JSONTYPE_ARRAY,  // array
	JSONTYPE_VALUE,  // number, true, false, or null
	JSONTYPE_ERROR   // out of data
};

// --------------------------------------------------------------------------
//   Array Functions
// --------------------------------------------------------------------------

// Get pointer to first value in array
// When given pointer to an array, returns pointer to the first
// returns NULL if array is empty or not an array.
const char *JSON_ArrayGetFirstValue(const char *json, const char *jsonEnd);

// Get pointer to next value in array
// When given pointer to a value, returns pointer to the next value
// returns NULL when no next value.
const char *JSON_ArrayGetNextValue(const char *json, const char *jsonEnd);

// Get pointers to values in an array
// returns 0 if not an array, array is empty, or out of data
// returns number of values in the array and copies into index if successful
unsigned int JSON_ArrayGetIndex(const char *json, const char *jsonEnd, const char **indexes, unsigned int numIndexes);

// Get pointer to indexed value from array
// returns NULL if not an array, no index, or out of data
const char *JSON_ArrayGetValue(const char *json, const char *jsonEnd, unsigned int index);

// --------------------------------------------------------------------------
//   Object Functions
// --------------------------------------------------------------------------

// Get pointer to named value from object
// returns NULL if not an object, name not found, or out of data
const char *JSON_ObjectGetNamedValue(const char *json, const char *jsonEnd, const char *name);

// --------------------------------------------------------------------------
//   Value Functions
// --------------------------------------------------------------------------

// Get type of value
// returns JSONTYPE_ERROR if out of data
unsigned int JSON_ValueGetType(const char *json, const char *jsonEnd);

// Get value as string
// returns 0 if out of data
// returns length and copies into string if successful, including terminating nul.
// string values are stripped of enclosing quotes but not escaped
unsigned int JSON_ValueGetString(const char *json, const char *jsonEnd, char *outString, unsigned int stringLen);

// Get value as appropriate type
// returns 0 if value is false, value is null, or out of data
// returns 1 if value is true
// returns value otherwise
double JSON_ValueGetDouble(const char *json, const char *jsonEnd);
float JSON_ValueGetFloat(const char *json, const char *jsonEnd);
int JSON_ValueGetInt(const char *json, const char *jsonEnd);

#endif

//#ifdef JSON_IMPLEMENTATION
//#include <stdio.h>

// --------------------------------------------------------------------------
//   Internal Functions
// --------------------------------------------------------------------------

static const char *JSON_SkipSeparators(const char *json, const char *jsonEnd);
static const char *JSON_SkipString(const char *json, const char *jsonEnd);
static const char *JSON_SkipStruct(const char *json, const char *jsonEnd);
static const char *JSON_SkipValue(const char *json, const char *jsonEnd);
static const char *JSON_SkipValueAndSeparators(const char *json, const char *jsonEnd);

//#endif
