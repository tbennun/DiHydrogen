////////////////////////////////////////////////////////////////////////////////
// Copyright 2019-2022 Lawrence Livermore National Security, LLC and other
// DiHydrogen Project Developers. See the top-level LICENSE file for details.
//
// SPDX-License-Identifier: Apache-2.0
////////////////////////////////////////////////////////////////////////////////
#include "h2/gpu/runtime.hpp"

#include "h2/gpu/logger.hpp"

#include <cstdlib>
#include <cstring>

#include <hip/hip_runtime.h>

// Note: The behavior of functions in this file may be impacted by the
// following environment variables:
//
//   - FLUX_TASK_LOCAL_ID
//   - SLURM_LOCALID
//   - SLURM_NTASKS_PER_NODE
//   - OMPI_COMM_WORLD_LOCAL_RANK
//   - OMPI_COMM_WORLD_LOCAL_SIZE
//   - MV2_COMM_WORLD_LOCAL_RANK
//   - MV2_COMM_WORLD_LOCAL_SIZE
//   - MPI_LOCALRANKID
//   - MPI_LOCALNRANKS
//
// The user may set the following to any string that matches "[^0].*"
// to effect certain behaviors, as described below:
//
//   - H2_SELECT_DEVICE_0: If set to a truthy value, every MPI rank
//                         will call hipSetDevice(0). This could save
//                         you from a bad binding (e.g., if using
//                         mpibind) or it could cause oversubscription
//                         (e.g., if you also set
//                         ROCR_VISIBLE_DEVICES=0 or something).
//
//   - H2_SELECT_DEVICE_RR: If set to a truthy value, every MPI rank
//                          will call
//                          hipSetDevice(local_rank%num_visible_gpus). This
//                          option is considered AFTER
//                          H2_SELECT_DEVICE_0, so if both are set,
//                          device 0 will be selected.
//
// The behavior is undefined if the value of the H2_* variables
// differs across processes in one MPI universe.

namespace
{

// There are a few cases here:
//
// mpibind=off: See all GPUs/GCDs on a node.
// mpibind=on: See ngpus/local_rnks GPUs.
//   -> ngpus > local_rnks: Many choices.
//   -> ngpus = local_rnks: Pick rank 0.
//   -> ngpus < local_rnks: Oversubscription.
//
// We should have reasonable behavior for all cases (which might just
// be to raise an error).
bool initialized_ = false;

static int guess_local_rank() noexcept
{
    // Start with launchers, then poke the MPI libs
    char const* env = std::getenv("FLUX_TASK_LOCAL_ID");
    if (!env)
        env = std::getenv("SLURM_LOCALID");
    if (!env)
        env = std::getenv("OMPI_COMM_WORLD_LOCAL_RANK"); // Open-MPI
    if (!env)
        env = std::getenv("MV2_COMM_WORLD_LOCAL_RANK"); // MVAPICH2
    if (!env)
        env = std::getenv("MPI_LOCALRANKID"); // MPICH
    return (env ? std::atoi(env) : -1);
}

static int guess_local_size() noexcept
{
    // Let's assume that ranks are balanced across nodes in flux-land...
    if (char const* flux_size = std::getenv("FLUX_JOB_SIZE"))
    {
        char const* nnodes = std::getenv("FLUX_JOB_NNODES");
        if (nnodes)
        {
            auto const int_nnodes = std::atoi(nnodes);
            return (std::atoi(flux_size) + int_nnodes - 1) / int_nnodes;
        }
    }

    char const* env = std::getenv("SLURM_NTASKS_PER_NODE");
    if (!env)
        env = std::getenv("OMPI_COMM_WORLD_LOCAL_SIZE"); // Open-MPI
    if (!env)
        env = std::getenv("MV2_COMM_WORLD_LOCAL_SIZE"); // MVAPICH2
    if (!env)
        env = std::getenv("MPI_LOCALNRANKS"); // MPICH
    return (env ? std::atoi(env) : -1);
}

// Unset -> false
// Empty -> false
// 0 -> false
static bool check_bool_cstr(char const* const str)
{
    return (str && std::strlen(str) && str[0] != '0');
}

static bool force_device_zero() noexcept
{
    return check_bool_cstr(std::getenv("H2_SELECT_DEVICE_0"));
}

static bool force_round_robin() noexcept
{
    return check_bool_cstr(std::getenv("H2_SELECT_DEVICE_RR"));
}

static void error(char const* const msg)
{
    H2_GPU_ERROR(msg);
    std::terminate();
}

static void error(std::string const& msg)
{
    H2_GPU_ERROR(msg);
    std::terminate();
}

// This just uses the HIP runtime and/or user-provided environment
// variables. A more robust solution might tap directly into HWLOC or
// something of that nature. We should also look into whether we can
// (easily) access more information about the running job, such as the
// REAL number of GPUs on a node (since the runtime is swayed by env
// variables) or even just whether or not a job has been launched with
// mpibind enabled.
static int get_reasonable_default_gpu_id() noexcept
{
    // Check if the user has requested device 0.
    if (force_device_zero())
        return 0;

    int const lrank = guess_local_rank();
    int const lsize = guess_local_size();
    if (lrank < 0)
    {
        H2_GPU_WARN("Could not guess local rank; setting device 0.");
        return 0;
    }

    if (lsize < 0)
    {
        H2_GPU_WARN("Could not guess local size; setting device 0.");
        return 0;
    }

    // Force the round-robin if it's been requested.
    int const ngpus = h2::gpu::num_gpus();
    if (force_round_robin())
        return lrank % ngpus;

    // At this point, we can just branch based on the relationship of
    // ngpus and nlocal_rnks. If we risk oversubscription, we can
    // error out at this point.
    if (lsize <= ngpus)
        return lrank;

    error("More local ranks than (visible) GPUs.");
    return -1;
}

static void set_reasonable_default_gpu()
{
    h2::gpu::set_gpu(get_reasonable_default_gpu_id());
}

static void log_gpu_info(int const gpu_id)
{
    hipDeviceProp_t props;
    H2_CHECK_HIP(hipGetDeviceProperties(&props, gpu_id));
    H2_GPU_INFO("GPU ID {}: name=\"{}\", pci={:#x}", gpu_id, props.name, props.pciBusID);
}

} // namespace

int h2::gpu::num_gpus()
{
    int count;
    H2_CHECK_HIP(hipGetDeviceCount(&count));
    return count;
}

int h2::gpu::current_gpu()
{
    int dev;
    H2_CHECK_HIP(hipGetDevice(&dev));
    return dev;
}

void h2::gpu::set_gpu(int id)
{
    H2_GPU_INFO("setting device to id={}", id);
    H2_CHECK_HIP(hipSetDevice(id));
}

void h2::gpu::init_runtime()
{
    if (!initialized_)
    {
        H2_GPU_INFO("initializing gpu runtime");
        H2_CHECK_HIP(hipInit(0));
        H2_GPU_INFO("found {} devices", num_gpus());
        set_reasonable_default_gpu();
        initialized_ = true;
    }
    else
    {
        H2_GPU_INFO("H2 GPU already initialized; current gpu={}", current_gpu());
    }
    log_gpu_info(current_gpu());
}

void h2::gpu::finalize_runtime()
{
    if (!initialized_)
        return;

    H2_GPU_INFO("finalizing gpu runtime");
    initialized_ = false;
}

bool h2::gpu::runtime_is_initialized()
{
    return initialized_;
}

bool h2::gpu::runtime_is_finalized()
{
    return !initialized_;
}

hipStream_t h2::gpu::make_stream()
{
    hipStream_t stream;
    H2_CHECK_HIP(hipStreamCreate(&stream));
    H2_GPU_INFO("created stream {}", (void*) stream);
    return stream;
}

hipStream_t h2::gpu::make_stream_nonblocking()
{
    hipStream_t stream;
    H2_CHECK_HIP(hipStreamCreateWithFlags(&stream, hipStreamNonBlocking));
    H2_GPU_INFO("created non-blocking stream {}", (void*) stream);
    return stream;
}

void h2::gpu::destroy(hipStream_t stream)
{
    H2_GPU_INFO("destroy stream {}", (void*) stream);
    H2_CHECK_HIP(hipStreamDestroy(stream));
}

hipEvent_t h2::gpu::make_event()
{
    hipEvent_t event;
    H2_CHECK_HIP(hipEventCreate(&event));
    H2_GPU_INFO("created event {}", (void*) event);
    return event;
}

hipEvent_t h2::gpu::make_event_notiming()
{
    hipEvent_t event;
    H2_CHECK_HIP(hipEventCreateWithFlags(&event, hipEventDisableTiming));
    H2_GPU_INFO("created non-timing event {}", (void*) event);
    return event;
}

void h2::gpu::destroy(hipEvent_t const event)
{
    H2_GPU_INFO("destroy event {}", (void*) event);
    H2_CHECK_HIP(hipEventDestroy(event));
}

void h2::gpu::sync()
{
    H2_GPU_INFO("synchronizing gpu");
    H2_CHECK_HIP(hipDeviceSynchronize());
}

void h2::gpu::sync(hipEvent_t event)
{
    H2_GPU_INFO("synchronizing event {}", (void*) event);
    H2_CHECK_HIP(hipEventSynchronize(event));
}

void h2::gpu::sync(hipStream_t stream)
{
    H2_GPU_INFO("synchronizing stream {}", (void*) stream);
    H2_CHECK_HIP(hipStreamSynchronize(stream));
}