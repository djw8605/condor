 ###############################################################
 # 
 # Copyright 2011 Red Hat, Inc. 
 # 
 # Licensed under the Apache License, Version 2.0 (the "License"); you 
 # may not use this file except in compliance with the License.  You may 
 # obtain a copy of the License at 
 # 
 #    http://www.apache.org/licenses/LICENSE-2.0 
 # 
 # Unless required by applicable law or agreed to in writing, software 
 # distributed under the License is distributed on an "AS IS" BASIS, 
 # WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 # See the License for the specific language governing permissions and 
 # limitations under the License. 
 # 
 ############################################################### 

if(WINDOWS)

	file (GLOB RmdirFiles condor_rmdir.* main.* common.h bprint.* harylist.*)
	
	# these commands to build with libctiny instead of the standard c-runtime.
    #condor_glob ( libcHdrs libcSrcs "${RmdirFiles}" )
	#condor_static_lib ( libctiny "${libcHdrs};${libcSrcs}" )
	#condor_exe( condor_rmdir "${RmdirFiles}" ${C_BIN} "libctiny" OFF )
	
	condor_exe( condor_rmdir "${RmdirFiles}" ${C_BIN} "" OFF )


endif(WINDOWS)
