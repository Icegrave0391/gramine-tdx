#!/bin/bash
# Test script for Gramine-TDX Serverless Checkpoint/Restore functionality

set -e

echo "=== Gramine-TDX Serverless Checkpoint/Restore Test ==="

# Set environment variables for checkpoint/restore
export GRAMINE_SERVERLESS_CHECKPOINT=1
export GRAMINE_AUTO_RESTORE=1
export GRAMINE_CHECKPOINT_POINT=2

echo "Environment variables set:"
echo "  GRAMINE_SERVERLESS_CHECKPOINT=$GRAMINE_SERVERLESS_CHECKPOINT"
echo "  GRAMINE_AUTO_RESTORE=$GRAMINE_AUTO_RESTORE"
echo "  GRAMINE_CHECKPOINT_POINT=$GRAMINE_CHECKPOINT_POINT"

# Ensure we have the necessary files
if [ ! -f "gene1.txt" ]; then
    echo "Creating sample gene1.txt..."
    echo "ATCGATCGATCGATCGATCGATCGATCGATCGATCGATCG" > gene1.txt
fi

if [ ! -f "gene2.txt" ]; then
    echo "Creating sample gene2.txt..."
    echo "GCTAGCTAGCTAGCTAGCTAGCTAGCTAGCTAGCTAGCTA" > gene2.txt
fi

echo -e "\n=== Test 1: Standard Execution ==="
echo "Running standard execution mode..."
python3 __main__.py

echo -e "\n=== Test 2: Checkpoint Test Mode ==="
echo "Testing checkpoint creation and restore..."
python3 __main__.py --checkpoint-test

echo -e "\n=== Test 3: Serverless Mode ==="
echo "Running in serverless mode with checkpoint/restore..."
python3 __main__.py --serverless

echo -e "\n=== Test 4: Gramine VM Execution ==="
echo "Testing with Gramine VM..."

# Rebuild manifest if needed
if [ ! -f "python.manifest" ] || [ "python.manifest.template" -nt "python.manifest" ]; then
    echo "Rebuilding manifest..."
    make clean && make
fi

# Test with Gramine VM
echo "Running with gramine-vm..."
gramine-vm python __main__.py --checkpoint-test

echo -e "\n=== Test 5: Performance Comparison ==="
echo "Comparing cold start vs warm start performance..."

echo "Cold start (standard execution):"
time python3 __main__.py

echo -e "\nWarm start simulation (with checkpoint):"
time python3 __main__.py --checkpoint-test

echo -e "\n=== Test Complete ==="
echo "Check the output above for:"
echo "1. Successful checkpoint creation"
echo "2. Successful checkpoint restore"
echo "3. Performance improvements in warm starts"
echo "4. Gramine VM compatibility"

# Show results
if [ -f "result.txt" ]; then
    echo -e "\n=== Function Result ==="
    cat result.txt | python3 -m json.tool
fi
