import handler
import sys
import os
import json
import time

# Import checkpoint functionality
try:
    from gramine_checkpoint import (
        create_checkpoint, restore_checkpoint, checkpoint_available,
        serverless_function, setup_serverless_signals,
        CHECKPOINT_MODULES_LOADED, CHECKPOINT_READY_FOR_EXECUTION
    )
    CHECKPOINT_AVAILABLE = True
except ImportError:
    print("Warning: Gramine checkpoint functionality not available")
    CHECKPOINT_AVAILABLE = False
    
    # Mock checkpoint functions
    def create_checkpoint(point=None):
        print(f"Mock: create_checkpoint({point})")
        return True
    
    def restore_checkpoint():
        print("Mock: restore_checkpoint()")
        return True
    
    def checkpoint_available():
        return False
    
    def serverless_function(checkpoint_after_init=True):
        def decorator(func):
            return func
        return decorator
    
    def setup_serverless_signals():
        pass

def initialize_serverless_runtime():
    """Initialize the serverless runtime environment"""
    print("Initializing serverless runtime...")
    
    # Setup signal handlers for checkpoint/restore
    if CHECKPOINT_AVAILABLE:
        setup_serverless_signals()
    
    # Pre-initialize the handler module (this loads heavy modules)
    print("Pre-loading handler modules...")
    import handler  # This triggers handler.initialize_handler()
    
    # Create checkpoint after module initialization
    if CHECKPOINT_AVAILABLE and os.getenv('GRAMINE_SERVERLESS_CHECKPOINT') == '1':
        print("Creating checkpoint after module initialization...")
        create_checkpoint(CHECKPOINT_MODULES_LOADED)
    
    print("Serverless runtime initialization complete")

@serverless_function(checkpoint_after_init=True)
def main(event_dict=None):
    """Main function with checkpoint/restore support"""
    return handler.handler(event_dict, None)

def run_serverless_loop():
    """Run in serverless mode with checkpoint/restore"""
    initialize_serverless_runtime()
    
    print("Entering serverless execution loop...")
    
    request_count = 0
    
    while True:
        try:
            # In a real implementation, this would wait for actual requests
            # For demo, we simulate requests
            print(f"\n--- Request {request_count + 1} ---")
            
            if request_count == 0:
                # First request - create checkpoint after successful execution
                event = {"request_id": f"req_{request_count}", "demo": True}
                
                start_time = time.time()
                result = main(event)
                execution_time = time.time() - start_time
                
                print(f"First execution completed in {execution_time:.3f}s")
                print(f"Result: {json.dumps(result, indent=2)}")
                
                # Create checkpoint for subsequent requests
                if CHECKPOINT_AVAILABLE:
                    create_checkpoint(CHECKPOINT_READY_FOR_EXECUTION)
                
            else:
                # Subsequent requests - should be faster due to checkpoint
                event = {"request_id": f"req_{request_count}", "demo": True}
                
                start_time = time.time()
                result = main(event)
                execution_time = time.time() - start_time
                
                print(f"Execution completed in {execution_time:.3f}s (from checkpoint)")
                print(f"Result: {json.dumps(result, indent=2)}")
                
                # Restore to checkpoint for next request
                if CHECKPOINT_AVAILABLE and os.getenv('GRAMINE_AUTO_RESTORE') == '1':
                    restore_checkpoint()
            
            request_count += 1
            
            # Demo: stop after a few requests
            if request_count >= 3:
                print("\nDemo complete - stopping serverless loop")
                break
                
            # Simulate delay between requests
            time.sleep(1)
            
        except KeyboardInterrupt:
            print("\nShutting down serverless runtime...")
            break
        except Exception as e:
            print(f"Error in serverless loop: {e}")
            continue

def main_entry():
    """Main entry point - choose between serverless and standard mode"""
    if len(sys.argv) > 1 and sys.argv[1] == '--serverless':
        run_serverless_loop()
    elif len(sys.argv) > 1 and sys.argv[1] == '--checkpoint-test':
        # Test checkpoint functionality
        print("Testing checkpoint functionality...")
        initialize_serverless_runtime()
        
        print("Testing checkpoint creation...")
        if create_checkpoint(CHECKPOINT_READY_FOR_EXECUTION):
            print("Checkpoint creation successful")
            
            print("Testing checkpoint restore...")
            if restore_checkpoint():
                print("Checkpoint restore successful")
            else:
                print("Checkpoint restore failed")
        else:
            print("Checkpoint creation failed")
    else:
        # Standard single execution
        initialize_serverless_runtime()
        
        start_time = time.time()
        result = main(None)
        execution_time = time.time() - start_time
        
        print(f"Execution completed in {execution_time:.3f}s")
        print(json.dumps(result, indent=2))

if __name__ == "__main__":
    main_entry()