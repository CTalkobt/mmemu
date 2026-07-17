#!/bin/bash
# Unified test runner wrapper - simplifies cross-backend testing
# Usage: ./run-tests.sh [-mmemu|-xmega65|-real|-all] [test-files...]

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Defaults
BACKENDS="-mmemu"
TEST_FILES=()
VERBOSE=""
MACHINE="c64"

# Parse arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        -mmemu|-xmega65|-real|-all)
            BACKENDS="$1"
            shift
            ;;
        -verbose|-v)
            VERBOSE="-verbose"
            shift
            ;;
        -machine)
            MACHINE="$2"
            shift 2
            ;;
        -help|-h)
            echo "Usage: ./run-tests.sh [options] [test-files...]"
            echo ""
            echo "Options:"
            echo "  -mmemu         Test on mmsim only (default)"
            echo "  -xmega65       Test on xemu-xmega65 only"
            echo "  -real          Test on real MEGA65 hardware"
            echo "  -all           Test on all available backends"
            echo "  -machine TYPE  Machine preset (default: c64)"
            echo "  -verbose, -v   Verbose output"
            echo "  -help, -h      Show this help"
            echo ""
            echo "Examples:"
            echo "  ./run-tests.sh                              # Test arithmetic.bin on mmsim"
            echo "  ./run-tests.sh -all tests/45gs02/*.bin      # All backends, all tests"
            echo "  ./run-tests.sh -xmega65 -machine mega65     # xemu only, MEGA65 machine"
            exit 0
            ;;
        *)
            TEST_FILES+=("$1")
            shift
            ;;
    esac
done

# Use default test if none specified
if [ ${#TEST_FILES[@]} -eq 0 ]; then
    TEST_FILES=("tests/45gs02/arithmetic.bin")
fi

# Check if test runner is built
if [ ! -f "./bin/mmemu-test-runner" ]; then
    echo -e "${RED}❌ Test runner not built${NC}"
    echo "Run: make -j 12 test-runner"
    exit 1
fi

# Determine if we need emulator
needs_emulator=false
if [[ "$BACKENDS" == "-mmemu" ]] || [[ "$BACKENDS" == "-all" ]]; then
    needs_emulator=true
fi

# Start emulator if needed
if $needs_emulator; then
    if [ ! -f "./bin/mmemu-cli" ]; then
        echo -e "${RED}❌ CLI not built${NC}"
        echo "Run: make -j 12 cli"
        exit 1
    fi

    echo -e "${YELLOW}Starting mmsim with serial monitor server...${NC}"
    timeout 120 ./bin/mmemu-cli -m "$MACHINE" --serial-monitor-port 6502 >/dev/null 2>&1 &
    EMU_PID=$!
    sleep 2

    # Check if emulator started successfully
    if ! kill -0 $EMU_PID 2>/dev/null; then
        echo -e "${RED}❌ Failed to start emulator${NC}"
        exit 1
    fi
fi

# Run tests
echo -e "${YELLOW}Running test runner...${NC}"
echo ""

if ./bin/mmemu-test-runner \
    $BACKENDS \
    -machine "$MACHINE" \
    $VERBOSE \
    "${TEST_FILES[@]}"; then

    RESULT=$?
    echo ""
    echo -e "${GREEN}✅ All tests passed${NC}"
else
    RESULT=$?
    echo ""
    echo -e "${RED}❌ Some tests failed${NC}"
fi

# Cleanup
if [ -n "$EMU_PID" ]; then
    echo -e "${YELLOW}Cleaning up...${NC}"
    kill $EMU_PID 2>/dev/null || true
    wait $EMU_PID 2>/dev/null || true
fi

exit $RESULT
