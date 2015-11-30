#ifndef MTRS_COMMON_H
# define MTRS_COMMON_H 1

/*****************************************************************************
 * OS-specific headers and thread types
 *****************************************************************************/
#if defined( WIN32 ) || defined( UNDER_CE )
#   define WIN32_LEAN_AND_MEAN

#	ifndef _WIN32_WINNT
#		define _WIN32_WINNT 0x0500
#	endif

#   include <windows.h>
#endif

/*****************************************************************************
 * Required headers
 *****************************************************************************/
#if defined( _MSC_VER )
//#   pragma warning( disable : 4244 )

// CRT's memory leak detection
#if defined(DEBUG) || defined(_DEBUG)
#	define _CRTDBG_MAP_ALLOC
#	include <stdlib.h>
#	include <crtdbg.h>
#else

#if (_MSC_VER >= 1400)
#	define strdup	_strdup
#	define getcwd	_getcwd
#endif

#endif

#endif


#include <stdlib.h>
#include <stdarg.h>

#include <string.h>
#include <stdio.h>
#include <inttypes.h>
#include <stddef.h>
#include <assert.h>

#include <errno.h>
#include <time.h>

#ifndef __cplusplus
# include <stdbool.h>
#endif

#ifndef WIN32
#include <unistd.h>
#define msleep(x) usleep(x*1000)
#endif

#define ANC_UNUSED(x) (void)(x)

#if defined(DEBUG) || defined(_DEBUG)
#define  LOGI(...)	printf(__VA_ARGS__)
#define  LOGE(...)	printf(__VA_ARGS__)
#define  LOGW(...)	printf(__VA_ARGS__)
#define  LOGD(...)	printf(__VA_ARGS__)
#else
#define  LOGI(...) printf(__VA_ARGS__)
#define  LOGE(...) printf(__VA_ARGS__)
#define  LOGW(...)
#define  LOGD(...)
#endif

#define CLOCK_FREQ INT64_C(1000000)

#endif
