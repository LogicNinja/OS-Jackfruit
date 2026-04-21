# Multi-Container Runtime with Kernel Memory Monitor

## 1. Team Information

- **Member 1:** Ankita Vellara - PES2UG24AM021
- **Member 2:** K Ashritha Reddy - PES2UG24AM071

---

## 2. Build, Load, and Run Instructions  

### Install Dependencies

```bash
sudo apt update
sudo apt install -y build-essential linux-headers-$(uname -r)
```
**Build the Project**  

Run all build commands from the boilerplate/ directory:  

```bash
cd boilerplate
make
```
This builds:  

- engine
- memory_hog
- cpu_hog
- io_pulse
- monitor.ko

**Rebuild Workloads as Static Binaries**

Since containers use an Alpine root filesystem:  

```bash
gcc -O2 -Wall -static -o memory_hog memory_hog.c
gcc -O2 -Wall -static -o cpu_hog cpu_hog.c
gcc -O2 -Wall -static -o io_pulse io_pulse.c
```
**Root Filesystem Setup**

Root filesystems are stored inside boilerplate/:  

```
rootfs-base/
rootfs-alpha/
rootfs-beta/
```
If these directories are not already present, create them:  

```bash
mkdir rootfs-base
wget https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/alpine-minirootfs-3.20.3-x86_64.tar.gz
tar -xzf alpine-minirootfs-3.20.3-x86_64.tar.gz -C rootfs-base
cp -a rootfs-base rootfs-alpha
cp -a rootfs-base rootfs-beta
```
Copy Workloads into Root Filesystems 
```bash
cp memory_hog rootfs-alpha/
cp memory_hog rootfs-beta/
cp cpu_hog rootfs-alpha/
cp cpu_hog rootfs-beta/
cp io_pulse rootfs-beta/
```
**Load the Kernel Module**

```bash
sudo insmod monitor.ko
lsmod | grep monitor
ls -l /dev/container_monitor
sudo dmesg | tail -5
```

Verify:

- module is loaded
- /dev/container_monitor exists
- kernel logs show successful initialization
  
**Start the Supervisor (Terminal 1)**

```bash
sudo ./engine supervisor ./rootfs-base
```
The supervisor:  

- manages containers
- initializes logging
- opens /dev/container_monitor
- waits for CLI commands
  
**Launch Containers (Terminal 2)**

Open a new terminal in boilerplate/:  

```bash
sudo ./engine start alpha ./rootfs-alpha /memory_hog --soft-mib 100 --hard-mib 200
sudo ./engine start beta  ./rootfs-beta  /memory_hog --soft-mib 100 --hard-mib 200
```

**Inspect Container Metadata**

```bash
sudo ./engine ps
```

Displays:

- container ID
- PID
- state
- start time
- log file path

**Inspect Logs**

```bash
sudo ./engine logs alpha
sudo ./engine logs beta
```

Or directly:  

```bash
ls logs/
cat logs/alpha.log
cat logs/beta.log
```

**Stop a Container**

```bash
sudo ./engine stop beta
sudo ./engine ps
```

**Soft Limit Memory Test**

```bash
sudo dmesg | grep "SOFT LIMIT"
```

Expected: warnings when memory exceeds soft limit  

**Hard Limit Memory Test**

```bash
sudo ./engine start hardtest ./rootfs-alpha /memory_hog --soft-mib 10 --hard-mib 20
sleep 5
sudo dmesg | grep -E "SOFT|HARD" | tail -10
sudo ./engine ps
```

Expected:

- HARD LIMIT message in kernel logs
- container marked as killed
  
**Scheduling Experiment**

```bash
sudo ./engine start cpu-hi ./rootfs-alpha /cpu_hog --nice -5
sudo ./engine start cpu-lo ./rootfs-beta /cpu_hog --nice 15
```

**Check logs:**

```
sudo ./engine logs cpu-hi
sudo ./engine logs cpu-lo
```

Observation:

- cpu-hi receives higher CPU share due to better priority
- cpu-lo still progresses due to scheduler fairness

**Cleanup**

Stop containers:  

```bash
sudo ./engine stop cpu-hi
sudo ./engine stop cpu-lo
```

**Check for leftover processes:**

```bash
ps aux | grep -E "engine|cpu_hog|memory_hog" | grep -v grep
```

**Stop the supervisor:**

```Ctrl + C```

**Check again:**

```bash
ps aux | grep -E "engine|cpu_hog|memory_hog" | grep -v grep
```

**Unload kernel module:**

```bash
sudo rmmod monitor
sudo dmesg | tail -5
```

**Optional cleanup:**

```bash
make clean
```

---
## 3. Demo with Screenshots

### 1. Multi-container supervision   
<img width="677" height="126" alt="Screenshot 2026-04-12 201340" src="https://github.com/user-attachments/assets/7cf83451-4a60-4883-9912-90dc6099053c" />

One supervisor process managing multiple container instances concurrently.

### 2. Metadata tracking  
<img width="677" height="174" alt="image" src="https://github.com/user-attachments/assets/f65e9b68-f032-4357-b8d7-0e44caf194ee" />  

Supervisor metadata table showing container ID, PID, state, start time, and log file.  

### 3. Bounded-buffer logging  
<img width="604" height="732" alt="image" src="https://github.com/user-attachments/assets/6c14e9c8-5134-43cc-a5eb-088959282cca" />  

Container stdout/stderr captured through the bounded-buffer logging pipeline and written into persistent log files.  

### 4. CLI and IPC  
<img width="510" height="56" alt="image" src="https://github.com/user-attachments/assets/2e03b2a4-f06b-4d54-a02c-42223368ed83" />

CLI request sent to the supervisor over the control IPC channel and acknowledged by the supervisor.  

### 5. Soft-limit warning  
<img width="686" height="232" alt="image" src="https://github.com/user-attachments/assets/5f027141-3ee3-47bf-8b63-ce780872ec8f" />  

Kernel monitor warning generated when container RSS crossed the soft memory threshold.  

### 6. Hard-limit enforcement  
<img width="683" height="399" alt="image" src="https://github.com/user-attachments/assets/cfc95c39-2e31-422e-8283-3385e5ea5e86" />  
  
Kernel monitor hard-limit enforcement causing process termination and updated supervisor state.  

### 7. Scheduling experiment  
<img width="663" height="472" alt="image" src="https://github.com/user-attachments/assets/550fac3e-3c41-48a1-9085-4effcea1b316" />  
 
Observable scheduling differences between concurrent workloads under different CPU priorities or workload types.  

### 8. Clean teardown  
<img width="681" height="824" alt="image" src="https://github.com/user-attachments/assets/243885d7-2910-4976-b13d-2c84239ef74b" />  

Supervisor shutdown with containers reaped, logger thread exiting, and no lingering zombie processes.  

---

## 4. Engineering Analysis

### Isolation Mechanisms
The runtime isolates containers using PID, UTS, and mount namespaces. PID namespaces give each container its own process view, UTS namespaces isolate the hostname, and mount namespaces separate filesystem mounts such as `/proc`. Filesystem isolation is provided using `chroot()`, which changes the visible root directory to the container's private rootfs.

Even with these mechanisms, all containers still share the same host kernel. That means scheduling, physical memory, device drivers, and kernel services are still global. This is why containers are lightweight compared to full virtual machines.

### Supervisor and Process Lifecycle
A long-running supervisor is useful because container management continues after process creation. The supervisor launches containers, tracks metadata, handles CLI requests, captures logs, and reaps children when they exit. Without a persistent parent, exited children could become zombies and container state would be difficult to manage reliably.

The supervisor also acts as the central point for signal delivery. Commands like `stop` go through the supervisor, which finds the correct container process, sends the signal, and updates internal state.

### IPC, Threads, and Synchronization
The project uses two IPC mechanisms: a UNIX domain socket for CLI-to-supervisor control messages, and pipes for container stdout/stderr logging. This separation works well because control traffic is request-response based, while logs are continuous streams.

The logging system uses a bounded buffer with a mutex and condition variables. The mutex protects shared buffer state such as `head`, `tail`, and `count`, while the condition variables block producers when the buffer is full and the consumer when it is empty. Without this synchronization, log data could be overwritten, partially read, or corrupted.

The metadata list is also protected by a mutex so that container state is updated safely while commands, shutdown logic, and child-exit handling run concurrently. In the kernel module, the monitored list is protected by a mutex for the same reason.

The bounded buffer avoids lost data and deadlock by making producers wait only when the buffer is full and consumers wait only when it is empty. During shutdown, waiting threads are woken up so they can exit cleanly instead of blocking forever.

---

## 5. Design Decisions and Tradeoffs

### Namespace Isolation
We used PID, UTS, and mount namespaces along with `chroot()` and separate rootfs copies for each container. The tradeoff is that `chroot()` is simpler to implement than `pivot_root()`, but it provides weaker filesystem isolation. We chose it because it was easier to build and debug while still demonstrating the required isolation mechanisms clearly.

### Supervisor Architecture
We used one long-running supervisor process to manage all containers. The tradeoff is that this creates a single point of failure, but it makes lifecycle management much simpler. We chose this design because it gives one central place for metadata tracking, signal handling, logging, and cleanup.

### IPC and Logging
We used a UNIX domain socket for CLI-to-supervisor control and pipes with a bounded buffer for log collection. The tradeoff is added implementation complexity compared to direct logging, but it separates control traffic from streaming output cleanly. We chose it because the two communication paths have different purposes and this design handles both more reliably.

### Kernel Monitor
We used a kernel module with `ioctl` registration to enforce memory limits. The tradeoff is that kernel code is harder to write and debug than user-space code. We chose it because memory enforcement is more reliable in kernel space, where the system has direct access to process memory information and termination control.

### Scheduling Experiments
We used simple synthetic workloads such as CPU-bound, I/O-bound, and memory-heavy programs. The tradeoff is that they are less realistic than full applications, but they make behavior easier to observe. We chose them because they highlight scheduler effects clearly without extra application complexity.

## 6. Scheduler Experiment Results

| Experiment | Configuration | Measurement | Observation | Conclusion |
| --- | --- | --- | --- | --- |
| Memory monitoring experiment | Two containers, `alpha` and `beta`, were launched with `/memory_hog` using `--soft-mib 100 --hard-mib 200` | Kernel log output showed soft-limit warning entries for both containers during execution | Both memory-heavy containers crossed the configured soft threshold and were detected by the kernel monitor | This confirms that the monitor correctly tracks container RSS and uses the soft limit as an early-warning mechanism before hard enforcement |
| CPU scheduling experiment | Two CPU-bound containers, `cpu-hi` and `cpu-lo`, were run concurrently using `/cpu_hog` | Logs for both containers showed steady progress from `elapsed=1` through `elapsed=10`, followed by successful completion | Both workloads continued to receive CPU time throughout the run, and neither container was starved while running in parallel | This shows that the Linux scheduler maintained fairness across competing CPU-bound processes while still allowing controlled comparison between differently configured workloads |





