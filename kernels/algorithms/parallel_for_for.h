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

#include "parallel_for.h"

namespace embree
{
  template<typename ArrayArray, typename Func>
    __forceinline void sequential_for_for( ArrayArray& array2, const size_t minStepSize, const Func& func ) 
  {
    size_t k=0;
    for (size_t i=0; i!=array2.size(); ++i) {
      const size_t N = array2[i]->size();
      if (N) func(array2[i],range<size_t>(0,N),k);
      k+=N;
    }
  }

  class ParallelForForState
  {
  protected:

    ParallelForForState ()
      : K(0), M(0), sizes(NULL), prefix_sum(NULL) {}

    template<typename ArrayArray>
      __forceinline void init (ArrayArray& array2)
    {
      /* compute prefix sum of number of elements of sub arrays */
      size_t sum=0;
      for (size_t i=0; i<M; i++) 
      {
        const size_t N = array2[i] ? array2[i]->size() : 0;
        prefix_sum[i] = sum;
        sizes[i] = N;
        sum += N;
      }
      K = sum;
    }

  public:

    __forceinline size_t size() const {
      return K;
    }

    __forceinline void start_indices(const size_t k0, size_t& i0, size_t& j0) const
    {
      auto iter = std::upper_bound(&prefix_sum[0], &prefix_sum[M], k0);
      i0 = iter-&prefix_sum[0]-1;
      j0 = k0-prefix_sum[i0];
    }
    
  public: // FIXME: make private
    size_t* sizes;
    size_t* prefix_sum;
    size_t K;
    size_t M;
  };

  class ParallelForForStackState : public ParallelForForState
  {
  public:

    template<typename ArrayArray>
      __forceinline ParallelForForStackState ( ArrayArray& array2 )
    {
      M = array2.size();
      prefix_sum = (size_t*) alloca(M*sizeof(size_t)); // FIXME: is alloca safe here when function has __forceinline
      sizes = (size_t*) alloca(M*sizeof(size_t));
      ParallelForForState::init(array2);
    }

    ~ParallelForForStackState() {
    }
  };

  class ParallelForForHeapState : public ParallelForForState
  {
  public:

    __forceinline ParallelForForHeapState () {}

    template<typename ArrayArray>
      __forceinline ParallelForForHeapState ( ArrayArray& array2 )
    {
      init(array2);
    }

    template<typename ArrayArray>
      __forceinline void init( ArrayArray& array2 )
    {
      if (M != array2.size()) 
      {
        delete[] prefix_sum;
        delete[] sizes;
        M = array2.size();
        prefix_sum = new size_t[M];
        sizes = new size_t[M];
      }
      ParallelForForState::init(array2);
    }

    ~ParallelForForHeapState() {
      delete[] prefix_sum;
      delete[] sizes;
    }
  };

  template<typename ArrayArray, typename Func>
    __forceinline void parallel_for_for( ArrayArray& array2, const size_t minStepSize, const Func& f)
  {
    ParallelForForStackState state(array2);

    /* fast path for small number of iterations */
    size_t N = state.size();
    size_t taskCount = (N+minStepSize-1)/minStepSize;
    if (taskCount == 1) 
      return sequential_for_for(array2,minStepSize,f);

    taskCount = min(taskCount,LockStepTaskScheduler::instance()->getNumThreads());
    
    /* parallel invokation of all tasks */
    parallel_for(taskCount, [&](const size_t taskIndex) 
    {
      /* calculate range */
      const size_t k0 = (taskIndex+0)*N/taskCount;
      const size_t k1 = (taskIndex+1)*N/taskCount;
      size_t i0, j0; state.start_indices(k0,i0,j0);

      /* iterate over arrays */
      size_t k=k0;
      for (size_t i=i0; k<k1; i++) {
        const size_t N = state.sizes[i];
        const size_t r0 = j0, r1 = min(N,r0+k1-k);
        if (r1 > r0) f(array2[i],range<size_t>(r0,r1),k);
        k+=r1-r0; j0 = 0;
      }
    });
  }

  template<typename ArrayArray, typename Func>
    __forceinline void parallel_for_for( ArrayArray& array2, const Func& f)
  {
    parallel_for_for(array2,1,f);
  }
}