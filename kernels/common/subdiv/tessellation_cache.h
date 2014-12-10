// ======================================================================== //
// Copyright 2009-2014 Intel Corporation                                    //
//                                                                          //
// Licensed under the Apache License, Version 2.0 (the "License");          //
// you may not use this file except in compliance with the License.         //
// You may obtain a copy of the License at                                  //
//                                                                          //
//     http://www.apache.org/licenses/LICENSE-2.0                           //
//                                                                          //
// Unless required by applicable law or agreed to in writing, software      //
// distributed under the License is distributed on an "AS IS" BASIS,        //
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. //
// See the License for the specific language governing permissions and      //
// limitations under the License.                                           //
// ======================================================================== //

#pragma once

#include "common/default.h"

#if DEBUG
#define CACHE_STATS(x) 
#else
#define CACHE_STATS(x) 
#endif

namespace embree
{
  // FIXME: implement 4 or 8 way associative cache on Xeon using ssei or avxi
  
  class __aligned(64) TessellationCache {
  private:
    /* default sizes */
    static const size_t DEFAULT_64B_BLOCKS = (1<<14);
    static const size_t CACHE_ENTRIES      = DEFAULT_64B_BLOCKS / 4;
    
  public:

    static const size_t CACHE_MISS = (size_t)-1;

#if defined(__MIC__)
    typedef unsigned int InputTagType;
#else
    typedef size_t InputTagType;
#endif

    struct CacheTag {
    private:
      unsigned int prim_tag;
      unsigned int commit_tag;
      unsigned int usedBlocks;
      unsigned int subtree_root;     


    public:

      __forceinline unsigned int toTag(InputTagType prim)
      {
#if defined(__MIC__)
	return prim;
#else
        return ((size_t)prim) >> 6;
#endif
      }

      __forceinline void reset() 
      {
        assert(sizeof(CacheTag) == 16);
        prim_tag     = (unsigned int)-1;
        commit_tag   = (unsigned int)-1;
        subtree_root = (unsigned int)-1;
        usedBlocks   = 0;
      }

      __forceinline bool match(InputTagType primID, const unsigned int commitCounter)
      {
        return prim_tag == toTag(primID) && commit_tag == commitCounter;
      }

      __forceinline void set(InputTagType primID, 
                             const unsigned int commitCounter,
                             const unsigned int root32bit,
                             const unsigned int blocks)
      {
        prim_tag     = toTag(primID);
        commit_tag   = commitCounter;
        subtree_root = root32bit;
        usedBlocks   = blocks;
      }

      __forceinline void update(InputTagType primID, 
                                const unsigned int commitCounter)
      {
        prim_tag   = toTag(primID);
        commit_tag = commitCounter;
      }

      __forceinline void updateRootRef(const unsigned int root32bit)
      {
        subtree_root = root32bit;
      }

      __forceinline unsigned int getRootRef() const
      {
        return subtree_root;
      }

      __forceinline void clearRootRefBits()
      {
#if defined(__MIC__)
        /* bvh4i currently requires a different 'reset' */
        // FIXME
	if (subtree_root & ((unsigned int)1<<3))
	    subtree_root >>= 4;
	  else
	    subtree_root >>= 4+1;
#else
        subtree_root &= ~(((unsigned int)1 << 4)-1);
#endif
      }

      __forceinline unsigned int blocks() const
      {
        return usedBlocks;
      }
      
    };

  private:
    CacheTag tags[CACHE_ENTRIES];


    /* allocated memory to store cache entries */
    float *lazymem;

    /* total number of allocated 64bytes blocks */
    size_t allocated64BytesBlocks;

    /* current number of allocated blocks */
    size_t blockCounter;


    /* stats */
    CACHE_STATS(
                static AtomicCounter cache_accesses;
                static AtomicCounter cache_hits;
                static AtomicCounter cache_misses;
                static AtomicCounter cache_clears;
                static AtomicCounter cache_evictions;                
                );
                

    /* alloc cache memory */
    __forceinline float *alloc_mem(const size_t blocks)
    {
      return (float*)_mm_malloc(64 * blocks,64);
    }

    /* free cache memory */
    __forceinline void free_mem(void *mem)
    {
      assert(mem);
      _mm_free(mem);
    }
    
    __forceinline unsigned int addrToCacheIndex(InputTagType primID)
    {
#if defined(__MIC__)
      return primID % CACHE_ENTRIES;
#else
      return (((size_t)primID)>>6) % CACHE_ENTRIES;
#endif
    }

    /* reset cache */
    __forceinline void clear()
    {
      CACHE_STATS(cache_clears++);
      blockCounter =  0;	  
      for (size_t i=0;i<CACHE_ENTRIES;i++)
        tags[i].reset();
    }

  public:


    __forceinline float *getPtr() const
    {
      return lazymem;
    }


    TessellationCache()  
      {
      }

    /* initialize cache */
    void init()
    {

      clear();
      allocated64BytesBlocks = DEFAULT_64B_BLOCKS;	
      lazymem = alloc_mem( allocated64BytesBlocks );
      assert((size_t)lazymem % 64 == 0);
    }

    __forceinline unsigned int allocated64ByteBlocks() 
    {
      return allocated64BytesBlocks;
    }


    /* lookup cache entry using 64bit pointer as tag */
    __forceinline size_t lookup(InputTagType primID, const unsigned int commitCounter)
    {
      CACHE_STATS(cache_accesses++);
      
      /* direct mapped */
      const unsigned int index = addrToCacheIndex(primID);
      if (likely(tags[index].match(primID,commitCounter)))
        {
          CACHE_STATS(cache_hits++);
#if defined(__MIC__)
          return tags[index].getRootRef();
#else
          return (size_t)getPtr() + tags[index].getRootRef();
#endif
        }
      CACHE_STATS(cache_misses++);
      return CACHE_MISS;
    }

    /* insert entry using 'neededBlocks' cachelines into cache */
    __forceinline CacheTag &request(InputTagType primID, 
                                    const unsigned int commitCounter, 
                                    const size_t neededBlocks)
    {
      const unsigned int index = addrToCacheIndex(primID);
      assert(!tags[index].match(primID,commitCounter));
      if (likely(tags[index].blocks() >= neededBlocks))
        {
	  //DBG_PRINT("EVICT");
          CACHE_STATS(cache_evictions++);
          tags[index].update(primID,commitCounter);
          tags[index].clearRootRefBits();
          return tags[index];
        }

      /* not enough space to hold entry? */
      if (unlikely(blockCounter + neededBlocks >= allocated64BytesBlocks))
        {   
	  //DBG_PRINT("RESIZE");
          /* can the cache hold this subtree space at all in each cache entries? */
#define BIG_CACHE_ENTRIES 16
          if (unlikely(BIG_CACHE_ENTRIES*neededBlocks > allocated64BytesBlocks)) 
            {
              const unsigned int new_allocated64BytesBlocks = BIG_CACHE_ENTRIES*neededBlocks;
#if DEBUG
              std::cout << "EXTENDING TESSELLATION CACHE (PER THREAD) FROM " 
                        << allocated64BytesBlocks << " TO " 
                        << new_allocated64BytesBlocks << " BLOCKS = " 
                        << new_allocated64BytesBlocks*64 << " BYTES" << std::endl << std::flush;
#endif
              free_mem(lazymem);
              allocated64BytesBlocks = new_allocated64BytesBlocks; 
              lazymem = alloc_mem(allocated64BytesBlocks);
              assert(lazymem);

            }
          clear();
        }

      //DBG_PRINT("NEW ALLOC");
	  
      /* allocate entry */
      const size_t currentIndex = blockCounter;
      blockCounter += neededBlocks;

      assert(blockCounter < allocated64BytesBlocks);

#if defined(__MIC__)
      unsigned int curNode = currentIndex;
#else
      size_t curNode = (size_t)&lazymem[currentIndex*16] - (size_t)getPtr(); //(size_t)&lazymem[currentIndex*16];
#endif
      /* insert new entry at the beginning */
      tags[index].set(primID,commitCounter,curNode,neededBlocks);
      return tags[index];     
    }

    __forceinline char *getCacheMemoryPtr(const CacheTag &t) const
    {
      return (char*)((char*)getPtr() + t.getRootRef());
    }

#if defined(__MIC__)
    __forceinline void updateRootRef(CacheTag &t, unsigned int new_root)
    {
      t.updateRootRef( new_root );      
    }
#else
    __forceinline void updateRootRef(CacheTag &t, size_t new_root)
    {
      t.updateRootRef( new_root - (size_t)getPtr() );      
    }
#endif    
    
    /* print stats for debugging */                 
    static void printStats();
    static void clearStats();

  };

};