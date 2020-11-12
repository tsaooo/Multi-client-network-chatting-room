#include <iostream>
#include <vector>
#include <map>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/stat.h>
#include <arpa/inet.h>
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
#define USERPIPE 4
#define LOST 5

#define WRITE 1
#define READ 0

typedef struct token_list{
    string tok[2000];
    int length;
}token_list;

typedef struct npipe_info{
    int remain;
    int fd[2];
}npipe_info;

struct cli_info{
    string name;
    string ip;
    in_port_t port;
    int g_num;
    vector <npipe_info> npipe_list;
    map <int, int[2]> upipe_map;
    map <string, string> env_var;
};

map <string, vector<int> > group_map;
map <int, int> socket_map;
map <int, int> uid_map;
map <int, cli_info> clinfo_map;
bool user[MAXUSERS] = {false};
vector <token_list> cmds;
vector <pid_t> pid_list;

int p1_fd[2], p2_fd[2], mode;

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
void insert_plist(int n, int *f, int uid){
    npipe_info tmp = {n, f[0], f[1]};
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
inline bool is_userpipeOUT(string cmd, size_t &pos){
    if((pos = cmd.find('>')) != string::npos){
        for(int i = pos+1; i<cmd.length(); i++)
            if(!isdigit(cmd[i]) && cmd[i] != ' ')
                return false;
        return true;
    }
    return false;
}

inline bool is_userpipeIN(string cmd, size_t &pos, int &len){
    if((pos = cmd.find('<')) != string::npos){
        len = 0;
        for(int i = pos+1; i<cmd.length(); i++, len++)
            if(!isdigit(cmd[i])){
                if(cmd[i] == ' ')
                    return true;
                return false;
            }
        return true;
    }
    return false;
}

int parse_cmd(string input, int &upipe_number){      
    size_t pos;
    token_list unsplit_cmds, cmd;
    int n, len;
    bool npipe = true, out_redir = false, errpipe = false, upipeout = false, upipe_in = false;
    
    if((pos = input.find("tell")) != string::npos){
        cmd.tok[0] = "tell";
        pos = input.find(' ', 5);
        cmd.tok[1] = input.substr(5,pos - 5);
        cmd.tok[2] = input.substr(pos+1);
        cmd.length = 3;
        cmds.push_back(cmd);
        return NORMAL;
    }
    if(input.find("yell") != string::npos){
        cmd.tok[0] = "yell";
        cmd.tok[1] = input.substr(5);
        cmd.length = 2;
        cmds.push_back(cmd);
        return NORMAL;
    }

    split(input, '|', &unsplit_cmds);
    n = unsplit_cmds.length;
    if(is_userpipeIN(unsplit_cmds.tok[0], pos, len)){
        upipe_number = stoi(unsplit_cmds.tok[0].substr(pos + 1 , len));
        unsplit_cmds.tok[0].erase(pos, len + 1);        //erase '<[number]'
    }
    else upipe_number = -1;
    if(( upipeout = is_userpipeOUT(unsplit_cmds.tok[n-1], pos) )){
        unsplit_cmds.tok[n-1].erase(pos, 1);
    }
    if(( out_redir = is_outredir(unsplit_cmds.tok[n-1], pos) ))
        unsplit_cmds.tok[n-1].erase(pos, 1);
    if(( errpipe = is_errpipe(unsplit_cmds.tok[n-1], &pos) ))
        unsplit_cmds.tok[n-1].erase(pos, 1);
    if(( npipe = is_numpipe(unsplit_cmds.tok[n-1], &pos) ))
        n -= 1;
    
    for(int i=0; i<n; i++){
        token_list command;
        split(unsplit_cmds.tok[i], ' ', &command);
        cmds.push_back(command);
    }
    if(upipe_in) cmds.front().length--;
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
    else if(upipeout){
        cmds.back().length--;
        return USERPIPE;
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
    size_t pos2 = -1;
    string exp = cmds.back().tok[cmds.back().length];
    while((pos2 = exp.find('+', pos1+1)) != string::npos){
        sum += stoi(exp.substr(pos1+1, pos2 - pos1 - 1));
        pos1 = pos2;
    }
    sum += stoi(exp.substr(pos2+1, string::npos));
    return sum;
}

void last_cmdcntl(int uid, int fd_in = STDIN_FILENO){
    int n, idx, out = socket_map[uid], err = socket_map[uid], fd[2];
    int *last = fd;
    pid_t cur_pid;

    if(mode == ERRPIPE || mode == NUMPIPE){
        n = numpipe_parse();
        idx = search_plist(n, uid);
        if(idx != -1)
            last = clinfo_map[uid].npipe_list.at(idx).fd;
        else{
            pipe(last);
            insert_plist(n, last, uid);
        }
        out = last[WRITE];
        if(mode == ERRPIPE) err = out;
    }
    if(mode == USERPIPE){
        int dest = stoi(cmds.back().tok[cmds.back().length]);
        pipe(last);
        out = last[WRITE];
        clinfo_map[uid].upipe_map[dest][READ] = last[READ];
        clinfo_map[uid].upipe_map[dest][WRITE] = last[WRITE];
    }
    while((cur_pid = fork()) < 0){
        waitpid(-1, NULL, 0);
        cout << "too much process, wait success\n";
    }
    if(cur_pid == 0){
        if(mode == OUTFILE){
            const char *fd_name = cmds.back().tok[cmds.back().length].c_str();
            out = open(fd_name, O_WRONLY | O_CREAT, 0666);
            ftruncate(out, 0); 
            lseek(out, 0, SEEK_SET); 
        }
        else if(mode == ERRPIPE || mode == NUMPIPE || mode == USERPIPE) 
            close(last[READ]);       
        else if(mode == LOST)
            out = open("/dev/null", O_WRONLY);
        run(cmds.back(), fd_in, out, err);
    }
    else{
        if(mode != NUMPIPE && mode != ERRPIPE && mode != USERPIPE) pid_list.push_back(cur_pid);
        int p;
        if(fd_in != STDIN_FILENO) close(fd_in);
        if(mode == USERPIPE) close(out);

        vector <pid_t> :: iterator it = pid_list.begin();
        for(; it!=pid_list.end(); it++){
            int STATUS;
            waitpid(*it, &STATUS, 0);
        }
    }
}

void pipe_control(int uid, int fd_in = STDIN_FILENO){
    pid_t pid1, pid2;
    int *front_pipe = p1_fd, *end_pipe = p2_fd;
    pipe(front_pipe);
    
    if((pid1 = fork()) == 0){
        close(front_pipe[READ]);
        run(cmds.front(), fd_in, front_pipe[WRITE], socket_map[uid]);
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
            cout << "too much process, wait success\n";
        }
        if(pid2 == 0){
            close(end_pipe[READ]);
            run(cmds.at(i), front_pipe[READ], end_pipe[WRITE], socket_map[uid]);
        }
        else{
            pid_list.push_back(pid2);
            close(front_pipe[READ]);
            close(end_pipe[WRITE]);
            swap(front_pipe, end_pipe);
            swap(pid1, pid2);
        }
    }
    last_cmdcntl(uid, front_pipe[READ]);
}

void init(int uid){
    cmds.clear();
    update_plist(uid);
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
inline int get_uid(){
    for(int i = 0; i<MAXUSERS; i++)
        if(!user[i]){
            user[i] = true;
            return i+1;
        }
    return -1;
}
inline void broadcast(string m){
    for(int i=0; i<MAXUSERS; i++)
        if(user[i])
            send(socket_map[i+1], m.c_str(), m.length(), 0);
}

void clear_pipe(int uid){
    for(int i=0; i<MAXUSERS; i++)
        if(user[i]){
            if(clinfo_map[i+1].upipe_map[uid][READ] != 0)
                close(clinfo_map[i+1].upipe_map[uid][READ]);
            clinfo_map[i+1].upipe_map.erase(uid);
        }
    map <int, int[2]>:: iterator it;
    vector <npipe_info>:: iterator it2;
    for(it = clinfo_map[uid].upipe_map.begin(); it!=clinfo_map[uid].upipe_map.end(); it++)
        if(it->second[READ] != 0)
            close(it->second[READ]);
    for(it2 = clinfo_map[uid].npipe_list.begin(); it2!=clinfo_map[uid].npipe_list.end(); it2++){
        close(it2->fd[READ]);
        close(it2->fd[WRITE]);
    } 
    clinfo_map[uid].upipe_map.clear();
    clinfo_map[uid].npipe_list.clear();
}

bool handle_builtin(token_list input, int uid){
    int i=0;
    const string builtin_list[9] = {"setenv", "printenv", "exit", 
                              "who", "tell", "yell", "name", "group", "grouptell"};
    for(;i<9 && input.tok[0] != builtin_list[i]; i++);
    switch(i){
    case 0:
        clinfo_map[uid].env_var[input.tok[1]] = input.tok[2];
        break;
    case 1:{
        string var = clinfo_map[uid].env_var[input.tok[1]] + '\n';
        send(socket_map[uid], var.c_str(), var.length(), 0);
        break;
    }
    case 2:{
        string message = "*** User '' left. ***\n";
        message.insert(10, clinfo_map[uid].name);
        user[uid-1] = false;
        close(socket_map[uid]);
        uid_map.erase(socket_map[uid]);
        clear_pipe(uid);
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
        char str[1100];
        if(!user[dest-1]){
            sprintf(str, "*** Error: user #%d does not exist yet. ***\n", dest);
            send(socket_map[uid], str, strlen(str), 0);
        }
        else{
            sprintf(str, "*** %s told you ***: %s\n", clinfo_map[uid].name.c_str(), input.tok[2].c_str());
            send(socket_map[dest], str, strlen(str), 0);
        }
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
                if(name == clinfo_map[i+1].name){
                    exist = true;
                    break;
                }
        if(exist){
            sprintf(str, "*** User '%s' already exists. ***\n", name.c_str());
            send(socket_map[uid], str, strlen(str), 0);
        }
        else{
            clinfo_map[uid].name = name;
            sprintf(str, "*** User from %s:%d is named '%s'. ***\n", clinfo_map[uid].ip.c_str(), clinfo_map[uid].port, name.c_str());
            message = str;
            broadcast(message);
        }
        break;
    }
    case 7:{
        vector <int> guser;
        for(int i =2; i<input.length; i++){
            int n = stoi(input.tok[i]);
            guser.push_back(n);
        }
        group_map[input.tok[1]] = guser;
        return true;
    }
    case 8:{
        string gname = input.tok[1];
        vector <int>::iterator it = group_map[gname].begin();
        bool exist;
        for(; it!= group_map[gname].end(); it++)
            if(*it == uid) exist = true;
        if(!exist){
            char str[50];
            sprintf(str, "Error: you are not in %s", gname.c_str());
            send(socket_map[uid], str, strlen(str), 0);
        }
        vector <int>::iterator it2 = group_map[gname].begin();
        for(; it2!= group_map[gname].end(); it2++){
            if(user[*it])
                send(socket_map[*it], input.tok[2].c_str(), input.tok[2].length(), 0);
        }
        return true;
    }
    default:
        return false;
        break;
    }
    return true;    
}

void shell(string input_str, int uid){
    int IN = STDIN_FILENO, OUT = STDOUT_FILENO;
    int source = -1, npipe_idx;
    map<string, string>::iterator it;
    for(it = clinfo_map[uid].env_var.begin(); it!= clinfo_map[uid].env_var.end(); it++)
        setenv(it->first.c_str(), it->second.c_str(), 1);
    mode = parse_cmd(input_str, source);

    if(source != -1){
        char str[150];
        if(!user[source-1]){
            sprintf(str, "*** Error: user #%d does not exist yet. ***\n", source);
            send(socket_map[uid], str, strlen(str), 0);
            IN = open("/dev/null", O_RDONLY);
        }
        else if(clinfo_map[source].upipe_map[uid][READ] == 0){
            sprintf(str, "*** Error: the pipe #%d->#%d does not exist yet. ***\n", source, uid);
            send(socket_map[uid], str, strlen(str), 0);
            IN = open("/dev/null", O_RDONLY);
        }
        else{
            sprintf(str, "*** %s (#%d) just received from %s (#%d) by '%s' ***\n"
                    , clinfo_map[uid].name.c_str(), uid, clinfo_map[source].name.c_str(), source, input_str.c_str());
            string mes = str;
            broadcast(mes);
            IN = clinfo_map[source].upipe_map[uid][READ];
            clinfo_map[source].upipe_map.erase(uid);
        }
    }

    if((npipe_idx = search_plist(0, uid)) != -1){
        IN = clinfo_map[uid].npipe_list.at(npipe_idx).fd[READ];
        close(clinfo_map[uid].npipe_list.at(npipe_idx).fd[WRITE]);
        clinfo_map[uid].npipe_list.erase(clinfo_map[uid].npipe_list.begin()+npipe_idx);
    }

    if(mode == USERPIPE){
        char str[150];
        int dest = stoi(cmds.back().tok[cmds.back().length]);

        if(!user[dest-1]){
            sprintf(str, "*** Error: user #%d does not exist yet. ***\n", dest);
            send(socket_map[uid], str, strlen(str), 0);
            mode = LOST;
        }
        else if(clinfo_map[uid].upipe_map[dest][READ] != 0){
            sprintf(str, "*** Error: the pipe #%d->#%d already exists. ***\n", uid, dest);
            send(socket_map[uid], str, strlen(str), 0);
            mode = LOST;
        }
        else{
            sprintf(str, "*** %s (#%d) just piped '%s' to %s (#%d) ***\n"
                    , clinfo_map[uid].name.c_str(), uid, input_str.c_str(), clinfo_map[dest].name.c_str(), dest);
            string mes = str;
            broadcast(mes);
        }
    }
    if(!handle_builtin(cmds.front(), uid)){
        if(cmds.size() == 1)
            last_cmdcntl(uid, IN);
        else{
            pipe_control(uid, IN);
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
    const char *welcom = "****************************************\n** Welcome to the information server. **\n****************************************\n";
    char str[100];

    signal(SIGCHLD, reaper);
    FD_ZERO(&afds);
    FD_SET(msock, &afds);
    while(true){
        update_fdset(afds);
        FD_SET(msock, &afds);
        rfds = afds;
        if(select(nfds, &rfds, NULL, NULL, 0) < 0){
            //fprintf(stderr, "select error : %s\n", strerror(errno));
            if(errno == EINTR) continue;
            else exit(0);
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
            sprintf(str, "*** User '%s' entered from %s:%d. ***\n", clinfo_map[uid].name.c_str(), 
                                                                  clinfo_map[uid].ip.c_str(), clinfo_map[uid].port);
            string mes = str;
            broadcast(mes);
            send(ssock, "% ", 2, 0);
        }
        for(int fd = 0; fd < nfds; fd++){
            if(FD_ISSET(fd, &rfds) && fd != msock){
                char buf[MAXINLENG];
                int num_data;
                if((num_data = recv(fd, &buf, MAXINLENG, 0)) < 0){
                    //fprintf(stderr, "recv error: %s\n", strerror(errno));
                    if(errno == EINTR) continue;
                    else exit(0);
                }
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
