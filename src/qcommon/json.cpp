#include <stdio.h>
#include <string.h>

#include "json.h"

#define IS_SEPARATOR(x)    ((x) == ' ' || (x) == '\t' || (x) == '\n' || (x) == '\r' || (x) == ',' || (x) == ':')
#define IS_STRUCT_OPEN(x)  ((x) == '{' || (x) == '[')
#define IS_STRUCT_CLOSE(x) ((x) == '}' || (x) == ']')

static const char *JSON_SkipSeparators(const char *json, const char *jsonEnd)
{
	while (json < jsonEnd && IS_SEPARATOR(*json))
		json++;

	return json;
}

static const char *JSON_SkipValueAndSeparators(const char *json, const char *jsonEnd)
{
	json = JSON_SkipValue(json, jsonEnd);
	return JSON_SkipSeparators(json, jsonEnd);
}

static const char *JSON_SkipStruct(const char *json, const char *jsonEnd)
{
	json = JSON_SkipSeparators(json + 1, jsonEnd);
	while (json < jsonEnd && !IS_STRUCT_CLOSE(*json))
		json = JSON_SkipValueAndSeparators(json, jsonEnd);

	return (json + 1 > jsonEnd) ? jsonEnd : json + 1;
}

static const char *JSON_SkipString(const char *json, const char *jsonEnd)
{
	for (json++; json < jsonEnd && *json != '"'; json++)
		if (*json == '\\')
			json++;

	return (json + 1 > jsonEnd) ? jsonEnd : json + 1;
}

static const char *JSON_SkipValue(const char *json, const char *jsonEnd)
{
	if (json >= jsonEnd)
		return jsonEnd;
	else if (*json == '"')
		json = JSON_SkipString(json, jsonEnd);
	else if (IS_STRUCT_OPEN(*json))
		json = JSON_SkipStruct(json, jsonEnd);
	else
	{
		while (json < jsonEnd && !IS_SEPARATOR(*json) && !IS_STRUCT_CLOSE(*json))
			json++;
	}

	return json;
}

// returns 0 if value requires more parsing, 1 if no more data/false/null, 2 if true
static unsigned int JSON_NoParse(const char *json, const char *jsonEnd)
{
	if (!json || json >= jsonEnd || *json == 'f' || *json == 'n')
		return 1;

	if (*json == 't')
		return 2;

	return 0;
}

// --------------------------------------------------------------------------
//   Array Functions
// --------------------------------------------------------------------------

const char *JSON_ArrayGetFirstValue(const char *json, const char *jsonEnd)
{
	if (!json || json >= jsonEnd || !IS_STRUCT_OPEN(*json))
		return NULL;

	json = JSON_SkipSeparators(json + 1, jsonEnd);

	return (json >= jsonEnd || IS_STRUCT_CLOSE(*json)) ? NULL : json;
}

const char *JSON_ArrayGetNextValue(const char *json, const char *jsonEnd)
{
	if (!json || json >= jsonEnd || IS_STRUCT_CLOSE(*json))
		return NULL;

	json = JSON_SkipValueAndSeparators(json, jsonEnd);

	return (json >= jsonEnd || IS_STRUCT_CLOSE(*json)) ? NULL : json;
}

unsigned int JSON_ArrayGetIndex(const char *json, const char *jsonEnd, const char **indexes, unsigned int numIndexes)
{
	unsigned int length = 0;

	for (json = JSON_ArrayGetFirstValue(json, jsonEnd); json; json = JSON_ArrayGetNextValue(json, jsonEnd))
	{
		if (indexes && numIndexes)
		{
			*indexes++ = json;
			numIndexes--;
		}
		length++;
	}

	return length;
}

const char *JSON_ArrayGetValue(const char *json, const char *jsonEnd, unsigned int index)
{
	for (json = JSON_ArrayGetFirstValue(json, jsonEnd); json && index; json = JSON_ArrayGetNextValue(json, jsonEnd))
		index--;

	return json;
}

// --------------------------------------------------------------------------
//   Object Functions
// --------------------------------------------------------------------------

const char *JSON_ObjectGetNamedValue(const char *json, const char *jsonEnd, const char *name)
{
	unsigned int nameLen = strlen(name);

	for (json = JSON_ArrayGetFirstValue(json, jsonEnd); json; json = JSON_ArrayGetNextValue(json, jsonEnd))
	{
		if (*json == '"')
		{
			const char *thisNameStart, *thisNameEnd;

			thisNameStart = json + 1;
			json = JSON_SkipString(json, jsonEnd);
			thisNameEnd = json - 1;
			json = JSON_SkipSeparators(json, jsonEnd);

			if ((unsigned int)(thisNameEnd - thisNameStart) == nameLen)
				if (strncmp(thisNameStart, name, nameLen) == 0)
					return json;
		}
	}

	return NULL;
}

// --------------------------------------------------------------------------
//   Value Functions
// --------------------------------------------------------------------------

unsigned int JSON_ValueGetType(const char *json, const char *jsonEnd)
{
	if (!json || json >= jsonEnd)
		return JSONTYPE_ERROR;
	else if (*json == '"')
		return JSONTYPE_STRING;
	else if (*json == '{')
		return JSONTYPE_OBJECT;
	else if (*json == '[')
		return JSONTYPE_ARRAY;

	return JSONTYPE_VALUE;
}

unsigned int JSON_ValueGetString(const char *json, const char *jsonEnd, char *outString, unsigned int stringLen)
{
	const char *stringEnd, *stringStart;

	if (!json)
	{
		*outString = '\0';
		return 0;
	}

	stringStart = json;
	stringEnd = JSON_SkipValue(stringStart, jsonEnd);
	if (stringEnd >= jsonEnd)
	{
		*outString = '\0';
		return 0;
	}

	// skip enclosing quotes if they exist
	if (*stringStart == '"')
		stringStart++;

	if (*(stringEnd - 1) == '"')
		stringEnd--;

	stringLen--;
	if (stringLen > stringEnd - stringStart)
		stringLen = stringEnd - stringStart;

	json = stringStart;
	while (stringLen--)
		*outString++ = *json++;
	*outString = '\0';

	return stringEnd - stringStart;
}

double JSON_ValueGetDouble(const char *json, const char *jsonEnd)
{
	char cValue[256];
	double dValue = 0.0;
	unsigned int np = JSON_NoParse(json, jsonEnd);

	if (np)
		return (double)(np - 1);

	if (!JSON_ValueGetString(json, jsonEnd, cValue, 256))
		return 0.0;

	sscanf(cValue, "%lf", &dValue);

	return dValue;
}

float JSON_ValueGetFloat(const char *json, const char *jsonEnd)
{
	char cValue[256];
	float fValue = 0.0f;
	unsigned int np = JSON_NoParse(json, jsonEnd);

	if (np)
		return (float)(np - 1);

	if (!JSON_ValueGetString(json, jsonEnd, cValue, 256))
		return 0.0f;

	sscanf(cValue, "%f", &fValue);

	return fValue;
}

int JSON_ValueGetInt(const char *json, const char *jsonEnd)
{
	char cValue[256];
	int iValue = 0;
	unsigned int np = JSON_NoParse(json, jsonEnd);

	if (np)
		return np - 1;

	if (!JSON_ValueGetString(json, jsonEnd, cValue, 256))
		return 0;

	sscanf(cValue, "%d", &iValue);

	return iValue;
}

#undef IS_SEPARATOR
#undef IS_STRUCT_OPEN
#undef IS_STRUCT_CLOSE