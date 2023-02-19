#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <signal.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include "tokenizer.h"

/* Convenience macro to silence compiler warnings about unused function parameters. */
#define unused __attribute__((unused))

#define pdebug printf("here\n")

/* Whether the shell is connected to an actual terminal or not. */
bool shell_is_interactive;

/* File descriptor for the shell input */
int shell_terminal;

/* Terminal mode settings for the shell */
struct termios shell_tmodes;

/* Process group id for the shell */
pid_t shell_pgid;

/* PATH -- environmental variable */
char* PATH = "/home/vagrant/.vscode-server/bin/da76f93349a72022ca4670c1b84860304616aaa2/bin/rem\
ote-cli:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin:/usr/games:/usr/local/game\
s:/snap/bin:/usr/local/go/bin:/home/vagrant/.bin:/home/vagrant/.cargo/bin:/home/vagrant/.fzf/bin\
:/home/vagrant/.bin:/home/vagrant/.cargo/bin";

int cmd_exit(struct tokens* tokens);
int cmd_help(struct tokens* tokens);
int cmd_cd(struct tokens* tokens);
int cmd_pwd(struct tokens* tokens);

/* Built-in command functions take token array (see parse.h) and return int */
typedef int cmd_fun_t(struct tokens* tokens);

/* Built-in command struct and lookup table */
typedef struct fun_desc {
  cmd_fun_t* fun;
  char* cmd;
  char* doc;
} fun_desc_t;

fun_desc_t cmd_table[] = {
    {cmd_help, "?", "show this help menu"},
    {cmd_exit, "exit", "exit the command shell"},
    {cmd_pwd, "pwd", "print current working directory"},
    {cmd_cd, "cd", "open a directory"},
};

/* Changes current working dir to another */
int cmd_cd(struct tokens* tokens) {
  int err = chdir(tokens_get_token(tokens, 1));
  if (err == -1) {
    fprintf(stderr, "%s\n", strerror(errno));
    return -1;
  }
  return 1;
}

/* Prints current working dir */
int cmd_pwd(unused struct tokens* tokens) {
  static char buf[256];
  char* cwd = getcwd(buf, sizeof buf);
  if (cwd == NULL) {
    fprintf(stderr, "%s\n", strerror(errno));
    return -1;
  }
  fprintf(stdout, "%s\n", cwd);
  return 1;
}

/* Prints a helpful description for the given command */
int cmd_help(unused struct tokens* tokens) {
  for (unsigned int i = 0; i < sizeof(cmd_table) / sizeof(fun_desc_t); i++)
    printf("%s - %s\n", cmd_table[i].cmd, cmd_table[i].doc);
  return 1;
}

/* Exits this shell */
int cmd_exit(unused struct tokens* tokens) { exit(0); }

/* Looks up the built-in command, if it exists. */
int lookup(char cmd[]) {
  for (unsigned int i = 0; i < sizeof(cmd_table) / sizeof(fun_desc_t); i++)
    if (cmd && (strcmp(cmd_table[i].cmd, cmd) == 0))
      return i;
  return -1;
}

/* Intialization procedures for this shell */
void init_shell() {
  /* Our shell is connected to standard input. */
  shell_terminal = STDIN_FILENO;

  /* Check if we are running interactively */
  shell_is_interactive = isatty(shell_terminal);

  if (shell_is_interactive) {
    /* If the shell is not currently in the foreground, we must pause the shell until it becomes a
     * foreground process. We use SIGTTIN to pause the shell. When the shell gets moved to the
     * foreground, we'll receive a SIGCONT. */
    while (tcgetpgrp(shell_terminal) != (shell_pgid = getpgrp()))
      kill(-shell_pgid, SIGTTIN);

    /* Saves the shell's process id */
    shell_pgid = getpid();

    /* Take control of the terminal */
    tcsetpgrp(shell_terminal, shell_pgid);

    /* Save the current termios to a variable, so it can be restored later. */
    tcgetattr(shell_terminal, &shell_tmodes);

    /* Signal handle */
    signal(SIGINT, SIG_IGN);
    signal(SIGTSTP, SIG_IGN);
  }
}

/*Executes a single process, whose args are from start to finish */
void single_program(struct tokens* tokens, int start, int finish) {
  /* Redirection */
  int redir = 0;
  for (int i = start; i < finish; i++) {
    if (strcmp(tokens_get_token(tokens, i), ">") == 0) {
      redir = 1;
      if (freopen(tokens_get_token(tokens, i + 1), "a", stdout) == NULL) {
        perror("freopen filed");
        exit(-1);
      }
    }
    else if (strcmp(tokens_get_token(tokens, i), "<") == 0) {
      redir = 1;
      if (freopen(tokens_get_token(tokens, i + 1), "r", stdin) == NULL) {
        perror("freopen filed");
        exit(-1);
      }
    }
  }

  /* Init argv[] */
  char** args = malloc(sizeof(char*) * (finish - start + 1 - 2 * redir));
  int x = 0;
  for (int i = start; i < finish; i++) {
    char* tokeni = tokens_get_token(tokens, i);
    if (strcmp(tokeni, ">") == 0 || strcmp(tokeni, "<") == 0) {
      /* Skip 2 words */
      i++;
    }
    else {
      args[x++] = tokeni;
    }
  }
  args[finish - start - 2 * redir] = NULL;

  /* Execute program */
  char* exefile = tokens_get_token(tokens, start);

  if (*exefile == '/') {  //Full path
    execv(exefile, args);

    /* Program arrive here only if execv() failed */
    perror("execv failed");
  }
  else {    //Not a full path, use PATH ev
    char* strh = PATH;
    int tokenlen = strlen(exefile);
    while (true) {
      /* Open next default path */
      int i;
      for (i = 0; *(strh + i) != ':' && *(strh + i) != '\0'; i++);
      char* pbuf = malloc(sizeof(char) * (i + 1 + tokenlen + 1));
      strncpy(pbuf, strh, i + 1);
      pbuf[i] = '/';
      strcpy(pbuf + i + 1, exefile);

      execv(pbuf, args);

      if (errno != ENOENT)
        perror("execv failed");

      /* Change strh to next path */
      if (*(strh + i) == ':')
        strh = strh + i + 1;
      else if (*(strh + i) == '\0')
        break;
    }
    fprintf(stderr, "Command '%s' not found\n", exefile);
  }
  exit(-1);
}

//Executes programs
void exec_programs(struct tokens* tokens) {
  /* Runs a given program, if valid path */
  pid_t cpid = fork();
  if (cpid == 0) {  //child
    int tokens_len = tokens_get_length(tokens);

    /* Signal handle */
    signal(SIGINT, SIG_DFL);
    signal(SIGTSTP, SIG_DFL);

    /* Count the no of pipe */
    int pipenum = 0;
    for (int i = 0; i < tokens_get_length(tokens); i++) {
      if (strcmp(tokens_get_token(tokens, i), "|") == 0) {
        pipenum++;
      }
    }

    /* No pipe */
    if (pipenum == 0){
      if(tokens_len > 0)
        single_program(tokens, 0, tokens_len);
      else 
        exit(-1);
    }

    /* One or more pipe*/
    int idx = 0;
    int* pipeidx = malloc(sizeof(int) * pipenum);
    for (int i = 0; i < tokens_get_length(tokens); i++) {
      if (strcmp(tokens_get_token(tokens, i), "|") == 0) {
        pipeidx[idx++] = i;
      }
    }
    for (int i = 0; i < pipenum; i++) {
      int pipefd[2];
      if (pipe(pipefd) == -1)
        perror("pipe failed");

      pid_t cpid = fork();
      if (cpid > 0) {
        close(pipefd[0]);
        if (dup2(pipefd[1], STDOUT_FILENO) == -1)
          perror("dup2 failed");
        if (i == 0 && pipeidx[i] > 0)
          single_program(tokens, 0, pipeidx[i]);
        else if(pipeidx[i-1] + 1 < pipeidx[i])
          single_program(tokens, pipeidx[i - 1] + 1, pipeidx[i]);
          
        if (wait(NULL) == -1)
          perror("wait failed");
      }
      else if (cpid == 0) {
        close(pipefd[1]);
        if (-1 == dup2(pipefd[0], STDIN_FILENO))
          perror("dup2 failed");
        if (i == pipenum - 1&& pipeidx[i]+1 < tokens_len)
          single_program(tokens, pipeidx[i] + 1, tokens_len);
        printf("The program should not arrive here.\n");
        exit(-1);
      }
      else {
        perror("fork failed");
      }
    }
    printf("The program should not arrive here.\n");
    exit(-1);
  }
  else if (cpid == -1) {
    perror("fork failed");
  }
  else {        //terminal process
    /* Set child process group id to its pid */
    if(setpgid(cpid, cpid))
      perror("setpgid failed");
    
    /* Set foreground pgrp to child processes */
    if(tcsetpgrp(0, cpid))
      perror("tcset pgrp failed");
    if (wait(NULL) == -1)
      perror("wait failed");

    signal(SIGTTOU, SIG_IGN);
    if(tcsetpgrp(0, getpid()))
      perror("tcsetpgrp failed");
    signal(SIGTTOU, SIG_DFL);
  }
}

int main(unused int argc, unused char* argv[]) {
  init_shell();

  static char line[4096];
  int line_num = 0;

  /* Please only print shell prompts when standard input is not a tty */
  if (shell_is_interactive)
    fprintf(stdout, "%d: ", line_num);

  while (fgets(line, 4096, stdin)) {
    /* Split our line into words. */
    struct tokens* tokens = tokenize(line);

    /* Find which built-in function to run. */
    int fundex = lookup(tokens_get_token(tokens, 0));

    if (fundex >= 0) {
      cmd_table[fundex].fun(tokens);
    }
    else {
      exec_programs(tokens);
    }

    if (shell_is_interactive)
      /* Please only print shell prompts when standard input is not a tty */
      fprintf(stdout, "%d: ", ++line_num);

    /* Clean up memory */
    tokens_destroy(tokens);
  }

  return 0;
}
