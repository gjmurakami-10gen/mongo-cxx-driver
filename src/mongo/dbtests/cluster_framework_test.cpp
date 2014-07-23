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
#include <sstream>
#include <iomanip>

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

vector<string> &split(const string &s, char delim, vector<string> &elems) {
    stringstream ss(s);
    string item;
    while (getline(ss, item, delim)) {
        elems.push_back(item);
    }
    return elems;
}

inline stringstream& ssclear(stringstream& ss) {
   ss.str("");
   ss.clear();
   return ss;
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

        Shell() {
            port = DefaultPort;
            connect("mongo_shell.log").read();
        }

        void spawn(const string& logFileName) {
            pid_t pid;
            (pid = fork()) != -1 || die("Shell spawn - fork error");
            if (pid == 0) {
                umask(0);
                setsid() != -1 || die("Shell spawn - error on setsid");
                int fdLog = open(logFileName.c_str(), O_WRONLY|O_CREAT|O_TRUNC, 0664);
                fdLog != -1 || die("Shell spawn - error on open of log file for mongo shell");
                dup2(fdLog, 1) != -1 || die("Shell spawn - error on dup of log file for stdout");
                dup2(fdLog, 2) != -1 || die("Shell spawn - error on dup of log file for stderr");
                close(0);
                (pid = fork()) != -1 || die("Shell spawn - fork error");
                if (pid == 0) {
                    const char *envMongoShell = getenv("MONGO_SHELL");
                    string mongoShell( envMongoShell ? envMongoShell : "../mongo/mongo");
                    execv(mongoShell.c_str(), (char *const *)spawn_argvp);
                }
                _exit(0);
            }
            int stat_loc;
            waitpid(pid, &stat_loc, 0) != -1 || warn("Shell spawn - waitpid error");
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

        Shell& sh(const string& s, ostream& os) {
            vector<string> lines;
            split(s, '\n', lines);
            for (vector<string>::iterator is = lines.begin(); is != lines.end(); is++) {
                *is += '\n';
                os.write(is->c_str(), is->length()).flush();
                string result = x(*is);
                os.write(result.c_str(), result.length()).flush();
            }
            return *this;
        }
    };

    class ClusterTest;

    class TestNode {
    public:
        ClusterTest *cluster;
        string conn;
        string var;
        string hostPort;
        string host;
        uint16_t port;

        TestNode(ClusterTest *aCluster, const string& aConn) {
            cluster = aCluster;
            conn = aConn;
            conn.erase(remove(conn.begin(), conn.end(), '\"'), conn.end());
            //var = cluster->var;
            hostPort = conn.substr(strlen("connection to "));
            vector<string> vHostPort;
            split(hostPort, ':', vHostPort);
            host = vHostPort[0];
            port = atoi(vHostPort[1].c_str());
        }
        static vector<TestNode> vFromStringList(ClusterTest *aCluster, string sList);
    };

    vector<TestNode> TestNode::vFromStringList(ClusterTest *aCluster, string sList) {
        vector<TestNode> result;
        cerr << "TestNode::vFromStringList sList: " << sList << endl;
        return result;
    }

    class ClusterTest {
    public:
        Shell *ms;
        string var;
        string opts;

        ClusterTest(Shell *aMs, string anOpts) {
            ms = aMs;
            var = "ct";
            opts = anOpts;
        }
        virtual ~ClusterTest(void) {
        }
        string x(const string& s, string prompt = Shell::Prompt) {
            return ms->x(s, prompt);
        }
        string x_s(const string& s, string prompt = Shell::Prompt) {
            return ms->x_s(s, prompt);
        }
        Shell& sh(const string& s, ostream& os) {
            return ms->sh(s, os);
        }
        bool exists(void) {
            string js = "typeof " + var + ";";
            return ms->x_s(js) == "object";
        }
        ClusterTest& ensureCluster(void) {
            if (exists())
                restart();
            else {
                //FileUtils.mkdir_p(@opts[:dataPath])
                start();
            }
            return *this;
        }
        virtual string start(void) {
            return string();
        }
        virtual string restart(void) {
            return string();
        }
        virtual string stop(void) {
            return string();
        }
    };

    class ReplSetTest : public ClusterTest {
    public:
        static const string Opts;

        ReplSetTest(Shell *aMs, string anOpts = Opts) : ClusterTest(aMs, anOpts) {
            var = "rs";
        }
        ~ReplSetTest(void) {

        }
        string start(void) {
            stringstream os, js;
            ssclear(js) << "var " << var << " = new ReplSetTest( " << opts << " );";
            sh(js.str(), os);
            ssclear(js) << var << ".startSet();";
            sh(js.str(), os);
            os.str().find("ReplSetTest Starting") != string::npos || die("ReplSetTest start error on startSet");
            ssclear(js) << var << ".initiate();";
            sh(js.str(), os);
            os.str().find("Config now saved locally.  Should come online in about a minute.") != string::npos || die("ReplSetTest start error on initiate");
            ssclear(js) << var << ".awaitReplication();";
            sh(js.str(), os);
            os.str().find("ReplSetTest awaitReplication: finished: all") != string::npos || die("ReplSetTest start error on awaitReplication");
            return os.str();
        }
        string stop(void) {
            stringstream os, js;
            ssclear(js) << var << ".stopSet();";
            sh(js.str(), os);
            os.str().find("ReplSetTest stopSet *** Shut down repl set - test worked ***") != string::npos || die("ReplSetTest stop error on stopSet");
            return os.str();
        }
        string restart(void) {
            stringstream os, js;
            ssclear(js) << var << ".restartSet();";
            sh(js.str(), os);
            ssclear(js) << var << ".awaitSecondaryNodes(30000);";
            sh(js.str(), os);
            ssclear(js) << var << ".awaitReplication(30000);";
            sh(js.str(), os);
            os.str().find("ReplSetTest awaitReplication: finished: all") != string::npos || die("ReplSetTest restart error on awaitReplication");
            return os.str();
        }
        string status(void) {
            stringstream js;
            ssclear(js) << var << ".restartSet();";
            return x_s(js.str());
        }
        string nodes(void) {
            stringstream js;
            ssclear(js) << var << ".nodes;";
            return x_s(js.str());
        }
        TestNode primary(void) {
            stringstream js;
            ssclear(js) << var << ".getPrimary();";
            return TestNode(this, x_s(js.str()));
        }
    };

    class ShardingTest : public ClusterTest {
    public:
        static const string Opts;

        ShardingTest(Shell *aMs, string anOpts = Opts) : ClusterTest(aMs, anOpts) {
            var = "sc";
        }
        ~ShardingTest(void) {

        }
    };

    class Orchestrator {
    public:
        enum { Single, Replica, Sharded };


        Orchestrator(int type, string opts) {

        }
        ~Orchestrator(void) {

        }
    };

    const uint16_t mongo::Shell::DefaultPort = 30001;
    const int mongo::Shell::Retries = 10;
    const string mongo::Shell::Prompt = "> ";
    const string mongo::Shell::Bye = "bye\n";

    const string mongo::ReplSetTest::Opts = "{ name: 'test', nodes: 3, startPort: 31000 }";
    const string mongo::ShardingTest::Opts = "{ name: 'test', shards: 2, rs: { nodes: 3 }, mongos: 2, other: { separateConfig: true } }";
}

namespace mongo_test {
    using namespace mongo;

    TEST(ShellTest, Basic) {
        Shell *ms = new Shell();
        string response = ms->x_s("1+2");
        ASSERT_EQUALS("3\n", response);
        ClusterTest *ct = new ClusterTest(ms, "");
        bool exists = ct->exists();
        ASSERT_EQUALS(0, exists);
        ms->stop();
        delete ct;
        delete ms;
    }
    TEST(ReplSetTest, Basic) {
        Shell *ms = new Shell();
        string response;
        response = ms->x_s("1+2");
        ASSERT_EQUALS("3\n", response);
        ReplSetTest *rs = new ReplSetTest(ms);
        bool exists = rs->exists();
        ASSERT_EQUALS(0, exists);
        stringstream ss;
        response = rs->start();
        response = rs->status();
        cerr << "status:\n" << response;
        vector<TestNode> nodes = TestNode::vFromStringList(rs, rs->nodes());
        TestNode primary = rs->primary();
        cerr << "primary.host_port: " << primary.hostPort << endl;
        response = rs->stop();
        ms->stop();
        delete rs;
        delete ms;
    }
}
