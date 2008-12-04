/***************************************************************
 *
 * Copyright (C) 1990-2007, Condor Team, Computer Sciences Department,
 * University of Wisconsin-Madison, WI.
 * 
 * Licensed under the Apache License, Version 2.0 (the "License"); you
 * may not use this file except in compliance with the License.  You may
 * obtain a copy of the License at
 * 
 *    http://www.apache.org/licenses/LICENSE-2.0
 * 
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 ***************************************************************/


#include "condor_common.h"
#include "directory.h"
#include "condor_daemon_core.h"
#include "condor_timer_manager.h"
#include "condor_lock_file.h"
#include "condor_netdb.h"

// Check URL static method
int
CondorLockFile::Rank( const char *lock_url )
{
#ifdef WIN32
	return 0;
#else
		// Identify it

		// ** This is a quick hack & probably should be improved **
	if ( strncmp( lock_url, "file:", 5 ) ) {
		dprintf( D_FULLDEBUG, "CondorLockFile: '%s': Not a file URL\n",
				 lock_url );
		return 0;		// We don't handle this lock
	}

		// Verify that it's a valid directory
	const char	*dirpath = (lock_url + 5);
	StatInfo	statinfo( dirpath );
	if ( statinfo.Error() ) {
		dprintf( D_FULLDEBUG, "CondorLockFile: '%s' does not exist\n",
				 dirpath );
		return 0;		// We don't handle this lock
	}
	if ( ! statinfo.IsDirectory() ) {
		dprintf( D_FULLDEBUG, "CondorLockFile: '%s' is not a directory\n",
				 dirpath );
		return 0;		// We don't handle this lock
	}

	// Note: We probably should verify that we can write to this directory

		// I like it
	return 100;
#endif
}

// Build a file-based lock
CondorLockImpl *
CondorLockFile::Construct( const char	*lock_url,
						   const char	*lock_name,
						   Service		*app_service,
						   LockEvent	lock_event_acquired,
						   LockEvent	lock_event_lost,
						   time_t		poll_period,
						   time_t		lock_hold_time,
						   bool			auto_update )
{
	CondorLockImpl	*newlock = new CondorLockFile(
		lock_url, lock_name, app_service, lock_event_acquired,
		lock_event_lost, poll_period, lock_hold_time, auto_update );
	return newlock;
}

// Basic CondorLockFile constructor
CondorLockFile::CondorLockFile( )
		: CondorLockImpl( )
{
	// Do nothing
}

// Basic CondorLockFile constructor
CondorLockFile::CondorLockFile( const char	*l_url,
								const char	*l_name,
								Service		*ap_service,
								LockEvent	le_acquired,
								LockEvent	le_lost,
								time_t		poll_per,
								time_t		lock_ht,
								bool		auto_up )
		: CondorLockImpl( ap_service, le_acquired, le_lost,
						  poll_per, lock_ht, auto_up )
{
	if ( BuildLock( l_url, l_name ) ) {
		EXCEPT( "Error building lock for URL '%s'", l_url );
	}
}

// Destructor
CondorLockFile::~CondorLockFile( void )
{
	(void) FreeLock( );
}

// Shared initialization code
int
CondorLockFile::BuildLock( const char	*l_url,
						   const char	*l_name )
{
#ifdef WIN32
	dprintf( D_ALWAYS, "File locks not supported under Windows\n" );
	return -1;
#endif

		// Verify the rank
	if ( Rank( l_url ) <= 0 ) {
		return -1;
	}

		// Copy the URL & name out
	this->lock_url = l_url;
	this->lock_name = l_name;

		// Create the lock file name from it
	lock_file.sprintf( "%s/%s.lock", l_url + 5, l_name );

		// Build a temporary file name
	char	hostname[128];
	if ( condor_gethostname( hostname, sizeof( hostname ) ) ) {
		sprintf( hostname, "unknown-%d", rand( ) );
	}
	temp_file.sprintf( "%s.%s-%d", lock_file.Value(), hostname, getpid( ) );

	dprintf( D_FULLDEBUG, "HA Lock Init: lock file='%s'\n",
			 lock_file.Value() );
	dprintf( D_FULLDEBUG, "HA Lock Init: temp file='%s'\n",
			 temp_file.Value() );

		// Build the lock internals
	return ImplementLock( );

}

// Low level function to change the URL / name of the lock
//  Returns:
//   0: No change / ok
//  -1: Fatal error
//   1: Can't change w/o rebuilding
int 
CondorLockFile::ChangeUrlName( const char *l_url,
							   const char *l_name )
{
	// I don't handle any changes well, at least for now..
	if ( this->lock_url != l_url ) {
		dprintf( D_ALWAYS, "Lock URL Changed -> '%s'\n", l_name );
		return 1;
	}
	if ( this->lock_name != l_name ) {
		dprintf( D_ALWAYS, "Lock name Changed -> '%s'\n", l_name );
		return 1;
	}

	// Otherwiwse, ok
	return 0;
}

// Internal "get lock"
// Return values:
//		 0: Ok, lock aquired
//		 1: Ok, lock busy, not aquired
//		-1: Failure
int
CondorLockFile::GetLock( time_t lock_ht )
{
#ifdef WIN32
	return -1;
#else
	int				status;

		// Step 1: Stat the lock file
	struct stat		statbuf;
	status = stat( lock_file.Value( ), &statbuf );
	if ( 0 == status ) {
		time_t	expire = statbuf.st_mtime;
		time_t	now = time( NULL );

			// If the lock has expired, kill it!
		if ( now >= expire ) {
			dprintf( D_ALWAYS,
					 "GetLock warning: Expired lock found '%s'\n",
					 lock_file.Value( ) );

				// Expired lock found, remove it
			status = unlink( lock_file.Value( ) );

				// Ok if somebody else killed it
			if ( status && ( ENOENT != errno ) ) {
				dprintf( D_ALWAYS,
						 "GetLock warning: Error expiring lock: %d %s\n",
						 errno, strerror( errno ) );
			}
		} else {
			return 1;
		}

		// ENOENT is ok, but other errors are bad
	} else if ( ENOENT != errno ) {
		dprintf( D_FULLDEBUG,
				 "GetLock: Error stating lock file '%s': %d %s\n",
				 lock_file.Value( ), errno, strerror( errno ) );
		return -1;
	}

		// At this point in time, we believe that the lock file
		// does not exist.  Proceed.

		// Step 2: Create the temporary file
	int		fd = creat( temp_file.Value( ), S_IRWXU );
	if ( fd < 0 ) {
		dprintf( D_ALWAYS,
				 "GetLock: Error creating temp lock file '%s': %d %s\n",
				 temp_file.Value( ), errno, strerror( errno ) );
		return -1;
	}
	close( fd );

		// Step 3: Set the expiration time
	status = SetExpireTime( temp_file.Value( ), lock_ht );
	if ( status ) {
		dprintf( D_ALWAYS, "GetLock: Error setting expiration time" );
		unlink( temp_file.Value( ) );
		return -1;
	}

		// Step 4: Run the link race...
	status = link( temp_file.Value( ), lock_file.Value( ) );
	(void) unlink( temp_file.Value( ) );

		// Now, let's see how our car finished
	if ( 0 == status ) {				// We won!
		return 0;
	} else if ( EEXIST == errno ) {		// Somebody else won the race
		dprintf( D_FULLDEBUG, "GetLock: Lock held by somebody else\n" );
		return 1;
	} else {							// Some other error occurred
		dprintf( D_ALWAYS,
				 "GetLock: Error linking '%s' to lock file '%s': %d %s\n",
				 temp_file.Value(), lock_file.Value( ),
				 errno, strerror( errno ) );
		return -1;
	}
#endif
}

// Update the lock's timestamp
int
CondorLockFile::UpdateLock( time_t lock_ht )
{
#ifdef WIN32
	return -1;
#else
	return SetExpireTime( lock_file.Value( ), lock_ht );
#endif
}

// Release the lock
int
CondorLockFile::FreeLock( void )
{
#ifdef WIN32
	return -1;
#else
	int		status = unlink( lock_file.Value( ) );

	if ( status ) {
		dprintf( D_ALWAYS, "FreeLock: Error unlink lock '%s': %d %s\n",
				 lock_file.Value(), errno, strerror( errno ) );
	} else {
		dprintf( D_FULLDEBUG, "FreeLock: Lock unlinked ok\n" );
	}

		// In either case, return ok to the application
	return 0;
#endif
}

// Update the lock's timestamp
int
CondorLockFile::SetExpireTime( const char *file, time_t lock_ht )
{
#ifdef WIN32
	return -1;
#else
	time_t			expire = time( NULL ) + lock_ht;
	struct utimbuf	timebuf;

		// First, update the file
	timebuf.actime = expire;
	timebuf.modtime = expire;
	int		status = utime( file, &timebuf );
	if ( status ) {
		dprintf( D_ALWAYS, "UpdateLock: Error updating '%s': %d %s\n",
				 file, errno, strerror( errno ) );
		return -1;
	}

		// Stat it to verify
	struct stat		statbuf;
	status = stat( file, &statbuf );
	if ( 0 != status ) {
		dprintf( D_ALWAYS,
				 "UpdateLock: Error stating lock file '%s': %d %s\n",
				 lock_file.Value(), errno, strerror( errno ) );
		return -1;
	}
	if ( statbuf.st_mtime != expire ) {
		dprintf( D_ALWAYS,
				 "UpdateLock: lock file '%s' utime wrong (%ld != %ld)\n",
				 file, (long) expire, (long) statbuf.st_mtime );
		return -1;
	}

		// All done
	return 0;
#endif
}