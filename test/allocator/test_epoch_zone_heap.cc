/*
 *  (c) Copyright 2016-2017 Hewlett Packard Enterprise Development Company LP.
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU Lesser General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *  As an exception, the copyright holders of this Library grant you permission
 *  to (i) compile an Application with the Library, and (ii) distribute the
 *  Application containing code generated by the Library and added to the
 *  Application during this compilation process under terms of your choice,
 *  provided you also meet the terms and conditions of the Application license.
 *
 */

#include <unistd.h> // sleep
#include <list>
#include <random>
#include <limits>
#include <vector>
#include <thread>
#include <chrono>

#include <gtest/gtest.h>
#include "nvmm/memory_manager.h"
#include "test_common/test.h"

using namespace nvmm;

// random number and string generator
std::random_device r;
std::default_random_engine e1(r());
uint64_t rand_uint64(uint64_t min = 0,
                     uint64_t max = std::numeric_limits<uint64_t>::max()) {
    std::uniform_int_distribution<uint64_t> uniform_dist(min, max);
    return uniform_dist(e1);
}

// regular free
TEST(EpochZoneHeap, Free) {
    PoolId pool_id = 1;
    size_t size = 128 * 1024 * 1024LLU; // 128 MB

    MemoryManager *mm = MemoryManager::GetInstance();
    Heap *heap = NULL;

    // create a heap
    EXPECT_EQ(ID_NOT_FOUND, mm->FindHeap(pool_id, &heap));
    EXPECT_EQ(NO_ERROR, mm->CreateHeap(pool_id, size));
    EXPECT_EQ(ID_FOUND, mm->CreateHeap(pool_id, size));

    // get the heap
    EXPECT_EQ(NO_ERROR, mm->FindHeap(pool_id, &heap));
    EXPECT_EQ(NO_ERROR, heap->Open());

    // allocate & free
    GlobalPtr ptr = heap->Alloc(sizeof(int));
    heap->Free(ptr);

    // allocate again, because of immediate free, the new ptr should be the same
    // as the previous ptr
    GlobalPtr ptr1 = heap->Alloc(sizeof(int));
    EXPECT_EQ(ptr, ptr1);
    heap->Free(ptr1);

    // destroy the heap
    EXPECT_EQ(NO_ERROR, heap->Close());
    delete heap;
    EXPECT_EQ(NO_ERROR, mm->DestroyHeap(pool_id));
    EXPECT_EQ(ID_NOT_FOUND, mm->DestroyHeap(pool_id));
}

// delayed free
TEST(EpochZoneHeap, DelayedFree) {
    PoolId pool_id = 1;
    size_t size = 128 * 1024 * 1024LLU; // 128 MB

    MemoryManager *mm = MemoryManager::GetInstance();
    EpochManager *em = EpochManager::GetInstance();
    Heap *heap = NULL;

    // create a heap
    EXPECT_EQ(ID_NOT_FOUND, mm->FindHeap(pool_id, &heap));
    EXPECT_EQ(NO_ERROR, mm->CreateHeap(pool_id, size));
    EXPECT_EQ(ID_FOUND, mm->CreateHeap(pool_id, size));

    // get the heap
    EXPECT_EQ(NO_ERROR, mm->FindHeap(pool_id, &heap));
    EXPECT_EQ(NO_ERROR, heap->Open());

    EpochCounter e1;
    GlobalPtr ptr1;

    // allocate & delayed free
    {
        EpochOp op(em);
        e1 = op.reported_epoch();
        std::cout << "first epoch " << e1 << std::endl;
        ptr1 = heap->Alloc(op, sizeof(int));
        heap->Free(op, ptr1);
        // allocate again, because of delayed free, the new ptr should be
        // different from t he
        // previous ptr
        GlobalPtr ptr2 = heap->Alloc(op, sizeof(int));
        EXPECT_NE(ptr1, ptr2);
        heap->Free(op, ptr2);
    }

    // wait a few epoches and make sure the background thread picks up this
    // chunk and frees it
    EpochCounter e2;
    while (1) {
        {
            // Begin epoch in a new scope block so that we exit the epoch when
            // we out of scope and don't block others when we then sleep.
            {
                EpochOp op(em);
                e2 = op.reported_epoch();
            }
            if (e2 - e1 >= 3 && e2 % 5 == (e1 + 3) % 5) {
                std::cout << "sleeping at epoch " << e2 << std::endl;
                sleep(1); // making sure the background thread wakes up in this
                          // epoch
                break;
            }
        }
    }

    while (1) {
        {
            EpochOp op(em);
            EpochCounter e3 = op.reported_epoch();
            if (e3 > e2) {
                break;
            }
        }
    }

    // now the ptr that was delayed freed must have been actually freed
    {
        EpochOp op(em);
        std::cout << "final epoch " << op.reported_epoch() << std::endl;
        GlobalPtr ptr2 = heap->Alloc(op, sizeof(int));
        EXPECT_EQ(ptr1, ptr2);
        heap->Free(ptr2);
    }

    // destroy the heap
    EXPECT_EQ(NO_ERROR, heap->Close());
    delete heap;
    EXPECT_EQ(NO_ERROR, mm->DestroyHeap(pool_id));
    EXPECT_EQ(ID_NOT_FOUND, mm->DestroyHeap(pool_id));
}

//
// Simple Resize
// Test case :
// 1. Create heap
// 2. Allocate it completely. Verify if allocation is from shelf 0
// 3. Resize heap
// 4. Allocat, Verify if the allocation is from shelf 1
//
TEST(EpochZoneHeap, Resize) {
    PoolId pool_id = 1;
    size_t min_alloc_size = 128;
    size_t heap_size = min_alloc_size * 1024 * 1024LLU; // 128 MB
    size_t allocated_size = 0;
    size_t alloc_size = 1024 * 1024LLU; // 1MB per alloc
    GlobalPtr ptr[512];
    int i = 0;

    MemoryManager *mm = MemoryManager::GetInstance();
    Heap *heap = NULL;

    // create a heap
    EXPECT_EQ(ID_NOT_FOUND, mm->FindHeap(pool_id, &heap));
    EXPECT_EQ(NO_ERROR, mm->CreateHeap(pool_id, heap_size));
    EXPECT_EQ(ID_FOUND, mm->CreateHeap(pool_id, heap_size));

    // get the heap
    EXPECT_EQ(NO_ERROR, mm->FindHeap(pool_id, &heap));
    EXPECT_EQ(NO_ERROR, heap->Open());

    // You should be able allocate (allocated_size - alloc_size) from the heap.
    do {
       ptr[i] = heap->Alloc(alloc_size);
       allocated_size += alloc_size;
       EXPECT_NE(ptr[i],0);       
       EXPECT_EQ((int)ptr[i].GetShelfId().GetShelfIndex(),1);       
       i++;
    } while(allocated_size < (heap_size - alloc_size)); // Loop until last item.

    // Just do one more allocation in case we are able to allocate entire heap
    ptr[i] = heap->Alloc(alloc_size);
    if(ptr[i] != 0)
        i++;
    allocated_size = heap_size;

    heap_size = heap_size * 2;    
    EXPECT_EQ(NO_ERROR, heap->Resize(heap_size));
    EXPECT_EQ(heap->Size(), heap_size);

    do {
        ptr[i] = heap->Alloc(alloc_size);
        allocated_size += alloc_size;
        EXPECT_NE(ptr[i],0);
        EXPECT_EQ((int)ptr[i].GetShelfId().GetShelfIndex(),2);
        i++;
    } while(allocated_size < (heap_size - alloc_size)); // Loop until last item.

    ptr[i] = heap->Alloc(alloc_size);
    if(ptr[i]==0)
       i--;

    do{
        heap->Free(ptr[i--]);
    }while(i>=0);
    // destroy the heap
    EXPECT_EQ(NO_ERROR, heap->Close());
    delete heap;
    EXPECT_EQ(NO_ERROR, mm->DestroyHeap(pool_id));
    EXPECT_EQ(ID_NOT_FOUND, mm->DestroyHeap(pool_id));
}

// Resize multiple times in a loop
TEST(EpochZoneHeap, MultipleResize) {
    PoolId pool_id = 1;
    size_t min_alloc_size = 128;
    size_t heap_size = min_alloc_size * 1024 * 1024LLU; // 128 MB
    size_t resize_size = heap_size;
    size_t alloc_size = heap_size/2 ;
    int total_shelfs = 96;
    GlobalPtr ptr[512];
    GlobalPtr ptr_fail;
    int i = 0, j = 0;

    MemoryManager *mm = MemoryManager::GetInstance();
    Heap *heap = NULL;

    // create a heap
    EXPECT_EQ(ID_NOT_FOUND, mm->FindHeap(pool_id, &heap));
    EXPECT_EQ(NO_ERROR, mm->CreateHeap(pool_id, heap_size));
    EXPECT_EQ(ID_FOUND, mm->CreateHeap(pool_id, heap_size));

    // get the heap
    EXPECT_EQ(NO_ERROR, mm->FindHeap(pool_id, &heap));
    EXPECT_EQ(NO_ERROR, heap->Open());
#ifdef LFSWORKAROUND
    total_shelfs = 4;
#endif
    for(i=0;i<total_shelfs;i++) {
       ptr[i] = heap->Alloc(alloc_size);
       EXPECT_NE(ptr[i],0);
       EXPECT_EQ((int)ptr[i].GetShelfId().GetShelfIndex(),i+1);
       ptr_fail = heap->Alloc(alloc_size);
       EXPECT_EQ(ptr_fail,0);
       heap_size += resize_size;
       EXPECT_EQ(heap->Resize(heap_size), NO_ERROR);       
       EXPECT_EQ(heap->Size(), heap_size); 
    }
    ptr[i] = heap->Alloc(alloc_size);
    EXPECT_NE(ptr[i],0);
    EXPECT_EQ((int)ptr[i].GetShelfId().GetShelfIndex(),i+1);
    ptr_fail = heap->Alloc(alloc_size);
    EXPECT_EQ(ptr_fail,0);
    
    heap->Free(ptr[i]);    
    ptr[i] = heap->Alloc(alloc_size);
    EXPECT_NE(ptr[i],0);
    EXPECT_EQ((int)ptr[i].GetShelfId().GetShelfIndex(),i+1);

    for(j=0;j<total_shelfs;j++) {
        heap->Free(ptr[j]);
    }
    // destroy the heap
    EXPECT_EQ(NO_ERROR, heap->Close());
    delete heap;
    EXPECT_EQ(NO_ERROR, mm->DestroyHeap(pool_id));
    EXPECT_EQ(ID_NOT_FOUND, mm->DestroyHeap(pool_id));
#ifdef LFSWORKAROUND
    sleep(10);
#endif
}

#ifndef LFSWORKAROUND
// Resize multiple times in a loop
TEST(EpochZoneHeap, MultipleResizeBoundary) {
    PoolId pool_id = 1;
    size_t min_alloc_size = 128;
    size_t heap_size = min_alloc_size * 1024LLU; // 128 KB
    size_t resize_size = heap_size;
    size_t alloc_size = heap_size/2 ;
    uint32_t total_resize_count = 126;
    GlobalPtr ptr[512];
    GlobalPtr ptr_fail;
    uint32_t i = 0, j = 0;

    MemoryManager *mm = MemoryManager::GetInstance();
    Heap *heap = NULL;

    // create a heap
    EXPECT_EQ(ID_NOT_FOUND, mm->FindHeap(pool_id, &heap));
    EXPECT_EQ(NO_ERROR, mm->CreateHeap(pool_id, heap_size));
    EXPECT_EQ(ID_FOUND, mm->CreateHeap(pool_id, heap_size));

    // get the heap
    EXPECT_EQ(NO_ERROR, mm->FindHeap(pool_id, &heap));
    EXPECT_EQ(NO_ERROR, heap->Open());

    // total_shelfs-1,as we have one shelf already created
    for(i=0;i<total_resize_count;i++) {
       ptr[i] = heap->Alloc(alloc_size);
       EXPECT_NE(ptr[i],0);
       EXPECT_EQ((int)ptr[i].GetShelfId().GetShelfIndex(),i+1);
       ptr_fail = heap->Alloc(alloc_size);
       EXPECT_EQ(ptr_fail,0);
       heap_size += resize_size;
       EXPECT_EQ(heap->Resize(heap_size), NO_ERROR);       
       EXPECT_EQ(heap->Size(), heap_size); 
    }
    ptr[i] = heap->Alloc(alloc_size);
    EXPECT_NE(ptr[i],0);
    EXPECT_EQ((int)ptr[i].GetShelfId().GetShelfIndex(),i+1);
    ptr_fail = heap->Alloc(alloc_size);
    EXPECT_EQ(ptr_fail,0);
    
    heap->Free(ptr[i]);    
    ptr[i] = heap->Alloc(alloc_size);
    EXPECT_NE(ptr[i],0);
    EXPECT_EQ((int)ptr[i].GetShelfId().GetShelfIndex(),i+1);

    for(j=0;j<(total_resize_count + 1);j++) {
        heap->Free(ptr[j]);
    }
    // destroy the heap
    EXPECT_EQ(NO_ERROR, heap->Close());
    delete heap;
    EXPECT_EQ(NO_ERROR, mm->DestroyHeap(pool_id));
    EXPECT_EQ(ID_NOT_FOUND, mm->DestroyHeap(pool_id));
}


// Resize multiple times in a loop - until it fails.
// Failure should be graceful
TEST(EpochZoneHeap, MultipleResizeBoundaryFail) {
    PoolId pool_id = 1;
    size_t min_alloc_size = 128;
    size_t heap_size = min_alloc_size * 1024LLU; // 128 KB
    size_t resize_size = heap_size;
    size_t alloc_size = heap_size/2 ;
    uint32_t total_resize_count = 126;
    GlobalPtr ptr[512];
    GlobalPtr ptr_fail;
    uint32_t i = 0, j = 0;

    MemoryManager *mm = MemoryManager::GetInstance();
    Heap *heap = NULL;

    // create a heap
    EXPECT_EQ(ID_NOT_FOUND, mm->FindHeap(pool_id, &heap));
    EXPECT_EQ(NO_ERROR, mm->CreateHeap(pool_id, heap_size));
    EXPECT_EQ(ID_FOUND, mm->CreateHeap(pool_id, heap_size));

    // get the heap
    EXPECT_EQ(NO_ERROR, mm->FindHeap(pool_id, &heap));
    EXPECT_EQ(NO_ERROR, heap->Open());
    
    // total_shelfs-2,as we have one shelf already created 
    // and one more shelf for header
    for(i=0;i<total_resize_count;i++) {
       ptr[i] = heap->Alloc(alloc_size);
       EXPECT_NE(ptr[i],0);
       EXPECT_EQ((int)ptr[i].GetShelfId().GetShelfIndex(),i+1);
       ptr_fail = heap->Alloc(alloc_size);
       EXPECT_EQ(ptr_fail,0);
       heap_size += resize_size;
       EXPECT_EQ(heap->Resize(heap_size), NO_ERROR);       
       EXPECT_EQ(heap->Size(), heap_size); 
    }
    heap_size += resize_size;
    EXPECT_EQ(heap->Resize(heap_size), HEAP_RESIZE_FAILED);

    for(j=0;j<total_resize_count;j++) {
        heap->Free(ptr[j]);
    }
    // destroy the heap
    EXPECT_EQ(NO_ERROR, heap->Close());
    delete heap;
    EXPECT_EQ(NO_ERROR, mm->DestroyHeap(pool_id));
    EXPECT_EQ(ID_NOT_FOUND, mm->DestroyHeap(pool_id));
}
#endif


// Resize to a smaller size than the current size, it should not do 
// anything and just return success.
TEST(EpochZoneHeap, SmallerResize) {
    PoolId pool_id = 1;

    int min_alloc_size = 128;
    size_t alloc_size = 1024 * 1024LLU;
    size_t heap_size = min_alloc_size * alloc_size; // 128 MB
    size_t new_size = heap_size/2;
    GlobalPtr ptr[512];
    int i = 0;

    MemoryManager *mm = MemoryManager::GetInstance();
    Heap *heap = NULL;

    // create a heap
    EXPECT_EQ(ID_NOT_FOUND, mm->FindHeap(pool_id, &heap));
    EXPECT_EQ(NO_ERROR, mm->CreateHeap(pool_id, heap_size));
    EXPECT_EQ(ID_FOUND, mm->CreateHeap(pool_id, heap_size));

    // get the heap
    EXPECT_EQ(NO_ERROR, mm->FindHeap(pool_id, &heap));
    EXPECT_EQ(NO_ERROR, heap->Open());

    for(i=0;i<min_alloc_size-1;i++) {
       ptr[i] = heap->Alloc(alloc_size);
       EXPECT_NE(ptr[i],0);
       EXPECT_EQ((int)ptr[i].GetShelfId().GetShelfIndex(),1);
    }
    ptr[i] = heap->Alloc(alloc_size);
    EXPECT_EQ(ptr[i],0);
    
    // Since new size is lesser, resize wont do anything
    EXPECT_EQ(NO_ERROR, heap->Resize(new_size));
    std::cout<<"Total heap size= " <<heap->Size()<<std::endl;
    EXPECT_EQ(heap->Size(), heap_size);
    // destroy the heap
    EXPECT_EQ(NO_ERROR, heap->Close());
    delete heap;
    EXPECT_EQ(NO_ERROR, mm->DestroyHeap(pool_id));
    EXPECT_EQ(ID_NOT_FOUND, mm->DestroyHeap(pool_id));
}

// Resize to add a new shelf which is non power of 2,
// EpochZoneHeap creates a shelf of size power of 2.
TEST(EpochZoneHeap, PowerOfTwoResize) {
    PoolId pool_id = 1;

    int min_alloc_size = 128;
    size_t alloc_size = 1024 * 1024LLU;
    size_t heap_size = min_alloc_size * alloc_size; // 128 MB
    size_t new_size = 2 * heap_size - 10;
    GlobalPtr ptr[512];
    int i = 0;

    MemoryManager *mm = MemoryManager::GetInstance();
    Heap *heap = NULL;

    // create a heap
    EXPECT_EQ(ID_NOT_FOUND, mm->FindHeap(pool_id, &heap));
    EXPECT_EQ(NO_ERROR, mm->CreateHeap(pool_id, heap_size));
    EXPECT_EQ(ID_FOUND, mm->CreateHeap(pool_id, heap_size));

    // get the heap
    EXPECT_EQ(NO_ERROR, mm->FindHeap(pool_id, &heap));
    EXPECT_EQ(NO_ERROR, heap->Open());

    for(i=0;i<min_alloc_size-1;i++) {
       ptr[i] = heap->Alloc(alloc_size);
       EXPECT_NE(ptr[i],0);
       EXPECT_EQ((int)ptr[i].GetShelfId().GetShelfIndex(),1);
    }
    ptr[i] = heap->Alloc(alloc_size);
    EXPECT_EQ(ptr[i],0);
    
    // Since new size is lesser, resize wont do anything
    EXPECT_EQ(NO_ERROR, heap->Resize(new_size));
    std::cout<<"Total heap size= " <<heap->Size()<<std::endl;
    EXPECT_EQ(heap->Size(), heap_size * 2);
    // destroy the heap
    EXPECT_EQ(NO_ERROR, heap->Close());
    delete heap;
    EXPECT_EQ(NO_ERROR, mm->DestroyHeap(pool_id));
    EXPECT_EQ(ID_NOT_FOUND, mm->DestroyHeap(pool_id));
}

// Verify the offset allocation method
TEST(EpochZoneHeap, OffsetAllocResize) {
    PoolId pool_id = 1;
    int min_alloc_size = 128;
    size_t alloc_size = 1024 * 1024LLU;
    size_t allocated_size = 0;
    size_t heap_size = min_alloc_size * alloc_size; // 128 MB
    Offset ptr[512];
    int i = 0;

    MemoryManager *mm = MemoryManager::GetInstance();
    Heap *heap = NULL;

    // create a heap
    EXPECT_EQ(ID_NOT_FOUND, mm->FindHeap(pool_id, &heap));
    EXPECT_EQ(NO_ERROR, mm->CreateHeap(pool_id, heap_size));
    EXPECT_EQ(ID_FOUND, mm->CreateHeap(pool_id, heap_size));

    // get the heap
    EXPECT_EQ(NO_ERROR, mm->FindHeap(pool_id, &heap));
    EXPECT_EQ(NO_ERROR, heap->Open());

    // We will be able to allocate (min_alloc_size - 1) objects of alloc_size 
    // in a heap size (min_alloc_size * alloc_size)
    do {
       ptr[i] = heap->AllocOffset(alloc_size);
       allocated_size += alloc_size;
       std::cout <<"ptr = "<<ptr[i]<<std::endl;
       EXPECT_NE(ptr[i],0);
       EXPECT_EQ((int)((GlobalPtr)ptr[i]).GetShelfId().GetShelfIndex(),0);
    } while(allocated_size < (heap_size - alloc_size)); // Loop until last item.
    ptr[i] = heap->Alloc(alloc_size);
    EXPECT_EQ(ptr[i],0);

    heap_size = heap_size * 2;
    EXPECT_EQ(NO_ERROR, heap->Resize(heap_size));
    std::cout<<"Total heap size= " <<heap->Size()<<std::endl;
    EXPECT_EQ(heap->Size(), heap_size);
    allocated_size = heap_size;
    
     do {
       ptr[i] = heap->AllocOffset(alloc_size);
       allocated_size += alloc_size;
       std::cout <<"ptr = "<<ptr[i]<<std::endl;
       EXPECT_NE(ptr[i],0);
       EXPECT_EQ((int)((GlobalPtr)ptr[i]).GetShelfId().GetShelfIndex(),1);
    } while(allocated_size < (heap_size - alloc_size)); // Loop until last item.

    // Free loop
    do{
        heap->Free(ptr[i--]);
    }while(i>=0);
 
    // destroy the heap
    EXPECT_EQ(NO_ERROR, heap->Close());
    delete heap;
    EXPECT_EQ(NO_ERROR, mm->DestroyHeap(pool_id));
    EXPECT_EQ(ID_NOT_FOUND, mm->DestroyHeap(pool_id));
}

// Allocate from heap, Resize from heap1, New space should be available heap1
TEST(EpochZoneHeap, AllocResize) {
    
    PoolId pool_id = 1;
    size_t min_alloc_size = 128;
    size_t heap_size = min_alloc_size * 1024 * 1024LLU; // 128 MB
    size_t allocated_size = 0;
    size_t alloc_size = 1024 * 1024LLU; // 1MB per alloc
    GlobalPtr ptr[512];
    int i = 0;

    MemoryManager *mm = MemoryManager::GetInstance();
    Heap *heap = NULL;

    // create a heap
    EXPECT_EQ(ID_NOT_FOUND, mm->FindHeap(pool_id, &heap));
    EXPECT_EQ(NO_ERROR, mm->CreateHeap(pool_id, heap_size));
    EXPECT_EQ(ID_FOUND, mm->CreateHeap(pool_id, heap_size));

    // get the heap
    EXPECT_EQ(NO_ERROR, mm->FindHeap(pool_id, &heap));
    EXPECT_EQ(NO_ERROR, heap->Open());

    // You should be able allocate (allocated_size - alloc_size) from the heap.
    do {
       ptr[i] = heap->Alloc(alloc_size);
       allocated_size += alloc_size;
       EXPECT_NE(ptr[i],0);       
       EXPECT_EQ((int)ptr[i].GetShelfId().GetShelfIndex(),1);       
       i++;
    } while(allocated_size < (heap_size - alloc_size)); // Loop until last item.

    // Just do one more allocation in case we are able to allocate entire heap
    ptr[i] = heap->Alloc(alloc_size);
    if(ptr[i] != 0)
        i++;
    

    // Open a new heap data structure and resize
    Heap *heap1;
    EXPECT_EQ(NO_ERROR, mm->FindHeap(pool_id, &heap1));    
    EXPECT_EQ(NO_ERROR, heap1->Open());
    heap_size = heap_size * 2;
    EXPECT_EQ(NO_ERROR, heap1->Resize(heap_size));

    // Allocate from new heap data structure
    ptr[i] = heap->Alloc(alloc_size);
    EXPECT_EQ((int)ptr[i].GetShelfId().GetShelfIndex(),2);

    // Both heap and heap1 should report same size
    EXPECT_EQ(heap->Size(),heap1->Size());

    std::cout<<"Allocated , gptr = "<<ptr[i]<<std::endl;

    // Allocated the data items from heap, free it using heap1
    do {
       heap1->Free(ptr[i]);
       i--;
    } while (i>=0);

    EXPECT_EQ(NO_ERROR, heap->Close());
    EXPECT_EQ(NO_ERROR, heap1->Close());

    delete heap;
    delete heap1;
    EXPECT_EQ(NO_ERROR, mm->DestroyHeap(pool_id));

}
// DelayedFree Resize test
TEST(EpochZoneHeap, DelayedFreeResize) {
    PoolId pool_id = 1;
    size_t heap_size = 128 * 1024 * 1024LLU; // 128 MB
    size_t alloc_size = heap_size / 2;

    MemoryManager *mm = MemoryManager::GetInstance();
    EpochManager *em = EpochManager::GetInstance();
    Heap *heap = NULL;

    // create a heap
    EXPECT_EQ(ID_NOT_FOUND, mm->FindHeap(pool_id, &heap));
    EXPECT_EQ(NO_ERROR, mm->CreateHeap(pool_id, heap_size));
    EXPECT_EQ(ID_FOUND, mm->CreateHeap(pool_id, heap_size));

    // get the heap
    EXPECT_EQ(NO_ERROR, mm->FindHeap(pool_id, &heap));
    EXPECT_EQ(NO_ERROR, heap->Open());

    EpochCounter e1;
    GlobalPtr ptr1, ptr3;

    // allocate & delayed free
    {
        EpochOp op(em);
        e1 = op.reported_epoch();
        std::cout << "first epoch " << e1 << std::endl;
        ptr1 = heap->Alloc(op, alloc_size);
        EXPECT_NE(0,ptr1);
        heap->Free(op, ptr1);
        // allocate again, because of delayed free, free will not happen
        // and the alloc will fail.
        GlobalPtr ptr2 = heap->Alloc(op, alloc_size);
        EXPECT_EQ(0, ptr2);

        // First shelf is full, Resize and create one more shelf
        heap->Resize(heap_size * 2);

        // Alloc from next shelf
        ptr3 = heap->Alloc(op, alloc_size);
        EXPECT_NE(0,ptr3); 
        heap->Free(op, ptr3);
        // allocate again, because of delayed free, free will not happen
        // and the alloc will fail.
        ptr2 = heap->Alloc(op, alloc_size);
        EXPECT_EQ(0, ptr2);
    }

    // wait a few epoches and make sure the background thread picks up this
    // chunk and frees it
    EpochCounter e2;
    while (1) {
        {
            // Begin epoch in a new scope block so that we exit the epoch when
            // we out of scope and don't block others when we then sleep.
            {
                EpochOp op(em);
                e2 = op.reported_epoch();
            }
            if (e2 - e1 >= 3 && e2 % 5 == (e1 + 3) % 5) {
                std::cout << "sleeping at epoch " << e2 << std::endl;
                sleep(2); // making sure the background thread wakes up in this
                          // epoch
                break;
            }
        }
    }

    while (1) {
        {
            EpochOp op(em);
            EpochCounter e3 = op.reported_epoch();
            if (e3 > e2) {
                break;
            }
        }
    }

    // now the ptr that was delayed freed must have been actually freed
    {
        EpochOp op(em);
        std::cout << "final epoch " << op.reported_epoch() << std::endl;
        // Alloc should from first shelf. Verify.
        GlobalPtr ptr2 = heap->Alloc(op, alloc_size);
        EXPECT_EQ(ptr1, ptr2);

        // Alloc should from next shelf. Verify.
        GlobalPtr ptr4 = heap->Alloc(op, alloc_size);
        EXPECT_EQ(ptr3, ptr4);        
        heap->Free(ptr2);
        heap->Free(ptr4);
    }

    // destroy the heap
    EXPECT_EQ(NO_ERROR, heap->Close());
    delete heap;
    EXPECT_EQ(NO_ERROR, mm->DestroyHeap(pool_id));
    EXPECT_EQ(ID_NOT_FOUND, mm->DestroyHeap(pool_id));
}

#ifndef LFSWORKAROUND
// Multiple DelayedFree Resize test
TEST(EpochZoneHeap, MultipleDelayedFreeResize) {
    PoolId pool_id = 1;

    size_t shelf_size = 128 * 1024 * 1024LLU; // 128 MB
    size_t heap_size = shelf_size;
    size_t alloc_size = heap_size / 4;
    const int total_shelf = 16;
    int total_allocs;

    MemoryManager *mm = MemoryManager::GetInstance();
    EpochManager *em = EpochManager::GetInstance();
    Heap *heap = NULL;


    // create a heap
    EXPECT_EQ(ID_NOT_FOUND, mm->FindHeap(pool_id, &heap));
    EXPECT_EQ(NO_ERROR, mm->CreateHeap(pool_id, heap_size));
    EXPECT_EQ(ID_FOUND, mm->CreateHeap(pool_id, heap_size));

    // get the heap
    EXPECT_EQ(NO_ERROR, mm->FindHeap(pool_id, &heap));
    EXPECT_EQ(NO_ERROR, heap->Open());

    EpochCounter e1;
    GlobalPtr ptr;
    std::cout<<"Going to resize the heap"<<std::endl;
    for (int i = 0;i<total_shelf; i++) {
        heap_size += shelf_size;
        EXPECT_EQ(NO_ERROR,heap->Resize(heap_size));
        std::cout<<"Resize: "<<i<<" Done" <<std::endl;
    }

    // allocate & delayed free
    {
        EpochOp op(em);
        e1 = op.reported_epoch();
        std::cout << "first epoch " << e1 << std::endl;
        total_allocs = 0;
        while (1){
            ptr = heap->Alloc(op, alloc_size);
            if(ptr == 0) 
               break;
            total_allocs++;
            heap->Free(op, ptr);
        }
        EXPECT_GE (total_allocs, total_shelf * 3);        

        // allocate again, because of delayed free, free will not happen
        // and the alloc will fail.
        ptr = heap->Alloc(op, alloc_size);
        EXPECT_EQ(0, ptr);
    }
     
    // wait a few epoches and make sure the background thread picks up this
    // chunk and frees it
    EpochCounter e2;
    while (1) {
        {
            // Begin epoch in a new scope block so that we exit the epoch when
            // we out of scope and don't block others when we then sleep.
            {
                EpochOp op(em);
                e2 = op.reported_epoch();
            }
            if (e2 - e1 >= 3 && e2 % 5 == (e1 + 3) % 5) {
                std::cout << "sleeping at epoch " << e2 << std::endl;
                sleep(2); // making sure the background thread wakes up in this
                          // epoch
                break;
            }
        }
    }

    while (1) {
        {
            EpochOp op(em);
            EpochCounter e3 = op.reported_epoch();
            if (e3 > e2) {
                break;
            }
        }
    }

    // now the ptr that was delayed freed must have been actually freed
    {
        EpochOp op(em);
        std::cout << "final epoch " << op.reported_epoch() << std::endl;
        // Alloc should from first shelf. Verify.
        for(int i = 0;i < total_allocs;i++) {
            GlobalPtr ptr = heap->Alloc(op, alloc_size);
            EXPECT_NE(ptr,0);
            heap->Free(op, ptr);
        }
        // This allocate should fail
        ptr = heap->Alloc(op, alloc_size);
        EXPECT_EQ(0, ptr);
    }

    // destroy the heap
    EXPECT_EQ(NO_ERROR, heap->Close());
    delete heap;
    EXPECT_EQ(NO_ERROR, mm->DestroyHeap(pool_id));
    EXPECT_EQ(ID_NOT_FOUND, mm->DestroyHeap(pool_id));    
    
}


// Multiple DelayedFree Resize test - Close test
TEST(EpochZoneHeap, MultipleDelayedFreeResizeClose) {
    PoolId pool_id = 1;

    size_t shelf_size = 128 * 1024 * 1024LLU; // 128 MB
    size_t heap_size = shelf_size;
    size_t alloc_size = heap_size / 4;
    const int total_shelf = 16;
    int total_allocs;

    MemoryManager *mm = MemoryManager::GetInstance();
    EpochManager *em = EpochManager::GetInstance();
    Heap *heap = NULL;


    // create a heap
    EXPECT_EQ(ID_NOT_FOUND, mm->FindHeap(pool_id, &heap));
    EXPECT_EQ(NO_ERROR, mm->CreateHeap(pool_id, heap_size));
    EXPECT_EQ(ID_FOUND, mm->CreateHeap(pool_id, heap_size));

    // get the heap
    EXPECT_EQ(NO_ERROR, mm->FindHeap(pool_id, &heap));
    EXPECT_EQ(NO_ERROR, heap->Open());

    EpochCounter e1;
    GlobalPtr ptr;
    std::cout<<"Going to resize the heap"<<std::endl;
    for (int i = 0;i<total_shelf; i++) {
        heap_size += shelf_size;
        EXPECT_EQ(NO_ERROR,heap->Resize(heap_size));
        std::cout<<"Resize: "<<i<<" Done" <<std::endl;
    }

    // allocate & delayed free
    {
        EpochOp op(em);
        e1 = op.reported_epoch();
        std::cout << "first epoch " << e1 << std::endl;
        total_allocs = 0;
        while (1){
            ptr = heap->Alloc(op, alloc_size);
            if(ptr == 0) 
               break;
            total_allocs++;
            heap->Free(op, ptr);
        }
        EXPECT_GE (total_allocs, total_shelf * 3);        

        // allocate again, because of delayed free, free will not happen
        // and the alloc will fail.
        ptr = heap->Alloc(op, alloc_size);
        EXPECT_EQ(0, ptr);
    }
    // Close the heap
    EXPECT_EQ(NO_ERROR, heap->Close());
    // Open the heap
    EXPECT_EQ(NO_ERROR, heap->Open()); 

    // wait a few epoches and make sure the background thread picks up this
    // chunk and frees it
    EpochCounter e2;
    while (1) {
        {
            // Begin epoch in a new scope block so that we exit the epoch when
            // we out of scope and don't block others when we then sleep.
            {
                EpochOp op(em);
                e2 = op.reported_epoch();
            }
            if (e2 - e1 >= 3 && e2 % 5 == (e1 + 3) % 5) {
                std::cout << "sleeping at epoch " << e2 << std::endl;
                sleep(2); // making sure the background thread wakes up in this
                          // epoch
                break;
            }
        }
    }

    while (1) {
        {
            EpochOp op(em);
            EpochCounter e3 = op.reported_epoch();
            if (e3 > e2) {
                break;
            }
        }
    }

    // now the ptr that was delayed freed must have been actually freed
    {
        EpochOp op(em);
        std::cout << "final epoch " << op.reported_epoch() << std::endl;
        // Alloc should from first shelf. Verify.
        for(int i = 0;i < total_allocs;i++) {
            GlobalPtr ptr = heap->Alloc(op, alloc_size);
            EXPECT_NE(ptr,0);
            heap->Free(op, ptr);
        }
        // This allocate should fail
        ptr = heap->Alloc(op, alloc_size);
        EXPECT_EQ(0, ptr);
    }

    // destroy the heap
    EXPECT_EQ(NO_ERROR, heap->Close());
    delete heap;
    EXPECT_EQ(NO_ERROR, mm->DestroyHeap(pool_id));
    EXPECT_EQ(ID_NOT_FOUND, mm->DestroyHeap(pool_id));    
    
}
#endif
// 

TEST(EpochZoneHeap, Permissions) {
    PoolId pool_id = 1;
    size_t size = 128 * 1024 * 1024LLU; // 128 MB

    MemoryManager *mm = MemoryManager::GetInstance();
    Heap *heap = NULL;

    // create a heap
    EXPECT_EQ(ID_NOT_FOUND, mm->FindHeap(pool_id, &heap));
    EXPECT_EQ(NO_ERROR, mm->CreateHeap(pool_id, size, 128, S_IRUSR | S_IWUSR | S_IRGRP ));
    EXPECT_EQ(ID_FOUND, mm->CreateHeap(pool_id, size));

    EXPECT_EQ(NO_ERROR, mm->FindHeap(pool_id, &heap));
    EXPECT_EQ(NO_ERROR, heap->Open());

    mode_t mode;
    EXPECT_EQ(NO_ERROR, heap->GetPermission(&mode));
    EXPECT_NE(0, mode & S_IRGRP);
    EXPECT_EQ(0, mode & S_IWGRP);

    EXPECT_EQ(NO_ERROR, heap->SetPermission(mode | S_IWGRP));
    EXPECT_EQ(NO_ERROR, heap->GetPermission(&mode));
    EXPECT_NE(0, mode & S_IWGRP);

    EXPECT_EQ(NO_ERROR,heap->Resize(size * 2));    

    EXPECT_EQ(NO_ERROR, heap->SetPermission(S_IRUSR | S_IWUSR));
    EXPECT_EQ(NO_ERROR, heap->GetPermission(&mode));
    EXPECT_EQ(0, mode & (S_IRGRP | S_IWGRP));

    EXPECT_EQ(NO_ERROR, heap->Close());
    delete heap;
    EXPECT_EQ(NO_ERROR, mm->DestroyHeap(pool_id));
    EXPECT_EQ(ID_NOT_FOUND, mm->DestroyHeap(pool_id));

}

// merge
TEST(EpochZoneHeap, Merge) {
    PoolId pool_id = 1;
    size_t size = 128 * 1024 * 1024LLU; // 128 MB

    MemoryManager *mm = MemoryManager::GetInstance();
    Heap *heap = NULL;

    // create a heap
    EXPECT_EQ(ID_NOT_FOUND, mm->FindHeap(pool_id, &heap));
    EXPECT_EQ(NO_ERROR, mm->CreateHeap(pool_id, size));
    EXPECT_EQ(ID_FOUND, mm->CreateHeap(pool_id, size));

    // get the heap
    EXPECT_EQ(NO_ERROR, mm->FindHeap(pool_id, &heap));
    EXPECT_EQ(NO_ERROR, heap->Open());

    // in unit of 64-byte:
    // [0, 8) has been allocated to the header
    // [4096, 8192) has been allocated to the merge bitmap

    uint64_t min_obj_size = heap->MinAllocSize();

    // merge at levels < max_zone_level-2
    // allocate 64 byte x 24, covering [8, 32)
    GlobalPtr ptr[24];
    GlobalPtr new_ptr;
#if 0
    for(int i=0; i<24; i++) {
        ptr[i]= heap->Alloc(min_obj_size);
    }
    // free 64 byte x 24
    for(int i=0; i<24; i++) {
        heap->Free(ptr[i]);
    }

    // before merge, allocate 1024 bytes
    new_ptr = heap->Alloc(16*min_obj_size);
    EXPECT_EQ(32*min_obj_size, new_ptr.GetOffset());

    // merge
    heap->Merge();

    // after merge, allocate 1024 bytes
    new_ptr = heap->Alloc(16*min_obj_size);
    EXPECT_EQ(16*min_obj_size, new_ptr.GetOffset());
#endif

    // merge at the last 3 levels
    // allocate 16MB x 7
    for (int i = 0; i < 7; i++) {
        ptr[i] = heap->Alloc(262144 * min_obj_size);
    }
    // free 16MB x 7
    for (int i = 0; i < 7; i++) {
        heap->Free(ptr[i]);
    }

    // before merge, allocate 64MB
    new_ptr = heap->Alloc(1048576 * min_obj_size);
    EXPECT_EQ(0UL, new_ptr.GetOffset());

    // merge
    heap->Merge();

    // after merge, allocate 64MB
    new_ptr = heap->Alloc(1048576 * min_obj_size);
    EXPECT_EQ(1048576 * min_obj_size, new_ptr.GetOffset());

    // destroy the heap
    EXPECT_EQ(NO_ERROR, heap->Close());
    delete heap;
    EXPECT_EQ(NO_ERROR, mm->DestroyHeap(pool_id));
    EXPECT_EQ(ID_NOT_FOUND, mm->DestroyHeap(pool_id));
}

// Test large pool id
TEST(EpochZoneHeap, LargePoolId) {
    MemoryManager *mm = MemoryManager::GetInstance();
    PoolId pool_id = 1;
    for (PoolId i = 10; i <= 14; i++) {
        pool_id = PoolId(1 << i);
        if (i == 14)
            pool_id = PoolId((1 << i) - 1);
        std::cout << "Creating heap with pool id=" << pool_id << "\n";
        size_t size = 128 * 1024 * 1024; // 128MB
#ifdef LFSWORKAROUND
        sleep(10);
#endif
        ErrorCode ret = mm->CreateHeap(pool_id, size);
        assert(ret == NO_ERROR);

        // acquire the heap
        Heap *heap = NULL;
        ret = mm->FindHeap(pool_id, &heap);
        assert(ret == NO_ERROR);

        // open the heap
        ret = heap->Open();
        assert(ret == NO_ERROR);

        // use the heap
        GlobalPtr ptr = heap->Alloc(
            sizeof(int)); // Alloc returns a GlobalPtr consisting of a shelf ID
                          // and offset
        assert(ptr.IsValid() == true);
        int *int_ptr = (int *)mm->GlobalToLocal(
            ptr); // convert the GlobalPtr into a local pointer
        *int_ptr = 123;
        assert(*int_ptr == 123);

        heap->Free(ptr);
        // close the heap
        ret = heap->Close();
        assert(ret == NO_ERROR);

        // release the heap
        delete heap;

        // delete the heap
        ret = mm->DestroyHeap(pool_id);
        assert(ret == NO_ERROR);
    }
}

// Larger data item size
TEST(EpochZoneHeap, Largeallocsize) {
    PoolId pool_id = 1;
    size_t size = 128 * 1024 * 1024LLU; // 128 MB

    MemoryManager *mm = MemoryManager::GetInstance();
    Heap *heap = NULL;

    // create a heap
    EXPECT_EQ(ID_NOT_FOUND, mm->FindHeap(pool_id, &heap));
    EXPECT_EQ(NO_ERROR, mm->CreateHeap(pool_id, size,512));
    EXPECT_EQ(ID_FOUND, mm->CreateHeap(pool_id, size,512));

    // get the heap
    EXPECT_EQ(NO_ERROR, mm->FindHeap(pool_id, &heap));
    EXPECT_EQ(NO_ERROR, heap->Open());

    // allocate & free
    GlobalPtr ptr = heap->Alloc(sizeof(int));   

    // allocate again, This offset should 512 + ptr
    GlobalPtr ptr1 = heap->Alloc(sizeof(int));
   
    std::cout<<"ptr :"<<ptr.GetOffset()<<std::endl;
    std::cout<<"ptr1 :"<<ptr1.GetOffset()<<std::endl;
    EXPECT_EQ(ptr.GetOffset()+512, ptr1.GetOffset());

    heap->Free(ptr);
    heap->Free(ptr1);

    // destroy the heap
    EXPECT_EQ(NO_ERROR, heap->Close());
    delete heap;
    EXPECT_EQ(NO_ERROR, mm->DestroyHeap(pool_id));
    EXPECT_EQ(ID_NOT_FOUND, mm->DestroyHeap(pool_id));
}

void AllocFree(Heap *heap, int cnt) {
    std::cout << "Thread " << std::this_thread::get_id() << " started"
              << std::endl;
    std::list<GlobalPtr> ptrs;
    for (int i = 0; i < cnt; i++) {
        if (rand_uint64(0, 1) == 1) {
            GlobalPtr ptr = heap->Alloc(rand_uint64(0, 1024 * 1024));
            if (ptr)
                ptrs.push_back(ptr);
        } else {
            if (!ptrs.empty()) {
                GlobalPtr ptr = ptrs.front();
                ptrs.pop_front();
                heap->Free(ptr);
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    for (auto ptr : ptrs) {
        heap->Free(ptr);
    }
    std::cout << "Thread " << std::this_thread::get_id() << " ended"
              << std::endl;
}

// merge and concurrent alloc and free
TEST(EpochZoneHeap, MergeAllocFree) {
    PoolId pool_id = 1;
    size_t size = 1024 * 1024 * 1024LLU; // 1024 MB
    int thread_cnt = 16;
    int loop_cnt = 1000;

    MemoryManager *mm = MemoryManager::GetInstance();
    Heap *heap = NULL;

    // create a heap
    EXPECT_EQ(ID_NOT_FOUND, mm->FindHeap(pool_id, &heap));
    EXPECT_EQ(NO_ERROR, mm->CreateHeap(pool_id, size));
    EXPECT_EQ(ID_FOUND, mm->CreateHeap(pool_id, size));

    // get the heap
    EXPECT_EQ(NO_ERROR, mm->FindHeap(pool_id, &heap));
    EXPECT_EQ(NO_ERROR, heap->Open());

    // start the threads
    std::vector<std::thread> workers;
    for (int i = 0; i < thread_cnt; i++) {
        workers.push_back(std::thread(AllocFree, heap, loop_cnt));
    }

    for (int i = 0; i < 5; i++) {
        heap->Merge();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    for (auto &worker : workers) {
        if (worker.joinable())
            worker.join();
    }

    heap->Merge();

    // destroy the heap
    EXPECT_EQ(NO_ERROR, heap->Close());
    delete heap;
    EXPECT_EQ(NO_ERROR, mm->DestroyHeap(pool_id));
    EXPECT_EQ(ID_NOT_FOUND, mm->DestroyHeap(pool_id));
}

int main(int argc, char **argv) {
    InitTest(nvmm::trace, false);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
