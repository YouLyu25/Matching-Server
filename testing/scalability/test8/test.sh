#! /bin/bash

TEST_FILE1=test2.xml
TEST_FILE2=test3.xml
NUM_REQUESTS=50

for ((i = 0; i < $NUM_REQUESTS; ++i))
do
	./client $TEST_FILE1 &
	./client $TEST_FILE2 &
done

#wait
