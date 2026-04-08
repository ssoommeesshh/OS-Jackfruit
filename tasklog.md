### Task 1: Multi-Container Runtime with Parent Supervisor

#Main goal: to implement a supervisor process which handles multiple containers at the same time

The supervisors tasks are :
- Create a container/s
- Run them simultaneously
- Track their lifetime and maintain thier meta data
- Handle cleanup, reap all children no zombie


Main functions changed/implemented: `run_supervisor()` and `child_fn()`

Workflow:
run_supervisor()
      ↓
receives request to start container
      ↓
clone(child_fn)  - clone is bascially a fork() but it provides more control, and allows u to specify a starting point for the process (in this case its child _fn)
      ↓
child_fn runs → sets up container
      ↓
container executes command
      ↓
supervisor tracks + reaps it


# `child_fn()`
this function defines what happens inside a container

```C(Simplified code)
dup2(log_fd, STDOUT_FILENO);
dup2(log_fd, STDERR_FILENO);

sethostname(cfg->id, strlen(cfg->id));

chroot(cfg->rootfs);
chdir("/");

mount("proc", "/proc", "proc", 0, NULL);

execl("/bin/sh", "/bin/sh", "-c", cfg->command, NULL);
```

- dup2() is a system call which redirects output from terminal to the log files, (dup2() - creates a duplicate of some open file descriptor)
- sethosename() - sets up UTS namespace to isolate each process 
- chroot() - sets up file system used by this process (also for isolation) - here it sees only `-/ rootfs-alpha` (if alpha process) not the system/host file system
- mount() - `/proc` to make ps, top other commands to work inside container
- execl() - to execute the process

# `run_supervisor()`
working: 
- Initialize system
- Listen for commands
- Create containers
- Track metadata
- Handle exits
- Shutdown cleanly





