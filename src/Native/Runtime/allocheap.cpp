//
// Copyright (c) Microsoft Corporation.  All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.
//
#include "rhcommon.h"
#include "commontypes.h"
#include "daccess.h"
#include "commonmacros.h"
#include "debugmacrosext.h"
#include "palredhawkcommon.h"
#include "palredhawk.h"
#include "assert.h"
#include "static_check.h"
#include "slist.h"
#include "holder.h"
#include "crst.h"
#include "range.h"
#ifdef FEATURE_RWX_MEMORY
#include "memaccessmgr.h"
#endif
#include "allocheap.h"

#include "commonmacros.inl"
#include "slist.inl"

using namespace rh::util;

//-------------------------------------------------------------------------------------------------
AllocHeap::AllocHeap()
    : m_blockList(),
      m_rwProtectType(PAGE_READWRITE),
      m_roProtectType(PAGE_READWRITE),
#ifdef FEATURE_RWX_MEMORY
      m_pAccessMgr(NULL),
      m_hCurPageRW(),
#endif // FEATURE_RWX_MEMORY
      m_pNextFree(NULL),
      m_pFreeCommitEnd(NULL),
      m_pFreeReserveEnd(NULL),
      m_pbInitialMem(NULL),
      m_fShouldFreeInitialMem(false),
      m_lock(CrstAllocHeap)
      COMMA_INDEBUG(m_fIsInit(false))
{
    ASSERT(!_UseAccessManager());
}

#ifdef FEATURE_RWX_MEMORY
//-------------------------------------------------------------------------------------------------
AllocHeap::AllocHeap(
    UInt32 rwProtectType,
    UInt32 roProtectType,
    MemAccessMgr* pAccessMgr)
    : m_blockList(),
      m_rwProtectType(rwProtectType),
      m_roProtectType(roProtectType == 0 ? rwProtectType : roProtectType),
      m_pAccessMgr(pAccessMgr),
      m_hCurPageRW(),
      m_pNextFree(NULL),
      m_pFreeCommitEnd(NULL),
      m_pFreeReserveEnd(NULL),
      m_pbInitialMem(NULL),
      m_fShouldFreeInitialMem(false),
      m_lock(CrstAllocHeap)
      COMMA_INDEBUG(m_fIsInit(false))
{
    ASSERT(!_UseAccessManager() || (m_rwProtectType != m_roProtectType && m_pAccessMgr != NULL));
}
#endif // FEATURE_RWX_MEMORY

//-------------------------------------------------------------------------------------------------
bool AllocHeap::Init()
{
    ASSERT(!m_fIsInit);
    INDEBUG(m_fIsInit = true;)

    return true;
}

//-------------------------------------------------------------------------------------------------
// This is for using pre-allocated memory on heap construction.
// Should never use this more than once, and should always follow construction of heap.

bool AllocHeap::Init(
    UInt8 *    pbInitialMem,
    UIntNative cbInitialMemCommit,
    UIntNative cbInitialMemReserve,
    bool       fShouldFreeInitialMem)
{
    ASSERT(!m_fIsInit);

#ifdef FEATURE_RWX_MEMORY
    // Manage the committed portion of memory
    if (_UseAccessManager())
    {
        m_pAccessMgr->ManageMemoryRange(MemRange(pbInitialMem, cbInitialMemCommit), true);
    }
#endif // FEATURE_RWX_MEMORY

    BlockListElem *pBlock = new BlockListElem(pbInitialMem, cbInitialMemReserve);
    if (pBlock == NULL)
        return false;
    m_blockList.PushHead(pBlock);

    if (!_UpdateMemPtrs(pbInitialMem,
                        pbInitialMem + cbInitialMemCommit,
                        pbInitialMem + cbInitialMemReserve))
    {
        return false;
    }

    m_pbInitialMem = pbInitialMem;
    m_fShouldFreeInitialMem = fShouldFreeInitialMem;

    INDEBUG(m_fIsInit = true;)
    return true;
}

//-------------------------------------------------------------------------------------------------
AllocHeap::~AllocHeap()
{
    while (!m_blockList.IsEmpty())
    {
        BlockListElem *pCur = m_blockList.PopHead();
        if (pCur->GetStart() != m_pbInitialMem || m_fShouldFreeInitialMem)
            PalVirtualFree(pCur->GetStart(), pCur->GetLength(), MEM_RELEASE);
        delete pCur;
    }
}

//-------------------------------------------------------------------------------------------------
UInt8 * AllocHeap::_Alloc(
    UIntNative cbMem,
    UIntNative alignment
    WRITE_ACCESS_HOLDER_ARG
    )
{
#ifndef FEATURE_RWX_MEMORY
    const void* pRWAccessHolder = NULL;
#endif // FEATURE_RWX_MEMORY

    ASSERT((alignment & (alignment - 1)) == 0); // Power of 2 only.
    ASSERT(alignment <= OS_PAGE_SIZE);          // Can't handle this right now.
    ASSERT((m_rwProtectType == m_roProtectType) == (pRWAccessHolder == NULL));
    ASSERT(!_UseAccessManager() || pRWAccessHolder != NULL);

    if (_UseAccessManager() && pRWAccessHolder == NULL)
        return NULL;

    CrstHolder lock(&m_lock);

    UInt8 * pbMem = _AllocFromCurBlock(cbMem, alignment PASS_WRITE_ACCESS_HOLDER_ARG);
    if (pbMem != NULL)
        return pbMem;

    // Must allocate new block
    if (!_AllocNewBlock(cbMem))
        return NULL;

    pbMem = _AllocFromCurBlock(cbMem, alignment PASS_WRITE_ACCESS_HOLDER_ARG);
    ASSERT_MSG(pbMem != NULL, "AllocHeap::Alloc: failed to alloc mem after new block alloc");

    return pbMem;
}

//-------------------------------------------------------------------------------------------------
UInt8 * AllocHeap::Alloc(
    UIntNative cbMem
    WRITE_ACCESS_HOLDER_ARG)
{
    return _Alloc(cbMem, 1 PASS_WRITE_ACCESS_HOLDER_ARG);
}

//-------------------------------------------------------------------------------------------------
UInt8 * AllocHeap::AllocAligned(
    UIntNative cbMem,
    UIntNative alignment
    WRITE_ACCESS_HOLDER_ARG)
{
    return _Alloc(cbMem, alignment PASS_WRITE_ACCESS_HOLDER_ARG);
}

//-------------------------------------------------------------------------------------------------
bool AllocHeap::Contains(void* pvMem, UIntNative cbMem)
{
    MemRange range(pvMem, cbMem);
    for (BlockList::Iterator it = m_blockList.Begin(); it != m_blockList.End(); ++it)
    {
        if (it->Contains(range))
        {
            return true;
        }
    }
    return false;
}

#ifdef FEATURE_RWX_MEMORY
//-------------------------------------------------------------------------------------------------
bool AllocHeap::_AcquireWriteAccess(
    UInt8* pvMem,
    UIntNative cbMem,
    WriteAccessHolder* pHolder)
{
    ASSERT(!_UseAccessManager() || m_pAccessMgr != NULL);

    if (_UseAccessManager())
        return m_pAccessMgr->AcquireWriteAccess(MemRange(pvMem, cbMem), m_hCurPageRW, pHolder);
    else
        return true;
}

//-------------------------------------------------------------------------------------------------
bool AllocHeap::AcquireWriteAccess(
    void* pvMem,
    UIntNative cbMem,
    WriteAccessHolder* pHolder)
{
    return _AcquireWriteAccess(static_cast<UInt8*>(pvMem), cbMem, pHolder);
}
#endif // FEATURE_RWX_MEMORY

//-------------------------------------------------------------------------------------------------
bool AllocHeap::_UpdateMemPtrs(UInt8* pNextFree, UInt8* pFreeCommitEnd, UInt8* pFreeReserveEnd)
{
    ASSERT(MemRange(pNextFree, pFreeReserveEnd).Contains(MemRange(pNextFree, pFreeCommitEnd)));
    ASSERT(ALIGN_DOWN(pFreeCommitEnd, OS_PAGE_SIZE) == pFreeCommitEnd);
    ASSERT(ALIGN_DOWN(pFreeReserveEnd, OS_PAGE_SIZE) == pFreeReserveEnd);

#ifdef FEATURE_RWX_MEMORY
    // See if we need to update current allocation holder or protect committed pages.
    if (_UseAccessManager())
    {
        if (pFreeCommitEnd - pNextFree > 0)
        {
#ifndef STRESS_MEMACCESSMGR
            // Create or update the alloc cache, used to speed up new allocations.
            // If there is available commited memory and either m_pNextFree is
            // being updated past a page boundary or the current cache is empty,
            // then update the cache.
            if (ALIGN_DOWN(m_pNextFree, OS_PAGE_SIZE) != ALIGN_DOWN(pNextFree, OS_PAGE_SIZE) ||
                m_hCurPageRW.GetRange().GetLength() == 0)
            {
                // Update current alloc page write access holder.
                if (!_AcquireWriteAccess(ALIGN_DOWN(pNextFree, OS_PAGE_SIZE),
                                         OS_PAGE_SIZE,
                                         &m_hCurPageRW))
                {
                    return false;
                }
            }
#endif // STRESS_MEMACCESSMGR

        }
        else
        {   // No available committed memory. Release the cache.
            m_hCurPageRW.Release();
        }
    }
#endif // FEATURE_RWX_MEMORY

    m_pNextFree = pNextFree;
    m_pFreeCommitEnd = pFreeCommitEnd;
    m_pFreeReserveEnd = pFreeReserveEnd;
    return true;
}

//-------------------------------------------------------------------------------------------------
bool AllocHeap::_UpdateMemPtrs(UInt8* pNextFree, UInt8* pFreeCommitEnd)
{
    return _UpdateMemPtrs(pNextFree, pFreeCommitEnd, m_pFreeReserveEnd);
}

//-------------------------------------------------------------------------------------------------
bool AllocHeap::_UpdateMemPtrs(UInt8* pNextFree)
{
    return _UpdateMemPtrs(pNextFree, m_pFreeCommitEnd);
}

//-------------------------------------------------------------------------------------------------
bool AllocHeap::_AllocNewBlock(UIntNative cbMem)
{
    cbMem = ALIGN_UP(max(cbMem, s_minBlockSize), OS_PAGE_SIZE);;

    UInt8 * pbMem = reinterpret_cast<UInt8*>
        (PalVirtualAlloc(NULL, cbMem, MEM_COMMIT, m_roProtectType));

    if (pbMem == NULL)
        return false;

    BlockListElem *pBlockListElem = new BlockListElem(pbMem, cbMem);
    if (pBlockListElem == NULL)
    {
        PalVirtualFree(pbMem, 0, MEM_RELEASE);
        return false;
    }

    // Add to the list. While there is no race for writers (we hold the lock) we have the
    // possibility of simultaneous readers, and using the interlocked version creates a
    // memory barrier to make sure any reader sees a consistent list.
    m_blockList.PushHeadInterlocked(pBlockListElem);

    return _UpdateMemPtrs(pbMem, pbMem + cbMem, pbMem + cbMem);
}

//-------------------------------------------------------------------------------------------------
UInt8 * AllocHeap::_AllocFromCurBlock(
    UIntNative cbMem,
    UIntNative alignment
    WRITE_ACCESS_HOLDER_ARG)
{
    UInt8 * pbMem = NULL;

    cbMem += (UInt8 *)ALIGN_UP(m_pNextFree, alignment) - m_pNextFree;

    if (m_pNextFree + cbMem <= m_pFreeCommitEnd ||
        _CommitFromCurBlock(cbMem))
    {
        ASSERT(cbMem + m_pNextFree <= m_pFreeCommitEnd);
#ifdef FEATURE_RWX_MEMORY
        if (pRWAccessHolder != NULL)
        {
            if (!_AcquireWriteAccess(m_pNextFree, cbMem, pRWAccessHolder))
                return NULL;
        }
#endif // FEATURE_RWX_MEMORY
        pbMem = ALIGN_UP(m_pNextFree, alignment);

        if (!_UpdateMemPtrs(m_pNextFree + cbMem))
            return NULL;
    }

    return pbMem;
}

//-------------------------------------------------------------------------------------------------
bool AllocHeap::_CommitFromCurBlock(UIntNative cbMem)
{
    ASSERT(m_pFreeCommitEnd < m_pNextFree + cbMem);

    if (m_pNextFree + cbMem <= m_pFreeReserveEnd)
    {
        UIntNative cbMemToCommit = ALIGN_UP(cbMem, OS_PAGE_SIZE);

#ifdef FEATURE_RWX_MEMORY
        if (_UseAccessManager())
        {
            if (!m_pAccessMgr->ManageMemoryRange(MemRange(m_pFreeCommitEnd, cbMemToCommit), false))
                return false;
        }
        else
        {
            UInt32 oldProtectType;
            if (!PalVirtualProtect(m_pFreeCommitEnd, cbMemToCommit, m_roProtectType, &oldProtectType))
                return false;
        }
#endif // FEATURE_RWX_MEMORY

        return _UpdateMemPtrs(m_pNextFree, m_pFreeCommitEnd + cbMemToCommit);
    }

    return false;
}

//-------------------------------------------------------------------------------------------------
void * __cdecl operator new(UIntNative n, AllocHeap * alloc)
{
    return alloc->Alloc(n);
}

//-------------------------------------------------------------------------------------------------
void * __cdecl operator new[](UIntNative n, AllocHeap * alloc)
{
    return alloc->Alloc(n);
}

