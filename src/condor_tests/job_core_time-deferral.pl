#!/usr/bin/env perl
#########################################################
## Job Deferral Tests
##
## This test is comprised of smaller tests that will
## check the smaller pieces of the job deferral feature
## If any of the subtests fail, the other tests will still
## run but the entire test will exit with a failure. 
## fail. All the files are deleted after the test completes,
## UNLESS the script is passed a true flag. The files will
## be deleted before each subtest is ran.
##
## We also can be passed in a universe, the default is vanilla
##
## Test #1
## Job is deferred for a small period of time based on
## their current time
##
## Test #2
## Job is deferred for the same time as in Test #1 but 
## now the execution time is based on our clock. This will
## check that the job runs exactly when we say it should.
## We allow a small 1 second fluff time to check to make
## sure that the times are exact (which might not be what
## we want to do).
##
## Test #3
## The job is set to have ran sometime in the past, so 
## we want to make sure that it fails and is never executed
##
## Test #4
## The job is set to have ran in the past like in Test #3
## but we set a window time greater larger then the offset
## so the job should still run
##
#########################################################
use CondorTest;
use POSIX;

$testname = 'Job Deferral Testing - ';

##
## Universe
## 
my $universe = ($ARGV[0] ? $ARGV[0] : "vanilla");

##
## Check if we should cleanup afterwards
##
my $cleanup = ($ARGV[1] ? $ARGV[1] : 0);

##
## Get the base submission file
## Each test will append different parameters
##
$baseCmd = "job_core_time-deferral.cmd";
open(FILE, $baseCmd) || die("Failed to open base command file '$baseCmd'");
@lines = <FILE>;
close(FILE);

##
## If we encounter an abort, and this flag isn't set, then
## we know it's a mistake
##
my $ABORTING = 0;

## -----------------------------------------------------
## Test Parameters
## -----------------------------------------------------
@exact   = ( ); # Exact Execution Timestamp (Set to true to use)
@deltas  = ( ); # Offset CurrentTime
@windows = ( ); # Deferral Window
@preps   = ( ); # Number of Seconds to prep job
@fail    = ( ); # If set to true, then this test is meant to fail

##
## Test #1
## Have the job set to be deferred to run on our time
## This assumes that our clocks are in synch
## This will be a better test when offsets are in place
##
push(@exact,   1);
push(@deltas,  60);
push(@windows, 0);
push(@preps,   20);
push(@fail,    0);

##
## Test #2
## Set the deferral time to be in the past and make sure
## that the job fails. We don't want jobs to run when 
## they shouldn't
##
#push(@exact,   0);
#push(@deltas,  -600); # 600 sec = 10 min
#push(@windows, 0);
#push(@fail,    1);

##
## Test #3
## Set the deferral time to be in the past, but we will
## also set a window time. This will make sure that
## jobs that missed their run time but are within the
## the window can still run.
##
push(@exact,   1);
push(@deltas,  -120);  # 120 sec = 2 min
push(@windows, 180); # 180 sec = 3 min
push(@preps,   20);
push(@fail,    0);

##
## Test #4
## Have the job set to be deferred based on their time
## For some reason, this tests ALWAYS fails in the nightly builds
## for vanilla universe, even though it works when it run it locally
## So until I can figure out what's wrong, I am going to disable it
## I don't it is that big of deal because most people will never
## construct deferral times this way.
##
## Update: 05/08/2007
## This test is too flakey on all universes (sometimes it fails,
## sometimes it works). So rather than piss people off, I'm just
## going to disable it for now
##
#unless ($universe eq "vanilla") {
#	push(@exact,   0);
#	push(@deltas,  90);
#	push(@windows, 0);
#	push(@preps,   30);
#	push(@fail,    0);
#} # UNIVERSE

## -----------------------------------------------------
## PREPARE & SUBMIT TESTS
## If we fail on one of them, keep going so they can
## see all the failures at once
## -----------------------------------------------------
my $success = true;
for ( $ctr = 0, $cnt = scalar(@deltas); $ctr < $cnt; $ctr++ ) {
	my $exact   = $exact[$ctr];
	my $delta   = $deltas[$ctr];
	my $window  = $windows[$ctr];
	my $prep	= $preps[$ctr];
	my $fail    = $fail[$ctr];
	my $test    = $testname."Test \#".($ctr + 1);
	my $cmdFile = $baseCmd.$ctr;
	my $logFile = $cmdFile.".log";
	my $outFile = $cmdFile.".out";
	my $errFile = $cmdFile.".err";
	
	##
	## This variable is used by our callback methods 
	## to know whether the job did what it was suppose to
	##
	my $testFailure = 0;
	
	##
	## The expected time is the time we think that this job
	## will be executed
	##
	my $expectedTime = time() + $delta;
	
	#print "TEST \#$ctr\n";
	#print "\texact:    $exact\n";
	#print "\tdelta:    $delta\n";
	#print "\twindow:   $window\n";
	#print "\texpected: $expectedTime\n";
	#print "\tfail:     ".($fail ? "TRUE" : "FALSE")."\n";
	
	##
	## Clean up the files before we run
	##
	unlink($cmdFile);
	unlink($logFile);
	unlink($outFile);
	unlink($errFile);
	
	##
	## Add the test parameters to a new submit file
	##
	open(FILE, "> $cmdFile") || die("Failed to open command file '$cmdFile' for writing");
	print FILE "@lines\n";
	print FILE "Universe = $universe\n";
	
	##
	## We can either be given an exact time to run which will
	## be based on the submitting machines clock
	##
	if ($exact) {
		##
		## We are assuming that there is some time offset
		## calculation in place to make sure that this is executed at
		## our time and not the executing machine's clock
		## Otherwise the clocks must be in synch
		##
		print FILE "DeferralTime = $expectedTime\n";	
	##
	## Or a delta that will tell the executing machine to run at 
	## a time based on their own clock
	##
	} else {
		## 
		##
		## By using CurrenTime we are assuming that our clock is the
		## same as the machine it is running on
		##
		print FILE "DeferralTime = (CurrentTime + $delta)\n";	
	}
	
	##
	## We will only put in the deferral window and prep time if 
	## the valeus are not empty
	##
	if ($window > 0) {
		print FILE "DeferralWindow = $window\n";
	}
	if ($prep > 0) {
		print FILE "DeferralPrep = $prep\n";
	}
	
	##
	## Add all our files
	## These are deleted before we execute the test each time
	##
	print FILE "Log	= $logFile\n";
	print FILE "Output = $outFile\n";
	print FILE "Error = $errFile\n";
	print FILE "Queue\n";
	close(FILE);
	
	##
	## success
	## Dynamically create our callback function
	##
	my $success = sub {
		my %info = @_;
		$cluster = $info{"cluster"};
		$job = $info{"job"};
		##
		## This is the time we get from our log that says they
		## begun execution
		##
		my $executeTime = extractExecuteTime($info{'log'});
		##
		## This is the time that they said they ran on the execute machine
		##
		open(FILE, $outFile) || die("Failed to open output file '$outFile'");
		my @output = <FILE>;
		close(FILE);
		my $reportTime  = "@output";
		chomp($reportTime);
		
		print "\n-----------------------------------------\n";
		print "\texecute:  $executeTime\n";
		print "\treport:   $reportTime\n";
		print "\texpected: $expectedTime\n\n";
		
		##
		## If this job wasn't suppose to fail, make sure we ran
		## successfully when it was suppose to
		##
		if (!$fail) {
			##
			## No timestamp is bad mojo!
			##
			if (!$executeTime) {
				$testFailure = "Bad - Unable to extract execution timestamp from ".
							   "log file for Job $cluster.$job! ".
							   "Cowardly failing!";
				return (0);
			}
			
			##
			## Make sure that the time is greater than or equal to what
			## the expected time is.
			##
			## At this point we are assuming that the clocks are in synch or
			## that the offset insured that it ran based on our time not 
			## its own
			##
			if ($executeTime < $expectedTime) {
				$testFailure = "Bad - Job $cluster.$job executed ".
							   ($expectedTime - $executeTime).
							   " seconds before it was suppose to!\n";
				return (0);
			}
			
			##
			## If it is suppose to be an exact time, make sure that it ran
			## exactly when we wanted it to. I am assuming that the clocks 
			## are in synch or that the offset insured that it ran based
			## on our time not its own.
			##
			## Just for sanity, I am allowing the job to be 1 second off
			## We only do this test if the window time is wasn't in the past (Test #4)
			##
			if ( $exact && 
				($delta >= 0 ) &&
				($executeTime != $expectedTime) &&
				($executeTime != ($expectedTime + 1)) ) {
				$testFailure = "Bad - Job $cluster.$job execution time differs by ".
								abs($expectedTime - $executeTime)." seconds ".
								"from when it was suppose to run exactly at.\n";
				return (0);
			}
			
			print "Good - Job $cluster.$job executed successfully!\n";
			
		##
		## The job was suppose to fail and never run
		## Make sure that it didn't!
		##
		} else {
			##
			## Bums! It ran!
			##
			if ($reportTime) {
				$testFailure = "Bad - Job $cluster.$job was not suppose to ".
							   "execute but it did!\n";
				return (0);
			}
		}
		return (1);
	};
	
	##
	## Failure callback
	## When the deferral fails, the job will be put on hold
	##
	my $held = sub {
		%info = @_;
		$cluster = $info{"cluster"};
		$job = $info{"job"};
	
		##
		## If this test wasn't suppose to fail
		##
		if ( !$fail ) {
			$testFailure = "Bad - Job $cluster.$job failed but wasn't suppose to!\n";
			return (0);
		}
		
		print "Good - Job $cluster.$job failed to run when it was suppose to!\n";
		
		##
		## Remove the job
		## We set the aborting flag so that we know the abort message 
		## isn't a mistake
		##
		$ABORTING = 1;
		my @adarray;
		my $status = 1;
		my $cmd = "condor_rm $cluster";
		$status = CondorTest::runCondorTool($cmd,\@adarray,2);
		if ( !$status ) {
			print "ERROR: Test failure due to Condor Tool Failure<$cmd>\n";
			return(0);
		}
		return (1);
	};
	
	##
	## aborted
	## The job is being aborted, so we need to make sure that
	## we are the one doing the abort
	##
	my $aborted = sub {
		%info = @_;
		$cluster = $info{"cluster"};
		$job = $info{"job"};
	
		##
		## Make sure this was meant to happen
		## 
		if ( $ABORTING ) {
			print "Good - Job $cluster.$job is being removed after being held.\n";
		##
		## Bad mojo!
		##
		} else {
			print "Bad - Job $cluster.$job received an unexpected abort event.\n";
			exit(1);
		}
	};
	
	##
	## submitted
	## We need to get the info for the job when it is submitted
	##
	my $submitted = sub {
		%info = @_;
		$cluster = $info{"cluster"};
		$job = $info{"job"};
	
		print "Good - Job $cluster.$job was submitted!\n";
		
		##
		## To help improve the chances of our job running, we're going 
		## to call a 'condor_reschedule'
		##
		my @adarray;
		my $status = 1;
		my $cmd = "condor_reschedule";
		$status = CondorTest::runCondorTool($cmd, \@adarray, 2);
		if ( !$status ) {
			print "Test failure due to Condor Tool Failure <$cmd>\n";
			exit(1);
		}
		my $cmd = "condor_q -anal";
		$status = CondorTest::runCondorTool($cmd, \@adarray, 2);
		if ( !$status ) {
			print "Test failure due to Condor Tool Failure <$cmd>\n";
			exit(1);
		}
		print "Output from condor_q:\n".join("\n", @adarray)."\n";
	};
		
	##
	## timeout
	## If the job never matches and run, we'll execute this method
	##
	my $timeout = sub {
		$cluster = $info{"cluster"};
		$job = $info{"job"};
		
		print "Bad - Job $cluster.$job never began execution! Removing...\n";
		
		##
		## Remove the job from the queue
		##
		my @adarray;
		my $status = 1;
		my $cmd = "condor_rm $cluster";
		$status = CondorTest::runCondorTool($cmd, \@adarray, 2);
		if ( !$status ) {
			print "Test failure due to Condor Tool Failure <$cmd>\n";
			exit(1);
		}
		return (0);
	};
	
	##
	## Run ye olde' test
	##
	CondorTest::RegisterSubmit( $test, $submitted );
	CondorTest::RegisterAbort( $test, $aborted );
	CondorTest::RegisterHold( $test, $held );
	CondorTest::RegisterExitedSuccess( $test, $success );
	
	##
	## This callback is to make sure our job actually runs
	## Sometimes the job fails to match and execute.
	##
	CondorTest::RegisterTimed( $test, $timeout, abs($delta * 2) );
	
	##
	## Kind of a hack
	## Our callback methods will set global variables that 
	## we can check to see if the job executed/aborted the
	## way we expected it to 
	##
	$ABORTING = 0;
	CondorTest::RunTest( $test, $cmdFile, 0);
	if ($testFailure) {
		print "$test: CondorTest::RunTest() failed - $testFailure\n";
		$success = false;
	} else {
		print "$test: SUCCESS\n";
	}
	CondorTest::RemoveTimed( $test );
	
	##
	## Clean up the files after we run if we were told to
	##
	if ($cleanup) { 
		unlink($cmdFile);
		unlink($logFile);
		unlink($outFile);
		unlink($errFile);
	}
} # FOREACH

## 
## Return an exit status based on what happened with all our tests!
##
exit( $success ? 0 : -1 );

## -----------------------------------------------------
## HELPER FUNCTIONS
## -----------------------------------------------------

##
## extractExecuteTime()
## For a given log file, get the last execution time
##
sub extractExecuteTime {
    my ($log) = @_;
    open(FILE, "<$log") || die "Can't open log file '$log'\n";
    while (<FILE>) {
    	##
    	## Pull out the date and time of when the job was actually 
    	## executed by the starter. We always want the last time 
    	## that may be in the logfile
    	##
    	if ( $_ =~ /^001\s+\(.*\)\s+(\d{2})\/(\d{2})\s+(\d{2}):(\d{2}):(\d{2})/ ) {
    		$month  = $1;
    		$day    = $2;
    		$hour   = $3;
    		$minute = $4;
    		$second = $5;
    	}
    } # WHILE
    close(FILE);
    
    ##
    ## Convert what we got back into a timestamp
    ## The current log file does not include the year of when a job
    ## was executed so we will use the current year
	##
	(undef, undef, undef, undef, undef, $year, undef, undef, $isdst) = localtime(time); 
	$timestamp = mktime($second, $minute, $hour, $day, $month - 1, $year, 0, 0, $isdst);
    
    #print "$month/$day/".($year + 1900)." $hour:$minute:$second\n";
    #print "RUN TIME: $timestamp\n";
    return ($timestamp);
};
