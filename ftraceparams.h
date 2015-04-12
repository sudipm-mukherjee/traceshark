/*
 * Traceshark - a visualizer for visualizing ftrace traces
 * Copyright (C) 2015  Viktor Rosendahl <viktor.rosendahl@gmail.com>
 *
 *   This program is free software: you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation, either version 3 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef FTRACEPARAMS_H
#define FTRACEPARAMS_H

#include "mempool.h"
#include "traceevent.h"
#include <cstring>
#include <cstdint>

#define TASKNAME_MAXLEN (128) /* Should be enough, I wouldn't expect more
				 than about 16 */

extern char *eventstrings[];

typedef enum {
	CPU_FREQUENCY = 0,
	CPU_IDLE,
	SCHED_MIGRATE_TASK,
	SCHED_SWITCH,
	SCHED_WAKEUP,
	SCHED_WAKEUP_NEW,
	NR_EVENTS,
} event_t;

#define ABSURD_UNSIGNED (2147483647)

#define is_this_event(EVENTNAME, EVENT) (strcmp(eventstrings[EVENTNAME], \
						 EVENT.eventName->ptr) == 0)

static __always_inline unsigned int param_after_char(const TraceEvent &event,
					    int n_param, char ch)
{
	char *last;
	char *first;
	char *c;
	bool found = false;
	unsigned int param = 0;
	unsigned int digit;


	last = event.argv[n_param]->ptr + event.argv[n_param]->len - 1;
	first = event.argv[n_param]->ptr;
	for (c = last; c >= first; c--) {
		if (*c == ch) {
			found = true;
			break;
		}
	}
	if (!found)
		return ABSURD_UNSIGNED; /* return absurd if error */
	c++;
	for (; c <= last; c++) {
		digit = *c - '0';
		param *= 10;
		param += digit;
	}
	return param;
}

static __always_inline unsigned int param_inside_braces(TraceEvent &event,
					       unsigned int n_param)
{
	unsigned int len = event.argv[n_param]->len;
	char *first = event.argv[n_param]->ptr;
	char *end = first + len - 1; /* now pointing to the final ']' */
	char *c;
	unsigned int digit, param = 0;

	first++; /* Skipt the leading '[' */

	if (len > 2) {
		for (c = first; c < end; c++) {
			digit = *c - '0';
			param *= 10;
			param += digit;
		}
		return param;
	}

	return ABSURD_UNSIGNED;
}

#define cpufreq_event(EVENT) (is_this_event(CPU_FREQUENCY, EVENT) && \
			      EVENT.argc >= 2)
#define cpufreq_cpu(EVENT) (param_after_char(EVENT, 1, '='))
#define cpufreq_freq(EVENT) (param_after_char(EVENT, 0, '='))

#define cpuidle_event(EVENT) (is_this_event(CPU_IDLE, EVENT) && EVENT.argc >= 2)
#define cpuidle_cpu(EVENT) (param_after_char(EVENT, 1, '='))
static __always_inline int cpuidle_state(const TraceEvent &event)
{
	int32_t state;
	uint32_t ustate;
	ustate = param_after_char(event, 0, '=');
	state = *((int*) &ustate); /* the string is a signed printed as 
				    * unsigned :) */
	return state;
}

#define sched_migrate(EVENT) \
	(is_this_event(SCHED_MIGRATE_TASK, EVENT) && EVENT.argc >= 5)
#define sched_migrate_destCPU(EVENT) (param_after_char(EVENT, EVENT.argc - 1, \
						       '='))
#define sched_migrate_origCPU(EVENT) (param_after_char(EVENT, EVENT.argc - 2, \
						       '='))
#define sched_migrate_prio(EVENT) (param_after_char(EVENT, EVENT.argc - 3, \
						    '='))
#define sched_migrate_pid(EVENT) (param_after_char(EVENT, EVENT.argc - 4, \
						   '='))

#define sched_switch(EVENT) \
	(is_this_event(SCHED_SWITCH, EVENT) && EVENT.argc >= 6)
#define sched_switch_newprio(EVENT) (param_inside_braces(EVENT, EVENT.argc - 1))
#define sched_switch_newpid(EVENT) \
	(param_after_char(EVENT, EVENT.argc - 2, ':'))

static __always_inline char sched_switch_state(TraceEvent &event)
{
	unsigned int i;
	for (i = 3; i < event.argc; i++) {
		if (strcmp(event.argv[i]->ptr, "==>") == 0)
			break;
	}
	if (i < event.argc)
		return event.argv[i - 1]->ptr[0];
	return '\0';
}

static __always_inline unsigned int sched_switch_oldprio(TraceEvent &event)
{
	unsigned int i;
	for (i = 3; i < event.argc; i++) {
		if (strcmp(event.argv[i]->ptr, "==>") == 0)
			break;
	}
	if (i < event.argc)
		return param_inside_braces(event, i - 2);
	return ABSURD_UNSIGNED;
}

static __always_inline unsigned int sched_switch_oldpid(TraceEvent &event)
{
	unsigned int i;
	for (i = 3; i < event.argc; i++) {
		if (strcmp(event.argv[i]->ptr, "==>") == 0)
			break;
	}
	if (i < event.argc)
		return param_after_char(event, i - 3, ':');
	return ABSURD_UNSIGNED;
}

static __always_inline char * __sched_switch_oldname_strdup(TraceEvent &event,
	MemPool *pool)
{
	unsigned int i;
	unsigned int endidx;
	unsigned int len = 0;
	char *retstr;
	char *c;
	char *d;
	char *end;

	/* Find the index of the '==>' */
	for (i = 3; i < event.argc; i++) {
		if (strcmp(event.argv[i]->ptr, "==>") == 0)
			break;
	}
	if (!(i < event.argc))
		return NULL;
	endidx = i - 3;

	/* + 1 needed for null termination */
	retstr = (char*) pool->preallocChars(TASKNAME_MAXLEN + 1);
	c = retstr;

	/* This loop will merge any strings before the final string, in case
	 * such strings exists due to the task name containing spaces, and
	 * then the taskname would be split into several strings
	 */
	for (i = 0; i < endidx; i++) {
		len += event.argv[i]->len;
		if (len > TASKNAME_MAXLEN)
			return NULL;
		strncpy(c, event.argv[i]->ptr, event.argv[i]->len);
		c += event.argv[i]->len;
		*c = ' ';
		len++;
		c++;
	}

	/*
	 * Localize the separing '-' in the final string. The final
	 * string is the only sting in case of no spaces in the task name.
	 * we are searching backwards because we are interested in the last '-',
	 * since the task name can contain '-' characters
	 */
	for (end = event.argv[endidx]->ptr + event.argv[endidx]->len - 1;
	     end > event.argv[endidx]->ptr; end--) {
		if (*end == '-')
			break;
	}

	/* Copy the final portion up to the '-' we found previously */
	for (d = event.argv[endidx]->ptr; d < end; d++) {
		len++;
		if (len > TASKNAME_MAXLEN)
			return NULL;
		*c = *d;
		c++;
	}
	/* Terminate the string */
	*c = '\0';
	len++;
	/* commmit the allocation */
	if (pool->commitChars(len))
		return retstr;
	return NULL;
}

char *sched_switch_oldname_strdup(TraceEvent &event, MemPool *pool);

/* TODO: Check what code could be shared between this and the above function */
static __always_inline char * __sched_switch_newname_strdup(TraceEvent &event,
	MemPool *pool)
{
	unsigned int i;
	unsigned int startidx, endidx;
	unsigned int len = 0;
	char *retstr;
	char *c;
	char *d;
	char *end;

	endidx = event.argc - 2;

	/* Find the index of the '==>' */
	for (i = 3; i < event.argc; i++) {
		if (strcmp(event.argv[i]->ptr, "==>") == 0)
			break;
	}
	if (!(i < event.argc))
		return NULL;
	startidx = i + 1;

	/* + 1 needed for null termination */
	retstr = (char*) pool->preallocChars(TASKNAME_MAXLEN + 1);
	c = retstr;

	/* This loop will merge any strings before the final string, in case
	 * such strings exists due to the task name containing spaces, and
	 * then the taskname would be split into several strings
	 */
	for (i = startidx; i < endidx; i++) {
		len += event.argv[i]->len;
		if (len > TASKNAME_MAXLEN)
			return NULL;
		strncpy(c, event.argv[i]->ptr, event.argv[i]->len);
		c += event.argv[i]->len;
		*c = ' ';
		len++;
		c++;
	}

	/*
	 * Localize the separing '-' in the final string. The final
	 * string is the only sting in case of no spaces in the task name.
	 * we are searching backwards because we are interested in the last '-',
	 * since the task name can contain '-' characters
	 */
	for (end = event.argv[endidx]->ptr + event.argv[endidx]->len - 1;
	     end > event.argv[endidx]->ptr; end--) {
		if (*end == '-')
			break;
	}

	/* Copy the final portion up to the '-' we found previously */
	for (d = event.argv[endidx]->ptr; d < end; d++) {
		len++;
		if (len > TASKNAME_MAXLEN)
			return NULL;
		*c = *d;
		c++;
	}
	/* Terminate the string */
	*c = '\0';
	len++;
	/* commmit the allocation */
	if (pool->commitChars(len))
		return retstr;
	return NULL;
}

char *sched_switch_newname_strdup(TraceEvent &event, MemPool *pool);

#define sched_wakeup(EVENT) \
	(is_this_event(SCHED_WAKEUP, EVENT) && EVENT.argc >= 4)
#define sched_wakeup_cpu(EVENT) (param_after_char(EVENT, EVENT.argc - 1, \
						  ':'))

static __always_inline bool sched_wakeup_success(TraceEvent &event)
{
	const TString *ss = event.argv[event.argc - 2];
	char *last = ss->ptr + ss->len - 1; /* Empty string should not be 
					     produced by parser */
	return *last == '1';
}

#define sched_wakeup_prio(EVENT) (param_inside_braces(EVENT, EVENT.argc - 3))
#define sched_wakeup_pid(EVENT) (param_after_char(EVENT, EVENT.argc - 4, \
						  ':'))
#endif
