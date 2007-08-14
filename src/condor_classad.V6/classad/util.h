/*********************************************************************
 *
 * Condor ClassAd library
 * Copyright (C) 1990-2003, Condor Team, Computer Sciences Department,
 * University of Wisconsin-Madison, WI and Rajesh Raman.
 *
 * This source code is covered by the Condor Public License, which can
 * be found in the accompanying LICENSE file, or online at
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
 *********************************************************************/

#ifndef __UTILS_H__
#define __UTILS_H__

#include "classad/common.h"

BEGIN_NAMESPACE( classad )

// macro for recognising octal digits
#define isodigit(x) (( (x) - '0' < 8 ) && ((x) - '0' >= 0))

// A structure which represents the Absolute Time Literal
struct abstime_t 
{
	time_t secs;   // seconds from the epoch (UTC) 
	int    offset; // seconds east of Greenwich 
};

// Get a random number between 0 and something large--usually an int
int get_random_integer(void);
// Get a random number between 0 and 1
double get_random_real(void);

long timezone_offset(void);

/* This converts a string so that sequences like \t
 * (two-characters, slash and t) are converted into the 
 * correct characters like tab. It also converts octal sequences. 
 */
void convert_escapes(std::string &text, bool &validStr); 

void getLocalTime(time_t *now, struct tm *localtm);
void getGMTime(time_t *now, struct tm *localtm);

void absTimeToString(const abstime_t &atime, std::string &buffer);
void relTimeToString(double rtime, std::string &buffer);

void day_numbers(int year, int month, int day, int &weekday, int &yearday);
int fixed_from_gregorian(int year, int month, int day);
bool is_leap_year(int year);

int classad_isinf(double x);
int classad_isnan(double x);

END_NAMESPACE // classad

#endif//__UTILS_H__