//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~//
// Copyright (c) 2016-20, Lawrence Livermore National Security, LLC
// and RAJA project contributors. See the RAJA/COPYRIGHT file for details.
//
// SPDX-License-Identifier: (BSD-3-Clause)
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~//

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <vector>

#include "memoryManager.hpp"

#include "RAJA/RAJA.hpp"

/*
 *  Halo exchange Example
 *
 *  Packs and Unpacks data from 3D variables as is done in a halo exchange.
 *  It illustrates how to use the workgroup set of constructs.
 *
 *  RAJA features shown:
 *    - `WorkPool` template object
 *    - `WorkGroup` template object
 *    - `WorkSite` template object
 *    -  Index range segment
 *    -  WorkGroup policies
 *
 * If CUDA is enabled, CUDA unified memory is used.
 */

/*
  CUDA_BLOCK_SIZE - specifies the number of threads in a CUDA thread block when using forall
  CUDA_WORKGROUP_BLOCK_SIZE - specifies the number of threads in a CUDA thread block when using workgroup
*/
#if defined(RAJA_ENABLE_CUDA)
const int CUDA_BLOCK_SIZE = 256;
const int CUDA_WORKGROUP_BLOCK_SIZE = 1024;
#endif

#if defined(RAJA_ENABLE_HIP)
const int HIP_BLOCK_SIZE = 256;
const int HIP_WORKGROUP_BLOCK_SIZE = 1024;
#endif

/*
  num_neighbors - specifies the number of neighbors that each process would be
                  communicating with in 3D halo exchange
*/
const int num_neighbors = 26;

//
// Functions for checking and printing results
//
void checkResult(std::vector<double*> const& vars, std::vector<double*> const& vars_ref,
                 int var_size, int num_vars);
void printResult(std::vector<double*> const& vars, int var_size, int num_vars);

//
// Functions for allocating and populating packing and unpacking lists
//
void create_pack_lists(std::vector<int*>& pack_index_lists, std::vector<int>& pack_index_list_lengths,
                       const int halo_width, const int* grid_dims);
void create_unpack_lists(std::vector<int*>& unpack_index_lists, std::vector<int>& unpack_index_list_lengths,
                         const int halo_width, const int* grid_dims);
void destroy_pack_lists(std::vector<int*>& pack_index_lists);
void destroy_unpack_lists(std::vector<int*>& unpack_index_lists);

struct memory_manager_allocator
{
  void* allocate(size_t size)
  {
    return memoryManager::allocate<char>(size);
  }

  void deallocate(void* ptr)
  {
    char* ptrc = static_cast<char*>(ptr);
    memoryManager::deallocate(ptrc);
  }
};

#if defined(RAJA_ENABLE_CUDA) || defined(RAJA_ENABLE_HIP)

struct pinned_allocator
{
  void* allocate(size_t size)
  {
    void *ptr;
#if defined(RAJA_ENABLE_CUDA)
    cudaErrchk(cudaMallocHost((void **)&ptr, size));
#elif defined(RAJA_ENABLE_HIP)
    hipErrchk(hipHostMalloc((void **)&ptr, size));
#endif
    return ptr;
  }

  void deallocate(void* ptr)
  {
#if defined(RAJA_ENABLE_CUDA)
    cudaErrchk(cudaFreeHost(ptr));
#elif defined(RAJA_ENABLE_HIP)
    hipErrchk(hipHostFree(ptr));
#endif
  }
};

#endif

int main(int argc, char **argv)
{

  std::cout << "\n\nRAJA halo exchange example...\n";

  if (argc != 1 && argc != 7) {
    std::cerr << "Usage: tut_halo-exchange "
              << "[grid_x grid_y grid_z halo_width num_vars num_cycles]\n";
    std::exit(1);
  }

  //
  // Define grid dimensions
  // Define halo width
  // Define number of grid variables
  // Define number of cycles
  //
  const int grid_dims[3] = { (argc != 7) ? 100 : std::atoi(argv[1]),
                             (argc != 7) ? 100 : std::atoi(argv[2]),
                             (argc != 7) ? 100 : std::atoi(argv[3]) };
  const int halo_width =     (argc != 7) ?   1 : std::atoi(argv[4]);
  const int num_vars   =     (argc != 7) ?   3 : std::atoi(argv[5]);
  const int num_cycles =     (argc != 7) ?   3 : std::atoi(argv[6]);

  std::cout << "grid dimensions "    << grid_dims[0]
            << " x "                 << grid_dims[1]
            << " x "                 << grid_dims[2] << "\n"
            << "halo width"          << halo_width   << "\n"
            << "number of variables" << num_vars     << "\n"
            << "number of cycles"    << num_cycles   << "\n";

  if ( grid_dims[0] < halo_width ||
       grid_dims[1] < halo_width ||
       grid_dims[2] < halo_width ) {
    std::cerr << "Error: "
              << "grid dimensions must not be smaller than the halo width\n";
    std::exit(1);
  }

  const int grid_plus_halo_dims[3] = { grid_dims[0] + 2*halo_width,
                                       grid_dims[1] + 2*halo_width,
                                       grid_dims[2] + 2*halo_width };

  const int var_size = grid_plus_halo_dims[0] *
                       grid_plus_halo_dims[1] *
                       grid_plus_halo_dims[2] ;


  //
  // Allocate and initialize grid variables.
  //
  std::vector<double*> vars    (num_vars, nullptr);
  std::vector<double*> vars_ref(num_vars, nullptr);

  for (int v = 0; v < num_vars; ++v) {
    vars[v]     = memoryManager::allocate<double>(var_size);
    vars_ref[v] = memoryManager::allocate<double>(var_size);
  }


  //
  // Generate index lists for packing and unpacking
  //
  std::vector<int*> pack_index_lists(num_neighbors, nullptr);
  std::vector<int > pack_index_list_lengths(num_neighbors, 0);
  create_pack_lists(pack_index_lists, pack_index_list_lengths, halo_width, grid_dims);

  std::vector<int*> unpack_index_lists(num_neighbors, nullptr);
  std::vector<int > unpack_index_list_lengths(num_neighbors, 0);
  create_unpack_lists(unpack_index_lists, unpack_index_list_lengths, halo_width, grid_dims);


  //
  // Generate index lists for packing and unpacking
  //
  using range_segment = RAJA::TypedRangeSegment<int>;


//----------------------------------------------------------------------------//
  {
    std::cout << "\n Running C-style halo exchange...\n";


    std::vector<double*> buffers(num_neighbors, nullptr);

    for (int l = 0; l < num_neighbors; ++l) {

      int buffer_len = num_vars * pack_index_list_lengths[l];

      buffers[l] = memoryManager::allocate<double>(buffer_len);

    }

    for (int c = 0; c < num_cycles; ++c ) {

      // set vars
      for (int v = 0; v < num_vars; ++v) {

        double* var = vars[v];

        for (int i = 0; i < var_size; i++) {
          var[i] = i + v;
        }
      }

      for (int l = 0; l < num_neighbors; ++l) {

        double* buffer = buffers[l];
        int* list = pack_index_lists[l];
        int  len  = pack_index_list_lengths[l];

        // pack
        for (int v = 0; v < num_vars; ++v) {

          double* var = vars[v];

          for (int i = 0; i < len; i++) {
            buffer[i] = var[list[i]];
          }

          buffer += len;
        }

        // send single message
      }

      for (int l = 0; l < num_neighbors; ++l) {

        // recv single message

        double* buffer = buffers[l];
        int* list = unpack_index_lists[l];
        int  len  = unpack_index_list_lengths[l];

        // unpack
        for (int v = 0; v < num_vars; ++v) {

          double* var = vars[v];

          for (int i = 0; i < len; i++) {
            var[list[i]] = buffer[i];
          }

          buffer += len;
        }
      }

    }

    for (int l = 0; l < num_neighbors; ++l) {

      memoryManager::deallocate(buffers[l]);

    }

    // copy result of exchange for reference later
    for (int v = 0; v < num_vars; ++v) {

      double* var     = vars[v];
      double* var_ref = vars_ref[v];

      for (int i = 0; i < var_size; i++) {
        var_ref[i] = var[i];
      }
    }
  }


//----------------------------------------------------------------------------//
// Separate packing/unpacking loops using forall
//----------------------------------------------------------------------------//
  {
    std::cout << "\n Running RAJA loop forall halo exchange...\n";

    using forall_policy = RAJA::loop_exec;

    std::vector<double*> buffers(num_neighbors, nullptr);

    for (int l = 0; l < num_neighbors; ++l) {

      int buffer_len = num_vars * pack_index_list_lengths[l];

      buffers[l] = memoryManager::allocate<double>(buffer_len);

    }

    for (int c = 0; c < num_cycles; ++c ) {

      // set vars
      for (int v = 0; v < num_vars; ++v) {

        double* var = vars[v];

        RAJA::forall<forall_policy>(range_segment(0, var_size), [=] (int i) {
          var[i] = i + v;
        });
      }

      for (int l = 0; l < num_neighbors; ++l) {

        double* buffer = buffers[l];
        int* list = pack_index_lists[l];
        int  len  = pack_index_list_lengths[l];

        // pack
        for (int v = 0; v < num_vars; ++v) {

          double* var = vars[v];

          // _rajaloop_forall_pack_halo_exchange_start
          RAJA::forall<forall_policy>(range_segment(0, len), [=] (int i) {
            buffer[i] = var[list[i]];
          });
          // _rajaloop_forall_pack_halo_exchange_end

          buffer += len;
        }

        // send single message
      }

      for (int l = 0; l < num_neighbors; ++l) {

        // recv single message

        double* buffer = buffers[l];
        int* list = unpack_index_lists[l];
        int  len  = unpack_index_list_lengths[l];

        // unpack
        for (int v = 0; v < num_vars; ++v) {

          double* var = vars[v];

          // _rajaloop_forall_unpack_halo_exchange_start
          RAJA::forall<forall_policy>(range_segment(0, len), [=] (int i) {
            var[list[i]] = buffer[i];
          });
          // _rajaloop_forall_unpack_halo_exchange_end

          buffer += len;
        }
      }

    }

    for (int l = 0; l < num_neighbors; ++l) {

      memoryManager::deallocate(buffers[l]);

    }

    // check results against reference copy
    checkResult(vars, vars_ref, var_size, num_vars);
    //printResult(vars, var_size, num_vars);
  }


//----------------------------------------------------------------------------//
// RAJA::WorkGroup with allows deferred execution
// This has overhead and indirection not in the separate loop version,
// but can be useful for debugging.
//----------------------------------------------------------------------------//
  {
  std::cout << "\n Running RAJA loop workgroup halo exchange...\n";

    using forall_policy = RAJA::loop_exec;

    using workgroup_policy = RAJA::WorkGroupPolicy <
                                 RAJA::loop_work,
                                 RAJA::ordered,
                                 RAJA::ragged_array_of_objects >;

    using workpool = RAJA::WorkPool< workgroup_policy,
                                     int,
                                     RAJA::xargs<>,
                                     memory_manager_allocator >;

    using workgroup = RAJA::WorkGroup< workgroup_policy,
                                       int,
                                       RAJA::xargs<>,
                                       memory_manager_allocator >;

    using worksite = RAJA::WorkSite< workgroup_policy,
                                     int,
                                     RAJA::xargs<>,
                                     memory_manager_allocator >;

    std::vector<double*> buffers(num_neighbors, nullptr);

    for (int l = 0; l < num_neighbors; ++l) {

      int buffer_len = num_vars * pack_index_list_lengths[l];

      buffers[l] = memoryManager::allocate<double>(buffer_len);

    }

    workpool pool_pack  (memory_manager_allocator{});
    workpool pool_unpack(memory_manager_allocator{});

    for (int c = 0; c < num_cycles; ++c ) {

      // set vars
      for (int v = 0; v < num_vars; ++v) {

        double* var = vars[v];

        RAJA::forall<forall_policy>(range_segment(0, var_size), [=] (int i) {
          var[i] = i + v;
        });
      }

      for (int l = 0; l < num_neighbors; ++l) {

        double* buffer = buffers[l];
        int* list = pack_index_lists[l];
        int  len  = pack_index_list_lengths[l];

        // pack
        for (int v = 0; v < num_vars; ++v) {

          double* var = vars[v];

          // _rajaloop_workgroup_pack_enqueue_halo_exchange_start
          pool_pack.enqueue(range_segment(0, len), [=] (int i) {
            buffer[i] = var[list[i]];
          });
          // _rajaloop_workgroup_pack_enqueue_halo_exchange_end

          buffer += len;
        }
      }

      // _rajaloop_workgroup_pack_run_halo_exchange_start
      workgroup group_pack = pool_pack.instantiate();

      worksite site_pack = group_pack.run();
      // _rajaloop_workgroup_pack_run_halo_exchange_end

      // send all messages

      // recv all messages

      for (int l = 0; l < num_neighbors; ++l) {

        double* buffer = buffers[l];
        int* list = unpack_index_lists[l];
        int  len  = unpack_index_list_lengths[l];

        // unpack
        for (int v = 0; v < num_vars; ++v) {

          double* var = vars[v];

          // _rajaloop_workgroup_unpack_enqueue_halo_exchange_start
          pool_unpack.enqueue(range_segment(0, len), [=] (int i) {
            var[list[i]] = buffer[i];
          });
          // _rajaloop_workgroup_unpack_enqueue_halo_exchange_end

          buffer += len;
        }
      }

      // _rajaloop_workgroup_unpack_run_halo_exchange_start
      workgroup group_unpack = pool_unpack.instantiate();

      worksite site_unpack = group_unpack.run();
      // _rajaloop_workgroup_unpack_run_halo_exchange_end
    }

    for (int l = 0; l < num_neighbors; ++l) {

      memoryManager::deallocate(buffers[l]);

    }

    // check results against reference copy
    checkResult(vars, vars_ref, var_size, num_vars);
    //printResult(vars, var_size, num_vars);
  }


//----------------------------------------------------------------------------//


#if defined(RAJA_ENABLE_OPENMP)

//----------------------------------------------------------------------------//
// Separate packing/unpacking loops using forall
//----------------------------------------------------------------------------//
  {
    std::cout << "\n Running RAJA Openmp forall halo exchange...\n";

    using forall_policy = RAJA::omp_parallel_for_exec;

    std::vector<double*> buffers(num_neighbors, nullptr);

    for (int l = 0; l < num_neighbors; ++l) {

      int buffer_len = num_vars * pack_index_list_lengths[l];

      buffers[l] = memoryManager::allocate<double>(buffer_len);

    }

    for (int c = 0; c < num_cycles; ++c ) {

      // set vars
      for (int v = 0; v < num_vars; ++v) {

        double* var = vars[v];

        RAJA::forall<forall_policy>(range_segment(0, var_size), [=] (int i) {
          var[i] = i + v;
        });
      }

      for (int l = 0; l < num_neighbors; ++l) {

        double* buffer = buffers[l];
        int* list = pack_index_lists[l];
        int  len  = pack_index_list_lengths[l];

        // pack
        for (int v = 0; v < num_vars; ++v) {

          double* var = vars[v];

          RAJA::forall<forall_policy>(range_segment(0, len), [=] (int i) {
            buffer[i] = var[list[i]];
          });

          buffer += len;
        }

        // send single message
      }

      for (int l = 0; l < num_neighbors; ++l) {

        // recv single message

        double* buffer = buffers[l];
        int* list = unpack_index_lists[l];
        int  len  = unpack_index_list_lengths[l];

        // unpack
        for (int v = 0; v < num_vars; ++v) {

          double* var = vars[v];

          RAJA::forall<forall_policy>(range_segment(0, len), [=] (int i) {
            var[list[i]] = buffer[i];
          });

          buffer += len;
        }
      }

    }

    for (int l = 0; l < num_neighbors; ++l) {

      memoryManager::deallocate(buffers[l]);

    }

    // check results against reference copy
    checkResult(vars, vars_ref, var_size, num_vars);
    //printResult(vars, var_size, num_vars);
  }


//----------------------------------------------------------------------------//
// RAJA::WorkGroup may allow effective parallelism across loops with Openmp.
//----------------------------------------------------------------------------//
  {
    std::cout << "\n Running RAJA OpenMP workgroup halo exchange...\n";

    using forall_policy = RAJA::omp_parallel_for_exec;

    using workgroup_policy = RAJA::WorkGroupPolicy <
                                 RAJA::omp_work,
                                 RAJA::ordered,
                                 RAJA::ragged_array_of_objects >;

    using workpool = RAJA::WorkPool< workgroup_policy,
                                     int,
                                     RAJA::xargs<>,
                                     memory_manager_allocator >;

    using workgroup = RAJA::WorkGroup< workgroup_policy,
                                       int,
                                       RAJA::xargs<>,
                                       memory_manager_allocator >;

    using worksite = RAJA::WorkSite< workgroup_policy,
                                     int,
                                     RAJA::xargs<>,
                                     memory_manager_allocator >;

    std::vector<double*> buffers(num_neighbors, nullptr);

    for (int l = 0; l < num_neighbors; ++l) {

      int buffer_len = num_vars * pack_index_list_lengths[l];

      buffers[l] = memoryManager::allocate<double>(buffer_len);

    }

    workpool pool_pack  (memory_manager_allocator{});
    workpool pool_unpack(memory_manager_allocator{});

    for (int c = 0; c < num_cycles; ++c ) {

      // set vars
      for (int v = 0; v < num_vars; ++v) {

        double* var = vars[v];

        RAJA::forall<forall_policy>(range_segment(0, var_size), [=] (int i) {
          var[i] = i + v;
        });
      }

      for (int l = 0; l < num_neighbors; ++l) {

        double* buffer = buffers[l];
        int* list = pack_index_lists[l];
        int  len  = pack_index_list_lengths[l];

        // pack
        for (int v = 0; v < num_vars; ++v) {

          double* var = vars[v];

          pool_pack.enqueue(range_segment(0, len), [=] (int i) {
            buffer[i] = var[list[i]];
          });

          buffer += len;
        }
      }

      workgroup group_pack = pool_pack.instantiate();

      worksite site_pack = group_pack.run();

      // send all messages

      // recv all messages

      for (int l = 0; l < num_neighbors; ++l) {

        double* buffer = buffers[l];
        int* list = unpack_index_lists[l];
        int  len  = unpack_index_list_lengths[l];

        // unpack
        for (int v = 0; v < num_vars; ++v) {

          double* var = vars[v];

          pool_unpack.enqueue(range_segment(0, len), [=] (int i) {
            var[list[i]] = buffer[i];
          });

          buffer += len;
        }
      }

      workgroup group_unpack = pool_unpack.instantiate();

      worksite site_unpack = group_unpack.run();
    }

    for (int l = 0; l < num_neighbors; ++l) {

      memoryManager::deallocate(buffers[l]);

    }

    // check results against reference copy
    checkResult(vars, vars_ref, var_size, num_vars);
    //printResult(vars, var_size, num_vars);
  }

#endif


//----------------------------------------------------------------------------//


#if defined(RAJA_ENABLE_CUDA)

//----------------------------------------------------------------------------//
// Separate packing/unpacking loops using forall
//----------------------------------------------------------------------------//
  {
    std::cout << "\n Running RAJA Cuda forall halo exchange...\n";


    std::vector<double*> cuda_vars(num_vars, nullptr);
    std::vector<int*>    cuda_pack_index_lists(num_neighbors, nullptr);
    std::vector<int*>    cuda_unpack_index_lists(num_neighbors, nullptr);

    for (int v = 0; v < num_vars; ++v) {
      cuda_vars[v] = memoryManager::allocate_gpu<double>(var_size);
    }

    for (int l = 0; l < num_neighbors; ++l) {
      int pack_len = pack_index_list_lengths[l];
      cuda_pack_index_lists[l] = memoryManager::allocate_gpu<int>(pack_len);
      cudaErrchk(cudaMemcpy( cuda_pack_index_lists[l], pack_index_lists[l], pack_len * sizeof(int), cudaMemcpyDefault ));

      int unpack_len = unpack_index_list_lengths[l];
      cuda_unpack_index_lists[l] = memoryManager::allocate_gpu<int>(unpack_len);
      cudaErrchk(cudaMemcpy( cuda_unpack_index_lists[l], unpack_index_lists[l], unpack_len * sizeof(int), cudaMemcpyDefault ));
    }

    std::swap(vars,               cuda_vars);
    std::swap(pack_index_lists,   cuda_pack_index_lists);
    std::swap(unpack_index_lists, cuda_unpack_index_lists);


    using forall_policy = RAJA::cuda_exec_async<CUDA_BLOCK_SIZE>;

    std::vector<double*> buffers(num_neighbors, nullptr);

    for (int l = 0; l < num_neighbors; ++l) {

      int buffer_len = num_vars * pack_index_list_lengths[l];

      buffers[l] = memoryManager::allocate_gpu<double>(buffer_len);

    }

    for (int c = 0; c < num_cycles; ++c ) {

      // set vars
      for (int v = 0; v < num_vars; ++v) {

        double* var = vars[v];

        RAJA::forall<forall_policy>(range_segment(0, var_size), [=] RAJA_DEVICE (int i) {
          var[i] = i + v;
        });
      }

      for (int l = 0; l < num_neighbors; ++l) {

        double* buffer = buffers[l];
        int* list = pack_index_lists[l];
        int  len  = pack_index_list_lengths[l];

        // pack
        for (int v = 0; v < num_vars; ++v) {

          double* var = vars[v];

          RAJA::forall<forall_policy>(range_segment(0, len), [=] RAJA_DEVICE (int i) {
            buffer[i] = var[list[i]];
          });

          buffer += len;
        }

        cudaErrchk(cudaDeviceSynchronize());

        // send single message
      }

      for (int l = 0; l < num_neighbors; ++l) {

        // recv single message

        double* buffer = buffers[l];
        int* list = unpack_index_lists[l];
        int  len  = unpack_index_list_lengths[l];

        // unpack
        for (int v = 0; v < num_vars; ++v) {

          double* var = vars[v];

          RAJA::forall<forall_policy>(range_segment(0, len), [=] RAJA_DEVICE (int i) {
            var[list[i]] = buffer[i];
          });

          buffer += len;
        }
      }

      cudaErrchk(cudaDeviceSynchronize());
    }

    for (int l = 0; l < num_neighbors; ++l) {

      memoryManager::deallocate_gpu(buffers[l]);

    }


    std::swap(vars,               cuda_vars);
    std::swap(pack_index_lists,   cuda_pack_index_lists);
    std::swap(unpack_index_lists, cuda_unpack_index_lists);

    for (int v = 0; v < num_vars; ++v) {
      cudaErrchk(cudaMemcpy( vars[v], cuda_vars[v], var_size * sizeof(double), cudaMemcpyDefault ));
      memoryManager::deallocate_gpu(cuda_vars[v]);
    }

    for (int l = 0; l < num_neighbors; ++l) {
      memoryManager::deallocate_gpu(cuda_pack_index_lists[l]);
      memoryManager::deallocate_gpu(cuda_unpack_index_lists[l]);
    }


    // check results against reference copy
    checkResult(vars, vars_ref, var_size, num_vars);
    //printResult(vars, var_size, num_vars);
  }


//----------------------------------------------------------------------------//
// RAJA::WorkGroup with cuda_work allows deferred kernel fusion execution
//----------------------------------------------------------------------------//
  {
    std::cout << "\n Running RAJA Cuda workgroup halo exchange...\n";


    std::vector<double*> cuda_vars(num_vars, nullptr);
    std::vector<int*>    cuda_pack_index_lists(num_neighbors, nullptr);
    std::vector<int*>    cuda_unpack_index_lists(num_neighbors, nullptr);

    for (int v = 0; v < num_vars; ++v) {
      cuda_vars[v] = memoryManager::allocate_gpu<double>(var_size);
    }

    for (int l = 0; l < num_neighbors; ++l) {
      int pack_len = pack_index_list_lengths[l];
      cuda_pack_index_lists[l] = memoryManager::allocate_gpu<int>(pack_len);
      cudaErrchk(cudaMemcpy( cuda_pack_index_lists[l], pack_index_lists[l], pack_len * sizeof(int), cudaMemcpyDefault ));

      int unpack_len = unpack_index_list_lengths[l];
      cuda_unpack_index_lists[l] = memoryManager::allocate_gpu<int>(unpack_len);
      cudaErrchk(cudaMemcpy( cuda_unpack_index_lists[l], unpack_index_lists[l], unpack_len * sizeof(int), cudaMemcpyDefault ));
    }

    std::swap(vars,               cuda_vars);
    std::swap(pack_index_lists,   cuda_pack_index_lists);
    std::swap(unpack_index_lists, cuda_unpack_index_lists);


    using forall_policy = RAJA::cuda_exec_async<CUDA_BLOCK_SIZE>;

    using workgroup_policy = RAJA::WorkGroupPolicy <
                                 RAJA::cuda_work_async<CUDA_WORKGROUP_BLOCK_SIZE>,
                                 RAJA::unordered_cuda_loop_y_block_iter_x_threadblock_average,
                                 RAJA::constant_stride_array_of_objects >;

    using workpool = RAJA::WorkPool< workgroup_policy,
                                     int,
                                     RAJA::xargs<>,
                                     pinned_allocator >;

    using workgroup = RAJA::WorkGroup< workgroup_policy,
                                       int,
                                       RAJA::xargs<>,
                                       pinned_allocator >;

    using worksite = RAJA::WorkSite< workgroup_policy,
                                     int,
                                     RAJA::xargs<>,
                                     pinned_allocator >;

    std::vector<double*> buffers(num_neighbors, nullptr);

    for (int l = 0; l < num_neighbors; ++l) {

      int buffer_len = num_vars * pack_index_list_lengths[l];

      buffers[l] = memoryManager::allocate_gpu<double>(buffer_len);

    }

    workpool pool_pack  (pinned_allocator{});
    workpool pool_unpack(pinned_allocator{});

    for (int c = 0; c < num_cycles; ++c ) {

      // set vars
      for (int v = 0; v < num_vars; ++v) {

        double* var = vars[v];

        RAJA::forall<forall_policy>(range_segment(0, var_size), [=] RAJA_DEVICE (int i) {
          var[i] = i + v;
        });
      }

      for (int l = 0; l < num_neighbors; ++l) {

        double* buffer = buffers[l];
        int* list = pack_index_lists[l];
        int  len  = pack_index_list_lengths[l];

        // pack
        for (int v = 0; v < num_vars; ++v) {

          double* var = vars[v];

          pool_pack.enqueue(range_segment(0, len), [=] RAJA_DEVICE (int i) {
            buffer[i] = var[list[i]];
          });

          buffer += len;
        }
      }

      workgroup group_pack = pool_pack.instantiate();

      worksite site_pack = group_pack.run();

      cudaErrchk(cudaDeviceSynchronize());

      // send all messages

      // recv all messages

      for (int l = 0; l < num_neighbors; ++l) {

        double* buffer = buffers[l];
        int* list = unpack_index_lists[l];
        int  len  = unpack_index_list_lengths[l];

        // unpack
        for (int v = 0; v < num_vars; ++v) {

          double* var = vars[v];

          pool_unpack.enqueue(range_segment(0, len), [=] RAJA_DEVICE (int i) {
            var[list[i]] = buffer[i];
          });

          buffer += len;
        }
      }

      workgroup group_unpack = pool_unpack.instantiate();

      worksite site_unpack = group_unpack.run();

      cudaErrchk(cudaDeviceSynchronize());
    }

    for (int l = 0; l < num_neighbors; ++l) {

      memoryManager::deallocate_gpu(buffers[l]);

    }


    std::swap(vars,               cuda_vars);
    std::swap(pack_index_lists,   cuda_pack_index_lists);
    std::swap(unpack_index_lists, cuda_unpack_index_lists);

    for (int v = 0; v < num_vars; ++v) {
      cudaErrchk(cudaMemcpy( vars[v], cuda_vars[v], var_size * sizeof(double), cudaMemcpyDefault ));
      memoryManager::deallocate_gpu(cuda_vars[v]);
    }

    for (int l = 0; l < num_neighbors; ++l) {
      memoryManager::deallocate_gpu(cuda_pack_index_lists[l]);
      memoryManager::deallocate_gpu(cuda_unpack_index_lists[l]);
    }


    // check results against reference copy
    checkResult(vars, vars_ref, var_size, num_vars);
    //printResult(vars, var_size, num_vars);
  }

#endif


//----------------------------------------------------------------------------//


#if defined(RAJA_ENABLE_HIP)

//----------------------------------------------------------------------------//
// Separate packing/unpacking loops using forall
//----------------------------------------------------------------------------//
  {
    std::cout << "\n Running RAJA Hip forall halo exchange...\n";


    std::vector<double*> hip_vars(num_vars, nullptr);
    std::vector<int*>    hip_pack_index_lists(num_neighbors, nullptr);
    std::vector<int*>    hip_unpack_index_lists(num_neighbors, nullptr);

    for (int v = 0; v < num_vars; ++v) {
      hip_vars[v] = memoryManager::allocate_gpu<double>(var_size);
    }

    for (int l = 0; l < num_neighbors; ++l) {
      int pack_len = pack_index_list_lengths[l];
      hip_pack_index_lists[l] = memoryManager::allocate_gpu<int>(pack_len);
      hipErrchk(hipMemcpy( hip_pack_index_lists[l], pack_index_lists[l], pack_len * sizeof(int), hipMemcpyHostToDevice ));

      int unpack_len = unpack_index_list_lengths[l];
      hip_unpack_index_lists[l] = memoryManager::allocate_gpu<int>(unpack_len);
      hipErrchk(hipMemcpy( hip_unpack_index_lists[l], unpack_index_lists[l], unpack_len * sizeof(int), hipMemcpyHostToDevice ));
    }

    std::swap(vars,               hip_vars);
    std::swap(pack_index_lists,   hip_pack_index_lists);
    std::swap(unpack_index_lists, hip_unpack_index_lists);


    using forall_policy = RAJA::hip_exec_async<HIP_BLOCK_SIZE>;

    std::vector<double*> buffers(num_neighbors, nullptr);

    for (int l = 0; l < num_neighbors; ++l) {

      int buffer_len = num_vars * pack_index_list_lengths[l];

      buffers[l] = memoryManager::allocate_gpu<double>(buffer_len);

    }

    for (int c = 0; c < num_cycles; ++c ) {

      // set vars
      for (int v = 0; v < num_vars; ++v) {

        double* var = vars[v];

        RAJA::forall<forall_policy>(range_segment(0, var_size), [=] RAJA_DEVICE (int i) {
          var[i] = i + v;
        });
      }

      for (int l = 0; l < num_neighbors; ++l) {

        double* buffer = buffers[l];
        int* list = pack_index_lists[l];
        int  len  = pack_index_list_lengths[l];

        // pack
        for (int v = 0; v < num_vars; ++v) {

          double* var = vars[v];

          RAJA::forall<forall_policy>(range_segment(0, len), [=] RAJA_DEVICE (int i) {
            buffer[i] = var[list[i]];
          });

          buffer += len;
        }

        hipErrchk(hipDeviceSynchronize());

        // send single message
      }

      for (int l = 0; l < num_neighbors; ++l) {

        // recv single message

        double* buffer = buffers[l];
        int* list = unpack_index_lists[l];
        int  len  = unpack_index_list_lengths[l];

        // unpack
        for (int v = 0; v < num_vars; ++v) {

          double* var = vars[v];

          RAJA::forall<forall_policy>(range_segment(0, len), [=] RAJA_DEVICE (int i) {
            var[list[i]] = buffer[i];
          });

          buffer += len;
        }
      }

      hipErrchk(hipDeviceSynchronize());
    }

    for (int l = 0; l < num_neighbors; ++l) {

      memoryManager::deallocate_gpu(buffers[l]);

    }


    std::swap(vars,               hip_vars);
    std::swap(pack_index_lists,   hip_pack_index_lists);
    std::swap(unpack_index_lists, hip_unpack_index_lists);

    for (int v = 0; v < num_vars; ++v) {
      hipErrchk(hipMemcpy( vars[v], hip_vars[v], var_size * sizeof(double), hipMemcpyDeviceToHost ));
      memoryManager::deallocate_gpu(hip_vars[v]);
    }

    for (int l = 0; l < num_neighbors; ++l) {
      memoryManager::deallocate_gpu(hip_pack_index_lists[l]);
      memoryManager::deallocate_gpu(hip_unpack_index_lists[l]);
    }


    // check results against reference copy
    checkResult(vars, vars_ref, var_size, num_vars);
    //printResult(vars, var_size, num_vars);
  }


//----------------------------------------------------------------------------//
// RAJA::WorkGroup with hip_work allows deferred kernel fusion execution
//----------------------------------------------------------------------------//
  {
    std::cout << "\n Running RAJA Hip workgroup halo exchange...\n";


    std::vector<double*> hip_vars(num_vars, nullptr);
    std::vector<int*>    hip_pack_index_lists(num_neighbors, nullptr);
    std::vector<int*>    hip_unpack_index_lists(num_neighbors, nullptr);

    for (int v = 0; v < num_vars; ++v) {
      hip_vars[v] = memoryManager::allocate_gpu<double>(var_size);
    }

    for (int l = 0; l < num_neighbors; ++l) {
      int pack_len = pack_index_list_lengths[l];
      hip_pack_index_lists[l] = memoryManager::allocate_gpu<int>(pack_len);
      hipErrchk(hipMemcpy( hip_pack_index_lists[l], pack_index_lists[l], pack_len * sizeof(int), hipMemcpyHostToDevice ));

      int unpack_len = unpack_index_list_lengths[l];
      hip_unpack_index_lists[l] = memoryManager::allocate_gpu<int>(unpack_len);
      hipErrchk(hipMemcpy( hip_unpack_index_lists[l], unpack_index_lists[l], unpack_len * sizeof(int), hipMemcpyHostToDevice ));
    }

    std::swap(vars,               hip_vars);
    std::swap(pack_index_lists,   hip_pack_index_lists);
    std::swap(unpack_index_lists, hip_unpack_index_lists);


    using forall_policy = RAJA::hip_exec_async<HIP_BLOCK_SIZE>;

    using workgroup_policy = RAJA::WorkGroupPolicy <
                                 RAJA::hip_work_async<HIP_WORKGROUP_BLOCK_SIZE>,
#if defined(RAJA_ENABLE_HIP_INDIRECT_FUNCTION_CALL)
                                 RAJA::unordered_hip_loop_y_block_iter_x_threadblock_average,
#else
                                 RAJA::ordered,
#endif
                                 RAJA::constant_stride_array_of_objects >;

    using workpool = RAJA::WorkPool< workgroup_policy,
                                     int,
                                     RAJA::xargs<>,
                                     pinned_allocator >;

    using workgroup = RAJA::WorkGroup< workgroup_policy,
                                       int,
                                       RAJA::xargs<>,
                                       pinned_allocator >;

    using worksite = RAJA::WorkSite< workgroup_policy,
                                     int,
                                     RAJA::xargs<>,
                                     pinned_allocator >;

    std::vector<double*> buffers(num_neighbors, nullptr);

    for (int l = 0; l < num_neighbors; ++l) {

      int buffer_len = num_vars * pack_index_list_lengths[l];

      buffers[l] = memoryManager::allocate_gpu<double>(buffer_len);

    }

    workpool pool_pack  (pinned_allocator{});
    workpool pool_unpack(pinned_allocator{});

    for (int c = 0; c < num_cycles; ++c ) {

      // set vars
      for (int v = 0; v < num_vars; ++v) {

        double* var = vars[v];

        RAJA::forall<forall_policy>(range_segment(0, var_size), [=] RAJA_DEVICE (int i) {
          var[i] = i + v;
        });
      }

      for (int l = 0; l < num_neighbors; ++l) {

        double* buffer = buffers[l];
        int* list = pack_index_lists[l];
        int  len  = pack_index_list_lengths[l];

        // pack
        for (int v = 0; v < num_vars; ++v) {

          double* var = vars[v];

          pool_pack.enqueue(range_segment(0, len), [=] RAJA_DEVICE (int i) {
            buffer[i] = var[list[i]];
          });

          buffer += len;
        }
      }

      workgroup group_pack = pool_pack.instantiate();

      worksite site_pack = group_pack.run();

      hipErrchk(hipDeviceSynchronize());

      // send all messages

      // recv all messages

      for (int l = 0; l < num_neighbors; ++l) {

        double* buffer = buffers[l];
        int* list = unpack_index_lists[l];
        int  len  = unpack_index_list_lengths[l];

        // unpack
        for (int v = 0; v < num_vars; ++v) {

          double* var = vars[v];

          pool_unpack.enqueue(range_segment(0, len), [=] RAJA_DEVICE (int i) {
            var[list[i]] = buffer[i];
          });

          buffer += len;
        }
      }

      workgroup group_unpack = pool_unpack.instantiate();

      worksite site_unpack = group_unpack.run();

      hipErrchk(hipDeviceSynchronize());
    }

    for (int l = 0; l < num_neighbors; ++l) {

      memoryManager::deallocate_gpu(buffers[l]);

    }


    std::swap(vars,               hip_vars);
    std::swap(pack_index_lists,   hip_pack_index_lists);
    std::swap(unpack_index_lists, hip_unpack_index_lists);

    for (int v = 0; v < num_vars; ++v) {
      hipErrchk(hipMemcpy( vars[v], hip_vars[v], var_size * sizeof(double), hipMemcpyDeviceToHost ));
      memoryManager::deallocate_gpu(hip_vars[v]);
    }

    for (int l = 0; l < num_neighbors; ++l) {
      memoryManager::deallocate_gpu(hip_pack_index_lists[l]);
      memoryManager::deallocate_gpu(hip_unpack_index_lists[l]);
    }


    // check results against reference copy
    checkResult(vars, vars_ref, var_size, num_vars);
    //printResult(vars, var_size, num_vars);
  }

#endif


//----------------------------------------------------------------------------//


//
// Clean up.
//
  for (int v = 0; v < num_vars; ++v) {
    memoryManager::deallocate(vars[v]);
    memoryManager::deallocate(vars_ref[v]);
  }

  destroy_pack_lists(pack_index_lists);
  destroy_unpack_lists(unpack_index_lists);


  std::cout << "\n DONE!...\n";

  return 0;
}


//
// Function to compare result to reference and report P/F.
//
void checkResult(std::vector<double*> const& vars, std::vector<double*> const& vars_ref,
                 int var_size, int num_vars)
{
  bool correct = true;
  for (int v = 0; v < num_vars; ++v) {
    double* var = vars[v];
    double* var_ref = vars_ref[v];
    for (int i = 0; i < var_size; i++) {
      if ( var[i] != var_ref[i] ) { correct = false; }
    }
  }
  if ( correct ) {
    std::cout << "\n\t result -- PASS\n";
  } else {
    std::cout << "\n\t result -- FAIL\n";
  }
}

//
// Function to print result.
//
void printResult(std::vector<double*> const& vars, int var_size, int num_vars)
{
  std::cout << std::endl;
  for (int v = 0; v < num_vars; ++v) {
    double* var = vars[v];
    for (int i = 0; i < var_size; i++) {
      std::cout << "result[" << i << "] = " << var[i] << std::endl;
    }
  }
  std::cout << std::endl;
}


struct Extent
{
  int i_min;
  int i_max;
  int j_min;
  int j_max;
  int k_min;
  int k_max;
};

//
// Function to generate index lists for packing.
//
void create_pack_lists(std::vector<int*>& pack_index_lists,
                       std::vector<int >& pack_index_list_lengths,
                       const int halo_width, const int* grid_dims)
{
  std::vector<Extent> pack_index_list_extents(num_neighbors);

  // faces
  pack_index_list_extents[0]  = Extent{halo_width  , halo_width   + halo_width,
                                       halo_width  , grid_dims[1] + halo_width,
                                       halo_width  , grid_dims[2] + halo_width};
  pack_index_list_extents[1]  = Extent{grid_dims[0], grid_dims[0] + halo_width,
                                       halo_width  , grid_dims[1] + halo_width,
                                       halo_width  , grid_dims[2] + halo_width};
  pack_index_list_extents[2]  = Extent{halo_width  , grid_dims[0] + halo_width,
                                       halo_width  , halo_width   + halo_width,
                                       halo_width  , grid_dims[2] + halo_width};
  pack_index_list_extents[3]  = Extent{halo_width  , grid_dims[0] + halo_width,
                                       grid_dims[1], grid_dims[1] + halo_width,
                                       halo_width  , grid_dims[2] + halo_width};
  pack_index_list_extents[4]  = Extent{halo_width  , grid_dims[0] + halo_width,
                                       halo_width  , grid_dims[1] + halo_width,
                                       halo_width  , halo_width   + halo_width};
  pack_index_list_extents[5]  = Extent{halo_width  , grid_dims[0] + halo_width,
                                       halo_width  , grid_dims[1] + halo_width,
                                       grid_dims[2], grid_dims[2] + halo_width};

  // edges
  pack_index_list_extents[6]  = Extent{halo_width  , halo_width   + halo_width,
                                       halo_width  , halo_width   + halo_width,
                                       halo_width  , grid_dims[2] + halo_width};
  pack_index_list_extents[7]  = Extent{halo_width  , halo_width   + halo_width,
                                       grid_dims[1], grid_dims[1] + halo_width,
                                       halo_width  , grid_dims[2] + halo_width};
  pack_index_list_extents[8]  = Extent{grid_dims[0], grid_dims[0] + halo_width,
                                       halo_width  , halo_width   + halo_width,
                                       halo_width  , grid_dims[2] + halo_width};
  pack_index_list_extents[9]  = Extent{grid_dims[0], grid_dims[0] + halo_width,
                                       grid_dims[1], grid_dims[1] + halo_width,
                                       halo_width  , grid_dims[2] + halo_width};
  pack_index_list_extents[10] = Extent{halo_width  , halo_width   + halo_width,
                                       halo_width  , grid_dims[1] + halo_width,
                                       halo_width  , halo_width   + halo_width};
  pack_index_list_extents[11] = Extent{halo_width  , halo_width   + halo_width,
                                       halo_width  , grid_dims[1] + halo_width,
                                       grid_dims[2], grid_dims[2] + halo_width};
  pack_index_list_extents[12] = Extent{grid_dims[0], grid_dims[0] + halo_width,
                                       halo_width  , grid_dims[1] + halo_width,
                                       halo_width  , halo_width   + halo_width};
  pack_index_list_extents[13] = Extent{grid_dims[0], grid_dims[0] + halo_width,
                                       halo_width  , grid_dims[1] + halo_width,
                                       grid_dims[2], grid_dims[2] + halo_width};
  pack_index_list_extents[14] = Extent{halo_width  , grid_dims[0] + halo_width,
                                       halo_width  , halo_width   + halo_width,
                                       halo_width  , halo_width   + halo_width};
  pack_index_list_extents[15] = Extent{halo_width  , grid_dims[0] + halo_width,
                                       halo_width  , halo_width   + halo_width,
                                       grid_dims[2], grid_dims[2] + halo_width};
  pack_index_list_extents[16] = Extent{halo_width  , grid_dims[0] + halo_width,
                                       grid_dims[1], grid_dims[1] + halo_width,
                                       halo_width  , halo_width   + halo_width};
  pack_index_list_extents[17] = Extent{halo_width  , grid_dims[0] + halo_width,
                                       grid_dims[1], grid_dims[1] + halo_width,
                                       grid_dims[2], grid_dims[2] + halo_width};

  // corners
  pack_index_list_extents[18] = Extent{halo_width  , halo_width   + halo_width,
                                       halo_width  , halo_width   + halo_width,
                                       halo_width  , halo_width   + halo_width};
  pack_index_list_extents[19] = Extent{halo_width  , halo_width   + halo_width,
                                       halo_width  , halo_width   + halo_width,
                                       grid_dims[2], grid_dims[2] + halo_width};
  pack_index_list_extents[20] = Extent{halo_width  , halo_width   + halo_width,
                                       grid_dims[1], grid_dims[1] + halo_width,
                                       halo_width  , halo_width   + halo_width};
  pack_index_list_extents[21] = Extent{halo_width  , halo_width   + halo_width,
                                       grid_dims[1], grid_dims[1] + halo_width,
                                       grid_dims[2], grid_dims[2] + halo_width};
  pack_index_list_extents[22] = Extent{grid_dims[0], grid_dims[0] + halo_width,
                                       halo_width  , halo_width   + halo_width,
                                       halo_width  , halo_width   + halo_width};
  pack_index_list_extents[23] = Extent{grid_dims[0], grid_dims[0] + halo_width,
                                       halo_width  , halo_width   + halo_width,
                                       grid_dims[2], grid_dims[2] + halo_width};
  pack_index_list_extents[24] = Extent{grid_dims[0], grid_dims[0] + halo_width,
                                       grid_dims[1], grid_dims[1] + halo_width,
                                       halo_width  , halo_width   + halo_width};
  pack_index_list_extents[25] = Extent{grid_dims[0], grid_dims[0] + halo_width,
                                       grid_dims[1], grid_dims[1] + halo_width,
                                       grid_dims[2], grid_dims[2] + halo_width};

  const int grid_i_stride = 1;
  const int grid_j_stride = grid_dims[0] + 2*halo_width;
  const int grid_k_stride = grid_j_stride * (grid_dims[1] + 2*halo_width);

  for (int l = 0; l < num_neighbors; ++l) {

    Extent extent = pack_index_list_extents[l];

    pack_index_list_lengths[l] = (extent.i_max - extent.i_min) *
                                 (extent.j_max - extent.j_min) *
                                 (extent.k_max - extent.k_min) ;

    pack_index_lists[l] = memoryManager::allocate<int>(pack_index_list_lengths[l]);

    int* pack_list = pack_index_lists[l];

    int list_idx = 0;
    for (int kk = extent.k_min; kk < extent.k_max; ++kk) {
      for (int jj = extent.j_min; jj < extent.j_max; ++jj) {
        for (int ii = extent.i_min; ii < extent.i_max; ++ii) {

          int pack_idx = ii * grid_i_stride +
                         jj * grid_j_stride +
                         kk * grid_k_stride ;

          pack_list[list_idx] = pack_idx;

          list_idx += 1;
        }
      }
    }
  }
}

//
// Function to destroy packing index lists.
//
void destroy_pack_lists(std::vector<int*>& pack_index_lists)
{
  for (int l = 0; l < num_neighbors; ++l) {
    memoryManager::deallocate(pack_index_lists[l]);
  }
}


//
// Function to generate index lists for unpacking.
//
void create_unpack_lists(std::vector<int*>& unpack_index_lists, std::vector<int>& unpack_index_list_lengths,
                         const int halo_width, const int* grid_dims)
{
  std::vector<Extent> unpack_index_list_extents(num_neighbors);

  // faces
  unpack_index_list_extents[0]  = Extent{0                        ,                  halo_width,
                                         halo_width               , grid_dims[1] +   halo_width,
                                         halo_width               , grid_dims[2] +   halo_width};
  unpack_index_list_extents[1]  = Extent{grid_dims[0] + halo_width, grid_dims[0] + 2*halo_width,
                                         halo_width               , grid_dims[1] +   halo_width,
                                         halo_width               , grid_dims[2] +   halo_width};
  unpack_index_list_extents[2]  = Extent{halo_width               , grid_dims[0] +   halo_width,
                                         0                        ,                  halo_width,
                                         halo_width               , grid_dims[2] +   halo_width};
  unpack_index_list_extents[3]  = Extent{halo_width               , grid_dims[0] +   halo_width,
                                         grid_dims[1] + halo_width, grid_dims[1] + 2*halo_width,
                                         halo_width               , grid_dims[2] +   halo_width};
  unpack_index_list_extents[4]  = Extent{halo_width               , grid_dims[0] +   halo_width,
                                         halo_width               , grid_dims[1] +   halo_width,
                                         0                        ,                  halo_width};
  unpack_index_list_extents[5]  = Extent{halo_width               , grid_dims[0] +   halo_width,
                                         halo_width               , grid_dims[1] +   halo_width,
                                         grid_dims[2] + halo_width, grid_dims[2] + 2*halo_width};

  // edges
  unpack_index_list_extents[6]  = Extent{0                        ,                  halo_width,
                                         0                        ,                  halo_width,
                                         halo_width               , grid_dims[2] +   halo_width};
  unpack_index_list_extents[7]  = Extent{0                        ,                  halo_width,
                                         grid_dims[1] + halo_width, grid_dims[1] + 2*halo_width,
                                         halo_width               , grid_dims[2] +   halo_width};
  unpack_index_list_extents[8]  = Extent{grid_dims[0] + halo_width, grid_dims[0] + 2*halo_width,
                                         0                        ,                  halo_width,
                                         halo_width               , grid_dims[2] +   halo_width};
  unpack_index_list_extents[9]  = Extent{grid_dims[0] + halo_width, grid_dims[0] + 2*halo_width,
                                         grid_dims[1] + halo_width, grid_dims[1] + 2*halo_width,
                                         halo_width               , grid_dims[2] +   halo_width};
  unpack_index_list_extents[10] = Extent{0                        ,                  halo_width,
                                         halo_width               , grid_dims[1] +   halo_width,
                                         0                        ,                  halo_width};
  unpack_index_list_extents[11] = Extent{0                        ,                  halo_width,
                                         halo_width               , grid_dims[1] +   halo_width,
                                         grid_dims[2] + halo_width, grid_dims[2] + 2*halo_width};
  unpack_index_list_extents[12] = Extent{grid_dims[0] + halo_width, grid_dims[0] + 2*halo_width,
                                         halo_width               , grid_dims[1] +   halo_width,
                                         0                        ,                  halo_width};
  unpack_index_list_extents[13] = Extent{grid_dims[0] + halo_width, grid_dims[0] + 2*halo_width,
                                         halo_width               , grid_dims[1] +   halo_width,
                                         grid_dims[2] + halo_width, grid_dims[2] + 2*halo_width};
  unpack_index_list_extents[14] = Extent{halo_width               , grid_dims[0] +   halo_width,
                                         0                        ,                  halo_width,
                                         0                        ,                  halo_width};
  unpack_index_list_extents[15] = Extent{halo_width               , grid_dims[0] +   halo_width,
                                         0                        ,                  halo_width,
                                         grid_dims[2] + halo_width, grid_dims[2] + 2*halo_width};
  unpack_index_list_extents[16] = Extent{halo_width               , grid_dims[0] +   halo_width,
                                         grid_dims[1] + halo_width, grid_dims[1] + 2*halo_width,
                                         0                        ,                  halo_width};
  unpack_index_list_extents[17] = Extent{halo_width               , grid_dims[0] +   halo_width,
                                         grid_dims[1] + halo_width, grid_dims[1] + 2*halo_width,
                                         grid_dims[2] + halo_width, grid_dims[2] + 2*halo_width};

  // corners
  unpack_index_list_extents[18] = Extent{0                        ,                  halo_width,
                                         0                        ,                  halo_width,
                                         0                        ,                  halo_width};
  unpack_index_list_extents[19] = Extent{0                        ,                  halo_width,
                                         0                        ,                  halo_width,
                                         grid_dims[2] + halo_width, grid_dims[2] + 2*halo_width};
  unpack_index_list_extents[20] = Extent{0                        ,                  halo_width,
                                         grid_dims[1] + halo_width, grid_dims[1] + 2*halo_width,
                                         0                        ,                  halo_width};
  unpack_index_list_extents[21] = Extent{0                        ,                  halo_width,
                                         grid_dims[1] + halo_width, grid_dims[1] + 2*halo_width,
                                         grid_dims[2] + halo_width, grid_dims[2] + 2*halo_width};
  unpack_index_list_extents[22] = Extent{grid_dims[0] + halo_width, grid_dims[0] + 2*halo_width,
                                         0                        ,                  halo_width,
                                         0                        ,                  halo_width};
  unpack_index_list_extents[23] = Extent{grid_dims[0] + halo_width, grid_dims[0] + 2*halo_width,
                                         0                        ,                  halo_width,
                                         grid_dims[2] + halo_width, grid_dims[2] + 2*halo_width};
  unpack_index_list_extents[24] = Extent{grid_dims[0] + halo_width, grid_dims[0] + 2*halo_width,
                                         grid_dims[1] + halo_width, grid_dims[1] + 2*halo_width,
                                         0                        ,                  halo_width};
  unpack_index_list_extents[25] = Extent{grid_dims[0] + halo_width, grid_dims[0] + 2*halo_width,
                                         grid_dims[1] + halo_width, grid_dims[1] + 2*halo_width,
                                         grid_dims[2] + halo_width, grid_dims[2] + 2*halo_width};

  const int grid_i_stride = 1;
  const int grid_j_stride = grid_dims[0] + 2*halo_width;
  const int grid_k_stride = grid_j_stride * (grid_dims[1] + 2*halo_width);

  for (int l = 0; l < num_neighbors; ++l) {

    Extent extent = unpack_index_list_extents[l];

    unpack_index_list_lengths[l] = (extent.i_max - extent.i_min) *
                                   (extent.j_max - extent.j_min) *
                                   (extent.k_max - extent.k_min) ;

    unpack_index_lists[l] = memoryManager::allocate<int>(unpack_index_list_lengths[l]);

    int* unpack_list = unpack_index_lists[l];

    int list_idx = 0;
    for (int kk = extent.k_min; kk < extent.k_max; ++kk) {
      for (int jj = extent.j_min; jj < extent.j_max; ++jj) {
        for (int ii = extent.i_min; ii < extent.i_max; ++ii) {

          int unpack_idx = ii * grid_i_stride +
                           jj * grid_j_stride +
                           kk * grid_k_stride ;

          unpack_list[list_idx] = unpack_idx;

          list_idx += 1;
        }
      }
    }
  }
}

//
// Function to destroy unpacking index lists.
//
void destroy_unpack_lists(std::vector<int*>& unpack_index_lists)
{
  for (int l = 0; l < num_neighbors; ++l) {
    memoryManager::deallocate(unpack_index_lists[l]);
  }
}
