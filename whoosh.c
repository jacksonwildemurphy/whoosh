/* This is the main file for the `whoosh` interpreter and the part
   that you modify. */

#include <stdlib.h>
#include <stdio.h>
#include "csapp.h"
#include "ast.h"
#include "fail.h"

typedef int PIPE[2];

static void run_script(script *scr);
static void run_group(script_group *group);
static void run_command(script_group *group);
static void set_var(script_var *var, int new_value);
static void close_unused_pipes(PIPE* pipes_arr, int num_pipes, int open_wr_i, int open_rd_i);
static void run_and_commands(script_group *group);
static const char** get_argv(script_command *command);
static void set_pid_var(script_group* group, int command_index, int pid);


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


  // if (scr->num_groups == 1) {
  //   run_group(&scr->groups[0]);
  // } else {
  //   /* You'll have to make run_script do better than this */
  //   fail("only 1 group supported");
  // }
}

static void run_group(script_group *group) {
  int i, repeats = group->repeats;
  for(i = 0; i < repeats; i++){
    // if (group->result_to != NULL)
    //   fail("setting variables not supported");

    if (group->num_commands == 1) {
      run_command(group);
    } else if (group->mode == GROUP_AND) {
      run_and_commands(group);
    }
  }
}

/* This run_command function is for the special case where
  a group has a single command. */

static void run_command(script_group *group) {
  script_command* command = &group->commands[0];
  pid_t pid;
  const char **argv = get_argv(&group->commands[0]);

  if((pid = Fork()) == 0)
    Execve(argv[0], (char * const *)argv, environ);

  free(argv);
  int child_status;
  (void)Waitpid(pid, &child_status, 0);

  if(command->pid_to != NULL)
    set_var(command->pid_to, pid);
  if(group->result_to != NULL)
    set_var(group->result_to, WEXITSTATUS(child_status));
}

/* This run_command function is a good start, but note that it runs
   the command as a replacement for the `whoosh` script, instead of
   creating a new process. */

static void run_and_commands(script_group *group) {
  size_t num_commands = group->num_commands;
  size_t num_pipes = num_commands - 1;
  PIPE pipes_arr[(num_commands-1)];
  int pids[num_commands];
  int pid;

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


  close_unused_pipes(pipes_arr, num_pipes, -1, -1);
  for(command_i = 0; command_i < num_commands; command_i++)
    (void)Wait(NULL);

}

/* For a commands in a group, sets any pid variables.
  For example, in "/bin/sleep 1000 @ $sleep"
  the variable $sleep is set to the pid of the command*/
static void set_pid_var(script_group* group, int command_index, int pid){
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
