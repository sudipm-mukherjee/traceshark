/*
 * Traceshark - a visualizer for visualizing ftrace and perf traces
 * Copyright (C) 2015-2017  Viktor Rosendahl <viktor.rosendahl@gmail.com>
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
 *     DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 *     CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *     SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 *     NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 *     LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 *     HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 *     CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 *     OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
 *     EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef TLIST_H
#define TLIST_H

#include <climits>
#include <cstdlib>

extern "C" {
#include <sys/mman.h>
}

namespace vtl {

#define TLIST_MAX(A, B) ((A) >= (B) ? A:B)
#define TLIST_MIN(A, B) ((A) < (B) ? A:B)

/*
 * Here we assume that UINT_MAX can be expressed as 2^N - 1
 * This is a bit theoretical, if we have for example 64-bit unsigned ints, then
 * the idea is that we want to keep the array of pointers at a size of no more
 * than one million elements, in order to avoid execessive use of address space
 * and overcommit of memory, or rather address space
 */
#define TLIST_INDEX_MAX (TLIST_MIN(0xffffffffff, UINT_MAX))

/*
 * This code is needed in order to take into account platforms with small
 * unsigned ints, since it's not guaranteed to be more than 2^16 -1, although
 * I have to say that I don't think traceshark would run well on a platform with
 * 16-bit ints.
 */
#if UINT_MAX > 0x100000
/* This is the normal case */
#define TLIST_MAP_NR_ELEMENTS (0x100000)
#define TLIST_MAP_SHIFT (20) /* Number of zero bits above */
#else
/* Ouch, we are probably on a 16-bit platform! */
#define TLIST_MAP_NR_ELEMENTS (0x1000)
#define TLIST_MAP_SHIFT (12) /* Number of zero bits above */
#endif

#define TLIST_MAP_ELEMENT_MASK (TLIST_MAP_NR_ELEMENTS - 1)
#define TLIST_MAP_MASK (TLIST_INDEX_MAX - TLIST_MAP_ELEMENT_MASK)

template<class T>
class TList
{
public:
	TList();
	~TList();
	__always_inline void append(const T &element);
	__always_inline T& increase();
	__always_inline T& preAlloc();
	__always_inline void commit();
	__always_inline T value(unsigned int index) const;
	__always_inline const T& at(unsigned int index) const;
	__always_inline T& last();
	__always_inline unsigned int size() const;
	void clear();
	void softclear();
	__always_inline T& operator[](unsigned int index);
	__always_inline const T& operator[](unsigned int index) const;
	__always_inline void swap(unsigned int a, unsigned int b);
private:
	__always_inline T& subscript(unsigned int index) const;
	__always_inline unsigned int mapFromIndex(unsigned int index) const;
	__always_inline unsigned int mapIndexFromIndex(unsigned int index)
		const;
	void clearAll();
	void setupMem();
	void addMem();
	void decMem();
	unsigned int nrMaps;
	unsigned int nrElements;
	T **mapArray;
};

template<class T>
TList<T>::TList():
nrMaps(0), nrElements(0)
{
	setupMem();
}

template<class T>
TList<T>::~TList()
{
	clearAll();
}

template<class T>
__always_inline unsigned int TList<T>::mapFromIndex(unsigned int index) const
{
	return (index >> TLIST_MAP_SHIFT);
}

template<class T>
__always_inline unsigned int TList<T>::mapIndexFromIndex(unsigned int index)
const
{
	return (index & TLIST_MAP_ELEMENT_MASK);
}

template<class T>
void TList<T>::setupMem()
{
	unsigned int maxNrMaps = mapFromIndex(TLIST_MAP_MASK) + 1;
	mapArray = (T**) mmap(nullptr, (size_t) maxNrMaps * sizeof(T*),
			     PROT_READ | PROT_WRITE,
			     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (mapArray == MAP_FAILED)
		abort();
	addMem();
}

template<class T>
void TList<T>::addMem()
{
	mapArray[nrMaps] = (T*) mmap(nullptr, (size_t) TLIST_MAP_NR_ELEMENTS *
				sizeof(T), PROT_READ | PROT_WRITE,
				MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (mapArray[nrMaps] == MAP_FAILED)
		abort();
	nrMaps++;
}

template<class T>
void TList<T>::decMem()
{
	nrMaps--;
	munmap(mapArray[nrMaps], TLIST_MAP_NR_ELEMENTS * sizeof(T));
}

template<class T>
void TList<T>::clearAll()
{
	unsigned int maxNrMaps = mapFromIndex(TLIST_MAP_MASK) + 1;
	unsigned int i;
	for (i = 0; i < nrMaps; i++)
		munmap(mapArray[i], TLIST_MAP_NR_ELEMENTS * sizeof(T));
	munmap(mapArray, maxNrMaps * sizeof(T*));
	nrMaps = 0;
	nrElements = 0;
}

template<class T>
__always_inline T& TList<T>::increase()
{
	unsigned int map = mapFromIndex(nrElements);
	unsigned int mapIndex = mapIndexFromIndex(nrElements);
	nrElements++;
	if (map == nrMaps)
		addMem();
	return mapArray[map][mapIndex];
}

template<class T>
__always_inline T& TList<T>::preAlloc()
{
	unsigned int map = mapFromIndex(nrElements);
	unsigned int mapIndex = mapIndexFromIndex(nrElements);
	if (map == nrMaps)
		addMem();
	return mapArray[map][mapIndex];
}

template<class T>
__always_inline void TList<T>::commit()
{
	nrElements++;
}

template<class T>
__always_inline void TList<T>::append(const T &element)
{
	unsigned int map = mapFromIndex(nrElements);
	unsigned int mapIndex = mapIndexFromIndex(nrElements);
	nrElements++;
	if (map == nrMaps)
		addMem();
	mapArray[map][mapIndex] = element;
}

template<class T>
__always_inline const T& TList<T>::at(unsigned int index) const
{
	unsigned int map = mapFromIndex(index);
	unsigned int mapIndex = mapIndexFromIndex(index);
	return mapArray[map][mapIndex];
}

template<class T>
__always_inline T TList<T>::value(unsigned int index) const
{
	if (index >= nrElements) {
		T dvalue;
		return dvalue;
	}
	return at(index);
}

template<class T>
__always_inline T& TList<T>::last()
{
	unsigned int index = nrElements - 1;
	unsigned int map = mapFromIndex(index);
	unsigned int mapIndex = mapIndexFromIndex(index);
	return mapArray[map][mapIndex];
	
}

template<class T>
__always_inline unsigned int TList<T>::size() const
{
	return nrElements;
}

template<class T>
void TList<T>::clear()
{
	clearAll();
	setupMem();
}

template<class T>
void TList<T>::softclear()
{
	nrElements = 0;
}

template<class T>
__always_inline void TList<T>::swap(unsigned int a, unsigned int b)
{
	T foo;
	T &ta = subscript(a);
	T &tb = subscript(b);
	foo = ta;
	ta = tb;
	tb = foo;
}

template<class T>
__always_inline T& TList<T>::subscript(unsigned int index) const
{
	unsigned int map = mapFromIndex(index);
	unsigned int mapIndex = mapIndexFromIndex(index);
	return mapArray[map][mapIndex];
}

template<class T>
__always_inline T& TList<T>::operator[](unsigned int index)
{
	return subscript(index);
}

template<class T>
__always_inline const T& TList<T>::operator[](unsigned int index) const
{
	return subscript(index);
}

}

#endif /* TLIST_H */