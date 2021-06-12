#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <signal.h>

extern int errno; // for perror
volatile sig_atomic_t fgonly = 0; // type makes sure any changes or reads by signals are uninterrupted

// SIGTSTP (ctrl-z) handler: activates foreground-only mode
// check if the global fg-only flag is set, toggle it, and print text
void sigtstphandler(int signo) { 
    if(fgonly == 0) {
        write(STDOUT_FILENO, "Entering foreground-only mode (& is now ignored)\n: ", 52);
        fgonly = 1;
    }
    else if(fgonly == 1) {
        write(STDOUT_FILENO, "Exiting foreground-only mode\n: ", 32);
        fgonly = 0;
    }
}

// prints/sets appropriate status message for terminated child process
void waithandler(pid_t spawnpid, int childexitmethod, char status[50], int bg) {
    int exitstatus, termsig;
    // if process exited normally
    if(WIFEXITED(childexitmethod) != 0) {
        exitstatus = WEXITSTATUS(childexitmethod);
        if(bg == 0) { // only set status for foreground commands
            snprintf(status, 49, "exit value %d", exitstatus);
        }
        return;
    }
    // if process was killed by a signal
    else if(WIFSIGNALED(childexitmethod) != 0) {
        termsig = WTERMSIG(childexitmethod);
        printf("process %d terminated by signal %d\n", spawnpid, termsig);
        fflush(stdout);
        if(bg == 0) { // only set status for foreground commands
            snprintf(status, 49, "terminated by signal %d", termsig);
        }
        return;
    }
}

// executes non built in commands with fork() and exec()
void cmdexec(char argtokens[512][50], int numargs, int bg, int bgpids[10], char status[50]) {
    pid_t spawnpid = -5;
    int i, sourcefd, targetfd, dupres, finalnumargs=numargs, childexitmethod;
    char inputrd[30], outputrd[30];

    // clear input and output redirect strs
    memset(inputrd, '\0', sizeof(char)*30);
    memset(outputrd, '\0', sizeof(char)*30);

    // checks for input and output redirect and adjusts list of args accordingly
    for(i=1; i<numargs; i++) {
        // if source file is specified, copy filename to buffer
        // and remove the < and filename from list of args
        if(strcmp(argtokens[i], "<") == 0) {
            strcpy(inputrd, argtokens[i+1]);
            memset(argtokens[i], '\0', sizeof(char)*20);
            memset(argtokens[i+1], '\0', sizeof(char)*20);
            finalnumargs -= 2;
        }
        // if destination file is specified, copy filename to buffer
        // and remove the > and filename from list of args
        if(strcmp(argtokens[i], ">") == 0) {
            strcpy(outputrd, argtokens[i+1]);
            memset(argtokens[i], '\0', sizeof(char)*20);
            memset(argtokens[i+1], '\0', sizeof(char)*20);
            finalnumargs -= 2;
        }
    }
    finalnumargs++; // add room for NULL str at the end

    spawnpid = fork();
    if(spawnpid == -1) {
        perror("fork error");
    }
    else if(spawnpid == 0) { // child process
        // setup to ignore all signals except SIGINT (ctrl-c)
        struct sigaction sigintaction = {0}, ignoreaction = {0};

        sigintaction.sa_handler = SIG_DFL; // default action after exec() - terminate on SIGINT
        sigfillset(&sigintaction.sa_mask); 
        sigintaction.sa_flags = 0;

        ignoreaction.sa_handler = SIG_IGN;

        sigaction(SIGINT, &sigintaction, NULL);
        sigaction(SIGTERM, &ignoreaction, NULL);
        sigaction(SIGHUP, &ignoreaction, NULL);
        sigaction(SIGQUIT, &ignoreaction, NULL);
        sigaction(SIGTSTP, &ignoreaction, NULL);

        // build array of pointers from processed list of args to pass to execvp()
        char *execargs[finalnumargs];
        for(i=0; i<finalnumargs-1; i++) {
            execargs[i] = argtokens[i];
        }
        execargs[finalnumargs-1] = NULL;

        // opens specified input file and redirects stdin
        if(inputrd[0] != '\0') {
            sourcefd = open(inputrd, O_RDONLY);
            if(sourcefd == -1) {
                perror("source open()");
                exit(1);
            }
            dupres = dup2(sourcefd, 0);
            if(dupres == -1) {
                perror("source dup2() error");
                exit(1);
            }
        }
        // opens specified output file and redirects stdout
        if(outputrd[0] != '\0') {
            targetfd = open(outputrd, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if(targetfd == -1) {
                perror("target open()");
                exit(1);
            }
            dupres = dup2(targetfd, 1);
            if(dupres == -1) {
                perror("source dup2() error");
                exit(1);
            }
        }        
        // call exec, return 1 if failed
        execvp(execargs[0], execargs);
        perror("exec failed");
        exit(1);
    }

    // if process is in foreground, call waitpid() on it and handle the termination
    if(bg == 0) {     
        waitpid(spawnpid, &childexitmethod, 0);
        waithandler(spawnpid, childexitmethod, status, 0);
        return;
    }
    // if process is backgrounded, print pid and add to list of bg pids
    if(bg == 1) {
        printf("background pid is %d\n", spawnpid);
        fflush(stdout);
        i=0;
        while(bgpids[i] != 0) {
            i++;
        }
        bgpids[i] = spawnpid;
        return;
    }
}


int main() {
    char inputstr[2048];
    char argtokens[512][50];
    char *tok, cwdbuf[50], *varexp, temp[20], pidstr[10], status[50];
    int tokitr, i, cdres, pid, bg=0, bgpids[100], childpid, childexitmethod;
    struct sigaction ignoreaction = {0}, sigtstpaction = {0};

    sigtstpaction.sa_handler = sigtstphandler; // set SIGTSTP handler
    sigfillset(&sigtstpaction.sa_mask);        
    sigtstpaction.sa_flags = SA_RESTART;
    ignoreaction.sa_handler = SIG_IGN;

    sigaction(SIGTSTP, &sigtstpaction, NULL); // set SIGTSTP handler
    sigaction(SIGINT, &ignoreaction, NULL);   // ignore SIGINT so the parent process (shell) doesn't terminate
    sigaction(SIGTERM, &ignoreaction, NULL);
    sigaction(SIGHUP, &ignoreaction, NULL);
    sigaction(SIGQUIT, &ignoreaction, NULL);

    memset(bgpids, 0, sizeof(int)*10);
    memset(status, '\0', sizeof(char)*50);
    strcpy(status, "exit value 0"); // initialize status for shell startup
    
    while(1) {
        memset(argtokens, '\0', sizeof(char)*512*20); // clear args list for every new command

        // check every background process in the list to see if they're terminated
        for(i=0; i<100; i++) {
            if(bgpids[i] != 0) { // if there is a pid, call waitpid(WNOHANG) on it
                childpid = waitpid(bgpids[i], &childexitmethod, WNOHANG);
                if(childpid < 0) {
                    perror("error with waitpid()");
                }
                else if(childpid > 0) {
                    waithandler(bgpids[i], childexitmethod, status, 1); // print message when terminated
                    printf("background pid %d is done: %s\n", bgpids[i], status);
                    fflush(stdout);
                    bgpids[i] = 0; // remove background pid from the list
                }

            }
        }

        tokitr=1; // token iterator; start at 1 because the first strtok happens outside the loop
        printf(": ");
        fflush(stdout);
        fgets(inputstr, 2048, stdin); // input str is max 2048 chars
        
        // tokenize first token of input str
        tok = strtok(inputstr, " "); 
        strcpy(argtokens[0], tok);
        // tokenize the rest of the str
        while(tok != NULL) {
            tok = strtok(NULL, " ");
            if(tok == NULL) {
                break; // if strtok does not return any more tokens, break
            }
            strcpy(argtokens[tokitr], tok);
            // if token contains instance of $$, replace with current pid
            varexp = strstr(argtokens[tokitr], "$$"); // look for substring
            if(varexp != NULL) {
                pid = getpid();
                sprintf(pidstr, "%d", pid); // itoa equivalent
                strcpy(temp, varexp+2);     // copy out rest of str after the $$
                strcpy(varexp, pidstr);     // copy pid into location of $$ in token
                varexp += strlen(pidstr);   // increment ptr by length of pid
                strcpy(varexp, temp);       // copy rest of str back in
            }

            tokitr++;
        }
        // remove newline from end of last arg
        i = strlen(argtokens[tokitr-1])-1;
        argtokens[tokitr-1][i] = '\0'; 
        
        // checks for built in commands
        // ignores inputs that begin with # or have length 0 and reprompts
        if(argtokens[0][0] != '#' && strlen(argtokens[0]) > 1) {
            // exits shell
            if(strcmp(argtokens[0], "exit") == 0) {
                exit(0);
            }
            // changes to specified directory or to HOME if no dir specified
            else if(strcmp(argtokens[0], "cd") == 0) {
                if(strlen(argtokens[1]) == 0) {
                    cdres = chdir("/nfs/stak/users/maoa");
                }
                else{ cdres = chdir(argtokens[1]); }
                if(cdres) { // chdir returns non zero on failure
                    perror("error changing dir");
                }
            }
            // prints the status of the last command executed
            else if(strcmp(argtokens[0], "status") == 0) {
                printf("%s\n", status);
                fflush(stdout);
            }
            // any other command will be sent to exec()
            else { // if last arg is & and foreground-only mode is not on, set background flag
                if(strcmp(argtokens[tokitr-1], "&") == 0) {
                    if(fgonly == 0) {
                        bg = 1; 
                    }
                    else bg = 0;
                    argtokens[tokitr-1][0] = '\0'; // remove & from arg list and dec arg count
                    tokitr--;
                }
                else bg = 0;
                cmdexec(argtokens, tokitr, bg, bgpids, status);
            }
        }

    }
    return 0;
}