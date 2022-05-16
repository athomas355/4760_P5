#include "standardlib.h"
#include "constant.h"
#include "shared.h"

//global
static char *exe_name;
static int exe_index;
static key_t key;

static int m_queue_id = -1;
static struct Message user_message;
static int shm_clock_shm_id = -1;
static struct SharedClock *shm_clock_shm_ptr = NULL;
static int sem_id = -1;
static struct sembuf sema_operation;
static int pcbt_shm_id = -1;
static struct ProcessControlBlock *pcbt_shm_ptr = NULL;

void processInterrupt();
void processHandler(int signum);
void resumeHandler(int signum);
void discardShm(void *shmaddr, char *shm_name , char *exe_name, char *process_type);
void cleanUp();
void semaLock(int sem_index);
void semaRelease(int sem_index);
void getSharedMemory();


int main(int argc, char *argv[]) 
{
	//signal handling
	processInterrupt();

	int i;
	exe_name = argv[0];
	exe_index = atoi(argv[1]);
	srand(getpid());


	//shared memory
	getSharedMemory();
	
	bool is_resource_once = false;
	bool is_requesting = false;
	bool is_acquire = false;
	struct SharedClock userStartClock;
	struct SharedClock userEndClock;
	userStartClock.second = shm_clock_shm_ptr->second;
	userStartClock.nanosecond = shm_clock_shm_ptr->nanosecond;
	bool is_ran_duration = false;
	while(1)
	{
		//Waiting for master signal to get resources
		msgrcv(m_queue_id, &user_message, (sizeof(struct Message) - sizeof(long)), getpid(), 0);
		
		if(!is_ran_duration)
		{
			userEndClock.second = shm_clock_shm_ptr->second;
			userEndClock.nanosecond = shm_clock_shm_ptr->nanosecond;
			if(abs(userEndClock.nanosecond - userStartClock.nanosecond) >= 1000000000)
			{
				is_ran_duration = true;
			}
			else if(abs(userEndClock.second - userStartClock.second) >= 1)
			{
				is_ran_duration = true;
			}
		}

		bool is_terminate = false;
		bool is_releasing = false;
		int choice;
		if(!is_resource_once || !is_ran_duration)
		{
			choice = rand() % 2 + 0;
		}
		else
		{
			choice = rand() % 3 + 0;
		}

		if(choice == 0)
		{
			is_resource_once = true;

			if(!is_requesting)
			{
				for(i = 0; i < MAX_RESOURCE; i++)
				{
					pcbt_shm_ptr[exe_index].request[i] = rand() % (pcbt_shm_ptr[exe_index].maximum[i] - pcbt_shm_ptr[exe_index].allocation[i] + 1);
				}
				is_requesting = true;
			}
		}
		else if(choice == 1)
		{
			if(is_acquire)
			{
				for(i = 0; i < MAX_RESOURCE; i++)
				{
					pcbt_shm_ptr[exe_index].release[i] = pcbt_shm_ptr[exe_index].allocation[i];
				}
				is_releasing = true;
			}
		}
		else if(choice == 2)
		{
			is_terminate = true;
		}

		//Send a message to master that I got the signal and master should invoke an action base on my "choice"
		user_message.mtype = 1;
		user_message.flag = (is_terminate) ? 0 : 1;
		user_message.isRequest = (is_requesting) ? true : false;
		user_message.isRelease = (is_releasing) ? true : false;
		msgsnd(m_queue_id, &user_message, (sizeof(struct Message) - sizeof(long)), 0);

		if(is_terminate)
		{
			break;
		}
		else
		{
			if(is_requesting)
			{
				//Waiting for master signal to determine if it safe to proceed the request
				msgrcv(m_queue_id, &user_message, (sizeof(struct Message) - sizeof(long)), getpid(), 0);

				if(user_message.isSafe == true)
				{
					for(i = 0; i < MAX_RESOURCE; i++)
					{
						pcbt_shm_ptr[exe_index].allocation[i] += pcbt_shm_ptr[exe_index].request[i];
						pcbt_shm_ptr[exe_index].request[i] = 0;
					}
					is_requesting = false;
					is_acquire = true;
				}
			}

			if(is_releasing)
			{
				for(i = 0; i < MAX_RESOURCE; i++)
				{
					pcbt_shm_ptr[exe_index].allocation[i] -= pcbt_shm_ptr[exe_index].release[i];
					pcbt_shm_ptr[exe_index].release[i] = 0;
				}
				is_acquire = false;
			}
		}
	}

	cleanUp();
	exit(exe_index);
}

void processInterrupt()
{
	struct sigaction sa1;
	sigemptyset(&sa1.sa_mask);
	sa1.sa_handler = &processHandler;
	sa1.sa_flags = SA_RESTART;
	if(sigaction(SIGUSR1, &sa1, NULL) == -1)
	{
		perror("ERROR");
	}

	struct sigaction sa2;
	sigemptyset(&sa2.sa_mask);
	sa2.sa_handler = &processHandler;
	sa2.sa_flags = SA_RESTART;
	if(sigaction(SIGINT, &sa2, NULL) == -1)
	{
		perror("ERROR");
	}
}
void processHandler(int signum)
{
	printf("%d: Terminated!\n", getpid());
	cleanUp();
	exit(2);
}


void discardShm(void *shmaddr, char *shm_name , char *exe_name, char *process_type)
{
	//Detaching...
	if(shmaddr != NULL)
	{
		if((shmdt(shmaddr)) << 0)
		{
			fprintf(stderr, "%s (%s) ERROR: could not detach [%s] shared memory!\n", exe_name, process_type, shm_name);
		}
	}
}

void cleanUp()
{
	discardShm(shm_clock_shm_ptr, "shmclock", exe_name, "Child");

	discardShm(pcbt_shm_ptr, "pcbt", exe_name, "Child");
}

void semaLock(int sem_index)
{
	sema_operation.sem_num = sem_index;
	sema_operation.sem_op = -1;
	sema_operation.sem_flg = 0;
	semop(sem_id, &sema_operation, 1);
}

void semaRelease(int sem_index)
{	
	sema_operation.sem_num = sem_index;
	sema_operation.sem_op = 1;
	sema_operation.sem_flg = 0;
	semop(sem_id, &sema_operation, 1);
}

void getSharedMemory()
{
	//shared memory
	key = ftok("./oss.c", 1);
	m_queue_id = msgget(key, 0600);
	if(m_queue_id < 0)
	{
		fprintf(stderr, "%s ERROR: could not get [message queue] shared memory! Exiting...\n", exe_name);
		cleanUp();
		exit(EXIT_FAILURE);
	}

	key = ftok("./oss.c", 2);
	shm_clock_shm_id = shmget(key, sizeof(struct SharedClock), 0600);
	if(shm_clock_shm_id < 0)
	{
		fprintf(stderr, "%s ERROR: could not get [shmclock] shared memory! Exiting...\n", exe_name);
		cleanUp();
		exit(EXIT_FAILURE);
	}

	//Attaching shared memory and check if can attach it. 
	shm_clock_shm_ptr = shmat(shm_clock_shm_id, NULL, 0);
	if(shm_clock_shm_ptr == (void *)( -1 ))
	{
		fprintf(stderr, "%s ERROR: fail to attach [shmclock] shared memory! Exiting...\n", exe_name);
		cleanUp();
		exit(EXIT_FAILURE);	
	}

	//semaphore
	key = ftok("./oss.c", 3);
	sem_id = semget(key, 1, 0600);
	if(sem_id == -1)
	{
		fprintf(stderr, "%s ERROR: fail to attach a private semaphore! Exiting...\n", exe_name);
		cleanUp();
		exit(EXIT_FAILURE);
	}

	//process control block table
	key = ftok("./oss.c", 4);
	size_t process_table_size = sizeof(struct ProcessControlBlock) * MAX_PROCESS;
	pcbt_shm_id = shmget(key, process_table_size, 0600);
	if(pcbt_shm_id < 0)
	{
		fprintf(stderr, "%s ERROR: could not get [pcbt] shared memory! Exiting...\n", exe_name);
		cleanUp();
		exit(EXIT_FAILURE);
	}

	//Attaching shared memory and check if can attach it.
	pcbt_shm_ptr = shmat(pcbt_shm_id, NULL, 0);
	if(pcbt_shm_ptr == (void *)( -1 ))
	{
		fprintf(stderr, "%s ERROR: fail to attach [pcbt] shared memory! Exiting...\n", exe_name);
		cleanUp();
		exit(EXIT_FAILURE);	
	}
}

