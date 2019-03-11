# ThreadMon-Task
[![Build Status](https://travis-ci.com/ntauth/threadmon-task.svg?branch=master)](https://travis-ci.com/ntauth/threadmon-task)

Candidate exercise for the "Multi-thread safe resource monitoring infrastructure
for the ATLAS experiment" **CERN-HSF**@GSoC2019 project.

# Description
The source code contains the following core components:
- `thread_ex`: a `std::thread` wrapper that augments its functionalities by allowing for
    callback functions to be invoked (eg. to signal thread completion) and various counters
    to be queried (`get_cpu_time`, `get_wall_time`).
- `thread_pool`: a class that holds a *vector* of `thread_ex`'s.
- `thread_pool_monitor`: a class that represents the instrumentation needed to monitor
   the resource usage of a thread pool. Each `thread_pool_monitor` spawns a watchdog thread
   that periodically queries information for each thread by means of pluggable functions
   called *probes* (`probe_fn_t`).
   
The program runs 4 worker threads, each one of which computes a montecarlo method approximation
of pi with different intervals. Empirically, the bigger the interval, the more cpu time the worker
thread consumes.

# Build
```bash
# Build
mkdir build
cd build
cmake ..
make
# Run (press any key to stop the threads)
./threadmon_task
````