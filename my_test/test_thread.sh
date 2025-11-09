#!/bin/sh

if [ "$1" = "-v" ]; then
    # EXECUTABLE="./mdriver" maybe later?
    OPTION="VERBOSE=1"
else
    OPTION=""
fi

TEST_SCRIPT_DIR=/workspace/my_test/
THREADS_DIR=/workspace/threads/build/
THREADS_TEST_DIR=tests/threads/

cd $THREADS_DIR

while true; do
    selected=$(find "$THREADS_TEST_DIR" -name '*.result' | fzf)

    if [ -z "$selected" ]; then
        echo "No test selected. Exiting..."
        exit 0
    fi

    removing=$(echo "$selected" | sed 's/\.result$/.output/')

    make_result=$(echo "$selected" | sed 's/\.result$/.result/')
    make_output=$(echo "$selected" | sed 's/\.result$/.output/')
    make_error=$(echo "$selected" | sed 's/\.result$/.errors/')

    rm $removing                          # remove prev test result
    rm $TEST_SCRIPT_DIR"result"   # remove log
    rm $TEST_SCRIPT_DIR"output"   # remove log
    rm $TEST_SCRIPT_DIR"error"    # remove log

    make $selected $OPTION

    cat $make_result > $TEST_SCRIPT_DIR"result"
    cat $make_output > $TEST_SCRIPT_DIR"output"
    cat $make_error > $TEST_SCRIPT_DIR"error"
done
