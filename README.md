# Project 6: Memory Management
In this project, we'll implement memory management for an OS simulator. We'll implement a second-chance (CLOCK) page replacement algorithm. This simulation utilizes message queues for communication, a frame table for the allocation of memory, and a PCB to keep record of each process. At the end, it will display final statistics. 

# How to Run: 
1. run 'make' 
2. ./oss [-h] [-n proc] [-s simul] [-i intervalInMs] [-f logfile] 
3.  EX: ./oss -n 5 -s 2 -i 100000 -f logfile.txt

