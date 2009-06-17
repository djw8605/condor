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
#include <math.h>
#include <float.h>
#include "condor_state.h"
#include "condor_debug.h"
#include "condor_config.h"
#include "condor_attributes.h"
#include "condor_api.h"
#include "condor_query.h"
#include "daemon.h"
#include "dc_startd.h"
#include "daemon_types.h"
#include "dc_collector.h"
#include "condor_string.h"  // for strlwr() and friends
#include "get_daemon_name.h"
#include "condor_netdb.h"
#include "condor_claimid_parser.h"
#include "misc_utils.h"
#include "ConcurrencyLimitUtils.h"

#if HAVE_DLOPEN
#include "NegotiatorPlugin.h"
#endif

// the comparison function must be declared before the declaration of the
// matchmaker class in order to preserve its static-ness.  (otherwise, it
// is forced to be extern.)

static int comparisonFunction (AttrList *, AttrList *, void *);
#include "matchmaker.h"

/* This extracts the machine name from the global job ID user@machine.name#timestamp#cluster.proc*/
static int get_scheddname_from_gjid(const char * globaljobid, char * scheddname );

// possible outcomes of negotiating with a schedd
enum { MM_ERROR, MM_DONE, MM_RESUME };

// possible outcomes of a matchmaking attempt
enum { _MM_ERROR, MM_NO_MATCH, MM_GOOD_MATCH, MM_BAD_MATCH };

typedef int (*lessThanFunc)(AttrList*, AttrList*, void*);

static bool want_simple_matching = false;

MyString ResourceWeightAttr = ATTR_RESOURCE_WEIGHT;

//added by ameet - dirty hack - needs to be removed soon!!!
//#include "../condor_c++_util/queuedbmanager.h"
//QueueDBManager queueDBManager;

Matchmaker::
Matchmaker ()
{
	char buf[64];

	NegotiatorName = NULL;

	AccountantHost  = NULL;
	PreemptionReq = NULL;
	PreemptionRank = NULL;
	NegotiatorPreJobRank = NULL;
	NegotiatorPostJobRank = NULL;
	sockCache = NULL;

	sprintf (buf, "MY.%s > MY.%s", ATTR_RANK, ATTR_CURRENT_RANK);
	Parse (buf, rankCondStd);

	sprintf (buf, "MY.%s >= MY.%s", ATTR_RANK, ATTR_CURRENT_RANK);
	Parse (buf, rankCondPrioPreempt);

	negotiation_timerID = -1;
	GotRescheduleCmd=false;
	job_attr_references = NULL;
	
	stashedAds = new AdHash(1000, HashFunc);

	MatchList = NULL;
	cachedAutoCluster = -1;
	cachedName = NULL;
	cachedAddr = NULL;

	want_simple_matching = false;
	want_matchlist_caching = false;
	ConsiderPreemption = true;
	want_nonblocking_startd_contact = true;

	completedLastCycleTime = (time_t) 0;

	publicAd = NULL;

	update_collector_tid = -1;

	update_interval = 5*MINUTE; 
    DynQuotaMachConstraint = NULL;

	groupQuotasHash = NULL;

	strcpy(RejectsTable, "rejects");
	strcpy(MatchesTable, "matches");

	prevLHF = 0;
}


Matchmaker::
~Matchmaker()
{
	if (AccountantHost) free (AccountantHost);
	AccountantHost = NULL;
	if (job_attr_references) free (job_attr_references);
	job_attr_references = NULL;
	delete rankCondStd;
	delete rankCondPrioPreempt;
	delete PreemptionReq;
	delete PreemptionRank;
	delete NegotiatorPreJobRank;
	delete NegotiatorPostJobRank;
	delete sockCache;
	if (MatchList) {
		delete MatchList;
	}
	if ( cachedName ) free(cachedName);
	if ( cachedAddr ) free(cachedAddr);

	if (NegotiatorName) free (NegotiatorName);
	if (publicAd) delete publicAd;
    if (DynQuotaMachConstraint) delete DynQuotaMachConstraint;
	if (groupQuotasHash) delete groupQuotasHash;
}


void Matchmaker::
initialize ()
{
	// read in params
	reinitialize ();

    // register commands
    daemonCore->Register_Command (RESCHEDULE, "Reschedule", 
            (CommandHandlercpp) &Matchmaker::RESCHEDULE_commandHandler, 
			"RESCHEDULE_commandHandler", (Service*) this, DAEMON);
    daemonCore->Register_Command (RESET_ALL_USAGE, "ResetAllUsage",
            (CommandHandlercpp) &Matchmaker::RESET_ALL_USAGE_commandHandler, 
			"RESET_ALL_USAGE_commandHandler", this, ADMINISTRATOR);
    daemonCore->Register_Command (RESET_USAGE, "ResetUsage",
            (CommandHandlercpp) &Matchmaker::RESET_USAGE_commandHandler, 
			"RESET_USAGE_commandHandler", this, ADMINISTRATOR);
    daemonCore->Register_Command (DELETE_USER, "DeleteUser",
            (CommandHandlercpp) &Matchmaker::DELETE_USER_commandHandler, 
			"DELETE_USER_commandHandler", this, ADMINISTRATOR);
    daemonCore->Register_Command (SET_PRIORITYFACTOR, "SetPriorityFactor",
            (CommandHandlercpp) &Matchmaker::SET_PRIORITYFACTOR_commandHandler, 
			"SET_PRIORITYFACTOR_commandHandler", this, ADMINISTRATOR);
    daemonCore->Register_Command (SET_PRIORITY, "SetPriority",
            (CommandHandlercpp) &Matchmaker::SET_PRIORITY_commandHandler, 
			"SET_PRIORITY_commandHandler", this, ADMINISTRATOR);
    daemonCore->Register_Command (SET_ACCUMUSAGE, "SetAccumUsage",
            (CommandHandlercpp) &Matchmaker::SET_ACCUMUSAGE_commandHandler, 
			"SET_ACCUMUSAGE_commandHandler", this, ADMINISTRATOR);
    daemonCore->Register_Command (SET_BEGINTIME, "SetBeginUsageTime",
            (CommandHandlercpp) &Matchmaker::SET_BEGINTIME_commandHandler, 
			"SET_BEGINTIME_commandHandler", this, ADMINISTRATOR);
    daemonCore->Register_Command (SET_LASTTIME, "SetLastUsageTime",
            (CommandHandlercpp) &Matchmaker::SET_LASTTIME_commandHandler, 
			"SET_LASTTIME_commandHandler", this, ADMINISTRATOR);
    daemonCore->Register_Command (GET_PRIORITY, "GetPriority",
		(CommandHandlercpp) &Matchmaker::GET_PRIORITY_commandHandler, 
			"GET_PRIORITY_commandHandler", this, READ);
    daemonCore->Register_Command (GET_RESLIST, "GetResList",
		(CommandHandlercpp) &Matchmaker::GET_RESLIST_commandHandler, 
			"GET_RESLIST_commandHandler", this, READ);

	// Set a timer to renegotiate.
    negotiation_timerID = daemonCore->Register_Timer (0,  NegotiatorInterval,
			(TimerHandlercpp) &Matchmaker::negotiationTime, 
			"Time to negotiate", this);

	update_collector_tid = daemonCore->Register_Timer (
			0, update_interval,
			(TimerHandlercpp) &Matchmaker::updateCollector,
			"Update Collector", this );


#if HAVE_DLOPEN
	NegotiatorPluginManager::Load();
	NegotiatorPluginManager::Initialize();
#endif
}

int Matchmaker::
reinitialize ()
{
	char *tmp;
	static bool first_time = true;

    // Initialize accountant params
    accountant.Initialize();

	init_public_ad();

	// get timeout values

 	NegotiatorInterval = param_integer("NEGOTIATOR_INTERVAL",300);

	NegotiatorTimeout = param_integer("NEGOTIATOR_TIMEOUT",30);

	// up to 1 year per submitter by default
 	MaxTimePerSubmitter = param_integer("NEGOTIATOR_MAX_TIME_PER_SUBMITTER",31536000);

	// up to 1 year per spin by default
 	MaxTimePerSpin = param_integer("NEGOTIATOR_MAX_TIME_PER_PIESPIN",31536000);

	// deal with a possibly resized socket cache, or create the socket
	// cache if this is the first time we got here.
	// 
	// we call the resize method which:
	// - does nothing if the size is the same
	// - preserves the old sockets if the size has grown 
	// - does nothing (except dprintf into the log) if the size has shrunk.
	//
	// the user must call condor_restart to actually shrink the sockCache.

	int socket_cache_size = param_integer("NEGOTIATOR_SOCKET_CACHE_SIZE",DEFAULT_SOCKET_CACHE_SIZE,1);
	if( socket_cache_size ) {
		dprintf (D_ALWAYS,"NEGOTIATOR_SOCKET_CACHE_SIZE = %d\n", socket_cache_size);
	}
	if (sockCache) {
		sockCache->resize(socket_cache_size);
	} else {
		sockCache = new SocketCache(socket_cache_size);
	}

	// get PreemptionReq expression
	if (PreemptionReq) delete PreemptionReq;
	PreemptionReq = NULL;
	tmp = param("PREEMPTION_REQUIREMENTS");
	if( tmp ) {
		if( Parse(tmp, PreemptionReq) ) {
			EXCEPT ("Error parsing PREEMPTION_REQUIREMENTS expression: %s",
					tmp);
		}
		dprintf (D_ALWAYS,"PREEMPTION_REQUIREMENTS = %s\n", tmp);
		free( tmp );
		tmp = NULL;
	} else {
		dprintf (D_ALWAYS,"PREEMPTION_REQUIREMENTS = None\n");
	}

	NegotiatorMatchExprNames.clearAll();
	NegotiatorMatchExprValues.clearAll();
	tmp = param("NEGOTIATOR_MATCH_EXPRS");
	if( tmp ) {
		NegotiatorMatchExprNames.initializeFromString( tmp );
		free( tmp );
		tmp = NULL;

			// Now read in the values of the macros in the list.
		NegotiatorMatchExprNames.rewind();
		char const *expr_name;
		while( (expr_name=NegotiatorMatchExprNames.next()) ) {
			char *expr_value = param( expr_name );
			if( !expr_value ) {
				dprintf(D_ALWAYS,"Warning: NEGOTIATOR_MATCH_EXPRS references a macro '%s' which is not defined in the configuration file.\n",expr_name);
				NegotiatorMatchExprNames.deleteCurrent();
				continue;
			}
			NegotiatorMatchExprValues.append( expr_value );
			free( expr_value );
		}

			// Now change the names of the ExprNames so they have the prefix
			// "MatchExpr" that is expected by the schedd.
		size_t prefix_len = strlen(ATTR_NEGOTIATOR_MATCH_EXPR);
		NegotiatorMatchExprNames.rewind();
		while( (expr_name=NegotiatorMatchExprNames.next()) ) {
			if( strncmp(expr_name,ATTR_NEGOTIATOR_MATCH_EXPR,prefix_len) != 0 ) {
				MyString new_name = ATTR_NEGOTIATOR_MATCH_EXPR;
				new_name += expr_name;
				NegotiatorMatchExprNames.insert(new_name.Value());
				NegotiatorMatchExprNames.deleteCurrent();
			}
		}
	}

	dprintf (D_ALWAYS,"ACCOUNTANT_HOST = %s\n", AccountantHost ? 
			AccountantHost : "None (local)");
	dprintf (D_ALWAYS,"NEGOTIATOR_INTERVAL = %d sec\n",NegotiatorInterval);
	dprintf (D_ALWAYS,"NEGOTIATOR_TIMEOUT = %d sec\n",NegotiatorTimeout);
	dprintf (D_ALWAYS,"MAX_TIME_PER_SUBMITTER = %d sec\n",MaxTimePerSubmitter);
	dprintf (D_ALWAYS,"MAX_TIME_PER_PIESPIN = %d sec\n",MaxTimePerSpin);

	if( tmp ) free( tmp );

	if (PreemptionRank) delete PreemptionRank;
	PreemptionRank = NULL;
	tmp = param("PREEMPTION_RANK");
	if( tmp ) {
		if( Parse(tmp, PreemptionRank) ) {
			EXCEPT ("Error parsing PREEMPTION_RANK expression: %s", tmp);
		}
	}

	dprintf (D_ALWAYS,"PREEMPTION_RANK = %s\n", (tmp?tmp:"None"));

	if( tmp ) free( tmp );

	if (NegotiatorPreJobRank) delete NegotiatorPreJobRank;
	NegotiatorPreJobRank = NULL;
	tmp = param("NEGOTIATOR_PRE_JOB_RANK");
	if( tmp ) {
		if( Parse(tmp, NegotiatorPreJobRank) ) {
			EXCEPT ("Error parsing NEGOTIATOR_PRE_JOB_RANK expression: %s", tmp);
		}
	}

	dprintf (D_ALWAYS,"NEGOTIATOR_PRE_JOB_RANK = %s\n", (tmp?tmp:"None"));

	if( tmp ) free( tmp );

	if (NegotiatorPostJobRank) delete NegotiatorPostJobRank;
	NegotiatorPostJobRank = NULL;
	tmp = param("NEGOTIATOR_POST_JOB_RANK");
	if( tmp ) {
		if( Parse(tmp, NegotiatorPostJobRank) ) {
			EXCEPT ("Error parsing NEGOTIATOR_POST_JOB_RANK expression: %s", tmp);
		}
	}

	dprintf (D_ALWAYS,"NEGOTIATOR_POST_JOB_RANK = %s\n", (tmp?tmp:"None"));
	
	if( tmp ) free( tmp );


		// how often we update the collector, fool
 	update_interval = param_integer ("NEGOTIATOR_UPDATE_INTERVAL", 
									 5*MINUTE);



	char *preferred_collector = param ("COLLECTOR_HOST_FOR_NEGOTIATOR");
	if ( preferred_collector ) {
		CollectorList* collectors = daemonCore->getCollectorList();
		collectors->resortLocal( preferred_collector );
		free( preferred_collector );
	}

	useResourceWeights = param_boolean("NEGOTIATOR_USE_RESOURCE_WEIGHTS", false);

	want_simple_matching = param_boolean("NEGOTIATOR_SIMPLE_MATCHING",false);
	want_matchlist_caching = param_boolean("NEGOTIATOR_MATCHLIST_CACHING",true);
	ConsiderPreemption = param_boolean("NEGOTIATOR_CONSIDER_PREEMPTION",true);
	want_inform_startd = param_boolean("NEGOTIATOR_INFORM_STARTD", true);
	want_nonblocking_startd_contact = param_boolean("NEGOTIATOR_USE_NONBLOCKING_STARTD_CONTACT",true);

	// we should figure these out automatically someday ....
	preemption_req_unstable = ! (param_boolean("PREEMPTION_REQUIREMENTS_STABLE",true)) ;
	preemption_rank_unstable = ! (param_boolean("PREEMPTION_RANK_STABLE",true)) ;

	if (DynQuotaMachConstraint) delete DynQuotaMachConstraint;
	DynQuotaMachConstraint = NULL;
	tmp = param("GROUP_DYNAMIC_MACH_CONSTRAINT");
	if( tmp ) {
        dprintf(D_FULLDEBUG, "%s = %s\n", "GROUP_DYNAMIC_MACH_CONSTRAINT",
                tmp);
		if( Parse(tmp, DynQuotaMachConstraint) ) {
			dprintf(
                D_ALWAYS, 
                "Error parsing GROUP_DYNAMIC_MACH_CONSTRAINT expression: %s",
					tmp
            );
            DynQuotaMachConstraint = NULL;
		}
        free (tmp);
	}

	if( first_time ) {
		first_time = false;
	} else { 
			// be sure to try to publish a new negotiator ad on reconfig
		updateCollector();
	}


	// done
	return TRUE;
}


int Matchmaker::
RESCHEDULE_commandHandler (int, Stream *strm)
{
	// read the required data off the wire
	if (!strm->end_of_message())
	{
		dprintf (D_ALWAYS, "Could not read eom\n");
		return FALSE;
	}

	if (GotRescheduleCmd) return TRUE;
	GotRescheduleCmd=true;
	daemonCore->Reset_Timer(negotiation_timerID,0,
							NegotiatorInterval);
	return TRUE;
}


int Matchmaker::
RESET_ALL_USAGE_commandHandler (int, Stream *strm)
{
	// read the required data off the wire
	if (!strm->end_of_message())
	{
		dprintf (D_ALWAYS, "Could not read eom\n");
		return FALSE;
	}

	// reset usage
	dprintf (D_ALWAYS,"Resetting the usage of all users\n");
	accountant.ResetAllUsage();
	
	return TRUE;
}

int Matchmaker::
DELETE_USER_commandHandler (int, Stream *strm)
{
	char	scheddName[64];
	char	*sn = scheddName;
	int		len = 64;

	// read the required data off the wire
	if (!strm->get(sn, len) 	|| 
		!strm->end_of_message())
	{
		dprintf (D_ALWAYS, "Could not read accountant record name\n");
		return FALSE;
	}

	// reset usage
	dprintf (D_ALWAYS,"Deleting accountanting record of %s\n",scheddName);
	accountant.DeleteRecord (scheddName);
	
	return TRUE;
}

int Matchmaker::
RESET_USAGE_commandHandler (int, Stream *strm)
{
	char	scheddName[64];
	char	*sn = scheddName;
	int		len = 64;

	// read the required data off the wire
	if (!strm->get(sn, len) 	|| 
		!strm->end_of_message())
	{
		dprintf (D_ALWAYS, "Could not read schedd name\n");
		return FALSE;
	}

	// reset usage
	dprintf (D_ALWAYS,"Resetting the usage of %s\n",scheddName);
	accountant.ResetAccumulatedUsage (scheddName);
	
	return TRUE;
}


int Matchmaker::
SET_PRIORITYFACTOR_commandHandler (int, Stream *strm)
{
	float	priority;
	char	scheddName[64];
	char	*sn = scheddName;
	int		len = 64;

	// read the required data off the wire
	if (!strm->get(sn, len) 	|| 
		!strm->get(priority) 	|| 
		!strm->end_of_message())
	{
		dprintf (D_ALWAYS, "Could not read schedd name and priority\n");
		return FALSE;
	}

	// set the priority
	dprintf (D_ALWAYS,"Setting the priority factor of %s to %f\n",scheddName,priority);
	accountant.SetPriorityFactor (scheddName, priority);
	
	return TRUE;
}


int Matchmaker::
SET_PRIORITY_commandHandler (int, Stream *strm)
{
	float	priority;
	char	scheddName[64];
	char	*sn = scheddName;
	int		len = 64;

	// read the required data off the wire
	if (!strm->get(sn, len) 	|| 
		!strm->get(priority) 	|| 
		!strm->end_of_message())
	{
		dprintf (D_ALWAYS, "Could not read schedd name and priority\n");
		return FALSE;
	}

	// set the priority
	dprintf (D_ALWAYS,"Setting the priority of %s to %f\n",scheddName,priority);
	accountant.SetPriority (scheddName, priority);
	
	return TRUE;
}

int Matchmaker::
SET_ACCUMUSAGE_commandHandler (int, Stream *strm)
{
	float	accumUsage;
	char	scheddName[64];
	char	*sn = scheddName;
	int		len = 64;

	// read the required data off the wire
	if (!strm->get(sn, len) 	|| 
		!strm->get(accumUsage) 	|| 
		!strm->end_of_message())
	{
		dprintf (D_ALWAYS, "Could not read schedd name and accumulatedUsage\n");
		return FALSE;
	}

	// set the priority
	dprintf (D_ALWAYS,"Setting the accumulated usage of %s to %f\n",
			scheddName,accumUsage);
	accountant.SetAccumUsage (scheddName, accumUsage);
	
	return TRUE;
}

int Matchmaker::
SET_BEGINTIME_commandHandler (int, Stream *strm)
{
	int	beginTime;
	char	scheddName[64];
	char	*sn = scheddName;
	int		len = 64;

	// read the required data off the wire
	if (!strm->get(sn, len) 	|| 
		!strm->get(beginTime) 	|| 
		!strm->end_of_message())
	{
		dprintf (D_ALWAYS, "Could not read schedd name and begin usage time\n");
		return FALSE;
	}

	// set the priority
	dprintf (D_ALWAYS,"Setting the begin usage time of %s to %d\n",
			scheddName,beginTime);
	accountant.SetBeginTime (scheddName, beginTime);
	
	return TRUE;
}

int Matchmaker::
SET_LASTTIME_commandHandler (int, Stream *strm)
{
	int	lastTime;
	char	scheddName[64];
	char	*sn = scheddName;
	int		len = 64;

	// read the required data off the wire
	if (!strm->get(sn, len) 	|| 
		!strm->get(lastTime) 	|| 
		!strm->end_of_message())
	{
		dprintf (D_ALWAYS, "Could not read schedd name and last usage time\n");
		return FALSE;
	}

	// set the priority
	dprintf (D_ALWAYS,"Setting the last usage time of %s to %d\n",
			scheddName,lastTime);
	accountant.SetLastTime (scheddName, lastTime);
	
	return TRUE;
}


int Matchmaker::
GET_PRIORITY_commandHandler (int, Stream *strm)
{
	// read the required data off the wire
	if (!strm->end_of_message())
	{
		dprintf (D_ALWAYS, "GET_PRIORITY: Could not read eom\n");
		return FALSE;
	}

	// get the priority
	AttrList* ad=accountant.ReportState();
	dprintf (D_ALWAYS,"Getting state information from the accountant\n");
	
	if (!ad->put(*strm) ||
	    !strm->end_of_message())
	{
		dprintf (D_ALWAYS, "Could not send priority information\n");
		delete ad;
		return FALSE;
	}

	delete ad;

	return TRUE;
}


int Matchmaker::
GET_RESLIST_commandHandler (int, Stream *strm)
{
    char    scheddName[64];
    char    *sn = scheddName;
    int     len = 64;

    // read the required data off the wire
    if (!strm->get(sn, len)     ||
        !strm->end_of_message())
    {
        dprintf (D_ALWAYS, "Could not read schedd name\n");
        return FALSE;
    }

    // reset usage
    dprintf (D_ALWAYS,"Getting resource list of %s\n",scheddName);

	// get the priority
	AttrList* ad=accountant.ReportState(scheddName);
	dprintf (D_ALWAYS,"Getting state information from the accountant\n");
	
	if (!ad->put(*strm) ||
	    !strm->end_of_message())
	{
		dprintf (D_ALWAYS, "Could not send resource list\n");
		delete ad;
		return FALSE;
	}

	delete ad;

	return TRUE;
}


/*
Look for an ad matching the given constraint string
in the table given by arg.  Return a duplicate on success.
Otherwise, return 0.
*/
#if 0
static ClassAd * lookup_global( const char *constraint, void *arg )
{
	ClassAdList *list = (ClassAdList*) arg;
	ClassAd *ad;
	ClassAd queryAd;

	if ( want_simple_matching ) {
		return 0;
	}

	CondorQuery query(ANY_AD);
	query.addANDConstraint(constraint);
	query.getQueryAd(queryAd);
	queryAd.SetTargetTypeName (ANY_ADTYPE);

	list->Open();
	while( (ad = list->Next()) ) {
		if(queryAd <= *ad) {
			return new ClassAd(*ad);
		}
	}

	return 0;
}
#endif

char *
Matchmaker::
compute_significant_attrs(ClassAdList & startdAds)
{
	char *result = NULL;

	// Figure out list of all external attribute references in all startd ads
	dprintf(D_FULLDEBUG,"Entering compute_significant_attrs()\n");
	ClassAd *startd_ad = NULL;
	ClassAd *sample_startd_ad = NULL;
	startdAds.Open ();
	StringList internal_references;	// not used...
	StringList external_references;	// this is what we want to compute. 
	while ((startd_ad = startdAds.Next ())) { // iterate through all startd ads
		if ( !sample_startd_ad ) {
			sample_startd_ad = new ClassAd(*startd_ad);
		}
			// Make a stringlist of all attribute names in this startd ad.
		StringList AttrsToExpand;
		startd_ad->ResetName();
		const char *attr_name = startd_ad->NextNameOriginal();
		while ( attr_name ) {
			AttrsToExpand.append(attr_name);
			attr_name = startd_ad->NextNameOriginal();
		}
			// Get list of external references for all attributes.  Note that 
			// it is _not_ sufficient to just get references via requirements
			// and rank.  Don't understand why? Ask Todd <tannenba@cs.wisc.edu>
		AttrsToExpand.rewind();
		while ( (attr_name = AttrsToExpand.next()) ) {
			startd_ad->GetReferences(attr_name,internal_references,
					external_references);

#if 0	// Debug code
			static int extlen = 0;
			result = external_references.print_to_string();
			if ( result && (strlen(result) != extlen) ) {
				extlen = strlen(result);
				dprintf(D_FULLDEBUG,"CHANGE: Startd being considered in compute_significant_attrs() is:\n");
				startd_ad->dPrint(D_FULLDEBUG);
				dprintf(D_FULLDEBUG,"CHANGE: In compute_significant_attrs() attr=%s - result=%s\n",
						attr_name, result ? result : "(none)" );
				if ( result ) {
					free(result);
				}
				result = NULL;
			}
#endif

		}	// while attr_name
	}	// while startd_ad

	// Now add external attributes references from negotiator policy exprs; at
	// this point, we only have to worry about PREEMPTION_REQUIREMENTS.
	// PREEMPTION_REQUIREMENTS is evaluated in the context of a machine ad 
	// followed by a job ad.  So to help figure out the external (job) attributes
	// that are significant, we take a sample startd ad and add any startd_job_exprs
	// to it.
	if (!sample_startd_ad) {	// if no startd ads, just return.
		return NULL;	// if no startd ads, there are no sig attrs
	}
	char *startd_job_exprs = param("STARTD_JOB_EXPRS");
	if ( startd_job_exprs ) {	// add in startd_job_exprs
		StringList exprs(startd_job_exprs);
		exprs.rewind();
		char *v = NULL;
		while ( (v=exprs.next()) ) {
			sample_startd_ad->Assign(v,true);
		}
		free(startd_job_exprs);
	}
	char *tmp=param("PREEMPTION_REQUIREMENTS");
	if ( tmp && PreemptionReq ) {	// add references from preemption_requirements
		const char* preempt_req_name = "preempt_req__";	// any name will do
		sample_startd_ad->AssignExpr(preempt_req_name,tmp);
		sample_startd_ad->GetReferences(preempt_req_name,internal_references,
					external_references);
	}
	free(tmp);
	if (sample_startd_ad) {
		delete sample_startd_ad;
		sample_startd_ad = NULL;
	}
		// Always get rid of the follow attrs:
		//    CurrentTime - for obvious reasons
		//    RemoteUserPrio - not needed since we negotiate per user
		//    SubmittorPrio - not needed since we negotiate per user
	external_references.remove_anycase(ATTR_CURRENT_TIME);
	external_references.remove_anycase(ATTR_REMOTE_USER_PRIO);
	external_references.remove_anycase(ATTR_REMOTE_USER_RESOURCES_IN_USE);
	external_references.remove_anycase(ATTR_REMOTE_GROUP_RESOURCES_IN_USE);
	external_references.remove_anycase(ATTR_SUBMITTOR_PRIO);
	external_references.remove_anycase(ATTR_SUBMITTER_USER_PRIO);
	external_references.remove_anycase(ATTR_SUBMITTER_USER_RESOURCES_IN_USE);
	external_references.remove_anycase(ATTR_SUBMITTER_GROUP_RESOURCES_IN_USE);
		// Note: print_to_string mallocs memory on the heap
	result = external_references.print_to_string();
	dprintf(D_FULLDEBUG,"Leaving compute_significant_attrs() - result=%s\n",
					result ? result : "(none)" );
	return result;
}


bool Matchmaker::
getGroupInfoFromUserId( const char *user, float & groupQuota, float & groupUsage )
{
	/*  Given a user id in the form group.user, strip off the group name
		return any associated quota and usage for that group.  On failure,
		return false and quota=usage=0.
		NOTE - Since we discover the quotas in negotationTime(), we
		assert that this function should not be called ahead of a call 
		to negotationTime().
	 */
	ASSERT(groupQuotasHash);

	groupQuota = 0.0;
	groupUsage = 0.0;

	if (!user) return false;

	MyString groupname(user);

		// User Id is group-name.user-name, so replace the
		// '.' with a NULL.
	int pos = groupname.FindChar('.');
	if ( pos <= 0 ) {
		return false;
	}
	groupname.setChar( pos , '\0' );
	if ( groupQuotasHash->lookup(groupname,groupQuota) == -1 ) {
		// hash lookup failed, must not be a group name
		return false;
	}

	groupUsage = accountant.GetResourcesUsed(groupname.Value());

	return true;
}



int Matchmaker::
negotiationTime ()
{
	ClassAdList startdAds;
	ClaimIdHash claimIds(MyStringHash);
	ClassAdList scheddAds;
	ClassAdList allAds;

	/**
		Check if we just finished a cycle less than NEGOTIATOR_CYCLE_DELAY 
		seconds ago.  If we did, reset our timer so at least 
		NEGOTIATOR_CYCLE_DELAY seconds will elapse between cycles.  We do 
		this to help ensure all the startds have had time to update the 
		collector after the last negotiation cycle (otherwise, we might match
		the same resource twice).  Note: we must do this check _before_ we 
		reset GotRescheduledCmd to false to prevent postponing a new 
		cycle indefinitely.
	**/
	int elapsed = time(NULL) - completedLastCycleTime;
	int cycle_delay = param_integer("NEGOTIATOR_CYCLE_DELAY",20,0);
	if ( elapsed < cycle_delay ) {
		daemonCore->Reset_Timer(negotiation_timerID,
							cycle_delay - elapsed,
							NegotiatorInterval);
		dprintf(D_FULLDEBUG,
			"New cycle requested but just finished one -- delaying %u secs\n",
			cycle_delay - elapsed);
		return FALSE;
	}

	dprintf( D_ALWAYS, "---------- Started Negotiation Cycle ----------\n" );

	GotRescheduleCmd=false;  // Reset the reschedule cmd flag

	// We need to nuke our MatchList from the previous negotiation cycle,
	// since a different set of machines may now be available.
	if (MatchList) delete MatchList;
	MatchList = NULL;

	// ----- Get all required ads from the collector
	dprintf( D_ALWAYS, "Phase 1:  Obtaining ads from collector ...\n" );
	if( !obtainAdsFromCollector( allAds, startdAds, scheddAds,
		claimIds ) )
	{
		dprintf( D_ALWAYS, "Aborting negotiation cycle\n" );
		// should send email here
		return FALSE;
	}

	// Save this for future use.
	// This _must_ come before trimming the startd ads.
	int untrimmed_num_startds = startdAds.MyLength();

    // Get number of available slots in any state.
    int numDynGroupSlots = untrimmed_num_startds;

	// Register a lookup function that passes through the list of all ads.
	// ClassAdLookupRegister( lookup_global, &allAds );

	// Compute the significant attributes to pass to the schedd, so
	// the schedd can do autoclustering to speed up the negotiation cycles.
	if ( job_attr_references ) {
		free(job_attr_references);
	}
	job_attr_references = compute_significant_attrs(startdAds);

	// ----- Recalculate priorities for schedds
	dprintf( D_ALWAYS, "Phase 2:  Performing accounting ...\n" );
	accountant.UpdatePriorities();
	accountant.CheckMatches( startdAds );
	// if don't care about preemption, we can trim out all non Unclaimed ads now.
	// note: we cannot trim out the Unclaimed ads before we call CheckMatches,
	// otherwise CheckMatches will do the wrong thing (because it will not see
	// any of the claimed machines!).
	int num_trimmed = trimStartdAds(startdAds);
	if ( num_trimmed > 0 ) {
		dprintf(D_FULLDEBUG,
			"Trimmed out %d startd ads not Unclaimed\n",num_trimmed);
	}

		// We insert NegotiatorMatchExprXXX attributes into the
		// "matched ad".  In the negotiator, this means the machine ad.
		// The schedd will later propogate these attributes into the
		// matched job ad that is sent to the startd.  So in different
		// matching contexts, the negotiator match exprs are in different
		// ads, but they should always be in at least one.
	insertNegotiatorMatchExprs( startdAds );

	if ( !groupQuotasHash ) {
		groupQuotasHash = new groupQuotasHashType(100,HashFunc);
		ASSERT(groupQuotasHash);
	}

	char *groups = param("GROUP_NAMES");
	if ( groups ) {

		// HANDLE GROUPS (as desired by CDF)

		// Populate the groupArray, which contains an entry for
		// each group.
		SimpleGroupEntry* groupArray;
		int i;
		StringList groupList;		
		strlwr(groups); // the accountant will want lower case!!!
		groupList.initializeFromString(groups);
		free(groups);		
		groupArray = new SimpleGroupEntry[ groupList.number() ];
		ASSERT(groupArray);

        // Restrict number of slots available for dynamic quotas.
        if ( numDynGroupSlots && DynQuotaMachConstraint ) {
            int matchedSlots = startdAds.Count( DynQuotaMachConstraint );
            if ( matchedSlots ) {
                dprintf(D_FULLDEBUG,
                    "GROUP_DYNAMIC_MACH_CONSTRAINT constraint reduces machine "
                    "count from %d to %d\n", numDynGroupSlots, matchedSlots);
                numDynGroupSlots = matchedSlots;
            } else {
                dprintf(D_ALWAYS, "warning: 0 out of %d machines match "
                        "GROUP_DYNAMIC_MACH_CONSTRAINT for dynamic quotas\n",
                        numDynGroupSlots);
                numDynGroupSlots = 0;
            }
        }

		MyString tmpstr;
		i = 0;
		groupQuotasHash->clear();
		groupList.rewind();
		while ((groups = groupList.next ()))
		{
			tmpstr.sprintf("GROUP_QUOTA_%s",groups);
			float quota = param_double(tmpstr.Value(), -1.0 );
			if ( quota >= 0.0 ) {
                // Static groups quotas take priority over any dynamic quota
                dprintf(D_FULLDEBUG, "group %s static quota = %.3f\n",
                        groups, quota);
            } else {
                // Next look for a floating point dynamic quota.
                tmpstr.sprintf("GROUP_QUOTA_DYNAMIC_%s", groups);
                double quota_fraction =
                    param_double(
                        tmpstr.Value(),     // name
                        0.0,                // default value
                        0.0,                // min value
                        1.0                 // max value
                    );
                if (quota_fraction != 0.0) {
                    // use specified dynamic quota
                    quota = (int)(quota_fraction * numDynGroupSlots);
                    dprintf(D_FULLDEBUG,
                        "group %s dynamic quota for %d slots = %.3f\n",
                            groups, numDynGroupSlots, quota);
                } else {
                    // neither a static nor dynamic quota was defined
                    dprintf(D_ALWAYS,
                        "ERROR - no quota specified for group %s, ignoring\n",
                        groups);
                    continue;
                }
            }
            if ( quota <= 0 ) {
                // Quota for group may have been set to zero by admin.
                dprintf(D_ALWAYS,
                    "zero quota for group %s, ignoring\n",
                    groups);
                continue;
            }

			// store this groups quota into our groupQuotas hash so we 
			// can easily retrieve the quota for this group elsewhere in 
			// this class.
			MyString groupQuotaKey(groups);
			groupQuotasHash->insert(groupQuotaKey,quota);

			// fill in the info into the groupArray, so we can sort
			// the groups into the order we want to negotiate them.
			int usage  = accountant.GetResourcesUsed(groups);
			float usageRW = accountant.GetResourcesUsedFloat(groups);
			groupArray[i].groupName = groups;  // don't free this! (in groupList)
			groupArray[i].maxAllowed = quota;
			groupArray[i].usageRW = usageRW;
			groupArray[i].usage = usage;
				// the 'prio' field is used to sort the group array, i.e. to
				// decide which groups get to negotiate first.  
				// we sort groups based upon the percentage of their quota
				// currently being used, so that groups using the least 
				// percentage amount of their quota get to negotiate first.
			groupArray[i].prio = ( 100 * usage ) / quota;
			dprintf(D_FULLDEBUG,
				"Group Table : group %s quota %.3f usage %d(%.3f) prio %2.2f\n",
					groups,quota,usage,usageRW,groupArray[i].prio);
			i++;
		}
		int groupArrayLen = i;

			// pull out the submitter ads that specify a group from the
			// scheddAds list, and insert them into a list specific to 
			// the specified group.
		ClassAd *ad = NULL;
		char scheddName[80];
		scheddAds.Open();
		while( (ad=scheddAds.Next()) ) {
			if (!ad->LookupString(ATTR_NAME, scheddName, sizeof(scheddName))) {
				continue;
			}
			scheddName[79] = '\0'; // make certain we have a terminating NULL
			char *sep = strchr(scheddName,'.');	// is there a group seperator?
			if ( !sep ) {
				continue;
			}
			*sep = '\0';
			for (i=0; i<groupArrayLen; i++) {
				if ( strcasecmp(scheddName,groupArray[i].groupName)==0 ) {
					groupArray[i].submitterAds.Insert(ad);
					scheddAds.Delete(ad);
					break;
				}
			}
		}

			// now sort the group array
		qsort(groupArray,groupArrayLen,sizeof(SimpleGroupEntry),groupSortCompare);		

			// and negotiate for each group
		for (i=0;i<groupArrayLen;i++) {
			if ( groupArray[i].submitterAds.MyLength() == 0 ) {
				dprintf(D_ALWAYS,
					"Group %s - skipping, no submitters\n",
					groupArray[i].groupName);
				continue;
			}
			if(useResourceWeights) {
				if ( groupArray[i].usageRW >= groupArray[i].maxAllowed  &&
					 !ConsiderPreemption ) 
					{
						dprintf(D_ALWAYS,
								"Group %s - skipping, at or over quota (usage=%.3f)\n",
								groupArray[i].groupName,groupArray[i].usageRW);
						continue;
					}
			} else {
				if ( groupArray[i].usage >= groupArray[i].maxAllowed  &&
					 !ConsiderPreemption ) 
					{
						dprintf(D_ALWAYS,
								"Group %s - skipping, at or over quota (usage=%d)\n",
								groupArray[i].groupName,groupArray[i].usage);
						continue;
					}
			}
			dprintf(D_ALWAYS,"Group %s - negotiating\n",
				groupArray[i].groupName);
			negotiateWithGroup( untrimmed_num_startds, startdAds, 
				claimIds, groupArray[i].submitterAds, 
				groupArray[i].maxAllowed, groupArray[i].groupName );
		}

			// if GROUP_AUTOREGROUP is set to true, then for any submitter
			// assigned to a group that did match, insert the submitter
			// ad back into the main scheddAds list.  this way, we will
			// try to match it again below .
		bool default_autoregroup = param_boolean("GROUP_AUTOREGROUP",false);
		for (i=0; i<groupArrayLen; i++) {
			ad = NULL;
			MyString autoregroup_param;
			autoregroup_param.sprintf("GROUP_AUTOREGROUP_%s",groupArray[i].groupName);
			if(param_boolean(autoregroup_param.Value(),default_autoregroup)) {
				dprintf(D_ALWAYS,
						"Group %s - autoregroup inserting %d submitters\n",
						groupArray[i].groupName,
						groupArray[i].submitterAds.MyLength());

				groupArray[i].submitterAds.Open();
				while( (ad=groupArray[i].submitterAds.Next()) ) {
					scheddAds.Insert(ad);				
				}
			}
		}

			// finally, cleanup 
		delete []  groupArray;
		groupArray = NULL;

			// print out a message stating we are about to negotiate below w/
			// all users who did not specify a group
		dprintf(D_ALWAYS,"Group *none* - negotiating\n");

	} // if (groups)
	
		// negotiate w/ all users who do not belong to a group.
	negotiateWithGroup(untrimmed_num_startds, startdAds, claimIds, scheddAds);
	
	// ----- Done with the negotiation cycle
	dprintf( D_ALWAYS, "---------- Finished Negotiation Cycle ----------\n" );

	completedLastCycleTime = time(NULL);

	return TRUE;
}

Matchmaker::SimpleGroupEntry::
SimpleGroupEntry()
{
	groupName = NULL;
	prio = 0;
	maxAllowed = (float) INT_MAX;
}

Matchmaker::SimpleGroupEntry::
~SimpleGroupEntry()
{
	// Note: don't free groupName!  See comment above.
}

int Matchmaker::
negotiateWithGroup ( int untrimmed_num_startds, ClassAdList& startdAds,
					 ClaimIdHash& claimIds, 
					 ClassAdList& scheddAds, 
					 float groupQuota, const char* groupAccountingName)
{
	ClassAd		*schedd;
	MyString    scheddName;
	MyString    scheddAddr;
	int			result;
	int			numStartdAds;
	double      resourceWeightTotal;
	double		maxPrioValue;
	double		maxAbsPrioValue;
	double		normalFactor;
	double		normalAbsFactor;
	double		scheddPrio;
	double		scheddPrioFactor;
	double		scheddShare = 0.0;
	double		scheddAbsShare = 0.0;
	double		scheddLimitRoundoff;
	int			scheddLimit;
	int			scheddUsage;
	int			totalTime;
	int			MaxscheddLimit;
	double      MaxscheddLimitRW;
	int			hit_schedd_prio_limit;
	int			hit_network_prio_limit;
	bool ignore_schedd_limit;
	int			num_idle_jobs;
	time_t		startTime;
	int			userprioCrumbs = 0;
	

	// ----- Sort the schedd list in decreasing priority order
	dprintf( D_ALWAYS, "Phase 3:  Sorting submitter ads by priority ...\n" );
	scheddAds.Sort( (lessThanFunc)comparisonFunction, this );

	int spin_pie=0;
	do {
		spin_pie++;
		hit_schedd_prio_limit = FALSE;
		hit_network_prio_limit = FALSE;
		calculateNormalizationFactor( scheddAds, maxPrioValue, normalFactor,
									  maxAbsPrioValue, normalAbsFactor);
		numStartdAds = untrimmed_num_startds;
			// If operating on a group with a quota, consider the size of 
			// the "pie" to be limited to the groupQuota, so each user in 
			// the group gets a reasonable sized slice.
		if ( numStartdAds > groupQuota ) {
			numStartdAds = groupQuota;
		}
		if(useResourceWeights) {
			resourceWeightTotal = sumResourceWeights(startdAds);
			if ( resourceWeightTotal > groupQuota ) {
				resourceWeightTotal = groupQuota;
			}
		} else {
			resourceWeightTotal = numStartdAds;
		}

			// Calculate how many machines are left over after dishing out
			// rounded share of machines to each submitter.
			// What's left are the user-prio "pie crumbs".
		calculateUserPrioCrumbs(
			scheddAds,
			groupAccountingName,
			groupQuota,
			numStartdAds,
			maxPrioValue,
			maxAbsPrioValue,
			normalFactor,
			normalAbsFactor,
			resourceWeightTotal,
				/* result parameters: */
			userprioCrumbs );

		MaxscheddLimit = 0;
		MaxscheddLimitRW = 0.0;
		// ----- Negotiate with the schedds in the sorted list
		dprintf( D_ALWAYS, "Phase 4.%d:  Negotiating with schedds ...\n",
			spin_pie );
		dprintf (D_FULLDEBUG, "    NumStartdAds = %d\n", numStartdAds);
		dprintf (D_FULLDEBUG, "    NormalFactor = %f\n", normalFactor);
		dprintf (D_FULLDEBUG, "    MaxPrioValue = %f\n", maxPrioValue);
		dprintf (D_FULLDEBUG, "    NumScheddAds = %d\n", scheddAds.MyLength());
		scheddAds.Open();
		while( (schedd = scheddAds.Next()) )
		{
			// get the name and address of the schedd
			if( !schedd->LookupString( ATTR_NAME, scheddName ) ||
				!schedd->LookupString( ATTR_SCHEDD_IP_ADDR, scheddAddr ) )
			{
				dprintf (D_ALWAYS,"  Error!  Could not get %s and %s from ad\n",
							ATTR_NAME, ATTR_SCHEDD_IP_ADDR);
				dprintf( D_ALWAYS, "  Ignoring this schedd and continuing\n" );
				scheddAds.Delete( schedd );
				continue;
			}	
			num_idle_jobs = 0;
			schedd->LookupInteger(ATTR_IDLE_JOBS,num_idle_jobs);
			if ( num_idle_jobs < 0 ) {
				num_idle_jobs = 0;
			}

			totalTime = 0;
			schedd->LookupInteger(ATTR_TOTAL_TIME_IN_CYCLE,totalTime);
			if ( totalTime < 0 ) {
				totalTime = 0;
			}

			if (( num_idle_jobs > 0 ) && (totalTime < MaxTimePerSubmitter) ) {
				dprintf(D_ALWAYS,"  Negotiating with %s at %s\n",
					scheddName.Value(), scheddAddr.Value());
				dprintf(D_ALWAYS, "%d seconds so far\n", totalTime);
			}

			// store the verison of the schedd, so we can take advantage of
			// protocol improvements in newer versions while still being
			// backwards compatible.
			char *schedd_ver_string = NULL;
			schedd->LookupString(ATTR_VERSION, &schedd_ver_string);
			ASSERT(schedd_ver_string);
			CondorVersionInfo	scheddVersion(schedd_ver_string);
			free(schedd_ver_string);
			schedd_ver_string = NULL;

			double scheddLimitRW = 0.0;
			double scheddUsageRW = 0.0;

			float resourceWeight = 1.0;
			if(useResourceWeights) 
				schedd->EvalFloat(ResourceWeightAttr.Value(), NULL, resourceWeight);

			calculateScheddLimit(
				scheddName.Value(),
				groupAccountingName,
				groupQuota,
				numStartdAds,
				maxPrioValue,
				maxAbsPrioValue,
				normalFactor,
				normalAbsFactor,
				resourceWeight,
				resourceWeightTotal,
					/* result parameters: */
				scheddLimit,
				scheddLimitRW,
				scheddUsage,
				scheddUsageRW,
				scheddShare,
				scheddAbsShare,
				scheddPrio,
				scheddPrioFactor,
				scheddLimitRoundoff );

			int scheddLimitWithoutCrumbs = scheddLimit;
			if( scheddLimitRoundoff > 0 && userprioCrumbs > 0 &&
			    num_idle_jobs > 0)
			{
					// give this schedd a chance to eat one user-prio crumb,
					// because its limit was rounded down, and the crumbs
					// have not all been eaten yet
				scheddLimit++;
			}

			if( scheddLimit > MaxscheddLimit ) {
				MaxscheddLimit = scheddLimit;
			}
			if( scheddLimitRW > MaxscheddLimitRW ) {
				MaxscheddLimitRW = scheddLimitRW;
			}

			if ( num_idle_jobs > 0 ) {
				dprintf (D_FULLDEBUG, "  Calculating schedd limit with the "
					"following parameters\n");
				dprintf (D_FULLDEBUG, "    ScheddPrio       = %f\n",
					scheddPrio);
				dprintf (D_FULLDEBUG, "    ScheddPrioFactor = %f\n",
					 scheddPrioFactor);
				dprintf (D_FULLDEBUG, "    scheddShare      = %f\n",
					scheddShare);
				dprintf (D_FULLDEBUG, "    scheddAbsShare   = %f\n",
					scheddAbsShare);
				dprintf (D_FULLDEBUG, "    ScheddUsage      = %d\n",
					scheddUsage);
				dprintf (D_FULLDEBUG, "    scheddLimit      = %d\n",
					scheddLimit);
				dprintf (D_FULLDEBUG, "    userprioCrumbs   = %d (%d)\n",
					userprioCrumbs, scheddLimit - scheddLimitWithoutCrumbs);
				dprintf (D_FULLDEBUG, "    MaxscheddLimit   = %d\n",
					MaxscheddLimit);
				dprintf (D_FULLDEBUG, "    scheddLimitRW    = %f\n",
					scheddLimitRW);
				dprintf (D_FULLDEBUG, "    scheddUsageRW    = %f\n",
					scheddUsageRW);
				dprintf (D_FULLDEBUG, "    MaxscheddLimitRW = %f\n",
					MaxscheddLimitRW);
			}

			// initialize reasons for match failure; do this now
			// in case we never actually call negotiate() below.
			rejForNetwork = 0;
			rejForNetworkShare = 0;
			rejForConcurrencyLimit = 0;
			rejPreemptForPrio = 0;
			rejPreemptForPolicy = 0;
			rejPreemptForRank = 0;
			rejForGroupQuota = 0;

			// Optimizations: 
			// If number of idle jobs = 0, don't waste time with negotiate.
			// Likewise, if limit is 0, don't waste time with negotiate EXCEPT
			// on the first spin of the pie (spin_pie==1), we must
			// still negotiate because on the first spin we tell the negotiate
			// function to ignore the scheddLimit w/ respect to jobs which
			// are strictly preferred by resource offers (via startd rank).
			if ( num_idle_jobs == 0 ) {
				dprintf(D_FULLDEBUG,
					"  Negotiating with %s skipped because no idle jobs\n",
					scheddName.Value());
				result = MM_DONE;
			} else if (totalTime > MaxTimePerSubmitter) {
				dprintf(D_ALWAYS,
					"  Negotiation with %s skipped because of time limits:\n",
					scheddName.Value());
				dprintf(D_ALWAYS,
					"  %d seconds spent, max allowed %d\n ",
					totalTime, MaxTimePerSubmitter);
				result = MM_DONE;
			} else {
				if ( scheddLimit < 1 && spin_pie > 1 ) {
					result = MM_RESUME;
				} else {
					if ( spin_pie == 1 && ConsiderPreemption ) {
						ignore_schedd_limit = true;
					} else {
						ignore_schedd_limit = false;
					}
					int numMatched = 0;
					startTime = time(NULL);
					double limitRWUsed = 0.0;
					result=negotiate( scheddName.Value(),schedd,scheddPrio,
								  scheddAbsShare, scheddLimit, scheddLimitRW,
								  startdAds, claimIds, 
								  scheddVersion, ignore_schedd_limit,
								  startTime, numMatched, limitRWUsed);
					updateNegCycleEndTime(startTime, schedd);

					if( numMatched > scheddLimitWithoutCrumbs ) {
							// this schedd ate some of the free crumbs
						dprintf(D_FULLDEBUG,
						        "Removing up to %d user prio crumb(s) "
						        "from %d total remaining.\n",
						        numMatched - scheddLimitWithoutCrumbs,
						        userprioCrumbs);

						userprioCrumbs -= numMatched - scheddLimitWithoutCrumbs;
						if( userprioCrumbs < 0 ) {
								// The schedd must have gotten some
								// extra machines via startd rank.
								// There is not much point in keeping
								// track of whether the extra takings
								// were user-prio crumbs or startd
								// rank entitlements.  Either way, the
								// pie has shrunk.
							userprioCrumbs = 0;
						}
					}
				}
			}

			switch (result)
			{
				case MM_RESUME:
					// the schedd hit its resource limit.  must resume 
					// negotiations at a later negotiation cycle.
					dprintf(D_FULLDEBUG,
							"  This schedd hit its scheddlimit.\n");
					hit_schedd_prio_limit = TRUE;
					break;
				case MM_DONE: 
					if (rejForNetworkShare) {
							// We negotiated for all jobs, but some
							// jobs were rejected because this user
							// exceeded her fair-share of network
							// resources.  Resume negotiations for
							// this user at a later cycle.
						hit_network_prio_limit = TRUE;
					} else {
							// the schedd got all the resources it
							// wanted. delete this schedd ad.
						dprintf(D_FULLDEBUG,"  Schedd %s got all it wants; "
								"removing it.\n", scheddName.Value() );
						scheddAds.Delete( schedd);
					}
					break;
				case MM_ERROR:
				default:
					dprintf(D_ALWAYS,"  Error: Ignoring schedd for this cycle\n" );
					sockCache->invalidateSock( scheddAddr.Value() );
					scheddAds.Delete( schedd );
			}
		}
		scheddAds.Close();
		// does MaxScheddLimit[RW] ever get updated in this loop?
	} while ( (hit_schedd_prio_limit == TRUE || hit_network_prio_limit == TRUE)
			  && ( useResourceWeights ? (MaxscheddLimitRW > 0.0) : (MaxscheddLimit > 0) )
			  && (startdAds.MyLength() > 0) );

	return TRUE;
}

static int
comparisonFunction (AttrList *ad1, AttrList *ad2, void *m)
{
	char	*scheddName1 = NULL;
	char	*scheddName2 = NULL;
	double	prio1, prio2;
	Matchmaker *mm = (Matchmaker *) m;



	if (!ad1->LookupString (ATTR_NAME, &scheddName1) ||
		!ad2->LookupString (ATTR_NAME, &scheddName2))
	{
		if (scheddName1) free(scheddName1);
		if (scheddName2) free(scheddName2);
		return -1;
	}

	prio1 = mm->accountant.GetPriority(scheddName1);
	prio2 = mm->accountant.GetPriority(scheddName2);
	
	// the scheddAds should be secondarily sorted based on ATTR_NAME
	// because we assume in the code that follows that ads with the
	// same ATTR_NAME are adjacent in the scheddAds list.  this is
	// usually the case because 95% of the time each user in the
	// system has a different priority.

	if (prio1==prio2) {
		int namecomp = strcmp(scheddName1,scheddName2);
		free(scheddName1);
		free(scheddName2);
		if (namecomp != 0) return (namecomp < 0);

			// We don't always want to negotiate with schedds with the
			// same name in the same order or we might end up only
			// running jobs this user has submitted to the first
			// schedd.  The general problem is that we rely on the
			// schedd to order each user's jobs, so when a user
			// submits to multiple schedds, there is no guaranteed
			// order.  Our hack is to order the schedds randomly,
			// which should be a little bit better than always
			// negotiating in the same order.  We use the timestamp on
			// the classads to get a random ordering among the schedds
			// (consistent throughout our sort).

		int ts1=0, ts2=0;
		ad1->LookupInteger (ATTR_LAST_HEARD_FROM, ts1);
		ad2->LookupInteger (ATTR_LAST_HEARD_FROM, ts2);
		return ( (ts1 % 1009) < (ts2 % 1009) );
	}

	free(scheddName1);
	free(scheddName2);
	return (prio1 < prio2);
}

int Matchmaker::
trimStartdAds(ClassAdList &startdAds)
{
	int removed = 0;
	ClassAd *ad = NULL;
	char curState[80];
	char const *claimed_state_str = state_to_string(claimed_state);
	char const *preempting_state_str = state_to_string(preempting_state);
	ASSERT(claimed_state_str && preempting_state_str);

		// If we are not considering preemption, we can save time
		// (and also make the spinning pie algorithm more correct) by
		// getting rid of ads that are not in the Unclaimed state.
	
	if ( ConsiderPreemption ) {
			// we need to keep all the ads.
		return 0;
	}

	startdAds.Open();
	while( (ad=startdAds.Next()) ) {
		if(ad->LookupString(ATTR_STATE, curState, sizeof(curState))) {
			if ( strcmp(curState,claimed_state_str)==0
			     || strcmp(curState,preempting_state_str)==0)
			{
				startdAds.Delete(ad);
				removed++;
			}
		}
	}
	startdAds.Close();

	return removed;
}

double Matchmaker::
sumResourceWeights(ClassAdList &startdAds)
{
	ClassAd *ad = NULL;
	double sum = 0.0;

	startdAds.Open();
	while( (ad=startdAds.Next()) ) {
		float resourceWeight = 1.0;
		if(useResourceWeights)
			ad->EvalFloat(ResourceWeightAttr.Value(), NULL, resourceWeight);
		sum+=resourceWeight;
	}
	return sum;
}

bool Matchmaker::
obtainAdsFromCollector (
						ClassAdList &allAds,
						ClassAdList &startdAds, 
						ClassAdList &scheddAds, 
						ClaimIdHash &claimIds )
{
	CondorQuery privateQuery(STARTD_PVT_AD);
	QueryResult result;
	ClassAd *ad, *oldAd;
	MapEntry *oldAdEntry;
	int newSequence, oldSequence, reevaluate_ad;
	char    *remoteHost = NULL;
	MyString buffer;
	CollectorList* collects = daemonCore->getCollectorList();

	if ( want_simple_matching ) {
		CondorQuery publicQuery(STARTD_AD);
		CondorQuery submittorQuery(SUBMITTOR_AD);

		dprintf(D_ALWAYS, "  Getting all startd ads ...\n");
		result = collects->query(publicQuery,startdAds);
		if( result!=Q_OK ) {
			dprintf(D_ALWAYS, "Couldn't fetch ads: %s\n", getStrQueryResult(result));
			return false;
		}

		dprintf(D_ALWAYS, "  Getting all submittor ads ...\n");
		result = collects->query(submittorQuery,scheddAds);
		if( result!=Q_OK ) {
			dprintf(D_ALWAYS, "Couldn't fetch ads: %s\n", getStrQueryResult(result));
			return false;
		}

		dprintf(D_ALWAYS,"  Getting startd private ads ...\n");
		ClassAdList startdPvtAdList;
		result = collects->query(privateQuery,startdPvtAdList);
		if( result!=Q_OK ) {
			dprintf(D_ALWAYS, "Couldn't fetch ads: %s\n", getStrQueryResult(result));
			return false;
		}
		MakeClaimIdHash(startdPvtAdList,claimIds);

		dprintf(D_ALWAYS, 
			"Got ads (simple matching): %d startd, %d submittor, %d private\n",
	        startdAds.MyLength(),scheddAds.MyLength(),claimIds.getNumElements());

		return true;
	}

	CondorQuery publicQuery(ANY_AD);
	dprintf(D_ALWAYS, "  Getting all public ads ...\n");
	result = collects->query (publicQuery, allAds);
	if( result!=Q_OK ) {
		dprintf(D_ALWAYS, "Couldn't fetch ads: %s\n", getStrQueryResult(result));
		return false;
	}

	dprintf(D_ALWAYS, "  Sorting %d ads ...\n",allAds.MyLength());

	allAds.Open();
	while( (ad=allAds.Next()) ) {

		// Insert each ad into the appropriate list.
		// After we insert it into a list, do not delete the ad...

		// let's see if we've already got it - first lookup the sequence 
		// number from the new ad, then let's look and see if we've already
		// got something for this one.		
		if(!strcmp(ad->GetMyTypeName(),STARTD_ADTYPE)) {

			// first, let's make sure that will want to actually use this
			// ad, and if we can use it (old startds had no seq. number)
			reevaluate_ad = false; 
			ad->LookupBool(ATTR_WANT_AD_REVAULATE, reevaluate_ad);
			newSequence = -1;	
			ad->LookupInteger(ATTR_UPDATE_SEQUENCE_NUMBER, newSequence);

			if(!ad->LookupString(ATTR_NAME, &remoteHost)) {
				dprintf(D_FULLDEBUG,"Rejecting unnamed startd ad.");
				continue;
			}

			// Next, let's transform the ad. The first thing we might
			// do is replace the Requirements attribute with whatever
			// we find in NegotiatorRequirements
			ExprTree  *negReqTree, *reqTree;
			char *subReqs, *newReqs;
			subReqs = newReqs = NULL;
			negReqTree = reqTree = NULL;
			int length;
			// TODO: Does this leak memory?
			negReqTree = ad->Lookup(ATTR_NEGOTIATOR_REQUIREMENTS);
			if ( negReqTree != NULL && negReqTree->RArg() != NULL ) {

				// Save the old requirements expression
				reqTree = ad->Lookup(ATTR_REQUIREMENTS);
				if(reqTree != NULL && reqTree->RArg() != NULL) {
				// Now, put the old requirements back into the ad
				reqTree->RArg()->PrintToNewStr(&subReqs); //Print allocs mem
				length = strlen(subReqs) + strlen(ATTR_REQUIREMENTS) + 7;
				newReqs = (char *)malloc(length+16);
				snprintf(newReqs, length+15, "Saved%s = %s", 
							ATTR_REQUIREMENTS, subReqs); 
				ad->InsertOrUpdate(newReqs);
				free(newReqs);
				free(subReqs);
				} else {
					char *tmpstr;
					reqTree->PrintToNewStr(&tmpstr);
				}
		
				// Get the requirements expression we're going to 
				// subsititute in, and convert it to a string... 
				// Sadly, this might be the best interface :(
				negReqTree->RArg()->PrintToNewStr(&subReqs); //Print allocs mem
				length = strlen(subReqs) + strlen(ATTR_REQUIREMENTS);
				newReqs = (char *)malloc(length+16);

				snprintf(newReqs, length+15, "%s = %s", ATTR_REQUIREMENTS, 
							subReqs); 
				ad->InsertOrUpdate(newReqs);

				free(newReqs);
				free(subReqs);
				
			}

			if( reevaluate_ad && newSequence != -1 ) {
				oldAd = NULL;
				oldAdEntry = NULL;
				MyString rhost(remoteHost);
				stashedAds->lookup( rhost, oldAdEntry);
				// if we find it...
				oldSequence = -1;
				if( oldAdEntry ) {
					oldSequence = oldAdEntry->sequenceNum;
					oldAd = oldAdEntry->oldAd;
				}

					// Find classad expression that decides if
					// new ad should replace old ad
				char *exprStr = param("STARTD_AD_REEVAL_EXPR");
				if (!exprStr) {
						// This matches the "old" semantic.
					exprStr = strdup("target.UpdateSequenceNumber > my.UpdateSequenceNumber");
				}

				ExprTree *expr = NULL;
				::Parse(exprStr, expr); // expr will be null on error

				int replace = true;
				if (expr == NULL) {
					// error evaluating expression
					dprintf(D_ALWAYS, "Can't compile STARTD_AD_REEVAL_EXPR %s, treating as TRUE\n", exprStr);
					replace = true;
				} else {

						// Expression is valid, now evaluate it
						// old ad is "my", new one is "target"
					EvalResult er;
					int evalRet = expr->EvalTree(oldAd, ad, &er);

					if( !evalRet || (er.type != LX_BOOL && er.type != LX_INTEGER)) {
							// Something went wrong
						dprintf(D_ALWAYS, "Can't evaluate STARTD_AD_REEVAL_EXPR %s as a bool, treating as TRUE\n", exprStr);
						replace = true;
					} else {
							// evaluation OK, result type bool
						replace = er.i;
					}

						// But, if oldAd was null (i.e.. the first time), always replace
					if (!oldAd) {
						replace = true;
					}
				}

				free(exprStr);
				delete expr ;

					//if(newSequence > oldSequence) {
				if (replace) {
					if(oldSequence >= 0) {
						delete(oldAdEntry->oldAd);
						delete(oldAdEntry->remoteHost);
						delete(oldAdEntry);
						stashedAds->remove(rhost);
					}
					MapEntry *me = new MapEntry;
					me->sequenceNum = newSequence;
					me->remoteHost = strdup(remoteHost);
					me->oldAd = new ClassAd(*ad); 
					stashedAds->insert(rhost, me); 
				} else {
					/*
					  We have a stashed copy of this ad, and it's the
					  the same or a more recent ad, and we
					  we don't want to use the one in allAds. We determine
					  if an ad is more recent by evaluating an expression
					  from the config file that decides "newness".  By default,
					  this is just based on the sequence number.  However,
					  we need to make sure that the "stashed" ad gets into
					  allAds for this negotiation cycle, but we don't want 
					  to get stuck in a loop evaluating the, so we remove
					  the sequence number before we put it into allAds - this
					  way, when we encounter it a few iteration later we
					  won't reconsider it
					*/

					allAds.Delete(ad);
					ad = new ClassAd(*(oldAdEntry->oldAd));
					ad->Delete(ATTR_UPDATE_SEQUENCE_NUMBER);
					allAds.Insert(ad);
				}
			}
			startdAds.Insert(ad);
		} else if( !strcmp(ad->GetMyTypeName(),SUBMITTER_ADTYPE) ||
				   ( !strcmp(ad->GetMyTypeName(),SCHEDD_ADTYPE) &&
					 !ad->Lookup(ATTR_NUM_USERS) ) ) {
				// CRUFT: Before 7.3.2, submitter ads had a MyType of
				//   "Scheduler". The only way to tell the difference
				//   was that submitter ads didn't have ATTR_NUM_USERS.
    		ad->Assign(ATTR_TOTAL_TIME_IN_CYCLE, 0);
			scheddAds.Insert(ad);
		}
        free(remoteHost);
        remoteHost = NULL;
	}
	allAds.Close();

	dprintf(D_ALWAYS,"  Getting startd private ads ...\n");
	ClassAdList startdPvtAdList;
	result = collects->query (privateQuery, startdPvtAdList);
	if( result!=Q_OK ) {
		dprintf(D_ALWAYS, "Couldn't fetch ads: %s\n", getStrQueryResult(result));
		return false;
	}

	MakeClaimIdHash(startdPvtAdList,claimIds);

	dprintf(D_ALWAYS, "Got ads: %d public and %d private\n",
	        allAds.MyLength(),claimIds.getNumElements());

	dprintf(D_ALWAYS, "Public ads include %d submitter, %d startd\n",
		scheddAds.MyLength(), startdAds.MyLength() );

	return true;
}

void
Matchmaker::MakeClaimIdHash(ClassAdList &startdPvtAdList, ClaimIdHash &claimIds)
{
	ClassAd *ad;
	startdPvtAdList.Open();
	while( (ad = startdPvtAdList.Next()) ) {
		MyString name;
		MyString ip_addr;
		MyString claim_id;

		if( !ad->LookupString(ATTR_NAME, name) ) {
			continue;
		}
		if( !ad->LookupString(ATTR_MY_ADDRESS, ip_addr) )
		{
			continue;
		}
			// As of 7.1.3, we look up CLAIM_ID first and CAPABILITY
			// second.  Someday CAPABILITY can be phased out.
		if( !ad->LookupString(ATTR_CLAIM_ID, claim_id) &&
			!ad->LookupString(ATTR_CAPABILITY, claim_id) )
		{
			continue;
		}

			// hash key is name + ip_addr
		name += ip_addr;
		if( claimIds.insert(name,claim_id)!=0 ) {
			dprintf(D_ALWAYS,
					"WARNING: failed to insert claim id hash table entry "
					"for '%s'\n",name.Value());
		}
	}
	startdPvtAdList.Close();
}

int Matchmaker::
negotiate( char const *scheddName, const ClassAd *scheddAd, double priority, double share,
		   int scheddLimit, double scheddLimitRW,
		   ClassAdList &startdAds, ClaimIdHash &claimIds, 
		   const CondorVersionInfo & scheddVersion,
		   bool ignore_schedd_limit, time_t startTime, 
		   int &numMatched, double &limitRWUsed)
{
	ReliSock	*sock;
	int			reply;
	int			cluster, proc;
	int			result;
	time_t		currentTime;
	ClassAd		request;
	ClassAd		*offer;
	bool		only_consider_startd_rank;
	bool		display_overlimit = true;
	char		remoteUser[128];
	int negotiate_command = NEGOTIATE;

	numMatched = 0;

	// Because of GCB, we may end up contacting a different
	// address than scheddAddr!  This is used for logging (to identify
	// the schedd) and to uniquely identify the host in the socketCache.
	// Do not attempt direct connections to this sinful string!
	MyString scheddAddr;
	if( !scheddAd->LookupString( ATTR_SCHEDD_IP_ADDR, scheddAddr ) ) {
		dprintf( D_ALWAYS, "Matchmaker::negotiate: Internal error: Missing IP address for schedd %s.  Please contact the Condor developers.\n", scheddName);
		return MM_ERROR;
	}

		// Starting w/ ver 6.7.15, the schedd supports the 
		// NEGOTIATE_WITH_SIGATTRS command that expects a
		// list of significant attributes.  
	if ( job_attr_references && scheddVersion.built_since_version(6,7,15) ) {
		negotiate_command = NEGOTIATE_WITH_SIGATTRS;
	}

	// Used for log messages to identify the schedd.
	// Not for other uses, as it may change!
	MyString schedd_id;
	schedd_id.sprintf("%s (%s)", scheddName, scheddAddr.Value());
	
	// 0.  connect to the schedd --- ask the cache for a connection
	sock = sockCache->findReliSock( scheddAddr.Value() );
	if( ! sock ) {
		dprintf( D_FULLDEBUG, "Socket to %s not in cache, creating one\n", 
				 schedd_id.Value() );
			// not in the cache already, create a new connection and
			// add it to the cache.  We want to use a Daemon object to
			// send the first command so we setup a security session. 
		Daemon schedd( scheddAd, DT_SCHEDD, 0 );
		sock = schedd.reliSock( NegotiatorTimeout );
		if( ! sock ) {
			dprintf( D_ALWAYS, "    Failed to connect to %s\n", schedd_id.Value() );
			return MM_ERROR;
		}
		if( ! schedd.startCommand(negotiate_command, sock, NegotiatorTimeout) ) {
			dprintf( D_ALWAYS, "    Failed to send NEGOTIATE to %s\n",
					 schedd_id.Value() );
			delete sock;
			return MM_ERROR;
		}
			// finally, add it to the cache for later...
		sockCache->addReliSock( scheddAddr.Value(), sock );
	} else { 
		dprintf( D_FULLDEBUG, "Socket to %s already in cache, reusing\n", 
				 schedd_id.Value() );
			// this address is already in our socket cache.  since
			// we've already got a TCP connection, we do *NOT* want to
			// use a Daemon::startCommand() to create a new security
			// session, we just want to encode the NEGOTIATE int on
			// the socket...
		sock->encode();
		if( ! sock->put(negotiate_command) ) {
			dprintf( D_ALWAYS, "    Failed to send NEGOTIATE to %s\n",
					 schedd_id.Value() );
			sockCache->invalidateSock( scheddAddr.Value() );
			return MM_ERROR;
		}
	}

	// 1.  send NEGOTIATE command, followed by the scheddName (user@uiddomain)
	sock->encode();
	if (!sock->put(scheddName))
	{
		dprintf (D_ALWAYS, "    Failed to send scheddName to %s\n",
			schedd_id.Value() );
		sockCache->invalidateSock(scheddAddr.Value());
		return MM_ERROR;
	}
	// send the significant attributes if the schedd can understand it
	if ( negotiate_command == NEGOTIATE_WITH_SIGATTRS ) {
		if (!sock->put(job_attr_references)) 
		{
			dprintf (D_ALWAYS, "    Failed to send significant attrs to %s\n",
				schedd_id.Value() );
			sockCache->invalidateSock(scheddAddr.Value());
			return MM_ERROR;
		}
	}
	if (!sock->end_of_message())
	{
		dprintf (D_ALWAYS, "    Failed to send scheddName/eom to %s\n",
			schedd_id.Value() );
		sockCache->invalidateSock(scheddAddr.Value());
		return MM_ERROR;
	}

	
	// 2.  negotiation loop with schedd
	for (numMatched=0;true;numMatched++)
	{
		// Service any interactive commands on our command socket.
		// This keeps condor_userprio hanging to a minimum when
		// we are involved in a lot of schedd negotiating.
		// It also performs the important function of draining out
		// any reschedule requests queued up on our command socket, so
		// we do not negotiate over & over unnecesarily.
		daemonCore->ServiceCommandSocket();
	
		currentTime = time(NULL);

		if( (currentTime - startTime) > MaxTimePerSpin) {
			dprintf (D_ALWAYS, 	
			"    Reached max time per spin: %d ... stopping\n", 
				MaxTimePerSpin);
			break;	// get out of the infinite for loop & stop negotiating
		}


		// Handle the case if we are over the scheddLimit
		if(useResourceWeights) {
			if( limitRWUsed >= scheddLimitRW ) {
				if( ignore_schedd_limit ) {
					only_consider_startd_rank = true;
					if( display_overlimit ) {
						display_overlimit = false;
						dprintf(D_FULLDEBUG,
								"    Over submitter resource limit (%f) ... "
								"only consider startd ranks\n", scheddLimitRW);
					}
				} else {
					dprintf (D_ALWAYS, 	
							 "    Reached submitter resource limit: %f ... stopping\n", limitRWUsed);
					break;	// get out of the infinite for loop & stop negotiating
				}
			} else {
				only_consider_startd_rank = false;
			}
		} else {
			if ( numMatched >= scheddLimit ) {
				if ( ignore_schedd_limit ) {
					only_consider_startd_rank = true;
					if ( display_overlimit ) {  // print message only once
						display_overlimit = false;
						dprintf (D_FULLDEBUG, 	
								 "    Over submitter resource limit (%d) ... "
								 "only consider startd ranks\n", scheddLimit);
					}
				} else {
					dprintf (D_ALWAYS, 	
							 "    Reached submitter resource limit: %d ... stopping\n", numMatched);
					break;	// get out of the infinite for loop & stop negotiating
				}
			} else {
				only_consider_startd_rank = false;
			}
		}


		// 2a.  ask for job information
		dprintf (D_FULLDEBUG, "    Sending SEND_JOB_INFO/eom\n");
		sock->encode();
		if (!sock->put(SEND_JOB_INFO) || !sock->end_of_message())
		{
			dprintf (D_ALWAYS, "    Failed to send SEND_JOB_INFO/eom\n");
			sockCache->invalidateSock(scheddAddr.Value());
			return MM_ERROR;
		}

		// 2b.  the schedd may either reply with JOB_INFO or NO_MORE_JOBS
		dprintf (D_FULLDEBUG, "    Getting reply from schedd ...\n");
		sock->decode();
		if (!sock->get (reply))
		{
			dprintf (D_ALWAYS, "    Failed to get reply from schedd\n");
			sock->end_of_message ();
            sockCache->invalidateSock(scheddAddr.Value());
			return MM_ERROR;
		}

		// 2c.  if the schedd replied with NO_MORE_JOBS, cleanup and quit
		if (reply == NO_MORE_JOBS)
		{
			dprintf (D_ALWAYS, "    Got NO_MORE_JOBS;  done negotiating\n");
			sock->end_of_message ();
				// If we have negotiated above our scheddLimit, we have only
				// considered matching if the offer strictly prefers the request.
				// So in this case, return MM_RESUME since there still may be 
				// jobs which the schedd wants scheduled but have not been considered
				// as candidates for no preemption or user priority preemption.
			if ( useResourceWeights ) {
				if( limitRWUsed >= scheddLimitRW ) {
					return MM_RESUME;
				} else {
					return MM_DONE;
				}
			} else {
				if ( numMatched >= scheddLimit ) {
					return MM_RESUME;
				} else {
					return MM_DONE;
				}
			}
		}
		else
		if (reply != JOB_INFO)
		{
			// something goofy
			dprintf(D_ALWAYS,"    Got illegal command %d from schedd\n",reply);
			sock->end_of_message ();
            sockCache->invalidateSock(scheddAddr.Value());
			return MM_ERROR;
		}

		// 2d.  get the request 
		dprintf (D_FULLDEBUG,"    Got JOB_INFO command; getting classad/eom\n");
		if (!request.initFromStream(*sock) || !sock->end_of_message())
		{
			dprintf(D_ALWAYS, "    JOB_INFO command not followed by ad/eom\n");
			sock->end_of_message();
            sockCache->invalidateSock(scheddAddr.Value());
			return MM_ERROR;
		}
		if (!request.LookupInteger (ATTR_CLUSTER_ID, cluster) ||
			!request.LookupInteger (ATTR_PROC_ID, proc))
		{
			dprintf (D_ALWAYS, "    Could not get %s and %s from request\n",
					ATTR_CLUSTER_ID, ATTR_PROC_ID);
			sockCache->invalidateSock( scheddAddr.Value() );
			return MM_ERROR;
		}
		dprintf(D_ALWAYS, "    Request %05d.%05d:\n", cluster, proc);

		// insert the submitter user priority attributes into the request ad
		// first insert old-style ATTR_SUBMITTOR_PRIO
		request.Assign(ATTR_SUBMITTOR_PRIO , (float)priority );  
		// next insert new-style ATTR_SUBMITTER_USER_PRIO
		request.Assign(ATTR_SUBMITTER_USER_PRIO , (float)priority );  
		// next insert the submitter user usage attributes into the request
		request.Assign(ATTR_SUBMITTER_USER_RESOURCES_IN_USE, 
					   accountant.GetResourcesUsed ( scheddName ));
		float temp_groupQuota, temp_groupUsage;
		if (getGroupInfoFromUserId(scheddName,temp_groupQuota,temp_groupUsage))
		{
			// this is a group, so enter group usage info
			request.Assign(ATTR_SUBMITTER_GROUP_RESOURCES_IN_USE,temp_groupUsage);
			request.Assign(ATTR_SUBMITTER_GROUP_QUOTA,temp_groupQuota);
		}

		// 2e.  find a compatible offer for the request --- keep attempting
		//		to find matches until we can successfully (1) find a match,
		//		AND (2) notify the startd; so quit if we got a MM_GOOD_MATCH,
		//		or if MM_NO_MATCH could be found
		result = MM_BAD_MATCH;
		while (result == MM_BAD_MATCH) 
		{
			// 2e(i).  find a compatible offer
			if (!(offer=matchmakingAlgorithm(scheddName, scheddAddr.Value(), request,
											 startdAds, priority,
											 share, 
											 limitRWUsed, scheddLimitRW,
											 only_consider_startd_rank)))
			{
				int want_match_diagnostics = 0;
				request.LookupBool (ATTR_WANT_MATCH_DIAGNOSTICS,
									want_match_diagnostics);
				char *diagnostic_message = NULL;
				// no match found
				dprintf(D_ALWAYS, "      Rejected %d.%d %s %s: ",
						cluster, proc, scheddName, scheddAddr.Value());
				if (rejForNetwork) {
					diagnostic_message = "insufficient bandwidth";
					dprintf(D_ALWAYS|D_NOHEADER, "%s\n",
							diagnostic_message);
				} else {
					if (rejForNetworkShare) {
						diagnostic_message = "network share exceeded";
					} else if (rejForConcurrencyLimit) {
						diagnostic_message =
							"concurrency limit reached";
					} else if (rejPreemptForPolicy) {
						diagnostic_message =
							"PREEMPTION_REQUIREMENTS == False";
					} else if (rejPreemptForPrio) {
						diagnostic_message = "insufficient priority";
					} else if (rejForGroupQuota) {
						diagnostic_message = "group quota exceeded";
					} else {
						diagnostic_message = "no match found";
					}
					dprintf(D_ALWAYS|D_NOHEADER, "%s\n",
							diagnostic_message);
				}
				sock->encode();
				if ((want_match_diagnostics) ? 
					(!sock->put(REJECTED_WITH_REASON) ||
					 !sock->put(diagnostic_message) ||
					 !sock->end_of_message()) :
					(!sock->put(REJECTED) || !sock->end_of_message()))
					{
						dprintf (D_ALWAYS, "      Could not send rejection\n");
						sock->end_of_message ();
						sockCache->invalidateSock(scheddAddr.Value());
						
						return MM_ERROR;
					}
				result = MM_NO_MATCH;
				continue;
			}

			if ((offer->LookupString(ATTR_PREEMPTING_ACCOUNTING_GROUP, remoteUser)==1) ||
				(offer->LookupString(ATTR_PREEMPTING_USER, remoteUser)==1) ||
				(offer->LookupString(ATTR_ACCOUNTING_GROUP, remoteUser)==1) ||
			    (offer->LookupString(ATTR_REMOTE_USER, remoteUser)==1))
			{
                char	*remoteHost = NULL;
                double	remotePriority;

				offer->LookupString(ATTR_NAME, &remoteHost);
				remotePriority = accountant.GetPriority (remoteUser);


				float newStartdRank;
				float oldStartdRank = 0.0;
				if(! offer->EvalFloat(ATTR_RANK, &request, newStartdRank)) {
					newStartdRank = 0.0;
				}
				offer->LookupFloat(ATTR_CURRENT_RANK, oldStartdRank);

				// got a candidate preemption --- print a helpful message
				dprintf( D_ALWAYS, "      Preempting %s (user prio=%.2f, startd rank=%.2f) on %s "
						 "for %s (user prio=%.2f, startd rank=%.2f)\n", remoteUser,
						 remotePriority, oldStartdRank, remoteHost, scheddName,
						 priority, newStartdRank );
                free(remoteHost);
                remoteHost = NULL;
			}

			// 2e(ii).  perform the matchmaking protocol
			result = matchmakingProtocol (request, offer, claimIds, sock, 
					scheddName, scheddAddr.Value());

			// 2e(iii). if the matchmaking protocol failed, do not consider the
			//			startd again for this negotiation cycle.
			if (result == MM_BAD_MATCH)
				startdAds.Delete (offer);

			// 2e(iv).  if the matchmaking protocol failed to talk to the 
			//			schedd, invalidate the connection and return
			if (result == MM_ERROR)
			{
				sockCache->invalidateSock (scheddAddr.Value());
				return MM_ERROR;
			}
		}

		// 2f.  if MM_NO_MATCH was found for the request, get another request
		if (result == MM_NO_MATCH) 
		{
			numMatched--;		// haven't used any resources this cycle
			continue;
		}

		// 2g.  Delete ad from list so that it will not be considered again in 
		//		this negotiation cycle
		int reevaluate_ad = false;
		offer->LookupBool(ATTR_WANT_AD_REVAULATE, reevaluate_ad);
		if( reevaluate_ad ) {
			reeval(offer);
			// Shuffle this resource to the end of the list.  This way, if
			// two resources with the same RANK match, we'll hand them out
			// in a round-robin way
			startdAds.Delete (offer);
			startdAds.Insert (offer);
		} else  {
			startdAds.Delete (offer);
		}	

		limitRWUsed += GetResourceWeight(offer);

	}


	// break off negotiations
	sock->encode();
	if (!sock->put (END_NEGOTIATE) || !sock->end_of_message())
	{
		dprintf (D_ALWAYS, "    Could not send END_NEGOTIATE/eom\n");
        sockCache->invalidateSock(scheddAddr.Value());
	}

	// ... and continue negotiating with others
	return MM_RESUME;
}

void Matchmaker::
updateNegCycleEndTime(time_t startTime, ClassAd *submitter) {
	MyString buffer;
	time_t endTime;
	int oldTotalTime;

	endTime = time(NULL);
	submitter->LookupInteger(ATTR_TOTAL_TIME_IN_CYCLE, oldTotalTime);
	buffer.sprintf("%s = %ld", ATTR_TOTAL_TIME_IN_CYCLE, (oldTotalTime + 
					(endTime - startTime)) );
	submitter->InsertOrUpdate(buffer.Value());
}

float Matchmaker::
EvalNegotiatorMatchRank(char const *expr_name,ExprTree *expr,
                        ClassAd &request,ClassAd *resource)
{
	EvalResult result;
	float rank = -(FLT_MAX);

	if(expr && expr->EvalTree(resource,&request,&result)) {
		if( result.type == LX_FLOAT ) {
			rank = result.f;
		} else if( result.type == LX_INTEGER ) {
			rank = result.i;
		} else {
			dprintf(D_ALWAYS, "Failed to evaluate %s "
			                  "expression to a float.\n",expr_name);
		}
	} else if(expr) {
		dprintf(D_ALWAYS, "Failed to evaluate %s "
		                  "expression.\n",expr_name);
	}
	return rank;
}

float Matchmaker::
GetResourceWeight(ClassAd *candidate) 
{
	float ResourceWeight = 1.0;
	MyString candidateName;
	if(!useResourceWeights) {
		return ResourceWeight;
	}
	candidate->LookupString(ATTR_NAME, candidateName);
	
	if(candidate->EvalFloat(ResourceWeightAttr.Value(), NULL, 
							  ResourceWeight) == 0) {
		dprintf(D_FULLDEBUG, "Can't get ResourceWeight for '%s'; using 1.0\n", 
				candidateName.Value());
		ResourceWeight = 1.0;
	}
	return ResourceWeight;
}

bool Matchmaker::
GroupQuotaPermits(ClassAd *candidate, double &used, double total) 
{
	float ResourceWeight = GetResourceWeight(candidate);
	if((used + ResourceWeight) <= total) {
		dprintf(D_FULLDEBUG, 
				"GroupQuota available.  ResourceWeight: %.3f "
				"Available: was %.3f, becomes %.3f\n", 
				ResourceWeight, total - used, total - (used+ResourceWeight));
		used += ResourceWeight;
		return true;
	} 
	dprintf(D_FULLDEBUG, "GroupQuota not available.  ResourceWeight: %.3f "
			"Available: %.3f\n", ResourceWeight, total - used);
	return false;
}


/*
Warning: scheddAddr may not be the actual address we'll use to contact the
schedd, thanks to GCB.  It _is_ suitable for use as a unique identifier, for
display to the user, or for calls to sockCache->invalidateSock.
*/
ClassAd *Matchmaker::
matchmakingAlgorithm(const char *scheddName, const char *scheddAddr, ClassAd &request,
					 ClassAdList &startdAds,
					 double preemptPrio, double share,
					 double limitRWUsed, double scheddLimitRW,
					 bool only_for_startdrank)
{
		// to store values pertaining to a particular candidate offer
	ClassAd 		*candidate;
	double			candidateRankValue;
	double			candidatePreJobRankValue;
	double			candidatePostJobRankValue;
	double			candidatePreemptRankValue;
	PreemptState	candidatePreemptState;
		// to store the best candidate so far
	ClassAd 		*bestSoFar = NULL;	
	ClassAd 		*cached_bestSoFar = NULL;	
	double			bestRankValue = -(FLT_MAX);
	double			bestPreJobRankValue = -(FLT_MAX);
	double			bestPostJobRankValue = -(FLT_MAX);
	double			bestPreemptRankValue = -(FLT_MAX);
	PreemptState	bestPreemptState = (PreemptState)-1;
	bool			newBestFound;
		// to store results of evaluations
	char			remoteUser[256];
	EvalResult		result;
	float			tmp;
		// request attributes
	int				requestAutoCluster = -1;

	dprintf(D_FULLDEBUG, "matchmakingAlgorithm: limit %f used %f\n", scheddLimitRW, limitRWUsed);

		// Check resource constraints requested by request
	rejForConcurrencyLimit = 0;
	MyString limits;
	if (request.LookupString(ATTR_CONCURRENCY_LIMITS, limits)) {
		limits.lower_case();
		StringList list(limits.Value());
		char *limit;
		MyString str;
		list.rewind();
		while ((limit = list.next())) {
			double increment;

			ParseConcurrencyLimit(limit, increment);

			str = limit;
			double count = accountant.GetLimit(str);

			double max = accountant.GetLimitMax(str);

			dprintf(D_FULLDEBUG,
					"Concurrency Limit: %s is %f\n",
					limit, count);

			if (count < 0) {
 				EXCEPT("ERROR: Concurrency Limit %s is %f (below 0)",
					   limit, count);
			}

			if (count + increment > max) {
				dprintf(D_FULLDEBUG,
						"Concurrency Limit %s is %f, requesting %f, "
						"but cannot exceed %f\n",
						limit, count, increment, max);

				rejForConcurrencyLimit++;
				return NULL;
			}
		}
	}

	request.LookupInteger(ATTR_AUTO_CLUSTER_ID, requestAutoCluster);

		// If this incoming job is from the same user, same schedd,
		// and is in the same autocluster, and we have a MatchList cache,
		// then we can just pop off
		// the top entry in our MatchList if we have one.  The 
		// MatchList is essentially just a sorted cache of the machine
		// ads that match jobs of this type (i.e. same autocluster).
	if ( MatchList &&
		 cachedAutoCluster != -1 &&
		 cachedAutoCluster == requestAutoCluster &&
		 cachedPrio == preemptPrio &&
		 cachedOnlyForStartdRank == only_for_startdrank &&
		 strcmp(cachedName,scheddName)==0 &&
		 strcmp(cachedAddr,scheddAddr)==0 &&
		 MatchList->cache_still_valid(request,PreemptionReq,PreemptionRank,
					preemption_req_unstable,preemption_rank_unstable) )
	{
		// we can use cached information.  pop off the best
		// candidate from our sorted list.
		cached_bestSoFar = MatchList->pop_candidate();
		dprintf(D_FULLDEBUG,"Attempting to use cached MatchList: %s (MatchList length: %d, Autocluster: %d, Schedd Name: %s, Schedd Address: %s)\n",
			cached_bestSoFar?"Succeeded.":"Failed",
			MatchList->length(),
			requestAutoCluster,
			scheddName,
			scheddAddr
			);
		if ( ! cached_bestSoFar ) {
				// if we don't have a candidate, fill in
				// all the rejection reason counts.
			MatchList->get_diagnostics(
				rejForNetwork,
				rejForNetworkShare,
				rejForConcurrencyLimit,
				rejPreemptForPrio,
				rejPreemptForPolicy,
				rejPreemptForRank,
				rejForGroupQuota);
		}
			//  TODO  - compare results, reserve net bandwidth
		return cached_bestSoFar;
	}

		// Delete our old MatchList, since we know that if we made it here
		// we no longer are dealing with a job from the same autocluster.
		// (someday we will store it in case we see another job with
		// the same autocluster, but we aren't that smart yet...)
	if ( MatchList ) { 
		delete MatchList;
		MatchList = NULL;
		cachedAutoCluster = -1;
		if ( cachedName ) {
			free(cachedName);
			cachedName = NULL;
		}
		if ( cachedAddr ) {
			free(cachedAddr);
			cachedAddr = NULL;
		}
	}

		// Create a new MatchList cache if desired via config file,
		// and the job ad contains autocluster info,
		// and there are machines potentially available to consider.		
	if ( want_matchlist_caching &&		// desired via config file
		 requestAutoCluster != -1 &&	// job ad contains autocluster info
		 startdAds.Length() > 0 )		// machines available
	{
		MatchList = new MatchListType( startdAds.Length() );
		cachedAutoCluster = requestAutoCluster;
		cachedPrio = preemptPrio;
		cachedOnlyForStartdRank = only_for_startdrank;
		cachedName = strdup(scheddName);
		cachedAddr = strdup(scheddAddr);
	}


	// initialize reasons for match failure
	rejForNetwork = 0;
	rejForNetworkShare = 0;
	rejPreemptForPrio = 0;
	rejPreemptForPolicy = 0;
	rejPreemptForRank = 0;
	rejForGroupQuota = 0;

	// scan the offer ads
	startdAds.Open ();
	while ((candidate = startdAds.Next ())) {

			// the candidate offer and request must match
		if( !( *candidate == request ) ) {
				// they don't match; continue
			continue;
		}

		candidatePreemptState = NO_PREEMPTION;

		remoteUser[0] = '\0';
			// If there is already a preempting user, we need to preempt that user.
			// Otherwise, we need to preempt the user who is running the job.
		if (!candidate->LookupString(ATTR_PREEMPTING_ACCOUNTING_GROUP, remoteUser)) {
			if (!candidate->LookupString(ATTR_PREEMPTING_USER, remoteUser)) {
				if (!candidate->LookupString(ATTR_ACCOUNTING_GROUP, remoteUser)) {
					candidate->LookupString(ATTR_REMOTE_USER, remoteUser);
				}
			}
		}

		// if only_for_startdrank flag is true, check if the offer strictly
		// prefers this request.  Since this is the only case we care about
		// when the only_for_startdrank flag is set, if the offer does 
		// not prefer it, just continue with the next offer ad....  we can
		// skip all the below logic about preempt for user-priority, etc.
		if ( only_for_startdrank ) {
			if ( remoteUser[0] == '\0' ) {
					// offer does not have a remote user, thus we cannot eval
					// startd rank yet because it does not make sense (the
					// startd has nothing to compare against).  
					// So try the next offer...
				continue;
			}
			if ( !(rankCondStd->EvalTree(candidate, &request, &result) && 
					result.type == LX_INTEGER && result.i == TRUE) ) {
					// offer does not strictly prefer this request.
					// try the next offer since only_for_statdrank flag is set

				continue;
			}
			// If we made it here, we have a candidate which strictly prefers
			// this request.  Set the candidatePreemptState properly so that
			// we consider PREEMPTION_RANK down below as we should.
			candidatePreemptState = RANK_PREEMPTION;
		}

		// if there is a remote user, consider preemption ....
		// Note: we skip this if only_for_startdrank is true since we already
		//       tested above for the only condition we care about.
		if ( (remoteUser[0] != '\0') &&
			 (!only_for_startdrank) ) {
			if( rankCondStd->EvalTree(candidate, &request, &result) && 
					result.type == LX_INTEGER && result.i == TRUE ) {
					// offer strictly prefers this request to the one
					// currently being serviced; preempt for rank
				candidatePreemptState = RANK_PREEMPTION;
			} else if( accountant.GetPriority(remoteUser) >= preemptPrio +
				PriorityDelta ) {
					// RemoteUser on machine has *worse* priority than request
					// so we can preempt this machine *but* we need to check
					// on two things first
				candidatePreemptState = PRIO_PREEMPTION;
					// (1) we need to make sure that PreemptionReq's hold (i.e.,
					// if the PreemptionReq expression isn't true, dont preempt)
				if (PreemptionReq && 
						!(PreemptionReq->EvalTree(candidate,&request,&result) &&
						result.type == LX_INTEGER && result.i == TRUE) ) {
					rejPreemptForPolicy++;
					continue;
				}
					// (2) we need to make sure that the machine ranks the job
					// at least as well as the one it is currently running 
					// (i.e., rankCondPrioPreempt holds)
				if(!(rankCondPrioPreempt->EvalTree(candidate,&request,&result)&&
						result.type == LX_INTEGER && result.i == TRUE ) ) {
						// machine doesn't like this job as much -- find another
					rejPreemptForRank++;
					continue;
				}
			} else {
					// don't have better priority *and* offer doesn't prefer
					// request --- find another machine
				if (strcmp(remoteUser, scheddName)) {
						// only set rejPreemptForPrio if we aren't trying to
						// preempt one of our own jobs!
					rejPreemptForPrio++;
				}
				continue;
			}
		}

		if(useResourceWeights && 
		   !GroupQuotaPermits(candidate, limitRWUsed, scheddLimitRW)) {
			rejForGroupQuota++;
			continue;
		}

		candidatePreJobRankValue = EvalNegotiatorMatchRank(
		  "NEGOTIATOR_PRE_JOB_RANK",NegotiatorPreJobRank,
		  request,candidate);

		// calculate the request's rank of the offer
		if(!request.EvalFloat(ATTR_RANK,candidate,tmp)) {
			tmp = 0.0;
		}
		candidateRankValue = tmp;

		candidatePostJobRankValue = EvalNegotiatorMatchRank(
		  "NEGOTIATOR_POST_JOB_RANK",NegotiatorPostJobRank,
		  request,candidate);

		candidatePreemptRankValue = -(FLT_MAX);
		if(candidatePreemptState != NO_PREEMPTION) {
			candidatePreemptRankValue = EvalNegotiatorMatchRank(
			  "PREEMPTION_RANK",PreemptionRank,
			  request,candidate);
		}

		if ( MatchList ) {
			MatchList->add_candidate(
					candidate,
					candidateRankValue,
					candidatePreJobRankValue,
					candidatePostJobRankValue,
					candidatePreemptRankValue,
					candidatePreemptState
					);
		}

		// NOTE!!!   IF YOU CHANGE THE LOGIC OF THE BELOW LEXICOGRAPHIC
		// SORT, YOU MUST ALSO CHANGE THE LOGIC IN METHOD
   		//     Matchmaker::MatchListType::sort_compare() !!!
		// THIS STATE OF AFFAIRS IS TEMPORARY.  ONCE WE ARE CONVINVED
		// THAT THE MatchList LOGIC IS WORKING PROPERLY, AND AUTOCLUSTERS
		// ARE AUTOMATIC, THEN THE MatchList SORTING WILL ALWAYS BE USED
		// AND THE LEXICOGRAPHIC SORT BELOW WILL BE REMOVED.
		// - Todd Tannenbaum <tannenba@cs.wisc.edu> 10/2004
		// ----------------------------------------------------------
		// the quality of a match is determined by a lexicographic sort on
		// the following values, but more is better for each component
		//  1. negotiator pre job rank
		//  1. job rank of offer 
		//  2. negotiator post job rank
		//	3. preemption state (2=no preempt, 1=rank-preempt, 0=prio-preempt)
		//  4. preemption rank (if preempting)

		newBestFound = false;
		if(candidatePreJobRankValue < bestPreJobRankValue);
		else if(candidatePreJobRankValue > bestPreJobRankValue) {
			newBestFound = true;
		}
		else if(candidateRankValue < bestRankValue);
		else if(candidateRankValue > bestRankValue) {
			newBestFound = true;
		}
		else if(candidatePostJobRankValue < bestPostJobRankValue);
		else if(candidatePostJobRankValue > bestPostJobRankValue) {
			newBestFound = true;
		}
		else if(candidatePreemptState < bestPreemptState);
		else if(candidatePreemptState > bestPreemptState) {
			newBestFound = true;
		}
		//NOTE: if NO_PREEMPTION, PreemptRank is a constant
		else if(candidatePreemptRankValue < bestPreemptRankValue);
		else if(candidatePreemptRankValue > bestPreemptRankValue) {
			newBestFound = true;
		}

		if( newBestFound || !bestSoFar ) {
			bestSoFar = candidate;
			bestPreJobRankValue = candidatePreJobRankValue;
			bestRankValue = candidateRankValue;
			bestPostJobRankValue = candidatePostJobRankValue;
			bestPreemptState = candidatePreemptState;
			bestPreemptRankValue = candidatePreemptRankValue;
		}
	}
	startdAds.Close ();

	if ( MatchList ) {
		MatchList->set_diagnostics(rejForNetwork, rejForNetworkShare, 
		    rejForConcurrencyLimit,
			rejPreemptForPrio, rejPreemptForPolicy, rejPreemptForRank,
			rejForGroupQuota);
			// only bother sorting if there is more than one entry
		if ( MatchList->length() > 1 ) {
			dprintf(D_FULLDEBUG,"Start of sorting MatchList (len=%d)\n",
				MatchList->length());
			MatchList->sort();
			dprintf(D_FULLDEBUG,"Finished sorting MatchList\n");
		}
		// compare
		ClassAd *bestCached = MatchList->pop_candidate();
		// TODO - do bestCached and bestSoFar refer to the same
		// machine preference? (sanity check)
		if(bestCached != bestSoFar) {
			dprintf(D_ALWAYS, "INSANE: bestCached != bestSoFar\n");
		}
		bestCached = NULL; // just to remove unused variable warning
	}

	if(!bestSoFar)
	{
	/* Insert an entry into the rejects table only if no matches were found at all */
		insert_into_rejects(scheddName,request);
	}
	// this is the best match
	return bestSoFar;
}

class NotifyStartdOfMatchHandler {
public:
	MyString m_startdName;
	MyString m_startdAddr;
	int m_timeout;
	MyString m_claim_id;
	DCStartd m_startd;
	bool m_nonblocking;

	NotifyStartdOfMatchHandler(char const *startdName,char const *startdAddr,int timeout,char const *claim_id,bool nonblocking):
		
		m_startdName(startdName),
		m_startdAddr(startdAddr),
		m_timeout(timeout),
		m_claim_id(claim_id),
		m_startd(startdAddr),
		m_nonblocking(nonblocking) {}

	static void startCommandCallback(bool success,Sock *sock,CondorError * /*errstack*/,void *misc_data)
	{
		NotifyStartdOfMatchHandler *self = (NotifyStartdOfMatchHandler *)misc_data;
		ASSERT(misc_data);

		if(!success) {
			dprintf (D_ALWAYS,"      Failed to initiate socket to send MATCH_INFO to %s\n",
					 self->m_startdName.Value());
		}
		else {
			self->WriteMatchInfo(sock);
		}
		if(sock) {
			delete sock;
		}
		delete self;
	}

	bool WriteMatchInfo(Sock *sock)
	{
		ClaimIdParser idp( m_claim_id.Value() );
		ASSERT(sock);

		// pass the startd MATCH_INFO and claim id string
		dprintf (D_FULLDEBUG, "      Sending MATCH_INFO/claim id to %s\n",
		         m_startdName.Value());
		dprintf (D_FULLDEBUG, "      (Claim ID is \"%s\" )\n",
		         idp.publicClaimId() );

		if ( !sock->put_secret (m_claim_id.Value()) ||
			 !sock->end_of_message())
		{
			dprintf (D_ALWAYS,
			        "      Could not send MATCH_INFO/claim id to %s\n",
			        m_startdName.Value() );
			dprintf (D_FULLDEBUG,
			        "      (Claim ID is \"%s\")\n",
			        idp.publicClaimId() );
			return false;
		}
		return true;
	}

	bool startCommand()
	{
		dprintf (D_FULLDEBUG, "      Connecting to startd %s at %s\n", 
					m_startdName.Value(), m_startdAddr.Value()); 

		if(!m_nonblocking) {
			Stream::stream_type st = m_startd.hasUDPCommandPort() ? Stream::safe_sock : Stream::reli_sock;
			Sock *sock =  m_startd.startCommand(MATCH_INFO,st,m_timeout);
			bool result = false;
			if(!sock) {
				dprintf (D_ALWAYS,"      Failed to initiate socket (blocking mode) to send MATCH_INFO to %s\n",
						 m_startdName.Value());
			}
			else {
				result = WriteMatchInfo(sock);
			}
			if(sock) {
				delete sock;
			}
			delete this;
			return result;
		}

		Stream::stream_type st = m_startd.hasUDPCommandPort() ? Stream::safe_sock : Stream::reli_sock;
		m_startd.startCommand_nonblocking (
			MATCH_INFO,
			st,
			m_timeout,
			NULL,
			NotifyStartdOfMatchHandler::startCommandCallback,
			this);

			// Since this is nonblocking, we cannot give any immediate
			// feedback on whether the message to the startd succeeds.
		return true;
	}
};

void Matchmaker::
insertNegotiatorMatchExprs( ClassAdList &cal )
{
	ClassAd *ad;
	cal.Open();
	while( ( ad = cal.Next() ) ) {
		insertNegotiatorMatchExprs( ad );
	}
	cal.Close();
}

void Matchmaker::
insertNegotiatorMatchExprs(ClassAd *ad)
{
	ASSERT(ad);

	NegotiatorMatchExprNames.rewind();
	NegotiatorMatchExprValues.rewind();
	char const *expr_name;
	while( (expr_name=NegotiatorMatchExprNames.next()) ) {
		char const *expr_value = NegotiatorMatchExprValues.next();
		ASSERT(expr_value);

		ad->AssignExpr(expr_name,expr_value);
	}
}

/*
Warning: scheddAddr may not be the actual address we'll use to contact the
schedd, thanks to GCB.  It _is_ suitable for use as a unique identifier, for
display to the user, or for calls to sockCache->invalidateSock.
*/
int Matchmaker::
matchmakingProtocol (ClassAd &request, ClassAd *offer, 
						ClaimIdHash &claimIds, Sock *sock,
					    const char* scheddName, const char* scheddAddr)
{
	int  cluster, proc;
	MyString startdAddr;
	char remoteUser[512];
	char accountingGroup[256];
	char remoteOwner[256];
    MyString startdName;
	char const *claim_id;
	SafeSock startdSock;
	bool send_failed;
	int want_claiming = -1;
	ExprTree *savedRequirements;
	int length;
	char *tmp;


	// these will succeed
	request.LookupInteger (ATTR_CLUSTER_ID, cluster);
	request.LookupInteger (ATTR_PROC_ID, proc);

	// see if offer supports claiming or not
	offer->LookupBool(ATTR_WANT_CLAIMING,want_claiming);

	// if offer says nothing, see if request says something
	if ( want_claiming == -1 ) {
		request.LookupBool(ATTR_WANT_CLAIMING,want_claiming);
	}

	// these should too, but may not
	if (!offer->LookupString (ATTR_STARTD_IP_ADDR, startdAddr)		||
		!offer->LookupString (ATTR_NAME, startdName))
	{
		// fatal error if we need claiming
		if ( want_claiming ) {
			dprintf (D_ALWAYS, "      Could not lookup %s and %s\n", 
					ATTR_NAME, ATTR_STARTD_IP_ADDR);
			return MM_BAD_MATCH;
		}
	}

	// find the startd's claim id from the private ad
	MyString claim_id_buf;
	if ( want_claiming ) {
		if (!(claim_id = getClaimId (startdName.Value(), startdAddr.Value(), claimIds, claim_id_buf)))
		{
			dprintf(D_ALWAYS,"      %s has no claim id\n", startdName.Value());
			return MM_BAD_MATCH;
		}
	} else {
		// Claiming is *not* desired
		claim_id = "null";
	}

	savedRequirements = NULL;
	length = strlen("Saved") + strlen(ATTR_REQUIREMENTS) + 2;
	tmp = (char *)malloc(length);
	snprintf(tmp, length, "Saved%s", ATTR_REQUIREMENTS);
	savedRequirements = offer->Lookup(tmp);
	free(tmp);
	if(savedRequirements != NULL && savedRequirements->RArg() != NULL) {
		char *savedReqStr, *replacementReqStr;
		savedReqStr = NULL;
		savedRequirements->RArg()->PrintToNewStr(&savedReqStr);
		length = strlen(savedReqStr) + strlen(ATTR_REQUIREMENTS);
        replacementReqStr = (char *)malloc(length+16);
        snprintf(replacementReqStr, length+15, "%s = %s", 
							ATTR_REQUIREMENTS, savedReqStr); 
        offer->InsertOrUpdate(replacementReqStr);
		dprintf(D_ALWAYS, "Inserting %s into the ad\n", replacementReqStr);	
		free(replacementReqStr);
		free(savedReqStr);
	}	

		// Stash the Concurrency Limits in the offer, they are part of
		// what's being provided to the request after all. The limits
		// will be available to the Accountant when the match is added
		// and also to the Schedd when considering to reuse a
		// claim. Both are key, first so the Accountant can properly
		// recreate its state on startup, and second so the Schedd has
		// the option of checking if a claim should be reused for a
		// job incase it has different limits. The second part is
		// because the limits are not in the Requirements.
		//
		// NOTE: Because the Concurrency Limits should be available to
		// the Schedd, they must be stashed before PERMISSION_AND_AD
		// is sent.
	MyString limits;
	if (request.LookupString(ATTR_CONCURRENCY_LIMITS, limits)) {
		limits.lower_case();
		offer->Assign(ATTR_MATCHED_CONCURRENCY_LIMITS, limits);
	} else {
		offer->Delete(ATTR_MATCHED_CONCURRENCY_LIMITS);
	}

	// ---- real matchmaking protocol begins ----
	// 1.  contact the startd 
	if (want_claiming && want_inform_startd) {
			// The following sends a message to the startd to inform it
			// of the match.  Although it is a UDP message, it still may
			// block, because if there is no cached security session,
			// a TCP connection is created.  Therefore, the following
			// handler supports the nonblocking interface to startCommand.

		NotifyStartdOfMatchHandler *h =
			new NotifyStartdOfMatchHandler(
				startdName.Value(),startdAddr.Value(),NegotiatorTimeout,claim_id,want_nonblocking_startd_contact);

		if(!h->startCommand()) {
			return MM_BAD_MATCH;
		}
	}	// end of if want_claiming

	// 3.  send the match and claim_id to the schedd
	sock->encode();
	send_failed = false;	

	dprintf(D_FULLDEBUG,
		"      Sending PERMISSION, claim id, startdAd to schedd\n");
	if (!sock->put(PERMISSION_AND_AD) ||
		!sock->put_secret(claim_id) ||
		!offer->put(*sock)		||	// send startd ad to schedd
		!sock->end_of_message())
	{
			send_failed = true;
	}

	if ( send_failed )
	{
		ClaimIdParser cidp(claim_id);
		dprintf (D_ALWAYS, "      Could not send PERMISSION\n" );
		dprintf( D_FULLDEBUG, "      (Claim ID is \"%s\")\n", cidp.publicClaimId());
		sockCache->invalidateSock( scheddAddr );
		return MM_ERROR;
	}

	if (offer->LookupString(ATTR_REMOTE_USER, remoteOwner) == 0) {
		strcpy(remoteOwner, "none");
	}
	if (offer->LookupString(ATTR_ACCOUNTING_GROUP, accountingGroup)) {
		snprintf(remoteUser,sizeof(remoteUser),"%s (%s=%s)",
			remoteOwner,ATTR_ACCOUNTING_GROUP,accountingGroup);
	} else {
		strcpy(remoteUser,remoteOwner);
	}
	if (offer->LookupString (ATTR_STARTD_IP_ADDR, startdAddr) == 0) {
		startdAddr = "<0.0.0.0:0>";
	}
	dprintf(D_ALWAYS, "      Matched %d.%d %s %s preempting %s %s %s\n",
			cluster, proc, scheddName, scheddAddr, remoteUser,
			startdAddr.Value(), startdName.Value() );

	/* CONDORDB Insert into matches table */
	insert_into_matches(scheddName, request, *offer);

    // 4. notifiy the accountant
	dprintf(D_FULLDEBUG,"      Notifying the accountant\n");
	accountant.AddMatch(scheddName, offer);

	// done
	dprintf (D_ALWAYS, "      Successfully matched with %s\n", startdName.Value());
	return MM_GOOD_MATCH;
}

void
Matchmaker::calculateScheddLimit(
	char const *scheddName,
	char const *groupAccountingName,
	int groupQuota,
	int numStartdAds,
	double maxPrioValue,
	double maxAbsPrioValue,
	double normalFactor,
	double normalAbsFactor,
	float resourceWeight,
	double resourceWeightTotal,
		/* result parameters: */
	int &scheddLimit,
	double &scheddLimitRW,
	int &scheddUsage,
	double &scheddUsageRW,
	double &scheddShare,
	double &scheddAbsShare,
	double &scheddPrio,
	double &scheddPrioFactor,
	double &scheddLimitRoundoff )
{
		// calculate the percentage of machines that this schedd can use
	scheddPrio = accountant.GetPriority ( scheddName );
	scheddUsage = accountant.GetResourcesUsed ( scheddName );
	scheddUsageRW = accountant.GetResourcesUsedFloat( scheddName );
	scheddShare = maxPrioValue/(scheddPrio*normalFactor);
	double unroundedScheddLimit;
	if ( param_boolean("NEGOTIATOR_IGNORE_USER_PRIORITIES",false) ) {
			// why is this not assigned to numStartdAds?
		unroundedScheddLimit = 500000;
		scheddLimitRW = DBL_MAX;
	} else {
		unroundedScheddLimit = (scheddShare*numStartdAds)-scheddUsage;
		scheddLimitRW = (scheddShare*resourceWeightTotal)-scheddUsageRW;
	}
	if( unroundedScheddLimit < 0 ) {
		unroundedScheddLimit = 0;
	}
	if( scheddLimitRW < 0 ) {
		scheddLimitRW = 0.0;
	}

	if ( groupAccountingName ) {
		int maxAllowed = groupQuota - accountant.GetResourcesUsed(groupAccountingName);
		float maxAllowedRW = resourceWeightTotal 
			- accountant.GetResourcesUsedFloat(groupAccountingName);
		if ( maxAllowed < 0 ) maxAllowed = 0;
		if ( maxAllowedRW < 0 ) maxAllowedRW = 0.0;
		if ( unroundedScheddLimit > maxAllowed ) {
			unroundedScheddLimit = maxAllowed;
		}
		if ( scheddLimitRW > maxAllowedRW ) {
			scheddLimitRW = maxAllowedRW;
		}
	}

	scheddLimit  = (int) rint(unroundedScheddLimit);
	scheddLimitRoundoff = unroundedScheddLimit - scheddLimit;
	if(scheddLimitRoundoff < 0.0) {
		dprintf(D_ALWAYS, "Negative scheddLimitRoundoff: %.3f\n", scheddLimitRoundoff);
	}

		// calculate this schedd's absolute fair-share for allocating
		// resources other than CPUs (like network capacity and licenses)
	scheddPrioFactor = accountant.GetPriorityFactor ( scheddName );
	scheddAbsShare =
		maxAbsPrioValue/(scheddPrioFactor*normalAbsFactor);
}

void
Matchmaker::calculateUserPrioCrumbs(
	ClassAdList &scheddAds,
	char const *groupAccountingName,
	int groupQuota,
	int numStartdAds,
	double maxPrioValue,
	double maxAbsPrioValue,
	double normalFactor,
	double normalAbsFactor,
	double resourceWeightTotal,
		/* result parameters: */
	int &userprioCrumbs )
{
	ClassAd *schedd;

		// Calculate how many machines are left over after dishing out
		// rounded integer shares of machines to each submitter.
		// What's left are the user-prio "pie crumbs".
	userprioCrumbs = 0;

	double roundoff_sum = 0.0;

	scheddAds.Open();
	while ((schedd = scheddAds.Next()))
	{
		int scheddLimit = 0;
		int scheddUsage = 0;
		double scheddShare = 0.0;
		double scheddAbsShare = 0.0;
		double scheddPrio = 0.0;
		double scheddPrioFactor = 0.0;
		MyString scheddName;
		double scheddLimitRoundoff = 0.0;
		float resourceWeight = 1.0;
		double scheddLimitRW = 0.0;
		double scheddUsageRW = 0.0;

		schedd->LookupString( ATTR_NAME, scheddName );
		if(useResourceWeights)
			schedd->EvalFloat(ResourceWeightAttr.Value(), NULL, resourceWeight);

		calculateScheddLimit(
			scheddName.Value(),
			groupAccountingName,
			groupQuota,
			numStartdAds,
			maxPrioValue,
			maxAbsPrioValue,
			normalFactor,
			normalAbsFactor,
			resourceWeight,
			resourceWeightTotal,
				/* result parameters: */
			scheddLimit,
			scheddLimitRW,
			scheddUsage,
			scheddUsageRW,
			scheddShare,
			scheddAbsShare,
			scheddPrio,
			scheddPrioFactor,
			scheddLimitRoundoff );
		roundoff_sum += scheddLimitRoundoff;
	}
	scheddAds.Close();

	userprioCrumbs = (int)rint( roundoff_sum );
	if(userprioCrumbs < 0) {
		dprintf(D_ALWAYS, "Negative userprioCrumbs: %d\n", userprioCrumbs);
	}
}

void Matchmaker::
calculateNormalizationFactor (ClassAdList &scheddAds,
							  double &max, double &normalFactor,
							  double &maxAbs, double &normalAbsFactor)
{
	ClassAd *ad;
	char	*scheddName = NULL;
	double	prio, prioFactor;
	char	*old_scheddName = NULL;

	// find the maximum of the priority values (i.e., lowest priority)
	max = maxAbs = DBL_MIN;
	scheddAds.Open();
	while ((ad = scheddAds.Next()))
	{
		// this will succeed (comes from collector)
		ad->LookupString (ATTR_NAME, &scheddName);
		prio = accountant.GetPriority (scheddName);
		if (prio > max) max = prio;
		prioFactor = accountant.GetPriorityFactor (scheddName);
		if (prioFactor > maxAbs) maxAbs = prioFactor;
		free(scheddName);
		scheddName = NULL;
	}
	scheddAds.Close();

	// calculate the normalization factor, i.e., sum of the (max/scheddprio)
	// also, do not factor in ads with the same ATTR_NAME more than once -
	// ads with the same ATTR_NAME signify the same user submitting from multiple
	// machines.
	normalFactor = 0.0;
	normalAbsFactor = 0.0;
	scheddAds.Open();
	while ((ad = scheddAds.Next()))
	{
		ad->LookupString (ATTR_NAME, &scheddName);
		if ( scheddName != NULL && old_scheddName != NULL )
		{
			if ( strcmp(scheddName,old_scheddName) == 0 )
			{
				free(old_scheddName);
				old_scheddName = scheddName;
				continue;
			}
		}
		if ( old_scheddName != NULL )
		{
			free(old_scheddName);
			old_scheddName = NULL;
		}
		old_scheddName = scheddName;
		prio = accountant.GetPriority (scheddName);
		normalFactor = normalFactor + max/prio;
		prioFactor = accountant.GetPriorityFactor (scheddName);
		normalAbsFactor = normalAbsFactor + maxAbs/prioFactor;
	}
	if ( scheddName != NULL )
	{
		free(scheddName);
		scheddName = NULL;
	}
	scheddAds.Close();

	// done
	return;
}


char const *
Matchmaker::getClaimId (const char *startdName, const char *startdAddr, ClaimIdHash &claimIds, MyString &claim_id_buf)
{
	MyString key = startdName;
	key += startdAddr;
	if( claimIds.lookup(key,claim_id_buf)!=0 ) {
		return NULL;
	}
	return claim_id_buf.Value();
}

void Matchmaker::
addRemoteUserPrios( ClassAd	*ad )
{	
	MyString	remoteUser;
	MyString	buffer,buffer1,buffer2,buffer3;
	MyString    slot_prefix;
	float	prio;
	int     total_slots, i;
	float     preemptingRank;
	float temp_groupQuota, temp_groupUsage;

	if ( !ConsiderPreemption ) {
			// Hueristic - no need to take the time to populate ad with 
			// accounting information if no preemption is to be considered.
		return;
	}

		// If there is a preempting user, use that for computing remote user prio.
		// Otherwise, use the current user.
	if( ad->LookupString( ATTR_PREEMPTING_ACCOUNTING_GROUP , remoteUser ) ||
		ad->LookupString( ATTR_PREEMPTING_USER , remoteUser ) ||
		ad->LookupString( ATTR_ACCOUNTING_GROUP , remoteUser ) ||
		ad->LookupString( ATTR_REMOTE_USER , remoteUser ) ) 
	{
		prio = (float) accountant.GetPriority( remoteUser.Value() );
		ad->Assign(ATTR_REMOTE_USER_PRIO, prio);
		ad->Assign(ATTR_REMOTE_USER_RESOURCES_IN_USE,
			accountant.GetResourcesUsed( remoteUser.Value() ));
		if (getGroupInfoFromUserId(remoteUser.Value(),
									temp_groupQuota,temp_groupUsage))
		{
			// this is a group, so enter group usage info
			ad->Assign(ATTR_REMOTE_GROUP_RESOURCES_IN_USE,temp_groupUsage);
			ad->Assign(ATTR_REMOTE_GROUP_QUOTA,temp_groupQuota);
		}
	}
	if( ad->LookupFloat( ATTR_PREEMPTING_RANK, preemptingRank ) ) {
			// There is already a preempting claim (waiting for the previous
			// claim to retire), so set current rank to the preempting
			// rank, since any new preemption must trump the
			// current preempter.
		ad->Assign(ATTR_CURRENT_RANK, preemptingRank);
	}
		
	char* resource_prefix = param("STARTD_RESOURCE_PREFIX");
	if (!resource_prefix) {
		resource_prefix = strdup("slot");
	}
	total_slots = 0;
	if (!ad->LookupInteger(ATTR_TOTAL_SLOTS, total_slots)) {
		total_slots = 0;
	}
	if (!total_slots && (param_boolean("ALLOW_VM_CRUFT", true))) {
		if (!ad->LookupInteger(ATTR_TOTAL_VIRTUAL_MACHINES, total_slots)) {
			total_slots = 0;
		}
	}
		// This won't fire if total_slots is still 0...
	for(i = 1; i <= total_slots; i++) {
		slot_prefix.sprintf("%s%d_", resource_prefix, i);
		buffer.sprintf("%s%s", slot_prefix.Value(), ATTR_PREEMPTING_ACCOUNTING_GROUP);
		buffer1.sprintf("%s%s", slot_prefix.Value(), ATTR_PREEMPTING_USER);
		buffer2.sprintf("%s%s", slot_prefix.Value(), ATTR_ACCOUNTING_GROUP);
		buffer3.sprintf("%s%s", slot_prefix.Value(), ATTR_REMOTE_USER);
			// If there is a preempting user, use that for computing remote user prio.
		if( ad->LookupString( buffer.Value() , remoteUser ) ||
			ad->LookupString( buffer1.Value() , remoteUser ) ||
			ad->LookupString( buffer2.Value() , remoteUser ) ||
			ad->LookupString( buffer3.Value() , remoteUser ) ) 
		{
				// If there is a user on that VM, stick that user's priority
				// information into the ad	
			prio = (float) accountant.GetPriority( remoteUser.Value() );
			buffer.sprintf("%s%s", slot_prefix.Value(), 
					ATTR_REMOTE_USER_PRIO);
			ad->Assign(buffer.Value(),prio);
			buffer.sprintf("%s%s", slot_prefix.Value(), 
					ATTR_REMOTE_USER_RESOURCES_IN_USE);
			ad->Assign(buffer.Value(),
					accountant.GetResourcesUsed(remoteUser.Value()));
			if (getGroupInfoFromUserId(remoteUser.Value(),
										temp_groupQuota,temp_groupUsage))
			{
					// this is a group, so enter group usage info
				buffer.sprintf("%s%s = %d", slot_prefix.Value(), 
					ATTR_REMOTE_GROUP_RESOURCES_IN_USE, 
					temp_groupUsage);
				ad->Insert( buffer.Value() );
				buffer.sprintf("%s%s = %d", slot_prefix.Value(),
					ATTR_REMOTE_GROUP_QUOTA, 
					temp_groupQuota);
				ad->Insert( buffer.Value() );
			}
		}	
	}
	free( resource_prefix );
}

void Matchmaker::
reeval(ClassAd *ad) 
{
	int cur_matches;
	MapEntry *oldAdEntry = NULL;
	char    *remoteHost = NULL;
	char    buffer[255];
	
	cur_matches = 0;
	ad->EvalInteger("CurMatches", NULL, cur_matches);

	ad->LookupString(ATTR_NAME, &remoteHost);
	MyString rhost(remoteHost);
	stashedAds->lookup( rhost, oldAdEntry);
		
	cur_matches++;
	snprintf(buffer, 255, "CurMatches = %d", cur_matches);
	ad->InsertOrUpdate(buffer);
	if(oldAdEntry) {
		delete(oldAdEntry->oldAd);
		oldAdEntry->oldAd = new ClassAd(*ad);
	}
    free(remoteHost);
}

unsigned int Matchmaker::HashFunc(const MyString &Key) {
	return Key.Hash();
}

Matchmaker::MatchListType::
MatchListType(int maxlen)
{
	ASSERT(maxlen > 0);
	AdListArray = new AdListEntry[maxlen];
	ASSERT(AdListArray);
	adListMaxLen = maxlen;
	already_sorted = false;
	adListLen = 0;
	adListHead = 0;
	m_rejForNetwork = 0; 
	m_rejForNetworkShare = 0;
	m_rejForConcurrencyLimit = 0;
	m_rejPreemptForPrio = 0;
	m_rejPreemptForPolicy = 0; 
	m_rejPreemptForRank = 0;
	m_rejForGroupQuota = 0;
}

Matchmaker::MatchListType::
~MatchListType()
{
	if (AdListArray) {
		delete [] AdListArray;
	}
}


#if 0
Matchmaker::AdListEntry* Matchmaker::MatchListType::
peek_candidate()
{
	ClassAd* candidate = NULL;
	int temp_adListHead = adListHead;

	while ( temp_adListHead < adListLen && !candidate ) {
		candidate = AdListArray[temp_adListHead].ad;
		temp_adListHead++;
	}

	if ( candidate ) {
		temp_adListHead--;
		ASSERT( temp_adListHead >= 0 );
		return AdListArray[temp_adListHead];
	} else {
		return NULL;
	}
}
#endif

ClassAd* Matchmaker::MatchListType::
pop_candidate()
{
	ClassAd* candidate = NULL;

	while ( adListHead < adListLen && !candidate ) {
		candidate = AdListArray[adListHead].ad;
		adListHead++;
	}

	return candidate;
}

bool Matchmaker::MatchListType::
cache_still_valid(ClassAd &request, ExprTree *preemption_req, ExprTree *preemption_rank,
				  bool preemption_req_unstable, bool preemption_rank_unstable)
{
	AdListEntry* next_entry = NULL;

	if ( !preemption_req_unstable && !preemption_rank_unstable ) {
		return true;
	}

	// Set next_entry to be a "peek" at the next entry on
	// our cached match list, i.e. don't actually pop it off our list.
	{
		ClassAd* candidate = NULL;
		int temp_adListHead = adListHead;

		while ( temp_adListHead < adListLen && !candidate ) {
			candidate = AdListArray[temp_adListHead].ad;
			temp_adListHead++;
		}

		if ( candidate ) {
			temp_adListHead--;
			ASSERT( temp_adListHead >= 0 );
			next_entry =  &AdListArray[temp_adListHead];
		} else {
			next_entry = NULL;
		}
	}

	if ( preemption_req_unstable ) 
	{
		if ( !next_entry ) {
			return false;
		}
		
		if ( next_entry->PreemptStateValue == PRIO_PREEMPTION ) {
			EvalResult result;
			if (preemption_req && 
				!(preemption_req->EvalTree(next_entry->ad,&request,&result) &&
						result.type == LX_INTEGER && result.i == TRUE) ) 
			{
				dprintf(D_FULLDEBUG,
					"Cache invalidated due to preemption_requirements\n");
				return false;
			}
		}
	}

	if ( next_entry && preemption_rank_unstable ) 
	{		
		if( next_entry->PreemptStateValue != NO_PREEMPTION) {
			double candidatePreemptRankValue = -(FLT_MAX);
			candidatePreemptRankValue = EvalNegotiatorMatchRank(
					"PREEMPTION_RANK",preemption_rank,request,next_entry->ad);
			if ( candidatePreemptRankValue != next_entry->PreemptRankValue ) {
				// ranks don't match ....  now what?
				// ideally we would just want to resort the cache, but for now
				// we do the safest thing - just invalidate the cache.
				dprintf(D_FULLDEBUG,
					"Cache invalidated due to preemption_rank\n");
				return false;
				
			}
		}
	}

	return true;
}


void Matchmaker::MatchListType::
get_diagnostics(int & rejForNetwork,
					int & rejForNetworkShare,
					int & rejForConcurrencyLimit,
					int & rejPreemptForPrio,
					int & rejPreemptForPolicy,
				    int & rejPreemptForRank,
				    int & rejForGroupQuota)
{
	rejForNetwork = m_rejForNetwork;
	rejForNetworkShare = m_rejForNetworkShare;
	rejForConcurrencyLimit = m_rejForConcurrencyLimit;
	rejPreemptForPrio = m_rejPreemptForPrio;
	rejPreemptForPolicy = m_rejPreemptForPolicy;
	rejPreemptForRank = m_rejPreemptForRank;
	rejForGroupQuota = m_rejForGroupQuota;
}

void Matchmaker::MatchListType::
set_diagnostics(int rejForNetwork,
					int rejForNetworkShare,
					int rejForConcurrencyLimit,
					int rejPreemptForPrio,
					int rejPreemptForPolicy,
				    int rejPreemptForRank,
				    int rejForGroupQuota)
{
	m_rejForNetwork = rejForNetwork;
	m_rejForNetworkShare = rejForNetworkShare;
	m_rejForConcurrencyLimit = rejForConcurrencyLimit;
	m_rejPreemptForPrio = rejPreemptForPrio;
	m_rejPreemptForPolicy = rejPreemptForPolicy;
	m_rejPreemptForRank = rejPreemptForRank;
	m_rejForGroupQuota = rejForGroupQuota;
}

void Matchmaker::MatchListType::
add_candidate(ClassAd * candidate,
					double candidateRankValue,
					double candidatePreJobRankValue,
					double candidatePostJobRankValue,
					double candidatePreemptRankValue,
					PreemptState candidatePreemptState)
{
	ASSERT(AdListArray);
	ASSERT(adListLen < adListMaxLen);  // don't write off end of array!

	AdListArray[adListLen].ad = candidate;
	AdListArray[adListLen].RankValue = candidateRankValue;
	AdListArray[adListLen].PreJobRankValue = candidatePreJobRankValue;
	AdListArray[adListLen].PostJobRankValue = candidatePostJobRankValue;
	AdListArray[adListLen].PreemptRankValue = candidatePreemptRankValue;
	AdListArray[adListLen].PreemptStateValue = candidatePreemptState;

	adListLen++;
}


int Matchmaker::
groupSortCompare(const void* elem1, const void* elem2)
{
	const SimpleGroupEntry* Elem1 = (const SimpleGroupEntry*) elem1;
	const SimpleGroupEntry* Elem2 = (const SimpleGroupEntry*) elem2;

	if ( Elem1->prio < Elem2->prio ) {
		return -1;
	} 
	if ( Elem1->prio == Elem2->prio ) {
		return 0;
	} 
	return 1;
}

int Matchmaker::MatchListType::
sort_compare(const void* elem1, const void* elem2)
{
	const AdListEntry* Elem1 = (const AdListEntry*) elem1;
	const AdListEntry* Elem2 = (const AdListEntry*) elem2;

	const double candidateRankValue = Elem1->RankValue;
	const double candidatePreJobRankValue = Elem1->PreJobRankValue;
	const double candidatePostJobRankValue = Elem1->PostJobRankValue;
	const double candidatePreemptRankValue = Elem1->PreemptRankValue;
	const PreemptState candidatePreemptState = Elem1->PreemptStateValue;

	const double bestRankValue = Elem2->RankValue;
	const double bestPreJobRankValue = Elem2->PreJobRankValue;
	const double bestPostJobRankValue = Elem2->PostJobRankValue;
	const double bestPreemptRankValue = Elem2->PreemptRankValue;
	const PreemptState bestPreemptState = Elem2->PreemptStateValue;

	if ( candidateRankValue == bestRankValue &&
		 candidatePreJobRankValue == bestPreJobRankValue &&
		 candidatePostJobRankValue == bestPostJobRankValue &&
		 candidatePreemptRankValue == bestPreemptRankValue &&
		 candidatePreemptState == bestPreemptState )
	{
		return 0;
	}

	// the quality of a match is determined by a lexicographic sort on
	// the following values, but more is better for each component
	//  1. negotiator pre job rank
	//  1. job rank of offer 
	//  2. negotiator post job rank
	//	3. preemption state (2=no preempt, 1=rank-preempt, 0=prio-preempt)
	//  4. preemption rank (if preempting)

	bool newBestFound = false;

	if(candidatePreJobRankValue < bestPreJobRankValue);
	else if(candidatePreJobRankValue > bestPreJobRankValue) {
		newBestFound = true;
	}
	else if(candidateRankValue < bestRankValue);
	else if(candidateRankValue > bestRankValue) {
		newBestFound = true;
	}
	else if(candidatePostJobRankValue < bestPostJobRankValue);
	else if(candidatePostJobRankValue > bestPostJobRankValue) {
		newBestFound = true;
	}
	else if(candidatePreemptState < bestPreemptState);
	else if(candidatePreemptState > bestPreemptState) {
		newBestFound = true;
	}
	//NOTE: if NO_PREEMPTION, PreemptRank is a constant
	else if(candidatePreemptRankValue < bestPreemptRankValue);
	else if(candidatePreemptRankValue > bestPreemptRankValue) {
		newBestFound = true;
	}

	if ( newBestFound ) {
		// candidate is better: candidate is elem1, and qsort man page
		// says return < 0 is elem1 is less than elem2
		return -1;
	} else {
		return 1;
	}
}
			
void Matchmaker::MatchListType::
sort()
{
	// Should only be called ONCE.  If we call for a sort more than
	// once, this code has a bad logic errror, so ASSERT it.
	ASSERT(already_sorted == false);

	// Note: since we must use static members, sort() is
	// _NOT_ thread safe!!!
	qsort(AdListArray,adListLen,sizeof(AdListEntry),sort_compare);

	already_sorted = true;
}


void Matchmaker::
init_public_ad()
{
	MyString line;

	if( publicAd ) delete( publicAd );
	publicAd = new ClassAd();

	publicAd->SetMyTypeName(NEGOTIATOR_ADTYPE);
	publicAd->SetTargetTypeName("");

	char* defaultName = NULL;
	if( NegotiatorName ) {
		line.sprintf("%s = \"%s\"", ATTR_NAME, NegotiatorName );
	} else {
		defaultName = default_daemon_name();
		if( ! defaultName ) {
			EXCEPT( "default_daemon_name() returned NULL" );
		}
		line.sprintf("%s = \"%s\"", ATTR_NAME, defaultName );
		delete [] defaultName;
	}
	publicAd->Insert(line.Value());

	line.sprintf ("%s = \"%s\"", ATTR_NEGOTIATOR_IP_ADDR,
			daemonCore->InfoCommandSinfulString() );
	publicAd->Insert(line.Value());

#if !defined(WIN32)
	line.sprintf("%s = %d", ATTR_REAL_UID, (int)getuid() );
	publicAd->Insert(line.Value());
#endif

        // Publish all DaemonCore-specific attributes, which also handles
        // NEGOTIATOR_ATTRS for us.
    daemonCore->publish(publicAd);
}

void
Matchmaker::updateCollector() {
	dprintf(D_FULLDEBUG, "enter Matchmaker::updateCollector\n");

		// log classad into sql log so that it can be updated to DB
	FILESQL::daemonAdInsert(publicAd, "NegotiatorAd", FILEObj, prevLHF);	

	if (publicAd) {
#if HAVE_DLOPEN
		NegotiatorPluginManager::Update(*publicAd);
#endif
		daemonCore->sendUpdates(UPDATE_NEGOTIATOR_AD, publicAd, NULL, true);
	}

			// Reset the timer so we don't do another period update until 
	daemonCore->Reset_Timer( update_collector_tid, update_interval, update_interval );

	dprintf( D_FULLDEBUG, "exit Matchmaker::UpdateCollector\n" );
}


void
Matchmaker::invalidateNegotiatorAd( void )
{
	ClassAd cmd_ad;
	MyString line;

		// Set the correct types
	cmd_ad.SetMyTypeName( QUERY_ADTYPE );
	cmd_ad.SetTargetTypeName( NEGOTIATOR_ADTYPE );

		// We only want to invalidate this negotiator.  using our
		// sinful string seems like the safest bet for that, since
		// even if the names somehow get messed up, at least the
		// sinful string should be unique...
	line.sprintf( "%s = %s == \"%s\"", ATTR_REQUIREMENTS,
				  ATTR_NEGOTIATOR_IP_ADDR,
				  daemonCore->InfoCommandSinfulString() );
	cmd_ad.Insert( line.Value() );

	daemonCore->sendUpdates( INVALIDATE_NEGOTIATOR_ADS, &cmd_ad, NULL, false );
}

/* CONDORDB functions */
void Matchmaker::insert_into_rejects(char const *userName, ClassAd& job)
{
	int cluster, proc;
//	char startdname[80];
	char globaljobid[200];
	char scheddName[200];
	ClassAd tmpCl;
	ClassAd *tmpClP = &tmpCl;
	char tmp[512];

	time_t clock;

	(void)time(  (time_t *)&clock );

	job.LookupInteger (ATTR_CLUSTER_ID, cluster);
	job.LookupInteger (ATTR_PROC_ID, proc);
	job.LookupString( ATTR_GLOBAL_JOB_ID, globaljobid); 
	get_scheddname_from_gjid(globaljobid,scheddName);
//	machine.LookupString(ATTR_NAME, startdname);

	snprintf(tmp, 512, "reject_time = %d", (int)clock);
	tmpClP->Insert(tmp);
	
	tmpClP->Assign("username",userName);
		
	snprintf(tmp, 512, "scheddname = \"%s\"", scheddName);
	tmpClP->Insert(tmp);
	
	snprintf(tmp, 512, "cluster_id = %d", cluster);
	tmpClP->Insert(tmp);

	snprintf(tmp, 512, "proc_id = %d", proc);
	tmpClP->Insert(tmp);

	snprintf(tmp, 512, "GlobalJobId = \"%s\"", globaljobid);
	tmpClP->Insert(tmp);
	
	FILEObj->file_newEvent("Rejects", tmpClP);
}
void Matchmaker::insert_into_matches(char const * userName,ClassAd& request, ClassAd& offer)
{
	char startdname[80],remote_user[80];
	char globaljobid[200];
	float remote_prio;
	int cluster, proc;
	char scheddName[200];
	ClassAd tmpCl;
	ClassAd *tmpClP = &tmpCl;

	time_t clock;
	char tmp[512];

	(void)time(  (time_t *)&clock );

	request.LookupInteger (ATTR_CLUSTER_ID, cluster);
	request.LookupInteger (ATTR_PROC_ID, proc);
	request.LookupString( ATTR_GLOBAL_JOB_ID, globaljobid); 
	get_scheddname_from_gjid(globaljobid,scheddName);
	offer.LookupString( ATTR_NAME, startdname); 

	snprintf(tmp, 512, "match_time = %d", (int) clock);
	tmpClP->Insert(tmp);
	
	tmpClP->Assign("username",userName);
		
	snprintf(tmp, 512, "scheddname = \"%s\"", scheddName);
	tmpClP->Insert(tmp);
	
	snprintf(tmp, 512, "cluster_id = %d", cluster);
	tmpClP->Insert(tmp);

	snprintf(tmp, 512, "proc_id = %d", proc);
	tmpClP->Insert(tmp);

	snprintf(tmp, 512, "GlobalJobId = \"%s\"", globaljobid);
	tmpClP->Insert(tmp);

	snprintf(tmp, 512, "machine_id = \"%s\"", startdname);
	tmpClP->Insert(tmp);

	if(offer.LookupString( ATTR_REMOTE_USER, remote_user) != 0)
	{
		remote_prio = (float) accountant.GetPriority(remote_user);

		snprintf(tmp, 512, "remote_user = \"%s\"", remote_user);
		tmpClP->Insert(tmp);

		snprintf(tmp, 512, "remote_priority = %f", remote_prio);
		tmpClP->Insert(tmp);
	}
	
	FILEObj->file_newEvent("Matches", tmpClP);
}
/* This extracts the machine name from the global job ID [user@]machine.name#timestamp#cluster.proc*/
static int get_scheddname_from_gjid(const char * globaljobid, char * scheddname )
{
	int i;

	scheddname[0] = '\0';

	for (i=0;
         globaljobid[i]!='\0' && globaljobid[i]!='#';i++)
		scheddname[i]=globaljobid[i];

	if(globaljobid[i] == '\0') 
	{
		scheddname[0] = '\0';
		return -1; /* Parse error, shouldn't happen */
	}
	else if(globaljobid[i]=='#')
	{
		scheddname[i]='\0';	
		return 1;
	}

	return -1;
}
