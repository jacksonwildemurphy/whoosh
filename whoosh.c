/* Written by jackson murphy and the teaching staff of CS4400 at the university of utah.
  This is the main file for the `whoosh` interpreter.
  Last modified on March 23, 2017
*/

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include "csapp.h"
#include "ast.h"
#include "fail.h"

typedef int PIPE[2];

static void run_script(script *scr);
static void run_group(script_group *group);
static void run_command(script_group *group);
static void run_and_commands(script_group *group);
static void run_or_commands(script_group *group);

static void ctl_c_handler(int sig);
static void sigchld_handler(int sig);
static void terminate_processes(pid_t* pids, int num_pids, pid_t except_pid);
static void set_var(script_var *var, int new_value);
static void close_unused_pipes(PIPE* pipes_arr, int num_pipes, int open_wr_i, int open_rd_i);
static const char** get_argv(script_command *command);
static void set_pid_var(script_group* group, int command_index, int pid);

// Flag is set to 1 when a SIGINT signal is received by the program.
static volatile int got_ctl_c = 0;

int main(int argc, char **argv) {
  script *scr;

  if ((argc != 1) && (argc != 2)) {
    fprintf(stderr, "usage: %s [<script-file>]\n", argv[0]);
    exit(1);
  }

  scr = parse_script_file((argc > 1) ? argv[1] : NULL);

  run_script(scr);

  return 0;
}

static void run_script(script *scr) {
  size_t i, num_groups = scr->num_groups;
  for(i = 0; i < num_groups; i++){
    run_group(&scr->groups[i]);
  }
}

static void run_group(script_group *group) {
  int i, repeats = group->repeats;
  for(i = 0; i < repeats; i++){
    if (group->num_commands == 1) {
      run_command(group);
    } else if (group->mode == GROUP_AND) {
      run_and_commands(group);
    } else if (group->mode == GROUP_OR) {
      run_or_commands(group);
    }
  }
}

/* This run_command function is for the special case where
  a group has a single command. */
static void run_command(script_group *group) {
  script_command* command = &group->commands[0];
  pid_t pid;
  const char **argv = get_argv(&group->commands[0]);

  if((pid = Fork()) == 0){
    setpgid(0, 0);
    Execve(argv[0], (char * const *)argv, environ);
  }

  free(argv);
  int child_status;
  (void)Waitpid(pid, &child_status, 0);

  if(command->pid_to != NULL)
    set_var(command->pid_to, pid);

  if(group->result_to != NULL){
    if(WIFEXITED(child_status))
      set_var(group->result_to, WEXITSTATUS(child_status));
    else
      set_var(group->result_to, -WTERMSIG(child_status));
  }
}

/* This run_and_commands function is run for a group of commands that
  are piped together. */
static void run_and_commands(script_group *group) {
  size_t num_commands = group->num_commands;
  size_t num_pipes = num_commands - 1;
  PIPE pipes_arr[(num_commands-1)];
  pid_t pids[num_commands];
  pid_t pid;

  // Create the pipes
  size_t pipe_i;
  for(pipe_i = 0; pipe_i < num_pipes; pipe_i++)
    Pipe(pipes_arr[pipe_i]);

  // Run the first command
  if((pid = Fork()) == 0){
    Dup2(pipes_arr[0][1], 1);
    close_unused_pipes(pipes_arr, num_pipes, 0, -1);
    const char **argv = get_argv(&group->commands[0]);
    Execve(argv[0], (char * const *)argv, environ);
  }
  pids[0] = pid;
  set_pid_var(group, 0, pid);


  // Run the remaining commands except the last
  size_t command_i;
  for(command_i = 1; command_i < num_commands - 1; command_i++){
    if((pid = Fork()) == 0){
      Dup2(pipes_arr[command_i - 1][0], 0);
      Dup2(pipes_arr[command_i][1], 1);
      close_unused_pipes(pipes_arr, num_pipes, command_i, command_i-1);
      const char **argv = get_argv(&group->commands[command_i]);
      Execve(argv[0], (char * const *)argv, environ);
    }
    pids[command_i] = pid;
    set_pid_var(group, command_i, pid);

  }

  // Run the last command
  if((pid = Fork()) == 0){
    Dup2(pipes_arr[num_pipes - 1][0], 0);
    close_unused_pipes(pipes_arr, num_pipes, -1, num_pipes - 1);
    const char **argv = get_argv(&group->commands[num_commands - 1]);
    Execve(argv[0], (char * const *)argv, environ);
  }
  pids[num_commands - 1] = pid;
  set_pid_var(group, num_commands-1, pid);


  // Close parent process's pipes
  close_unused_pipes(pipes_arr, num_pipes, -1, -1);

  // Wait for each command to finish
  for(command_i = 0; command_i < num_commands; command_i++){
    int child_status;
    Wait(&child_status);
    // Possibly set the group's "result_to" to last command's exit status
    if(command_i == num_commands-1){
      if(group->result_to != NULL){
        if(WIFEXITED(child_status))
          set_var(group->result_to, WEXITSTATUS(child_status));
        else
          set_var(group->result_to, -WTERMSIG(child_status));
      }
    }
  }
}

/* This function is run for a group of commands separated
  by the || operator. All commands are run at the same time,
  and once any command finishes the rest are terminated. */
static void run_or_commands(script_group *group) {
  // sigset_t sigs, empty_mask;
  // Sigemptyset(&sigs);
  // Sigemptyset(&empty_mask);
  // Signal(SIGINT, ctl_c_handler);
  // Signal(SIGCHLD, sigchld_handler);
  // Sigaddset(&sigs, SIGINT);
  // Sigprocmask(SIG_BLOCK, &sigs, NULL);

  size_t num_commands = group->num_commands;
  pid_t pids[num_commands];
  pid_t pid;
  size_t command_i;
  for(command_i = 0; command_i < num_commands; command_i++){
    if((pid = Fork()) == 0){
      setpgid(0, 0);
      const char **argv = get_argv(&group->commands[command_i]);
      Execve(argv[0], (char * const *)argv, environ);
    }
    pids[command_i] = pid;
    set_pid_var(group, command_i, pid);
  }

  // Sigsuspend(&empty_mask);
  // if(got_ctl_c){
  //   terminate_processes(pids, num_commands, -1);
  //   if(group->result_to != NULL)
  //     set_var(group->result_to, -2);
  //   got_ctl_c = 0;
  // }
  // else{
    int child_status;
    pid = Wait(&child_status);
    terminate_processes(pids, num_commands, pid);
    if(group->result_to != NULL){
      if(WIFEXITED(child_status))
        set_var(group->result_to, WEXITSTATUS(child_status));
      else
        set_var(group->result_to, -WTERMSIG(child_status));
    }


  //}
  return;
}

/* This function is called when a SIGINT signal is received. */
static void ctl_c_handler(int sig){
  got_ctl_c = 1;
}

/* This function intentionally left empty. */
static void sigchld_handler(int sig){
}

static void terminate_processes(pid_t* pids, int num_pids, pid_t except_pid){
  int i;
  for(i = 0; i < num_pids; i++){
    if(pids[i] != except_pid)
      Kill(pids[i], SIGTERM);
  }
}
/* For a commands in a group, sets any pid variables.
  For example, in "/bin/sleep 1000 @ $sleep"
  the variable $sleep is set to the pid of the command*/
static void set_pid_var(script_group* group, int command_index, pid_t pid){
    if(group->commands[command_index].pid_to != NULL)
      set_var(group->commands[command_index].pid_to, pid);
}

/* Closes a process's unused pipe ends
*/
static void close_unused_pipes(PIPE* pipes_arr, int num_pipes, int open_wr_i, int open_rd_i){
  int pipes_i;
  for(pipes_i = 0; pipes_i < num_pipes; pipes_i++){
    if(open_rd_i != pipes_i){
      close(pipes_arr[pipes_i][0]);
    }
    if(open_wr_i != pipes_i){
      close(pipes_arr[pipes_i][1]);
    }
  }
}

const char** get_argv(script_command *command){
  const char **argv;
  int i;

  argv = malloc(sizeof(char *) * (command->num_arguments + 2));
  argv[0] = command->program;

  for (i = 0; i < command->num_arguments; i++) {
    if (command->arguments[i].kind == ARGUMENT_LITERAL)
      argv[i+1] = command->arguments[i].u.literal;
    else
      argv[i+1] = command->arguments[i].u.var->value;
  }

  argv[command->num_arguments + 1] = NULL;

  return argv;
}

/* You'll likely want to use this set_var function for converting a
   numeric value to a string and installing it as a variable's
   value: */

static void set_var(script_var *var, int new_value) {
  char buffer[32];
  free((void *)var->value);
  snprintf(buffer, sizeof(buffer), "%d", new_value);
  var->value = strdup(buffer);
}
