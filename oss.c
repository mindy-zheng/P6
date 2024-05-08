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

void help(); 

// Key for shared memory
#define SH_KEY 89918991
#define PERMS 0644
unsigned int shm_clock[2] = {0, 0}; // Creating simulated clock in shared memory

// Defined variables 
#define HALF_NANO 5000000000	// Condition for printing the process table; half a nano
#define ONE_SEC 1000000000	// 1,000,000,000 (billion) nanoseconds is equal to 1 second
#define NANO_INCR 100000	// 0.1 millisecond 
#define FRAME_SIZE 256 	// 
int total_launched = 0, total_terminated = 0, second_passed = 0, half_passed = 0;
unsigned long long launch_passed = 0;  

// PCB Structure 
typedef struct PCB { 
	int occupied; 		// Either true or false	
	pid_t pid; 			// Process ID of current assigned child 
	int startSeconds; 	// Start seconds when it was forked
	int startNano; 		// Start nano when it was forked
	int awaitingResponse; 	// Process waiting response 
} PCB; 
struct PCB processTable[18];
void deadlock_detection();

void displayPCB(); 

// Message Queue Structure 
typedef struct msgbuffer { 
	long mtype; 
	int resourceAction; 	// 0 means request, 1 means release
	int resourceID; 		// R0, R1, R2, etc.. 
	pid_t targetPID; 		// PID of child process that wants to release or 							request resources 
} msgbuffer;

msgbuffer buf; 
int msqid; 
unsigned shm_id;
unsigned *shm_ptr; 


// Resource and allocation tables 
int allocatedMatrix[10][18]; 
int requestMatrix[10][18]; 
int allResources[10]; 

void displayResources(); 
	
static void myhandler(int);
static int setupinterrupt(void);
static int setupitimer(void);
void terminate(); 
void dispatch_message(int i); 

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

void incrementClock() {
	int incrementNano = NANO_INCR;
	shm_clock[1] += incrementNano;  

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
	
	// Initializing PCB table: 
	for (int i = 0; i < 18; i++) { 
		processTable[i].occupied = 0;
		processTable[i].pid = 0; 
		processTable[i].startSeconds = 0; 
		processTable[i].startNano = 0; 
		processTable[i].awaitingResponse = 0; 
	} 

	// Set up Resource Matrix
	for (int i = 0; i < 10; i++) { 
		for (int j = 0; j < 18; j++) { 
			allocatedMatrix[i][j] = 0; 
			requestMatrix[i][j] = 0; 
		}
	} 
	
	for (int i = 0; i < 10; i++) { 
		allResources[i] = 0; 
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
	system("touch msgq.txt");
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
		incrementClock(); 
	
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
					processTable[total_launched].awaitingResponse = 0; 
					processTable[total_launched].startSeconds = shm_clock[0]; 
					processTable[total_launched].startNano = shm_clock[1]; 
				} 
				total_launched += 1; 
			} 

			launch_passed = 0; 
		} 

		// Check if any processes have terminated 
		for (int i = 0; i < total_launched; i++) { 
			int status; 
			pid_t childPid = processTable[i].pid; 
			pid_t result = waitpid(childPid, &status, WNOHANG); 
			if (result > 0) {
				FILE* fptr = fopen(filename, "a+"); 
				if (fptr == NULL) { 
					perror("Error opening file"); 
					exit(1); 
				} 
 
				char *detection_message = "\nMaster detected process P%d terminated\n"; 
				lfprintf(fptr, detection_message, i); 
				printf(detection_message, i); 

				// Release Resources if child has terminated 
				lfprintf(fptr, "Releasing Resources: "); 
				printf("Releasing Resources: "); 

				for (int t = 0; t < 10; t++) { 
					if (allocatedMatrix[t][i] != 0) { 
						allResources[t] -= allocatedMatrix[t][i]; 
						lfprintf(fptr, "R%d: %d ", t, allocatedMatrix[t][i]); 
						printf("R%d: %d ", t, allocatedMatrix[t][i]); 
					} 
					requestMatrix[t][i] = 0; 
					allocatedMatrix[t][i] = 0; 
				} 
				lfprintf(fptr, "\n"); 
				printf("\n"); 
				processTable[i].occupied = 0; 
				processTable[i].awaitingResponse = 0; 
				total_terminated += 1; 
				fclose(fptr); 
			}
		} 
		
		if (proc == total_terminated) { 
			terminate(); 
		} 

		// Check message from children 
		msgbuffer msg; 
		if (msgrcv(msqid, &msg, sizeof(msgbuffer), getpid(), IPC_NOWAIT) == -1) { 
			if (errno == ENOMSG) { 
				// Loops.. printf("Got no message, so maybe do nothing\n"); 
			} else { 
				printf("Got an error message from msgrcv\n"); 
				perror("msgrcv"); 
				terminate(); 
			} 
		} else {
			// printf("Recived %d from worker\n",message.data);
			int targetPID = -1; 
			pid_t messenger = msg.targetPID; 
			
			for (int i = 0; i < proc; i++) { 
				if (processTable[i].pid == messenger) { 
					targetPID = i; 
				} 
			} 

			// Check the content of the msg 
			int sendmsg = 0; 
			if (msg.resourceAction == 1) { 
				FILE* fptr = fopen(filename, "a+"); 
				if (fptr == NULL) { 
					perror("Error opening and appending to file\n"); 
					terminate(); 
				} 
			
				char *acknowledged_message = "Master has acknowledged Process P%d releasing R%d at time %u:%u\n\n"; 
				lfprintf(fptr, acknowledged_message, targetPID, msg.resourceID, shm_clock[0], shm_clock[1]); 
				printf(acknowledged_message, targetPID, msg.resourceID, shm_clock[0], shm_clock[1]); 

				// Child is releasing instance of resource
				allResources[msg.resourceID] -= 1; 
				allocatedMatrix[msg.resourceID][targetPID] -= 1; 
				sendmsg = 1;  
				fclose(fptr); 
			} else { 
				FILE* fptr = fopen(filename, "a+"); 
				if (fptr == NULL) { 
					perror("Error opening and appending to file"); 
					terminate(); 
				} 
				
				char *detected_req_message = "\nMaster has detected Process P%d request R%d at time %u:%u:\n"; 
				lfprintf(fptr, detected_req_message, targetPID, msg.resourceID, shm_clock[0], shm_clock[1]); 
				printf(detected_req_message, targetPID, msg.resourceID, shm_clock[0], shm_clock[1]);
				
				// Child requesting a resource 
				if (allResources[msg.resourceID] != 20) { 
					char *grant_message = "Master granting P%d request R%d at time %u:%u\n";
					lfprintf(fptr, grant_message, targetPID, msg.resourceID, shm_clock[0], shm_clock[1]);
					printf(grant_message, targetPID, msg.resourceID, shm_clock[0], shm_clock[1]);
					allResources[msg.resourceID] += 1; 
					allocatedMatrix[msg.resourceID][targetPID] += 1; 
					sendmsg = 1; 
				} else { 
				// If the child resource cannot be granted, put them in the wait queue 
					char *resource_unavailable = "Master: no instances of R%d are available, P%d added to wait queue at time %u:%u\n\n"; 
					lfprintf(fptr, resource_unavailable, targetPID, msg.resourceID, shm_clock[0], shm_clock[1]);
					printf(resource_unavailable, targetPID, msg.resourceID, shm_clock[0], shm_clock[1]);
					requestMatrix[msg.resourceID][targetPID] = 1; 
				}

				fclose(fptr); 
			}
			
			// Send confirmation message 
			if (sendmsg == 1) { 
				buf.mtype = processTable[targetPID].pid; 
				processTable[targetPID].awaitingResponse = 0; 
			
				if (msgsnd(msqid, &buf, sizeof(msgbuffer) - sizeof(long), 0) == -1) { 
					perror("msgsnd to child failed \n"); 
					terminate(); 
				}
			}
		}

		// End of sending message back to the children 

		for (int i = 0; i < total_launched; i++) { 
			if (processTable[i].occupied == 1) { 
				if (processTable[i].awaitingResponse == 0) { 
					// Update the awaiting response flag for child process 
					dispatch_message(i); 
				}
			}
		} 

		// If current seconds is at least one second or later than one in shared memory 
		if (shm_clock[0] >= second_passed + 1) { 
			// Run deadlock detection 
			deadlock_detection(); 
		} 
		// Show all resource and process tables 
		if (shm_clock[0] >= half_passed + HALF_NANO || (shm_clock[1] == 0 && shm_clock[0] > 1)) { 
			displayPCB();
			displayResources(); 
			half_passed = shm_clock[1]; 
		}

	}

	terminate(); 

	return 0; 

} 

void help() { 
	printf("This program is designed to simulate resource management inside of an operating system. It will primarily manage resources and take care of any possible deadlock issues :\n"); 
	printf("[-h] - outputs a help message and terminates the program.\n");
	printf("[-n proc] - specifies total number of child processes.\n");
	printf("[-s simul] - specifies maximum number of child processes that can simultaneously run.\n");
	printf("[-i intervalInMsToLaunchChildren] - specifies how often a children should be launched based on sys clock in milliseconds\n"); 
	printf("[-f logfile] - outputs log to a specified file\n"); 
} 
void deadlock_detection() { 
	second_passed = shm_clock[0];
	FILE* fptr = fopen(filename, "a+");
   	if (fptr == NULL) {
		perror("Error opening and appending to the file");
		terminate();
    }
	// Print values in resource vector
    int num_resources = 10;
    int num_processes = total_launched;

   	char *deadlock_detect_msg = "Master running deadlock detection at time %u:%u\n";
   	lfprintf(fptr, deadlock_detect_msg, shm_clock[0], shm_clock[1]);
    printf(deadlock_detect_msg, shm_clock[0], shm_clock[1]);

    // Check for available resources and grant child resource if available 
		for (int i = 0; i < num_processes; i++) {
			for (int j = 0; j < num_processes; j++) { 
				if (requestMatrix[j][i] == 1 && allResources[j] != 20) { 
					allResources[j] += 1; 
					requestMatrix[j][i] = 0; 
					allocatedMatrix[j][i] += 1; 
					processTable[i].awaitingResponse = 0; 
				
					char *grant_and_removal_msg = "Master detected resource R%d is available, now granting it to process P%d\n     Master removing process P%d from wait queue at time %u:%u\n"; 
					lfprintf(fptr, grant_and_removal_msg, j, i, i, shm_clock[0], shm_clock[1]); 
					printf(grant_and_removal_msg, j, i, i, shm_clock[0], shm_clock[1]); 
			
					// Send resource message back to child in wait queue 
					buf.mtype = processTable[i].pid; 
					if (msgsnd(msqid, &buf, sizeof(msgbuffer) - sizeof(long), 0) == -1) { 
						perror("msgsnd to child failed\n"); 
						terminate(); 
					}
				break; 
				}
			}
		}
		
		// Find child with least amount of time in the system or the most recent child 
		int deadlocked_count = 0; 
		int least_active_PID = 0; 
		for (int i = 0; i < num_processes; i++) { 
			for (int j = 0; j < num_resources; j++) { 
				if (requestMatrix[j][i] == 1) { 
					// Update last child that was potentially deadlocked 
					least_active_PID = i; 
					deadlocked_count += 1; 
				}
			}
		} 

		// If there are more than 1 processes waiting for a resource, remove 
		if (deadlocked_count > 1) { 
			printf("Processes "); 
			lfprintf(fptr, "Processes "); 
			for (int i = 0; i < total_launched; i++) { 
				for (int j = 0; j < 10; j++) { 
					if (requestMatrix[j][i] == 1) { 
						lfprintf(fptr, "P%d ", i); 
						printf("P%d ", i); 
					}
				}
			}

			printf("are deadlocked.\nMaster terminating P%d to remove deadlock\n", least_active_PID); 
			lfprintf(fptr, "are deadlocked.\nMaster terminating P%d to remove deadlock\n", least_active_PID);

			// Terminate deadlocked process 
			if (kill(processTable[least_active_PID].pid, SIGKILL) == -1) { 
				perror("Kill error in parent"); 
				terminate(); 
			} else { 
				int status; 
				if (waitpid(processTable[least_active_PID].pid, &status, 0) == -1) { 
					perror("waitpid error in parent\n"); 
					terminate(); 
				} 
			} 
			
			char *removal_msg = "Master terminated Process P%d \nReleasing process P%d resources: "; 
			lfprintf(fptr, removal_msg, least_active_PID, least_active_PID); 
			printf(removal_msg, least_active_PID, least_active_PID);

			for (int t = 0; t <10; t++) { 
				if (allocatedMatrix[t][least_active_PID] > 0) { 
					lfprintf(fptr, "R%d:%d ", t, allocatedMatrix[t][least_active_PID]); 
					printf("R%d:%d ", t, allocatedMatrix[t][least_active_PID]); 
					allResources[t] -= allocatedMatrix[t][least_active_PID]; 
				} 
			
				requestMatrix[t][least_active_PID] = 0; 
				allocatedMatrix[t][least_active_PID] = 0; 
			} 

			lfprintf(fptr, "\n\n"); 
			printf("\n\n"); 
		
			processTable[least_active_PID].occupied = 0; 
			processTable[least_active_PID].awaitingResponse = 0; 
			total_terminated += 1; 
		} else { 
			char *no_deadlocks = "No deadlocks detected\n\n"; 
			lfprintf(fptr, no_deadlocks); 
			printf("%s", no_deadlocks); 
		} 
		
		fclose(fptr); 
		// Run the algorithm to check if it's gone: 
		if (deadlocked_count > 1) { 
			deadlock_detection(); 
		} 

} 
			
void displayResources() {
    int num_resources = 10;
    int num_processes = total_launched;

    FILE* fptr = fopen(filename, "a+");
    if (fptr == NULL) {
        perror("Error opening file\n");
        terminate();
    }

    // Print to the file
    lfprintf(fptr, "Allocated Matrix:\n");
    lfprintf(fptr, "Resources: %5s %5s %5s %5s %5s %5s %5s %5s %5s %5s %5s\n", "", "R0", "R1", "R2", "R3", "R4", "R5", "R6", "R7", "R8", "R9");
    for (int j = 0; j < num_processes; j++) {
        lfprintf(fptr, "P%-5d ", j);
        for (int i = 0; i < num_resources; i++) {
            lfprintf(fptr, " %-5d", allocatedMatrix[i][j]);
        }
        lfprintf(fptr, "\n");
    }

    // Requested Table data
    lfprintf(fptr, "\n");
    lfprintf(fptr, "Requested Matrix:\n");
    lfprintf(fptr, "Resources: %5s %5s %5s %5s %5s %5s %5s %5s %5s %5s %5s\n", "", "R0", "R1", "R2", "R3", "R4", "R5", "R6", "R7", "R8", "R9");
    for (int j = 0; j < num_processes; j++) {
        lfprintf(fptr, "P%-5d ", j);
        for (int i = 0; i < num_resources; i++) {
            lfprintf(fptr, " %-5d", requestMatrix[i][j]);
        }
        lfprintf(fptr, "\n");
    }
    lfprintf(fptr, "\n");

    // Output to the screen
    printf("Allocated Matrix:\n");
    printf("Resources: %5s %5s %5s %5s %5s %5s %5s %5s %5s %5s %5s\n", "", "R0", "R1", "R2", "R3", "R4", "R5", "R6", "R7", "R8", "R9");
    for (int j = 0; j < num_processes; j++) {
        printf("P%-5d ", j);
        for (int i = 0; i < num_resources; i++) {
            printf(" %-5d", allocatedMatrix[i][j]);
        }
        printf("\n");
    }

    // Output to the screen - requested matrix
    printf("\n");
    printf("Requested Matrix:\n");
    printf("Resources: %5s %5s %5s %5s %5s %5s %5s %5s %5s %5s %5s\n", "", "R0", "R1", "R2", "R3", "R4", "R5", "R6", "R7", "R8", "R9");
    for (int j = 0; j < num_processes; j++) {
        printf("P%-5d ", j);
        for (int i = 0; i < num_resources; i++) {
            printf(" %-5d", requestMatrix[i][j]);
        }
        printf("\n");
    }
    printf("\n");
    fclose(fptr);
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

// A function to dispatch a message to a child process 
void dispatch_message(int targetPID) { 
	// Send message and update awaiting response flag for child 
	buf.mtype = processTable[targetPID].pid; 
	if (msgsnd(msqid, &buf, sizeof(msgbuffer) - sizeof(long), 0) == -1) { 
		perror("msgsnd to child failed\n"); 
		terminate(); 
	} 
	processTable[targetPID].awaitingResponse = 1; 
} 


void terminate() {
    // Terminate all processes, clean msg queue and remove shared memory
    kill(0, SIGTERM);
    msgctl(msqid, IPC_RMID, NULL);
    shmdt(shm_ptr);
    shmctl(shm_id, IPC_RMID, NULL);
    exit(0);
}
