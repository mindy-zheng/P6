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

#define SH_KEY 89918991
#define PERMS 0644
#define TWO_FIFTY_MS 250000000 // 250ms 
#define ONEMS 1000000 // 1ms
unsigned int shm_clock[2] = {0, 0};


typedef struct msgbuffer {
	long mtype;
    int memory_address;
    int opt_type;           // Option type: worker wants to read(1) or write(0)
    int termination_flag; 
} msgbuffer;
                                                                            msgbuffer buf;
int msqid;

int total_refs = 0; 
void terminate_check(); 
void exec_task(); 

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
	
		// Default behavior is to not terminate unless said to. 
		buf.termination_flag = 0; 
		// At every 1000 memory references, the user will check whether it should terminate. 
		if (total_refs % 1000 == 0) { 
			if (total_refs != 0) { 
				// Give it 25% chance of terminating
				int rand_term = rand() % 101; 
				if (rand_term <= 25) { 
					// Store termination message to send to parent
					buf.termination_flag = 1; 
				}
			}
		}
		
		int memory_adr = rand() % 64001; 
		int read_write_decision = rand() % 101; 
		
		if (read_write_decision <= 25) { 
			// Parent will send a write message to parent if less or equal to 25 
			buf.opt_type = 1; // 25 percent chance of writing 
		} else { 
			buf.opt_type = 0; // 75 percent chance of reading 
		} 
		total_refs += 1; 
		buf.mtype = getppid(); 
		buf.memory_address = memory_adr; 

		if (msgsnd(msqid, &buf, sizeof(msgbuffer) - sizeof(long), 0) == -1) {
            perror("msgsnd to parent failed\n");
            exit(1);
        }
		// Terminate the child after sending the message 
		if (buf.termination_flag == 1) { 
			exit(0); 
		}
	}
	return 0; 
}
