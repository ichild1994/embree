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

#include "bvh4i_intersector16_subdiv.h"
#include "bvh4i_leaf_intersector.h"
#include "geometry/subdivpatch1.h"

namespace embree
{
  extern size_t g_subdivision_level;

  static AtomicCounter numLazyBuildPatches = 0;

  static AtomicMutex mtx;
  std::vector<unsigned int> patchids;



  namespace isa
  {

    static __aligned(64) RegularGridLookUpTables gridLookUpTables;

    __forceinline void createSubPatchBVH4iLeaf(BVH4i::NodeRef &ref,
					       const unsigned int patchIndex,
					       const unsigned int subdiv_level) 
    {
      *(volatile unsigned int*)&ref = (patchIndex << BVH4i::encodingBits) | BVH4i::leaf_mask | BVH4i::aux_flag_mask | subdiv_level;
    }

    BBox3fa createSubTree(BVH4i::NodeRef &curNode,
			  BVH4i *bvh,
			  BVH4i::Node  * __restrict__ nodes,
			  const SubdivPatch1 &patch,
			  const unsigned int u_start,
			  const unsigned int u_end,
			  const unsigned int v_start,
			  const unsigned int v_end,
			  const unsigned int subdiv_level)
    {
      if (u_end-u_start <= 1)
	{
	  assert(u_end-u_start==1);
	  assert(v_end-v_start==1);
	  //const float inv_grid_size = 1.0f / (float)grid_size;
	  // const float u0 = (float)u_start * inv_grid_size;
	  // const float u1 = (float)u_end   * inv_grid_size;
	  // const float v0 = (float)v_start * inv_grid_size;
	  // const float v1 = (float)v_end   * inv_grid_size;

	  const float u0 = gridLookUpTables.lookUp(subdiv_level,u_start);
	  const float u1 = gridLookUpTables.lookUp(subdiv_level,u_end);
	  const float v0 = gridLookUpTables.lookUp(subdiv_level,v_start);
	  const float v1 = gridLookUpTables.lookUp(subdiv_level,v_end);

	  // DBG_PRINT( u0 );
	  // DBG_PRINT( u1 );
	  // DBG_PRINT( v0 );
	  // DBG_PRINT( v1 );

	  BBox3fa quadBounds = patch.evalQuadBounds(u0,u1,v0,v1);
	  const unsigned int data = (v_start << 8) | u_start;
	  createSubPatchBVH4iLeaf( curNode, data, 0);

	  assert( curNode.isAuxFlagSet() );
	  return quadBounds;
	}

      /* allocate new bvh4i node */
      const size_t num64BytesBlocksPerNode = 2;
      const size_t currentIndex = bvh->used64BytesBlocks.add(num64BytesBlocksPerNode);

      if (currentIndex + num64BytesBlocksPerNode >= bvh->numAllocated64BytesBlocks)
	{
	  FATAL("not enough bvh node space allocated");
	}

      DBG_PRINT(currentIndex);

      createBVH4iNode<2>(curNode,currentIndex);

      BVH4i::Node &node = *(BVH4i::Node*)curNode.node(nodes);

      node.setInvalid();

      const unsigned int u_mid = (u_start+u_end)/2;
      const unsigned int v_mid = (v_start+v_end)/2;

      /* create four subtrees */

      BBox3fa bounds0 = createSubTree( node.child(0), bvh, nodes, patch, u_start, u_mid, v_start, v_mid,  subdiv_level);
      node.setBounds(0, bounds0);

      BBox3fa bounds1 = createSubTree( node.child(1), bvh, nodes, patch, u_mid, u_end, v_start, v_mid,  subdiv_level);
      node.setBounds(1, bounds1);

      BBox3fa bounds2 = createSubTree( node.child(2), bvh, nodes, patch, u_mid, u_end, v_mid, v_end,  subdiv_level);
      node.setBounds(2, bounds2);

      BBox3fa bounds3 = createSubTree( node.child(3), bvh, nodes, patch, u_start, u_mid, v_mid, v_end,  subdiv_level);
      node.setBounds(3, bounds3);

      BBox3fa bounds( empty );
      bounds.extend( bounds0 );
      bounds.extend( bounds1 );
      bounds.extend( bounds2 );
      bounds.extend( bounds3 );

      return bounds;
    }

    BVH4i::NodeRef initLazySubdivTree(SubdivPatch1 &subdiv_patch,
				      BVH4i *bvh,
				      BVH4i::Node  * __restrict__ nodes,
				      const unsigned int subdiv_level)
    {
      const unsigned int build_state = atomic_add((atomic_t*)&subdiv_patch.under_construction,+1);
      /* parent ptr */

      // BVH4i::NodeRef parent_ref = subdiv_patch.bvh4i_parent_ref;
      // BVH4i::Node *  parent_ptr = (BVH4i::Node*)parent_ref.node(nodes);
      // volatile unsigned int *p  = (unsigned int *)&parent_ptr->child( subdiv_patch.bvh4i_parent_local_index );

      volatile unsigned int *p  = (unsigned int *)&subdiv_patch.bvh4i_subtree_root;

      /* check whether another thread currently builds this patch */
      if (build_state != 0)
	{
	  atomic_add((atomic_t*)&subdiv_patch.under_construction,-1);

	  while (subdiv_patch.under_construction != 0)
	    __pause(512);	  

	  while ( *p == BVH4i::invalidNode)
	    __pause(512);

	  return *p;	  
	}

      /* got the lock, lets build the tree */

#if 0
      mtx.lock();
      for (size_t i=0;i<patchids.size();i++)
	if (patchIndex == patchids[i])
	  FATAL("already build");

      patchids.push_back(patchIndex);
      mtx.unlock();
#endif

      numLazyBuildPatches++;
      const unsigned int grid_size = ((unsigned int)1 << subdiv_level)+1;

#if 1
      mtx.lock();
      DBG_PRINT( numLazyBuildPatches );
      DBG_PRINT( bvh->numAllocated64BytesBlocks );
      mtx.unlock();
#endif
      /* new node index is valid now */


      BBox3fa bounds = createSubTree( *(BVH4i::NodeRef*)&subdiv_patch.bvh4i_subtree_root,
				      bvh,
				      nodes,
				      subdiv_patch,
				      0,grid_size-1,
				      0,grid_size-1,
				      subdiv_level);



      /* release lock */
      const unsigned int last_index = atomic_add((atomic_t*)&subdiv_patch.under_construction,-1);

      BVH4i::NodeRef newNodeRef = *p;

      /* return new node ref */
      return newNodeRef; // 
    }
    


    __aligned(64) float u_start[16] = { 0,0,0,0,1,1,1,1,1,1,1,1,0,0,0,0 };
    __aligned(64) float v_start[16] = { 0,0,0,0,0,0,0,0,1,1,1,1,1,1,1,1 };

    bool intersect1Eval(const SubdivPatch1 &subdiv_patch,
			const float u_start,
			const float u_end,
			const float v_start,
			const float v_end,
			const size_t rayIndex, 
			const mic_f &dir_xyz,
			const mic_f &org_xyz,
			Ray16& ray16)
    {
      const RegularCatmullClarkPatch &regular_patch = subdiv_patch.patch;
      regular_patch.prefetchData();

      __aligned(64) Vec3fa vtx[4];

      const mic_f uu = select(0x0ff0,mic_f(u_end),mic_f(u_start));
      const mic_f vv = select(0xff00,mic_f(v_end),mic_f(v_start));

      if (likely(subdiv_patch.isRegular()))
	{
	  // vtx[0] = regular_patch.eval(s[0],t[0]);
	  // vtx[1] = regular_patch.eval(s[1],t[0]);
	  // vtx[2] = regular_patch.eval(s[1],t[1]);
	  // vtx[3] = regular_patch.eval(s[0],t[1]);
	  const mic_f _vtx = regular_patch.eval4(uu,vv);
	  store16f(vtx,_vtx);
	}
      else if (likely(subdiv_patch.isGregoryPatch()))
	{
	  // Vec2f s(0.0f,1.0f);
	  // Vec2f t(0.0f,1.0f);
	  // __aligned(64) GregoryPatch gpatch( regular_patch.v, subdiv_patch.f_m );
	  // vtx[0] = gpatch.eval(s[0],t[0]);
	  // vtx[1] = gpatch.eval(s[1],t[0]);
	  // vtx[2] = gpatch.eval(s[1],t[1]);
	  // vtx[3] = gpatch.eval(s[0],t[1]);
	  const mic_f _vtx = GregoryPatch::eval4( regular_patch.v, subdiv_patch.f_m, uu, vv );
	  store16f(vtx,_vtx);
	}
      else
	FATAL("not implemented");

      return intersect1_quad(rayIndex, 
			     dir_xyz,
			     org_xyz,
			     ray16,
			     vtx[0],
			     vtx[1],
			     vtx[2],
			     vtx[3],
			     subdiv_patch.geomID,
			     subdiv_patch.primID);
    }

  template<bool ENABLE_INTERSECTION_FILTER>
    struct SubdivLeafIntersector
    {
      // ==================
      // === single ray === 
      // ==================
      static __forceinline bool intersect(BVH4i::NodeRef curNode,
					  const mic_f &dir_xyz,
					  const mic_f &org_xyz,
					  const mic_f &min_dist_xyz,
					  mic_f &max_dist_xyz,
					  Ray& ray, 
					  const void *__restrict__ const accel,
					  const Scene*__restrict__ const geometry)
      {
	unsigned int items = curNode.items();
	unsigned int index = curNode.offsetIndex();
	const SubdivPatch1 *__restrict__ const patch_ptr = (SubdivPatch1*)accel + index;
	FATAL("NOT IMPLEMENTED");

	// return SubdivPatchIntersector1<ENABLE_INTERSECTION_FILTER>::intersect1(dir_xyz,
	// 								       org_xyz,
	// 								       ray,
	// 								       *patch_ptr);	
      }

      static __forceinline bool occluded(BVH4i::NodeRef curNode,
					 const mic_f &dir_xyz,
					 const mic_f &org_xyz,
					 const mic_f &min_dist_xyz,
					 const mic_f &max_dist_xyz,
					 Ray& ray,
					 const void *__restrict__ const accel,
					 const Scene*__restrict__ const geometry)
      {
	unsigned int items = curNode.items();
	unsigned int index = curNode.offsetIndex();
	const SubdivPatch1 *__restrict__ const patch_ptr = (SubdivPatch1*)accel + index;
	FATAL("NOT IMPLEMENTED");
	// return SubdivPatchIntersector1<ENABLE_INTERSECTION_FILTER>::occluded1(dir_xyz,
	// 								      org_xyz,
	// 								      ray,
	// 								      *patch_ptr);	
      }


    };

    static unsigned int BVH4I_LEAF_MASK = BVH4i::leaf_mask; // needed due to compiler efficiency bug
    static unsigned int M_LANE_7777 = 0x7777;               // needed due to compiler efficiency bug

    // ============================================================================================
    // ============================================================================================
    // ============================================================================================

    template<typename LeafIntersector, bool ENABLE_COMPRESSED_BVH4I_NODES>
    void BVH4iIntersector16Subdiv<LeafIntersector,ENABLE_COMPRESSED_BVH4I_NODES>::intersect(mic_i* valid_i, BVH4i* bvh, Ray16& ray16)
    {
      /* near and node stack */
      __aligned(64) float   stack_dist[3*BVH4i::maxDepth+1];
      __aligned(64) NodeRef stack_node[3*BVH4i::maxDepth+1];


      /* setup */
      const mic_m m_valid    = *(mic_i*)valid_i != mic_i(0);
      const mic3f rdir16     = rcp_safe(ray16.dir);
      const mic_f inf        = mic_f(pos_inf);
      const mic_f zero       = mic_f::zero();

      store16f(stack_dist,inf);

      Node      * __restrict__ nodes       = (Node     *)bvh->nodePtr();
      const Triangle1 * __restrict__ accel = (Triangle1*)bvh->triPtr();

      stack_node[0] = BVH4i::invalidNode;
      long rayIndex = -1;
      while((rayIndex = bitscan64(rayIndex,toInt(m_valid))) != BITSCAN_NO_BIT_SET_64)	    
        {
	  stack_node[1] = bvh->root;
	  size_t sindex = 2;

	  const mic_f org_xyz      = loadAOS4to16f(rayIndex,ray16.org.x,ray16.org.y,ray16.org.z);
	  const mic_f dir_xyz      = loadAOS4to16f(rayIndex,ray16.dir.x,ray16.dir.y,ray16.dir.z);
	  const mic_f rdir_xyz     = loadAOS4to16f(rayIndex,rdir16.x,rdir16.y,rdir16.z);
	  const mic_f org_rdir_xyz = org_xyz * rdir_xyz;
	  const mic_f min_dist_xyz = broadcast1to16f(&ray16.tnear[rayIndex]);
	  mic_f       max_dist_xyz = broadcast1to16f(&ray16.tfar[rayIndex]);

	  const unsigned int leaf_mask = BVH4I_LEAF_MASK;

	  while (1)
	    {

	      NodeRef curNode = stack_node[sindex-1];
	      sindex--;

	      traverse_single_intersect<ENABLE_COMPRESSED_BVH4I_NODES>(curNode,
								      sindex,
								      rdir_xyz,
								      org_rdir_xyz,
								      min_dist_xyz,
								      max_dist_xyz,
								      stack_node,
								      stack_dist,
								      nodes,
								      leaf_mask);
		   

	      /* return if stack is empty */
	      if (unlikely(curNode == BVH4i::invalidNode)) break;

	      STAT3(normal.trav_leaves,1,1,1);
	      STAT3(normal.trav_prims,4,4,4);

	      //////////////////////////////////////////////////////////////////////////////////////////////////

#if 1
	      const unsigned int patchIndex = curNode.offsetIndex();
	      SubdivPatch1& subdiv_patch = ((SubdivPatch1*)accel)[patchIndex];

	      const unsigned int subdiv_level = 2; // g_subdivision_level; cannot use g_subdivision_level with a working "free" for sub-trees

	      if (unlikely(subdiv_patch.bvh4i_subtree_root == BVH4i::invalidNode))
		{
		  BVH4i::NodeRef newNodeRef = initLazySubdivTree(subdiv_patch,
								 bvh,
								 nodes,
								 subdiv_level);

		}

	      assert(subdiv_patch.bvh4i_subtree_root != BVH4i::invalidNode);

	      // -------------------------------------
	      // -------------------------------------
	      // -------------------------------------

	      __aligned(64) float   sub_stack_dist[64];
	      __aligned(64) NodeRef sub_stack_node[64];
	      sub_stack_node[0] = BVH4i::invalidNode;
	      sub_stack_node[1] = subdiv_patch.bvh4i_subtree_root;
	      store16f(sub_stack_dist,inf);
	      size_t sub_sindex = 2;
	      bool hit = false;

	      while (1)
		{
		  curNode = sub_stack_node[sub_sindex-1];
		  sub_sindex--;

		  traverse_single_intersect<ENABLE_COMPRESSED_BVH4I_NODES>(curNode,
									   sub_sindex,
									   rdir_xyz,
									   org_rdir_xyz,
									   min_dist_xyz,
									   max_dist_xyz,
									   sub_stack_node,
									   sub_stack_dist,
									   nodes,
									   leaf_mask);
		 		   

		  /* return if stack is empty */
		  if (unlikely(curNode == BVH4i::invalidNode)) break;

		  assert(curNode.isAuxFlagSet());
		  const unsigned int uv = curNode.offsetIndex();
		  const unsigned int u  = uv & 0xff;
		  const unsigned int v  = uv >> 8;
	      
		  const float u_start = gridLookUpTables.lookUp(subdiv_level,u);
		  const float u_end   = gridLookUpTables.lookUp(subdiv_level,u+1);
		  const float v_start = gridLookUpTables.lookUp(subdiv_level,v);
		  const float v_end   = gridLookUpTables.lookUp(subdiv_level,v+1);
	      
		  hit |= intersect1Eval(subdiv_patch,
					u_start,
					u_end,
					v_start,
					v_end,
					rayIndex,
					dir_xyz,
					org_xyz,
					ray16);
		}
	      // -------------------------------------
	      // -------------------------------------
	      // -------------------------------------

	      if (hit)
		compactStack(stack_node,stack_dist,sindex,mic_f(ray16.tfar[rayIndex]));

#else
	      unsigned int items = curNode.items();
	      assert(items == 1);
	      unsigned int index = curNode.offsetIndex();
	      const SubdivPatch1 *__restrict__ const patch_ptr = (SubdivPatch1*)accel + index;
	
	      bool hit = false;
	      for (size_t i=0;i<items;i++)
	       	hit |= subdivide_intersect1(rayIndex,
					    dir_xyz,
					    org_xyz,
					    ray16,
					    patch_ptr[i]);

	      if (hit)
		compactStack(stack_node,stack_dist,sindex,mic_f(ray16.tfar[rayIndex]));

#endif	

	      // ------------------------
	    }
	}
    }

    template<typename LeafIntersector,bool ENABLE_COMPRESSED_BVH4I_NODES>    
    void BVH4iIntersector16Subdiv<LeafIntersector,ENABLE_COMPRESSED_BVH4I_NODES>::occluded(mic_i* valid_i, BVH4i* bvh, Ray16& ray16)
    {
      /* near and node stack */
      __aligned(64) NodeRef stack_node[3*BVH4i::maxDepth+1];

      /* setup */
      const mic_m m_valid = *(mic_i*)valid_i != mic_i(0);
      const mic3f rdir16  = rcp_safe(ray16.dir);
      mic_m terminated    = !m_valid;
      const mic_f inf     = mic_f(pos_inf);
      const mic_f zero    = mic_f::zero();

      const Node      * __restrict__ nodes = (Node     *)bvh->nodePtr();
      const Triangle1 * __restrict__ accel = (Triangle1*)bvh->triPtr();

      stack_node[0] = BVH4i::invalidNode;

      long rayIndex = -1;
      while((rayIndex = bitscan64(rayIndex,toInt(m_valid))) != BITSCAN_NO_BIT_SET_64)	    
        {
	  stack_node[1] = bvh->root;
	  size_t sindex = 2;

	  const mic_f org_xyz      = loadAOS4to16f(rayIndex,ray16.org.x,ray16.org.y,ray16.org.z);
	  const mic_f dir_xyz      = loadAOS4to16f(rayIndex,ray16.dir.x,ray16.dir.y,ray16.dir.z);
	  const mic_f rdir_xyz     = loadAOS4to16f(rayIndex,rdir16.x,rdir16.y,rdir16.z);
	  const mic_f org_rdir_xyz = org_xyz * rdir_xyz;
	  const mic_f min_dist_xyz = broadcast1to16f(&ray16.tnear[rayIndex]);
	  const mic_f max_dist_xyz = broadcast1to16f(&ray16.tfar[rayIndex]);
	  const mic_i v_invalidNode(BVH4i::invalidNode);
	  const unsigned int leaf_mask = BVH4I_LEAF_MASK;

	  while (1)
	    {
	      NodeRef curNode = stack_node[sindex-1];
	      sindex--;

	      traverse_single_occluded< ENABLE_COMPRESSED_BVH4I_NODES >(curNode,
								       sindex,
								       rdir_xyz,
								       org_rdir_xyz,
								       min_dist_xyz,
								       max_dist_xyz,
								       stack_node,
								       nodes,
								       leaf_mask);

	      /* return if stack is empty */
	      if (unlikely(curNode == BVH4i::invalidNode)) break;

	      STAT3(shadow.trav_leaves,1,1,1);
	      STAT3(shadow.trav_prims,4,4,4);

	      /* intersect one ray against four triangles */

	      //////////////////////////////////////////////////////////////////////////////////////////////////

	      const bool hit = false;

	      FATAL("NOT YET IMPLEMENTED");

	      if (unlikely(hit)) break;
	      //////////////////////////////////////////////////////////////////////////////////////////////////

	    }


	  if (unlikely(all(toMask(terminated)))) break;
	}


      store16i(m_valid & toMask(terminated),&ray16.geomID,0);

    }

    template<typename LeafIntersector, bool ENABLE_COMPRESSED_BVH4I_NODES>
    void BVH4iIntersector1Subdiv<LeafIntersector,ENABLE_COMPRESSED_BVH4I_NODES>::intersect(BVH4i* bvh, Ray& ray)
    {
      /* near and node stack */
      __aligned(64) float   stack_dist[3*BVH4i::maxDepth+1];
      __aligned(64) NodeRef stack_node[3*BVH4i::maxDepth+1];

      /* setup */
      //const mic_m m_valid    = *(mic_i*)valid_i != mic_i(0);
      const mic3f rdir16     = rcp_safe(mic3f(mic_f(ray.dir.x),mic_f(ray.dir.y),mic_f(ray.dir.z)));
      const mic_f inf        = mic_f(pos_inf);
      const mic_f zero       = mic_f::zero();

      store16f(stack_dist,inf);

      const Node      * __restrict__ nodes = (Node    *)bvh->nodePtr();
      const Triangle1 * __restrict__ accel = (Triangle1*)bvh->triPtr();

      stack_node[0] = BVH4i::invalidNode;      
      stack_node[1] = bvh->root;

      size_t sindex = 2;

      const mic_f org_xyz      = loadAOS4to16f(ray.org.x,ray.org.y,ray.org.z);
      const mic_f dir_xyz      = loadAOS4to16f(ray.dir.x,ray.dir.y,ray.dir.z);
      const mic_f rdir_xyz     = loadAOS4to16f(rdir16.x[0],rdir16.y[0],rdir16.z[0]);
      const mic_f org_rdir_xyz = org_xyz * rdir_xyz;
      const mic_f min_dist_xyz = broadcast1to16f(&ray.tnear);
      mic_f       max_dist_xyz = broadcast1to16f(&ray.tfar);
	  
      const unsigned int leaf_mask = BVH4I_LEAF_MASK;
	  
      while (1)
	{
	  NodeRef curNode = stack_node[sindex-1];
	  sindex--;

	  traverse_single_intersect<ENABLE_COMPRESSED_BVH4I_NODES>(curNode,
								   sindex,
								   rdir_xyz,
								   org_rdir_xyz,
								   min_dist_xyz,
								   max_dist_xyz,
								   stack_node,
								   stack_dist,
								   nodes,
								   leaf_mask);            		    

	  /* return if stack is empty */
	  if (unlikely(curNode == BVH4i::invalidNode)) break;


	  /* intersect one ray against four triangles */

	  //////////////////////////////////////////////////////////////////////////////////////////////////

	  bool hit = LeafIntersector::intersect(curNode,
						dir_xyz,
						org_xyz,
						min_dist_xyz,
						max_dist_xyz,
						ray,
						accel,
						(Scene*)bvh->geometry);
	  if (hit)
	    compactStack(stack_node,stack_dist,sindex,max_dist_xyz);
	}
    }

    template<typename LeafIntersector, bool ENABLE_COMPRESSED_BVH4I_NODES>
    void BVH4iIntersector1Subdiv<LeafIntersector,ENABLE_COMPRESSED_BVH4I_NODES>::occluded(BVH4i* bvh, Ray& ray)
    {
      /* near and node stack */
      __aligned(64) NodeRef stack_node[3*BVH4i::maxDepth+1];

      /* setup */
      const mic3f rdir16      = rcp_safe(mic3f(ray.dir.x,ray.dir.y,ray.dir.z));
      const mic_f inf         = mic_f(pos_inf);
      const mic_f zero        = mic_f::zero();

      const Node      * __restrict__ nodes = (Node     *)bvh->nodePtr();
      const Triangle1 * __restrict__ accel = (Triangle1*)bvh->triPtr();

      stack_node[0] = BVH4i::invalidNode;
      stack_node[1] = bvh->root;
      size_t sindex = 2;

      const mic_f org_xyz      = loadAOS4to16f(ray.org.x,ray.org.y,ray.org.z);
      const mic_f dir_xyz      = loadAOS4to16f(ray.dir.x,ray.dir.y,ray.dir.z);
      const mic_f rdir_xyz     = loadAOS4to16f(rdir16.x[0],rdir16.y[0],rdir16.z[0]);
      const mic_f org_rdir_xyz = org_xyz * rdir_xyz;
      const mic_f min_dist_xyz = broadcast1to16f(&ray.tnear);
      const mic_f max_dist_xyz = broadcast1to16f(&ray.tfar);

      const unsigned int leaf_mask = BVH4I_LEAF_MASK;
	  
      while (1)
	{
	  NodeRef curNode = stack_node[sindex-1];
	  sindex--;
            
	  
	  traverse_single_occluded< ENABLE_COMPRESSED_BVH4I_NODES>(curNode,
								   sindex,
								   rdir_xyz,
								   org_rdir_xyz,
								   min_dist_xyz,
								   max_dist_xyz,
								   stack_node,
								   nodes,
								   leaf_mask);	    

	  /* return if stack is empty */
	  if (unlikely(curNode == BVH4i::invalidNode)) break;


	  /* intersect one ray against four triangles */

	  //////////////////////////////////////////////////////////////////////////////////////////////////

	  bool hit = LeafIntersector::occluded(curNode,
					       dir_xyz,
					       org_xyz,
					       min_dist_xyz,
					       max_dist_xyz,
					       ray,
					       accel,
					       (Scene*)bvh->geometry);

	  if (unlikely(hit))
	    {
	      ray.geomID = 0;
	      return;
	    }
	  //////////////////////////////////////////////////////////////////////////////////////////////////

	}
    }


    // ----------------------------------------------------------------------------------------------------------------
    // ----------------------------------------------------------------------------------------------------------------
    // ----------------------------------------------------------------------------------------------------------------



    typedef BVH4iIntersector16Subdiv< SubdivLeafIntersector    < true  >, false > SubdivIntersector16SingleMoellerFilter;
    typedef BVH4iIntersector16Subdiv< SubdivLeafIntersector    < false >, false > SubdivIntersector16SingleMoellerNoFilter;

    DEFINE_INTERSECTOR16   (BVH4iSubdivMeshIntersector16        , SubdivIntersector16SingleMoellerFilter);
    DEFINE_INTERSECTOR16   (BVH4iSubdivMeshIntersector16NoFilter, SubdivIntersector16SingleMoellerNoFilter);

    typedef BVH4iIntersector1Subdiv< SubdivLeafIntersector    < true  >, false > SubdivMeshIntersector1MoellerFilter;
    typedef BVH4iIntersector1Subdiv< SubdivLeafIntersector    < false >, false > SubdivMeshIntersector1MoellerNoFilter;

    DEFINE_INTERSECTOR1    (BVH4iSubdivMeshIntersector1        , SubdivMeshIntersector1MoellerFilter);
    DEFINE_INTERSECTOR1    (BVH4iSubdivMeshIntersector1NoFilter, SubdivMeshIntersector1MoellerNoFilter);

  }
}