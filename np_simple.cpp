#include <iostream>
#include <vector>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
using namespace std;

#define MAXCMDLENG 256
#define MAXUSERS 30
#define MAXINLENG 15000
#define ERRPIPE 0
#define NORMAL 1
#define NUMPIPE 2
#define OUTFILE 3

#define WRITE 1
#define READ 0

typedef struct token_list{
    string tok[1000];
    int length;
}token_list;

typedef struct npipe_info{
    int remain;
    int fd[2];
}npipe_info;

vector <token_list> cmds;
vector <pid_t> pid_list;
vector <npipe_info> npipe_list;

int p1_fd[2], p2_fd[2], mode;

void update_plist(){
    vector <npipe_info>::iterator it = npipe_list.begin();
    for(;it!=npipe_list.end(); it++)
        it->remain--;
}

int search_plist(int n){
    for(int i=0;i < npipe_list.size(); i++)
        if(npipe_list[i].remain == n)
            return i;
    return -1;
}
void insert_plist(int n, int *f){
    npipe_info tmp = {n, f[0], f[1]};
    npipe_list.push_back(tmp);
}

void split(string s, char delim, token_list *l){
    size_t pos = 0;
    int n = 0;
    
    while((pos = s.find(delim)) != string::npos){
        if(pos!=0){
            l->tok[n] = s.substr(0, pos);
            n++;
        }
        s.erase(0, pos+1);
    }
    if(s.length()!=0){
        l->tok[n] = s;
        n++;
    }
    l->length = n;
}

inline bool is_numpipe(string cmd, size_t *pos){
    //pos : index of last '|' 
    for (int i=0; i<cmd.length(); i++)
        if(!isdigit(cmd[i]) && cmd[i] != ' ' && cmd[i] != '+')
            return false;
    return true;
}
inline bool is_outredir(string cmd, size_t &pos){
    //pos : index of '>' being search for
    if((pos = cmd.find('>'))!= string::npos)
        if(cmd[pos+1] == ' ')
            return true;
    return false;
}
inline bool is_errpipe(string cmd, size_t *pos){
    //pos : index of last '!' being search for
    if((*pos = cmd.find('!')) != string::npos){
        for (int i=*pos+1; i<cmd.length(); i++)
            if(!isdigit(cmd[i]) && cmd[i] != ' ')
                return false;
        return true;
    }
    return false;
}

int parse_cmd(string input){      
    size_t pos;
    token_list unsplit_cmds;
    int n, len;
    bool npipe = true, out_redir = false, errpipe = false;

    split(input, '|', &unsplit_cmds);
    n = unsplit_cmds.length;
    if(( out_redir = is_outredir(unsplit_cmds.tok[n-1], pos) ))
        unsplit_cmds.tok[n-1].erase(pos, 1);
    if(( errpipe = is_errpipe(unsplit_cmds.tok[n-1], &pos) ))
        unsplit_cmds.tok[n-1].erase(pos, 1);
    if(( npipe = is_numpipe(unsplit_cmds.tok[n-1], &pos) ))
        n -= 1;
    token_list cmd;
    
    for(int i=0; i<n; i++){
        split(unsplit_cmds.tok[i], ' ', &cmd);
        cmds.push_back(cmd);
    }
    if(npipe){
        cmds.back().tok[cmds.back().length] = unsplit_cmds.tok[n];
        return NUMPIPE;
    }
    else if(errpipe){
        cmds.back().length--;
        return ERRPIPE;
    }
    else if(out_redir){
        cmds.back().length--;
        return OUTFILE;
    }
    else return NORMAL;
}

const char **tkltocstr(token_list c){
    const char **str;
    int i = 0;
    str = new const char*[c.length+1]{NULL};
    for(;i<c.length;i++)
        str[i] = c.tok[i].c_str();
    return str;
}

inline void redirect(int newfd, int oldfd){
    if(oldfd != newfd){
        dup2(newfd, oldfd);
        close(newfd);   
    }
}

void run(token_list cmd, int fd_in, int out, int err){
    //signal(SIGPIPE, SIG_IGN);
    redirect(fd_in, STDIN_FILENO);
    if(out != err){
        redirect(out, STDOUT_FILENO);
        redirect(err, STDERR_FILENO);
    }
    else{
        dup2(out, STDOUT_FILENO);
        dup2(out, STDERR_FILENO);
        close(out);
    }
    const char **argv = tkltocstr(cmd);
    execvp(argv[0], (char**)argv);
    if(errno == ENOENT){
        cerr << "Unknown command: [" << cmd.tok[0].c_str() << "].\n";
    }
    exit(0);
}

int numpipe_parse(){
    int sum = 0;
    size_t pos1 = -1;
    size_t pos2;
    string exp = cmds.back().tok[cmds.back().length];
    while((pos2 = exp.find('+', pos1+1)) != string::npos){
        sum += stoi(exp.substr(pos1+1, pos2 - pos1 - 1));
        pos1 = pos2;
    }
    sum += stoi(exp.substr(pos2+1, string::npos));
    return sum;
}

void last_cmdcntl(int fd_in = STDIN_FILENO){
    int n, idx, out = STDOUT_FILENO, err = STDERR_FILENO, fd[2];
    int *last = fd;
    pid_t cur_pid;

    if(mode == ERRPIPE || mode == NUMPIPE){
        n = numpipe_parse();
        idx = search_plist(n);
        if(idx != -1)
            last = npipe_list.at(idx).fd;
        else{
            pipe(last);
            insert_plist(n, last);
        }
        out = last[WRITE];
        if(mode == ERRPIPE) err = out;
    }
    while((cur_pid = fork()) < 0){
        waitpid(-1, NULL, 0);
    }
    if(cur_pid == 0){
        if(mode == OUTFILE){
            const char *fd_name = cmds.back().tok[cmds.back().length].c_str();
            out = open(fd_name, O_WRONLY | O_CREAT, 0666);
            ftruncate(out, 0); 
            lseek(out, 0, SEEK_SET); 
        }
        else if(mode == ERRPIPE || mode == NUMPIPE) 
            close(last[READ]);       
        run(cmds.back(), fd_in, out, err);
    }
    else{
        if(mode != NUMPIPE && mode != ERRPIPE) pid_list.push_back(cur_pid);
        if(fd_in != STDIN_FILENO) close(fd_in);

        vector <pid_t> :: iterator it = pid_list.begin();
        for(; it!=pid_list.end(); it++){
            int STATUS;
            waitpid(*it, &STATUS, 0);
        }
    }
}

void pipe_control(int fd_in = STDIN_FILENO){
    pid_t pid1, pid2;
    int *front_pipe = p1_fd, *end_pipe = p2_fd;
    pipe(front_pipe);
    
    if((pid1 = fork()) == 0){
        close(front_pipe[READ]);
        run(cmds.front(), fd_in, front_pipe[WRITE], STDERR_FILENO);
    }
    //parent wait all previous round hang childs
    else{
        pid_list.push_back(pid1);
        close(front_pipe[WRITE]);
        if(fd_in != STDIN_FILENO) close(fd_in);
    }
    for(int i = 1;i < cmds.size()-1; i++){
        //Pid1 --pipe1--> Pid2 --pipe2-->
        pipe(end_pipe);
        while((pid2 = fork()) < 0){
            waitpid(-1, NULL, 0);
        }
        if(pid2 == 0){
            close(end_pipe[READ]);
            run(cmds.at(i), front_pipe[READ], end_pipe[WRITE], STDERR_FILENO);
        }
        else{
            pid_list.push_back(pid2);
            close(front_pipe[READ]);
            close(end_pipe[WRITE]);
            swap(front_pipe, end_pipe);
            swap(pid1, pid2);
        }
    }
    last_cmdcntl(front_pipe[READ]);
}

void init(){
    cmds.clear();
    update_plist();
    pid_list.clear();
    p1_fd[WRITE] = 0;
    p1_fd[READ] = 0;
    p2_fd[WRITE] = 0;
    p2_fd[READ] = 0;
}

int passivesock(int p){
    struct sockaddr_in info;
    int sockfd = 0;
    bzero(&info, sizeof(info));

    info.sin_family = PF_INET;
    info.sin_addr.s_addr = INADDR_ANY;
    info.sin_port = htons((u_short)p);
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if(sockfd == -1) std::cerr << "create socket fail";
    int enable = 1;
    if(setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(int)) < 0 )
        cerr << strerror(errno);
    bind(sockfd, (struct sockaddr *)&info, sizeof(info));
    listen(sockfd, 30);
    return sockfd;
}
void reaper(int a){
    pid_t pid;
    while((pid = waitpid(-1, NULL, WNOHANG)) > 0);
}

bool handle_builtin(token_list input){
    int i=0;
    const string builtin_list[3] = {"setenv", "printenv", "exit"};
    for(;i<3 && input.tok[0] != builtin_list[i]; i++);
    switch(i){
    case 0:
        setenv(input.tok[1].c_str(), input.tok[2].c_str(), 1);
        break;
    case 1:{
        printf("%s\n",getenv(input.tok[1].c_str()));
        break;
    }
    case 2:{
        exit(0);
        break;
    }
    default:
        return false;
        break;
    }
    return true;    
}

void shell(){
    int IN = STDIN_FILENO, OUT = STDOUT_FILENO;
    int npipe_idx, num_data;
    string input_str;

    while(true){
        char buf[MAXCMDLENG];
        cout << "% " << flush;
        if((num_data = read(STDIN_FILENO, &buf, MAXCMDLENG)) < 0){
            fprintf(stderr, "read error: %s\n", strerror(errno));
            if(errno == EINTR);
            else exit(0);
        }
        for(int i = 0; i<num_data ; i++){
            if(buf[i] == '\n')
                break;
            else if(buf[i] != '\r')
                input_str += buf[i];
        }
        if(!input_str.empty()){
            mode = parse_cmd(input_str);

            if((npipe_idx = search_plist(0)) != -1){
                IN = npipe_list.at(npipe_idx).fd[READ];
                close(npipe_list.at(npipe_idx).fd[WRITE]);
                npipe_list.erase(npipe_list.begin()+npipe_idx);
            }

            if(!handle_builtin(cmds.front())){
                if(cmds.size() == 1)
                    last_cmdcntl(IN);
                else{
                    pipe_control(IN);
                }
            }
        }
        init();
        input_str.clear();
    }
}

int main(int argc, char *argv[]){
    setenv("PATH", "bin:.", 1);
    int port = atoi(argv[1]);
    int msock = passivesock(port);
    int ssock;
    
    struct sockaddr_in client_info;
    socklen_t addrlen;

    while(true){
        int pid = 0;
        addrlen = sizeof(client_info);
        ssock = accept(msock, (struct sockaddr *)&client_info, &addrlen);
        if((pid = fork()) == 0){
            signal(SIGCHLD, reaper);
            //redirect child to remote
            close(msock);
            dup2(ssock, STDIN_FILENO);  
            dup2(ssock, STDOUT_FILENO);
            dup2(ssock, STDERR_FILENO);
            close(ssock);
            shell();
        }
        else{
            close(ssock);
            waitpid(pid, NULL, 0);
        }
    }
}