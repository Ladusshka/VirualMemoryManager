# Virtual Memory Manager Simulation

A user-space simulation of simplified operating-system virtual memory management, implemented in C/C++ as a university programming assignment.

The project models process address spaces, two-level i386-style paging, physical page allocation, virtual-to-physical address translation, and process creation. Simulated processes run as POSIX threads and access memory through a `CCPU` abstraction rather than directly dereferencing virtual addresses.

## Project Goals

The main goal is to demonstrate the core mechanisms used by an operating system to manage virtual memory:

- allocation and release of physical page frames;
- per-process virtual address spaces;
- two-level page tables;
- virtual-to-physical address translation;
- controlled memory growth and shrinking;
- process creation using POSIX threads;
- optional address-space copying similar to `fork()`;
- page-fault handling and, in an advanced implementation, copy-on-write.

This is a pure user-space simulation. It does not use privileged CPU instructions, kernel modules, or assembly code.

## Simulation Model

### CPU

The simulated CPU is represented by the provided `CCPU` class and a project-specific derived class. Each simulated process owns one CPU instance responsible for translating its virtual addresses and mediating memory reads and writes.

### Processes

Each simulated process is represented by a POSIX thread. A process has its own page-table hierarchy and virtual heap.

The simulation supports up to `PROCESS_MAX = 64` concurrently running processes, including the initial process.

### Virtual Address Space

Only the process heap is simulated. Its virtual address range starts at address `0` and extends to:

```text
allocated_pages * 4096 - 1
```

The page size is fixed at 4 KiB.

### Physical Memory

The memory manager receives one contiguous block of memory during initialization. All data pages and page-table structures must be allocated from this managed block.

Only a small amount of auxiliary memory may be obtained through the regular C/C++ runtime, so large process allocations cannot be backed by `new` or `malloc`.

## Paging Architecture

The address translation model follows basic 32-bit i386 paging:

- 4 KiB pages;
- 12-bit page offset;
- two levels of page tables;
- 1024 entries per page directory;
- 1024 entries per page table;
- 32-bit page-table entries.

Relevant page-table flags include:

| Flag | Meaning |
|---|---|
| `BIT_PRESENT` | The page or page table is present. |
| `BIT_WRITE` | The mapped page is writable. |
| `BIT_USER` | The page is accessible from the simulated user process. |
| `BIT_REFERENCED` | The page has been accessed. |
| `BIT_DIRTY` | The page has been written to. |

## Required Interface

The solution is centered around the following components.

### `MemMgr`

Initializes the memory-management structures, creates the initial simulated CPU, runs the initial process, waits for all child processes to finish, and releases allocated resources.

### `GetMemLimit`

Returns the current size of the process heap in pages.

### `SetMemLimit`

Changes the size of the process heap. It must support both expansion and shrinking while correctly updating page tables and physical-page ownership.

### `NewProcess`

Creates a new simulated process and runs the supplied entry function in a POSIX thread.

The `copyMem` argument determines whether the child starts with:

- an empty address space; or
- a copy of the parent process address space.

### `pageFaultHandler`

Handles failed address translations. A basic implementation may reject invalid access. An advanced implementation can use this method to resolve copy-on-write faults.

The provided `CCPU` implementation already supplies operations such as `ReadInt`, `WriteInt`, and virtual-to-physical address translation.

## Process Creation Modes

### Empty Address Space

When `copyMem` is `false`, the child process starts with a memory limit of zero and allocates its own pages independently.

### Copied Address Space

When `copyMem` is `true`, the child receives the logical contents of the parent address space.

A straightforward implementation may duplicate all mapped pages. A memory-efficient implementation uses copy-on-write so that the parent and child initially share read-only pages and create private copies only after a write fault.

## Technical Constraints

- Language: C/C++
- Threading: POSIX Threads (`pthread`)
- Page size: 4096 bytes
- Maximum concurrent processes: 64
- STL is not available in the target evaluation environment
- C++11 thread APIs are not available
- Data pages and page tables must remain inside the memory block supplied to `MemMgr`
- Memory accesses are performed through the `CCPU` interface
- Integer reads and writes are assumed to be 4-byte aligned

## Repository Structure

A typical repository layout may look like this:

```text
.
├── solution.cpp        # Memory manager and CCPU-derived implementation
├── tests/              # Local test cases, if included
├── assignment/         # Original interface or task materials, if permitted
└── README.md
```

The exact structure may differ depending on which supporting files are included in the repository.

## Building

A typical local build with POSIX thread support can be performed with:

```bash
g++ -std=c++17 -Wall -Wextra -pedantic -pthread solution.cpp -o memory-manager
```

The university evaluation environment may use different compiler flags and conditional-compilation blocks. The submitted `solution.cpp` should therefore remain compatible with the provided Progtest interface.

## Testing

Use the tests supplied with the assignment to verify at least the following cases:

- increasing and decreasing a process memory limit;
- reading and writing valid mapped addresses;
- rejecting access outside the allocated address space;
- creating multiple processes concurrently;
- reclaiming memory after process termination;
- creating a process with an empty address space;
- copying a parent address space when `copyMem` is enabled;
- preserving isolation between parent and child writes;
- handling allocation failure without corrupting internal state;
- remaining within the 64-process concurrency limit.

## Educational Context

This repository was created as a university assignment focused on operating systems and virtual memory. It is intended to illustrate memory-management principles in a controlled simulation rather than provide a production-ready allocator or operating-system kernel.

## Academic Integrity

The repository is published for educational and portfolio purposes. Students working on the same or a similar assignment should follow their university's academic-integrity rules and avoid submitting this implementation as their own work.

## License

Add an appropriate license before distributing or reusing the source code. Also verify whether the university permits publication of the assignment template, test files, and provided framework code.
