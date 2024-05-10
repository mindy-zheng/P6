// Name: Mindy Zheng
// Date: 5/1/2024 
#include <unistd.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/time.h> 
#include <math.h>
#include <signal.h> 
#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h> 
#include <sys/wait.h>
#include <time.h> 
#include <stdbool.h> 
#include <sys/msg.h>
#include <errno.h>
#include <stdarg.h> 
#include <limits.h> 
#include <stdint.h> 

void help(); 

// Key for shared memory
#define SH_KEY 89918991
#define PERMS 0644
unsigned int shm_clock[2] = {0, 0}; // Creating simulated clock in shared memory

// Defined variables 
#define HALF_NANO 5000000000	// Condition for printing the process table; half a nano
#define ONE_SEC 1000000000	// 1,000,000,000 (billion) nanoseconds is equal to 1 second
#define NANO_INCR 100000	// 0.1 millisecond 
#define FRAME_SIZE 256 	// require a frame table of 256 structures  
int total_launched = 0, total_terminated = 0, second_passed = 0, half_passed = 0;
unsigned long long launch_passed = 0;  

// PCB Structure 
typedef struct PCB { 
	int occupied; 		// Either true or false	
	pid_t pid; 			// Process ID of current assigned child 
	int startSeconds; 	// Start seconds when it was forked
	int startNano; 		// Start nano when it was forked
	int pages[64];		// Page table requiring 64 entries
	int childMemoryRefs; 	// memory references made by child process
	int targetPage;			// the page table index that the process wants to req in
	int isBlocked;			// Process is blocked and in queue 
	int isRequestAtFront; 	// indicates whether the process has a request at the front of the queue  
} PCB; 

struct PCB processTable[18];

// Frame Table Structure
typedef struct FT { 
	int dirtyBit;		// Indicates whether the frame has been written to 
	int pageOccupied; 	// Which page is in this frame 
	pid_t pageID; 		
	int headOfQueue; 	// Indicates whether frame is at the head of the FIFO queue 
} FT;

struct FT frameTable[FRAME_SIZE]; 

void displayPCB(); 
void displayMemory(); 

// FIFO queues
int FIFO_queue[18]; 
// Saves the time for requests
unsigned int front_queue_time[2] = {0, 0}; 

// Message Queue Structure 
typedef struct msgbuffer { 
	long mtype;
	int memory_address;	
	int opt_type;			// Option type: worker wants to read(1) or write(0) 
	int termination_flag; 
} msgbuffer;

msgbuffer buf; 
int msqid; 

unsigned shm_id;
unsigned *shm_ptr; 

// Process statistics variables 
int total_refs = 0; // Total number of memory references made 
int total_page = 0; // Total number of page faults 

	
static void myhandler(int);
static int setupinterrupt(void);
static int setupitimer(void);
void terminate();
void process_child_requests(int k);
void handle_page(int rw, int k, int r_adr, int p_adr);
void handle_fault(int rw, int k, int r_adr, int p_adr);


// Limit logfile from reaching more than 10k lines 
int lfprintf(FILE *stream,const char *format, ... ) {
    static int lineCount = 0;
    lineCount++;

    if (lineCount > 10000)
        return 1;

    va_list args;
    va_start(args, format);
    vfprintf(stream,format, args);
    va_end(args);

    return 0;
}

void incrementClock(int nanoseconds) {
	shm_clock[1] += nanoseconds;  

	if (shm_clock[1] >= ONE_SEC) { 
		unsigned incrementSec = shm_clock[1] / ONE_SEC; 
		shm_clock[0] += incrementSec; 
		shm_clock[1] %= ONE_SEC;
	}
	
	memcpy(shm_ptr, shm_clock, sizeof(unsigned int) * 2); 
}

char *filename = NULL; 
 
int main(int argc, char **argv) { 
	srand(time(NULL) + getpid()); 
	signal(SIGINT, terminate); 
	signal(SIGALRM, terminate); 
	alarm(5); 
	
	int opt; 
	const char opstr[] = "f:hn:s:i:"; 
	int proc = 0; 	// [-n]: total number of processes 
	int simul = 0; 	// [s]: max number of child processes that can simultaneously run 
	int interval = 1; // [-i]: interval in ms to launch children 

	// ./oss [-h] [-n proc] [-s simul] [-i intervalInMsToLaunchChildren] [-f logfile] 
	while ((opt = getopt(argc, argv, opstr)) != -1) {
		switch(opt) {
			case 'f': { 
				char *opened_file = optarg; 
				FILE* fptr = fopen(opened_file, "a"); 
				if (fptr) { 
					filename = opened_file; 
					fclose(fptr); 
				} else { 
					printf("File doesn't exist - ERROR\n"); 
					exit(1); 
				} 
				break;
			}  
			case 'h': 
				help(); 
				break; 
			case 'n': 
				proc = atoi(optarg); 
				if (proc > 18) { 
					printf("The max number of processes is 18\n"); 
					exit(1); 
				} 
				break; 
			case 's': 
				simul = atoi(optarg); 
				if (simul > 18) {
                    printf("The max number of simultaneous processes is 18\n");
                    exit(1);
                }
			case 'i': 
				interval = atoi(optarg); 
				break; 
			default: 
				help(); 
				exit(EXIT_FAILURE);
		} 
	}
	// Create msgq file 
	system("touch msgq.txt");
	
	// Initializing PCB table: 
	for (int i = 0; i < 18; i++) { 
		processTable[i].occupied = 0;
		processTable[i].pid = 0; 
		processTable[i].startSeconds = 0; 
		processTable[i].startNano = 0;
		processTable[i].isBlocked = 0; 
		processTable[i].isRequestAtFront = 0; 
		processTable[i].childMemoryRefs = 0; 
		processTable[i].targetPage = INT16_MIN; // Minimum value
		
		// Initialize the process page table:  
		for (int k = 0; k < 64; k++) { 
			// Set it to be a negative number 
			processTable[i].pages[k] = INT16_MIN;
		}
	} 

	/* Frame Table Structure
typedef struct FT {
    int dirtyBit;       // Indicates whether the frame has been written to
    int pageOccupied;   // Which page is in this frame
    pid_t pageID;
    int headOfQueue;    // Indicates whether frame is at the head of the FIFO queue
} FT; */ 

	// Initialize the frame table 
	for (int i = 0; i < FRAME_SIZE; i++) { 
		frameTable[i].pageOccupied = -1; 
		frameTable[i].pageID = -1; 
		frameTable[i].dirtyBit = 0; 
		frameTable[i].headOfQueue = -1; 
	}

	// If the queue has negative values, then a page isn't in that spot. 
	for (int j = 0; j < 18; j++) { 
		FIFO_queue[j] = INT16_MIN; 
	}  


	// Set up shared memory channels! 
	shm_id = shmget(SH_KEY, sizeof(unsigned) * 2, 0777 | IPC_CREAT); 
	if (shm_id == -1) { 
		fprintf(stderr, "Shared memory get failed in seconds\n");
		terminate(); 
	}
	
	shm_ptr = (unsigned*)shmat(shm_id, NULL, 0); 
	if (shm_ptr == NULL) { 
		perror("Unable to connect to shared memory segment\n");
		terminate(); 
	} 
	memcpy(shm_ptr, shm_clock, sizeof(unsigned) * 2); 

	// Set up message queues! 
	key_t msgkey;

	// Generating key for message queue
	if ((msgkey = ftok("msgq.txt", 1)) == -1) { 
		perror("ftok");
		terminate();  
	} 
	
	// Creating message queue 	
	if ((msqid = msgget(msgkey, 0666 | IPC_CREAT)) == -1) { 
		perror("msgget in parent");
		terminate();  
	} 

	while (total_terminated != proc) { 
		launch_passed += NANO_INCR; 
		incrementClock(launch_passed); 
	
		// Calculate if we should launch child 
		if (launch_passed >= interval || total_launched == 0) { 
			if (total_launched < proc && total_launched < simul + total_terminated)	{ 
				pid_t pid = fork(); 
				if (pid == 0) { 
					char *args[] = {"./worker", NULL}; 
					execvp(args[0], args); 
				} else {
					processTable[total_launched].pid = pid; 
					processTable[total_launched].occupied = 1;
					processTable[total_launched].startSeconds = shm_clock[0]; 
					processTable[total_launched].startNano = shm_clock[1]; 
				} 
				total_launched += 1; 
			} 

			launch_passed = 0; 
		} 

		if (proc == total_terminated) { 
			terminate(); 
		} 

		for (int k = 0; k < total_launched; k++) { 
		// Check message and send messages to the child processes  
			if (processTable[k].occupied == 1 && processTable[k].isBlocked == 0) { 
				process_child_requests(k); 
			} 
		} 

		// Show all tables we have
		if (shm_clock[1] >= half_passed + HALF_NANO || (shm_clock[1] == 0 && shm_clock[0] > 1)) { 
			displayPCB(); 
			displayMemory(); 
			half_passed = shm_clock[1]; 
		} 
	} 

	// Cleaning up code to display memory statistics
	terminate(); 
	return 0; 
} 

void displayMemory() { 
	FILE *fptr = fopen(filename, "a+"); 
	if (fptr == NULL) { 
		perror("Error opening the file"); 
		terminate(); 
	} 

	// Print the page table info 
	lfprintf(fptr, "\n"); 
	printf("\n"); 
	
	lfprintf(fptr, "%-8s%-8s", "", "\t\t\t\t\t Page Table Entries: \n"); 
	printf("%-8s%-8s", "", "\t\t\t\t\t Page Table Entries: \n");
	
	for (int i = 0; i < 64; ++i) { 
		if (i == 0) { 
			printf("P %-6s", " "); 
			lfprintf(fptr, "P %-6s", " ");
		} 
		printf("%-4d", i); 
		lfprintf(fptr, "%-4d", i); 
	} 
	
	lfprintf(fptr, "\n"); 
	printf("\n"); 
	lfprintf(fptr, "\n"); 
	printf("\n"); 

	for (int i = 0; i < total_launched; i++) { 
		if (processTable[i].occupied == 1) { 
			// Print pages for child process 
			lfprintf(fptr, "%-8d", i);
			printf("%-8d", i); 
			for (int j = 0; j < 64; ++j) { 
				// Print empty space if page isn't in frame table: 
				if (processTable[i].pages[j] < 0) { 
					lfprintf(fptr, "%-4s", "*"); 
					printf("%-4s", "*");
				} else { 
				// There is indeed a value in the frame table 
					lfprintf(fptr, "%-4d", processTable[i].pages[j]);
					printf("%-4d", processTable[i].pages[j]); 
				} 
			}
			lfprintf(fptr, "\n"); 
			printf("\n"); 
		} 
	}
	lfprintf(fptr, "\n"); 
	printf("\n"); 
	
	// Print which processes have a page 
	lfprintf(fptr, "\n");
    printf("\n");
	lfprintf(fptr, "%-13s%-15s%-15s%-15s\n", "Occupied", "Dirty Bit", "SecondChance", "NextFrame");
	printf("%-13s%-15s%-15s%-15s\n", "Occupied", "Dirty Bit", "SecondChance", "NextFrame");

	for (int i = 0; i < FRAME_SIZE; ++i) { 
		struct FT currentFrame = frameTable[i]; 
		if (currentFrame.headOfQueue == 1) { 
			lfprintf(fptr, "%-13d%-15d%-15d%-15s\n", i, currentFrame.pageOccupied, currentFrame.dirtyBit, "*"); 
			printf("%-13d%-15d%-15d%-15s\n", i, currentFrame.pageOccupied, currentFrame.dirtyBit, "*");
		} else { 
			lfprintf(fptr, "%-13d%-15d%-15d%-15s\n", i, currentFrame.pageOccupied, currentFrame.dirtyBit, "");
            printf("%-13d%-15d%-15d%-15s\n", i, currentFrame.pageOccupied, currentFrame.dirtyBit, "");
		}
	}
	lfprintf(fptr, "\n"); 
	printf("\n"); 
	fclose(fptr); 
}

void process_child_requests(int k) { 
	// Send a messsage to the child process 
	buf.mtype = processTable[k].pid; 
	if (msgsnd(msqid, &buf, sizeof(msgbuffer)-sizeof(long), IPC_NOWAIT) == -1) { 
		fprintf(stderr, "Error, msgsnd to worker failed\n"); 
		exit(EXIT_FAILURE);
	} 

	// After sending essage to the child, check what address and whether it's read or write. 
	msgbuffer msg; 
	if (msgrcv(msqid, &msg, sizeof(msgbuffer), getpid(), 0) == -1) { 
		fprintf(stderr, "Error, msgrcv to parent failed\n"); 
		exit(EXIT_FAILURE);
	} 
	
	// Log info about the requests
	FILE* fptr = fopen(filename, "a+"); 
	if (fptr == NULL) { 
		perror("Error opening file"); 
		terminate(); 
	}
	
	// first periodic check if termination 
	if (msg.termination_flag == 1) { 
		// Clear resources b/c child wants to terminate 
		char* termination_msg = "OSS: detected P%d terminated at time: %u: %u\n"; 
		lfprintf(fptr, termination_msg, k, shm_clock[0], shm_clock[1]); 
		printf(termination_msg, k, shm_clock[0], shm_clock[1]);
		
		// Statistics regarding final memory reference 
		char* mem_ref = "OSS: Process P%d had %d total memory references before termination\n";
		lfprintf(fptr, mem_ref, k, processTable[k].childMemoryRefs); 
		printf(mem_ref, k, processTable[k].childMemoryRefs);
		// Update process table and terminatino count 
		processTable[k].occupied = 0; 
		processTable[k].isBlocked = 0; 
		total_terminated += 1; 
		fclose(fptr); 
		return; 
	} 
		
	fclose(fptr); 
	// Fetch requested memory addr and optin to read or write

	int read_write_decision = msg.opt_type; // Read(1), write(0) 
	int child_req_adr = msg.memory_address; 
	
	// Calculate page that the child requests 
	int page_table_index = (int)child_req_adr/1024; 
	
	// Determine if the request can be fufilled. If the page is in mem, and if we can store it in our frame table, or if frame table is empty. Handle wanting to write or read of memory address 
	if (processTable[k].pages[page_table_index] >= 0) { 
		// Requested cild page is already loaded into memory. 
		handle_page(read_write_decision, k, child_req_adr, page_table_index); 
	} else { 
		handle_fault(read_write_decision, k, child_req_adr, page_table_index); 
	} 

} 

// Function to handle when the page is in memory as opposed to not loaded 
void handle_page(int read_or_write, int i, int req_adr, int page_adr) { 
	FILE* fptr = fopen(filename, "a+");
    if (fptr == NULL) {
    	perror("Error opening and appending to file");
        terminate();
    }
		
	if (read_or_write == 0) { 
   		// Requesting read
        char* read_req_msg = "\nOSS: P%d requesting read of address %d at time %u:%u\n\n";
       	lfprintf(fptr, read_req_msg, i, req_adr, shm_clock[0], shm_clock[1]); 
		printf(read_req_msg, i, req_adr, shm_clock[0], shm_clock[1]); 

		char* read_msg = "\nOSS: Indicating to P%d that read at address %d has happened at address %d";
		lfprintf(fptr, read_msg, i, req_adr);
		printf(read_msg, i, req_adr);
	} else { 
		// Requesting write
		char* write_req_msg = "\nOSS: P%d requesting write of address %d at time %u:%u\n\n";	
		lfprintf(fptr, write_req_msg, i, req_adr, shm_clock[0], shm_clock[1]);
		printf(write_req_msg, i, req_adr, shm_clock[0], shm_clock[1]);
		
		char* write_msg = "\nOSS: Indicating to P%d that write has happened to address %d";
		lfprintf(fptr, write_msg, i, req_adr); 
        printf(write_req_msg, i, req_adr);

		// Update dirty bit and print message if the dirty bit is set to 0, the frame hasn't been written to. 
		int frame_index = processTable[i].pages[page_adr]; 
		if (frameTable[frame_index].dirtyBit == 0) { 
			frameTable[frame_index].dirtyBit == 1; 
			// Increment clock by - NANO_INCR 100000
			incrementClock(NANO_INCR);
			
			char *dirty_bit_msg = "OSS: Dirty bit of frame %d set, adding additional time to clock at %u:%u\n"; 
			lfprintf(fptr, dirty_bit_msg, frame_index, shm_clock[0], shm_clock[1]); 
			printf(dirty_bit_msg, frame_index, shm_clock[0], shm_clock[1]);
		}
	}
		
	// Update clock normally 
	incrementClock(NANO_INCR); 
	processTable[i].childMemoryRefs += 1; 
	total_refs += 1; 
	fclose(fptr); 
}

// Function to handle a page not being in memory 
void handle_fault(int read_or_write, int i, int req_adr, int page_adr) {		
	FILE* fptr = fopen(filename, "a+");
    if (fptr == NULL) {
        perror("Error opening and appending to file");
        terminate();
    }
	
	if (read_or_write == 0) { 
		char* read_req_msg = "\nOSS: P%d requesting read of address %d at time %u:%u\n\n";
		lfprintf(fptr, read_req_msg, i, req_adr, shm_clock[0], shm_clock[1]); 		printf(read_req_msg, i, req_adr, shm_clock[0], shm_clock[1]);

	} else {
		char* write_req_msg = "\nOSS: P%d requesting write of address %d at time %u:%u\n\n"; 
		lfprintf(fptr, write_req_msg, i, req_adr, shm_clock[0], shm_clock[1]);
		printf(write_req_msg, i, req_adr, shm_clock[0], shm_clock[1]);
	} 

	// Find the next available frame that's not in memory. we'll find a page if there are any empty ones. 
	for (int k = 0; k < 256; k++) { 
		struct FT currentFrame = frameTable[k]; 
		if (currentFrame.pageOccupied == -1) {
			char* write_req_msg = "\nOSS: P%d requesting write of address %d at time %u:%u\n\n"; 
			lfprintf(fptr, write_req_msg, i, req_adr, shm_clock[0], shm_clock[1]);
			printf(write_req_msg, i, req_adr, shm_clock[0], shm_clock[1]);
			
			char* page_fault = "\n OSS: Address %d is not in a frame, page fault"; 
			lfprintf(fptr, page_fault, req_adr); 
			printf(page_fault, req_adr);  

			// Setting dirty bit to 1 since this is a new frame 
			currentFrame.dirtyBit = 1; 
			incrementClock(NANO_INCR); 
			
			char* dirty_bit_msg = "\nOSS: Dirty bit of frame %d was set, adding additional time to the clock"; 
			lfprintf(fptr, dirty_bit_msg, k); 
			printf(dirty_bit_msg, k); 

			// Store the page that will occupy the frame 
			currentFrame.pageOccupied = page_adr; 
			currentFrame.pageID = processTable[i].pid; 
			
			// Update page table whose page is added to the frame table 
			processTable[i].pages[page_adr] = k; 
			processTable[i].childMemoryRefs += 1; 
		
			// Print frame table updates. 
			if (read_or_write == 0) {
				char* read_msg = "\nOSS: Indicating to P%d that read at address %d has happened at address %d"; 
				// Read Success message
				lfprintf(fptr, read_msg, i, req_adr); 
				printf(read_msg, i, req_adr); 
			} else {
				char* write_msg = "\nOSS: Indicating to P%d that write has happened to address %d"; 
				// Write success message
				lfprintf(fptr, write_msg, i, req_adr); 
				printf(write_msg, i, req_adr);
			} 
			// Update final statistics
			total_refs += 1; 
			total_page += 1; 
			return; 
		}
	}
	// Update statistics of memory usage
	total_refs += 1; 
	total_page += 1; // Update page faults since not in memory
	incrementClock(NANO_INCR); 
	fclose(fptr); 
} 

/*// Message Queue Structure
typedef struct msgbuffer {
    long mtype;
    int memory_address;
    int opt_type;           // Option type: worker wants to read(1) or write(0)
    int termination_flag;
} msgbuffer; */ 

/* Frame Table Structure
typedef struct FT {
    int dirtyBit;       // Indicates whether the frame has been written to
    int pageOccupied;   // Which page is in this frame
    pid_t pageID;
    int headOfQueue;    // Indicates whether frame is at the head of the FIFO queue
} FT;

struct FT frameTable[FRAME_SIZE];
*/ 


void help() { 
	printf("This program is designed to implement a memory mangagement module. We'll be implementing the second-chance (CLOCK) page replacement algorithms.\n"); 
	printf("[-h] - outputs a help message and terminates the program.\n");
	printf("[-n proc] - specifies total number of child processes.\n");
	printf("[-s simul] - specifies maximum number of child processes that can simultaneously run.\n");
	printf("[-i intervalInMsToLaunchChildren] - specifies how often a children should be launched based on sys clock in milliseconds\n"); 
	printf("[-f logfile] - outputs log to a specified file\n"); 
} 
			
void displayPCB() {
    FILE* fptr = fopen(filename, "a+");
    if (fptr == NULL) {
        perror("Error opening and appending to the file");
        terminate();
    }

    // Print to the logfile
    lfprintf(fptr, "\nOSS PID: %d SysClockS: %d SysclockNano: %d\nProcess Table: \n%-6s%-10s%-8s%-12s%-12s\n", getpid(), shm_clock[0], shm_clock[1], "Entry", "Occupied", "PID", "StartS", "StartN");
    for (int i = 0; i < total_launched; i++) {
        lfprintf(fptr, "%-6d%-10d%-8d%-12u%-12u\n", i, processTable[i].occupied, processTable[i].pid, processTable[i].startSeconds, processTable[i].startNano);
    }
    fprintf(fptr, "\n");

    // Output to screen
    printf("\nOSS PID: %d SysClockS: %d SysclockNano: %d\nProcess Table: \n%-6s%-10s%-8s%-12s%-12s\n", getpid(), shm_clock[0], shm_clock[1], "Entry", "Occupied", "PID", "StartS", "StartN");
    for (int i = 0; i < total_launched; i++) {
        printf("%-6d%-10d%-8d%-12u%-12u\n", i, processTable[i].occupied, processTable[i].pid, processTable[i].startSeconds, processTable[i].startNano);
    }
    printf("\n");
    fclose(fptr);
}


// Function to terminate all processes, clean the queues, and removed shared memory. In additional, clean up the memory statistics
void terminate() {
	// Output the final data
	float memoryStats = 0; 
	float faultStats = 0;
	FILE* fptr = fopen(filename, "a+"); 
	if (fptr == NULL) { 
		perror("Error opening file in termination\n"); 
		terminate(); 
	} 
	
	// Determine the output statistics 
	if (total_refs != 0 && shm_clock[0] != 0) { 
		memoryStats = (float)total_refs / shm_clock[0]; 
	} 
	
	if (total_page != 0 && total_refs != 0) {
        faultStats = (float)(total_page) / (total_refs);
    }
	// Output statistics
	
	if (memoryStats == 0) { 
		char* final_ref = "\nThe total number of memory accesses in the system is: %d\n"; 
		lfprintf(fptr, final_ref, total_refs); 
		printf(final_ref, total_refs); 
	} else { 
		char* per_sec = "\nThe memory accesses per second in the system is: %.1f\n"; 
		lfprintf(fptr, per_sec, memoryStats); 
		printf(per_sec, memoryStats); 
	} 
	
	if (faultStats == 0) { 
		char* final_fault = "\nThe number of page faults in the system was: %d\n"; 
		lfprintf(fptr, final_fault, total_page); 
		printf(final_fault, total_page); 
	} else { 
		char* page_per_sec = "\nThe number of page faults per memory access in the system is: %.1f\n"; 
		lfprintf(fptr, page_per_sec, faultStats); 
		printf(page_per_sec, faultStats); 
	} 
	
	fclose(fptr); 

    msgctl(msqid, IPC_RMID, NULL);
    shmdt(shm_ptr);
    shmctl(shm_id, IPC_RMID, NULL);
	kill(0, SIGTERM);
}
