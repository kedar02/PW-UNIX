// W rozwiązaniu tego zadania będziemy potrzebować kilku mechanizmów z biblioteki standardowej C++ oraz biblioteki Unixa. W szczególności będziemy potrzebować:

//     fork() - funkcji do tworzenia procesów potomnych
//     pipe() - funkcji do tworzenia potoków (ang. pipes) do przesyłania danych między procesami
//     dup2() - funkcji do przekierowywania wejścia/wyjścia plików
//     execvp() - funkcji do uruchamiania programów w innych procesach
//     waitpid() - funkcji do oczekiwania na zakończenie działania procesu potomnego

// TODO koniec linii w echo
    
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <string.h>

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
} Task;

Task tasks[MAX_N_TASKS];

char out_buffer[MAX_OUT_SIZE+1];

char ** words;
char buffer[MAX_IN_SIZE];

int tasksSize = 0;

bool good_command = false;

void showArray(char* arr[])
{
  int i = 0;
  while(arr[i] != NULL) {
    printf("arr[i]: %sEND\n", arr[i]);
    i++;
  }
}

char * lastLine(char * s)
{
  int countEndLine = 0;
  char * it = s;
  while (*it != '\0')
  {
    if (*it == '\n') countEndLine++;
    it++;
  }
  if (countEndLine == 0)
    return s;
  it--;
  if (*it == '\n')
  {
    *it = '\0';
    if (countEndLine == 1)
      return s;
  }
  while (*it != '\n')
    it--;
  return ++it;
}

int main() 
{

  while (read_line(buffer, MAX_IN_SIZE, stdin)) {

    fprintf(stderr, "Zaczynam iteracje w while(getline()).\n");
    good_command = false;
    // rozdzielamy polecenie od reszty linii
    words = split_string(buffer);

    // obsługujemy polecenie run
    if (strcmp(words[0], "run") == 0) {
      good_command = true;
      int pipe_dsc[2];
      ASSERT_SYS_OK(pipe(pipe_dsc));
     // fprintf(stderr, "Otwieram pipe, nr deskryptorow: read %d write %d\n", pipe_dsc[0], pipe_dsc[1]);

      int pipe_err_dsc[2];
      ASSERT_SYS_OK(pipe(pipe_err_dsc));
      //fprintf(stderr, "Otwieram pipe_err, nr deskryptorow: read %d write %d\n", pipe_err_dsc[0], pipe_err_dsc[1]);


      pid_t child_pid;
      ASSERT_SYS_OK(child_pid = fork());

      if (!child_pid)
      {
        // Child process.
        printf("Task %d started: pid %d.\n", tasksSize, getpid());

        ASSERT_SYS_OK(dup2(pipe_dsc[1], STDOUT_FILENO));
        ASSERT_SYS_OK(close(pipe_dsc[0]));
        ASSERT_SYS_OK(close(pipe_dsc[1])); 
        //fprintf(stderr, "Zamykam pipe, nr deskryptorow: read %d write %d\n", pipe_dsc[0], pipe_dsc[1]);

        ASSERT_SYS_OK(dup2(pipe_err_dsc[1], STDERR_FILENO));
        ASSERT_SYS_OK(close(pipe_err_dsc[0]));
        ASSERT_SYS_OK(close(pipe_err_dsc[1])); 
        //fprintf(stderr, "Zamykam pipe_err, nr deskryptorow: read %d write %d\n", pipe_err_dsc[0], pipe_err_dsc[1]);

        ASSERT_SYS_OK(execvp(words[1], &words[1]));
      }
      else
      {
        // Parent process.
        tasksSize++;
        tasks[tasksSize-1].pid = child_pid;
        tasks[tasksSize-1].read_out_dsc = pipe_dsc[0];
        tasks[tasksSize-1].read_err_dsc = pipe_err_dsc[0];
      }
    }

    if (strcmp(words[0], "out") == 0) {
      good_command = true;
      int T = atoi(words[1]);
      ASSERT_SYS_OK(read(tasks[T].read_out_dsc, out_buffer, sizeof(out_buffer)-1));
      printf("Task %d stdout: %s.\n", T, lastLine(out_buffer));
    }

    if (strcmp(words[0], "err") == 0) {
      good_command = true;
      int T = atoi(words[1]);
      ASSERT_SYS_OK(read(tasks[T].read_err_dsc, out_buffer, sizeof(out_buffer)-1));
      printf("Task %d stderr: %s.\n", T, lastLine(out_buffer));
    }

    if (strcmp(words[0], "kill") == 0) {
      good_command = true;
      int T = atoi(words[1]);
      fprintf(stderr, "Kill process: %d.\n", T);
      ASSERT_SYS_OK(kill(tasks[T].pid, SIGINT));
    }

    if (strcmp(words[0], "sleep") == 0) {
      good_command = true;
      int N = atoi(words[1]);
      fprintf(stderr, "Start sleep for %d ms.\n", N);
      usleep(N*1000);
    }

    if (strcmp(words[0], "quit") == 0) {
      good_command = true;
      fprintf(stderr, "Quit.\n");
      free_split_string(words);
      break;
    }

    if (!good_command) {
      fprintf(stderr, "Command not found!!!\n");
      return 1;
    }

    free_split_string(words);
    //free(buffer);

    // Look for already dead processes:
    int exitStatus = 0, childPid;
    for (int i = 0; i < tasksSize; i++)
    {
      ASSERT_SYS_OK(childPid = waitpid(tasks[i].pid, &exitStatus, WNOHANG));
      if (childPid == 0) { // Child is still active.
        fprintf(stderr, "Task %d is still active.\n", i);
        continue;
      }
      if (WIFEXITED(exitStatus) != 0)
        printf("Task %d ended: status %d.\n", i, exitStatus);
      else
        printf("Task %d ended: signalled.\n", i);      
    } 
  }

  fprintf(stderr, "End of while loop\n");

  //fprintf(stderr, "Num of tasks: %d\n", tasksSize);

  int exitStatus = 0, childPid;
  // After end of input wait for all processes to finish:
  for (int i = 0; i < tasksSize; i++)
  {
    fprintf(stderr, "Start waiting for task: %d\n", i);
    ASSERT_SYS_OK(childPid = waitpid(tasks[i].pid, &exitStatus, 0));
    if (childPid == 0) { // Child is already dead.
      fprintf(stderr, "Task %d is already dead.\n", i);
      continue;
    }
    if (WIFEXITED(exitStatus) != 0)
      printf("Task %d ended: status %d.\n", i, exitStatus);
    else
      printf("Task %d ended: signalled.\n", i);      
    fprintf(stderr, "End waiting for pid: %d\n", i);
  } 

  return 0;
}