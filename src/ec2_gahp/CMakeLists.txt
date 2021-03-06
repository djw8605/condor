 ###############################################################
 # 
 # Copyright (C) 1990-2010, Redhat. 
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

# Openssl on rhel3/4 don't have EVP_sha256(), which the ec2_gahp uses.
if( HAVE_EXT_OPENSSL AND HAVE_EXT_CURL AND HAVE_EXT_PCRE AND HAVE_PTHREADS AND
NOT ${SYSTEM_NAME} MATCHES "rhel3"  AND NOT ${SYSTEM_NAME} MATCHES "rhel4" )

	include_directories(${CMAKE_CURRENT_SOURCE_DIR})
	include_directories(${CMAKE_CURRENT_BINARY_DIR})

    condor_glob( HeaderFiles SourceFiles "queryAPI-sim.cpp" )

    # Build the GAHP; curl requires libz on some systems.
	condor_exe( ec2_gahp
    	"${HeaderFiles};${SourceFiles}"	${C_SBIN}
	    "${CONDOR_TOOL_LIBS};${PCRE_FOUND};${OPENSSL_FOUND};${KRB5_FOUND};${CURL_FOUND};${ZLIB_FOUND}"
    	OFF )

    # Build an executable needed by the tests.
	condor_exe_test( queryAPI-sim
        "queryAPI-sim.cpp"
        "" )

    # Declare the test.
    #
    # FIXME: the cmake code is broken and dumps the list_* files into
    # this directory instead of the condor_tests directory.  It's also
    # broken and dumping them in the *source* directories, rather than
    # the *object* directories, but that's a whole different failure.
    #
    # condor_pl_test( job_ec2_basic "EC2 Query API test" "quick;full;quicknolink" )

endif()
