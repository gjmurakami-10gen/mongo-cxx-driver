/*    Copyright 2012 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#include "mongo/platform/basic.h"

#include "mongo/client/dbclientinterface.h"
#include "mongo/dbtests/mock/mock_replica_set.h"
#include "mongo/unittest/unittest.h"

#include <fcntl.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <set>
#include <string>

using mongo::BSONElement;
using mongo::BSONObj;
using mongo::BSONObjIterator;
using mongo::ConnectionString;
using mongo::MockRemoteDBServer;
using mongo::MockReplicaSet;

using std::string;
using std::vector;
using std::ostream;

using namespace std;

bool warn(const string& msg)
{
    cerr << msg << ": " << strerror(errno) << endl;
    return 0;
}

bool die(const string& msg)
{
    warn(msg);
    exit(1);
    return 0;
}

namespace mongo {
    const char *spawn_argvp[] = {
        "mongo",
        "--nodb",
        "--shell",
        "--listen",
        "30001",
        "src/mongo/dbtests/cluster_test.js",
        0
    };

    class Shell {
    public:
        static const uint16_t DefaultPort;
        static const int Retries;
        static const string Prompt;
        static const string Bye;

        int sock;
        uint16_t port;
        pid_t pid;

        Shell() {
            port = DefaultPort;
            connect("mongo_shell.log").read();
        }

        void spawn(const string& logFileName) {
            if ((pid = fork()) == 0) {
                int fdLog = open(logFileName.c_str(), O_WRONLY|O_CREAT|O_TRUNC, 0664);
                fdLog != -1 || die("Shell spawn - error on open of log file for mongo shell");
                dup2(fdLog, 1) != -1 || die("Shell spawn - error on dup of log file for stdout");
                dup2(fdLog, 2) != -1 || die("Shell spawn - error on dup of log file for stderr");
                int fdNull = open("/dev/null", O_RDONLY);
                fdNull != -1 || die("Shell spawn - error on open of /dev/null");
                dup2(fdNull, 0) != -1 || die("Shell spawn - error on dup of /dev/null for stdin");
                setsid() != -1 || die("Shell spawn - error on setsid");
                if ((pid = fork()) == 0) {
                    const char *envMongoShell = getenv("MONGO_SHELL");
                    string mongoShell( envMongoShell ? envMongoShell : "../mongo/mongo");
                    execv(mongoShell.c_str(), (char *const *)spawn_argvp);
                }
                exit(0);
            }
        }

        Shell& connect(const string& logFileName) {
            for (int i = 0; i < Retries; i++) {
                sock = socket(AF_INET, SOCK_STREAM, 0);
                sock != -1 || die("Shell connect - socket call error");
                struct sockaddr_in serv_addr;
                memset(&serv_addr, 0, sizeof(serv_addr));
                serv_addr.sin_family = AF_INET;
                inet_pton(AF_INET, "127.0.0.1", &serv_addr.sin_addr) > 0 || die("inet_pton error");
                serv_addr.sin_port = htons(port);
                if (::connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) == 0)
                    return *this;
                close(sock);
                spawn(logFileName);
                sleep(1);
            }
            die("Shell connect - error on connect to mongo shell");
            return *this;
        }

        string read(string prompt = Prompt) {
            string s;
            char buf[BUFSIZ];
            ssize_t count;
            while (true) {
                count = ::read(sock, buf, BUFSIZ);
                if (count > 0) {
                    s.append(buf, count);
                    if (s.rfind(prompt) != string::npos)
                        return s;
                }
                else if (count == 0) {
                    warn("Shell read - count is 0 (EOF)");
                }
                else if (count == -1) {
                    warn("Shell read - socket read error");
                }
            }
        }

        Shell& puts(const string& s) {
            string t = s;
            if (*t.rbegin() != '\n')
                t += '\n';
            ssize_t ret = write(sock, t.c_str(), t.length());
            if (ret == -1)
                warn("Shell puts - write error");
            return *this;
        }

        Shell& stop(void) {
            puts("exit").read(Bye);
            shutdown(sock, SHUT_RDWR) != -1 || warn("Shell stop - shutdown error");
            close(sock) != -1 || warn("Shell stop - socket close error");
            int stat_loc;
            waitpid(pid, &stat_loc, 0) != -1 || warn("Shell stop - waitpid error");
            return *this;
        }

        string x(const string& s, string prompt = Prompt) {
            return puts(s).read(prompt);
        }

        string x_s(const string& s, string prompt = Prompt) {
           string result = x(s);
           size_t pos = result.rfind(prompt);
           if (pos != string::npos)
               result.resize(pos);
           return result;
        }
    };

    const uint16_t mongo::Shell::DefaultPort = 30001;
    const int mongo::Shell::Retries = 10;
    const string mongo::Shell::Prompt = "> ";
    const string mongo::Shell::Bye = "bye\n";
}

namespace mongo_test {
    using namespace mongo;

    TEST(ShellTest, Basic) {
        Shell *shell = new Shell();
        string response = shell->x_s("1+2");
        ASSERT_EQUALS("3\n", response);
        shell->stop();
        delete shell;
    }
}
