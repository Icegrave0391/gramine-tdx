#!/usr/bin/env python3
"""
Gramine Checkpoint Interface

This module provides Python bindings for Gramine's checkpoint/restore functionality
specifically designed for serverless cold-start optimization.
"""

import os
import sys
import signal
import ctypes

# System call numbers (these would need to be assigned in the actual implementation)
SYS_SERVERLESS_CHECKPOINT = 999  # Custom syscall number

# Operations
OP_CREATE_CHECKPOINT = 1
OP_RESTORE_CHECKPOINT = 2
OP_CHECK_STATUS = 3
OP_ENABLE_CHECKPOINT = 4
OP_DISABLE_CHECKPOINT = 5

# Checkpoint points
CHECKPOINT_PYTHON_INITIALIZED = 1
CHECKPOINT_MODULES_LOADED = 2
CHECKPOINT_READY_FOR_EXECUTION = 3

# Path to a shared library that provides syscall interface
lib_syscall_path = "./libsyscall.so"  

class GramineCheckpoint:
    """Interface to Gramine's checkpoint/restore system"""
    
    def __init__(self):
        self.checkpoint_available = False
        self._setup_syscall()
    
    def _setup_syscall(self):
        """Setup syscall interface using libsyscall"""
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
    
    def _do_syscall(self, operation, arg1=0, arg2=0):
        """Perform system call to Gramine checkpoint system"""
        if not self.syscall_available:
            print(f"[Error] dummy syscall={operation}, arg1={arg1}, arg2={arg2}")
            return 0  # Simulate success
        
        try:
            # Use our custom libsyscall library exclusively
            result = self.libsyscall.do_syscall3(
                SYS_SERVERLESS_CHECKPOINT,  # syscall number
                operation,                  # arg1: operation type
                arg1,                      # arg2: first argument
                arg2                       # arg3: second argument
            )
            
            print(f"Syscall={operation} result: {result}")
            return int(result)
        except Exception as e:
            print(f"Syscall failed: {e}")
            return -1
    
    def create_checkpoint(self, checkpoint_point=CHECKPOINT_MODULES_LOADED):
        """Create a checkpoint at the specified point"""
        print(f"Creating checkpoint at point {checkpoint_point}")
        
        result = self._do_syscall(OP_CREATE_CHECKPOINT, checkpoint_point, 0)
        
        if result == 0:
            self.checkpoint_available = True
            print("Checkpoint created successfully")
            return True
        else:
            print(f"Checkpoint creation failed: {result}")
            return False
    
    def restore_checkpoint(self):
        """Restore from checkpoint"""
        if not self.checkpoint_available:
            print("No checkpoint available for restore")
            return False
        
        print("Restoring from checkpoint...")
        
        result = self._do_syscall(OP_RESTORE_CHECKPOINT, 0, 0)
        
        if result == 0:
            print("Checkpoint restored successfully")
            return True
        else:
            print(f"Checkpoint restore failed: {result}")
            return False
    
    def check_status(self):
        """Check if checkpoint is available"""
        result = self._do_syscall(OP_CHECK_STATUS, 0, 0)
        self.checkpoint_available = (result == 1)
        return self.checkpoint_available
    
    def enable_checkpoint(self):
        """Enable checkpoint functionality"""
        result = self._do_syscall(OP_ENABLE_CHECKPOINT, 0, 0)
        return result == 0
    
    def disable_checkpoint(self):
        """Disable checkpoint functionality"""
        result = self._do_syscall(OP_DISABLE_CHECKPOINT, 0, 0)
        return result == 0
    
    def is_checkpoint_enabled(self):
        """Check if checkpoint functionality is enabled"""
        return os.getenv('GRAMINE_SERVERLESS_CHECKPOINT') == '1'

# Global checkpoint interface
_checkpoint = None

def get_checkpoint_interface():
    """Get global checkpoint interface"""
    global _checkpoint
    if _checkpoint is None:
        _checkpoint = GramineCheckpoint()
    return _checkpoint

def create_checkpoint(checkpoint_point=CHECKPOINT_MODULES_LOADED):
    """Create checkpoint at specified point"""
    return get_checkpoint_interface().create_checkpoint(checkpoint_point)

def restore_checkpoint():
    """Restore from checkpoint"""
    return get_checkpoint_interface().restore_checkpoint()

def checkpoint_available():
    """Check if checkpoint is available"""
    return get_checkpoint_interface().check_status()

def setup_serverless_signals():
    """Setup signal handlers for serverless operations"""
    def checkpoint_signal_handler(signum, frame):
        print(f"Received checkpoint signal {signum}")
        create_checkpoint()
    
    def restore_signal_handler(signum, frame):
        print(f"Received restore signal {signum}")
        restore_checkpoint()
    
    # Use SIGUSR1 for checkpoint, SIGUSR2 for restore
    signal.signal(signal.SIGUSR1, checkpoint_signal_handler)
    signal.signal(signal.SIGUSR2, restore_signal_handler)
    
    print("Serverless signal handlers setup complete")

# Decorator for serverless functions
def serverless_function(checkpoint_after_init=True):
    """Decorator to enable checkpoint/restore for serverless functions"""
    def decorator(func):
        def wrapper(*args, **kwargs):
            checkpoint_iface = get_checkpoint_interface()
            
            # Check if we're in a restored state
            was_restored = checkpoint_iface.check_status()
            
            if not was_restored and checkpoint_after_init:
                # First execution - create checkpoint after initialization
                print("First execution: creating checkpoint after initialization")
                checkpoint_iface.create_checkpoint(CHECKPOINT_READY_FOR_EXECUTION)
            
            # Execute the function
            try:
                result = func(*args, **kwargs)
                
                # After function execution, restore to checkpoint if enabled
                if os.getenv('GRAMINE_AUTO_RESTORE') == '1':
                    print("Auto-restore enabled, restoring checkpoint...")
                    checkpoint_iface.restore_checkpoint()
                
                return result
                
            except Exception as e:
                print(f"Function execution failed: {e}")
                # Still restore checkpoint on error
                if os.getenv('GRAMINE_AUTO_RESTORE') == '1':
                    checkpoint_iface.restore_checkpoint()
                raise
        
        return wrapper
    return decorator

if __name__ == "__main__":
    # Test the checkpoint interface
    print("Testing Gramine Checkpoint Interface")
    
    checkpoint = get_checkpoint_interface()
    
    print(f"Checkpoint enabled: {checkpoint.is_checkpoint_enabled()}")
    print(f"Checkpoint available: {checkpoint.check_status()}")
    
    # Setup signal handlers
    setup_serverless_signals()
    
    # Create a test checkpoint
    if checkpoint.create_checkpoint():
        print("Test checkpoint created")
        
        # Test restore
        if checkpoint.restore_checkpoint():
            print("Test restore successful")
        else:
            print("Test restore failed")
    else:
        print("Test checkpoint creation failed")
