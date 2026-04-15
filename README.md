# Multi-Container Runtime with Kernel Memory Monitor

## 1. Team Information

- **Member 1:** Ankita Vellara - PES2UG24AM021
- **Member 2:** K Ashritha Reddy - PES2UG24AM071

---

## 2. Build, Load, and Run Instructions

### Environment  

This project must be run on:

- Ubuntu 22.04 or 24.04
- Inside a VM
- Secure Boot disabled
- Not on WSL

Install dependencies:  

```bash
sudo apt update
sudo apt install -y build-essential linux-headers-$(uname -r)
```

**Build**  
All build steps are run from the boilerplate/ directory.

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

**Prepare the Root Filesystem**  
From the repository root:

```bash
mkdir rootfs-base
wget https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/alpine-minirootfs-3.20.3-x86_64.tar.gz
tar -xzf alpine-minirootfs-3.20.3-x86_64.tar.gz -C rootfs-base

cp -a ./rootfs-base ./rootfs-alpha
cp -a ./rootfs-base ./rootfs-beta
```
If a helper workload should run inside a container, copy it into that container rootfs before launch:

```bash
cp boilerplate/memory_hog ./rootfs-alpha/
cp boilerplate/cpu_hog ./rootfs-alpha/
cp boilerplate/io_pulse ./rootfs-beta/
```

**Load the Kernel Module**  
From boilerplate/:

```bash
sudo insmod monitor.ko
ls -l /dev/container_monitor
dmesg | tail
```
Expected result:

- monitor.ko loads successfully
- /dev/container_monitor is created
- dmesg shows that the container monitor module was loaded

**Start the Supervisor**  
From boilerplate/:

```bash
sudo ./engine supervisor ../rootfs-base
```
The supervisor is a long-running parent process. It creates the control socket, accepts CLI requests, launches containers, registers them with the kernel monitor, collects logs, and reaps exited children.

**Launch Containers**  
Open another terminal in boilerplate/.  
Start two background containers:  

```bash
sudo ./engine start alpha ../rootfs-alpha /bin/sh --soft-mib 48 --hard-mib 80
sudo ./engine start beta ../rootfs-beta /bin/sh --soft-mib 64 --hard-mib 96
```

List tracked containers:  

```bash
sudo ./engine ps
```

Inspect logs for one container:  

```bash
sudo ./engine logs alpha
```

Stop containers:  

```bash
sudo ./engine stop alpha
sudo ./engine stop beta
```

**Run Memory Test**    
Copy the memory workload into one container rootfs:

```bash
cp boilerplate/memory_hog ./rootfs-alpha/
```

Start the container with the memory workload:

```bash
sudo ./engine start memtest ../rootfs-alpha /memory_hog --soft-mib 48 --hard-mib 80
```

Inspect the container log:

```bash
sudo ./engine logs memtest
```

Inspect kernel messages:

```bash
dmesg | tail -n 30
``` 
Expected behavior:

- the process gradually increases RSS
- the kernel module logs a soft-limit warning once the soft threshold is crossed
- the process is killed when the hard threshold is crossed

**Run Scheduling Experiments**  
Copy workloads into container rootfs directories:

```bash
cp boilerplate/cpu_hog ./rootfs-alpha/
cp boilerplate/io_pulse ./rootfs-beta/
```

Launch two workloads concurrently:

```bash
sudo ./engine start cpu1 ../rootfs-alpha /cpu_hog --nice 5
sudo ./engine start io1 ../rootfs-beta /io_pulse --nice 0
```

Inspect logs and compare behavior:

```bash
sudo ./engine logs cpu1
sudo ./engine logs io1
sudo ./engine ps
```

**Shutdown and Cleanup**  
Stop all containers:

```bash
sudo ./engine stop cpu1
sudo ./engine stop io1
sudo ./engine stop memtest
```

Stop the supervisor with ```Ctrl+C``` in the supervisor terminal.  

Unload the kernel module:

```bash
sudo rmmod monitor
dmesg | tail
```
Optional cleanup:

```bash
cd boilerplate
make clean
```

## 3. Demo with Screenshots

1. Multi-container supervision
Show two or more containers running under one supervisor process.

Caption: One supervisor process managing multiple container instances concurrently.

2. Metadata tracking
Show the output of ./engine ps.

Caption: Supervisor metadata table showing container ID, PID, state, start time, and log file.

3. Bounded-buffer logging
Show the container log file output and evidence that the logging pipeline is active.

Caption: Container stdout/stderr captured through the bounded-buffer logging pipeline and written into persistent log files.

4. CLI and IPC
Show a CLI command being issued and the supervisor responding.

Caption: CLI request sent to the supervisor over the control IPC channel and acknowledged by the supervisor.

5. Soft-limit warning
Show dmesg or monitor output for a soft-limit event.

Caption: Kernel monitor warning generated when container RSS crossed the soft memory threshold.

6. Hard-limit enforcement
Show the container being killed after exceeding the hard limit and the supervisor reflecting the termination.

Caption: Kernel monitor hard-limit enforcement causing process termination and updated supervisor state.

7. Scheduling experiment
Show terminal outputs or measurements comparing workloads or priorities.

Caption: Observable scheduling differences between concurrent workloads under different CPU priorities or workload types.

8. Clean teardown
Show that there are no zombie processes and that shutdown completes cleanly.

Caption: Supervisor shutdown with containers reaped, logger thread exiting, and no lingering zombie processes.





