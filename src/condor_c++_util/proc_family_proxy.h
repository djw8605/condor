/***************************Copyright-DO-NOT-REMOVE-THIS-LINE**
  *
  * Condor Software Copyright Notice
  * Copyright (C) 1990-2006, Condor Team, Computer Sciences Department,
  * University of Wisconsin-Madison, WI.
  *
  * This source code is covered by the Condor Public License, which can
  * be found in the accompanying LICENSE.TXT file, or online at
  * www.condorproject.org.
  *
  * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
  * AND THE UNIVERSITY OF WISCONSIN-MADISON "AS IS" AND ANY EXPRESS OR
  * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
  * WARRANTIES OF MERCHANTABILITY, OF SATISFACTORY QUALITY, AND FITNESS
  * FOR A PARTICULAR PURPOSE OR USE ARE DISCLAIMED. THE COPYRIGHT
  * HOLDERS AND CONTRIBUTORS AND THE UNIVERSITY OF WISCONSIN-MADISON
  * MAKE NO MAKE NO REPRESENTATION THAT THE SOFTWARE, MODIFICATIONS,
  * ENHANCEMENTS OR DERIVATIVE WORKS THEREOF, WILL NOT INFRINGE ANY
  * PATENT, COPYRIGHT, TRADEMARK, TRADE SECRET OR OTHER PROPRIETARY
  * RIGHT.
  *
  ****************************Copyright-DO-NOT-REMOVE-THIS-LINE**/

#ifndef _PROC_FAMILY_PROXY_H
#define _PROC_FAMILY_PROXY_H

#include "proc_family_interface.h"
#include "MyString.h"

class ProcFamilyClient;

class ProcFamilyProxyReaperHelper;

class ProcFamilyProxy : public ProcFamilyInterface {

public:

	ProcFamilyProxy(char* address_suffix = NULL);
	~ProcFamilyProxy();

	// all registration with the ProcD is done in the
	// child (so we can guarantee it hasn't exited yet)
	//
	bool register_subfamily_parent(pid_t,
	                               pid_t,
	                               int,
	                               PidEnvID*,
	                               const char*)
	{
		return true;
	}


	// tell the procd to start tracking a new subfamily
	//
	bool register_subfamily_child(pid_t,
	                              pid_t,
	                              int,
	                              PidEnvID*,
	                              const char*);

	// for Windows: register_subfamily simply calls
	// register_subfamily_child to do the real work
	//
	bool register_subfamily(pid_t root,
	                        pid_t watcher,
	                        int snapshot_interval,
	                        PidEnvID* penvid,
	                        const char* login)
	{
		return register_subfamily_child(root,
		                                watcher,
		                                snapshot_interval,
		                                penvid,
		                                login);
	}

	// ask the procd for usage information about a process
	// family
	//
	bool get_usage(pid_t, ProcFamilyUsage&, bool);

	// tell the procd to send a signal to a single process
	//
	bool signal_process(pid_t, int);

	// tell the procd to suspend a family tree
	//
	bool suspend_family(pid_t);

	// tell the procd to continue a suspended family tree
	//
	bool continue_family(pid_t);

	// tell the procd to kill an entire family (and all
	// subfamilies of that family)
	//
	bool kill_family(pid_t);

	// tell the procd we don't care about this family any
	// more
	//
	bool unregister_family(pid_t);

	// reaper for the ProcD (called by our PrcFamilyProxyReaperHelper
	// member)
	//
	int procd_reaper(int, int);

private:

	// start a procd
	//
	bool start_procd();

	// tell the procd to exit
	//
	void stop_procd();

	// recover from a ProcD problem; kill and restart it if we're
	// the one who launched it
	//
	void recover_from_procd_error();

	// static flag so that we can guarantee we are only
	// instantiated once
	//
	static int s_instantiated;

	// the address that our ProcD is using
	//
	MyString m_procd_addr;

	// the log file that our ProcD will use
	//
	MyString m_procd_log;

	// the ProcD's pid, if we started one and haven't told it to exit yet;
	// otherwise, -1
	//
	int m_procd_pid;

	// object for managing our connection to the ProcD
	//
	ProcFamilyClient* m_client;

	// helper object to assist DaemonCore in sending us reap events
	//
	ProcFamilyProxyReaperHelper* m_reaper_helper;

	// ID of reaper for the ProcD, if registered
	//
	int m_reaper_id;
};

#endif