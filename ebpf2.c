#include <linux/sched.h>
#include <linux/nsproxy.h>
#include <linux/pid_namespace.h>
#include <linux/utsname.h>

// Indexes of the config map
#define CONFIG_TASK_MODE 0
#define CONFIG_CONTAINER_MODE 1

// Must match the python program syscall id lis
enum syscall_id {
    SYS_EXIT,
    SYS_EXECVE,
    SYS_EXECVEAT,
    SYS_MMAP,
    SYS_MPROTECT,
    SYS_CLONE,
    SYS_FORK,
    SYS_VFORK,
    SYS_NEWSTAT,
    SYS_NEWFSTAT,
    SYS_NEWLSTAT,
    SYS_MKNOD,
    SYS_MKNODAT,
    SYS_DUP,
    SYS_DUP2,
    SYS_DUP3,
    SYS_MEMFD_CREATE,
    SYS_SOCKET,
    SYS_CLOSE,
    SYS_IOCTL,
    SYS_ACCESS,
    SYS_FACCESSAT,
    SYS_KILL,
    SYS_LISTEN,
    SYS_CONNECT,
    SYS_ACCEPT,
    SYS_ACCEPT4,
    SYS_BIND,
    SYS_GETSOCKETNAME,
    SYS_PRCTL,
    SYS_PTRACE,
    SYS_PROCESS_VM_WTRITEV,
    SYS_PROCESS_VM_READV,
    SYS_INIT_MODULE,
    SYS_FINIT_MODULE,
    SYS_DELETE_MODULE,
    SYS_SYMLINK,
    SYS_SYMLINKAT,
    SYS_GETDENTS,
    SYS_GETDENTS64,
    SYS_CREAT,
    SYS_OPEN,
    SYS_OPENAT,
    SYS_MOUNT,
    SYS_UMOUNT,
    SYS_UNLINK,
    SYS_UNLINKAT,
    SYS_SETUID,
    SYS_SETGID,
    SYS_SETREUID,
    SYS_SETREGID,
    SYS_SETRESUID,
    SYS_SETRESGID,
    SYS_SETFSUID,
    SYS_SETFSGID,
};

//ebpf event data
typedef struct {
    u32 real_pid;
    u32 ns_pid;
    u32 ns_id;
    u32 mnt_id;
    u64 ts;
    char comm[TASK_COMM_LEN];
    char uts_name[TASK_COMM_LEN];
    enum syscall_id call;
    char path[128];
} data_t;

typedef struct {
    char name[TASK_COMM_LEN];
} taskname_buf_t;

BPF_HASH(config_map, u32, u32);
//Holds pid and ns_id to track
BPF_HASH(pids_map, u32, u32);
//Used to get arguments at runtime from the python proram
BPF_HASH(taskname_buf, u32, taskname_buf_t);
BPF_PERF_OUTPUT(events);

static u32 get_task_pid_ns_id(struct task_struct *task) {
    return task->nsproxy->pid_ns_for_children->ns.inum;
}

static u32 get_task_ns_pid(struct task_struct *task) {
    return task->thread_pid->numbers[task->nsproxy->pid_ns_for_children->level].nr;
}

static char *get_task_uts_name(struct task_struct *task) {
    return task->nsproxy->uts_ns->name.nodename;
}

static u32 add_pid()
{
    u32 pid = bpf_get_current_pid_tgid();
    if (pids_map.lookup(&pid) == 0)
        pids_map.update(&pid, &pid);

    return pid;
}

static  void add_pid_fork(u32 pid)
{
    pids_map.update(&pid, &pid);
}

static void remove_pid()
{
    u32 pid = bpf_get_current_pid_tgid();
    if (pids_map.lookup(&pid) != 0)
        pids_map.delete(&pid);
}

static u32 add_pid_ns_if_needed() {
    struct task_struct *task;
    task = (struct task_struct *) bpf_get_current_task();
    u32 pid_ns = get_task_pid_ns_id(task);
    if (pids_map.lookup(&pid_ns) != 0)
        // Container pidns was already added to map
        return pid_ns;
    // If pid equals 1 - start tracing the container
    if (get_task_ns_pid(task) == 1) {
        // A new container was started, add pid namespace to map
        pids_map.update(&pid_ns, &pid_ns);
        return pid_ns;
    }
    return 0;
}

static void remove_pid_ns_if_needed() {
    struct task_struct *task;
    task = (struct task_struct *) bpf_get_current_task();
    u32 pid_ns = get_task_pid_ns_id(task);
    if (pids_map.lookup(&pid_ns) != 0) {
        // If pid equals 1 - stop tracing this pid namespace
        if (get_task_ns_pid(task) == 1) {
            pids_map.delete(&pid_ns);
        }
    }
}

static int get_config(u32 key)
{
    u32 *config = config_map.lookup(&key);

    if (config == NULL)
        return 0;

    return *config;
}

static  int container_mode()
{
    return get_config(CONFIG_CONTAINER_MODE);
}

static  int task_mode()
{
    return get_config(CONFIG_TASK_MODE);
}

static int is_container()
{
    struct task_struct *task;
    task = (struct task_struct *) bpf_get_current_task();
    u32 task_pid_ns = get_task_pid_ns_id(task);
    u32 * pid_ns = pids_map.lookup(&task_pid_ns);
    if (pid_ns == 0)
        return 0;

    return *pid_ns;
}

static int is_task()
{
    u32 task_pid = bpf_get_current_pid_tgid();
    u32 * pid = pids_map.lookup(&task_pid);
    if (pid == 0)
        return 0;

    return *pid;
}

//Usage of strcmp is not permitted in eBpf programs
static int comp_with_taskname_buf(const char *str_ptr)
{
    char str_a[TASK_COMM_LEN];
    char str_b[TASK_COMM_LEN];
    bpf_probe_read_str(str_a, TASK_COMM_LEN, str_ptr);
    u32 key = 0;
    taskname_buf_t *elem = (taskname_buf_t *) taskname_buf.lookup(&key);
    bpf_probe_read_str(str_b,TASK_COMM_LEN,elem->name);

    for(int i = 0; i< TASK_COMM_LEN; i++){
        if(str_a[i] != str_b[i])
            return 0;
        if(str_a[i] == '\0' && str_b[i] == '\0')
            return 1;
    }
    return 1;
}

//Current process matches the taskname provided
static int is_my_task()
{
    char name_ref[TASK_COMM_LEN];
    bpf_get_current_comm(name_ref, TASK_COMM_LEN);
    return comp_with_taskname_buf(name_ref);
}

//Current proces matches the container id provided
static int is_my_container()
{
    struct task_struct *task;
    task = (struct task_struct *)bpf_get_current_task();
    return comp_with_taskname_buf(get_task_uts_name(task));
}

//Fills the event data struct
static int init_data(data_t *data){
    struct task_struct *task;
    task = (struct task_struct *) bpf_get_current_task();
    char *uts_name = get_task_uts_name(task);
    if (uts_name)
        bpf_probe_read_str(&data->uts_name, TASK_COMM_LEN, uts_name);
    data->ns_pid = get_task_ns_pid(task);
    data->ns_id = get_task_pid_ns_id(task);
    data->real_pid = bpf_get_current_pid_tgid();
    data->ts = bpf_ktime_get_ns();
    bpf_get_current_comm(&data->comm, sizeof(data->comm));
    return 0;
}

int syscall__execve(struct pt_regs *ctx, const char __user *filename) {

    data_t data = {};
    u32 should_track = 0;
    if(container_mode() && is_my_container()){
        add_pid_ns_if_needed();
        should_track = is_container();
    } else if (task_mode() && is_my_task()){
        add_pid();
        should_track = 1;
    }else if(!container_mode() && !task_mode()){
        add_pid_ns_if_needed();
        should_track = is_container();
    }

    if(!should_track)
        return 0;

    init_data(&data);
    data.call = SYS_EXECVE;
    bpf_probe_read_str(data.path, sizeof(data.path), filename);
    events.perf_submit(ctx, &data, sizeof(data));
    return 0;
}

int syscall__execveat(struct pt_regs *ctx, const char __user *pathname) {

    data_t data = {};
    u32 should_track = 0;
    if(container_mode() && is_my_container()){
        add_pid_ns_if_needed();
        should_track = is_container();
    } else if (task_mode() && is_my_task()){
        add_pid();
        should_track = 1;
    }else if (!container_mode() && !task_mode()){
        add_pid_ns_if_needed();
        should_track = is_container();
    }

    if(!should_track)
        return 0;

    init_data(&data);
    data.call = SYS_EXECVEAT;
    bpf_probe_read_str(data.path, sizeof(data.path), pathname);
    events.perf_submit(ctx,&data, sizeof(data));
    return 0;
}

int trace_do_exit(struct pt_regs *ctx) {

    data_t data = {};
    u32 should_track = 0;
    if (task_mode() && is_task()){
        should_track = 1;
    }else if (is_container())
        should_track = 1;

    if(!should_track)
        return 0;
        
    init_data(&data);
    data.call = SYS_EXIT;
    remove_pid_ns_if_needed();
    remove_pid();
    events.perf_submit(ctx, &data, sizeof(data));
    return 0;
}

static int trace_generic(struct pt_regs *ctx, u32 id) {

    data_t data = {};
    u32 should_track = 0;
    if (task_mode() && is_task()){
        should_track = 1;
    }else if (is_container())
        should_track = 1;

    if(!should_track)
        return 0;

    init_data(&data);
    data.call = id;
    events.perf_submit(ctx, &data, sizeof(data));
    return 0;
}

//Needs further testing
static int trace_ret_fork_generic(struct pt_regs *ctx) {

    if(container_mode())
        return 0;

    u32 retval = PT_REGS_RC(ctx);
    if(is_my_task() && retval > 0)
        add_pid_fork(retval);
    return 0;
}

#define TRACE_SYSCALL(name, id)                                         \
int syscall__##name(struct pt_regs *ctx)                                \
{                                                                       \
    trace_generic(ctx, id);                                                  \
    return 0;                                                           \
}                                                                       \

#define TRACE_RET_FORK(name, id)                                        \
int trace_ret_##name(struct pt_regs *ctx)                               \
{                                                                       \
    trace_ret_fork_generic(ctx);                                    \
    return 0;                                                           \
}                                                                       \


TRACE_RET_FORK(fork, SYS_FORK);

TRACE_RET_FORK(vfork, SYS_VFORK);

TRACE_RET_FORK(clone, SYS_CLOSE);

TRACE_SYSCALL(mmap, SYS_MMAP);

TRACE_SYSCALL(mprotect, SYS_MPROTECT);

TRACE_SYSCALL(clone, SYS_CLONE);

TRACE_SYSCALL(fork, SYS_FORK);

TRACE_SYSCALL(vfork, SYS_VFORK);

TRACE_SYSCALL(newstat, SYS_NEWSTAT);

TRACE_SYSCALL(newfstat, SYS_NEWFSTAT);

TRACE_SYSCALL(newlstat, SYS_NEWLSTAT);

TRACE_SYSCALL(mknod, SYS_MKNOD);

TRACE_SYSCALL(mknodat, SYS_MKNODAT);

TRACE_SYSCALL(dup, SYS_DUP);

TRACE_SYSCALL(dup2, SYS_DUP2);

TRACE_SYSCALL(dup3, SYS_DUP3);

TRACE_SYSCALL(memfd_create, SYS_MEMFD_CREATE);

TRACE_SYSCALL(socket, SYS_SOCKET);

TRACE_SYSCALL(close, SYS_CLOSE);

TRACE_SYSCALL(ioctl, SYS_IOCTL);

TRACE_SYSCALL(access, SYS_ACCESS);

TRACE_SYSCALL(faccessat, SYS_FACCESSAT);

TRACE_SYSCALL(kill, SYS_KILL);

TRACE_SYSCALL(listen, SYS_LISTEN);

TRACE_SYSCALL(connect, SYS_CONNECT);

TRACE_SYSCALL(accept, SYS_ACCEPT);

TRACE_SYSCALL(accept4, SYS_ACCEPT4);

TRACE_SYSCALL(bind, SYS_BIND);

TRACE_SYSCALL(getsockname, SYS_GETSOCKETNAME);

TRACE_SYSCALL(prctl, SYS_PRCTL);

TRACE_SYSCALL(ptrace, SYS_PTRACE);

TRACE_SYSCALL(process_vm_writev, SYS_PROCESS_VM_WTRITEV);

TRACE_SYSCALL(process_vm_readv, SYS_PROCESS_VM_READV);

TRACE_SYSCALL(init_module, SYS_INIT_MODULE);

TRACE_SYSCALL(finit_module, SYS_FINIT_MODULE);

TRACE_SYSCALL(delete_module, SYS_DELETE_MODULE);

TRACE_SYSCALL(symlink, SYS_SYMLINK);

TRACE_SYSCALL(symlinkat, SYS_SYMLINKAT);

TRACE_SYSCALL(getdents, SYS_GETDENTS);

TRACE_SYSCALL(getdents64, SYS_GETDENTS64);

TRACE_SYSCALL(creat, SYS_CREAT);

TRACE_SYSCALL(open, SYS_OPEN);

TRACE_SYSCALL(openat, SYS_OPENAT);

TRACE_SYSCALL(mount, SYS_MOUNT);

TRACE_SYSCALL(umount, SYS_UMOUNT);

TRACE_SYSCALL(unlink, SYS_UNLINK);

TRACE_SYSCALL(unlinkat, SYS_UNLINKAT);

TRACE_SYSCALL(setuid, SYS_SETUID);

TRACE_SYSCALL(setgid, SYS_SETGID);

TRACE_SYSCALL(setreuid, SYS_SETREUID);

TRACE_SYSCALL(setregid, SYS_SETREGID);

TRACE_SYSCALL(setresuid, SYS_SETRESUID);

TRACE_SYSCALL(setresgid, SYS_SETRESGID);

TRACE_SYSCALL(setfsuid, SYS_SETFSUID);

TRACE_SYSCALL(setfsgid, SYS_SETFSGID);
