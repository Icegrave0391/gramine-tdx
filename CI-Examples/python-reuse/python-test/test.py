import os
import sys
import signal
import ctypes

lib_syscall_path = "./libsyscall.so" 
lib_syscall = ctypes.CDLL(lib_syscall_path)

SYS_CR = 999  # Syscall number for checkpoint/restore
OP_CREATE_CHECKPOINT = 1
OP_RESTORE_CHECKPOINT = 2

class GramineCheckpoint:
    def __init__(self):
        self.checkpoint_available = False
        self.syscall_available = False
        self._setup_syscall()
            
    def _setup_syscall(self):
        try:
            # Load our custom libsyscall library
            self.libsyscall = ctypes.CDLL(lib_syscall_path)
            
            # Define the function signature for do_syscall3
            # long do_syscall3(long n, uintptr_t a1, uintptr_t a2, uintptr_t a3)
            self.libsyscall.do_syscall3.argtypes = [
                ctypes.c_long,     # syscall number
                ctypes.c_ulong,    # arg1 (uintptr_t)  
                ctypes.c_ulong,    # arg2 (uintptr_t)
                ctypes.c_ulong     # arg3 (uintptr_t)
            ]
            self.libsyscall.do_syscall3.restype = ctypes.c_long
            self.syscall_available = True
                
        except OSError as e:
            print(f"Error: Could not load libsyscall.so: {e}")
            print(f"Make sure to compile it first: gcc -shared -fPIC -o libsyscall.so libsyscall.c")
            self.syscall_available = False
        except Exception as e:
            print(f"Error: Could not setup libsyscall interface: {e}")
            self.syscall_available = False

    def do_CR(self, comm="C"):
        if comm not in ("C", "R"):
            raise ValueError("Invalid command for checkpoint/restore. Use 'C' or 'R'.")
        op = OP_CREATE_CHECKPOINT if comm == "C" else OP_RESTORE_CHECKPOINT
        if not self.syscall_available:
            print("Checkpoint/Restore interface not available.")
            return -1
        self.libsyscall.do_syscall3(SYS_CR, op, 0, 0)

global_counter = 0

if __name__ == "__main__":
    # try to use syscall 
    gcp = GramineCheckpoint()

    ##### do a checkpoint
    gcp.do_CR(comm="C")
    ##### checkpoint saved here #####
    
    global_counter += 1
    print(f"global_counter = {global_counter}", flush=True)
    
    ##### do a restore
    gcp.do_CR(comm="R")

    sys.exit(0)