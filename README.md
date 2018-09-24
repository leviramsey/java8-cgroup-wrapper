Cgroup-Aware Java 8 Docker Image
================================

This project provides a Docker image based on `openjdk:8-alpine` with a wrapper for `java` which
will size the GC threadpools based on the number of CPUs allocated to the cgroup and size the heap
based on the cgroup's memory allocation.

JDK8's support for running in a cgroup, especially in the way that Docker containers are generally
deployed by container schedulers like DC/OS, is limited to nonexistent.  GC (and JIT, but that's
generally less of a problem; this project does not set JIT threading, though a PR for the same
would be gladly accepted) threads are only sized based on the cgroup if CPU slices are used: CFS
or share-based timeslicing causes the JVM to size the pools based on the host's core count.  This
can result in application threads being crowded out by GC threads with implications for latency
(potentially including failed health checks and automatic restarts or extra instability in clustered
applications).

While JDK8's support for setting heap from the cgroup is more extensive, it is still limited in
comparison to later versions.  One can have a heap of 50% of the cgroup or 100% of the cgroup: nothing
in between.

Building
========

```
docker build -t $tag .
```

Usage
=====

The overall philosophy is that magic should be opt-in.  The wrapper will only set GC/heap-size options if:

* The relevant options (`-Xms`, `-Xmx`, and `-XX:ParallelGCThreads=` are not included in the arguments
AND
* The `JAVA_USE_CGROUP` environment variable is set to `yes`; if not so set, the wrapper will defer
entirely to the stock JVM.

Note that passing either of `-Xms` or `-Xmx` will prevent the heap-size determination from occurring
(most server usage of the JVM should set both to the same value).

Heuristics Implemented
======================

The heuristic used for GC thread pool sizing is similar to that described in the JDK8 documentation:
* CPU allocation is determined using either `cfs_quota`/`cfs_period` or as a
fallback `share`/`1024` (both integer division).  If neither of these provides an allocation, then
the JVM is left to its own devices (CPU slices or host-wide).
* If the allocation is less than 1, that will be considered an allocation of 1.
* If the allocation is greater than 999, that will be considered an allocation of 999.
* If the detected allocation is more than 7 cores, then there will be approximately 0.625 GC threads
per core.
* For 5-7 cores, there will be 5 GC threads
* For 4 or fewer cores, there will be as many GC threads as there are cores.

The heuristic used for heap sizing is:

* cgroup limits of more than about a terabyte will be considered to be 1TB.
* If the cgroup limit is 2GB or less, the heap is `JAVA_HEAP_PERCENTAGE` (minimum of 1, maximum of
100; default of 90) percent of half the cgroup limit.
* If the cgroup limit is more than 2GB, the heap is `JAVA_HEAP_PERCENTAGE` (same interpretation as
with less than 2GB) percent of 1GB less than the cgroup limit.

| cgroup memory | `JAVA_HEAP_PERCENTAGE` | Heap size |
| --- | --- | --- |
| 512MB | 90 | 230.4MB (235929KB in fact) |
| 512MB | 67 | 171.5MB (175636KB) |
| 1GB | 90 | 460.8MB (471859KB) |
| 1GB | 75 | 768MB |
| 2GB | 90 | 921.6MB (943718KB) |
| 2GB | 60 | 614.4MB (629145KB) |
| 4GB | 90 | 2764.8MB (2831155KB) |
| 4GB | 55 | 1689.6MB (1730150KB) |
