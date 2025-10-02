import os
import sys
import signal
import ctypes

lib_syscall_path = "./libsyscall.so" 
lib_syscall = ctypes.CDLL(lib_syscall_path)

class GramineCheckpoint:
    def __init__(self):
        self.checkpoint_available = False
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
            # TODO(chuqi): remove debug
            print("Syscall interface ready via libsyscall")
                
        except OSError as e:
            print(f"Error: Could not load libsyscall.so: {e}")
            print(f"Make sure to compile it first: gcc -shared -fPIC -o libsyscall.so libsyscall.c")
            self.syscall_available = False
        except Exception as e:
            print(f"Error: Could not setup libsyscall interface: {e}")
            self.syscall_available = False

if __name__ == "__main__":
    print("This is a module, not a standalone script.")
    sys.exit(0)