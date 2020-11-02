#include <string.h>
#include <signal.h>
#include <string>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <iostream>

int passivesock(int p){
    struct sockaddr_in info;
    int sockfd = 0;
    bzero(&info, sizeof(info));

    info.sin_family = PF_INET;
    info.sin_addr.s_addr = INADDR_ANY;
    info.sin_port = htons((u_short)p);
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if(sockfd == -1) std::cerr << "create socket fail";

    bind(sockfd, (struct sockaddr *)&info, sizeof(info));
    listen(sockfd, 30);
    return sockfd;
}
void reaper(int a){
    while(waitpid(-1, NULL, WNOHANG) > 0){
        std::cerr << "reap success\n";
    }
}

int main(int argc, char *argv[]){
    setenv("PATH", "bin:.", 1);
    int port = atoi(argv[1]);
    int msock = passivesock(port);
    int ssock;
    
    struct sockaddr_in client_info;
    socklen_t addrlen;
    signal(SIGCHLD, reaper);

    while(true){
        int pid = 0;
        addrlen = sizeof(client_info);
        ssock = accept(msock, (struct sockaddr *)&client_info, &addrlen);
        if((pid = fork()) == 0){
            //redirect child to remote
            close(msock);
            dup2(ssock, STDIN_FILENO);  
            dup2(ssock, STDOUT_FILENO);
            dup2(ssock, STDERR_FILENO);
            close(ssock);
            execlp("npshell", "npshell", NULL);
            std::cerr << "fail to create slave process"<<std::endl;
            exit(0);
        }
        else{
            close(ssock);
        }
    }
}