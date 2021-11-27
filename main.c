#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <math.h>
#include <fcntl.h>
#include <sys/wait.h>

// ignoreBackgroundCommand declared as global var so SIGTSTP's signal-handler can modify it
int ignoreBackgroundCommand = 0;
// print message and set above variable appropriately upon SIGTSTP command
void handleSIGTSTP(int signalNum) {
    if(ignoreBackgroundCommand) {   //only-foreground mode is now on, turning it off
        ignoreBackgroundCommand = 0;
        char* exitForegroundOnly = "\nExiting foreground-only mode\n";
        write(STDOUT_FILENO, exitForegroundOnly, 30);
        fflush(stdout);
    }
    else {  // only-foreground mode is now off, turning it on
        ignoreBackgroundCommand = 1;
        char* enterForegroundOnly = "\nEntering foreground-only mode (& is now ignored)\n";
        write(STDOUT_FILENO, enterForegroundOnly, 50);
        fflush(stdout);
    }
}

// return whether a user entry is a comment line (empty or starts with #)
int isCommentLine(char line[], int numChars) {
    if(line[0] == '#' || numChars == 1) {
        return 1;
    }
    return 0;
}

// takes any string and converts all occurances of "$$" to the parent PID
void expandLineVariable(char argument[], int numChars) {
    int intPID = getpid();
    char stringPID[64];
    sprintf(stringPID, "%d", intPID);   // convert PID to string
    int pidLen = strlen(stringPID), argLen = strlen(argument), currIndex = 0;
    char expandedArg[numChars];     // will hold the expanded variable by end of execution
    int skipNext = 0;   // used to skip an iteration when needed

    for(size_t i = 0; i < argLen - 1; i++) {
        if(skipNext) {
            skipNext = 0;
            if(i == argLen - 2) {  // if last iteration (edge case) 
                expandedArg[currIndex] = argument[i + 1];   // assign final letter
            }
            continue;
        }
        if(argument[i] == '$' && argument[i + 1] == '$') {
            // if occurance of "$$" is found
            strcat(expandedArg, stringPID);
            currIndex += pidLen;
            skipNext = 1;   // skip next to avoid reiterating the second '$'
        }
        else {
            expandedArg[currIndex] = argument[i];
            currIndex += 1;
            if(i == argLen - 2) {  // if last iteration (edge case) 
                expandedArg[currIndex] = argument[i + 1];   // assign final letter
            }
        }
    }
    strcpy(argument, expandedArg);  // copy final result back into original variable
    memset(expandedArg, '\0', numChars);
}

// takes a command line and extracts its details into appropriate variables
void extractLineDetails(
        char line[], 
        int numChars, 
        char command[],
        char arguments[512][numChars],
        char inputFile[],
        char outputFile[],
        int* backgroundExec
    ) {
    char *saveptr;
    // helper vars to extract info from line...
    char nextComm;
    int hasArgs = 1;
    int hasInputFile = 0;

    // extract command
    char *token = strtok_r(line, " ", &saveptr);
    token[strcspn(token, "\n")] = 0;    // clip off a potential '\n'
    strcpy(command, token);

    // extract all arguments
    for(size_t i = 0; i < 512; i++) {
        token = strtok_r(NULL, " ", &saveptr);
        if(token == NULL) {
            // if line only consisted of the command
            break;
        }
        token[strcspn(token, "\n")] = 0;
        if(!strcmp(token, "<") || !strcmp(token, ">") || !strcmp(token, "&")) {
            // if there were no args, but other symbols after the command
            hasArgs = 0;
            nextComm = *token;
            break;
        }
        strcpy(arguments[i], token);
        if((numChars - 1) < (token - line + strlen(token) + 1)) {
            // if this is the last argument AND last part of the line
            break;
        }
        nextComm = line[token - line + strlen(token) + 1];  // find next symbol in command line
        if(nextComm == '<' || nextComm == '>' || nextComm == '&') {
            // if this is the last argument AND there's more parts to the line
            break;          
        }
    }

    // extract input file, if present
    if(nextComm == '<') {
        if(hasArgs) {
            token = strtok_r(NULL, " ", &saveptr);  // this token holds the '<'
        }
        token = strtok_r(NULL, " ", &saveptr);
        token[strcspn(token, "\n")] = 0;
        hasInputFile = 1;
        strcpy(inputFile, token);
    }

    // extract output file
    if(hasInputFile || (nextComm == '>' && hasArgs)) {
        // if the '>' has not been accounted for yet
        token = strtok_r(NULL, " ", &saveptr);  // this token holds the '>'
    }
    if(token != NULL) {
        token[strcspn(token, "\n")] = 0;  
        if(!strcmp(token, ">")) {
            token = strtok_r(NULL, " ", &saveptr);
            token[strcspn(token, "\n")] = 0;
            strcpy(outputFile, token);
        }
    }

    // extract '&' symbol
    if(line[numChars - 2] == '&') {
        *backgroundExec = 1;
    }
    else {
        *backgroundExec = 0;
    }
}

// return whether the command is a built-in command or not
int isDefaultCommand(char command[]) {
    if(strcmp(command, "cd") && strcmp(command, "exit") && strcmp(command, "status")) {
        return 0;
    }
    return 1;
}

// iterates through all part of command line and applies 'expandLineVariable' to them
void expandInputs(   
    int numChars,
    int argCount, 
    char command[],
    char arguments[512][numChars],
    char inputFile[],
    char outputFile[]
    ) {
    if(command[0] != '\0') {
        expandLineVariable(command, numChars);
    }
    for(size_t i = 0; i <= argCount; i++) {
        if(arguments[i][0] != '\0' && strstr(arguments[i], "$$")){
            // if argument is present and there is a "$$" substring within it
            expandLineVariable(arguments[i], numChars);
        }
    }
    if(inputFile[0] != '\0') {
        expandLineVariable(inputFile, numChars);
    }
    if(outputFile[0] != '\0') {
        expandLineVariable(outputFile, numChars);
    }
}

// redirects either stdin or stdout (determined by isOutput param) to a provided file
void redirectFile(char fileName[], int* status, int isOutput) {
    int openedFile;
    if(isOutput) {  // opening a file to write into
        if(strcmp(fileName, "/dev/null")) {
            // if the file was passed in by user
            openedFile = open(fileName, O_WRONLY | O_CREAT | O_TRUNC, 0777);
        }
        else {
            // if no file was passed in, open "/dev/null"
            openedFile = open(fileName, O_WRONLY);
        }
    }
    else {  // opening a file to read from
        openedFile = open(fileName, O_RDONLY);
    }
    if(openedFile == -1) {
		perror("open()");
		*status = 1;
	}
    else {
        // call dup2 to appropriately redirect input or output
        if(isOutput) {
            dup2(openedFile, 1);
        }
        else {
            dup2(openedFile, 0);
        }
    }
}

// finds the number of arguments in a command line
int getArgCount(int numChars, char arguments[512][numChars]) {
    int argCount = 0;
    for(size_t i = 0; i < 512; i++) {
        if(arguments[i][0] == '\0') {
            return argCount;
        }
        else {
            argCount += 1;
        }
    }
    return argCount;
}

// change directory to given path (relative OR absolute)
// if no path was given, change to HOME directory
void changeDir(char pathArg[], int argCount) {
    if(argCount == 0) {    // no argument was given
        char* homePath = getenv("HOME");
        chdir(homePath);
    }
    else {  // cd to argument path
        chdir(pathArg);
    }
}

// executes a non-built-in command using fork() and exec()
void executePathCommand(
    int numChars,
    int argCount,
    char command[], 
    char arguments[512][numChars],
    int* status,
    int backgroundExec,
    int currentBackgroundPIDS[],
    int* backgroundIndex
    ) {
    // based on example from 'Executing a New Program' exploration
    int childStatus;
    // put command and arguments in required list format for execvp
    char *execParam[argCount + 2];
    execParam[0] = command;
    for(int i = 0; i < argCount; i++) {
        execParam[i + 1] = arguments[i];
    }
    execParam[argCount + 1] = NULL;

	pid_t spawnPid = fork();    // create child process
	switch(spawnPid){
	case -1:
		exit(1);
		break;
	case 0: // child process
        // return back to default SIGINT if action is in foreground
        if(!backgroundExec) signal(SIGINT, SIG_DFL);
        signal(SIGTSTP, SIG_IGN); // ignore SIGTSTP
		execvp(execParam[0], execParam);
		perror("execvp");
		exit(2);
		break;
	default:    // parent process
        if(!backgroundExec) {
            // if foreground process
            spawnPid = waitpid(spawnPid, &childStatus, 0);
            if(WIFEXITED(childStatus)) {
                // exited properly, update status
                *status = WEXITSTATUS(childStatus);
            }
            else if(WIFSIGNALED(childStatus)) {
                // exited abnormally, print message and update status
                *status = WTERMSIG(childStatus);
                printf("\nterminated by signal %d\n", *status);
                fflush(stdout);
            }
        }
        else {
            // if background process, append to tracker array and updated its index
            currentBackgroundPIDS[*backgroundIndex] = spawnPid;
            *backgroundIndex += 1;
        }
		break;
	}
}

// check-in on the currently running background processes
void checkBackgroundProcesses(
    int currentBackgroundPIDS[], 
    int backgroundIndex
    ) {
    int childStatus;
    for(size_t i = 0; i < backgroundIndex; i++) {
        int currPID = currentBackgroundPIDS[i];
        if(currPID != -1) { // if this background process is currently running
            int returnedPID = waitpid(currPID, &childStatus, WNOHANG);
            if(returnedPID != 0) {  // if child has finished
                currentBackgroundPIDS[i] = -1;
                printf("background pid %d is done: exit value %d\n",
                currPID, childStatus);
                fflush(stdout);
            }
        }
    }
}

// takes a command line and parses/executes it
void processLine(
    char line[], 
    int numChars, 
    int* status, 
    int* running,
    int currentBackgroundPIDS[],
    int* backgroundIndex) {
    if(isCommentLine(line, numChars)) {
        return;
    }
    // following variables hold the individual parts of the command line
    char command[numChars];
    char arguments[512][numChars];
    char inputFile[numChars];
    char outputFile[numChars];
    int backgroundExec = 0;
    
    // 'clear out' vars for each iteration
    command[0] = '\0';
    for(size_t i = 0; i < 512; i++)
        arguments[i][0] = '\0';
    inputFile[0] = '\0';
    outputFile[0] = '\0';

    // populate above variables based on the given command line
    extractLineDetails(line, numChars, command, arguments, 
    inputFile, outputFile, &backgroundExec);
    // get the number of arguments
    int argCount = getArgCount(numChars, arguments);
    // ignore backgroundExec if default command or SIGTSTP is on
    if(ignoreBackgroundCommand || isDefaultCommand(command)) backgroundExec = 0;
    // change all occurances of '$$' to the parent PID
    expandInputs(numChars, argCount, command, arguments, inputFile, outputFile);
        
    // redirect all output to specified file if requested
    int outRedirected = 0;  // if the output was redirected to a file or not
    int oldStdOut;  // used to return back to stdout afterwards
    if(outputFile[0] != '\0' || backgroundExec) {
        // if there's no output file, or if command needs to execute in background
        outRedirected = 1;
        oldStdOut = dup(STDOUT_FILENO);
        if(outputFile[0] == '\0') {
            // no output file was given, but command needs to execute in background
            strcpy(outputFile, "/dev/null");
        }
        redirectFile(outputFile, status, 1);
    }

    int inRedirected = 0;  // if the input was redirected to a file
    int oldStdIn;  // used to return back to stdin afterwards
    if(inputFile[0] != '\0' || backgroundExec) {
        // if there's no input file or command is executing in background
        inRedirected = 1;
        oldStdIn = dup(STDIN_FILENO);
        if(inputFile[0] == '\0') {
            // no input file was given, but command needs to execute in background
            strcpy(inputFile, "/dev/null");
        }
        redirectFile(inputFile, status, 0);
    }

    // handle default commands
    if(!strcmp(command, "exit")) {
        *running = 0;
    }
    else if(!strcmp(command, "cd")) {
        changeDir(arguments[0], argCount);
    }
    else if(!strcmp(command, "status")) {
        printf("exit status %d\n", *status);
        fflush(stdout);
    }
    else {
        // if not a default command
        executePathCommand(numChars, argCount, command, arguments, status,
        backgroundExec, currentBackgroundPIDS, backgroundIndex);
    }

    // return back to stdout/stdin if it was changed
    if(outRedirected) {
        dup2(oldStdOut, STDOUT_FILENO);
        close(oldStdOut);
    }
    if(inRedirected) {
        dup2(oldStdIn, STDIN_FILENO);
        close(oldStdIn);
    }

    // if background process, tell user the background processes' PID
    if(backgroundExec) {
        printf("background pid is %d\n", 
        currentBackgroundPIDS[*backgroundIndex - 1]);
        fflush(stdout);
    }
    checkBackgroundProcesses(currentBackgroundPIDS, *backgroundIndex);
}

int main() {
    // declare env vars
    int currentBackgroundPIDS[512], running = 1, status = 0, backgroundIndex = 0;

    // handle SIGTSTP signal
	struct sigaction SIGINT_action = {0};
	SIGINT_action.sa_handler = handleSIGTSTP;
	sigfillset(&SIGINT_action.sa_mask);
	SIGINT_action.sa_flags = 0;
    sigaction(SIGTSTP, &SIGINT_action, NULL);

    // handle SIGINT signal
    signal(SIGINT, SIG_IGN);

    while(running) {
        printf(": ");
        fflush(stdout);
        size_t maxLineSize = 2048;
        char* userInput = (char*) malloc (maxLineSize * sizeof(char));
        // get command line from user
        int numChars = getline(&userInput, &maxLineSize, stdin);
        if(numChars == -1) {
            clearerr(stdin);    // getline was interfered with, clear stdin and retry
        }
        else {
            char line[2049];    // copy to char array for easier processing
            strcpy(line, userInput);
            processLine(line, numChars, &status, &running, 
            currentBackgroundPIDS, &backgroundIndex);
        }
    }
}
