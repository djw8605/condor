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
#include "io_loop.h"
#include "condor_debug.h"
#include "condor_config.h"
#include "condor_daemon_core.h"
#include "PipeBuffer.h"
#include "globus_utils.h"
#include "subsystem_info.h"
#include "file_transfer.h"
#include "openssl/md5.h"
#include "directory.h"
#include "_unordered_map.h"
#include "basename.h"
#include "my_username.h"


const char * version = "$GahpVersion 2.0.1 Jul 30 2012 Condor_FT_GAHP $";

int async_mode = 0;
int new_results_signaled = 0;

int flush_request_tid = -1;

int download_sandbox_reaper_id = -1;
int upload_sandbox_reaper_id = -1;
int destroy_sandbox_reaper_id = -1;

// The list of results ready to be output to IO
StringList result_list;

// pipe buffers
PipeBuffer stdin_buffer; 

// this appears at the bottom of this file
extern "C" int display_dprintf_header(char **buf,int *bufpos,int *buflen);

#ifdef WIN32
int STDIN_FILENO = fileno(stdin);
#endif


void
usage()
{
	dprintf( D_ALWAYS,
		"Usage: condor_ft-gahp\n"
			 );
	DC_Exit( 1 );
}

int
default_reaper(Service *, int pid, int exit_status)
{
	dprintf( D_ALWAYS, "child %d exited with status %d\n", pid, exit_status );
	return 0;
}

void
main_init( int, char ** const)
{
	dprintf(D_ALWAYS, "FT-GAHP IO thread\n");
	dprintf(D_SECURITY | D_FULLDEBUG, "FT-GAHP IO thread\n");

	int stdin_pipe = -1;
#if defined(WIN32)
	// if our parent is not DaemonCore, then our assumption that
	// the pipe we were passed in via stdin is overlapped-mode
	// is probably wrong. we therefore create a new pipe with the
	// read end overlapped and start a "forwarding thread"
	if (daemonCore->InfoCommandSinfulString(daemonCore->getppid()) == NULL) {

		dprintf(D_FULLDEBUG, "parent is not DaemonCore; creating a forwarding thread\n");

		int pipe_ends[2];
		if (daemonCore->Create_Pipe(pipe_ends, true) == FALSE) {
			EXCEPT("failed to create forwarding pipe");
		}
		forwarding_pipe = pipe_ends[1];
		HANDLE thread_handle = (HANDLE)_beginthreadex(NULL,                   // default security
		                                              0,                      // default stack size
		                                              pipe_forward_thread,    // start function
		                                              NULL,                   // arg: write end of pipe
		                                              0,                      // not suspended
													  NULL);                  // don't care about the ID
		if (thread_handle == NULL) {
			EXCEPT("failed to create forwarding thread");
		}
		CloseHandle(thread_handle);
		stdin_pipe = pipe_ends[0];
	}
#endif

	if (stdin_pipe == -1) {
		// create a DaemonCore pipe from our stdin
		stdin_pipe = daemonCore->Inherit_Pipe(fileno(stdin),
		                                      false,    // read pipe
		                                      true,     // registerable
		                                      false);   // blocking
	}

	stdin_buffer.setPipeEnd(stdin_pipe);

	(void)daemonCore->Register_Pipe (stdin_buffer.getPipeEnd(),
					"stdin pipe",
					(PipeHandler)&stdin_pipe_handler,
					"stdin_pipe_handler");

	// Setup dprintf to display pid
	DebugId = display_dprintf_header;

		// Print out the GAHP version to the screen
		// now we're ready to roll
	printf ("%s\n", version);
	fflush(stdout);
	dprintf(D_FULLDEBUG,"put stdout: %s\n",version);

	download_sandbox_reaper_id = daemonCore->Register_Reaper(
				"download_sandbox_reaper",
				&download_sandbox_reaper,
				"download_sandbox",
				NULL
				);
	dprintf(D_FULLDEBUG, "registered download_sandbox_reaper() at %i\n", download_sandbox_reaper_id);

	upload_sandbox_reaper_id = daemonCore->Register_Reaper(
				"upload_sandbox_reaper",
				&upload_sandbox_reaper,
				"upload_sandbox",
				NULL
				);
	dprintf(D_FULLDEBUG, "registered upload_sandbox_reaper() at %i\n", upload_sandbox_reaper_id);

	destroy_sandbox_reaper_id = daemonCore->Register_Reaper(
				"destroy_sandbox_reaper",
				&destroy_sandbox_reaper,
				"destroy_sandbox",
				NULL
				);
	dprintf(D_FULLDEBUG, "registered destroy_sandbox_reaper() at %i\n", destroy_sandbox_reaper_id);

	dprintf (D_FULLDEBUG, "FT-GAHP IO initialized\n");
}


int
stdin_pipe_handler(Service*, int) {

	std::string* line;
	while ((line = stdin_buffer.GetNextLine()) != NULL) {

		const char * command = line->c_str();

		dprintf (D_ALWAYS, "got stdin: %s\n", command);

		Gahp_Args args;

		if (parse_gahp_command (command, &args) &&
			verify_gahp_command (args.argv, args.argc)) {

				// Catch "special commands first
			if (strcasecmp (args.argv[0], GAHP_COMMAND_RESULTS) == 0) {
					// Print number of results
				std::string rn_buff;
				sprintf( rn_buff, "%d", result_list.number() );
				const char * commands [] = {
					GAHP_RESULT_SUCCESS,
					rn_buff.c_str() };
				gahp_output_return (commands, 2);

					// Print each result line
				char * next;
				result_list.rewind();
				while ((next = result_list.next()) != NULL) {
					printf ("%s\n", next);
					fflush(stdout);
					dprintf(D_FULLDEBUG,"put stdout: %s\n",next);
					result_list.deleteCurrent();
				}

				new_results_signaled = FALSE;
			} else if (strcasecmp (args.argv[0], GAHP_COMMAND_VERSION) == 0) {
				printf ("S %s\n", version);
				fflush (stdout);
				dprintf(D_FULLDEBUG,"put stdout: S %s\n",version);
			} else if (strcasecmp (args.argv[0], GAHP_COMMAND_QUIT) == 0) {
				gahp_output_return_success();
				DC_Exit(0);
			} else if (strcasecmp (args.argv[0], GAHP_COMMAND_ASYNC_MODE_ON) == 0) {
				async_mode = TRUE;
				new_results_signaled = FALSE;
				gahp_output_return_success();
			} else if (strcasecmp (args.argv[0], GAHP_COMMAND_ASYNC_MODE_OFF) == 0) {
				async_mode = FALSE;
				gahp_output_return_success();
			} else if (strcasecmp (args.argv[0], GAHP_COMMAND_QUIT) == 0) {
				gahp_output_return_success();
				return 0; // exit
			} else if (strcasecmp (args.argv[0], GAHP_COMMAND_COMMANDS) == 0) {
				const char * commands [] = {
					GAHP_RESULT_SUCCESS,
					GAHP_COMMAND_DOWNLOAD_SANDBOX,
					GAHP_COMMAND_UPLOAD_SANDBOX,
					GAHP_COMMAND_DESTROY_SANDBOX,
					GAHP_COMMAND_ASYNC_MODE_ON,
					GAHP_COMMAND_ASYNC_MODE_OFF,
					GAHP_COMMAND_RESULTS,
					GAHP_COMMAND_QUIT,
					GAHP_COMMAND_VERSION,
					GAHP_COMMAND_COMMANDS};
				gahp_output_return (commands, 10);
			} else if (strcasecmp (args.argv[0], GAHP_COMMAND_DOWNLOAD_SANDBOX) == 0) {

				int tid = daemonCore->Create_Thread(do_command_download_sandbox, (void*)strdup(command), NULL, download_sandbox_reaper_id);

				if( tid ) {
					dprintf (D_ALWAYS, "BOSCO: created download_sandbox thread, id: %i\n", tid);

					// this is a "success" in the sense that the gahp command was
					// well-formatted.  whether or not the file transfer works or
					// not is not what we are reporting here.
					gahp_output_return_success();

					SandboxEnt e;
					e.pid = tid;
					e.request_id = args.argv[1];
					e.sandbox_id = args.argv[2];
					// transfer started, record the entry in the map
					std::pair<int, struct SandboxEnt> p(tid, e);
					sandbox_map.insert(p);
				} else {
					dprintf (D_ALWAYS, "BOSCO: Create_Thread FAILED!\n");
					gahp_output_return_success();
				}

			} else if (strcasecmp (args.argv[0], GAHP_COMMAND_UPLOAD_SANDBOX) == 0) {

				int tid = daemonCore->Create_Thread(do_command_upload_sandbox, (void*)strdup(command), NULL, upload_sandbox_reaper_id);

				if( tid ) {
					dprintf (D_ALWAYS, "BOSCO: created upload_sandbox thread, id: %i\n", tid);

					// this is a "success" in the sense that the gahp command was
					// well-formatted.  whether or not the file transfer works or
					// not is not what we are reporting here.
					gahp_output_return_success();

					SandboxEnt e;
					e.pid = tid;
					e.request_id = args.argv[1];
					e.sandbox_id = args.argv[2];
					// transfer started, record the entry in the map
					std::pair<int, struct SandboxEnt> p(tid, e);
					sandbox_map.insert(p);
				} else {
					dprintf (D_ALWAYS, "BOSCO: Create_Thread FAILED!\n");
					gahp_output_return_success();
				}

			} else if (strcasecmp (args.argv[0], GAHP_COMMAND_DESTROY_SANDBOX) == 0) {

				int tid = daemonCore->Create_Thread(do_command_destroy_sandbox, (void*)strdup(command), NULL, destroy_sandbox_reaper_id);

				if( tid ) {
					dprintf (D_ALWAYS, "BOSCO: created destroy_sandbox thread, id: %i\n", tid);

					// this is a "success" in the sense that the gahp command was
					// well-formatted.  whether or not the file transfer works or
					// not is not what we are reporting here.
					gahp_output_return_success();

					SandboxEnt e;
					e.pid = tid;
					e.request_id = args.argv[1];
					e.sandbox_id = args.argv[2];
					// transfer started, record the entry in the map
					std::pair<int, struct SandboxEnt> p(tid, e);
					sandbox_map.insert(p);
				} else {
					dprintf (D_ALWAYS, "BOSCO: Create_Thread FAILED!\n");
					gahp_output_return_success();
				}

			} else {
				// should never get here if verify does its job
				dprintf(D_ALWAYS, "FTGAHP: got bad command: %s\n", args.argv[0]);
				gahp_output_return_error();
			}
			
		} else {
			gahp_output_return_error();
		}

		delete line;
	}

	// check if GetNextLine() returned NULL because of an error or EOF
	if (stdin_buffer.IsError() || stdin_buffer.IsEOF()) {
		dprintf (D_ALWAYS, "stdin buffer closed, exiting\n");
		DC_Exit (1);
	}

	return TRUE;
}


void
handle_results( std::string line ) {
		// Add this to the list
	result_list.append (line.c_str());

	if (async_mode) {
		if (!new_results_signaled) {
			printf ("R\n");
			fflush (stdout);
			dprintf(D_FULLDEBUG,"put stdout: R\n");
		}
		new_results_signaled = TRUE;	// So that we only do it once
	}
}


// Check the parameters to make sure the command
// is syntactically correct
int
verify_gahp_command(char ** argv, int argc) {

	if (strcasecmp (argv[0], GAHP_COMMAND_DOWNLOAD_SANDBOX) ==0 ||
		strcasecmp (argv[0], GAHP_COMMAND_UPLOAD_SANDBOX) ==0 ||
		strcasecmp (argv[0], GAHP_COMMAND_DESTROY_SANDBOX) ==0) {
		// Expecting:GAHP_COMMAND_*_SANDBOX <req_id> <sandbox_id> <job_ad>
		return verify_number_args (argc, 4) &&
				verify_request_id (argv[1]);
	} else if (strcasecmp (argv[0], GAHP_COMMAND_RESULTS) == 0 ||
				strcasecmp (argv[0], GAHP_COMMAND_VERSION) == 0 ||
				strcasecmp (argv[0], GAHP_COMMAND_COMMANDS) == 0 ||
				strcasecmp (argv[0], GAHP_COMMAND_QUIT) == 0 ||
				strcasecmp (argv[0], GAHP_COMMAND_ASYNC_MODE_ON) == 0 ||
				strcasecmp (argv[0], GAHP_COMMAND_ASYNC_MODE_OFF) == 0) {
	    // These are no-arg commands
	    return verify_number_args (argc, 1);
	}

	dprintf (D_ALWAYS, "Unknown command\n");

	return FALSE;
}

int
verify_number_args (const int is, const int should_be) {
	if (is != should_be) {
		dprintf (D_ALWAYS, "Wrong # of args %d, should be %d\n", is, should_be);
		return FALSE;
	}
	return TRUE;
}

int
verify_request_id (const char * s) {
    unsigned int i;
	for (i=0; i<strlen(s); i++) {
		if (!isdigit(s[i])) {
			dprintf (D_ALWAYS, "Bad request id %s\n", s);
			return FALSE;
		}
	}

	return TRUE;
}

int
verify_number (const char * s) {
	if (!s || !(*s))
		return FALSE;
	
	int i=0;
   
	do {
		if (s[i]<'0' || s[i]>'9')
			return FALSE;
	} while (s[++i]);

	return TRUE;
}
		

void
gahp_output_return (const char ** results, const int count) {
	int i=0;
	std::string buff;

	for (i=0; i<count; i++) {
		buff+=results[i];
		if (i < (count - 1 )) {
			buff+=' ';
		}
	}

	buff += '\n';
	printf ("%s", buff.c_str());
	fflush(stdout);
	dprintf(D_FULLDEBUG,"put stdout: %s",buff.c_str());
}

void
gahp_output_return_success() {
	const char* result[] = {GAHP_RESULT_SUCCESS};
	gahp_output_return (result, 1);
}

void
gahp_output_return_error() {
	const char* result[] = {GAHP_RESULT_ERROR};
	gahp_output_return (result, 1);
}

void
main_config()
{
}

void
main_shutdown_fast()
{
}

void
main_shutdown_graceful()
{
	daemonCore->Cancel_And_Close_All_Pipes();
}

void
main_pre_command_sock_init( )
{
	daemonCore->WantSendChildAlive( false );
}

int
main( int argc, char **argv )
{
	set_mySubSystem("FT_GAHP", SUBSYSTEM_TYPE_GAHP);

	dc_main_init = main_init;
	dc_main_config = main_config;
	dc_main_shutdown_fast = main_shutdown_fast;
	dc_main_shutdown_graceful = main_shutdown_graceful;
	dc_main_pre_command_sock_init = main_pre_command_sock_init;
	return dc_main( argc, argv );
}

// This function is called by dprintf - always display our pid in our
// log entries.
extern "C"
int
display_dprintf_header(char **buf,int *bufpos,int *buflen)
{
	static pid_t mypid = 0;

	if (!mypid) {
		mypid = daemonCore->getpid();
	}

	return sprintf_realloc( buf, bufpos, buflen, "[%ld] ", (long)mypid );
}

void
enqueue_result (std::string req_id, const char ** results, const int argc)
{
	std::string buffer;

	buffer = req_id;

	for ( int i = 0; i < argc; i++ ) {
		buffer += ' ';
		if ( results[i] == NULL ) {
			buffer += "NULL";
		} else {
			for ( int j = 0; results[i][j] != '\0'; j++ ) {
				switch ( results[i][j] ) {
				case ' ':
				case '\\':
				case '\r':
				case '\n':
					buffer += '\\';
				default:
					buffer += results[i][j];
				}
			}
		}
	}
	handle_results( buffer );
}

void
enqueue_result (int req_id, const char ** results, const int argc)
{
	std::string buffer;

	// how is this legit??!?  ZKM
	sprintf( buffer, "%d", req_id );

	for ( int i = 0; i < argc; i++ ) {
		buffer += ' ';
		if ( results[i] == NULL ) {
			buffer += "NULL";
		} else {
			for ( int j = 0; results[i][j] != '\0'; j++ ) {
				switch ( results[i][j] ) {
				case ' ':
				case '\\':
				case '\r':
				case '\n':
					buffer += '\\';
				default:
					buffer += results[i][j];
				}
			}
		}
	}
	handle_results( buffer );
}


bool
create_sandbox_dir(std::string sid, std::string &iwd)
{
	define_sandbox_path(sid, iwd);

	dprintf(D_ALWAYS, "BOSCO: create, sandbox path: %s\n", iwd.c_str());

	// who are we? need to change priv?
	if (mkdir_and_parents_if_needed(iwd.c_str(), 0700)) {
		return true;
	} else {
		return false;
	}
}


// IT IS legitimate for the command DESTROY_SANDBOX to happen while a file
// transfer is currently happening.  so, tear it down properly to avoid any
// memory leaks.  this means looking it up in our map, which should be O(1),
// and then removing the actual filesystem portion.
bool
destroy_sandbox(std::string sid, std::string &err)
{
	dprintf(D_ALWAYS, "BOSCO: destroy, sandbox id: %s\n", sid.c_str());

	std::string iwd;
	define_sandbox_path(sid, iwd);

	dprintf(D_ALWAYS, "BOSCO: destroy, sandbox path: %s\n", iwd.c_str());

	// remove (rm -rf) the sandbox dir
	char *buff = condor_dirname( iwd.c_str() );
	std::string parent_dir = buff;
	free( buff );
	buff = condor_dirname( parent_dir.c_str() );
	std::string gparent_dir = buff;
	free( buff );

	dprintf( D_FULLDEBUG, "parent_dir: %s\n", parent_dir.c_str() );
	dprintf( D_FULLDEBUG, "gparent_dir: %s\n", gparent_dir.c_str() );

	Directory d( parent_dir.c_str() );
	if ( !d.Remove_Full_Path( iwd.c_str() ) ) {
		dprintf(D_ALWAYS, "Failed to remove %s\n", iwd.c_str());
		// TODO Should we return failure?
	}
	d.Rewind();
	if ( d.Next() == NULL ) {
		dprintf(D_FULLDEBUG, "Removing empty directory %s\n", parent_dir.c_str());
		Directory d2( gparent_dir.c_str() );
		if ( !d2.Remove_Full_Path( parent_dir.c_str() ) ) {
			dprintf(D_ALWAYS, "Failed to remove %s\n", parent_dir.c_str());
			// TODO Should we return failure?
		}
		d2.Rewind();
		if ( d2.Next() == NULL ) {
			dprintf(D_FULLDEBUG, "Removing empty directory %s\n", gparent_dir.c_str());
			if ( !d2.Remove_Full_Path( gparent_dir.c_str() ) ) {
				dprintf(D_ALWAYS, "Failed to remove %s\n", gparent_dir.c_str());
				// TODO Should we return failure?
			}
		}
	}

	err = "NULL";
	return true;
}


// input:
//   sid:  the sandbox id as a string
// in/output:
//   &path: the path it would result in
void
define_sandbox_path(std::string sid, std::string &path)
{

	// find a suitable path for our sandbox
	char* t_path = NULL;
	if(!t_path) {
		t_path = param("BOSCO_SANDBOX_DIR");
	}
	if(!t_path) {
		// this is probably stupid, as it may get "cleaned up" by condor_preen
		// and it it assumes that execute dir is writable by the end user.  and
		// probably we can't write to it as a user, so hopefully it is defined
		// per-user, or not at all.
		t_path = param("EXECUTE");
	}
	if(!t_path) {
		// this is probably stupid as well, since it may quickly fill up.  and
		// without username as part of the path, there are potential collisions
		// on sandbox ids.  when someone has a better default, please insert.

		t_path = strdup("/tmp");
	}

	// whatever path we decided will reside in the reference-passwed "path"
	path = t_path;
	free(t_path);


	// hash the id into ascii.  MD5 is fine here, because we use the actual
	// sandbox id as part of the path, thus making it immune to MD5 collisions.
	// we're only using it to keep filesystems free of directories that contain
	// too many files.  in the year 2040, when 2^16 files exist in a single
	// directory, feel free to extend this to 3, or (log_v2 n)
	unsigned char hash_buf[MD5_DIGEST_LENGTH];
	MD5((unsigned char*)(sid.c_str()), sid.length(), hash_buf);

	char c_hex_md5[MD5_DIGEST_LENGTH*2+1];
	for (int i = 0; i < MD5_DIGEST_LENGTH; i++) {
		sprintf(&(c_hex_md5[i*2]), "%02x", hash_buf[i]);
	}
	c_hex_md5[MD5_DIGEST_LENGTH*2] = 0;

	// now construct a two-level directory from the hash.  this potential loop
	// is unrolled, because MD5_DIGEST_LENGTH is currently (and likely always
	// will be 16).  see above comment.
	path += DIR_DELIM_CHAR;
	path += c_hex_md5[0];
	path += c_hex_md5[1];
	path += c_hex_md5[2];
	path += c_hex_md5[3];
	path += DIR_DELIM_CHAR;
	path += c_hex_md5[0];
	path += c_hex_md5[1];
	path += c_hex_md5[2];
	path += c_hex_md5[3];
	path += c_hex_md5[4];
	path += c_hex_md5[5];
	path += c_hex_md5[6];
	path += c_hex_md5[7];
	path += DIR_DELIM_CHAR;
	path += sid;

	// no return value.  setting the passed-by-reference parameter 'path'
	// is all we do in this function
}

int do_command_download_sandbox(void *arg, Stream*) {
	dprintf(D_ALWAYS, "FTGAHP: download sandbox\n");

	Gahp_Args args;
	parse_gahp_command ((char*)arg, &args);

	// first two args: result id and sandbox id:
	std::string rid = args.argv[1];
	std::string sid = args.argv[2];

	// third arg: job ad
	ClassAd ad;
	classad::ClassAdParser my_parser;

	if (!(my_parser.ParseClassAd(args.argv[3], ad))) {
		// FAIL
		return 1;
	}

	// first, create the directory that will be IWD.  returns the
	// directory created.
	std::string iwd;
	bool success;
	success = create_sandbox_dir(sid, iwd);
	if (!success) {
		// FAIL
		return 1;
	}

	// rewrite the IWD to the newly created sandbox dir
	ad.Assign(ATTR_JOB_IWD, iwd.c_str());
	char ATTR_SANDBOX_ID[] = "SandboxId";
	ad.Assign(ATTR_SANDBOX_ID, sid.c_str());

	// directory was created, let's set up the FileTransfer object
	FileTransfer ft;

	if (!ft.Init(&ad)) {
		// FAIL
		return 1;
	}

	// lookup ATTR_VERSION and set it.  this changes the wire
	// protocol and it is important that this happens before
	// calling DownloadFiles.
	char* peer_version = NULL;
	ad.LookupString(ATTR_VERSION, &peer_version);
	ft.setPeerVersion(peer_version);
	free (peer_version);

	dprintf(D_ALWAYS, "BOSCO: calling Download files\n");

	// the "true" param to DownloadFiles here means blocking (i.e. "in the foreground")
	if (!ft.DownloadFiles(true)) {
		// FAIL
		return 1;
	}

	// SUCCEED
	return 0;
}

int do_command_upload_sandbox(void *arg, Stream*) {
	dprintf(D_ALWAYS, "FTGAHP: upload sandbox\n");

	Gahp_Args args;
	parse_gahp_command ((char*)arg, &args);

	// first two args: result id and sandbox id:
	std::string rid = args.argv[1];
	std::string sid = args.argv[2];

	// third arg: job ad
	ClassAd ad;
	classad::ClassAdParser my_parser;

	if (!(my_parser.ParseClassAd(args.argv[3], ad))) {
		// FAIL
		return 1;
	}

	// rewrite the IWD to the actual sandbox dir
	std::string iwd;
	define_sandbox_path(sid, iwd);
	ad.Assign(ATTR_JOB_IWD, iwd.c_str());
	char ATTR_SANDBOX_ID[] = "SandboxId";
	ad.Assign(ATTR_SANDBOX_ID, sid.c_str());

	// directory was created, let's set up the FileTransfer object
	FileTransfer ft;

	if (!ft.Init(&ad)) {
		// FAIL
		return 1;
	}

	// lookup ATTR_VERSION and set it.  this changes the wire
	// protocol and it is important that this happens before
	// calling UploadFiles.
	char* peer_version = NULL;
	ad.LookupString(ATTR_VERSION, &peer_version);
	ft.setPeerVersion(peer_version);
	free (peer_version);

	dprintf(D_ALWAYS, "BOSCO: calling upload files\n");

	// the "true" param to UploadFiles here means blocking (i.e. "in the foreground")
	if (!ft.UploadFiles(true)) {
		// FAIL
		return 1;
	}

	// SUCCEED
	return 0;

}

int do_command_destroy_sandbox(void *arg, Stream*) {

	dprintf(D_ALWAYS, "FTGAHP: destroy sandbox\n");

	Gahp_Args args;
	parse_gahp_command ((char*)arg, &args);

	// first two args: result id and sandbox id:
	std::string rid = args.argv[1];
	std::string sid = args.argv[2];

	std::string err;
	if(!destroy_sandbox(sid, err)) {
		// FAIL
		dprintf( D_ALWAYS, "BOSCO: destroy_sandbox error: %s\n", err.c_str());
		return 1;
	}

	// SUCCEED
	return 0;
}


int download_sandbox_reaper(Service*, int pid, int status) {

	// map pid to the SandboxEnt stucture we have recorded
	SandboxMap::iterator i;
	i = sandbox_map.find(pid);
	SandboxEnt e = i->second;

	if (status == 0) {
		std::string path;
		define_sandbox_path(e.sandbox_id, path);

		const char * res[2] = {
			"NULL",
			path.c_str()
		};

		enqueue_result(e.request_id, res, 2);
	} else {
		const char * res[2] = {
			"ERROR",
			"NULL"
		};
		enqueue_result(e.request_id, res, 2);
	}

	// remove from the map
	sandbox_map.erase(pid);

	return 0;
}

int upload_sandbox_reaper(Service*, int pid, int status) {

	// map pid to the SandboxEnt stucture we have recorded
	SandboxMap::iterator i;
	i = sandbox_map.find(pid);
	SandboxEnt e = i->second;

	if(status == 0) {
		const char * res[1] = {
			"NULL"
		};
		enqueue_result(e.request_id, res, 1);
	} else {
		const char * res[1] = {
			"ERROR"
		};
		enqueue_result(e.request_id, res, 1);
	}

	// remove from the map
	sandbox_map.erase(pid);

	return 0;
}


int destroy_sandbox_reaper(Service*, int pid, int status) {

	// map pid to the SandboxEnt stucture we have recorded
	SandboxMap::iterator i;
	i = sandbox_map.find(pid);
	SandboxEnt e = i->second;

	if(status == 0) {
		const char * res[1] = {
			"NULL"
		};
		enqueue_result(e.request_id, res, 1);
	} else {
		const char * res[1] = {
			"ERROR"
		};
		enqueue_result(e.request_id, res, 1);
	}

	// remove from the map
	sandbox_map.erase(pid);

	return 0;
}

