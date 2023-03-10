#include <fcntl.h>
#include <sys/stat.h>
#include <semaphore.h>
#include <assert.h>
#include <fcntl.h> // For O_* constants.
#include <semaphore.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h> // For mode constants.
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <errno.h>

#include "err.h"
#include "utils.h"


// CONSTANTS:
#define MAX_N_TASKS 4096
#define MAX_IN_SIZE 511
#define MAX_OUT_SIZE 1022

typedef struct {
  pid_t pid;
  int read_out_dsc;
  int read_err_dsc;
  int write_out_dsc;
  int write_err_dsc;
  int nr;
  char lastReadOut[MAX_OUT_SIZE];
  char lastReadErr[MAX_OUT_SIZE];
  pthread_mutex_t mutex;
} Task;

// Storage compartment.
struct SharedStorage {
    sem_t mutex;
    sem_t executorLoopMutex;
};

Task tasks[MAX_N_TASKS];

struct SharedStorage* shared_storage;

pthread_t readSTDOUT_threads[MAX_N_TASKS];
pthread_t readSTDERR_threads[MAX_N_TASKS];
pthread_t wait_threads[MAX_N_TASKS];

char ** words;
char buffer[MAX_IN_SIZE];

int tasksSize = 0;

bool good_command = false;

void cleanStuff()
{
  // Clean:
  for (int i = 0; i < tasksSize; i++) {
    ASSERT_ZERO(pthread_join(wait_threads[i], NULL));
    close(tasks[i].read_err_dsc);
    close(tasks[i].read_out_dsc);
    close(tasks[i].write_err_dsc);
    close(tasks[i].write_out_dsc);
    ASSERT_ZERO(pthread_join(readSTDOUT_threads[i], NULL));
    ASSERT_ZERO(pthread_join(readSTDERR_threads[i], NULL));
    ASSERT_SYS_OK(pthread_mutex_destroy(&(tasks[i].mutex)));
  }
  ASSERT_SYS_OK(sem_destroy(&(shared_storage->mutex)));
  ASSERT_SYS_OK(sem_destroy(&(shared_storage->executorLoopMutex)));
  munmap(shared_storage,  sizeof(struct SharedStorage));
  close(-1);
}

void* constantlyWait(void * data)
{
  Task * task = data;
  fprintf(stderr, "Start thread waiting for task: %d.\n", task -> nr);
  pid_t childPid;
  int exitStatus, i = task -> nr;
  ASSERT_SYS_OK(childPid = waitpid(task -> pid, &exitStatus, 0));
  if (childPid == 0) { // Child is already dead.
    fprintf(stderr, "Task %d is already dead.\n", i);
    return 0;
  }
  ASSERT_SYS_OK(sem_wait(&(shared_storage->executorLoopMutex)));
  if (WIFEXITED(exitStatus) != 0)
    printf("Task %d ended: status %d.\n", i, WEXITSTATUS(exitStatus));
  else
    printf("Task %d ended: signalled.\n", i); 
  ASSERT_SYS_OK(sem_post(&(shared_storage->executorLoopMutex)));
  fprintf(stderr, "Finish thread waiting for pid: %d\n", task -> pid);
  return 0;
}

void* constantlyReadSTDOUT(void * data)
{
  Task * task = data;
  fprintf(stderr, "Start thread reading STDOUT from task.\n");
  FILE *stream = fdopen(task->read_out_dsc, "r");
  if (!stream) {
    fprintf(stderr, "Stream error :(\n");
    exit(1);
  }
  char out_buffer[MAX_OUT_SIZE+1];
  while (read_line(out_buffer, MAX_OUT_SIZE, stream)) {
    ASSERT_SYS_OK(pthread_mutex_lock(&(task->mutex)));
    out_buffer[strcspn(out_buffer, "\n")] = '\0'; // Delete newline sign.
    fprintf(stderr, "Thread read out: %s\n", out_buffer);
    strcpy(task -> lastReadOut, out_buffer);
    ASSERT_SYS_OK(pthread_mutex_unlock(&(task->mutex)));
  }
  fclose(stream);
  fprintf(stderr, "Exited reading thread loop\n");
  return 0;
}

void* constantlyReadSTDERR(void * data)
{
  Task * task = data;
  fprintf(stderr, "Start thread reading STDERR from task.\n");
  FILE *stream = fdopen(task->read_err_dsc, "r");
  if (!stream) {
    fprintf(stderr, "Stream error :(\n");
    exit(1);
  }
  char err_buffer[MAX_OUT_SIZE+1];
  while (read_line(err_buffer, MAX_OUT_SIZE, stream)) {
    ASSERT_SYS_OK(pthread_mutex_lock(&(task->mutex)));
    err_buffer[strcspn(err_buffer, "\n")] = '\0'; // Delete newline sign.
    fprintf(stderr, "Thread read err: %s\n", err_buffer);
    strcpy(task -> lastReadErr, err_buffer);
    ASSERT_SYS_OK(pthread_mutex_unlock(&(task->mutex)));
  }
  fclose(stream);
  fprintf(stderr, "Exited reading thread loop\n");
  return 0;
}

bool doesExist(pid_t pid)
{
  if (kill(pid, 0) == 0) {
      // Process with PID exists
      return true;
  } else {
      if (errno == ESRCH) {
          // Process with PID does not exist
      } else {
          // An error occurred, but the process might exist
      }
      return false;
  }
}

void endExec()
{
  for (int i = 0; i < tasksSize; i++)
  {
    if(doesExist(tasks[i].pid))
      ASSERT_SYS_OK(kill(tasks[i].pid, SIGKILL));
  }
}

int main() 
{
  // Create shared storage.
  shared_storage = mmap(
      NULL,
      sizeof(struct SharedStorage),
      PROT_READ | PROT_WRITE,
      MAP_SHARED | MAP_ANONYMOUS,
      -1,
      0);

  if (shared_storage == MAP_FAILED)
      syserr("mmap");

  // Inicjalizacja semafor??w:
  ASSERT_SYS_OK(sem_init(&(shared_storage->mutex), 1, 1));
  ASSERT_SYS_OK(sem_init(&(shared_storage->executorLoopMutex), 1, 1));

  while (read_line(buffer, MAX_IN_SIZE, stdin)) {

    ASSERT_SYS_OK(sem_wait(&(shared_storage->executorLoopMutex)));

    fprintf(stderr, "Zaczynam iteracje w while(getline()).\n");
    good_command = false;

    // Rozdzielanie linii wej??cia.
    words = split_string(buffer);

    // obs??ugujemy polecenie run
    if (strcmp(words[0], "run") == 0) {
      good_command = true;
      int pipe_dsc[2];
      ASSERT_SYS_OK(pipe(pipe_dsc));
      fprintf(stderr, "Otwieram pipe, nr deskryptorow: read %d write %d\n", pipe_dsc[0], pipe_dsc[1]);

      int pipe_err_dsc[2];
      ASSERT_SYS_OK(pipe(pipe_err_dsc));
      fprintf(stderr, "Otwieram pipe_err, nr deskryptorow: read %d write %d\n", pipe_err_dsc[0], pipe_err_dsc[1]);

      ASSERT_SYS_OK(sem_wait(&(shared_storage->mutex)));

      pid_t child_pid;
      ASSERT_SYS_OK(child_pid = fork());

      if (!child_pid)
      {
        // Child process.
        ASSERT_SYS_OK(dup2(pipe_dsc[1], STDOUT_FILENO));
        ASSERT_SYS_OK(close(pipe_dsc[0]));
        ASSERT_SYS_OK(close(pipe_dsc[1])); 

        ASSERT_SYS_OK(dup2(pipe_err_dsc[1], STDERR_FILENO));
        ASSERT_SYS_OK(close(pipe_err_dsc[0]));
        ASSERT_SYS_OK(close(pipe_err_dsc[1])); 

        ASSERT_SYS_OK(sem_post(&(shared_storage->mutex)));

        ASSERT_SYS_OK(execvp(words[1], &words[1]));

      }
      else
      {
        ASSERT_SYS_OK(sem_wait(&(shared_storage->mutex)));
        
        // Parent process.
        tasksSize++;
        tasks[tasksSize-1].pid = child_pid;
        tasks[tasksSize-1].read_out_dsc = pipe_dsc[0];
        tasks[tasksSize-1].read_err_dsc = pipe_err_dsc[0];
        tasks[tasksSize-1].write_out_dsc = pipe_dsc[1];
        tasks[tasksSize-1].write_err_dsc = pipe_err_dsc[1];
        tasks[tasksSize-1].nr = tasksSize - 1;
        strcpy(tasks[tasksSize-1].lastReadOut, "");
        strcpy(tasks[tasksSize-1].lastReadErr, "");
        ASSERT_SYS_OK(pthread_mutex_init(&(tasks[tasksSize-1].mutex), 0));

        ASSERT_ZERO(pthread_create(&readSTDOUT_threads[tasksSize-1], NULL, constantlyReadSTDOUT, &tasks[tasksSize-1]));
        ASSERT_ZERO(pthread_create(&readSTDERR_threads[tasksSize-1], NULL, constantlyReadSTDERR, &tasks[tasksSize-1]));

        ASSERT_ZERO(pthread_create(&wait_threads[tasksSize-1], NULL, constantlyWait, &tasks[tasksSize-1]));

        printf("Task %d started: pid %d.\n", tasksSize-1, getpid());

        ASSERT_SYS_OK(sem_post(&(shared_storage->mutex)));
      }
    }

    if (strcmp(words[0], "out") == 0) {
      good_command = true;
      int T = atoi(words[1]);
      fprintf(stderr, "Start out task %d.\n", T);
      ASSERT_SYS_OK(pthread_mutex_lock(&(tasks[T].mutex)));
      printf("Task %d stdout: \'%s\'.\n", T, tasks[T].lastReadOut);
      ASSERT_SYS_OK(pthread_mutex_unlock(&(tasks[T].mutex)));
    }


    if (strcmp(words[0], "err") == 0) {
      good_command = true;
      int T = atoi(words[1]);
      fprintf(stderr, "Start err task %d.\n", T);
      ASSERT_SYS_OK(pthread_mutex_lock(&(tasks[T].mutex)));
      printf("Task %d stderr: \'%s\'.\n", T, tasks[T].lastReadErr);
      ASSERT_SYS_OK(pthread_mutex_unlock(&(tasks[T].mutex)));
    }

    if (strcmp(words[0], "kill") == 0) {
      good_command = true;
      int T = atoi(words[1]);
      fprintf(stderr, "Kill process: %d.\n", T);
      if (doesExist(tasks[T].pid))
        ASSERT_SYS_OK(kill(tasks[T].pid, SIGINT));
    }

    if (strcmp(words[0], "sleep") == 0) {
      good_command = true;
      int N = atoi(words[1]);
      fprintf(stderr, "Start sleep for %d ms.\n", N);
      ASSERT_SYS_OK(sem_post(&(shared_storage->executorLoopMutex)));
      usleep(N*1000);
      ASSERT_SYS_OK(sem_wait(&(shared_storage->executorLoopMutex)));
    }

    if (strcmp(words[0], "") == 0) {
      // Just an empty line.
      good_command = true;
    }

    if (strcmp(words[0], "quit") == 0) {
      good_command = true;
      fprintf(stderr, "Quit.\n");
      endExec();
      free_split_string(words);
      ASSERT_SYS_OK(sem_post(&(shared_storage->executorLoopMutex)));
      break;
    }

    if (!good_command) {
      fprintf(stderr, "Command: \"%s\" not found :(\n", words[0]);
      return 1;
    }

    free_split_string(words);

    ASSERT_SYS_OK(sem_post(&(shared_storage->executorLoopMutex)));
  }

  fprintf(stderr, "End of while loop\n");

  endExec();

  cleanStuff();

  return 0;
}