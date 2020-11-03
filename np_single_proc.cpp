#include <iostream>
#include <vector>
#include <map>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/stat.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
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
    string tok[5000];
    int length;
}token_list;

typedef struct npipe_info{
    int remain;
    int fd[2];
    int ch_count;
}npipe_info;

struct cli_info{
    string name;
    string ip;
    in_port_t port;
    vector <npipe_info> npipe_list;
    map <string, string> env_var;
};

//vector <npipe_info> p_list;
map <int, int> socket_map;
map <int, int> uid_map;
map <int, cli_info> clinfo_map;
bool user[MAXUSERS] = {false};
//vector <string> cmds;

token_list *cmds;
int p1_fd[2], p2_fd[2], mode, count;

void update_plist(int uid){
    vector <npipe_info>::iterator it = clinfo_map[uid].npipe_list.begin();
    for(;it!=clinfo_map[uid].npipe_list.end(); it++)
        it->remain--;
}

int search_plist(int n, int uid){
    for(int i=0;i < clinfo_map[uid].npipe_list.size(); i++)
        if(clinfo_map[uid].npipe_list[i].remain == n)
            return i;
    return -1;
}
void insert_plist(int n, int *f, int c, int uid){
    npipe_info tmp = {n, f[0], f[1], c};
    clinfo_map[uid].npipe_list.push_back(tmp);
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
inline bool is_outredir(string cmd, size_t *pos){
    //pos : index of '>' being search for
    if((*pos = cmd.find('>'))!= string::npos)
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
    int n;
    bool npipe = true, out_redir = false, errpipe = false;

    split(input, '|', &unsplit_cmds);
    n = unsplit_cmds.length;
    if(( out_redir = is_outredir(unsplit_cmds.tok[n-1], &pos) ))
        unsplit_cmds.tok[n-1].erase(pos, 1);
    if(( errpipe = is_errpipe(unsplit_cmds.tok[n-1], &pos) ))
        unsplit_cmds.tok[n-1].erase(pos, 1);
    if(( npipe = is_numpipe(unsplit_cmds.tok[n-1], &pos) ))
        n -= 1;
    count = n;
    cmds = new token_list[n];
    if(n > 1)
        for(int i=0; i<n; i++)
            split(unsplit_cmds.tok[i], ' ', &cmds[i]);
    else
        split(unsplit_cmds.tok[0], ' ', &cmds[0]);

    if(npipe){
        cmds[n-1].tok[cmds[n-1].length] = unsplit_cmds.tok[n];
        return NUMPIPE;
    }
    else if(errpipe){
        cmds[count-1].length--;
        return ERRPIPE;
    }
    else if(out_redir){
        cmds[count-1].length--;
        return OUTFILE;
    }
    else return NORMAL;
}

const char **tkltocstr(token_list c){
    const char **argv;
    int i = 0;
    argv = new const char*[c.length+1]{NULL};
    for(;i<c.length;i++)
        argv[i] = c.tok[i].c_str();
    return argv;
}

inline void redirect(int newfd, int oldfd){
    if(oldfd != newfd){
        dup2(newfd, oldfd);
        close(newfd);   
    }
}

void run(token_list cmd, int fd_in, int out, int err){
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
    string exp = cmds[count-1].tok[cmds[count-1].length];
    while((pos2 = exp.find('+', pos1+1)) != string::npos){
        sum += stoi(exp.substr(pos1+1, pos2 - pos1 - 1));
        pos1 = pos2;
    }
    sum += stoi(exp.substr(pos2+1, string::npos));
    return sum;
}

void last_cmdcntl(int uid, bool fst, pid_t lpid, int fd_in = STDIN_FILENO){
    int n, index, inpipe_WRITE, out = socket_map[uid], err = socket_map[uid], fd[2];
    int *last = fd, wait_count;
    pid_t cur_pid;
    bool PIPEIN = false, ign = false, merge = false;

    if(fst)
        if((index = search_plist(0, uid)) != -1){
            wait_count = clinfo_map[uid].npipe_list.at(index).ch_count;
            fd_in = clinfo_map[uid].npipe_list.at(index).fd[READ];
            close(clinfo_map[uid].npipe_list.at(index).fd[WRITE]);
            clinfo_map[uid].npipe_list.erase(clinfo_map[uid].npipe_list.begin()+index);
            PIPEIN = true;
        }
    if(mode == ERRPIPE || mode == NUMPIPE){
        n = numpipe_parse();
        //n = stoi(cmds[count-1].tok[cmds[count-1].length]);
        index = search_plist(n, uid);
        if(index != -1){
            last = clinfo_map[uid].npipe_list.at(index).fd;
            clinfo_map[uid].npipe_list.at(index).ch_count++;
        }
        else{
            pipe(last);
            insert_plist(n, last, 1, uid);
        }
    }

    if((cur_pid = fork()) == 0){
        if(mode == OUTFILE){
            const char * fd_name = cmds[count-1].tok[cmds[count-1].length].c_str();
            out = open(fd_name, O_WRONLY | O_CREAT, 0666);
            ftruncate(out, 0); 
            lseek(out, 0, SEEK_SET); 
        }
        else if(mode == ERRPIPE || mode == NUMPIPE) {
            //if(PIPEIN) close(inpipe_WRITE);
            close(last[READ]);
            out = last[WRITE];
            if(mode == ERRPIPE) err = out;
        }
        run(cmds[count-1], fd_in, out, err);
    }
    else{
        int p;
        if(PIPEIN) close(fd_in);
        //if(mode == ERRPIPE || mode == NUMPIPE) close(last[WRITE]);  
        if(!fst) waitpid(lpid, NULL, 0);
        else if(PIPEIN){
            //close(inpipe_WRITE);
            for (int i = 0; i < wait_count;){
                
                if((p = wait(NULL)) != cur_pid) i++;
                else ign =true;
            }
        }
        if(mode != ERRPIPE && mode != NUMPIPE && !ign) waitpid(cur_pid, NULL, 0);
    }
}

void pipe_control(int uid){
    pid_t pid1, pid2;
    int *front_pipe = p1_fd, *end_pipe = p2_fd;
    int fd_in = STDIN_FILENO, inpipe_WRITE, index, i, n, wait_count;
    bool PIPEIN = false, ign = false;
    pipe(front_pipe);
    
    if((index = search_plist(0, uid)) != -1){
        fd_in = clinfo_map[uid].npipe_list.at(index).fd[READ];
        wait_count = clinfo_map[uid].npipe_list.at(index).ch_count;
        close(clinfo_map[uid].npipe_list.at(index).fd[WRITE]);
        clinfo_map[uid].npipe_list.erase(clinfo_map[uid].npipe_list.begin()+index);
        PIPEIN = true;
    }
    if((pid1 = fork()) == 0){
        close(front_pipe[READ]);
        run(cmds[0], fd_in, front_pipe[WRITE], socket_map[uid]);
    }
    //parent wait all previous round hang childs
    else{
        close(front_pipe[WRITE]);
        if(PIPEIN){
            close(fd_in);
            //close(inpipe_WRITE);
            for (int i = 0; i < wait_count;){
                int p;
                if((p = wait(NULL)) != pid1 )i++;
                else ign = true;
            }
        }
    }

    for(int i = 1;i < count-1; i++){
        //Pid1 --pipe1--> Pid2 --pipe2-->
        pipe(end_pipe);
        if((pid2 = fork()) == 0){
            close(end_pipe[READ]);
            run(cmds[i], front_pipe[READ], end_pipe[WRITE], socket_map[uid]);
        }
        else{
            close(front_pipe[READ]);
            close(end_pipe[WRITE]);
            if(!ign) waitpid(pid1, NULL, 0);
            swap(front_pipe, end_pipe);
            swap(pid1, pid2);
            pipe(end_pipe);
        }
    }
    //if need to pipe stdout or stderr to next "n" line
    last_cmdcntl(uid, false, pid1, fd_in = front_pipe[READ]);
}

void init(int uid){
    delete [] cmds;
    update_plist(uid);
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

    bind(sockfd, (struct sockaddr *)&info, sizeof(info));
    listen(sockfd, 30);
    return sockfd;
}
void reaper(int a){
    while(waitpid(-1, NULL, WNOHANG) > 0){
        std::cerr << "reap success\n";
    }
}
inline int get_uid(){
    for(int i = 0; i<MAXUSERS; i++)
        if(!user[i]){
            user[i] = true;
            return i+1;
        }
    return -1;
}
inline void broadcast(string m){
    for(int i=0; i<MAXUSERS; i++){
        if(user[i]){
            send(socket_map[i+1], m.c_str(), m.length(), 0);
        }
    }
}
bool handle_builtin(token_list input, int uid){
    int i=0;
    const string builtin_list[7] = {"setenv", "printenv", "exit", 
                              "who", "tell", "yell", "name"};
    for(;i<7 && input.tok[0] != builtin_list[i]; i++);
    switch(i){
    case 0:
        //setenv(input.tok[1].c_str(), input.tok[2].c_str(), 1);
        clinfo_map[uid].env_var[input.tok[1]] = input.tok[2];
        break;
    case 1:{
        string var = clinfo_map[uid].env_var[input.tok[1]];
        send(socket_map[uid], var.c_str(), var.length(), 0);
        //printf("%s\n",getenv(input.tok[1].c_str()));
        break;
    }
    case 2:{
        string message = "*** User '' left. ***\n";
        message.insert(10, clinfo_map[uid].name);
        //may need clear clinfo_map[uid]
        user[uid-1] = false;
        close(socket_map[uid]);
        //TODO : close user pipe to uid
        broadcast(message);
        break;
    }
    case 3:{
        string message = "<ID>\t<nickname>\t<IP:port>\t<indicate me>\n";
        if(send(socket_map[uid], message.c_str(), message.length(), 0) == -1)
            cerr << strerror(errno) << endl;
        message.clear();
        for(int i=0; i<MAXUSERS; i++){
            if(user[i]){
                char str[50];
                sprintf(str, "%d\t%s\t%s:%d", i+1, clinfo_map[i+1].name.c_str(), clinfo_map[i+1].ip.c_str(), clinfo_map[i+1].port);
                message = str;
                if(i+1 == uid) message += "\t<-me";
                message += "\n";
                send(socket_map[uid], message.c_str(), message.length(), 0);
            }
        }
        break;
    }
    case 4:{
        int dest = stoi(input.tok[1]);
        char str[50];
        if(!user[dest-1])
            sprintf(str, "*** Error: user #%s does not exist yet. ***\n", input.tok[1].c_str());
        else
            sprintf(str, "*** %s told you ***: %s\n", clinfo_map[uid].name.c_str(), input.tok[2].c_str());
        send(socket_map[dest], str, strlen(str), 0);
        break;
    }
    case 5:{
        string message = "***  yelled ***: ";
        message.insert(4, clinfo_map[uid].name);
        message += input.tok[1] + "\n";
        broadcast(message);
        break;
    }
    case 6:{
        string name = input.tok[1];
        string message;
        char str[80];
        bool exist = false;
        for(int i=0; i<MAXUSERS;i++)
            if(user[i])
                if(name == clinfo_map[i+1].name) exist = true;
        if(exist){
            sprintf(str, "*** User ’%s’ already exists. ***\n", name.c_str());
            send(socket_map[uid], str, strlen(str), 0);
        }
        else{
            clinfo_map[uid].name = name;
            sprintf(str, "*** User from %s:%d is named ’%s’. ***\n", clinfo_map[uid].ip.c_str(), clinfo_map[uid].port, name.c_str());
            message = str;
            broadcast(message);
        }
        break;
    }
    default:
        return false;
        break;
    }
    return true;    
}
void shell(string input_str, int uid){
    //int i, IN = STDIN_FILENO, OUT = STDOUT_FILENO;
    map<string, string>::iterator it;
    for(it = clinfo_map[uid].env_var.begin(); it!= clinfo_map[uid].env_var.end(); it++){}
        setenv(it->first.c_str(), it->second.c_str(), 1);
    mode = parse_cmd(input_str);
    if(!handle_builtin(cmds[0], uid)){
        if(count == 1)
            last_cmdcntl(uid, true, -1);
        else{
            pipe_control(uid);
        }
    }
    init(uid);
    send(socket_map[uid], "% ", 2, 0);
}
inline void update_fdset(fd_set &fds){
    FD_ZERO(&fds);
    for(int i=0; i<MAXUSERS; i++){
        if(user[i]){
            FD_SET(socket_map[i+1], &fds);
        }
    }
}

int main(int argc, char* const argv[]){
    if(argc < 1){
        cerr << ("usage : np_single_proc [port]");
        exit(0);
    }
    fd_set rfds, afds;
    int uid, nfds = getdtablesize();
    int port = atoi(argv[1]);
    int msock = passivesock(port), ssock;
    struct sockaddr_in client_info;
    socklen_t addrlen;
    string input_str;
    const char *welcom = "***************************************\n** Welcome to the information server **\n***************************************\n";
    char str[100];

    signal(SIGCHLD, reaper);
    FD_ZERO(&afds);
    FD_SET(msock, &afds);
    while(true){
        update_fdset(afds);
        FD_SET(msock, &afds);
        rfds = afds;
        if(select(nfds, &rfds, NULL, NULL, 0) < 0){
            fprintf(stderr, "select error : %s", strerror(errno));
            continue;
        }
        if(FD_ISSET(msock, &rfds)){
            addrlen = sizeof(client_info);
            ssock = accept(msock, (sockaddr*)&client_info, &addrlen);
            if((uid = get_uid()) == -1){
                cerr << "too much user";
                continue;       //TODO
            }

            FD_SET(ssock, &afds);
            clinfo_map[uid] = {
                .name = "(no name)",
                .ip = inet_ntoa(client_info.sin_addr),
                .port = htons(client_info.sin_port)
            };
            clinfo_map[uid].env_var["PATH"] = "bin:.";
            socket_map[uid] = ssock;
            uid_map[ssock] = uid;
            send(ssock, welcom, strlen(welcom), 0);
            sprintf(str, "*** User ’%s’ entered from %s:%d. ***\n", clinfo_map[uid].name.c_str(), 
                                                                  clinfo_map[uid].ip.c_str(), clinfo_map[uid].port);
            string mes = str;
            broadcast(mes);
            send(ssock, "% ", 2, 0);
        }
        for(int fd = 0; fd < nfds; fd++){
            if(FD_ISSET(fd, &rfds) && fd != msock){
                char buf[MAXCMDLENG];
                int num_data = recv(fd, &buf, MAXCMDLENG, 0);
                uid = uid_map[fd];
                for(int i = 0; i<num_data ; i++){
                    if(buf[i] == '\n'){
                        if(input_str.empty())
                            cout << "get empty string";
                        else{
                            printf("get cmd \"%s\" from uid %d\n", input_str.c_str(), uid);
                            shell(input_str, uid);
                        }
                    }
                    else if(buf[i] != '\r')
                        input_str += buf[i];
                }
                input_str.clear();
            }
        }
    }
}
