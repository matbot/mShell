/* Mathew McDade
 * Spring 2019
 * smallsh: A simple shell. Supports three builtin functions: cd, status, and exit. All other
 * commands are forked and run using the exec() function. Non-builtin functions can be run in
 * background mode using the '&' character at the end of the command. Foreground only mode can be
 * toggled using the SIGTSTP signal, C^Z. When in foreground only mode, the background character
 * will be ignored. The shell supports a maximum of 512 arguments and a maximum input length of
 * 2048 characters. You probably won't use that many.
 */

#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <signal.h>
#include <fcntl.h>

#define MAX_ARGS 512
#define MAX_INPUT 2048

/* Global flag values to service foreground only mode via the TSTP signal handler. Per the GNU C
 * library, "the behavior is undefined if a signal handler reads any nonlocal object, or writes to
 * any nonlocal object whose type is not sig_atomic_t volatile."
 * https://www.gnu.org/software/autoconf/manual/autoconf-2.61/html_node/Volatile-Objects.html
 */
volatile sig_atomic_t allowBackground = 1;
volatile sig_atomic_t sigtstpTriggered = 0;
volatile sig_atomic_t processActive = 0;
//TSTP signal handler function declarations.
void allowbackground();
void foregroundonly();

//This is a large struct but it makes passing all command information much neater.
struct Command
{
	char rawCommand[MAX_INPUT];	//raw user input, will get destroyed by strtok in parser function.
	int numArgs;
	char* args[MAX_ARGS];		//an array to hold distinct command units.
	bool inputRedirect;			//flag for '<' command.
	char inputFile[50];
	bool outputRedirect;		//flag for '>' command.
	char outputFile[50];
	bool backgroundProcess;		//flag for '&' command.
	pid_t pid;
};

/* smallsh builtin: status
 * Takes no arguments. Prints the exit status of most recently completed process.
 */
void reportStatus(int status) {
	if(WIFEXITED(status)) {
		printf("exit value %d\n",WEXITSTATUS(status));
		fflush(stdout);
	}
	else if(WIFSIGNALED(status)) {
		printf("terminated by signal %d\n",WTERMSIG(status));
		fflush(stdout);
	}
}

/* smallsh builtin: cd
 * Takes a single argument, the path to the dir to be changed to. This can be the relative or
 * absolute path. If no argument is provided cd will change the current directory to the user's
 * HOME directory.
 */
void cd(char* dir) {
	if(dir==NULL) {
		chdir(getenv("HOME"));
	}
	else {
		if(chdir(dir)!=0) {
			perror("Nope");	//not today.
			fflush(stderr);
		}
	}
}


//Check if any zombies are available and, if so, print its pid and exit status.
void burnZombie() {
	pid_t zPid;
	int zStatus;
	//Don't wait for a zombie to appear, obviously.
	while((zPid = waitpid(0,&zStatus,WNOHANG)) > 0) {
		if(zPid > 0) {
			printf("background pid %d is done: ",zPid);
			reportStatus(zStatus);
		}
	}
}

//Burn all the zombies, kill all the children. Closing up shop.
void burnEverything() {
	int status;
	pid_t pid;
	while((pid = waitpid(-1,&status,WNOHANG)) > 0) {};
	//important not to terminate yourself, yet.
	signal(SIGTERM,SIG_IGN);
	kill(0,SIGTERM);
	while((pid = waitpid(-1,&status,WNOHANG)) > 0) {};
}

//signal handling tools.
void sigintIgnore() {
	struct sigaction SIGINT_action = {{0}};
	SIGINT_action.sa_handler = SIG_IGN;
	sigaction(SIGINT,&SIGINT_action,NULL);
}
void sigintDefault() {
	struct sigaction SIGINT_action = {{0}};
	SIGINT_action.sa_handler = SIG_DFL;
	sigaction(SIGINT,&SIGINT_action,NULL);
}
//The TSTP signal handler functions could definitely be combined, need to do this.			!!
//It was also difficult to get the desired behaviour of immediate vs. delayed message output.
void allowbackground() {
	struct sigaction query;
	if(sigaction(SIGTSTP,NULL,&query) == -1) {
		//handle error.
	}
	else {
		query.sa_handler = foregroundonly;
		if(sigaction(SIGTSTP,&query,NULL) == -1) {
			//handle error.
		}
		else {
			allowBackground = true;
			sigtstpTriggered = true;
			if(!processActive) {
				char* bgOn = "\nExiting foreground-only mode.\n: ";
				//using write() for reentrant safe output.
				write(STDOUT_FILENO, bgOn, strlen(bgOn));
				fflush(stdout);
				sigtstpTriggered = false;
			}
		}	 
	}
}
void foregroundonly() {
	struct sigaction query;
	if(sigaction(SIGTSTP,NULL,&query) == -1) {
		//handle error.
	}
	else {
		query.sa_handler = allowbackground;
		if(sigaction(SIGTSTP,&query,NULL) == -1) {
			//handle error.
		}
		else {
			allowBackground = false;
			sigtstpTriggered = true;
			if(!processActive) {
				char* bgOff = "\nEntering foreground-only mode (& is now ignored).\n: ";
				write(STDOUT_FILENO, bgOff, strlen(bgOff));
				fflush(stdout);
				sigtstpTriggered = false;
			}
		}
	}
}

void sigtstpSet() {
	struct sigaction sa;
	sa.sa_handler = foregroundonly;
	sigfillset(&sa.sa_mask);
	sa.sa_flags = SA_RESTART;
	if(sigaction(SIGTSTP,&sa,NULL) == -1) {
		perror("SIGTSTP init error.\n");
		fflush(stderr);
	}
}

//Takes care of all input and output redirection for commands.
void inputoutputRedirect(struct Command* cmnd) {
	int ifile,ofile;
	if(cmnd->inputRedirect) {
		//if input redirect without argument, redirect to /dev/null:
	     if(strlen(cmnd->inputFile)==0) {
			ifile = open("/dev/null",O_RDONLY);
			if(ifile == -1) {
				perror("smallsh: cannot open /dev/null input.\n");
				fflush(stderr);
				exit(EXIT_FAILURE);
			}
			if(dup2(ifile,STDIN_FILENO) == -1) {
				perror("smallsh: cannot open /dev/null input.\n");
				fflush(stderr);
				exit(EXIT_FAILURE);
			}
	     }
		//redirect from specified input file.
	     else {
	          ifile = open(cmnd->inputFile,O_RDONLY);
	          if(ifile==-1) {
	      	    fprintf(stderr,"Unable to open input file: %s.\n",cmnd->inputFile);
			    fflush(stderr);
	      	    exit(EXIT_FAILURE);
	          }
	          if(dup2(ifile,STDIN_FILENO) == -1) {
	      	    fprintf(stderr,"Unable to open input file: %s.\n",cmnd->inputFile);
			    fflush(stderr);
			    exit(EXIT_FAILURE);
	      	}
	     }
		close(ifile);
	}
	if(cmnd->outputRedirect) {
		//if output redirect without argument, redirect to /dev/null:
	     if(strlen(cmnd->outputFile)==0) {
			ofile = open("/dev/null",O_WRONLY|O_CREAT|O_TRUNC,0644);
			if(ofile == -1) {
				perror("smallsh: cannot open /dev/null output.\n");
				fflush(stderr);
				exit(EXIT_FAILURE);
			}
			if(dup2(ofile,STDOUT_FILENO) == -1) {
				perror("smallsh: cannot open /dev/null output.\n");
				fflush(stderr);
				exit(EXIT_FAILURE);
			}
	     }
		//redirect from specified input file.
	     else {
	          ofile = open(cmnd->outputFile,O_WRONLY|O_CREAT|O_TRUNC,0644);
	          if(ofile==-1) {
	      	    fprintf(stderr,"Unable to open output file: %s.\n",cmnd->outputFile);
			    fflush(stderr);
	      	    exit(EXIT_FAILURE);
	          }
	          if(dup2(ofile,STDOUT_FILENO) == -1) {
	      	    fprintf(stderr,"Unable to open output file: %s.\n",cmnd->outputFile);
			    fflush(stderr);
			    exit(EXIT_FAILURE);
	      	}
		}
		close(ofile);
	}
}

//Adapted from Block 3.3: Advanced User Input example code.
void getCommand(struct Command* cmnd) {
	int numChars = -1;
	size_t bufferSize = 0;
	char* line = NULL;
	//loops until user gives us a potentially viable command.
	while(1) {
		printf(": ");
		fflush(stdout);
		numChars = getline(&line,&bufferSize,stdin);
		//if the user entered nothing or if it's a comment line
		if(numChars <= 1 || line[0]==' ' || line[0]=='#') {
			clearerr(stdin);
		}
		else {
			//append a null terminator.
			line[strcspn(line,"\n")] = '\0';
			strcpy(cmnd->rawCommand,line);
			free(line);
			line = NULL;
			break;
		}
	}
}

//Turns the raw user command input into useful information in the Command struct.
void parseCommand(struct Command* cmnd) {
	char buff[2048];
	char* pos;
	//sweet $$ expansion. I like this one.
	while((pos = strstr(cmnd->rawCommand,"$$"))) {
		strcpy(buff,pos+2);
		sprintf(pos,"%d%s",getpid(),buff);
	}
	//Begin tokenizing the raw user input.
	char* token = strtok(cmnd->rawCommand," ");
	while(token != NULL) {
		//flag input redirect and store file name, might get broken by '< >' input.		!!
		if(strcmp(token,"<")==0) {
			cmnd->inputRedirect = true;
			token = strtok(NULL," ");
			strcpy(cmnd->inputFile,token);
			token = strtok(NULL," ");
		}
		//flag output redirect and store file name, might get broken by '> &' input.		!!
		else if(strcmp(token, ">")==0) {
			cmnd->outputRedirect = true;
			token = strtok(NULL," ");
			strcpy(cmnd->outputFile,token);
			token = strtok(NULL," ");
		}
		//else add the token to the command array.
		else {
			cmnd->args[cmnd->numArgs++] = token;
			token = strtok(NULL," ");
		}
	}
	//flag background process and remove & from command array.
	if(strcmp(cmnd->args[cmnd->numArgs-1], "&")==0) {
		cmnd->backgroundProcess = true;
		cmnd->args[cmnd->numArgs-1] = NULL;
	}
	//make sure the command array is NULL terminated.
	else {
		cmnd->args[cmnd->numArgs] = NULL;
	}
}

//Route commands to either builtin function or fork and execute.
void routeCommand(struct Command* cmnd, int* exitStatus) {
	processActive = true;
	//Execute smallsh builtins.
	if(strcmp(cmnd->args[0],"cd")==0) {
		//cd function will check for a valid directory and chdir to HOME if no argument.
		cd(cmnd->args[1]);
	}
	else if(strcmp(cmnd->args[0],"status")==0) {
		//print the most recent exit status.
		reportStatus(*exitStatus);
	}
	else if(strcmp(cmnd->args[0],"exit")==0) {
		burnEverything();
		free(cmnd);
		cmnd = NULL;
		exit(0);
	}

	//Else route non-builtins to foreground or background mode exec.
	else {
		pid_t childPid = fork();
		switch(childPid) {
			//fork error.
			case(-1): {
						perror("Hull Breach!\n");
						exit(1);
					}
			//in the child process.
			case(0): {

						signal(SIGTSTP,SIG_IGN);
						//if command run with & flag and not in foreground only mode:
						if(cmnd->backgroundProcess && allowBackground) {
							//if the command doesn't already have a file redirect,
							//set the redirect flag to true, which the redirect handler
							//will point to /dev/null.
							if(!cmnd->inputRedirect) {
								cmnd->inputRedirect = true;
							}
							if(!cmnd->outputRedirect) {
								cmnd->outputRedirect = true;
							}
						}
						//else the process is foreground, so enable SIGINT.
						else {
							sigintDefault();
						}
						inputoutputRedirect(cmnd);
						//execute.
						execvp(cmnd->args[0],cmnd->args);
						fprintf(stderr, "%s: no such file or directory.\n", cmnd->args[0]);
						fflush(stderr);
						exit(EXIT_FAILURE);
				    }
			//in the parent process.
			default: {
					    //if child is a background process:
					    if(cmnd->backgroundProcess && allowBackground) {
							printf("background pid is %d\n",childPid);
							fflush(stdout);
					    }
					    //else child is in foreground.
					    else {
							waitpid(childPid,exitStatus,0);
							if(WIFSIGNALED(*exitStatus)) {
								printf("terminated by signal %d\n",WTERMSIG(*exitStatus));
								fflush(stdout);
							}
							if(sigtstpTriggered) {
								if(allowBackground) {
									printf("\nExiting foreground-only mode.\n");
									fflush(stdout);
								}
								else {
									printf("\nEntering foreground-only mode (& will be ignored).\n");
									fflush(stdout);
								}
								sigtstpTriggered = false;
							}

					    }
				    }
			//I don't like this gap.
		}
	}
	processActive = false;
}

void shell(void) {
	//handle signals.
	sigintIgnore();
	sigtstpSet();
	int exitStatus = 0;

	//start loop.
	while(1) {
		//allocate a Command struct.
		struct Command* cmnd;
		//calloc is very useful here due to setting member variables to 0, including bools.
		cmnd = calloc(1, sizeof(struct Command));
		
		//scoop up a completed zombie process.
		burnZombie();

		//get user input.
		getCommand(cmnd);
		
		//parse command.
		parseCommand(cmnd);

		//route and execute command.
		routeCommand(cmnd,&exitStatus);

		//free Command struct.
		free(cmnd);
		cmnd = NULL;
	}
}

int main() 
{
	shell();
	return 0;
}
