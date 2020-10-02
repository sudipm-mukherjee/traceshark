// SPDX-License-Identifier: (GPL-2.0-or-later OR BSD-2-Clause)
/*
 * Traceshark - a visualizer for visualizing ftrace and perf traces
 * Copyright (C) 2014-2020  Viktor Rosendahl <viktor.rosendahl@gmail.com>
 *
 * This file is dual licensed: you can use it either under the terms of
 * the GPL, or the BSD license, at your option.
 *
 *  a) This program is free software; you can redistribute it and/or
 *     modify it under the terms of the GNU General Public License as
 *     published by the Free Software Foundation; either version 2 of the
 *     License, or (at your option) any later version.
 *
 *     This program is distributed in the hope that it will be useful,
 *     but WITHOUT ANY WARRANTY; without even the implied warranty of
 *     MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *     GNU General Public License for more details.
 *
 *     You should have received a copy of the GNU General Public
 *     License along with this library; if not, write to the Free
 *     Software Foundation, Inc., 51 Franklin St, Fifth Floor, Boston,
 *     MA 02110-1301 USA
 *
 * Alternatively,
 *
 *  b) Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *     1. Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *     2. Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 *     THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND
 *     CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES,
 *     INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 *     MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 *     DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
 *     CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *     SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 *     NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 *     LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 *     HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 *     CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 *     OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
 *     EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <climits>
#include <cstdlib>
#include <cstring>

extern "C" {
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
}

#include <cerrno>

#include <QtGlobal>
#include <QList>
#include <QString>

#include "vtl/compiler.h"
#include "vtl/error.h"
#include "vtl/tlist.h"

#include "analyzer/cpufreq.h"
#include "analyzer/cpuidle.h"
#include "parser/genericparams.h"
#include "analyzer/traceanalyzer.h"
#include "parser/tracefile.h"
#include "parser/traceparser.h"
#include "misc/errors.h"
#include "misc/setting.h"
#include "misc/settingstore.h"
#include "misc/traceshark.h"
#include "threads/workthread.h"
#include "threads/workitem.h"
#include "threads/workqueue.h"

vtl_always_inline static int clib_open(const char *pathname, int flags,
				       mode_t mode)
{
	return open(pathname, flags, mode);
}

vtl_always_inline static int clib_close(int fd)
{
	return close(fd);
}

TraceAnalyzer::TraceAnalyzer(const SettingStore *sstore)
	: events(nullptr), cpuTaskMaps(nullptr), cpuFreq(nullptr),
	  cpuIdle(nullptr), black(0, 0, 0), white(255, 255, 255),
	  migrationOffset(0), migrationScale(0), maxCPU(0), nrCPUs(0),
	  endTime(false, 0, 0, 6), startTime(false, 0, 0, 6), endTimeDbl(0),
	  startTimeDbl(0), endTimeIdx(0), maxFreq(0), minFreq(0),
	  maxIdleState(0), minIdleState(0), timePrecision(0), CPUs(nullptr),
	  customPlot(nullptr), pidFilterInclusive(false),
	  OR_pidFilterInclusive(false), setstor(sstore)
{
	taskNamePool = new StringPool<>(16384, 256);
	parser = new TraceParser();
	filterState.disableAll();
	OR_filterState.disableAll();
}

TraceAnalyzer::~TraceAnalyzer()
{
	int dummy;

	TraceAnalyzer::close(&dummy);
	delete parser;
	delete taskNamePool;
}

int TraceAnalyzer::open(const QString &fileName)
{
	int retval = parser->open(fileName);
	if (retval == 0)
		prepareDataStructures();
	return retval;
}

void TraceAnalyzer::prepareDataStructures()
{
	cpuTaskMaps = new vtl::AVLTree<int, CPUTask,
				       vtl::AVLBALANCE_USEPOINTERS>
		[NR_CPUS_ALLOWED];
	cpuFreq = new CpuFreq[NR_CPUS_ALLOWED];
	cpuIdle = new CpuIdle[NR_CPUS_ALLOWED];
	CPUs = new CPU[NR_CPUS_ALLOWED];
	schedOffset.resize(0);
	schedOffset.resize(NR_CPUS_ALLOWED);
	schedScale.resize(0);
	schedScale.resize(NR_CPUS_ALLOWED);
	cpuIdleOffset.resize(0);
	cpuIdleOffset.resize(NR_CPUS_ALLOWED);
	cpuIdleScale.resize(0);
	cpuIdleScale.resize(NR_CPUS_ALLOWED);
	cpuFreqOffset.resize(0);
	cpuFreqOffset.resize(NR_CPUS_ALLOWED);
	cpuFreqScale.resize(0);
	cpuFreqScale.resize(NR_CPUS_ALLOWED);
}

bool TraceAnalyzer::isOpen() const
{
	return parser->isOpen();
}

void TraceAnalyzer::close(int *ts_errno)
{
	if (cpuTaskMaps != nullptr) {
		delete[] cpuTaskMaps;
		cpuTaskMaps = nullptr;
	}
	if (cpuFreq != nullptr) {
		delete[] cpuFreq;
		cpuFreq = nullptr;
	}
	if (cpuIdle != nullptr) {
		delete[] cpuIdle;
		cpuIdle = nullptr;
	}
	if (CPUs != nullptr) {
		delete[] CPUs;
		CPUs = nullptr;
	}

	DEFINE_TASKMAP_ITERATOR(iter) = taskMap.begin();
	while (iter != taskMap.end()) {
		Task *task = iter.value().task;
		delete task;
		iter++;
	}

	taskMap.clear();
	disableAllFilters();
	migrations.clear();
	colorMap.clear();
	parser->close(ts_errno);
	taskNamePool->clear();
}

void TraceAnalyzer::resetProperties()
{
	maxCPU = 0;
	startTime = VTL_TIME_ZERO;
	startTimeDbl = 0;
	endTime = VTL_TIME_ZERO;
	endTimeDbl = 0;
	endTimeIdx = 0;
	minFreq = UINT_MAX;
	maxFreq = 0;
	minIdleState = INT_MAX;
	maxIdleState = INT_MIN;
	timePrecision = 0;
	events = nullptr;
}

void TraceAnalyzer::processTrace()
{
	resetProperties();
	/*
	 * We do the processing from the main thread, since otherwise
	 * we would have to wait for it
	 */
	threadProcess();
	colorizeTasks();
}

void TraceAnalyzer::threadProcess()
{
	parser->waitForTraceType();
	events = parser->getEventsTList();
	switch (getTraceType()) {
	case TRACE_TYPE_FTRACE:
		processFtrace();
		break;
	case TRACE_TYPE_PERF:
		processPerf();
		break;
	default:
		return;
	}
	processSchedAddTail();
	processFreqAddTail();
}

void TraceAnalyzer::processSchedAddTail()
{
	/* Add the "tail" to all tasks, i.e. extend them until endTime */
	unsigned int cpu;
	for (cpu = 0; cpu < getNrCPUs(); cpu++) {
		DEFINE_CPUTASKMAP_ITERATOR(iter) = cpuTaskMaps[cpu].begin();
		while (iter != cpuTaskMaps[cpu].end()) {
			CPUTask &task = iter.value();
			unsigned int d;
			double lastTime;
			int lastIndex = task.schedTimev.size() - 1;
			iter++;
			/*
			 * I think that this should not happen, there should
			 * always be some data in a CPUTask at this point but
			 * check anyway
			 */
			if (lastIndex < 0)
				continue;
			/* Check if tail is necessary */
			lastTime = task.schedTimev[lastIndex];
			if (lastTime >= endTimeDbl)
				continue;
			d = task.schedData.read(task.schedData.size() - 1);
			task.schedTimev.append(endTimeDbl);
			task.schedData.append(d);
			task.schedEventIdx.append(endTimeIdx);
		}
	}

	DEFINE_TASKMAP_ITERATOR(iter) = taskMap.begin();
	while (iter != taskMap.end()) {
		Task &task = *iter.value().task;
		unsigned int d;
		double lastTime;
		int s = task.schedTimev.size();
		iter++;
		task.generateDisplayName();
		if (s <= 0)
			continue;
		lastTime = task.schedTimev[s - 1];
		if (lastTime >= endTimeDbl
		    || task.exitStatus == STATUS_FINAL)
			continue;
		d = task.schedData.read(task.schedData.size() - 1);
		task.schedTimev.append(endTimeDbl);
		task.schedData.append(d);
		task.schedEventIdx.append(endTimeIdx);
	}
}

void TraceAnalyzer::processFreqAddTail()
{
	unsigned int cpu;
	double end = getEndTime().toDouble();

	for (cpu = 0; cpu <= maxCPU; cpu++) {
		if (!cpuFreq[cpu].data.isEmpty()) {
			double freq = cpuFreq[cpu].data.last();
			cpuFreq[cpu].data.append(freq);
			cpuFreq[cpu].timev.append(end);
		}
	}
}

unsigned int TraceAnalyzer::guessTimePrecision()
{
	int s = events->size();
	int r, p;

	r = 0;
	if (s < 1)
		return r;

	p = events->at(0).time.getPrecision();
	if (p > r)
		r = p;

	p = events->at(s / 2).time.getPrecision();
	if (p > r)
		r = p;

	p = events->at(s - 1).time.getPrecision();
	if (p > r)
		r = p;

	return r;
}

/*
 * This function is supposed to be called seldom, thus it's ok to not have it
 * as optimized as the other functions, e.g. in terms of inlining
 */
void TraceAnalyzer::handleWrongTaskOnCPU(const TraceEvent &/*event*/,
					 unsigned int cpu,
					 CPU *eventCPU, int oldpid,
					 const vtl::Time &oldtime,
					 int idx)
{
	int epid = eventCPU->pidOnCPU;
	vtl::Time prevtime, faketime;
	double fakeDbl;
	CPUTask *cpuTask;
	Task *task;

	if (epid > 0) {
		cpuTask = &cpuTaskMaps[cpu][epid];
		Q_ASSERT(!cpuTask->isNew);
		Q_ASSERT(!cpuTask->schedTimev.isEmpty());
		prevtime = eventCPU->lastSched;
		faketime = prevtime + FAKE_DELTA;
		fakeDbl = faketime.toDouble();
		cpuTask->schedTimev.append(fakeDbl);
		cpuTask->schedData.append(FLOOR_BIT);
		cpuTask->schedEventIdx.append(eventCPU->lastSchedIdx);

		task = findTask(epid);
		Q_ASSERT(task != nullptr);
		task->lastSleepEntry = faketime;
		task->schedTimev.append(fakeDbl);
		task->schedData.append(FLOOR_BIT);
		task->schedEventIdx.append(eventCPU->lastSchedIdx);
	}

	if (oldpid > 0) {
		cpuTask = &cpuTaskMaps[cpu][oldpid];
		if (cpuTask->isNew) {
			cpuTask->pid = oldpid;
		}
		cpuTask->isNew = false;
		faketime = oldtime - FAKE_DELTA;
		fakeDbl = faketime.toDouble();
		cpuTask->schedTimev.append(fakeDbl);
		cpuTask->schedData.append(SCHED_BIT);
		cpuTask->schedEventIdx.append(idx);

		task = &taskMap[oldpid].getTask();
		if (task->isNew) {
			task->pid = oldpid;
		}
		task->isNew = false;
		task->schedTimev.append(fakeDbl);
		task->schedData.append(SCHED_BIT);
		task->schedEventIdx.append(idx);
	}
}

void TraceAnalyzer::colorizeTasks()
{
	unsigned int cpu;
	double nf;
	int n;
	int ncolor;
	double s;
	int step;
	int red;
	int green;
	int blue;
	int i, j;
	QList<TColor> colorList;
	const TColor black(0, 0, 0);
	const TColor white(255, 255, 255);
	TColor gray;
	TColor tmp;
	long int rnd = 0;

	srand48(290876);

	for (cpu = 0; cpu <= getMaxCPU(); cpu++) {
		DEFINE_CPUTASKMAP_ITERATOR(iter) = cpuTaskMaps[cpu].begin();
		while (iter != cpuTaskMaps[cpu].end()) {
			CPUTask &task = iter.value();
			iter++;
			if (colorMap.contains(task.pid))
				continue;
			TColor color;
			colorMap.insert(task.pid, color);
		}
	}

	n = colorMap.size();
	nf = (double) n;
	s = 0.95 * cbrt( (1 / nf) * (255 * 255 * 255 ));
	s = TSMIN(s, 128.0);
	s = TSMAX(s, 1.0);
retry:
	step = (int) s;
	for (red = 0; red < 256; red += step) {
		for (green = 0; green < 256; green += step)  {
			for (blue = 0; blue < 256; blue += step) {
				TColor color(red, green, blue);
				if (color.SqDistance(black) < 10000)
					continue;
				if (color.SqDistance(white) < 12000)
					continue;
				gray = TColor(red, red, red);
				if (color.SqDistance(gray) < 2500)
					continue;
				colorList.append(color);
			}
		}
	}

	ncolor = colorList.size();
	if (ncolor < n) {
		s = s * 0.95;
		if (s >= 1) {
			colorList.clear();
			vtl::warnx("Retrying colors in %s:%d\n", __FILE__,
				   __LINE__);
			goto retry;
		}
	}

	/*
	 * Randomize the order by swapping every element with a random
	 * element
	 */
	for (i = 0; i < ncolor; i++) {
		rnd = lrand48();
		j = rnd % ncolor;
		tmp = colorList[j];
		colorList[j] = colorList[i];
		colorList[i] = tmp;
	}

	i = 0;

	DEFINE_COLORMAP_ITERATOR(iter) = colorMap.begin();
	while (iter != colorMap.end()) {
		TColor &color = iter.value();
		iter++;
		color = colorList.at(i % ncolor);
		i++;
	}
}


int TraceAnalyzer::binarySearch(const vtl::Time &time, int start, int end)
	const
{
	int pivot = (end + start) / 2;
	if (pivot == start)
		return pivot;
	if (time < events->at(pivot).time)
		return binarySearch(time, start, pivot);
	else
		return binarySearch(time, pivot, end);
}

int TraceAnalyzer::binarySearchFiltered(const vtl::Time &time, int start,
					int end) const
{
	int pivot = (end + start) / 2;
	if (pivot == start)
		return pivot;
	if (time < filteredEvents.at(pivot)->time)
		return binarySearchFiltered(time, start, pivot);
	else
		return binarySearchFiltered(time, pivot, end);
}

int TraceAnalyzer::findIndexBefore(const vtl::Time &time) const
{
	if (events->size() < 1)
		return -1;

	int end = events->size() - 1;

	/* Basic sanity checks */
	if (time > events->at(end).time)
		return end;
	if (time < events->at(0).time)
		return 0;

	int c = binarySearch(time, 0, end);

	while (c > 0 && events->at(c).time >= time)
		c--;
	return c;
}

int TraceAnalyzer::findIndexAfter(const vtl::Time &time) const
{
	if (events->size() < 1)
		return -1;

	int end = events->size() - 1;

	/* Basic sanity checks */
	if (time > events->at(end).time)
		return end;
	if (time < events->at(0).time)
		return 0;

	int c = binarySearch(time, 0, end);

	while (c < end && events->at(c).time <= time)
		c++;
	return c;
}

int TraceAnalyzer::findFilteredIndexBefore(const vtl::Time &time) const
{
	if (filteredEvents.size() < 1)
		return -1;

	int end = filteredEvents.size() - 1;

	/* Basic sanity checks */
	if (time > filteredEvents.at(end)->time)
		return end;
	if (time < filteredEvents.at(0)->time)
		return 0;

	int c = binarySearchFiltered(time, 0, end);

	while (c > 0 && filteredEvents.at(c)->time >= time)
		c--;
	return c;
}

const TraceEvent *TraceAnalyzer::findPreviousSchedEvent(const vtl::Time &time,
							int pid,
							int *index) const
{
	int start = findIndexBefore(time);
	int i;

	if (start < 0)
		return nullptr;

	for (i = start; i >= 0; i--) {
		const TraceEvent &event = events->at(i);
		if (event.type == SCHED_SWITCH  &&
		    generic_sched_switch_newpid(event) == pid) {
			if (index != nullptr)
				*index = i;
			return &event;
		}
	}
	return nullptr;
}

const TraceEvent *TraceAnalyzer::findNextSchedSleepEvent(const vtl::Time &time,
							 int pid,
							 int *index) const
{
	int start = findIndexAfter(time);
	int i;
	int s = events->size();

	if (start < 0)
		return nullptr;

	for (i = start; i < s; i++) {
		const TraceEvent &event = events->at(i);
		if (event.type == SCHED_SWITCH &&
		    generic_sched_switch_oldpid(event) == pid &&
		    !task_state_is_runnable(
			    generic_sched_switch_state(event))) {
			if (index != nullptr)
				*index = i;
			return &event;
		}
	}
	return nullptr;
}

const TraceEvent *TraceAnalyzer::findFilteredEvent(int index,
						   int *filterIndex) const
{
	const TraceEvent *eptr = &events->at(index);
	vtl::Time time = eptr->time;
	int s = filteredEvents.size();
	int i;
	int start = findFilteredIndexBefore(time);

	if (start < 0)
		return nullptr;

	for (i = start; i < s; i++) {
		const TraceEvent *cptr = filteredEvents[i];
		if (cptr == eptr) {
			*filterIndex = i;
			return cptr;
		}
		if (cptr->time > time)
			break;
	}
	return nullptr;
}

const TraceEvent *TraceAnalyzer::findPreviousWakEvent(int startidx,
						      int pid,
						      event_t wanted,
						      int *index) const
{
	int i;
	int epid = 0;

	if (startidx < 0 || startidx >= (int) events->size())
		return nullptr;

	if (wanted != SCHED_WAKEUP && wanted != SCHED_WAKEUP_NEW &&
	    wanted != SCHED_WAKING)
		return nullptr;

	for (i = startidx; i >= 0; i--) {
		const TraceEvent &event = events->at(i);
		if ((event.type == wanted ||
		     (wanted == SCHED_WAKEUP &&
		      event.type == SCHED_WAKEUP_NEW))) {
			if (wanted == SCHED_WAKING)
				epid = generic_sched_waking_pid(event);
			else
				epid = generic_sched_wakeup_pid(event);
			if (epid != pid)
				continue;
			if (index != nullptr)
				*index = i;
			return &event;
		}
	}
	return nullptr;
}

const TraceEvent *TraceAnalyzer::findWakingEvent(const TraceEvent *wakeup,
						 int *index) const
{
	int i;
	int startidx = findIndexBefore(wakeup->time);
	int wpid = generic_sched_wakeup_pid(*wakeup);
	int pid;

	if (wpid == INT_MAX)
		return nullptr;

	if (startidx < 0 || startidx >= (int) events->size())
		return nullptr;

	for (i = startidx; i >= 0; i--) {
		const TraceEvent &event = events->at(i);
		if (event.type != SCHED_WAKING)
			continue;
		pid = generic_sched_waking_pid(event);
		if (pid == wpid) {
			if (index != nullptr)
				*index = i;
			return &event;
		} else if (pid == INT_MAX) {
			/*
			 * If we encounter a single waking event where we
			 * can not parse the arguments, then we give up
			 */
			return nullptr;
		}
	}
	return nullptr;
}

void TraceAnalyzer::setSchedOffset(unsigned int cpu, double offset)
{
	schedOffset[cpu] = offset;
}

void TraceAnalyzer::setSchedScale(unsigned int cpu, double scale)
{
	schedScale[cpu] = scale;
}

void TraceAnalyzer::setCpuIdleOffset(unsigned int cpu, double offset)
{
	cpuIdleOffset[cpu] = offset;
}

void TraceAnalyzer::setCpuIdleScale(unsigned int cpu, double scale)
{
	cpuIdleScale[cpu] = scale / maxIdleState;
}

void TraceAnalyzer::setCpuFreqOffset(unsigned int cpu, double offset)
{
	cpuFreqOffset[cpu] = offset;
}

void TraceAnalyzer::setCpuFreqScale(unsigned int cpu, double scale)
{
	cpuFreqScale[cpu] = scale / maxFreq;
}

void TraceAnalyzer::setMigrationOffset(double offset)
{
	migrationOffset = offset;
}

void TraceAnalyzer::setMigrationScale(double scale)
{
	migrationScale = scale;
}

void TraceAnalyzer::setQCustomPlot(QCustomPlot *plot)
{
	customPlot = plot;
}

void TraceAnalyzer::addCpuFreqWork(unsigned int cpu,
				   QList<AbstractWorkItem*> &list)
{
	double scale = cpuFreqScale.value(cpu);
	double offset = cpuFreqOffset.value(cpu);
	CpuFreq *freq = cpuFreq + cpu;
	freq->scale = scale;
	freq->offset = offset;
	WorkItem<CpuFreq> *freqItem = new WorkItem<CpuFreq>
		(freq, &CpuFreq::doScale);
	list.append(freqItem);
}

void TraceAnalyzer::addCpuIdleWork(unsigned int cpu,
				   QList<AbstractWorkItem*> &list)
{
	double scale = cpuIdleScale.value(cpu);
	double offset = cpuIdleOffset.value(cpu);
	CpuIdle *idle = cpuIdle + cpu;
	idle->scale = scale;
	idle->offset = offset;
	WorkItem<CpuIdle> *idleItem = new WorkItem<CpuIdle>
		(idle, &CpuIdle::doScale);
	list.append(idleItem);
}

void TraceAnalyzer::addCpuSchedWork(unsigned int cpu,
				    QList<AbstractWorkItem*> &list)
{
	double scale = schedScale.value(cpu);
	double offset = schedOffset.value(cpu);
	DEFINE_CPUTASKMAP_ITERATOR(iter) = cpuTaskMaps[cpu].begin();
	while (iter != cpuTaskMaps[cpu].end()) {
		CPUTask &task = iter.value();
		task.scale = scale;
		task.offset = offset;
		WorkItem<CPUTask> *taskItem = new WorkItem<CPUTask>
			(&task, &CPUTask::doScale);
		list.append(taskItem);
		taskItem = new WorkItem<CPUTask>(&task,
						 &CPUTask::doScaleWakeup);
		list.append(taskItem);
		taskItem = new WorkItem<CPUTask>(&task,
						 &CPUTask::doScaleRunning);
		list.append(taskItem);
		taskItem = new WorkItem<CPUTask>(&task,
						 &CPUTask::doScalePreempted);
		list.append(taskItem);
		taskItem = new WorkItem<CPUTask>(&task,
						 &CPUTask::doScaleUnint);
		list.append(taskItem);
		iter++;
	}
}

/*
 * This function must be called from the application mainthread because it
 * creates objects that are children customPlot, which is created by the
 * mainthread
 */
void TraceAnalyzer::scaleMigration()
{
	QList<Migration>::iterator iter;
	double unit = migrationScale / getNrCPUs();
	const int width = setstor->getValue(Setting::MIGRATION_WIDTH).intv();
	for (iter = migrations.begin(); iter != migrations.end(); iter++) {
		Migration &m = *iter;
		double s = migrationOffset + (m.oldcpu + 1) * unit;
		double e = migrationOffset + (m.newcpu + 1) * unit;
		QColor color = getTaskColor(m.pid);
		/*
		 * The constructor will save a pointer to the MigrationArrow
		 * object in the customPlot object.
		 */
		new MigrationArrow(s, e, m.time.toDouble(), color,
				   customPlot, width);
	}
}

bool TraceAnalyzer::enableMigrations()
{
	return (setstor->getValue(Setting::SHOW_MIGRATION_GRAPHS).boolv() &&
		(setstor->getValue(Setting::SHOW_MIGRATION_UNLIMITED).boolv() ||
		 migrations.size() < MAX_NR_MIGRATIONS));
}

void TraceAnalyzer::doScale()
{
	QList<AbstractWorkItem*> workList;
	unsigned int cpu;
	int i;
	int s = 0;
	bool useWorkList =
		setstor->getValue(Setting::SHOW_CPUFREQ_GRAPHS).boolv() ||
		setstor->getValue(Setting::SHOW_CPUIDLE_GRAPHS).boolv() ||
		setstor->getValue(Setting::SHOW_SCHED_GRAPHS).boolv();

	if (useWorkList) {
		for (cpu = 0; cpu <= getMaxCPU(); cpu++) {
			/* CpuFreq items */
			if (setstor->getValue(Setting::SHOW_CPUFREQ_GRAPHS)
			    .boolv())
				addCpuFreqWork(cpu, workList);
			/* CpuIdle items */
			if (setstor->getValue(Setting::SHOW_CPUIDLE_GRAPHS)
			    .boolv())
				addCpuIdleWork(cpu, workList);
			/* Task items */
			if (setstor->getValue(Setting::SHOW_SCHED_GRAPHS)
			    .boolv())
				addCpuSchedWork(cpu, workList);
		}
		s = workList.size();
		for (i = 0; i < s; i++)
			scalingQueue.addWorkItem(workList[i]);
		scalingQueue.start();
	}

	/* Migration scaling is done from the mainthread */
	if (enableMigrations())
		scaleMigration();

	if (useWorkList) {
		scalingQueue.wait();
		for (i = 0; i < s; i++)
			delete workList[i];
	}
}

void TraceAnalyzer::doStats()
{
	QList<AbstractWorkItem*> workList;
	int i, s;

	DEFINE_TASKMAP_ITERATOR(iter);
	for(iter = taskMap.begin(); iter != taskMap.end(); iter++) {
		Task *task = iter.value().task;
		WorkItem<Task> *taskItem = new WorkItem<Task>
			(task, &Task::doStats);
		workList.append(taskItem);
		statsQueue.addWorkItem(taskItem);
	}

	statsQueue.start();
	statsQueue.wait();

	s = workList.size();
	for (i = 0; i < s; i++)
		delete workList[i];
}

void TraceAnalyzer::doLimitedStats()
{
	QList<AbstractWorkItem*> workList;
	int i, s;

	DEFINE_TASKMAP_ITERATOR(iter);
	for(iter = taskMap.begin(); iter != taskMap.end(); iter++) {
		Task *task = iter.value().task;
		WorkItem<Task> *taskItem = new WorkItem<Task>
			(task, &Task::doStatsTimeLimited);
		workList.append(taskItem);
		statsLimitedQueue.addWorkItem(taskItem);
	}

	statsLimitedQueue.start();
	statsLimitedQueue.wait();

	s = workList.size();
	for (i = 0; i < s; i++)
		delete workList[i];
}

void TraceAnalyzer::processFtrace()
{
	processGeneric(TRACE_TYPE_FTRACE);
}

void TraceAnalyzer::processPerf()
{
	processGeneric(TRACE_TYPE_PERF);
}

void TraceAnalyzer::processAllFilters()
{
	int i;
	int s = events->size();
	const TraceEvent *eptr;

	filteredEvents.clear();

	for (i = 0; i < s; i++) {
		const TraceEvent &event = events->at(i);
		eptr = &event;
		/* OR filters */
		if (OR_filterState.isEnabled(FilterState::FILTER_CPU)) {
			if (OR_filterCPUMap.contains(event.cpu)) {
				filteredEvents.append(eptr);
				continue;
			}
		}
		if (OR_filterState.isEnabled(FilterState::FILTER_PID) &&
		    !processPidFilter(event, OR_filterPidMap,
				      OR_pidFilterInclusive)) {
			filteredEvents.append(eptr);
			continue;
		}
		if (OR_filterState.isEnabled(FilterState::FILTER_EVENT)) {
			if (OR_filterEventMap.contains(event.type) ) {
				filteredEvents.append(eptr);
				continue;
			}
		}
		if (OR_filterState.isEnabled(FilterState::FILTER_TIME)) {
			if (event.time >= OR_filterTimeLow &&
			    event.time <= OR_filterTimeHigh) {
				filteredEvents.append(eptr);
				continue;
			}
		}
		/* AND filters */
		if (filterState.isEnabled(FilterState::FILTER_CPU) &&
		    !filterCPUMap.contains(event.cpu)) {
			continue;
		}
		if (filterState.isEnabled(FilterState::FILTER_PID) &&
		    processPidFilter(event, filterPidMap,
				     pidFilterInclusive)) {
			continue;
		}
		if (filterState.isEnabled(FilterState::FILTER_EVENT) &&
		    !filterEventMap.contains(event.type)) {
			continue;
		}
		if (filterState.isEnabled(FilterState::FILTER_TIME) &&
		    (event.time < filterTimeLow || event.time > filterTimeHigh))
			continue;
		filteredEvents.append(eptr);
	}
}

void TraceAnalyzer::createPidFilter(QMap<int, int> &map,
				    bool orlogic, bool inclusive)
{
	/*
	 * An empty map is interpreted to mean that no filtering is desired,
	 * a map of the same size as the taskMap should mean that the user
	 * wants to filter on all pids, which is the same as no filtering
	 */
	if (map.isEmpty() || map.size() == taskMap.size()) {
		if (filterState.isEnabled(FilterState::FILTER_PID))
			disableFilter(FilterState::FILTER_PID);
		return;
	}

	if (orlogic) {
		OR_pidFilterInclusive = inclusive;
		OR_filterPidMap = map;
		OR_filterState.enable(FilterState::FILTER_PID);
	} else {
		pidFilterInclusive = inclusive;
		filterPidMap = map;
		filterState.enable(FilterState::FILTER_PID);
	}
	if (filterState.isEnabled())
		processAllFilters();
}

void TraceAnalyzer::createCPUFilter(QMap<unsigned, unsigned> &map,
				    bool orlogic)
{
	if (map.isEmpty() || map.size() == (int) nrCPUs) {
		if (filterState.isEnabled(FilterState::FILTER_CPU))
			disableFilter(FilterState::FILTER_CPU);
		return;
	}

	if (orlogic) {
		OR_filterCPUMap = map;
		OR_filterState.enable(FilterState::FILTER_CPU);
	} else {
		filterCPUMap = map;
		filterState.enable(FilterState::FILTER_CPU);
	}
	/* No need to process filters if we only have OR-filters */
	if (filterState.isEnabled())
		processAllFilters();
}

void TraceAnalyzer::createEventFilter(QMap<event_t, event_t> &map,
				      bool orlogic)
{
	/*
	 * An empty map is interpreted to mean that no filtering is desired,
	 * a map of the same size as the taskMap should mean that the user
	 * wants to filter on all pids, which is the same as no filtering
	 */
	if (map.isEmpty() || map.size() == TraceEvent::getNrEvents()) {
		if (filterState.isEnabled(FilterState::FILTER_EVENT))
			disableFilter(FilterState::FILTER_EVENT);
		return;
	}

	if (orlogic) {
		OR_filterEventMap = map;
		OR_filterState.enable(FilterState::FILTER_EVENT);
	} else {
		filterEventMap = map;
		filterState.enable(FilterState::FILTER_EVENT);
	}
	/* No need to process filters if we only have OR-filters */
	if (filterState.isEnabled())
		processAllFilters();
}

void TraceAnalyzer::createTimeFilter(const vtl::Time &low,
				     const vtl::Time &high,
				     bool orlogic)
{
	vtl::Time start = getStartTime();
	vtl::Time end = getEndTime();

	if (low < start && high > end)
		return;

	if (orlogic) {
		OR_filterTimeLow = low;
		OR_filterTimeHigh = high;
		OR_filterState.enable(FilterState::FILTER_TIME);
	} else {
		filterTimeLow = low;
		filterTimeHigh = high;
		filterState.enable(FilterState::FILTER_TIME);
	}
	/* No need to process filters if we only have OR-filters */
	if (filterState.isEnabled())
		processAllFilters();
}

void TraceAnalyzer::disableFilter(FilterState::filter_t filter)
{
	filterState.disable(filter);
	OR_filterState.disable(filter);
	switch (filter) {
	case FilterState::FILTER_PID:
		filterPidMap.clear();
		OR_filterPidMap.clear();
		break;
	case FilterState::FILTER_EVENT:
		filterEventMap.clear();
		OR_filterEventMap.clear();
		break;
	case FilterState::FILTER_TIME:
		/* We need to do nothing */
		break;
	case FilterState::FILTER_CPU:
		filterCPUMap.clear();
		OR_filterCPUMap.clear();
		break;
	case FilterState::FILTER_ARG:
		/* Argument filtering is not yet implemented */
		break;
	default:
		break;
	}
	if (filterState.isEnabled())
		processAllFilters();
	else
		filteredEvents.clear();
}

void TraceAnalyzer::addPidToFilter(int pid) {
	DEFINE_FILTER_PIDMAP_ITERATOR(iter);

	iter = filterPidMap.find(pid);
	if (iter != filterPidMap.end()) {
		if (filterState.isEnabled(FilterState::FILTER_PID))
			return;
		goto epilogue;
	}
	filterPidMap[pid] = pid;

epilogue:
	/* Disable filter if we are filtering on all pids */
	if (filterPidMap.size() == taskMap.size()) {
		disableFilter(FilterState::FILTER_PID);
		return;
	}

	filterState.enable(FilterState::FILTER_PID);
	processAllFilters();
}

void TraceAnalyzer::removePidFromFilter(int pid) {
	DEFINE_FILTER_PIDMAP_ITERATOR(iter);

	if (!filterState.isEnabled(FilterState::FILTER_PID))
		return;

	iter = filterPidMap.find(pid);
	if (iter == filterPidMap.end()) {
		return;
	}

	iter = filterPidMap.erase(iter);
	if (filterPidMap.isEmpty()) {
		disableFilter(FilterState::FILTER_PID);
		return;
	}
	processAllFilters();
}

void TraceAnalyzer::disableAllFilters()
{
	filterState.disableAll();
	OR_filterState.disableAll();

	filterPidMap.clear();
	OR_filterPidMap.clear();

	filterEventMap.clear();
	OR_filterEventMap.clear();

	filteredEvents.clear();
}

bool TraceAnalyzer::isFiltered() const
{
	/*
	 * No need to consider us filtered if we only have OR-filters, so we
	 * only check the normal (AND) filters
	 */
	return filterState.isEnabled();
}

bool TraceAnalyzer::filterActive(FilterState::filter_t filter) const
{
	return filterState.isEnabled(filter) ||
		OR_filterState.isEnabled(filter);
}

#define WRITE_BUFFER_SIZE (256 * sysconf(_SC_PAGESIZE))
#define WRITE_BUFFER_LIMIT ((WRITE_BUFFER_SIZE - 64 * 1024))

const char TraceAnalyzer::spaceStr[] = \
	"                                     ";
const int TraceAnalyzer::spaceStrLen = strlen(spaceStr);

const char *const TraceAnalyzer::cpuevents[] =
{
	"cpu-cycles", "cycles",
};

const int TraceAnalyzer::CPUEVENTS_NR = arraylen(cpuevents);

event_t TraceAnalyzer::determineCPUEvent(bool &ok)
{
	int i, j;
	int maxevent;
	const StringTree<> *stree = TraceEvent::getStringTree();
	const TString *ename;
	event_t rval = (event_t) 0;
	event_t event;
	ok = false;

	maxevent = (int) stree->getMaxEvent();

	for (i = 0; i <= maxevent; i++) {
		event = (event_t) i;
		ename = stree->stringLookup(event);
		for (j = 0; j < CPUEVENTS_NR; j++) {
			if (!strcmp(ename->ptr, cpuevents[j])) {
				rval = event;
				ok = true;
				break;
			}
		}
		if (ok)
			break;
	}
	return rval;
}

bool TraceAnalyzer::exportTraceFile(const char *fileName, int *ts_errno,
				    exporttype_t export_type)
{
	bool isFtrace = false, isPerf = false;
	char *wbuf, *wb;
	int fd, w;
	int written, written_io, space, nrspaces, write_rval;
	int nr_elements;
	int idx;
	int i;
	const TraceEvent *eptr;
	bool rval = true;
	const char *ename;
	char tbuf[40];
	event_t cpuevent_type = (event_t) 0;
	bool ok;
	bool filtered = isFiltered();

	nr_elements = filtered ? filteredEvents.size() : events->size();
	*ts_errno = 0;

	if (!isOpen()) {
		*ts_errno = - TS_ERROR_INTERNAL;
		return false;
	}

	switch (getTraceType()) {
	case TRACE_TYPE_FTRACE:
		isFtrace = true;
		break;
	case TRACE_TYPE_PERF:
		isPerf = true;
		break;
	default:
		*ts_errno = - TS_ERROR_INTERNAL;
		return false;
	}

	wbuf = (char*) mmap(nullptr, (size_t) WRITE_BUFFER_SIZE,
			    PROT_READ|PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS,
			    -1, 0);

	if (wbuf == MAP_FAILED)
		mmap_err();

	if (!parser->traceFile->isIntact(ts_errno)) {
		rval = false;
		if (*ts_errno == 0)
			*ts_errno = - TS_ERROR_FILECHANGED;
		goto error_munmap;
	}

	parser->traceFile->allocMmap();
	fd =  clib_open(fileName, O_WRONLY | O_CREAT,
			(S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH));

	if (fd < 0) {
		rval = false;
		*ts_errno = errno;
		goto error_munmap;
	}

	idx = 0;

	if (export_type == EXPORT_TYPE_CPU_CYCLES) {
		cpuevent_type = determineCPUEvent(ok);
		if (!ok) {
			rval = false;
			*ts_errno = - TS_ERROR_NOCPUEV;
			goto error_close;
		}
	}

	if (!isPerf)
		goto skip_perf;

	do {
		written = 0;
		space = WRITE_BUFFER_SIZE;
		wb = wbuf;

		while (idx < nr_elements && written < WRITE_BUFFER_LIMIT) {
			if (filtered)
				eptr = filteredEvents[idx];
			else
				eptr = &(*events)[idx];
			idx++;
			if (export_type == EXPORT_TYPE_CPU_CYCLES &&
			    eptr->type != cpuevent_type)
				continue;
			eptr->time.sprint(tbuf);
			w = snprintf(wb, space,
				     "%s %5u [%03u] %s: ",
				     eptr->taskName->ptr, eptr->pid,
				     eptr->cpu, tbuf);
			if (w > 0) {
				written += w;
				space   -= w;
				wb      += w;
			}
			if (eptr->intArg != 0) {
				w = snprintf(wb, space, "%10u ",
					     eptr->intArg);
				if (w > 0) {
					written += w;
					space   -= w;
					wb      += w;
				}
			}

			ename = eptr->getEventName()->ptr;
			nrspaces = TSMAX(1, spaceStrLen - strlen(ename));
			nrspaces = TSMIN(nrspaces, space);
			if (nrspaces > 0) {
				strncpy(wb, spaceStr, nrspaces);
				written += nrspaces;
				space   -= nrspaces;
				wb      += nrspaces;
			}

			w = snprintf(wb, space, "%s:", ename);
			if (w > 0) {
				written += w;
				space   -= w;
				wb      += w;
			}

			for (i = 0; i < eptr->argc; i++) {
				w = snprintf(wb, space, " %s",
					     eptr->argv[i]->ptr);
				if (w > 0) {
					written += w;
					space   -= w;
					wb      += w;
				}
			}
			w = snprintf(wb, space, "\n");
			if (w > 0) {
				written += w;
				space   -= w;
				wb      += w;
			}
			if (eptr->postEventInfo != nullptr &&
			    eptr->postEventInfo->len > 0) {
				size_t cs = TSMIN(space,
						  eptr->postEventInfo->len);
				parser->traceFile->readChunk(
					eptr->postEventInfo, wb, space,
					ts_errno);
				if (*ts_errno != 0) {
					rval = false;
					goto error_close;
				}
				if (cs > 0) {
					written += cs;
					space   -= cs;
					wb      += cs;
				}
			}
		}

		if (written > 0) {
			written_io = 0;
			do {
				write_rval = write(fd, wbuf, written);
				if (write_rval > 0) {
					written_io += write_rval;
				}
				if (write_rval < 0 && errno != EINTR) {
					rval = false;
					*ts_errno = errno;
					goto error_close;
				}
			} while(written_io < written);
		}
		if (idx >= nr_elements)
			break;
	} while(true);

	if (!parser->traceFile->isIntact(ts_errno)) {
		rval = false;
		if (*ts_errno == 0)
			*ts_errno = - TS_ERROR_FILECHANGED;
		goto error_close;
	}

skip_perf:
	if (!isFtrace)
		goto skip_ftrace;

/* Insert ftrace code here */

skip_ftrace:
error_close:
	if (clib_close(fd) != 0) {
		if (errno != EINTR) {
			rval = false;
			*ts_errno = errno;
		}
	}

error_munmap:
	parser->traceFile->freeMmap();
	if (munmap(wbuf, WRITE_BUFFER_SIZE) != 0)
		munmap_err();

	return rval;
}

TraceFile *TraceAnalyzer::getTraceFile()
{
	return parser->traceFile;
}
