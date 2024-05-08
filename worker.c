// Name: Mindy Zheng
// Date: 4/15/2024

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

#define SH_KEY 89918991
#define PERMS 0644
#define TWO_FIFTY_MS 250000000 // 250ms 
#define ONEMS 1000000 // 1ms 
unsigned int shm_clock[2] = {0, 0};


typedef struct msgbuffer { 
	long mtype;                                                                 
	int resourceAction;     // 0 means request, 1 means release
    int resourceID;         // R0, R1, R2, etc..
    pid_t targetPID;         // PID of child process that wants to release or                            request resources
} msgbuffer;
                                                                            msgbuffer buf;
int msqid;

int last_term = 0, last_check = 0, termination_req = 0; 

// Amount of resources the child has of each resource type in the matrix 
int current_resources[10] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
void childDecision(int x); 
int checkState(); 


int main(int argc, char **argv) { 
	srand(time(NULL) + getpid()); 
	// Set up message queue 
	key_t msgkey = ftok("msgq.txt", 1);  
	if (msgkey == -1) { 
		perror("Worker failed to generate key for msg q\n"); 
		exit(1);
	} 

	msqid = msgget(msgkey, PERMS); 
	if (msqid == -1) { 
		perror("Worker failed to access msgqueue\n"); 
		exit(1); 
	} 

 	// // Update clock, check timer, and recieve and send messages
	while (1) { 
		if (msgrcv(msqid, &buf, sizeof(buf), getpid(), 0) == -1) {
			perror("Failed to recieve message in the worker\n"); 
			exit(1); 
		} 
	
		// Check and wait to see if 1ms has passed, then we can send a message back 
		while (1) { 
			// Update clock in shared memory 
			int shm_id = shmget(SH_KEY, sizeof(int) * 2, 0777); 
			if (shm_id == -1) { 
				perror("Failed to access shared memory in worker\n"); 
				exit(EXIT_FAILURE);
			} 
		
			int *shm_ptr = (int*)shmat(shm_id, NULL, SHM_RDONLY); 
			if (shm_ptr == NULL) { 
				perror("Failed to attach to shared memory in worker\n"); 
				exit(EXIT_FAILURE); 
			} 

			// Update simulated clock time 
			shm_clock[0] = shm_ptr[0]; // Seconds
			shm_clock[1] = shm_ptr[1]; // Nanoseconds
			shmdt(shm_ptr); 
	
			if (checkState() == 1) { 
				int choice = rand() % 101; // Release or request a R
				if (choice <= 10) {
					// If the random number generated is less than 10%, set to release resource
					childDecision(1);   
				} else {
					// If the random number is greater than 10, set to request 
					childDecision(0); 
				}
				break; 
			}

		} 
	}
} 
void childDecision(int resourceAction) {
	if (resourceAction == 1) {
	// Check if there are resources the child can release
    // Initialize an array to hold releasable resources
    	int resourceToRelease[10];
        int releaseableResourceCount = 0;
        for (int i = 10; i < 10; i++) {
        // If the child currently has any resource of i, then add i to the list
  	      if (current_resources[i] != 0) {
  	        resourceToRelease[releaseableResourceCount] = i;
            releaseableResourceCount++; 
          }
    	}

        if (releaseableResourceCount == 0) {
           // If the child process doesn't have any resources to release, request a resource instead
     	   buf.resourceAction = 0;
           buf.resourceID = rand() % 10; // Choose random resource type
        } else {
           // If child has resources to release, choose a random resource from list of releasable resources
           int rand_resource = resourceToRelease[rand() % releaseableResourceCount];
           buf.resourceID = rand_resource;
           buf.resourceAction = resourceAction;
        }
	} else {
   	// If 0, the child wants to request a resource
    	int resourceToRequest[10];
        int requestResourceCount = 0;
        for (int i = 0; i < 10; i++) {
            // If child doesn't have the max amount of resource i, add i to the requestable resources                
			 if (current_resources[i] != 20) {
			 	resourceToRequest[requestResourceCount] = i;
                requestResourceCount += 1;
             }
        }

        // If the child process can't request any resources, it will release resource instead
        if (requestResourceCount == 0) {
        	buf.resourceAction = 1;
            buf.resourceID = rand() % 10;
        } else {
         // if the child can request, choose a random resource
            int rand_resource = resourceToRequest[rand() % requestResourceCount];
            buf.resourceID = rand_resource; 
            buf.resourceAction = resourceAction;
		}
	}
            // Send message to parent process with child PID, the resource type, and whether child wants to request or release
    buf.mtype = getppid();
    buf.targetPID = getpid();
    if (msgsnd(msqid, &buf, sizeof(msgbuffer) - sizeof(long), 0) == -1) { 
		perror("msgsnd to parent failed\n");
        exit(1);
    }

    // wait for the message back
    msgbuffer recievedmsg;
    if (msgrcv(msqid, &recievedmsg, sizeof(msgbuffer), getpid(), -0) == -1) {
	    perror("Couldn't recieve message back from parent in worker\n");
        exit(1);
    }

    // Update child's current resources based on decision. If the child request a resource and parent approves, increment the count of that resource
    // If the child released resource, decrement
    if (buf.resourceAction == 0) {
    	current_resources[buf.resourceID] += 1;
    } else {
    	current_resources[buf.resourceID] -= 1;
	}
}

int checkState() { 
	// Check if 250ms has initially passed; if so, set the termination req to 1 
	if (shm_clock[1] >= TWO_FIFTY_MS && termination_req == 0) { 
		termination_req = 1; 
	} 
	
	// If the termination requirement time is 1 and either 250 ms has passed since the last termination check or 1s has passed overall, termination becomes a likliehood
	if (termination_req == 1 && ((shm_clock[1] >= last_term + TWO_FIFTY_MS) || (shm_clock[1] == 0 && shm_clock[0] >= 1 ))) { 
		// Generate a random number between 0 and 100. If the number is less than or equal to 10, terminate the process (10% chance)  
		int rand_term = rand() % 101; 
		if (rand_term <= 10) { 
			exit(0); 
		} 
		// Update last termination check 
		last_term = shm_clock[1]; 
	} 

	// Check if 1ms has passed since the last check, or if 1s has passed. If so, take action and send a request or release to the parent process 
	int current_clock = shm_clock[1]; // Store current value to reduce repeated access
	if (current_clock >= last_check + ONEMS || (current_clock == 0 && shm_clock[0] >= 1)) { 
		last_check = current_clock; // Update and return 1 that a decision can be made
		return 1; 
	} 

	// If none of the conditions are met; state remains 
	return 0; 
} 



