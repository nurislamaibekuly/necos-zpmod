// Diagnostic wrapper around pfnModelIndex. See the MODEL_INDEX definition in
// enginecallback.h for why this exists.
//
// This is instrumentation, not a fix. It is meant to be temporary: once the
// offending call site is identified it should be repaired properly and this
// file deleted, so that a bad name fails loudly again instead of silently
// resolving to model 0.

#include "extdll.h"
#include "util.h"
#include "cbase.h"
#include "enginecallback.h"
#include "eiface.h"

#include <unistd.h>
#include <stdio.h>
#include <stdint.h>

#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/mach_vm.h>
#endif

#define ZP_IDX_LOG "/tmp/zpmod_idx.log"

// A model name is a short string, so require the whole window to be inside a
// readable region. A partially mapped name still means we are not holding a
// string.
static const size_t kNameWindow = 64;

static bool ZPRegionIsReadable( const void *p, size_t need )
{
	if( !p )
		return false;

	const uintptr_t lo = (uintptr_t)p;
	const uintptr_t hi = lo + need;
	if( hi < lo ) // overflow
		return false;

#ifdef __APPLE__
	mach_vm_address_t addr = (mach_vm_address_t)p;
	mach_vm_size_t size = 0;
	vm_region_basic_info_data_64_t info;
	mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
	mach_port_t object_name = MACH_PORT_NULL;

	kern_return_t kr = mach_vm_region( mach_task_self(), &addr, &size,
		VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &count, &object_name );

	if( kr != KERN_SUCCESS )
		return false;

	// mach_vm_region reports the region containing addr, so addr has come back
	// already clamped up to the region base. Check we are inside it and that it
	// is readable.
	if( (mach_vm_address_t)p < addr )
		return false;
	if( (mach_vm_address_t)p + need > addr + size )
		return false;

	return ( info.protection & VM_PROT_READ ) != 0;
#else
	// Linux: scan /proc/self/maps for a read-permission mapping covering the
	// whole [p, p+need) window.
	FILE *fp = fopen( "/proc/self/maps", "r" );
	if( !fp )
		return false;

	char line[512];
	bool ok = false;
	while( fgets( line, sizeof(line), fp ) )
	{
		unsigned long a, b;
		char perms[8];
		if( sscanf( line, "%lx-%lx %7s", &a, &b, perms ) != 3 )
			continue;
		if( lo >= a && hi <= b && perms[0] == 'r' )
		{
			ok = true;
			break;
		}
	}
	fclose( fp );
	return ok;
#endif
}

// Append-only, unbuffered and fsync'd. We only ever write here when something is
// wrong, and we want the line on disk no matter what the process does next.
static void ZPLogBadIndex( const char *name, const void *caller )
{
	FILE *fp = fopen( ZP_IDX_LOG, "a" );
	if( !fp )
		return;

	fprintf( fp, "BAD MODEL_INDEX name=%p caller=%p stringbase=%p\n",
		(const void *)name, caller, (void *)gpGlobals->pStringBase );

	fflush( fp );
	fsync( fileno( fp ) );
	fclose( fp );
}

int ZPModelIndexGuarded( const char *name )
{
	if( ZPRegionIsReadable( name, kNameWindow ) )
		return g_engfuncs.pfnModelIndex( name );

	// Unreadable name: this is the crash. Record who did it, and do not call
	// through into strcasecmp with it.
	ZPLogBadIndex( name, __builtin_return_address( 0 ) );
	return 0;
}
